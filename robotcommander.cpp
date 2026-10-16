#include "robotcommander.h"
#include "protocol.h"

#include <QDateTime>
#include <QHostAddress>
#include <QRandomGenerator>
#include <QSettings>
#include <QUdpSocket>

namespace {
constexpr int    kRetryIntervalMs = 100;   // ESP32 README §5 建议 100ms
constexpr int    kMaxAttempts     = 3;     // 初始 + 2 次重传
constexpr qint64 kCommandTimeoutMs = 400;  // 100ms * 3 + 余量
constexpr int    kSlowRetryIntervalMs = 1000; // 0x8A：协议 CLI 每 1 s 重试，共 3 次

QString typeName(quint8 type)
{
    switch (type) {
    case proto::TypeGameStart:     return QStringLiteral("比赛开始");
    case proto::TypeGameEnd:       return QStringLiteral("比赛结束");
    case proto::TypeAssignment:    return QStringLiteral("分配ID/MAC");
    case proto::TypeStatusRequest: return QStringLiteral("请求状态");
    case proto::TypeSetHp:         return QStringLiteral("设置HP");
    case proto::TypeYellowCard:    return QStringLiteral("黄牌处罚");
    case proto::TypeForcePowerOff: return QStringLiteral("强制断电");
    case proto::TypeReviveApprove: return QStringLiteral("复活批准");
    case proto::TypeArmorThresholdSet: return QStringLiteral("装甲板阈值");
    case proto::TypeForcePowerOn:  return QStringLiteral("请求通电");
    default:                       return QStringLiteral("0x%1").arg(type, 2, 16);
    }
}
} // namespace

RobotCommander::RobotCommander(QUdpSocket *uplinkSocket, QObject *parent)
    : QObject(parent)
    , m_uplink(uplinkSocket)
{
    m_retryTimer = new QTimer(this);
    m_retryTimer->setInterval(50);   // 50ms 检查一次重传队列
    connect(m_retryTimer, &QTimer::timeout, this, &RobotCommander::onRetryTick);
    m_retryTimer->start();
}

void RobotCommander::learnEndpoint(quint8 robotId, quint32 ipv4,
                                   quint16 port)
{
    if (robotId != 0 && ipv4 != 0)
        m_endpoints.insert(robotId, {ipv4, port});
}

quint32 RobotCommander::allocateTxid()
{
    // Devices remember completed commands across server restarts. Persist
    // before sending so a new command cannot accidentally replay an old ID.
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope,
                       QStringLiteral("Scompetition"), QStringLiteral("RobotCommands"));
    quint32 id = settings.value(QStringLiteral("lastTxid"),
                               QRandomGenerator::global()->generate()).toUInt() + 1u;
    if (!id) id = 1;
    settings.setValue(QStringLiteral("lastTxid"), id);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        emit logMessage(QStringLiteral("[下行] 无法保存事务号，命令未发送"));
        return 0;
    }
    return id;
}

void RobotCommander::forgetEndpoint(quint8 robotId)
{
    m_endpoints.remove(robotId);
}

void RobotCommander::cancelPending()
{
    m_pending.clear();
}

void RobotCommander::cancelPendingType(quint8 type)
{
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (it->type == type) it = m_pending.erase(it);
        else ++it;
    }
}

void RobotCommander::cancelPendingForTarget(quint8 targetId)
{
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (it->targetId == targetId) it = m_pending.erase(it);
        else ++it;
    }
}

quint32 RobotCommander::sendCommand(quint8 type, quint8 targetId,
                                  const QByteArray &datagram, bool needsAck)
{
    if (!m_uplink || datagram.isEmpty())
        return 0;

    // 最新 V2 明确不支持比赛控制广播，所有业务命令必须指定 robot_id。
    if (targetId == 0) {
        emit logMessage(QStringLiteral("[下行] %1 被拒绝：最新 V2 不支持广播")
                            .arg(typeName(type)));
        return 0;
    }

    // 单播命令
    const auto ep = m_endpoints.constFind(targetId);
    if (ep == m_endpoints.constEnd()) {
        emit logMessage(QStringLiteral("[下行] %1 到机器人%2 失败：未知端点")
                            .arg(typeName(type)).arg(targetId));
        return 0;
    }

    return sendCommandTo(type, targetId, ep->first, ep->second, datagram, needsAck);
}

quint32 RobotCommander::sendCommandTo(quint8 type, quint8 targetId, quint32 ipv4,
                                      quint16 port, const QByteArray &datagram,
                                      bool needsAck)
{
    if (!m_uplink || m_uplink->state() != QAbstractSocket::BoundState
        || ipv4 == 0 || port == 0 || datagram.isEmpty())
        return 0;

    const QHostAddress address(ipv4);
    quint32 txid = 0;
    if (needsAck) {
        const int txidOffset = type == proto::TypeSetHp ? 7
                               : type == proto::TypeArmorThresholdSet ? 11
                               : type == proto::TypeAssignment ? 17
                               : type == proto::TypeReviveApprove ? 6 : 5;
        if (datagram.size() < txidOffset + 4)
            return 0;
        txid = static_cast<quint32>(static_cast<quint8>(datagram.at(txidOffset)))
              | (static_cast<quint32>(static_cast<quint8>(datagram.at(txidOffset + 1))) << 8)
              | (static_cast<quint32>(static_cast<quint8>(datagram.at(txidOffset + 2))) << 16)
              | (static_cast<quint32>(static_cast<quint8>(datagram.at(txidOffset + 3))) << 24);
        if (txid == 0)
            return 0;
    }

    const qint64 written = m_uplink->writeDatagram(datagram, address, port);
    if (written != datagram.size()) {
        qWarning().noquote()
            << QStringLiteral("[下行] %1 到机器人%2 @%3:%4 发送失败: %5")
                   .arg(typeName(type)).arg(targetId).arg(address.toString())
                   .arg(port).arg(m_uplink->errorString());
        return 0;
    }

    if (needsAck) {
        Pending p;
        p.datagram = datagram;
        p.type = type;
        p.targetId = targetId;
        p.ipv4 = ipv4;
        p.port = port;
        p.attempts = 1;
        // ESP32 holds 0x8A until L431 confirms its Flash save (<=200 ms) and
        // queues every copy; the firmware CLI retries once per second.
        const bool slow = type == proto::TypeArmorThresholdSet;
        p.retryIntervalMs = slow ? kSlowRetryIntervalMs : kRetryIntervalMs;
        p.deadlineMs = QDateTime::currentMSecsSinceEpoch()
                       + (slow ? kSlowRetryIntervalMs : kCommandTimeoutMs);
        m_pending.insert(pendingKey(type, targetId, txid), p);
    }

    emit logMessage(QStringLiteral("[下行] %1 -> 机器人%2 @%3:%4 (txid=%5)")
                        .arg(typeName(type))
                        .arg(targetId)
                        .arg(address.toString())
                        .arg(port)
                        .arg(txid));
    // No-ACK requests return only a send-success flag, not a transaction ID.
    return needsAck ? txid : 1u;
}

quint32 RobotCommander::sendGameStart(quint8 targetId)
{
    const quint32 txid = allocateTxid();
    return sendCommand(proto::TypeGameStart, targetId,
                       proto::makeGameStart(targetId, txid), true);
}

quint32 RobotCommander::sendGameEnd(quint8 targetId)
{
    const quint32 txid = allocateTxid();
    return sendCommand(proto::TypeGameEnd, targetId,
                       proto::makeGameEnd(targetId, txid), true);
}

quint32 RobotCommander::sendYellowCard(quint8 targetId)
{
    if (targetId == 0) return 0;   // 黄牌只能单播
    const quint32 txid = allocateTxid();
    return sendCommand(proto::TypeYellowCard, targetId,
                       proto::makeYellowCard(targetId, txid), true);
}

quint32 RobotCommander::sendForcePowerOff(quint8 targetId)
{
    if (targetId == 0) return 0;
    const quint32 txid = allocateTxid();
    return sendCommand(proto::TypeForcePowerOff, targetId,
                       proto::makeForcePowerOff(targetId, txid), true);
}

quint32 RobotCommander::sendForcePowerOn(quint8 targetId)
{
    if (targetId == 0) return 0;
    const quint32 txid = allocateTxid();
    return sendCommand(proto::TypeForcePowerOn, targetId,
                       proto::makeForcePowerOn(targetId, txid), true);
}

quint32 RobotCommander::sendSetHp(quint8 targetId, quint16 hp)
{
    if (targetId == 0 || hp > 300) return 0;
    const quint32 txid = allocateTxid();
    return sendCommand(proto::TypeSetHp, targetId,
                       proto::makeSetHp(targetId, hp, txid), true);
}

quint32 RobotCommander::sendArmorThreshold(quint8 targetId, quint8 node, quint8 profile,
                                           quint32 value)
{
    if (!targetId || node < 1 || node > proto::kArmorNodeCount || profile > 1
        || value < proto::kArmorThresholdMin || value > proto::kArmorThresholdMax)
        return 0;
    const quint32 txid = allocateTxid();
    return sendCommand(proto::TypeArmorThresholdSet, targetId,
                       proto::makeArmorThresholdSet(targetId, node, profile, value, txid), true);
}

quint32 RobotCommander::sendReviveApproval(quint8 targetId, quint8 sequence,
                                          quint32 requestTx)
{
    if (!targetId || !requestTx) return 0;
    return sendCommand(proto::TypeReviveApprove, targetId,
                       proto::makeReviveApprove(targetId, sequence, requestTx), true);
}

quint32 RobotCommander::sendAssignment(quint32 ipv4, quint8 robotId,
                                       const QByteArray &deviceMac6,
                                       const QByteArray &controllerMac6)
{
    if (robotId == 0 || deviceMac6.size() != 6 || controllerMac6.size() != 6)
        return 0;
    const quint32 txid = allocateTxid();
    return sendCommandTo(proto::TypeAssignment, robotId, ipv4,
                         proto::kDownlinkPort,
                         proto::makeAssignment(robotId, deviceMac6,
                                               controllerMac6, txid),
                         true);
}

bool RobotCommander::sendStatusRequest(quint8 targetId)
{
    // STATUS_REQUEST 不需要 ACK，但用统一 sendCommand 路径（needsAck=false）
    return sendCommand(proto::TypeStatusRequest, targetId,
                       proto::makeStatusRequest(targetId), false);
}

void RobotCommander::finishAck(quint64 key, quint32 txid, quint8 result)
{
    auto it = m_pending.find(key);
    if (it == m_pending.end() || result > 2) return;
    const quint8 type = it->type;
    const quint8 targetId = it->targetId;
    m_pending.erase(it);
    // result=2 is execution-unknown, not proof of non-execution. Business logic
    // decides whether/how to retry. In particular, do not allocate another tx.
    emit commandAcked(txid, type, result, targetId);
    emit logMessage(QStringLiteral("[ACK] %1 机器人%2 返回 result=%3 (txid=%4)")
                        .arg(typeName(type)).arg(targetId).arg(result).arg(txid));
}

void RobotCommander::onAck(quint8 ackedType, quint32 txid, quint8 result,
                           quint8 robotId)
{
    // Compatibility for trusted ID-attributed injection only. Raw F0 has no ID
    // and therefore cannot use this path to claim a command was confirmed.
    if (!robotId || !m_endpoints.contains(robotId)) return;
    const quint64 key = pendingKey(ackedType, robotId, txid);
    const auto it = m_pending.constFind(key);
    if (it == m_pending.constEnd()
        || it->ipv4 != m_endpoints.value(robotId).first) return;
    finishAck(key, txid, result);
}

void RobotCommander::onAckFrom(quint8 ackedType, quint32 txid, quint8 result,
                               const QHostAddress &addr, quint16 sourcePort)
{
    if (addr.protocol() != QAbstractSocket::IPv4Protocol || !sourcePort
        || !txid || result > 2) return;
    const quint32 ipv4 = addr.toIPv4Address();
    quint64 found = 0;
    for (auto it = m_pending.constBegin(); it != m_pending.constEnd(); ++it) {
        if (it->type != ackedType || quint32(it.key()) != txid || it->ipv4 != ipv4)
            continue;
        // Multiple IDs on one IP can share request_tx. F0 contains no target;
        // an ambiguous source must not be guessed/confirmed.
        if (found) return;
        found = it.key();
    }
    // ESP32/simulator ACKs leave the uplink's source port, not command port 5006.
    // Match the pinned destination IP, not a learned/changed endpoint or 5006.
    if (found) finishAck(found, txid, result);
}

void RobotCommander::onRetryTick()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // Business callbacks can cancel/send commands during timeout/log signals.
    // Iterate a key snapshot and remove/update before emitting, not live iterators.
    const auto keys = m_pending.keys();
    for (quint64 key : keys) {
        auto it = m_pending.find(key);
        if (it == m_pending.end() || now < it->deadlineMs) continue;
        Pending p = it.value();
        if (p.attempts >= kMaxAttempts) {
            m_pending.erase(it);
            emit commandTimeout(quint32(key), p.type, p.targetId);
            emit logMessage(QStringLiteral("[超时] %1 机器人%2 未收到 ACK，执行结果未知 (txid=%3)")
                                .arg(typeName(p.type)).arg(p.targetId).arg(quint32(key)));
            continue;
        }
        ++it->attempts;
        it->deadlineMs = now + it->retryIntervalMs;
        const qint64 written = m_uplink->writeDatagram(
            p.datagram, QHostAddress(p.ipv4), p.port);
        if (written != p.datagram.size()) {
            qWarning().noquote()
                << QStringLiteral("[重传] %1 机器人%2 写入失败: %3")
                       .arg(typeName(p.type)).arg(p.targetId).arg(m_uplink->errorString());
        }
        emit logMessage(QStringLiteral("[重传] %1 机器人%2 第%3次 (txid=%4)")
                            .arg(typeName(p.type)).arg(p.targetId)
                            .arg(p.attempts + 1).arg(quint32(key)));
    }
}
