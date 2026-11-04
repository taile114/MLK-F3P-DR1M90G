#include "modelfaultserver.h"
#include <QLocalSocket>
#include <QJsonDocument>
#include <QTimer>
#include <memory>

ModelFaultServer::ModelFaultServer(Handler handler, QObject *parent, int maxRequestBytes)
    : QObject(parent), m_handler(std::move(handler))
    , m_maxRequest(qBound(1000, maxRequestBytes, 8192)) {
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    m_server.setMaxPendingConnections(8);
    connect(&m_server,&QLocalServer::newConnection,this,[this] {
        while (auto *socket=m_server.nextPendingConnection()) {
            if (m_clients>=8) { socket->abort(); socket->deleteLater(); continue; }
            ++m_clients;
            socket->setReadBufferSize(m_maxRequest + 1);
            auto buffer=std::make_shared<QByteArray>();
            auto *timeout=new QTimer(socket); timeout->setSingleShot(true); timeout->start(2000);
            connect(timeout,&QTimer::timeout,socket,[socket]{socket->abort();});
            connect(socket,&QLocalSocket::disconnected,this,[this,socket]{--m_clients; socket->deleteLater();});
            connect(socket,&QLocalSocket::readyRead,this,[this,socket,buffer] {
                *buffer+=socket->readAll();
                if (buffer->size()>m_maxRequest) { socket->abort(); return; }
                const int end=buffer->indexOf('\n');
                if (end<0) return;
                QJsonParseError error;
                const auto doc=QJsonDocument::fromJson(buffer->left(end),&error);
                const auto reply=(error.error==QJsonParseError::NoError && doc.isObject() && end==buffer->size()-1)
                    ? m_handler(doc.object()) : QJsonObject{{"ok",false},{"error","invalid JSON request"}};
                socket->write(QJsonDocument(reply).toJson(QJsonDocument::Compact)+'\n');
                socket->disconnectFromServer();
            });
        }
    });
}
bool ModelFaultServer::start(const QString &path) {
    // Do not unlink an active server owned by another instance.
    QLocalSocket probe; probe.connectToServer(path);
    if (probe.waitForConnected(50)) return false;
    if (probe.error()!=QLocalSocket::ServerNotFoundError
        && probe.error()!=QLocalSocket::ConnectionRefusedError) return false;
    QLocalServer::removeServer(path);
    return m_server.listen(path);
}
