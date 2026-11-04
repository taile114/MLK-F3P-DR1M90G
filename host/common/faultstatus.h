#pragma once

#include "faultresult.h"
#include "../remote-pc/src/frame.h"

#include <QJsonObject>
#include <QJsonValue>
#include <limits>

/* UDP v6/type8 完整故障状态（schema mlk.fault.status.v1）。摘要独立于
 * BoardStatus v4/type7 中的 modelFault 粗分类，二者不能互相覆盖。
 * 帧头固定 28 字节，tsUs 为最后已处理窗口的 endUs，无窗口为 0。
 * 版本/类型常量与帧构造见 frame.h（kFaultStatusVersion / kTypeFaultStatus）。 */
namespace mlk {

/* 板端与 PC 端都按该门槛判定状态过期；模型窗口 seq 与包传输序号分开。 */
constexpr qint64 kFaultStatusMaxAgeMs = kFaultResultMaxAgeMs;
constexpr qint64 kFaultStatusLinkTimeoutMs = 5000;

inline QJsonObject faultStatusJson(const FaultResult &result, qint64 ageMs, bool alarmConfirmed)
{
    QJsonObject status;
    writeFaultResultFields(status, result);
    status.insert(QStringLiteral("schema"), QString::fromLatin1(kFaultStatusSchema));
    status.insert(QStringLiteral("ageMs"),
                  ageMs < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(double(ageMs)));
    status.insert(QStringLiteral("alarmConfirmed"), alarmConfirmed);
    return status;
}

inline bool readFaultStatus(const QJsonObject &object, FaultResult &result, qint64 *ageMs,
                            bool *alarmConfirmed, QString *error)
{
    const auto schema = object.value(QStringLiteral("schema"));
    if (!schema.isString() || schema.toString() != QLatin1String(kFaultStatusSchema)) {
        if (error) *error = QStringLiteral("状态摘要 schema 不符");
        return false;
    }
    const auto age = object.value(QStringLiteral("ageMs"));
    qint64 parsedAge = -1;
    if (age.isDouble()) {
        quint64 value = 0;
        if (!faultUnsignedValue(age, 253402300799999999ull, &value)
            || value > quint64(std::numeric_limits<qint64>::max())) {
            if (error) *error = QStringLiteral("ageMs 字段无效");
            return false;
        }
        parsedAge = qint64(value);
    } else if (!age.isNull()) {
        if (error) *error = QStringLiteral("ageMs 字段无效");
        return false;
    }
    const auto confirmed = object.value(QStringLiteral("alarmConfirmed"));
    if (!confirmed.isBool()) {
        if (error) *error = QStringLiteral("alarmConfirmed 字段无效");
        return false;
    }
    FaultResult parsed;
    if (!readFaultResultFields(object, parsed, error)) return false;
    if (!parsed.validate(error)) return false;
    result = parsed;
    if (ageMs) *ageMs = parsedAge;
    if (alarmConfirmed) *alarmConfirmed = confirmed.toBool();
    return true;
}

inline bool faultResultSameWindow(const FaultResult &a, const FaultResult &b)
{
    return a.run == b.run && a.seq == b.seq;
}

inline bool faultResultNewerWindow(const FaultResult &incoming, const FaultResult &current)
{
    return incoming.run == current.run && incoming.seq > current.seq;
}

} // namespace mlk
