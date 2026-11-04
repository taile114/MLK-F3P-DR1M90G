#include "mainwindow.h"
#include "modelfault.h"
#include "peerdiscovery.h"

#include <QDateTime>
#include <QFileInfo>
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
#include <QRandomGenerator>
#endif
#include <QtWidgets>
#include <cmath>

#include "faultstore.h"
#include "capturedialog.h"
#include "modelfaultserver.h"
#include "injectdialog.h"
#include "receiver.h"
#include "sensorpanel.h"
#include "plsettingsdialog.h"
#include "plotwidget.h"
#include "npupanel.h"
#include "networkstatus.h"
#include "sharedconfig.h"
#include "workorders.h"
#include "replaypanel.h"
#include "diagnosisui.h"
#include "faultstatus.h"

using mlk::Frame;
namespace {
void setMetric(QLabel *label, const QString &value) {
    if (label) label->setText(QStringLiteral("%1：%2").arg(label->property("caption").toString(),value));
}
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_rx(new mlk::Receiver(this))
{
    setWindowTitle(QStringLiteral("MLK-F3P 设备运维上位机"));
    m_cfg.storageDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/records");
    m_cfg.load();
    if (!m_cfg.ensureIdentity()) qWarning("remote-pc: identity could not be loaded or saved");
    m_discovery = new mlk::PeerDiscovery(mlk::PeerDiscovery::Remote, this);
    connect(m_discovery, &mlk::PeerDiscovery::peerDiscovered, this,
        [this](const QHostAddress &address, quint16 port) {
            setTarget(address.toString(), port); m_cfg.save(); setupSecure(); queryConfig();
            showToast(QStringLiteral("已发现配对核心板 %1:%2").arg(address.toString()).arg(port), 3000);
        });
    setupSecure();

    auto *central = new QWidget(this);
    setCentralWidget(central);
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(14, 6, 14, 6);
    root->setSpacing(8);

    /* 顶栏：左侧 CPU/内存摘要与侧栏开关，右侧记录与配置；标题覆盖在顶栏中线上。 */
    auto *bar = new QHBoxLayout;
    auto *cpuBox = new QVBoxLayout;
    cpuBox->setSpacing(0);
    m_cpuSummary = new QLabel(QStringLiteral("CPU: -"), central);
    m_cpuSummary->setObjectName(QStringLiteral("cpuSummary"));
    m_cpuSummary->setStyleSheet(QStringLiteral("font-size: 12px; color: #c9d1d9;"));
    m_cpuSummary->setTextFormat(Qt::PlainText);
    cpuBox->addWidget(m_cpuSummary);
    m_sysToggle = new QPushButton(QStringLiteral("展开"), central);
    m_sysToggle->setObjectName(QStringLiteral("systemStatusToggle"));
    m_sysToggle->setCheckable(true);
    m_sysToggle->setToolTip(QStringLiteral("展开核心板系统占用侧栏"));
    auto *leftBox = new QHBoxLayout;
    leftBox->setSpacing(6);
    leftBox->addLayout(cpuBox);
    leftBox->addWidget(m_sysToggle, 0, Qt::AlignBottom);
    bar->addLayout(leftBox);
    bar->addStretch(1);

    auto *rightBox = new QHBoxLayout;
    auto *settings = new QPushButton(QStringLiteral("配置"), central);
    settings->setObjectName(QStringLiteral("settingsButton"));
    settings->setToolTip(QStringLiteral("参数 / 加密 / 传输 / 密钥（F2）"));
    m_recordBtn = new QPushButton(QStringLiteral("未记录"), central);
    m_recordBtn->setCheckable(true);
    m_recordBtn->setObjectName(QStringLiteral("recordButton"));
    m_recordBtn->setStyleSheet(QStringLiteral(
        "QPushButton:checked { background: #238636; color: #ffffff; border: 1px solid #2ea043; }"));
    m_recordBtn->setToolTip(QStringLiteral("按配置时长保存全部接收样本；四传感器始终加密存储"));
    m_connectionStatus = new QLabel(QStringLiteral("未连接"),central);
    m_connectionStatus->setObjectName(QStringLiteral("connectionStatus"));
    m_connectionStatus->setTextFormat(Qt::PlainText);
    auto *connectionBox = new QVBoxLayout;
    connectionBox->setSpacing(0);
    m_networkStatus = new QLabel(QStringLiteral("网络：不可用"), central);
    m_networkStatus->setObjectName(QStringLiteral("networkConnectionStatus"));
    m_networkStatus->setTextFormat(Qt::PlainText);
    m_networkStatus->setStyleSheet(m_cpuSummary->styleSheet());
    connectionBox->addWidget(m_networkStatus);
    connectionBox->addWidget(m_connectionStatus);
    leftBox->addLayout(connectionBox);
    rightBox->addWidget(m_recordBtn);
    rightBox->addWidget(settings);
    bar->addLayout(rightBox);
    root->addLayout(bar);

    /* 标题与板端时间是覆盖层：水平中心对齐到波形 2×2 网格的中心（两列之间的
     * 分隔线），侧栏展开/收起时随网格一起移动。板端时间同时是寿命预测的时间
     * 基准，回放暂停时定格（见 updateDiagnosisUi）。 */
    m_title = new QLabel(QStringLiteral("设备智能运维 远程看板"), central);
    m_title->setObjectName(QStringLiteral("dashboardTitle"));
    QFont tf = m_title->font();
    tf.setBold(true);
    tf.setPointSize(16);
    m_title->setFont(tf);
    m_title->setStyleSheet(QStringLiteral("color: #e6edf3; background: transparent;"));
    m_timeLabel = new QLabel(QStringLiteral("时间：—"), central);
    m_timeLabel->setObjectName(QStringLiteral("boardTime"));
    m_timeLabel->setToolTip(
        QStringLiteral("寿命预测的时间基准（板端本地时间），数据集回放暂停时定格"));
    /* objectName 由 sourceStatus 改为 boardTime，内联保留原主题的灰度。 */
    m_timeLabel->setStyleSheet(QStringLiteral("color: #8b949e; background: transparent;"));

    auto *content = new QHBoxLayout;
    content->setSpacing(10);

    /* 侧栏：核心板系统占用明细，默认收起，把宽度让给波形。 */
    /* 侧栏是浮在窗口内容之上的抽屉：不进布局，不挤压波形区。 */
    m_sidebar = new QWidget(central);
    m_sidebar->setObjectName(QStringLiteral("sysSidebar"));
    m_sidebar->setAttribute(Qt::WA_StyledBackground, true);
    m_sidebar->setFixedWidth(320);
    m_sidebar->setStyleSheet(QStringLiteral(
        "QWidget#sysSidebar { background: #0d1117; border: 1px solid #30363d; border-radius: 6px; }"));
    auto *sv = new QVBoxLayout(m_sidebar);
    sv->setContentsMargins(8, 6, 8, 6);
    sv->setSpacing(4);
    auto *sideTitle = new QLabel(QStringLiteral("核心板"), m_sidebar);
    sideTitle->setObjectName(QStringLiteral("sysTitle"));
    sv->addWidget(sideTitle);
    m_cpuText = new QLabel(QStringLiteral("-"), m_sidebar);
    m_cpuCores = new QLabel(QString(), m_sidebar);
    m_cpuCores->setObjectName(QStringLiteral("sysDetail"));
    m_cpuCores->setWordWrap(true);
    m_memText = new QLabel(QStringLiteral("-"), m_sidebar);
    m_netText = new QLabel(QStringLiteral("网络：-"), m_sidebar);
    m_diskText = new QLabel(QStringLiteral("磁盘：-"), m_sidebar);
    m_cpuBar = new QProgressBar(m_sidebar);
    m_cpuBar->setObjectName(QStringLiteral("cpuBar"));
    m_cpuBar->setRange(0, 100);
    m_cpuBar->setValue(0);
    m_cpuBar->setFormat(QStringLiteral("%v%"));
    m_memBar = new QProgressBar(m_sidebar);
    m_memBar->setObjectName(QStringLiteral("memBar"));
    m_memBar->setRange(0, 100);
    m_memBar->setValue(0);
    m_memBar->setFormat(QStringLiteral("%v%"));
    m_diskBar = new QProgressBar(m_sidebar);
    m_diskBar->setObjectName(QStringLiteral("diskBar"));
    m_diskBar->setRange(0, 100);
    m_diskBar->setValue(0);
    m_diskBar->setFormat(QStringLiteral("%v%"));
    m_cpuPlot = new mlk::PlotWidget(m_sidebar);
    m_cpuPlot->setSeriesCount(1);
    m_cpuPlot->setSeriesColor(0, QColor(0x37, 0xc0, 0x8a));
    m_cpuPlot->setDisplayScale(0.01, QStringLiteral("%"));
    m_cpuPlot->setMinimumHeight(60);
    m_cpuPlot->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_memPlot = new mlk::PlotWidget(m_sidebar);
    m_memPlot->setSeriesCount(1);
    m_memPlot->setSeriesColor(0, QColor(0x5a, 0xa9, 0xe6));
    m_memPlot->setDisplayScale(0.01, QStringLiteral("%"));
    m_memPlot->setMinimumHeight(60);
    m_memPlot->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    sv->addWidget(new QLabel(QStringLiteral("CPU"), m_sidebar));
    sv->addWidget(m_cpuText);
    sv->addWidget(m_cpuBar);
    sv->addWidget(m_cpuPlot, 1);
    sv->addWidget(m_cpuCores);
    sv->addWidget(new QLabel(QStringLiteral("内存"), m_sidebar));
    sv->addWidget(m_memText);
    sv->addWidget(m_memBar);
    sv->addWidget(m_memPlot, 1);
    m_npuPanel = new mlk::NpuPanel(sv);
    sv->addWidget(new QLabel(QStringLiteral("网络"), m_sidebar));
    sv->addWidget(m_netText);
    sv->addWidget(new QLabel(QStringLiteral("磁盘"), m_sidebar));
    sv->addWidget(m_diskText);
    sv->addWidget(m_diskBar);
    m_statAbs = new QLabel(QStringLiteral("-"), m_sidebar);
    sv->addWidget(new QLabel(QStringLiteral("本应用 CPU"), m_sidebar));
    sv->addWidget(m_statAbs);
    m_addr = new QLineEdit(m_cfg.addr, m_sidebar);
    m_port = new QSpinBox(m_sidebar); m_port->setRange(1,65535); m_port->setValue(m_cfg.port);
    m_listen = new QPushButton(QStringLiteral("监听"),m_sidebar); m_listen->setCheckable(true);
    m_demo = new QCheckBox(QStringLiteral("演示数据"),m_sidebar);
    auto *injectBtn = new QPushButton(QStringLiteral("模拟注入"),m_sidebar);
    auto *link = new QFormLayout; link->addRow(QStringLiteral("本机地址"),m_addr);
    link->addRow(QStringLiteral("本机端口"),m_port); sv->addLayout(link);
    sv->addWidget(m_listen); sv->addWidget(m_demo); sv->addWidget(injectBtn);
    m_sidebar->setVisible(false);

    auto *mainCol = new QVBoxLayout;
    mainCol->setSpacing(8);
    m_grid = new QWidget(central);
    auto *grid = new QGridLayout(m_grid);
    m_panels[mlk::kTemp] = new SensorPanel(QStringLiteral("温度"), QStringLiteral("℃"),
                                           QColor(0xe6, 0x7a, 0x5a), m_grid);
    m_panels[mlk::kCurrent] = new SensorPanel(QStringLiteral("电流"), QStringLiteral("mA"),
                                              QColor(0xe6, 0xc0, 0x37), m_grid);
    m_panels[mlk::kVibration] = new SensorPanel(QStringLiteral("振动"), QStringLiteral("g"),
                                                QColor(0x5a, 0xa9, 0xe6), m_grid);
    m_panels[mlk::kSpeed] = new SensorPanel(QStringLiteral("转速"), QStringLiteral("rpm"),
                                            QColor(0x37, 0xc0, 0x8a), central);
    m_panels[mlk::kVibration]->setSeriesNames({ QStringLiteral("X"), QStringLiteral("Y"),
                                                QStringLiteral("Z") });
    m_panels[mlk::kVibration]->setSeriesColors({ QColor(0x5a, 0xa9, 0xe6),
                                                 QColor(0x37, 0xc0, 0x8a),
                                                 QColor(0xe6, 0xc0, 0x37) });
    /* ADXL345 ±16 g：256 LSB/g，三轴合成即振动加速度幅值。 */
    m_panels[mlk::kVibration]->setVectorMagnitude(256.0);

    m_panels[mlk::kSpeed]->setUnavailable(QStringLiteral("未接入"));
    for (int i = 0; i < mlk::kChannelCount; ++i) {
        m_panels[i]->setObjectName(QStringLiteral("channel%1").arg(i));
        m_panels[i]->setPlotMinimumHeight(120);
        m_panels[i]->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        grid->addWidget(m_panels[i], i / 2, i % 2);
    }
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(8);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setRowStretch(0, 1);
    grid->setRowStretch(1, 1);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    m_grid->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    mainCol->addWidget(m_grid, 1);

    /* 底部两栏占同一行：「边缘模型检测」与「指标」等高。行高取指标区自然高度的
     * 两倍（下限 440 px），其余垂直空间留给上方四路波形；边缘模型检测的内容放进
     * 滚动区，状态文字换行不再改变行高与波形大小。 */
    auto *bottomRow = new QWidget(central);
    auto *bottom = new QHBoxLayout(bottomRow);
    bottom->setContentsMargins(0, 0, 0, 0);
    bottom->setSpacing(10);
    auto *aiBox = new QGroupBox(QStringLiteral("边缘模型检测"), bottomRow);
    auto *ai = new QVBoxLayout(aiBox);
    ai->setContentsMargins(8, 6, 8, 6);
    ai->setSpacing(4);
    auto *aiScroll = new QScrollArea(aiBox);
    aiScroll->setObjectName(QStringLiteral("edgeModelScroll"));
    aiScroll->setWidgetResizable(true);
    aiScroll->setFrameShape(QFrame::NoFrame);
    aiScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *aiBody = new QWidget(aiScroll);
    auto *aiBodyLayout = new QVBoxLayout(aiBody);
    aiBodyLayout->setContentsMargins(0, 0, 0, 0);
    aiBodyLayout->setSpacing(6);

    /* 上 block：故障状态。只由 UDP v6/type8 摘要及其时效控制，BoardStatus 的
     * v1 粗摘要和 RUL 帧不参与该 block 显示。 */
    auto *faultBox = new QGroupBox(QStringLiteral("故障状态"), aiBody);
    auto *faultLayout = new QVBoxLayout(faultBox);
    faultLayout->setContentsMargins(8, 6, 8, 6);
    faultLayout->setSpacing(2);
    m_faultLive = new QLabel(faultBox);
    m_faultLive->setObjectName(QStringLiteral("faultLiveStatus"));
    m_faultLive->setTextFormat(Qt::PlainText);
    m_faultLive->setWordWrap(true);
    faultLayout->addWidget(m_faultLive);
    aiBodyLayout->addWidget(faultBox);
    updateFaultLiveUi();

    /* 下 block：寿命预测。模型指标与回放控制同属该 block。 */
    auto *rulBox = new QGroupBox(QStringLiteral("寿命预测"), aiBody);
    auto *rul = new QVBoxLayout(rulBox);
    rul->setContentsMargins(8, 6, 8, 6);
    rul->setSpacing(4);
    auto *rg = new QGridLayout;
    rg->setHorizontalSpacing(10);
    rg->setVerticalSpacing(2);
    m_diagRul = new QLabel(QStringLiteral("寿命预测：-"), rulBox);
    m_diagRul->setWordWrap(true);
    m_diagRul->setObjectName(QStringLiteral("diagnosisRulValue"));
    m_statAi = new QLabel(QStringLiteral("推理耗时：-"), rulBox);
    m_diagState = new QLabel(QStringLiteral("状态：-"), rulBox);
    m_diagWindow = new QLabel(QStringLiteral("窗口：-"), rulBox);
    m_diagBackend = new QLabel(QStringLiteral("后端：-"), rulBox);
    rg->addWidget(m_diagRul, 0, 0, 1, 3);
    rg->addWidget(m_statAi, 0, 3, 1, 3);
    rg->addWidget(m_diagState, 1, 0, 1, 2);
    rg->addWidget(m_diagWindow, 1, 2, 1, 2);
    rg->addWidget(m_diagBackend, 1, 4, 1, 2);
    rg->setColumnStretch(1, 1);
    rul->addLayout(rg);
    m_replayPanel = new mlk::ReplayPanel(false, rulBox);
    rul->addWidget(m_replayPanel);
    aiBodyLayout->addWidget(rulBox);
    /* 内容比视口矮时贴顶排列，不把两个 block 拉开。 */
    aiBodyLayout->addStretch(1);
    aiScroll->setWidget(aiBody);
    ai->addWidget(aiScroll, 1);
    bottom->addWidget(aiBox, 3);

    auto *metrics = new QGroupBox(QStringLiteral("指标"), central);
    auto *mg = new QGridLayout(metrics);
    mg->setContentsMargins(10, 10, 10, 10);
    mg->setHorizontalSpacing(18);
    mg->setVerticalSpacing(10);
    m_statLatency = new QLabel(metrics);
    m_statLatency->setObjectName(QStringLiteral("communicationRtt"));
    m_statLatency->setToolTip(QStringLiteral("配置查询至匹配回复的应用往返耗时；使用电脑单调时钟，包含两端处理和排队，不依赖板端校时。"));
    m_statSampleAge = new QLabel(metrics);
    m_statSampleAge->setObjectName(QStringLiteral("sampleTimeDifference"));
    m_statSampleAge->setToolTip(QStringLiteral("电脑处理时刻减波形时间戳；v5 为包内首样本 UTC。包含时钟偏差、采样攒块和应用排队，不能作为网络传输耗时。"));
    m_statRead = new QLabel(QStringLiteral("读取耗时：—"), metrics);
    m_statRate = new QLabel(QStringLiteral("UI 刷新：—"), metrics);
    m_statPkts = new QLabel(QStringLiteral("累计帧：0"), metrics);
    m_statLost = new QLabel(QStringLiteral("丢帧：0"), metrics);
    m_statVdma = new QLabel(QStringLiteral("显示：—"), metrics);
    m_statUptime = new QLabel(QStringLiteral("运行：0s"), metrics);
    m_statDmaBlocks = new QLabel(QStringLiteral("DMA 块：—"), metrics);
    m_statDmaLost = new QLabel(QStringLiteral("DMA 丢块：—"), metrics);
    m_statDmaAge = new QLabel(QStringLiteral("DMA 延迟：—"), metrics);
    /* 每项自带标题，避免标签与数值分列后隔得太远。 */
    auto mkMetric = [&](int r, int c, const QString &caption, QLabel *v) {
        v->setProperty("caption", caption);
        v->setText(QStringLiteral("%1：—").arg(caption));
        mg->addWidget(v, r, c, 1, 2);
    };
    mkMetric(0, 0, QStringLiteral("应用往返"), m_statLatency);
    mkMetric(0, 2, QStringLiteral("读取耗时"), m_statRead);
    mkMetric(0, 4, QStringLiteral("UI 刷新"), m_statRate);
    mkMetric(1, 0, QStringLiteral("累计帧"), m_statPkts);
    mkMetric(1, 2, QStringLiteral("丢帧"), m_statLost);
    mkMetric(1, 4, QStringLiteral("运行"), m_statUptime);
    mkMetric(2, 0, QStringLiteral("显示"), m_statVdma);
    mkMetric(2, 2, QStringLiteral("DMA 块"), m_statDmaBlocks);
    mkMetric(2, 4, QStringLiteral("DMA 丢块"), m_statDmaLost);
    mkMetric(3, 0, QStringLiteral("DMA 延迟"), m_statDmaAge);
    setMetric(m_statVdma, QStringLiteral("远程 UDP"));
    /* 数据来源：PL 特征摘要原先的位置，字体与其它指标一致；只显示第一段
     * （如 /dev/pl_ctrl），完整 availability 文本放 tooltip。 */
    m_srcText = new QLabel(QStringLiteral("来源：—"), metrics);
    m_srcText->setObjectName(QStringLiteral("sourceText"));
    m_srcText->setTextFormat(Qt::PlainText);
    m_srcText->setWordWrap(false);
    m_srcText->setProperty("caption", QStringLiteral("数据来源"));
    m_srcText->setToolTip(QStringLiteral("数据来源"));
    mg->addWidget(m_srcText, 3, 2, 1, 2);
    mkMetric(4, 0, QStringLiteral("数据时间差"), m_statSampleAge);
    /* PL 特征摘要：在指标列之后单独一列；内容由板端下发的表格 HTML 决定。 */
    m_featureText = new QLabel(
        QStringLiteral("<table cellspacing=\"0\" cellpadding=\"4\">"
                       "<tr><td colspan=\"3\">PL 特征摘要：</td></tr>"
                       "<tr><td></td><td colspan=\"2\">—</td></tr>"
                       "</table>"), metrics);
    m_featureText->setTextFormat(Qt::RichText);
    m_featureText->setWordWrap(false);
    m_featureText->setObjectName(QStringLiteral("featureSummary"));
    m_featureText->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_featureText->setContentsMargins(14, 0, 0, 0);
    m_featureText->setProperty("caption", QStringLiteral("PL 特征摘要"));
    mg->addWidget(m_featureText, 0, 6, 5, 1);
    mg->setColumnMinimumWidth(6, 170);
    /* 五个指标行均分竖直余量：行距不再挤在顶部，框底不留大片空白。 */
    for (int r = 0; r < 5; ++r) mg->setRowStretch(r, 1);
    bottom->addWidget(metrics, 2);
    /* 两栏等高并固定：指标区自然高度的两倍（下限 440 px），余量归上方波形。 */
    bottomRow->setFixedHeight(qMax(metrics->sizeHint().height() * 2, 440));
    mainCol->addWidget(bottomRow);

    content->addLayout(mainCol, 1);
    root->addLayout(content, 1);

    connect(m_sysToggle, &QPushButton::toggled, this, [this](bool on) {
        m_sidebar->setVisible(on);
        m_sysToggle->setText(on ? QStringLiteral("收起") : QStringLiteral("展开"));
        if (on) {
            updateSidebarGeometry();
            m_sidebar->raise();
            /* 优先级：展开抽屉 < 配置对话框 < 虚拟键盘。对话框本身是顶层窗口，
             * 虚拟键盘是 WindowStaysOnTopHint 的顶层工具窗，这里只需把键盘再抬起来。 */

        }
        QTimer::singleShot(0, this, &MainWindow::updateHeaderPosition);
    });

    m_uiTimer = new QTimer(this);
    connect(m_uiTimer, &QTimer::timeout, this, [this] {
        for (int i = 0; i < mlk::kChannelCount; ++i)
            m_panels[i]->refreshUi();
    });

    m_uiTimer->start(qMax(33, 1000 / qMax(1, m_cfg.uiFps)));

    connect(m_listen, &QPushButton::toggled, this, &MainWindow::toggleListen);
    connect(m_rx, &mlk::Receiver::frameReceived, this, [this](const mlk::Frame &frame) {
        const bool wasConnected=m_linkSeen.isValid() && m_linkSeen.elapsed()<=5000;
        m_linkSeen.restart();
        if (!wasConnected) updateConnectionStatus();
        if (m_demo->isChecked()) m_demo->setChecked(false);
        onFrame(frame);
    });
    connect(m_rx, &mlk::Receiver::badPacket, this, [this] {
        statusBar()->showMessage(QStringLiteral("bad packet"), 1000);
    });
    connect(m_rx, &mlk::Receiver::secureReady, this, [this] {
        statusBar()->showMessage(QStringLiteral("加密会话已建立"), 4000);
    });
    connect(m_demo, &QCheckBox::toggled, this, &MainWindow::onDemoToggled);
    connect(settings, &QPushButton::clicked, this, &MainWindow::openSettings);
    auto *shortcut = new QShortcut(QKeySequence(Qt::Key_F2), this);
    connect(shortcut, &QShortcut::activated, this, &MainWindow::openSettings);
    m_recordTimer = new QTimer(this);
    connect(m_recordTimer,&QTimer::timeout,this,&MainWindow::saveSnapshot);
    connect(m_recordBtn,&QPushButton::toggled,this,[this](bool on) {
        m_recordBtn->setText(on ? QStringLiteral("记录中") : QStringLiteral("未记录"));
        if (on) {
            QString error;
            if (!m_captureRecorder.empty() && !m_captureRecorder.flush(nullptr,&error)) {
                QSignalBlocker blocker(m_recordBtn); m_recordBtn->setChecked(false);
                m_recordBtn->setText(QStringLiteral("未记录")); showToast(error,8000); return;
            }
            m_captureRecorder.begin(m_cfg,m_demo->isChecked()?"remote-pc-demo":"remote-pc");m_recording=true;
            m_recordTimer->start(qMax(1,m_cfg.recordSegmentSec)*1000);
        }
        else { m_recordTimer->stop(); saveSnapshot(); m_recording=false; }
    });
    connect(injectBtn, &QPushButton::clicked, this, [this] {
        InjectDialog dlg(m_panels[0]->lastPhysical(), m_panels[1]->lastPhysical(),
                         m_panels[2]->lastPhysical(), m_panels[3]->lastPhysical(),
                         m_cfg.injectDurationMs, this);
        if (dlg.exec() == QDialog::Accepted) {
            const double vals[4] = { dlg.value(0), dlg.value(1), dlg.value(2), dlg.value(3) };
            performInject(vals, dlg.durationMs());
        }
    });

    m_statTimer = new QTimer(this);
    m_statTimer->setInterval(500);
    connect(m_statTimer, &QTimer::timeout, this, &MainWindow::onTick);
    m_statTimer->start();

    m_demoTimer = new QTimer(this);
    m_demoTimer->setInterval(20);
    connect(m_demoTimer, &QTimer::timeout, this, &MainWindow::onDemoTick);

    m_demoDiagTimer = new QTimer(this);
    m_demoDiagTimer->setInterval(1000);
    connect(m_demoDiagTimer, &QTimer::timeout, this, &MainWindow::onDemoDiagTick);

    connect(m_replayPanel, &mlk::ReplayPanel::requested, this, &MainWindow::requestReplay);
    const auto requestSeed = mlk::crypto::randomBytes(4);
    m_replayRequest = (requestSeed.size() == 4 ? mlk::rd32(requestSeed.constData()) : quint32(QDateTime::currentMSecsSinceEpoch())) | 1;
    m_replayRetry = new QTimer(this); m_replayRetry->setInterval(500);
    connect(m_replayRetry, &QTimer::timeout, this, [this] {
        if (m_replayPending.isEmpty()) { m_replayRetry->stop(); return; }
        if (++m_replayAttempts > 4) {
            m_replayPending.clear(); m_replayRetry->stop();
            m_replayPanel->showError(QStringLiteral("回放命令未获确认，请检查链路并回读状态")); return;
        }
        m_rx->send(QHostAddress(m_cfg.targetAddr), quint16(m_cfg.targetPort), m_replayPending);
    });

    auto *dock = new QDockWidget(QStringLiteral("本地记录 / 故障工单"), this);
    dock->setObjectName(QStringLiteral("faultRecordsDock"));
    auto *dockBody = new QWidget(dock);
    auto *dl = new QVBoxLayout(dockBody);
    auto *refresh = new QPushButton(QStringLiteral("刷新"), dockBody);
    m_faultList = new QListWidget(dockBody);
    dl->addWidget(refresh);
    dl->addWidget(m_faultList, 1);
    dock->setWidget(dockBody);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    connect(refresh, &QPushButton::clicked, this, &MainWindow::refreshFaults);
    connect(m_faultList, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *it) {
                if (it)
                    replayFault(it->data(Qt::UserRole).toString());
            });
    dock->hide();
    auto *records = new QPushButton(QStringLiteral("记录列表"),central);
    records->setObjectName(QStringLiteral("recordListButton"));
    rightBox->insertWidget(rightBox->indexOf(settings),records);
    connect(records,&QPushButton::clicked,dock,[dock] { dock->setVisible(!dock->isVisible()); });
    auto *captures=new QPushButton(QStringLiteral("回放"),central);captures->setObjectName("captureReplayButton");
    rightBox->insertWidget(rightBox->indexOf(settings),captures);
    connect(captures,&QPushButton::clicked,this,&MainWindow::openCaptures);
    m_captureApi=new ModelFaultServer([this](const QJsonObject &o) {return mlk::captureRequest(m_cfg,o);},this);
    const auto captureSocket=qEnvironmentVariable("MLK_CAPTURE_PC_SOCKET",
        QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation)+"/mlk-capture-pc.sock");
    if (!m_captureApi->start(captureSocket)) qWarning("capture API unavailable: %s",qPrintable(m_captureApi->errorString()));
    refreshFaults();
    m_latestOrder = mlk::WorkOrders(m_cfg.storageDir,m_cfg.passphrase).latest();
    m_faultTimeUs = m_latestOrder.value("faultTimeUs").toString().toULongLong();
    auto *sync = new QTimer(this); sync->setInterval(2000);
    connect(sync,&QTimer::timeout,this,&MainWindow::queryConfig); sync->start();
    applyConfig();
    m_diag.ver = mlk::kInferenceVersion; m_diagValid = true; updateDiagnosisUi();
    m_uptime.start();
    statusBar()->showMessage(QStringLiteral("未监听"));
    QTimer::singleShot(0, this, [this] {
        if (m_cfg.autoConnect && !m_rx->isListening() && !m_demo->isChecked())
            startListening(quint16(m_cfg.port));
    });
}

void MainWindow::startListening(quint16 port)
{
    m_port->setValue(port);
    m_listen->setChecked(true);
}

void MainWindow::enableDemo(bool on)
{
    m_demo->setChecked(on);
}

void MainWindow::applyConfig()
{
    if (m_recording && !m_captureRecorder.configMatches(m_cfg)) {
        saveSnapshot();
        if (m_captureRecorder.empty()) m_captureRecorder.begin(m_cfg,m_demo->isChecked()?"remote-pc-demo":"remote-pc");
        else m_recordBtn->setChecked(false);
    }
    for (int i = 0; i < mlk::kChannelCount; ++i) {
        if (i == mlk::kCurrent) m_panels[i]->setScale(1,0);
        else if (i == mlk::kSpeed) m_panels[i]->setScale(0.1,0);
        else m_panels[i]->setScale(m_cfg.ch[i].k, m_cfg.ch[i].b);
        m_panels[i]->setAlarm(m_cfg.ch[i].alarmLo, m_cfg.ch[i].alarmHi, m_cfg.ch[i].alarmEnabled);
        m_panels[i]->setFftPoints(m_cfg.fftPoints);
    }
    if (!m_listen->isChecked()) {
        m_addr->setText(m_cfg.addr);
        m_port->setValue(m_cfg.port);
    }
    if (m_uiTimer) m_uiTimer->setInterval(qMax(33,1000/qMax(1,m_cfg.uiFps)));
    const int recordMs=qMax(1,m_cfg.recordSegmentSec)*1000;
    if (m_recordTimer && m_recordTimer->interval()!=recordMs) m_recordTimer->setInterval(recordMs);
}

void MainWindow::setupSecure()
{
    clearPendingCommands();
    const QByteArray psk = QByteArray::fromHex(m_cfg.pskHex.toLatin1());
    const QByteArray peer = QByteArray::fromHex(m_cfg.peerPubHex.toLatin1());
    const QByteArray spub = QByteArray::fromHex(m_cfg.selfPubHex.toLatin1());
    const QByteArray spriv = QByteArray::fromHex(m_cfg.selfPrivHex.toLatin1());
    const mlk::SecureChannel::Mode m =
        m_cfg.secMode == 1 ? mlk::SecureChannel::Psk
                           : (m_cfg.secMode == 2 ? mlk::SecureChannel::Ecdh
                                                 : mlk::SecureChannel::Off);
    m_sec.configure(m, psk, spub, spriv, peer);
    if (m_discovery)
        m_discovery->configure(m_sec, m_rx->isListening() ? m_rx->localPort() : quint16(m_cfg.port), m_cfg.autoDiscover && m_rx->isListening(),
                               QHostAddress(m_cfg.targetAddr), quint16(m_cfg.targetPort));
    m_rx->setChannel(m_cfg.secMode ? &m_sec : nullptr);
    m_rx->setExpectedPeer(QHostAddress(m_cfg.targetAddr),quint16(m_cfg.targetPort));
    m_linkSeen.invalidate(); m_configSeen.invalidate();
}

void MainWindow::setTarget(const QString &addr, int port)
{
    clearPendingCommands();
    m_cfg.targetAddr = addr;
    m_cfg.targetPort = port;
    m_rx->setExpectedPeer(QHostAddress(addr),quint16(port));
    m_linkSeen.invalidate(); m_configSeen.invalidate();
}

void MainWindow::setSecurity(int mode, const QString &pskHex)
{
    m_cfg.secMode = mode;
    if (!pskHex.isEmpty())
        m_cfg.pskHex = pskHex;
    setupSecure();
}

void MainWindow::setInjectDuration(int ms)
{
    if (ms > 0)
        m_cfg.injectDurationMs = ms;
}

void MainWindow::injectValues(double temp, double current, double vib, double speed)
{
    const double vals[4] = { temp, current, vib, speed };
    performInject(vals, m_cfg.injectDurationMs);
}

void MainWindow::performInject(const double vals[4], int durationMs)
{
    qint16 raw[4];
    for (int i = 0; i < mlk::kChannelCount; ++i) {
        const double k = m_cfg.ch[i].k != 0.0 ? m_cfg.ch[i].k : 1.0;
        const double r = qBound(-32768.0, (vals[i] - m_cfg.ch[i].b) / k, 32767.0);
        raw[i] = qint16(qRound(r));
    }
    const quint64 ts = quint64(QDateTime::currentMSecsSinceEpoch()) * 1000ull;
    const QByteArray d = mlk::buildInjectFrame(raw, quint16(durationMs), m_injectSeq++, ts);
    const qint64 n = m_rx->send(QHostAddress(m_cfg.targetAddr), quint16(m_cfg.targetPort), d);
    if (n < 0)
        statusBar()->showMessage(QStringLiteral("注入失败: %1").arg(m_rx->errorString()), 4000);
    else
        statusBar()->showMessage(QStringLiteral("已注入 %1:%2 (%3 ms)")
                                     .arg(m_cfg.targetAddr)
                                     .arg(m_cfg.targetPort)
                                     .arg(durationMs),
                                 4000);
}

void MainWindow::toggleListen(bool on)
{
    if (m_discovery)
        m_discovery->configure(m_sec, quint16(m_port->value()), on && m_cfg.autoDiscover,
                               QHostAddress(m_cfg.targetAddr), quint16(m_cfg.targetPort));
    if (on) {
        if (m_rx->start(QHostAddress(m_addr->text()), quint16(m_port->value()))) {
            m_listen->setText(QStringLiteral("停止"));
            statusBar()->showMessage(
                QStringLiteral("监听 %1:%2").arg(m_addr->text()).arg(m_port->value()));
            if (m_sec.mode() == mlk::SecureChannel::Ecdh && !m_cfg.peerPubHex.isEmpty())
                m_rx->beginHandshake(QHostAddress(m_cfg.targetAddr), quint16(m_cfg.targetPort));
        } else {
            m_listen->setChecked(false);
            QMessageBox::warning(this, QStringLiteral("绑定失败"), m_rx->errorString());
        }
    } else {
        clearPendingCommands();
        m_linkSeen.invalidate(); m_configSeen.invalidate();
        m_rx->stop();
        m_listen->setText(QStringLiteral("监听"));
        statusBar()->showMessage(QStringLiteral("未监听"));
    }
}

void MainWindow::onFrame(const mlk::Frame &f)
{
    if (f.type >= mlk::kTypeConfig) { handleControl(f); return; }
    if (f.type == mlk::kTypeWaveform)
        handleWaveform(f);
    else if (f.type == mlk::kTypeDiagnosis)
        handleDiagnosis(f);
}

void MainWindow::handleWaveform(const mlk::Frame &f)
{
    const int ch = f.ch;
    if (ch >= 0 && ch < mlk::kChannelCount) {
        if (m_haveSeq[ch] && f.seq > m_lastSeq[ch] + 1)
            m_lost += f.seq - m_lastSeq[ch] - 1;
        m_lastSeq[ch] = f.seq;
        m_haveSeq[ch] = true;
        m_channelSeen[ch].restart();
        if (!f.samplesY.isEmpty()) m_panels[ch]->addSamples(f.samples,f.samplesY,f.samplesZ,f.rateHz);
        else m_panels[ch]->addSamples(f.samples, f.rateHz);
        if (m_recording) {
            if (m_captureRecorder.full(f)) saveSnapshot();
            if (!m_captureRecorder.append(f)) {
                m_recordBtn->setChecked(false);
                showToast(QStringLiteral("记录已停止：样本块无效或待保存缓冲已满"),8000);
            }
        }
    }

    ++m_pkts;
    ++m_tickPkts;

    if (f.rateHz)
        m_lastRate = f.rateHz;

    const quint64 now = quint64(QDateTime::currentMSecsSinceEpoch()) * 1000ull;
    if (f.ver>=4 && f.tsUs && f.tsUs<=253402300799999999ULL) {
        const double ms = double(qint64(now)-qint64(f.tsUs)) / 1000.0;
        m_sampleAgeMs = m_sampleAgeSeen.isValid() ? m_sampleAgeMs * 0.9 + ms * 0.1 : ms;
        m_sampleAgeSeen.restart();
    }
}

void MainWindow::saveFault(const mlk::Frame &f)
{
    const auto ts=f.tsUs?f.tsUs:mlk::captureNowUs();
    const QJsonObject order{{"id",QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"faultTimeUs",QString::number(ts)},{"class",int(f.cls)},{"grade",int(f.grade)},
        {"message",QStringLiteral("分类事件")}};
    QString error;
    if (!mlk::WorkOrders(m_cfg.storageDir,m_cfg.passphrase).save(order,&error)
        || !mlk::CaptureStore(m_cfg.storageDir,m_cfg.passphrase).putLabel(mlk::faultCaptureLabel(order),&error))
        showToast(error,5000);
    else {m_latestOrder=order;m_faultTimeUs=ts;refreshFaults();}
}

void MainWindow::refreshFaults()
{
    if (!m_faultList)
        return;
    m_faultList->clear();
    const mlk::WorkOrders orderStore(m_cfg.storageDir, m_cfg.passphrase);
    const QDir orders(QDir(m_cfg.storageDir).filePath(QStringLiteral("orders")));
    for (const auto &file : orders.entryInfoList({"*.mlko"},QDir::Files,QDir::Time)) {
        const QString id = file.completeBaseName();
        QJsonObject order;
        /* 读取工单 JSON 失败（口令不符/容器损坏/字段非法）时保留文件名占位，
         * 记录仍然可见，其余工单与记录列表不受影响。 */
        if (!orderStore.load(id, order)) {
            auto *item = new QListWidgetItem(QStringLiteral("工单 · %1").arg(id), m_faultList);
            item->setData(Qt::UserRole, file.absoluteFilePath());
            continue;
        }
        /* v2 工单显示结构化 diagnosis 的具体故障与顶层等级；历史无 diagnosis 的
         * 工单保留原名。diagnosis 快照不含 grade，等级只在顶层。 */
        QString label = QStringLiteral("工单 · %1").arg(id);
        const auto diagnosis = order.value(QStringLiteral("diagnosis")).toObject();
        const int classIndex = diagnosis.value(QStringLiteral("classIndex")).toInt(mlk::kFaultUnavailable);
        const int grade = order.value(QStringLiteral("grade")).toInt(-1);
        if (order.value(QStringLiteral("schema")).toString() == QLatin1String(mlk::kFaultOrderSchema)
            && classIndex >= mlk::kFaultOverload && classIndex <= mlk::kFaultMountTilt
            && mlk::faultGradeValid(classIndex, grade))
            label = QStringLiteral("工单 · %1 · 分级 %2 · %3")
                        .arg(mlk::faultDisplayName(classIndex))
                        .arg(grade)
                        .arg(id);
        auto *item=new QListWidgetItem(label,m_faultList);
        item->setData(Qt::UserRole,file.absoluteFilePath());
    }
    for (const auto &entry:mlk::CaptureStore(m_cfg.storageDir,m_cfg.passphrase).list()) {
        auto *item=new QListWidgetItem(QStringLiteral("采集 · %1").arg(QFileInfo(entry.path).fileName()),m_faultList);
        item->setData(Qt::UserRole,entry.path);
    }
    mlk::FaultStore store(m_cfg.storageDir, m_cfg.encrypt, m_cfg.passphrase);
    for (const QString &p : store.list()) {
        auto *it = new QListWidgetItem(QFileInfo(p).fileName(), m_faultList);
        it->setData(Qt::UserRole, p);
    }
}

void MainWindow::replayFault(const QString &path)
{
    openCaptures();
    if (path.endsWith(".mlko")) m_captureDialog->openOrder(QFileInfo(path).completeBaseName());
    else if (path.endsWith(".mlkc")) m_captureDialog->openCapture(path);
    else m_captureDialog->openLegacy(path);
}

void MainWindow::handleDiagnosis(const mlk::Frame &f)
{
    if (f.ver == mlk::kInferenceVersion) {
        if (m_demo->isChecked()) m_demo->setChecked(false);
        m_replaySeen.restart(); m_replayExpired = false;
        m_replayPanel->showFrame(f);
        if ((f.valid & mlk::kValidRul) && (f.tsUs != m_rulLoggedTimestamp || f.windowIndex != m_rulLoggedWindow)) {
            QString error;
            if (!m_rulJournal.save(f, m_cfg.storageDir, m_cfg.encrypt, m_cfg.passphrase,
                                   m_cfg.retentionMonths, m_cfg.minFreeMb, &error))
                m_replayPanel->showError(QStringLiteral("寿命记录失败：%1").arg(error));
            else { m_rulLoggedTimestamp = f.tsUs; m_rulLoggedWindow = f.windowIndex; }
        }
        if (f.requestId && f.requestId == m_replayRequest) {
            m_replayPending.clear(); m_replayRetry->stop();
            if (f.commandStatus == 2) m_replayPanel->showError(QStringLiteral("回放命令未执行：%1").arg(f.message));
        }
    }
    m_diag = f;
    m_diagValid = true;
    if (mlk::hasClassification(f) && f.cls > 0 && m_lastCls == 0) {
        ++m_faults;
        saveFault(f);
    }
    if (mlk::hasClassification(f)) m_lastCls = f.cls;

    ++m_pkts;
    ++m_tickPkts;
    updateDiagnosisUi();
}

void MainWindow::updateDiagnosisUi()
{
    if (!m_diagValid)
        return;
    mlk::showEdgeAi(m_diag, m_diagRul, m_statAi, m_diagBackend, m_diagState, m_diagWindow);
}

bool MainWindow::faultStatusLinkAlive() const
{
    return m_faultStatusValid && m_faultStatusSeen.isValid()
        && m_faultStatusSeen.elapsed() <= mlk::kFaultStatusLinkTimeoutMs;
}

qint64 MainWindow::faultStatusAgeNow() const
{
    /* m_faultStatusAgeSeen 只在接纳新窗口时重置；同窗口重发不推进年龄。 */
    if (!m_faultStatusValid || !m_faultStatusAgeSeen.isValid() || m_faultStatusAgeMs < 0)
        return -1;
    /* 板端年龄加收到后的本地单调时钟经过，避免把两端 UTC 差当成结果新鲜度。 */
    return m_faultStatusAgeMs + qint64(m_faultStatusAgeSeen.elapsed());
}

bool MainWindow::faultStatusExpired() const
{
    if (!m_faultStatusValid || !faultStatusLinkAlive())
        return true;
    const qint64 ageMs = faultStatusAgeNow();
    return ageMs >= 0 && ageMs > mlk::kFaultStatusMaxAgeMs;
}

/* 完整故障状态行：只有模型新消息能改内容，超时或板端报告不可用时一律显示
 * N/A；无效不等于正常。窗口时间与后端放在 tooltip，不新增控件。 */
void MainWindow::updateFaultLiveUi()
{
    if (!m_faultLive)
        return;
    const bool linkAlive = faultStatusLinkAlive();
    const qint64 ageNowMs = faultStatusAgeNow();
    const bool ageAlive = ageNowMs < 0 || ageNowMs <= mlk::kFaultStatusMaxAgeMs;
    const bool live = linkAlive && ageAlive && m_faultStatus.valid;
    QString text, color;
    if (live) {
        const int classIndex = m_faultStatus.classIndex;
        const bool alarm = classIndex != mlk::kFaultNormal;
        text = QStringLiteral("实时主故障：%1　分级：%2　分数：%3　状态：%4　告警：%5")
                   .arg(mlk::faultDisplayName(classIndex))
                   .arg(m_faultStatus.grade)
                   .arg(m_faultStatus.hasConfidence
                            ? QString::number(m_faultStatus.confidence, 'f', 3)
                            : QStringLiteral("N/A"))
                   .arg(mlk::faultStateDisplay(m_faultStatus.state))
                   .arg(alarm ? (m_faultAlarmConfirmed ? QStringLiteral("已确认")
                                                       : QStringLiteral("待确认"))
                              : QStringLiteral("无"));
        color = classIndex == mlk::kFaultNormal ? QStringLiteral("#37c08a")
              : m_faultStatus.grade >= 2 ? QStringLiteral("#e65a5a")
                                         : QStringLiteral("#e6c037");
    } else {
        QString state = QStringLiteral("N/A");
        QString reason;
        if (!m_faultStatusValid) {
            /* 从未收到完整状态：保持 N/A，不显示成正常。 */
        } else if (!linkAlive) {
            state = mlk::faultStateDisplay(mlk::kFaultStale);
            reason = QStringLiteral("状态消息超时");
        } else if (!ageAlive) {
            state = mlk::faultStateDisplay(mlk::kFaultStale);
            reason = QStringLiteral("结果年龄超限");
        } else {
            /* 板端报告的结果本身不可用（加载/等待/数据不足/停止/错误）。 */
            state = mlk::faultStateDisplay(m_faultStatus.state);
            reason = m_faultStatus.reason;
        }
        text = QStringLiteral("实时主故障：N/A　分级：N/A　分数：N/A　状态：%1　告警：无").arg(state);
        if (!reason.isEmpty())
            text += QStringLiteral("　原因：%1").arg(reason);
        color = QStringLiteral("#8b949e");
    }

    if (m_faultLive->text() != text)
        m_faultLive->setText(text);
    const QString style = QStringLiteral("color: %1;%2").arg(
        color, live ? QStringLiteral(" font-weight: bold;") : QString());
    if (m_faultLive->property("faultStyle").toString() != style) {
        m_faultLive->setProperty("faultStyle", style);
        m_faultLive->setStyleSheet(style);
    }

    QString tooltip;
    if (m_faultStatusValid) {
        const QString window = m_faultStatus.endUs
            ? QDateTime::fromMSecsSinceEpoch(qint64(m_faultStatus.endUs / 1000)).toLocalTime()
                  .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
            : QStringLiteral("—");
        tooltip = QStringLiteral("窗口结束：%1　后端：%2")
                      .arg(window, mlk::faultBackendName(m_faultStatus.backend));
        tooltip += QStringLiteral("\nendUs：%1").arg(QString::number(m_faultStatus.endUs));
        if (m_faultStatusAgeMs >= 0) {
            const QString since = m_faultStatusAgeSeen.isValid()
                ? QStringLiteral("%1 ms").arg(qint64(m_faultStatusAgeSeen.elapsed()))
                : QStringLiteral("—");
            tooltip += QStringLiteral("\n板端年龄：%1 ms　本地已过：%2")
                           .arg(m_faultStatusAgeMs)
                           .arg(since);
        }
    } else {
        tooltip = QStringLiteral("尚未收到完整故障状态（UDP v6/type8）");
    }
    if (!m_faultStatusError.isEmpty())
        tooltip += QStringLiteral("\n最近一次解析错误：%1").arg(m_faultStatusError);
    if (m_faultLive->toolTip() != tooltip)
        m_faultLive->setToolTip(tooltip);
}

void MainWindow::requestReplay(quint8 action, quint8 backend, quint32 index, quint32 interval)
{
    if (!m_replayPending.isEmpty()) { m_replayPanel->showError(QStringLiteral("上一条回放命令等待确认")); return; }
    if (!m_rx->isListening()) { m_replayPanel->showError(QStringLiteral("请先启动监听")); return; }
    if (QHostAddress(m_cfg.targetAddr).isNull()) { m_replayPanel->showError(QStringLiteral("PS 目标地址无效")); return; }
    if (m_demo->isChecked()) m_demo->setChecked(false);
    ++m_replayRequest; if (!m_replayRequest) ++m_replayRequest;
    m_replayPending = mlk::buildReplayControl(action, backend, index, interval, m_replayRequest,
        quint64(QDateTime::currentMSecsSinceEpoch()) * 1000);
    m_replayAttempts = 1;
    m_rx->send(QHostAddress(m_cfg.targetAddr), quint16(m_cfg.targetPort), m_replayPending);
    m_replayRetry->start();
}

void MainWindow::onTick()
{
    if (m_discovery) m_discovery->setConnected(m_linkSeen.isValid() && m_linkSeen.elapsed() <= 5000);
    updateConnectionStatus();
    if (m_replaySeen.isValid() && m_replaySeen.elapsed() > 3500 && !m_replayExpired) {
        m_replayExpired = true; m_replayPanel->setConnected(false);
        if (m_diag.ver == mlk::kInferenceVersion) { m_diag.valid = 0; updateDiagnosisUi(); }
    }
    for (int i=0;i<4;++i)
        if (!m_channelSeen[i].isValid() || m_channelSeen[i].elapsed() > (i==mlk::kTemp ? 6000 : 2000))
            m_panels[i]->setUnavailable(QStringLiteral("等待板端有效数据"));
    if (!m_statusSeen.isValid() || m_statusSeen.elapsed()>5000) {
        m_networkStatus->setText(QStringLiteral("网络：不可用（板端离线）"));
        m_npuPanel->setUnavailable(QStringLiteral("不可用（板端状态超时）"));
        m_cpuSummary->setText(QStringLiteral("核心板：离线"));
        m_cpuText->setText(QStringLiteral("不可用")); m_memText->setText(QStringLiteral("不可用"));
        m_netText->setText(QStringLiteral("不可用")); m_timeLabel->setText(QStringLiteral("板端时间：不可用"));
        updateDiagnosisUi();
    }
    /* 完整故障状态按板端年龄与本地单调时钟刷新；与寿命状态和板端心跳无关。 */
    updateFaultLiveUi();
    m_statRate->setText(QStringLiteral("接收：%1 包/s").arg(m_tickPkts * 2));
    m_tickPkts = 0;
    m_statPkts->setText(QStringLiteral("累计帧：%1").arg(m_pkts));
    m_statLost->setText(QStringLiteral("丢帧：%1").arg(m_lost));
    setMetric(m_statLatency,m_rttSeen.isValid() && m_rttSeen.elapsed()<=5000
        ? QStringLiteral("%1 ms").arg(m_rttMs,0,'f',1) : QStringLiteral("—"));
    setMetric(m_statSampleAge,m_sampleAgeSeen.isValid() && m_sampleAgeSeen.elapsed()<=5000
        ? QStringLiteral("%1 ms").arg(m_sampleAgeMs,0,'f',1) : QStringLiteral("—"));
    if (qEnvironmentVariableIsSet("MLK_LINK_METRICS_LOG")
        && (!m_metricsLogSeen.isValid() || m_metricsLogSeen.elapsed()>=5000)) {
        qInfo().noquote() << QStringLiteral("remote-pc 链路：%1，%2")
            .arg(m_statLatency->text(),m_statSampleAge->text());
        m_metricsLogSeen.restart();
    }
    const qint64 s = m_uptime.elapsed() / 1000;
    m_statUptime->setText(QStringLiteral("%1:%2 · 故障 %3")
                              .arg(s / 60, 2, 10, QLatin1Char('0'))
                              .arg(s % 60, 2, 10, QLatin1Char('0'))
                              .arg(m_faults));
}

void MainWindow::onDemoToggled(bool on)
{
    if (m_recording) {
        saveSnapshot();
        if (!m_captureRecorder.empty()) m_recordBtn->setChecked(false);
        else m_captureRecorder.begin(m_cfg,on?"remote-pc-demo":"remote-pc");
    }
    if (on) {
        m_demoTimer->start();
        m_demoDiagTimer->start();
    } else {
        m_demoTimer->stop();
        m_demoDiagTimer->stop();
    }
}

void MainWindow::onDemoTick()
{
    static constexpr int n = 32;
    const quint64 ts = quint64(QDateTime::currentMSecsSinceEpoch()) * 1000ull;
    for (int ch = 0; ch < mlk::kChannelCount; ++ch) {
        Frame f;
        f.ver = mlk::kVersion;
        f.type = mlk::kTypeWaveform;
        f.ch = quint8(ch);
        f.seq = m_demoSeq[ch]++;
        f.cnt = m_demoCnt[ch];
        f.rateHz = 1000;
        f.tsUs = ts;
        f.samples.resize(n);
        for (int i = 0; i < n; ++i) {
            m_demoPhase[ch] += 0.02;
            const double p = m_demoPhase[ch];
            double v = 0;
            switch (ch) {
            case mlk::kTemp:
                v = 2500 + 150 * std::sin(p * 0.2);
                break;
            case mlk::kCurrent:
                v = 250 + 120 * std::sin(p * 2.0);
                break;
            case mlk::kVibration:
                v = 100 + 400 * std::sin(p * 8.0);
                break;
            case mlk::kSpeed:
                v = 1500 + 40 * std::sin(p * 0.1);
                break;
            default:
                break;
            }
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
            f.samples[i] = qint16(v + (QRandomGenerator::global()->bounded(200) - 100) * 0.1);
#else
            f.samples[i] = qint16(v + (qrand() % 200 - 100) * 0.1);
#endif
        }
        m_demoCnt[ch] += n;
        onFrame(f);
    }
}

void MainWindow::onDemoDiagTick()
{
    static int step = 0;
    Frame f;
    f.ver = mlk::kVersion;
    f.type = mlk::kTypeDiagnosis;
    f.seq = quint32(step);
    f.tsUs = quint64(QDateTime::currentMSecsSinceEpoch()) * 1000ull;
    const int phase = (step / 5) % 4;
    f.cls = quint8(phase < 2 ? 0 : (phase == 2 ? 1 : 2));
    f.grade = f.cls;
    f.conf = quint16(850 + (step * 37) % 140);
    f.rul = quint32(2000 > step * 3 ? 2000 - step * 3 : 0);
    f.inferUs = quint32(1800 + (step * 53) % 900);
    ++step;
    onFrame(f);
}

void MainWindow::saveSnapshot()
{
    if (m_captureRecorder.empty()) return;
    QString path,error;
    if (!m_captureRecorder.flush(&path,&error)) {
        showToast(QStringLiteral("采集保存失败（样本保留待重试）：%1").arg(error),8000); return;
    }
    refreshFaults();
    if (m_captureDialog && m_captureDialog->isVisible()) m_captureDialog->refresh();
    showToast(QStringLiteral("已加密保存四传感器记录：%1").arg(QFileInfo(path).fileName()),5000);
}

void MainWindow::openCaptures() {
    if (!m_captureDialog) m_captureDialog=new mlk::CaptureDialog(m_cfg,this);
    else m_captureDialog->configure(m_cfg);
    m_captureDialog->show();m_captureDialog->raise();m_captureDialog->activateWindow();
}


void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    updateHeaderPosition();
    updateSidebarGeometry();
    QTimer::singleShot(0,this,[this] { updateHeaderPosition(); updateSidebarGeometry(); });
}

void MainWindow::updateSidebarGeometry()
{
    if (!m_sidebar || !m_grid || !centralWidget())
        return;
    const QPoint topLeft = m_grid->mapTo(centralWidget(), QPoint(0, 0));
    const int margin = 14;
    m_sidebar->setGeometry(margin, topLeft.y(),
                           m_sidebar->width(),
                           qMax(120, centralWidget()->height() - topLeft.y() - 8));
    if (m_sidebar->isVisible())
        m_sidebar->raise();
}


void MainWindow::updateHeaderPosition()
{
    if (!m_grid || !m_title || !m_timeLabel || !centralWidget())
        return;
    /* 网格水平中心即温度/电流与振动/转速两列之间的分隔线；标题与板端时间
     * 都以该点为水平中心，侧栏展开或窗口缩放时重新对齐。 */
    const QPoint center = m_grid->mapTo(centralWidget(), QPoint(m_grid->width() / 2, 0));
    m_title->adjustSize();
    m_title->move(center.x() - m_title->width() / 2, 1);
    m_timeLabel->adjustSize();
    m_timeLabel->move(center.x() - m_timeLabel->width() / 2, 1 + m_title->height());
    m_title->raise();
    m_timeLabel->raise();
}

void MainWindow::showToast(const QString &text, int ms) { statusBar()->showMessage(text,ms); }
MainWindow::~MainWindow() { delete m_captureApi; if (!m_captureRecorder.empty()) saveSnapshot(); }
void MainWindow::openSettings()
{
    bool accepted = false;
    const auto password = QInputDialog::getText(this,QStringLiteral("本机配置验证"),QStringLiteral("remote-pc 配置密码"),QLineEdit::Password,{},&accepted);
    if (!accepted) return;
    if (password != m_cfg.settingsPassword) { showToast(QStringLiteral("配置密码错误"),4000); return; }
    auto draft = m_cfg;
    PlSettingsDialog dialog(draft,this,true);
    connect(&dialog,&PlSettingsDialog::recordsRemoved,this,[this] {
        refreshFaults();
        if (m_captureDialog && m_captureDialog->isVisible()) m_captureDialog->refresh();
    });
    if (dialog.exec() != QDialog::Accepted) return;
    const bool sharedChanged = !mlk::sameSharedValues(draft,m_cfg);
    // Endpoint-local credentials, transport settings and the board-owned owner
    // flag never follow the shared snapshot. Shared values stay local while
    // remote-pc owns them, even if the board is offline.
    draft.remotePriority=m_cfg.remotePriority; draft.sharedRevision=m_cfg.sharedRevision;
    draft.sharedShadow=m_cfg.sharedShadow;
    const bool transportChanged = draft.autoDiscover != m_cfg.autoDiscover || draft.secMode != m_cfg.secMode || draft.pskHex != m_cfg.pskHex
        || draft.selfPubHex != m_cfg.selfPubHex || draft.selfPrivHex != m_cfg.selfPrivHex
        || draft.peerPubHex != m_cfg.peerPubHex || draft.addr != m_cfg.addr || draft.port != m_cfg.port;
    const bool targetChanged = draft.targetAddr != m_cfg.targetAddr || draft.targetPort != m_cfg.targetPort;
    m_cfg=draft; m_cfg.save(); applyConfig();
    if (targetChanged) {
        clearPendingCommands();
        m_rx->setExpectedPeer(QHostAddress(m_cfg.targetAddr),quint16(m_cfg.targetPort));
        m_configSeen.invalidate(); m_statusSeen.invalidate(); m_linkSeen.invalidate();
    }
    if (transportChanged) {
        const bool listening=m_rx->isListening(); m_rx->stop(); setupSecure();
        if (listening) { m_listen->setChecked(false); startListening(quint16(m_cfg.port)); }
    }
    if (sharedChanged) {
        if (m_cfg.remotePriority) {
            /* 配置与连接独立：连不上时先保存本机值，连接恢复后自动提交并覆盖板端。 */
            m_cfg.sharedShadow=true; m_cfg.save();
            if (m_configSeen.isValid() && m_configSeen.elapsed()<=5000 && m_sec.ready()) {
                if (m_configPending.isEmpty()) {
                    pushSharedConfig();
                    showToast(QStringLiteral("共享配置已发送，等待板端确认"),4000);
                } else {
                    showToast(QStringLiteral("上一笔共享配置仍等待确认；本机值已保存"),5000);
                }
            } else {
                showToast(QStringLiteral("共享配置已保存本机；连接板端后自动同步"),5000);
            }
        } else {
            showToast(QStringLiteral("共享参数由 ps-local 决定，本机修改未提交"),5000);
        }
    }
    queryConfig();
}
void MainWindow::pushSharedConfig()
{
    if (!m_cfg.remotePriority || !m_cfg.sharedShadow) return;
    if (!m_rx->isListening() || m_sec.mode()==mlk::SecureChannel::Off || !m_sec.ready()) return;
    if (!m_configSeen.isValid() || m_configSeen.elapsed()>5000) return;
    if (!m_configPending.isEmpty() || QHostAddress(m_cfg.targetAddr).isNull()) return;
    auto request=mlk::sharedConfig(m_cfg);
    request.insert("revision",double(m_cfg.sharedRevision));
    request.insert("op","set");
    m_configRequest=QUuid::createUuid().toString(QUuid::WithoutBraces);
    request.insert("request",m_configRequest);
    m_configPending=mlk::buildObjectFrame(mlk::kTypeConfig,request);
    if (m_configPending.isEmpty()) { m_configRequest.clear(); return; }
    m_configAttempts=1;
    m_rx->send(QHostAddress(m_cfg.targetAddr),quint16(m_cfg.targetPort),m_configPending);
}
void MainWindow::queryConfig()
{    if (!m_rx->isListening()) return;
    if (m_sec.mode()==mlk::SecureChannel::Ecdh && m_cfg.peerPubHex.isEmpty()) return;
    if (!m_configPending.isEmpty()) {
        if (++m_configAttempts > 4) { m_configPending.clear(); showToast(QStringLiteral("共享配置确认超时，正在重新读取板端值"),6000); }
        else m_rx->send(QHostAddress(m_cfg.targetAddr),quint16(m_cfg.targetPort),m_configPending);
    }
    if (m_sec.mode()==mlk::SecureChannel::Ecdh && m_linkSeen.isValid() && m_linkSeen.elapsed()>5000)
        setupSecure();
    if (m_sec.mode()==mlk::SecureChannel::Ecdh && !m_sec.ready()) {
        if (!m_handshakeSeen.isValid() || m_handshakeSeen.elapsed()>3000) {
            m_rx->beginHandshake(QHostAddress(m_cfg.targetAddr),quint16(m_cfg.targetPort)); m_handshakeSeen.restart();
        }
        return;
    }
    if (!m_probeRequest.isEmpty() && m_probeStarted.elapsed()<=5000) return;
    m_probeRequest=QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_probeStarted.start();
    if (m_rx->send(QHostAddress(m_cfg.targetAddr),quint16(m_cfg.targetPort),
        mlk::buildObjectFrame(mlk::kTypeConfig,{{"op","get"},{"request",m_probeRequest}}))<0) {
        m_probeRequest.clear();m_probeStarted.invalidate();
    }
}
void MainWindow::handleControl(const mlk::Frame &f)
{
    if (f.type==mlk::kTypeConfig && f.object.value("op").toString()=="snapshot") {
        auto draft=m_cfg;
        if (!mlk::readSharedConfig(f.object,draft)) return;
        if (!m_probeRequest.isEmpty() && f.object.value("request").toString()==m_probeRequest
            && m_probeStarted.isValid()) {
            if (m_probeStarted.elapsed()<=5000) {
                m_rttMs=double(m_probeStarted.nsecsElapsed())/1000000.0;m_rttSeen.restart();
            }
            m_probeRequest.clear();m_probeStarted.invalidate();
        }
        /* 板端优先时采用板端值；remote-pc 优先时本机值是权威，连上后把差异
         * 提交回板端，板端离线期间的本地修改由本机快照覆盖。 */
        if (!draft.remotePriority || !m_cfg.sharedShadow) {
            const bool changed=!mlk::sameSharedValues(draft,m_cfg) || draft.sharedRevision!=m_cfg.sharedRevision;
            m_cfg=draft; m_cfg.sharedShadow=true; if(changed) m_cfg.save(); applyConfig();
        } else {
            /* 电流零点 / 增益由板端采样与归零确定，始终采用板端值；其余共享
             * 参数以本机为准，连接后提交并覆盖板端离线期间的修改。 */
            const bool calibrationChanged = m_cfg.currentZero != draft.currentZero
                                            || m_cfg.currentGain != draft.currentGain;
            m_cfg.currentZero = draft.currentZero;
            m_cfg.currentGain = draft.currentGain;
            const bool differs = !mlk::sameSharedValues(draft, m_cfg);
            const bool changed = m_cfg.sharedRevision != draft.sharedRevision
                                 || !m_cfg.remotePriority || calibrationChanged;
            m_cfg.sharedRevision = draft.sharedRevision;
            m_cfg.remotePriority = true;
            if (changed) m_cfg.save();
            if (calibrationChanged) applyConfig();
            if (differs) pushSharedConfig();
        }
        m_configSeen.restart();
        const auto error=f.object.value("error").toString();
        if (!m_configPending.isEmpty() && f.object.value("request").toString()==m_configRequest) {
            m_configPending.clear();
            if (error.isEmpty()) showToast(QStringLiteral("共享配置已由板端确认 · v%1").arg(m_cfg.sharedRevision),4000);
        }
        m_srcText->setText(QStringLiteral("共享配置：%1 优先 · v%2").arg(m_cfg.remotePriority?QStringLiteral("remote-pc"):QStringLiteral("ps-local")).arg(m_cfg.sharedRevision));
        if (!error.isEmpty()) showToast(error,6000);
    } else if (f.type==mlk::kTypeWorkOrder) {
        if (m_sec.mode()==mlk::SecureChannel::Off || !m_sec.ready()) return;
        QString error;
        if (!mlk::WorkOrders(m_cfg.storageDir,m_cfg.passphrase).save(f.object,&error)) { showToast(error,5000); return; }
        if (!mlk::CaptureStore(m_cfg.storageDir,m_cfg.passphrase).putLabel(mlk::faultCaptureLabel(f.object),&error)) {
            showToast(QStringLiteral("故障标签保存失败：%1").arg(error),5000); return;
        }
        if (m_captureDialog && m_captureDialog->isVisible()) m_captureDialog->refresh();
        const auto ts=f.object.value("faultTimeUs").toString().toULongLong();
        if (ts>=m_faultTimeUs) { m_faultTimeUs=ts; m_latestOrder=f.object; }
        m_rx->send(QHostAddress(m_cfg.targetAddr),quint16(m_cfg.targetPort),mlk::buildObjectFrame(mlk::kTypeOrderAck,
            {{"id",f.object.value("id")},{"status","stored"}}));
        updateDiagnosisUi(); refreshFaults();
    } else if (f.type==mlk::kTypeBoardStatus) {
        const auto &o=f.object; m_statusSeen.restart();
        const auto model=o.value("modelFault").toObject();
        m_modelFault=model.isEmpty() ? QJsonObject{}
            : mlk::validModelFaultSummary(model) ? model : mlk::unknownModelFault();
        updateDiagnosisUi();
        bool timeValid=false;
        const auto faultTime=o.value("faultTimeUs").toString().toULongLong(&timeValid);
        if (timeValid && faultTime<=253402300799999999ULL && faultTime!=m_faultTimeUs) {
            m_faultTimeUs=faultTime; updateDiagnosisUi();
        }
        m_cpuSummary->setText(QStringLiteral("CPU: %1%\n内存: %2%").arg(o.value("cpu").toDouble(),0,'f',1).arg(o.value("memory").toDouble(),0,'f',1));
        m_cpuText->setText(QStringLiteral("%1%").arg(o.value("cpu").toDouble(),0,'f',1));
        m_cpuBar->setValue(qRound(o.value("cpu").toDouble())); m_memBar->setValue(qRound(o.value("memory").toDouble()));
        m_diskBar->setValue(qRound(o.value("disk").toDouble()));
        m_cpuPlot->addSamples(0,{qint16(qBound(0.0,o.value("cpu").toDouble()*100,10000.0))}); m_cpuPlot->refresh();
        m_memPlot->addSamples(0,{qint16(qBound(0.0,o.value("memory").toDouble()*100,10000.0))}); m_memPlot->refresh();
        m_memText->setText(QStringLiteral("%1 / %2 MiB").arg(o.value("memoryUsedKb").toDouble()/1024,0,'f',0).arg(o.value("memoryTotalKb").toDouble()/1024,0,'f',0));
        m_diskText->setText(QStringLiteral("%1%").arg(o.value("disk").toDouble(),0,'f',1));
        m_netText->setText(QStringLiteral("%1 · RX %2 / TX %3 KB/s").arg(o.value("net").toString()).arg(o.value("rxKbps").toDouble(),0,'f',1).arg(o.value("txKbps").toDouble(),0,'f',1));
        QStringList cores; const auto values=o.value("cores").toArray();
        for (int i=0;i<values.size();++i) cores << QStringLiteral("CPU%1 %2%").arg(i).arg(values[i].toDouble(),0,'f',0);
        m_cpuCores->setText(cores.join("  "));
        mlk::NetworkStatus network;
        m_networkStatus->setText(mlk::NetworkStatus::fromJson(o.value("network").toObject(),network)
            ? network.text() : QStringLiteral("网络：不可用（板端未提供）"));
        mlk::NpuStats npu;
        if (mlk::NpuStats::fromJson(o.value("npu").toObject(), npu)) m_npuPanel->updateStats(npu);
        else m_npuPanel->setUnavailable(QStringLiteral("板端未提供有效 NPU 状态"));
        m_featureText->setText(o.value("features").toString()); m_statDmaBlocks->setText(o.value("dmaBlocks").toString());
        m_statDmaLost->setText(o.value("dmaLost").toString()); m_statDmaAge->setText(o.value("dmaAge").toString());
        m_statRead->setText(o.value("readTime").toString());
        m_timeLabel->setText(QStringLiteral("板端时间：%1").arg(o.value("time").toString())); updateHeaderPosition();
    } else if (f.type==mlk::kTypeFaultStatus) {
        /* 完整故障状态（UDP v6/type8）独立解析与保存：与寿命 m_diag、v1 粗摘要
         * m_modelFault 分离，解析失败或晚到包都不能污染既有显示。 */
        mlk::FaultResult next;
        qint64 boardAgeMs = -1;
        bool confirmed = false;
        QString error;
        if (!mlk::readFaultStatus(f.object,next,&boardAgeMs,&confirmed,&error)) {
            /* 同一原因只记录一次：逐包日志会被 1 Hz 心跳刷屏。 */
            if (m_faultStatusError != error) {
                m_faultStatusError = error;
                qWarning().noquote() << QStringLiteral("remote-pc 完整故障状态丢弃：%1").arg(error);
                showToast(QStringLiteral("完整故障状态无效：%1").arg(error),4000);
            }
            updateFaultLiveUi();
        } else {
            m_faultStatusError.clear();
            bool adopt = false, sameWindow = false;
            if (!m_faultStatusValid) {
                adopt = true; // 首次 run
            } else if (mlk::faultResultSameWindow(next,m_faultStatus)) {
                sameWindow = true; // 同窗口重发
            } else if (next.run == m_faultStatus.run) {
                adopt = mlk::faultResultNewerWindow(next,m_faultStatus); // 只接受更新的窗口
            } else {
                adopt = faultStatusExpired(); // 不同 run：旧状态超时后按新会话接纳
            }
            if (adopt) {
                m_faultStatus = next;
                m_faultStatusValid = true;
                m_faultStatusAgeMs = boardAgeMs;
                m_faultAlarmConfirmed = confirmed;
                m_faultStatusSeen.start();
                m_faultStatusAgeSeen.start();
            } else if (sameWindow) {
                /* 同窗口重发只续链路活跃时间，不刷新结果年龄；停止/过期等不可用
                 * 状态立即撤销当前显示，但不能据此从不可用回升为有效。 */
                if (!next.valid) {
                    m_faultStatus = next;
                    m_faultAlarmConfirmed = confirmed;
                }
                m_faultStatusSeen.start();
            }
            updateFaultLiveUi();
        }
    }
}

void MainWindow::updateConnectionStatus()
{
    if (!m_connectionStatus) return;
    QString text; QString color=QStringLiteral("#8b949e");
    if (!m_rx->isListening()) text=QStringLiteral("未连接");
    else if (m_sec.mode()==mlk::SecureChannel::Ecdh && m_cfg.peerPubHex.isEmpty()) text=QStringLiteral("等待配对");
    else if (m_sec.mode()==mlk::SecureChannel::Ecdh && !m_sec.ready()) text=QStringLiteral("正在握手");
    else if (!m_linkSeen.isValid()) text=QStringLiteral("等待板卡");
    else if (m_linkSeen.elapsed()>5000) { text=QStringLiteral("连接超时"); color=QStringLiteral("#e6c037"); }
    else { text=m_sec.mode()==mlk::SecureChannel::Off?QStringLiteral("明文已连接"):QStringLiteral("加密已连接"); color=QStringLiteral("#37c08a"); }
    if (m_connectionStatus->text()!=text) {
        qInfo().noquote() << QStringLiteral("remote-pc 连接：%1，目标 %2:%3")
            .arg(text,m_cfg.targetAddr).arg(m_cfg.targetPort);
        m_connectionStatus->setText(text);
        m_connectionStatus->setStyleSheet(QStringLiteral("font-size: 12px; color: %1;").arg(color));
    }
    m_connectionStatus->setToolTip(QStringLiteral("目标 %1:%2；5 秒未收到目标板卡有效数据即超时").arg(m_cfg.targetAddr).arg(m_cfg.targetPort));
}

void MainWindow::clearPendingCommands()
{
    m_probeRequest.clear();m_probeStarted.invalidate();m_rttSeen.invalidate();
    m_sampleAgeSeen.invalidate();
    m_configPending.clear(); m_configRequest.clear(); m_configAttempts=0;
    m_replayPending.clear(); m_replayAttempts=0;
    if (m_replayRetry) m_replayRetry->stop();
}
