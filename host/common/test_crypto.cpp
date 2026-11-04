#include <QByteArray>
#include <QCoreApplication>
#include <cstdio>

#include "crypto.h"
#include "securechannel.h"

using namespace mlk;

static int g_failures = 0;

static void check(bool ok, const char *name)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        ++g_failures;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const QByteArray key = crypto::randomBytes(32);
    const QByteArray nonce = crypto::randomBytes(12);
    const QByteArray pt = QByteArrayLiteral("hello mlk");
    QByteArray sealed = crypto::seal(key, nonce, QByteArray(), pt);
    QByteArray out;
    check(!sealed.isEmpty() && crypto::open(key, nonce, QByteArray(), sealed, out) && out == pt,
          "aes-256-gcm roundtrip");
    QByteArray bad = sealed;
    bad[bad.size() - 1] = char(bad[bad.size() - 1] ^ 0x01);
    check(!crypto::open(key, nonce, QByteArray(), bad, out), "aes-256-gcm rejects tamper");

    {
        SecureChannel a;
        SecureChannel b;
        const QByteArray psk = crypto::randomBytes(32);
        a.configure(SecureChannel::Psk, psk, QByteArray(), QByteArray(), QByteArray());
        b.configure(SecureChannel::Psk, psk, QByteArray(), QByteArray(), QByteArray());
        check(a.ready() && b.ready(), "psk ready");
        const QByteArray d = a.seal(QByteArrayLiteral("frame-bytes"));
        QByteArray p;
        check(!d.isEmpty() && b.open(d, p) && p == QByteArrayLiteral("frame-bytes"),
              "psk seal/open");
        check(!b.open(d, p), "psk replay rejected");
        check(!a.open(d, p), "outbound reflection rejected");
        const auto back = b.seal(QByteArrayLiteral("reverse"));
        check(a.open(back,p) && p == "reverse", "psk bidirectional first packet");
        const auto earlier = a.seal("earlier"), later = a.seal("later");
        check(b.open(later,p) && b.open(earlier,p) && !b.open(earlier,p), "64 packet reorder window");
        auto altered = a.seal("tamper"); altered[8] = char(altered[8] ^ 1);
        check(!b.open(altered,p), "sender session authenticated");
        a.configure(SecureChannel::Psk, psk, {}, {}, {});
        const auto restarted = a.seal("restart");
        check(b.open(restarted,p) && p == "restart", "sender restart new session");
        check(d.mid(4,16) != restarted.mid(4,16), "restart nonce domain differs");
    }

    {
        QByteArray apub, apriv, bpub, bpriv;
        crypto::x25519Keygen(apub, apriv);
        crypto::x25519Keygen(bpub, bpriv);
        SecureChannel a;
        SecureChannel b;
        a.configure(SecureChannel::Ecdh, QByteArray(), apub, apriv, bpub);
        b.configure(SecureChannel::Ecdh, QByteArray(), bpub, bpriv, apub);
        const QByteArray hello = a.buildHello();
        QByteArray reply;
        check(b.handleHello(hello, reply), "ecdh handle hello");
        check(a.handleReply(reply), "ecdh handle reply");
        check(a.ready() && b.ready(), "ecdh ready");
        const QByteArray d = a.seal(QByteArrayLiteral("diag"));
        QByteArray p;
        check(b.open(d, p) && p == QByteArrayLiteral("diag"), "ecdh a->b");
        const QByteArray d2 = b.seal(QByteArrayLiteral("cfg"));
        QByteArray p2;
        check(a.open(d2, p2) && p2 == QByteArrayLiteral("cfg"), "ecdh b->a");
    }

    {
        QByteArray apub, apriv, cpub, cpriv, wrongPub, wrongPriv;
        crypto::x25519Keygen(apub, apriv);
        crypto::x25519Keygen(cpub, cpriv);
        crypto::x25519Keygen(wrongPub, wrongPriv);
        SecureChannel a;
        SecureChannel c;
        a.configure(SecureChannel::Ecdh, QByteArray(), apub, apriv, cpub);
        c.configure(SecureChannel::Ecdh, QByteArray(), cpub, cpriv, wrongPub);
        const QByteArray hello = a.buildHello();
        QByteArray reply;
        c.handleHello(hello, reply);
        check(!a.handleReply(reply), "ecdh wrong pin rejected");
    }

    std::printf("failures=%d\n", g_failures);
    return g_failures ? 1 : 0;
}
