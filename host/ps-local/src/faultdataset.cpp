#include "faultdataset.h"
#include "faulttypes.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStringList>

namespace mlk {
namespace {

constexpr const char *kEvalSchema = "mlk.fault.evalset.v1";
constexpr int kInputFloats = 27683;
constexpr qint64 kMaxIndexBytes = 16 * 1024 * 1024;

bool fail(QString *error, const QString &text)
{
    if (error) *error = text;
    return false;
}

QStringList splitCsv(const QString &line)
{
    return line.split(QLatin1Char(','));
}

int columnOf(const QStringList &header, const char *name)
{
    return header.indexOf(QLatin1String(name));
}

int toInt(const QString &text, int fallback)
{
    bool ok = false;
    const int value = text.trimmed().toInt(&ok);
    return ok ? value : fallback;
}

} // namespace

bool FaultDataset::load(const QString &root, QString *error)
{
    m_loaded = false;
    m_rows.clear();
    m_inputsPath.clear();
    m_modelId.clear();
    m_inputFloats = 0;
    m_root = root;

    const QDir directory(root);
    if (!directory.exists()) return fail(error, QStringLiteral("评估集目录不存在：%1").arg(root));
    QFile manifest(directory.filePath(QStringLiteral("evalset.json")));
    if (!manifest.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("缺少 evalset.json：%1").arg(manifest.fileName()));
    if (manifest.size() <= 0 || manifest.size() > 256 * 1024) {
        manifest.close();
        return fail(error, QStringLiteral("evalset.json 长度无效"));
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(manifest.readAll(), &parseError);
    manifest.close();
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail(error, QStringLiteral("evalset.json 无法解析"));
    const auto object = document.object();
    if (object.value(QStringLiteral("schema")).toString() != QLatin1String(kEvalSchema))
        return fail(error, QStringLiteral("评估集 schema 不符"));
    m_modelId = object.value(QStringLiteral("model")).toString();
    if (m_modelId.isEmpty() || object.value(QStringLiteral("task")).toString() != QLatin1String(kFaultTask)
        || object.value(QStringLiteral("inputStage")).toString() != QLatin1String("frozen_normalized_features"))
        return fail(error, QStringLiteral("评估集模型、任务或输入阶段不符"));
    const auto classes = object.value(QStringLiteral("classes")).toArray();
    if (classes.size() != kFaultClassCount)
        return fail(error, QStringLiteral("评估集类别数量不符"));
    for (int i = 0; i < kFaultClassCount; ++i)
        if (classes[i].toString() != faultName(i))
            return fail(error, QStringLiteral("评估集类别顺序不符"));
    m_inputFloats = object.value(QStringLiteral("inputFloats")).toInt(0);
    if (m_inputFloats != kInputFloats)
        return fail(error, QStringLiteral("评估集输入长度不是 %1").arg(kInputFloats));
    const auto inputs = object.value(QStringLiteral("inputs")).toString();
    const auto index = object.value(QStringLiteral("index")).toString();
    if (inputs.isEmpty() || index.isEmpty())
        return fail(error, QStringLiteral("evalset.json 缺少 inputs/index"));
    if (inputs.contains(QLatin1Char('/')) || inputs.contains(QLatin1Char('\\'))
        || inputs.contains(QLatin1Char(':')) || inputs.contains(QLatin1String(".."))
        || index.contains(QLatin1Char('/')) || index.contains(QLatin1Char('\\'))
        || index.contains(QLatin1Char(':')) || index.contains(QLatin1String("..")))
        return fail(error, QStringLiteral("评估集文件名无效"));

    m_inputsPath = directory.filePath(inputs);
    const QFileInfo inputInfo(m_inputsPath);
    if (!inputInfo.exists()) return fail(error, QStringLiteral("缺少输入文件：%1").arg(inputs));

    QFile table(directory.filePath(index));
    if (!table.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("缺少索引文件：%1").arg(index));
    if (table.size() <= 0 || table.size() > kMaxIndexBytes) {
        table.close();
        return fail(error, QStringLiteral("索引文件长度无效"));
    }
    const auto lines = QString::fromUtf8(table.readAll()).split(QLatin1Char('\n'));
    table.close();

    int row = 0;
    int sampleColumn = -1, idColumn = -1, endColumn = -1;
    int classColumn = -1, gradeColumn = -1, referenceClassColumn = -1, referenceGradeColumn = -1;
    for (const auto &raw : lines) {
        QString line = raw;
        if (line.endsWith(QLatin1Char('\r'))) line.chop(1); /* 兼容 CRLF 索引文件 */
        if (line.isEmpty()) continue;
        const auto fields = splitCsv(line);
        if (!row) {
            sampleColumn = columnOf(fields, "sample");
            idColumn = columnOf(fields, "window_id");
            endColumn = columnOf(fields, "window_end_us");
            classColumn = columnOf(fields, "class_target");
            gradeColumn = columnOf(fields, "grade_target");
            referenceClassColumn = columnOf(fields, "reference_class");
            referenceGradeColumn = columnOf(fields, "reference_grade");
            if (classColumn < 0 || gradeColumn < 0)
                return fail(error, QStringLiteral("索引缺少 class_target/grade_target"));
            ++row;
            continue;
        }
        if (fields.size() <= classColumn || fields.size() <= gradeColumn)
            return fail(error, QStringLiteral("索引第 %1 行列数不完整").arg(row + 1));
        if (sampleColumn >= 0 && (sampleColumn >= fields.size()
            || toInt(fields[sampleColumn], -1) != m_rows.size()))
            return fail(error, QStringLiteral("索引 sample 顺序不符"));
        FaultEvalSample sample;
        if (idColumn >= 0 && idColumn < fields.size()) sample.windowId = fields[idColumn];
        if (endColumn >= 0 && endColumn < fields.size()) sample.windowEndUs = fields[endColumn].toULongLong();
        sample.targetClass = toInt(fields[classColumn], -1);
        sample.targetGrade = toInt(fields[gradeColumn], -1);
        if (referenceClassColumn >= 0 && referenceClassColumn < fields.size())
            sample.referenceClass = toInt(fields[referenceClassColumn], -1);
        if (referenceGradeColumn >= 0 && referenceGradeColumn < fields.size())
            sample.referenceGrade = toInt(fields[referenceGradeColumn], -1);
        if (sample.targetClass < 0 || sample.targetClass > 4 || sample.targetGrade < 0
            || sample.targetGrade > 3 || !faultGradeValid(sample.targetClass, sample.targetGrade))
            return fail(error, QStringLiteral("索引第 %1 行真值越界").arg(row + 1));
        m_rows.append(sample);
        ++row;
    }
    if (m_rows.isEmpty()) return fail(error, QStringLiteral("索引没有样本"));
    const qint64 expected = qint64(m_rows.size()) * kInputFloats * 4;
    if (inputInfo.size() != expected)
        return fail(error, QStringLiteral("输入文件长度 %1 与 %2 窗不符")
                               .arg(inputInfo.size()).arg(m_rows.size()));
    const int declared = object.value(QStringLiteral("samples")).toInt(0);
    if (declared != m_rows.size())
        return fail(error, QStringLiteral("evalset.json 声明 %1 窗，索引为 %2 窗")
                               .arg(declared).arg(m_rows.size()));
    m_loaded = true;
    return true;
}

} // namespace mlk
