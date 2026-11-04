#include <QtTest>
#include "npumonitor.h"
#include "networkstatus.h"

namespace {
QByteArray interrupts(quint64 hard, quint64 soft)
{
    return QByteArray("           CPU0       CPU1\n")
        + " 41: " + QByteArray::number(hard) + " 0 GICv3 144 Level HARD_NPU_IRQ\n"
        + " 42: 0 " + QByteArray::number(soft) + " GICv3 129 Level SOFT_NPU_IRQ\n";
}
}

class NpuMonitorTest : public QObject
{
    Q_OBJECT
private slots:
    void telemetryRoundTrip()
    {
        mlk::NpuMonitor monitor;
        monitor.sample(true, true, true, interrupts(9007199254740993ULL, 0), 0);
        const auto original = monitor.sample(true, true, true, interrupts(9007199254741026ULL, 0), 1000);
        mlk::NpuStats decoded;
        const auto wire = QJsonDocument::fromJson(QJsonDocument(original.toJson()).toJson()).object();
        QVERIFY(mlk::NpuStats::fromJson(wire, decoded));
        QCOMPARE(decoded.hardInterrupts, original.hardInterrupts);
        QCOMPARE(decoded.hardInterruptsPerSec, 33.0);
        QCOMPARE(decoded.text(), original.text());
        QVERIFY(mlk::NpuStats::fromJson(mlk::NpuStats{}.toJson(), decoded));
        QCOMPARE(decoded.hardInterruptsPerSec, -1.0);
        for (const auto &key : wire.keys()) {
            auto missing = wire; missing.remove(key);
            QVERIFY(!mlk::NpuStats::fromJson(missing, decoded));
        }
        auto bad = wire; bad["hardInterrupts"] = 9007199254741026.0;
        QVERIFY(!mlk::NpuStats::fromJson(bad, decoded));
        bad = wire; bad["hardInterruptsPerSec"] = -2;
        QVERIFY(!mlk::NpuStats::fromJson(bad, decoded));
        bad = wire; bad["hardReady"] = false;
        QVERIFY(!mlk::NpuStats::fromJson(bad, decoded));
        bad = wire; bad["version"] = 2;
        QVERIFY(!mlk::NpuStats::fromJson(bad, decoded));
    }

    void boardNetworkSelection()
    {
        QVector<mlk::NetworkLink> links = {{"eth0", false, false, ""}, {"wlan0", true, true, "aic8800"},
                                          {"eth1", true, false, "GobiNet"}};
        const QByteArray routes = "Iface Destination Gateway Flags RefCnt Use Metric Mask\n"
            "eth0 00000000 0100000A 0003 0 0 1 00000000\n"
            "wlan0 00000000 0100000A 0003 0 0 600 00000000\n"
            "eth1 00000000 0100000A 0003 0 0 700 00000000\n";
        auto status = mlk::selectNetworkStatus(routes, links);
        QCOMPARE(status.text(), QStringLiteral("网络：WiFi"));
        status = mlk::selectNetworkStatus(QByteArray(routes).replace("0 0 700", "0 0 100"), links);
        QCOMPARE(status.text(), QStringLiteral("网络：4G"));
        QCOMPARE(status.interface, QStringLiteral("eth1"));
        links[0].connected = true;
        QCOMPARE(mlk::selectNetworkStatus(routes, links).kind, QStringLiteral("ethernet"));
        QCOMPARE(mlk::selectNetworkStatus({}, links).kind, QStringLiteral("wifi"));
        links[1].connected = false;
        QCOMPARE(mlk::selectNetworkStatus({}, links).kind, QStringLiteral("cellular"));
        for (auto &link : links) link.connected = false;
        QCOMPARE(mlk::selectNetworkStatus(routes, links).text(), QStringLiteral("网络：未连接"));
        mlk::NetworkStatus decoded;
        QVERIFY(mlk::NetworkStatus::fromJson(status.toJson(), decoded));
        QCOMPARE(decoded.text(), status.text());
        QVERIFY(!mlk::NetworkStatus::fromJson({}, decoded));
        auto invalid = status.toJson(); invalid["kind"] = "wifi-from-pc";
        QVERIFY(!mlk::NetworkStatus::fromJson(invalid, decoded));
    }

    void hardActivityAndIdle()
    {
        mlk::NpuMonitor monitor;
        auto stats = monitor.sample(true, true, true, interrupts(100, 0), 0);
        QCOMPARE(stats.hardInterrupts, quint64(100));
        QCOMPARE(stats.softInterrupts, quint64(0));
        QCOMPARE(stats.hardInterruptsPerSec, -1.0);
        QVERIFY(stats.text().contains(QStringLiteral("等待活动采样")));
        stats = monitor.sample(true, true, true, interrupts(156, 0), 2000);
        QCOMPARE(stats.hardInterruptsPerSec, 28.0);
        QCOMPARE(stats.softInterruptsPerSec, 0.0);
        QVERIFY(stats.text().contains(QStringLiteral("检测到中断活动")));
        QVERIFY(stats.text().contains(QStringLiteral("使用率：驱动未提供")));
        stats = monitor.sample(true, true, true, interrupts(156, 0), 3000);
        QCOMPARE(stats.hardInterruptsPerSec, 0.0);
        QVERIFY(stats.text().contains(QStringLiteral("本周期无中断")));
        QVERIFY(!stats.text().contains(QStringLiteral("空闲")));
    }

    void softActivity()
    {
        mlk::NpuMonitor monitor;
        monitor.sample(true, true, true, interrupts(0, 0), 1000);
        auto stats = monitor.sample(true, true, true, interrupts(0, 4), 1500);
        QCOMPARE(stats.softInterruptsPerSec, 8.0);
        QCOMPARE(stats.hardInterruptsPerSec, 0.0);
        QVERIFY(stats.text().contains(QStringLiteral("检测到中断活动")));
    }

    void missingDevices()
    {
        mlk::NpuMonitor monitor;
        auto stats = monitor.sample(false, false, false, interrupts(100, 200), 1000);
        QVERIFY(!stats.hardIrqAvailable);
        QVERIFY(!stats.softIrqAvailable);
        QVERIFY(stats.text().contains(QStringLiteral("缺少 硬核、软核、CMA")));
        stats = monitor.sample(true, true, false, interrupts(100, 200), 2000);
        QVERIFY(stats.text().contains(QStringLiteral("缺少 CMA")));
        QVERIFY(!stats.text().contains(QStringLiteral("驱动就绪")));
    }

    void unreadableStatisticsAndRecovery()
    {
        mlk::NpuMonitor monitor;
        monitor.sample(true, true, true, interrupts(100, 0), 1000);
        auto stats = monitor.sample(true, true, true, {}, 2000);
        QVERIFY(!stats.hardIrqAvailable);
        QVERIFY(stats.text().contains(QStringLiteral("活动统计受限")));
        QVERIFY(stats.text().contains(QStringLiteral("中断统计不可读")));
        stats = monitor.sample(true, true, true, interrupts(200, 0), 3000);
        QCOMPARE(stats.hardInterruptsPerSec, -1.0);
        stats = monitor.sample(true, true, true, interrupts(210, 0), 4000);
        QCOMPARE(stats.hardInterruptsPerSec, 10.0);
    }

    void resetAndDeviceRemoval()
    {
        mlk::NpuMonitor monitor;
        monitor.sample(true, true, true, interrupts(100, 0), 1000);
        auto stats = monitor.sample(true, true, true, interrupts(5, 0), 2000);
        QCOMPARE(stats.hardInterruptsPerSec, -1.0);
        monitor.sample(false, true, true, interrupts(10, 0), 3000);
        stats = monitor.sample(true, true, true, interrupts(20, 0), 4000);
        QCOMPARE(stats.hardInterruptsPerSec, -1.0);
        monitor.reset();
        stats = monitor.sample(true, true, true, interrupts(25, 0), 0);
        QCOMPARE(stats.hardInterruptsPerSec, -1.0);
    }

    void varyingCpuColumnsAndIrqNumbers()
    {
        mlk::NpuMonitor monitor;
        auto stats = monitor.sample(true, true, true,
            " CPU0 CPU1 CPU2 CPU3\n"
            " 93: 1 2 3 4 GICv3 144 Level HARD_NPU_IRQ\n"
            " 94: 5 6 7 8 GICv3 129 Level SOFT_NPU_IRQ\n", 0);
        QCOMPARE(stats.hardInterrupts, quint64(10));
        QCOMPARE(stats.softInterrupts, quint64(26));
        monitor.reset();
        stats = monitor.sample(true, true, true,
            " CPU0\n 7: 4000000000 GICv3 144 Level hard_npu\n"
            " 9: 0 GICv3 129 Level soft_npu\n", 0);
        QCOMPARE(stats.hardInterrupts, quint64(4000000000ULL));
        QVERIFY(stats.softIrqAvailable);
    }

    void malformedStatistics()
    {
        mlk::NpuMonitor monitor;
        auto stats = monitor.sample(true, true, true,
            " CPU0 CPU1\n 41: invalid 0 GICv3 144 Level HARD_NPU_IRQ\n"
            " 42: 0 0 GICv3 129 Level NOT_SOFT_NPU_IRQ\n", 0);
        QVERIFY(!stats.hardIrqAvailable);
        QVERIFY(!stats.softIrqAvailable);
        stats = monitor.sample(true, true, true,
            "41: 1 2 GICv3 144 Level HARD_NPU_IRQ\n", 1000);
        QVERIFY(!stats.hardIrqAvailable);
        stats = monitor.sample(true, true, true,
            " CPU0 CPU1\n 41: 18446744073709551615 1 GICv3 144 Level HARD_NPU_IRQ\n", 2000);
        QVERIFY(!stats.hardIrqAvailable);
    }

    void nonAdvancingClock()
    {
        mlk::NpuMonitor monitor;
        monitor.sample(true, true, true, interrupts(1, 0), 1000);
        auto stats = monitor.sample(true, true, true, interrupts(2, 0), 1000);
        QCOMPARE(stats.hardInterruptsPerSec, -1.0);
        stats = monitor.sample(true, true, true, interrupts(3, 0), 500);
        QCOMPARE(stats.hardInterruptsPerSec, -1.0);
    }
};

QTEST_GUILESS_MAIN(NpuMonitorTest)
#include "test_npu_monitor.moc"
