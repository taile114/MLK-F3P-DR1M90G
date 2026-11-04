#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QThread>

#include "faultresult.h"
#include "faultworker.h"

/* 实时故障诊断控制器：模式与发布者所有权、run/窗口身份、确认与时效都在这里，
 * UI 只发起异步请求并显示状态。API 2 外部发布者与内置工作线程不能同时有效。 */
namespace mlk {

class FaultLiveController : public QObject
{
    Q_OBJECT
public:
    explicit FaultLiveController(QObject *parent = nullptr);
    ~FaultLiveController() override;

    bool start(const FaultRunConfig &config, QString *error = nullptr);
    void stop();
    void enqueue(const mlk::NativeBlock &block);
    /* 500 ms 定时调用：只处理时效，不阻塞 UI。 */
    void tick();

    bool running() const { return m_running; }
    bool busy() const
    {
        return m_running || m_state == kFaultLoading || m_state == kFaultStopping;
    }
    int state() const { return m_state; }
    QString stateDetail() const { return m_stateDetail; }
    QString run() const { return m_run; }
    mlk::FaultResult current() const { return m_current; }
    qint64 ageMs() const;
    bool confirmed() const { return m_confirmed; }
    int confirmedClass() const { return m_confirmedClass; }
    int confirmedGrade() const { return m_confirmedGrade; }
    bool externalPublisher() const { return m_external; }

    /* API 2 外部发布者入口：内置任务运行时返回 false 并给出 publisher_busy。 */
    bool acceptExternal(const mlk::FaultResult &result, QString *error);
    void releaseExternal();

signals:
    void stateChanged();
    void resultPublished(const mlk::FaultResult &raw, qint64 ageMs);
    void confirmedEvent(const mlk::FaultResult &snapshot);
    void confirmedCleared();
    void journal(const QJsonObject &record);

private:
    void ensureWorker();
    void onWorkerState(int state, const QString &detail);
    void onWorkerResult(const mlk::FaultResult &result);
    void onWorkerFailed(const QString &error);
    void applyConfirmation(const mlk::FaultResult &result);
    void clearConfirmation();
    void publishInvalid(int state, const QString &reason);

    QThread m_thread;
    mlk::FaultWorker *m_worker = nullptr;
    mlk::FaultRunConfig m_config;
    mlk::FaultResult m_current;
    QElapsedTimer m_clock;
    qint64 m_ageAtReceipt = -1;
    QString m_run;
    QString m_stateDetail;
    bool m_running = false;
    bool m_external = false;
    bool m_confirmed = false;
    int m_confirmedClass = kFaultUnavailable;
    int m_confirmedGrade = 0;
    int m_confirmCount = 0;
    int m_normalCount = 0;
    mlk::FaultResult m_snapshot;
    int m_state = kFaultStopped;
};

} // namespace mlk
