#include "klocalwindow.h"
#include "peerdiscovery.h"
#include "alarmcontroller.h"
#include "modelfaultserver.h"
#include "modelfault.h"

#include <QDateTime>
#include <QtWidgets>
#include <limits>
#include <QFile>
#include <QEventLoop>
#include <QSharedPointer>

#include "datasource.h"
#include "faultstore.h"
#include "capturedialog.h"
#include "modelfaultserver.h"
#include "boardtime.h"
#include "plotwidget.h"
#include "npupanel.h"
#include "networkstatus.h"
#include "pl_host.h"
#include "plsettingsdialog.h"
#include "sensorpanel.h"
#include "virtualkeyboard.h"
#include "replaycontroller.h"
#include "replaypanel.h"
#include "diagnosisui.h"
#include "receiver.h"
#include "sharedconfig.h"
#include "workorders.h"
#include "workorderoutbox.h"
#include "networksettingsdialog.h"
#include "faultlivecontroller.h"
#include "faultevaluator.h"
#include "faultevalpanel.h"
#include "faultlog.h"
#include "faultpanel.h"
#include "faultresult.h"
#include "faultstatus.h"

namespace {
/* 指标与诊断统一用「标签：数据」一行显示，避免标签与数值分列后隔得太远。 */
void setMetric(QLabel *label, const QString &value) {
    if (!label)
        return;
    label->setText(QStringLiteral("%1：%2").arg(label->property("caption").toString(), value));
}
/* PL 特征摘要：标题一行，下面按 RMS（X/Y/Z）、FFT、功率逐行缩进列出。
 * 用表格排版，行距与对齐都不依赖等宽字体。 */
QString featureTableHtml(const QString &rmsX, const QString &rmsY, const QString &rmsZ,
                         const QString &bins, const QString &power) {
    return QStringLiteral(
               "<table cellspacing=\"0\" cellpadding=\"4\">"
               "<tr><td colspan=\"3\">PL 特征摘要：</td></tr>"
               "<tr><td></td><td>RMS：</td><td>X：%1</td></tr>"
               "<tr><td></td><td></td><td>Y：%2</td></tr>"
               "<tr><td></td><td></td><td>Z：%3</td></tr>"
               "<tr><td></td><td>FFT：</td><td>%4 bin</td></tr>"
               "<tr><td></td><td>功率：</td><td>%5</td></tr>"
               "</table>")
        .arg(rmsX, rmsY, rmsZ, bins, power);
}
QString featureNoteHtml(const QString &note) {
    return QStringLiteral(
               "<table cellspacing=\"0\" cellpadding=\"4\">"
               "<tr><td colspan=\"3\">PL 特征摘要：</td></tr>"
               "<tr><td></td><td colspan=\"2\">%1</td></tr>"
               "</table>")
        .arg(note);
}
}

KLocalWindow::KLocalWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("MLK-F3P PS 本地看板"));
    m_cfg.load();
    if (!m_cfg.ensureIdentity()) qWarning("ps-local: identity could not be loaded or saved");
    m_latestOrder = mlk::WorkOrders(m_cfg.storageDir, m_cfg.passphrase).latest();
    m_faultTimeUs = m_latestOrder.value("faultTimeUs").toString().toULongLong();

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
    auto *network = new QPushButton(QStringLiteral("网络配置"), central);
    network->setObjectName(QStringLiteral("networkConfig"));
    network->setToolTip(QStringLiteral("千兆网 / 4G / WiFi；连接优先级 千兆网 > 4G > WiFi"));
    leftBox->addWidget(network, 0, Qt::AlignBottom);
    m_remoteConnection = new QLabel(QStringLiteral("上位机：未连接"), central);
    m_remoteConnection->setObjectName(QStringLiteral("remoteConnectionStatus"));
    m_remoteConnection->setTextFormat(Qt::PlainText);
    auto *connectionBox = new QVBoxLayout;
    connectionBox->setSpacing(0);
    m_networkStatus = new QLabel(QStringLiteral("网络：未连接"), central);
    m_networkStatus->setObjectName(QStringLiteral("networkConnectionStatus"));
    m_networkStatus->setTextFormat(Qt::PlainText);
    m_networkStatus->setStyleSheet(m_cpuSummary->styleSheet());
    m_remoteConnection->setStyleSheet(m_cpuSummary->styleSheet());
    connectionBox->addWidget(m_networkStatus);
    connectionBox->addWidget(m_remoteConnection);
    leftBox->addLayout(connectionBox);
    connect(network, &QPushButton::clicked, this, [this] {
        NetworkSettingsDialog dialog(this);
        dialog.show();
        if (m_vk) m_vk->raise();
        QEventLoop loop;
        connect(&dialog, &QDialog::finished, &loop, &QEventLoop::quit);
        loop.exec();
        if (m_vk) m_vk->hideKeyboard();
    });
    bar->addLayout(leftBox);
    bar->addStretch(1);

    auto *rightBox = new QHBoxLayout;
    auto *settings = new QPushButton(QStringLiteral("配置"), central);
    settings->setToolTip(QStringLiteral("参数 / 加密 / 传输 / 密钥（F2）"));
    m_recordBtn = new QPushButton(QStringLiteral("未记录"), central);
    m_recordBtn->setCheckable(true);
    m_recordBtn->setObjectName(QStringLiteral("recordButton"));
    m_recordBtn->setStyleSheet(QStringLiteral(
        "QPushButton:checked { background: #238636; color: #ffffff; border: 1px solid #2ea043; }"));
    m_recordBtn->setToolTip(QStringLiteral("按配置时长保存全部接收样本；四传感器始终加密存储"));
    auto *sync = new QPushButton(QStringLiteral("时间同步"), central);
    sync->setObjectName(QStringLiteral("timeSyncButton"));
    sync->setToolTip(QStringLiteral("立即用网络时间源校时；网络不可用时在「配置」中手动设置时间"));
    rightBox->addWidget(sync);
    rightBox->addWidget(m_recordBtn);
    auto *captures = new QPushButton(QStringLiteral("回放"), central);
    captures->setObjectName("captureReplayButton");
    rightBox->addWidget(captures);
    connect(captures, &QPushButton::clicked, this, &KLocalWindow::openCaptures);
    rightBox->addWidget(settings);
    bar->addLayout(rightBox);
    root->addLayout(bar);

    /* 标题与板端时间是覆盖层：水平中心对齐到波形 2×2 网格的中心（两列之间的
     * 分隔线），侧栏展开/收起时随网格一起移动。板端时间是世界时钟，始终运行；
     * 模型窗口时间显示在「边缘模型检测」区块内。 */
    m_title = new QLabel(QStringLiteral("设备智能运维 本地看板"), central);
    m_title->setObjectName(QStringLiteral("dashboardTitle"));
    QFont tf = m_title->font();
    tf.setBold(true);
    tf.setPointSize(16);
    m_title->setFont(tf);
    m_title->setStyleSheet(QStringLiteral("color: #e6edf3; background: transparent;"));
    m_timeLabel = new QLabel(QStringLiteral("时间：—"), central);
    m_timeLabel->setObjectName(QStringLiteral("boardTime"));
    m_timeLabel->setToolTip(QStringLiteral("板端本地时间（世界时钟），不随模型回放暂停或停止"));
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
    m_panels[mlk::kCurrent]->setZeroButtonVisible(true);
    connect(m_panels[mlk::kCurrent], &SensorPanel::zeroRequested,
            this, &KLocalWindow::zeroCurrent);
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
     * 滚动区，模式切换与状态文字换行都不再改变行高与波形大小。 */
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

    /* 上 block：故障诊断。实时诊断与数据集评估各自带页内滚动，外层只负责把
     * 两个 block 一起滚动，不参与行高计算。 */
    m_faultPanel = new FaultPanel(aiBody);
    aiBodyLayout->addWidget(m_faultPanel);

    /* 下 block：寿命预测。模型指标与回放控制同属该 block，紧跟上 block。 */
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
    m_replayPanel = new mlk::ReplayPanel(true, rulBox);
    rul->addWidget(m_replayPanel);
    aiBodyLayout->addWidget(rulBox);
    /* 内容比视口矮时贴顶排列，不把两个 block 拉开。 */
    aiBodyLayout->addStretch(1);
    aiScroll->setWidget(aiBody);
    ai->addWidget(aiScroll, 1);
    /* 与上方传感器两列等宽：分隔线与温度/电流、振动/转速之间的竖线对齐。 */
    bottom->addWidget(aiBox, 1);

    auto *metrics = new QGroupBox(QStringLiteral("指标"), central);
    auto *mg = new QGridLayout(metrics);
    mg->setContentsMargins(10, 10, 10, 10);
    mg->setHorizontalSpacing(18);
    mg->setVerticalSpacing(10);
    m_statLatency = new QLabel(QStringLiteral("读取周期：—"), metrics);
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
    mkMetric(0, 0, QStringLiteral("读取周期"), m_statLatency);
    mkMetric(0, 2, QStringLiteral("读取耗时"), m_statRead);
    mkMetric(0, 4, QStringLiteral("UI 刷新"), m_statRate);
    mkMetric(1, 0, QStringLiteral("累计帧"), m_statPkts);
    mkMetric(1, 2, QStringLiteral("丢帧"), m_statLost);
    mkMetric(1, 4, QStringLiteral("运行"), m_statUptime);
    mkMetric(2, 0, QStringLiteral("显示"), m_statVdma);
    mkMetric(2, 2, QStringLiteral("DMA 块"), m_statDmaBlocks);
    mkMetric(2, 4, QStringLiteral("DMA 丢块"), m_statDmaLost);
    mkMetric(3, 0, QStringLiteral("DMA 延迟"), m_statDmaAge);
    setMetric(m_statVdma, QStringLiteral("HDMI 1080P"));
    /* 数据来源：PL 特征摘要原先的位置，字体与其它指标一致；只显示第一段
     * （如 /dev/pl_ctrl），完整 availability 文本放 tooltip。 */
    m_srcText = new QLabel(QStringLiteral("来源：—"), metrics);
    m_srcText->setObjectName(QStringLiteral("sourceText"));
    m_srcText->setTextFormat(Qt::PlainText);
    m_srcText->setWordWrap(false);
    m_srcText->setProperty("caption", QStringLiteral("数据来源"));
    m_srcText->setToolTip(QStringLiteral("数据来源"));
    mg->addWidget(m_srcText, 3, 2, 1, 2);
    /* PL 特征摘要：在指标列之后单独一列，逐行列出 RMS / FFT / 功率。 */
    m_featureText = new QLabel(featureNoteHtml(QStringLiteral("—")), metrics);
    m_featureText->setTextFormat(Qt::RichText);
    m_featureText->setWordWrap(false);
    m_featureText->setObjectName(QStringLiteral("featureSummary"));
    m_featureText->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_featureText->setContentsMargins(14, 0, 0, 0);
    m_featureText->setProperty("caption", QStringLiteral("PL 特征摘要"));
    mg->addWidget(m_featureText, 0, 6, 4, 1);
    mg->setColumnMinimumWidth(6, 170);
    /* 四个指标行均分竖直余量：行距不再挤在顶部，框底不留大片空白。 */
    for (int r = 0; r < 4; ++r) mg->setRowStretch(r, 1);
    bottom->addWidget(metrics, 1);
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
            if (m_vk)
                m_vk->raise();
        }
        QTimer::singleShot(0, this, &KLocalWindow::updateHeaderPosition);
    });

    m_uiTimer = new QTimer(this);
    connect(m_uiTimer, &QTimer::timeout, this, [this] {
        for (int i = 0; i < mlk::kChannelCount; ++i)
            m_panels[i]->refreshUi();
    });
    m_alarm = new AlarmController(this);
    connect(m_alarm,&AlarmController::hardwareStatus,this,[this](const QString &text) {
        m_alarmStatus=text;
        if (m_srcText) m_srcText->setToolTip(text);
        qInfo("%s",qPrintable(text));
    });
    /* 实时故障诊断：内置工作线程与外部 API 2 发布者共用一个控制器，模式互斥。 */
    m_faultLive = new mlk::FaultLiveController(this);
    connect(m_faultLive, &mlk::FaultLiveController::stateChanged, this,
            &KLocalWindow::updateFaultPanel);
    connect(m_faultLive, &mlk::FaultLiveController::resultPublished, this,
            [this](const mlk::FaultResult &result, qint64 ageMs) {
                m_faultRecordPolicy.observe(result, m_alarm && (m_alarm->desiredState() & 0x0fu));
                pruneNormalRecordings();
                if (m_faultPanel)
                    m_faultPanel->showResult(result, ageMs, m_faultLive->confirmed(),
                                             m_faultLive->externalPublisher());
                updateCompatModelState(result);
                if (m_faultModeOn) publishFaultStatus();
            });
    connect(m_faultLive, &mlk::FaultLiveController::confirmedEvent, this,
            &KLocalWindow::applyFaultEvent);
    connect(m_faultLive, &mlk::FaultLiveController::confirmedCleared, this, [this] {
        m_faultAlarmConfirmed = false;
        if (m_alarm) m_alarm->modelGrade(0);
        if (m_faultModeOn) publishFaultStatus();
    });
    connect(m_faultLive, &mlk::FaultLiveController::journal, this,
            [this](const QJsonObject &record) {
                QString error;
                if (!m_faultJournal.append(record, m_cfg.storageDir, m_cfg.passphrase,
                                                m_cfg.retentionMonths,
                                                qint64(m_cfg.minFreeMb), &error))
                    qWarning("fault journal: %s", qPrintable(error));
            });
    connect(m_faultPanel, &FaultPanel::startRequested, this, &KLocalWindow::startFaultDiagnosis);
    connect(m_faultPanel, &FaultPanel::stopRequested, this, &KLocalWindow::stopFaultDiagnosis);

    /* 数据集评估：只读运行包与评估集，不参与实时告警链路。 */
    m_faultEval = new mlk::FaultEvalController(this);
    connect(m_faultPanel, &FaultPanel::evalStartRequested, this,
            &KLocalWindow::startFaultEvaluation);
    connect(m_faultPanel, &FaultPanel::evalStopRequested, this,
            &KLocalWindow::stopFaultEvaluation);
    connect(m_faultEval, &mlk::FaultEvalController::stateChanged, this,
            [this](const QString &detail) {
                if (!m_faultPanel || !m_faultPanel->evalPanel()) return;
                m_faultPanel->evalPanel()->showState(detail);
                m_faultPanel->evalPanel()->setBusy(m_faultEval && m_faultEval->running());
            });
    connect(m_faultEval, &mlk::FaultEvalController::progress, this,
            [this](int done, int total) {
                if (m_faultPanel && m_faultPanel->evalPanel())
                    m_faultPanel->evalPanel()->showProgress(done, total);
            });
    connect(m_faultEval, &mlk::FaultEvalController::finished, this,
            [this](const mlk::FaultEvalReport &report) {
                if (!m_faultPanel || !m_faultPanel->evalPanel()) return;
                auto *panel = m_faultPanel->evalPanel();
                panel->setBusy(false);
                panel->showProgress(report.evaluated, report.evaluated);
                panel->showReport(report);
                panel->showState(QStringLiteral("完成：%1 窗，类别 %2，类别+等级 %3")
                                     .arg(report.evaluated)
                                     .arg(QString::number(report.classAccuracy() * 100.0, 'f', 2))
                                     .arg(QString::number(report.jointAccuracy() * 100.0, 'f', 2)));
                showToast(QStringLiteral("数据集评估完成：类别准确率 %1%")
                              .arg(QString::number(report.classAccuracy() * 100.0, 'f', 2)), 6000);
            });
    connect(m_faultEval, &mlk::FaultEvalController::failed, this, [this](const QString &error) {
        if (!m_faultPanel || !m_faultPanel->evalPanel()) return;
        m_faultPanel->evalPanel()->setBusy(false);
        m_faultPanel->evalPanel()->showState(error);
        showToast(QStringLiteral("数据集评估失败：%1").arg(error), 6000);
    });
    {
        QSettings store;
        const auto root = QString::fromLocal8Bit(qgetenv("MLK_FAULT_ROOT"));
        const auto directory = root.isEmpty()
            ? store.value(QStringLiteral("fault/modelDirectory"),
                          QStringLiteral("/opt/mlk_fault")).toString() : root;
        m_faultPanel->setDirectory(directory);
        m_faultPanel->setDiscardNormalRecordings(store.value(QStringLiteral("fault/discardNormalRecordings"),false).toBool());
        m_faultPanel->setBackend(store.value(QStringLiteral("fault/backend"),
                                             int(mlk::kFaultBackendCpuCnnNpu)).toInt());
        if (m_faultPanel->evalPanel()) {
            auto *panel = m_faultPanel->evalPanel();
            panel->setPackageDirectory(directory);
            const auto dataset = QString::fromLocal8Bit(qgetenv("MLK_FAULT_EVAL_ROOT"));
            panel->setDatasetDirectory(dataset.isEmpty()
                ? store.value(QStringLiteral("fault/evalDirectory")).toString() : dataset);
            panel->setBackend(store.value(QStringLiteral("fault/evalBackend"),
                                          int(mlk::kFaultBackendCpuCnnNpu)).toInt());
        }
    }    applyConfig();
    m_uiTimer->start(qMax(33, 1000 / qMax(1, m_cfg.uiFps)));

    m_toast = new QLabel(this);
    m_toast->setAlignment(Qt::AlignCenter);
    m_toast->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_toast->setStyleSheet(QStringLiteral(
        "QLabel { background: #1c2028; color: #e6edf3; border: 1px solid #30363d;"
        " border-radius: 10px; padding: 10px 22px; font-size: 14px; }"));
    m_toast->hide();
    m_toastTimer = new QTimer(this);
    m_toastTimer->setSingleShot(true);
    connect(m_toastTimer, &QTimer::timeout, m_toast, &QLabel::hide);

    auto *drv = new mlk::DriverSource(this);
    m_src = drv;
    m_driver = drv;
    drv->configureCurrent(m_cfg.currentZero, m_cfg.currentGain);
    connect(m_src, &mlk::DataSource::frameReceived, this, &KLocalWindow::onFrame);
    connect(m_src, &mlk::DataSource::secureReady, this, [this] {
        showToast(QStringLiteral("加密会话已建立"), 4000);
    });
    connect(m_src, &mlk::DataSource::availabilityChanged, this, [this](const QString &text) {
        /* 数据来源中文名只留第一段（如 /dev/pl_ctrl），块统计放到指标框；
         * 完整 availability 文本放 tooltip。 */
        if (!m_srcText)
            return;
        m_srcText->setText(QStringLiteral("来源：%1")
                               .arg(text.section(QStringLiteral(" · "), 0, 0)));
        m_srcText->setToolTip(text);
    });
    /* PL 频谱窗口（§4.6）的 FFT 输入端只接 X 轴（fpga/uisrc/01_rtl/soc/sensor_soc_axi.v
     * 的 .I_sample(S_raw[63:48])），因此不再把它当作振动频谱显示。振动频谱改由
     * PlotWidget 对时域 X/Y/Z 三轴与合成曲线分别做 FFT，四色曲线与时域一致。 */
    connect(m_src, &mlk::DataSource::timingUpdated, this, &KLocalWindow::onTiming);
    connect(drv, &mlk::DriverSource::currentCalibrationChanged, this,
            [this](qint32 zero, qint32 gain) {
                m_panels[mlk::kCurrent]->setToolTip(
                    gain > 0 ? QStringLiteral("零点 %1 LSB · 增益 %2 µA/LSB").arg(zero).arg(gain)
                             : QStringLiteral("零点 %1 LSB · 电流未标定")
                                   .arg(zero));
            });
    connect(drv, &mlk::DriverSource::currentHealthChanged, this,
            [this](bool ok, const QString &detail) {
                if (!ok) {
                    m_alarm->invalidate(mlk::kCurrent);
                    m_panels[mlk::kCurrent]->setUnavailable(detail);
                }
            });
    connect(drv, &mlk::DriverSource::dmaStatsChanged, this,
            [this](quint64 blocks, quint64 lostBlocks, bool overflow, double ageUs) {
                setMetric(m_statDmaBlocks, QString::number(blocks));
                setMetric(m_statDmaLost,
                          overflow ? QStringLiteral("%1（溢出）").arg(lostBlocks)
                                   : QString::number(lostBlocks));
                setMetric(m_statDmaAge, QStringLiteral("%1 ms").arg(ageUs / 1000.0, 0, 'f', 2));
            });
    /* 转速（接口 v1.2）：寄存器轮询与 DMA 转速记录在 DriverSource 里已按 PL
     * 事件时间戳去重，这里只负责方向、量化与可用性显示。 */
    connect(drv, &mlk::DriverSource::speedChanged, this, &KLocalWindow::onSpeedSample);
    connect(drv, &mlk::DriverSource::speedAvailabilityChanged, this,
            &KLocalWindow::onSpeedAvailability);
    if (m_srcText) {
        m_srcText->setText(QStringLiteral("来源：%1").arg(m_src->name()));
        m_srcText->setToolTip(m_src->name());
    }
    // start() 会同步发出 availabilityChanged，必须放在连接之后，否则采集方式停在初值。
    drv->start();

    m_sys = new mlk::SystemMonitor(this);
    connect(m_sys, &mlk::SystemMonitor::statsUpdated, this, &KLocalWindow::onSystemStats);
    m_sys->start();

    connect(settings, &QPushButton::clicked, this, &KLocalWindow::openSettings);
    m_recordTimer = new QTimer(this);
    m_recordTimer->setInterval(qMax(1, m_cfg.recordSegmentSec) * 1000);
    connect(m_recordTimer, &QTimer::timeout, this, &KLocalWindow::saveSnapshot);
    connect(m_recordBtn, &QPushButton::toggled, this, [this](bool on) {
        if (on) {
            QString error;
            if (!m_captureRecorder.empty() && !m_captureRecorder.flush(nullptr,&error)) {
                QSignalBlocker blocker(m_recordBtn); m_recordBtn->setChecked(false);
                showToast(QStringLiteral("待保存记录写入失败：%1").arg(error),8000); return;
            }
            m_captureRecorder.begin(m_cfg,"ps-local");
            m_recording=true; m_recordTimer->start(qMax(1,m_cfg.recordSegmentSec)*1000);
        } else {
            m_recordTimer->stop(); saveSnapshot(); m_recording=false;
        }
        m_recordBtn->setText(on?QStringLiteral("记录中"):QStringLiteral("未记录"));
    });
    auto *openSc = new QShortcut(QKeySequence(Qt::Key_F2), this);
    connect(openSc, &QShortcut::activated, this, &KLocalWindow::openSettings);

    m_statTimer = new QTimer(this);
    m_statTimer->setInterval(500);
    connect(m_statTimer, &QTimer::timeout, this, &KLocalWindow::onTick);
    m_statTimer->start();

    m_featureTimer = new QTimer(this);
    m_featureTimer->setInterval(1000);
    connect(m_featureTimer, &QTimer::timeout, this, &KLocalWindow::updatePlFeatures);
    m_featureTimer->start();

    /* 板端时间：世界时钟，1 s 刷新一次，不随模型回放暂停或停止。 */
    m_timeTimer = new QTimer(this);
    m_timeTimer->setInterval(1000);
    connect(m_timeTimer, &QTimer::timeout, this, &KLocalWindow::updateBoardTime);
    updateBoardTime();
    m_timeTimer->start();

    /* 网络校时：后台每 10 s 检测网络时间源，可用后立即步进；手动按钮与
     * 「配置」中的设置时间共用同一服务。 */
    m_boardTime = new mlk::BoardTime(this);
    connect(m_boardTime, &mlk::BoardTime::stateChanged, this,
            [this, sync](bool network, const QString &detail) {
                Q_UNUSED(network);
                sync->setToolTip(QStringLiteral("%1；点击立即校时").arg(detail));
            });
    connect(m_boardTime, &mlk::BoardTime::syncFinished, this,
            [this](bool ok, const QString &message) {
                showToast(message, ok ? 4000 : 6000);
            });
    connect(sync, &QPushButton::clicked, this, [this] {
        showToast(m_boardTime->networkTimeAvailable()
                      ? QStringLiteral("正在按网络时间校准…")
                      : QStringLiteral("正在检测网络时间源…"), 2000);
        m_boardTime->syncNow();
    });
    m_boardTime->start();

    m_vk = new VirtualKeyboard(nullptr);
    QSettings settingsStore;
    const auto modelDirectory = QString::fromLocal8Bit(qgetenv("MLK_RUL_ROOT"));
    m_replayPanel->setDirectory(modelDirectory.isEmpty()
        ? settingsStore.value(QStringLiteral("rul/modelDirectory"), QStringLiteral("/opt/mlk_rul")).toString()
        : modelDirectory);
    m_replay = new mlk::ReplayController(this);
    m_replayRx = new mlk::Receiver(this);
    m_discovery = new mlk::PeerDiscovery(mlk::PeerDiscovery::Board, this);
    connect(m_discovery, &mlk::PeerDiscovery::peerDiscovered, this,
        [this](const QHostAddress &address, quint16 port) {
            m_cfg.targetAddr = address.toString(); m_cfg.targetPort = port;
            m_cfg.save(); setupSecure();
            showToast(QStringLiteral("已发现配对上位机 %1:%2").arg(address.toString()).arg(port), 3000);
        });
    connect(m_replayRx, &mlk::Receiver::frameReceivedFrom, this,
        [this](const mlk::Frame &frame, const QHostAddress &, quint16) {
            if (frame.type != mlk::kTypeConfig && frame.type != mlk::kTypeReplayControl
                && frame.type != mlk::kTypeOrderAck) return;
            m_remoteSeen.restart();
            updateRemoteConnectionStatus();
        });
    auto *ordersTimer = new QTimer(this);
    connect(ordersTimer, &QTimer::timeout, this, &KLocalWindow::retryOrders);
    m_orderOutbox = new mlk::WorkOrderOutbox(this);
    m_orderOutbox->configure(m_cfg.storageDir, m_cfg.passphrase);
    connect(m_orderOutbox, &mlk::WorkOrderOutbox::orderReady, this, [this](const QJsonObject &order) {
        if (m_sec.mode() != mlk::SecureChannel::Off && m_sec.ready())
            sendPacket(mlk::buildObjectFrame(mlk::kTypeWorkOrder, order));
    });
    ordersTimer->start(2000);
    connect(m_replayRx, &mlk::Receiver::frameReceivedFrom, this, &KLocalWindow::handleControl);
    setupSecure();
    connect(m_replayPanel, &mlk::ReplayPanel::requested, this,
        [this](quint8 action, quint8 backend, quint32 index, quint32 interval) {
            if (faultPublisherActive()) {
                m_replayPanel->showError(QStringLiteral("实时故障诊断进行中，先停止再回放"));
                return;
            }
            if (!m_replay->setRoot(m_replayPanel->directory())) {
                m_replayPanel->showError(QStringLiteral("先停止回放，再修改模型目录")); return;
            }
            QSettings().setValue(QStringLiteral("rul/modelDirectory"), m_replayPanel->directory());
            const auto error = m_replay->control(action, backend, index, interval);
            if (!error.isEmpty()) m_replayPanel->showError(error);
        });
    connect(m_replay, &mlk::ReplayController::changed, this, [this](const mlk::Frame &f) {
        /* 回放只由面板按钮或远端指令启动；本窗口不再在开机时自动开始或自动续播。 */
        handleDiagnosis(f); m_replayPanel->showFrame(f); sendInference(f);
    });
    connect(m_replay, &mlk::ReplayController::resultReady, this, [this](const mlk::Frame &f) {
        emit m_replay->recordRequested(mlk::buildInferenceFrame(f), m_cfg.storageDir, m_cfg.encrypt,
            m_cfg.passphrase, m_cfg.retentionMonths, m_cfg.minFreeMb);
    });
    connect(m_replay, &mlk::ReplayController::recordFailed, this, [this](const QString &error) {
        m_replayPanel->showError(QStringLiteral("寿命记录失败：%1").arg(error));
    });
    connect(m_replayRx, &mlk::Receiver::frameReceivedFrom, this,
        [this](const mlk::Frame &f, const QHostAddress &address, quint16 port) {
            if (f.type != mlk::kTypeReplayControl) return;
            m_replayPeer = address; m_replayPort = port;
            const auto key = address.toString() + QLatin1Char(':') + QString::number(port)
                           + QLatin1Char(':') + QString::number(f.requestId);
            if (m_replies.contains(key)) {
                const auto cached = m_replies.value(key);
                auto reply = m_replay->snapshot(); reply.requestId = cached.requestId;
                reply.commandStatus = cached.commandStatus;
                if (cached.commandStatus == 2) reply.message = cached.message;
                m_replayRx->send(address, port, mlk::buildInferenceFrame(reply)); return;
            }
            QString error;
            if (faultPublisherActive())
                error = QStringLiteral("实时故障诊断进行中，不能启动寿命回放");
            else if (!m_replay->setRoot(m_replayPanel->directory())) error = QStringLiteral("模型目录在运行中不可修改");
            else error = m_replay->control(f.action, f.backend, f.windowIndex, f.intervalMs);
            auto reply = m_replay->snapshot(); reply.requestId = f.requestId;
            reply.commandStatus = error.isEmpty() ? 1 : 2;
            if (!error.isEmpty()) reply.message = error;
            m_replies.insert(key, reply); m_replyOrder.enqueue(key);
            if (m_replyOrder.size() > 64) m_replies.remove(m_replyOrder.dequeue());
            m_replayRx->send(address, port, mlk::buildInferenceFrame(reply));
        });
    /* 寿命模型不再开机自动回放：模型目录只做缺省值，回放由「开始/继续」按钮
     * 或远端回放指令启动，避免上电即在无人操作时占用 NPU 与 CMA。 */
    m_modelState=mlk::unknownModelFault();
    m_faultServer=new ModelFaultServer([this](const QJsonObject &o) {
        /* api=2 走独立校验器；api=1 保持既有语义。同一实例长度上限放宽到 4096 B。 */
        if (o.value("api").toDouble(0) >= 2) return reportDiagnosisResult(o);
        const bool ok=reportModelState(o);
        return QJsonObject{{"ok",ok},{"error",ok ? "" : "invalid state or work-order persistence failed"}};
    },this,4096);
    const auto apiPath=qEnvironmentVariable("MLK_FAULT_SOCKET",QStringLiteral("/run/mlk-fault.sock"));
    if (!m_faultServer->start(apiPath)) qWarning("model fault API unavailable: %s",qPrintable(m_faultServer->errorString()));
    m_captureApi=new ModelFaultServer([this](const QJsonObject &o) {
        return mlk::captureRequest(m_cfg,o);
    },this);
    if (!m_captureApi->start(qEnvironmentVariable("MLK_CAPTURE_PS_SOCKET","/run/mlk-capture.sock")))
        qWarning("capture API unavailable: %s",qPrintable(m_captureApi->errorString()));
    auto *modelTimer=new QTimer(this);
    modelTimer->setInterval(250);
    connect(modelTimer,&QTimer::timeout,this,[this] {
        /* 旧十秒 watchdog 只处理 v1 发布者；API 2 的时效由控制器按窗口判定。 */
        if (faultPublisherActive()) return;
        if (m_modelSeen.isValid() && m_modelSeen.elapsed()>10000) {
            m_modelSeen.invalidate(); m_modelState=mlk::unknownModelFault();
            m_alarm->modelGrade(0); updateDiagnosisUi();
        }
    });
    modelTimer->start();
    m_diag.ver = mlk::kInferenceVersion; m_diagValid = true; updateDiagnosisUi();
    m_uptime.start();
    updateHeaderPosition();
    updateSidebarGeometry();
    QTimer::singleShot(0,this,[this] { updateHeaderPosition(); updateSidebarGeometry(); });
}

void KLocalWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    updateHeaderPosition();
    updateSidebarGeometry();
}

void KLocalWindow::updateSidebarGeometry()
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

/* 板端本地时间：标题下方的世界时钟，与模型回放状态无关。模型窗口时间在
 * 「边缘模型检测」区块内单独显示，暂停或停止时定格。 */
void KLocalWindow::updateBoardTime()
{
    if (!m_timeLabel)
        return;
    m_timeLabel->setText(QStringLiteral("时间：%1").arg(
        QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
    updateHeaderPosition();
}

void KLocalWindow::updateHeaderPosition()
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
    if (m_toast && m_toast->isVisible())
        m_toast->move((centralWidget()->width() - m_toast->width()) / 2,
                      (centralWidget()->height() - m_toast->height()) / 2);
}

KLocalWindow::~KLocalWindow()
{
    cancelFaultStart();
    delete m_faultServer; m_faultServer=nullptr;
    delete m_faultEval; m_faultEval=nullptr;
    /* 先停故障工作线程并释放引擎，再撤销告警与 RUL 资源。 */
    delete m_faultLive; m_faultLive=nullptr;
    delete m_alarm; m_alarm=nullptr;
    delete m_captureApi; m_captureApi=nullptr;
    if (!m_captureRecorder.empty()) saveSnapshot();
    delete m_replay; // Drain the active forward and release inference resources first.
    m_replay = nullptr;
    if (m_src)
        m_src->stop();
    delete m_vk;
    if (m_featureFd >= 0)
        pl_close(m_featureFd);
}

void KLocalWindow::zeroCurrent()
{
    if (!m_driver) {
        showToast(QStringLiteral("归零失败：数据源未就绪"), 5000);
        return;
    }
    QString error;
    if (!m_driver->zeroCurrent(&error)) {
        showToast(error.isEmpty() ? QStringLiteral("归零失败") : error, 5000);
        return;
    }
    QString text = QStringLiteral("电流已归零（零点 %1 LSB）").arg(m_driver->currentZero());
    if (m_faultLive && m_faultLive->busy()) {
        m_faultLive->stop();
        m_faultAlarmConfirmed = false;
        if (m_alarm) m_alarm->modelGrade(0);
        text += QStringLiteral("；实时故障诊断已停止，请重新开始");
    }
    m_cfg.currentZero = m_driver->currentZero();
    m_cfg.currentGain = m_driver->currentGain();
    ++m_cfg.sharedRevision;
    if (!m_cfg.save()) text += QStringLiteral("；零点保存失败，重启后需重新归零");
    showToast(text, 5000);
    m_panels[mlk::kCurrent]->refreshUi();
}

void KLocalWindow::showToast(const QString &text, int ms)
{
    if (!m_toast)
        return;
    m_toast->setText(text);
    m_toast->adjustSize();
    QWidget *area = centralWidget();
    m_toast->move((area->width() - m_toast->width()) / 2,
                  (area->height() - m_toast->height()) / 2);
    m_toast->show();
    m_toast->raise();
    if (m_vk)
        m_vk->raise();
    m_toastTimer->start(ms);
}

void KLocalWindow::applyConfig()
{
    if (m_orderOutbox) m_orderOutbox->configure(m_cfg.storageDir, m_cfg.passphrase);
    /* 标度或电流参数变化会改变模型口径：先停止实时诊断并撤销有效位。 */
    const QString previousScale = m_faultInput.scaleId();
    m_faultInput.configure(m_cfg);
    if (!previousScale.isEmpty() && previousScale != m_faultInput.scaleId()
        && m_faultLive && m_faultLive->busy()) {
        m_faultLive->stop();
        m_faultAlarmConfirmed = false;
        if (m_alarm) m_alarm->modelGrade(0);
        showToast(QStringLiteral("标度或电流参数变化，实时故障诊断已停止"), 6000);
    }
    if (m_driver) m_driver->configureCurrent(m_cfg.currentZero, m_cfg.currentGain);
    if (m_recording && !m_captureRecorder.configMatches(m_cfg)) {
        saveSnapshot();
        if (m_captureRecorder.empty()) m_captureRecorder.begin(m_cfg,"ps-local");
        else m_recordBtn->setChecked(false);
    }
    if (m_alarm) m_alarm->configure(m_cfg);
    if (m_uiTimer) {
        const int interval = qMax(33, 1000 / qMax(1, m_cfg.uiFps));
        if (m_uiTimer->interval() != interval)
            m_uiTimer->setInterval(interval);
    }
    setMetric(m_statRate, QStringLiteral("%1 fps").arg(m_cfg.uiFps));
    if (m_recordTimer) {
        const int ms = qMax(1, m_cfg.recordSegmentSec) * 1000;
        if (m_recordTimer->interval()!=ms)
            m_recordTimer->setInterval(ms);
        m_recordBtn->setToolTip(QStringLiteral("按 %1 s 分段加密保存四传感器全部接收样本")
                                    .arg(qMax(1, m_cfg.recordSegmentSec)));
    }
    for (int i = 0; i < mlk::kChannelCount; ++i) {
        /* 电流帧已由 DriverSource 换算成 mA，面板不再乘设置页系数；转速面板
         * 使用 signed32、0.1 rpm/LSB，与 UDP 和记录保持一致。 */
        if (i == mlk::kCurrent)
            m_panels[i]->setScale(1.0, 0.0);
        else if (i == mlk::kSpeed)
            m_panels[i]->setScale(0.1, 0.0);
        else
            m_panels[i]->setScale(m_cfg.ch[i].k, m_cfg.ch[i].b);
        m_panels[i]->setAlarm(m_cfg.ch[i].alarmLo, m_cfg.ch[i].alarmHi, m_cfg.ch[i].alarmEnabled);
        m_panels[i]->setFftPoints(m_cfg.fftPoints);
    }

}

void KLocalWindow::setupSecure()
{
    m_remoteSeen.invalidate();
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
        m_discovery->configure(m_sec, quint16(m_cfg.port), m_cfg.autoDiscover,
                               QHostAddress(m_cfg.targetAddr), quint16(m_cfg.targetPort));
    if (auto *udp = qobject_cast<mlk::UdpSource *>(m_src))
        udp->setChannel(m_cfg.secMode ? &m_sec : nullptr);
    if (m_replayRx) {
        m_replayRx->setChannel(m_cfg.secMode ? &m_sec : nullptr);
        m_replayRx->setExpectedPeer(QHostAddress(m_cfg.targetAddr),quint16(m_cfg.targetPort));
        m_replies.clear(); m_replyOrder.clear(); m_replayPeer.clear(); m_replayPort = 0;
        if (!m_replayRx->start(QHostAddress(m_cfg.addr), quint16(m_cfg.port)) && m_replayPanel)
            m_replayPanel->showError(QStringLiteral("UDP 监听失败：%1").arg(m_replayRx->errorString()));
    }
    updateRemoteConnectionStatus();
}

void KLocalWindow::sendInference(const mlk::Frame &frame)
{
    if (!m_replayRx || !m_replayRx->isListening()) return;
    const QHostAddress target = m_replayPeer.isNull() ? QHostAddress(m_cfg.targetAddr) : m_replayPeer;
    const quint16 port = m_replayPort ? m_replayPort : quint16(m_cfg.targetPort);
    if (!target.isNull()) m_replayRx->send(target, port, mlk::buildInferenceFrame(frame));
}

void KLocalWindow::openSettings()
{
    QDialog passDlg(this);
    passDlg.setWindowTitle(QStringLiteral("配置验证"));
    passDlg.setWindowModality(Qt::WindowModal);
    auto *lay = new QVBoxLayout(&passDlg);
    auto *edit = new QLineEdit(&passDlg);
    edit->setEchoMode(QLineEdit::Password);
    edit->setMinimumWidth(280);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &passDlg);
    bb->button(QDialogButtonBox::Ok)->setDefault(true);
    connect(bb, &QDialogButtonBox::accepted, &passDlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &passDlg, &QDialog::reject);
    lay->addWidget(new QLabel(QStringLiteral("配置密码："), &passDlg));
    lay->addWidget(edit);
    lay->addWidget(bb);
    edit->setFocus();
    passDlg.show();
    QEventLoop passLoop;
    connect(&passDlg, &QDialog::accepted, &passLoop, &QEventLoop::quit);
    connect(&passDlg, &QDialog::rejected, &passLoop, &QEventLoop::quit);
    passLoop.exec();
    passDlg.hide();
    if (m_vk)
        m_vk->hideKeyboard();
    if (passDlg.result() != QDialog::Accepted)
        return;
    if (edit->text() != m_cfg.settingsPassword) {
        QMessageBox::warning(this, QStringLiteral("配置验证"), QStringLiteral("密码错误"));
        return;
    }
    const auto previous = m_cfg;
    auto draft = m_cfg;
    PlSettingsDialog dlg(draft, this, false,
                         m_boardTime && m_boardTime->networkTimeAvailable());
    connect(&dlg, &PlSettingsDialog::recordsRemoved, this, [this] {
        if (m_captureDialog) m_captureDialog->refresh();
    });
    connect(&dlg, &PlSettingsDialog::timeSetRequested, this, [this](const QDateTime &time) {
        QString error;
        if (!m_boardTime || !m_boardTime->setDateTime(time, &error))
            showToast(error.isEmpty() ? QStringLiteral("设置板端时间失败") : error, 6000);
        else
            showToast(QStringLiteral("板端时间已设置为 %1")
                          .arg(time.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))), 5000);
    });
    connect(&dlg, &PlSettingsDialog::rebootRequested, this, [this] {
        QString error;
        if (!m_boardTime || !m_boardTime->reboot(&error))
            showToast(error.isEmpty() ? QStringLiteral("重启失败") : error, 6000);
        else
            showToast(QStringLiteral("正在重启核心板…"), 8000);
    });
    dlg.setWindowModality(Qt::WindowModal);
    dlg.show();
    QEventLoop settingsLoop;
    connect(&dlg, &QDialog::accepted, &settingsLoop, &QEventLoop::quit);
    connect(&dlg, &QDialog::rejected, &settingsLoop, &QEventLoop::quit);
    settingsLoop.exec();
    dlg.hide();
    if (m_vk)
        m_vk->hideKeyboard();
    if (dlg.result() == QDialog::Accepted) {
        if (m_cfg.sharedRevision != previous.sharedRevision) {
            // A remote write may have committed while the modal dialog was open.
            for (int i=0;i<4;++i) draft.ch[i] = m_cfg.ch[i];
            draft.currentZero = m_cfg.currentZero; draft.currentGain = m_cfg.currentGain;
            draft.sharedRevision = m_cfg.sharedRevision;
            showToast(QStringLiteral("共享参数已由远端更新；已保留板端最新值"), 5000);
        }
        if (!mlk::sameSharedValues(draft, m_cfg)) draft.sharedRevision = m_cfg.sharedRevision + 1;
        if (!draft.save()) { showToast(QStringLiteral("配置保存失败"),5000); return; }
        m_cfg = draft;
        const bool transportChanged = previous.autoDiscover != m_cfg.autoDiscover || previous.secMode != m_cfg.secMode || previous.pskHex != m_cfg.pskHex
            || previous.selfPubHex != m_cfg.selfPubHex || previous.selfPrivHex != m_cfg.selfPrivHex
            || previous.peerPubHex != m_cfg.peerPubHex || previous.addr != m_cfg.addr || previous.port != m_cfg.port
            || previous.targetAddr != m_cfg.targetAddr || previous.targetPort != m_cfg.targetPort;
        applyConfig();
        if (transportChanged) setupSecure();
        auto snapshot = mlk::sharedConfig(m_cfg); snapshot.insert("op", "snapshot");
        sendPacket(mlk::buildObjectFrame(mlk::kTypeConfig, snapshot));
    }
}

void KLocalWindow::onFrame(const mlk::Frame &f)
{
    if (f.type == mlk::kTypeWaveform) {
        const auto stamped=mlk::timestampWaveform(f);
        handleWaveform(stamped); sendWaveform(stamped);
        /* 模型分支位于记录落盘与 128 点网络分片之后使用同一个 stamped Frame，
         * 不对同一帧重复换算时间与物理单位。 */
        if (m_faultLive && m_faultLive->running()) {
            const auto block = m_faultInput.acceptFrame(stamped);
            if (block.dataValid) m_faultLive->enqueue(block);
        }
    }
    else if (f.type == mlk::kTypeDiagnosis)
        handleDiagnosis(f);
}

void KLocalWindow::countFrame()
{
    if (m_pkts == std::numeric_limits<quint32>::max())
        m_pkts = 0;
    else
        ++m_pkts;
    ++m_tickPkts;
}

/* 安装方向在应用入口统一反转；本地、UDP、告警与记录共用 0.1 rpm
 * 的 signed32 样本，不在显示链路压缩为 signed16。 */
void KLocalWindow::onSpeedSample(qint32 rpmMilli, quint32 count, quint32 tsTicks,
                                 bool online, bool overflow, bool error, bool fromDma)
{
    if (!online || error)
        return;   /* 错误事件不携带新样本，不能当有效读数 */
    /* 两条路径（寄存器轮询 / DMA 记录）带同一个 PL 事件时间戳：只追加一次。
     * 波形本身按样本索引绘制，PL 时间戳用于去重与提示，即「优先用记录」的
     * 判据由 DriverSource 按来源给出。 */
    if (m_haveSpeedTs && tsTicks == m_speedLastTs)
        return;
    m_haveSpeedTs = true;
    m_speedLastTs = tsTicks;

    const qint32 deci = mlk::displaySpeedDeciRpm(rpmMilli);
    m_panels[mlk::kSpeed]->addSamples({ qint32(deci) }, m_speedRate);
    mlk::Frame speed; speed.ch = mlk::kSpeed; speed.rateHz = quint16(m_speedRate); speed.cnt=count;
    speed.tsUs = quint64(QDateTime::currentMSecsSinceEpoch()) * 1000;
    speed.samples = {qint32(deci)}; m_alarm->observe(speed); recordWaveform(speed); sendWaveform(speed);

    if (m_faultLive && m_faultLive->running()) {
        const auto block = m_faultInput.acceptSpeed(
            deci, count, quint64(QDateTime::currentMSecsSinceEpoch()) * 1000, overflow);
        if (block.dataValid) m_faultLive->enqueue(block);
    }

    QString tip = QStringLiteral("%1 · 样本 %2 · 事件时间戳 %3（10 ns/LSB）")
                      .arg(online ? QStringLiteral("在线") : QStringLiteral("离线"))
                      .arg(count)
                      .arg(tsTicks);
    if (overflow)
        tip += QStringLiteral(" · 换算饱和");
    tip += fromDma ? QStringLiteral(" · 来源 DMA 记录") : QStringLiteral(" · 来源寄存器轮询");
    m_panels[mlk::kSpeed]->setToolTip(tip);
}

/* 离线/超时：显示原因而不是 0 rpm。恢复在线后由下一个样本自动切回；
 * 清掉时间戳记忆，避免恢复后的第一个样本被去重挡掉。 */
void KLocalWindow::onSpeedAvailability(bool available, const QString &reason)
{
    m_haveSpeedTs = false;
    if (available)
        return;
    const QString why = reason.isEmpty() ? QStringLiteral("无有效数据") : reason;
    m_alarm->invalidate(mlk::kSpeed);
    m_panels[mlk::kSpeed]->setUnavailable(why);
    m_panels[mlk::kSpeed]->setToolTip(why);
}

void KLocalWindow::handleWaveform(const mlk::Frame &f)
{
    m_alarm->observe(f);
    const int ch = f.ch;
    if (ch >= 0 && ch < mlk::kChannelCount) {
        if (m_haveSeq[ch] && f.seq > m_lastSeq[ch] + 1)
            m_lost += f.seq - m_lastSeq[ch] - 1;
        m_lastSeq[ch] = f.seq;
        m_haveSeq[ch] = true;
        if (ch == mlk::kVibration && !f.samplesY.isEmpty()
            && f.samplesY.size() == f.samples.size()
            && f.samplesZ.size() == f.samples.size()) {
            m_panels[ch]->addSamples(f.samples, f.samplesY, f.samplesZ, f.rateHz);
        } else {
            m_panels[ch]->addSamples(f.samples, f.rateHz);
        }
        recordWaveform(f);
    }
    if (f.rateHz)
        m_lastRate = f.rateHz;

    countFrame();

    if (f.tsUs) {
        const quint64 now = quint64(QDateTime::currentMSecsSinceEpoch()) * 1000ull;
        if (f.tsUs <= now) {
            const double ms = double(now - f.tsUs) / 1000.0;
            m_latencyMs = m_latencyMs <= 0 ? ms : m_latencyMs * 0.9 + ms * 0.1;
            m_haveLatency = true;
        }
    }
}

void KLocalWindow::handleDiagnosis(const mlk::Frame &f)
{
    /* 内置实时故障模型运行时，旧分类注入不得覆盖当前状态。 */
    if (mlk::hasClassification(f) && !faultPublisherActive()) {
        QJsonObject state{{"api",1},{"valid",true},{"class",int(f.cls)},
            {"grade",int(f.cls ? (f.grade<=3 ? int(f.grade) : 0) : 0)},
            {"id",m_modelState.value("id")},{"faultTimeUs",m_modelState.value("faultTimeUs")},
            {"message",QStringLiteral("分类模型故障事件")}};
        if (f.cls && (m_modelState.value("class").toInt()!=f.cls
            || m_modelState.value("grade").toInt()!=state.value("grade").toInt()
            || !m_modelState.value("valid").toBool())) {
            state.insert("id",QUuid::createUuid().toString(QUuid::WithoutBraces));
            state.insert("faultTimeUs",QString::number(f.tsUs ? f.tsUs : quint64(QDateTime::currentMSecsSinceEpoch())*1000));
        }
        reportModelState(state);
    }
    m_diag = f;
    m_diagValid = true;
    countFrame();
    updateDiagnosisUi();
}

void KLocalWindow::updateDiagnosisUi()
{
    if (!m_diagValid)
        return;
    mlk::showEdgeAi(m_diag, m_diagRul, m_statAi, m_diagBackend, m_diagState, m_diagWindow);
}

void KLocalWindow::onSystemStats(const mlk::SystemStats &stats)
{
    QJsonArray coreValues; for (double core : stats.cpuCores) coreValues.append(core);
    m_boardStatus = {{"cpu",stats.cpuPercent}, {"memory",stats.memPercent}, {"cores",coreValues},
        {"memoryUsedKb",double(stats.memUsedKb)}, {"memoryTotalKb",double(stats.memTotalKb)},
        {"disk",stats.diskPercent}, {"net",stats.netName}, {"netUp",stats.netUp}, {"network",stats.network.toJson()},
        {"rxKbps",stats.rxKbps}, {"txKbps",stats.txKbps}, {"gpu",stats.gpuText}, {"npu",stats.npu.toJson()},
        {"time",QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))},
        {"features",m_featureText->text()}, {"dmaBlocks",m_statDmaBlocks->text()},
        {"dmaLost",m_statDmaLost->text()}, {"dmaAge",m_statDmaAge->text()},
        {"readCycle",m_statLatency->text()}, {"readTime",m_statRead->text()},
        {"faultTimeUs",QString::number(m_faultTimeUs)},
        {"modelFault",mlk::modelFaultSummary(m_modelState)},{"alarm",m_alarmStatus}};
    sendPacket(mlk::buildObjectFrame(mlk::kTypeBoardStatus,m_boardStatus));
    m_cpuSummary->setText(QStringLiteral("CPU: %1%\n内存: %2%")
                              .arg(stats.cpuPercent, 0, 'f', 1)
                              .arg(stats.memPercent, 0, 'f', 1));
    m_cpuText->setText(QStringLiteral("%1%").arg(stats.cpuPercent, 0, 'f', 1));
    m_cpuBar->setValue(qRound(stats.cpuPercent));
    m_cpuPlot->addSamples(0, { qint16(qBound(0, qRound(stats.cpuPercent * 100), 32767)) });
    m_cpuPlot->refresh();
    QStringList cores;
    for (int i = 0; i < stats.cpuCores.size(); ++i)
        cores << QStringLiteral("CPU%1 %2%").arg(i).arg(stats.cpuCores[i], 0, 'f', 0);
    m_cpuCores->setText(cores.join(QStringLiteral("  ")));
    m_statAbs->setText(QStringLiteral("%1% / 1核 %2%")
                           .arg(stats.appCpuPercent, 0, 'f', 1)
                           .arg(stats.appOneCorePercent, 0, 'f', 1));

    m_memText->setText(QStringLiteral("%1 / %2 MiB")
                           .arg(stats.memUsedKb / 1024)
                           .arg(stats.memTotalKb / 1024));
    m_memBar->setValue(qRound(stats.memPercent));
    m_memPlot->addSamples(0, { qint16(qBound(0, qRound(stats.memPercent * 100), 32767)) });
    m_memPlot->refresh();

    if (stats.netName.isEmpty()) {
        m_netText->setText(QStringLiteral("无网络接口"));
    } else {
        m_netText->setText(QStringLiteral("%1 %2  ↓ %3 KB/s  ↑ %4 KB/s")
                               .arg(stats.netName,
                                    stats.netUp ? QStringLiteral("up") : QStringLiteral("down"))
                               .arg(stats.rxKbps, 0, 'f', 1)
                               .arg(stats.txKbps, 0, 'f', 1));
    }

    m_diskText->setText(QStringLiteral("%1 / %2 GiB  (%3%)")
                            .arg(stats.diskUsedBytes / 1024.0 / 1024.0 / 1024.0, 0, 'f', 1)
                            .arg(stats.diskTotalBytes / 1024.0 / 1024.0 / 1024.0, 0, 'f', 1)
                            .arg(stats.diskPercent, 0, 'f', 1));
    m_diskBar->setValue(qRound(stats.diskPercent));
    m_npuPanel->updateStats(stats.npu);
    m_networkStatus->setText(stats.network.text());
    /* 链路刚连上时立即检测网络时间源，不必等下一次 10 s 轮询。 */
    const bool linked = stats.network.kind != QStringLiteral("disconnected");
    if (linked && !m_networkLinked && m_boardTime)
        m_boardTime->refreshNow();
    m_networkLinked = linked;
}

void KLocalWindow::saveSnapshot()
{
    QString path,error;
    if (m_captureRecorder.empty()) return;
    mlk::CaptureEntry entry;
    if (!m_captureRecorder.flush(&path,&error,&entry)) {
        showToast(QStringLiteral("采集保存失败（样本保留待重试）：%1").arg(error),8000); return;
    }
    m_faultRecordPolicy.saved(entry);
    pruneNormalRecordings();
    if (m_captureDialog && m_captureDialog->isVisible()) m_captureDialog->refresh();
    showToast(QStringLiteral("已加密保存四传感器记录：%1").arg(QFileInfo(path).fileName()),5000);
}

void KLocalWindow::pruneNormalRecordings()
{
    if (!m_faultRecordPolicy.hasMatureSegments()) return;
    QJsonArray labels; QString error;
    if (!mlk::CaptureStore(m_faultRecordPolicy.directory(),m_cfg.passphrase).labels(labels,&error)) {
        m_faultRecordPolicy.end();
        showToast(QStringLiteral("标签库不可读，已停止自动清理并保留波形：%1").arg(error),6000);
        return;
    }
    const auto paths=m_faultRecordPolicy.discardable(labels);
    if (paths.isEmpty()) return;
    QStringList failed;
    const int removed=mlk::removeCaptureFiles(m_faultRecordPolicy.directory(),paths,&failed);
    if (!failed.isEmpty()) showToast(QStringLiteral("部分正常分段未能清理，文件已保留"),5000);
    else if (removed) showToast(QStringLiteral("已清理 %1 个正常波形分段").arg(removed),3000);
    if (m_captureDialog && m_captureDialog->isVisible()) m_captureDialog->refresh();
}

void KLocalWindow::recordWaveform(const mlk::Frame &frame) {
    if (!m_recording) return;
    if (m_captureRecorder.full(frame)) saveSnapshot();
    if (!m_captureRecorder.append(frame)) {
        m_recordBtn->setChecked(false);
        showToast(QStringLiteral("记录已停止：样本块无效或待保存缓冲已满"),8000);
    }
}
void KLocalWindow::openCaptures() {
    saveSnapshot();
    if (!m_captureDialog) m_captureDialog=new mlk::CaptureDialog(m_cfg,this);
    else m_captureDialog->configure(m_cfg);
    m_captureDialog->show(); m_captureDialog->raise(); m_captureDialog->activateWindow();
}

void KLocalWindow::updatePlFeatures()
{
    if (m_featureFd < 0)
        m_featureFd = pl_open(nullptr);
    if (m_featureFd < 0) {
        m_featureText->setText(featureNoteHtml(QStringLiteral("不可用")));
        return;
    }
    pl_features_t f = {};
    if (pl_read_features(m_featureFd, &f) != 0) {
        m_featureText->setText(featureNoteHtml(QStringLiteral("不可用")));
        return;
    }
    if (!f.available) {
        m_featureText->setText(featureNoteHtml(QStringLiteral("等待完整窗口")));
        return;
    }
    auto rms = [](const pl_feature_channel_t &ch) { return double(ch.rms) / 65536.0; };
    m_featureText->setText(featureTableHtml(QString::number(rms(f.ch[0]), 'f', 3),
                                            QString::number(rms(f.ch[1]), 'f', 3),
                                            QString::number(rms(f.ch[2]), 'f', 3),
                                            QString::number(f.fft_peak_bin),
                                            QString::number(double(f.fft_peak_power) / 65536.0, 'f', 2)));
}

void KLocalWindow::onTiming(int pollMs, int readUs)
{
    setMetric(m_statLatency, QStringLiteral("%1 ms").arg(pollMs));
    setMetric(m_statRead, QStringLiteral("%1 µs").arg(readUs));
}

void KLocalWindow::onTick()
{
    if (m_discovery) m_discovery->setConnected(m_remoteSeen.isValid() && m_remoteSeen.elapsed() <= 5000);
    updateRemoteConnectionStatus();
    m_tickPkts = 0;
    /* 模型结果年龄独立于网络心跳：过期即撤销告警并显示 N/A。 */
    if (m_faultLive) m_faultLive->tick();
    if (m_faultModeOn && (!m_faultStatusClock.isValid() || m_faultStatusClock.elapsed() >= 1000)) {
        m_faultStatusClock.restart();
        publishFaultStatus();
    }
    setMetric(m_statPkts, QString::number(m_pkts));
    setMetric(m_statLost, QString::number(m_lost));
    const qint64 s = m_uptime.elapsed() / 1000;
    setMetric(m_statUptime, QStringLiteral("%1:%2")
                              .arg(s / 60, 2, 10, QLatin1Char('0'))
                              .arg(s % 60, 2, 10, QLatin1Char('0')));
}

void KLocalWindow::updateRemoteConnectionStatus()
{
    if (!m_remoteConnection) return;
    QString state;
    QString color = QStringLiteral("#8b949e");
    if (!m_replayRx || !m_replayRx->isListening()) state = QStringLiteral("监听不可用");
    else if (m_sec.mode()==mlk::SecureChannel::Ecdh && m_cfg.peerPubHex.isEmpty()) state = QStringLiteral("等待配对");
    else if (!m_remoteSeen.isValid()) state = QStringLiteral("未连接");
    else if (m_remoteSeen.elapsed() > 5000) {
        state = QStringLiteral("连接超时"); color = QStringLiteral("#e6c037");
    } else {
        state = m_sec.mode() == mlk::SecureChannel::Off
            ? QStringLiteral("明文已连接") : QStringLiteral("加密已连接");
        color = QStringLiteral("#37c08a");
    }
    m_remoteConnection->setText(QStringLiteral("上位机：%1").arg(state));
    m_remoteConnection->setStyleSheet(QStringLiteral("font-size: 12px; color: %1;").arg(color));
    m_remoteConnection->setToolTip(QStringLiteral("目标 %1:%2；5 秒未收到目标上位机有效回包即超时")
        .arg(m_cfg.targetAddr).arg(m_cfg.targetPort));
}

void KLocalWindow::sendPacket(const QByteArray &packet)
{
    if (packet.isEmpty() || !m_replayRx) return;
    const QHostAddress target = m_replayPeer.isNull() ? QHostAddress(m_cfg.targetAddr) : m_replayPeer;
    const quint16 port = m_replayPort ? m_replayPort : quint16(m_cfg.targetPort);
    if (!target.isNull()) m_replayRx->send(target, port, packet);
}

void KLocalWindow::sendWaveform(const mlk::Frame &frame)
{
    if (frame.ch >= 4) return;
    const auto stamped=mlk::timestampWaveform(frame);
    for (int offset=0; offset<frame.samples.size(); offset+=128) {
        auto part = stamped; part.seq = m_wireSeq[frame.ch]++;
        if (part.rateHz) part.tsUs+=quint64(offset)*1000000/part.rateHz;
        part.samples = frame.samples.mid(offset,128);
        part.samplesY = frame.samplesY.mid(offset,128); part.samplesZ = frame.samplesZ.mid(offset,128);
        sendPacket(mlk::buildWaveform(part));
    }
}

void KLocalWindow::handleControl(const mlk::Frame &f, const QHostAddress &address, quint16 port)
{
    if (f.type != mlk::kTypeConfig && f.type != mlk::kTypeOrderAck) return;
    // Writes and ACKs require authenticated transport. Plain read-only snapshots remain available.
    const bool authenticated = m_sec.mode() != mlk::SecureChannel::Off && m_sec.ready();
    if (f.type == mlk::kTypeOrderAck) {
        if (authenticated && f.object.value("status").toString() == "stored")
            mlk::WorkOrders(m_cfg.storageDir,m_cfg.passphrase).acknowledge(f.object.value("id").toString());
        return;
    }
    const auto op = f.object.value("op").toString();
    if (op != "get" && op != "set") return;
    if (authenticated) { m_replayPeer = address; m_replayPort = port; }
    const auto request = f.object.value("request").toString();
    if (op == "set" && !request.isEmpty() && m_configReplies.contains(request)) {
        auto snapshot = mlk::sharedConfig(m_cfg); snapshot.insert("op","snapshot");
        snapshot.insert("request",request); snapshot.insert("error",m_configReplies.value(request).value("error"));
        m_replayRx->send(address,port,mlk::buildObjectFrame(mlk::kTypeConfig,snapshot)); return;
    }
    QString error;
    if (op == "set") {
        auto candidate = m_cfg;
        if (!authenticated) error = QStringLiteral("共享配置写入需要加密会话");
        else if (!m_cfg.remotePriority) error = QStringLiteral("ps-local 优先，远端共享参数写入被拒绝");
        else if (!mlk::readSharedConfig(f.object,candidate) || !candidate.remotePriority)
            error = QStringLiteral("共享参数无效，控制权只能在板端修改");
        else {
            /* remote-pc 优先时上位机是共享参数的权威：板端在离线期间的本地修改
             * 在远端重连后被其完整快照覆盖，因此不要求版本号一致。 */
            candidate.sharedRevision = m_cfg.sharedRevision;
            if (!mlk::sameSharedValues(candidate,m_cfg)) {
                if (candidate.sharedRevision >= 0xfffffffeU) error=QStringLiteral("共享版本已达到上限");
                else {
                    candidate.sharedRevision = m_cfg.sharedRevision + 1;
                    if (!candidate.save()) error=QStringLiteral("共享配置保存失败");
                    else { m_cfg=candidate; applyConfig(); }
                }
            }
        }
    }
    auto snapshot = mlk::sharedConfig(m_cfg); snapshot.insert("op","snapshot");
    snapshot.insert("request",f.object.value("request")); snapshot.insert("error",error);
    if (op == "set" && authenticated && !request.isEmpty()) {
        m_configReplies.insert(request,snapshot); m_configReplyOrder.enqueue(request);
        if (m_configReplyOrder.size()>64) m_configReplies.remove(m_configReplyOrder.dequeue());
    }
    m_replayRx->send(address,port,mlk::buildObjectFrame(mlk::kTypeConfig,snapshot));
}

bool KLocalWindow::reportFault(const QString &id, quint64 faultTimeUs, int faultClass,
                              int grade, const QString &message)
{
    const QJsonObject order{{"id",id}, {"faultTimeUs",QString::number(faultTimeUs)},
        {"class",faultClass}, {"grade",grade}, {"message",message}};
    QString error;
    if (!mlk::WorkOrders(m_cfg.storageDir,m_cfg.passphrase).save(order,&error)) {
        showToast(QStringLiteral("故障工单保存失败：%1").arg(error),5000); return false;
    }
    if (faultTimeUs >= m_faultTimeUs) { m_faultTimeUs=faultTimeUs; m_latestOrder=order; }
    if (!mlk::CaptureStore(m_cfg.storageDir,m_cfg.passphrase).putLabel(mlk::faultCaptureLabel(order),&error)) {
        showToast(QStringLiteral("故障标签保存失败：%1").arg(error),5000); return false;
    }
    if (m_captureDialog && m_captureDialog->isVisible()) m_captureDialog->refresh();
    updateDiagnosisUi(); retryOrders(); return true;
}

void KLocalWindow::retryOrders()
{
    if (m_sec.mode() == mlk::SecureChannel::Off || !m_sec.ready()) return;
    if (!m_orderOutbox) return;
    m_orderOutbox->configure(m_cfg.storageDir, m_cfg.passphrase);
    m_orderOutbox->retry();
}

bool KLocalWindow::reportModelState(const QJsonObject &state)
{
    if (faultPublisherActive()) return false;
    if (!mlk::validModelFault(state)) return false;
    // Fault IDs are immutable work-order IDs. Repeated heartbeats are idempotent.
    bool stored=true;
    if (mlk::modelAlarmGrade(state)>0 && (state!=m_modelState || !m_modelStored)) {
        const QJsonObject order{{"id",state.value("id")},{"faultTimeUs",state.value("faultTimeUs")},
            {"class",state.value("class")},{"grade",state.value("grade")},{"message",state.value("message")}};
        mlk::WorkOrders store(m_cfg.storageDir,m_cfg.passphrase);
        QJsonObject previous;
        if (store.load(state.value("id").toString(),previous) && previous!=order) return false;
        stored=reportFault(state.value("id").toString(),state.value("faultTimeUs").toString().toULongLong(),
            state.value("class").toInt(),state.value("grade").toInt(),state.value("message").toString());
    }
    m_modelStored=stored; m_modelState=state; m_modelSeen.restart();
    m_alarm->modelGrade(mlk::modelAlarmGrade(state));
    updateDiagnosisUi();
    return stored;
}

/* ---- 实时故障诊断（模型诊断 API 2） -------------------------------------- */

bool KLocalWindow::faultPublisherActive() const
{
    return m_faultStartTimer || (m_faultEval && m_faultEval->running())
        || (m_faultLive && (m_faultLive->busy() || m_faultLive->externalPublisher()));
}

void KLocalWindow::updateFaultPanel()
{
    if (!m_faultPanel || !m_faultLive) return;
    m_faultPanel->showState(m_faultLive->state(), m_faultLive->stateDetail());
    m_faultPanel->setBusy(m_faultStartTimer || m_faultLive->busy());
}

void KLocalWindow::cancelFaultStart()
{
    const bool pendingEvaluation = m_faultStartTimer && m_faultPendingRoot.isEmpty();
    if (m_faultStartTimer) {
        m_faultStartTimer->stop();
        m_faultStartTimer->deleteLater();
        m_faultStartTimer = nullptr;
    }
    m_faultPendingRoot.clear();
    if (pendingEvaluation && m_faultPanel && m_faultPanel->evalPanel()) {
        m_faultPanel->evalPanel()->setBusy(m_faultEval && m_faultEval->running());
        m_faultPanel->evalPanel()->showState(QStringLiteral("评估启动已取消"));
    }
}

void KLocalWindow::waitForFaultResources(const std::function<void()> &ready)
{
    const auto idle = [this] {
        return (!m_faultEval || !m_faultEval->running())
            && (!m_replay || m_replay->snapshot().inferenceState == mlk::kStopped);
    };
    if (idle()) { ready(); return; }
    auto elapsed = QSharedPointer<QElapsedTimer>::create();
    elapsed->start();
    m_faultStartTimer = new QTimer(this);
    m_faultStartTimer->setInterval(100);
    connect(m_faultStartTimer, &QTimer::timeout, this, [this, idle, elapsed, ready] {
        if (idle()) {
            m_faultStartTimer->stop();
            m_faultStartTimer->deleteLater();
            m_faultStartTimer = nullptr;
            ready();
        } else if (elapsed->elapsed() >= 8000) {
            cancelFaultStart();
            updateFaultPanel();
            if (m_faultPanel && m_faultPanel->evalPanel()) {
                m_faultPanel->evalPanel()->setBusy(m_faultEval && m_faultEval->running());
                m_faultPanel->evalPanel()->showState(QStringLiteral("等待引擎释放超时"));
            }
            showToast(QStringLiteral("等待引擎释放超时，请停止旧任务后重试"), 6000);
        }
    });
    m_faultStartTimer->start();
}

void KLocalWindow::resetFaultAdapter()
{
    m_faultInput.configure(m_cfg);
    m_faultInput.restart();
}

void KLocalWindow::startFaultDiagnosis(const QString &root, int backend)
{
    if (!m_faultLive) return;
    if (root.isEmpty()) {
        showToast(QStringLiteral("故障运行包目录为空"), 5000);
        return;
    }
    if (m_faultLive->busy() || m_faultLive->externalPublisher()) {
        showToast(QStringLiteral("故障诊断已在进行"), 4000);
        return;
    }
    cancelFaultStart();
    /* 取消评估后等待工作线程释放引擎，再加载实时任务。 */
    if (m_faultEval && m_faultEval->running()) {
        m_faultEval->stop();
        if (m_faultPanel && m_faultPanel->evalPanel()) {
            m_faultPanel->evalPanel()->showState(QStringLiteral("停止中，等待引擎释放"));
        }
    }
    m_faultPendingRoot = root;
    m_faultPendingBackend = backend;
    /* 两类模型互斥：先停 RUL 回放，等它释放引擎后再启动故障任务。 */
    const auto replayState = m_replay ? quint8(m_replay->snapshot().inferenceState)
                                      : quint8(mlk::kStopped);
    if (m_replay && replayState != mlk::kStopped)
        m_replay->control(quint8(mlk::kReplayStop), 1, 0, 1000);
    m_faultPanel->showState(mlk::kFaultLoading, QStringLiteral("等待引擎释放"));
    m_faultPanel->setBusy(true);
    waitForFaultResources([this] { beginFaultRun(); });
}

void KLocalWindow::beginFaultRun()
{
    if (m_faultPendingRoot.isEmpty() || !m_faultLive) return;
    const QString root = m_faultPendingRoot;
    const int backend = m_faultPendingBackend;
    m_faultPendingRoot.clear();
    m_faultInput.configure(m_cfg);
    m_faultInput.restart();
    mlk::FaultRunConfig config;
    config.root = root;
    config.backend = backend;
    config.sourceRun = m_faultInput.sourceRun();
    config.scaleId = m_faultInput.scaleId();
    QString error;
    if (m_cfg.passphrase.isEmpty()) {
        showToast(QStringLiteral("请先在配置中设置记录口令，再开始诊断和加密记录"), 6000);
        updateFaultPanel();
        return;
    }
    saveSnapshot(); // Seal previous/manual recording data before the new retention session.
    m_faultRecordPolicy.end();
    if (!m_recording) m_recordBtn->setChecked(true);
    if (!m_recording) {
        showToast(QStringLiteral("无法开始原始数据记录，诊断未启动"), 6000);
        updateFaultPanel();
        return;
    }
    if (!m_faultLive->start(config, &error)) {
        showToast(error, 6000);
        updateFaultPanel();
        return;
    }
    if (m_faultPanel->discardNormalRecordings())
        m_faultRecordPolicy.begin(m_cfg.storageDir,m_cfg.recordSegmentSec);
    m_faultModeOn = true;
    m_faultAlarmConfirmed = false;
    m_faultPacketSeq = 0;
    m_faultStatusClock.invalidate();
    QSettings store;
    store.setValue(QStringLiteral("fault/modelDirectory"), root);
    store.setValue(QStringLiteral("fault/backend"), backend);
    store.setValue(QStringLiteral("fault/discardNormalRecordings"),m_faultPanel->discardNormalRecordings());
    store.setValue(QStringLiteral("inference/mode"), QStringLiteral("fault"));
    showToast(QStringLiteral("正在加载故障运行包"), 4000);
}

void KLocalWindow::stopFaultDiagnosis()
{
    cancelFaultStart();
    if (!m_faultLive) return;
    if (m_faultLive->externalPublisher()) {
        /* 外部发布者按显式停止释放会话；之后再启动内置任务不受旧 run 影响。 */
        m_faultLive->releaseExternal();
    } else {
        m_faultLive->stop();
    }
    m_faultAlarmConfirmed = false;
    if (m_alarm) m_alarm->modelGrade(0);
    m_faultModeOn = false;
    m_faultStatusClock.invalidate();
    QSettings().setValue(QStringLiteral("inference/mode"), QStringLiteral("off"));
    if (m_faultPanel) m_faultPanel->setBusy(false);
    saveSnapshot(); // Seal the current segment; recording remains enabled.
    m_faultRecordPolicy.end();
}

void KLocalWindow::startFaultEvaluation(const QString &packageRoot, const QString &datasetRoot,
                                        int backend)
{
    if (!m_faultEval || !m_faultPanel || !m_faultPanel->evalPanel()) return;
    auto *panel = m_faultPanel->evalPanel();
    if (m_faultEval->running()) {
        showToast(QStringLiteral("评估已在进行，先停止"), 4000);
        return;
    }
    if (faultPublisherActive()) {
        showToast(QStringLiteral("先停止实时故障诊断，再开始数据集评估"), 5000);
        return;
    }
    /* 评估与回放共用 NPU 与 CMA，开始前必须先停止回放。 */
    const auto replayState = m_replay ? quint8(m_replay->snapshot().inferenceState)
                                      : quint8(mlk::kStopped);
    if (m_replay && replayState != mlk::kStopped)
        m_replay->control(quint8(mlk::kReplayStop), 1, 0, 1000);

    panel->clearReport();
    panel->setBusy(true);
    panel->showProgress(0, 0);
    panel->showState(QStringLiteral("等待引擎释放"));
    waitForFaultResources([this, panel, packageRoot, datasetRoot, backend] {
        QString error;
        if (!m_faultEval->start(packageRoot, datasetRoot, backend, &error)) {
            panel->setBusy(false);
            panel->showState(error);
            showToast(error, 6000);
            return;
        }
        QSettings store;
        store.setValue(QStringLiteral("fault/modelDirectory"), packageRoot);
        store.setValue(QStringLiteral("fault/evalDirectory"), datasetRoot);
        store.setValue(QStringLiteral("fault/evalBackend"), backend);
    });
}

void KLocalWindow::stopFaultEvaluation()
{
    if (m_faultStartTimer && m_faultPendingRoot.isEmpty()) {
        cancelFaultStart();
        if (m_faultPanel && m_faultPanel->evalPanel()) {
            m_faultPanel->evalPanel()->setBusy(false);
            m_faultPanel->evalPanel()->showState(QStringLiteral("评估已取消"));
        }
        return;
    }
    if (!m_faultEval || !m_faultEval->running()) return;
    m_faultEval->stop();
    if (m_faultPanel && m_faultPanel->evalPanel())
        m_faultPanel->evalPanel()->showState(QStringLiteral("停止中"));
}

void KLocalWindow::publishFaultStatus()
{
    if (!m_faultLive || !m_replayRx) return;
    const auto result = m_faultLive->current();
    if (result.run.isEmpty() && !m_faultModeOn) return;
    /* 尚无 run 身份时不发摘要：接收端要求 run 为 UUID，不能用空身份占位。 */
    if (result.run.isEmpty()) return;
    /* 摘要单独发包，不挤占 BoardStatus 的 1200 字节预算；tsUs 为最后窗口 endUs。 */
    const auto status = mlk::faultStatusJson(result, m_faultLive->ageMs(),
                                             m_faultAlarmConfirmed);
    const auto packet = mlk::buildFaultStatus(status, ++m_faultPacketSeq, result.endUs);
    if (!packet.isEmpty()) sendPacket(packet);
}

void KLocalWindow::applyFaultEvent(const mlk::FaultResult &snapshot)
{
    if (!snapshot.alarmEligible()) return;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject diagnosis{{"classIndex", snapshot.classIndex},
                          {"fault", snapshot.fault()},
                          {"model", snapshot.model},
                          {"preprocess", snapshot.preprocess},
                          {"run", snapshot.run},
                          {"seq", double(snapshot.seq)},
                          {"startUs", mlk::faultTimeString(snapshot.startUs)},
                          {"endUs", mlk::faultTimeString(snapshot.endUs)},
                          {"confidence", snapshot.hasConfidence
                               ? QJsonValue(snapshot.confidence) : QJsonValue(QJsonValue::Null)}};
    QJsonObject order{{"schema", QString::fromLatin1(mlk::kFaultOrderSchema)},
                      {"id", id},
                      {"faultTimeUs", mlk::faultTimeString(snapshot.endUs)},
                      {"class", mlk::faultLegacyClass(snapshot.grade)},
                      {"grade", snapshot.grade},
                      {"message", QStringLiteral("%1 等级 %2")
                                      .arg(mlk::faultDisplayName(snapshot.classIndex))
                                      .arg(snapshot.grade)},
                      {"diagnosis", diagnosis}};
    QString error;
    const bool stored = mlk::WorkOrders(m_cfg.storageDir, m_cfg.passphrase).save(order, &error);
    if (stored) {
        m_faultRecordPolicy.observe(snapshot,true);
        saveSnapshot(); // Make the diagnosis window available for immediate waveform review.
        if (!mlk::CaptureStore(m_cfg.storageDir, m_cfg.passphrase)
                 .putLabel(mlk::faultCaptureLabel(order), &error))
            showToast(QStringLiteral("故障标签保存失败：%1").arg(error), 6000);
        m_faultTimeUs = snapshot.endUs;
        m_latestOrder = order;
        if (m_captureDialog && m_captureDialog->isVisible()) m_captureDialog->refresh();
        retryOrders();
    } else {
        /* 保存失败不阻断物理告警，事件由 pending 重试与界面共同提示。 */
        showToast(QStringLiteral("故障工单保存失败：%1（声光仍按确认等级执行）").arg(error), 6000);
    }
    m_faultAlarmConfirmed = true;
    if (m_alarm) m_alarm->modelGrade(snapshot.grade);
    updateDiagnosisUi();
    publishFaultStatus();
}

void KLocalWindow::updateCompatModelState(const mlk::FaultResult &result)
{
    if (result.valid && (result.classIndex == mlk::kFaultNormal || result.alarmEligible())) {
        /* 旧端只读粗分类摘要：grade=1→class 1，grade=2/3→class 2。 */
        m_modelState = QJsonObject{{"api", 1},
                                   {"valid", true},
                                   {"class", mlk::faultLegacyClass(result.grade)},
                                   {"grade", result.grade},
                                   {"id", m_modelState.value("id")},
                                   {"faultTimeUs", mlk::faultTimeString(result.endUs)},
                                   {"message", QStringLiteral("%1 等级 %2")
                                                   .arg(mlk::faultDisplayName(result.classIndex))
                                                   .arg(result.grade)}};
        m_modelStored = true;
    } else {
        m_modelState = mlk::unknownModelFault();
    }
    m_modelSeen.restart();
    updateDiagnosisUi();
}

QJsonObject KLocalWindow::reportDiagnosisResult(const QJsonObject &state)
{
    auto bad = [](const QString &code) {
        return QJsonObject{{"ok", false}, {"error", code}};
    };
    if (!m_faultLive) return bad(QStringLiteral("unsupported_api"));
    if (m_faultLive->running()) return bad(QStringLiteral("publisher_busy"));
    mlk::FaultResult result;
    QString error;
    if (!mlk::FaultResult::fromJson(state, result, &error))
        return bad(QStringLiteral("invalid_result"));
    QString code;
    if (!m_faultLive->acceptExternal(result, &code))
        return bad(code.isEmpty() ? QStringLiteral("invalid_result") : code);
    m_faultModeOn = true;
    QSettings().setValue(QStringLiteral("inference/mode"), QStringLiteral("fault-external"));
    updateFaultPanel();
    return QJsonObject{{"ok", true}, {"error", QString()},
                       {"acceptedSeq", double(result.seq)}};
}
