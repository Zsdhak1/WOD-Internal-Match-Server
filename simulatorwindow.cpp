#include "simulatorwindow.h"

#include "protocol.h"
#include "responsiveui.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFormLayout>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkDatagram>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QRandomGenerator>
#include <QSpinBox>
#include <QTimer>
#include <QUdpSocket>
#include <QVBoxLayout>

namespace {
constexpr int kDefaultHp = 300;      // V2 默认开局 HP
constexpr int kPreparationHp = 200;
constexpr int kStatusIntervalMs = 100;
constexpr int kAnnounceIntervalMs = 2000;
constexpr int kDemoIntervalMs = 1800;
constexpr int kReliableRetryMs = 100;
constexpr int kMaxReliableAttempts = 3;
}

SimulatorWindow::SimulatorWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("模拟车载端 (V1+V2)"));

    m_uplink = new QUdpSocket(this);
    m_downlink = new QUdpSocket(this);
    connect(m_downlink, &QUdpSocket::readyRead, this,
            &SimulatorWindow::onDownlinkReadyRead);
    // Reliable-event ACKs return to the source port of the uplink socket.
    connect(m_uplink, &QUdpSocket::readyRead, this,
            &SimulatorWindow::onDownlinkReadyRead);

    m_statusTimer = new QTimer(this);
    m_statusTimer->setInterval(kStatusIntervalMs);
    connect(m_statusTimer, &QTimer::timeout, this, &SimulatorWindow::sendStatus);
    m_announceTimer = new QTimer(this);
    m_announceTimer->setInterval(kAnnounceIntervalMs);
    connect(m_announceTimer, &QTimer::timeout, this, &SimulatorWindow::sendDeviceAnnounce);

    m_demoTimer = new QTimer(this);
    m_demoTimer->setInterval(kDemoIntervalMs);
    connect(m_demoTimer, &QTimer::timeout, this, &SimulatorWindow::advanceDemo);

    m_retryTimer = new QTimer(this);
    m_retryTimer->setInterval(50);
    connect(m_retryTimer, &QTimer::timeout, this,
            &SimulatorWindow::retryReliableEvents);
    m_retryTimer->start();

    buildUi();
    loadDeviceIdentity();
    m_uiScaler = new ResponsiveUiScaler;
    m_uiScaler->capture(this, QSize(760, 720), 0.55, 2.5);

    const QRect available = screen() ? screen()->availableGeometry()
                                     : QRect(0, 0, 1280, 800);
    const QSize proportional(qMax(1, qRound(available.width() * 0.40)),
                             qMax(1, qRound(available.height() * 0.78)));
    const QSize initialSize = responsiveWindowSizeFromEnvironment(
        "SCOMP_SIMULATOR_SIZE",
        QSize(qMin(proportional.width(), available.width()),
              qMin(proportional.height(), available.height())));
    resize(initialSize);
    applyResponsiveUi();
    setRunningUi(false);
}

SimulatorWindow::~SimulatorWindow()
{
    delete m_uiScaler;
    m_uiScaler = nullptr;
}

void SimulatorWindow::buildUi()
{
    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(16, 14, 16, 14);

    auto *controlsScroll = new QScrollArea(central);
    controlsScroll->setWidgetResizable(true);
    controlsScroll->setFrameShape(QFrame::NoFrame);
    controlsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *controls = new QWidget(controlsScroll);
    auto *controlsLayout = new QVBoxLayout(controls);
    controlsLayout->setContentsMargins(0, 0, 0, 0);

    auto *title = new QLabel(tr("模拟车载端 · ESP32 V1/V2 UDP 协议测试器"), controls);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSize(17);
    title->setFont(titleFont);
    controlsLayout->addWidget(title);

    auto *networkGroup = new QGroupBox(tr("连接参数"), controls);
    auto *networkForm = new QFormLayout(networkGroup);
    networkForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    m_serverEdit = new QLineEdit(QStringLiteral("127.0.0.1"), networkGroup);
    m_serverEdit->setObjectName(QStringLiteral("simServerAddress"));
    m_localAddressEdit = new QLineEdit(QStringLiteral("0.0.0.0"), networkGroup);
    m_localAddressEdit->setObjectName(QStringLiteral("simLocalAddress"));
    m_localAddressEdit->setToolTip(tr("默认监听所有网卡。同机多车分别填写 127.0.0.2、127.0.0.3；每个地址保存独立设备身份。"));
    connect(m_localAddressEdit, &QLineEdit::editingFinished, this, &SimulatorWindow::loadDeviceIdentity);
    m_portEdit = new QSpinBox(networkGroup);
    m_portEdit->setRange(1, 65535);
    m_portEdit->setValue(proto::kUplinkPort);
    m_portEdit->setObjectName(QStringLiteral("simServerPort"));
    m_versionEdit = new QComboBox(networkGroup);
    m_versionEdit->addItem(tr("V1（旧协议，回归测试）"), proto::kVersion1);
    m_versionEdit->addItem(tr("V2（正式协议）"), proto::kVersion2);
    m_versionEdit->setCurrentIndex(1);
    m_versionEdit->setObjectName(QStringLiteral("simProtocolVersion"));
    connect(m_versionEdit, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        const bool v2 = protocolVersion() == proto::kVersion2;
        m_robotIdEdit->setMinimum(v2 ? 0 : 1);
        m_robotIdEdit->setValue(v2 ? m_assignedId : qMax(1, m_robotIdEdit->value()));
        m_formalMode = false;
        m_roundEnded = false;
        m_hpEdit->setMaximum(v2 ? kPreparationHp : 5000);
        m_hpEdit->setValue(v2 ? kPreparationHp : 5000);
        m_teamEdit->setCurrentIndex(v2 ? 0 : 1);
        setRunningUi(m_running);
    });
    networkForm->addRow(tr("服务器地址"), m_serverEdit);
    networkForm->addRow(tr("本机绑定 IPv4"), m_localAddressEdit);
    networkForm->addRow(tr("UDP 端口"), m_portEdit);
    networkForm->addRow(tr("协议版本"), m_versionEdit);
    controlsLayout->addWidget(networkGroup);

    auto *robotGroup = new QGroupBox(tr("机器人身份与状态"), controls);
    auto *robotForm = new QFormLayout(robotGroup);
    robotForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    m_teamEdit = new QComboBox(robotGroup);
    m_teamEdit->setObjectName(QStringLiteral("simTeam"));
    m_teamEdit->addItem(tr("V2：在服务端分配红/蓝方"), 0);
    m_teamEdit->addItem(tr("红方"), 1);
    m_teamEdit->addItem(tr("蓝方"), 2);
    m_robotIdEdit = new QSpinBox(robotGroup);
    m_robotIdEdit->setObjectName(QStringLiteral("simRobotId"));
    m_robotIdEdit->setRange(0, 255);
    m_robotIdEdit->setSpecialValueText(tr("未分配"));
    m_deviceMacDisplay = new QLineEdit(robotGroup);
    m_deviceMacDisplay->setObjectName(QStringLiteral("simDeviceMac"));
    m_deviceMacDisplay->setReadOnly(true);
    m_controllerMacDisplay = new QLineEdit(robotGroup);
    m_controllerMacDisplay->setObjectName(QStringLiteral("simControllerMac"));
    m_controllerMacDisplay->setReadOnly(true);
    m_controllerMacDisplay->setPlaceholderText(tr("等待服务端分配手柄 MAC"));
    m_hpEdit = new QSpinBox(robotGroup);
    m_hpEdit->setObjectName(QStringLiteral("simHp"));
    m_hpEdit->setRange(0, kPreparationHp);
    m_hpEdit->setValue(kPreparationHp);
    m_heatEdit = new QSpinBox(robotGroup);
    m_heatEdit->setObjectName(QStringLiteral("simHeat"));
    m_heatEdit->setRange(0, 1000);
    m_heatEdit->setValue(0);
    m_powerEdit = new QSpinBox(robotGroup);
    m_powerEdit->setObjectName(QStringLiteral("simPower"));
    m_powerEdit->setRange(0, 500);
    m_powerEdit->setValue(0);
    m_powerEdit->setSuffix(tr(" W"));
    m_aliveEdit = new QCheckBox(tr("存活"), robotGroup);
    m_aliveEdit->setObjectName(QStringLiteral("simAlive"));
    m_aliveEdit->setChecked(true);
    m_shootEdit = new QCheckBox(tr("允许射击"), robotGroup);
    m_shootEdit->setObjectName(QStringLiteral("simShoot"));
    m_shootEdit->setChecked(true);
    m_powerOnEdit = new QCheckBox(tr("底盘已通电"), robotGroup);
    m_powerOnEdit->setObjectName(QStringLiteral("simPowerOn"));
    m_powerOnEdit->setChecked(true);
    robotForm->addRow(tr("队伍"), m_teamEdit);
    robotForm->addRow(tr("机器人 ID"), m_robotIdEdit);
    robotForm->addRow(tr("模拟 ESP32 MAC"), m_deviceMacDisplay);
    robotForm->addRow(tr("已绑定手柄 MAC"), m_controllerMacDisplay);
    robotForm->addRow(tr("当前 HP"), m_hpEdit);
    robotForm->addRow(tr("当前热量"), m_heatEdit);
    robotForm->addRow(tr("当前功率"), m_powerEdit);
    robotForm->addRow(QString(), m_aliveEdit);
    robotForm->addRow(QString(), m_shootEdit);
    robotForm->addRow(QString(), m_powerOnEdit);
    controlsLayout->addWidget(robotGroup);

    auto *controlRow = new QHBoxLayout;
    m_runningButton = new QPushButton(tr("启动模拟车"), controls);
    m_runningButton->setObjectName(QStringLiteral("simRunning"));
    connect(m_runningButton, &QPushButton::clicked, this, &SimulatorWindow::onToggleRunning);
    auto *autoDemo = new QCheckBox(tr("自动演示攻击/受击/死亡/复活"), controls);
    m_autoDemo = autoDemo;
    connect(autoDemo, &QCheckBox::toggled, this, &SimulatorWindow::onAutoDemoToggled);
    controlRow->addWidget(m_runningButton);
    controlRow->addWidget(autoDemo);
    controlRow->addStretch(1);
    controlsLayout->addLayout(controlRow);

    auto *events = new QGroupBox(tr("一次性事件"), controls);
    m_eventsGroup = events;
    auto *eventLayout = new QGridLayout(events);
    int eventIndex = 0;
    const auto addEventButton = [this, eventLayout, events, &eventIndex](
                                    const QString &text, auto slot) {
        auto *button = new QPushButton(text, events);
        connect(button, &QPushButton::clicked, this, slot);
        eventLayout->addWidget(button, eventIndex / 4, eventIndex % 4);
        ++eventIndex;
    };
    addEventButton(tr("攻击"), &SimulatorWindow::onSendAttack);
    addEventButton(tr("受击 -20"), &SimulatorWindow::onSendHit);
    addEventButton(tr("死亡"), &SimulatorWindow::onSendDeath);
    addEventButton(tr("复活"), &SimulatorWindow::onSendRevive);
    addEventButton(tr("允许射击"), &SimulatorWindow::onSendShootEnabled);
    addEventButton(tr("禁止射击"), &SimulatorWindow::onSendShootDisabled);
    addEventButton(tr("链路断开"), &SimulatorWindow::onSendLinkDown);
    addEventButton(tr("链路恢复"), &SimulatorWindow::onSendLinkUp);
    for (int column = 0; column < 4; ++column)
        eventLayout->setColumnStretch(column, 1);
    controlsLayout->addWidget(events);
    controlsLayout->addStretch(1);
    controlsScroll->setWidget(controls);
    root->addWidget(controlsScroll, 3);

    auto *logTitle = new QLabel(tr("发送日志"), central);
    QFont logFont = logTitle->font();
    logFont.setBold(true);
    logTitle->setFont(logFont);
    root->addWidget(logTitle);
    m_log = new QPlainTextEdit(central);
    m_log->setObjectName(QStringLiteral("simLog"));
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(1000);
    root->addWidget(m_log, 2);

    setCentralWidget(central);
}

void SimulatorWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    applyResponsiveUi();
}

void SimulatorWindow::applyResponsiveUi()
{
    if (m_uiScaler)
        m_uiScale = m_uiScaler->apply(size());
}

quint8 SimulatorWindow::team() const
{
    return static_cast<quint8>(m_teamEdit->currentData().toInt());
}

void SimulatorWindow::loadDeviceIdentity()
{
    if (m_running)
        return;
    const QHostAddress local(m_localAddressEdit->text().trimmed());
    if (local.protocol() != QAbstractSocket::IPv4Protocol)
        return;
    m_identityProfile = local.toString();
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope,
                       QStringLiteral("Scompetition"), QStringLiteral("RobotSimulator"));
    settings.beginGroup(m_identityProfile);
    m_deviceMac = settings.value(QStringLiteral("deviceMac")).toByteArray();
    if (m_deviceMac.size() != 6 || m_deviceMac == QByteArray(6, char(0))) {
        m_deviceMac.resize(6);
        m_deviceMac[0] = char(0x02); // Locally administered unicast MAC.
        for (int i = 1; i < 6; ++i)
            m_deviceMac[i] = char(QRandomGenerator::global()->bounded(256));
        settings.setValue(QStringLiteral("deviceMac"), m_deviceMac);
    }
    const int storedId = settings.value(QStringLiteral("robotId"), 0).toInt();
    m_controllerMac = settings.value(QStringLiteral("controllerMac")).toByteArray();
    m_assignedId = storedId > 0 && storedId <= 255 && m_controllerMac.size() == 6
                           && m_controllerMac != QByteArray(6, char(0)) ? quint8(storedId) : 0;
    if (!m_assignedId)
        m_controllerMac = QByteArray(6, char(0));
    m_deviceMacDisplay->setText(QString::fromLatin1(m_deviceMac.toHex(':').toUpper()));
    m_controllerMacDisplay->setText(m_assignedId
        ? QString::fromLatin1(m_controllerMac.toHex(':').toUpper()) : QString());
    if (protocolVersion() == proto::kVersion2)
        m_robotIdEdit->setValue(m_assignedId);
    m_completedCommands.clear();
    m_completedCommandOrder.clear();
    // Avoid reusing the server's persisted reliable-event transaction IDs.
    m_nextTxid = QRandomGenerator::global()->generate() | 1u;
}

bool SimulatorWindow::saveDeviceAssignment(quint8 id, const QByteArray &controller)
{
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope,
                       QStringLiteral("Scompetition"), QStringLiteral("RobotSimulator"));
    settings.beginGroup(m_identityProfile);
    settings.setValue(QStringLiteral("robotId"), id);
    settings.setValue(QStringLiteral("controllerMac"), controller);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        appendLog(tr("分配失败：无法保存模拟设备身份"));
        return false;
    }
    if (m_assignedId != id)
        m_pendingEvents.clear();
    m_assignedId = id;
    m_controllerMac = controller;
    m_robotIdEdit->setValue(id);
    m_controllerMacDisplay->setText(QString::fromLatin1(controller.toHex(':').toUpper()));
    setRunningUi(m_running);
    return true;
}

bool SimulatorWindow::canSendEvents() const
{
    return m_running && (protocolVersion() == proto::kVersion1 || m_assignedId != 0);
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
    if (!canSendEvents())
        return;
    const quint32 txid = allocateTxid();
    PendingEvent ev;
    ev.type = type;
    ev.txid = txid;
    ev.attempts = 1;
    ev.deadlineMs = QDateTime::currentMSecsSinceEpoch() + kReliableRetryMs;
    ev.datagram = reliableEventFrameV2(type, txid);
    m_pendingEvents.insert(txid, ev);
    sendDatagram(ev.datagram,
                 QStringLiteral("%1 (txid=%2)").arg(
                     type == proto::TypeDeath ? tr("死亡") : tr("复活"))
                     .arg(txid));
}

void SimulatorWindow::onToggleRunning()
{
    if (!m_running) {
        const QHostAddress server(m_serverEdit->text().trimmed());
        const QHostAddress local(m_localAddressEdit->text().trimmed());
        if (server.protocol() != QAbstractSocket::IPv4Protocol || server.isNull()
            || local.protocol() != QAbstractSocket::IPv4Protocol) {
            appendLog(tr("启动失败：服务器和本机地址必须是有效 IPv4 地址"));
            return;
        }
        loadDeviceIdentity();
        if (!m_uplink->bind(local, quint16(0), QUdpSocket::DontShareAddress)) {
            appendLog(tr("无法绑定本机发送地址：%1").arg(m_uplink->errorString()));
            return;
        }
        // 绑定 5006 接收下行命令（仅 V2）
        if (protocolVersion() == proto::kVersion2) {
            if (!m_downlink->bind(local, proto::kDownlinkPort,
                                  QUdpSocket::DontShareAddress)) {
                appendLog(tr("无法绑定下行端口 %1：%2")
                              .arg(proto::kDownlinkPort)
                              .arg(m_downlink->errorString()));
                m_uplink->close();
                m_downlink->close();
                return;
            } else {
                appendLog(tr("已监听下行命令 UDP %1").arg(proto::kDownlinkPort));
            }
        }
        m_running = true;
        if (protocolVersion() == proto::kVersion2) {
            sendDeviceAnnounce();
            m_announceTimer->start();
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
        m_running = false;
        m_statusTimer->stop();
        m_announceTimer->stop();
        m_demoTimer->stop();
        m_autoDemo->setChecked(false);
        m_downlink->close();
        m_uplink->close();
        m_pendingEvents.clear();
        appendLog(tr("模拟车已停止"));
    }
    setRunningUi(m_running);
}

void SimulatorWindow::setRunningUi(bool running)
{
    m_runningButton->setText(running ? tr("停止模拟车") : tr("启动模拟车"));
    m_serverEdit->setEnabled(!running);
    m_localAddressEdit->setEnabled(!running);
    m_portEdit->setEnabled(!running);
    m_versionEdit->setEnabled(!running);
    const bool v1 = protocolVersion() == proto::kVersion1;
    m_teamEdit->setEnabled(!running && v1);
    m_robotIdEdit->setEnabled(!running && v1);
    m_eventsGroup->setEnabled(canSendEvents());
    m_autoDemo->setEnabled(canSendEvents());
}

void SimulatorWindow::sendDeviceAnnounce()
{
    if (!m_running || protocolVersion() != proto::kVersion2)
        return;
    sendDatagram(proto::makeDeviceAnnounce(m_assignedId, m_deviceMac, m_controllerMac,
                                           0, m_udpSendFailures), tr("设备注册"));
}

void SimulatorWindow::sendStatus()
{
    if (!canSendEvents())
        return;
    const QString addressText = m_serverEdit->text().trimmed();
    QHostAddress address(addressText);
    if (address.isNull()) {
        appendLog(tr("服务器地址无效: %1").arg(addressText));
        return;
    }
    sendDatagram(statusFrame(), tr("状态"));
    if (protocolVersion() == proto::kVersion2)
        sendDatagram(proto::makeDeviceHealth(robotId(), m_componentOnline), tr("健康状态"));
}

void SimulatorWindow::sendDatagram(const QByteArray &data, const QString &description)
{
    if (!m_running)
        return;
    QHostAddress address(m_serverEdit->text().trimmed());
    if (address.isNull())
        return;

    const qint64 result = m_uplink->writeDatagram(
        data, address, static_cast<quint16>(m_portEdit->value()));
    if (result != data.size()) {
        ++m_udpSendFailures;
        appendLog(tr("发送失败 [%1]: %2").arg(description, m_uplink->errorString()));
    } else if (description != tr("状态") && description != tr("健康状态")) {
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
    if (!canSendEvents()) return;
    if (protocolVersion() == proto::kVersion1)
        sendDatagram(eventFrameV1(proto::TypeAttack), tr("攻击"));
    else
        sendDatagram(eventFrameV2(proto::TypeAttack), tr("攻击"));
}

void SimulatorWindow::onSendHit()
{
    if (!canSendEvents() || !m_aliveEdit->isChecked() || m_roundEnded)
        return;
    const int nextHp = qMax(0, m_hpEdit->value() - 20);
    m_hpEdit->setValue(nextHp);
    if (protocolVersion() == proto::kVersion1)
        sendDatagram(proto::makeHitV1(robotId(), team(), static_cast<quint16>(nextHp)),
                     tr("受击"));
    else
        sendDatagram(eventFrameV2(proto::TypeHit), tr("受击"));
    if (!nextHp && m_aliveEdit->isChecked())
        onSendDeath();
    if (m_running)
        sendStatus();
}

void SimulatorWindow::onSendDeath()
{
    if (!canSendEvents()) return;
    m_hpEdit->setValue(0);
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
    if (!canSendEvents() || m_roundEnded
        || (protocolVersion() == proto::kVersion2 && m_yellowCards >= 3))
        return;
    m_aliveEdit->setChecked(true);
    m_shootEdit->setChecked(true);
    m_hpEdit->setValue(m_hpEdit->maximum());
    if (protocolVersion() == proto::kVersion1)
        sendDatagram(eventFrameV1(proto::TypeRevive), tr("复活"));
    else
        queueReliableEvent(proto::TypeRevive);
    if (m_running)
        sendStatus();
}

void SimulatorWindow::onSendShootEnabled()
{
    if (!canSendEvents()) return;
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
    if (!canSendEvents()) return;
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
    if (!canSendEvents()) return;
    m_componentOnline &= ~quint8(1);
    if (protocolVersion() == proto::kVersion2)
        sendDatagram(eventFrameV2(proto::TypeLinkDown), tr("链路断开"));
}

void SimulatorWindow::onSendLinkUp()
{
    if (!canSendEvents()) return;
    m_componentOnline |= 1;
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
    for (QUdpSocket *socket : {m_downlink, m_uplink}) {
        while (socket->hasPendingDatagrams()) {
            const QNetworkDatagram dg = socket->receiveDatagram();
            if (dg.isValid())
                handleDownlink(dg.data(), dg.senderAddress(), dg.senderPort());
        }
    }
}

void SimulatorWindow::handleDownlink(const QByteArray &data,
                                     const QHostAddress &addr, quint16 port)
{
    if (!m_running || protocolVersion() != proto::kVersion2
        || addr != QHostAddress(m_serverEdit->text().trimmed())
        || port != static_cast<quint16>(m_portEdit->value()))
        return;
    proto::Frame frame;
    if (!proto::parseDatagram(data, frame) || frame.version != proto::kVersion2)
        return;
    const quint8 type = frame.type;
    const quint32 txid = frame.transactionId;
    if (type == proto::TypeAck) {
        auto it = m_pendingEvents.find(txid);
        if (it != m_pendingEvents.end() && it->type == frame.ackedFrameType) {
            m_pendingEvents.erase(it);
            appendLog(tr("[ACK] 服务器已确认 %1 txid=%2 result=%3")
                          .arg(frame.ackedFrameType == proto::TypeDeath ? tr("死亡") : tr("复活"))
                          .arg(txid).arg(frame.result));
        }
        return;
    }
    if (!proto::isDownlinkCommand(type))
        return;
    // ASSIGNMENT addresses an unassigned device by MAC, not by its old ID.
    if (type == proto::TypeAssignment) {
        if (frame.deviceMac != m_deviceMac)
            return;
    } else if (!m_assignedId || frame.robotId != m_assignedId) {
        return;
    }
    if (type == proto::TypeStatusRequest) {
        sendStatus();
        return;
    }
    if (txid == 0) {
        sendAck(addr, port, type, txid, 2);
        return;
    }
    const quint64 key = (quint64(type) << 32) | txid;
    const auto completed = m_completedCommands.constFind(key);
    if (completed != m_completedCommands.constEnd()) {
        sendAck(addr, port, type, txid, completed.value());
        return;
    }
    quint8 result = 0;
    switch (type) {
    case proto::TypeAssignment:
        if (!frame.robotId || frame.controllerMac == QByteArray(6, char(0))
            || !saveDeviceAssignment(frame.robotId, frame.controllerMac)) {
            result = 2;
        } else {
            appendLog(tr("[分配成功] 机器人 %1，手柄 %2；红蓝队伍由服务端维护")
                          .arg(m_assignedId).arg(m_controllerMacDisplay->text()));
            sendDeviceAnnounce();
        }
        break;
    case proto::TypeGameStart:
        m_formalMode = true;
        m_roundEnded = false;
        m_yellowCards = 0;
        m_hpEdit->setMaximum(kDefaultHp);
        m_hpEdit->setValue(kDefaultHp);
        m_aliveEdit->setChecked(true);
        m_shootEdit->setChecked(true);
        break;
    case proto::TypeGameEnd:
        m_roundEnded = true;
        m_shootEdit->setChecked(false);
        sendDatagram(eventFrameV2(proto::TypeShootDisabled), tr("禁止射击"));
        sendDatagram(eventFrameV2(proto::TypeCombatEnd), tr("脱离战斗"));
        break;
    case proto::TypeSetHp:
        if (frame.hp > m_hpEdit->maximum()) {
            result = 2;
        } else if (m_yellowCards >= 3 && frame.hp > 0) {
            result = 1;
        } else {
            m_hpEdit->setValue(frame.hp);
            m_aliveEdit->setChecked(frame.hp != 0);
            m_shootEdit->setChecked(frame.hp != 0 && !m_roundEnded);
        }
        break;
    case proto::TypeYellowCard:
        if (!m_formalMode || m_roundEnded) {
            result = 1;
            break;
        }
        ++m_yellowCards;
        m_hpEdit->setValue(m_yellowCards >= 3 ? 0 : qMax(0, m_hpEdit->value() - 50));
        if (m_hpEdit->value() == 0)
            onSendDeath();
        break;
    case proto::TypeForcePowerOff:
        m_powerOnEdit->setChecked(false);
        break;
    case proto::TypeForcePowerOn:
        m_powerOnEdit->setChecked(true);
        break;
    default:
        return;
    }
    if (result != 2) {
        if (m_completedCommandOrder.size() >= 128)
            m_completedCommands.remove(m_completedCommandOrder.takeFirst());
        m_completedCommandOrder.append(key);
        m_completedCommands.insert(key, result);
    }
    sendAck(addr, port, type, txid, result);
    sendStatus();
    appendLog(tr("[下行] type=0x%1 txid=%2 result=%3").arg(type, 2, 16).arg(txid).arg(result));
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
        sendDatagram(ev.datagram,
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
