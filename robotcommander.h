#ifndef ROBOTCOMMANDER_H
#define ROBOTCOMMANDER_H

#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QPair>
#include <QTimer>

class QUdpSocket;

// V2 下行命令管理：服务器 -> ESP32:5006 的可靠命令。
// 使用共享的上行 socket（绑定 5005）发送，这样 ESP32 的 ACK 会回到 5005。
// 普通命令分配唯一 transaction_id；复活批准必须使用请求的原 txid。
// 未收到 ACK 时用相同帧重传最多 2 次；超时仅表示执行结果未知。
class RobotCommander : public QObject
{
    Q_OBJECT

public:
    explicit RobotCommander(QUdpSocket *uplinkSocket, QObject *parent = nullptr);

    // 由 RobotManager 在学习到 ESP32 端点时调用
    void learnEndpoint(quint8 robotId, quint32 ipv4, quint16 port);
    void forgetEndpoint(quint8 robotId);
    void cancelPending();
    void cancelPendingType(quint8 type);
    void cancelPendingForTarget(quint8 targetId);

    // 所有命令都返回 transaction_id；0 表示无法发送（未知端点或 socket 不可用）。
    // 最新 V2 不支持广播，所有业务命令都必须指定非零 robotId。
    quint32 sendGameStart(quint8 targetId);
    quint32 sendGameEnd(quint8 targetId);
    quint32 sendYellowCard(quint8 targetId);
    quint32 sendForcePowerOff(quint8 targetId);
    quint32 sendForcePowerOn(quint8 targetId);
    quint32 sendSetHp(quint8 targetId, quint16 hp);
    // 0x8A：node=1..4，profile 0=静止 1=运动；ACK result=0 仅表示 L431 已保存。
    quint32 sendArmorThreshold(quint8 targetId, quint8 node, quint8 profile, quint32 value);
    // Returns requestTx (never allocates a new tx), or 0 on send failure.
    quint32 sendReviveApproval(quint8 targetId, quint8 sequence, quint32 requestTx);
    quint32 sendAssignment(quint32 ipv4, quint8 robotId,
                           const QByteArray &deviceMac6,
                           const QByteArray &controllerMac6);
    bool sendStatusRequest(quint8 targetId);   // 无 ACK；true 仅表示数据报已发送

    // Wire ACKs must use onAckFrom. The old API is retained for trusted,
    // explicitly attributed callers/tests only; robotId=0 cannot confirm anything.
    void onAck(quint8 ackedType, quint32 txid, quint8 result, quint8 robotId);

    void onAckFrom(quint8 ackedType, quint32 txid, quint8 result,
                   const QHostAddress &addr, quint16 sourcePort);

    // 当前 pending 命令数（供 UI 显示）
    int pendingCount() const { return m_pending.size(); }

signals:
    void commandAcked(quint32 txid, quint8 type, quint8 result, quint8 robotId);
    void commandTimeout(quint32 txid, quint8 type, quint8 robotId);
    void logMessage(const QString &message);

private slots:
    void onRetryTick();

private:
    struct Pending {
        QByteArray   datagram;
        quint8       type = 0;
        quint8       targetId = 0;
        quint32      ipv4 = 0;
        quint16      port = 0;
        int          attempts = 0;
        qint64       deadlineMs = 0;
        int          retryIntervalMs = 100; // 0x8A 需等 L431 Flash：1000 ms
    };

    quint32 sendCommand(quint8 type, quint8 targetId, const QByteArray &datagram,
                        bool needsAck);
    quint32 sendCommandTo(quint8 type, quint8 targetId, quint32 ipv4,
                          quint16 port, const QByteArray &datagram,
                          bool needsAck);
    quint32 allocateTxid();
    static quint64 pendingKey(quint8 type, quint8 targetId, quint32 txid)
    { return (quint64(type) << 40) | (quint64(targetId) << 32) | txid; }
    void finishAck(quint64 key, quint32 txid, quint8 result);

    QUdpSocket *m_uplink = nullptr;
    QHash<quint64, Pending> m_pending; // (type, targetId, tx), not tx alone
    QHash<quint8, QPair<quint32, quint16>> m_endpoints;  // robotId -> (IPv4, port)
    QTimer *m_retryTimer = nullptr;
};

#endif // ROBOTCOMMANDER_H
