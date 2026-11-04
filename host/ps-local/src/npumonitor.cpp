#include "npumonitor.h"

#include <QRegularExpression>
#include <QStringList>
#include <limits>

namespace mlk {
namespace {
void readInterrupts(const QByteArray &data, NpuStats &stats)
{
    const auto lines = QString::fromLatin1(data).split(QLatin1Char('\n'));
    const QRegularExpression space(QStringLiteral("\\s+"));
    const QRegularExpression cpu(QStringLiteral("^CPU[0-9]+$"));
    int cpuCount = 0;
    for (const auto &line : lines) {
        const auto fields = line.trimmed().split(space, QString::SkipEmptyParts);
        if (fields.isEmpty() || !cpu.match(fields.first()).hasMatch()) continue;
        for (const auto &field : fields) {
            if (!cpu.match(field).hasMatch()) return;
            ++cpuCount;
        }
        break;
    }
    if (!cpuCount) return;

    // Match driver action names, never a fixed Linux IRQ number. The numeric
    // GIC interrupt ID after the CPU columns must not be counted as an event.
    const QRegularExpression hard(QStringLiteral("(?:^|[\\s,])(?:HARD_NPU_IRQ|hard_npu)(?:$|[\\s,])"),
                                  QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression soft(QStringLiteral("(?:^|[\\s,])(?:SOFT_NPU_IRQ|soft_npu)(?:$|[\\s,])"),
                                  QRegularExpression::CaseInsensitiveOption);
    for (const auto &line : lines) {
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon < 0) continue;
        bool numericIrq = false;
        line.left(colon).trimmed().toUInt(&numericIrq);
        if (!numericIrq) continue;
        const auto fields = line.mid(colon + 1).trimmed().split(space, QString::SkipEmptyParts);
        if (fields.size() <= cpuCount) continue;
        quint64 total = 0;
        bool valid = true;
        for (int i = 0; i < cpuCount; ++i) {
            bool ok = false;
            const auto count = fields[i].toULongLong(&ok);
            if (!ok || count > std::numeric_limits<quint64>::max() - total) { valid = false; break; }
            total += count;
        }
        if (!valid) continue;
        const auto action = fields.mid(cpuCount).join(QLatin1Char(' '));
        if (stats.hardReady && hard.match(action).hasMatch()) {
            stats.hardIrqAvailable = true;
            stats.hardInterrupts = total;
        }
        if (stats.softReady && soft.match(action).hasMatch()) {
            stats.softIrqAvailable = true;
            stats.softInterrupts = total;
        }
    }
}

} // namespace

NpuStats NpuMonitor::sample(bool hardReady, bool softReady, bool cmaReady,
                           const QByteArray &interrupts, qint64 nowMs)
{
    NpuStats stats;
    stats.hardReady = hardReady;
    stats.softReady = softReady;
    stats.cmaReady = cmaReady;
    readInterrupts(interrupts, stats);
    if (m_previousMs >= 0 && nowMs > m_previousMs) {
        const double seconds = double(nowMs - m_previousMs) / 1000.0;
        if (stats.hardIrqAvailable && m_previous.hardIrqAvailable
            && stats.hardInterrupts >= m_previous.hardInterrupts)
            stats.hardInterruptsPerSec = double(stats.hardInterrupts - m_previous.hardInterrupts) / seconds;
        if (stats.softIrqAvailable && m_previous.softIrqAvailable
            && stats.softInterrupts >= m_previous.softInterrupts)
            stats.softInterruptsPerSec = double(stats.softInterrupts - m_previous.softInterrupts) / seconds;
    }
    m_previous = stats;
    m_previousMs = nowMs;
    return stats;
}

void NpuMonitor::reset()
{
    m_previous = NpuStats{};
    m_previousMs = -1;
}

} // namespace mlk
