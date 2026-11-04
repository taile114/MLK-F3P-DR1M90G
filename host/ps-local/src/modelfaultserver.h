#pragma once
#include <QObject>
#include <QLocalServer>
#include <QJsonObject>
#include <functional>

class ModelFaultServer : public QObject {
public:
    using Handler = std::function<QJsonObject(const QJsonObject &)>;
    /* maxRequestBytes 为单行请求上限：模型 API 2 需要 4096 B，采集接口与旧
     * API v1 保持 1000 B，不能在一个实例上无条件放宽。 */
    explicit ModelFaultServer(Handler handler, QObject *parent = nullptr,
                              int maxRequestBytes = 1000);
    bool start(const QString &path);
    QString errorString() const { return m_server.errorString(); }
private:
    QLocalServer m_server;
    Handler m_handler;
    int m_clients=0;
    int m_maxRequest=1000;
};
