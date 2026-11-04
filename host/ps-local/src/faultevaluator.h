#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QThread>
#include <QVector>

#include <atomic>

namespace mlk {

/* 数据集评估的汇总结果。真值与预测逐窗比对后累计，不保存参考预测。 */
struct FaultEvalReport {
    QString model;
    QString preprocess;
    QString backend;
    int samples = 0;
    int evaluated = 0;
    int classCorrect = 0;
    int jointCorrect = 0;
    int faultSamples = 0;
    int faultJointCorrect = 0;
    double meanInferUs = 0.0;
    double medianInferUs = 0.0;
    double p95InferUs = 0.0;
    double maxInferUs = 0.0;
    QVector<QVector<int>> classConfusion; /* [真值][预测]，5×5 */
    QVector<QVector<int>> gradeConfusion; /* [真值等级][预测等级]，4×4 */
    QVector<int> classJointCorrect;       /* 逐类「类别+等级」同时正确的窗数 */

    double classAccuracy() const;
    double jointAccuracy() const;
    double faultJointAccuracy() const;
    /* 单窗处理速度：1 秒内可处理的窗口数，只含引擎调用时间。 */
    double windowsPerSecond() const;
    QJsonObject toJson() const;
};

/* 评估工作线程对象：加载运行包与评估集，逐窗调用共享库并累计指标。
 * 与实时诊断互斥：调用方保证实时任务已停止；本对象不复用 FaultWorker 的
 * 窗口缓存，也不产生告警、工单或网络包。 */
class FaultEvaluator : public QObject
{
    Q_OBJECT
public:
    explicit FaultEvaluator(QObject *parent = nullptr);
    void prepareRun() { m_cancel.store(false); }

public slots:
    void startRun(const QString &packageRoot, const QString &datasetRoot, int backend);
    void stopRun();

signals:
    void stateChanged(const QString &detail);
    void progress(int done, int total);
    void finished(const mlk::FaultEvalReport &report);
    void failed(const QString &error);

private:
    std::atomic<bool> m_cancel{false};
    bool m_running = false;
};

/* 评估控制器：拥有工作线程，UI 线程只投递请求。 */
class FaultEvalController : public QObject
{
    Q_OBJECT
public:
    explicit FaultEvalController(QObject *parent = nullptr);
    ~FaultEvalController() override;

    bool start(const QString &packageRoot, const QString &datasetRoot, int backend, QString *error);
    void stop();
    bool running() const { return m_running; }

signals:
    void stateChanged(const QString &detail);
    void progress(int done, int total);
    void finished(const mlk::FaultEvalReport &report);
    void failed(const QString &error);

private:
    void ensureWorker();

    QThread m_thread;
    FaultEvaluator *m_worker = nullptr;
    bool m_running = false;
};

} // namespace mlk

Q_DECLARE_METATYPE(mlk::FaultEvalReport)
