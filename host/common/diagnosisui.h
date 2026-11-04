#pragma once
#include "../remote-pc/src/frame.h"
#include <QLabel>
#include <cmath>

namespace mlk {
inline QString backendText(quint8 backend) {
    return backend == 1 ? QStringLiteral("融合 CPU")
         : backend == 2 ? QStringLiteral("CPU/NPU（FF6 六分支）") : QStringLiteral("不可用");
}
inline QString stateText(quint8 state) {
    const char *names[] = {"已停止", "加载中", "就绪", "运行中", "已暂停", "已完成", "不可用", "停止中"};
    return QString::fromUtf8(names[state <= kStopping ? state : quint8(kInferenceError)]);
}
/* 「边缘模型检测」的模型行：只显示模型实际产出的量。故障分类与分级由
 * 「故障诊断」区块承担（见 docs/NPU-AI接口约定.md），本行不重复分类结论，
 * 也不再叠加故障时间。 */
inline void showEdgeAi(const Frame &f, QLabel *rul, QLabel *time, QLabel *backend,
                       QLabel *state, QLabel *window) {
    const bool v3 = f.ver == kInferenceVersion;
    const bool validRul = v3 && (f.valid & kValidRul) != 0;
    if (validRul) {
        QString text = QStringLiteral("寿命预测：%1 轮（观测老化试验）").arg(f.rulValue, 0, 'f', 2);
        /* 偏差取该窗口的寿命标签（运行包随附的实际值，不是采集数据）作分母：
         * 标签缺失或为 0 时百分比无定义，只显示预测值。 */
        if (f.rulLabelValid && std::isfinite(f.rulLabel) && f.rulLabel > 0.0f) {
            const double deviation = std::abs(double(f.rulValue) - double(f.rulLabel)) / double(f.rulLabel) * 100.0;
            text += QStringLiteral("　偏差：%1%").arg(deviation, 0, 'f', 1);
        }
        /* 本次回放已处理窗口的平均偏差，由回放控制器累计。 */
        if (f.rulDeviationCount > 0)
            text += QStringLiteral("　平均偏差：%1%").arg(f.rulDeviationAvg, 0, 'f', 1);
        rul->setText(text);
        rul->setStyleSheet(QStringLiteral("color: #37c08a; font-weight: bold;"));
    } else {
        rul->setText(QStringLiteral("寿命预测：-"));
        rul->setStyleSheet(QString());
    }
    time->setText(f.inferUs ? QStringLiteral("推理耗时：%1 ms").arg(f.inferUs / 1000.0, 0, 'f', 2)
                            : QStringLiteral("推理耗时：-"));
    backend->setText(QStringLiteral("后端：%1").arg(backendText(f.backend)));
    state->setText(QStringLiteral("状态：%1").arg(stateText(f.inferenceState)));
    if (v3 && f.windowCount)
        window->setText(QStringLiteral("窗口：%1 / %2").arg(f.windowIndex).arg(f.windowCount));
    else
        window->setText(QStringLiteral("窗口：-"));
}

/* 上位机（remote-pc）的「诊断」区块：仍按 v1/v2 分类字段显示。 */
inline void showDiagnosis(const Frame &f, QLabel *cls, QLabel *grade, QLabel *conf,
                           QLabel *rul, QLabel *alert, QLabel *time) {
    const bool classified = hasClassification(f);
    const char *names[] = {"正常", "轻微故障", "重度故障"};
    cls->setText(classified ? QString::fromUtf8(names[f.cls]) : QStringLiteral("不可用"));
    cls->setStyleSheet(classified ? (f.cls == 0 ? QStringLiteral("color: #37c08a;")
        : f.cls == 1 ? QStringLiteral("color: #e6c037;") : QStringLiteral("color: #e65a5a;")) : QString());
    grade->setText(classified ? QString::number(f.grade) : QStringLiteral("不可用"));
    conf->setText(classified ? QStringLiteral("%1%").arg(f.conf / 10.0, 0, 'f', 1) : QStringLiteral("不可用"));
    alert->setText(classified && f.cls ? QStringLiteral("告警") : QString());
    if (f.ver == kInferenceVersion) {
        const bool valid = (f.valid & kValidRul) != 0;
        rul->setText(valid ? QStringLiteral("%1 轮（观测老化试验）").arg(f.rulValue, 0, 'f', 3) : QStringLiteral("不可用"));
        time->setText(valid ? QStringLiteral("%1 ms").arg(f.inferUs / 1000.0, 0, 'f', 2) : QStringLiteral("不可用"));
    } else {
        rul->setText(QStringLiteral("%1（单位未定义）").arg(f.rul));
        time->setText(QStringLiteral("%1 ms").arg(f.inferUs / 1000.0, 0, 'f', 2));
    }
}
}
