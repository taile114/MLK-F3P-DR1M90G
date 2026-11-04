#pragma once
#include "workorders.h"
#include "faultresult.h"
#include <QJsonObject>
#include <cmath>

namespace mlk {
// Local model API v1. RUL regression has no classification validity and never
// enters this path. grade=0 means an ungraded detection: alarm uses grade 1.
inline bool validModelFaultSummary(const QJsonObject &o) {
    if (o.value("api").toDouble(-1)!=1 || !o.value("valid").isBool()) return false;
    for (const auto &key : {"class","grade"}) {
        const auto v=o.value(key);
        if (!v.isDouble() || std::floor(v.toDouble())!=v.toDouble()) return false;
    }
    const int cls=o.value("class").toInt(-1), grade=o.value("grade").toInt(-1);
    if (!o.value("valid").toBool() || cls==0) return cls==0 && grade==0;
    return cls>=1 && cls<=2 && grade>=0 && grade<=3;
}
inline bool validModelFault(const QJsonObject &o) {
    return validModelFaultSummary(o) && (!o.value("valid").toBool()
        || o.value("class").toInt()==0 || WorkOrders::valid(o));
}
// 模型诊断 API 2（schema mlk.fault.result.v2）的入口校验，直接复用结果校验器，
// 供 Socket api=2 分支使用；v1 粗分类语义不变。
inline bool validModelFaultV2(const QJsonObject &o) {
    FaultResult result;
    return FaultResult::fromJson(o, result, nullptr);
}
inline QJsonObject modelFaultSummary(const QJsonObject &o) {
    return {{"api",1},{"valid",o.value("valid")},{"class",o.value("class")},{"grade",o.value("grade")}};
}
inline QJsonObject unknownModelFault() {
    return {{"api",1},{"valid",false},{"class",0},{"grade",0}};
}
inline int modelAlarmGrade(const QJsonObject &o) {
    return o.value("valid").toBool() && o.value("class").toInt()>0
        ? qMax(1,o.value("grade").toInt()) : 0;
}
}
