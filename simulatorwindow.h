#ifndef SIMULATORWINDOW_H
#define SIMULATORWINDOW_H

#include <QHash>
#include <QMainWindow>

class QCheckBox;
class QComboBox;
class QGroupBox;
class QHostAddress;
class QLineEdit;
class QNetworkDatagram;
class QPlainTextEdit;
class QPushButton;
class QResizeEvent;
class QSpinBox;
class QTimer;
class QUdpSocket;
class ResponsiveUiScaler;

// 模拟 ESP32 车载节点，发送 V2 UDP 二进制帧并监听下行命令。
// 同时保留 V1 模式做服务器回归测试。
class SimulatorWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit SimulatorWindow(QWidget *parent = nullptr);
    // Explicit ownership keeps the responsive scaler's lifetime tied to the
    // window and ensures every translation unit sees one destructor symbol.
    ~SimulatorWindow() override;

    // Deterministic protocol clock: does not bypass the UDP/state-machine path.
    // Set before starting the simulator; advance runs timers without sleeping 5s.
    void setProtocolTimeForTest(qint64 nowMs);
    void advanceProtocolTimeForTest(qint64 elapsedMs);

protected:
    void resizeEvent(QResizeEvent *event) override;

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
    void sendDeviceAnnounce();
    void advanceDemo();
    void onDownlinkReadyRead();
    void retryReliableEvents();

private:
    void buildUi();
    void applyResponsiveUi();
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
    void loadDeviceIdentity();
    bool saveDeviceAssignment(quint8 id, const QByteArray &controller);
    bool canSendEvents() const;
    quint32 allocateTxid();
    qint64 protocolNowMs() const;
    void loadReviveState();
    bool saveReviveState();
    void clearReviveRequest();
    void requestRevive();
    void executeRevive();
    void advanceReviveState();
    void handleReviveApproval(quint8 sequence, quint32 txid,
                              const QHostAddress &addr, quint16 port);

    QUdpSocket *m_uplink = nullptr;   // 发送到服务器 5005
    QUdpSocket *m_downlink = nullptr; // 监听 5006 接收服务器命令
    QTimer *m_statusTimer = nullptr;
    QTimer *m_announceTimer = nullptr;
    QTimer *m_demoTimer = nullptr;
    QTimer *m_retryTimer = nullptr;
    bool m_running = false;
    int m_demoStep = 0;
    quint32 m_nextTxid = 1;
    QByteArray m_deviceMac;
    QByteArray m_controllerMac;
    quint8 m_assignedId = 0;
    QString m_identityProfile;
    quint32 m_udpSendFailures = 0;
    QHash<quint64, quint8> m_completedCommands;
    QList<quint64> m_completedCommandOrder;
    int m_yellowCards = 0;
    bool m_formalMode = false;
    bool m_roundEnded = false;
    quint8 m_componentOnline = 0x3f;
    qint64 m_protocolTestNowMs = -1;
    qint64 m_deathDeadlineMs = -1;
    quint8 m_nextRequestSequence = 0;
    QByteArray m_reviveRequest; // Persisted 10B frame; F0 ACK never clears it.
    bool m_reviveRequestPersisted = false;
    qint64 m_reviveRetryAtMs = 0;
    QHash<quint32, quint8> m_completedApprovals; // tx -> exact sequence, this epoch
    // V2 可靠事件：等待服务器 ACK
    struct PendingEvent {
        quint8 type = 0;
        quint32 txid = 0;
        int attempts = 0;
        qint64 deadlineMs = 0;
        QByteArray datagram;
    };
    QHash<quint32, PendingEvent> m_pendingEvents;   // key = txid

    QLineEdit *m_serverEdit = nullptr;
    QLineEdit *m_localAddressEdit = nullptr;
    QLineEdit *m_deviceMacDisplay = nullptr;
    QLineEdit *m_controllerMacDisplay = nullptr;
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
    QGroupBox *m_eventsGroup = nullptr;
    QCheckBox *m_autoDemo = nullptr;
    QPlainTextEdit *m_log = nullptr;
    ResponsiveUiScaler *m_uiScaler = nullptr;
    qreal m_uiScale = 1.0;
};

#endif // SIMULATORWINDOW_H
