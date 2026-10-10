#include "robotmanager.h"
#include "protocol.h"

#include <QDateTime>
#include <QSettings>
#include <QStringList>
#include <QTimer>
#include <QUdpSocket>

#include <algorithm>

namespace {
// 状态帧约 10 Hz 上报一次；连续超过该时长未收到任何帧，判定该车离线。
constexpr qint64 kOfflineAfterMs = 800;
constexpr int    kSweepIntervalMs = 200;
constexpr qint64 kInvalidFrameLogIntervalMs = 1000;

inline QString robotTag(quint8 team, quint8 id)
{
    return team == 0
               ? QStringLiteral("机器人%1（未分配队伍）").arg(id)
               : QStringLiteral("队伍%1·机器人%2").arg(team).arg(id);
}

QString macText(const QByteArray &mac)
{
    QStringList parts;
    parts.reserve(6);
    for (const char byte : mac)
        parts.append(QStringLiteral("%1").arg(static_cast<quint8>(byte), 2, 16,
                                              QLatin1Char('0')).toUpper());
    return parts.join(QLatin1Char(':'));
}

bool hasNonZeroByte(const QByteArray &bytes)
{
    for (const char byte : bytes) {
        if (byte != 0)
            return true;
    }
    return false;
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
    if (!proto::parseDatagram(data, f)) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_lastInvalidFrameLogMs >= kInvalidFrameLogIntervalMs) {
            m_lastInvalidFrameLogMs = now;
            const QByteArray prefix = data.left(16).toHex(' ');
            qWarning().noquote()
                << QStringLiteral("[UDP] 丢弃非法帧 %1:%2 size=%3 prefix=%4")
                       .arg(addr.toString()).arg(port).arg(data.size())
                       .arg(QString::fromLatin1(prefix));
        }
        return;
    }

    // V2 ACK 帧不属于机器人状态，直接转发给下行管理器
    if (f.version == proto::kVersion2 && f.type == proto::TypeAck) {
        emit ackReceived(f.ackedFrameType, f.transactionId, f.result, f.robotId);
        return;
    }

    if (addr.protocol() != QAbstractSocket::IPv4Protocol) {
        qWarning().noquote() << QStringLiteral("[UDP] 非 IPv4 来源已忽略: %1:%2")
                                    .arg(addr.toString()).arg(port);
        return;
    }

    const quint32 senderIpv4 = addr.toIPv4Address();
    const QString senderText = addr.toString();

    // DEVICE_ANNOUNCE is the stable identity/registry frame. It may carry
    // robot_id=0 before assignment, so it must not create a fake robot 0.
    if (f.version == proto::kVersion2 && f.type == proto::TypeDeviceAnnounce) {
        const QString deviceMac = macText(f.deviceMac);
        if (deviceMac.isEmpty()) {
            qWarning().noquote() << QStringLiteral("[设备] DEVICE_ANNOUNCE 缺少 ESP32 MAC");
            return;
        }

        const bool isNew = !m_devices.contains(deviceMac);
        DeviceInfo &device = m_devices[deviceMac];
        const quint8 previousId = device.robotId;
        if (previousId != 0 && previousId != f.robotId) {
            const int oldIndex = m_index.value(keyOfV2(previousId), -1);
            m_index.remove(keyOfV2(previousId));
            if (oldIndex >= 0 && oldIndex < m_robots.size()
                && m_robots[oldIndex].protocolVersion == proto::kVersion2
                && m_robots[oldIndex].robotId == previousId) {
                m_robots.removeAt(oldIndex);
                for (auto it = m_index.begin(); it != m_index.end(); ++it)
                    if (it.value() > oldIndex) --it.value();
            }
            m_robotTeamMap.remove(previousId);
            saveTeamMap();
            m_endpoints.remove(previousId);
            emit endpointForgotten(previousId);
            emit robotsChanged();
        }
        device.deviceMac = deviceMac;
        device.controllerMac = hasNonZeroByte(f.controllerMac)
                                   ? macText(f.controllerMac) : QString();
        device.addressText = senderText;
        device.ipv4 = senderIpv4;
        device.sourcePort = port;
        device.robotId = f.robotId;
        device.eventQueueDrops = f.eventQueueDrops;
        device.udpSendFailures = f.udpSendFailures;
        device.lastSeenMs = QDateTime::currentMSecsSinceEpoch();
        if (device.firstSeenMs == 0) device.firstSeenMs = device.lastSeenMs;

        if (f.robotId != 0) {
            m_endpoints.insert(f.robotId, {senderIpv4, proto::kDownlinkPort});
            emit endpointLearned(f.robotId, senderIpv4, proto::kDownlinkPort);
        }

        if (isNew) {
            qInfo().noquote()
                << QStringLiteral("[设备] 发现 ESP32 %1 @%2 robot=%3 controller=%4")
                       .arg(deviceMac, senderText)
                       .arg(f.robotId == 0 ? QStringLiteral("未分配")
                                           : QString::number(f.robotId),
                            device.controllerMac.isEmpty()
                                ? QStringLiteral("未分配") : device.controllerMac);
        }

        emit devicesChanged();
        return;
    }

    if (f.version == proto::kVersion2 && f.type == proto::TypeDeviceHealth
        && f.robotId == 0) {
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
    if (r.firstSeenMs == 0) r.firstSeenMs = r.lastSeenMs;
    r.addressText = senderText;
    r.sourcePort = port;
    r.protocolVersion = f.version;
    if (f.version == proto::kVersion2) {
        r.team = team;   // V2 team 来自服务器映射，可能为 0
        r.downlinkPort = proto::kDownlinkPort;
        m_endpoints.insert(f.robotId, {senderIpv4, proto::kDownlinkPort});
        emit endpointLearned(f.robotId, senderIpv4, proto::kDownlinkPort);
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
        } else if (f.version == proto::kVersion1 && data.size() == 12) {
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
    case proto::TypeCombatEnd:
        // A pause in firing does not revoke shooting permission. Only the
        // dedicated permission event or the next STATUS defines that state.
        emit logMessage(QStringLiteral("[战斗] %1 脱离战斗")
                            .arg(robotTag(r.team, f.robotId)));
        break;
    case proto::TypeDeviceHealth:
        {
        const bool wasLinkUp = r.linkUp;
        r.componentOnline = f.componentOnline;
        r.healthReceived = true;
        r.lastHealthSeenMs = r.lastSeenMs;
        r.linkUp = (f.componentOnline & 0x01U) != 0;
        if (!r.linkUp && wasLinkUp)
            emit logMessage(QStringLiteral("[健康] %1 L431 离线，组件位图=0x%2")
                                .arg(robotTag(r.team, f.robotId))
                                .arg(f.componentOnline, 2, 16, QLatin1Char('0')));
        break;
        }
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

QVector<RobotManager::DeviceInfo> RobotManager::devices() const
{
    QVector<DeviceInfo> result;
    result.reserve(m_devices.size());
    for (auto it = m_devices.constBegin(); it != m_devices.constEnd(); ++it)
        result.append(it.value());
    std::sort(result.begin(), result.end(), [](const DeviceInfo &left,
                                               const DeviceInfo &right) {
        if (left.robotId != right.robotId)
            return left.robotId < right.robotId;
        return left.deviceMac < right.deviceMac;
    });
    return result;
}

bool RobotManager::deviceForMac(const QString &deviceMac, DeviceInfo *device) const
{
    const auto it = m_devices.constFind(deviceMac.trimmed().toUpper());
    if (it == m_devices.constEnd())
        return false;
    if (device)
        *device = it.value();
    return true;
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

bool RobotManager::endpointForRobot(quint8 robotId, quint32 *ipv4,
                                    quint16 *port) const
{
    const auto it = m_endpoints.constFind(robotId);
    if (it == m_endpoints.constEnd())
        return false;
    if (ipv4) *ipv4 = it->first;
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
    const qint64 written = socket->writeDatagram(frame, addr, port);
    if (written != frame.size()) {
        qWarning().noquote()
            << QStringLiteral("[ACK] %1 回执发送失败 txid=%2 result=%3: %4")
                   .arg(ackedType).arg(txid).arg(result)
                   .arg(socket->errorString());
    }
}

// ---------- 持久化 ----------

void RobotManager::loadTeamMap()
{
    QSettings s(QSettings::defaultFormat(), QSettings::UserScope,
                QStringLiteral("Scompetition"), QStringLiteral("RobotTeams"));
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
    QSettings s(QSettings::defaultFormat(), QSettings::UserScope,
                QStringLiteral("Scompetition"), QStringLiteral("RobotTeams"));
    s.clear();
    for (auto it = m_robotTeamMap.constBegin(); it != m_robotTeamMap.constEnd(); ++it)
        s.setValue(QString::number(it.key()), it.value());
}

void RobotManager::loadProcessedTxids()
{
    QSettings s(QSettings::defaultFormat(), QSettings::UserScope,
                QStringLiteral("Scompetition"), QStringLiteral("RobotTxids"));
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
    QSettings s(QSettings::defaultFormat(), QSettings::UserScope,
                QStringLiteral("Scompetition"), QStringLiteral("RobotTxids"));
    s.clear();
    for (auto it = m_processedTxids.constBegin(); it != m_processedTxids.constEnd(); ++it) {
        s.beginGroup(QString::number(it.key()));
        for (quint64 v : it.value())
            s.setValue(QString::number(v), true);
        s.endGroup();
    }
}
