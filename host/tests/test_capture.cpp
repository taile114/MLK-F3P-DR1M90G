#include <QtTest>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QDir>
#include <QDataStream>
#include "capturestore.h"
#include "crypto.h"
#include <openssl/evp.h>
#include <cmath>
#include <limits>

class CaptureTest:public QObject {
    Q_OBJECT
    mlk::Capture fixture() {
        mlk::Capture c;c.id="test-capture";c.source="test";c.startUs=1700000000000000ULL;c.endUs=c.startUs+200000;
        for (int ch=0;ch<4;++ch) {
            mlk::Frame f;f.ch=quint8(ch);f.rateHz=100;f.tsUs=c.startUs;f.seq=quint32(ch+5);f.cnt=17;
            if(ch==0) {f.rateHz=10;f.samples={250,260};}
            if(ch==1) {f.rateHz=10;f.samples={100,-200};}
            if(ch==2) {f.samples={3,4,5};f.samplesY={4,0,-12};f.samplesZ={0,3,0};}
            if(ch==3) {f.rateHz=10;f.samples={1000,-1500};}
            c.blocks.append(f);
        }
        return c;
    }
private slots:
    void sensorConversionAndWideWaveforms() {
        QCOMPARE(mlk::currentMilliAmp(20000,20000,1250),0);
        QCOMPARE(mlk::currentMilliAmp(20800,20000,1250),1000);
        QCOMPARE(mlk::currentMilliAmp(19200,20000,1250),-1000);
        QCOMPARE(mlk::currentMilliAmp(24000,20000,1250),5000);
        QCOMPARE(mlk::currentMilliAmp(20121,20121,1250),0);
        QCOMPARE(mlk::displaySpeedDeciRpm(-3277000),32770);
        QCOMPARE(mlk::displaySpeedDeciRpm(-12000000),120000);
        QCOMPARE(mlk::displaySpeedDeciRpm(12000000),-120000);
        QCOMPARE(mlk::displaySpeedDeciRpm(0),0);
        QCOMPARE(mlk::displaySpeedDeciRpm(std::numeric_limits<qint32>::min()),21474836);
        for (quint8 ch : {quint8(mlk::kCurrent),quint8(mlk::kSpeed)}) {
            mlk::Frame f; f.ch=ch; f.rateHz=10; f.tsUs=1700000000000000ULL;
            f.samples={0,32770,120000,-120000,21474836,-21474836};
            const auto bytes=mlk::buildWaveform(f); mlk::Frame parsed;
            QCOMPARE(bytes.size(),mlk::kHeaderSize+4*f.samples.size());
            QVERIFY(mlk::parseFrame(bytes,parsed)); QCOMPARE(parsed.ver,quint8(6));
            QCOMPARE(parsed.samples,f.samples);
            QVERIFY(!mlk::parseFrame(bytes.left(bytes.size()-1),parsed));
            auto legacy=bytes.left(mlk::kHeaderSize); legacy[2]=5;
            legacy[14]=2; legacy[15]=0;
            mlk::wr16(legacy,12345); mlk::wr16(legacy,quint16(-23456));
            QVERIFY(mlk::parseFrame(legacy,parsed));
            QCOMPARE(parsed.samples,(QVector<qint32>{12345,-23456}));
        }
        QTemporaryDir dir; auto c=fixture(); c.blocks[3].samples={120000,-120000};
        mlk::CaptureStore store(dir.path(),"wide-test"); QString path,error;
        QVERIFY2(store.save(c,&path,&error),qPrintable(error)); mlk::Capture loaded;
        QVERIFY2(store.load(path,loaded,&error),qPrintable(error));
        QCOMPARE(loaded.blocks[3].samples,c.blocks[3].samples);
        const auto rows=mlk::CaptureStore::rows(loaded,10000);
        QCOMPARE(rows[0].value[2],12000.0); QCOMPARE(rows[10].value[2],-12000.0);
    }
    void legacyCapturePayload() {
        // Independent v1 disk fixture: all channels were signed16.
        auto c=fixture(); QJsonArray scales;
        for (int i=0;i<4;++i) scales.append(QJsonObject{{"k",c.k[i]},{"b",c.b[i]}});
        const QJsonObject meta{{"schema","mlk.capture.v1"},{"id",c.id},{"source",c.source},
            {"startUs",QString::number(c.startUs)},{"endUs",QString::number(c.endUs)},
            {"channels",scales},{"timebase","estimated_utc_first_sample"}};
        const auto json=QJsonDocument(meta).toJson(QJsonDocument::Compact);
        QByteArray plain; QDataStream ds(&plain,QIODevice::WriteOnly);
        ds.setVersion(QDataStream::Qt_5_12); ds.setByteOrder(QDataStream::LittleEndian);
        ds << quint32(json.size()); ds.writeRawData(json.constData(),json.size());
        ds << quint32(c.blocks.size());
        for (const auto &f:c.blocks) {
            ds << f.ch << quint8(f.samplesY.isEmpty()?1:3) << f.rateHz << f.seq << f.cnt
               << f.tsUs << quint32(f.samples.size());
            for (const auto *axis:{&f.samples,&f.samplesY,&f.samplesZ})
                for (auto v:*axis) ds << qint16(v);
        }
        const QByteArray salt(16,'s'),nonce(12,'n'),password("legacy-password");
        QByteArray key(32,'\0');
        QCOMPARE(PKCS5_PBKDF2_HMAC(password.constData(),password.size(),
            reinterpret_cast<const uchar *>(salt.constData()),salt.size(),100000,EVP_sha256(),
            key.size(),reinterpret_cast<uchar *>(key.data())),1);
        const auto header=QByteArray("MLKC\1\1",6)+salt+nonce;
        QTemporaryDir dir; const auto path=dir.path()+"/legacy.mlkc"; QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const auto bytes=header+mlk::crypto::seal(key,nonce,header,plain);
        QCOMPARE(file.write(bytes),qint64(bytes.size())); file.close();
        mlk::Capture loaded; QString error;
        QVERIFY2(mlk::CaptureStore(dir.path(),password).load(path,loaded,&error),qPrintable(error));
        for (int i=0;i<4;++i) {
            QCOMPARE(loaded.blocks[i].samples,c.blocks[i].samples);
            QCOMPARE(loaded.blocks[i].samplesY,c.blocks[i].samplesY);
            QCOMPARE(loaded.blocks[i].samplesZ,c.blocks[i].samplesZ);
        }
    }
    void encryptedRoundTripAndCsv() {
        QTemporaryDir dir;mlk::CaptureStore store(dir.path(),"test-password");auto original=fixture();QString path,error;
        QVERIFY2(store.save(original,&path,&error),qPrintable(error));
        QCOMPARE(store.list().size(),1);QCOMPARE(store.pathForId(original.id),path);
        QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));auto bytes=file.readAll();file.close();
        QCOMPARE(bytes.left(6),QByteArray("MLKC\1\1",6));QVERIFY(!bytes.contains("test-capture"));
        QVERIFY(!(QFileInfo(path).permissions() & (QFileDevice::ReadOther|QFileDevice::ReadGroup)));
        mlk::Capture loaded;QVERIFY2(store.load(path,loaded,&error),qPrintable(error));
        const auto fixtureDir=qEnvironmentVariable("MLK_CAPTURE_FIXTURE_DIR");
        if(!fixtureDir.isEmpty())QVERIFY(mlk::CaptureStore(fixtureDir,"test-password").save(original));
        QCOMPARE(loaded.sampleCount(),qint64(15));QCOMPARE(loaded.blocks[2].samplesY,original.blocks[2].samplesY);
        QCOMPARE(loaded.blocks[0].tsUs,original.startUs);QCOMPARE(loaded.blocks[0].rateHz,quint16(10));
        QCOMPARE(loaded.blocks[3].samples[1],qint16(-1500));
        QVERIFY(!mlk::CaptureStore(dir.path(),"wrong").load(path,loaded));
        auto rows=mlk::CaptureStore::rows(loaded,10000);QCOMPARE(rows.size(),20);
        QCOMPARE(rows[0].value[0],25.0);QCOMPARE(rows[0].value[1],100.0);QCOMPARE(rows[0].value[2],100.0);
        QVERIFY(std::abs(rows[0].value[6]-5*0.0039)<1e-12);
        QCOMPARE(rows[10].value[0],26.0);QCOMPARE(rows[10].value[1],-200.0);QCOMPARE(rows[10].value[2],-150.0);
        QCOMPARE(rows[0].validMask,quint8(127));
        QVERIFY(store.exportCsv(loaded,dir.path()+"/training.csv",10000,&error));
        QFile csv(dir.path()+"/training.csv");QVERIFY(csv.open(QIODevice::ReadOnly));auto text=csv.readAll();
        QCOMPARE(text.count('\n'),21);QVERIFY(text.startsWith(mlk::CaptureStore::csvHeader().toUtf8()));
        // Authenticate both the header and ciphertext; do not replace output on failure.
        for (const int offset:{4,5,6,22,34,int(bytes.size()-1)}) {
            auto corrupt=bytes;corrupt[offset]=char(corrupt[offset]^1);QVERIFY(file.open(QIODevice::WriteOnly));file.write(corrupt);file.close();
            mlk::Capture unchanged;unchanged.id="sentinel";
            QVERIFY(!store.load(path,unchanged));QCOMPARE(unchanged.id,QStringLiteral("sentinel"));
        }
    }
    void missingStaleAndAlignment() {
        auto c=fixture();c.blocks.remove(3);c.blocks.remove(1);
        c.blocks[0].tsUs+=100000;c.endUs=c.startUs+7000000;
        auto rows=mlk::CaptureStore::rows(c,100000);QVERIFY(!rows.isEmpty());
        QVERIFY(std::isnan(rows[0].value[0])); // no backward filling from a future temperature
        QVERIFY(std::isnan(rows[1].value[1]));QVERIFY(std::isnan(rows[1].value[2]));
        QVERIFY(std::isnan(rows[6].value[3]));QVERIFY(!(rows[6].validMask&0x40));
        QVERIFY(std::isnan(rows.back().value[0]));
        c.blocks[1].samplesY.clear();c.blocks[1].samplesZ.clear();
        auto series=mlk::CaptureStore::series(c);QVERIFY(series[4].isEmpty());QVERIFY(series[5].isEmpty());
        auto frame=c.blocks[1];frame.tsUs=0;frame.rateHz=100;frame.samples={1,2,3};
        QCOMPARE(mlk::timestampWaveform(frame,100000).tsUs,quint64(80000));
        frame.ver=4;frame.tsUs=100000;
        const auto normalized=mlk::timestampWaveform(frame,999999);
        QCOMPARE(normalized.tsUs,quint64(80000));QCOMPARE(normalized.ver,mlk::kWaveformVersion);
        QCOMPARE(mlk::timestampWaveform(normalized,999999).tsUs,normalized.tsUs);
        mlk::Frame parsed;
        QVERIFY(mlk::parseFrame(mlk::buildWaveform(normalized),parsed));QCOMPARE(parsed.ver,mlk::kWaveformVersion);
        QCOMPARE(parsed.tsUs,normalized.tsUs);
    }
    void labelsAndFutureSegments() {
        QTemporaryDir dir;mlk::AppConfig cfg;cfg.storageDir=dir.path();cfg.passphrase="labels";
        auto c=fixture();mlk::CaptureStore store(dir.path(),cfg.passphrase);QString path;
        QJsonObject label{{"api",1},{"id","fault-1"},{"startUs",QString::number(c.startUs+10000)},
            {"endUs",QString::number(c.startUs+20000)},{"label","bearing-outer-race"},
            {"abnormal",true},{"author","training-team"},{"note","test"}};
        QVERIFY(store.putLabel(label)); // event precedes segment flush
        QVERIFY(store.save(c,&path));QJsonArray all;QVERIFY(store.labels(all));
        QCOMPARE(mlk::CaptureStore::matchingLabels(all,c.startUs,c.endUs,true).size(),1);
        auto reply=mlk::captureRequest(cfg,{{"api",1},{"op","list"},{"abnormalOnly",true}});
        QVERIFY(reply.value("ok").toBool());QCOMPARE(reply.value("records").toArray().size(),1);
        label.insert("abnormal",false);QVERIFY(store.putLabel(label));QVERIFY(store.labels(all));QCOMPARE(all.size(),1);
        QVERIFY(mlk::CaptureStore::matchingLabels(all,c.startUs,c.endUs,true).isEmpty());
        label.insert("endUs",label.value("startUs"));QVERIFY(!store.putLabel(label));
        label.insert("id","../escape");QVERIFY(!store.putLabel(label));
        QVERIFY(!mlk::CaptureStore(dir.path(),"wrong").labels(all));
        QFile labels(dir.path()+"/capture_labels.mlkl");QVERIFY(labels.open(QIODevice::ReadOnly));
        QVERIFY(!labels.readAll().contains("bearing-outer-race"));
    }
    void failedSaveRetainsBufferAndLimits() {
        QTemporaryDir dir;mlk::AppConfig cfg;cfg.storageDir=dir.path();cfg.minFreeMb=0;cfg.passphrase="";
        mlk::CaptureRecorder recorder;recorder.begin(cfg,"test");auto f=fixture().blocks[0];f.tsUs=mlk::captureNowUs();
        QVERIFY(recorder.append(f));QString error;QVERIFY(!recorder.flush(nullptr,&error));QVERIFY(!recorder.empty());
        cfg.passphrase="secret";QVERIFY(!recorder.configMatches(cfg));
        mlk::CaptureStore store(dir.path(),"secret");auto c=fixture();QString path;QVERIFY(store.save(c,&path));
        QVERIFY(store.putLabel({{"api",1},{"id","retain"},{"startUs",QString::number(c.startUs)},
            {"endUs",QString::number(c.endUs)},{"abnormal",true},{"label","fault"},{"author","test"},{"note",""}}));
        QVERIFY(store.prune(0,0,1,0));QVERIFY(!QFile::exists(path));QVERIFY(QFile::exists(dir.path()+"/capture_labels.mlkl"));
        c.blocks[0].samplesY={1};QVERIFY(!store.save(c));
        QVERIFY(mlk::CaptureStore::rows(c,1).isEmpty());
    }
    void recordDeletionIsScopedToRecords() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QTemporaryDir outside; QVERIFY(outside.isValid());
        const QStringList names{QStringLiteral("capture_1_2_id.mlkc"), QStringLiteral("fault_1.mlkf")};
        for (const auto &name : names) {
            QFile file(dir.path() + QLatin1Char('/') + name);
            QVERIFY(file.open(QIODevice::WriteOnly)); file.write("x");
        }
        QFile keep(dir.path() + QStringLiteral("/capture_labels.mlkl"));
        QVERIFY(keep.open(QIODevice::WriteOnly)); keep.write("x");
        QFile escape(outside.path() + QStringLiteral("/capture_9_9_other.mlkc"));
        const QString escapePath = escape.fileName();
        QVERIFY(escape.open(QIODevice::WriteOnly)); escape.write("x");
        QStringList failed;
        QCOMPARE(mlk::removeCaptureFiles(dir.path(), names, &failed), 2);
        QVERIFY(failed.isEmpty());
        QVERIFY(QFile::exists(dir.path() + QStringLiteral("/capture_labels.mlkl")));
        QCOMPARE(mlk::removeCaptureFiles(dir.path(), {QStringLiteral("capture_labels.mlkl"),
                                                     QStringLiteral("../../etc/passwd"),
                                                     escapePath}, &failed), 0);
        QCOMPARE(failed.size(), 3);
        QVERIFY(QFile::exists(escapePath));
    }
};
QTEST_GUILESS_MAIN(CaptureTest)
#include "test_capture.moc"
