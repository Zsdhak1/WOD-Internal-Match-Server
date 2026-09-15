#include "robotcommander.h"
#include "protocol.h"

#include <QDateTime>
#include <QUdpSocket>

namespace {
constexpr int    kRetryIntervalMs = 100;   // ESP32 README §5 建议 100ms
constexpr int    kMaxAttempts     = 3;     // 初始 + 2 次重传
constexpr qint64 kCommandTimeoutMs = 400;  // 100ms * 3 + 余量

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

void RobotCommander::learnEndpoint(quint8 robotId, const QHostAddress &ip,
                                   quint16 port)
{
    m_endpoints.insert(robotId, {ip, port});
}

quint32 RobotCommander::allocateTxid()
{
    // ESP32 要求非 0 transaction_id
    const quint32 id = m_nextTxid;
    m_nextTxid = (m_nextTxid + 1 == 0) ? 1 : m_nextTxid + 1;
    return id;
}

quint32 RobotCommander::sendCommand(quint8 type, quint8 targetId,
                                  const QByteArray &datagram, bool needsAck)
{
    if (!m_uplink || datagram.isEmpty())
        return 0;

    // 广播命令：targetId=0 时发送到所有已知端点
    if (targetId == 0) {
        if (m_endpoints.isEmpty()) {
            emit logMessage(QStringLiteral("[下行] %1 广播失败：无已知 ESP32 端点")
                                .arg(typeName(type)));
            return 0;
        }
        // 广播命令也用同一个 txid，ESP32 各自回 ACK；我们按 robotId 区分
        const quint32 txid = allocateTxid();
        for (auto it = m_endpoints.constBegin(); it != m_endpoints.constEnd(); ++it) {
            const QHostAddress &ip = it->first;
            const quint16 port = it->second;
            m_uplink->writeDatagram(datagram, ip, port);
            if (needsAck) {
                Pending p;
                p.datagram = datagram;
                p.type = type;
                p.targetId = it.key();   // 实际目标 robotId
                p.ip = ip;
                p.port = port;
                p.attempts = 1;
                p.deadlineMs = QDateTime::currentMSecsSinceEpoch() + kCommandTimeoutMs;
                // 广播时每车一个 pending 项，key 为 (txid << 8 | robotId)
                m_pending.insert((txid << 8) | it.key(), p);
            }
        }
        emit logMessage(QStringLiteral("[下行] %1 广播到 %2 台 ESP32 (txid=%3)")
                            .arg(typeName(type))
                            .arg(m_endpoints.size())
                            .arg(txid));
        return txid;
    }

    // 单播命令
    const auto ep = m_endpoints.constFind(targetId);
    if (ep == m_endpoints.constEnd()) {
        emit logMessage(QStringLiteral("[下行] %1 到机器人%2 失败：未知端点")
                            .arg(typeName(type)).arg(targetId));
        return 0;
    }

    const quint32 txid = allocateTxid();
    m_uplink->writeDatagram(datagram, ep->first, ep->second);

    if (needsAck) {
        Pending p;
        p.datagram = datagram;
        p.type = type;
        p.targetId = targetId;
        p.ip = ep->first;
        p.port = ep->second;
        p.attempts = 1;
        p.deadlineMs = QDateTime::currentMSecsSinceEpoch() + kCommandTimeoutMs;
        m_pending.insert(txid, p);
    }

    emit logMessage(QStringLiteral("[下行] %1 -> 机器人%2 @%3:%4 (txid=%5)")
                        .arg(typeName(type))
                        .arg(targetId)
                        .arg(ep->first.toString())
                        .arg(ep->second)
                        .arg(txid));
    return txid;
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

quint32 RobotCommander::sendAssignment(quint8 robotId, const QByteArray &mac6)
{
    if (robotId == 0 || mac6.size() != 6) return 0;
    const quint32 txid = allocateTxid();
    return sendCommand(proto::TypeAssignment, robotId,
                       proto::makeAssignment(robotId, mac6, txid), true);
}

quint32 RobotCommander::sendStatusRequest(quint8 targetId)
{
    // STATUS_REQUEST 不需要 ACK，但用统一 sendCommand 路径（needsAck=false）
    return sendCommand(proto::TypeStatusRequest, targetId,
                       proto::makeStatusRequest(targetId), false);
}

void RobotCommander::onAck(quint8 ackedType, quint32 txid, quint8 result,
                           quint8 robotId)
{
    // 单播命令的 pending key 就是 txid
    auto it = m_pending.find(txid);
    if (it != m_pending.end()) {
        m_pending.erase(it);
        emit commandAcked(txid, ackedType, result, robotId);
        emit logMessage(QStringLiteral("[ACK] %1 机器人%2 返回 result=%3 (txid=%4)")
                            .arg(typeName(ackedType))
                            .arg(robotId)
                            .arg(result)
                            .arg(txid));
        return;
    }

    // 广播命令的 pending key 是 (txid << 8 | robotId)
    const quint32 bkey = (txid << 8) | robotId;
    it = m_pending.find(bkey);
    if (it != m_pending.end()) {
        m_pending.erase(it);
        emit commandAcked(txid, ackedType, result, robotId);
        emit logMessage(QStringLiteral("[ACK] %1 机器人%2 返回 result=%3 (txid=%4, 广播)")
                            .arg(typeName(ackedType))
                            .arg(robotId)
                            .arg(result)
                            .arg(txid));
    }
}

void RobotCommander::onRetryTick()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<quint32> toRemove;
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
        Pending &p = it.value();
        if (now < p.deadlineMs)
            continue;
        if (p.attempts >= kMaxAttempts) {
            toRemove.append(it.key());
            emit commandTimeout(it.key() & 0xFFFFFF00, p.type, p.targetId);
            emit logMessage(QStringLiteral("[超时] %1 机器人%2 未收到 ACK (txid=%3)")
                                .arg(typeName(p.type))
                                .arg(p.targetId)
                                .arg(it.key() & 0xFFFFFF00));
            continue;
        }
        // 用相同 txid 重发
        m_uplink->writeDatagram(p.datagram, p.ip, p.port);
        ++p.attempts;
        p.deadlineMs = now + kRetryIntervalMs;
        emit logMessage(QStringLiteral("[重传] %1 机器人%2 第%3次 (txid=%4)")
                            .arg(typeName(p.type))
                            .arg(p.targetId)
                            .arg(p.attempts)
                            .arg(it.key() & 0xFFFFFF00));
    }
    for (quint32 key : toRemove)
        m_pending.remove(key);
}
