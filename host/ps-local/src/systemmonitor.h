#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QVector>
#include "npumonitor.h"
#include "networkstatus.h"

class QTimer;

namespace mlk {

struct SystemStats
{
    double cpuPercent = 0.0;
    QVector<double> cpuCores;

    double memPercent = 0.0;
    quint64 memUsedKb = 0;
    quint64 memTotalKb = 0;

    double diskPercent = 0.0;
    quint64 diskUsedBytes = 0;
    quint64 diskTotalBytes = 0;

    double rxKbps = 0.0;
    double txKbps = 0.0;
    QString netName;
    bool netUp = false;
    NetworkStatus network;

    double appCpuPercent = 0.0;
    double appOneCorePercent = 0.0;

    bool gpuAvailable = false;
    QString gpuText = QStringLiteral("不可用（无 GPU 设备）");
    NpuStats npu;
};

class SystemMonitor : public QObject
{
    Q_OBJECT
public:
    explicit SystemMonitor(QObject *parent = nullptr);

    void start();
    void stop();

signals:
    void statsUpdated(const mlk::SystemStats &stats);

private slots:
    void poll();

private:
    QTimer *m_timer;
    QElapsedTimer m_clock;
    qint64 m_prevMs = -1;
    quint64 m_prevCpuTotal = 0;
    quint64 m_prevCpuIdle = 0;
    QVector<quint64> m_prevCoreTotal;
    QVector<quint64> m_prevCoreIdle;
    quint64 m_prevRx = 0;
    quint64 m_prevTx = 0;
    quint64 m_prevAppTicks = 0;
    bool m_appInit = false;
    bool m_netInit = false;
    QString m_netName;
    NpuMonitor m_npu;
};

} // namespace mlk
