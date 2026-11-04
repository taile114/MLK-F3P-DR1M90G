#pragma once

#include <QElapsedTimer>
#include <QMainWindow>

#include "appconfig.h"
#include "frame.h"
#include "faultresult.h"
#include "securechannel.h"
#include "inferencejournal.h"
#include "capturestore.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTimer;
class QProgressBar;
namespace mlk {
class NpuPanel; class PlotWidget; }
class SensorPanel;
class ModelFaultServer;

namespace mlk {
class Receiver;
class PeerDiscovery;
class ReplayPanel;
class CaptureDialog;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
public slots:
    void openSettings();
    void openCaptures();
public:

    void startListening(quint16 port);
    void enableDemo(bool on);
    void setTarget(const QString &addr, int port);
    void setSecurity(int mode, const QString &pskHex);
    void setInjectDuration(int ms);
    void injectValues(double temp, double current, double vib, double speed);

protected:
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void toggleListen(bool on);
    void onFrame(const mlk::Frame &f);
    void onTick();
    void onDemoToggled(bool on);
    void onDemoTick();
    void onDemoDiagTick();

private:
    mlk::PeerDiscovery *m_discovery = nullptr;
    void updateHeaderPosition();
    void updateSidebarGeometry();
    void queryConfig();
    void pushSharedConfig();
    void updateConnectionStatus();
    void clearPendingCommands();
    void handleControl(const mlk::Frame &frame);
    void saveSnapshot();
    void showToast(const QString &text, int ms=3000);
    void handleWaveform(const mlk::Frame &f);
    void handleDiagnosis(const mlk::Frame &f);
    void updateDiagnosisUi();
    void updateFaultLiveUi();
    bool faultStatusLinkAlive() const;
    bool faultStatusExpired() const;
    qint64 faultStatusAgeNow() const;
    void applyConfig();
    void setupSecure();
    void saveFault(const mlk::Frame &f);
    void refreshFaults();
    void replayFault(const QString &path);
    void performInject(const double vals[4], int durationMs);
    void requestReplay(quint8 action, quint8 backend, quint32 index, quint32 interval);

    QWidget *m_grid = nullptr, *m_sidebar = nullptr;
    QLabel *m_title = nullptr, *m_timeLabel = nullptr, *m_cpuSummary = nullptr;
    QPushButton *m_sysToggle = nullptr, *m_recordBtn = nullptr;
    QTimer *m_uiTimer = nullptr, *m_recordTimer = nullptr;
    QLabel *m_cpuText = nullptr, *m_cpuCores = nullptr, *m_memText = nullptr, *m_netText = nullptr;
    QLabel *m_diskText = nullptr, *m_statAbs = nullptr;
    mlk::NpuPanel *m_npuPanel = nullptr;
    QProgressBar *m_cpuBar = nullptr, *m_memBar = nullptr, *m_diskBar = nullptr;
    mlk::PlotWidget *m_cpuPlot = nullptr, *m_memPlot = nullptr;
    QLabel *m_diagBackend = nullptr, *m_diagState = nullptr, *m_diagWindow = nullptr;
    QLabel *m_statRead = nullptr, *m_statVdma = nullptr, *m_statDmaBlocks = nullptr;
    QLabel *m_statDmaLost = nullptr, *m_statDmaAge = nullptr, *m_featureText = nullptr, *m_srcText = nullptr;
    QJsonObject m_modelFault;
    /* 完整故障状态（UDP v6/type8）独立保存：模型原始结果、确认告警与历史事件
     * 分开，BoardStatus 的 v1 粗摘要和 RUL 帧不能覆盖具体故障类型。
     * m_faultStatusValid 只表示已收到该消息；5 秒链路与结果年龄时效另算。 */
    mlk::FaultResult m_faultStatus;
    bool m_faultStatusValid = false;
    QElapsedTimer m_faultStatusSeen;    // 链路活跃时间：同窗口重发也刷新
    QElapsedTimer m_faultStatusAgeSeen; // 结果年龄时基：只在接纳新窗口时重置
    qint64 m_faultStatusAgeMs = -1;
    bool m_faultAlarmConfirmed = false;
    QString m_faultStatusError;
    QLabel *m_faultLive = nullptr;
    QElapsedTimer m_channelSeen[4], m_configSeen, m_statusSeen, m_handshakeSeen;
    quint64 m_faultTimeUs = 0;
    QJsonObject m_latestOrder;
    QLabel *m_networkStatus = nullptr;
    QLabel *m_connectionStatus = nullptr;
    QElapsedTimer m_linkSeen;
    QByteArray m_configPending;
    QString m_configRequest;
    int m_configAttempts = 0;
    bool m_recording = false;
    mlk::CaptureRecorder m_captureRecorder;
    mlk::CaptureDialog *m_captureDialog = nullptr;
    ModelFaultServer *m_captureApi = nullptr;

    mlk::AppConfig m_cfg;
    mlk::SecureChannel m_sec;
    QListWidget *m_faultList = nullptr;
    quint16 m_lastRate = 1000;
    mlk::Receiver *m_rx;
    mlk::ReplayPanel *m_replayPanel = nullptr;
    QTimer *m_replayRetry = nullptr;
    QByteArray m_replayPending;
    quint32 m_replayRequest = 0;
    int m_replayAttempts = 0;
    QElapsedTimer m_replaySeen;
    bool m_replayExpired = false;
    mlk::InferenceJournal m_rulJournal;
    quint64 m_rulLoggedTimestamp = 0;
    quint32 m_rulLoggedWindow = 0;
    SensorPanel *m_panels[mlk::kChannelCount];
    QLineEdit *m_addr;
    QSpinBox *m_port;
    QPushButton *m_listen;
    QCheckBox *m_demo;
    QLabel *m_statLatency;
    QLabel *m_statSampleAge;
    QString m_probeRequest;
    QElapsedTimer m_probeStarted, m_rttSeen, m_sampleAgeSeen, m_metricsLogSeen;
    double m_rttMs = 0;
    QLabel *m_statRate;
    QLabel *m_statPkts;
    QLabel *m_statLost;
    QLabel *m_statAi;
    QLabel *m_statUptime;
    QLabel *m_diagRul;
    QTimer *m_statTimer;
    QTimer *m_demoTimer;
    QTimer *m_demoDiagTimer;
    QElapsedTimer m_uptime;

    quint32 m_pkts = 0;
    quint32 m_tickPkts = 0;
    quint32 m_lost = 0;
    quint32 m_faults = 0;
    quint8 m_lastCls = 0;
    quint32 m_lastSeq[mlk::kChannelCount] = {};
    bool m_haveSeq[mlk::kChannelCount] = {};
    double m_sampleAgeMs = 0;

    quint32 m_injectSeq = 0;
    quint32 m_demoSeq[mlk::kChannelCount] = {};
    quint32 m_demoCnt[mlk::kChannelCount] = {};
    double m_demoPhase[mlk::kChannelCount] = {};

    mlk::Frame m_diag;
    bool m_diagValid = false;
};
