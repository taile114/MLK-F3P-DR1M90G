#pragma once

#include "fault_engine_api.h"
#include "faultinputadapter.h"
#include "faulttypes.h"

#include <QJsonObject>
#include <QString>
#include <QVector>

/* 冻结短上下文的本地预处理：四秒原生块窗口 → 27683 个已标准化 float。
 * 算法按 host/故障诊断模型接口需求.md §2.2、npu/fault-diagnosis/
 * F3P部署与量化设计.md §3 实现；标准化参数、频带权重与水平参考随运行包
 * 冻结，启动不重新拟合，也不从当前窗口重学水平参考。 */
namespace mlk {

struct FaultQuality
{
    int blocks = 0;
    int gapBlocks = 0;
    double tfRatio = 0.0;
    double fastRatio = 0.0;
    double slowRatio = 0.0;
    double poseAgeMs = -1.0;
    double coverage = 0.0;
    bool gravityOk = false;
    bool temperatureValid = false;
    double temperatureC = 0.0;
    bool overtemperatureMasked = false;
    QJsonObject toJson() const;
};

struct FaultWindowInput
{
    quint64 startUs = 0;
    quint64 endUs = 0;
    QVector<NativeBlock> vibration;
    QVector<NativeBlock> temperature;
    QVector<NativeBlock> current;
    QVector<NativeBlock> speed;
};

struct FaultWindowOutput
{
    bool ok = false;
    QString reason;          /* waiting_context / missing_speed / tf_coverage / ... */
    FaultQuality quality;
    QVector<float> input;    /* MLK_FAULT_INPUT_FLOATS，已标准化 */
};

class FaultPreprocessor
{
public:
    /* 从运行包根目录读取 preprocess.json。包内资产缺失或不符即失败，
     * 调用方据此把状态置为 error，而不是用默认值继续推断。 */
    bool loadAssets(const QString &root, QString *error);
    bool loaded() const { return m_loaded; }
    QString preprocessId() const { return m_preprocess; }

    bool build(const FaultWindowInput &window, FaultWindowOutput &out, QString *error) const;

    /* 单块单轴频谱，供数值对照与单元检查使用；freqHz 升序，power 为单边 PSD。 */
    static void blockSpectrum(const QVector<float> &samples, double fs,
                              QVector<double> &freqHz, QVector<double> &power);

private:
    bool readArray(const QJsonObject &norm, const char *key, int expected, QVector<double> &out,
                   QString *error) const;
    double standardize(const QVector<double> &scale, const QVector<double> &shift, int index,
                       double value) const;
    bool usableVibration(const NativeBlock &block) const;
    void buildTimeFrequency(const FaultWindowInput &window, QVector<double> &values,
                            FaultQuality &quality) const;
    void buildSpectrum(const FaultWindowInput &window, QVector<double> &values) const;
    void buildStatistics(const FaultWindowInput &window, QVector<double> &values) const;
    void buildPose(const FaultWindowInput &window, QVector<double> &values,
                   FaultQuality &quality) const;
    void buildFastState(const FaultWindowInput &window, QVector<double> &values,
                        FaultQuality &quality) const;
    void buildSlowState(const FaultWindowInput &window, QVector<double> &values,
                        FaultQuality &quality) const;

    bool m_loaded = false;
    QString m_preprocess = QString::fromLatin1(kFaultPreprocess);
    int m_bands = 64;
    int m_tfFreqs = 193;
    int m_psdFreqs = 769;
    double m_maxHz = 600.0;
    QVector<double> m_bandWeights;          /* [bands][tfFreqs] */
    QVector<double> m_tfMean, m_tfStd;      /* [8][bands] */
    QVector<double> m_psdMean, m_psdStd;    /* [3][psdFreqs] */
    QVector<double> m_statsMean, m_statsStd;/* [12] */
    QVector<double> m_poseMean, m_poseStd;  /* [5] */
    QVector<double> m_fastMean, m_fastStd;  /* [6] */
    QVector<double> m_slowMean, m_slowStd;  /* [9] */
    double m_reference[3] = {0.0, 0.0, -1.0};
    int m_minBlocks = 12;
    int m_minBlockSamples = 64;
    int m_minVibrationRateHz = 1600;
    double m_tfMinRatio = 0.45;
    double m_fastMinRatio = 0.75;
    double m_slowMinRatio = 0.60;
    double m_poseMaxAgeMs = 1500.0;
};

} // namespace mlk

#include <QMetaType>
Q_DECLARE_METATYPE(mlk::FaultQuality)
