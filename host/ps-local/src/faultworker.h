#pragma once

#include <QElapsedTimer>
#include <QMutex>
#include <QObject>
#include <QVector>

#include "faultinputadapter.h"
#include "faultpreprocess.h"
#include "faultresult.h"
#include "faultruntime.h"

class QTimer;

namespace mlk {

struct FaultRunConfig
{
    QString root;
    int backend = kFaultBackendCpu;
    QString sourceRun;
    QString scaleId;
};

/* 故障推断工作线程：原生块环、四秒窗口、两秒步长、冻结预处理与引擎调用。
 * 同一时刻只有一个窗口在途，积压窗口只保留最新一个；UI 线程只入队。 */
class FaultWorker : public QObject
{
    Q_OBJECT
public:
    explicit FaultWorker(QObject *parent = nullptr);
    ~FaultWorker() override;

    /* 供 UI 线程调用：只做加锁入队，不触碰引擎与预处理状态。 */
    void enqueue(const mlk::NativeBlock &block);
    void requestDrain();

public slots:
    void startRun(const mlk::FaultRunConfig &config);
    void drain();
    void stopRun();

signals:
    void stateChanged(int state, const QString &detail);
    void runReady(const QString &run, const QString &model, const QString &preprocess);
    void runFailed(const QString &error);
    void predictionReady(const mlk::FaultResult &result, const mlk::FaultQuality &quality);
    void journalRecord(const QJsonObject &record);

private:
    void publishUnavailable(int state, const QString &reason, const FaultQuality &quality);
    void prune();
    bool collectWindow(FaultWindowInput &window) const;

    QMutex m_mutex;
    QVector<NativeBlock> m_queue;
    QVector<NativeBlock> m_blocks[kChannelCount];
    QTimer *m_timer = nullptr;
    QElapsedTimer m_publishClock;
    QString m_lastUnavailable;

    FaultRunConfig m_config;
    FaultPreprocessor m_preprocessor;
    FaultRuntime m_runtime;
    QString m_run;
    bool m_active = false;
    bool m_haveWindow = false;
    quint64 m_nextEnd = 0;
    quint64 m_lastStart = 0;
    quint64 m_lastEnd = 0;
    quint32 m_seq = 0;
    int m_state = kFaultStopped;
};

} // namespace mlk

Q_DECLARE_METATYPE(mlk::FaultRunConfig)
