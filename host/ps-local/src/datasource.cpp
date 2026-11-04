#include "datasource.h"

#include <QDateTime>
#include <QDebug>
#include <QHostAddress>
#include <QTimer>
#include <cerrno>
#include <cstring>

#include "../driver/pl_ctrl_uapi.h"   /* SPEED_STATUS 位图（§14） */
#include "pl_host.h"
#include "receiver.h"
#include "sensorunits.h"

namespace {
/* 与 platform/flash/build_hdmi_dtb.sh 的 sensor-dma-buffer 预留区一致。 */
constexpr unsigned kDmaBase = 0x3ef00000u;
constexpr unsigned kDmaBlocks = 4u;
constexpr double kPlTsHz = 1.0e8;   /* PL_TS 单位 10 ns */
constexpr double kAdxl345LsbPerG = 256.0;
/* 块序号回退超过此值按「环被重新 START」处理：START 会清 DMA_BLK_SEQ。
 * 20 ms 轮询下正常运行远达不到该量级，因此不会掩盖真实丢块。 */
constexpr quint32 kSeqRestart = 1u << 20;
/* 采集到读取的延迟上限；超过 10 s 视为时基回绕或读取滞后，不参与平滑。 */
constexpr double kLatencyMaxUs = 10.0e6;
/* 采样率估计上限。ADXL345 的 ODR 为 3200 Hz，实测 DMA 帧率约 3.1 kHz
 * （4 KiB 块 512 字 = 170 帧，实板约 55 ms 一块）；上限只用于丢弃异常估计。 */
constexpr int kMaxVibRate = 20000;
/* 契约 §4.6：N=256 时频谱窗口为 0x400～0x7FF，共 256 个 bin。 */
constexpr int kSpectrumBins = 256;

/* 转速（接口 v1.2）：100 ms 一个样本。超过 2 s 没有新的有效样本就报不可用，
 * 与「编码器离线」共用同一条可用性通道，避免把旧值当实时读数继续显示。 */
constexpr qint64 kSpeedTimeoutMs = 2000;
/* 同一个事件可能先被寄存器轮询读到、又被流内记录读到（或反之），两路都要
 * 去重：以 PL 事件时间戳为准，200 ms 窗口覆盖一个采样周期与两路时延，
 * 又远小于 TS 的 42.9 s 回绕周期。 */
constexpr qint32 kSpeedDedupTicks = 20000000;
/* 环形模式下流内记录优先：1 s 内见过记录就抑制寄存器路径的重复追加
 * （记录带 PL 时间戳，是波形的首选时基）。 */
constexpr qint64 kSpeedDmaPreferMs = 1000;
constexpr qint64 kSpeedLogMs = 10000;   /* 转速寄存器读失败日志节流 */

QString dmaErrorText(quint32 code)
{
    switch (code) {
    case 1: return QStringLiteral("参数非法（长度或对齐）");
    case 2: return QStringLiteral("写响应非 OKAY");
    case 3: return QStringLiteral("传输被中止");
    default: return QStringLiteral("未知错误码");
    }
}
} // namespace

namespace mlk {

DataSource::DataSource(QObject *parent)
    : QObject(parent)
{
}

DataSource::~DataSource() = default;

UdpSource::UdpSource(quint16 port, QObject *parent)
    : DataSource(parent)
    , m_rx(new Receiver(this))
    , m_port(port)
{
    connect(m_rx, &Receiver::frameReceived, this, &DataSource::frameReceived);
    connect(m_rx, &Receiver::secureReady, this, &DataSource::secureReady);
}

void UdpSource::setChannel(SecureChannel *ch)
{
    m_rx->setChannel(ch);
}

bool UdpSource::start()
{
    const bool ok = m_rx->start(QHostAddress(QStringLiteral("0.0.0.0")), m_port);
    emit availabilityChanged(ok ? QStringLiteral("UDP :%1").arg(m_port)
                                : QStringLiteral("UDP 绑定失败"));
    return ok;
}

void UdpSource::stop()
{
    m_rx->stop();
}

DriverSource::DriverSource(QObject *parent)
    : DataSource(parent)
    , m_timer(new QTimer(this))
{
    // 20 ms polling: the register interface returns the latest sample, not the
    // full-rate stream. The time-domain curve and the FFT axis therefore use
    // this decimated rate, not the sensor ODR. 50 Hz keeps the UI light.
    m_timer->setInterval(20);
    connect(m_timer, &QTimer::timeout, this, &DriverSource::poll);
}

/* 析构时再走一次 stop()：正常退出时 KLocalWindow 已先行停环，重复调用无副作用；
 * 其他删除路径也不会留下运行中的 DMA 环。 */
DriverSource::~DriverSource()
{
    stop();
}

bool DriverSource::start()
{
    m_clock.start();
    if (!openDevice())
        m_retryAt = m_clock.elapsed() + 1000;
    m_timer->start();
    return true;
}

void DriverSource::stop()
{
    m_timer->stop();
    if (m_ringOn || m_ring.map) {
        pl_dma_ring_stop(&m_ring, m_fd);
        m_ringOn = false;
    }
    if (m_fd >= 0) {
        pl_close(m_fd);
        m_fd = -1;
    }
}

bool DriverSource::openDevice()
{
    m_fd = pl_open(nullptr);
    if (m_fd < 0) {
        emit availabilityChanged(QStringLiteral("不可用：%1").arg(
            QString::fromLocal8Bit(std::strerror(errno))));
        return false;
    }
    uint32_t id = 0;
    if (pl_probe(m_fd, &id) != 0 || pl_configure(m_fd, 5) != 0) {
        const QString why = QString::fromLocal8Bit(std::strerror(errno));
        pl_close(m_fd);
        m_fd = -1;
        emit availabilityChanged(QStringLiteral("不可用：%1").arg(why));
        return false;
    }
    m_haveTemp = false;
    m_haveAcc = false;
    m_haveCurrentCount = false;
    m_currentFault = false;
    m_calibrationReady = false;
    applyCurrentCalibration();
    m_lastCurrentMs = m_clock.elapsed();
    m_currentReason.clear();
    m_lastCalMs = m_clock.elapsed();
    m_rateStart = m_clock.elapsed();
    m_accFrames = 0;
    m_ring = {};
    m_ring.mem_fd = -1;
    m_spectrum = QVector<quint32>(kSpectrumBins, 0);
    m_ringSamples = 0;
    m_ringLostBlocks = 0;
    m_ringBlocks = 0;
    m_ringResyncs = 0;
    m_ringSeq = 0;
    m_haveRingSeq = false;
    m_sampleClock.reset();
    m_frames = DmaFrameAssembler();
    m_frameBuf.clear();
    m_dmaOvfCount = 0;
    m_dmaErrcode = 0;
    m_dmaOverflow = false;
    m_dmaBusy = true;
    m_latencyUs = 0.0;
    m_latencyValid = false;
    m_haveSpeedCount = false;
    m_speedCount = 0;
    m_speedSeq = 0;
    m_haveSpeedTs = false;
    m_speedAvailable = true;
    m_haveDmaSpeed = false;
    m_lastDmaSpeedMs = -1;
    m_speedBuf.clear();
    /* 从打开时刻起算 2 s 超时：超过就报「无有效数据」。 */
    m_lastSpeedSampleMs = m_clock.elapsed();
    /* 首次 poll 即读一次 DMA 状态，随后按 1 s 节流。 */
    m_lastDmaStatMs = m_clock.elapsed() - 1000;
    /* 环大小取 4 块：单块没有读取余量，PL 会在读完前覆盖。 */
    m_ringOn = pl_dma_ring_open(&m_ring, m_fd, kDmaBase, kDmaBlocks) == 0;
    if (m_ringOn)
        emit availabilityChanged(QStringLiteral("/dev/pl_ctrl · DMA 环"));
    else
        emit availabilityChanged(QStringLiteral("/dev/pl_ctrl · 寄存器轮询"));
    return true;
}

void DriverSource::closeDevice(const QString &reason)
{
    if (m_ringOn || m_ring.map) {
        pl_dma_ring_stop(&m_ring, m_fd);
        m_ringOn = false;
    }
    if (m_fd >= 0)
        pl_close(m_fd);
    m_fd = -1;
    m_retryAt = m_clock.elapsed() + 1000;
    emit availabilityChanged(reason);
}

void DriverSource::poll()
{
    const qint64 now = m_clock.elapsed();
    const qint64 t0 = m_clock.nsecsElapsed();
    if (m_fd < 0) {
        if (now < m_retryAt)
            return;
        if (!openDevice()) {
            m_retryAt = now + 1000;
            return;
        }
    }

    pl_sensors_t v = {};
    if (pl_read_sensors(m_fd, &v) != 0) {
        if (errno == EAGAIN)
            return;
        closeDevice(QStringLiteral("不可用：%1").arg(
            QString::fromLocal8Bit(std::strerror(errno))));
        return;
    }

    const qint64 t1 = m_clock.nsecsElapsed();
    m_readUs = int((t1 - t0) / 1000);
    if (m_lastPollStartNs)
        m_pollMs = int((t0 - m_lastPollStartNs) / 1000000);
    m_lastPollStartNs = t0;
    if (now - m_lastTimingEmitMs >= 1000) {
        m_lastTimingEmitMs = now;
        emit timingUpdated(m_pollMs, m_readUs);
    }

    const bool tempChanged = m_haveTemp && v.temp_count != m_tempCount;
    const bool accChanged = m_haveAcc && v.acc_count != m_accCount;

    // QSettings owns calibration; restore PL registers after power-on/reset.
    if (!m_calibrationReady || now - m_lastCalMs >= 1000) {
        m_lastCalMs = now;
        qint32 zero = 0, gain = 0;
        if (pl_read_current_calibration(m_fd, &zero, &gain) != 0
            || zero != m_curZero || gain != m_curGain)
            m_calibrationReady = applyCurrentCalibration();
        else
            m_calibrationReady = true;
    }

    const bool currentChanged = (!m_haveCurrentCount && v.current_count != 0)
        || (m_haveCurrentCount && v.current_count != m_currentCount);
    if (currentChanged) m_lastCurrentMs = now;
    m_currentFault = (v.faults & 0x04u) != 0;
    QString reason;
    if (m_currentFault) reason = QStringLiteral("ADS1115 通信异常");
    else if (!m_calibrationReady) reason = QStringLiteral("电流标定写入失败");
    else if (m_curGain <= 0) reason = QStringLiteral("电流未标定");
    else if (v.current_raw == 32767 || v.current_raw == -32768)
        reason = QStringLiteral("ADC 饱和，检查霍尔输出");
    else if (now - m_lastCurrentMs > 500) reason = QStringLiteral("电流无有效数据");
    if (reason != m_currentReason) {
        m_currentReason = reason;
        emit currentHealthChanged(reason.isEmpty(), reason);
    }
    if (reason.isEmpty() && currentChanged) {
        Frame f;
        f.ver = kWaveformVersion;
        f.type = kTypeWaveform;
        f.ch = kCurrent;
        f.seq = m_currentSeq++;
        f.cnt = v.current_count;
        f.rateHz = 50;
        f.samples = { currentMilliAmp(v.current_raw, m_curZero, m_curGain) };
        emit frameReceived(f);
    }
    m_currentCount = v.current_count;
    m_haveCurrentCount = true;

    if (tempChanged) {
        Frame f;
        f.ver = kVersion;
        f.type = kTypeWaveform;
        f.ch = kTemp;
        f.seq = m_tempSeq++;
        f.cnt = v.temp_count;
        f.rateHz = 1;
        f.tsUs = 0;
        f.samples = { qint16(v.temp_x10) };
        emit frameReceived(f);
    }

    if (accChanged && !m_ringOn) {
        ++m_accFrames;
        if (now - m_rateStart >= 1000) {
            const int rate = int((qint64(m_accFrames) * 1000) / qMax<qint64>(1, now - m_rateStart));
            m_vibRate = qBound(1, rate, kMaxVibRate);
            m_accFrames = 0;
            m_rateStart = now;
        }
        Frame f;
        f.ver = kVersion;
        f.type = kTypeWaveform;
        f.ch = kVibration;
        f.seq = m_accSeq++;
        f.cnt = v.acc_count;
        f.rateHz = quint16(m_vibRate);
        f.tsUs = 0;
        f.samples = { qint16(v.acc_x) };
        f.samplesY = { qint16(v.acc_y) };
        f.samplesZ = { qint16(v.acc_z) };
        emit frameReceived(f);
    }

    m_tempCount = v.temp_count;
    m_accCount = v.acc_count;
    m_haveTemp = true;
    m_haveAcc = true;

    /* 转速：每轮 poll 读一次寄存器（seq→字段→seq 一致才接受，失败静默）。
     * 环形模式下随后解析流内记录，两路用 PL 时间戳去重，只追加一次。 */
    readSpeed();

    if (m_ringOn) {
        pollRing();
        /* 完整频谱窗口每 1 s 取一次；读失败（旧码流无该窗口）静默跳过。 */
        if (now - m_lastSpectrumMs >= 1000) {
            m_lastSpectrumMs = now;
            pollSpectrum();
        }
        /* DMA 状态寄存器每 1 s 读一次，统计随 dmaStatsChanged 一起上报。 */
        if (now - m_lastDmaStatMs >= 1000) {
            m_lastDmaStatMs = now;
            pollDmaStat();
            emit dmaStatsChanged(m_ringBlocks, m_ringLostBlocks, m_dmaOverflow,
                                 m_latencyValid ? m_latencyUs : 0.0);
        }
    }

    /* 2 s 没有新的有效样本（离线、错误事件连续、或码流没有该通道）就报
     * 不可用，而不是让面板一直显示最后一次的读数。§14 规定错误事件也清
     * online，所以这里用超时去抖：单次 CRC 失败不会把读数闪成「编码器离线」。 */
    if (m_speedAvailable && m_lastSpeedSampleMs >= 0
        && now - m_lastSpeedSampleMs > kSpeedTimeoutMs) {
        m_speedAvailable = false;
        emit speedAvailabilityChanged(
            false, m_speedReason.isEmpty() ? QStringLiteral("无有效数据") : m_speedReason);
    }
}

/* 转速：寄存器路径。每轮 poll 读一次，seq→字段→seq 的一致性由
 * pl_read_speed() 保证；这里只判断是否有新事件、是否值得发样本。 */
void DriverSource::readSpeed()
{
    pl_speed_t speed = {};
    if (pl_read_speed(m_fd, &speed) != 0) {
        /* 旧码流没有该窗口，或读数一直撞上 PL 发布：静默，日志节流到 10 s，
         * 可用性由下面的 2 s 超时判定。 */
        const qint64 now = m_clock.elapsed();
        if (now - m_lastSpeedLogMs >= kSpeedLogMs) {
            m_lastSpeedLogMs = now;
            qWarning("pl_ctrl: 转速寄存器不可用（%s）", std::strerror(errno));
        }
        return;
    }

    SpeedSample s;
    s.raw = speed.raw;
    s.rpmMilli = speed.rpm_milli;
    s.status = speed.status;
    s.count = speed.count;
    s.ts = speed.ts;
    s.seq = speed.seq;
    s.errCount = speed.err_count;
    s.online = (speed.status & PL_CTRL_SPEED_STATUS_ONLINE) != 0;
    s.configured = (speed.status & PL_CTRL_SPEED_STATUS_CONFIGURED) != 0;
    s.overflow = (speed.status & PL_CTRL_SPEED_STATUS_SATURATED) != 0;
    s.error = (speed.status & PL_CTRL_SPEED_STATUS_ERROR) != 0;
    s.fromDma = false;
    noteSpeedState(s);

    /* 成功样本数或 seq 变化才算新事件；seq 变而 count 不变是错误事件，
     * 不产生样本（错误事件保留上一次的有效值，不能当新读数重复发）。 */
    const bool changed = !m_haveSpeedCount || speed.count != m_speedCount
                         || speed.seq != m_speedSeq;
    m_speedCount = speed.count;
    m_speedSeq = speed.seq;
    m_haveSpeedCount = true;
    if (changed)
        publishSpeed(s);
}

/* 转速：流内记录（CH=3/4/5）。W0 的状态字就是 SPEED_STATUS 低 16 位，
 * 按同一套位图解码；记录不携带 SPEED_COUNT，成功样本数取最近一次寄存器读数。 */
void DriverSource::acceptSpeedRecord(const DmaSpeedRecord &rec)
{
    SpeedSample s;
    s.raw = rec.raw_speed;
    s.rpmMilli = rec.rpm_milli;
    s.status = rec.flags;
    s.count = m_speedCount;
    s.ts = rec.ts;
    s.online = (rec.flags & DmaFrameAssembler::kSpeedFlagOnline) != 0;
    s.configured = (rec.flags & DmaFrameAssembler::kSpeedFlagConfigured) != 0;
    s.overflow = (rec.flags & DmaFrameAssembler::kSpeedFlagSaturated) != 0;
    s.error = (rec.flags & DmaFrameAssembler::kSpeedFlagError) != 0;
    s.fromDma = true;
    noteSpeedState(s);
    publishSpeed(s);
}

/* 发布一条新样本：两路（寄存器轮询 / 流内记录）共用，去重与可用性恢复都
 * 只在这里实现一次。 */
void DriverSource::publishSpeed(const SpeedSample &s)
{
    if (!s.online || s.error)
        return;   /* 离线或错误事件不产生样本，可用性交给 2 s 超时判断 */
    const qint64 now = m_clock.elapsed();
    /* 环形模式下记录优先：1 s 内见过记录就不再追加寄存器读到的同一样本。 */
    if (!s.fromDma && m_ringOn && m_haveDmaSpeed
        && now - m_lastDmaSpeedMs <= kSpeedDmaPreferMs)
        return;
    if (!speedTsNew(s.ts))
        return;   /* 同一事件已被另一条路径追加过 */
    if (s.fromDma) {
        m_haveDmaSpeed = true;
        m_lastDmaSpeedMs = now;
    }
    m_haveSpeedTs = true;
    m_speedLastTs = s.ts;
    m_lastSpeedSampleMs = now;
    emit speedChanged(s.rpmMilli, s.count, s.ts, s.online, s.overflow, s.error, s.fromDma);
    if (!m_speedAvailable) {
        m_speedAvailable = true;
        emit speedAvailabilityChanged(true, QString());
    }
}

/* 记录当前状态对应的不可用原因：超时真正触发时才用它，所以单个错误事件
 * 只更新原因、不立刻清空面板。 */
void DriverSource::noteSpeedState(const SpeedSample &s)
{
    if (!s.online)
        m_speedReason = QStringLiteral("编码器离线");
    else if (!s.configured)
        m_speedReason = QStringLiteral("编码器未配置");
    else
        m_speedReason.clear();
}

/* 同一事件可能先被寄存器读到、又被流内记录读到（或反之），两路时延不同，
 * 以 PL 事件时间戳去重：dt=0 是同一事件，略早于已发布事件的记录也视为同一
 * 事件。窗口 200 ms 覆盖一个采样周期与两路时延，又远小于 TS 的 42.9 s 回绕。 */
bool DriverSource::speedTsNew(quint32 ts) const
{
    if (!m_haveSpeedTs)
        return true;
    const qint32 dt = qint32(ts - m_speedLastTs);
    return !(dt <= 0 && dt > -kSpeedDedupTicks);
}

void DriverSource::pollSpectrum()
{
    unsigned bins = 0;
    unsigned seq = 0;
    const int n = pl_read_spectrum(m_fd, m_spectrum.data(), unsigned(m_spectrum.size()),
                                  &bins, &seq);
    if (n != 0 || bins < 2)
        return;
    m_spectrum.resize(int(bins));
    emit spectrumChanged(m_spectrum, quint32(seq));
}

void DriverSource::configureCurrent(qint32 zero, qint32 gain)
{
    if (zero < -32768 || zero > 32767 || gain < 0 || gain > 1000000) return;
    if (m_curZero == zero && m_curGain == gain) return;
    m_curZero = zero; m_curGain = gain;
    m_calibrationReady = false;
    m_lastCalMs = m_clock.isValid() ? m_clock.elapsed() - 1000 : -1000;
    if (m_fd >= 0) applyCurrentCalibration();
}

bool DriverSource::applyCurrentCalibration()
{
    qint32 zero = 0, gain = 0;
    m_calibrationReady = pl_set_current_calibration(m_fd, m_curZero, m_curGain) == 0
        && pl_read_current_calibration(m_fd, &zero, &gain) == 0
        && zero == m_curZero && gain == m_curGain;
    if (m_calibrationReady) emit currentCalibrationChanged(zero, gain);
    return m_calibrationReady;
}

bool DriverSource::zeroCurrent(QString *error)
{
    if (error)
        error->clear();
    auto fail = [error](const QString &text) {
        if (error)
            *error = text;
        return false;
    };
    if (m_fd < 0)
        return fail(QStringLiteral("归零失败：设备不可用"));
    pl_sensors_t v = {};
    if (pl_read_sensors(m_fd, &v) != 0)
        return fail(QStringLiteral("归零失败：读取采样超时"));
    if (v.faults & 0x04u)
        return fail(QStringLiteral("归零失败：ADS1115 通信异常"));
    if (v.current_raw == 32767 || v.current_raw == -32768)
        return fail(QStringLiteral("归零失败：ADC 饱和，检查霍尔输出"));
    if (!m_calibrationReady || m_curGain <= 0)
        return fail(QStringLiteral("归零失败：电流未标定"));
    if (!m_haveCurrentCount || m_clock.elapsed() - m_lastCurrentMs > 500)
        return fail(QStringLiteral("归零失败：电流无有效数据"));
    const qint32 oldGain = m_curGain;
    if (pl_zero_current(m_fd, int32_t(v.current_raw)) != 0)
        return fail(QStringLiteral("归零失败：写入 0x4C 校验不通过"));
    qint32 zero = 0, gain = 0;
    if (pl_read_current_calibration(m_fd, &zero, &gain) != 0
        || zero != v.current_raw || gain != oldGain)
        return fail(QStringLiteral("归零失败：写入 0x4C 校验不通过"));
    m_curZero = zero;
    m_curGain = gain;
    emit currentCalibrationChanged(m_curZero, m_curGain);
    /* 立刻补一帧 0 mA，面板不必等下一个采样周期。 */
    Frame f;
    f.ver = kVersion;
    f.type = kTypeWaveform;
    f.ch = kCurrent;
    f.seq = m_currentSeq++;
    f.cnt = v.current_count;
    f.rateHz = 50;
    f.tsUs = 0;
    f.samples = { qint16(0) };
    emit frameReceived(f);
    return true;
}

void DriverSource::pollRing()
{
    /* 按序取出仍保留的完成块（上限 8 块，避免阻塞 UI 线程）。
     * pl_dma_ring_next 提供稳定快照；跳号仅代表已无法安全取回的块。 */
    for (int guard = 0; guard < 8; ++guard) {
        const uint64_t *words = nullptr;
        uint32_t seq = 0;
        if (pl_dma_ring_next(&m_ring, m_fd, &words, &seq) != 0)
            return;
        if (!words)
            return;

        ++m_ringBlocks;
        if (m_haveRingSeq) {
            const quint32 delta = seq - m_ringSeq;   /* 32 位回绕 */
            if (delta >= kSeqRestart) {
                /* 序号回退：环被重新 START，DMA_BLK_SEQ 已清零，按重新对齐
                 * 处理，不计为丢块。 */
                m_frames.reset();
                m_sampleClock.reset();
            } else if (delta > 1) {
                m_ringLostBlocks += delta - 1;
                /* 跨块余字来自被覆写的块，已不可靠：丢弃半帧后重新对齐。 */
                m_frames.reset();
            }
        }
        m_ringSeq = seq;
        m_haveRingSeq = true;

        m_frameBuf.clear();
        m_speedBuf.clear();
        m_frames.push(words, PL_DMA_BLOCK_WORDS, m_frameBuf, &m_speedBuf);
        m_ringResyncs = m_frames.resyncCount();
        /* 转速记录先发布：它的样本与振动帧无关，且本块可能没有完整振动帧。 */
        for (size_t i = 0; i < m_speedBuf.size(); ++i)
            acceptSpeedRecord(m_speedBuf[i]);
        if (m_frameBuf.empty())
            continue;   /* 本块没有完整帧：余字已留给下一块，不发空帧 */

        /* 采样率由本块完整帧的首末 TS 估计（TS 同为 10 ns 计数）。 */
        const quint32 tsFirst = m_frameBuf.front().ts;
        const quint32 tsLast = m_frameBuf.back().ts;
        if (m_frameBuf.size() > 1) {
            const quint32 span = tsLast - tsFirst;   /* 32 位回绕 */
            if (span)
                m_vibRate = qBound(
                    1, qRound(double(m_frameBuf.size() - 1) * kPlTsHz / double(span)),
                    kMaxVibRate);
        }

        const int n = int(m_frameBuf.size());
        m_ringSamples += quint64(n);

        Frame f;
        f.ver = kWaveformVersion;
        f.type = kTypeWaveform;
        f.ch = kVibration;
        f.seq = m_accSeq++;   /* 应用内自增；块序号只用于 DMA 统计，避免重复计数 */
        f.cnt = quint16(seq);
        f.rateHz = quint16(m_vibRate);
        uint32_t nowTs = 0;
        if (pl_read_pl_ts(m_fd, &nowTs) == 0)
            f.tsUs = m_sampleClock.timestamp(tsFirst, nowTs,
                quint64(QDateTime::currentMSecsSinceEpoch()) * 1000, m_clock.nsecsElapsed() / 1000);
        f.samples.resize(n);
        f.samplesY.resize(n);
        f.samplesZ.resize(n);
        for (int i = 0; i < n; ++i) {
            const DmaFrame &fr = m_frameBuf[size_t(i)];
            f.samples[i] = fr.x;
            f.samplesY[i] = fr.y;
            f.samplesZ[i] = fr.z;
        }
        emit frameReceived(f);

        updateLatency(tsLast);
    }
}

void DriverSource::updateLatency(quint32 blockLastTs)
{
    uint32_t nowTs = 0;
    if (pl_read_pl_ts(m_fd, &nowTs) != 0) {
        m_latencyValid = false;   /* 读失败：本轮延迟未知，平滑值保持不变 */
        return;
    }
    /* 数据字 TS 与 PL_TS_NOW 同源（§4.4），差值即「采集到 PS 读取」的延迟；
     * 1 LSB = 10 ns，故 µs = ticks / 100。 */
    const quint32 delta = nowTs - blockLastTs;   /* 32 位回绕 */
    const double us = double(delta) / 100.0;
    if (us > kLatencyMaxUs) {
        m_latencyValid = false;   /* 回绕或异常差值：不污染平滑 */
        return;
    }
    m_latencyUs = m_latencyUs <= 0.0 ? us : m_latencyUs * 0.9 + us * 0.1;
    m_latencyValid = true;
}

void DriverSource::pollDmaStat()
{
    pl_dma_status_t status = {};
    if (pl_read_dma_status(m_fd, &status) == 0) {
        if (status.overflow && !m_dmaOverflow)
            ++m_dmaOvfCount;   /* OVF 粘滞：置位次数即每次 START 后的溢出次数 */
        m_dmaOverflow = status.overflow != 0;
        if (status.errcode != m_dmaErrcode) {
            m_dmaErrcode = status.errcode;
            if (status.errcode)
                emit availabilityChanged(QStringLiteral("DMA 错误：%1（ERRCODE=%2）")
                                             .arg(dmaErrorText(status.errcode))
                                             .arg(status.errcode));
            else
                emit availabilityChanged(QStringLiteral("/dev/pl_ctrl · DMA 环"));
        }
    }

    uint32_t ctrl = 0;
    if (pl_read_dma_ctrl(m_fd, &ctrl) == 0) {
        const bool busy = (ctrl & PL_DMA_CTRL_BUSY) != 0;
        if (busy != m_dmaBusy) {
            m_dmaBusy = busy;
            /* ERRCODE 文案优先，避免 BUSY 文案把它盖掉。 */
            if (!m_dmaErrcode)
                emit availabilityChanged(busy ? QStringLiteral("/dev/pl_ctrl · DMA 环")
                                              : QStringLiteral("/dev/pl_ctrl · DMA 环已停止"));
        }
    }
}

} // namespace mlk
