#pragma once

#include <QSettings>
#include <QCoreApplication>
#include <QString>
#include <QFile>
#include "crypto.h"
#include "sensorunits.h"

namespace mlk {

struct ChannelConfig
{
    double k = 1.0;
    double b = 0.0;
    double alarmLo = 0.0;
    double alarmHi = 0.0;
    bool alarmEnabled = false;
};

struct AppConfig
{
    AppConfig()
    {
        // Wire units: 0.1 C, mA, 0.0039 g, 0.1 rpm; current/speed signed32.
        ch[0].k = 0.1;
        ch[1].k = 1.0;
        ch[2].k = 0.0039;
        ch[3].k = 0.1;
    }

    QString settingsOrganization = QCoreApplication::organizationName();
    QString settingsApplication = QCoreApplication::applicationName();
    ChannelConfig ch[4];
    qint32 currentZero = kCurrentZeroDefault;
    qint32 currentGain = kCurrentGainDefault;
    int fftPoints = 1024;
    int uiFps = 2;
    QString addr = QStringLiteral("0.0.0.0");
    int port = 8000;
    QString targetAddr = QStringLiteral("192.168.1.10");
    int targetPort = 8000;
    bool autoDiscover = true;
    bool autoConnect = true;
    int injectDurationMs = 3000;
    QString storageDir = QStringLiteral("/data/mlk_f3p");
    bool encrypt = true;
    QString passphrase = QStringLiteral("mlk-f3p");
    int retentionMonths = 3;
    int minFreeMb = 512;
    int recordSegmentSec = 60;
    QString settingsPassword = QStringLiteral("root");
    bool remotePriority = false;
    quint32 sharedRevision = 1;
    /* 本机保存的共享参数是否可作为提交值：首次收到板端快照时先采用板端值，
     * 之后 remote-pc 优先期间以本机值为准并在连接时覆盖板端。 */
    bool sharedShadow = false;

    int secMode = 2;
    QString pskHex;
    QString selfPubHex;
    QString selfPrivHex;
    QString peerPubHex;

    bool ensureIdentity()
    {
        if (!selfPubHex.isEmpty() || !selfPrivHex.isEmpty())
            return QByteArray::fromHex(selfPubHex.toLatin1()).size() == 32
                && QByteArray::fromHex(selfPrivHex.toLatin1()).size() == 32;
        QByteArray pub, priv;
        if (!crypto::x25519Keygen(pub, priv)) return false;
        selfPubHex = pub.toHex(); selfPrivHex = priv.toHex();
        return save();
    }

    void load()
    {
        QSettings s(QSettings::defaultFormat(), QSettings::UserScope, settingsOrganization, settingsApplication);
        for (int i = 0; i < 4; ++i) {
            s.beginGroup(QStringLiteral("ch%1").arg(i));
            ch[i].k = s.value(QStringLiteral("k"), ch[i].k).toDouble();
            ch[i].b = s.value(QStringLiteral("b"), ch[i].b).toDouble();
            ch[i].alarmLo = s.value(QStringLiteral("alarmLo"), ch[i].alarmLo).toDouble();
            ch[i].alarmHi = s.value(QStringLiteral("alarmHi"), ch[i].alarmHi).toDouble();
            ch[i].alarmEnabled = s.value(QStringLiteral("alarmEnabled"), ch[i].alarmEnabled).toBool();
            s.endGroup();
        }
        s.beginGroup(QStringLiteral("app"));
        currentZero = s.value(QStringLiteral("currentZero"), currentZero).toInt();
        currentGain = s.value(QStringLiteral("currentGain"), currentGain).toInt();
        if (currentZero < -32768 || currentZero > 32767) currentZero = kCurrentZeroDefault;
        if (currentGain < 0 || currentGain > 1000000) currentGain = kCurrentGainDefault;
        fftPoints = s.value(QStringLiteral("fftPoints"), fftPoints).toInt();
        uiFps = s.value(QStringLiteral("uiFps"), uiFps).toInt();
        addr = s.value(QStringLiteral("addr"), addr).toString();
        port = s.value(QStringLiteral("port"), port).toInt();
        targetAddr = s.value(QStringLiteral("targetAddr"), targetAddr).toString();
        targetPort = s.value(QStringLiteral("targetPort"), targetPort).toInt();
        autoDiscover = s.value(QStringLiteral("autoDiscover"), autoDiscover).toBool();
        autoConnect = s.value(QStringLiteral("autoConnect"), autoConnect).toBool();
        injectDurationMs = s.value(QStringLiteral("injectDurationMs"), injectDurationMs).toInt();
        storageDir = s.value(QStringLiteral("storageDir"), storageDir).toString();
        encrypt = s.value(QStringLiteral("encrypt"), encrypt).toBool();
        passphrase = s.value(QStringLiteral("passphrase"), passphrase).toString();
        retentionMonths = s.value(QStringLiteral("retentionMonths"), retentionMonths).toInt();
        minFreeMb = s.value(QStringLiteral("minFreeMb"), minFreeMb).toInt();
        recordSegmentSec = s.value(QStringLiteral("recordSegmentSec"), recordSegmentSec).toInt();
        settingsPassword = s.value(QStringLiteral("settingsPassword"), settingsPassword).toString();
        secMode = s.value(QStringLiteral("secMode"), secMode).toInt();
        pskHex = s.value(QStringLiteral("pskHex"), pskHex).toString();
        selfPubHex = s.value(QStringLiteral("selfPubHex"), selfPubHex).toString();
        selfPrivHex = s.value(QStringLiteral("selfPrivHex"), selfPrivHex).toString();
        peerPubHex = s.value(QStringLiteral("peerPubHex"), peerPubHex).toString();
        remotePriority = s.value(QStringLiteral("remotePriority"), remotePriority).toBool();
        sharedRevision = s.value(QStringLiteral("sharedRevision"), sharedRevision).toUInt();
        sharedShadow = s.value(QStringLiteral("sharedShadow"), sharedShadow).toBool();
        s.endGroup();
    }

    bool save() const
    {
        QSettings s(QSettings::defaultFormat(), QSettings::UserScope, settingsOrganization, settingsApplication);
        for (int i = 0; i < 4; ++i) {
            s.beginGroup(QStringLiteral("ch%1").arg(i));
            s.setValue(QStringLiteral("k"), ch[i].k);
            s.setValue(QStringLiteral("b"), ch[i].b);
            s.setValue(QStringLiteral("alarmLo"), ch[i].alarmLo);
            s.setValue(QStringLiteral("alarmHi"), ch[i].alarmHi);
            s.setValue(QStringLiteral("alarmEnabled"), ch[i].alarmEnabled);
            s.endGroup();
        }
        s.beginGroup(QStringLiteral("app"));
        s.setValue(QStringLiteral("currentZero"), currentZero);
        s.setValue(QStringLiteral("currentGain"), currentGain);
        s.setValue(QStringLiteral("fftPoints"), fftPoints);
        s.setValue(QStringLiteral("uiFps"), uiFps);
        s.setValue(QStringLiteral("addr"), addr);
        s.setValue(QStringLiteral("port"), port);
        s.setValue(QStringLiteral("targetAddr"), targetAddr);
        s.setValue(QStringLiteral("targetPort"), targetPort);
        s.setValue(QStringLiteral("autoDiscover"), autoDiscover);
        s.setValue(QStringLiteral("autoConnect"), autoConnect);
        s.setValue(QStringLiteral("injectDurationMs"), injectDurationMs);
        s.setValue(QStringLiteral("storageDir"), storageDir);
        s.setValue(QStringLiteral("encrypt"), encrypt);
        s.setValue(QStringLiteral("passphrase"), passphrase);
        s.setValue(QStringLiteral("retentionMonths"), retentionMonths);
        s.setValue(QStringLiteral("minFreeMb"), minFreeMb);
        s.setValue(QStringLiteral("recordSegmentSec"), recordSegmentSec);
        s.setValue(QStringLiteral("settingsPassword"), settingsPassword);
        s.setValue(QStringLiteral("secMode"), secMode);
        s.setValue(QStringLiteral("pskHex"), pskHex);
        s.setValue(QStringLiteral("selfPubHex"), selfPubHex);
        s.setValue(QStringLiteral("selfPrivHex"), selfPrivHex);
        s.setValue(QStringLiteral("peerPubHex"), peerPubHex);
        s.setValue(QStringLiteral("remotePriority"), remotePriority);
        s.setValue(QStringLiteral("sharedRevision"), sharedRevision);
        s.setValue(QStringLiteral("sharedShadow"), sharedShadow);
        s.endGroup();
        s.sync();
        if (s.status() != QSettings::NoError) return false;
#ifdef Q_OS_UNIX
        return QFile(s.fileName()).setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
#else
        return true;
#endif
    }
};

} // namespace mlk
