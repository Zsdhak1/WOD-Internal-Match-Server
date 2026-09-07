#include "matchserver.h"

#include "matchprotocol.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QNetworkDatagram>
#include <QJsonArray>
#include <QProcess>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace {
constexpr quint8 kDefaultRobotId = 1;
constexpr quint8 kRedTeam = 1;
constexpr quint8 kBlueTeam = 2;
constexpr int kVideoWidth = 960;
constexpr int kVideoHeight = 540;
constexpr int kVideoBytesPerFrame = kVideoWidth * kVideoHeight * 4;

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

QString ffmpegExecutable()
{
    const QDir applicationDir(QCoreApplication::applicationDirPath());
    const QStringList candidates = {
        applicationDir.filePath(QStringLiteral("ffmpeg.exe")),
        applicationDir.filePath(QStringLiteral("tools/ffmpeg/ffmpeg.exe")),
        applicationDir.filePath(QStringLiteral("tools/ffmpeg.exe")),
        QStandardPaths::findExecutable(QStringLiteral("ffmpeg"))
    };
    for (const QString &candidate : candidates) {
        if (!candidate.isEmpty() && QFileInfo::exists(candidate))
            return candidate;
    }
    return QString();
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
    if (!startVideoSockets()) {
        m_server->close();
        m_port = 0;
        const QString message = tr("视频 UDP 端口启动失败，选手端服务未启动");
        emit serverStateChanged(false, message);
        emit logMessage(QStringLiteral("[选手端] %1").arg(message));
        return false;
    }
    const QString message = tr("选手端 TCP 登记服务已启动，端口 %1").arg(m_port);
    emit serverStateChanged(true, message);
    emit logMessage(QStringLiteral("[选手端] %1").arg(message));
    return true;
}

void MatchServer::stop()
{
    if (!m_server->isListening() && m_sessions.isEmpty() && m_videoSockets.isEmpty())
        return;

    const auto sockets = m_sessions.keys();
    for (QTcpSocket *socket : sockets) {
        if (socket)
            socket->disconnectFromHost();
    }
    m_sessions.clear();
    m_teamSessions.clear();
    m_server->close();
    stopVideoSockets();
    m_port = 0;
    emitVideoSources();
    emit clientCountChanged(0);
    emit serverStateChanged(false, tr("选手端 TCP 登记服务已停止"));
    emit logMessage(QStringLiteral("[选手端] 登记服务已停止"));
}

quint16 MatchServer::videoPort(quint8 team) const
{
    if (team != kRedTeam && team != kBlueTeam)
        return 0;
    if (m_port > 65533)
        return 0;
    return static_cast<quint16>(m_port + team);
}

bool MatchServer::startVideoSockets()
{
    if (m_port > 65533)
        return false;

    for (const quint8 team : {kRedTeam, kBlueTeam}) {
        auto *socket = new QUdpSocket(this);
        const quint16 port = videoPort(team);
        if (!socket->bind(QHostAddress::AnyIPv4, port,
                          QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
            emit logMessage(tr("视频 UDP 端口 %1 启动失败: %2")
                                .arg(port)
                                .arg(socket->errorString()));
            socket->deleteLater();
            stopVideoSockets();
            return false;
        }
        m_videoSockets.insert(team, socket);
        connect(socket, &QUdpSocket::readyRead, this, [this, team] {
            onVideoReadyRead(team);
        });
    }
    return true;
}

void MatchServer::stopVideoSockets()
{
    stopVideoDecoder(kRedTeam);
    stopVideoDecoder(kBlueTeam);
    for (auto *socket : std::as_const(m_videoSockets)) {
        if (socket)
            socket->close();
        if (socket)
            socket->deleteLater();
    }
    m_videoSockets.clear();
    m_videoBuffers.clear();
}

void MatchServer::onVideoReadyRead(quint8 team)
{
    QUdpSocket *socket = m_videoSockets.value(team);
    if (!socket)
        return;

    QProcess *decoder = m_videoDecoders.value(team);
    while (socket->hasPendingDatagrams()) {
        const QNetworkDatagram datagram = socket->receiveDatagram();
        if (datagram.data().isEmpty())
            continue;
        if (!decoder) {
            startVideoDecoder(team);
            decoder = m_videoDecoders.value(team);
        }
        if (decoder && decoder->state() != QProcess::NotRunning
            && decoder->bytesToWrite() < 2 * 1024 * 1024) {
            decoder->write(datagram.data());
        }
    }
}

void MatchServer::startVideoDecoder(quint8 team)
{
    if (m_videoDecoders.contains(team))
        return;

    const QString executable = ffmpegExecutable();
    if (executable.isEmpty()) {
        emit logMessage(tr("未找到 FFmpeg，无法解码 %1 方视频流").arg(team == kRedTeam ? tr("红") : tr("蓝")));
        return;
    }

    auto *decoder = new QProcess(this);
    decoder->setProcessChannelMode(QProcess::SeparateChannels);
    m_videoDecoders.insert(team, decoder);
    connect(decoder, &QProcess::readyReadStandardOutput, this, [this, team] {
        consumeVideoOutput(team);
    });
    connect(decoder, &QProcess::errorOccurred, this, [this, team, decoder](QProcess::ProcessError) {
        if (m_videoDecoders.value(team) == decoder) {
            emit logMessage(tr("%1 方视频解码器错误: %2")
                                .arg(team == kRedTeam ? tr("红") : tr("蓝"))
                                .arg(decoder->errorString()));
            if (decoder->error() == QProcess::FailedToStart) {
                m_videoDecoders.remove(team);
                m_videoBuffers.remove(team);
                decoder->deleteLater();
            }
        }
    });
    connect(decoder, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, team, decoder](int, QProcess::ExitStatus) {
        if (m_videoDecoders.value(team) != decoder)
            return;
        emit logMessage(tr("%1 方视频解码器已停止").arg(team == kRedTeam ? tr("红") : tr("蓝")));
        m_videoDecoders.remove(team);
        m_videoBuffers.remove(team);
        decoder->deleteLater();
    });

    decoder->start(executable, {
        QStringLiteral("-hide_banner"),
        QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-fflags"), QStringLiteral("nobuffer"),
        QStringLiteral("-flags"), QStringLiteral("low_delay"),
        QStringLiteral("-f"), QStringLiteral("mpegts"),
        QStringLiteral("-i"), QStringLiteral("pipe:0"),
        QStringLiteral("-an"),
        QStringLiteral("-f"), QStringLiteral("rawvideo"),
        QStringLiteral("-pix_fmt"), QStringLiteral("bgra"),
        QStringLiteral("-s"), QStringLiteral("%1x%2").arg(kVideoWidth).arg(kVideoHeight),
        QStringLiteral("-r"), QStringLiteral("30"),
        QStringLiteral("pipe:1")
    });
}

void MatchServer::stopVideoDecoder(quint8 team)
{
    QProcess *decoder = m_videoDecoders.take(team);
    if (!decoder)
        return;
    decoder->closeWriteChannel();
    if (decoder->state() != QProcess::NotRunning) {
        decoder->kill();
        decoder->waitForFinished(300);
    }
    decoder->deleteLater();
}

void MatchServer::consumeVideoOutput(quint8 team)
{
    QProcess *decoder = m_videoDecoders.value(team);
    if (!decoder)
        return;

    QByteArray &buffer = m_videoBuffers[team];
    buffer.append(decoder->readAllStandardOutput());
    while (buffer.size() >= kVideoBytesPerFrame) {
        const QByteArray frameBytes = buffer.left(kVideoBytesPerFrame);
        buffer.remove(0, kVideoBytesPerFrame);
        const QImage frame(reinterpret_cast<const uchar *>(frameBytes.constData()),
                           kVideoWidth, kVideoHeight, QImage::Format_ARGB32);
        QString sourceId;
        QTcpSocket *sessionSocket = m_teamSessions.value(team);
        const auto session = m_sessions.constFind(sessionSocket);
        if (session != m_sessions.constEnd())
            sourceId = session->sourceId;
        if (!sourceId.isEmpty())
            emit videoFrameReceived(sourceId, frame.copy());
    }
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
            {QStringLiteral("sourceName"), sessionIt->sourceName},
            {QStringLiteral("videoPort"), videoPort(sessionIt->team)}
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
            {QStringLiteral("videoPort"), videoPort(sessionIt->team)},
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
