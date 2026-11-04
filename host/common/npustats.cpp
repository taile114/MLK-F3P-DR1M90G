#include "npustats.h"

#include <QJsonValue>
#include <QStringList>
#include <cmath>

namespace mlk {
namespace {
QString activity(const QString &name, bool ready, bool available, quint64 total, double rate)
{
    if (!ready) return name + QStringLiteral("：设备未就绪");
    if (!available) return name + QStringLiteral("：中断统计不可读");
    if (rate < 0) return QStringLiteral("%1：累计 %2 次，等待采样").arg(name).arg(total);
    return QStringLiteral("%1：%2 次/s，累计 %3 次").arg(name).arg(rate, 0, 'f', 1).arg(total);
}
} // namespace

QString NpuStats::text() const
{
    QStringList missing;
    if (!hardReady) missing << QStringLiteral("硬核");
    if (!softReady) missing << QStringLiteral("软核");
    if (!cmaReady) missing << QStringLiteral("CMA");
    QString state;
    if (!missing.isEmpty())
        state = QStringLiteral("未就绪（缺少 %1）").arg(missing.join(QStringLiteral("、")));
    else if (hardInterruptsPerSec > 0 || softInterruptsPerSec > 0)
        state = QStringLiteral("驱动就绪 · 检测到中断活动");
    else if (!hardIrqAvailable || !softIrqAvailable)
        state = QStringLiteral("驱动就绪 · 活动统计受限");
    else if (hardInterruptsPerSec < 0 || softInterruptsPerSec < 0)
        state = QStringLiteral("驱动就绪 · 等待活动采样");
    else
        state = QStringLiteral("驱动就绪 · 本周期无中断");
    return state + QLatin1Char('\n')
        + activity(QStringLiteral("硬核"), hardReady, hardIrqAvailable, hardInterrupts, hardInterruptsPerSec)
        + QLatin1Char('\n')
        + activity(QStringLiteral("软核"), softReady, softIrqAvailable, softInterrupts, softInterruptsPerSec)
        + QStringLiteral("\n使用率：驱动未提供");
}

QJsonObject NpuStats::toJson() const
{
    return {{"version", 1}, {"hardReady", hardReady}, {"softReady", softReady},
        {"cmaReady", cmaReady}, {"hardIrqAvailable", hardIrqAvailable},
        {"softIrqAvailable", softIrqAvailable},
        {"hardInterrupts", QString::number(hardInterrupts)},
        {"softInterrupts", QString::number(softInterrupts)},
        {"hardInterruptsPerSec", hardInterruptsPerSec < 0 ? QJsonValue() : QJsonValue(hardInterruptsPerSec)},
        {"softInterruptsPerSec", softInterruptsPerSec < 0 ? QJsonValue() : QJsonValue(softInterruptsPerSec)}};
}

bool NpuStats::fromJson(const QJsonObject &o, NpuStats &stats)
{
    if (o.value("version") != QJsonValue(1)) return false;
    for (const char *key : {"hardReady", "softReady", "cmaReady", "hardIrqAvailable", "softIrqAvailable"})
        if (!o.value(key).isBool()) return false;
    NpuStats parsed;
    parsed.hardReady = o.value("hardReady").toBool();
    parsed.softReady = o.value("softReady").toBool();
    parsed.cmaReady = o.value("cmaReady").toBool();
    parsed.hardIrqAvailable = o.value("hardIrqAvailable").toBool();
    parsed.softIrqAvailable = o.value("softIrqAvailable").toBool();
    auto counter = [&o](const char *key, quint64 &value) {
        const auto json = o.value(key);
        const auto text = json.toString();
        if (!json.isString() || text.isEmpty()) return false;
        for (QChar c : text) if (c < QLatin1Char('0') || c > QLatin1Char('9')) return false;
        bool ok = false;
        value = text.toULongLong(&ok);
        return ok;
    };
    auto rate = [&o](const char *key, double &value) {
        const auto json = o.value(key);
        if (json.isNull()) { value = -1; return true; }
        value = json.toDouble(-1);
        return json.isDouble() && std::isfinite(value) && value >= 0;
    };
    if (!counter("hardInterrupts", parsed.hardInterrupts) || !counter("softInterrupts", parsed.softInterrupts)
        || !rate("hardInterruptsPerSec", parsed.hardInterruptsPerSec)
        || !rate("softInterruptsPerSec", parsed.softInterruptsPerSec)
        || (parsed.hardIrqAvailable && !parsed.hardReady)
        || (parsed.softIrqAvailable && !parsed.softReady)
        || (parsed.hardInterruptsPerSec >= 0 && !parsed.hardIrqAvailable)
        || (parsed.softInterruptsPerSec >= 0 && !parsed.softIrqAvailable)) return false;
    stats = parsed;
    return true;
}

} // namespace mlk
