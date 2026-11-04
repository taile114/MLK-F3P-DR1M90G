#include "inferencejournal.h"
#include "crypto.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QStorageInfo>
#include <QUuid>
#include <openssl/evp.h>

namespace mlk {
namespace {
QByteArray passwordKey(const QString &password, const QByteArray &salt) {
    QByteArray key(32, '\0'); const auto p = password.toUtf8();
    if (PKCS5_PBKDF2_HMAC(p.constData(), p.size(), reinterpret_cast<const uchar *>(salt.constData()),
        salt.size(), 100000, EVP_sha256(), key.size(), reinterpret_cast<uchar *>(key.data())) != 1) return {};
    return key;
}
bool fail(QString *error, const QString &text) { if (error) *error = text; return false; }
}
bool InferenceJournal::save(const Frame &frame, const QString &directory, bool encrypt,
    const QString &password, int retentionMonths, int minFreeMb, QString *error) {
    if (frame.ver != 3 || !(frame.valid & kValidRul)) return fail(error, QStringLiteral("无有效寿命结果"));
    QDir dir(QDir(directory).filePath(QStringLiteral("rul")));
    const bool existed=dir.exists();
    if (!dir.mkpath(QStringLiteral("."))) return fail(error, QStringLiteral("无法创建寿命记录目录"));
    // Qt's Time comparator repeatedly converts mtimes to local time while
    // sorting. Keep numeric mtimes and an incremental index instead: all of
    // this runs in the same event loop that receives the live UDP stream.
    if (!existed || m_indexDirectory!=dir.absolutePath() || !m_indexAge.isValid()
        || m_indexAge.elapsed()>=30000) {
        m_records.clear();m_indexDirectory=dir.absolutePath();
        for (const auto &file:dir.entryInfoList({QStringLiteral("rul_*.mlkr")},QDir::Files,QDir::NoSort))
            m_records.insert(file.lastModified().toMSecsSinceEpoch(),file.absoluteFilePath());
        m_indexAge.restart();
    }
    QStorageInfo disk(dir.absolutePath());
    const auto now=QDateTime::currentDateTimeUtc();
    while (!m_records.isEmpty()) {
        const auto old=m_records.cbegin();
        const bool expired = retentionMonths > 0
            && QDateTime::fromMSecsSinceEpoch(old.key(),Qt::UTC).addMonths(retentionMonths)<now;
        disk.refresh();
        if (!expired && m_records.size() < 10000 && (!disk.isValid() || disk.bytesAvailable() >= qint64(qMax(0, minFreeMb)) * 1024 * 1024)) break;
        const auto path=old.value();
        if (QFile::exists(path) && !QFile::remove(path)) return fail(error, QStringLiteral("无法清理寿命记录"));
        m_records.erase(m_records.begin());
    }
    disk.refresh();
    if (disk.isValid() && disk.bytesAvailable() < qint64(qMax(0, minFreeMb)) * 1024 * 1024)
        return fail(error, QStringLiteral("寿命记录可用空间不足"));
    if (encrypt && (m_key.isEmpty() || password != m_password)) {
        m_salt = crypto::randomBytes(16); m_key = passwordKey(password, m_salt); m_password = password;
        if (m_salt.size() != 16 || m_key.size() != 32) return fail(error, QStringLiteral("寿命记录密钥派生失败"));
    }
    auto salt = encrypt ? m_salt : QByteArray(16, '\0');
    auto nonce = crypto::randomBytes(12);
    if (nonce.size() != 12) return fail(error, QStringLiteral("寿命记录随机数不可用"));
    QByteArray header("MLKR", 4); header.append(char(1)); header.append(char(encrypt ? 1 : 0));
    header += salt; header += nonce;
    const auto plain = buildInferenceFrame(frame);
    const auto body = encrypt ? crypto::seal(m_key, nonce, header, plain) : plain;
    if (body.isEmpty()) return fail(error, QStringLiteral("寿命记录加密失败"));
    const auto name = QStringLiteral("rul_%1_%2_%3.mlkr").arg(frame.tsUs).arg(frame.windowIndex)
        .arg(QUuid::createUuid().toString().mid(1, 36));
    QSaveFile file(dir.filePath(name));
    if (!file.open(QIODevice::WriteOnly) || file.write(header + body) != header.size() + body.size() || !file.commit())
        return fail(error, file.errorString());
    m_records.insert(QFileInfo(file.fileName()).lastModified().toMSecsSinceEpoch(),file.fileName());
    return true;
}
bool InferenceJournal::load(const QString &path, const QString &password, Frame &frame, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4096) return fail(error, QStringLiteral("寿命记录不可读"));
    const auto data = file.readAll();
    if (data.size() < 34 || data.left(4) != "MLKR" || data[4] != 1 || (data[5] != 0 && data[5] != 1))
        return fail(error, QStringLiteral("寿命记录格式错误"));
    QByteArray plain;
    if (data[5] == 1) {
        const auto key = passwordKey(password, data.mid(6, 16));
        if (key.isEmpty() || !crypto::open(key, data.mid(22, 12), data.left(34), data.mid(34), plain))
            return fail(error, QStringLiteral("寿命记录口令或认证失败"));
    } else plain = data.mid(34);
    if (!parseFrame(plain, frame) || frame.ver != 3 || !(frame.valid & kValidRul))
        return fail(error, QStringLiteral("寿命记录载荷无效"));
    return true;
}
}
