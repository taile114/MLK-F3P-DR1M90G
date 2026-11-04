#pragma once

#include <QObject>
#include <QHostAddress>
#include <QElapsedTimer>
#include <QHash>
#include <QQueue>
#include <QTimer>
#include <QUdpSocket>
#include "securechannel.h"

namespace mlk {

class PeerDiscovery : public QObject
{
    Q_OBJECT
public:
    enum Role { Board = 1, Remote = 2 };
    explicit PeerDiscovery(Role role, QObject *parent = nullptr, quint16 port = 8001,
                           const QHostAddress &bind = QHostAddress::AnyIPv4);
    bool configure(const SecureChannel &security, quint16 dataPort, bool enabled,
                   const QHostAddress &hint = {}, quint16 hintPort = 8000);
    void setConnected(bool connected) { m_connected = connected; }
    bool probe(const QHostAddress &peer, const QHostAddress &local);
    bool isListening() const { return m_socket.state() == QAbstractSocket::BoundState; }
signals:
    void peerDiscovered(const QHostAddress &address, quint16 port);
private:
    struct Pending { QByteArray nonce; qint64 sent = 0; };
    struct Candidate { QHostAddress peer, local; };
    QByteArray packet(quint8 kind, const QByteArray &nonce, const QHostAddress &local) const;
    void receive();
    void tick();
    void scan();
    Role m_role;
    quint16 m_port, m_dataPort = 8000, m_hintPort = 8000;
    QHostAddress m_bind, m_hint;
    SecureChannel::Mode m_mode = SecureChannel::Off;
    QByteArray m_key;
    QUdpSocket m_socket;
    QTimer m_timer;
    QElapsedTimer m_clock;
    qint64 m_nextScan = 0;
    bool m_connected = false;
    QHash<quint32, Pending> m_pending;
    QQueue<Candidate> m_candidates;
};

} // namespace mlk
