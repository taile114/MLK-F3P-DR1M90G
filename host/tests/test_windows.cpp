#include <QtTest>
#include <QFontDatabase>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QDir>
#include "klocalwindow.h"
#include "datasource.h"
#include "mainwindow.h"
#include "theme.h"
#include "workorders.h"
#include "sensorpanel.h"
#include "faultstore.h"
#include "capturedialog.h"
#include <QTableView>
#include "receiver.h"
#include "sharedconfig.h"
#include "alarmcontroller.h"
#include "modelfault.h"
#include "mlk_fault_client.h"
#include <future>
#include <QLocalSocket>
#include "plsettingsdialog.h"
#include <QComboBox>
#include <QLineEdit>
#include <QDialog>
#include <QScrollArea>
#include "wifidialog.h"
#include "ifaddr.h"
#include <QSocketNotifier>
#include <QListWidget>
#include <QScrollBar>
#include <QWheelEvent>
#include <QSlider>
#include <QProgressBar>
#include "plotwidget.h"
#include "networkstatus.h"
#include "boardtime.h"
#include "replaypanel.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

class WindowTest : public QObject {
    Q_OBJECT
private slots:
    void localNpuSidebar() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.path());
        QCoreApplication::setOrganizationName("MLK-Npu-Tests");
        QCoreApplication::setApplicationName("ps");
        qputenv("MLK_FAULT_SOCKET",(dir.path()+"/fault.sock").toUtf8());
        qputenv("MLK_CAPTURE_PS_SOCKET",(dir.path()+"/capture.sock").toUtf8());
        mlk::AppConfig cfg; cfg.storageDir=dir.path(); cfg.autoDiscover=false;
        cfg.autoConnect=false; QVERIFY(cfg.save());
        const auto font=QString::fromLocal8Bit(qgetenv("MLK_TEST_FONT"));
        if(!font.isEmpty()) QVERIFY(QFontDatabase::addApplicationFont(font)>=0);
        applyTheme(*qApp);
        KLocalWindow board; board.resize(1920,1080); board.show();
        auto *label=board.findChild<QLabel *>("npuStatus"); QVERIFY(label);
        auto *sidebar=board.findChild<QWidget *>("sysSidebar"); QVERIFY(sidebar);
        sidebar->show();
        bool hasNpuTitle=false;
        for(auto *item:sidebar->findChildren<QLabel *>()) {
            QVERIFY(item->text()!=QStringLiteral("GPU"));
            if(item->text()==QStringLiteral("NPU")) hasNpuTitle=true;
        }
        QVERIFY(hasNpuTitle);
        mlk::NpuMonitor monitor;
        const QByteArray irqs=" CPU0 CPU1\n 41: 100 0 GICv3 144 Level HARD_NPU_IRQ\n"
                              " 42: 0 0 GICv3 129 Level SOFT_NPU_IRQ\n";
        monitor.sample(true,true,true,irqs,0);
        mlk::SystemStats stats;
        stats.npu=monitor.sample(true,true,true,
            QByteArray(irqs).replace("100 0","133 0"),1000);
        stats.gpuText=QStringLiteral("不可用（无 GPU 设备）");
        stats.network={QStringLiteral("wifi"),QStringLiteral("wlan0")};
        QVERIFY(QMetaObject::invokeMethod(&board,"onSystemStats",Qt::DirectConnection,
                                         Q_ARG(mlk::SystemStats,stats)));
        QVERIFY(label->text().contains(QStringLiteral("检测到中断活动")));
        QVERIFY(label->text().contains(QStringLiteral("硬核：33.0 次/s")));
        auto *npuBar=board.findChild<QProgressBar *>("npuBar"); QVERIFY(npuBar);
        QCOMPARE(npuBar->format(),QStringLiteral("使用率：驱动未提供"));
        QVERIFY(!npuBar->isEnabled());
        QCOMPARE(npuBar->height(),board.findChild<QProgressBar *>("cpuBar")->height());
        auto *plot=board.findChild<mlk::PlotWidget *>("npuPlot"); QVERIFY(plot);
        QCOMPARE(plot->samples().last(),qint16(330));
        QCOMPARE(plot->series(1).last(),qint16(0));
        auto *network=board.findChild<QLabel *>("networkConnectionStatus"); QVERIFY(network);
        QCOMPARE(network->text(),QStringLiteral("网络：WiFi"));
        mlk::NetworkStatus eth; eth.kind=QStringLiteral("ethernet"); eth.interface=QStringLiteral("eth0");
        QCOMPARE(eth.text(),QStringLiteral("网络：千兆网"));
        // PPPoE 在 eth0 上建立的 ppp0 属于千兆网，不能被识别成 4G。
        const QVector<mlk::NetworkLink> pppLinks{{QStringLiteral("ppp0"),true,false,QString()}};
        const auto ppp = mlk::selectNetworkStatus("Iface\tDestination\tGateway\tFlags\tRefCnt\tUse\tMetric\tMask\tMTU\tWindow\tIRTT\n"
                                                  "ppp0\t00000000\t00000000\t0001\t0\t0\t0\t00000000\t0\t0\t0\n",
                                                  pppLinks);
        QCOMPARE(ppp.kind,QStringLiteral("ethernet"));
        QCOMPARE(ppp.text(),QStringLiteral("网络：千兆网"));
        // 点到点接口的 ip 输出没有紧跟前缀长度，地址仍应被解析出来。
        QCOMPARE(mlk::ipv4AddressFromIpOutput(QStringLiteral(
                     "22: ppp0    inet 100.67.27.233 peer 100.67.0.1/32 scope global ppp0")),
                 QStringLiteral("100.67.27.233"));
        QCOMPARE(mlk::ipv4AddressFromIpOutput(QStringLiteral(
                     "2: eth0    inet 100.67.234.111/16 brd 100.67.255.255 scope global eth0")),
                 QStringLiteral("100.67.234.111/16"));
        auto *connection=board.findChild<QLabel *>("remoteConnectionStatus"); QVERIFY(connection);
        QVERIFY(network->geometry().bottom()<connection->geometry().top());
        QCOMPARE(network->geometry().left(),connection->geometry().left());
        QCOMPARE(network->font().pixelSize(),board.findChild<QLabel *>("cpuSummary")->font().pixelSize());
        // A second sample makes the same grid renderer as CPU/memory draw history.
        stats.npu.hardInterruptsPerSec=25;
        QVERIFY(QMetaObject::invokeMethod(&board,"onSystemStats",Qt::DirectConnection,
                                         Q_ARG(mlk::SystemStats,stats)));
        QApplication::processEvents();
        QVERIFY(label->isVisible());
        QVERIFY(sidebar->rect().contains(label->geometry()));
        QVERIFY(label->height()>=label->heightForWidth(label->width()));
        QVERIFY(sidebar->rect().contains(plot->geometry()));
        QVERIFY(plot->height()>=60);
        const auto output=qEnvironmentVariable("MLK_TEST_OUTPUT");
        if(!output.isEmpty()) {
            QVERIFY(QDir().mkpath(output));
            QVERIFY(board.grab().save(output+"/local-npu-sidebar.png"));
        }
        stats.npu=monitor.sample(false,true,false,irqs,2000);
        QVERIFY(QMetaObject::invokeMethod(&board,"onSystemStats",Qt::DirectConnection,
                                         Q_ARG(mlk::SystemStats,stats)));
        QVERIFY(label->text().contains(QStringLiteral("缺少 硬核、CMA")));
        QVERIFY(plot->samples().isEmpty());
    }
    void communicationTimingAndReceiveStyle() {
        QTemporaryDir dir;QVERIFY(dir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.path());
        QCoreApplication::setOrganizationName("MLK-Timing-Tests");QCoreApplication::setApplicationName("pc");
        qputenv("MLK_CAPTURE_PC_SOCKET",(dir.path()+"/capture.sock").toUtf8());
        mlk::AppConfig cfg;cfg.port=19840;cfg.targetPort=19841;cfg.targetAddr="127.0.0.1";
        cfg.autoDiscover=false;cfg.storageDir=dir.path();cfg.secMode=1;
        const QByteArray secret(32,'r');cfg.pskHex=secret.toHex();QVERIFY(cfg.save());
        mlk::SecureChannel security;security.configure(mlk::SecureChannel::Psk,secret,{},{},{});
        mlk::Receiver peer;peer.setChannel(&security);QVERIFY(peer.start(QHostAddress::LocalHost,19841));
        QJsonObject pending;
        connect(&peer,&mlk::Receiver::frameReceived,this,[&](const mlk::Frame &frame) {
            if (frame.type==mlk::kTypeConfig && frame.object.value("op")=="get") pending=frame.object;
        });
        MainWindow pc;pc.show();
        auto *rtt=pc.findChild<QLabel *>("communicationRtt");
        auto *age=pc.findChild<QLabel *>("sampleTimeDifference");
        auto *status=pc.findChild<QLabel *>("connectionStatus");
        QVERIFY(rtt && age && status);
        QTRY_VERIFY(!pending.value("request").toString().isEmpty());
        auto reply=mlk::sharedConfig(cfg);reply.insert("op","snapshot");reply.insert("request","unmatched");
        QVERIFY(peer.send(QHostAddress::LocalHost,19840,mlk::buildObjectFrame(mlk::kTypeConfig,reply))>0);
        QTest::qWait(20);QMetaObject::invokeMethod(&pc,"onTick",Qt::DirectConnection);
        QVERIFY(rtt->text().contains(QChar(0x2014)));
        reply.insert("request",pending.value("request"));
        QTest::qWait(50);
        QVERIFY(peer.send(QHostAddress::LocalHost,19840,mlk::buildObjectFrame(mlk::kTypeConfig,reply))>0);
        QTRY_VERIFY(status->text().contains(QStringLiteral("加密已连接")));
        QTest::qWait(30);QMetaObject::invokeMethod(&pc,"onTick",Qt::DirectConnection);
        QVERIFY(!rtt->text().contains(QChar(0x2014)));const auto measured=rtt->text();
        const auto match=QRegularExpression("([0-9]+\\.[0-9]+) ms").match(measured);
        QVERIFY(match.hasMatch());QVERIFY(match.captured(1).toDouble()>=50);
        class StyleCounter:public QObject {
        public:int changes=0;
        protected:bool eventFilter(QObject *,QEvent *event) override {
            if(event->type()==QEvent::StyleChange)++changes;
            return false;
        }
        } counter;
        status->installEventFilter(&counter);
        auto *receiver=pc.findChild<mlk::Receiver *>();QVERIFY(receiver);
        mlk::Frame wave;wave.ver=mlk::kWaveformVersion;wave.type=mlk::kTypeWaveform;
        wave.ch=mlk::kVibration;wave.rateHz=3200;wave.tsUs=mlk::captureNowUs()-2000000;
        wave.samples={1,2};wave.samplesY={3,4};wave.samplesZ={5,6};
        for(int i=0;i<200;++i) {
            wave.seq=quint32(i);
            QVERIFY(QMetaObject::invokeMethod(receiver,"frameReceived",Qt::DirectConnection,Q_ARG(mlk::Frame,wave)));
        }
        QMetaObject::invokeMethod(&pc,"onTick",Qt::DirectConnection);
        QCOMPARE(counter.changes,0);QCOMPARE(rtt->text(),measured);
        const auto ageValue=QRegularExpression("([0-9]+\\.[0-9]+) ms").match(age->text());
        QVERIFY(ageValue.hasMatch());QVERIFY(ageValue.captured(1).toDouble()>=2000);
        QCOMPARE(pc.findChild<SensorPanel *>("channel2")->buffer().size(),400);
        qunsetenv("MLK_CAPTURE_PC_SOCKET");
    }
    void captureReplayRequiresActivation() {
        QTemporaryDir dir;QVERIFY(dir.isValid());
        mlk::AppConfig cfg;cfg.storageDir=dir.path();cfg.passphrase="replay-click-test";
        mlk::CaptureStore store(cfg.storageDir,cfg.passphrase);
        QStringList paths;
        for (int i=0;i<30;++i) {
            mlk::Capture capture;capture.id=QStringLiteral("click-%1").arg(i);
            capture.source="test";capture.startUs=1700000000000000ULL+quint64(i)*1000000;
            capture.endUs=capture.startUs+1000000;
            mlk::Frame frame;frame.ch=mlk::kTemp;frame.rateHz=100;frame.tsUs=capture.startUs;frame.samples={qint16(i),qint16(i+1)};
            capture.blocks.append(frame);QString path;QVERIFY(store.save(capture,&path));paths.append(path);
        }
        mlk::CaptureDialog dialog(cfg);dialog.show();
        auto *filter=dialog.findChild<QComboBox *>("captureFilter");
        auto *list=dialog.findChild<QListWidget *>("captureList");
        auto *table=dialog.findChild<QTableView *>("captureData");
        auto *details=dialog.findChild<QLabel *>("captureDetails");
        auto *play=dialog.findChild<QPushButton *>("capturePlay");
        auto *position=dialog.findChild<QSlider *>("capturePosition");
        QVERIFY(filter && list && table && details && play && position);
        filter->setCurrentIndex(1);QCoreApplication::processEvents();
        QCOMPARE(list->count(),30);QCOMPARE(table->model()->rowCount(),0);
        QSignalSpy resets(table->model(),&QAbstractItemModel::modelReset);
        list->setCurrentRow(1);QTest::mouseMove(list->viewport(),list->visualItemRect(list->item(2)).center());
        QCoreApplication::processEvents();QCOMPARE(resets.count(),0);
        auto clickRow=[&](int row) {
            list->scrollToItem(list->item(row));QCoreApplication::processEvents();
            QTest::mouseClick(list->viewport(),Qt::LeftButton,Qt::NoModifier,list->visualItemRect(list->item(row)).center());
        };
        clickRow(0);QVERIFY(table->model()->rowCount()>0);
        const auto loaded=details->text();QVERIFY(loaded.contains("click-29"));
        resets.clear();position->setValue(5000);play->click();QVERIFY(play->isChecked());
        list->setCurrentRow(2);
        for (int row=1;row<=4;++row) QTest::mouseMove(list->viewport(),list->visualItemRect(list->item(row)).center());
        const QPoint point=list->viewport()->rect().center();
        QWheelEvent wheel(point,list->viewport()->mapToGlobal(point),QPoint(),QPoint(0,-120),
            Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
        QApplication::sendEvent(list->viewport(),&wheel);QCoreApplication::processEvents();
        QCOMPARE(details->text(),loaded);QCOMPARE(resets.count(),0);QVERIFY(play->isChecked());
        const int scroll=list->verticalScrollBar()->value();QVERIFY(scroll>0);
        const int playbackPosition=position->value();
        dialog.refresh();QCOMPARE(resets.count(),0);QVERIFY(play->isChecked());
        QCOMPARE(position->value(),playbackPosition);QCOMPARE(list->verticalScrollBar()->value(),scroll);
        play->click();
        QFile added(dir.filePath("directory-change.tmp"));QVERIFY(added.open(QIODevice::WriteOnly));added.close();
        QTest::qWait(80);QCOMPARE(resets.count(),0);QCOMPARE(details->text(),loaded);
        clickRow(0);QCOMPARE(resets.count(),0); // The loaded immutable segment is reused.
        clickRow(1);QVERIFY(details->text().contains("click-28"));QVERIFY(resets.count()>0);
        resets.clear();list->setCurrentRow(2);QCOMPARE(resets.count(),0);
        QTest::keyClick(list,Qt::Key_Return);QVERIFY(details->text().contains("click-27"));
        filter->setCurrentIndex(0);QCOMPARE(list->count(),0);QCOMPARE(table->model()->rowCount(),0);
        filter->setCurrentIndex(1);QCOMPARE(table->model()->rowCount(),0);
        dialog.openCapture(paths.first());QVERIFY(details->text().contains("click-0"));
        QVERIFY(table->model()->rowCount()>0);
    }
    void fourSensorCaptureAndFaultReplay() {
        QTemporaryDir dir;QVERIFY(dir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.path());
        QCoreApplication::setOrganizationName("MLK-Capture-Tests");QCoreApplication::setApplicationName("board");
        qputenv("MLK_RUL_AUTOSTART","0");qputenv("MLK_FAULT_SOCKET",(dir.path()+"/fault.sock").toUtf8());
        qputenv("MLK_CAPTURE_PS_SOCKET",(dir.path()+"/capture-ps.sock").toUtf8());
        qputenv("MLK_CAPTURE_PC_SOCKET",(dir.path()+"/capture-pc.sock").toUtf8());
        mlk::AppConfig cfg;cfg.port=19500;cfg.targetPort=19501;cfg.targetAddr="127.0.0.1";
        cfg.autoDiscover=false;cfg.storageDir=dir.path()+"/board";cfg.passphrase="board-secret";
        cfg.minFreeMb=0;cfg.recordSegmentSec=3;cfg.secMode=1;cfg.pskHex=QString(64,'c');QVERIFY(cfg.save());
        KLocalWindow board;board.show();
        QCoreApplication::setApplicationName("remote");
        cfg.settingsApplication="remote";cfg.port=19501;cfg.targetPort=19500;
        cfg.storageDir=dir.path()+"/pc";cfg.passphrase="pc-secret";QVERIFY(cfg.save());
        MainWindow pc;pc.show();QTest::qWait(200);
        board.findChild<QPushButton *>("recordButton")->click();pc.findChild<QPushButton *>("recordButton")->click();
        const auto ts=mlk::captureNowUs()-40000;
        for (int ch=0;ch<4;++ch) {
            mlk::Frame f;f.ch=quint8(ch);f.tsUs=ts;f.rateHz=50;f.samples={qint16(100+ch),qint16(200+ch),qint16(-300-ch)};
            if(ch==mlk::kVibration){
                f.samples.clear();f.samplesY.clear();f.samplesZ.clear();f.tsUs=ts-5940000;
                for(int i=0;i<300;++i){f.samples.append(qint16(200*std::sin(i*0.1)));f.samplesY.append(qint16(150*std::cos(i*0.1)));f.samplesZ.append(qint16(i%30-15));}
            }
            QVERIFY(QMetaObject::invokeMethod(&board,"onFrame",Qt::DirectConnection,Q_ARG(mlk::Frame,f)));
        }
        QTRY_COMPARE(pc.findChild<SensorPanel *>("channel3")->buffer().size(),3);
        const auto eventUs=mlk::captureNowUs();
        QVERIFY(board.reportFault("capture-fault",eventUs,1,2,"recorded event"));
        QJsonObject order;
        QTRY_VERIFY(mlk::WorkOrders(cfg.storageDir,cfg.passphrase).load("capture-fault",order));
        board.findChild<QPushButton *>("recordButton")->click();pc.findChild<QPushButton *>("recordButton")->click();
        const auto boardFiles=mlk::CaptureStore(dir.path()+"/board","board-secret").list();
        const auto pcFiles=mlk::CaptureStore(cfg.storageDir,cfg.passphrase).list();
        QCOMPARE(boardFiles.size(),1);QCOMPARE(pcFiles.size(),1);
        mlk::Capture local,remote;QString error;
        QVERIFY(mlk::CaptureStore(dir.path()+"/board","board-secret").load(boardFiles[0].path,local,&error));
        QVERIFY(mlk::CaptureStore(cfg.storageDir,cfg.passphrase).load(pcFiles[0].path,remote,&error));
        QCOMPARE(local.sampleCount(),qint64(909));QCOMPARE(remote.sampleCount(),local.sampleCount());
        const auto a=mlk::CaptureStore::series(local),b=mlk::CaptureStore::series(remote);
        for(int ch=0;ch<6;++ch){
            QCOMPARE(a[ch].size(),ch<3?3:300);QCOMPARE(b[ch].size(),a[ch].size());
            for(int i=0;i<a[ch].size();++i){QCOMPARE(a[ch][i].tsUs,b[ch][i].tsUs);QCOMPARE(a[ch][i].value,b[ch][i].value);}
        }
        QVERIFY(!mlk::CaptureStore(cfg.storageDir,"wrong").load(pcFiles[0].path,remote));
        auto *live=pc.findChild<SensorPanel *>("channel0");const auto liveBuffer=live->buffer();
        pc.findChild<QPushButton *>("captureReplayButton")->click();
        auto *dialog=pc.findChild<mlk::CaptureDialog *>();QVERIFY(dialog);QVERIFY(dialog->isVisible());
        QCOMPARE(dialog->findChild<QListWidget *>("captureList")->count(),1);
        for(int ch=0;ch<4;++ch)QVERIFY(dialog->findChild<QWidget *>(QStringLiteral("capturePlot%1").arg(ch)));
        auto *captureList=dialog->findChild<QListWidget *>("captureList");
        QCOMPARE(dialog->findChild<QTableView *>("captureData")->model()->rowCount(),0);
        QTest::mouseClick(captureList->viewport(),Qt::LeftButton,Qt::NoModifier,captureList->visualItemRect(captureList->item(0)).center());
        QVERIFY(dialog->findChild<QTableView *>("captureData")->model()->rowCount()>0);
        dialog->openOrder("capture-fault");QCOMPARE(dialog->findChild<QComboBox *>("captureRelated")->count(),1);
        QVERIFY(dialog->findChild<QLabel *>("captureDetails")->text().contains("capture-fault"));
        QCOMPARE(live->buffer(),liveBuffer); // offline reading does not mutate the live dashboard
        dialog->findChild<QPushButton *>("capturePlay")->click();QTest::qWait(200);
        dialog->findChild<QPushButton *>("capturePlay")->click();
        const auto output=QString::fromLocal8Bit(qgetenv("MLK_TEST_OUTPUT"));
        if(!output.isEmpty())QVERIFY(dialog->grab().save(output+"/capture-pc.png"));
        dialog->close();
        const auto missing=QJsonObject{{"id","missing-data"},{"faultTimeUs",QString::number(ts-10000000)},
            {"class",1},{"grade",1},{"message","no matching acquisition"}};
        QVERIFY(mlk::WorkOrders(cfg.storageDir,cfg.passphrase).save(missing));
        dialog->openOrder("missing-data");
        QVERIFY(dialog->findChild<QLabel *>("captureStatus")->text().contains(QStringLiteral("无关联采集记录")));
        QVERIFY(!dialog->findChild<QPushButton *>("captureExport")->isEnabled());
        board.findChild<QPushButton *>("captureReplayButton")->click();
        auto *boardDialog=board.findChild<mlk::CaptureDialog *>();QVERIFY(boardDialog);
        QCOMPARE(boardDialog->findChild<QListWidget *>("captureList")->count(),1);
        auto *boardList=boardDialog->findChild<QListWidget *>("captureList");
        QCOMPARE(boardDialog->findChild<QTableView *>("captureData")->model()->rowCount(),0);
        QTest::mouseClick(boardList->viewport(),Qt::LeftButton,Qt::NoModifier,boardList->visualItemRect(boardList->item(0)).center());
        QVERIFY(boardDialog->findChild<QTableView *>("captureData")->model()->rowCount()>0);
        if(!output.isEmpty())QVERIFY(boardDialog->grab().save(output+"/capture-ps.png"));
        // The trainer's local socket accepts arbitrary taxonomies, with persisted labels.
        QLocalSocket socket;socket.connectToServer(dir.path()+"/capture-pc.sock");QVERIFY(socket.waitForConnected(1000));
        socket.write("{\"api\":1,\"op\":\"list\",\"abnormalOnly\":true}\n");socket.flush();
        QTRY_VERIFY(socket.bytesAvailable()>0);
        const auto reply=QJsonDocument::fromJson(socket.readAll()).object();QVERIFY(reply.value("ok").toBool());
        QCOMPARE(reply.value("records").toArray().size(),1);
        boardDialog->close();dialog->close();
        board.findChild<QPushButton *>("recordButton")->click();pc.findChild<QPushButton *>("recordButton")->click();
        QTimer samples;samples.setInterval(100);
        connect(&samples,&QTimer::timeout,&board,[&] {
            mlk::Frame f;f.ch=mlk::kTemp;f.samples={250};f.rateHz=10;
            QMetaObject::invokeMethod(&board,"onFrame",Qt::DirectConnection,Q_ARG(mlk::Frame,f));
        });
        samples.start();QTest::qWait(6700);samples.stop();
        board.findChild<QPushButton *>("recordButton")->click();pc.findChild<QPushButton *>("recordButton")->click();
        // Configuration snapshots arrive every 2 s; they must not restart the
        // 3 s recording timer and prevent periodic segment commits.
        QVERIFY(mlk::CaptureStore(cfg.storageDir,cfg.passphrase).list().size()>=4);
        QVERIFY(mlk::CaptureStore(dir.path()+"/board","board-secret").list().size()>=4);
        qunsetenv("MLK_CAPTURE_PS_SOCKET");qunsetenv("MLK_CAPTURE_PC_SOCKET");
    }
    void physicalAlarmPolicyAndModelApi() {
        mlk::AppConfig cfg;
        for(int ch=0;ch<4;++ch) { cfg.ch[ch].alarmEnabled=true; cfg.ch[ch].alarmLo=-1; cfg.ch[ch].alarmHi=1; cfg.ch[ch].k=0.1; cfg.ch[ch].b=0; }
        AlarmController alarm; alarm.configure(cfg);
        mlk::Frame f; f.type=mlk::kTypeWaveform; f.samples={10};
        f.ch=mlk::kTemp; alarm.observe(f); QCOMPARE(alarm.desiredState(),0u); // inclusive bound
        f.samples={11}; alarm.observe(f); QCOMPARE(alarm.desiredState(),1u);
        f.ch=mlk::kCurrent; f.samples={2}; alarm.observe(f); QCOMPARE(alarm.desiredState(),3u); // mA fixed scale
        f.ch=mlk::kVibration; f.samples={0}; f.samplesY={-11}; f.samplesZ={0};
        alarm.observe(f); QCOMPARE(alarm.desiredState(),7u); // any axis
        f.ch=mlk::kSpeed; f.samples={-11}; f.samplesY.clear(); f.samplesZ.clear();
        alarm.observe(f); QCOMPARE(alarm.desiredState(),15u);
        alarm.modelGrade(3); QCOMPARE(alarm.desiredState(),63u);
        alarm.invalidate(mlk::kCurrent); QCOMPARE(alarm.desiredState(),61u);
        cfg.ch[0].alarmEnabled=false; alarm.configure(cfg); QCOMPARE(alarm.desiredState(),60u);
        QTest::qWait(2050); QCOMPARE(alarm.desiredState(),48u); // stale samples never remain live
        alarm.modelGrade(0); QCOMPARE(alarm.desiredState(),0u);

        QTemporaryDir dir; QVERIFY(dir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.path());
        QCoreApplication::setOrganizationName("MLK-Alarm-Tests"); QCoreApplication::setApplicationName("ps");
        cfg.settingsOrganization=QCoreApplication::organizationName(); cfg.settingsApplication=QCoreApplication::applicationName();
        cfg.storageDir=dir.path(); cfg.passphrase="alarm-test"; cfg.port=19818;
        cfg.autoDiscover=false; QVERIFY(cfg.save());
        const auto path=dir.path()+"/fault.sock";
        qputenv("MLK_FAULT_SOCKET",path.toUtf8()); qputenv("MLK_RUL_AUTOSTART","0");
        KLocalWindow board;
        QJsonObject state{{"api",1},{"valid",true},{"class",1},{"grade",2},
            {"id","model-test-1"},{"faultTimeUs","1700000000000000"},{"message","test classification"}};
        auto request=[&](const QByteArray &json, bool accepted=true) {
            QLocalSocket socket; socket.connectToServer(path); QVERIFY(socket.waitForConnected(1000));
            socket.write(json+'\n'); socket.flush();
            QTRY_VERIFY(socket.bytesAvailable()>0);
            auto reply=QJsonDocument::fromJson(socket.readAll()).object();
            QCOMPARE(reply.value("ok").toBool(),accepted);
        };
        request(QJsonDocument(state).toJson(QJsonDocument::Compact));
        const auto mode=QFileInfo(path).permissions();
        QVERIFY(!(mode & (QFileDevice::ReadGroup|QFileDevice::WriteGroup|QFileDevice::ReadOther|QFileDevice::WriteOther)));
        request("{bad json}",false);
        char clientReply[1024]={};
        const auto socketPath=path.toUtf8();
        const auto jsonRequest=QJsonDocument(state).toJson(QJsonDocument::Compact);
        auto client=std::async(std::launch::async,[&] {
            return mlk_fault_publish_json(socketPath.constData(),jsonRequest.constData(),clientReply,sizeof(clientReply),2000);
        });
        QTRY_VERIFY(client.wait_for(std::chrono::milliseconds(0))==std::future_status::ready);
        QCOMPARE(client.get(),0);
        QVERIFY(QJsonDocument::fromJson(clientReply).object().value("ok").toBool());
        QVERIFY(board.reportModelState(state)); // idempotent heartbeat
        auto *controller=board.findChild<AlarmController *>(); QVERIFY(controller);
        QCOMPARE(controller->desiredState()>>4,2u);
        QJsonObject order; QVERIFY(mlk::WorkOrders(dir.path(),"alarm-test").load("model-test-1",order));
        QCOMPARE(order.value("grade").toInt(),2);
        auto invalid=state; invalid.insert("grade",4); QVERIFY(!board.reportModelState(invalid));
        invalid=state; invalid.insert("grade",3); QVERIFY(!board.reportModelState(invalid)); // immutable ID
        invalid=state; invalid.insert("valid",1); QVERIFY(!board.reportModelState(invalid));
        invalid=state; invalid.insert("class",1.5); QVERIFY(!board.reportModelState(invalid));
        invalid=state; invalid.insert("id","../bad"); QVERIFY(!board.reportModelState(invalid));
        invalid=state; invalid.insert("grade",0); invalid.insert("id","ungraded");
        QVERIFY(board.reportModelState(invalid)); QCOMPARE(controller->desiredState()>>4,1u);
        QJsonObject healthy{{"api",1},{"valid",true},{"class",0},{"grade",0}};
        QVERIFY(board.reportModelState(healthy)); QCOMPARE(controller->desiredState()>>4,0u);
        QVERIFY(board.reportModelState(state));
        QTest::qWait(10300); QCOMPARE(controller->desiredState()>>4,0u); // publisher lease
        qunsetenv("MLK_FAULT_SOCKET");
    }
    void pairingAndAutomaticListen() {
        QTemporaryDir config; QVERIFY(config.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,config.path());
        QCoreApplication::setOrganizationName("MLK-Pairing-Tests"); QCoreApplication::setApplicationName("pc");
        mlk::AppConfig cfg;
        QVERIFY(cfg.ensureIdentity());
        cfg.port=19811; cfg.targetAddr="127.0.0.1"; cfg.targetPort=19812; cfg.autoDiscover=false; cfg.storageDir=config.path(); QVERIFY(cfg.save());
        {
            MainWindow unpaired;
            QTRY_COMPARE(unpaired.findChild<QLabel *>("connectionStatus")->text(),QStringLiteral("等待配对"));
            mlk::Frame legacy; legacy.ver=2; legacy.type=mlk::kTypeDiagnosis; legacy.cls=2; legacy.grade=3;
            QVERIFY(QMetaObject::invokeMethod(&unpaired,"onFrame",Qt::DirectConnection,Q_ARG(mlk::Frame,legacy)));
            QVERIFY(unpaired.findChild<QLabel *>("diagnosisRulValue")->text().contains(QStringLiteral("故障分级：3")));
        }
        mlk::AppConfig reloaded; reloaded.load(); QVERIFY(reloaded.ensureIdentity());
        QCOMPARE(reloaded.selfPubHex,cfg.selfPubHex); QCOMPARE(reloaded.selfPrivHex,cfg.selfPrivHex);
        applyTheme(*qApp);
        PlSettingsDialog dialog(cfg,nullptr,true); dialog.show();
        dialog.findChild<QPushButton *>("generateIdentity")->click();
        QByteArray peerPub,peerPriv; QVERIFY(mlk::SecureChannel::generateIdentity(peerPub,peerPriv));
        dialog.findChild<QLineEdit *>("peerPublicKey")->setText(peerPub.toHex());
        dialog.findChild<QComboBox *>("securityMode")->setCurrentIndex(2);
        dialog.accept(); QCOMPARE(dialog.result(),int(QDialog::Accepted));
        QVERIFY(cfg.autoConnect); QVERIFY(!cfg.autoDiscover);
        mlk::SecureChannel a,b;
        a.configure(mlk::SecureChannel::Ecdh,{},QByteArray::fromHex(cfg.selfPubHex.toLatin1()),QByteArray::fromHex(cfg.selfPrivHex.toLatin1()),peerPub);
        b.configure(mlk::SecureChannel::Ecdh,{},peerPub,peerPriv,QByteArray::fromHex(cfg.selfPubHex.toLatin1()));
        QCOMPARE(a.discoveryKey(),b.discoveryKey()); QVERIFY(a.discoveryKey().size()==32);
        cfg.port=19811; cfg.targetAddr="127.0.0.1"; cfg.targetPort=19812; cfg.autoDiscover=false;
        cfg.storageDir=config.path(); QVERIFY(cfg.save());
        MainWindow remote;
        auto *receiver=remote.findChild<mlk::Receiver *>(); QVERIFY(receiver);
        QTRY_VERIFY(receiver->isListening()); QCOMPARE(receiver->localPort(),quint16(19811));
        const auto output=QString::fromLocal8Bit(qgetenv("MLK_TEST_OUTPUT"));
        if(!output.isEmpty()) {
            dialog.resize(1000,900); dialog.show();
            dialog.findChild<QScrollArea *>()->ensureWidgetVisible(dialog.findChild<QLineEdit *>("peerPublicKey"));
            QTest::qWait(50); QVERIFY(dialog.grab().save(output+"/pairing-settings.png"));
        }
    }
    void hardwareRemoteWindow() {
        const auto path=qgetenv("MLK_HW_CONFIG");
        if(path.isEmpty()) QSKIP("Set MLK_HW_CONFIG for the opt-in physical WiFi test");
        QFile file(QString::fromLocal8Bit(path)); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto cfg=QJsonDocument::fromJson(file.readAll()).object();
        QTemporaryDir config; QVERIFY(config.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,config.path());
        QCoreApplication::setOrganizationName("MLK-Hardware-Tests");
        QCoreApplication::setApplicationName("remote");
        QSettings s;
        s.setValue("app/secMode",cfg.value("mode").toInt());
        s.setValue("app/pskHex",cfg.value("psk").toString());
        s.setValue("app/selfPubHex",cfg.value("selfPub").toString());
        s.setValue("app/selfPrivHex",cfg.value("selfPriv").toString());
        s.setValue("app/peerPubHex",cfg.value("peerPub").toString());
        const QString records=QString::fromLocal8Bit(qgetenv("MLK_TEST_OUTPUT"))+"/hardware-records";
        s.setValue("app/storageDir",records); s.setValue("app/minFreeMb",0);
        s.setValue("app/passphrase","hardware-window-password");
        s.setValue("app/recordSegmentSec",1); s.sync();
        applyTheme(*qApp);
        MainWindow remote; remote.resize(1920,1080);
        remote.setTarget(cfg.value("board").toString(),8000); remote.startListening(8000); remote.show();
        auto *status=remote.findChild<QLabel *>("connectionStatus"); QVERIFY(status);
        QTRY_COMPARE_WITH_TIMEOUT(status->text(),QStringLiteral("加密已连接"),15000);
        for(int i=0;i<4;++i) {
            auto *panel=remote.findChild<SensorPanel *>(QString("channel%1").arg(i)); QVERIFY(panel);
            QTRY_VERIFY_WITH_TIMEOUT(!panel->buffer().isEmpty(),15000);
        }
        auto *record=remote.findChild<QPushButton *>("recordButton"); QVERIFY(record);
        record->click(); QTest::qWait(3500); record->click();
        const auto saved=QDir(records).entryList({"capture_*.mlkc"},QDir::Files); QVERIFY(!saved.isEmpty());
        mlk::Capture capture;
        const auto savedPath=QDir(records).filePath(saved.front());
        QVERIFY(mlk::CaptureStore(records,"hardware-window-password").load(savedPath,capture));
        QVERIFY(!mlk::CaptureStore(records,"wrong-password").load(savedPath,capture));
        QVERIFY(remote.findChild<QLabel *>("diagnosisRulValue")->text().contains(QStringLiteral("故障时间：")));
        const auto output=QString::fromLocal8Bit(qgetenv("MLK_TEST_OUTPUT"));
        if(!output.isEmpty()) QVERIFY(remote.grab().save(output+"/hardware-pc.png"));
    }
    void actualWindowsExchangeControl() {
        QTemporaryDir config; QVERIFY(config.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,config.path());
        QCoreApplication::setOrganizationName(QStringLiteral("MLK-Rul-Tests"));
        QCoreApplication::setApplicationName(QStringLiteral("local"));
        QSettings local;
        local.setValue("app/autoDiscover",false); local.setValue("app/autoConnect",false);
        local.setValue("app/port",18000); local.setValue("app/targetAddr","127.0.0.1");
        local.setValue("app/targetPort",18001); local.setValue("app/storageDir",config.path());
        local.setValue("app/secMode",1); local.setValue("app/pskHex",QString(64,'a'));
        local.setValue("app/passphrase","board-password");
        local.setValue("app/minFreeMb",0); local.setValue("rul/modelDirectory",config.path()+"/missing"); local.sync();
        const auto font=QString::fromLocal8Bit(qgetenv("MLK_TEST_FONT"));
        if (!font.isEmpty()) QVERIFY(QFontDatabase::addApplicationFont(font)>=0);
        applyTheme(*qApp);
        qputenv("MLK_FAULT_SOCKET",(config.path()+"/fault.sock").toUtf8());
        KLocalWindow board; board.resize(1920,1080); board.show();
        QCoreApplication::setApplicationName(QStringLiteral("remote"));
        QSettings pc;
        pc.setValue("app/autoDiscover",false); pc.setValue("app/autoConnect",false);
        pc.setValue("app/secMode",1); pc.setValue("app/pskHex",QString(64,'a'));
        pc.setValue("app/passphrase","remote-password");
        pc.setValue("app/storageDir",config.path()+"/pc"); pc.setValue("app/minFreeMb",0); pc.sync();
        MainWindow remote; remote.resize(1920,1080); remote.setTarget("127.0.0.1",18000);
        remote.startListening(18001); remote.show();
        auto *start=remote.findChild<QPushButton *>("rulAction1"); QVERIFY(start);
        QVERIFY(!remote.findChild<QPushButton *>("rulAction5"));
        QVERIFY(!remote.findChild<QPushButton *>("rulAction6"));
        int acknowledgements=0;
        connect(remote.findChild<mlk::Receiver *>(),&mlk::Receiver::frameReceived,this,[&](const mlk::Frame &f) {
            if(f.ver==3 && f.type==mlk::kTypeDiagnosis && f.commandStatus) ++acknowledgements;
        });
        start->click(); QTRY_COMPARE(acknowledgements,1);
        board.findChild<mlk::SystemMonitor *>()->stop();
        mlk::NpuMonitor npuMonitor;
        const QByteArray irqs=" CPU0 CPU1\n 41: 100 0 GICv3 144 Level HARD_NPU_IRQ\n"
                              " 42: 0 0 GICv3 129 Level SOFT_NPU_IRQ\n";
        npuMonitor.sample(true,true,true,irqs,0);
        mlk::SystemStats telemetry;
        telemetry.npu=npuMonitor.sample(true,true,true,QByteArray(irqs).replace("100 0","133 0"),1000);
        telemetry.network={QStringLiteral("cellular"),QStringLiteral("wwan0")};
        auto *pcNpu=remote.findChild<QLabel *>("npuStatus"); QVERIFY(pcNpu);
        auto *pcNetwork=remote.findChild<QLabel *>("networkConnectionStatus"); QVERIFY(pcNetwork);
        QVERIFY(QMetaObject::invokeMethod(&board,"onSystemStats",Qt::DirectConnection,
                                         Q_ARG(mlk::SystemStats,telemetry)));
        QTRY_VERIFY(pcNpu->text().contains(QStringLiteral("硬核：33.0 次/s")));
        QCOMPARE(pcNetwork->text(),QStringLiteral("网络：4G"));
        QCOMPARE(pcNpu->text(),board.findChild<QLabel *>("npuStatus")->text());
        auto *pcNpuPlot=remote.findChild<mlk::PlotWidget *>("npuPlot"); QVERIFY(pcNpuPlot);
        QCOMPARE(pcNpuPlot->samples().last(),qint16(330));
        QCOMPARE(remote.findChild<QProgressBar *>("npuBar")->format(),QStringLiteral("使用率：驱动未提供"));
        telemetry.npu.hardInterruptsPerSec=25;
        QVERIFY(QMetaObject::invokeMethod(&board,"onSystemStats",Qt::DirectConnection,
                                         Q_ARG(mlk::SystemStats,telemetry)));
        QTRY_VERIFY(pcNpuPlot->samples().size()>=2);
        remote.findChild<QPushButton *>("systemStatusToggle")->setChecked(true);
        QApplication::processEvents();
        auto *pcSidebar=remote.findChild<QWidget *>("sysSidebar");
        QVERIFY(pcSidebar->rect().contains(pcNpuPlot->geometry()));
        QVERIFY(pcNpuPlot->height()>=60);
        QVERIFY(pcNetwork->geometry().bottom()<remote.findChild<QLabel *>("connectionStatus")->geometry().top());
        const auto npuOutput=qEnvironmentVariable("MLK_TEST_OUTPUT");
        if (!npuOutput.isEmpty()) QVERIFY(remote.grab().save(npuOutput+"/remote-npu-sidebar.png"));
        board.findChild<mlk::SystemMonitor *>()->start();
        auto *step=remote.findChild<QPushButton *>("rulAction3"); QVERIFY(step); step->click();
        auto *state=remote.findChild<QLabel *>("rulState"); QVERIFY(state);
        QTRY_VERIFY2(state->text().contains(QStringLiteral("Model manifest")),qPrintable(state->text()));
        QVERIFY(remote.findChild<QLabel *>("diagnosisRulValue")->text().contains(QStringLiteral("故障时间：N/A")));
        QTRY_COMPARE(acknowledgements,2);
        auto *stop=remote.findChild<QPushButton *>("rulAction4"); QVERIFY(stop); stop->click();
        QTRY_COMPARE(acknowledgements,3);
        QTRY_VERIFY(state->text().contains(QStringLiteral("已停止")));
        auto *connection=remote.findChild<QLabel *>("connectionStatus"); QVERIFY(connection);
        QTRY_COMPARE(connection->text(),QStringLiteral("加密已连接"));
        auto *toggle=remote.findChild<QPushButton *>("systemStatusToggle"); QVERIFY(toggle);
        QVERIFY(connection->geometry().left()>toggle->geometry().right());
        QVERIFY(connection->geometry().left()-toggle->geometry().right()<=10);
        auto *recordButton=remote.findChild<QPushButton *>("recordButton"); QVERIFY(recordButton);
        auto *recordList=remote.findChild<QPushButton *>("recordListButton"); QVERIFY(recordList);
        auto *settingsButton=remote.findChild<QPushButton *>("settingsButton"); QVERIFY(settingsButton);
        QVERIFY(recordButton->geometry().right()<recordList->geometry().left());
        QVERIFY(recordList->geometry().right()<settingsButton->geometry().left());
        auto *boardConnection=board.findChild<QLabel *>("remoteConnectionStatus"); QVERIFY(boardConnection);
        QTRY_COMPARE(boardConnection->text(),QStringLiteral("上位机：加密已连接"));
        QVERIFY(board.findChild<QPushButton *>("networkConfig"));
        QVERIFY(!remote.findChild<QPushButton *>("networkConfig"));
        mlk::Frame waveform; waveform.type=mlk::kTypeWaveform; waveform.ch=mlk::kTemp;
        waveform.rateHz=50; waveform.samples={250}; waveform.tsUs=quint64(QDateTime::currentMSecsSinceEpoch())*1000;
        QVERIFY(QMetaObject::invokeMethod(&board,"onFrame",Qt::DirectConnection,Q_ARG(mlk::Frame,waveform)));
        auto *temperature=remote.findChild<SensorPanel *>("channel0"); QVERIFY(temperature);
        QTRY_COMPARE(temperature->lastPhysical(),25.0);
        waveform.ch=mlk::kVibration; waveform.samples=QVector<qint32>(300,256);
        waveform.samplesY=QVector<qint32>(300,512); waveform.samplesZ=QVector<qint32>(300,-256);
        QVERIFY(QMetaObject::invokeMethod(&board,"onFrame",Qt::DirectConnection,Q_ARG(mlk::Frame,waveform)));
        auto *vibration=remote.findChild<SensorPanel *>("channel2"); QVERIFY(vibration);
        QTRY_VERIFY(vibration->buffer().size()>=300);
        QCOMPARE(vibration->buffer().back(),qint16(256));
        auto *boardSpeed=board.findChild<SensorPanel *>("channel3"); QVERIFY(boardSpeed);
        auto *pcSpeed=remote.findChild<SensorPanel *>("channel3"); QVERIFY(pcSpeed);
        quint32 speedTs=1;
        for (qint32 rpm : {4000,7000,12000,-12000,0}) {
            const qint32 plMilli=-rpm*1000;
            auto *driver=board.findChild<mlk::DriverSource *>(); QVERIFY(driver);
            driver->speedChanged(plMilli,speedTs,speedTs,true,false,false,true);
            ++speedTs;
            QTRY_COMPARE(boardSpeed->lastPhysical(),double(rpm));
            QTRY_COMPARE(pcSpeed->lastPhysical(),double(rpm));
            QCOMPARE(boardSpeed->buffer().back(),rpm*10);
            QCOMPARE(pcSpeed->buffer().back(),rpm*10);
        }
        waveform.ch=mlk::kCurrent; waveform.samples={mlk::currentMilliAmp(24000,20000,1250)};
        waveform.samplesY.clear(); waveform.samplesZ.clear();
        QVERIFY(QMetaObject::invokeMethod(&board,"onFrame",Qt::DirectConnection,Q_ARG(mlk::Frame,waveform)));
        QTRY_COMPARE(remote.findChild<SensorPanel *>("channel1")->lastPhysical(),5000.0);
        QJsonObject model{{"api",1},{"valid",true},{"class",2},{"grade",3},
            {"id","encrypted-model"},{"faultTimeUs",QString::number(quint64(QDateTime::currentMSecsSinceEpoch())*1000)},
            {"message","encrypted model state"}};
        QVERIFY(board.reportModelState(model));
        QTRY_VERIFY(remote.findChild<QLabel *>("diagnosisRulValue")->text().contains(QStringLiteral("故障分级：3")));
        QVERIFY(board.reportModelState({{"api",1},{"valid",true},{"class",0},{"grade",0}}));
        QTRY_VERIFY(remote.findChild<QLabel *>("diagnosisRulValue")->text().contains(QStringLiteral("故障检测：正常")));
        const auto eventTime=quint64(QDateTime::currentMSecsSinceEpoch())*1000;
        QVERIFY(board.reportFault("test-order",eventTime,1,2,QStringLiteral("温度故障接口测试")));
        QTRY_VERIFY(mlk::WorkOrders(config.path(),"board-password").pending().isEmpty());
        QJsonObject saved;
        QVERIFY(mlk::WorkOrders(config.path()+"/pc","remote-password").load("test-order",saved));
        QCOMPARE(saved.value("faultTimeUs").toString(),QString::number(eventTime));
        QVERIFY(!mlk::WorkOrders(config.path()+"/pc","wrong").load("test-order",saved));
        QVERIFY(remote.findChild<QLabel *>("diagnosisRulValue")->text().contains(QStringLiteral("故障时间：20")));
        QVERIFY2(board.width() <= 1920 && board.height() <= 1080, "HDMI controls must fit 1080p");
        QTest::qWait(1200);
        QVERIFY(qAbs(remote.findChild<QLabel *>("dashboardTitle")->geometry().center().x()-remote.centralWidget()->width()/2)<4);
        const auto output=QString::fromLocal8Bit(qgetenv("MLK_TEST_OUTPUT"));
        if (!output.isEmpty()) {
            QVERIFY(board.grab().save(output+"/hdmi.png"));
            QVERIFY(remote.grab().save(output+"/pc.png"));
        }
        auto *receiver=remote.findChild<mlk::Receiver *>(); QVERIFY(receiver);
        model.insert("id","model-before-disconnect");
        QVERIFY(board.reportModelState(model));
        QTRY_VERIFY(remote.findChild<QLabel *>("diagnosisRulValue")->text().contains(QStringLiteral("故障分级：3")));
        receiver->stop();
        QTRY_VERIFY_WITH_TIMEOUT(remote.findChild<QLabel *>("diagnosisRulValue")->text().contains(QStringLiteral("故障分级：N/A")),7000);
        QVERIFY(pcNetwork->text().contains(QStringLiteral("板端离线")));
        QVERIFY(pcNpu->text().contains(QStringLiteral("状态超时")));
        QVERIFY(pcNpuPlot->samples().isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(boardConnection->text(),QStringLiteral("上位机：连接超时"),7000);
        QVERIFY(receiver->start(QHostAddress::LocalHost,18001));
        auto *pause=remote.findChild<QPushButton *>("rulAction2"); QVERIFY(pause); pause->click();
        QTRY_COMPARE_WITH_TIMEOUT(boardConnection->text(),QStringLiteral("上位机：加密已连接"),7000);
        mlk::Frame oldBoard;
        oldBoard.type=mlk::kTypeBoardStatus;
        QVERIFY(QMetaObject::invokeMethod(&remote,"onFrame",Qt::DirectConnection,Q_ARG(mlk::Frame,oldBoard)));
        QVERIFY(pcNpu->text().contains(QStringLiteral("未提供")));
        QVERIFY(pcNetwork->text().contains(QStringLiteral("未提供")));
        QVERIFY(pcNpuPlot->samples().isEmpty());
        board.findChild<mlk::SystemMonitor *>()->stop();
        QVERIFY(QMetaObject::invokeMethod(&board,"onSystemStats",Qt::DirectConnection,
                                         Q_ARG(mlk::SystemStats,telemetry)));
        QTRY_COMPARE(pcNetwork->text(),QStringLiteral("网络：4G"));
        QTRY_VERIFY(!pcNpuPlot->samples().isEmpty());
    }
    void wifiControlWithoutShellCredentials() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto endpoint=(directory.path()+"/wlan-test").toLocal8Bit();
        const int socket=::socket(AF_UNIX,SOCK_DGRAM | SOCK_NONBLOCK,0); QVERIFY(socket>=0);
        sockaddr_un address{}; address.sun_family=AF_UNIX;
        std::strncpy(address.sun_path,endpoint.constData(),sizeof(address.sun_path)-1);
        QVERIFY(::bind(socket,reinterpret_cast<sockaddr *>(&address),sizeof(address))==0);
        QSocketNotifier notifier(socket,QSocketNotifier::Read);
        bool connected=false,saved=false; QList<QByteArray> commands;
        connect(&notifier,&QSocketNotifier::activated,this,[&] {
            char buffer[4096]; sockaddr_un peer{}; socklen_t size=sizeof(peer);
            const auto n=::recvfrom(socket,buffer,sizeof(buffer),0,reinterpret_cast<sockaddr *>(&peer),&size);
            if(n<0) return; const QByteArray command(buffer,int(n)); commands << command;
            QByteArray reply="OK\n";
            if(command=="SCAN_RESULTS") reply="bssid / frequency / signal level / flags / ssid\n00:11:22:33:44:55\t2412\t-40\t[WPA2-PSK-CCMP][ESS]\tTest network\n";
            else if(command=="ADD_NETWORK") reply="7\n";
            else if(command=="STATUS") reply=connected?"id=7\nwpa_state=COMPLETED\nssid=Test network\nip_address=192.0.2.2\n":"id=0\nwpa_state=DISCONNECTED\n";
            else if(command=="SELECT_NETWORK 7") connected=true;
            else if(command=="SAVE_CONFIG") saved=true;
            ::sendto(socket,reply.constData(),size_t(reply.size()),0,reinterpret_cast<sockaddr *>(&peer),size);
        });
        {
            WifiDialog dialog(nullptr,directory.path(),{"wlan-test"}); dialog.show();
            QTRY_COMPARE_WITH_TIMEOUT(dialog.findChild<QListWidget *>()->count(),1,6000);
            dialog.findChild<QListWidget *>()->setCurrentRow(0);
            QCOMPARE(dialog.findChild<QLineEdit *>("wifiSsid")->text(),QStringLiteral("Test network"));
            dialog.findChild<QLineEdit *>("wifiPassword")->setText("wifi-secret");
            dialog.findChild<QPushButton *>("wifiJoin")->click();
            QTRY_VERIFY_WITH_TIMEOUT(saved,6000);
            QVERIFY(dialog.findChild<QLabel *>("wifiStatus")->text().contains("192.0.2.2"));
            for(const auto &command:commands) QVERIFY(!command.contains("wifi-secret"));
            QVERIFY(commands.contains("SET_NETWORK 7 ssid "+QByteArray("Test network").toHex()));
        }
        notifier.setEnabled(false); ::close(socket);
    }
    void settingsSaveWithoutTransportCredentials() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.path());
        QCoreApplication::setOrganizationName("MLK-Settings-Tests"); QCoreApplication::setApplicationName("remote");
        mlk::AppConfig cfg; QVERIFY(cfg.ensureIdentity());
        cfg.secMode = 2; cfg.peerPubHex.clear(); cfg.addr.clear(); cfg.targetAddr.clear();
        cfg.storageDir = dir.path();
        PlSettingsDialog dialog(cfg, nullptr, true);
        dialog.accept();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QVERIFY(cfg.peerPubHex.isEmpty());
        QVERIFY(cfg.addr.isEmpty());
    }
    void replayWindowTimeFollowsModelState() {
        mlk::ReplayPanel panel(true);
        auto *details = panel.findChild<QLabel *>("rulDetails"); QVERIFY(details);
        mlk::Frame frame; frame.ver = mlk::kInferenceVersion; frame.type = mlk::kTypeDiagnosis;
        frame.task = 1; frame.valid = 1; frame.source = 1; frame.backend = 2; frame.rulUnit = 1;
        frame.modelId = QStringLiteral("motor_stable23_v1"); frame.datasetId = QStringLiteral("val184");
        frame.windowIndex = 3; frame.windowCount = 184; frame.inferenceState = mlk::kRunning;
        frame.tsUs = 1700000000000000ULL; panel.showFrame(frame);
        const auto running = details->text();
        QVERIFY2(running.contains(QStringLiteral("2023-11-1")), qPrintable(running));
        // A paused or stopped heartbeat must not move the model window time.
        frame.inferenceState = mlk::kPaused; frame.tsUs = 1800000000000000ULL;
        panel.showFrame(frame); QCOMPARE(details->text(), running);
        frame.inferenceState = mlk::kStopped; frame.tsUs = 1900000000000000ULL;
        panel.showFrame(frame); QCOMPARE(details->text(), running);
        frame.inferenceState = mlk::kRunning; frame.tsUs = 1900000000000000ULL;
        panel.showFrame(frame); QVERIFY(details->text() != running);
    }
    void boardWallClockRunsWhileModelPaused() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        qputenv("MLK_FAULT_SOCKET",(dir.path()+"/fault.sock").toUtf8());
        qputenv("MLK_CAPTURE_PS_SOCKET",(dir.path()+"/capture.sock").toUtf8());
        qputenv("MLK_RUL_AUTOSTART","0");
        qputenv("MLK_TIME_STATUS_BIN","false");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.path());
        QCoreApplication::setOrganizationName("MLK-Clock-Tests"); QCoreApplication::setApplicationName("board");
        QSettings settings;
        settings.setValue("app/autoDiscover",false); settings.setValue("app/autoConnect",false);
        settings.setValue("app/port",19420); settings.setValue("app/targetAddr","127.0.0.1");
        settings.setValue("app/targetPort",19421); settings.setValue("app/storageDir",dir.path());
        settings.setValue("app/secMode",0); settings.sync();
        KLocalWindow board;
        auto *clock = board.findChild<QLabel *>("boardTime"); QVERIFY(clock);
        mlk::Frame paused; paused.ver = mlk::kInferenceVersion; paused.type = mlk::kTypeDiagnosis;
        paused.task = 1; paused.source = 1; paused.backend = 2; paused.inferenceState = mlk::kPaused;
        paused.windowCount = 184; paused.modelId = QStringLiteral("motor_stable23_v1");
        paused.datasetId = QStringLiteral("val184");
        QVERIFY(QMetaObject::invokeMethod(&board,"onFrame",Qt::DirectConnection,Q_ARG(mlk::Frame,paused)));
        const auto before = clock->text();
        QTest::qWait(1200);
        QVERIFY2(clock->text() != before, qPrintable(clock->text()));
        qunsetenv("MLK_TIME_STATUS_BIN"); qunsetenv("MLK_RUL_AUTOSTART");
        qunsetenv("MLK_FAULT_SOCKET"); qunsetenv("MLK_CAPTURE_PS_SOCKET");
    }
    void boardTimeNetworkDetection() {
        qputenv("MLK_TIME_STATUS_BIN","true");
        mlk::BoardTime available;
        available.start();
        QTRY_VERIFY(available.networkTimeAvailable());
        QCOMPARE(available.stateText(), QStringLiteral("网络校时可用"));
        QVERIFY(!available.setDateTime(QDateTime::currentDateTime(), nullptr));
        qputenv("MLK_TIME_STATUS_BIN","false");
        mlk::BoardTime missing;
        missing.start();
        QTRY_COMPARE(missing.stateText(), QStringLiteral("网络校时不可用"));
        QVERIFY(!missing.networkTimeAvailable());
        qunsetenv("MLK_TIME_STATUS_BIN");
    }
    void sharedAuthorityAndConflict() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        qputenv("MLK_FAULT_SOCKET",(dir.path()+"/fault.sock").toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.path());
        QCoreApplication::setOrganizationName("MLK-Authority-Tests"); QCoreApplication::setApplicationName("board");
        QSettings settings;
        settings.setValue("app/autoDiscover",false); settings.setValue("app/autoConnect",false);
        settings.setValue("app/port",19400); settings.setValue("app/targetAddr","127.0.0.1");
        settings.setValue("app/targetPort",19401); settings.setValue("app/storageDir",dir.path());
        settings.setValue("app/secMode",1); settings.setValue("app/pskHex",QString(64,'b'));
        settings.setValue("rul/modelDirectory",dir.path()+"/missing"); settings.sync();
        KLocalWindow board;
        mlk::SecureChannel channel; channel.configure(mlk::SecureChannel::Psk,QByteArray(32,char(0xbb)),{},{},{});
        mlk::Receiver peer; peer.setChannel(&channel); QVERIFY(peer.start(QHostAddress::LocalHost,19401));
        QJsonObject snapshot; QString error;
        connect(&peer,&mlk::Receiver::frameReceived,this,[&](const mlk::Frame &f) {
            if(f.type==mlk::kTypeConfig) { snapshot=f.object; error=f.object.value("error").toString(); }
        });
        auto send=[&](const QJsonObject &o) {
            snapshot={}; peer.send(QHostAddress::LocalHost,19400,mlk::buildObjectFrame(mlk::kTypeConfig,o));
        };
        send({{"op","get"}}); QTRY_VERIFY(!snapshot.isEmpty());
        mlk::AppConfig cfg; QVERIFY(mlk::readSharedConfig(snapshot,cfg)); QVERIFY(!cfg.remotePriority);
        cfg.ch[0].k=0.2; auto request=mlk::sharedConfig(cfg); request.insert("op","set");
        send(request); QTRY_VERIFY(!snapshot.isEmpty()); QVERIFY(!error.isEmpty());
        QCOMPARE(snapshot.value("channels").toArray()[0].toObject().value("k").toDouble(),0.1);
        // Select remote ownership through the password-protected board UI.
        QTimer::singleShot(30,&board,[&] {
            const auto dialogs=board.findChildren<QDialog *>();
            for(auto *dialog:dialogs) if(dialog->windowTitle()==QStringLiteral("配置验证")) {
                dialog->findChild<QLineEdit *>()->setText("root"); dialog->accept();
            }
            QTimer::singleShot(30,&board,[&] {
                auto *dialog=board.findChild<PlSettingsDialog *>(); QVERIFY(dialog);
                dialog->findChild<QComboBox *>("configPriority")->setCurrentIndex(1); dialog->accept();
            });
        });
        board.openSettings();
        send({{"op","get"}}); QTRY_VERIFY(!snapshot.isEmpty());
        QVERIFY(mlk::readSharedConfig(snapshot,cfg)); QVERIFY(cfg.remotePriority);
        const auto revision=cfg.sharedRevision; cfg.ch[0].k=0.2;
        request=mlk::sharedConfig(cfg); request.insert("op","set");
        send(request); QTRY_VERIFY(!snapshot.isEmpty()); QVERIFY2(error.isEmpty(),qPrintable(error));
        QCOMPARE(snapshot.value("revision").toInt(),int(revision+1));
        // Identical resend is idempotent.
        send(request); QTRY_VERIFY(!snapshot.isEmpty()); QVERIFY2(error.isEmpty(),qPrintable(error));
        QCOMPARE(snapshot.value("revision").toInt(),int(revision+1));
        // remote-pc priority: a stale revision from the remote still wins over a
        // board-local edit made while the remote was offline.
        QJsonObject stale=request;
        stale.insert("revision",double(revision));
        stale.insert("channels",request.value("channels").toArray());
        auto channels=stale.value("channels").toArray();
        auto first=channels[0].toObject(); first.insert("k",0.3); channels[0]=first;
        stale.insert("channels",channels);
        send(stale); QTRY_VERIFY(!snapshot.isEmpty()); QVERIFY2(error.isEmpty(),qPrintable(error));
        QCOMPARE(snapshot.value("channels").toArray()[0].toObject().value("k").toDouble(),0.3);
        QCOMPARE(snapshot.value("revision").toInt(),int(revision+2));
        QVERIFY(!snapshot.contains("settingsPassword"));
        QCOMPARE(QSettings().value("app/settingsPassword").toString(),QStringLiteral("root"));
        QJsonObject order{{"id","pending"},{"faultTimeUs","1700000000000000"},{"class",1},{"grade",2},{"message","offline"}};
        mlk::WorkOrders store(dir.path(),"secret"); QVERIFY(store.save(order));
        QCOMPARE(store.pending(),QStringList{"pending"}); QVERIFY(store.save(order));
        order.insert("grade",3); QVERIFY(!store.save(order));
        QVERIFY(store.acknowledge("pending")); QVERIFY(store.pending().isEmpty());
    }
};
QTEST_MAIN(WindowTest)
#include "test_windows.moc"
