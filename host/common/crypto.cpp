#include "crypto.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace mlk {
namespace crypto {

QByteArray randomBytes(int n)
{
    QByteArray b(n, '\0');
    if (n > 0 && RAND_bytes(reinterpret_cast<unsigned char *>(b.data()), n) != 1)
        return {};
    return b;
}

QByteArray sha256(const QByteArray &data)
{
    QByteArray out(SHA256_DIGEST_LENGTH, '\0');
    unsigned int len = 0;
    EVP_Digest(data.constData(), size_t(data.size()),
               reinterpret_cast<unsigned char *>(out.data()), &len, EVP_sha256(), nullptr);
    return out;
}

QByteArray hmacSha256(const QByteArray &key, const QByteArray &data)
{
    QByteArray out(EVP_MAX_MD_SIZE, '\0');
    unsigned int len = 0;
    HMAC(EVP_sha256(), key.constData(), key.size(),
         reinterpret_cast<const unsigned char *>(data.constData()), size_t(data.size()),
         reinterpret_cast<unsigned char *>(out.data()), &len);
    out.resize(int(len));
    return out;
}

QByteArray hkdf(const QByteArray &ikm, const QByteArray &salt, const QByteArray &info, int outLen)
{
    const QByteArray prk = hmacSha256(salt, ikm);
    QByteArray okm;
    QByteArray t;
    for (int i = 1; okm.size() < outLen; ++i) {
        t = hmacSha256(prk, t + info + QByteArray(1, char(i)));
        okm += t;
    }
    return okm.left(outLen);
}

bool x25519Keygen(QByteArray &pub, QByteArray &priv)
{
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    if (!ctx)
        return false;
    EVP_PKEY *pkey = nullptr;
    bool ok = EVP_PKEY_keygen_init(ctx) == 1 && EVP_PKEY_keygen(ctx, &pkey) == 1;
    if (ok) {
        size_t len = 32;
        pub.resize(32);
        ok = EVP_PKEY_get_raw_public_key(pkey, reinterpret_cast<unsigned char *>(pub.data()), &len)
             == 1;
        len = 32;
        priv.resize(32);
        ok = ok
             && EVP_PKEY_get_raw_private_key(pkey,
                                             reinterpret_cast<unsigned char *>(priv.data()), &len)
                    == 1;
    }
    if (pkey)
        EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(ctx);
    return ok;
}

QByteArray x25519(const QByteArray &priv, const QByteArray &peerPub)
{
    QByteArray out;
    if (priv.size() != 32 || peerPub.size() != 32)
        return out;
    EVP_PKEY *self = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_X25519, nullptr, reinterpret_cast<const unsigned char *>(priv.constData()), 32);
    EVP_PKEY *peer = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_X25519, nullptr, reinterpret_cast<const unsigned char *>(peerPub.constData()), 32);
    EVP_PKEY_CTX *ctx = self ? EVP_PKEY_CTX_new(self, nullptr) : nullptr;
    if (ctx && peer && EVP_PKEY_derive_init(ctx) == 1 && EVP_PKEY_derive_set_peer(ctx, peer) == 1) {
        size_t len = 32;
        out.resize(32);
        if (EVP_PKEY_derive(ctx, reinterpret_cast<unsigned char *>(out.data()), &len) != 1)
            out.clear();
    }
    if (ctx)
        EVP_PKEY_CTX_free(ctx);
    if (peer)
        EVP_PKEY_free(peer);
    if (self)
        EVP_PKEY_free(self);
    return out;
}

QByteArray seal(const QByteArray &key, const QByteArray &nonce, const QByteArray &aad,
                const QByteArray &plain)
{
    QByteArray out;
    if (key.size() != 32 || nonce.size() != 12)
        return out;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    unsigned char tag[16] = {};
    QByteArray body(plain.size() + 16, '\0');
    int len = 0;
    int total = 0;
    bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1;
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1;
    ok = ok
         && EVP_EncryptInit_ex(ctx, nullptr, nullptr,
                               reinterpret_cast<const unsigned char *>(key.constData()),
                               reinterpret_cast<const unsigned char *>(nonce.constData()))
                == 1;
    if (ok && !aad.isEmpty())
        ok = EVP_EncryptUpdate(ctx, nullptr, &len,
                               reinterpret_cast<const unsigned char *>(aad.constData()), aad.size())
             == 1;
    ok = ok
         && EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char *>(body.data()), &len,
                              reinterpret_cast<const unsigned char *>(plain.constData()), plain.size())
                == 1;
    total = len;
    ok = ok && EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(body.data()) + total, &len)
                   == 1;
    total += len;
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return out;
    body.resize(total);
    out.append(reinterpret_cast<char *>(tag), 16);
    out += body;
    return out;
}

bool open(const QByteArray &key, const QByteArray &nonce, const QByteArray &aad,
          const QByteArray &sealed, QByteArray &plain)
{
    if (key.size() != 32 || nonce.size() != 12 || sealed.size() < 16)
        return false;
    const unsigned char *tag = reinterpret_cast<const unsigned char *>(sealed.constData());
    const QByteArray ct = sealed.mid(16);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    plain.resize(ct.size());
    int len = 0;
    int total = 0;
    bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1;
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1;
    ok = ok
         && EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                               reinterpret_cast<const unsigned char *>(key.constData()),
                               reinterpret_cast<const unsigned char *>(nonce.constData()))
                == 1;
    if (ok && !aad.isEmpty())
        ok = EVP_DecryptUpdate(ctx, nullptr, &len,
                               reinterpret_cast<const unsigned char *>(aad.constData()), aad.size())
             == 1;
    ok = ok
         && EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char *>(plain.data()), &len,
                              reinterpret_cast<const unsigned char *>(ct.constData()), ct.size())
                == 1;
    total = len;
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16,
                                   const_cast<unsigned char *>(tag))
                   == 1;
    ok = ok && EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(plain.data()) + total, &len)
                   == 1;
    total += len;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return false;
    plain.resize(total);
    return true;
}

QByteArray fingerprint(const QByteArray &pub)
{
    return sha256(pub).left(4).toHex();
}

} // namespace crypto
} // namespace mlk
