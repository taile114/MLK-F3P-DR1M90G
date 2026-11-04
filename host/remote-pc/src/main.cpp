#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QStringList>
#include <QTimer>
#include <QLockFile>
#include <QStandardPaths>
#include <QDir>
#include <QDebug>

#include "mainwindow.h"
#include "theme.h"

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("MLK-F3P"));
    QApplication::setApplicationName(QStringLiteral("mlk_udp_host"));

    applyTheme(app);
    QCommandLineParser p;
    p.setApplicationDescription(QStringLiteral("MLK-F3P UDP 上位机"));
    p.addHelpOption();
    const QCommandLineOption optListen({ QStringLiteral("l"), QStringLiteral("listen") },
                                       QStringLiteral("listen on UDP port"),
                                       QStringLiteral("port"));
    const QCommandLineOption optDemo(QStringLiteral("demo"),
                                     QStringLiteral("start with synthetic demo data"));
    const QCommandLineOption optTarget(QStringLiteral("target"),
                                       QStringLiteral("PS target host:port"), QStringLiteral("hp"));
    const QCommandLineOption optDur(QStringLiteral("dur"),
                                    QStringLiteral("injection duration in ms"),
                                    QStringLiteral("ms"));
    const QCommandLineOption optInject(QStringLiteral("inject"),
                                       QStringLiteral("inject temp,current,vib,speed"),
                                       QStringLiteral("vals"));
    const QCommandLineOption optSec(QStringLiteral("sec"),
                                    QStringLiteral("security: off|psk|ecdh"),
                                    QStringLiteral("mode"));
    const QCommandLineOption optPsk(QStringLiteral("psk"), QStringLiteral("psk hex"),
                                    QStringLiteral("hex"));
    p.addOption(optListen);
    p.addOption(optDemo);
    p.addOption(optTarget);
    p.addOption(optDur);
    p.addOption(optInject);
    p.addOption(optSec);
    p.addOption(optPsk);
    p.process(app);

    const auto runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    QLockFile instance(QDir(runtime).filePath(QStringLiteral("mlk-remote-pc.lock")));
    if (!instance.tryLock(0)) {
        qWarning("remote-pc is already running; use the existing window");
        return 0;
    }

    MainWindow w;
    w.resize(1920, 1080);
    if (p.isSet(optDemo))
        w.enableDemo(true);
    if (p.isSet(optTarget)) {
        const QString tv = p.value(optTarget);
        const int idx = tv.lastIndexOf(QLatin1Char(':'));
        if (idx > 0)
            w.setTarget(tv.left(idx), tv.mid(idx + 1).toInt());
    }
    if (p.isSet(optDur))
        w.setInjectDuration(p.value(optDur).toInt());
    if (p.isSet(optSec) || p.isSet(optPsk)) {
        const QString m = p.value(optSec).toLower();
        const int mode = m == QLatin1String("psk") ? 1 : (m == QLatin1String("ecdh") ? 2 : 0);
        w.setSecurity(mode, p.value(optPsk));
    }
    if (p.isSet(optListen))
        w.startListening(p.value(optListen).toUShort());
    w.show();
    if (p.isSet(optInject)) {
        const QStringList a = p.value(optInject).split(QLatin1Char(','));
        if (a.size() == 4) {
            QTimer::singleShot(0, &w, [&w, a] {
                w.injectValues(a[0].toDouble(), a[1].toDouble(), a[2].toDouble(),
                               a[3].toDouble());
            });
        }
    }
    return app.exec();
}
