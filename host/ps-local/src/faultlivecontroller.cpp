#include "faultlivecontroller.h"

#include <QDateTime>
#include <QMetaType>

namespace mlk {

FaultLiveController::FaultLiveController(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<mlk::FaultResult>("mlk::FaultResult");
    qRegisterMetaType<mlk::FaultQuality>("mlk::FaultQuality");
    qRegisterMetaType<mlk::FaultRunConfig>("mlk::FaultRunConfig");
    m_current = FaultResult::unavailable(kFaultStopped, QStringLiteral("stopped"));
}

FaultLiveController::~FaultLiveController()
{
    if (!m_worker) return;
    if (m_thread.isRunning()) {
        QMetaObject::invokeMethod(m_worker, "stopRun", Qt::BlockingQueuedConnection);
        m_thread.quit();
        m_thread.wait(8000);
    }
    m_worker = nullptr;
}

void FaultLiveController::ensureWorker()
{
    if (m_worker) return;
    m_worker = new mlk::FaultWorker;
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &mlk::FaultWorker::stateChanged, this,
            &FaultLiveController::onWorkerState);
    connect(m_worker, &mlk::FaultWorker::runReady, this,
            [this](const QString &run, const QString &model, const QString &preprocess) {
                Q_UNUSED(model);
                Q_UNUSED(preprocess);
                m_run = run;
                m_running = true;
                m_state = kFaultWaiting;
                m_stateDetail = QStringLiteral("等待四秒窗口");
                emit stateChanged();
            });
    connect(m_worker, &mlk::FaultWorker::runFailed, this, &FaultLiveController::onWorkerFailed);
    connect(m_worker, &mlk::FaultWorker::predictionReady, this,
            [this](const mlk::FaultResult &result, const mlk::FaultQuality &quality) {
                Q_UNUSED(quality);
                onWorkerResult(result);
            });
    connect(m_worker, &mlk::FaultWorker::journalRecord, this,
            [this](const QJsonObject &record) { emit journal(record); });
    m_thread.start();
}

bool FaultLiveController::start(const FaultRunConfig &config, QString *error)
{
    if (busy()) {
        if (error) *error = QStringLiteral("故障任务已在进行，先停止再启动");
        return false;
    }
    ensureWorker();
    m_config = config;
    m_run.clear();
    m_external = false;
    clearConfirmation();
    m_running = false;
    m_state = kFaultLoading;
    m_stateDetail = QStringLiteral("加载故障运行包");
    publishInvalid(kFaultLoading, QStringLiteral("loading"));
    emit stateChanged();
    QMetaObject::invokeMethod(m_worker, "startRun", Qt::QueuedConnection,
                              Q_ARG(mlk::FaultRunConfig, config));
    return true;
}

void FaultLiveController::stop()
{
    if (!m_worker) return;
    if (m_state == kFaultStopped && !m_running) return;
    m_running = false;
    m_state = kFaultStopping;
    m_stateDetail = QStringLiteral("停止中");
    publishInvalid(kFaultStopping, QStringLiteral("stopping"));
    emit stateChanged();
    QMetaObject::invokeMethod(m_worker, "stopRun", Qt::QueuedConnection);
}

void FaultLiveController::enqueue(const mlk::NativeBlock &block)
{
    if (!m_running || !m_worker) return;
    m_worker->enqueue(block);
}

qint64 FaultLiveController::ageMs() const
{
    if (!m_current.valid || !m_clock.isValid()) return -1;
    return m_ageAtReceipt + m_clock.elapsed();
}

void FaultLiveController::tick()
{
    if (!m_current.valid) return;
    if (ageMs() <= kFaultResultMaxAgeMs) return;
    const QString run = m_current.run;
    m_current = FaultResult::unavailable(kFaultStale, QStringLiteral("result_timeout"), run);
    clearConfirmation();
    emit resultPublished(m_current, -1);
}

void FaultLiveController::publishInvalid(int state, const QString &reason)
{
    const QString run = m_run;
    FaultResult invalid = FaultResult::unavailable(state, reason, run);
    invalid.model = m_current.model;
    invalid.preprocess = m_current.preprocess;
    invalid.backend = m_current.backend;
    if (m_current.seq) {
        invalid.seq = m_current.seq;
        invalid.startUs = m_current.startUs;
        invalid.endUs = m_current.endUs;
    }
    m_current = invalid;
}

void FaultLiveController::onWorkerState(int state, const QString &detail)
{
    m_state = state;
    m_stateDetail = detail;
    if (state == kFaultStopped) {
        m_running = false;
        publishInvalid(kFaultStopped, QStringLiteral("stopped"));
        emit resultPublished(m_current, -1);
        m_clock.invalidate();
        m_ageAtReceipt = -1;
    }
    emit stateChanged();
}

void FaultLiveController::onWorkerFailed(const QString &error)
{
    m_running = false;
    m_state = kFaultError;
    m_stateDetail = error;
    publishInvalid(kFaultError, QStringLiteral("engine_error"));
    m_clock.invalidate();
    m_ageAtReceipt = -1;
    emit stateChanged();
    emit resultPublished(m_current, -1);
}

void FaultLiveController::clearConfirmation()
{
    if (m_confirmed) {
        m_confirmed = false;
        emit confirmedCleared();
    }
    m_confirmedClass = kFaultUnavailable;
    m_confirmedGrade = 0;
    m_confirmCount = 0;
    m_normalCount = 0;
    m_snapshot = FaultResult{};
}

void FaultLiveController::applyConfirmation(const FaultResult &result)
{
    if (!result.valid || (result.classIndex != kFaultNormal && !result.alarmEligible())) {
        clearConfirmation();
        return;
    }
    if (result.classIndex == kFaultNormal) {
        m_confirmCount = 0;
        m_snapshot = FaultResult{};
        if (!m_confirmed) return;
        if (++m_normalCount >= kFaultRecoverWindows) {
            m_confirmed = false;
            m_confirmedClass = kFaultUnavailable;
            m_confirmedGrade = 0;
            m_normalCount = 0;
            emit confirmedCleared();
        }
        return;
    }
    m_normalCount = 0;
    if (m_snapshot.valid && m_snapshot.classIndex == result.classIndex
        && m_snapshot.grade == result.grade) {
        ++m_confirmCount;
    } else {
        m_snapshot = result;
        m_confirmCount = 1;
    }
    const bool changed = m_confirmed && (m_confirmedClass != result.classIndex
                                         || m_confirmedGrade != result.grade);
    if (m_confirmCount >= kFaultConfirmWindows && (!m_confirmed || changed)) {
        m_confirmed = true;
        m_confirmedClass = result.classIndex;
        m_confirmedGrade = result.grade;
        emit confirmedEvent(m_snapshot);
    }
}

void FaultLiveController::onWorkerResult(const FaultResult &result)
{
    if (!m_running) return;   /* 停止后晚到的结果按 run 失效处理 */
    if (!m_run.isEmpty() && !result.run.isEmpty() && result.run != m_run) return;
    if (result.valid && m_current.valid && result.run == m_current.run
        && result.seq <= m_current.seq) return;
    if (!result.valid && result.seq && m_current.valid && m_current.seq
        && result.run == m_current.run && result.seq < m_current.seq) return;

    m_current = result;
    m_state = result.state;
    m_stateDetail = result.valid ? QStringLiteral("按两秒步长诊断")
        : result.reason == QLatin1String("waiting_context") ? QStringLiteral("等待四秒窗口")
        : QStringLiteral("窗口不可用：%1").arg(result.reason);
    if (result.valid) {
        const qint64 nowUs = qint64(QDateTime::currentMSecsSinceEpoch()) * 1000;
        m_ageAtReceipt = qMax<qint64>(0, (nowUs - qint64(result.endUs)) / 1000);
        m_clock.restart();
    } else {
        m_clock.invalidate();
        m_ageAtReceipt = -1;
    }
    applyConfirmation(result);
    emit stateChanged();
    emit resultPublished(result, result.valid ? ageMs() : -1);
}

bool FaultLiveController::acceptExternal(const FaultResult &result, QString *error)
{
    auto reject = [error](const char *code) {
        if (error) *error = QString::fromLatin1(code);
        return false;
    };
    if (m_running || busy()) return reject("publisher_busy");
    QString why;
    if (!result.validate(&why)) return reject("invalid_result");
    const qint64 nowUs = qint64(QDateTime::currentMSecsSinceEpoch()) * 1000;
    const qint64 age = result.seq ? (nowUs - qint64(result.endUs)) / 1000 : -1;
    if (result.valid && age > kFaultResultMaxAgeMs) return reject("stale_result");
    const bool duplicate = m_external && result.valid && result.run == m_current.run
        && result.seq == m_current.seq;
    if (m_external && !m_current.run.isEmpty()) {
        if (result.run == m_current.run) {
            if (result.seq < m_current.seq) return reject("stale_result");
            if (result.seq == m_current.seq && result.valid) {
                if (!m_current.valid) return reject("window_conflict");
                if (result.classIndex != m_current.classIndex || result.grade != m_current.grade
                    || result.startUs != m_current.startUs || result.endUs != m_current.endUs
                    || result.hasConfidence != m_current.hasConfidence
                    || result.confidence != m_current.confidence)
                    return reject("window_conflict");
            }
        } else if (m_current.valid) {
            return reject("identity_mismatch");
        }
    }
    m_external = true;
    m_run = result.run;
    m_state = result.state;
    m_stateDetail.clear();
    m_current = result;
    if (result.valid) {
        m_ageAtReceipt = qMax<qint64>(0, age);
        m_clock.restart();
    } else {
        m_clock.invalidate();
        m_ageAtReceipt = -1;
    }
    if (!duplicate) applyConfirmation(result);
    emit stateChanged();
    emit resultPublished(result, result.valid ? ageMs() : -1);
    return true;
}

void FaultLiveController::releaseExternal()
{
    if (!m_external) return;
    m_external = false;
    m_run.clear();
    m_state = kFaultStopped;
    m_stateDetail = QStringLiteral("外部模型已释放");
    clearConfirmation();
    m_current = FaultResult::unavailable(kFaultStopped, QStringLiteral("stopped"));
    m_clock.invalidate();
    emit stateChanged();
    emit resultPublished(m_current, -1);
}

} // namespace mlk
