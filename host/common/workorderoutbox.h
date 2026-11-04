#pragma once

#include <QObject>
#include <QThread>
#include <memory>

class QJsonObject;

namespace mlk {

// Disk scans and password derivation must not run in the sensor/UI event loop.
class WorkOrderOutbox : public QObject {
    Q_OBJECT
public:
    explicit WorkOrderOutbox(QObject *parent = nullptr);
    ~WorkOrderOutbox() override;
    void configure(const QString &directory, const QString &password);
    void retry();
signals:
    void orderReady(const QJsonObject &order);
    void retryFinished();
private:
    struct State;
    std::unique_ptr<State> m_state;
    QThread m_thread;
    QObject *m_worker = nullptr;
    QString m_directory, m_password;
    quint64 m_generation = 0;
    bool m_busy = false;
};

}
