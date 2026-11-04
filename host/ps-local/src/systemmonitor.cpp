#include "systemmonitor.h"

#include <QFile>
#include <QFileInfo>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QStringList>
#include <QtGlobal>
#include <QStorageInfo>
#include <QTextStream>
#include <QTimer>
#include <sys/stat.h>
#include <unistd.h>

namespace mlk {

SystemMonitor::SystemMonitor(QObject *parent)
    : QObject(parent)
    , m_timer(new QTimer(this))
{
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, &SystemMonitor::poll);
}

void SystemMonitor::start()
{
    m_clock.start();
    m_npu.reset();
    poll();
    m_timer->start();
}

void SystemMonitor::stop()
{
    m_timer->stop();
}

static bool readCpuLine(const QString &line, quint64 &total, quint64 &idle)
{
    const QStringList parts = line.split(QLatin1Char(' '), QString::SkipEmptyParts);
    if (parts.size() < 8)
        return false;
    quint64 values[8] = {};
    for (int i = 0; i < 8; ++i)
        values[i] = parts[i + 1].toULongLong();
    total = 0;
    for (int i = 0; i < 8; ++i)
        total += values[i];
    // idle + iowait
    idle = values[3] + values[4];
    return true;
}

static bool characterDevice(const char *path)
{
    struct stat info;
    return ::stat(path, &info) == 0 && S_ISCHR(info.st_mode);
}

void SystemMonitor::poll()
{
    SystemStats s;
    const qint64 now = m_clock.elapsed();
    const double dt = m_prevMs < 0 ? 0.0 : double(now - m_prevMs) / 1000.0;

    QVector<quint64> coreTotal;
    QVector<quint64> coreIdle;
    QFile stat(QStringLiteral("/proc/stat"));
    if (stat.open(QIODevice::ReadOnly | QIODevice::Text)) {
        int core = 0;
        const QStringList lines = QString::fromLatin1(stat.readAll()).split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            quint64 total = 0;
            quint64 idle = 0;
            if (line.startsWith(QStringLiteral("cpu "))) {
                if (readCpuLine(line, total, idle)) {
                    if (m_prevCpuTotal && total >= m_prevCpuTotal) {
                        const quint64 dtot = total - m_prevCpuTotal;
                        const quint64 didle = idle >= m_prevCpuIdle ? idle - m_prevCpuIdle : 0;
                        if (dtot)
                            s.cpuPercent = 100.0 * double(dtot - qMin(didle, dtot)) / double(dtot);
                    }
                    m_prevCpuTotal = total;
                    m_prevCpuIdle = idle;
                }
            } else if (line.startsWith(QStringLiteral("cpu"))) {
                if (readCpuLine(line, total, idle)) {
                    double usage = 0.0;
                    if (m_prevCoreTotal.size() > core && total >= m_prevCoreTotal[core]) {
                        const quint64 dtot = total - m_prevCoreTotal[core];
                        const quint64 didle = idle >= m_prevCoreIdle[core]
                                                  ? idle - m_prevCoreIdle[core]
                                                  : 0;
                        if (dtot)
                            usage = 100.0 * double(dtot - qMin(didle, dtot)) / double(dtot);
                    }
                    s.cpuCores.append(usage);
                    coreTotal.append(total);
                    coreIdle.append(idle);
                    ++core;
                }
            }
        }
        m_prevCoreTotal = coreTotal;
        m_prevCoreIdle = coreIdle;
    }

    QFile mem(QStringLiteral("/proc/meminfo"));
    if (mem.open(QIODevice::ReadOnly | QIODevice::Text)) {
        quint64 total = 0;
        quint64 avail = 0;
        quint64 freeKb = 0;
        quint64 buffers = 0;
        quint64 cached = 0;
        const QStringList lines = QString::fromLatin1(mem.readAll()).split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            const QStringList p = line.split(QLatin1Char(' '), QString::SkipEmptyParts);
            if (p.size() < 2)
                continue;
            const quint64 v = p[1].toULongLong();
            if (p[0] == QStringLiteral("MemTotal:"))
                total = v;
            else if (p[0] == QStringLiteral("MemAvailable:"))
                avail = v;
            else if (p[0] == QStringLiteral("MemFree:"))
                freeKb = v;
            else if (p[0] == QStringLiteral("Buffers:"))
                buffers = v;
            else if (p[0] == QStringLiteral("Cached:"))
                cached = v;
        }
        if (!avail)
            avail = freeKb + buffers + cached;
        s.memTotalKb = total;
        s.memUsedKb = total > avail ? total - avail : 0;
        if (total)
            s.memPercent = 100.0 * double(s.memUsedKb) / double(total);
    }

    QFile self(QStringLiteral("/proc/self/stat"));
    if (self.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QByteArray data = self.readAll();
        const int close = data.lastIndexOf(')');
        if (close > 0) {
            const QList<QByteArray> f = data.mid(close + 2).split(' ');
            if (f.size() > 12) {
                const quint64 ticks = f[11].toULongLong() + f[12].toULongLong();
                if (m_appInit && dt > 0) {
                    const double hz = double(sysconf(_SC_CLK_TCK));
                    const double cpuSec = double(ticks - m_prevAppTicks) / hz;
                    s.appOneCorePercent = 100.0 * cpuSec / dt;
                    const int cores = qMax(1, int(sysconf(_SC_NPROCESSORS_ONLN)));
                    s.appCpuPercent = s.appOneCorePercent / cores;
                }
                m_prevAppTicks = ticks;
                m_appInit = true;
            }
        }
    }

    const QStorageInfo root = QStorageInfo::root();
    if (root.isValid()) {
        s.diskTotalBytes = quint64(root.bytesTotal());
        s.diskUsedBytes = quint64(root.bytesTotal() - root.bytesAvailable());
        if (s.diskTotalBytes)
            s.diskPercent = 100.0 * double(s.diskUsedBytes) / double(s.diskTotalBytes);
    }

    QFile net(QStringLiteral("/proc/net/dev"));
    quint64 rx = 0;
    quint64 tx = 0;
    if (net.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QStringList lines = QString::fromLatin1(net.readAll()).split(QLatin1Char('\n'));
        for (const QString &rawLine : lines) {
            const QString line = rawLine.trimmed();
            const int colon = line.indexOf(QLatin1Char(':'));
            if (colon <= 0)
                continue;
            const QString name = line.left(colon).trimmed();
            if (name == QStringLiteral("lo"))
                continue;
            const QStringList f = line.mid(colon + 1).split(
                QLatin1Char(' '), QString::SkipEmptyParts);
            if (f.size() < 9)
                continue;
            rx += f[0].toULongLong();
            tx += f[8].toULongLong();
        }
    }
    QVector<NetworkLink> links;
    for (const auto &iface : QNetworkInterface::allInterfaces()) {
        if (iface.flags().testFlag(QNetworkInterface::IsLoopBack)) continue;
        bool addressed = false;
        for (const auto &entry : iface.addressEntries())
            addressed = addressed || (entry.ip().protocol()==QAbstractSocket::IPv4Protocol && !entry.ip().isNull()
                && !entry.ip().isLinkLocal());
        const auto name=iface.name();
        const auto base=QStringLiteral("/sys/class/net/%1/").arg(name);
        const bool connected=iface.flags().testFlag(QNetworkInterface::IsUp)
            && iface.flags().testFlag(QNetworkInterface::IsRunning) && addressed;
        links.append({name,connected,QFileInfo::exists(base+"wireless"),
                      QFileInfo(QFileInfo(base+"device/driver").canonicalFilePath()).fileName()});
    }
    QFile routes(QStringLiteral("/proc/net/route"));
    QByteArray routeData;
    if (routes.open(QIODevice::ReadOnly)) routeData=routes.readAll();
    s.network=selectNetworkStatus(routeData,links);
    s.netName = s.network.interface;
    s.netUp = s.network.kind != QStringLiteral("disconnected");
    if (m_netInit && dt > 0) {
        s.rxKbps = double(rx - qMin(rx, m_prevRx)) / 1024.0 / dt;
        s.txKbps = double(tx - qMin(tx, m_prevTx)) / 1024.0 / dt;
    }
    m_prevRx = rx;
    m_prevTx = tx;
    m_netInit = true;

    QFile interrupts(QStringLiteral("/proc/interrupts"));
    QByteArray irqData;
    if (interrupts.open(QIODevice::ReadOnly)) irqData = interrupts.readAll();
    s.npu = m_npu.sample(characterDevice("/dev/hard_npu"), characterDevice("/dev/soft_npu"),
                         characterDevice("/dev/cma_mem"), irqData, now);

    // Retain GPU telemetry for the existing remote board-status message.
    s.gpuAvailable = QFileInfo::exists(QStringLiteral("/dev/dri/card0"))
                     || QFileInfo::exists(QStringLiteral("/dev/mali0"))
                     || QFileInfo::exists(QStringLiteral("/dev/kgsl-3d0"));
    if (s.gpuAvailable) {
        QFile busy(QStringLiteral("/sys/class/drm/card0/device/gpu_busy_percent"));
        if (busy.open(QIODevice::ReadOnly | QIODevice::Text))
            s.gpuText = QStringLiteral("使用率 %1%").arg(
                QString::fromLatin1(busy.readAll()).trimmed());
        else
            s.gpuText = QStringLiteral("驱动未提供使用率");
    } else {
        s.gpuText = QStringLiteral("不可用（无 GPU 设备）");
    }

    m_prevMs = now;
    emit statsUpdated(s);
}

} // namespace mlk
