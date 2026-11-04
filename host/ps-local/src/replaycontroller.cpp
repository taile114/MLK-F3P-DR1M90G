#include "replaycontroller.h"
#include "npu.h"
#include "rul_engine_api.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace mlk {
namespace {
constexpr qint64 kWindowBytes = MLK_RUL_INPUT_FLOATS * 4;
std::array<float, MLK_RUL_INPUT_FLOATS> decode(const QByteArray &bytes) {
    if (bytes.size() != kWindowBytes) throw std::runtime_error("Incomplete input window");
    std::array<float, MLK_RUL_INPUT_FLOATS> values;
    for (int i = 0; i < MLK_RUL_INPUT_FLOATS; ++i) {
        const auto bits = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData()) + i * 4);
        std::memcpy(&values[size_t(i)], &bits, 4);
        if (!std::isfinite(values[size_t(i)])) throw std::runtime_error("Nonfinite replay input");
    }
    return values;
}
}
QVector<float> loadRulLabels(const QString &root, int windowCount) {
    if (windowCount <= 0) return {};
    QVector<float> labels;
    QFile csv(QDir(root).filePath(QStringLiteral("labels.csv")));
    if (csv.open(QIODevice::ReadOnly) && csv.size() <= qint64(windowCount) * 32 + 4096) {
        labels.resize(windowCount);
        QVector<bool> seen(windowCount, false);
        for (const QByteArray &raw : csv.readAll().split('\n')) {
            const auto cells = raw.trimmed().split(',');
            if (cells.size() < 2) continue;
            bool ok = false;
            const auto index = cells.at(0).trimmed().toInt(&ok);
            if (!ok || index < 0 || index >= windowCount) continue;
            const auto value = cells.at(1).trimmed().toFloat(&ok);
            if (!ok || !std::isfinite(value)) continue;
            labels[index] = value;
            seen[index] = true;
        }
        if (!seen.contains(false)) return labels;
        labels.clear();
    }
    QFile binary(QDir(root).filePath(QStringLiteral("labels_f32.bin")));
    if (!binary.open(QIODevice::ReadOnly) || binary.size() != qint64(windowCount) * 4) return {};
    const auto bytes = binary.readAll();
    labels.resize(windowCount);
    for (int i = 0; i < windowCount; ++i) {
        const auto bits = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData()) + i * 4);
        std::memcpy(&labels[i], &bits, 4);
        if (!std::isfinite(labels[i])) { labels.clear(); break; }
    }
    return labels;
}
void ReplayWorker::load(const QString &root, int backend) {
    m_input.close(); m_count = 0; m_labels.clear(); npu_deinit();
    try {
        QFile metadata(QDir(root).filePath(QStringLiteral("model.json")));
        if (!metadata.open(QIODevice::ReadOnly) || metadata.size() > 65536) throw std::runtime_error("Model manifest missing/oversized");
        const auto doc = QJsonDocument::fromJson(metadata.readAll());
        const auto m = doc.object();
        if (m.value("schema").toInt() != 1 || m.value("engine_abi").toInt() != 1
            || m.value("model_id").toString() != QStringLiteral("motor_stable23_v1")
            || m.value("feature_version").toString() != QStringLiteral("motor_stable23_v1")
            || m.value("shape").toArray() != QJsonArray{1, 23, 110}
            || m.value("dtype").toString() != QStringLiteral("little_endian_float32")
            || !m.value("normalized").toBool()
            || m.value("execution_plan").toString() != QStringLiteral("ff6")
            || m.value("quantization_plan").toString() != QStringLiteral("qparams_ff6.tsv")
            || m.value("npu_branches").toInt() != 6
            || m.value("rul_unit").toString() != QStringLiteral("observed_aging_test_session"))
            throw std::runtime_error("Incompatible model/input contract");
        const auto dataset = m.value("dataset_id").toString();
        const int count = m.value("windows").toInt();
        if (!((dataset == QStringLiteral("val184") && count == 184)
              || (dataset == QStringLiteral("test165") && count == 165))) throw std::runtime_error("Unknown replay dataset");
        const auto hashes = m.value("sha256").toObject();
        if (hashes.size() != 5) throw std::runtime_error("Unexpected model file manifest");
        for (const char *name : {"weights.bin", "weights.tsv", "qparams_ff6.tsv", "input_f32.bin", "libmlk_rul.so"}) {
            QFile file(QDir(root).filePath(QLatin1String(name)));
            const auto expected = hashes.value(QLatin1String(name)).toString().toLatin1();
            if (expected.size() != 64 || !file.open(QIODevice::ReadOnly)) throw std::runtime_error("Required model file/hash missing");
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if (!hash.addData(&file) || hash.result().toHex() != expected) throw std::runtime_error("Model file checksum mismatch");
        }
        m_input.setFileName(QDir(root).filePath(QStringLiteral("input_f32.bin")));
        if (!m_input.open(QIODevice::ReadOnly) || m_input.size() != qint64(count) * kWindowBytes)
            throw std::runtime_error("Wrong replay length");
        /* 标签文件可选：缺失时只影响面板上的偏差显示，不影响回放与推理。 */
        m_labels = loadRulLabels(root, count);
        if (npu_init_backend(root.toUtf8().constData(), backend)) throw std::runtime_error(npu_error());
        const auto first = decode(m_input.read(kWindowBytes));
        npu_result_t result{};
        for (int i = 0; i < 5; ++i) if (npu_infer(first.data(), int(first.size()), &result)) throw std::runtime_error(npu_error());
        m_count = quint32(count);
        emit loaded(m_count, m.value("model_id").toString(), dataset, QString());
    } catch (const std::exception &e) {
        m_input.close(); npu_deinit();
        emit loaded(0, QString(), QString(), QString::fromUtf8(e.what()));
    }
}
void ReplayWorker::predict(quint32 index) {
    try {
        if (index >= m_count || !m_input.seek(qint64(index) * kWindowBytes)) throw std::runtime_error("Invalid replay window");
        const auto input = decode(m_input.read(kWindowBytes));
        npu_result_t result{};
        if (npu_infer(input.data(), int(input.size()), &result)) throw std::runtime_error(npu_error());
        const float label = m_labels.isEmpty() ? std::numeric_limits<float>::quiet_NaN()
                                               : m_labels.at(int(index));
        emit predicted(index, result.rul, result.infer_us, result.backend, label, QString());
    } catch (const std::exception &e) {
        emit predicted(index, 0, 0, 0, std::numeric_limits<float>::quiet_NaN(), QString::fromUtf8(e.what()));
    }
}
void ReplayWorker::unload() { m_input.close(); m_count = 0; m_labels.clear(); npu_deinit(); emit released(); }
void ReplayWorker::record(const QByteArray &wire, const QString &directory, bool encrypt,
                          const QString &password, int months, int minFreeMb) {
    Frame frame; QString error;
    if (!parseFrame(wire, frame) || !m_journal.save(frame, directory, encrypt, password, months, minFreeMb, &error))
        emit recordFailed(error);
}

ReplayController::ReplayController(QObject *parent) : QObject(parent), m_worker(new ReplayWorker) {
    m_status.ver = kInferenceVersion; m_status.type = kTypeDiagnosis;
    m_status.task = 1; m_status.source = kDatasetReplay; m_status.rulUnit = kObservedAgingSession;
    m_status.modelId = QStringLiteral("motor_stable23_v1");
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(this, &ReplayController::loadRequested, m_worker, &ReplayWorker::load);
    connect(this, &ReplayController::predictRequested, m_worker, &ReplayWorker::predict);
    connect(this, &ReplayController::unloadRequested, m_worker, &ReplayWorker::unload);
    connect(this, &ReplayController::recordRequested, m_worker, &ReplayWorker::record);
    connect(m_worker, &ReplayWorker::recordFailed, this, &ReplayController::recordFailed);
    connect(m_worker, &ReplayWorker::loaded, this, [this](quint32 count, const QString &model, const QString &dataset, const QString &error) {
        m_busy = false;
        if (m_stopping) { release(); return; }
        if (!error.isEmpty()) {
            m_play = m_step = false; m_status.inferenceState = kInferenceError;
            m_status.message = error; m_status.backend = 0; publish(); return;
        }
        m_loaded = true; m_status.windowCount = count; m_status.modelId = model; m_status.datasetId = dataset;
        /* 换数据集即重新开始统计偏差。 */
        m_deviationSum = 0; m_deviationCount = 0;
        m_status.rulDeviationAvg = 0; m_status.rulDeviationCount = 0;
        m_status.rulLabel = 0; m_status.rulLabelValid = 0;
        m_status.message.clear(); m_status.inferenceState = kReady;
        if (m_next >= count) { m_play = m_step = false; m_next = 0; m_status.message = QStringLiteral("窗口索引超出范围"); }
        publish(); if (m_play || m_step) next();
    });
    connect(m_worker, &ReplayWorker::predicted, this,
            [this](quint32 index, float rul, quint32 us, quint8 backend, float label, const QString &error) {
        m_busy = false;
        if (m_stopping) { release(); return; }
        if (!error.isEmpty()) {
            m_play = m_step = false; m_status.valid = 0; m_status.inferenceState = kInferenceError;
            m_status.backend = 0;
            m_status.message = error; publish(); release(); return;
        }
        /* 界面显示引擎实际使用的后端，而不是请求值。 */
        m_status.backend = backend;
        m_status.rulValue = rul; m_status.inferUs = us; m_status.valid = kValidRul;
        /* 运行包没有标签文件时 worker 回调 NaN，界面不显示偏差。 */
        m_status.rulLabelValid = std::isfinite(label) ? 1 : 0;
        m_status.rulLabel = m_status.rulLabelValid ? label : 0.0f;
        /* 逐窗口累计绝对相对偏差，供面板显示本次回放的平均偏差。 */
        if (m_status.rulLabelValid && m_status.rulLabel > 0.0f) {
            m_deviationSum += std::abs(double(rul) - double(m_status.rulLabel)) * 100.0 / double(m_status.rulLabel);
            ++m_deviationCount;
        }
        m_status.rulDeviationCount = m_deviationCount;
        m_status.rulDeviationAvg = m_deviationCount
            ? float(m_deviationSum / double(m_deviationCount)) : 0.0f;
        m_status.tsUs = quint64(QDateTime::currentMSecsSinceEpoch()) * 1000;
        m_status.windowIndex = index; m_next = index + 1; m_step = false;
        if (m_next >= m_status.windowCount) { m_play = false; m_status.inferenceState = kCompleted; }
        else m_status.inferenceState = m_play ? kRunning : kPaused;
        publish(); emit resultReady(m_status);
        if (m_play) m_timer.start(m_interval);
    });
    connect(m_worker, &ReplayWorker::released, this, [this] {
        m_busy = m_loaded = false;
        if (m_stopping) {
            m_stopping = false; m_next = 0; m_status.windowIndex = 0;
            m_status.inferenceState = kStopped; m_status.backend = 0; publish();
        }
    });
    m_timer.setSingleShot(true); connect(&m_timer, &QTimer::timeout, this, &ReplayController::next);
    connect(&m_heartbeat, &QTimer::timeout, this, &ReplayController::publish); m_heartbeat.start(1000);
    m_thread.start();
}
ReplayController::~ReplayController() {
    m_timer.stop(); m_heartbeat.stop();
    QMetaObject::invokeMethod(m_worker, "unload", Qt::BlockingQueuedConnection);
    m_thread.quit(); m_thread.wait();
}
bool ReplayController::setRoot(const QString &root) {
    if (m_busy || m_loaded) return root == m_root;
    m_root = root; return true;
}
void ReplayController::publish() { ++m_status.seq; emit changed(m_status); }
void ReplayController::release() {
    m_deviationSum = 0; m_deviationCount = 0;
    m_status.rulDeviationAvg = 0; m_status.rulDeviationCount = 0;
    m_busy = true; emit unloadRequested();
}
void ReplayController::next() {
    if (m_busy || !m_loaded || (!m_play && !m_step)) return;
    m_busy = true; m_status.inferenceState = kRunning;
    m_status.message.clear(); publish(); emit predictRequested(m_next);
}
QString ReplayController::control(quint8 action, quint8 backend, quint32 index, quint32 interval) {
    if (action < kReplayStart || action > kReplayStatus || backend < 1 || backend > 2
        || interval < 100 || interval > 60000) return QStringLiteral("无效回放命令");
    if (action == kReplayStatus) { publish(); return {}; }
    if (action == kReplayStop) {
        m_play = m_step = false; m_timer.stop(); m_stopping = true; m_next = 0;
        m_status.valid = 0; m_status.message.clear(); m_status.inferenceState = kStopping; publish();
        if (!m_busy) release();
        return {};
    }
    if (m_stopping) return QStringLiteral("正在停止，请稍后重试");
    if (action == kReplayPause) {
        m_play = m_step = false; m_timer.stop();
        if (m_loaded) m_status.inferenceState = kPaused;
        publish(); return {};
    }
    if (m_busy) return QStringLiteral("推理任务正在执行");
    if (action == kReplaySeek) {
        if (!m_loaded || index >= m_status.windowCount) return QStringLiteral("窗口索引不可用");
        m_play = m_step = false; m_timer.stop(); m_next = index;
        m_status.windowIndex = index; m_status.valid = 0; m_status.inferenceState = kPaused; publish(); return {};
    }
    m_timer.stop(); m_interval = int(interval);
    m_play = action == kReplayStart; m_step = action == kReplayStep;
    /* 停止后重新开始一律从 0 起；暂停中若手工改了窗口编号，则从该编号继续。 */
    const bool fromStop = m_status.inferenceState == kStopped || m_status.inferenceState == kStopping;
    if (action == kReplayStart && !fromStop && index != m_status.windowIndex) m_next = index;
    if (action == kReplayStart && fromStop) m_next = 0;
    if (!m_loaded || m_status.backend != backend) {
        if (m_root.isEmpty()) return QStringLiteral("模型目录未配置");
        m_next = fromStop ? 0 : index; m_status.valid = 0; m_status.windowCount = 0; m_status.datasetId.clear();
        m_status.backend = backend; m_status.inferenceState = kLoading;
        m_status.message.clear(); m_loaded = false; m_busy = true; publish(); emit loadRequested(m_root, backend);
    } else {
        if (m_next >= m_status.windowCount) m_next = 0;
        next();
    }
    return {};
}
}
