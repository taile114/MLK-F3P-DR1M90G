#pragma once

#include <QElapsedTimer>
#include <QMainWindow>
#include <QHash>
#include <QQueue>
#include <QHostAddress>
#include <functional>

#include "appconfig.h"
#include "frame.h"
#include "securechannel.h"
#include "systemmonitor.h"
#include "capturestore.h"
#include "faultinputadapter.h"
#include "faultlog.h"
#include "faultrecordpolicy.h"

class QLabel;
class QProgressBar;
class QPushButton;
class VirtualKeyboard;
class QTimer;
class SensorPanel;
class AlarmController;
class ModelFaultServer;
class FaultPanel;

namespace mlk {
class NpuPanel;
class DataSource;
class DriverSource;
class PlotWidget;
class ReplayController;
class ReplayPanel;
class Receiver;
class PeerDiscovery;
class CaptureDialog;
class BoardTime;
class FaultLiveController;
class FaultEvalController;
class WorkOrderOutbox;
struct FaultResult;
}

class KLocalWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit KLocalWindow(QWidget *parent = nullptr);
    ~KLocalWindow() override;

public slots:
    void openSettings();
    Q_INVOKABLE bool reportModelState(const QJsonObject &state);
    Q_INVOKABLE QJsonObject reportDiagnosisResult(const QJsonObject &state);
    Q_INVOKABLE bool reportFault(const QString &id, quint64 faultTimeUs, int faultClass,
                                int grade, const QString &message);


protected:
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onFrame(const mlk::Frame &f);
    void onTick();
    void onSystemStats(const mlk::SystemStats &stats);
    void onTiming(int pollMs, int readUs);
    void saveSnapshot();
    void openCaptures();
    void zeroCurrent();

private:
    AlarmController *m_alarm = nullptr;
    ModelFaultServer *m_faultServer = nullptr;
    ModelFaultServer *m_captureApi = nullptr;
    mlk::CaptureDialog *m_captureDialog = nullptr;
    mlk::CaptureRecorder m_captureRecorder;
    mlk::FaultJournal m_faultJournal;
    mlk::FaultRecordPolicy m_faultRecordPolicy;
    void pruneNormalRecordings();
    void recordWaveform(const mlk::Frame &frame);
    QJsonObject m_modelState;
    QElapsedTimer m_modelSeen;
    bool m_modelStored=true;
    QString m_alarmStatus;
    mlk::PeerDiscovery *m_discovery = nullptr;
    mlk::WorkOrderOutbox *m_orderOutbox = nullptr;
    void onSpeedSample(qint32 rpmMilli, quint32 count, quint32 tsTicks, bool online,
                       bool overflow, bool error, bool fromDma);
    void onSpeedAvailability(bool available, const QString &reason);
    void handleWaveform(const mlk::Frame &f);
    void handleDiagnosis(const mlk::Frame &f);
    void countFrame();
    void updateDiagnosisUi();
    void updatePlFeatures();
    void applyConfig();
    void setupSecure();
    void sendInference(const mlk::Frame &frame);
    void sendPacket(const QByteArray &packet);
    void sendWaveform(const mlk::Frame &frame);
    void handleControl(const mlk::Frame &frame, const QHostAddress &address, quint16 port);
    void retryOrders();

    /* 实时故障诊断：模式所有权、状态发布与兼容摘要 */
    void startFaultDiagnosis(const QString &root, int backend);
    void stopFaultDiagnosis();
    void beginFaultRun();
    void publishFaultStatus();
    void updateFaultPanel();
    void applyFaultEvent(const mlk::FaultResult &snapshot);
    void updateCompatModelState(const mlk::FaultResult &result);
    bool faultPublisherActive() const;
    void resetFaultAdapter();
    void waitForFaultResources(const std::function<void()> &ready);
    void cancelFaultStart();

    /* 数据集评估：与实时诊断互斥，结果只用于显示与导出，不产生告警或工单。 */
    void startFaultEvaluation(const QString &packageRoot, const QString &datasetRoot, int backend);
    void stopFaultEvaluation();

    void updateBoardTime();
    void updateRemoteConnectionStatus();
    void updateHeaderPosition();
    void updateSidebarGeometry();
    void showToast(const QString &text, int ms = 3000);

    mlk::AppConfig m_cfg;
    mlk::SecureChannel m_sec;
    mlk::DataSource *m_src = nullptr;
    mlk::DriverSource *m_driver = nullptr;
    mlk::ReplayController *m_replay = nullptr;
    mlk::ReplayPanel *m_replayPanel = nullptr;
    mlk::Receiver *m_replayRx = nullptr;
    QHostAddress m_replayPeer;
    quint16 m_replayPort = 0;
    QHash<QString, mlk::Frame> m_replies;
    QQueue<QString> m_replyOrder;
    SensorPanel *m_panels[mlk::kChannelCount] = {};
    QWidget *m_grid = nullptr;   /* 波形 2×2 网格，标题按其中心对齐 */
    QLabel *m_title = nullptr;
    QLabel *m_timeLabel = nullptr; /* 标题下的板端本地时间，寿命预测的时间基准 */
    QLabel *m_remoteConnection = nullptr;
    QElapsedTimer m_remoteSeen;
    QPushButton *m_recordBtn = nullptr;
    QTimer *m_recordTimer = nullptr;
    bool m_recording = false;
    QLabel *m_statLatency = nullptr;
    QLabel *m_statRate = nullptr;
    QLabel *m_statPkts = nullptr;
    QLabel *m_statLost = nullptr;
    QLabel *m_statAi = nullptr;
    QLabel *m_statVdma = nullptr;
    QLabel *m_statRead = nullptr;
    QLabel *m_statDmaBlocks = nullptr;
    QLabel *m_statDmaLost = nullptr;
    QLabel *m_statDmaAge = nullptr;
    QLabel *m_statAbs = nullptr;
    QLabel *m_statUptime = nullptr;
    QLabel *m_featureText = nullptr;
    QLabel *m_srcText = nullptr; /* 指标区：数据来源，紧跟 PL 特征摘要 */
    QLabel *m_diagRul = nullptr;
    QLabel *m_diagBackend = nullptr;
    QLabel *m_diagState = nullptr;
    QLabel *m_diagWindow = nullptr;
    QTimer *m_statTimer = nullptr;
    QTimer *m_uiTimer = nullptr;
    QTimer *m_featureTimer = nullptr;
    QTimer *m_timeTimer = nullptr; /* 1 s 刷新世界时钟 */
    mlk::BoardTime *m_boardTime = nullptr; /* 网络校时检测与手动校时 */
    int m_featureFd = -1;
    VirtualKeyboard *m_vk = nullptr;
    QLabel *m_toast = nullptr;
    QTimer *m_toastTimer = nullptr;
    QElapsedTimer m_uptime;

    QLabel *m_cpuText = nullptr;
    QLabel *m_cpuCores = nullptr;
    QLabel *m_memText = nullptr;
    QLabel *m_netText = nullptr;
    QLabel *m_diskText = nullptr;
    mlk::NpuPanel *m_npuPanel = nullptr;
    QProgressBar *m_cpuBar = nullptr;
    QProgressBar *m_memBar = nullptr;
    QProgressBar *m_diskBar = nullptr;
    mlk::PlotWidget *m_cpuPlot = nullptr;
    mlk::PlotWidget *m_memPlot = nullptr;
    mlk::SystemMonitor *m_sys = nullptr;
    QLabel *m_networkStatus = nullptr;
    bool m_networkLinked = false; /* 上一次系统统计中的链路状态，用于触发校时检测 */
    QLabel *m_cpuSummary = nullptr;
    QPushButton *m_sysToggle = nullptr;
    QWidget *m_sidebar = nullptr;
    quint32 m_pkts = 0;
    quint32 m_tickPkts = 0;
    quint32 m_lost = 0;
    quint32 m_lastSeq[mlk::kChannelCount] = {};
    bool m_haveSeq[mlk::kChannelCount] = {};
    double m_latencyMs = 0;
    bool m_haveLatency = false;
    quint16 m_lastRate = 1000;
    /* 转速（接口 v1.2）：面板是 setScale(0.1, 0) 的 int16 流水线（1 LSB =
     * 0.1 rpm，signed32），寄存器轮询与 DMA 转速记录用 PL 事件时间戳去重。 */
    bool m_haveSpeedTs = false;
    quint32 m_speedLastTs = 0;
    int m_speedRate = 10;   /* PL 采样周期 100 ms */

    quint32 m_wireSeq[4] = {};
    quint64 m_faultTimeUs = 0;
    QJsonObject m_latestOrder;
    QJsonObject m_boardStatus;
    QHash<QString,QJsonObject> m_configReplies;
    QQueue<QString> m_configReplyOrder;
    mlk::Frame m_diag;
    bool m_diagValid = false;

    /* 实时故障诊断（API 2）：与 RUL 回放互斥，二者不能同时持有推理引擎。 */
    mlk::FaultInputAdapter m_faultInput;
    mlk::FaultLiveController *m_faultLive = nullptr;
    mlk::FaultEvalController *m_faultEval = nullptr;
    FaultPanel *m_faultPanel = nullptr;
    QElapsedTimer m_faultStatusClock;
    quint32 m_faultPacketSeq = 0;
    bool m_faultAlarmConfirmed = false;
    bool m_faultModeOn = false;
    QString m_faultPendingRoot;
    int m_faultPendingBackend = 0;
    QTimer *m_faultStartTimer = nullptr;
};
