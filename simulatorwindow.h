#ifndef SIMULATORWINDOW_H
#define SIMULATORWINDOW_H

#include <QMainWindow>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTimer;
class QUdpSocket;

// 模拟 ESP32 车载节点，严格发送 protocol.h 定义的 UDP 二进制帧。
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
    void onAutoDemoToggled(bool enabled);
    void sendStatus();
    void advanceDemo();

private:
    void buildUi();
    QByteArray eventFrame(quint8 type) const;
    void sendDatagram(const QByteArray &data, const QString &description);
    void appendLog(const QString &message);
    quint8 team() const;
    quint8 robotId() const;
    void setRunningUi(bool running);

    QUdpSocket *m_socket = nullptr;
    QTimer *m_statusTimer = nullptr;
    QTimer *m_demoTimer = nullptr;
    bool m_running = false;
    int m_demoStep = 0;

    QLineEdit *m_serverEdit = nullptr;
    QSpinBox *m_portEdit = nullptr;
    QComboBox *m_teamEdit = nullptr;
    QSpinBox *m_robotIdEdit = nullptr;
    QSpinBox *m_hpEdit = nullptr;
    QSpinBox *m_heatEdit = nullptr;
    QCheckBox *m_aliveEdit = nullptr;
    QCheckBox *m_shootEdit = nullptr;
    QPushButton *m_runningButton = nullptr;
    QPlainTextEdit *m_log = nullptr;
};

#endif // SIMULATORWINDOW_H
