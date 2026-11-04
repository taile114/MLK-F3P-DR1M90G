#include "boardtime.h"

#include <QFile>
#include <QPair>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>
#include <QVector>
#include <QDebug>

namespace {
QString tool(const char *overrideName, const char *name)
{
    const auto env = qEnvironmentVariable(overrideName);
    if (!env.isEmpty())
        return env;
    const auto found = QStandardPaths::findExecutable(QString::fromLatin1(name));
    return found.isEmpty() ? QString::fromLatin1(name) : found;
}

QString chronySocket()
{
    return qEnvironmentVariable("MLK_CHRONY_SOCKET",
                                QStringLiteral("/var/run/chrony/chronyd.sock"));
}

/* 是否存在默认路由；ppp0 的默认路由为 onlink（Gateway 00000000）。 */
bool hasDefaultRoute()
{
    QFile file(QStringLiteral("/proc/net/route"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    while (!file.atEnd()) {
        const auto parts = QString::fromLatin1(file.readLine())
                               .split(QRegularExpression(QStringLiteral("\\s+")),
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
                                      Qt::SkipEmptyParts);
#else
                                      QString::SkipEmptyParts);
#endif
        if (parts.size() < 8 || parts.at(0) == QLatin1String("Iface"))
            continue;
        bool destOk = false;
        const uint dest = parts.at(1).toUInt(&destOk, 16);
        bool flagsOk = false;
        const uint flags = parts.at(3).toUInt(&flagsOk, 16);
        if (destOk && flagsOk && dest == 0 && (flags & 0x1))
            return true;
    }
    return false;
}
}

namespace mlk {

BoardTime::BoardTime(QObject *parent)
    : QObject(parent)
    , m_timer(new QTimer(this))
{
    m_timer->setInterval(10000);
    connect(m_timer, &QTimer::timeout, this, [this] { check(false); });
    m_status = new QProcess(this);
    m_step = new QProcess(this);
    setTimeout(m_status, 4000);
    setTimeout(m_step, 12000);
    connect(m_status, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int, QProcess::ExitStatus) {
                const bool manual = m_manual;
                m_manual = false;
                statusFinished(manual);
            });
    connect(m_status, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        const bool manual = m_manual;
        m_manual = false;
        m_network = false;
        if (!m_known) {
            m_known = true;
            emit stateChanged(false, stateText());
        }
        if (manual)
            finishSync(false, QStringLiteral("无法执行 mlk_time_status"));
    });
    connect(m_step, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus status) {
                stepFinished(status == QProcess::NormalExit && code == 0);
            });
    connect(m_step, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        const bool manual = m_syncPending;
        m_syncPending = false;
        m_stepKind = 0;
        if (manual)
            finishSync(false, QStringLiteral("无法执行 chronyc"));
    });
}

void BoardTime::setTimeout(QProcess *process, int ms) const
{
    process->setProcessChannelMode(QProcess::SeparateChannels);
    auto *guard = new QTimer(process);
    guard->setSingleShot(true);
    connect(guard, &QTimer::timeout, process, [process] {
        if (process->state() != QProcess::NotRunning)
            process->kill();
    });
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            guard, &QTimer::stop);
    connect(process, &QProcess::started, guard, [guard, ms] { guard->start(ms); });
}

void BoardTime::start()
{
    m_timer->start();
    refreshNow();
}

void BoardTime::refreshNow()
{
    check(false);
}

QString BoardTime::stateText() const
{
    if (!m_known)
        return QStringLiteral("检测中");
    return m_network ? QStringLiteral("网络校时可用") : QStringLiteral("网络校时不可用");
}

void BoardTime::check(bool manual)
{
    if (manual)
        m_manual = true;
    if (m_status->state() != QProcess::NotRunning)
        return;
    m_status->start(tool("MLK_TIME_STATUS_BIN", "mlk_time_status"), QStringList{});
}

void BoardTime::statusFinished(bool manual)
{
    const bool network = m_status->exitStatus() == QProcess::NormalExit
                         && m_status->exitCode() == 0;
    const bool wasNetwork = m_network;
    m_network = network;
    const bool first = !m_known;
    m_known = true;
    if (first || wasNetwork != m_network)
        emit stateChanged(m_network, stateText());
    if (!network) {
        /* 手动同步时先让 Chrony 等待时间源，连上网络后立即生效。 */
        if (manual) {
            if (m_step->state() != QProcess::NotRunning) {
                m_syncPending = true;
                return;
            }
            m_syncPending = true;
            m_stepKind = 2;
            m_step->start(tool("MLK_CHRONYC_BIN", "chronyc"),
                          {QStringLiteral("-h"), chronySocket(), QStringLiteral("waitsync"),
                           QStringLiteral("6"), QStringLiteral("1"), QStringLiteral("0"),
                           QStringLiteral("1")});
        }
        return;
    }
    /* 网络时间源刚可用时立即步进一次，不等 Chrony 的平滑调整。 */
    const bool wantStep = !wasNetwork || manual;
    if (!wantStep)
        return;
    if (m_step->state() != QProcess::NotRunning) {
        m_syncPending = m_syncPending || manual;
        return;
    }
    m_syncPending = manual;
    m_stepKind = 1;
    m_step->start(tool("MLK_CHRONYC_BIN", "chronyc"),
                  {QStringLiteral("-h"), chronySocket(), QStringLiteral("makestep")});
}

void BoardTime::stepFinished(bool ok)
{
    const bool manual = m_syncPending;
    const int kind = m_stepKind;
    m_syncPending = false;
    m_stepKind = 0;
    if (!manual)
        return;
    if (kind == 2) {
        finishSync(ok, ok ? QStringLiteral("已与网络时间源同步")
                          : (hasDefaultRoute()
                                 ? QStringLiteral("NTP 同步中（预计 1 分钟内完成）")
                                 : QStringLiteral("未连接网络，无法同步")));
        if (ok)
            refreshNow();
        return;
    }
    finishSync(ok, ok ? QStringLiteral("已按网络时间校准板端时钟")
                      : QStringLiteral("chronyc makestep 执行失败"));
}

void BoardTime::syncNow()
{
    check(true);
}

void BoardTime::finishSync(bool ok, const QString &message)
{
    m_manual = false;
    m_syncPending = false;
    emit syncFinished(ok, message);
}

bool BoardTime::setDateTime(const QDateTime &local, QString *error)
{
    if (!local.isValid()) {
        if (error) *error = QStringLiteral("时间无效");
        return false;
    }
    if (m_network) {
        if (error) *error = QStringLiteral("网络时间源可用，由 NTP 校时");
        return false;
    }
    const qint64 epoch = local.toSecsSinceEpoch();
    if (epoch < 1700000000LL || epoch > 2147483647LL) {
        if (error) *error = QStringLiteral("时间需在 2023-11-15 至 2038-01-19 之间");
        return false;
    }
    QProcess setter;
    setter.setProcessChannelMode(QProcess::SeparateChannels);
    setter.start(tool("MLK_SET_TIME_BIN", "mlk_set_time"), {QString::number(epoch)});
    bool ok = setter.waitForStarted(1500) && setter.waitForFinished(4000)
              && setter.exitStatus() == QProcess::NormalExit && setter.exitCode() == 0;
    if (!ok) {
        /* 旧镜像没有 mlk_set_time 时直接写墙钟。 */
        setter.start(QStringLiteral("date"),
                     {QStringLiteral("-u"), QStringLiteral("-s"),
                      QStringLiteral("@%1").arg(epoch)});
        ok = setter.waitForStarted(1500) && setter.waitForFinished(4000)
             && setter.exitStatus() == QProcess::NormalExit && setter.exitCode() == 0;
        if (!ok) {
            if (error)
                *error = QStringLiteral("写入板端时间失败：%1")
                             .arg(QString::fromLocal8Bit(setter.readAllStandardError()).trimmed());
            return false;
        }
    }
    /* 回读核对，防止网络校时拒绝写入或 date 未生效。 */
    QProcess reader;
    reader.start(QStringLiteral("date"), {QStringLiteral("-u"), QStringLiteral("+%s")});
    if (!reader.waitForStarted(1500) || !reader.waitForFinished(3000)) {
        if (error) *error = QStringLiteral("读取板端时间失败");
        return false;
    }
    const qint64 actual = QString::fromLatin1(reader.readAllStandardOutput()).trimmed().toLongLong();
    if (qAbs(actual - epoch) > 3) {
        if (error)
            *error = QStringLiteral("板端时间未生效（当前 %1）")
                         .arg(QDateTime::fromSecsSinceEpoch(actual).toString(
                             QStringLiteral("yyyy-MM-dd HH:mm:ss")));
        return false;
    }
    refreshNow();
    return true;
}

bool BoardTime::reboot(QString *error)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    QVector<QPair<QString, QStringList>> commands;
    const QString override = qEnvironmentVariable("MLK_REBOOT_BIN");
    if (!override.isEmpty())
        commands.append({override, {}});
    else
        commands = {{QStringLiteral("/sbin/reboot"), {}},
                    {QStringLiteral("reboot"), {}},
                    {QStringLiteral("systemctl"), {QStringLiteral("reboot")}},
                    {QStringLiteral("busybox"), {QStringLiteral("reboot")}}};
    for (const auto &command : commands) {
        process.start(command.first, command.second);
        if (!process.waitForStarted(1500))
            continue;
        if (!process.waitForFinished(5000))
            return true;   /* reboot 通常不返回；进程仍在运行视为已提交。 */
        if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0)
            return true;
        const auto detail = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        if (!detail.isEmpty()) {
            if (error) *error = detail;
            return false;
        }
    }
    if (error) *error = QStringLiteral("找不到可用的 reboot 命令");
    return false;
}

} // namespace mlk
