#pragma once

#include <QtGlobal>
#include <limits>

namespace mlk {
// ADS1115 PGA +/-4.096 V: 125 uV/LSB. ACS712-20A: 100 mV/A,
// nominal zero is VCC/2 = 2.5 V at a 5 V supply.
constexpr qint32 kCurrentZeroDefault = 20000;
constexpr qint32 kCurrentGainDefault = 1250; // uA/LSB

inline qint32 currentMilliAmp(qint16 raw, qint32 zero, qint32 gainUaPerLsb)
{
    const qint64 ua = (qint64(raw) - zero) * gainUaPerLsb;
    const qint64 ma = ua >= 0 ? (ua + 500) / 1000 : -((-ua + 500) / 1000);
    return qint32(qBound<qint64>(std::numeric_limits<qint32>::min(), ma,
                                std::numeric_limits<qint32>::max()));
}

inline qint32 displaySpeedDeciRpm(qint32 plMilliRpm)
{
    const qint64 milli = -qint64(plMilliRpm); // installed encoder orientation
    return qint32(milli >= 0 ? (milli + 50) / 100 : -((-milli + 50) / 100));
}
}
