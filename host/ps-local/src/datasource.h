#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QVector>
#include <vector>

#include "dma_frames.h"
#include "plsampleclock.h"
#include "frame.h"
#include "pl_host.h"
#include "sensorunits.h"

class QTimer;

namespace mlk {
class Receiver;
class SecureChannel;

class DataSource : public QObject
{
    Q_OBJECT
public:
    explicit DataSource(QObject *parent = nullptr);
    ~DataSource() override;

    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual QString name() const = 0;

signals:
    void frameReceived(const mlk::Frame &f);
    void secureReady();
    void availabilityChanged(const QString &text);
    void timingUpdated(int pollMs, int readUs);
    /* DMA 环汇总，最多每 1 s 一次。blocks 为累计已消费块数，lostBlocks 为
     * 块序号跳变丢弃的块数，overflow 为 DMA_STATUS.OVF 当前状态，
     * ageUs 为最近一块「采集到 PS 读取」的延迟（µs，<=0 表示未知）。 */
    void dmaStatsChanged(quint64 blocks, quint64 lostBlocks, bool overflow, double ageUs);
    /* PL 完整频谱窗口（契约 §4.6），最多每 1 s 一次。bins 为 (|X_k|²)>>8，
     * sequence 为同一次读取首末一致的 FFT_SEQUENCE。 */
    void spectrumChanged(const QVector<quint32> &bins, quint32 sequence);
};

class UdpSource : public DataSource
{
    Q_OBJECT
public:
    explicit UdpSource(quint16 port, QObject *parent = nullptr);

    bool start() override;
    void stop() override;
    QString name() const override { return QStringLiteral("UDP"); }

    void setChannel(SecureChannel *ch);

private:
    mlk::Receiver *m_rx;
    quint16 m_port;
};

class DriverSource : public DataSource
{
    Q_OBJECT
public:
    explicit DriverSource(QObject *parent = nullptr);
    ~DriverSource() override;

    bool start() override;
    void stop() override;
    QString name() const override { return QStringLiteral("/dev/pl_ctrl"); }

    /* 电流：换算集中在数据源，Frame 里是毫安物理量（契约 §3）。 */
    qint32 currentZero() const { return m_curZero; }
    qint32 currentGain() const { return m_curGain; }
    bool zeroCurrent(QString *error);
    void configureCurrent(qint32 zero, qint32 gain);

signals:
    void currentCalibrationChanged(qint32 zero, qint32 gain);
    void currentHealthChanged(bool ok, const QString &detail);
    /* 转速（接口 v1.2）：rpmMilli 为 0.001 rpm 的千分整数（反转 < 0），
     * count 为 SPEED_COUNT（成功样本数），tsTicks 为 PL 事件时间戳
     * （10 ns/LSB），overflow/error 取 SPEED_STATUS 对应位；fromDma 表示样本
     * 取自流内转速记录（环形 DMA 优先），否则来自寄存器轮询。
     * 只在确有新的成功样本时发一次；错误事件不伪造有效读数。 */
    void speedChanged(qint32 rpmMilli, quint32 count, quint32 tsTicks,
                      bool online, bool overflow, bool error, bool fromDma);
    /* 转速可用性：false 时 reason 给出原因（编码器离线 / 编码器未配置 /
     * 无有效数据）；恢复 true 后由下一个样本刷新显示。 */
    void speedAvailabilityChanged(bool available, const QString &reason);

private slots:
    void poll();

private:
    /* 一条转速样本：寄存器路径与 DMA 记录解析后共用同一条发布通道，
     * 这样两条路径的去重与可用性判断只有一处实现。 */
    struct SpeedSample
    {
        qint32 raw = 0;
        qint32 rpmMilli = 0;
        quint32 status = 0;
        quint32 count = 0;
        quint32 ts = 0;
        quint32 seq = 0;
        quint32 errCount = 0;
        bool online = false;
        bool configured = false;
        bool overflow = false;
        bool error = false;
        bool fromDma = false;
    };

    bool openDevice();
    bool applyCurrentCalibration();
    void closeDevice(const QString &reason);
    void readSpeed();
    void acceptSpeedRecord(const struct DmaSpeedRecord &rec);
    void publishSpeed(const SpeedSample &s);
    void noteSpeedState(const SpeedSample &s);
    bool speedTsNew(quint32 ts) const;

    int m_fd = -1;
    QTimer *m_timer;
    QElapsedTimer m_clock;
    qint64 m_retryAt = 0;
    quint32 m_tempSeq = 0;
    quint32 m_accSeq = 0;
    quint32 m_currentSeq = 0;
    quint16 m_tempCount = 0;
    quint16 m_accCount = 0;
    quint16 m_currentCount = 0;
    bool m_haveCurrentCount = false;
    bool m_currentFault = false;
    qint32 m_curZero = kCurrentZeroDefault;
    qint32 m_curGain = kCurrentGainDefault;
    bool m_calibrationReady = false;
    qint64 m_lastCurrentMs = 0;
    QString m_currentReason;
    qint64 m_lastCalMs = 0;
    bool m_haveTemp = false;
    bool m_haveAcc = false;
    int m_vibRate = 50;
    quint32 m_accFrames = 0;
    qint64 m_rateStart = 0;
    qint64 m_lastPollStartNs = 0;
    qint64 m_lastTimingEmitMs = 0;
    int m_pollMs = 0;
    int m_readUs = 0;
    pl_dma_ring_t m_ring = {};
    bool m_ringOn = false;
    void pollRing();
    void pollSpectrum();
    void pollDmaStat();
    void updateLatency(quint32 blockLastTs);

    DmaFrameAssembler m_frames;          /* 跨块三字帧重组 */
    std::vector<DmaFrame> m_frameBuf;    /* 单块解析结果，复用容量 */
    quint64 m_ringBlocks = 0;            /* 累计已消费块数 */
    quint64 m_ringSamples = 0;           /* 累计已解析振动帧数 */
    quint64 m_ringLostBlocks = 0;        /* 块序号跳变丢弃的块数 */
    quint64 m_ringResyncs = 0;           /* 失配重同步次数 */
    quint32 m_ringSeq = 0;
    bool m_haveRingSeq = false;
    PlSampleClock m_sampleClock;
    quint64 m_dmaOvfCount = 0;           /* OVF 置位次数（粘滞位，每次 START 后最多 1 次） */
    quint32 m_dmaErrcode = 0;
    bool m_dmaOverflow = false;
    bool m_dmaBusy = true;
    qint64 m_lastDmaStatMs = 0;
    qint64 m_lastSpectrumMs = 0;
    QVector<quint32> m_spectrum;
    double m_latencyUs = 0.0;            /* 平滑后的采集到读取延迟 */
    bool m_latencyValid = false;

    /* 转速（接口 v1.2）：pl_read_speed() 已保证 seq→字段→seq 一致，这里只做
     * 事件判定与两路合并——寄存器轮询与流内记录用 PL 时间戳去重，环形模式
     * 下优先用记录（记录带 PL 时间戳，是波形的首选时基）。 */
    bool m_haveSpeedCount = false;
    quint32 m_speedCount = 0;
    quint32 m_speedSeq = 0;
    bool m_haveSpeedTs = false;
    quint32 m_speedLastTs = 0;
    qint64 m_lastSpeedSampleMs = -1;
    bool m_speedAvailable = true;
    qint64 m_lastSpeedLogMs = 0;
    QString m_speedReason;                    /* 超时时给出的不可用原因 */
    bool m_haveDmaSpeed = false;
    qint64 m_lastDmaSpeedMs = -1;
    std::vector<DmaSpeedRecord> m_speedBuf;   /* 单块转速记录，复用容量 */
};

} // namespace mlk
