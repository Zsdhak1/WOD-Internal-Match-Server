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
// 每个命令分配唯一 transaction_id；未收到 ACK 时用相同 txid 重传最多 2 次。
class RobotCommander : public QObject
{
    Q_OBJECT

public:
    explicit RobotCommander(QUdpSocket *uplinkSocket, QObject *parent = nullptr);

    // 由 RobotManager 在学习到 ESP32 端点时调用
    void learnEndpoint(quint8 robotId, const QHostAddress &ip, quint16 port);

    // 所有命令都返回 transaction_id；0 表示无法发送（未知端点或 socket 不可用）。
    // targetId=0 表示广播（仅 GAME_START/GAME_END 允许），其他必须指定 robotId。
    quint32 sendGameStart(quint8 targetId);
    quint32 sendGameEnd(quint8 targetId);
    quint32 sendYellowCard(quint8 targetId);
    quint32 sendForcePowerOff(quint8 targetId);
    quint32 sendForcePowerOn(quint8 targetId);
    quint32 sendSetHp(quint8 targetId, quint16 hp);
    quint32 sendAssignment(quint8 robotId, const QByteArray &mac6);
    quint32 sendStatusRequest(quint8 targetId);   // 无 ACK

    // 由 RobotManager 在收到 ESP32 ACK 时调用
    void onAck(quint8 ackedType, quint32 txid, quint8 result, quint8 robotId);

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
        QHostAddress ip;
        quint16      port = 0;
        int          attempts = 0;
        qint64       deadlineMs = 0;
    };

    quint32 sendCommand(quint8 type, quint8 targetId, const QByteArray &datagram,
                        bool needsAck);
    quint32 allocateTxid();

    QUdpSocket *m_uplink = nullptr;
    QHash<quint32, Pending> m_pending;
    QHash<quint8, QPair<QHostAddress, quint16>> m_endpoints;  // robotId -> (ip, port)
    QTimer *m_retryTimer = nullptr;
    quint32 m_nextTxid = 1;
};

#endif // ROBOTCOMMANDER_H
