#include <QApplication>
#include <QDebug>
#include <QSocketNotifier>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>

#include "klocalwindow.h"
#include "theme.h"

namespace {

/* SIGTERM / SIGINT / SIGHUP 不直接终止进程：信号处理函数只写入自管道的一个
 * 字节（write 与 errno 属于 async-signal-safe 调用），由 QSocketNotifier 在
 * 正常事件循环里转成 QCoreApplication::quit()。事件循环返回后 main 才会销毁
 * KLocalWindow，数据源的析构随之停止 PL 传感器 DMA 环。 */
int g_stopFds[2] = { -1, -1 };
volatile sig_atomic_t g_stopWriteFd = -1;

void onStopSignal(int sig)
{
    const int savedErrno = errno;
    if (g_stopWriteFd >= 0) {
        const char byte = char(sig);
        const ssize_t ignored = ::write(int(g_stopWriteFd), &byte, 1);
        (void)ignored;
    }
    errno = savedErrno;
}

/* 自管道两端均设非阻塞：读端供 QSocketNotifier 一次排空后立即返回，写端避免
 * 管道写满时把信号处理函数阻塞住。返回 false 表示安装失败，此时上述信号保持
 * 默认行为（进程直接终止，DMA 环只能手工恢复）。 */
bool installStopHandler()
{
    if (::pipe(g_stopFds) != 0)
        return false;
    for (int end = 0; end < 2; ++end) {
        ::fcntl(g_stopFds[end], F_SETFD, FD_CLOEXEC);
        const int flags = ::fcntl(g_stopFds[end], F_GETFL, 0);
        if (flags >= 0)
            ::fcntl(g_stopFds[end], F_SETFL, flags | O_NONBLOCK);
    }
    g_stopWriteFd = g_stopFds[1];

    struct sigaction sa = {};
    sa.sa_handler = onStopSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    const int stopSignals[] = { SIGTERM, SIGINT, SIGHUP };
    bool ok = true;
    for (int sig : stopSignals) {
        if (::sigaction(sig, &sa, nullptr) != 0)
            ok = false;
    }
    return ok;
}

} // namespace

int main(int argc, char **argv)
{
    const bool stopHandlerReady = installStopHandler();

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("MLK-F3P"));
    QApplication::setApplicationName(QStringLiteral("mlk_ps_local"));
    applyTheme(app);

    if (stopHandlerReady) {
        auto *notifier = new QSocketNotifier(g_stopFds[0], QSocketNotifier::Read, &app);
        QObject::connect(notifier, &QSocketNotifier::activated, &app, [] {
            char drained[16];
            while (::read(g_stopFds[0], drained, sizeof(drained)) > 0) {
            }
            QCoreApplication::quit();
        });
    } else {
        qWarning("mlk_ps_local: SIGTERM/SIGINT/SIGHUP handler not installed; "
                 "the PL DMA ring is not stopped on these signals");
    }

    KLocalWindow w;
    if (app.arguments().contains(QStringLiteral("--kiosk"))) {
        w.showFullScreen();
    } else {
        w.resize(1280, 800);
        w.show();
    }
    return app.exec();
}
