#include "robotmanager.h"
#include "protocol.h"

#include <QDateTime>
#include <QSettings>
#include <QTimer>
#include <QUdpSocket>

namespace {
// 状态帧约 10 Hz 上报一次；连续超过该时长未收到任何帧，判定该车离线。
constexpr qint64 kOfflineAfterMs = 800;
constexpr int    kSweepIntervalMs = 200;

inline QString robotTag(quint8 team, quint8 id)
{
    return team == 0
               ? QStringLiteral("机器人%1（未分配队伍）").arg(id)
               : QStringLiteral("队伍%1·机器人%2").arg(team).arg(id);
}
} // namespace

RobotManager::RobotManager(QObject *parent)
    : QObject(parent)
{
    loadTeamMap();
    loadProcessedTxids();

    m_sweep = new QTimer(this);
    m_sweep->setInterval(kSweepIntervalMs);
    connect(m_sweep, &QTimer::timeout, this, &RobotManager::checkOffline);
    m_sweep->start();
}

void RobotManager::handleDatagram(const QByteArray &data,
                                  const QHostAddress &addr, quint16 port)
{
    proto::Frame f;
    if (!proto::parseDatagram(data, f))
        return; // 非法帧直接丢弃

    // V2 ACK 帧不属于机器人状态，直接转发给下行管理器
    if (f.version == proto::kVersion2 && f.type == proto::TypeAck) {
        emit ackReceived(f.ackedFrameType, f.transactionId, f.result, f.robotId);
        return;
    }

    // V2：team 由服务端映射决定；V1 沿用帧内 team
    const quint8 team = (f.version == proto::kVersion2)
                            ? m_robotTeamMap.value(f.robotId, 0)
                            : f.team;
    const quint64 key = (f.version == proto::kVersion2)
                            ? keyOfV2(f.robotId)
                            : keyOf(f.team, f.robotId);

    int idx = m_index.value(key, -1);
    const bool isNew = (idx < 0);
    if (isNew) {
        RobotInfo info;
        info.robotId = f.robotId;
        info.team    = team;
        info.protocolVersion = f.version;
        idx = m_robots.size();
        m_index.insert(key, idx);
        m_robots.append(info);
        emit logMessage(QStringLiteral("[登记] %1 首次上报 (V%2)")
                            .arg(robotTag(team, f.robotId))
                            .arg(f.version));
    }

    RobotInfo &r = m_robots[idx];
    const bool wasOnline = r.online;
    const quint8 prevVersion = r.protocolVersion;
    r.online = true;
    r.lastSeenMs = QDateTime::currentMSecsSinceEpoch();
    r.addr = addr;
    r.port = port;
    r.protocolVersion = f.version;
    if (f.version == proto::kVersion2) {
        r.team = team;   // V2 team 来自服务器映射，可能为 0
        m_endpoints.insert(f.robotId, {addr, port});
        emit endpointLearned(f.robotId, addr, port);
    } else if (team != r.team && r.team != 0) {
        // V1 帧自带 team；以帧为准但记录
        r.team = team;
    }

    // V2 可靠事件去重 + 通知调用方回 ACK
    if (f.version == proto::kVersion2 && proto::isReliableEvent(f.type)) {
        const quint64 txKey = (quint64(f.type) << 32) | f.transactionId;
        if (isTransactionProcessed(f.robotId, f.type, f.transactionId)) {
            // 已处理过：仍回 ACK，但不重复业务
            emit reliableEventNeedsAck(f.robotId, f.type, f.transactionId, addr, port);
            return;
        }
        markTransactionProcessed(f.robotId, f.type, f.transactionId);
        r.lastTxid = f.transactionId;
        emit reliableEventNeedsAck(f.robotId, f.type, f.transactionId, addr, port);
    }

    // 事件帧与状态帧合并到统一状态
    switch (f.type) {
    case proto::TypeStatus:
        r.hp = f.hp;
        if (f.version == proto::kVersion2) {
            r.heat = f.heat;
            r.power = f.power;
            r.powerOn = f.powerOn;
        } else if (f.version == proto::kVersion1 && f.heat != 0) {
            r.heat = f.heat;
        }
        r.alive = f.alive;
        r.shootEnabled = f.shootEnabled;
        break;
    case proto::TypeDeath:
        r.alive = false;
        r.shootEnabled = false;
        emit logMessage(QStringLiteral("[死亡] %1 阵亡")
                            .arg(robotTag(r.team, f.robotId)));
        emit combatEvent(r.team, f.robotId, f.type);
        break;
    case proto::TypeRevive:
        r.alive = true;
        emit logMessage(QStringLiteral("[复活] %1 复活")
                            .arg(robotTag(r.team, f.robotId)));
        break;
    case proto::TypeHit:
        if (f.version == proto::kVersion1)
            r.hp = f.hp;   // V1 帧含 HP；V2 帧只表示事件
        emit logMessage(QStringLiteral("[受击] %1 受到攻击")
                            .arg(robotTag(r.team, f.robotId)));
        emit combatEvent(r.team, f.robotId, f.type);
        break;
    case proto::TypeAttack:
        emit logMessage(QStringLiteral("[攻击] %1 进入攻击")
                            .arg(robotTag(r.team, f.robotId)));
        emit combatEvent(r.team, f.robotId, f.type);
        break;
    case proto::TypeShootEnabled:
        r.shootEnabled = true;
        emit logMessage(QStringLiteral("[射击] %1 允许射击")
                            .arg(robotTag(r.team, f.robotId)));
        break;
    case proto::TypeShootDisabled:
        r.shootEnabled = false;
        emit logMessage(QStringLiteral("[射击] %1 被禁止射击")
                            .arg(robotTag(r.team, f.robotId)));
        break;
    case proto::TypeLinkDown:
        r.linkUp = false;
        emit logMessage(QStringLiteral("[链路] %1 L431 业务链路断开")
                            .arg(robotTag(r.team, f.robotId)));
        break;
    case proto::TypeLinkUp:
        r.linkUp = true;
        emit logMessage(QStringLiteral("[链路] %1 L431 业务链路恢复")
                            .arg(robotTag(r.team, f.robotId)));
        break;
    default:
        break;
    }

    if (prevVersion != f.version && prevVersion != 0)
        emit logMessage(QStringLiteral("[协议] %1 从 V%2 切换到 V%3")
                            .arg(robotTag(r.team, f.robotId))
                            .arg(prevVersion).arg(f.version));

    if (!wasOnline && !isNew)
        emit logMessage(QStringLiteral("[上线] %1 恢复连接")
                            .arg(robotTag(r.team, f.robotId)));

    emit robotsChanged();
}

void RobotManager::checkOffline()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool changed = false;
    for (RobotInfo &r : m_robots) {
        if (r.online && now - r.lastSeenMs > kOfflineAfterMs) {
            r.online = false;
            changed = true;
            emit logMessage(QStringLiteral("[离线] %1 超过 %2 ms 未上报")
                                .arg(robotTag(r.team, r.robotId))
                                .arg(kOfflineAfterMs));
        }
    }
    if (changed)
        emit robotsChanged();
}

QVector<RobotManager::RobotInfo> RobotManager::robots() const
{
    return m_robots;
}

// ---------- V2: team 映射 ----------

void RobotManager::setTeamForRobot(quint8 robotId, quint8 team)
{
    if (m_robotTeamMap.value(robotId, 0) == team)
        return;
    m_robotTeamMap.insert(robotId, team);
    saveTeamMap();
    // 已存在的 RobotInfo 立即更新 team
    const quint64 key = keyOfV2(robotId);
    const int idx = m_index.value(key, -1);
    if (idx >= 0 && m_robots[idx].team != team) {
        m_robots[idx].team = team;
        emit robotsChanged();
    }
}

quint8 RobotManager::teamForRobot(quint8 robotId) const
{
    return m_robotTeamMap.value(robotId, 0);
}

// ---------- V2: 端点学习 ----------

bool RobotManager::endpointForRobot(quint8 robotId, QHostAddress *ip,
                                    quint16 *port) const
{
    const auto it = m_endpoints.constFind(robotId);
    if (it == m_endpoints.constEnd())
        return false;
    if (ip) *ip = it->first;
    if (port) *port = it->second;
    return true;
}

// ---------- V2: 事务去重 ----------

bool RobotManager::isTransactionProcessed(quint8 robotId, quint8 type,
                                          quint32 txid) const
{
    const auto it = m_processedTxids.constFind(robotId);
    if (it == m_processedTxids.constEnd())
        return false;
    const quint64 key = (quint64(type) << 32) | txid;
    return it->contains(key);
}

void RobotManager::markTransactionProcessed(quint8 robotId, quint8 type,
                                            quint32 txid)
{
    const quint64 key = (quint64(type) << 32) | txid;
    m_processedTxids[robotId].insert(key);
    // 保留最近 64 条，防 QSettings 无限膨胀
    if (m_processedTxids[robotId].size() > 64) {
        // 简单清空旧的一半（顺序不重要）
        auto &set = m_processedTxids[robotId];
        auto it = set.begin();
        for (int i = 0; i < 32 && it != set.end(); ++i)
            it = set.erase(it);
    }
    saveProcessedTxids();
}

// ---------- V2: ACK 发送 ----------

void RobotManager::sendAck(QUdpSocket *socket, const QHostAddress &addr,
                           quint16 port, quint8 ackedType, quint32 txid,
                           quint8 result)
{
    if (!socket)
        return;
    const QByteArray frame = proto::makeAck(ackedType, txid, result);
    socket->writeDatagram(frame, addr, port);
}

// ---------- 持久化 ----------

void RobotManager::loadTeamMap()
{
    QSettings s(QStringLiteral("Scompetition"), QStringLiteral("RobotTeams"));
    const QStringList keys = s.childKeys();
    for (const QString &k : keys) {
        bool ok = false;
        const quint8 id = k.toUInt(&ok);
        if (ok)
            m_robotTeamMap.insert(id, static_cast<quint8>(s.value(k).toUInt()));
    }
}

void RobotManager::saveTeamMap()
{
    QSettings s(QStringLiteral("Scompetition"), QStringLiteral("RobotTeams"));
    s.clear();
    for (auto it = m_robotTeamMap.constBegin(); it != m_robotTeamMap.constEnd(); ++it)
        s.setValue(QString::number(it.key()), it.value());
}

void RobotManager::loadProcessedTxids()
{
    QSettings s(QStringLiteral("Scompetition"), QStringLiteral("RobotTxids"));
    const QStringList groups = s.childGroups();
    for (const QString &g : groups) {
        bool ok = false;
        const quint8 id = g.toUInt(&ok);
        if (!ok) continue;
        s.beginGroup(g);
        const QStringList txKeys = s.childKeys();
        for (const QString &tk : txKeys) {
            const quint64 v = tk.toULongLong(&ok);
            if (ok)
                m_processedTxids[id].insert(v);
        }
        s.endGroup();
    }
}

void RobotManager::saveProcessedTxids()
{
    QSettings s(QStringLiteral("Scompetition"), QStringLiteral("RobotTxids"));
    s.clear();
    for (auto it = m_processedTxids.constBegin(); it != m_processedTxids.constEnd(); ++it) {
        s.beginGroup(QString::number(it.key()));
        for (quint64 v : it.value())
            s.setValue(QString::number(v), true);
        s.endGroup();
    }
}
