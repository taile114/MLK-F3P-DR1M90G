#pragma once

#include <QByteArray>
#include <QtGlobal>
#include <QHash>

#include "crypto.h"

namespace mlk {

class SecureChannel
{
public:
    enum Mode {
        Off,
        Psk,
        Ecdh,
    };

    void configure(Mode mode, const QByteArray &psk, const QByteArray &selfPub,
                   const QByteArray &selfPriv, const QByteArray &peerPub);

    Mode mode() const { return m_mode; }
    bool ready() const { return m_ready; }
    QByteArray discoveryKey() const;

    static bool generateIdentity(QByteArray &pub, QByteArray &priv)
    {
        return crypto::x25519Keygen(pub, priv);
    }
    static QByteArray fingerprint(const QByteArray &pub) { return crypto::fingerprint(pub); }

    static bool isSecureDatagram(const QByteArray &d);

    QByteArray buildHello();
    bool handleHello(const QByteArray &hello, QByteArray &reply);
    bool handleReply(const QByteArray &reply);

    QByteArray seal(const QByteArray &plain);
    bool open(const QByteArray &datagram, QByteArray &plain);

private:
    QByteArray wrap(quint8 kind, const QByteArray &payload) const;
    static bool unwrap(const QByteArray &d, quint8 &kind, QByteArray &payload);

    Mode m_mode = Off;
    QByteArray m_psk;
    QByteArray m_selfPub;
    QByteArray m_selfPriv;
    QByteArray m_peerPub;
    QByteArray m_key;
    bool m_ready = false;

    QByteArray m_ephPriv;
    QByteArray m_ephPub;
    QByteArray m_nonceC;
    QByteArray m_nonceS;

    quint64 m_sendSeq = 1;
    quint64 m_lastSeq = 0;
    QByteArray m_session;
    struct ReceiveWindow { quint64 highest = 0; quint64 bitmap = 0; };
    QHash<QByteArray, ReceiveWindow> m_windows;
};

} // namespace mlk
