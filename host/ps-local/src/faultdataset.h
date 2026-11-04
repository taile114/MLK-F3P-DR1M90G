#pragma once

#include <QString>
#include <QVector>

namespace mlk {

/* 数据集评估集的单窗真值。真值只来自 test_index.csv 的 class_target /
 * grade_target；reference_/quantized_ 列是离线准备阶段的参考预测，不参与
 * 本次评估的判定，只用于对照。 */
struct FaultEvalSample {
    QString windowId;
    quint64 windowEndUs = 0;
    int targetClass = -1;
    int targetGrade = -1;
    int referenceClass = -1;
    int referenceGrade = -1;
};

/* 冻结特征评估集：evalset.json + 定长 float32 输入文件 + 索引 CSV。
 *
 * evalset.json 的 schema 为 mlk.fault.evalset.v1，给出 inputs/index 文件名、
 * 窗口数与 27683 输入长度。评估直接使用训练侧已标准化的特征，不在应用内做
 * 原生采集预处理；该模式只评估模型分类与分级，不评估采集链路。 */
class FaultDataset
{
public:
    bool load(const QString &root, QString *error);
    bool loaded() const { return m_loaded; }

    QString root() const { return m_root; }
    QString modelId() const { return m_modelId; }
    QString inputsPath() const { return m_inputsPath; }
    int samples() const { return int(m_rows.size()); }
    int inputFloats() const { return m_inputFloats; }
    const QVector<FaultEvalSample> &rows() const { return m_rows; }

private:
    bool m_loaded = false;
    QString m_root;
    QString m_modelId;
    QString m_inputsPath;
    int m_inputFloats = 0;
    QVector<FaultEvalSample> m_rows;
};

} // namespace mlk
