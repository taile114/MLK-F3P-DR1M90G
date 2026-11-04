#include "workorders.h"
#include "crypto.h"
#include "faulttypes.h"
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonValue>
#include <QRegularExpression>
#include <openssl/evp.h>
#include <cmath>

namespace mlk {
namespace {
bool safeId(const QString &id) {
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
    return re.match(id).hasMatch();
}
bool integral(const QJsonValue &value, int low, int high, int *out) {
    if (!value.isDouble()) return false;
    const double d = value.toDouble();
    if (!std::isfinite(d) || std::floor(d) != d || d < double(low) || d > double(high)) return false;
    *out = int(d);
    return true;
}
/* 规范十进制无符号时间串：只接受无前导零、无符号、无空白的文本。 */
bool canonicalTime(const QJsonValue &value, quint64 *out) {
    if (!value.isString()) return false;
    const auto text = value.toString();
    if (text.isEmpty() || text.size() > 20) return false;
    bool ok = false;
    const quint64 parsed = text.toULongLong(&ok);
    if (!ok || parsed > 253402300799999999ULL || QString::number(parsed) != text) return false;
    *out = parsed;
    return true;
}
bool positiveSequence(const QJsonValue &value) {
    if (!value.isDouble()) return false;
    const double d = value.toDouble();
    return std::isfinite(d) && std::floor(d) == d && d >= 1 && d <= 4294967295.0;
}
/* v2 顶层保留 v1 的 id/faultTimeUs/class/grade/message，diagnosis 是首次确认窗口
 * 快照；未识别的额外字段不参与校验，保存与加载保持整份对象。 */
bool validOrderV2(const QJsonObject &o) {
    const auto schema = o.value("schema");
    if (!schema.isString() || schema.toString() != QLatin1String(kFaultOrderSchema)) return false;
    const auto diagnosisValue = o.value("diagnosis");
    if (!diagnosisValue.isObject()) return false;
    const auto diagnosis = diagnosisValue.toObject();
    int grade = 0, legacyClass = 0, classIndex = 0;
    if (!integral(o.value("grade"), 1, 3, &grade)) return false;
    if (!integral(o.value("class"), 1, 2, &legacyClass)
        || legacyClass != faultLegacyClass(grade)) return false;
    if (!integral(diagnosis.value("classIndex"), kFaultOverload, kFaultMountTilt, &classIndex))
        return false;
    const auto fault = diagnosis.value("fault");
    if (!fault.isString() || fault.toString() != faultName(classIndex)) return false;
    const auto model = diagnosis.value("model");
    const auto preprocess = diagnosis.value("preprocess");
    if (!model.isString() || model.toString().isEmpty()
        || !asciiWithin(model.toString(), kFaultIdentityMaxBytes)) return false;
    if (!preprocess.isString() || preprocess.toString().isEmpty()
        || !asciiWithin(preprocess.toString(), kFaultIdentityMaxBytes)) return false;
    const auto run = diagnosis.value("run");
    if (!run.isString() || !validUuid(run.toString())) return false;
    if (!positiveSequence(diagnosis.value("seq"))) return false;
    quint64 startUs = 0, endUs = 0, eventUs = 0;
    if (!canonicalTime(diagnosis.value("startUs"), &startUs)) return false;
    if (!canonicalTime(diagnosis.value("endUs"), &endUs)) return false;
    if (endUs <= startUs || endUs - startUs != kFaultWindowUs) return false;
    const auto confidence = diagnosis.value("confidence");
    if (!confidence.isDouble() || !std::isfinite(confidence.toDouble())
        || confidence.toDouble() < 0 || confidence.toDouble() > 1) return false;
    if (!canonicalTime(o.value("faultTimeUs"), &eventUs) || !eventUs || eventUs != endUs) return false;
    const auto message = o.value("message");
    if (!message.isString() || message.toString().toUtf8().size() > 128) return false;
    return QJsonDocument(o).toJson(QJsonDocument::Compact).size() <= 1000;
}
/* v1：无 schema 的旧工单，保持既有字段与长度校验。 */
bool validOrderV1(const QJsonObject &o) {
    bool ok = false; const auto ts = o.value("faultTimeUs").toString().toULongLong(&ok);
    const int cls = o.value("class").toInt(-1), grade = o.value("grade").toInt(-1);
    if (!o.value("class").isDouble() || !o.value("grade").isDouble()
        || std::floor(o.value("class").toDouble()) != o.value("class").toDouble()
        || std::floor(o.value("grade").toDouble()) != o.value("grade").toDouble()) return false;
    return ok && ts > 0 && ts <= 253402300799999999ULL
        && cls >= 1 && cls <= 2 && grade >= 0 && grade <= 255
        && o.value("message").isString() && o.value("message").toString().toUtf8().size() <= 512
        && QJsonDocument(o).toJson(QJsonDocument::Compact).size() <= 1000;
}
QByteArray keyFor(const QString &password, const QByteArray &salt) {
    QByteArray key(32, '\0'); const auto p = password.toUtf8();
    if (PKCS5_PBKDF2_HMAC(p.constData(), p.size(), reinterpret_cast<const uchar *>(salt.constData()),
        salt.size(), 100000, EVP_sha256(), 32, reinterpret_cast<uchar *>(key.data())) != 1) return {};
    return key;
}
bool fail(QString *error, const QString &message) { if (error) *error = message; return false; }
}
WorkOrders::WorkOrders(const QString &directory, const QString &password)
    : m_directory(QDir(directory).filePath(QStringLiteral("orders"))), m_password(password) {}
bool WorkOrders::valid(const QJsonObject &o) {
    if (!safeId(o.value("id").toString())) return false;
    return o.contains("schema") ? validOrderV2(o) : validOrderV1(o);
}
bool WorkOrders::save(const QJsonObject &o, QString *error) const {
    if (!valid(o)) return fail(error, QStringLiteral("工单字段无效"));
    QDir dir(m_directory);
    if (!dir.mkpath(".")) return fail(error, QStringLiteral("无法创建工单目录"));
    const auto id = o.value("id").toString();
    const auto path = dir.filePath(id + ".mlko");
    if (QFile::exists(path)) {
        QJsonObject previous;
        return load(id, previous, error) && (previous == o || fail(error, QStringLiteral("工单 ID 内容冲突")));
    }
    const auto salt = crypto::randomBytes(16), nonce = crypto::randomBytes(12);
    const auto key = keyFor(m_password, salt);
    if (salt.size() != 16 || nonce.size() != 12 || key.size() != 32) return fail(error, QStringLiteral("工单密钥不可用"));
    const auto header = QByteArray("MLKO\1", 5) + salt + nonce;
    const auto cipher = crypto::seal(key, nonce, header, QJsonDocument(o).toJson(QJsonDocument::Compact));
    if (cipher.isEmpty()) return fail(error, QStringLiteral("工单加密失败"));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return fail(error, file.errorString());
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const auto data = header + cipher;
    if (file.write(data) != data.size() || !file.commit()) return fail(error, file.errorString());
    return true;
}
bool WorkOrders::load(const QString &id, QJsonObject &o, QString *error) const {
    if (!safeId(id)) return fail(error, QStringLiteral("工单 ID 无效"));
    QFile file(QDir(m_directory).filePath(id + ".mlko"));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4096) return fail(error, QStringLiteral("工单不可读"));
    const auto data = file.read(4097);
    if (data.size() > 4096) return fail(error, QStringLiteral("工单不可读"));
    if (const auto *cached = m_loadCache.object(id)) {
        if (cached->bytes == data) {
            if (!cached->valid) return fail(error, QStringLiteral("工单口令或认证失败"));
            o = cached->object;
            return true;
        }
    }
    auto *cached = new CachedOrder;
    cached->bytes = data;
    QByteArray plain;
    if (data.size() >= 49 && data.left(5) == QByteArray("MLKO\1", 5)
        && crypto::open(keyFor(m_password, data.mid(5,16)), data.mid(21,12), data.left(33), data.mid(33), plain)) {
        cached->object = QJsonDocument::fromJson(plain).object();
        cached->valid = valid(cached->object) && cached->object.value("id").toString() == id;
    }
    const bool ok = cached->valid;
    if (ok) o = cached->object;
    m_loadCache.insert(id, cached);
    return ok || fail(error, QStringLiteral("工单口令或认证失败"));
}
QStringList WorkOrders::pending() const {
    QDir dir(m_directory); QStringList ids;
    for (const auto &f : dir.entryInfoList({"*.mlko"}, QDir::Files, QDir::Time | QDir::Reversed))
        if (!QFile::exists(f.absoluteFilePath() + ".ack")) ids << f.completeBaseName();
    return ids;
}
bool WorkOrders::acknowledge(const QString &id) const {
    if (!safeId(id) || !QFile::exists(QDir(m_directory).filePath(id + ".mlko"))) return false;
    QSaveFile file(QDir(m_directory).filePath(id + ".mlko.ack"));
    return file.open(QIODevice::WriteOnly) && file.write("ACK\n") == 4 && file.commit();
}
QJsonObject WorkOrders::latest() const {
    QJsonObject newest;
    for (const auto &f : QDir(m_directory).entryInfoList({"*.mlko"}, QDir::Files)) {
        QJsonObject o;
        if (load(f.completeBaseName(), o) && o.value("faultTimeUs").toString().toULongLong()
            > newest.value("faultTimeUs").toString().toULongLong()) newest = o;
    }
    return newest;
}
}
