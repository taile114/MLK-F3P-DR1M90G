#pragma once

#include <QtGlobal>

namespace mlk {

/* PL 100 MHz sample timestamps -> UTC. Anchor once, then advance by PL ticks;
 * polling jitter must not create sample gaps. UTC steps establish a new anchor. */
class PlSampleClock {
public:
    void reset() { m_ready = false; }
    quint64 timestamp(quint32 sample, quint32 now, quint64 utcUs, qint64 monotonicUs) {
        const quint64 ageTicks = quint32(now - sample);
        if (ageTicks > 1000000000ull) return 0; // Reject samples older than 10 s.
        const quint64 deltaTicks = quint32(now - m_lastNow);
        const qint64 elapsed = monotonicUs - m_lastMonotonic;
        const bool clockStep = m_ready
            && qAbs(qint64(utcUs) - qint64(m_utc + (m_ticks + deltaTicks) / 100)) > 100000;
        const bool restart = !m_ready || elapsed < 0 || elapsed >= 42949672ll
            || qAbs(qint64(deltaTicks / 100) - elapsed) > 1000000ll || clockStep;
        if (restart) { m_utc = utcUs; m_ticks = 0; m_ready = true; }
        else m_ticks += deltaTicks;
        m_lastNow = now;
        m_lastMonotonic = monotonicUs;
        const quint64 end = m_utc + m_ticks / 100;
        const quint64 ageUs = ageTicks / 100;
        return end > ageUs ? end - ageUs : 0;
    }
private:
    bool m_ready = false;
    quint32 m_lastNow = 0;
    quint64 m_utc = 0, m_ticks = 0;
    qint64 m_lastMonotonic = 0;
};

} // namespace mlk
