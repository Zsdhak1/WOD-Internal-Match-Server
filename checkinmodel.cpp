#include "checkinmodel.h"

#include <QSet>
#include <algorithm>

namespace checkin {
namespace {
QString robotKey(const RobotSnapshot &robot)
{
    // V1 identity includes team; V2 team remapping must not reset first connection.
    return robot.protocolVersion == 1
        ? QStringLiteral("robot:v1:%1:%2").arg(robot.team).arg(robot.robotId)
        : QStringLiteral("robot:v%1:%2").arg(robot.protocolVersion).arg(robot.robotId);
}
QString deviceKey(const DeviceSnapshot &device)
{
    const QString mac = device.deviceMac.trimmed().toUpper();
    return mac.isEmpty()
        ? QStringLiteral("device:address:%1:%2").arg(device.addressText).arg(device.sourcePort)
        : QStringLiteral("device:mac:%1").arg(mac);
}
bool recent(qint64 timestamp, qint64 now, qint64 window)
{
    return timestamp > 0 && now >= timestamp && now - timestamp <= window;
}
bool connected(Connection connection)
{
    return connection == Connection::Online || connection == Connection::RegistryRecent;
}
qint64 earliestKnown(qint64 left, qint64 right)
{
    if (left <= 0) return std::max(qint64(0), right);
    if (right <= 0) return left;
    return std::min(left, right);
}
}

CheckinModel::CheckinModel() : CheckinModel(Timing{}) {}
CheckinModel::CheckinModel(const Timing &timing) : m_timing(timing)
{
    m_timing.healthFreshnessMs = std::max(qint64(1), timing.healthFreshnessMs);
    m_timing.announceFreshnessMs = std::max(qint64(1), timing.announceFreshnessMs);
}

void CheckinModel::setEnabled(bool enabled, qint64 observedAtMs)
{
    if (m_enabled == enabled)
        return;
    if (enabled && m_sessionNumber == 0) {
        startNewSession(observedAtMs);
        return;
    }
    m_enabled = enabled;
    m_health.clear();
    for (auto it = m_records.begin(); it != m_records.end(); ++it) {
        it->minimumHealthMs = observedAtMs;
        it->row.healthFresh = false;
        it->row.modules.fill(Health::Unknown);
    }
}

void CheckinModel::startNewSession(qint64 observedAtMs)
{
    m_records.clear();
    m_health.clear();
    m_sessionStartedMs = observedAtMs;
    m_lastUpdatedMs = 0;
    ++m_sessionNumber;
    m_enabled = true;
}

void CheckinModel::observeDeviceHealth(quint8 robotId, quint8 bitmap,
                                       const QString &addressText, qint64 receivedAtMs)
{
    if (!m_enabled || robotId == 0 || receivedAtMs <= 0
        || receivedAtMs < m_sessionStartedMs)
        return;
    const auto previous = m_health.constFind(robotId);
    if (previous != m_health.constEnd() && previous->receivedAtMs > receivedAtMs)
        return;
    m_health.insert(robotId, Evidence{bitmap, addressText, receivedAtMs});
}

void CheckinModel::updateSnapshot(const QVector<RobotSnapshot> &robots,
                                  const QVector<DeviceSnapshot> &devices,
                                  qint64 observedAtMs)
{
    if (!m_enabled)
        return;
    m_lastUpdatedMs = observedAtMs;
    QSet<QString> seen;
    QSet<QString> matchedDevices;
    QHash<quint8, int> deviceCounts;
    for (const DeviceSnapshot &device : devices)
        if (device.robotId != 0) ++deviceCounts[device.robotId];

    const auto updateConnection = [observedAtMs](Record &record, Connection connection) {
        const bool wasConnected = connected(record.row.connection);
        if (!wasConnected && connected(connection)) {
            if (record.row.firstConnectedMs == 0)
                record.row.firstConnectedMs = observedAtMs;
            // Require new health evidence after a previously observed disconnection.
            if (record.row.offlineObservedMs > 0)
                record.minimumHealthMs = std::max(record.minimumHealthMs, record.row.offlineObservedMs);
            record.row.offlineObservedMs = 0;
        } else if (wasConnected && !connected(connection)) {
            record.row.offlineObservedMs = observedAtMs;
            record.minimumHealthMs = observedAtMs;
        }
        record.row.connection = connection;
    };

    for (const RobotSnapshot &robot : robots) {
        const DeviceSnapshot *device = nullptr;
        if (robot.protocolVersion == 2 && robot.robotId != 0 && deviceCounts.value(robot.robotId) == 1) {
            for (const DeviceSnapshot &candidate : devices) {
                if (candidate.robotId == robot.robotId
                    && (candidate.addressText.isEmpty() || robot.addressText.isEmpty()
                        || candidate.addressText == robot.addressText)) {
                    device = &candidate;
                    break;
                }
            }
        }
        const QString provisionalKey = robotKey(robot);
        const QString key = device ? deviceKey(*device) : provisionalKey;
        if (device && key != provisionalKey && m_records.contains(provisionalKey)) {
            // Registry may arrive after telemetry. Preserve the earlier observation.
            const Record old = m_records.take(provisionalKey);
            if (!m_records.contains(key)) m_records.insert(key, old);
            else if (old.row.firstConnectedMs > 0
                     && (m_records[key].row.firstConnectedMs == 0
                         || old.row.firstConnectedMs < m_records[key].row.firstConnectedMs))
                m_records[key].row.firstConnectedMs = old.row.firstConnectedMs;
        }
        Record &record = m_records[key];
        const bool identityChanged = record.row.robotId != robot.robotId
            || record.row.protocolVersion != robot.protocolVersion
            || record.row.addressText != robot.addressText;
        if (identityChanged) {
            // A new row can consume the health frame observed just before its first
            // snapshot. Existing identities must not inherit cached evidence from
            // before this binding/address change, even if it is newer than lastSeen.
            record.minimumHealthMs = std::max(record.minimumHealthMs,
                record.row.key.isEmpty() ? m_sessionStartedMs : observedAtMs);
        }
        Row &row = record.row;
        row.key = key;
        row.robotId = robot.robotId;
        row.team = robot.team;
        row.protocolVersion = robot.protocolVersion;
        row.hasRobotSnapshot = true;
        row.addressText = robot.addressText;
        row.sourcePort = robot.sourcePort;
        row.lastSeenMs = robot.lastSeenMs;
        row.serverFirstSeenMs = device
            ? earliestKnown(robot.firstSeenMs, device->firstSeenMs)
            : std::max(qint64(0), robot.firstSeenMs);
        row.hp = robot.hp;
        row.heat = robot.heat;
        row.power = robot.protocolVersion == 2 ? robot.power : -1; // V1 has no power field.
        row.rawBitmap = robot.componentOnline;
        row.issues.clear();
        row.notes.clear();
        if (device) {
            row.deviceMac = device->deviceMac.trimmed().toUpper();
            row.controllerMac = device->controllerMac;
            row.lastAnnounceMs = device->lastSeenMs;
            row.eventQueueDrops = device->eventQueueDrops;
            row.udpSendFailures = device->udpSendFailures;
            matchedDevices.insert(key);
        }
        if (deviceCounts.value(robot.robotId) > 1 && robot.protocolVersion == 2)
            row.issues << QStringLiteral("多个注册设备使用相同 robot ID，无法唯一关联 MAC");
        updateConnection(record, robot.lastSeenMs <= 0 ? Connection::Unknown
                                                       : (robot.online ? Connection::Online : Connection::Offline));
        finishRow(record, observedAtMs);
        seen.insert(key);
    }

    for (const DeviceSnapshot &device : devices) {
        const QString key = deviceKey(device);
        if (matchedDevices.contains(key)) continue;
        Record &record = m_records[key];
        Row &row = record.row;
        if (row.robotId != device.robotId || row.addressText != device.addressText)
            record.minimumHealthMs = std::max(record.minimumHealthMs, observedAtMs);
        row.key = key;
        row.robotId = device.robotId;
        row.team = 0;
        row.protocolVersion = 2;
        row.hasRobotSnapshot = false;
        row.deviceMac = device.deviceMac.trimmed().toUpper();
        row.controllerMac = device.controllerMac;
        row.addressText = device.addressText;
        row.sourcePort = device.sourcePort;
        row.lastSeenMs = device.lastSeenMs;
        row.serverFirstSeenMs = std::max(qint64(0), device.firstSeenMs);
        row.lastAnnounceMs = device.lastSeenMs;
        row.hp = row.heat = row.power = -1;
        row.rawBitmap = 0;
        row.eventQueueDrops = device.eventQueueDrops;
        row.udpSendFailures = device.udpSendFailures;
        row.issues.clear();
        row.notes.clear();
        if (device.robotId != 0 && deviceCounts.value(device.robotId) > 1)
            row.issues << QStringLiteral("多个注册设备使用相同 robot ID");
        updateConnection(record, device.lastSeenMs <= 0 ? Connection::Unknown
            : (recent(device.lastSeenMs, observedAtMs, m_timing.announceFreshnessMs)
                ? Connection::RegistryRecent : Connection::RegistryStale));
        finishRow(record, observedAtMs);
        seen.insert(key);
    }

    // Keep history for objects removed from RobotManager so reconnect retains first time.
    for (auto it = m_records.begin(); it != m_records.end(); ++it) {
        if (seen.contains(it.key())) continue;
        updateConnection(it.value(), Connection::Offline);
        it->row.issues.clear();
        it->row.notes = QStringList{QStringLiteral("不在当前快照中；保留本会话观察记录")};
        finishRow(it.value(), observedAtMs);
    }
}

void CheckinModel::finishRow(Record &record, qint64 observedAtMs)
{
    Row &row = record.row;
    row.modules.fill(Health::Unknown);
    row.healthFresh = false;
    row.healthReceivedMs = 0;
    const auto evidence = m_health.constFind(row.robotId);
    if (row.protocolVersion == 2 && row.hasRobotSnapshot && evidence != m_health.constEnd()) {
        row.healthReceivedMs = evidence->receivedAtMs;
        row.confirmedBitmap = evidence->bitmap;
        row.healthFresh = row.connection == Connection::Online
            && recent(evidence->receivedAtMs, observedAtMs, m_timing.healthFreshnessMs)
            && evidence->receivedAtMs >= record.minimumHealthMs
            && evidence->addressText == row.addressText
            && evidence->bitmap == row.rawBitmap;
    }
    if (row.healthFresh) {
        for (int bit = 0; bit < 6; ++bit) {
            row.modules[std::size_t(bit)] = (row.confirmedBitmap & (1U << bit))
                ? Health::Healthy : Health::Fault;
            if (row.modules[std::size_t(bit)] == Health::Fault)
                row.issues << QStringLiteral("%1 未在线（健康帧 bit%2=0）").arg(moduleName(bit)).arg(bit);
        }
        if (row.confirmedBitmap & 0xc0U)
            row.notes << QStringLiteral("bit6/7 为保留位，不参与检录判断");
    } else {
        row.notes << (row.protocolVersion == 1
            ? QStringLiteral("V1 不提供模块位图；模块健康未知")
            : QStringLiteral("未确认/过期健康帧；模块健康未知（原始位图不作为有效证据）"));
    }
    if (row.connection == Connection::Offline)
        row.issues << QStringLiteral("机器人离线：显示历史值，不能代表当前健康");
    if (row.connection == Connection::RegistryStale)
        row.notes << QStringLiteral("注册超时：不能仅据此判定机器人离线");
    if (!row.hasRobotSnapshot)
        row.notes << (row.robotId == 0 ? QStringLiteral("设备尚未分配 robot ID")
                                     : QStringLiteral("仅设备注册，尚无机器人遥测"));
    if (row.team == 0 && row.hasRobotSnapshot)
        row.notes << QStringLiteral("队伍尚未分配");
    if (row.hasRobotSnapshot && row.hp < 0)
        row.notes << QStringLiteral("HP 尚未上报；缺少状态不等于正常");
    if (row.eventQueueDrops > 0 || row.udpSendFailures > 0)
        row.issues << QStringLiteral("注册诊断累计计数：事件丢弃 %1 / UDP 发送失败 %2（非当前故障证明）")
            .arg(row.eventQueueDrops).arg(row.udpSendFailures);
}

QVector<Row> CheckinModel::rows() const
{
    QVector<Row> result;
    result.reserve(m_records.size());
    for (auto it = m_records.constBegin(); it != m_records.constEnd(); ++it)
        result.append(it->row);
    std::sort(result.begin(), result.end(), [](const Row &left, const Row &right) {
        if (left.robotId != right.robotId) return left.robotId < right.robotId;
        if (left.team != right.team) return left.team < right.team;
        return left.key < right.key;
    });
    return result;
}

QString CheckinModel::moduleName(int bit)
{
    // Protocol mapping documented by this project's README, DEVICE_HEALTH section.
    if (bit == 0) return QStringLiteral("L431PM");
    if (bit == 1) return QStringLiteral("枪管");
    if (bit >= 2 && bit <= 5) return QStringLiteral("装甲板 %1").arg(bit - 1);
    return QStringLiteral("保留位");
}
QString CheckinModel::healthText(Health health)
{
    switch (health) {
    case Health::Healthy: return QStringLiteral("在线");
    case Health::Fault: return QStringLiteral("异常 / 未在线");
    case Health::Unknown: return QStringLiteral("未知");
    }
    return QStringLiteral("未知");
}
QString CheckinModel::connectionText(Connection connection)
{
    switch (connection) {
    case Connection::Online: return QStringLiteral("在线");
    case Connection::Offline: return QStringLiteral("离线");
    case Connection::RegistryRecent: return QStringLiteral("注册近期 / 遥测未知");
    case Connection::RegistryStale: return QStringLiteral("注册超时 / 遥测未知");
    case Connection::Unknown: return QStringLiteral("未知 / 无上报");
    }
    return QStringLiteral("未知");
}
} // namespace checkin
