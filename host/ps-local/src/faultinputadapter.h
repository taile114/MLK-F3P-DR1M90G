#pragma once

#include <QMetaType>
#include <QString>
#include <QVector>

#include "appconfig.h"
#include "frame.h"

/* 单主故障模型的本地数据入口：把已打时间戳的 Frame 转成原生物理块
 * （fault.sample.v1）。单位换算只在这里决定，显示、网络与记录分支不再各自
 * 换算出另一套模型输入。 */
namespace mlk {

struct NativeBlock
{
    QString sourceRun;      /* 本次运行身份；重连、重启或时基重置后更换 */
    quint64 blockId = 0;    /* 本地完整块唯一 ID，不使用网络包序号 */
    quint32 channel = 0;    /* 0 温度、1 电流、2 振动、3 转速 */
    quint64 t0Us = 0;       /* 首样本 UTC 时刻 */
    quint32 rateHz = 0;     /* 块内实际采样率 */
    quint32 count = 0;      /* 每轴样本数 */
    quint32 axes = 1;       /* 振动 3，其余 1 */
    QVector<float> values;  /* [count,axes]，物理单位，按样本、轴排列 */
    QString scaleId;        /* 生成物理值使用的标度快照身份 */
    bool dataValid = false; /* 实际新观测；旧寄存器重读不算 */
    bool gapBefore = false; /* 与上一块之间存在缺口 */
    bool saturated = false; /* 量程饱和；只记录实际情况，不补造样本 */
    bool hasDmaSeq = false; /* 源层给出完整 DMA 块序号时保留 */
    quint32 dmaSeq = 0;

    quint64 endUs() const
    {
        return count && rateHz ? t0Us + quint64(count - 1) * 1000000ull / rateHz : t0Us;
    }
    double seconds() const { return count && rateHz ? double(count) / double(rateHz) : 0.0; }
};

class FaultInputAdapter
{
public:
    void configure(const AppConfig &config);
    /* 建立新的 source_run 并清空块序号与缺块记忆。缩放、标度或数据源变化时
     * 必须先 restart()，让工作线程清上下文并重新等待四秒。 */
    QString restart();
    QString sourceRun() const { return m_sourceRun; }
    QString scaleId() const { return m_scaleId; }

    NativeBlock acceptFrame(const Frame &stamped);
    /* 转速沿用 0.1 rpm 的整数量化：deciRpm 就是界面与其他链路共用值。 */
    NativeBlock acceptSpeed(qint32 deciRpm, quint32 count, quint64 t0Us, bool overflow);

private:
    void noteGap(NativeBlock &block);

    AppConfig m_config;
    QString m_sourceRun;
    QString m_scaleId;
    quint64 m_blockId = 0;
    quint32 m_lastSeq[kChannelCount] = {};
    bool m_haveSeq[kChannelCount] = {};
    quint64 m_lastEndUs[kChannelCount] = {};
};

} // namespace mlk

Q_DECLARE_METATYPE(mlk::NativeBlock)
