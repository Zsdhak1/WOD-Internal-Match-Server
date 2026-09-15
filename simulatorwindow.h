#ifndef SIMULATORWINDOW_H
#define SIMULATORWINDOW_H

#include <QHash>
#include <QMainWindow>

class QCheckBox;
class QComboBox;
class QHostAddress;
class QLineEdit;
class QNetworkDatagram;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTimer;
class QUdpSocket;

// 模拟 ESP32 车载节点，发送 V2 UDP 二进制帧并监听下行命令。
// 同时保留 V1 模式做服务器回归测试。
class SimulatorWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit SimulatorWindow(QWidget *parent = nullptr);

private slots:
    void onToggleRunning();
    void onSendAttack();
    void onSendHit();
    void onSendDeath();
    void onSendRevive();
    void onSendShootEnabled();
    void onSendShootDisabled();
    void onSendLinkDown();
    void onSendLinkUp();
    void onAutoDemoToggled(bool enabled);
    void sendStatus();
    void advanceDemo();
    void onDownlinkReadyRead();
    void retryReliableEvents();

private:
    void buildUi();
    QByteArray eventFrameV2(quint8 type) const;
    QByteArray reliableEventFrameV2(quint8 type, quint32 txid) const;
    QByteArray eventFrameV1(quint8 type) const;
    QByteArray statusFrame() const;
    void sendDatagram(const QByteArray &data, const QString &description);
    void sendAck(const QHostAddress &addr, quint16 port, quint8 ackedType,
                 quint32 txid, quint8 result);
    void handleDownlink(const QByteArray &data, const QHostAddress &addr,
                        quint16 port);
    void queueReliableEvent(quint8 type);
    void appendLog(const QString &message);
    quint8 team() const;
    quint8 robotId() const;
    quint8 protocolVersion() const;
    void setRunningUi(bool running);
    quint32 allocateTxid();

    QUdpSocket *m_uplink = nullptr;   // 发送到服务器 5005
    QUdpSocket *m_downlink = nullptr; // 监听 5006 接收服务器命令
    QTimer *m_statusTimer = nullptr;
    QTimer *m_demoTimer = nullptr;
    QTimer *m_retryTimer = nullptr;
    bool m_running = false;
    int m_demoStep = 0;
    quint32 m_nextTxid = 1;
    // V2 可靠事件：等待服务器 ACK
    struct PendingEvent {
        quint8 type = 0;
        quint32 txid = 0;
        int attempts = 0;
        qint64 deadlineMs = 0;
    };
    QHash<quint32, PendingEvent> m_pendingEvents;   // key = txid

    QLineEdit *m_serverEdit = nullptr;
    QSpinBox *m_portEdit = nullptr;
    QComboBox *m_versionEdit = nullptr;
    QComboBox *m_teamEdit = nullptr;
    QSpinBox *m_robotIdEdit = nullptr;
    QSpinBox *m_hpEdit = nullptr;
    QSpinBox *m_heatEdit = nullptr;
    QSpinBox *m_powerEdit = nullptr;
    QCheckBox *m_aliveEdit = nullptr;
    QCheckBox *m_shootEdit = nullptr;
    QCheckBox *m_powerOnEdit = nullptr;
    QPushButton *m_runningButton = nullptr;
    QPlainTextEdit *m_log = nullptr;
};

#endif // SIMULATORWINDOW_H
