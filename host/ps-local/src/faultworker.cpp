#include "faultworker.h"

#include <QDateTime>
#include <QJsonObject>
#include <QMetaObject>
#include <QTimer>
#include <QUuid>

namespace mlk {
namespace {
constexpr int kQueueLimit = 4096;
constexpr quint64 kKeepAfterWindowUs = 6000000ull;   /* 慢状态最长 6 s */
}

FaultWorker::FaultWorker(QObject *parent) : QObject(parent) {}

FaultWorker::~FaultWorker() { m_runtime.close(); }

void FaultWorker::enqueue(const NativeBlock &block)
{
    if (!block.dataValid || block.channel >= quint32(kChannelCount)) return;
    QMutexLocker locker(&m_mutex);
    m_queue.append(block);
    if (m_queue.size() > kQueueLimit) m_queue.remove(0, m_queue.size() - kQueueLimit);
}

void FaultWorker::requestDrain()
{
    QMetaObject::invokeMethod(this, "drain", Qt::QueuedConnection);
}

void FaultWorker::startRun(const FaultRunConfig &config)
{
    stopRun();
    m_config = config;
    m_run = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_seq = 0;
    m_nextEnd = 0;
    m_haveWindow = false;
    m_lastStart = 0;
    m_lastEnd = 0;
    for (auto &blocks : m_blocks) blocks.clear();
    m_lastUnavailable.clear();
    m_publishClock.invalidate();
    {
        QMutexLocker locker(&m_mutex);
        m_queue.clear();
    }
    m_state = kFaultLoading;
    emit stateChanged(m_state, QStringLiteral("加载故障运行包"));

    QString error;
    if (!m_preprocessor.loadAssets(config.root, &error)) {
        m_state = kFaultError;
        emit runFailed(error);
        return;
    }
    FaultRuntimeConfig runtimeConfig;
    runtimeConfig.root = config.root;
    runtimeConfig.backend = config.backend;
    if (!m_runtime.open(runtimeConfig, &error)) {
        m_state = kFaultError;
        emit runFailed(error);
        return;
    }
    if (!m_runtime.supportsClassMask()) {
        m_runtime.close();
        m_state = kFaultError;
        emit runFailed(QStringLiteral("故障运行库版本过旧，请安装匹配的温度屏蔽更新包"));
        return;
    }
    if (m_preprocessor.preprocessId() != m_runtime.preprocessId()) {
        m_runtime.close();
        m_state = kFaultError;
        emit runFailed(QStringLiteral("预处理版本与 model.json 不一致"));
        return;
    }

    m_active = true;
    m_state = kFaultWaiting;
    if (!m_timer) {
        m_timer = new QTimer(this);
        m_timer->setInterval(100);
        connect(m_timer, &QTimer::timeout, this, &FaultWorker::drain);
    }
    m_timer->start();
    emit runReady(m_run, m_runtime.modelId(), m_runtime.preprocessId());
    emit stateChanged(m_state, QStringLiteral("等待四秒窗口"));
}

void FaultWorker::stopRun()
{
    if (m_timer) m_timer->stop();
    if (m_active || m_runtime.isOpen()) {
        m_active = false;
        m_runtime.close();
        m_state = kFaultStopped;
        emit stateChanged(m_state, QStringLiteral("已停止"));
    }
}

void FaultWorker::prune()
{
    if (m_nextEnd == 0) return;
    const quint64 keep = m_nextEnd > kKeepAfterWindowUs ? m_nextEnd - kFaultWindowUs - 2000000ull : 0;
    for (auto &blocks : m_blocks) {
        int drop = 0;
        while (drop < blocks.size() && blocks[drop].endUs() < keep) ++drop;
        if (drop) blocks.remove(0, drop);
    }
}

bool FaultWorker::collectWindow(FaultWindowInput &window) const
{
    window = FaultWindowInput{};
    if (m_nextEnd <= kFaultWindowUs) return false;
    window.endUs = m_nextEnd;
    window.startUs = m_nextEnd - kFaultWindowUs;
    auto inside = [&window](const NativeBlock &block) {
        return block.dataValid && block.t0Us >= window.startUs && block.endUs() < window.endUs;
    };
    for (const auto &block : m_blocks[kVibration])
        if (inside(block)) window.vibration.append(block);
    for (const auto &block : m_blocks[kTemp])
        if (inside(block)) window.temperature.append(block);
    for (const auto &block : m_blocks[kCurrent])
        if (inside(block)) window.current.append(block);
    for (const auto &block : m_blocks[kSpeed])
        if (inside(block)) window.speed.append(block);
    return !window.vibration.isEmpty();
}

void FaultWorker::publishUnavailable(int state, const QString &reason, const FaultQuality &quality)
{
    /* 同一种不可用状态不按 100 ms 节拍重复发布；链路心跳由控制器负责。 */
    const QString key = QStringLiteral("%1:%2").arg(state).arg(reason);
    const bool repeated = key == m_lastUnavailable
        && m_publishClock.isValid() && m_publishClock.elapsed() < 1000;
    if (repeated) return;
    m_lastUnavailable = key;
    m_publishClock.restart();
    FaultResult result;
    result.run = m_run;
    result.model = m_runtime.modelId();
    if (result.model.isEmpty()) result.model = QStringLiteral("fault-single");
    result.preprocess = m_preprocessor.preprocessId();
    result.state = state;
    result.valid = false;
    result.classIndex = kFaultUnavailable;
    result.grade = -1;
    result.hasConfidence = false;
    result.backend = m_runtime.isOpen() ? m_config.backend : kFaultBackendUnknown;
    result.seq = m_seq;
    if (m_haveWindow) {
        result.startUs = m_lastStart;
        result.endUs = m_lastEnd;
    }
    result.reason = reason.left(kFaultReasonMaxBytes);
    result.quality = quality.toJson();
    m_state = state;
    emit predictionReady(result, quality);
    QJsonObject record{{QStringLiteral("run"), m_run}, {QStringLiteral("seq"), double(m_seq)},
                       {QStringLiteral("state"), faultStateName(state)},
                       {QStringLiteral("valid"), false}, {QStringLiteral("reason"), result.reason},
                       {QStringLiteral("quality"), quality.toJson()}};
    emit journalRecord(record);
}

void FaultWorker::drain()
{
    if (!m_active) return;
    QVector<NativeBlock> incoming;
    {
        QMutexLocker locker(&m_mutex);
        incoming.swap(m_queue);
    }
    for (const auto &block : incoming) {
        const int channel = int(block.channel);
        if (channel < 0 || channel >= kChannelCount) continue;
        if (channel == kVibration && !m_blocks[channel].isEmpty()) {
            const auto previous = m_blocks[channel].last().endUs();
            if (block.endUs() < previous || block.t0Us > previous + kFaultWindowUs) {
                // Time reset or long outage: do not wait for an obsolete future window.
                for (auto &history : m_blocks) history.clear();
                m_nextEnd = 0; m_haveWindow = false;
                publishUnavailable(kFaultWaiting, QStringLiteral("waiting_context"), FaultQuality{});
            }
        }
        m_blocks[channel].append(block);
    }

    const auto &vibration = m_blocks[kVibration];
    if (vibration.isEmpty()) {
        publishUnavailable(kFaultWaiting, QStringLiteral("waiting_context"), FaultQuality{});
        return;
    }
    const quint64 firstStart = vibration.first().t0Us;
    const quint64 latestEnd = vibration.last().endUs();
    if (m_nextEnd == 0) {
        if (latestEnd < firstStart + kFaultWindowUs) {
            FaultQuality quality;
            quality.blocks = vibration.size();
            publishUnavailable(kFaultWaiting, QStringLiteral("waiting_context"), quality);
            return;
        }
        m_nextEnd = firstStart + kFaultWindowUs;
    }
    if (latestEnd < m_nextEnd) return;
    /* 积压时只保留最新窗口：旧窗口不再补齐，避免节拍越拖越远。 */
    m_nextEnd += ((latestEnd - m_nextEnd) / kFaultStrideUs) * kFaultStrideUs;
    prune();

    FaultWindowInput window;
    if (!collectWindow(window)) {
        publishUnavailable(kFaultWaiting, QStringLiteral("waiting_context"), FaultQuality{});
        m_nextEnd += kFaultStrideUs;
        return;
    }

    QElapsedTimer timer;
    timer.start();
    FaultWindowOutput output;
    QString error;
    const bool built = m_preprocessor.build(window, output, &error);
    const quint32 preprocessUs = quint32(qMin<qint64>(timer.nsecsElapsed() / 1000,
                                                      4294967295ll));
    m_haveWindow = true;
    m_lastStart = window.startUs;
    m_lastEnd = window.endUs;

    if (!built) {
        publishUnavailable(kFaultDataInsufficient, output.reason, output.quality);
        m_nextEnd += kFaultStrideUs;
        return;
    }

    FaultEngineOutcome outcome;
    const quint32 excluded = output.quality.overtemperatureMasked ? MLK_FAULT_MASK_OVERTEMPERATURE : 0u;
    if (!m_runtime.predict(output.input, outcome, &error, excluded)) {
        publishUnavailable(kFaultError, QStringLiteral("engine_error"), output.quality);
        m_nextEnd += kFaultStrideUs;
        return;
    }
    if (!outcome.callOk || outcome.classIndex == kFaultUnavailable) {
        publishUnavailable(kFaultError, QStringLiteral("engine_invalid"), output.quality);
        m_nextEnd += kFaultStrideUs;
        return;
    }

    FaultResult result;
    result.run = m_run;
    result.model = m_runtime.modelId();
    result.preprocess = m_preprocessor.preprocessId();
    result.seq = ++m_seq;
    result.startUs = window.startUs;
    result.endUs = window.endUs;
    result.state = kFaultRunning;
    result.valid = true;
    result.classIndex = outcome.classIndex;
    result.grade = outcome.grade;
    result.hasConfidence = outcome.hasConfidence;
    result.confidence = outcome.confidence;
    result.backend = outcome.backend;
    result.preprocessUs = preprocessUs;
    result.inferUs = outcome.inferUs;
    result.quality = output.quality.toJson();
    m_state = kFaultRunning;
    emit predictionReady(result, output.quality);

    QJsonObject record{{QStringLiteral("run"), m_run}, {QStringLiteral("seq"), double(result.seq)},
                       {QStringLiteral("startUs"), faultTimeString(result.startUs)},
                       {QStringLiteral("endUs"), faultTimeString(result.endUs)},
                       {QStringLiteral("state"), faultStateName(result.state)},
                       {QStringLiteral("valid"), true},
                       {QStringLiteral("classIndex"), result.classIndex},
                       {QStringLiteral("fault"), result.fault()},
                       {QStringLiteral("grade"), result.grade},
                       {QStringLiteral("confidence"), result.confidence},
                       {QStringLiteral("backend"), faultBackendName(result.backend)},
                       {QStringLiteral("preprocessUs"), double(result.preprocessUs)},
                       {QStringLiteral("inferUs"), double(result.inferUs)},
                       {QStringLiteral("quality"), output.quality.toJson()}};
    emit journalRecord(record);
    m_nextEnd += kFaultStrideUs;
}

} // namespace mlk
