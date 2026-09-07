#include "robotmanager.h"
#include "protocol.h"

#include <QDateTime>
#include <QTimer>

namespace {
// 状态帧约 10 Hz 上报一次；连续超过该时长未收到任何帧，判定该车离线。
constexpr qint64 kOfflineAfterMs = 800;
constexpr int    kSweepIntervalMs = 200;

inline QString robotTag(quint8 team, quint8 id)
{
    return QStringLiteral("队伍%1·机器人%2").arg(team).arg(id);
}
} // namespace

RobotManager::RobotManager(QObject *parent)
    : QObject(parent)
{
    m_sweep = new QTimer(this);
    m_sweep->setInterval(kSweepIntervalMs);
    connect(m_sweep, &QTimer::timeout, this, &RobotManager::checkOffline);
    m_sweep->start();
}

void RobotManager::handleDatagram(const QByteArray &data, const QHostAddress &addr, quint16 port)
{
    proto::Frame f;
    if (!proto::parseDatagram(data, f))
        return; // 非法帧直接丢弃，避免日志被噪声刷屏

    const quint64 key = keyOf(f.team, f.robotId);
    int idx = m_index.value(key, -1);
    const bool isNew = (idx < 0);
    if (isNew) {
        RobotInfo info;
        info.robotId = f.robotId;
        info.team    = f.team;
        idx = m_robots.size();
        m_index.insert(key, idx);
        m_robots.append(info);
        emit logMessage(QStringLiteral("[登记] %1 首次上报").arg(robotTag(f.team, f.robotId)));
    }

    RobotInfo &r = m_robots[idx];
    const bool wasOnline = r.online;
    r.online = true;
    r.lastSeenMs = QDateTime::currentMSecsSinceEpoch();
    r.addr = addr;
    r.port = port;

    // 事件帧与状态帧合并到统一状态。
    switch (f.type) {
    case proto::TypeStatus:
        r.hp = f.hp;
        if (f.hasHeat)
            r.heat = f.heat;
        r.alive = f.alive;
        r.shootEnabled = f.shootEnabled;
        break;
    case proto::TypeDeath:
        r.alive = false;
        r.shootEnabled = false;
        emit logMessage(QStringLiteral("[死亡] %1 阵亡").arg(robotTag(f.team, f.robotId)));
        emit combatEvent(f.team, f.robotId, f.type);
        break;
    case proto::TypeRevive:
        r.alive = true;
        emit logMessage(QStringLiteral("[复活] %1 复活").arg(robotTag(f.team, f.robotId)));
        break;
    case proto::TypeHit:
        r.hp = f.hp;
        emit logMessage(QStringLiteral("[受击] %1 受到攻击，血量 -> %2")
                            .arg(robotTag(f.team, f.robotId))
                            .arg(f.hp));
        emit combatEvent(f.team, f.robotId, f.type);
        break;
    case proto::TypeAttack:
        emit logMessage(QStringLiteral("[攻击] %1 进入攻击").arg(robotTag(f.team, f.robotId)));
        emit combatEvent(f.team, f.robotId, f.type);
        break;
    case proto::TypeShootEnabled:
        r.shootEnabled = true;
        emit logMessage(QStringLiteral("[射击] %1 允许射击").arg(robotTag(f.team, f.robotId)));
        break;
    case proto::TypeShootDisabled:
        r.shootEnabled = false;
        emit logMessage(QStringLiteral("[射击] %1 被禁止射击").arg(robotTag(f.team, f.robotId)));
        break;
    default:
        break;
    }

    if (!wasOnline && !isNew)
        emit logMessage(QStringLiteral("[上线] %1 恢复连接").arg(robotTag(f.team, f.robotId)));

    // 只要状态确实有变化就通知 UI；lastSeen 的每秒推进不触发刷新。
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
