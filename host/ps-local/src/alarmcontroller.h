#pragma once
#include <QObject>
#include <QElapsedTimer>
#include <QTimer>
#include "appconfig.h"
#include "frame.h"

// The board is the sole writer of the physical alarm. Remote settings arrive
// through the revision/authority-checked shared configuration path.
class AlarmController : public QObject {
    Q_OBJECT
public:
    explicit AlarmController(QObject *parent = nullptr);
    ~AlarmController() override;
    void configure(const mlk::AppConfig &config);
    void observe(const mlk::Frame &frame);
    void invalidate(int channel);
    void modelGrade(int grade);
    unsigned desiredState() const;
signals:
    void hardwareStatus(const QString &text);
private:
    void update();
    mlk::AppConfig m_config;
    QVector<qint32> m_last[4];
    qint64 m_seen[4] = {-1,-1,-1,-1};
    QElapsedTimer m_clock;
    QTimer m_timer;
    int m_grade = 0;
    int m_fd = -1;
    QString m_status;
};
