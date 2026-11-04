#include "peerdiscovery.h"
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QtEndian>
#include <openssl/crypto.h>

namespace mlk {
namespace {
constexpr int kHeaderSize = 30;
constexpr int kPacketSize = kHeaderSize + 32;
bool privateV4(quint32 ip)
{
    return (ip >> 24) == 10 || (ip >> 20) == 0xac1 || (ip >> 16) == 0xc0a8
        || (ip >> 22) == 0x191; // RFC1918 and carrier-grade private LANs.
}
}

PeerDiscovery::PeerDiscovery(Role role, QObject *parent, quint16 port, const QHostAddress &bind)
    : QObject(parent), m_role(role), m_port(port), m_bind(bind)
{
    m_clock.start();
    m_timer.setInterval(100);
    connect(&m_timer, &QTimer::timeout, this, &PeerDiscovery::tick);
    connect(&m_socket, &QUdpSocket::readyRead, this, &PeerDiscovery::receive);
}

bool PeerDiscovery::configure(const SecureChannel &security, quint16 dataPort, bool enabled,
                              const QHostAddress &hint, quint16 hintPort)
{
    m_timer.stop(); m_socket.close(); m_candidates.clear(); m_pending.clear();
    m_key = security.discoveryKey(); m_mode = security.mode(); m_dataPort = dataPort;
    m_hint = hint; m_hintPort = hintPort; m_nextScan = 0; m_connected = false;
    if (!enabled || m_key.size() != 32 || !dataPort) return true;
    // A single discovery socket per endpoint; do not share a role's port.
    if (!m_socket.bind(m_bind, m_port, QUdpSocket::DontShareAddress)) return false;
    m_timer.start();
    return true;
}

QByteArray PeerDiscovery::packet(quint8 kind, const QByteArray &nonce, const QHostAddress &local) const
{
    if (m_key.size() != 32 || nonce.size() != 16 || local.protocol() != QAbstractSocket::IPv4Protocol)
        return {};
    QByteArray header("MLKD", 4);
    header.append(char(1)); header.append(char(kind)); header.append(char(m_role)); header.append(char(m_mode));
    const auto port = qToBigEndian(m_dataPort);
    header.append(reinterpret_cast<const char *>(&port), 2);
    header += nonce;
    const auto ip = qToBigEndian(local.toIPv4Address());
    header.append(reinterpret_cast<const char *>(&ip), 4);
    return header + crypto::hmacSha256(m_key, header);
}

bool PeerDiscovery::probe(const QHostAddress &peer, const QHostAddress &local)
{
    if (!isListening() || peer.protocol() != QAbstractSocket::IPv4Protocol) return false;
    const auto nonce = crypto::randomBytes(16), request = packet(1, nonce, local);
    if (request.isEmpty() || m_pending.size() >= 1024) return false;
    if (m_socket.writeDatagram(request, peer, m_port) != request.size()) return false;
    m_pending.insert(peer.toIPv4Address(), {nonce, m_clock.elapsed()});
    return true;
}

void PeerDiscovery::receive()
{
    while (m_socket.hasPendingDatagrams()) {
        const auto dg = m_socket.receiveDatagram(256);
        const auto data = dg.data();
        if (data.size() != kPacketSize || data.left(4) != "MLKD" || quint8(data[4]) != 1
            || quint8(data[6]) != (m_role == Board ? Remote : Board) || quint8(data[7]) != m_mode
            || dg.senderPort() != m_port || dg.senderAddress().protocol() != QAbstractSocket::IPv4Protocol)
            continue;
        const auto ip = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(data.constData() + 26));
        const auto port = qFromBigEndian<quint16>(reinterpret_cast<const uchar *>(data.constData() + 8));
        const auto tag = crypto::hmacSha256(m_key, data.left(kHeaderSize));
        if (ip != dg.senderAddress().toIPv4Address() || !port || tag.size() != 32
            || CRYPTO_memcmp(tag.constData(), data.constData() + kHeaderSize, 32)) continue;
        const auto nonce = data.mid(10, 16);
        if (quint8(data[5]) == 1) {
            auto local = dg.destinationAddress();
            if (local.isNull() || local == QHostAddress::AnyIPv4) local = m_bind;
            const auto reply = packet(2, nonce, local);
            if (!reply.isEmpty()) m_socket.writeDatagram(reply, dg.senderAddress(), m_port);
        } else if (quint8(data[5]) == 2) {
            const auto pending = m_pending.constFind(ip);
            if (pending == m_pending.cend() || pending->nonce != nonce || m_clock.elapsed() - pending->sent > 5000)
                continue;
            m_pending.remove(ip);
            if (dg.senderAddress() != m_hint || port != m_hintPort)
                emit peerDiscovered(dg.senderAddress(), port);
        }
    }
}

void PeerDiscovery::scan()
{
    m_candidates.clear();
    if (m_bind != QHostAddress::AnyIPv4) return; // Explicit bindings are used by isolated tests.
    for (const auto &interface : QNetworkInterface::allInterfaces()) {
        const auto flags = interface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning)
            || (flags & QNetworkInterface::IsLoopBack)
            || (interface.type() != QNetworkInterface::Wifi && interface.type() != QNetworkInterface::Ethernet))
            continue;
        for (const auto &entry : interface.addressEntries()) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol || !privateV4(entry.ip().toIPv4Address())
                || entry.prefixLength() < 24 || entry.prefixLength() > 30) continue;
            const quint32 mask = 0xffffffffU << (32 - entry.prefixLength());
            const quint32 first = entry.ip().toIPv4Address() & mask, last = first | ~mask;
            if (m_hint.isInSubnet(entry.ip(), entry.prefixLength()) && m_hint != entry.ip())
                m_candidates.enqueue({m_hint, entry.ip()});
            for (quint32 ip = first + 1; ip < last && m_candidates.size() < 1024; ++ip) {
                if (ip != entry.ip().toIPv4Address() && QHostAddress(ip) != m_hint)
                    m_candidates.enqueue({QHostAddress(ip), entry.ip()});
            }
        }
    }
}

void PeerDiscovery::tick()
{
    const auto now = m_clock.elapsed();
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (now - it->sent > 5000) it = m_pending.erase(it); else ++it;
    }
    if (m_connected) { m_candidates.clear(); return; }
    if (m_candidates.isEmpty() && now >= m_nextScan) { scan(); m_nextScan = now + 10000; }
    for (int i = 0; i < 16 && !m_candidates.isEmpty(); ++i) {
        const auto candidate = m_candidates.dequeue(); probe(candidate.peer, candidate.local);
    }
}

} // namespace mlk
