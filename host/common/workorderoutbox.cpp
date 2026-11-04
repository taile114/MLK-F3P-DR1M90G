#include "workorderoutbox.h"
#include "workorders.h"
#include <QMetaObject>
#include <QVector>

namespace mlk {

struct WorkOrderOutbox::State {
    QString directory, password;
    std::unique_ptr<WorkOrders> store;
};

WorkOrderOutbox::WorkOrderOutbox(QObject *parent)
    : QObject(parent), m_state(new State), m_worker(new QObject)
{
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread.start();
}

WorkOrderOutbox::~WorkOrderOutbox()
{
    m_thread.quit();
    m_thread.wait();
}

void WorkOrderOutbox::configure(const QString &directory, const QString &password)
{
    if (directory == m_directory && password == m_password) return;
    m_directory = directory;
    m_password = password;
    ++m_generation; // Discard a late batch loaded for previous credentials/directory.
}

void WorkOrderOutbox::retry()
{
    if (m_busy || m_directory.isEmpty()) return;
    m_busy = true;
    const auto directory = m_directory, password = m_password;
    const auto generation = m_generation;
    QMetaObject::invokeMethod(m_worker, [this, directory, password, generation] {
        auto &state = *m_state;
        if (!state.store || state.directory != directory || state.password != password) {
            state.store.reset(new WorkOrders(directory, password));
            state.directory = directory;
            state.password = password;
        }
        QVector<QJsonObject> batch;
        for (const auto &id : state.store->pending().mid(0, 4)) {
            QJsonObject order;
            if (state.store->load(id, order)) batch.append(order);
        }
        QMetaObject::invokeMethod(this, [this, generation, batch] {
            m_busy = false;
            if (generation == m_generation)
                for (const auto &order : batch) emit orderReady(order);
            emit retryFinished();
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

}
