#ifndef ROBOTMANAGER_H
#define ROBOTMANAGER_H

#include <QtGlobal>
#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QSet>
#include <QVector>

class QTimer;
class QUdpSocket;

// 登记并维护每台机器人的最新状态。
// V1 帧自带 team 字段；V2 帧不带，需要服务端维护 robot_id -> team 映射。
// 一台机器人 = (team, robotId)，与来源 IP/端口无关。
// V2 死亡/复活帧带 transaction_id，服务端必须回 ACK 并按 txid 去重。
class RobotManager : public QObject
{
    Q_OBJECT

public:
    struct RobotInfo {
        quint8    robotId = 0;
        quint8    team = 0;            // V1 自帧内; V2 来自服务器映射
        quint8    protocolVersion = 0; // 最近一次的协议版本（1 或 2）
        int       hp = -1;
        int       heat = -1;
        int       power = -1;          // V2 only: 整数瓦特
        bool      alive = true;
        bool      shootEnabled = true;
        bool      powerOn = false;     // V2 only: 底盘供电输出
        bool      linkUp = true;       // V2 only: L431 业务链路
        bool      online = false;
        qint64    lastSeenMs = 0;
        QHostAddress addr;
        quint16   port = 0;
        quint32   lastTxid = 0;        // 最近一次可靠事件的 txid
    };

    explicit RobotManager(QObject *parent = nullptr);

    void handleDatagram(const QByteArray &data, const QHostAddress &addr, quint16 port);
    QVector<RobotInfo> robots() const;

    // V2：服务端维护 robot_id -> team 映射。team=0 表示未分配。
    void   setTeamForRobot(quint8 robotId, quint8 team);
    quint8 teamForRobot(quint8 robotId) const;
    const QHash<quint8, quint8> &teamMap() const { return m_robotTeamMap; }

    // V2：下行命令需要 ESP32 的 IP:port。返回 false 表示未知。
    bool endpointForRobot(quint8 robotId, QHostAddress *ip, quint16 *port) const;

    // V2：回 ACK 给 ESP32。由调用方传入 uplink socket。
    void sendAck(QUdpSocket *socket, const QHostAddress &addr, quint16 port,
                 quint8 ackedType, quint32 txid, quint8 result);

    // V2：检查事务是否已处理过；处理后须调用 markProcessed。
    bool isTransactionProcessed(quint8 robotId, quint8 type, quint32 txid) const;
    void markTransactionProcessed(quint8 robotId, quint8 type, quint32 txid);

    // V1 兼容：旧 keyOf(team,id)
    static quint64 keyOf(quint8 team, quint8 id) { return (quint64(team) << 8) | id; }
    // V2 主键（team 可能为 0，未分配）
    static quint64 keyOfV2(quint8 robotId) { return quint64(robotId); }

signals:
    void robotsChanged();
    void logMessage(const QString &message);
    void combatEvent(quint8 team, quint8 robotId, quint8 type);
    // V2：收到可靠事件时要求调用方回 ACK
    void reliableEventNeedsAck(quint8 robotId, quint8 type, quint32 txid,
                               const QHostAddress &addr, quint16 port);
    // V2：收到 ESP32 的 ACK
    void ackReceived(quint8 ackedType, quint32 txid, quint8 result, quint8 robotId);
    // V2：学习到的 ESP32 端点，供下行命令使用
    void endpointLearned(quint8 robotId, const QHostAddress &ip, quint16 port);

private slots:
    void checkOffline();

private:
    void loadTeamMap();
    void saveTeamMap();
    void loadProcessedTxids();
    void saveProcessedTxids();

    QVector<RobotInfo> m_robots;
    QHash<quint64, int> m_index;             // key -> robots() 中的下标
    QHash<quint8, quint8> m_robotTeamMap;    // V2: robotId -> team
    QHash<quint8, QSet<quint64>> m_processedTxids; // V2: robotId -> set of (type<<32|txid)
    QHash<quint8, QPair<QHostAddress, quint16>> m_endpoints; // V2: robotId -> last addr
    QTimer *m_sweep = nullptr;
};

#endif // ROBOTMANAGER_H
