#pragma once

#include "capturestore.h"
#include "faultresult.h"
#include <QDir>
#include <QFileInfo>
#include <QPair>

namespace mlk {

/* Only new segments registered during an opted-in diagnosis session are eligible.
 * Unknown or abnormal windows always win over overlapping normal predictions. */
class FaultRecordPolicy {
    using Range = QPair<quint64, quint64>;
public:
    void begin(const QString &directory, int segmentSeconds, int contextSeconds = 10) {
        end(); m_active = true; m_directory = QDir(directory).absolutePath();
        m_context = quint64(qMax(0, contextSeconds)) * 1000000;
        m_history = quint64(qMax(1, segmentSeconds)) * 1000000 + 2*m_context + 2*kFaultWindowUs;
    }
    void end() {
        m_active = false; m_saved.clear(); m_normal.clear(); m_protected.clear(); m_latestStart = 0;
    }
    bool active() const { return m_active; }
    QString directory() const { return m_directory; }
    void saved(const CaptureEntry &entry) {
        if (!m_active || entry.endUs <= entry.startUs
            || QFileInfo(entry.path).absolutePath() != m_directory) return;
        auto absolute = entry; absolute.path = QFileInfo(entry.path).absoluteFilePath();
        m_saved.append(absolute);
        if (m_saved.size() > 4096) m_saved.removeFirst(); // Forgetting retains data.
    }
    void observe(const FaultResult &result, bool sensorAlarm = false) {
        if (!m_active || !result.startUs || result.endUs <= result.startUs) return;
        if (!result.valid && result.state != kFaultDataInsufficient && result.state != kFaultError) return;
        if (m_latestStart && result.startUs < m_latestStart) {
            // A changed timebase cannot authorize deletion of the previous epoch's files.
            m_saved.clear(); m_normal.clear(); m_protected.clear();
        }
        m_latestStart = result.startUs;
        if (result.valid && result.classIndex == kFaultNormal && !sensorAlarm)
            append(m_normal, result.startUs, result.endUs);
        else
            append(m_protected, result.startUs > m_context ? result.startUs-m_context : 0,
                   result.endUs+m_context);
        const auto cutoff = m_latestStart > m_history ? m_latestStart-m_history : 0;
        for (auto *ranges : {&m_normal, &m_protected}) {
            while (!ranges->isEmpty() && ranges->first().second < cutoff) ranges->removeFirst();
        }
    }
    bool hasMatureSegments() const {
        if (!m_active || m_latestStart <= m_context) return false;
        for (const auto &entry : m_saved)
            if (entry.endUs <= m_latestStart-m_context) return true;
        return false;
    }
    QStringList discardable(const QJsonArray &labels = {}) {
        QStringList paths;
        if (!m_active || m_latestStart <= m_context) return paths;
        const quint64 watermark = m_latestStart-m_context;
        QVector<CaptureEntry> pending;
        for (const auto &entry : m_saved) {
            if (entry.endUs > watermark) { pending.append(entry); continue; }
            bool protectedRange = false;
            if (!CaptureStore::matchingLabels(labels,entry.startUs,entry.endUs,true).isEmpty()) protectedRange = true;
            for (const auto &range : m_protected)
                if (range.first < entry.endUs && range.second > entry.startUs) { protectedRange = true; break; }
            if (!protectedRange) for (const auto &range : m_normal)
                if (range.first <= entry.startUs && range.second >= entry.endUs) {
                    paths.append(entry.path); break;
                }
            // An old unclassified or protected segment is retained and no longer tracked.
        }
        m_saved.swap(pending);
        return paths;
    }
private:
    static void append(QVector<Range> &ranges, quint64 begin, quint64 end) {
        if (!ranges.isEmpty() && begin <= ranges.last().second) {
            ranges.last().first = qMin(ranges.last().first, begin);
            ranges.last().second = qMax(ranges.last().second, end);
        } else ranges.append(qMakePair(begin, end));
    }
    bool m_active = false;
    QString m_directory;
    quint64 m_context = 10000000, m_history = 0, m_latestStart = 0;
    QVector<CaptureEntry> m_saved;
    QVector<Range> m_normal, m_protected;
};

} // namespace mlk
