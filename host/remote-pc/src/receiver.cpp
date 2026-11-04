#include "receiver.h"

#include <QNetworkDatagram>
#include <QUdpSocket>

namespace mlk {

Receiver::Receiver(QObject *parent)
    : QObject(parent)
    , m_sock(new QUdpSocket(this))
{
    connect(m_sock, &QUdpSocket::readyRead, this, &Receiver::onReadyRead);
}

bool Receiver::start(const QHostAddress &addr, quint16 port)
{
    stop();
    return m_sock->bind(addr, port, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
}

void Receiver::stop()
{
    if (m_sock->state() != QAbstractSocket::UnconnectedState)
        m_sock->close();
}

bool Receiver::isListening() const
{
    return m_sock->state() == QAbstractSocket::BoundState;
}

quint16 Receiver::localPort() const { return m_sock->localPort(); }

QString Receiver::errorString() const
{
    return m_sock->errorString();
}

bool Receiver::beginHandshake(const QHostAddress &addr, quint16 port)
{
    if (!m_ch || m_ch->mode() == SecureChannel::Off)
        return false;
    if (m_ch->mode() == SecureChannel::Psk)
        return m_ch->ready();
    const QByteArray hello = m_ch->buildHello();
    if (hello.isEmpty())
        return false;
    m_peer = addr;
    m_peerPort = port;
    return m_sock->writeDatagram(hello, addr, port) > 0;
}

qint64 Receiver::send(const QHostAddress &addr, quint16 port, const QByteArray &d)
{
    if (m_ch && m_ch->mode() != SecureChannel::Off) {
        if (!m_ch->ready())
            return -2;
        const QByteArray sealed = m_ch->seal(d);
        if (sealed.isEmpty())
            return -3;
        return m_sock->writeDatagram(sealed, addr, port);
    }
    return m_sock->writeDatagram(d, addr, port);
}

void Receiver::onReadyRead()
{
    while (m_sock->hasPendingDatagrams()) {
        const QNetworkDatagram dg = m_sock->receiveDatagram();
        if (!m_expectedPeer.isNull()
            && (dg.senderAddress() != m_expectedPeer || quint16(dg.senderPort()) != m_expectedPort))
            continue;
        const QByteArray d = dg.data();
        m_bytes += quint64(d.size());
        if (!dg.senderAddress().isNull()) {
            m_peer = dg.senderAddress();
            m_peerPort = quint16(dg.senderPort());
        }

        if (SecureChannel::isSecureDatagram(d) && m_ch && m_ch->mode() != SecureChannel::Off) {
            const quint8 kind = d.size() > 3 ? quint8(d[3]) : 0;
            if (kind == 1) {
                QByteArray reply;
                if (m_ch->handleHello(d, reply) && !reply.isEmpty())
                    m_sock->writeDatagram(reply, dg.senderAddress(), quint16(dg.senderPort()));
            } else if (kind == 2) {
                if (m_ch->handleReply(d))
                    emit secureReady();
            } else if (kind == 3) {
                QByteArray p;
                if (m_ch->open(d, p)) {
                    Frame f;
                    if (parseFrame(p, f)) {
                        emit frameReceived(f);
                        emit frameReceivedFrom(f, dg.senderAddress(), quint16(dg.senderPort()));
                    } else
                        emit badPacket();
                } else {
                    emit badPacket();
                }
            }
            continue;
        }

        if (m_ch && m_ch->mode() != SecureChannel::Off) { emit badPacket(); continue; }
        Frame f;
        if (parseFrame(d, f)) {
            emit frameReceived(f);
            emit frameReceivedFrom(f, dg.senderAddress(), quint16(dg.senderPort()));
        } else
            emit badPacket();
    }
}

} // namespace mlk
