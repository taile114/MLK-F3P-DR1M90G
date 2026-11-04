#pragma once

#include <QByteArray>
#include "npustats.h"

namespace mlk {

class NpuMonitor
{
public:
    NpuStats sample(bool hardReady, bool softReady, bool cmaReady,
                    const QByteArray &interrupts, qint64 nowMs);
    void reset();

private:
    NpuStats m_previous;
    qint64 m_previousMs = -1;
};

} // namespace mlk
