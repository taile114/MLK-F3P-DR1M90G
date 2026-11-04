#include "faultinputadapter.h"

#include <QUuid>

namespace mlk {

void FaultInputAdapter::configure(const AppConfig &config)
{
    const QString previous = m_scaleId;
    m_config = config;
    m_scaleId = QStringLiteral("k=%1,%2,%3,%4;b=%5,%6,%7,%8;cur=%9,%10;spd=0.1")
                    .arg(config.ch[0].k, 0, 'g', 10).arg(config.ch[1].k, 0, 'g', 10)
                    .arg(config.ch[2].k, 0, 'g', 10).arg(config.ch[3].k, 0, 'g', 10)
                    .arg(config.ch[0].b, 0, 'g', 10).arg(config.ch[1].b, 0, 'g', 10)
                    .arg(config.ch[2].b, 0, 'g', 10).arg(config.ch[3].b, 0, 'g', 10)
                    .arg(config.currentZero).arg(config.currentGain);
    if (previous != m_scaleId) m_sourceRun.clear();
}

QString FaultInputAdapter::restart()
{
    m_sourceRun = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_blockId = 0;
    for (int i = 0; i < kChannelCount; ++i) {
        m_haveSeq[i] = false;
        m_lastSeq[i] = 0;
        m_lastEndUs[i] = 0;
    }
    return m_sourceRun;
}

void FaultInputAdapter::noteGap(NativeBlock &block)
{
    const int ch = int(block.channel < quint32(kChannelCount) ? block.channel : 0);
    block.gapBefore = block.gapBefore
        || (m_haveSeq[ch] && m_lastEndUs[ch] && block.t0Us > m_lastEndUs[ch]
            && block.t0Us - m_lastEndUs[ch] > 3ull * 1000000ull / qMax(1u, block.rateHz));
    m_haveSeq[ch] = true;
    m_lastEndUs[ch] = block.endUs();
}

NativeBlock FaultInputAdapter::acceptFrame(const Frame &f)
{
    NativeBlock block;
    if (m_sourceRun.isEmpty()) restart();
    if (f.type != kTypeWaveform || f.ch >= kChannelCount || f.samples.isEmpty() || !f.rateHz
        || !f.tsUs)
        return block;
    const int ch = f.ch;
    const int axes = f.samplesY.size() == f.samples.size() && f.samplesZ.size() == f.samples.size()
        ? 3 : 1;
    if (axes == 3 && ch != kVibration) return block;

    block.sourceRun = m_sourceRun;
    block.blockId = ++m_blockId;
    block.channel = quint32(ch);
    block.t0Us = f.tsUs;
    block.rateHz = f.rateHz;
    block.count = quint32(f.samples.size());
    block.axes = quint32(axes);
    block.scaleId = m_scaleId;
    block.dataValid = true;
    block.hasDmaSeq = ch == kVibration && f.cnt > 0;
    block.dmaSeq = f.cnt;
    block.values.resize(int(block.count) * axes);

    const double k = m_config.ch[ch].k;
    const double b = m_config.ch[ch].b;
    auto saturated = [](qint32 raw) { return raw <= -32760 || raw >= 32760; };
    if (ch == kCurrent) {
        /* 电流 Frame 已是 mA；模型入口只要安培，不再重复应用 ADC 增益。 */
        for (quint32 i = 0; i < block.count; ++i) {
            block.values[int(i)] = float(double(f.samples[int(i)]) / 1000.0);
            if (saturated(f.samples[int(i)])) block.saturated = true;
        }
    } else if (ch == kVibration && axes == 3) {
        for (quint32 i = 0; i < block.count; ++i) {
            const qint32 raw[3] = {f.samples[int(i)], f.samplesY[int(i)], f.samplesZ[int(i)]};
            for (int a = 0; a < 3; ++a) {
                block.values[int(i * 3 + quint32(a))] = float(k * raw[a] + b);
                if (saturated(raw[a])) block.saturated = true;
            }
        }
    } else {
        for (quint32 i = 0; i < block.count; ++i) {
            const qint32 raw = f.samples[int(i)];
            block.values[int(i)] = float(k * raw + b);
            if (ch != kSpeed && saturated(raw)) block.saturated = true;
        }
    }

    /* 同一通道的 Frame 序号连续，跳号即说明中间块没有到达。 */
    if (m_haveSeq[ch] && f.seq != m_lastSeq[ch] + 1) block.gapBefore = true;
    noteGap(block);
    m_lastSeq[ch] = f.seq;
    return block;
}

NativeBlock FaultInputAdapter::acceptSpeed(qint32 deciRpm, quint32 count, quint64 t0Us,
                                           bool overflow)
{
    NativeBlock block;
    if (m_sourceRun.isEmpty()) restart();
    block.sourceRun = m_sourceRun;
    block.blockId = ++m_blockId;
    block.channel = kSpeed;
    block.t0Us = t0Us;
    block.rateHz = 10;   /* PL 100 ms 采样周期 */
    /* count is the cumulative PL SPEED_COUNT, not the length of this event. */
    Q_UNUSED(count);
    block.count = 1;
    block.axes = 1;
    block.scaleId = m_scaleId;
    block.dataValid = t0Us > 0;
    block.saturated = overflow;
    block.values.resize(int(block.count));
    for (quint32 i = 0; i < block.count; ++i)
        block.values[int(i)] = float(double(deciRpm) * 0.1);
    noteGap(block);
    return block;
}

} // namespace mlk
