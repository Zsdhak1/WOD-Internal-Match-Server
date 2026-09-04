#ifndef ROBOTMANAGER_H
#define ROBOTMANAGER_H

#include <QtGlobal>
#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QVector>

class QTimer;

// 登记并维护每台机器人的最新状态。一台机器人 = (队伍编号, 机器人ID)，由数据报内容决定，
// 与来源 IP/端口无关（同一热点下多车可能共源 IP）。所有操作都在 GUI 线程完成。
class RobotManager : public QObject
{
    Q_OBJECT

public:
    struct RobotInfo {
        quint8    robotId = 0;
        quint8    team = 0;
        int       hp = -1;           // -1 表示尚未收到血量信息
        bool      alive = true;
        bool      shootEnabled = true;
        bool      online = false;
        qint64    lastSeenMs = 0;
        QHostAddress addr;
        quint16   port = 0;
    };

    explicit RobotManager(QObject *parent = nullptr);

    void handleDatagram(const QByteArray &data, const QHostAddress &addr, quint16 port);
    QVector<RobotInfo> robots() const;

    static quint64 keyOf(quint8 team, quint8 id) { return (quint64(team) << 8) | id; }

signals:
    // 任一台车状态有实质变化(含上线/离线切换)时发出，UI 据此刷新。
    void robotsChanged();
    void logMessage(const QString &message);

private slots:
    void checkOffline();

private:
    QVector<RobotInfo> m_robots;
    QHash<quint64, int> m_index;     // key -> robots() 中的下标
    QTimer *m_sweep = nullptr;
};

#endif // ROBOTMANAGER_H
