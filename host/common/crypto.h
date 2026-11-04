#pragma once

#include <QByteArray>

namespace mlk {
namespace crypto {

QByteArray randomBytes(int n);
QByteArray sha256(const QByteArray &data);
QByteArray hmacSha256(const QByteArray &key, const QByteArray &data);
QByteArray hkdf(const QByteArray &ikm, const QByteArray &salt, const QByteArray &info, int outLen);

bool x25519Keygen(QByteArray &pub, QByteArray &priv);
QByteArray x25519(const QByteArray &priv, const QByteArray &peerPub);

QByteArray seal(const QByteArray &key, const QByteArray &nonce, const QByteArray &aad,
                const QByteArray &plain);
bool open(const QByteArray &key, const QByteArray &nonce, const QByteArray &aad,
          const QByteArray &sealed, QByteArray &plain);

QByteArray fingerprint(const QByteArray &pub);

} // namespace crypto
} // namespace mlk
