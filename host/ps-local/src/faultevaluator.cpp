#include "faultevaluator.h"

#include "faultdataset.h"
#include "faultruntime.h"
#include "faulttypes.h"

#include <QFile>
#include <QJsonArray>
#include <QMetaObject>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace mlk {
namespace {

constexpr int kClassCount = 5;
constexpr int kGradeCount = 4;

QVector<QVector<int>> zeroMatrix(int rows, int columns)
{
    QVector<QVector<int>> matrix(rows);
    for (auto &row : matrix) row.fill(0, columns);
    return matrix;
}

} // namespace

double FaultEvalReport::classAccuracy() const
{
    return evaluated ? double(classCorrect) / double(evaluated) : 0.0;
}

double FaultEvalReport::jointAccuracy() const
{
    return evaluated ? double(jointCorrect) / double(evaluated) : 0.0;
}

double FaultEvalReport::faultJointAccuracy() const
{
    return faultSamples ? double(faultJointCorrect) / double(faultSamples) : 0.0;
}

double FaultEvalReport::windowsPerSecond() const
{
    return meanInferUs > 0.0 ? 1000000.0 / meanInferUs : 0.0;
}

QJsonObject FaultEvalReport::toJson() const
{
    QJsonArray confusion;
    for (const auto &row : classConfusion) {
        QJsonArray line;
        for (int value : row) line.append(value);
        confusion.append(line);
    }
    QJsonArray grades;
    for (const auto &row : gradeConfusion) {
        QJsonArray line;
        for (int value : row) line.append(value);
        grades.append(line);
    }
    QJsonArray joint;
    for (int value : classJointCorrect) joint.append(value);
    return QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("mlk.fault.eval_report.v1")},
        {QStringLiteral("model"), model},
        {QStringLiteral("preprocess"), preprocess},
        {QStringLiteral("backend"), backend},
        {QStringLiteral("samples"), evaluated},
        {QStringLiteral("classCorrect"), classCorrect},
        {QStringLiteral("jointCorrect"), jointCorrect},
        {QStringLiteral("classAccuracy"), classAccuracy()},
        {QStringLiteral("jointAccuracy"), jointAccuracy()},
        {QStringLiteral("faultSamples"), faultSamples},
        {QStringLiteral("faultJointCorrect"), faultJointCorrect},
        {QStringLiteral("faultJointAccuracy"), faultJointAccuracy()},
        {QStringLiteral("meanInferUs"), meanInferUs},
        {QStringLiteral("medianInferUs"), medianInferUs},
        {QStringLiteral("p95InferUs"), p95InferUs},
        {QStringLiteral("maxInferUs"), maxInferUs},
        {QStringLiteral("windowsPerSecond"), windowsPerSecond()},
        {QStringLiteral("classConfusion"), confusion},
        {QStringLiteral("gradeConfusion"), grades},
        {QStringLiteral("classJointCorrect"), joint},
    };
}

FaultEvaluator::FaultEvaluator(QObject *parent)
    : QObject(parent)
{
}

void FaultEvaluator::stopRun()
{
    m_cancel.store(true);
}

void FaultEvaluator::startRun(const QString &packageRoot, const QString &datasetRoot, int backend)
{
    if (m_running) {
        emit failed(QStringLiteral("评估已在进行"));
        return;
    }
    m_running = true;
    FaultRuntime runtime;
    const auto failRun = [this, &runtime](const QString &detail) {
        runtime.close();
        m_running = false;
        emit failed(detail);
    };
    if (m_cancel.load()) { failRun(QStringLiteral("评估已取消")); return; }

    FaultDataset dataset;
    QString error;
    emit stateChanged(QStringLiteral("读取评估集"));
    if (!dataset.load(datasetRoot, &error)) {
        failRun(error);
        return;
    }

    QFile inputs(dataset.inputsPath());
    if (!inputs.open(QIODevice::ReadOnly)) {
        failRun(QStringLiteral("无法读取评估输入：%1").arg(inputs.errorString()));
        return;
    }

    emit stateChanged(QStringLiteral("加载运行包"));
    FaultRuntimeConfig config;
    config.root = packageRoot;
    config.backend = backend;
    if (!runtime.open(config, &error)) {
        failRun(error);
        return;
    }
    if (dataset.modelId() != runtime.modelId()) {
        failRun(QStringLiteral("评估集与运行包模型身份不符"));
        return;
    }

    const auto rows = dataset.rows();
    FaultEvalReport report;
    report.model = runtime.modelId();
    report.preprocess = runtime.preprocessId();
    report.backend = faultBackendName(backend);
    report.samples = int(rows.size());
    report.classConfusion = zeroMatrix(kClassCount, kClassCount);
    report.gradeConfusion = zeroMatrix(kGradeCount, kGradeCount);
    report.classJointCorrect.fill(0, kClassCount);

    const qint64 bytes = qint64(dataset.inputFloats()) * 4;
    QVector<float> values(dataset.inputFloats());
    QVector<int> latencies;
    latencies.reserve(rows.size());
    double totalUs = 0.0;

    emit stateChanged(QStringLiteral("逐窗评估"));
    for (int index = 0; index < rows.size(); ++index) {
        if (m_cancel.load()) {
            failRun(QStringLiteral("评估已取消"));
            return;
        }
        if (!inputs.seek(qint64(index) * bytes)) {
            failRun(QStringLiteral("读取第 %1 窗失败").arg(index));
            return;
        }
        const auto raw = inputs.read(bytes);
        if (raw.size() != bytes) {
            failRun(QStringLiteral("第 %1 窗数据不完整").arg(index));
            return;
        }
        bool finite = true;
        for (int i = 0; i < dataset.inputFloats(); ++i) {
            float value = 0.0f;
            std::memcpy(&value, raw.constData() + i * 4, 4);
            if (!std::isfinite(value)) { finite = false; break; }
            values[i] = value;
        }
        if (!finite) {
            failRun(QStringLiteral("第 %1 窗含非有限值").arg(index));
            return;
        }

        FaultEngineOutcome outcome;
        if (!runtime.predict(values, outcome, &error) || !outcome.callOk
            || outcome.classIndex == kFaultUnavailable) {
            failRun(QStringLiteral("第 %1 窗推断失败：%2")
                            .arg(index).arg(error.isEmpty() ? QStringLiteral("引擎未返回结果") : error));
            return;
        }

        const auto &row = rows[index];
        const bool classHit = outcome.classIndex == row.targetClass;
        const bool gradeHit = classHit && outcome.grade == row.targetGrade;
        report.classCorrect += classHit ? 1 : 0;
        report.jointCorrect += gradeHit ? 1 : 0;
        if (row.targetClass != kFaultNormal) {
            ++report.faultSamples;
            report.faultJointCorrect += gradeHit ? 1 : 0;
        }
        report.classConfusion[row.targetClass][outcome.classIndex] += 1;
        if (gradeHit) report.classJointCorrect[row.targetClass] += 1;
        const int targetGrade = qBound(0, row.targetGrade, kGradeCount - 1);
        const int predictedGrade = qBound(0, outcome.grade < 0 ? 0 : outcome.grade, kGradeCount - 1);
        report.gradeConfusion[targetGrade][predictedGrade] += 1;
        ++report.evaluated;
        totalUs += outcome.inferUs;
        latencies.append(int(outcome.inferUs));
        report.maxInferUs = qMax(report.maxInferUs, double(outcome.inferUs));

        if ((index + 1) % 20 == 0 || index + 1 == rows.size()) emit progress(index + 1, rows.size());
    }

    runtime.close();
    report.meanInferUs = report.evaluated ? totalUs / report.evaluated : 0.0;
    if (!latencies.isEmpty()) {
        std::sort(latencies.begin(), latencies.end());
        const auto percentile = [&latencies](double ratio) {
            const int index = qBound(0, int(std::ceil(ratio * latencies.size())) - 1,
                                     latencies.size() - 1);
            return double(latencies[index]);
        };
        report.medianInferUs = percentile(0.5);
        report.p95InferUs = percentile(0.95);
    }
    m_running = false;
    emit finished(report);
}

FaultEvalController::FaultEvalController(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<mlk::FaultEvalReport>("mlk::FaultEvalReport");
}

FaultEvalController::~FaultEvalController()
{
    /* 取消标志用直连设置：startRun 运行期间工作线程事件循环被占用，队列投递不会执行。 */
    if (m_worker) QMetaObject::invokeMethod(m_worker, "stopRun", Qt::DirectConnection);
    m_thread.quit();
    m_thread.wait();
}

void FaultEvalController::ensureWorker()
{
    if (m_worker) return;
    m_worker = new FaultEvaluator;
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &FaultEvaluator::stateChanged, this, &FaultEvalController::stateChanged);
    connect(m_worker, &FaultEvaluator::progress, this, &FaultEvalController::progress);
    connect(m_worker, &FaultEvaluator::finished, this,
            [this](const FaultEvalReport &report) {
                m_running = false;
                emit finished(report);
            });
    connect(m_worker, &FaultEvaluator::failed, this, [this](const QString &error) {
        m_running = false;
        emit failed(error);
    });
    m_thread.start();
}

bool FaultEvalController::start(const QString &packageRoot, const QString &datasetRoot, int backend,
                                QString *error)
{
    if (m_running) {
        if (error) *error = QStringLiteral("评估已在进行，先停止再启动");
        return false;
    }
    if (packageRoot.isEmpty()) {
        if (error) *error = QStringLiteral("先填写故障运行包目录");
        return false;
    }
    if (datasetRoot.isEmpty()) {
        if (error) *error = QStringLiteral("先填写评估集目录");
        return false;
    }
    ensureWorker();
    m_worker->prepareRun();
    m_running = true;
    QMetaObject::invokeMethod(m_worker, "startRun", Qt::QueuedConnection,
                              Q_ARG(QString, packageRoot), Q_ARG(QString, datasetRoot),
                              Q_ARG(int, backend));
    return true;
}

void FaultEvalController::stop()
{
    if (!m_worker || !m_running) return;
    /* 工作线程在 startRun 内循环，队列投递要等它返回才执行，因此直连置取消位。 */
    QMetaObject::invokeMethod(m_worker, "stopRun", Qt::DirectConnection);
}

} // namespace mlk
