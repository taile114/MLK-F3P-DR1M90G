#include <QtTest>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QComboBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QUdpSocket>
#include <limits>
#include "frame.h"
#include "diagnosisui.h"
#include "inferencejournal.h"
#include "replaycontroller.h"
#include "receiver.h"
#include "replaypanel.h"

class RulTest : public QObject {
    Q_OBJECT
    mlk::Frame result() {
        mlk::Frame f; f.ver = 3; f.type = 1; f.seq = 4; f.tsUs = 123456789;
        f.task = 1; f.valid = 1; f.source = 1; f.backend = 2; f.rulUnit = 1;
        f.rulValue = 12.125f; f.windowIndex = 92; f.windowCount = 184; f.inferUs = 44965;
        f.inferenceState = mlk::kPaused; f.modelId = QStringLiteral("motor_stable23_v1");
        f.datasetId = QStringLiteral("val184"); f.message = QStringLiteral("数据集回放"); return f;
    }
    void write(const QString &path, const QByteArray &data) {
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(data), qint64(data.size()));
    }
    void bundle(const QString &root) {
        const QString engine = QString::fromLocal8Bit(qgetenv("MLK_TEST_ENGINE"));
        QVERIFY2(!engine.isEmpty(), "Set MLK_TEST_ENGINE to the native fake_engine shared library");
        QVERIFY(QFile::copy(engine, root + QStringLiteral("/libmlk_rul.so")));
        write(root + "/weights.bin", "test weights"); write(root + "/weights.tsv", "test index");
        write(root + "/qparams_ff6.tsv", "test plan");
        QByteArray data;
        for (int n = 0; n < 184; ++n) for (int i = 0; i < 2530; ++i) {
            float value = i ? 0 : float(n); quint32 bits; std::memcpy(&bits, &value, 4); mlk::wr32(data, bits);
        }
        write(root + "/input_f32.bin", data);
        QJsonObject hashes;
        for (const char *name : {"weights.bin", "weights.tsv", "qparams_ff6.tsv", "input_f32.bin", "libmlk_rul.so"}) {
            QFile file(root + "/" + name); QVERIFY(file.open(QIODevice::ReadOnly));
            hashes[name] = QString::fromLatin1(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex());
        }
        QJsonObject m{{"schema",1},{"engine_abi",1},{"model_id","motor_stable23_v1"},
            {"feature_version","motor_stable23_v1"},{"shape",QJsonArray{1,23,110}},
            {"dtype","little_endian_float32"},{"normalized",true},{"windows",184},
            {"dataset_id","val184"},{"rul_unit","observed_aging_test_session"},
            {"execution_plan","ff6"},{"quantization_plan","qparams_ff6.tsv"},{"npu_branches",6},{"sha256",hashes}};
        write(root + "/model.json", QJsonDocument(m).toJson());
    }
private slots:
    void initTestCase() { qRegisterMetaType<mlk::Frame>(); }
    void defaultNpuAndReportedBackend() {
        for (bool local : {false, true}) {
            mlk::ReplayPanel panel(local);
            auto *backend = panel.findChild<QComboBox *>("rulBackend"); QVERIFY(backend);
            auto *start = panel.findChild<QPushButton *>("rulAction1"); QVERIFY(start);
            QVERIFY(!panel.findChild<QPushButton *>("rulAction5"));
            QVERIFY(!panel.findChild<QPushButton *>("rulAction6"));
            QCOMPARE(backend->currentData().toInt(), 2);
            QSignalSpy requests(&panel, &mlk::ReplayPanel::requested);
            start->click();
            QCOMPARE(requests.takeFirst().at(1).toUInt(), 2u);
            auto status = result();
            status.backend = 1; panel.showFrame(status);
            QCOMPARE(backend->currentData().toInt(), 1);
            // Heartbeats preserve an unsent backend choice.
            backend->setCurrentIndex(backend->findData(2));
            panel.showFrame(status);
            QCOMPARE(backend->currentData().toInt(), 2);
            start->click();
            QCOMPARE(requests.takeFirst().at(1).toUInt(), 2u);
            status.backend = 2; panel.showFrame(status);
            QCOMPARE(backend->currentData().toInt(), 2);
            // An actual CPU fallback must be visible after the request.
            status.backend = 1; panel.showFrame(status);
            QCOMPARE(backend->currentData().toInt(), 1);
            status.backend = 0; panel.showFrame(status);
            QCOMPARE(backend->currentData().toInt(), 1);
        }
    }
    void wireAndUnits() {
        const auto expected = result(); mlk::Frame f;
        const auto wire = mlk::buildInferenceFrame(expected); QCOMPARE(wire.size(), 224);
        QVERIFY(mlk::parseFrame(wire, f)); QCOMPARE(f.rulValue, expected.rulValue);
        QCOMPARE(f.rulUnit, expected.rulUnit); QCOMPARE(f.datasetId, expected.datasetId);
        QCOMPARE(f.message, expected.message); QVERIFY(!mlk::hasClassification(f));
        for (int n = 0; n < wire.size(); ++n) QVERIFY(!mlk::parseFrame(wire.left(n), f));
        auto bad = wire; bad[25] = char(128); QVERIFY(!mlk::parseFrame(bad, f));
        auto infinite = expected; infinite.rulValue = std::numeric_limits<float>::infinity();
        QVERIFY(!mlk::parseFrame(mlk::buildInferenceFrame(infinite), f));
        auto future = wire; future[2] = 99; QVERIFY(!mlk::parseFrame(future, f));
        auto outOfRange = expected; outOfRange.windowIndex = outOfRange.windowCount;
        QVERIFY(!mlk::parseFrame(mlk::buildInferenceFrame(outOfRange), f));
        QLabel cls, grade, conf, rul, alert, time;
        mlk::showDiagnosis(expected, &cls, &grade, &conf, &rul, &alert, &time);
        QCOMPARE(cls.text(), QStringLiteral("不可用")); QVERIFY(alert.text().isEmpty());
        QVERIFY(rul.text().contains(QStringLiteral("轮"))); QVERIFY(!rul.text().contains(" h"));
    }
    void legacyAndControl() {
        mlk::Frame f;
        QVERIFY(mlk::parseFrame(mlk::buildReplayControl(mlk::kReplaySeek,2,92,1000,77,1),f));
        QCOMPARE(f.requestId,quint32(77)); QCOMPARE(f.windowIndex,quint32(92));
        QVERIFY(!mlk::parseFrame(mlk::buildReplayControl(mlk::kReplayStart,2,0,0,1,0),f));
        qint16 values[4] = {1,-2,3,4};
        QVERIFY(mlk::parseFrame(mlk::buildInjectFrame(values,3000,1,42),f)); QCOMPARE(f.setpoint[1],qint16(-2));
        QByteArray old; mlk::wr16(old,mlk::kMagic); old.append(char(2)); old.append(char(1));
        old.append(20,'\0'); old.append(char(255)); old.append(3,'\0'); mlk::wr32(old,12); mlk::wr32(old,100);
        QVERIFY(mlk::parseFrame(old,f)); QVERIFY(!mlk::hasClassification(f));
    }
    void encryptedJournal() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); mlk::InferenceJournal journal; QString error;
        const auto expected = result();
        QVERIFY2(journal.save(expected,dir.path(),true,QStringLiteral("test-pass"),0,0,&error),qPrintable(error));
        const auto files = QDir(dir.path()+"/rul").entryList({"*.mlkr"},QDir::Files); QCOMPARE(files.size(),1);
        const auto path = dir.path()+"/rul/"+files[0]; mlk::Frame f;
        QVERIFY(journal.load(path,"test-pass",f,&error)); QCOMPARE(f.rulValue,expected.rulValue); QCOMPARE(f.source,quint8(1));
        QVERIFY(!journal.load(path,"wrong",f,&error));
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); auto bytes = file.readAll(); file.close();
        bytes[35] = char(bytes[35] ^ 1); write(path,bytes);
        QVERIFY(!journal.load(path,"test-pass",f,&error));
    }
    void journalRetentionAndLargeDirectory() {
        QTemporaryDir dir;QVERIFY(dir.isValid());QDir records(dir.path()+"/rul");QVERIFY(records.mkpath("."));
        const auto old=QDateTime::currentDateTimeUtc().addDays(-2);
        for(int i=0;i<10000;++i) {
            QFile file(records.filePath(QStringLiteral("rul_fixture_%1.mlkr").arg(i)));
            QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write("fixture"),qint64(7));
            QVERIFY(file.flush());
            QVERIFY(file.setFileTime(old.addMSecs(i),QFileDevice::FileModificationTime));
        }
        mlk::InferenceJournal journal;QString error;auto frame=result();QElapsedTimer timer;timer.start();
        QVERIFY2(journal.save(frame,dir.path(),false,{},0,0,&error),qPrintable(error));
        const auto indexedMs=timer.elapsed();timer.restart();
        for(int i=0;i<25;++i) {
            ++frame.tsUs;QVERIFY2(journal.save(frame,dir.path(),false,{},0,0,&error),qPrintable(error));
        }
        qInfo()<<"Journal indexing ms:"<<indexedMs<<"; 25 steady saves ms:"<<timer.elapsed();
        QCOMPARE(records.entryList({"rul_*.mlkr"},QDir::Files,QDir::NoSort).size(),10000);
        QVERIFY(!QFile::exists(records.filePath("rul_fixture_0.mlkr")));
        QVERIFY(!QFile::exists(records.filePath("rul_fixture_25.mlkr")));
        QVERIFY(QFile::exists(records.filePath("rul_fixture_26.mlkr")));
        // Month expiry follows file modification time, independent of filename.
        QTemporaryDir other;QDir otherRecords(other.path()+"/rul");QVERIFY(otherRecords.mkpath("."));
        QFile expired(otherRecords.filePath("rul_zz_expired.mlkr"));
        QVERIFY(expired.open(QIODevice::WriteOnly));expired.write("old");
        QVERIFY(expired.flush());
        QVERIFY(expired.setFileTime(QDateTime::currentDateTimeUtc().addMonths(-2),QFileDevice::FileModificationTime));expired.close();
        QFile young(otherRecords.filePath("rul_aa_young.mlkr"));QVERIFY(young.open(QIODevice::WriteOnly));young.write("young");young.close();
        QVERIFY2(journal.save(frame,other.path(),false,{},1,0,&error),qPrintable(error));
        QVERIFY(!expired.exists());QVERIFY(young.exists());
        // Returning to a directory rebuilds its index and preserves the cap.
        QVERIFY2(journal.save(frame,dir.path(),false,{},0,0,&error),qPrintable(error));
        QCOMPARE(records.entryList({"rul_*.mlkr"},QDir::Files,QDir::NoSort).size(),10000);
    }
    void missingModelAndRecovery() {
        mlk::ReplayController controller; QSignalSpy results(&controller,&mlk::ReplayController::resultReady);
        QVERIFY(controller.setRoot("/no/such/model"));
        QVERIFY(controller.control(mlk::kReplayStep,1,0,1000).isEmpty());
        QTRY_COMPARE(controller.snapshot().inferenceState,quint8(mlk::kInferenceError)); QCOMPARE(results.size(),0);
        QVERIFY(controller.control(mlk::kReplayStop,1,0,1000).isEmpty());
        QTRY_COMPARE(controller.snapshot().inferenceState,quint8(mlk::kStopped));
    }
    void incompatibleBundle_data() {
        QTest::addColumn<QString>("mutation");
        QTest::newRow("old-two-branch") << QStringLiteral("execution_plan");
        QTest::newRow("wrong-plan-name") << QStringLiteral("quantization_plan");
        QTest::newRow("wrong-branch-count") << QStringLiteral("npu_branches");
        QTest::newRow("corrupt-plan") << QStringLiteral("corrupt-plan");
        QTest::newRow("missing-plan") << QStringLiteral("missing-plan");
    }
    void incompatibleBundle() {
        QFETCH(QString, mutation);
        QTemporaryDir dir; QVERIFY(dir.isValid()); bundle(dir.path());
        if (mutation == QStringLiteral("corrupt-plan")) write(dir.path()+"/qparams_ff6.tsv", "changed plan");
        else if (mutation == QStringLiteral("missing-plan")) QVERIFY(QFile::remove(dir.path()+"/qparams_ff6.tsv"));
        else {
            QFile file(dir.path()+"/model.json"); QVERIFY(file.open(QIODevice::ReadOnly));
            auto m = QJsonDocument::fromJson(file.readAll()).object(); file.close();
            if (mutation == QStringLiteral("npu_branches")) m[mutation] = 2;
            else m[mutation] = QStringLiteral("head2");
            write(dir.path()+"/model.json", QJsonDocument(m).toJson());
        }
        mlk::ReplayController controller; QVERIFY(controller.setRoot(dir.path()));
        QSignalSpy results(&controller,&mlk::ReplayController::resultReady);
        QVERIFY(controller.control(mlk::kReplayStep,2,0,1000).isEmpty());
        QTRY_COMPARE(controller.snapshot().inferenceState,quint8(mlk::kInferenceError));
        QCOMPARE(results.size(),0); QCOMPARE(controller.snapshot().valid,quint8(0));
        QCOMPARE(controller.snapshot().backend,quint8(0));
    }
    void stopWhileLoading() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); bundle(dir.path());
        mlk::ReplayController controller; QVERIFY(controller.setRoot(dir.path()));
        QSignalSpy results(&controller,&mlk::ReplayController::resultReady);
        QVERIFY(controller.control(mlk::kReplayStart,1,0,1000).isEmpty());
        QVERIFY(controller.control(mlk::kReplayStop,1,0,1000).isEmpty());
        QTRY_COMPARE(controller.snapshot().inferenceState,quint8(mlk::kStopped));
        QCOMPARE(results.size(),0); QCOMPARE(controller.snapshot().valid,quint8(0));
    }
    void replayStateAndReload() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); bundle(dir.path());
        mlk::ReplayController controller; QVERIFY(controller.setRoot(dir.path()));
        QSignalSpy results(&controller,&mlk::ReplayController::resultReady);
        for (int backend : {1,2}) {
            QVERIFY(controller.control(mlk::kReplayStep,quint8(backend),0,1000).isEmpty());
            QTRY_COMPARE(results.size(),1); QCOMPARE(controller.snapshot().rulValue,0.125f);
            QCOMPARE(controller.snapshot().backend,quint8(backend));
            QVERIFY(controller.control(mlk::kReplaySeek,quint8(backend),92,1000).isEmpty());
            QVERIFY(controller.control(mlk::kReplayStep,quint8(backend),0,1000).isEmpty());
            QTRY_COMPARE(results.size(),2);
            QCOMPARE(controller.snapshot().windowIndex,quint32(92)); QCOMPARE(controller.snapshot().rulValue,92.125f);
            QVERIFY(controller.control(mlk::kReplaySeek,quint8(backend),184,1000).contains(QStringLiteral("不可用")));
            QVERIFY(controller.control(mlk::kReplayStop,quint8(backend),0,1000).isEmpty());
            QTRY_COMPARE(controller.snapshot().inferenceState,quint8(mlk::kStopped)); QCOMPARE(controller.snapshot().valid,quint8(0));
            results.clear(); // Next loop also expects one step result.
        }
    }
    void secureReceiverRejectsClearControl() {
        mlk::SecureChannel security; security.configure(mlk::SecureChannel::Psk,QByteArray(32,'p'),{},{},{});
        mlk::Receiver receiver; receiver.setChannel(&security);
        QUdpSocket portProbe; QVERIFY(portProbe.bind(QHostAddress(QHostAddress::LocalHost),quint16(0))); const auto port=portProbe.localPort(); portProbe.close();
        QVERIFY(receiver.start(QHostAddress::LocalHost,port)); QSignalSpy frames(&receiver,&mlk::Receiver::frameReceived);
        QUdpSocket sender; const auto command=mlk::buildReplayControl(mlk::kReplayStep,1,0,1000,1,0);
        sender.writeDatagram(command,QHostAddress::LocalHost,port); QTest::qWait(100); QCOMPARE(frames.size(),0);
        mlk::SecureChannel peer; peer.configure(mlk::SecureChannel::Psk,QByteArray(32,'p'),{},{},{});
        const auto secure=peer.seal(command); sender.writeDatagram(secure,QHostAddress::LocalHost,port);
        QTRY_COMPARE(frames.size(),1); sender.writeDatagram(secure,QHostAddress::LocalHost,port);
        QTest::qWait(100); QCOMPARE(frames.size(),1);
    }
};
QTEST_MAIN(RulTest)
#include "test_rul.moc"
