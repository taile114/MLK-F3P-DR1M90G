#include <QtTest>
#include <QComboBox>
#include <QFile>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QListWidget>
#include <QTableView>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QUuid>
#include <cmath>
#include <cstring>
#include <unistd.h>

#include "faultdataset.h"
#include "faultevaluator.h"
#include "faultevalpanel.h"
#include "faultlivecontroller.h"
#include "faultpanel.h"
#include "faultpreprocess.h"
#include "faultruntime.h"
#include "klocalwindow.h"
#include "theme.h"
#include "replaycontroller.h"
#include "replaypanel.h"
#include "plsampleclock.h"
#include "pl_host.h"
#include "../ps-local/driver/pl_ctrl_uapi.h"
#include "capturestore.h"
#include "capturedialog.h"
#include "workorders.h"
#include "workorderoutbox.h"
#include <QTimer>
#include "faultrecordpolicy.h"
#include "alarmcontroller.h"

namespace {
QJsonObject readJson(const QString &path) {
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
bool writeJson(const QString &path, const QJsonObject &object) {
    QFile file(path); if (!file.open(QIODevice::WriteOnly)) return false;
    const auto bytes = QJsonDocument(object).toJson(); return file.write(bytes) == bytes.size();
}
QString packageRoot() { return qEnvironmentVariable("MLK_TEST_FAULT_ROOT"); }
QString datasetRoot() { return qEnvironmentVariable("MLK_TEST_FAULT_EVAL"); }
QString fixtureRoot() { return QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()+"/fixtures/fault-preprocess"; }
mlk::FaultWindowInput fixtureWindow(const QJsonObject &document) {
    mlk::FaultWindowInput window;
    window.startUs=document.value("startUs").toString().toULongLong();
    window.endUs=document.value("endUs").toString().toULongLong();
    for (const auto &item:document.value("blocks").toArray()) {
        const auto b=item.toObject(); mlk::NativeBlock block; block.dataValid=true;
        block.channel=b.value("channel").toInt(); block.t0Us=b.value("t0Us").toString().toULongLong();
        block.rateHz=b.value("rateHz").toInt(); block.count=b.value("count").toInt(); block.axes=b.value("axes").toInt();
        for (const auto &v:b.value("values").toArray()) block.values.append(float(v.toDouble()));
        if (block.channel==mlk::kTemp) window.temperature.append(block);
        if (block.channel==mlk::kCurrent) window.current.append(block);
        if (block.channel==mlk::kSpeed) window.speed.append(block);
        if (block.channel==mlk::kVibration) window.vibration.append(block);
    }
    return window;
}
}

class FaultTest : public QObject {
    Q_OBJECT
private slots:
    void outboxDoesNotBlockSamplingAndInvalidatesCache() {
        QTemporaryDir root, other; QVERIFY(root.isValid()); QVERIFY(other.isValid());
        const QString password="outbox-test";
        mlk::WorkOrders store(root.path(),password);
        QStringList ids;
        for (int i=0;i<4;++i) {
            const QString id=QString("event-%1").arg(i); ids<<id;
            const QJsonObject order{{"id",id},{"faultTimeUs","1793520000000000"},
                {"class",1},{"grade",1},{"message","test"}};
            QVERIFY(store.save(order));
        }
        mlk::WorkOrderOutbox outbox; outbox.configure(root.path(),password);
        QSignalSpy ready(&outbox,&mlk::WorkOrderOutbox::orderReady);
        QSignalSpy finished(&outbox,&mlk::WorkOrderOutbox::retryFinished);
        int ticks=0; QTimer timer; timer.setInterval(5);
        connect(&timer,&QTimer::timeout,this,[&]{++ticks;}); timer.start();
        outbox.retry(); outbox.retry(); // Bound queued work to one batch.
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(),1,10000);
        QCOMPARE(ready.count(),4); QVERIFY(ticks>1);
        QElapsedTimer elapsed; elapsed.start();
        for (int i=0;i<10;++i) {
            outbox.retry(); QTRY_COMPARE_WITH_TIMEOUT(finished.count(),i+2,10000);
        }
        qInfo("ten_cached_outbox_retries_us=%lld",elapsed.nsecsElapsed()/1000);
        QCOMPARE(ready.count(),44);
        // Same ID and length, different ciphertext: the previous authenticated object is unusable.
        QFile file(root.filePath("orders/"+ids.first()+".mlko")); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto original=file.readAll(); file.close(); auto corrupt=original;
        QJsonObject loaded; QVERIFY(store.load(ids.first(),loaded));
        corrupt[corrupt.size()-1]=char(corrupt.back()^1);
        QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(corrupt),qint64(corrupt.size())); file.close();
        QVERIFY(!store.load(ids.first(),loaded));
        QVERIFY(!store.load(ids.first(),loaded)); // Authentication failures may be cached as well.
        QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(original),qint64(original.size())); file.close();
        QVERIFY(store.load(ids.first(),loaded)); QCOMPARE(loaded.value("id").toString(),ids.first());
        // A late cold batch must not publish after the password/directory changes.
        ready.clear(); outbox.configure(root.path(),"wrong-password"); outbox.retry();
        outbox.configure(other.path(),password);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(),12,10000); QCOMPARE(ready.count(),0);
        outbox.retry(); QTRY_COMPARE_WITH_TIMEOUT(finished.count(),13,10000); QCOMPARE(ready.count(),0);
        outbox.configure(root.path(),password); outbox.retry();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(),14,10000); QCOMPARE(ready.count(),4);
        for (const auto&id:ids) QVERIFY(store.acknowledge(id));
        ready.clear(); outbox.retry();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(),15,10000); QCOMPARE(ready.count(),0);
    }
    void faultJournalKeepsLogsAboveFreeSpaceThreshold() {
        QTemporaryDir root; QVERIFY(root.isValid());
        mlk::FaultJournal journal; QString error;
        const QJsonObject record{{"seq",1},{"confidence",0.684}};
        QVERIFY2(journal.append(record,root.path(),"journal-test",3,1,&error),qPrintable(error));
        const auto files=QDir(root.filePath("fault")).entryList({"*.mlkf"},QDir::Files);
        QCOMPARE(files.size(),1); QJsonArray records;
        QVERIFY2(mlk::FaultJournal::read(root.filePath("fault/"+files.first()),"journal-test",records,&error),qPrintable(error));
        QCOMPARE(records.size(),1); QCOMPARE(records.first().toObject(),record);
    }
    void preprocessingMatchesTrainingReference() {
        mlk::FaultPreprocessor preprocess; QString error;
        QVERIFY2(preprocess.loadAssets(packageRoot(),&error),qPrintable(error));
        for (const auto &prefix:{QString(),QString("thermal-")}) {
            const auto document=readJson(fixtureRoot()+"/"+prefix+"window.json");
            QVERIFY(!document.isEmpty()); auto window=fixtureWindow(document);
            QFile golden(fixtureRoot()+"/"+document.value("expectedFile").toString()); QVERIFY(golden.open(QIODevice::ReadOnly));
            const auto bytes=golden.readAll(); QCOMPARE(bytes.size(),int(MLK_FAULT_INPUT_BYTES));
            QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex()),
                     document.value("expectedSha256").toString());
            QVector<float> expected(MLK_FAULT_INPUT_FLOATS); std::memcpy(expected.data(),bytes.constData(),size_t(bytes.size()));
            mlk::FaultWindowOutput output;
            QVERIFY2(preprocess.build(window,output,&error),qPrintable(error));
            int offset=0;
            for (const auto &item:document.value("branches").toArray()) {
                const auto branch=item.toObject(); double maximum=0;
                for (int i=0;i<branch.value("count").toInt();++i) {
                    const double actual=output.input[offset+i], reference=expected[offset+i];
                    maximum=qMax(maximum,std::abs(actual-reference));
                    QVERIFY2(std::abs(actual-reference)<=3e-4+5e-6*std::abs(reference),
                        qPrintable(QString("%1 %2[%3] actual=%4 reference=%5").arg(prefix,branch.value("name").toString())
                                   .arg(i).arg(actual,0,'g',10).arg(reference,0,'g',10)));
                }
                qInfo("preprocess_parity %s%s max_abs=%.9g",qPrintable(prefix),qPrintable(branch.value("name").toString()),maximum);
                offset+=branch.value("count").toInt();
            }
            QCOMPARE(offset,int(MLK_FAULT_INPUT_FLOATS));
            const auto quality=document.value("quality").toObject();
            QCOMPARE(output.quality.fastRatio,quality.value("fast_valid_fraction").toDouble());
            QCOMPARE(output.quality.slowRatio,quality.value("slow_valid_fraction").toDouble());
            QVERIFY(output.quality.overtemperatureMasked);
            mlk::NativeBlock shortBlock=window.vibration.first(); shortBlock.count=1; shortBlock.values={100.f,100.f,100.f};
            window.vibration.append(shortBlock); mlk::FaultWindowOutput filtered;
            QVERIFY(preprocess.build(window,filtered,&error)); QCOMPARE(filtered.input,output.input);
        }
    }
    void temperatureGuardReselectsWithoutChangingOtherFaults() {
        mlk::FaultPreprocessor preprocess; QString error;
        QVERIFY2(preprocess.loadAssets(packageRoot(),&error),qPrintable(error));
        mlk::FaultRuntime runtime; mlk::FaultRuntimeConfig config; config.root=packageRoot(); config.backend=1;
        QVERIFY2(runtime.open(config,&error),qPrintable(error)); QVERIFY(runtime.supportsClassMask());
        for (const auto &prefix:{QString(),QString("thermal-")}) {
            const auto document=readJson(fixtureRoot()+"/"+prefix+"window.json");
            for (const auto &item:document.value("expectations").toArray()) {
                const auto expectation=item.toObject(); const double temperature=expectation.value("temperature").toDouble();
                auto window=fixtureWindow(document);
                for (auto &b:window.temperature) b.values={float(temperature)};
                mlk::FaultWindowOutput output; QVERIFY2(preprocess.build(window,output,&error),qPrintable(error));
                QCOMPARE(output.quality.overtemperatureMasked,temperature<=35.0);
                QVERIFY(std::abs(output.quality.temperatureC-temperature)<1e-5);
                mlk::FaultEngineOutcome raw,guarded;
                QVERIFY2(runtime.predict(output.input,raw,&error),qPrintable(error));
                const auto excluded=output.quality.overtemperatureMasked?MLK_FAULT_MASK_OVERTEMPERATURE:0u;
                QVERIFY2(runtime.predict(output.input,guarded,&error,excluded),qPrintable(error));
                QCOMPARE(raw.classIndex,expectation.value("rawClass").toInt());
                QCOMPARE(guarded.classIndex,expectation.value("guardedClass").toInt());
                qInfo("temperature_guard %sT=%.1f raw=%d guarded=%d masked=%d",qPrintable(prefix),temperature,
                      raw.classIndex,guarded.classIndex,int(output.quality.overtemperatureMasked));
            }
        }
        mlk::FaultDataset dataset; QVERIFY(dataset.load(datasetRoot(),&error));
        QFile inputs(dataset.inputsPath()); QVERIFY(inputs.open(QIODevice::ReadOnly));
        for (int wanted:{mlk::kFaultOverload,mlk::kFaultUndervoltage}) {
            int row=0; while (row<dataset.rows().size() && dataset.rows()[row].targetClass!=wanted) ++row;
            QVERIFY(row<dataset.rows().size()); QVERIFY(inputs.seek(qint64(row)*MLK_FAULT_INPUT_BYTES));
            const auto bytes=inputs.read(MLK_FAULT_INPUT_BYTES); QCOMPARE(bytes.size(),int(MLK_FAULT_INPUT_BYTES));
            QVector<float> values(MLK_FAULT_INPUT_FLOATS); std::memcpy(values.data(),bytes.constData(),size_t(bytes.size()));
            mlk::FaultEngineOutcome raw,guarded;
            QVERIFY(runtime.predict(values,raw,&error));
            QVERIFY(runtime.predict(values,guarded,&error,MLK_FAULT_MASK_OVERTEMPERATURE));
            QCOMPARE(guarded.classIndex,wanted); QCOMPARE(guarded.classIndex,raw.classIndex); QCOMPARE(guarded.grade,raw.grade);
        }
    }
    void dmaRingRecoversSlowPollsAndOwnsSnapshot() {
        QTemporaryFile registers; QVERIFY(registers.open()); QVERIFY(registers.resize(4096));
        QVector<uint64_t> memory(4 * PL_DMA_BLOCK_WORDS);
        pl_dma_ring_t ring = {}; ring.map = memory.data(); ring.blocks = 4;
        auto publish = [&](uint32_t done) {
            for (unsigned slot = 0; slot < 4; ++slot)
                for (unsigned word = 0; word < PL_DMA_BLOCK_WORDS; ++word)
                    memory[int(slot * PL_DMA_BLOCK_WORDS + word)] = 1000 + slot;
            return ::pwrite(registers.handle(), &done, sizeof(done), PL_CTRL_DMA_BLK_SEQ) == sizeof(done);
        };
        const uint64_t *words = nullptr; uint32_t seq = 0;
        QVERIFY(publish(3)); // UI/recording delayed polling by three complete blocks.
        for (uint32_t expected = 1; expected <= 3; ++expected) {
            QCOMPARE(pl_dma_ring_next(&ring, registers.handle(), &words, &seq), 0);
            QCOMPARE(seq, expected); QCOMPARE(words[0], uint64_t(999 + expected));
            QCOMPARE(words[PL_DMA_BLOCK_WORDS - 1], words[0]);
        }
        memory[2 * PL_DMA_BLOCK_WORDS] = 0; QCOMPARE(words[0], uint64_t(1002));
        QCOMPARE(pl_dma_ring_next(&ring, registers.handle(), &words, &seq), -1);
        QCOMPARE(errno, EAGAIN);
        QVERIFY(publish(6));
        for (uint32_t expected = 4; expected <= 6; ++expected) {
            QCOMPARE(pl_dma_ring_next(&ring, registers.handle(), &words, &seq), 0);
            QCOMPARE(seq, expected);
        }
        QVERIFY(publish(12)); // Actual overrun: 7..9 are gone or actively being overwritten.
        for (uint32_t expected = 10; expected <= 12; ++expected) {
            QCOMPARE(pl_dma_ring_next(&ring, registers.handle(), &words, &seq), 0);
            QCOMPARE(seq, expected);
        }
        ring.last_seq = 0xfffffffeu; QVERIFY(publish(0));
        QCOMPARE(pl_dma_ring_next(&ring, registers.handle(), &words, &seq), 0);
        QCOMPARE(seq, uint32_t(0xffffffffu));
        QCOMPARE(pl_dma_ring_next(&ring, registers.handle(), &words, &seq), 0);
        QCOMPARE(seq, uint32_t(0));
        ring.last_seq = 100; QVERIFY(publish(2));
        QCOMPARE(pl_dma_ring_next(&ring, registers.handle(), &words, &seq), 0);
        QCOMPARE(seq, uint32_t(2)); // START reset is not a wraparound backlog.
        ring.last_seq = 100; QVERIFY(publish(0));
        QCOMPARE(pl_dma_ring_next(&ring, registers.handle(), &words, &seq), -1);
        QCOMPARE(errno, EAGAIN); QCOMPARE(ring.last_seq, 0u); // No completed block after START.
    }
    void nativeWindowGapsUseFrozenQualityGates() {
        mlk::FaultPreprocessor preprocessor; QString error;
        QVERIFY2(preprocessor.loadAssets(packageRoot(), &error), qPrintable(error));
        mlk::FaultInputAdapter adapter; mlk::AppConfig cfg; adapter.configure(cfg);
        const quint64 base = 1700000000000000ull;
        mlk::FaultWindowInput window; window.startUs = base; window.endUs = base + 4000000;
        for (quint64 us = 0; us < 4000000; us += 20000) {
            mlk::NativeBlock b; b.dataValid = true; b.axes = 1; b.count = 1; b.rateHz = 50;
            b.t0Us = base + us; b.values = {1.f}; b.channel = mlk::kCurrent; window.current.append(b);
            if (us % 100000 == 0) { b.channel = mlk::kSpeed; b.values = {1500.f}; window.speed.append(b); }
            if (us % 1000000 == 0) { b.channel = mlk::kTemp; b.values = {25.f}; window.temperature.append(b); }
        }
        auto vibration = [&](int keepEvery) {
            adapter.restart(); window.vibration.clear();
            // Native board-sized blocks at measured ADXL345 cadence, with observed gaps.
            for (quint32 block = 0; block < 73; ++block) {
                if (keepEvery == 0 ? block % 11 == 5 : block % keepEvery != 0) continue;
                mlk::Frame f; f.ver = mlk::kWaveformVersion; f.ch = mlk::kVibration;
                f.seq = block; f.rateHz = 3123; f.tsUs = base + quint64(block) * 170 * 32020 / 100;
                f.samples = QVector<qint32>(170, 0); f.samplesY = f.samples;
                f.samplesZ = QVector<qint32>(170, -256);
                window.vibration.append(adapter.acceptFrame(f));
            }
        };
        vibration(0); mlk::FaultWindowOutput out;
        QVERIFY2(preprocessor.build(window, out, &error), qPrintable(error));
        qInfo("native_gap_window blocks=%d gaps=%d tf=%.6f fast=%.6f slow=%.6f",
              out.quality.blocks, out.quality.gapBlocks, out.quality.tfRatio,
              out.quality.fastRatio, out.quality.slowRatio);
        QVERIFY(out.quality.gapBlocks > 0); QVERIFY(out.quality.tfRatio >= 0.45);
        QCOMPARE(out.input.size(), int(MLK_FAULT_INPUT_FLOATS));
        vibration(4);
        QVERIFY(!preprocessor.build(window, out, &error));
        QCOMPARE(out.reason, QString("tf_coverage"));
        qInfo("sparse_window blocks=%d gaps=%d tf=%.6f rejected=%s", out.quality.blocks,
              out.quality.gapBlocks, out.quality.tfRatio, qPrintable(out.reason));
        QVERIFY(out.quality.blocks >= 12); QVERIFY(out.quality.tfRatio < 0.45);
    }
    void normalRecordingRetentionIsConservative() {
        QTemporaryDir directory; QTemporaryDir outside;
        const quint64 base=1700000000000000ull;
        auto entry=[&](const QString &id,int from,int to) {
            mlk::CaptureEntry e{id,directory.filePath("capture_"+id+".mlkc"),base+quint64(from)*1000000,base+quint64(to)*1000000};
            QFile f(e.path); if (f.open(QIODevice::WriteOnly)) f.write("fixture"); return e;
        };
        const auto old=entry("old",2,10), normal=entry("normal",2,10), fault=entry("fault",18,22);
        const auto unknown=entry("unknown",48,52), recent=entry("recent",75,80);
        mlk::FaultRecordPolicy policy;
        policy.saved(old); QVERIFY(policy.discardable().isEmpty()); // Default is verification: keep everything.
        policy.begin(directory.path(),60,10);
        for (const auto &e:{normal,fault,unknown,recent}) policy.saved(e);
        auto other=normal; other.path=outside.filePath("capture_other.mlkc"); policy.saved(other);
        mlk::FaultResult result; result.valid=true; result.state=mlk::kFaultRunning;
        result.classIndex=mlk::kFaultNormal;
        for (int second=0;second<=80;second+=2) {
            result.startUs=base+quint64(second)*1000000; result.endUs=result.startUs+4000000;
            result.valid=second!=46; result.state=result.valid?mlk::kFaultRunning:mlk::kFaultDataInsufficient;
            result.classIndex=second==24?mlk::kFaultOverload:mlk::kFaultNormal;
            policy.observe(result);
        }
        const auto paths=policy.discardable(); QCOMPARE(paths,QStringList{normal.path});
        QStringList failed; QCOMPARE(mlk::removeCaptureFiles(directory.path(),paths,&failed),1); QVERIFY(failed.isEmpty());
        QVERIFY(QFile::exists(old.path)); QVERIFY(QFile::exists(fault.path));
        QVERIFY(QFile::exists(unknown.path)); QVERIFY(QFile::exists(recent.path));
        policy.end(); QVERIFY(policy.discardable().isEmpty());
        policy.begin(directory.path(),60,10); policy.saved(old);
        for (int second=0;second<=40;second+=2) {
            result.valid=true; result.state=mlk::kFaultRunning; result.classIndex=mlk::kFaultNormal;
            result.startUs=base+quint64(second)*1000000; result.endUs=result.startUs+4000000;
            policy.observe(result,second==4); // A physical threshold alarm protects otherwise normal data.
        }
        QVERIFY(policy.discardable().isEmpty()); QVERIFY(QFile::exists(old.path));
        policy.begin(directory.path(),60,10); policy.saved(old);
        for (int second=0;second<=40;second+=2) {
            result.startUs=base+quint64(second)*1000000; result.endUs=result.startUs+4000000; policy.observe(result);
        }
        const QJsonArray labels{QJsonObject{{"startUs",QString::number(base+3000000)},
            {"endUs",QString::number(base+4000000)},{"abnormal",true}}};
        QVERIFY(policy.discardable(labels).isEmpty()); QVERIFY(QFile::exists(old.path));
        policy.begin(directory.path(),60,10); policy.saved(old);
        result.startUs=base+40000000; result.endUs=result.startUs+4000000; policy.observe(result);
        result.startUs=base; result.endUs=base+4000000; policy.observe(result); // Time reset retains the prior epoch.
        result.startUs=base+80000000; result.endUs=result.startUs+4000000; policy.observe(result);
        QVERIFY(policy.discardable().isEmpty()); QVERIFY(QFile::exists(old.path));
    }
    void sampleClockAndSpeedEvents() {
        mlk::PlSampleClock clock;
        const quint64 epoch=1700000000000000ull;
        const quint32 origin=0xffff0000u;
        for (quint64 block=0;block<1600;++block) {
            const quint64 sampleUs=block*32000, delayUs=block%2?20000:1000;
            const quint64 readUs=sampleUs+31500+delayUs;
            // Two PL counter wraps, alternating read latency, and a host UTC step.
            QCOMPARE(clock.timestamp(quint32(origin+sampleUs*100),quint32(origin+readUs*100),
                epoch+readUs+(block>500?10000000:0),qint64(readUs)),epoch+sampleUs+(block>500?10000000:0));
        }
        mlk::FaultInputAdapter adapter; mlk::AppConfig cfg; adapter.configure(cfg); adapter.restart();
        const auto speed=adapter.acceptSpeed(15000,600,epoch,false);
        QCOMPARE(speed.count,quint32(1)); QCOMPARE(speed.values.size(),1);
        QCOMPARE(speed.endUs(),epoch); QCOMPARE(speed.values.first(),1500.f);
        QCOMPARE(adapter.acceptSpeed(15000,0xffffffffu,epoch+100000,false).count,quint32(1));
        mlk::Frame f; f.ver=mlk::kWaveformVersion; f.ch=mlk::kVibration; f.rateHz=2000;
        f.samples=QVector<qint32>(64,0); f.samplesY=f.samples; f.samplesZ=QVector<qint32>(64,-256);
        f.tsUs=epoch; f.seq=10; QVERIFY(!adapter.acceptFrame(f).gapBefore);
        f.tsUs+=32000; ++f.seq; QVERIFY(!adapter.acceptFrame(f).gapBefore);
        f.tsUs+=64000; ++f.seq; QVERIFY(adapter.acceptFrame(f).gapBefore);
    }
    void initTestCase() {
        QVERIFY(!packageRoot().isEmpty()); QVERIFY(!datasetRoot().isEmpty());
        QCoreApplication::setOrganizationName("MLK-Fault-Tests");
        QCoreApplication::setApplicationName("integration");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        const auto font = qEnvironmentVariable("MLK_TEST_FONT");
        if (!font.isEmpty()) QVERIFY(QFontDatabase::addApplicationFont(font) >= 0);
        applyTheme(*qApp);
    }
    void realFrozenDatasetAndReport() {
        mlk::FaultDataset dataset; QString error;
        QVERIFY2(dataset.load(datasetRoot(), &error), qPrintable(error));
        QCOMPARE(dataset.samples(), 360); QCOMPARE(dataset.inputFloats(), 27683);
        mlk::FaultEvalController controller;
        QSignalSpy done(&controller, &mlk::FaultEvalController::finished);
        QSignalSpy failed(&controller, &mlk::FaultEvalController::failed);
        QVERIFY(controller.start(packageRoot(), datasetRoot(), 1, &error));
        QTRY_COMPARE_WITH_TIMEOUT(done.count() + failed.count(), 1, 30000);
        QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed[0][0].toString()));
        const auto report = qvariant_cast<mlk::FaultEvalReport>(done[0][0]);
        QCOMPARE(report.evaluated, 360); QCOMPARE(report.classCorrect, 360);
        QCOMPARE(report.jointCorrect, 356); QCOMPARE(report.backend, QString("cpu"));
        QVERIFY(report.p95InferUs >= report.medianInferUs); QVERIFY(report.meanInferUs > 0);
        QTemporaryDir output;
        FaultPanel panel; panel.resize(600, 750); panel.evalPanel()->setBackend(1);
        panel.findChild<QTabWidget *>("faultModeTabs")->setCurrentIndex(1);
        panel.evalPanel()->setDatasetDirectory(output.path()); panel.evalPanel()->showReport(report);
        panel.show(); QTest::qWait(30);
        panel.findChild<QPushButton *>("faultEvalExport")->click();
        const auto saved = readJson(output.filePath("fault_eval_report.json"));
        QCOMPARE(saved.value("samples").toInt(), 360);
        QCOMPARE(saved.value("jointCorrect").toInt(), 356);
        QCOMPARE(saved.value("schema").toString(), QString("mlk.fault.eval_report.v1"));
        const auto artifacts = qEnvironmentVariable("MLK_TEST_OUTPUT");
        if (!artifacts.isEmpty()) {
            QVERIFY(panel.grab().save(artifacts + "/fault-evaluation-panel.png"));
            QVERIFY(writeJson(artifacts + "/qt-eval-report.json", saved));
        }
    }
    void cancelBeforeWorkerStartsAndRestart() {
        mlk::FaultEvalController controller; QString error;
        QSignalSpy failed(&controller, &mlk::FaultEvalController::failed);
        QSignalSpy done(&controller, &mlk::FaultEvalController::finished);
        QVERIFY(controller.start(packageRoot(), datasetRoot(), 1, &error));
        controller.stop();
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
        QVERIFY(failed[0][0].toString().contains(QStringLiteral("取消")));
        QVERIFY(done.isEmpty()); QVERIFY(!controller.running());
        QVERIFY(controller.start(packageRoot(), datasetRoot(), 1, &error));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 30000);
        QCOMPARE(qvariant_cast<mlk::FaultEvalReport>(done[0][0]).jointCorrect, 356);
    }
    void rejectsUnsealedOrMissingAssets() {
        QTemporaryDir root; QVERIFY(root.isValid());
        auto manifest = readJson(packageRoot() + "/model.json");
        for (const auto &name : manifest.value("files").toObject().keys())
            QVERIFY(QFile::link(packageRoot() + "/" + name, root.filePath(name)));
        manifest["files"] = QJsonObject();
        QVERIFY(writeJson(root.filePath("model.json"), manifest));
        QString error;
        QVERIFY(!mlk::FaultRuntime::verifyPackage(root.path(), 1, nullptr, nullptr, &error));
        QVERIFY(error.contains(QStringLiteral("哈希")));
        QVERIFY(QFile::remove(root.filePath("preprocess.json")));
        mlk::FaultPreprocessor preprocess;
        QVERIFY(!preprocess.loadAssets(root.path(), &error));
    }
    void rejectsDifferentDatasetModelAndInvalidGrade() {
        QTemporaryDir root; QVERIFY(root.isValid());
        auto manifest = readJson(datasetRoot() + "/evalset.json");
        QVERIFY(QFile::link(datasetRoot() + "/test_inputs.bin", root.filePath("test_inputs.bin")));
        QVERIFY(QFile::link(datasetRoot() + "/test_index.csv", root.filePath("test_index.csv")));
        manifest["model"] = "different-model";
        QVERIFY(writeJson(root.filePath("evalset.json"), manifest));
        mlk::FaultEvalController controller; QString error;
        QSignalSpy failed(&controller, &mlk::FaultEvalController::failed);
        QVERIFY(controller.start(packageRoot(), root.path(), 1, &error));
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
        QVERIFY(failed[0][0].toString().contains(QStringLiteral("身份")));
        QVERIFY(QFile::remove(root.filePath("test_index.csv")));
        QFile index(root.filePath("test_index.csv")); QVERIFY(index.open(QIODevice::WriteOnly));
        index.write("sample,class_target,grade_target\n0,0,1\n"); index.close();
        mlk::FaultDataset dataset;
        QVERIFY(!dataset.load(root.path(), &error));
        QVERIFY(error.contains(QStringLiteral("越界")));
    }
    void externalOrderingExpiryAndConfirmation() {
        mlk::FaultLiveController controller; QString error;
        mlk::FaultResult result;
        result.model = "f3p-fault-single-v1";
        result.run = QUuid::createUuid().toString(QUuid::WithoutBraces);
        result.seq = 1; result.valid = true; result.state = mlk::kFaultRunning;
        result.endUs = quint64(QDateTime::currentMSecsSinceEpoch() - 5850) * 1000;
        result.startUs = result.endUs - mlk::kFaultWindowUs;
        result.classIndex = 1; result.grade = 2; result.backend = 1;
        result.hasConfidence = true; result.confidence = 0.9;
        QVERIFY2(controller.acceptExternal(result, &error), qPrintable(error));
        QVERIFY(controller.acceptExternal(result, &error)); QVERIFY(!controller.confirmed());
        result.seq = 2;
        QVERIFY(controller.acceptExternal(result, &error)); QVERIFY(controller.confirmed());
        result.seq = 1;
        QVERIFY(!controller.acceptExternal(result, &error)); QCOMPARE(error, QString("stale_result"));
        QCOMPARE(controller.current().seq, quint32(2));
        result.seq = 2; result.grade = 3;
        QVERIFY(!controller.acceptExternal(result, &error)); QCOMPARE(error, QString("window_conflict"));
        QTest::qWait(200); controller.tick();
        QVERIFY(!controller.current().valid); QVERIFY(!controller.confirmed());
        QCOMPARE(controller.current().reason, QString("result_timeout"));
    }
    void confidenceGatePreservesRawPredictionAndRequiresConsecutiveWindows() {
        mlk::FaultLiveController controller; QString error;
        QSignalSpy confirmed(&controller,&mlk::FaultLiveController::confirmedEvent);
        QSignalSpy cleared(&controller,&mlk::FaultLiveController::confirmedCleared);
        mlk::FaultResult r; r.model="f3p-fault-single-v1";
        r.run=QUuid::createUuid().toString(QUuid::WithoutBraces);
        r.valid=true; r.state=mlk::kFaultRunning; r.classIndex=mlk::kFaultOverload;
        r.grade=1; r.backend=mlk::kFaultBackendCpu; r.hasConfidence=true;
        r.endUs=quint64(QDateTime::currentMSecsSinceEpoch())*1000;
        r.startUs=r.endUs-mlk::kFaultWindowUs;
        for (double score:{0.740415,0.683509,0.672537,0.799999}) {
            r.confidence=score; ++r.seq; QVERIFY(controller.acceptExternal(r,&error));
            QVERIFY(!controller.confirmed()); QVERIFY(controller.current().valid);
            QCOMPARE(controller.current().classIndex,int(mlk::kFaultOverload));
            QCOMPARE(controller.current().confidence,score);
        }
        QCOMPARE(confirmed.count(),0);
        r.confidence=0.80; ++r.seq; QVERIFY(controller.acceptExternal(r,&error));
        QVERIFY(!controller.confirmed());
        r.confidence=0.70; ++r.seq; QVERIFY(controller.acceptExternal(r,&error));
        r.confidence=0.80; ++r.seq; QVERIFY(controller.acceptExternal(r,&error));
        QVERIFY(!controller.confirmed());
        ++r.seq; QVERIFY(controller.acceptExternal(r,&error));
        QVERIFY(controller.confirmed()); QCOMPARE(confirmed.count(),1);
        r.confidence=0.70; QVERIFY(!controller.acceptExternal(r,&error));
        QCOMPARE(error,QString("window_conflict")); QVERIFY(controller.confirmed());
        r.confidence=0.79; ++r.seq; QVERIFY(controller.acceptExternal(r,&error));
        QVERIFY(!controller.confirmed()); QCOMPARE(cleared.count(),1);
        FaultPanel panel; panel.showResult(r,0,false,false);
        bool found=false;
        for (auto *label:panel.findChildren<QLabel*>())
            found=found || label->text().contains(QStringLiteral("低置信度候选"));
        QVERIFY(found);
    }
    void windowEvaluationToLiveWaitsAndCanStop() {
        QTemporaryDir config; QVERIFY(config.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, config.path());
        qputenv("MLK_FAULT_ROOT", packageRoot().toUtf8());
        qputenv("MLK_FAULT_EVAL_ROOT", datasetRoot().toUtf8());
        qputenv("MLK_FAULT_SOCKET", config.filePath("fault.sock").toUtf8());
        qputenv("MLK_CAPTURE_PS_SOCKET", config.filePath("capture.sock").toUtf8());
        mlk::AppConfig cfg; cfg.storageDir = config.path(); cfg.passphrase="fault-test"; cfg.autoDiscover = false;
        cfg.autoConnect = false; QVERIFY(cfg.save());
        KLocalWindow window; window.resize(1920, 1080); window.show();
        auto *panel = window.findChild<FaultPanel *>(); QVERIFY(panel);
        auto *eval = window.findChild<mlk::FaultEvalController *>(); QVERIFY(eval);
        auto *live = window.findChild<mlk::FaultLiveController *>(); QVERIFY(live);
        auto *tabs = panel->findChild<QTabWidget *>("faultModeTabs");
        panel->evalPanel()->setBackend(1); panel->setBackend(1);
        tabs->setCurrentIndex(1);
        panel->findChild<QPushButton *>("faultEvalStart")->click(); QVERIFY(eval->running());
        auto *replay = window.findChild<mlk::ReplayController *>(); QVERIFY(replay);
        QSignalSpy replayLoads(replay, &mlk::ReplayController::loadRequested);
        auto *replayPanel = window.findChild<mlk::ReplayPanel *>(); QVERIFY(replayPanel);
        replayPanel->setDirectory("/missing-rul");
        replayPanel->requested(mlk::kReplayStart, 1, 0, 1000);
        QCOMPARE(replayLoads.count(), 0);
        tabs->setCurrentIndex(0);
        panel->findChild<QPushButton *>("faultStart")->click();
        QTRY_VERIFY_WITH_TIMEOUT(live->state() == mlk::kFaultWaiting, 10000);
        QVERIFY(window.findChild<QPushButton *>("recordButton")->isChecked());
        QVERIFY(!eval->running());
        panel->findChild<QPushButton *>("faultStop")->click();
        QTRY_COMPARE_WITH_TIMEOUT(live->state(), int(mlk::kFaultStopped), 10000);
        tabs->setCurrentIndex(1);
        panel->findChild<QPushButton *>("faultEvalStart")->click();
        QTRY_VERIFY_WITH_TIMEOUT(panel->findChild<QPushButton *>("faultEvalExport")->isEnabled(), 30000);
        QVERIFY(!live->busy()); QVERIFY(!eval->running());
        QVERIFY2(window.height() <= 1080, qPrintable(QString::number(window.height())));
        const auto artifacts = qEnvironmentVariable("MLK_TEST_OUTPUT");
        if (!artifacts.isEmpty()) QVERIFY(window.grab().save(artifacts + "/fault-window.png"));
        // A pending evaluation can also be replaced before its worker starts.
        QCOMPARE(replay->control(mlk::kReplayStop, 1, 0, 1000), QString());
        panel->findChild<QPushButton *>("faultEvalStart")->click(); QVERIFY(!eval->running());
        tabs->setCurrentIndex(0);
        panel->findChild<QPushButton *>("faultStart")->click();
        QTRY_COMPARE_WITH_TIMEOUT(live->state(), int(mlk::kFaultWaiting), 10000);
        QVERIFY(panel->findChild<QPushButton *>("faultEvalStart")->isEnabled());
        panel->findChild<QPushButton *>("faultStop")->click();
        QTRY_COMPARE_WITH_TIMEOUT(live->state(), int(mlk::kFaultStopped), 10000);
        tabs->setCurrentIndex(1);
    }
    void liveStreamRecordingAndLabeledReview() {
        QTemporaryDir config; QVERIFY(config.isValid());
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,config.path());
        qputenv("MLK_RUL_AUTOSTART","0");
        qputenv("MLK_FAULT_ROOT",packageRoot().toUtf8());
        qputenv("MLK_FAULT_SOCKET",config.filePath("fault.sock").toUtf8());
        qputenv("MLK_CAPTURE_PS_SOCKET",config.filePath("capture.sock").toUtf8());
        mlk::AppConfig cfg; cfg.storageDir=config.path()+"/captures"; cfg.passphrase="fault-review-test";
        cfg.minFreeMb=1; cfg.recordSegmentSec=300; cfg.autoConnect=false; cfg.autoDiscover=false;
        QVERIFY(cfg.save());
        KLocalWindow window; window.resize(1920,1080); window.show();
        auto *panel=window.findChild<FaultPanel *>(); QVERIFY(panel); panel->setBackend(1);
        auto *live=window.findChild<mlk::FaultLiveController *>(); QVERIFY(live);
        auto *record=window.findChild<QPushButton *>("recordButton"); QVERIFY(record); QVERIFY(!record->isChecked());
        QVERIFY(!panel->discardNormalRecordings());
        panel->findChild<QPushButton *>("faultStart")->click();
        QTRY_VERIFY_WITH_TIMEOUT(live->running(),10000); QVERIFY(record->isChecked());
        const quint64 base=(mlk::captureNowUs()/2000000)*2000000-8000000;
        quint32 seq[4]={}; mlk::PlSampleClock clock;
        auto frame=[&](int ch,quint64 us,quint16 rate) {
            mlk::Frame f; f.ver=mlk::kWaveformVersion; f.ch=quint8(ch); f.tsUs=base+us;
            f.rateHz=rate; f.seq=seq[ch]++; f.samples={ch==mlk::kTemp?250:1000};
            return f;
        };
        for (quint64 us=0;us<8000000;us+=20000) {
            if (us==4000000) {record->click();record->click();} // Model window spans both files.
            auto current=frame(mlk::kCurrent,us,50);
            QVERIFY(QMetaObject::invokeMethod(&window,"onFrame",Qt::DirectConnection,Q_ARG(mlk::Frame,current)));
            if (us%1000000==0) {
                auto temp=frame(mlk::kTemp,us,1);
                QVERIFY(QMetaObject::invokeMethod(&window,"onFrame",Qt::DirectConnection,Q_ARG(mlk::Frame,temp)));
            }
            if (us%100000==0) {
                auto speed=frame(mlk::kSpeed,us,10); speed.cnt=600+quint32(us/100000); speed.samples={15000};
                QVERIFY(QMetaObject::invokeMethod(&window,"onFrame",Qt::DirectConnection,Q_ARG(mlk::Frame,speed)));
            }
            // Full native DMA blocks with jittered polling; no artificial zero padding.
            for (quint64 block=us/32000;block*32000<us+20000;++block) {
                if (block*32000<us) continue;
                if (block%17==8) { ++seq[mlk::kVibration]; continue; } // Real missing block, preserved in quality.
                auto vib=frame(mlk::kVibration,block*32000,2000);
                const quint64 sampleUs=block*32000, readUs=sampleUs+31500+(block%2?20000:1000);
                vib.tsUs=clock.timestamp(quint32(0xffff0000u+sampleUs*100),
                    quint32(0xffff0000u+readUs*100),base+readUs,qint64(readUs));
                vib.samples=QVector<qint32>(64,0); vib.samplesY=vib.samples; vib.samplesZ=QVector<qint32>(64,-256);
                QVERIFY(QMetaObject::invokeMethod(&window,"onFrame",Qt::DirectConnection,Q_ARG(mlk::Frame,vib)));
            }
        }
        QTRY_VERIFY_WITH_TIMEOUT(live->current().valid,30000);
        QCOMPARE(live->state(),int(mlk::kFaultRunning));
        QVERIFY(live->current().quality.value("fastRatio").toDouble()>=0.75);
        QVERIFY(live->current().quality.value("slowRatio").toDouble()>=0.60);
        QVERIFY(live->current().quality.value("gapBlocks").toInt()>0);
        QVERIFY(panel->findChild<QLabel *>("faultLiveState")->text().contains(QStringLiteral("按两秒步长诊断")));
        QVERIFY(!panel->findChild<QLabel *>("faultLiveState")->text().contains(QStringLiteral("等待四秒窗口")));
        auto reset=frame(mlk::kVibration,0,2000);
        reset.samples=QVector<qint32>(64,0); reset.samplesY=reset.samples; reset.samplesZ=QVector<qint32>(64,-256);
        QVERIFY(QMetaObject::invokeMethod(&window,"onFrame",Qt::DirectConnection,Q_ARG(mlk::Frame,reset)));
        QTRY_COMPARE_WITH_TIMEOUT(live->state(),int(mlk::kFaultWaiting),10000);
        QVERIFY(!live->current().valid); QCOMPARE(live->current().reason,QString("waiting_context"));
        panel->findChild<QPushButton *>("faultStop")->click();
        QTRY_COMPARE_WITH_TIMEOUT(live->state(),int(mlk::kFaultStopped),10000);
        mlk::FaultResult result=live->current(); result.model="f3p-fault-single-v1";
        result.run=QUuid::createUuid().toString(QUuid::WithoutBraces); result.preprocess=mlk::kFaultPreprocess;
        result.seq=1; result.valid=true; result.state=mlk::kFaultRunning; result.classIndex=1; result.grade=2;
        result.startUs=base+2000000; result.endUs=base+6000000; result.backend=1;
        result.hasConfidence=true; result.confidence=0.95; result.reason.clear();
        QString why; QVERIFY2(result.validate(&why),qPrintable(why));
        result.confidence=0.74;
        QVERIFY(window.reportDiagnosisResult(result.toJson()).value("ok").toBool());
        ++result.seq;
        QVERIFY(window.reportDiagnosisResult(result.toJson()).value("ok").toBool());
        auto *alarm=window.findChild<AlarmController *>(); QVERIFY(alarm);
        QCOMPARE((alarm->desiredState()>>4)&3u,0u);
        QVERIFY(mlk::WorkOrders(cfg.storageDir,cfg.passphrase).latest().isEmpty());
        result.confidence=0.95; ++result.seq;
        const auto reply=window.reportDiagnosisResult(result.toJson());
        QVERIFY2(reply.value("ok").toBool(),QJsonDocument(reply).toJson().constData());
        ++result.seq;
        const auto confirmed=window.reportDiagnosisResult(result.toJson());
        QVERIFY2(confirmed.value("ok").toBool(),QJsonDocument(confirmed).toJson().constData());
        QCOMPARE((alarm->desiredState()>>4)&3u,2u);
        const auto order=mlk::WorkOrders(cfg.storageDir,cfg.passphrase).latest();
        QVERIFY(!order.isEmpty()); QCOMPARE(order.value("diagnosis").toObject().value("classIndex").toInt(),1);
        mlk::CaptureStore store(cfg.storageDir,cfg.passphrase); QJsonArray labels; QString error;
        QVERIFY2(store.labels(labels,&error),qPrintable(error)); QVERIFY(!labels.isEmpty());
        const auto label=labels.last().toObject(); QCOMPARE(label.value("label").toString(),QString("overload_grade_2"));
        QCOMPARE(label.value("scope").toString(),QString("model_window"));
        QCOMPARE(label.value("startUs").toString().toULongLong(),result.startUs);
        window.findChild<QPushButton *>("captureReplayButton")->click();
        auto *dialog=window.findChild<mlk::CaptureDialog *>(); QVERIFY(dialog);
        dialog->openOrder(order.value("id").toString());
        QCOMPARE(dialog->findChild<QComboBox *>("captureRelated")->count(),2);
        QVERIFY(dialog->findChild<QLabel *>("captureDetails")->text().contains(QStringLiteral("过载")));
        QVERIFY(dialog->findChild<QTableView *>("captureData")->model()->rowCount()>0);
        const auto artifacts=qEnvironmentVariable("MLK_TEST_OUTPUT");
        if (!artifacts.isEmpty()) QVERIFY(dialog->grab().save(artifacts+"/fault-labeled-review.png"));
        result.classIndex=mlk::kFaultNormal; result.grade=0;
        for (int i=0;i<3;++i) {++result.seq; QVERIFY(window.reportDiagnosisResult(result.toJson()).value("ok").toBool());}
        QCOMPARE((alarm->desiredState()>>4)&3u,0u);
        dialog->close(); record->setChecked(false);
        const auto logs=QDir(cfg.storageDir+"/fault").entryList({"fault_*.mlkf"},QDir::Files);
        QVERIFY2(!logs.isEmpty(),"live journal must not delete itself by converting MiB twice");
    }
};
QTEST_MAIN(FaultTest)
#include "test_fault.moc"
