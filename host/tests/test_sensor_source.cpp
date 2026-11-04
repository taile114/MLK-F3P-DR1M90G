#include <QtTest>
#include <QTemporaryFile>
#include <QSettings>
#include <QTemporaryDir>
#include <fcntl.h>
#include <unistd.h>
#include <cstdarg>
#include <cstring>
#include "datasource.h"
#include "appconfig.h"
#include "sharedconfig.h"
#include "../ps-local/driver/pl_ctrl_uapi.h"

namespace { QByteArray deviceFixture; }
extern "C" int __real_open(const char *, int, ...);
extern "C" int __wrap_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list args; va_start(args,flags); mode=va_arg(args,int); va_end(args); }
    if (!deviceFixture.isEmpty()) {
        if (std::strcmp(path,"/dev/pl_ctrl")==0) path=deviceFixture.constData();
        // This regression never accesses physical memory or a real PL device.
        else if (std::strcmp(path,"/dev/mem")==0) { errno=EACCES; return -1; }
    }
    return __real_open(path,flags,mode);
}

class SensorSourceTest : public QObject {
    Q_OBJECT
    void put(int fd, unsigned offset, quint32 value) {
        QCOMPARE(::pwrite(fd,&value,sizeof(value),offset),ssize_t(sizeof(value)));
    }
    quint32 get(int fd, unsigned offset) {
        quint32 value=0; const auto n=::pread(fd,&value,sizeof(value),offset);
        return n==sizeof(value)?value:0xffffffffu;
    }
private slots:
    void cleanup() { deviceFixture.clear(); }
    void acquisitionCalibrationAndHealth() {
        QTemporaryFile file; QVERIFY(file.open()); QVERIFY(file.resize(0x1000));
        const int fd=file.handle();
        put(fd,0,PL_CTRL_ID_VALUE); put(fd,0x38,1); put(fd,0x68,20800);
        deviceFixture=file.fileName().toLocal8Bit();
        mlk::DriverSource source;
        QSignalSpy frames(&source,&mlk::DriverSource::frameReceived);
        QSignalSpy health(&source,&mlk::DriverSource::currentHealthChanged);
        QVERIFY(source.start());
        QCOMPARE(get(fd,0x4c),20000u); QCOMPARE(get(fd,0x50),1250u);
        QTRY_VERIFY(!frames.isEmpty());
        auto f=qvariant_cast<mlk::Frame>(frames.last()[0]);
        QCOMPARE(f.ch,quint8(mlk::kCurrent)); QCOMPARE(f.samples.back(),1000);
        QString error; QVERIFY2(source.zeroCurrent(&error),qPrintable(error));
        QCOMPARE(source.currentZero(),20800); QCOMPARE(get(fd,0x50),1250u);
        QCOMPARE(qvariant_cast<mlk::Frame>(frames.last()[0]).samples.back(),0);
        // Simulate PL calibration registers cleared by a reset.
        put(fd,0x4c,0); put(fd,0x50,0);
        QTRY_COMPARE(get(fd,0x4c),20800u); QCOMPARE(get(fd,0x50),1250u);
        source.configureCurrent(20000,1250);
        QCOMPARE(get(fd,0x4c),20000u);
        put(fd,0x68,24000); put(fd,0x38,2);
        QTRY_COMPARE(qvariant_cast<mlk::Frame>(frames.last()[0]).samples.back(),5000);
        frames.clear(); health.clear(); put(fd,0x68,32767); put(fd,0x38,3);
        QTRY_VERIFY(!health.isEmpty());
        QVERIFY(health.last()[1].toString().contains(QStringLiteral("ADC 饱和")));
        QVERIFY(frames.isEmpty());
        put(fd,0x68,20800); put(fd,0x38,4);
        QTRY_VERIFY(!frames.isEmpty()); QCOMPARE(qvariant_cast<mlk::Frame>(frames.last()[0]).samples.back(),1000);
        health.clear(); frames.clear();
        QTRY_VERIFY(!health.isEmpty()); QVERIFY(health.last()[1].toString().contains(QStringLiteral("无有效数据")));
        QVERIFY(frames.isEmpty());
        source.configureCurrent(20000,0); put(fd,0x38,5); health.clear();
        QTRY_VERIFY(!health.isEmpty()); QVERIFY(health.last()[1].toString().contains(QStringLiteral("未标定")));
        QVERIFY(frames.isEmpty()); source.stop();
    }
    void persistentAndSharedCalibration() {
        QTemporaryDir dir; QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.path());
        QCoreApplication::setOrganizationName("MLK-Sensor-Tests");
        QCoreApplication::setApplicationName("calibration");
        mlk::AppConfig cfg; cfg.load(); QCOMPARE(cfg.currentZero,20000); QCOMPARE(cfg.currentGain,1250);
        cfg.currentZero=20123; QVERIFY(cfg.save());
        mlk::AppConfig restarted; restarted.load(); QCOMPARE(restarted.currentZero,20123);
        QCOMPARE(restarted.currentGain,1250);
        mlk::AppConfig remote; QVERIFY(mlk::readSharedConfig(mlk::sharedConfig(restarted),remote));
        QCOMPARE(remote.currentZero,20123); QCOMPARE(remote.currentGain,1250);
        auto invalid=mlk::sharedConfig(restarted);
        invalid.insert("currentCalibration",QJsonObject{{"zero",20000},{"gainUaPerLsb",-1}});
        QVERIFY(!mlk::readSharedConfig(invalid,remote)); QCOMPARE(remote.currentZero,20123);
    }
};
QTEST_GUILESS_MAIN(SensorSourceTest)
#include "test_sensor_source.moc"
