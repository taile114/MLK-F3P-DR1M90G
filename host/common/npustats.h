#pragma once

#include <QJsonObject>
#include <QString>

namespace mlk {

struct NpuStats
{
    bool hardReady = false;
    bool softReady = false;
    bool cmaReady = false;
    bool hardIrqAvailable = false;
    bool softIrqAvailable = false;
    quint64 hardInterrupts = 0;
    quint64 softInterrupts = 0;
    // Negative means no comparable sample; interrupt rate is not utilization.
    double hardInterruptsPerSec = -1.0;
    double softInterruptsPerSec = -1.0;

    QString text() const;
    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject &object, NpuStats &stats);
};

} // namespace mlk
