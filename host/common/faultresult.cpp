#include "faultresult.h"

#include <QJsonValue>
#include <cmath>
#include <limits>

namespace mlk {
namespace {

bool fail(QString *error, const QString &text)
{
    if (error) *error = text;
    return false;
}

bool integral(const QJsonValue &value, qint64 low, qint64 high, qint64 *out)
{
    if (!value.isDouble()) return false;
    const double d = value.toDouble();
    if (!std::isfinite(d) || std::floor(d) != d || d < double(low) || d > double(high))
        return false;
    *out = qint64(d);
    return true;
}

bool identityText(const QJsonValue &value, QString *out)
{
    if (!value.isString()) return false;
    const QString text = value.toString();
    if (text.isEmpty() || !asciiWithin(text, kFaultIdentityMaxBytes)) return false;
    *out = text;
    return true;
}

} // namespace

bool faultUnsignedValue(const QJsonValue &value, quint64 maximum, quint64 *out)
{
    if (!value.isDouble()) return false;
    const double d = value.toDouble();
    if (!std::isfinite(d) || std::floor(d) != d || d < 0 || d > double(maximum)) return false;
    *out = quint64(d);
    return true;
}

bool faultTimeText(const QJsonValue &value, quint64 *out)
{
    if (!value.isString()) return false;
    const QString text = value.toString();
    if (text.isEmpty() || text.size() > 20) return false;
    bool ok = false;
    const quint64 parsed = text.toULongLong(&ok);
    if (!ok || parsed > 253402300799999999ull || QString::number(parsed) != text) return false;
    *out = parsed;
    return true;
}

QString faultTimeString(quint64 value) { return QString::number(value); }

void writeFaultResultFields(QJsonObject &o, const FaultResult &r)
{
    o.insert(QStringLiteral("task"), r.task);
    o.insert(QStringLiteral("model"), r.model);
    o.insert(QStringLiteral("preprocess"), r.preprocess);
    o.insert(QStringLiteral("run"), r.run);
    o.insert(QStringLiteral("seq"), double(r.seq));
    o.insert(QStringLiteral("startUs"), faultTimeString(r.startUs));
    o.insert(QStringLiteral("endUs"), faultTimeString(r.endUs));
    o.insert(QStringLiteral("state"), faultStateName(r.state));
    o.insert(QStringLiteral("valid"), r.valid);
    o.insert(QStringLiteral("classIndex"), r.classIndex);
    o.insert(QStringLiteral("fault"), r.fault());
    o.insert(QStringLiteral("grade"), r.grade);
    if (r.hasConfidence) o.insert(QStringLiteral("confidence"), r.confidence);
    else o.insert(QStringLiteral("confidence"), QJsonValue(QJsonValue::Null));
    o.insert(QStringLiteral("backend"), faultBackendName(r.backend));
    o.insert(QStringLiteral("preprocessUs"), double(r.preprocessUs));
    o.insert(QStringLiteral("inferUs"), double(r.inferUs));
    o.insert(QStringLiteral("reason"), r.reason);
    if (!r.quality.isEmpty()) o.insert(QStringLiteral("quality"), r.quality);
}

QJsonObject FaultResult::toJson() const
{
    QJsonObject o{{QStringLiteral("api"), api}, {QStringLiteral("schema"), schema}};
    writeFaultResultFields(o, *this);
    return o;
}

bool FaultResult::validate(QString *error) const
{
    if (task != QLatin1String(kFaultTask)) return fail(error, QStringLiteral("task 不符"));
    if (model.isEmpty() || !asciiWithin(model, kFaultIdentityMaxBytes))
        return fail(error, QStringLiteral("model 无效"));
    if (preprocess.isEmpty() || !asciiWithin(preprocess, kFaultIdentityMaxBytes))
        return fail(error, QStringLiteral("preprocess 无效"));
    if (!validUuid(run)) return fail(error, QStringLiteral("run 不是 UUID"));
    if (state < kFaultStopped || state >= kFaultStateCount)
        return fail(error, QStringLiteral("state 无效"));
    if (backend < kFaultBackendUnknown || backend > kFaultBackendCpuCnnNpu)
        return fail(error, QStringLiteral("backend 无效"));
    if (reason.size() > kFaultReasonMaxBytes || !asciiWithin(reason, kFaultReasonMaxBytes))
        return fail(error, QStringLiteral("reason 无效"));
    if (valid) {
        if (state != kFaultRunning) return fail(error, QStringLiteral("有效结果必须为 running"));
        if (!seq) return fail(error, QStringLiteral("有效结果缺少窗口序号"));
        if (endUs <= startUs || endUs - startUs != kFaultWindowUs)
            return fail(error, QStringLiteral("窗口不是四秒"));
        if (classIndex < kFaultNormal || classIndex >= kFaultClassCount)
            return fail(error, QStringLiteral("classIndex 超出五类"));
        if (!faultGradeValid(classIndex, grade)) return fail(error, QStringLiteral("等级与类别不符"));
        if (!hasConfidence || !std::isfinite(confidence) || confidence < 0 || confidence > 1)
            return fail(error, QStringLiteral("confidence 无效"));
        if (!reason.isEmpty()) return fail(error, QStringLiteral("有效结果不得带原因码"));
        if (backend == kFaultBackendUnknown) return fail(error, QStringLiteral("有效结果缺少后端"));
        return true;
    }
    if (classIndex != kFaultUnavailable || grade != -1 || hasConfidence)
        return fail(error, QStringLiteral("不可用结果必须清空类别、等级和分数"));
    if (reason.isEmpty()) return fail(error, QStringLiteral("不可用结果必须带原因码"));
    if (!seq) {
        if (startUs || endUs) return fail(error, QStringLiteral("尚无窗口时窗口时间必须为零"));
    } else if (endUs <= startUs || endUs - startUs != kFaultWindowUs) {
        return fail(error, QStringLiteral("最后窗口时间无效"));
    }
    return true;
}

bool readFaultResultFields(const QJsonObject &o, FaultResult &result, QString *error)
{
    FaultResult r;
    if (!identityText(o.value(QStringLiteral("model")), &r.model))
        return fail(error, QStringLiteral("model 字段无效"));
    if (!identityText(o.value(QStringLiteral("preprocess")), &r.preprocess))
        return fail(error, QStringLiteral("preprocess 字段无效"));
    const auto task = o.value(QStringLiteral("task"));
    if (!task.isString() || task.toString() != QLatin1String(kFaultTask))
        return fail(error, QStringLiteral("task 字段无效"));
    r.task = task.toString();
    const auto run = o.value(QStringLiteral("run"));
    if (!run.isString() || !validUuid(run.toString()))
        return fail(error, QStringLiteral("run 字段无效"));
    r.run = run.toString();
    quint64 number = 0;
    if (!faultUnsignedValue(o.value(QStringLiteral("seq")), std::numeric_limits<quint32>::max(), &number))
        return fail(error, QStringLiteral("seq 字段无效"));
    r.seq = quint32(number);
    if (!faultTimeText(o.value(QStringLiteral("startUs")), &r.startUs))
        return fail(error, QStringLiteral("startUs 字段无效"));
    if (!faultTimeText(o.value(QStringLiteral("endUs")), &r.endUs))
        return fail(error, QStringLiteral("endUs 字段无效"));
    const auto state = o.value(QStringLiteral("state"));
    if (!state.isString() || !faultStateFromName(state.toString(), &r.state))
        return fail(error, QStringLiteral("state 字段无效"));
    const auto valid = o.value(QStringLiteral("valid"));
    if (!valid.isBool()) return fail(error, QStringLiteral("valid 字段无效"));
    r.valid = valid.toBool();
    qint64 value = 0;
    if (!integral(o.value(QStringLiteral("classIndex")), kFaultUnavailable, kFaultClassCount - 1, &value))
        return fail(error, QStringLiteral("classIndex 字段无效"));
    r.classIndex = int(value);
    const auto fault = o.value(QStringLiteral("fault"));
    if (!fault.isString() || fault.toString() != faultName(r.classIndex))
        return fail(error, QStringLiteral("fault 与 classIndex 不一致"));
    if (!integral(o.value(QStringLiteral("grade")), -1, 3, &value))
        return fail(error, QStringLiteral("grade 字段无效"));
    r.grade = int(value);
    const auto confidence = o.value(QStringLiteral("confidence"));
    if (confidence.isNull() || confidence.isUndefined()) {
        r.hasConfidence = false;
    } else if (confidence.isDouble() && std::isfinite(confidence.toDouble())) {
        r.hasConfidence = true;
        r.confidence = confidence.toDouble();
    } else {
        return fail(error, QStringLiteral("confidence 字段无效"));
    }
    const auto backend = o.value(QStringLiteral("backend"));
    if (!backend.isString() || !faultBackendFromName(backend.toString(), &r.backend))
        return fail(error, QStringLiteral("backend 字段无效"));
    if (!faultUnsignedValue(o.value(QStringLiteral("preprocessUs")),
                            std::numeric_limits<quint32>::max(), &number))
        return fail(error, QStringLiteral("preprocessUs 字段无效"));
    r.preprocessUs = quint32(number);
    if (!faultUnsignedValue(o.value(QStringLiteral("inferUs")),
                            std::numeric_limits<quint32>::max(), &number))
        return fail(error, QStringLiteral("inferUs 字段无效"));
    r.inferUs = quint32(number);
    const auto reason = o.value(QStringLiteral("reason"));
    if (!reason.isString()) return fail(error, QStringLiteral("reason 字段无效"));
    r.reason = reason.toString();
    if (o.contains(QStringLiteral("quality"))) {
        if (!o.value(QStringLiteral("quality")).isObject())
            return fail(error, QStringLiteral("quality 字段无效"));
        r.quality = o.value(QStringLiteral("quality")).toObject();
    }
    result = r;
    return true;
}

bool FaultResult::fromJson(const QJsonObject &object, FaultResult &result, QString *error)
{
    quint64 api = 0;
    if (!faultUnsignedValue(object.value(QStringLiteral("api")), 2, &api) || api != 2)
        return fail(error, QStringLiteral("仅支持 API 2"));
    const auto schema = object.value(QStringLiteral("schema"));
    if (!schema.isString() || schema.toString() != QLatin1String(kFaultResultSchema))
        return fail(error, QStringLiteral("schema 不符"));
    FaultResult next;
    if (!readFaultResultFields(object, next, error)) return false;
    if (!next.validate(error)) return false;
    result = next;
    return true;
}

bool validFaultResultV2(const QJsonObject &object)
{
    FaultResult ignored;
    return FaultResult::fromJson(object, ignored, nullptr);
}

FaultResult FaultResult::unavailable(int state, const QString &reason, const QString &run)
{
    FaultResult r;
    r.state = state;
    r.reason = reason;
    r.run = run;
    r.valid = false;
    r.classIndex = kFaultUnavailable;
    r.grade = -1;
    r.hasConfidence = false;
    r.confidence = 0.0;
    r.seq = 0;
    r.startUs = 0;
    r.endUs = 0;
    return r;
}

} // namespace mlk
