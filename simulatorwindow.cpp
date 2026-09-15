#include "simulatorwindow.h"

#include "protocol.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFormLayout>
#include <QFont>
#include <QGroupBox>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkDatagram>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QUdpSocket>
#include <QVBoxLayout>

namespace {
constexpr int kDefaultHp = 300;      // V2 默认开局 HP
constexpr int kStatusIntervalMs = 100;
constexpr int kDemoIntervalMs = 1800;
constexpr int kReliableRetryMs = 100;
constexpr int kMaxReliableAttempts = 3;
}

SimulatorWindow::SimulatorWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("模拟车载端 (V1+V2)"));
    resize(760, 720);

    m_uplink = new QUdpSocket(this);
    m_downlink = new QUdpSocket(this);
    connect(m_downlink, &QUdpSocket::readyRead, this,
            &SimulatorWindow::onDownlinkReadyRead);

    m_statusTimer = new QTimer(this);
    m_statusTimer->setInterval(kStatusIntervalMs);
    connect(m_statusTimer, &QTimer::timeout, this, &SimulatorWindow::sendStatus);

    m_demoTimer = new QTimer(this);
    m_demoTimer->setInterval(kDemoIntervalMs);
    connect(m_demoTimer, &QTimer::timeout, this, &SimulatorWindow::advanceDemo);

    m_retryTimer = new QTimer(this);
    m_retryTimer->setInterval(50);
    connect(m_retryTimer, &QTimer::timeout, this,
            &SimulatorWindow::retryReliableEvents);
    m_retryTimer->start();

    buildUi();
    setRunningUi(false);
}

void SimulatorWindow::buildUi()
{
    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(16, 14, 16, 14);

    auto *title = new QLabel(tr("模拟车载端 · ESP32 V1/V2 UDP 协议测试器"), central);
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
    m_portEdit->setValue(proto::kUplinkPort);
    m_versionEdit = new QComboBox(networkGroup);
    m_versionEdit->addItem(tr("V1（旧协议，回归测试）"), proto::kVersion1);
    m_versionEdit->addItem(tr("V2（正式协议）"), proto::kVersion2);
    m_versionEdit->setCurrentIndex(1);
    networkForm->addRow(tr("服务器地址"), m_serverEdit);
    networkForm->addRow(tr("UDP 端口"), m_portEdit);
    networkForm->addRow(tr("协议版本"), m_versionEdit);
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
    m_powerEdit = new QSpinBox(robotGroup);
    m_powerEdit->setRange(0, 500);
    m_powerEdit->setValue(0);
    m_powerEdit->setSuffix(tr(" W"));
    m_aliveEdit = new QCheckBox(tr("存活"), robotGroup);
    m_aliveEdit->setChecked(true);
    m_shootEdit = new QCheckBox(tr("允许射击"), robotGroup);
    m_shootEdit->setChecked(true);
    m_powerOnEdit = new QCheckBox(tr("底盘已通电"), robotGroup);
    m_powerOnEdit->setChecked(true);
    robotForm->addRow(tr("队伍"), m_teamEdit);
    robotForm->addRow(tr("机器人 ID"), m_robotIdEdit);
    robotForm->addRow(tr("当前 HP"), m_hpEdit);
    robotForm->addRow(tr("当前热量"), m_heatEdit);
    robotForm->addRow(tr("当前功率"), m_powerEdit);
    robotForm->addRow(QString(), m_aliveEdit);
    robotForm->addRow(QString(), m_shootEdit);
    robotForm->addRow(QString(), m_powerOnEdit);
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
    addEventButton(tr("链路断开"), &SimulatorWindow::onSendLinkDown);
    addEventButton(tr("链路恢复"), &SimulatorWindow::onSendLinkUp);
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

quint8 SimulatorWindow::protocolVersion() const
{
    return static_cast<quint8>(m_versionEdit->currentData().toInt());
}

quint32 SimulatorWindow::allocateTxid()
{
    const quint32 id = m_nextTxid;
    m_nextTxid = (m_nextTxid + 1 == 0) ? 1 : m_nextTxid + 1;
    return id;
}

QByteArray SimulatorWindow::statusFrame() const
{
    if (protocolVersion() == proto::kVersion1) {
        return proto::makeStatusV1WithHeat(
            robotId(), team(),
            static_cast<quint16>(m_hpEdit->value()),
            static_cast<quint16>(m_heatEdit->value()),
            m_aliveEdit->isChecked(),
            m_shootEdit->isChecked());
    }
    return proto::makeStatusV2(
        robotId(),
        static_cast<quint16>(m_hpEdit->value()),
        static_cast<quint16>(m_heatEdit->value()),
        static_cast<quint16>(m_powerEdit->value()),
        m_aliveEdit->isChecked(),
        m_shootEdit->isChecked(),
        m_powerOnEdit->isChecked());
}

QByteArray SimulatorWindow::eventFrameV2(quint8 type) const
{
    return proto::makeEventV2(type, robotId());
}

QByteArray SimulatorWindow::reliableEventFrameV2(quint8 type, quint32 txid) const
{
    return proto::makeReliableEventV2(type, robotId(), txid);
}

QByteArray SimulatorWindow::eventFrameV1(quint8 type) const
{
    return proto::makeEventV1(type, robotId(), team());
}

void SimulatorWindow::queueReliableEvent(quint8 type)
{
    const quint32 txid = allocateTxid();
    PendingEvent ev;
    ev.type = type;
    ev.txid = txid;
    ev.attempts = 1;
    ev.deadlineMs = QDateTime::currentMSecsSinceEpoch() + kReliableRetryMs;
    m_pendingEvents.insert(txid, ev);
    sendDatagram(reliableEventFrameV2(type, txid),
                 QStringLiteral("%1 (txid=%2)").arg(
                     type == proto::TypeDeath ? tr("死亡") : tr("复活"))
                     .arg(txid));
}

void SimulatorWindow::onToggleRunning()
{
    m_running = !m_running;
    if (m_running) {
        // 绑定 5006 接收下行命令（仅 V2）
        if (protocolVersion() == proto::kVersion2) {
            if (!m_downlink->bind(QHostAddress::AnyIPv4, proto::kDownlinkPort,
                                  QUdpSocket::ShareAddress)) {
                appendLog(tr("无法绑定下行端口 %1：%2")
                              .arg(proto::kDownlinkPort)
                              .arg(m_downlink->errorString()));
            } else {
                appendLog(tr("已监听下行命令 UDP %1").arg(proto::kDownlinkPort));
            }
        }
        m_statusTimer->start();
        sendStatus();
        appendLog(tr("模拟车已启动：%1·机器人%2 (V%3) -> %4:%5")
                      .arg(m_teamEdit->currentText())
                      .arg(robotId())
                      .arg(protocolVersion())
                      .arg(m_serverEdit->text())
                      .arg(m_portEdit->value()));
    } else {
        m_statusTimer->stop();
        m_demoTimer->stop();
        m_downlink->close();
        m_pendingEvents.clear();
        appendLog(tr("模拟车已停止"));
    }
    setRunningUi(m_running);
}

void SimulatorWindow::setRunningUi(bool running)
{
    m_runningButton->setText(running ? tr("停止模拟车") : tr("启动模拟车"));
    m_serverEdit->setEnabled(!running);
    m_portEdit->setEnabled(!running);
    m_versionEdit->setEnabled(!running);
    m_teamEdit->setEnabled(!running);
    m_robotIdEdit->setEnabled(!running);
    m_heatEdit->setEnabled(!running);
    m_powerEdit->setEnabled(!running);
    m_powerOnEdit->setEnabled(!running);
}

void SimulatorWindow::sendStatus()
{
    const QString addressText = m_serverEdit->text().trimmed();
    QHostAddress address(addressText);
    if (address.isNull()) {
        appendLog(tr("服务器地址无效: %1").arg(addressText));
        return;
    }
    sendDatagram(statusFrame(), tr("状态"));
}

void SimulatorWindow::sendDatagram(const QByteArray &data, const QString &description)
{
    QHostAddress address(m_serverEdit->text().trimmed());
    if (address.isNull())
        return;

    const qint64 result = m_uplink->writeDatagram(
        data, address, static_cast<quint16>(m_portEdit->value()));
    if (result < 0) {
        appendLog(tr("发送失败 [%1]: %2").arg(description, m_uplink->errorString()));
    } else if (description != tr("状态")) {
        appendLog(tr("已发送 [%1]，%2 字节").arg(description).arg(data.size()));
    }
}

void SimulatorWindow::sendAck(const QHostAddress &addr, quint16 port,
                              quint8 ackedType, quint32 txid, quint8 result)
{
    const QByteArray frame = proto::makeAck(ackedType, txid, result);
    m_uplink->writeDatagram(frame, addr, port);
}

void SimulatorWindow::onSendAttack()
{
    if (protocolVersion() == proto::kVersion1)
        sendDatagram(eventFrameV1(proto::TypeAttack), tr("攻击"));
    else
        sendDatagram(eventFrameV2(proto::TypeAttack), tr("攻击"));
}

void SimulatorWindow::onSendHit()
{
    const int nextHp = qMax(0, m_hpEdit->value() - 500);
    m_hpEdit->setValue(nextHp);
    if (protocolVersion() == proto::kVersion1)
        sendDatagram(proto::makeHitV1(robotId(), team(), static_cast<quint16>(nextHp)),
                     tr("受击"));
    else
        sendDatagram(eventFrameV2(proto::TypeHit), tr("受击"));
    if (m_running)
        sendStatus();
}

void SimulatorWindow::onSendDeath()
{
    m_aliveEdit->setChecked(false);
    m_shootEdit->setChecked(false);
    if (protocolVersion() == proto::kVersion1)
        sendDatagram(eventFrameV1(proto::TypeDeath), tr("死亡"));
    else
        queueReliableEvent(proto::TypeDeath);
    if (m_running)
        sendStatus();
}

void SimulatorWindow::onSendRevive()
{
    m_aliveEdit->setChecked(true);
    m_shootEdit->setChecked(true);
    m_hpEdit->setValue(kDefaultHp);
    if (protocolVersion() == proto::kVersion1)
        sendDatagram(eventFrameV1(proto::TypeRevive), tr("复活"));
    else
        queueReliableEvent(proto::TypeRevive);
    if (m_running)
        sendStatus();
}

void SimulatorWindow::onSendShootEnabled()
{
    m_shootEdit->setChecked(true);
    if (protocolVersion() == proto::kVersion1)
        sendDatagram(eventFrameV1(proto::TypeShootEnabled), tr("允许射击"));
    else
        sendDatagram(eventFrameV2(proto::TypeShootEnabled), tr("允许射击"));
    if (m_running)
        sendStatus();
}

void SimulatorWindow::onSendShootDisabled()
{
    m_shootEdit->setChecked(false);
    if (protocolVersion() == proto::kVersion1)
        sendDatagram(eventFrameV1(proto::TypeShootDisabled), tr("禁止射击"));
    else
        sendDatagram(eventFrameV2(proto::TypeShootDisabled), tr("禁止射击"));
    if (m_running)
        sendStatus();
}

void SimulatorWindow::onSendLinkDown()
{
    if (protocolVersion() == proto::kVersion2)
        sendDatagram(eventFrameV2(proto::TypeLinkDown), tr("链路断开"));
}

void SimulatorWindow::onSendLinkUp()
{
    if (protocolVersion() == proto::kVersion2)
        sendDatagram(eventFrameV2(proto::TypeLinkUp), tr("链路恢复"));
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
    case 0: onSendAttack(); break;
    case 1: onSendHit(); break;
    case 2: onSendShootDisabled(); break;
    case 3: onSendDeath(); break;
    case 4: onSendRevive(); break;
    case 5: onSendShootEnabled(); break;
    }
}

void SimulatorWindow::onDownlinkReadyRead()
{
    while (m_downlink->hasPendingDatagrams()) {
        const QNetworkDatagram dg = m_downlink->receiveDatagram();
        if (dg.isValid())
            handleDownlink(dg.data(), dg.senderAddress(), dg.senderPort());
    }
}

void SimulatorWindow::handleDownlink(const QByteArray &data,
                                     const QHostAddress &addr, quint16 port)
{
    if (data.size() < 4 || data[0] != 0x54 || data[1] != 0x52)
        return;
    const quint8 version = static_cast<quint8>(data[2]);
    const quint8 type = static_cast<quint8>(data[3]);
    if (version != proto::kVersion2) {
        appendLog(tr("[下行] 收到 V%1 包，仅支持 V2").arg(version));
        return;
    }

    switch (type) {
    case proto::TypeAck: {
        // 服务器回 ACK 给我们的可靠事件
        if (data.size() != 10) return;
        const quint8 ackedType = static_cast<quint8>(data[4]);
        const quint32 txid = proto::readU32(data.constData() + 5);
        const quint8 result = static_cast<quint8>(data[9]);
        auto it = m_pendingEvents.find(txid);
        if (it != m_pendingEvents.end()) {
            m_pendingEvents.erase(it);
            appendLog(tr("[ACK] 服务器已确认 %1 txid=%2 result=%3")
                          .arg(ackedType == proto::TypeDeath ? tr("死亡") : tr("复活"))
                          .arg(txid)
                          .arg(result));
        }
        return;
    }
    case proto::TypeGameStart:
        if (data.size() != 9) return;
        m_hpEdit->setValue(300);
        m_aliveEdit->setChecked(true);
        m_shootEdit->setChecked(true);
        sendAck(addr, port, type, proto::readU32(data.constData() + 5), 0);
        appendLog(tr("[下行] 收到 GAME_START txid=%1，HP->300")
                      .arg(proto::readU32(data.constData() + 5)));
        return;
    case proto::TypeGameEnd:
        if (data.size() != 9) return;
        sendAck(addr, port, type, proto::readU32(data.constData() + 5), 0);
        appendLog(tr("[下行] 收到 GAME_END txid=%1")
                      .arg(proto::readU32(data.constData() + 5)));
        return;
    case proto::TypeSetHp:
        if (data.size() != 11) return;
        {
            const quint16 hp = proto::readU16(data.constData() + 5);
            const quint32 txid = proto::readU32(data.constData() + 7);
            m_hpEdit->setValue(hp);
            sendAck(addr, port, type, txid, 0);
            appendLog(tr("[下行] 收到 SET_HP=%1 txid=%2").arg(hp).arg(txid));
        }
        return;
    case proto::TypeYellowCard:
        if (data.size() != 9) return;
        sendAck(addr, port, type, proto::readU32(data.constData() + 5), 0);
        appendLog(tr("[下行] 收到 YELLOW_CARD txid=%1")
                      .arg(proto::readU32(data.constData() + 5)));
        return;
    case proto::TypeForcePowerOff:
        if (data.size() != 9) return;
        m_powerOnEdit->setChecked(false);
        sendAck(addr, port, type, proto::readU32(data.constData() + 5), 0);
        appendLog(tr("[下行] 收到 FORCE_POWER_OFF txid=%1")
                      .arg(proto::readU32(data.constData() + 5)));
        return;
    case proto::TypeForcePowerOn:
        if (data.size() != 9) return;
        m_powerOnEdit->setChecked(true);
        sendAck(addr, port, type, proto::readU32(data.constData() + 5), 0);
        appendLog(tr("[下行] 收到 FORCE_POWER_ON txid=%1")
                      .arg(proto::readU32(data.constData() + 5)));
        return;
    case proto::TypeStatusRequest:
        if (data.size() != 5) return;
        sendStatus();
        appendLog(tr("[下行] 收到 STATUS_REQUEST，已补发状态"));
        return;
    case proto::TypeAssignment:
        if (data.size() != 15) return;
        sendAck(addr, port, type, proto::readU32(data.constData() + 11), 0);
        appendLog(tr("[下行] 收到 ASSIGNMENT txid=%1")
                      .arg(proto::readU32(data.constData() + 11)));
        return;
    default:
        appendLog(tr("[下行] 未知类型 0x%1，%2 字节")
                      .arg(type, 2, 16).arg(data.size()));
    }
}

void SimulatorWindow::retryReliableEvents()
{
    if (protocolVersion() != proto::kVersion2)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<quint32> toRemove;
    for (auto it = m_pendingEvents.begin(); it != m_pendingEvents.end(); ++it) {
        PendingEvent &ev = it.value();
        if (now < ev.deadlineMs)
            continue;
        if (ev.attempts >= kMaxReliableAttempts) {
            toRemove.append(it.key());
            appendLog(tr("[超时] 可靠事件 type=0x%1 txid=%2 未获服务器 ACK")
                          .arg(ev.type, 2, 16).arg(ev.txid));
            continue;
        }
        sendDatagram(reliableEventFrameV2(ev.type, ev.txid),
                     QStringLiteral("重传 %1 txid=%2 第%3次")
                         .arg(ev.type == proto::TypeDeath ? tr("死亡") : tr("复活"))
                         .arg(ev.txid).arg(ev.attempts + 1));
        ++ev.attempts;
        ev.deadlineMs = now + kReliableRetryMs;
    }
    for (quint32 key : toRemove)
        m_pendingEvents.remove(key);
}

void SimulatorWindow::appendLog(const QString &message)
{
    if (m_log)
        m_log->appendPlainText(QStringLiteral("[%1] %2")
                                   .arg(QDateTime::currentDateTime().toString("HH:mm:ss.zzz"))
                                   .arg(message));
}
