#ifndef MATCHSERVER_H
#define MATCHSERVER_H

#include "robotmanager.h"

#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVector>

class QTcpServer;
class QTcpSocket;
class QProcess;
class QUdpSocket;

// 面向选手端的 TCP 登记与控制服务。机器人状态仍走 UDP，由 MainWindow 注入并广播给已登记客户端。
class MatchServer : public QObject
{
    Q_OBJECT

public:
    struct VideoSourceInfo {
        QString sourceId;
        QString sourceName;
        QString displayName;
        quint8 team = 0;
        quint8 robotId = 0;
        bool online = false;
    };

    explicit MatchServer(QObject *parent = nullptr);

    bool start(quint16 port);
    void stop();
    bool isListening() const;
    quint16 port() const { return m_port; }
    quint16 videoPort(quint8 team) const;

    void publishRobots(const QVector<RobotManager::RobotInfo> &robots);
    void publishMatchState(const QJsonObject &state);

signals:
    void serverStateChanged(bool listening, const QString &message);
    void logMessage(const QString &message);
    void clientCountChanged(int count);
    void videoSourcesChanged(const QVector<MatchServer::VideoSourceInfo> &sources);
    void videoFrameReceived(const QString &sourceId, const QImage &frame);

private:
    struct Session {
        QByteArray buffer;
        QString token;
        QString displayName;
        QString sourceId;
        QString sourceName;
        quint8 team = 0;
        quint8 robotId = 0;
        bool registered = false;
    };

    void onNewConnection();
    void onSocketReadyRead(QTcpSocket *socket);
    void onSocketDisconnected(QTcpSocket *socket);
    void processMessage(QTcpSocket *socket, const QJsonObject &message);
    void sendMessage(QTcpSocket *socket, const QJsonObject &message);
    void sendError(QTcpSocket *socket, const QString &message);
    void sendSnapshot(QTcpSocket *socket);
    void broadcast(const QJsonObject &message);
    QJsonObject makeRobotSnapshot(const QVector<RobotManager::RobotInfo> &robots) const;
    void emitVideoSources();
    void emitClientCount();
    bool startVideoSockets();
    void stopVideoSockets();
    void onVideoReadyRead(quint8 team);
    void startVideoDecoder(quint8 team);
    void stopVideoDecoder(quint8 team);
    void consumeVideoOutput(quint8 team);

    QTcpServer *m_server = nullptr;
    QHash<QTcpSocket *, Session> m_sessions;
    QHash<quint8, QTcpSocket *> m_teamSessions;
    QJsonObject m_lastSnapshot;
    QJsonObject m_lastMatchState;
    quint16 m_port = 0;
    QHash<quint8, QUdpSocket *> m_videoSockets;
    QHash<quint8, QProcess *> m_videoDecoders;
    QHash<quint8, QByteArray> m_videoBuffers;
};

#endif // MATCHSERVER_H
