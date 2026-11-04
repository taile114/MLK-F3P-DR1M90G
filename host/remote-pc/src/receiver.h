#pragma once

#include <QHostAddress>
#include <QObject>

#include "frame.h"
#include "securechannel.h"

class QUdpSocket;

namespace mlk {

class Receiver : public QObject
{
    Q_OBJECT
public:
    void setExpectedPeer(const QHostAddress &address, quint16 port) { m_expectedPeer = address; m_expectedPort = port; }
    explicit Receiver(QObject *parent = nullptr);

    bool start(const QHostAddress &addr, quint16 port);
    void stop();
    bool isListening() const;
    quint16 localPort() const;
    QString errorString() const;
    quint64 bytesReceived() const { return m_bytes; }

    void setChannel(SecureChannel *ch) { m_ch = ch; }
    bool beginHandshake(const QHostAddress &addr, quint16 port);

    qint64 send(const QHostAddress &addr, quint16 port, const QByteArray &d);

signals:
    void frameReceived(const mlk::Frame &f);
    void frameReceivedFrom(const mlk::Frame &f, const QHostAddress &address, quint16 port);
    void badPacket();
    void secureReady();

private slots:
    void onReadyRead();

private:
    QHostAddress m_expectedPeer;
    quint16 m_expectedPort = 0;
    QUdpSocket *m_sock;
    SecureChannel *m_ch = nullptr;
    QHostAddress m_peer;
    quint16 m_peerPort = 0;
    quint64 m_bytes = 0;
};

} // namespace mlk
