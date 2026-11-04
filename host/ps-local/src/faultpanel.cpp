#include "faultpanel.h"

#include "faultevalpanel.h"

#include <QComboBox>
#include <QCheckBox>
#include <QDateTime>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QVBoxLayout>

FaultPanel::FaultPanel(QWidget *parent) : QGroupBox(QStringLiteral("故障诊断"), parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 6, 8, 6);
    root->setSpacing(4);

    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("faultModeTabs"));

    /* 模式一：实时诊断。 */
    auto *live = new QWidget(m_tabs);
    auto *liveLayout = new QVBoxLayout(live);
    liveLayout->setContentsMargins(4, 4, 4, 4);
    liveLayout->setSpacing(4);

    auto *controls = new QHBoxLayout;
    controls->setSpacing(6);
    m_directory = new QLineEdit(QStringLiteral("/opt/mlk_fault"), live);
    m_directory->setObjectName(QStringLiteral("faultDirectory"));
    m_directory->setToolTip(QStringLiteral("故障运行包目录：model.json、libmlk_fault.so 与 preprocess.json"));
    m_backend = new QComboBox(live);
    m_backend->setObjectName(QStringLiteral("faultBackend"));
    m_backend->addItem(QStringLiteral("CPU+CNN NPU"), int(mlk::kFaultBackendCpuCnnNpu));
    m_backend->addItem(QStringLiteral("CPU"), int(mlk::kFaultBackendCpu));
    m_start = new QPushButton(QStringLiteral("开始诊断"), live);
    m_start->setObjectName(QStringLiteral("faultStart"));
    m_stop = new QPushButton(QStringLiteral("停止"), live);
    m_stop->setObjectName(QStringLiteral("faultStop"));
    m_stop->setEnabled(false);
    controls->addWidget(new QLabel(QStringLiteral("包目录"), live));
    controls->addWidget(m_directory, 2);
    controls->addWidget(m_backend);
    controls->addWidget(m_start);
    controls->addWidget(m_stop);
    liveLayout->addLayout(controls);
    m_discardNormal = new QCheckBox(QStringLiteral("仅保留异常波形（验证时保持关闭）"), live);
    m_discardNormal->setObjectName(QStringLiteral("faultDiscardNormal"));
    m_discardNormal->setToolTip(QStringLiteral("仅清理本次诊断中完整判定正常的旧分段；故障前后各保留至少 10 秒，数据不足和传感器告警分段保留"));
    liveLayout->addWidget(m_discardNormal);

    m_state = new QLabel(QStringLiteral("状态：未启动"), live);
    m_state->setObjectName(QStringLiteral("faultLiveState"));
    m_state->setTextFormat(Qt::PlainText);
    m_main = new QLabel(QStringLiteral("实时主故障：N/A"), live);
    m_main->setObjectName(QStringLiteral("faultLiveStatus"));
    m_main->setTextFormat(Qt::PlainText);
    m_main->setWordWrap(true);
    m_detail = new QLabel(QString(), live);
    m_detail->setObjectName(QStringLiteral("faultLiveDetail"));
    m_detail->setTextFormat(Qt::PlainText);
    m_detail->setStyleSheet(QStringLiteral("font-size: 11px; color: #c9d1d9;"));
    m_quality = new QLabel(QString(), live);
    m_quality->setObjectName(QStringLiteral("faultLiveQuality"));
    m_quality->setTextFormat(Qt::PlainText);
    m_quality->setStyleSheet(QStringLiteral("font-size: 11px; color: #8b949e;"));
    m_quality->setWordWrap(true);
    auto *liveGrid = new QGridLayout;
    liveGrid->setHorizontalSpacing(8);
    liveGrid->setVerticalSpacing(3);
    liveGrid->addWidget(m_state, 0, 0);
    liveGrid->addWidget(m_main, 1, 0);
    liveGrid->addWidget(m_detail, 2, 0);
    liveGrid->addWidget(m_quality, 3, 0);
    liveGrid->setColumnStretch(0, 1);
    liveLayout->addLayout(liveGrid);
    liveLayout->addStretch(1);
    m_tabs->addTab(live, QStringLiteral("实时诊断"));

    /* 模式二：数据集评估。 */
    m_eval = new FaultEvalPanel(m_tabs);
    auto *evalScroll = new QScrollArea(m_tabs);
    evalScroll->setObjectName(QStringLiteral("faultEvalScroll"));
    evalScroll->setWidgetResizable(true);
    evalScroll->setFrameShape(QFrame::NoFrame);
    evalScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    evalScroll->setMinimumHeight(220);
    evalScroll->setWidget(m_eval);
    m_tabs->addTab(evalScroll, QStringLiteral("数据集评估"));

    root->addWidget(m_tabs);

    connect(m_start, &QPushButton::clicked, this, [this] {
        emit startRequested(m_directory->text().trimmed(), backend());
    });
    connect(m_stop, &QPushButton::clicked, this, [this] { emit stopRequested(); });
    connect(m_eval, &FaultEvalPanel::startRequested, this, &FaultPanel::evalStartRequested);
    connect(m_eval, &FaultEvalPanel::stopRequested, this, &FaultPanel::evalStopRequested);
    clearResult();
}

QString FaultPanel::directory() const { return m_directory->text().trimmed(); }

int FaultPanel::backend() const { return m_backend->currentData().toInt(); }
bool FaultPanel::discardNormalRecordings() const { return m_discardNormal->isChecked(); }
void FaultPanel::setDiscardNormalRecordings(bool enabled) { m_discardNormal->setChecked(enabled); }

void FaultPanel::setDirectory(const QString &directory)
{
    if (!directory.isEmpty()) m_directory->setText(directory);
}

void FaultPanel::setBackend(int backend)
{
    const int index = m_backend->findData(backend);
    if (index >= 0) m_backend->setCurrentIndex(index);
}

void FaultPanel::setBusy(bool busy)
{
    m_start->setEnabled(!busy);
    m_stop->setEnabled(busy);
    m_directory->setEnabled(!busy);
    m_backend->setEnabled(!busy);
    m_discardNormal->setEnabled(!busy);
}

void FaultPanel::showState(int state, const QString &detail)
{
    QString text = QStringLiteral("状态：%1").arg(mlk::faultStateDisplay(state));
    if (!detail.isEmpty()) text += QStringLiteral(" · %1").arg(detail);
    m_state->setText(text);
}

void FaultPanel::clearResult()
{
    m_main->setText(QStringLiteral("实时主故障：N/A"));
    m_main->setStyleSheet(QStringLiteral("color: #8b949e; font-weight: bold;"));
    m_detail->setText(QStringLiteral("窗口：-　后端：-　耗时：-"));
    m_quality->setText(QStringLiteral("质量：-"));
}

void FaultPanel::showResult(const mlk::FaultResult &result, qint64 ageMs, bool confirmed,
                            bool external)
{
    if (!result.valid) {
        m_main->setText(QStringLiteral("实时主故障：N/A　状态：%1　原因：%2")
                            .arg(mlk::faultStateDisplay(result.state),
                                 result.reason.isEmpty() ? QStringLiteral("-") : result.reason));
        m_main->setStyleSheet(QStringLiteral("color: #8b949e; font-weight: bold;"));
    } else {
        const QString fault = mlk::faultDisplayName(result.classIndex);
        const QString grade = result.classIndex == mlk::kFaultNormal
            ? QStringLiteral("0") : QString::number(result.grade);
        const bool lowConfidence = result.classIndex != mlk::kFaultNormal && !result.alarmEligible();
        m_main->setText((lowConfidence
                        ? QStringLiteral("低置信度候选：%1　分级：%2　分数：%3　告警：%4")
                        : QStringLiteral("实时主故障：%1　分级：%2　分数：%3　告警：%4"))
                            .arg(fault, grade,
                                 result.hasConfidence
                                     ? QString::number(result.confidence, 'f', 3)
                                     : QStringLiteral("N/A"),
                                 lowConfidence ? QStringLiteral("低于0.80，不确认")
                                           : confirmed ? QStringLiteral("已确认")
                                           : QStringLiteral("待确认")));
        m_main->setStyleSheet(lowConfidence
            ? QStringLiteral("color: #8b949e; font-weight: bold;")
            : result.classIndex == mlk::kFaultNormal
            ? QStringLiteral("color: #37c08a; font-weight: bold;")
            : result.grade >= 3 ? QStringLiteral("color: #e65a5a; font-weight: bold;")
                                : QStringLiteral("color: #e6c037; font-weight: bold;"));
    }
    QString window = QStringLiteral("-");
    if (result.endUs) {
        window = QDateTime::fromMSecsSinceEpoch(qint64(result.endUs / 1000)).toLocalTime()
                     .toString(QStringLiteral("HH:mm:ss"));
    }
    m_detail->setText(QStringLiteral("窗口：%1　后端：%2　预处理：%3 µs　推断：%4 µs　年龄：%5 ms%6")
                          .arg(window, mlk::faultBackendDisplay(result.backend))
                          .arg(result.preprocessUs).arg(result.inferUs)
                          .arg(ageMs >= 0 ? QString::number(ageMs) : QStringLiteral("N/A"))
                          .arg(external ? QStringLiteral("　来源：外部模型") : QString()));
    if (result.quality.isEmpty()) {
        m_quality->setText(QStringLiteral("质量：-"));
        return;
    }
    const auto q = result.quality;
    m_quality->setText(QStringLiteral("质量：块 %1　TF 有效 %2　快状态 %3　慢状态 %4　姿态年龄 %5 ms　缺口 %6%7")
                           .arg(q.value(QStringLiteral("blocks")).toInt())
                           .arg(q.value(QStringLiteral("tfRatio")).toDouble(), 0, 'f', 2)
                           .arg(q.value(QStringLiteral("fastRatio")).toDouble(), 0, 'f', 2)
                           .arg(q.value(QStringLiteral("slowRatio")).toDouble(), 0, 'f', 2)
                           .arg(q.value(QStringLiteral("poseAgeMs")).toDouble(), 0, 'f', 0)
                           .arg(q.value(QStringLiteral("gapBlocks")).toInt())
                           .arg(q.value(QStringLiteral("overtemperatureMasked")).toBool()
                                ? QStringLiteral("　≤35℃屏蔽过热") : QString()));
}
