#include "faultruntime.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>

namespace mlk {
namespace {

bool fail(QString *error, const QString &text)
{
    if (error) *error = text;
    return false;
}

QString readText(const QString &path, qint64 maximum, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(error, QStringLiteral("无法读取 %1：%2").arg(QFileInfo(path).fileName(), file.errorString()));
        return QString();
    }
    if (file.size() <= 0 || file.size() > maximum) {
        fail(error, QStringLiteral("%1 长度无效").arg(QFileInfo(path).fileName()));
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

bool verifyHash(const QString &path, const QString &expected, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("无法读取 %1").arg(QFileInfo(path).fileName()));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return fail(error, QStringLiteral("无法计算 %1 的哈希").arg(QFileInfo(path).fileName()));
    const QString actual = QString::fromLatin1(hash.result().toHex());
    if (actual.compare(expected, Qt::CaseInsensitive) != 0)
        return fail(error, QStringLiteral("%1 哈希不符").arg(QFileInfo(path).fileName()));
    return true;
}

} // namespace

FaultRuntime::~FaultRuntime() { close(); }

bool FaultRuntime::verifyPackage(const QString &root, int backend, QString *modelId,
                                 QString *preprocessId, QString *error)
{
    const QDir dir(root);
    if (!dir.exists()) return fail(error, QStringLiteral("故障包目录不存在：%1").arg(root));
    const QString manifestPath = dir.filePath(QStringLiteral("model.json"));
    QString text = readText(manifestPath, 1024 * 1024, error);
    if (text.isEmpty()) return false;
    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(text.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return fail(error, QStringLiteral("model.json 无法解析"));
    const auto o = doc.object();
    if (o.value(QStringLiteral("schema")).toString() != QLatin1String(kFaultRuntimeSchema))
        return fail(error, QStringLiteral("model.json schema 不符"));
    if (o.value(QStringLiteral("task")).toString() != QLatin1String(kFaultTask))
        return fail(error, QStringLiteral("model.json 任务不符"));
    if (o.value(QStringLiteral("contract")).toString() != QLatin1String(kFaultContract))
        return fail(error, QStringLiteral("model.json 分类契约不符"));
    if (o.value(QStringLiteral("preprocess")).toString() != QLatin1String(kFaultPreprocess))
        return fail(error, QStringLiteral("model.json 预处理版本不符"));
    const auto input = o.value(QStringLiteral("input")).toObject();
    if (input.value(QStringLiteral("layout")).toString() != QLatin1String(kFaultInputLayout)
        || input.value(QStringLiteral("floats")).toInt() != MLK_FAULT_INPUT_FLOATS)
        return fail(error, QStringLiteral("model.json 输入布局不符"));
    const auto classes = o.value(QStringLiteral("classes")).toArray();
    if (classes.size() != kFaultClassCount)
        return fail(error, QStringLiteral("model.json 类别表数量不符"));
    for (int i = 0; i < kFaultClassCount; ++i)
        if (classes[i].toString() != faultName(i))
            return fail(error, QStringLiteral("model.json 类别顺序不符"));
    const auto runtime = o.value(QStringLiteral("runtime")).toObject();
    if (backend != kFaultBackendCpu && backend != kFaultBackendCpuCnnNpu)
        return fail(error, QStringLiteral("不支持的故障后端"));
    if (runtime.value(QStringLiteral("abi")).toInt() != MLK_FAULT_ENGINE_ABI)
        return fail(error, QStringLiteral("model.json ABI 不符"));
    const auto backends = runtime.value(QStringLiteral("backends")).toArray();
    const QString requested = backend == kFaultBackendCpuCnnNpu ? QStringLiteral("cpu_cnn_npu")
                                                                : QStringLiteral("cpu");
    bool listed = false;
    for (const auto &value : backends)
        if (value.toString() == requested) listed = true;
    if (!listed) return fail(error, QStringLiteral("运行包未声明后端 %1").arg(requested));
    const QString library = runtime.value(QStringLiteral("library")).toString();
    if (library.isEmpty() || library.contains(QLatin1Char('/'))
        || library.contains(QLatin1Char('\\')) || library.contains(QLatin1Char(':'))
        || library.contains(QLatin1String("..")))
        return fail(error, QStringLiteral("model.json 运行库名称无效"));
    if (!QFile::exists(dir.filePath(library)))
        return fail(error, QStringLiteral("缺少运行库 %1").arg(library));
    if (!QFile::exists(dir.filePath(QStringLiteral("preprocess.json"))))
        return fail(error, QStringLiteral("缺少 preprocess.json"));

    const auto files = o.value(QStringLiteral("files")).toObject();
    for (const auto &name : {library, QStringLiteral("preprocess.json"), QStringLiteral("weights.bin"),
                            QStringLiteral("weights.tsv"), QStringLiteral("thresholds.bin"),
                            QStringLiteral("qparams.tsv")})
        if (!files.contains(name) || files.value(name).toString().size() != 64)
            return fail(error, QStringLiteral("model.json 缺少资产哈希：%1").arg(name));
    for (auto it = files.constBegin(); it != files.constEnd(); ++it) {
        const QString name = it.key();
        if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))
            || name.contains(QLatin1Char(':')) || name.contains(QLatin1String("..")))
            return fail(error, QStringLiteral("model.json 资产名称无效"));
        const QString filePath = dir.filePath(name);
        if (!QFile::exists(filePath))
            return fail(error, QStringLiteral("缺少资产 %1").arg(name));
        if (!verifyHash(filePath, it.value().toString(), error)) return false;
    }

    if (modelId) *modelId = o.value(QStringLiteral("model")).toString();
    if (preprocessId) *preprocessId = o.value(QStringLiteral("preprocess")).toString();
    return true;
}

bool FaultRuntime::open(const FaultRuntimeConfig &config, QString *error)
{
    close();
    m_config = config;
    m_config.root = QDir(config.root).absolutePath();
    QString modelId, preprocessId;
    if (!verifyPackage(m_config.root, config.backend, &modelId, &preprocessId, error))
        return false;

    const QDir dir(m_config.root);
    const QString manifest = readText(dir.filePath(QStringLiteral("model.json")), 1024 * 1024, error);
    const auto runtime = QJsonDocument::fromJson(manifest.toUtf8()).object()
                             .value(QStringLiteral("runtime")).toObject();
    const QString library = dir.filePath(runtime.value(QStringLiteral("library")).toString());

    m_library.setFileName(library);
    if (!m_library.load())
        return fail(error, QStringLiteral("加载运行库失败：%1").arg(m_library.errorString()));
    const auto abi = reinterpret_cast<mlk_fault_abi_fn>(
        m_library.resolve("mlk_fault_abi"));
    const auto create = reinterpret_cast<mlk_fault_create_fn>(
        m_library.resolve("mlk_fault_create"));
    m_predict = reinterpret_cast<mlk_fault_predict_fn>(m_library.resolve("mlk_fault_predict"));
    m_predictMasked = reinterpret_cast<mlk_fault_predict_masked_fn>(m_library.resolve("mlk_fault_predict_masked"));
    m_destroy = reinterpret_cast<mlk_fault_destroy_fn>(m_library.resolve("mlk_fault_destroy"));
    if (!abi || !create || !m_predict || !m_destroy) {
        const QString missing = m_library.errorString();
        close();
        return fail(error, QStringLiteral("运行库缺少 ABI 符号：%1").arg(missing));
    }
    if (abi() != MLK_FAULT_ENGINE_ABI) {
        close();
        return fail(error, QStringLiteral("运行库 ABI 版本不符"));
    }
    m_error[0] = '\0';
    m_context = create(qPrintable(m_config.root), m_config.backend, m_error, sizeof(m_error));
    if (!m_context) {
        const QString reason = QString::fromLocal8Bit(m_error);
        close();
        return fail(error, reason.isEmpty() ? QStringLiteral("引擎初始化失败") : reason);
    }
    m_modelId = modelId;
    m_preprocessId = preprocessId;
    return true;
}

void FaultRuntime::close()
{
    if (m_context && m_destroy) m_destroy(m_context);
    m_context = nullptr;
    m_predict = nullptr;
    m_predictMasked = nullptr;
    m_destroy = nullptr;
    if (m_library.isLoaded()) m_library.unload();
}

bool FaultRuntime::predict(const QVector<float> &input, FaultEngineOutcome &outcome, QString *error,
                           quint32 excludedClasses)
{
    outcome = FaultEngineOutcome{};
    if (!m_context || !m_predict) return fail(error, QStringLiteral("引擎尚未初始化"));
    if (excludedClasses && !m_predictMasked)
        return fail(error, QStringLiteral("故障运行库不支持类别屏蔽，请安装匹配的温度屏蔽更新包"));
    if (input.size() != MLK_FAULT_INPUT_FLOATS)
        return fail(error, QStringLiteral("输入长度不符：%1").arg(input.size()));
    mlk_fault_result raw;
    raw.struct_size = sizeof(mlk_fault_result);
    raw.valid = 0;
    raw.class_index = -1;
    raw.grade = -1;
    raw.confidence = -1.0f;
    raw.backend = 0;
    raw.infer_us = 0;
    raw.reserved = 0;
    m_error[0] = '\0';
    const int status = excludedClasses
        ? m_predictMasked(m_context, input.constData(), size_t(input.size()), excludedClasses,
                          &raw, m_error, sizeof(m_error))
        : m_predict(m_context, input.constData(), size_t(input.size()), &raw, m_error, sizeof(m_error));
    if (status != 0) {
        const QString reason = QString::fromLocal8Bit(m_error);
        return fail(error, reason.isEmpty() ? QStringLiteral("引擎调用失败") : reason);
    }
    if (raw.struct_size != sizeof(mlk_fault_result))
        return fail(error, QStringLiteral("引擎结果结构大小不符"));
    outcome.callOk = true;
    outcome.backend = raw.backend == MLK_FAULT_CPU ? kFaultBackendCpu
        : raw.backend == MLK_FAULT_CPU_CNN_NPU ? kFaultBackendCpuCnnNpu : kFaultBackendUnknown;
    outcome.inferUs = raw.infer_us;
    if (raw.backend != uint32_t(m_config.backend) || raw.reserved != 0 || raw.valid > 1)
        return fail(error, QStringLiteral("引擎返回后端或保留字段不符"));
    if (!raw.valid) {
        outcome.classIndex = kFaultUnavailable;
        outcome.grade = -1;
        return true;
    }
    if (raw.class_index < kFaultNormal || raw.class_index >= kFaultClassCount
        || !faultGradeValid(raw.class_index, raw.grade)
        || !std::isfinite(raw.confidence) || raw.confidence < 0.0f || raw.confidence > 1.0f) {
        return fail(error, QStringLiteral("引擎结果超出五类契约"));
    }
    outcome.classIndex = raw.class_index;
    outcome.grade = raw.grade;
    outcome.hasConfidence = true;
    outcome.confidence = double(raw.confidence);
    return true;
}

} // namespace mlk
