#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>

class QProcess;
class QTimer;

namespace mlk {

/* 板端时间来源与校时。
 *
 * 后台每 10 s 运行一次 mlk_time_status，检测网络时间源是否已选中；从不可用
 * 变为可用的上升沿用 chronyc makestep 立即步进，之后由 Chrony 平滑维持。
 * 手动入口供顶栏「时间同步」按钮调用；「配置」中的手动设置时间只在网络时间
 * 源不可用时开放。
 *
 * 外部命令可用环境变量替换，便于无板环境的测试：
 *   MLK_TIME_STATUS_BIN / MLK_CHRONYC_BIN / MLK_CHRONY_SOCKET /
 *   MLK_SET_TIME_BIN / MLK_REBOOT_BIN
 */
class BoardTime : public QObject
{
    Q_OBJECT
public:
    explicit BoardTime(QObject *parent = nullptr);

    void start();
    void refreshNow();

    bool networkTimeAvailable() const { return m_network; }
    QString stateText() const;

    /* 写入板端墙钟并回读核对；网络时间源可用时返回失败。阻塞至多约 4 s。 */
    bool setDateTime(const QDateTime &local, QString *error = nullptr);
    /* 网络时间可用时立即步进；结果通过 syncFinished 返回。 */
    void syncNow();
    bool reboot(QString *error = nullptr);

signals:
    void stateChanged(bool networkTime, const QString &detail);
    void syncFinished(bool ok, const QString &message);

private:
    void check(bool manual);
    void statusFinished(bool manual);
    void stepFinished(bool manual);
    void finishSync(bool ok, const QString &message);
    void setTimeout(QProcess *process, int ms) const;

    QTimer *m_timer = nullptr;
    QProcess *m_status = nullptr;
    QProcess *m_step = nullptr;
    bool m_network = false;
    bool m_known = false;
    bool m_manual = false;
    bool m_syncPending = false;
    int m_stepKind = 0; /* 0 空闲 · 1 makestep · 2 waitsync */
};

} // namespace mlk
