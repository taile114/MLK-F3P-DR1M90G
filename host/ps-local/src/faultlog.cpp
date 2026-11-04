#include "faultlog.h"

#include "crypto.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QStorageInfo>
#include <openssl/evp.h>

namespace mlk {
namespace {

constexpr int kHeaderSize = 22;    /* MLKF + 版本 + 加密位 + 16 字节盐 */
constexpr qint64 kMaxFileBytes = 64 * 1024 * 1024;
constexpr qint64 kMaxRecordBytes = 16384;
constexpr int kMaxFiles = 400;

bool fail(QString *error, const QString &text)
{
    if (error) *error = text;
    return false;
}

QByteArray deriveKey(const QString &password, const QByteArray &salt)
{
    QByteArray key(32, '\0');
    const auto utf8 = password.toUtf8();
    if (salt.size() != 16 || utf8.isEmpty()) return {};
    if (PKCS5_PBKDF2_HMAC(utf8.constData(), utf8.size(),
                          reinterpret_cast<const uchar *>(salt.constData()), salt.size(),
                          100000, EVP_sha256(), key.size(),
                          reinterpret_cast<uchar *>(key.data())) != 1) return {};
    return key;
}

QByteArray fileHeader(const QByteArray &salt)
{
    QByteArray header("MLKF", 4);
    header.append(char(1));
    header.append(char(1));
    header += salt;
    return header;
}

quint32 rd32(const char *p)
{
    return quint32(quint8(p[0])) | (quint32(quint8(p[1])) << 8) | (quint32(quint8(p[2])) << 16)
        | (quint32(quint8(p[3])) << 24);
}

void wr32(QByteArray &data, quint32 value)
{
    for (int i = 0; i < 4; ++i) data.append(char((value >> (8 * i)) & 0xFF));
}

} // namespace

bool FaultJournal::append(const QJsonObject &record, const QString &directory,
                          const QString &password, int retentionMonths, qint64 minFreeMb,
                          QString *error)
{
    const QDir base(QDir(directory).filePath(QStringLiteral("fault")));
    if (!base.exists() && !QDir().mkpath(base.absolutePath()))
        return fail(error, QStringLiteral("无法创建故障日志目录"));
    const QString path = base.filePath(QStringLiteral("fault_%1.mlkf")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd"))));
    if (m_path != path || m_password != password || m_key.isEmpty()) {
        QFile existing(path);
        if (existing.exists()) {
            if (!existing.open(QIODevice::ReadOnly))
                return fail(error, QStringLiteral("故障日志不可读：%1").arg(existing.errorString()));
            const auto header = existing.read(kHeaderSize);
            existing.close();
            if (header.size() != kHeaderSize || header.left(5) != QByteArray("MLKF\1", 5))
                return fail(error, QStringLiteral("故障日志格式无效"));
            m_salt = header.mid(6, 16);
        } else {
            m_salt = crypto::randomBytes(16);
            if (m_salt.size() != 16) return fail(error, QStringLiteral("故障日志随机盐不可用"));
        }
        m_key = deriveKey(password, m_salt);
        if (m_key.size() != 32) return fail(error, QStringLiteral("故障日志口令不可用"));
        m_password = password;
        m_path = path;
    }

    const auto plain = QJsonDocument(record).toJson(QJsonDocument::Compact);
    if (plain.isEmpty() || plain.size() > kMaxRecordBytes - 64)
        return fail(error, QStringLiteral("故障日志记录过长"));
    const auto nonce = crypto::randomBytes(12);
    if (nonce.size() != 12) return fail(error, QStringLiteral("故障日志随机数不可用"));
    const auto header = fileHeader(m_salt);
    const auto sealed = crypto::seal(m_key, nonce, header, plain);
    if (sealed.isEmpty()) return fail(error, QStringLiteral("故障日志加密失败"));

    QFile file(path);
    const bool created = !file.exists() || file.size() == 0;
    if (!file.open(QIODevice::ReadWrite))
        return fail(error, QStringLiteral("故障日志不可写：%1").arg(file.errorString()));
    if (created) {
        if (file.write(header) != header.size())
            return fail(error, file.errorString());
    } else if (file.size() > kMaxFileBytes) {
        file.close();
        return fail(error, QStringLiteral("故障日志已满"));
    }
    if (!file.seek(file.size())) return fail(error, file.errorString());
    QByteArray frame;
    wr32(frame, quint32(nonce.size() + sealed.size()));
    frame += nonce;
    frame += sealed;
    if (file.write(frame) != frame.size() || !file.flush())
        return fail(error, file.errorString());
    file.close();
    prune(base.absolutePath(), retentionMonths, minFreeMb, error);
    return true;
}

bool FaultJournal::read(const QString &path, const QString &password, QJsonArray &records,
                        QString *error)
{
    records = QJsonArray();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return fail(error, file.errorString());
    if (file.size() > kMaxFileBytes) return fail(error, QStringLiteral("故障日志过大"));
    const auto data = file.readAll();
    file.close();
    if (data.size() < kHeaderSize || data.left(5) != QByteArray("MLKF\1", 5))
        return fail(error, QStringLiteral("故障日志格式无效"));
    const auto salt = data.mid(6, 16);
    const auto header = fileHeader(salt);
    const auto key = deriveKey(password, salt);
    if (key.size() != 32) return fail(error, QStringLiteral("故障日志口令不可用"));
    int offset = kHeaderSize;
    while (offset + 4 <= data.size()) {
        const quint32 length = rd32(data.constData() + offset);
        if (length < 13 || length > quint32(kMaxRecordBytes) || offset + 4 + int(length) > data.size())
            return fail(error, QStringLiteral("故障日志记录长度无效"));
        const auto nonce = data.mid(offset + 4, 12);
        const auto sealed = data.mid(offset + 16, int(length) - 12);
        QByteArray plain;
        if (!crypto::open(key, nonce, header, sealed, plain))
            return fail(error, QStringLiteral("故障日志认证失败"));
        const auto doc = QJsonDocument::fromJson(plain);
        if (!doc.isObject()) return fail(error, QStringLiteral("故障日志记录无效"));
        records.append(doc.object());
        offset += 4 + int(length);
    }
    return true;
}

int FaultJournal::prune(const QString &directory, int retentionMonths, qint64 minFreeMb,
                        QString *error)
{
    QDir dir(directory);
    auto files = dir.entryInfoList({QStringLiteral("fault_*.mlkf")}, QDir::Files, QDir::Time);
    int removed = 0;
    QStorageInfo storage(dir.absolutePath());
    while (!files.isEmpty()) {
        storage.refresh();
        const auto file = files.last();
        const bool expired = retentionMonths > 0
            && file.lastModified().addMonths(retentionMonths) < QDateTime::currentDateTime();
        const bool tooMany = files.size() > kMaxFiles;
        const bool low = storage.isValid() && minFreeMb > 0
            && storage.bytesAvailable() < minFreeMb * 1024 * 1024;
        if (!expired && !tooMany && !low) break;
        if (!QFile::remove(file.absoluteFilePath()))
            return fail(error, QStringLiteral("无法清理故障日志 %1").arg(file.fileName()));
        files.removeLast();
        ++removed;
    }
    return removed;
}

} // namespace mlk
