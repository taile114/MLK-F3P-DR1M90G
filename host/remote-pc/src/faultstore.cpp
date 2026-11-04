#include "faultstore.h"

#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStorageInfo>

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace mlk {

namespace {

constexpr int kSaltLen = 16;
constexpr int kIvLen = 12;
constexpr int kTagLen = 16;

bool deriveKey(const QString &pass, const unsigned char *salt, unsigned char *key)
{
    const QByteArray p = pass.toUtf8();
    return PKCS5_PBKDF2_HMAC(p.constData(), p.size(), salt, kSaltLen, 100000, EVP_sha256(), 32, key)
           == 1;
}

QByteArray gcmEncrypt(const QByteArray &in, const unsigned char *key, const unsigned char *iv,
                      unsigned char *tag)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    QByteArray out(in.size() + 16, '\0');
    int len = 0;
    int total = 0;
    EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kIvLen, nullptr);
    EVP_EncryptInit_ex(ctx, nullptr, nullptr, key, iv);
    EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char *>(out.data()), &len,
                      reinterpret_cast<const unsigned char *>(in.constData()), in.size());
    total = len;
    EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(out.data()) + total, &len);
    total += len;
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagLen, tag);
    EVP_CIPHER_CTX_free(ctx);
    out.resize(total);
    return out;
}

bool gcmDecrypt(const QByteArray &in, const unsigned char *key, const unsigned char *iv,
                const unsigned char *tag, QByteArray &out)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    out.resize(in.size());
    int len = 0;
    int total = 0;
    bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1;
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kIvLen, nullptr) == 1;
    ok = ok && EVP_DecryptInit_ex(ctx, nullptr, nullptr, key, iv) == 1;
    ok = ok
         && EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char *>(out.data()), &len,
                              reinterpret_cast<const unsigned char *>(in.constData()), in.size())
                == 1;
    total = len;
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTagLen, const_cast<unsigned char *>(tag));
    ok = ok && EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(out.data()) + total, &len)
                   == 1;
    total += len;
    EVP_CIPHER_CTX_free(ctx);
    out.resize(total);
    return ok;
}

QByteArray packSamples(const QVector<qint16> ch[4], int chCount, int n)
{
    QByteArray p;
    p.resize(chCount * n * 2);
    char *w = p.data();
    for (int c = 0; c < chCount; ++c) {
        const QVector<qint16> &v = ch[c];
        for (int i = 0; i < n; ++i) {
            const qint16 s = i < v.size() ? v[i] : 0;
            w[0] = char(s & 0xFF);
            w[1] = char((s >> 8) & 0xFF);
            w += 2;
        }
    }
    return p;
}

} // namespace

FaultStore::FaultStore(const QString &dir, bool encrypt, const QString &passphrase,
                       int retentionMonths, qint64 minFreeBytes, int maxFiles)
    : m_dir(dir)
    , m_encrypt(encrypt)
    , m_pass(passphrase)
    , m_retentionMonths(retentionMonths)
    , m_minFreeBytes(minFreeBytes)
    , m_maxFiles(maxFiles)
{
    if (m_dir.isEmpty())
        m_dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                + QStringLiteral("/faults");
}

bool FaultStore::save(const FaultMeta &meta, const QVector<qint16> ch[4], QString *err) const
{
    if (!QDir().mkpath(m_dir)) {
        if (err)
            *err = QStringLiteral("无法创建目录 %1").arg(m_dir);
        return false;
    }

    QString pruneErr;
    prune(&pruneErr);

    unsigned char salt[kSaltLen] = {};
    unsigned char iv[kIvLen] = {};
    RAND_bytes(salt, kSaltLen);
    RAND_bytes(iv, kIvLen);

    QByteArray header;
    QDataStream ds(&header, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds.writeRawData("MLKF", 4);
    ds << quint8(1) << quint8(m_encrypt ? 1 : 0) << quint16(0);
    ds.writeRawData(reinterpret_cast<char *>(salt), kSaltLen);
    ds.writeRawData(reinterpret_cast<char *>(iv), kIvLen);
    ds << meta.chCount << meta.cls << meta.grade << meta.conf << meta.rateHz << quint16(0);
    ds << meta.tsUs << meta.rul << meta.n;
    for (int i = 0; i < 4; ++i)
        ds << meta.k[i] << meta.b[i];

    const QByteArray payload = packSamples(ch, meta.chCount, meta.n);

    QByteArray body;
    if (m_encrypt) {
        unsigned char key[32] = {};
        unsigned char tag[kTagLen] = {};
        if (!deriveKey(m_pass, salt, key)) {
            if (err)
                *err = QStringLiteral("密钥派生失败");
            return false;
        }
        body = gcmEncrypt(payload, key, iv, tag);
        body.append(reinterpret_cast<char *>(tag), kTagLen);
    } else {
        body = payload;
    }

    const QString stamp = QDateTime::fromMSecsSinceEpoch(qint64(meta.tsUs / 1000))
                              .toUTC().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    const QString base = QStringLiteral("fault_%1_%2").arg(stamp).arg(meta.tsUs);
    QString path = QDir(m_dir).filePath(base + QStringLiteral(".mlkf"));
    for (int i = 1; QFile::exists(path); ++i)
        path = QDir(m_dir).filePath(base + QStringLiteral("_%1.mlkf").arg(i));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (err)
            *err = f.errorString();
        return false;
    }
    f.write(header);
    f.write(body);
    f.close();
    return true;
}

int FaultStore::prune(QString *err) const
{
    QDir d(m_dir);
    if (!d.exists())
        return 0;
    const QFileInfoList files = d.entryInfoList({ QStringLiteral("fault_*.mlkf") },
                                                QDir::Files, QDir::Time | QDir::Reversed);
    if (files.isEmpty())
        return 0;

    qint64 freeBytes = 0;
    if (m_minFreeBytes > 0) {
        QStorageInfo storage(m_dir);
        storage.refresh();
        freeBytes = storage.bytesAvailable();
    }
    const QDateTime now = QDateTime::currentDateTime();
    const qint64 ageLimitSecs = qint64(m_retentionMonths) * 30 * 86400;
    int removed = 0;
    for (const QFileInfo &fi : files) {
        bool remove = false;
        if (ageLimitSecs > 0 && fi.lastModified().isValid()
            && fi.lastModified().secsTo(now) > ageLimitSecs)
            remove = true;
        if (m_maxFiles > 0 && files.size() - removed > m_maxFiles)
            remove = true;
        if (m_minFreeBytes > 0 && freeBytes < m_minFreeBytes)
            remove = true;
        if (!remove)
            continue;
        const qint64 size = fi.size();
        if (QFile::remove(fi.absoluteFilePath())) {
            ++removed;
            freeBytes += size;
        } else if (err && err->isEmpty()) {
            *err = QStringLiteral("无法删除 %1").arg(fi.absoluteFilePath());
        }
    }
    return removed;
}

int FaultStore::count() const
{
    return QDir(m_dir).entryList({ QStringLiteral("fault_*.mlkf") }, QDir::Files).size();
}

qint64 FaultStore::totalBytes() const
{
    const QFileInfoList files = QDir(m_dir).entryInfoList(
        { QStringLiteral("fault_*.mlkf") }, QDir::Files);
    qint64 total = 0;
    for (const QFileInfo &fi : files)
        total += fi.size();
    return total;
}

QStringList FaultStore::list() const
{
    QDir d(m_dir);
    QStringList files = d.entryList({ QStringLiteral("fault_*.mlkf") }, QDir::Files, QDir::Name);
    QStringList out;
    for (int i = files.size() - 1; i >= 0; --i)
        out << d.filePath(files[i]);
    return out;
}

bool FaultStore::load(const QString &path, FaultMeta &meta, QVector<qint16> ch[4], QString *err) const
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err)
            *err = f.errorString();
        return false;
    }
    const QByteArray all = f.readAll();
    if (all.size() < 125 || all.size() > 64*1024*1024 || all.left(4) != QByteArrayLiteral("MLKF")) {
        if (err)
            *err = QStringLiteral("不是 MLKF 文件");
        return false;
    }

    QDataStream ds(all);
    ds.setByteOrder(QDataStream::LittleEndian);
    char magic[4];
    ds.readRawData(magic, 4);
    quint8 ver = 0, enc = 0, chCount = 4, cls = 0, grade = 0;
    quint16 rsv = 0, conf = 0, rate = 0, pad = 0;
    quint64 tsUs = 0;
    quint32 rul = 0, n = 0;
    ds >> ver >> enc >> rsv;
    unsigned char salt[kSaltLen] = {};
    unsigned char iv[kIvLen] = {};
    ds.readRawData(reinterpret_cast<char *>(salt), kSaltLen);
    ds.readRawData(reinterpret_cast<char *>(iv), kIvLen);
    ds >> chCount >> cls >> grade >> conf >> rate >> pad;
    ds >> tsUs >> rul >> n;
    for (int i = 0; i < 4; ++i)
        ds >> meta.k[i] >> meta.b[i];

    const qint64 expected=qint64(chCount)*n*2;
    if (ds.status()!=QDataStream::Ok || ver!=1 || enc>1 || chCount<1 || chCount>4
        || n>2000000 || expected!=all.size()-ds.device()->pos()-(enc?16:0)) {
        if (err) *err=QStringLiteral("MLKF 版本、通道或载荷长度无效");
        return false;
    }

    const int headerLen = int(ds.device()->pos());
    meta.tsUs = tsUs;
    meta.rateHz = rate;
    meta.chCount = chCount;
    meta.cls = cls;
    meta.grade = grade;
    meta.conf = conf;
    meta.rul = rul;
    meta.n = n;

    QByteArray body = all.mid(headerLen);
    if (enc) {
        if (body.size() < kTagLen) {
            if (err)
                *err = QStringLiteral("数据不完整");
            return false;
        }
        const QByteArray tag = body.right(kTagLen);
        body.chop(kTagLen);
        unsigned char key[32] = {};
        if (!deriveKey(m_pass, salt, key)) {
            if (err)
                *err = QStringLiteral("密钥派生失败");
            return false;
        }
        QByteArray plain;
        if (!gcmDecrypt(body, key, iv, reinterpret_cast<const unsigned char *>(tag.constData()),
                        plain)) {
            if (err)
                *err = QStringLiteral("解密失败（口令错误或数据损坏）");
            return false;
        }
        body = plain;
    }

    const int cc = chCount > 4 ? 4 : chCount;
    for (int c = 0; c < 4; ++c) {
        ch[c].clear();
        if (c >= cc)
            continue;
        ch[c].resize(n);
        for (quint32 i = 0; i < n; ++i) {
            const int off = (c * n + i) * 2;
            if (off + 1 >= body.size())
                break;
            ch[c][i] = qint16(quint8(body[off]) | (quint16(quint8(body[off + 1])) << 8));
        }
    }
    return true;
}

} // namespace mlk
