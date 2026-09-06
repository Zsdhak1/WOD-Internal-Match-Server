#include "matchserver.h"

#include "matchprotocol.h"

#include <QDateTime>
#include <QJsonArray>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUuid>

#include <algorithm>

namespace {
constexpr quint8 kDefaultRobotId = 1;

QString teamName(quint8 team)
{
    if (team == 1) return QStringLiteral("红方");
    if (team == 2) return QStringLiteral("蓝方");
    return QStringLiteral("队伍%1").arg(team);
}

QString socketTag(QTcpSocket *socket)
{
    if (!socket)
        return QStringLiteral("<unknown>");
    return QStringLiteral("%1:%2").arg(socket->peerAddress().toString()).arg(socket->peerPort());
}
} // namespace

MatchServer::MatchServer(QObject *parent)
    : QObject(parent)
{
    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &MatchServer::onNewConnection);

    m_lastSnapshot = makeRobotSnapshot({});
    m_lastMatchState = {
        {QStringLiteral("type"), QStringLiteral("match_state")},
        {QStringLiteral("remainingSeconds"), 60},
        {QStringLiteral("running"), false},
        {QStringLiteral("redScore"), 0},
        {QStringLiteral("blueScore"), 0},
        {QStringLiteral("redName"), QStringLiteral("红方")},
        {QStringLiteral("blueName"), QStringLiteral("蓝方")}
    };
}

bool MatchServer::start(quint16 port)
{
    if (m_server->isListening())
        return true;

    if (!m_server->listen(QHostAddress::AnyIPv4, port)) {
        const QString message = tr("选手端 TCP 端口 %1 启动失败: %2")
                                    .arg(port)
                                    .arg(m_server->errorString());
        emit serverStateChanged(false, message);
        emit logMessage(QStringLiteral("[选手端] %1").arg(message));
        return false;
    }

    m_port = m_server->serverPort();
    const QString message = tr("选手端 TCP 登记服务已启动，端口 %1").arg(m_port);
    emit serverStateChanged(true, message);
    emit logMessage(QStringLiteral("[选手端] %1").arg(message));
    return true;
}

void MatchServer::stop()
{
    if (!m_server->isListening() && m_sessions.isEmpty())
        return;

    const auto sockets = m_sessions.keys();
    for (QTcpSocket *socket : sockets) {
        if (socket)
            socket->disconnectFromHost();
    }
    m_sessions.clear();
    m_teamSessions.clear();
    m_server->close();
    m_port = 0;
    emitVideoSources();
    emit clientCountChanged(0);
    emit serverStateChanged(false, tr("选手端 TCP 登记服务已停止"));
    emit logMessage(QStringLiteral("[选手端] 登记服务已停止"));
}

bool MatchServer::isListening() const
{
    return m_server->isListening();
}

void MatchServer::onNewConnection()
{
    while (m_server->hasPendingConnections()) {
        auto *socket = m_server->nextPendingConnection();
        if (!socket)
            continue;

        m_sessions.insert(socket, Session{});
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
            onSocketReadyRead(socket);
        });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
            onSocketDisconnected(socket);
        });
        emit logMessage(QStringLiteral("[选手端] TCP 连接 %1").arg(socketTag(socket)));
    }
}

void MatchServer::onSocketReadyRead(QTcpSocket *socket)
{
    auto it = m_sessions.find(socket);
    if (it == m_sessions.end())
        return;

    it->buffer.append(socket->readAll());
    if (it->buffer.size() > matchproto::kMaxLineBytes * 2) {
        sendError(socket, tr("请求缓冲区过大"));
        socket->disconnectFromHost();
        return;
    }

    while (true) {
        const int newline = it->buffer.indexOf('\n');
        if (newline < 0)
            break;

        const QByteArray line = it->buffer.left(newline);
        it->buffer.remove(0, newline + 1);
        if (line.size() > matchproto::kMaxLineBytes) {
            sendError(socket, tr("单条请求过大"));
            socket->disconnectFromHost();
            return;
        }

        QJsonObject message;
        QString error;
        if (!matchproto::decode(line, message, &error)) {
            sendError(socket, tr("JSON 请求无效: %1").arg(error));
            continue;
        }
        processMessage(socket, message);
    }
}

void MatchServer::onSocketDisconnected(QTcpSocket *socket)
{
    const auto it = m_sessions.find(socket);
    if (it != m_sessions.end()) {
        if (it->registered && m_teamSessions.value(it->team) == socket) {
            m_teamSessions.remove(it->team);
            emit logMessage(QStringLiteral("[登记] %1 已断开 (%2)")
                                .arg(teamName(it->team), socketTag(socket)));
        }
        m_sessions.erase(it);
    }

    emitVideoSources();
    emitClientCount();
    socket->deleteLater();
}

void MatchServer::processMessage(QTcpSocket *socket, const QJsonObject &message)
{
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("register")) {
        auto sessionIt = m_sessions.find(socket);
        if (sessionIt == m_sessions.end())
            return;

        if (sessionIt->registered) {
            sendError(socket, tr("当前连接已经登记"));
            return;
        }

        const int requestedTeam = message.value(QStringLiteral("team")).toInt();
        if (requestedTeam != 1 && requestedTeam != 2) {
            sendMessage(socket, {
                {QStringLiteral("type"), QStringLiteral("registration_result")},
                {QStringLiteral("ok"), false},
                {QStringLiteral("message"), tr("只能登记红方或蓝方")}
            });
            return;
        }

        const quint8 team = static_cast<quint8>(requestedTeam);
        if (m_teamSessions.contains(team)) {
            sendMessage(socket, {
                {QStringLiteral("type"), QStringLiteral("registration_result")},
                {QStringLiteral("ok"), false},
                {QStringLiteral("message"), tr("该队伍已经登记了另一台选手端")}
            });
            return;
        }

        sessionIt->registered = true;
        sessionIt->team = team;
        sessionIt->displayName = message.value(QStringLiteral("displayName")).toString().trimmed();
        if (sessionIt->displayName.isEmpty())
            sessionIt->displayName = teamName(team);
        const int requestedRobotId = message.value(QStringLiteral("robotId")).toInt();
        sessionIt->robotId = requestedRobotId >= 1 && requestedRobotId <= 255
                                 ? static_cast<quint8>(requestedRobotId)
                                 : kDefaultRobotId;
        sessionIt->sourceId = message.value(QStringLiteral("sourceId")).toString().trimmed();
        sessionIt->sourceName = message.value(QStringLiteral("sourceName")).toString().trimmed();
        if (sessionIt->sourceId.isEmpty())
            sessionIt->sourceName.clear();
        else if (sessionIt->sourceName.isEmpty())
            sessionIt->sourceName = sessionIt->sourceId;
        sessionIt->token = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_teamSessions.insert(team, socket);

        sendMessage(socket, {
            {QStringLiteral("type"), QStringLiteral("registration_result")},
            {QStringLiteral("ok"), true},
            {QStringLiteral("message"), tr("登记成功")},
            {QStringLiteral("token"), sessionIt->token},
            {QStringLiteral("displayName"), sessionIt->displayName},
            {QStringLiteral("team"), sessionIt->team},
            {QStringLiteral("teamName"), teamName(sessionIt->team)},
            {QStringLiteral("robotId"), sessionIt->robotId},
            {QStringLiteral("sourceId"), sessionIt->sourceId},
            {QStringLiteral("sourceName"), sessionIt->sourceName}
        });
        sendSnapshot(socket);
        sendMessage(socket, m_lastMatchState);
        emit logMessage(QStringLiteral("[登记成功] %1 · %2 (%3)")
                            .arg(teamName(sessionIt->team), sessionIt->displayName, socketTag(socket)));
        emitVideoSources();
        emitClientCount();
        return;
    }

    auto sessionIt = m_sessions.find(socket);
    if (sessionIt == m_sessions.end() || !sessionIt->registered) {
        sendError(socket, tr("请先登记红方或蓝方"));
        return;
    }

    if (type == QStringLiteral("ping")) {
        sendMessage(socket, {
            {QStringLiteral("type"), QStringLiteral("pong")},
            {QStringLiteral("timestamp"), QDateTime::currentMSecsSinceEpoch()}
        });
    } else if (type == QStringLiteral("get_snapshot")) {
        sendSnapshot(socket);
    } else if (type == QStringLiteral("camera_select")) {
        const QString sourceId = message.value(QStringLiteral("sourceId")).toString().trimmed();
        const QString sourceName = message.value(QStringLiteral("sourceName")).toString().trimmed();
        if (sourceId.isEmpty()) {
            sessionIt->sourceId.clear();
            sessionIt->sourceName.clear();
        } else {
            sessionIt->sourceId = sourceId;
            sessionIt->sourceName = sourceName.isEmpty() ? sourceId : sourceName;
        }

        sendMessage(socket, {
            {QStringLiteral("type"), QStringLiteral("camera_ack")},
            {QStringLiteral("ok"), true},
            {QStringLiteral("sourceId"), sessionIt->sourceId},
            {QStringLiteral("sourceName"), sessionIt->sourceName},
            {QStringLiteral("message"), sessionIt->sourceId.isEmpty()
                                              ? tr("已取消视频源登记")
                                              : tr("视频源登记成功，等待视频传输模块接入")}
        });
        emit logMessage(QStringLiteral("[视频源] %1 选择 %2 (%3)")
                            .arg(teamName(sessionIt->team), sessionIt->sourceName, sessionIt->sourceId));
        emitVideoSources();
    } else if (type == QStringLiteral("logout")) {
        socket->disconnectFromHost();
    } else {
        sendError(socket, tr("未知请求类型: %1").arg(type));
    }
}

void MatchServer::sendMessage(QTcpSocket *socket, const QJsonObject &message)
{
    if (socket && socket->state() == QAbstractSocket::ConnectedState)
        socket->write(matchproto::encode(message));
}

void MatchServer::sendError(QTcpSocket *socket, const QString &message)
{
    sendMessage(socket, {
        {QStringLiteral("type"), QStringLiteral("error")},
        {QStringLiteral("message"), message}
    });
}

void MatchServer::sendSnapshot(QTcpSocket *socket)
{
    sendMessage(socket, m_lastSnapshot);
}

void MatchServer::broadcast(const QJsonObject &message)
{
    for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it) {
        if (it->registered)
            sendMessage(it.key(), message);
    }
}

QJsonObject MatchServer::makeRobotSnapshot(const QVector<RobotManager::RobotInfo> &robots) const
{
    QJsonArray array;
    for (const auto &robot : robots) {
        const QJsonObject robotObject{
            {QStringLiteral("robotId"), robot.robotId},
            {QStringLiteral("team"), robot.team},
            {QStringLiteral("teamName"), teamName(robot.team)},
            {QStringLiteral("hp"), robot.hp},
            {QStringLiteral("heat"), robot.heat},
            {QStringLiteral("alive"), robot.alive},
            {QStringLiteral("shootEnabled"), robot.shootEnabled},
            {QStringLiteral("online"), robot.online},
            {QStringLiteral("address"), robot.addr.toString()}
        };
        array.append(robotObject);
    }

    return {
        {QStringLiteral("type"), QStringLiteral("robot_snapshot")},
        {QStringLiteral("timestamp"), QDateTime::currentMSecsSinceEpoch()},
        {QStringLiteral("robots"), array}
    };
}

void MatchServer::publishRobots(const QVector<RobotManager::RobotInfo> &robots)
{
    m_lastSnapshot = makeRobotSnapshot(robots);
    broadcast(m_lastSnapshot);
}

void MatchServer::publishMatchState(const QJsonObject &state)
{
    if (state.isEmpty())
        return;

    m_lastMatchState = state;
    if (!m_lastMatchState.contains(QStringLiteral("type")))
        m_lastMatchState.insert(QStringLiteral("type"), QStringLiteral("match_state"));
    broadcast(m_lastMatchState);
}

void MatchServer::emitVideoSources()
{
    QVector<VideoSourceInfo> sources;
    sources.reserve(m_teamSessions.size());
    for (auto it = m_teamSessions.cbegin(); it != m_teamSessions.cend(); ++it) {
        const auto sessionIt = m_sessions.constFind(it.value());
        if (sessionIt == m_sessions.constEnd() || !sessionIt->registered)
            continue;

        VideoSourceInfo source;
        source.sourceId = sessionIt->sourceId;
        source.sourceName = sessionIt->sourceName;
        source.displayName = sessionIt->displayName;
        source.team = sessionIt->team;
        source.robotId = sessionIt->robotId;
        source.online = true;
        sources.append(source);
    }
    std::sort(sources.begin(), sources.end(), [](const VideoSourceInfo &left,
                                                 const VideoSourceInfo &right) {
        return left.team < right.team;
    });
    emit videoSourcesChanged(sources);
}

void MatchServer::emitClientCount()
{
    int count = 0;
    for (const auto &session : m_sessions) {
        if (session.registered)
            ++count;
    }
    emit clientCountChanged(count);
}
