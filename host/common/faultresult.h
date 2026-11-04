#pragma once

#include "faulttypes.h"

#include <QJsonObject>
#include <QString>

/* 模型诊断 API 2（schema mlk.fault.result.v2）。同进程工作线程与外部
 * /run/mlk-fault.sock 发布者共用这一份结构与校验器，不能各存一份状态。
 * 规则见 host/故障诊断模型接口需求.md §4。 */
namespace mlk {

struct FaultResult
{
    int api = 2;
    QString schema = QString::fromLatin1(kFaultResultSchema);
    QString task = QString::fromLatin1(kFaultTask);
    QString model;
    QString preprocess = QString::fromLatin1(kFaultPreprocess);
    QString run;
    quint32 seq = 0;
    quint64 startUs = 0;
    quint64 endUs = 0;
    int state = kFaultStopped;
    bool valid = false;
    int classIndex = kFaultUnavailable;
    int grade = -1;
    bool hasConfidence = false;
    double confidence = 0.0;
    int backend = kFaultBackendUnknown;
    quint32 preprocessUs = 0;
    quint32 inferUs = 0;
    QString reason;
    QJsonObject quality;

    QString fault() const { return faultName(classIndex); }
    bool alarmEligible() const
    {
        return valid && classIndex > kFaultNormal && classIndex < kFaultClassCount
            && hasConfidence && confidence >= kFaultAlarmMinConfidence && confidence <= 1.0;
    }
    /* 只看结果年龄；是否撤销告警由控制器决定。 */
    bool fresh(qint64 ageMs) const
    {
        return valid && ageMs >= 0 && ageMs <= kFaultResultMaxAgeMs;
    }
    QJsonObject toJson() const;
    bool validate(QString *error) const;
    static bool fromJson(const QJsonObject &object, FaultResult &result, QString *error);
    static FaultResult unavailable(int state, const QString &reason, const QString &run = QString());
};

/* 只读写公共字段，不检查 api/schema 外壳：API 2 与 UDP v6/type8 摘要复用。 */
bool readFaultResultFields(const QJsonObject &object, FaultResult &result, QString *error);
void writeFaultResultFields(QJsonObject &object, const FaultResult &result);

/* 工单/记录读取端的轻量判定：整体是合法 API 2 结果。 */
bool validFaultResultV2(const QJsonObject &object);

/* ASCII、长度与整数校验工具，供 faultstatus 复用。 */
bool faultUnsignedValue(const QJsonValue &value, quint64 maximum, quint64 *out);
bool faultTimeText(const QJsonValue &value, quint64 *out);
QString faultTimeString(quint64 value);

} // namespace mlk

#include <QMetaType>
Q_DECLARE_METATYPE(mlk::FaultResult)
