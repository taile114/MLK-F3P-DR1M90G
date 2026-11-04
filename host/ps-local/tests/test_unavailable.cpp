#include <QtTest>
#include <QLabel>
#include <QFileInfo>
#include "klocalwindow.h"

class UnavailableTest : public QObject {
    Q_OBJECT
private slots:
    void missingDriverHasNoReadings() {
        if (QFileInfo::exists(QStringLiteral("/dev/pl_ctrl")))
            QSKIP("Requires a host without the real PL device");
        KLocalWindow window;
        window.show();
        QTest::qWait(1200); // Includes reconnection attempt.
        auto text = [&](const char *name) {
            auto *label = window.findChild<QLabel *>(QLatin1String(name));
            if (!label) return QString();
            return label->text();
        };
        QCOMPARE(text("temperatureValue"), QStringLiteral("不可用"));
        QCOMPARE(text("vibrationValue"), QStringLiteral("不可用"));
        QCOMPARE(text("currentValue"), QStringLiteral("未接入 / 不可用"));
        QCOMPARE(text("speedValue"), QStringLiteral("未接入 / 不可用"));
        QVERIFY(text("sourceText").startsWith(QStringLiteral("来源：")));
    }
};
QTEST_MAIN(UnavailableTest)
#include "test_unavailable.moc"
