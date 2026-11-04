#pragma once
#include <QJsonObject>
#include <QStringList>
#include <QCache>

namespace mlk {
// Durable, encrypted event records. An ACK is sent only after save succeeds.
class WorkOrders {
public:
    WorkOrders(const QString &directory, const QString &password);
    static bool valid(const QJsonObject &order);
    bool save(const QJsonObject &order, QString *error = nullptr) const;
    bool load(const QString &id, QJsonObject &order, QString *error = nullptr) const;
    QStringList pending() const;
    bool acknowledge(const QString &id) const;
    QJsonObject latest() const;
private:
    struct CachedOrder {
        QByteArray bytes;
        QJsonObject object;
        bool valid = false;
    };
    // Per store/password instance; compare full authenticated file bytes on every load.
    mutable QCache<QString, CachedOrder> m_loadCache{32};
    QString m_directory, m_password;
};
}
