#include "replaypanel.h"
#include "diagnosisui.h"
#include <QtWidgets>

namespace mlk {
ReplayPanel::ReplayPanel(bool local, QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    /* 面板位于「边缘模型检测」下半区，字号比全局略小，避免控件行把区块最小宽度撑大。 */
    QFont small = font();
    small.setPointSizeF(qMax(8.0, small.pointSizeF() - 1.5));
    setFont(small);
    auto *controls = new QHBoxLayout;
    m_backend = new QComboBox(this); m_backend->setObjectName(QStringLiteral("rulBackend"));
    m_backend->addItem(QStringLiteral("融合 CPU"), 1);
    m_backend->addItem(QStringLiteral("CPU/NPU（FF6 六分支）"), 2);
    m_backend->setCurrentIndex(m_backend->findData(2));
    connect(m_backend, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this] { m_backendEdited = true; });
    m_index = new QSpinBox(this); m_index->setRange(0, 9999); m_index->setObjectName(QStringLiteral("rulWindow"));
    m_interval = new QSpinBox(this); m_interval->setRange(100, 60000); m_interval->setValue(1000);
    m_interval->setSuffix(QStringLiteral(" ms"));
    controls->addWidget(m_backend); controls->addWidget(new QLabel(QStringLiteral("窗口（从 0 开始）"), this));
    controls->addWidget(m_index); controls->addWidget(m_interval);
    const QString names[] = {QStringLiteral("开始/继续"), QStringLiteral("暂停"), QStringLiteral("单步"),
                             QStringLiteral("停止")};
    for (int i = 0; i < 4; ++i) {
        auto *button = new QPushButton(names[i], this);
        button->setObjectName(QStringLiteral("rulAction%1").arg(i + 1));
        connect(button, &QPushButton::clicked, this, [this, i] {
            m_backendEdited = false;
            if (i == 3) { /* 停止后从头开始，窗口编号归零 */
                const QSignalBlocker blocker(m_index);
                m_index->setValue(0);
            }
            emit requested(quint8(i + 1), quint8(m_backend->currentData().toInt()),
                           quint32(m_index->value()), quint32(m_interval->value()));
        });
        controls->addWidget(button);
    }
    layout->addLayout(controls);
    if (local) {
        auto *row = new QHBoxLayout;
        row->addWidget(new QLabel(QStringLiteral("目录"), this));
        m_root = new QLineEdit(this); m_root->setObjectName(QStringLiteral("rulDirectory"));
        /* 目录框紧跟标签左对齐，固定 320 px，剩余空间用 stretch 吸收，
         * 避免 QHBoxLayout 把限宽控件推到行尾形成中段空白。 */
        m_root->setFixedWidth(320);
        m_root->setPlaceholderText(QStringLiteral("/opt/qt_play/模型目录"));
        row->addWidget(m_root, 0, Qt::AlignLeft);
        row->addStretch(1);
        layout->addLayout(row);
    }
    m_state = new QLabel(QStringLiteral("数据集回放 · 已停止"), this);
    m_state->setTextFormat(Qt::PlainText);
    m_state->setObjectName(QStringLiteral("rulState"));
    m_details = new QLabel(QStringLiteral("现场传感器保持独立采集；四值模拟注入不适用于该模型"), this);
    m_details->setObjectName(QStringLiteral("rulDetails"));
    m_details->setToolTip(QStringLiteral("窗口结果时间；模型暂停或停止后保持最后一次结果"));
    m_details->setTextFormat(Qt::PlainText); m_details->setWordWrap(true);
    layout->addWidget(m_state); layout->addWidget(m_details);
}
void ReplayPanel::setDirectory(const QString &root) { if (m_root) m_root->setText(root); }
QString ReplayPanel::directory() const { return m_root ? m_root->text().trimmed() : QString(); }
void ReplayPanel::showError(const QString &error) { m_state->setText(error); }
void ReplayPanel::setConnected(bool connected) {
    if (!connected) m_state->setText(QStringLiteral("回放链路不可用，结果可能已过期"));
}
void ReplayPanel::showFrame(const Frame &f) {
    if (f.ver != kInferenceVersion) return;
    if (!m_backendEdited && (f.backend == 1 || f.backend == 2)) {
        const QSignalBlocker blocker(m_backend);
        m_backend->setCurrentIndex(m_backend->findData(f.backend));
    }
    QStringList parts;
    parts << QStringLiteral("数据集回放") << stateText(f.inferenceState);
    const auto backend = backendText(f.backend);
    if (!backend.isEmpty()) parts << backend;
    if (!f.message.isEmpty()) parts << f.message;
    m_state->setText(parts.join(QStringLiteral(" · ")));
    /* 窗口结果时间跟随模型：运行/完成时取结果时间戳，暂停、停止或推理失败
     * 后保持最后一次结果。回放心跳每秒重发同一状态，不能把定格值刷新成当前
     * 时间。f.tsUs 为结果 UTC epoch（µs）；旧来源不可解读时退回接收时间。 */
    const bool epoch = f.tsUs >= 1600000000000000ULL && f.tsUs <= 253402300799999999ULL;
    const bool live = f.inferenceState == kRunning || f.inferenceState == kCompleted
                      || f.inferenceState == kReady;
    QString stamp;
    if (epoch && (live || m_windowClock.isEmpty()))
        stamp = QDateTime::fromMSecsSinceEpoch(qint64(f.tsUs / 1000)).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    else if (!epoch && live)
        stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    else
        stamp = m_windowClock;
    if (stamp.isEmpty())
        stamp = f.windowCount ? QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                              : QStringLiteral("尚无预测");
    m_windowClock = stamp;
    m_details->setText(QStringLiteral("模型 %1 · 数据集 %2 · 窗口 %3/%4 · %5")
        .arg(f.modelId, f.datasetId.isEmpty() ? QStringLiteral("未加载") : f.datasetId)
        .arg(f.windowIndex).arg(f.windowCount)
        .arg(stamp));
    if (f.windowCount) m_index->setMaximum(int(f.windowCount - 1));
    /* 仅在播放中跟随窗口推进；暂停/停止/完成时保留用户对窗口编号的修改。 */
    if (f.inferenceState == kRunning && !m_index->hasFocus()) {
        const QSignalBlocker blocker(m_index);
        m_index->setValue(int(f.windowIndex));
    }
    if (m_root) m_root->setEnabled(f.inferenceState == kStopped || f.inferenceState == kInferenceError);
}
}
