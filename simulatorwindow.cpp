#include "simulatorwindow.h"

#include "protocol.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QFont>
#include <QGroupBox>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QUdpSocket>
#include <QVBoxLayout>

namespace {
constexpr int kDefaultHp = 5000;
constexpr int kStatusIntervalMs = 100;
constexpr int kDemoIntervalMs = 1800;
}

SimulatorWindow::SimulatorWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("模拟车载端"));
    resize(720, 660);

    m_socket = new QUdpSocket(this);
    m_statusTimer = new QTimer(this);
    m_statusTimer->setInterval(kStatusIntervalMs);
    connect(m_statusTimer, &QTimer::timeout, this, &SimulatorWindow::sendStatus);

    m_demoTimer = new QTimer(this);
    m_demoTimer->setInterval(kDemoIntervalMs);
    connect(m_demoTimer, &QTimer::timeout, this, &SimulatorWindow::advanceDemo);

    buildUi();
    setRunningUi(false);
}

void SimulatorWindow::buildUi()
{
    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(16, 14, 16, 14);

    auto *title = new QLabel(tr("模拟车载端 · ESP32 UDP 协议测试器"), central);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSize(17);
    title->setFont(titleFont);
    root->addWidget(title);

    auto *networkGroup = new QGroupBox(tr("连接参数"), central);
    auto *networkForm = new QFormLayout(networkGroup);
    m_serverEdit = new QLineEdit(QStringLiteral("127.0.0.1"), networkGroup);
    m_portEdit = new QSpinBox(networkGroup);
    m_portEdit->setRange(1, 65535);
    m_portEdit->setValue(proto::kBindPort);
    networkForm->addRow(tr("服务器地址"), m_serverEdit);
    networkForm->addRow(tr("UDP 端口"), m_portEdit);
    root->addWidget(networkGroup);

    auto *robotGroup = new QGroupBox(tr("机器人身份与状态"), central);
    auto *robotForm = new QFormLayout(robotGroup);
    m_teamEdit = new QComboBox(robotGroup);
    m_teamEdit->addItem(tr("红方"), 1);
    m_teamEdit->addItem(tr("蓝方"), 2);
    m_robotIdEdit = new QSpinBox(robotGroup);
    m_robotIdEdit->setRange(1, 255);
    m_robotIdEdit->setValue(1);
    m_hpEdit = new QSpinBox(robotGroup);
    m_hpEdit->setRange(0, 5000);
    m_hpEdit->setValue(kDefaultHp);
    m_heatEdit = new QSpinBox(robotGroup);
    m_heatEdit->setRange(0, 1000);
    m_heatEdit->setValue(0);
    m_aliveEdit = new QCheckBox(tr("存活"), robotGroup);
    m_aliveEdit->setChecked(true);
    m_shootEdit = new QCheckBox(tr("允许射击"), robotGroup);
    m_shootEdit->setChecked(true);
    robotForm->addRow(tr("队伍"), m_teamEdit);
    robotForm->addRow(tr("机器人 ID"), m_robotIdEdit);
    robotForm->addRow(tr("当前 HP"), m_hpEdit);
    robotForm->addRow(tr("当前热量"), m_heatEdit);
    robotForm->addRow(QString(), m_aliveEdit);
    robotForm->addRow(QString(), m_shootEdit);
    root->addWidget(robotGroup);

    auto *controlRow = new QHBoxLayout;
    m_runningButton = new QPushButton(tr("启动模拟车"), central);
    connect(m_runningButton, &QPushButton::clicked, this, &SimulatorWindow::onToggleRunning);
    auto *autoDemo = new QCheckBox(tr("自动演示攻击/受击/死亡/复活"), central);
    connect(autoDemo, &QCheckBox::toggled, this, &SimulatorWindow::onAutoDemoToggled);
    controlRow->addWidget(m_runningButton);
    controlRow->addWidget(autoDemo);
    controlRow->addStretch(1);
    root->addLayout(controlRow);

    auto *events = new QGroupBox(tr("一次性事件"), central);
    auto *eventLayout = new QHBoxLayout(events);
    const auto addEventButton = [this, eventLayout](const QString &text, auto slot) {
        auto *button = new QPushButton(text, this);
        connect(button, &QPushButton::clicked, this, slot);
        eventLayout->addWidget(button);
    };
    addEventButton(tr("攻击"), &SimulatorWindow::onSendAttack);
    addEventButton(tr("受击 -500"), &SimulatorWindow::onSendHit);
    addEventButton(tr("死亡"), &SimulatorWindow::onSendDeath);
    addEventButton(tr("复活"), &SimulatorWindow::onSendRevive);
    addEventButton(tr("允许射击"), &SimulatorWindow::onSendShootEnabled);
    addEventButton(tr("禁止射击"), &SimulatorWindow::onSendShootDisabled);
    root->addWidget(events);

    auto *logTitle = new QLabel(tr("发送日志"), central);
    QFont logFont = logTitle->font();
    logFont.setBold(true);
    logTitle->setFont(logFont);
    root->addWidget(logTitle);
    m_log = new QPlainTextEdit(central);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(1000);
    root->addWidget(m_log, 1);

    setCentralWidget(central);
}

quint8 SimulatorWindow::team() const
{
    return static_cast<quint8>(m_teamEdit->currentData().toInt());
}

quint8 SimulatorWindow::robotId() const
{
    return static_cast<quint8>(m_robotIdEdit->value());
}

QByteArray SimulatorWindow::eventFrame(quint8 type) const
{
    return proto::makeEventDatagram(type, robotId(), team());
}

void SimulatorWindow::onToggleRunning()
{
    m_running = !m_running;
    if (m_running) {
        m_statusTimer->start();
        sendStatus();
        appendLog(tr("模拟车已启动：%1·机器人%2 -> %3:%4")
                      .arg(m_teamEdit->currentText())
                      .arg(robotId())
                      .arg(m_serverEdit->text())
                      .arg(m_portEdit->value()));
    } else {
        m_statusTimer->stop();
        m_demoTimer->stop();
        appendLog(tr("模拟车已停止"));
    }
    setRunningUi(m_running);
}

void SimulatorWindow::setRunningUi(bool running)
{
    m_runningButton->setText(running ? tr("停止模拟车") : tr("启动模拟车"));
    m_serverEdit->setEnabled(!running);
    m_portEdit->setEnabled(!running);
    m_teamEdit->setEnabled(!running);
    m_robotIdEdit->setEnabled(!running);
    m_heatEdit->setEnabled(!running);
}

void SimulatorWindow::sendStatus()
{
    const QString addressText = m_serverEdit->text().trimmed();
    QHostAddress address(addressText);
    if (address.isNull()) {
        appendLog(tr("服务器地址无效: %1").arg(addressText));
        return;
    }

        sendDatagram(proto::makeStatusDatagramWithHeat(robotId(), team(),
                                                       static_cast<quint16>(m_hpEdit->value()),
                                                       static_cast<quint16>(m_heatEdit->value()),
                                                       m_aliveEdit->isChecked(),
                                                       m_shootEdit->isChecked()),
                 tr("状态"));
}

void SimulatorWindow::sendDatagram(const QByteArray &data, const QString &description)
{
    QHostAddress address(m_serverEdit->text().trimmed());
    if (address.isNull())
        return;

    const qint64 result = m_socket->writeDatagram(data, address,
                                                  static_cast<quint16>(m_portEdit->value()));
    if (result < 0) {
        appendLog(tr("发送失败 [%1]: %2").arg(description, m_socket->errorString()));
    } else if (description != tr("状态")) {
        appendLog(tr("已发送 [%1]，%2 字节").arg(description).arg(data.size()));
    }
}

void SimulatorWindow::onSendAttack()
{
    sendDatagram(eventFrame(proto::TypeAttack), tr("攻击"));
}

void SimulatorWindow::onSendHit()
{
    const int nextHp = qMax(0, m_hpEdit->value() - 500);
    m_hpEdit->setValue(nextHp);
    sendDatagram(proto::makeHitDatagram(robotId(), team(), static_cast<quint16>(nextHp)), tr("受击"));
    if (m_running)
        sendStatus();
}

void SimulatorWindow::onSendDeath()
{
    m_aliveEdit->setChecked(false);
    m_shootEdit->setChecked(false);
    sendDatagram(eventFrame(proto::TypeDeath), tr("死亡"));
    if (m_running)
        sendStatus();
}

void SimulatorWindow::onSendRevive()
{
    m_aliveEdit->setChecked(true);
    m_shootEdit->setChecked(true);
    m_hpEdit->setValue(kDefaultHp);
    sendDatagram(eventFrame(proto::TypeRevive), tr("复活"));
    if (m_running)
        sendStatus();
}

void SimulatorWindow::onSendShootEnabled()
{
    m_shootEdit->setChecked(true);
    sendDatagram(eventFrame(proto::TypeShootEnabled), tr("允许射击"));
    if (m_running)
        sendStatus();
}

void SimulatorWindow::onSendShootDisabled()
{
    m_shootEdit->setChecked(false);
    sendDatagram(eventFrame(proto::TypeShootDisabled), tr("禁止射击"));
    if (m_running)
        sendStatus();
}

void SimulatorWindow::onAutoDemoToggled(bool enabled)
{
    if (enabled && m_running) {
        m_demoStep = 0;
        m_demoTimer->start();
        appendLog(tr("自动演示已启动"));
    } else {
        m_demoTimer->stop();
        if (!enabled)
            appendLog(tr("自动演示已停止"));
    }
}

void SimulatorWindow::advanceDemo()
{
    if (!m_running)
        return;

    switch (m_demoStep++ % 6) {
    case 0:
        onSendAttack();
        break;
    case 1:
        onSendHit();
        break;
    case 2:
        onSendShootDisabled();
        break;
    case 3:
        onSendDeath();
        break;
    case 4:
        onSendRevive();
        break;
    case 5:
        onSendShootEnabled();
        break;
    }
}

void SimulatorWindow::appendLog(const QString &message)
{
    if (m_log)
        m_log->appendPlainText(message);
}
