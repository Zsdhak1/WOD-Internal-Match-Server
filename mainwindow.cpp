#include "mainwindow.h"
#include "broadcastwindow.h"
#include "fieldcamera.h"
#include "checkinpanel.h"
#include "refereeentrypanel.h"
#include "matchprotocol.h"
#include "matchserver.h"
#include "protocol.h"
#include "responsiveui.h"
#include "robotcommander.h"
#include "robotmanager.h"

#include <QCheckBox>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QSaveFile>
#include <QStandardPaths>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QGuiApplication>
#include <QGroupBox>
#include <QGridLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLineEdit>
#include <QLabel>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QMessageBox>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSettings>
#include <QSplitter>
#include <QSpinBox>
#include <QStatusBar>
#include <QStyle>
#include <QStringList>
#include <QTableWidget>
#include <QTabWidget>
#include <QTime>
#include <QTimer>
#include <QUdpSocket>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <exception>
#include <utility>

namespace {
constexpr int kExpectedRobots = 2;
constexpr quint8 kRedTeam = 1;
constexpr quint8 kBlueTeam = 2;
constexpr int kColId    = 0;
constexpr int kColTeam  = 1;
constexpr int kColHp    = 2;
constexpr int kColHeat  = 3;
constexpr int kColPower = 4;   // V2 only
constexpr int kColState = 5;
constexpr int kColShoot = 6;
constexpr int kColPowerOn = 7; // V2 only
constexpr int kColLink  = 8;   // V2 only
constexpr int kColVer   = 9;
constexpr int kColAddr  = 10;

QStringList parseDelimitedLine(const QString &line, QChar delimiter)
{
    QStringList fields;
    QString field;
    bool quoted = false;

    for (int i = 0; i < line.size(); ++i) {
        const QChar ch = line.at(i);
        if (ch == QLatin1Char('"')) {
            if (quoted && i + 1 < line.size() && line.at(i + 1) == QLatin1Char('"')) {
                field += QLatin1Char('"');
                ++i;
            } else {
                quoted = !quoted;
            }
        } else if (ch == delimiter && !quoted) {
            fields.append(field.trimmed());
            field.clear();
        } else {
            field += ch;
        }
    }
    fields.append(field.trimmed());
    return fields;
}

QString jsonString(const QJsonObject &object, const QStringList &keys)
{
    for (const QString &key : keys) {
        const QJsonValue value = object.value(key);
        if (value.isString() && !value.toString().trimmed().isEmpty())
            return value.toString().trimmed();
    }
    return QString();
}

bool isTeamHeader(const QStringList &fields)
{
    if (fields.size() < 2)
        return false;

    const QString red = fields.at(0).trimmed().toLower();
    const QString blue = fields.at(1).trimmed().toLower();
    const bool redHeader = red == QStringLiteral("red")
                           || red == QStringLiteral("redteam")
                           || red == QStringLiteral("red_team")
                           || red.contains(QStringLiteral("红方"))
                           || red.contains(QStringLiteral("红队"))
                           || red.contains(QStringLiteral("红色"));
    const bool blueHeader = blue == QStringLiteral("blue")
                            || blue == QStringLiteral("blueteam")
                            || blue == QStringLiteral("blue_team")
                            || blue.contains(QStringLiteral("蓝方"))
                            || blue.contains(QStringLiteral("蓝队"))
                            || blue.contains(QStringLiteral("蓝色"));
    return redHeader && blueHeader;
}

bool parseMac6(const QString &text, QByteArray *mac, QString *error)
{
    const QString normalized = text.trimmed().replace(QLatin1Char('-'), QLatin1Char(':'));
    const QStringList parts = normalized.split(QLatin1Char(':'), Qt::SkipEmptyParts);
    if (parts.size() != 6) {
        if (error)
            *error = QObject::tr("MAC 必须为 6 段十六进制，例如 8D:23:AB:A5:3C:C9");
        return false;
    }

    QByteArray parsed;
    parsed.reserve(6);
    for (const QString &part : parts) {
        bool ok = false;
        const uint value = part.toUInt(&ok, 16);
        if (!ok || value > 0xffU) {
            if (error)
                *error = QObject::tr("MAC 段 %1 不是有效十六进制字节").arg(part);
            return false;
        }
        parsed.append(static_cast<char>(value));
    }

    if (mac)
        *mac = parsed;
    return true;
}

QByteArray macBytes(const QString &text)
{
    QByteArray bytes;
    QString error;
    return parseMac6(text, &bytes, &error) ? bytes : QByteArray();
}

QString controlStyleSheet(qreal scale)
{
    const auto px = [scale](int value) { return qMax(1, qRound(value * scale)); };
    return QStringLiteral(
        "QWidget { color: #dce6eb; }"
        "QWidget#serverWorkspace { background: #0c1520; }"
        "QLabel { background: transparent; }"
        "QGroupBox { background: #142330; border: %1px solid #43515e; border-radius: %2px; margin-top: %3px; "
        "padding: %4px %3px %3px; font-weight: 700; color: #dce6eb; }"
        "QGroupBox::title { subcontrol-origin: margin; left: %4px; padding: 0 %5px; "
        "color: #72b4ff; }"
        "QPushButton { background: #1e465c; border: 1px solid #376b82; color: #edf8ff; min-height: %6px; padding: 0 %7px; }"
        "QPushButton:hover { background: #28647e; border-color: #65c6e0; }"
        "QPushButton:pressed { background: #087f96; }"
        "QPushButton:focus { border: 2px solid #74d9ee; }"
        "QPushButton:disabled { background: #202e39; color: #728490; border-color: #30434f; }"
        "QComboBox, QLineEdit, QSpinBox { background: #203645; color: #eef8ff; border: 1px solid #456071; min-height: %8px; padding: 0 6px; selection-background-color: #087f96; }"
        "QComboBox QAbstractItemView { background: #203645; color: #eef8ff; selection-background-color: #087f96; }"
        "QPlainTextEdit, QTableWidget { background: #101a23; color: #dce6eb; border: 1px solid #334652; }"
        "QTableWidget { alternate-background-color: #101a23; gridline-color: #2d3c47; }"
        "QHeaderView::section { background: #1b2a35; color: #dce6eb; padding: %5px; }"
        "QFrame#controlHeader { background: #101b24; border: %1px solid #334652; "
        "border-radius: %2px; }"
        "QTabWidget::pane { border: 1px solid #334652; background: #101923; }"
        "QLabel#serverMatchOverview { background: #19384a; color: #edf8ff; font-weight: bold; border: 1px solid #356783; }"
        "QLabel#serverProgramMonitor { background: #070c12; color: #8fa9ba; border: 1px solid #356783; }"
        "QStatusBar { background: #0c1520; color: #aebdc6; }"
        "QTabBar::tab { background: #1b2a35; color: #aebdc6; padding: 6px 8px; margin-right: 1px; }"
        "QTabBar::tab:selected { background: #087f96; color: #ffffff; }"
        "QTabBar::tab:hover { background: #2d4a5d; color: #ffffff; }"
        "QLabel#serverActionStatus { background: #203443; color: #dcebf5; padding: 8px 12px; border: 1px solid #3d6172; }"
        "QLabel#serverActionStatus[state=success] { background: #145b48; color: #d9fff0; border-color: #2ca77e; }"
        "QLabel#serverActionStatus[state=error] { background: #672f3b; color: #ffe3e8; border-color: #d56a7b; }"
        "QLabel#serverActionStatus[state=pending] { background: #66501d; color: #fff5cf; border-color: #c99b32; }")
        .arg(px(1))
        .arg(px(6))
        .arg(px(10))
        .arg(px(12))
        .arg(px(5))
        .arg(px(30))
        .arg(px(11))
        .arg(px(28));
}

} // namespace

MainWindow::MainWindow(QWidget *parent, FieldCameraBackend *cameraBackend, std::function<qint64()> monotonicClock)
    : QMainWindow(parent), m_monotonicClock(std::move(monotonicClock))
{
    m_socket = new QUdpSocket(this);
    m_socket->setObjectName(QStringLiteral("serverUdpSocket"));
    connect(m_socket, &QUdpSocket::readyRead, this, &MainWindow::onReadyRead);

    m_commander = new RobotCommander(m_socket, this);
    connect(m_commander, &RobotCommander::logMessage, this,
            &MainWindow::onLogMessage);
    connect(m_commander, &RobotCommander::commandAcked, this,
            [this](quint32 txid, quint8 type, quint8 result, quint8 robotId) {
        if (competitionMode() && type == proto::TypeReviveApprove) {
            const QString device = m_activeDeviceMacs.value(robotId);
            m_competition.acknowledgeApproval(device, robotId, txid, result);
            persistCompetitionLedger();
            if (result != 0) showActionResult(tr("复活执行尚未确认 · 机器人 %1，结果 %2").arg(robotId).arg(result), QStringLiteral("error"));
            updateCompetition();
            return;
        }
        if (competitionMode() && type == proto::TypeGameStart
            && m_competition.phase() == CompetitionFlow::Phase::Starting) {
            m_startAcknowledged[robotId] = result == 0;
            if (result != 0) {
                m_competition.cancelStart(); sendGameCommand(false);
                showActionResult(tr("开赛失败：机器人 %1 拒绝，双方结束命令已发送，请核实设备").arg(robotId), QStringLiteral("error"));
            }
        }
        finishCommand(txid, result == 0 ? tr("机器人 %1 已确认执行").arg(robotId)
                                        : tr("机器人 %1 拒绝执行，错误码 %2").arg(robotId).arg(result), result == 0);
        const auto pending = m_pendingAssignments.find(txid);
        if (type == proto::TypeAssignment && pending != m_pendingAssignments.end()) {
            const PendingAssignment assignment = pending.value();
            m_pendingAssignments.erase(pending);
            if (result == 0 && m_robots) {
                m_robots->setTeamForRobot(assignment.robotId, assignment.team);
                onLogMessage(tr("[设备] 分配确认：%1 → 机器人 %2（%3）")
                                 .arg(assignment.deviceMac).arg(assignment.robotId)
                                 .arg(assignment.team == 1 ? tr("红方")
                                      : assignment.team == 2 ? tr("蓝方") : tr("未分配")));
                refreshTable();
            }
        }
        if (result != 0)
            onLogMessage(QStringLiteral("[警告] 机器人%1 执行 0x%2 失败 result=%3")
                             .arg(robotId).arg(type, 2, 16).arg(result));
    });
    connect(m_commander, &RobotCommander::commandTimeout, this,
            [this](quint32 txid, quint8 type, quint8 robotId) {
        if (competitionMode() && type == proto::TypeReviveApprove) {
            m_competition.approvalTimeout(robotId, txid);
            persistCompetitionLedger();
            showActionResult(tr("机器人 %1 复活确认超时，次数保留占用，需核实").arg(robotId), QStringLiteral("error"));
            updateCompetition(); return;
        }
        if (competitionMode() && type == proto::TypeGameStart
            && m_competition.phase() == CompetitionFlow::Phase::Starting) {
            m_competition.cancelStart(); sendGameCommand(false);
            showActionResult(tr("双方开赛确认未完成，已发送结束命令；请检查后重试"), QStringLiteral("error"));
        }
        m_pendingAssignments.remove(txid);
        finishCommand(txid, tr("机器人 %1 确认超时，无法确认执行成功").arg(robotId), false);
        onLogMessage(QStringLiteral("[超时] 机器人%1 命令 0x%2 未获 ACK")
                         .arg(robotId).arg(type, 2, 16));
    });

    m_robots = new RobotManager(this);
    connect(m_robots, &RobotManager::endpointForgotten, m_commander, &RobotCommander::forgetEndpoint);
    connect(m_robots, &RobotManager::robotsChanged, this, &MainWindow::refreshTable);
    connect(m_robots, &RobotManager::logMessage, this, &MainWindow::onLogMessage);
    connect(m_robots, &RobotManager::reliableEventNeedsAck, this,
            &MainWindow::onReliableEventNeedsAck);
    connect(m_robots, &RobotManager::ackReceivedFrom, m_commander,
            &RobotCommander::onAckFrom);
    connect(m_robots, &RobotManager::reviveRequested, this, &MainWindow::onReviveRequested);
    connect(m_robots, &RobotManager::endpointLearned, this,
            &MainWindow::onEndpointLearned);
    connect(m_robots, &RobotManager::combatEvent, this,
            [this](quint8 team, quint8 robotId, quint8 type) {
        if (!m_programModeEdit || !m_programModeEdit->currentData().toBool()
            || !m_programSourceEdit
            || (type != proto::TypeHit && type != proto::TypeAttack
                && type != proto::TypeDeath)) {
            return;
        }

        for (const auto &source : std::as_const(m_videoSources)) {
            if (!source.online || source.team != team
                || (source.robotId != 0 && source.robotId != robotId)) {
                continue;
            }
            const int index = m_programSourceEdit->findData(source.sourceId);
            if (index >= 0) {
                m_programSourceEdit->setCurrentIndex(index);
                if (m_autoSwitchTimer && m_autoSwitchIntervalEdit)
                    m_autoSwitchTimer->start(m_autoSwitchIntervalEdit->value() * 1000);
            }
            break;
        }
    });

    connect(m_robots, &RobotManager::combatEvent, this,
            [this](quint8 team, quint8 robotId, quint8 type) {
        if (competitionMode() && m_activeRobotIds.value(team) != robotId) return;
        m_referee.recordCombatEvent(team, robotId, type, refereeNow());
        if (competitionMode() && type == proto::TypeDeath)
            m_competition.observeLife(team, false, refereeNow());
        if (type == proto::TypeDeath)
            onRefereeTick();
    });

    m_refereeClock.start();
    m_refereeTimer = new QTimer(this);
    m_refereeTimer->setInterval(100);
    connect(m_refereeTimer, &QTimer::timeout, this, &MainWindow::onRefereeTick);
    m_refereeTimer->start();

    m_broadcast = new BroadcastWindow;
    // MatchReferee decides the result at 00:00 instead of the manual score.
    m_broadcast->setAutoFinishOnTimeout(false);
    connect(m_broadcast, &BroadcastWindow::timeExpired, this, &MainWindow::onTimeExpired);
    connect(m_broadcast, &BroadcastWindow::summaryExpandedChanged, this, [this](bool expanded) {
        if (m_summaryToggleBtn)
            m_summaryToggleBtn->setText(expanded ? tr("收起赛后数据") : tr("展开赛后数据"));
        publishMatchState();
    });
    m_matchServer = new MatchServer(this);
    connect(m_matchServer, &MatchServer::logMessage, this, &MainWindow::onLogMessage);
    connect(m_matchServer, &MatchServer::videoSourcesChanged, this,
            &MainWindow::onVideoSourcesChanged);
    connect(m_matchServer, &MatchServer::videoFrameReceived, this,
            [this](const QString &sourceId, const QImage &frame) {
        if (m_broadcast)
            m_broadcast->setSourceFrame(sourceId, frame);
    });
    connect(m_broadcast, &BroadcastWindow::sourceFrameUpdated, this,
            &MainWindow::onSourceFrameUpdated);
    connect(m_matchServer, &MatchServer::serverStateChanged, this,
            [this](bool listening, const QString &message) {
                if (m_clientStateLabel)
                    m_clientStateLabel->setText(message);
                if (m_clientListenBtn)
                    m_clientListenBtn->setText(listening ? tr("停止登记服务") : tr("启动登记服务"));
            });
    connect(m_matchServer, &MatchServer::clientCountChanged, this, [this](int count) {
        if (m_clientStateLabel && m_matchServer && m_matchServer->isListening())
            m_clientStateLabel->setText(tr("登记服务运行中 · 已登记选手 %1 人").arg(count));
    });
    connect(m_broadcast, &BroadcastWindow::presentationStateChanged,
            this, &MainWindow::publishMatchState);

    m_fieldCamera = cameraBackend ? new FieldCamera(cameraBackend, this) : new FieldCamera(this);
    m_fieldCamera->setObjectName(QStringLiteral("fieldCameraController"));
    connect(m_fieldCamera, &FieldCamera::frameReady, this, [this](const QImage &frame) {
        if (m_broadcast) m_broadcast->setProgramFrame(frame);
        if (frame.isNull()) onSourceFrameUpdated(QStringLiteral("field"));
    });
    connect(m_fieldCamera, &FieldCamera::devicesChanged, this, &MainWindow::refreshFieldCameraDevices);
    connect(m_fieldCamera, &FieldCamera::stateChanged, this, &MainWindow::onFieldCameraStateChanged);
    connect(m_fieldCamera, &FieldCamera::logMessage, this, &MainWindow::onLogMessage);
    buildUi();
    m_checkinPanel = new CheckinPanel(this);
    m_checkinPanel->setWindowFlag(Qt::Window, true);
    m_checkinPanel->resize(1400, 850);
    m_checkinPanel->hide();
    const auto refreshCheckin = [this] {
        m_checkinPanel->updateSnapshot(m_robots->robots(), m_robots->devices());
    };
    connect(m_robots, &RobotManager::robotsChanged, this, refreshCheckin);
    connect(m_robots, &RobotManager::devicesChanged, this, refreshCheckin);
    QSettings cameraSettings(QSettings::defaultFormat(), QSettings::UserScope,
                             QStringLiteral("Scompetition"), QStringLiteral("FieldCamera"));
    m_fieldCamera->selectDevice(cameraSettings.value(QStringLiteral("deviceId")).toByteArray());
    refreshFieldCameraDevices();
    onFieldCameraStateChanged();
    loadRefereeRules();
    QSettings ledger(QSettings::defaultFormat(), QSettings::UserScope,
                     QStringLiteral("Scompetition"), QStringLiteral("RevivalLedger"));
    const QByteArray saved = ledger.value(QStringLiteral("ledger")).toByteArray();
    if (!saved.isEmpty()) {
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(saved, &error);
        m_ledgerHealthy = error.error == QJsonParseError::NoError
                          && document.isObject() && m_competition.restoreLedger(document.object());
        if (!m_ledgerHealthy) showActionResult(tr("复活审批账本损坏，禁止自动批准；请保留数据并人工核实"), QStringLiteral("error"));
    }
    refreshCompetitionUi();
    populateScreens();
    connect(m_robots, &RobotManager::devicesChanged, this,
            &MainWindow::refreshDeviceList);
    connect(m_broadcast, &BroadcastWindow::visibilityChanged, this, [this](bool visible) {
        if (m_broadcastBtn)
            m_broadcastBtn->setText(visible ? tr("隐藏转播画面") : tr("显示转播画面"));
    });
    connect(m_broadcast, &BroadcastWindow::matchStateChanged, this, [this](bool running) {
        if (m_matchBtn)
            m_matchBtn->setText(competitionMode() ? tr("开始本局 · 5秒倒计时") : running ? tr("暂停比赛") : tr("开始比赛"));
    });
    m_autoSwitchTimer = new QTimer(this);
    connect(m_autoSwitchTimer, &QTimer::timeout, this, &MainWindow::onAutoSwitchTimeout);
    // Finish optional file/network initialization after the control window has
    // entered the event loop. A corrupt or locked layout file must never make
    // Explorer appear to do nothing after a double-click.
    QTimer::singleShot(0, this, [this] {
        if (!m_broadcast)
            return;
        m_broadcast->loadLayout(layoutFilePath());
        refreshLayoutPositionEditors();
        // Deferred startup is intentionally kept to lightweight UI state.
        // Network services are started by the explicit controls below.
        publishMatchState();
        updateProgramSourceList();
        refreshTable();
        refreshDeviceList();
        // The native window has a real splitter size only after the first
        // event-loop turn. Re-run the responsive pass so vertical layouts on
        // compact displays receive their intended proportions.
        applyResponsiveUi();
        // Test hook: SCOMP_AUTO_BROADCAST=1 opens the broadcast output right
        // after startup so resolution scaling can be verified unattended.
        if (qEnvironmentVariableIsSet("SCOMP_AUTO_BROADCAST"))
            onShowBroadcast();
    });

    for (int sourceNumber = 1; sourceNumber <= 5; ++sourceNumber) {
        auto *shortcut = new QShortcut(
            QKeySequence(QStringLiteral("Ctrl+%1").arg(sourceNumber)), this);
        shortcut->setContext(Qt::ApplicationShortcut);
        connect(shortcut, &QShortcut::activated, this, [this, sourceNumber] {
            if (m_programSourceEdit && sourceNumber <= m_programSourceEdit->count())
                m_programSourceEdit->setCurrentIndex(sourceNumber - 1);
        });
    }
}

MainWindow::~MainWindow()
{
    if (m_fieldCamera) m_fieldCamera->stop();
    if (m_autoSwitchTimer)
        m_autoSwitchTimer->stop();
    if (m_matchServer)
        m_matchServer->stop();
    if (m_socket)
        m_socket->close();

    if (m_broadcast) {
        m_broadcast->hide();
        delete m_broadcast;
        m_broadcast = nullptr;
    }
    delete m_uiScaler;
    m_uiScaler = nullptr;
}

void MainWindow::buildUi()
{
    setWindowTitle(tr("赛事转播控制台"));

    auto *central = new QWidget(this);
    central->setObjectName(QStringLiteral("serverWorkspace"));
    central->setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 11));
    central->setStyleSheet(controlStyleSheet(1.0));

    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(10);
    // Do not carry a large-screen minimum size into the compact breakpoint.
    root->setSizeConstraint(QLayout::SetNoConstraint);

    auto *header = new QFrame(central);
    header->setObjectName(QStringLiteral("controlHeader"));
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(14, 9, 14, 9);
    auto *title = new QLabel(tr("赛事转播控制台"), header);
    title->setStyleSheet(QStringLiteral("color: #e7f5ff;"));
    QFont titleFont = title->font();
    titleFont.setPointSize(16);
    titleFont.setBold(true);
    title->setFont(titleFont);
    headerLayout->addWidget(title);

    m_connLabel = new QLabel(header);
    m_connLabel->setWordWrap(true);
    m_connLabel->setStyleSheet(QStringLiteral("color: #aebdc6;"));
    headerLayout->addWidget(m_connLabel, 1);
    m_actionStatusLabel = new QLabel(tr("就绪 · 操作结果将在此显示"), header);
    m_actionStatusLabel->setObjectName(QStringLiteral("serverActionStatus"));
    m_actionStatusLabel->setWordWrap(true);
    m_actionStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_actionStatusLabel->setAccessibleName(tr("操作执行结果"));
    m_actionStatusLabel->setMinimumWidth(300);
    headerLayout->addWidget(m_actionStatusLabel);
    root->addWidget(header);

    m_workspace = new QSplitter(Qt::Horizontal, central);
    m_workspace->setChildrenCollapsible(false);
    m_workspace->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);

    const auto createGroup = [central](const QString &titleText) {
        auto *group = new QGroupBox(titleText, central);
        group->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
        return group;
    };

    auto *matchGroup = createGroup(tr("比赛与转播"));
    auto *matchLayout = new QFormLayout(matchGroup);
    matchLayout->setRowWrapPolicy(QFormLayout::WrapLongRows);
    matchLayout->setContentsMargins(8, 10, 8, 6);
    matchLayout->setHorizontalSpacing(12);
    matchLayout->setVerticalSpacing(8);
    m_screenEdit = new QComboBox(matchGroup);
    m_screenEdit->setMinimumWidth(180);
    connect(m_screenEdit, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::onBroadcastScreenChanged);
    m_broadcastBtn = new QPushButton(tr("显示转播画面"), matchGroup);
    connect(m_broadcastBtn, &QPushButton::clicked, this, &MainWindow::onShowBroadcast);
    matchLayout->addRow(tr("输出屏幕"), m_screenEdit);
    auto *matchActions = new QGridLayout;
    matchActions->setSpacing(6);
    matchActions->addWidget(m_broadcastBtn, 0, 0);
    m_matchBtn = new QPushButton(tr("开始本局 · 5秒倒计时"), matchGroup);
    m_matchBtn->setObjectName(QStringLiteral("startMatch"));
    connect(m_matchBtn, &QPushButton::clicked, this, &MainWindow::onToggleMatch);
    m_terminateMatchBtn = new QPushButton(tr("终止本局"), matchGroup);
    connect(m_terminateMatchBtn, &QPushButton::clicked,
            this, &MainWindow::onTerminateMatch);
    m_resetMatchBtn = new QPushButton(tr("重置比赛"), matchGroup);
    connect(m_resetMatchBtn, &QPushButton::clicked, this, &MainWindow::onResetMatch);
    matchActions->addWidget(m_matchBtn, 0, 1);
    matchActions->addWidget(m_terminateMatchBtn, 1, 0);
    matchActions->addWidget(m_resetMatchBtn, 1, 1);
    matchLayout->addRow(tr("比赛操作"), matchActions);
    m_noDeviceDebugEdit = new QCheckBox(tr("无车调试（跳过设备检查，仅用于测试）"), matchGroup);
    m_noDeviceDebugEdit->setObjectName(QStringLiteral("noDeviceDebug"));
    m_noDeviceDebugEdit->setToolTip(tr("开赛不绑定机器人、不发送 GAME_START、不等待设备确认；"
                                       "无血量数据，时间到后需裁判手动判定。每次启动默认关闭。"));
    m_noDeviceDebugEdit->setStyleSheet(QStringLiteral("QCheckBox:checked { color: #ff9f1a; font-weight: bold; }"));
    connect(m_noDeviceDebugEdit, &QCheckBox::toggled, this, &MainWindow::onNoDeviceDebugToggled);
    matchLayout->addRow(tr("调试"), m_noDeviceDebugEdit);
    m_competitionPhaseLabel = new QLabel(tr("BO3 · 第1局 · 等待裁判开赛 · 03:00"), matchGroup);
    m_competitionPhaseLabel->setObjectName(QStringLiteral("competitionPhase"));
    m_competitionPhaseLabel->setWordWrap(true);
    m_competitionPhaseLabel->setStyleSheet(QStringLiteral("color:#ffce48; font-weight:bold; padding:6px;"));
    matchLayout->addRow(m_competitionPhaseLabel);
    auto *revives = new QHBoxLayout;
    m_redRevivesLabel = new QLabel(tr("红方剩余复活：3 / 3"), matchGroup);
    m_redRevivesLabel->setObjectName(QStringLiteral("redRemainingRevives"));
    m_blueRevivesLabel = new QLabel(tr("蓝方剩余复活：3 / 3"), matchGroup);
    m_blueRevivesLabel->setObjectName(QStringLiteral("blueRemainingRevives"));
    revives->addWidget(m_redRevivesLabel); revives->addWidget(m_blueRevivesLabel);
    matchLayout->addRow(revives);
    auto *animationActions = new QHBoxLayout;
    m_settlementPreviewTypeEdit = new QComboBox(matchGroup);
    m_settlementPreviewTypeEdit->addItem(tr("红方胜利"), QStringLiteral("redwin"));
    m_settlementPreviewTypeEdit->addItem(tr("蓝方胜利"), QStringLiteral("bluewin"));
    m_settlementPreviewTypeEdit->addItem(tr("平局"), QStringLiteral("draw"));
    m_settlementPreviewTypeEdit->addItem(tr("终止"), QStringLiteral("termination"));
    animationActions->addWidget(m_settlementPreviewTypeEdit);
    m_testVictoryAnimationBtn = new QPushButton(tr("预览结算动画"), matchGroup);
    connect(m_testVictoryAnimationBtn, &QPushButton::clicked,
            this, &MainWindow::onTestVictoryAnimation);
    animationActions->addWidget(m_testVictoryAnimationBtn);
    animationActions->addStretch(1);
    matchLayout->addRow(tr("动画预览"), animationActions);
    auto *checkinButton = new QPushButton(tr("打开车辆检录（只读）"), matchGroup);
    checkinButton->setObjectName(QStringLiteral("openCheckin"));
    connect(checkinButton, &QPushButton::clicked, this, [this] {
        m_checkinPanel->updateSnapshot(m_robots->robots(), m_robots->devices());
        m_checkinPanel->setInspectionEnabled(true);
        m_checkinPanel->show();
        m_checkinPanel->raise();
        showActionResult(tr("车辆检录已打开 · 只读，不发送控制命令"));
    });
    matchLayout->addRow(tr("赛前检查"), checkinButton);

    auto *refereeGroup = createGroup(tr("胜负判定"));
    refereeGroup->setObjectName(QStringLiteral("refereeGroup"));
    auto *refereeLayout = new QVBoxLayout(refereeGroup);
    refereeLayout->setContentsMargins(8, 10, 8, 6);
    refereeLayout->setSpacing(8);
    m_refereeStatusLabel = new QLabel(refereeGroup);
    m_refereeStatusLabel->setObjectName(QStringLiteral("refereeStatus"));
    m_refereeStatusLabel->setWordWrap(true);
    m_refereeStatusLabel->setTextFormat(Qt::RichText);
    m_refereeStatusLabel->setMinimumHeight(40);
    m_refereeStatusLabel->setStyleSheet(QStringLiteral(
        "QLabel#refereeStatus { background: #f3f6f8; color: #1d2a33; border: 1px solid #c9d3da;"
        " border-radius: 5px; padding: 6px 9px; }"));
    refereeLayout->addWidget(m_refereeStatusLabel);

    auto *ruleGrid = new QGridLayout;
    ruleGrid->setHorizontalSpacing(12);
    ruleGrid->setVerticalSpacing(6);
    m_redCardDisqualifiesEdit = new QCheckBox(tr("红牌直接判负"), refereeGroup);
    m_redCardDisqualifiesEdit->setObjectName(QStringLiteral("ruleRedCard"));
    m_offlineGraceEdit = new QSpinBox(refereeGroup);
    m_offlineGraceEdit->setObjectName(QStringLiteral("ruleOfflineGrace"));
    m_offlineGraceEdit->setRange(0, 120);
    m_offlineGraceEdit->setSuffix(tr(" 秒"));
    m_offlineGraceEdit->setSpecialValueText(tr("不判负"));
    m_offlineGraceEdit->setToolTip(tr("比赛进行中一方连续离线超过该时长、且另一方在线时判负；暂停时不计时。"));
    m_ruleProfileEdit = new QComboBox(refereeGroup);
    m_ruleProfileEdit->setObjectName(QStringLiteral("ruleProfile"));
    m_ruleProfileEdit->addItem(tr("正式 BO3 · 复活 / 血量"), true);
    m_ruleProfileEdit->addItem(tr("旧调试 · 击杀积分"), false);
    ruleGrid->addWidget(m_ruleProfileEdit, 0, 0);
    connect(m_ruleProfileEdit, qOverload<int>(&QComboBox::currentIndexChanged), this, &MainWindow::onRefereeRulesChanged);
    ruleGrid->addWidget(m_redCardDisqualifiesEdit, 0, 1);
    auto *graceRow = new QHBoxLayout;
    graceRow->addWidget(new QLabel(tr("离线判负"), refereeGroup));
    graceRow->addWidget(m_offlineGraceEdit, 1);
    ruleGrid->addLayout(graceRow, 1, 1);
    ruleGrid->setColumnStretch(0, 1);
    ruleGrid->setColumnStretch(1, 1);
    refereeLayout->addLayout(ruleGrid);
    connect(m_redCardDisqualifiesEdit, &QCheckBox::toggled, this, &MainWindow::onRefereeRulesChanged);
    connect(m_offlineGraceEdit, qOverload<int>(&QSpinBox::valueChanged),
            this, &MainWindow::onRefereeRulesChanged);

    auto *verdictActions = new QHBoxLayout;
    verdictActions->setSpacing(6);
    m_declareRedBtn = new QPushButton(tr("判红方胜"), refereeGroup);
    m_declareRedBtn->setObjectName(QStringLiteral("declareRedWin"));
    m_declareRedBtn->setStyleSheet(QStringLiteral("color: #ff6872; font-weight: 700;"));
    m_declareBlueBtn = new QPushButton(tr("判蓝方胜"), refereeGroup);
    m_declareBlueBtn->setObjectName(QStringLiteral("declareBlueWin"));
    m_declareBlueBtn->setStyleSheet(QStringLiteral("color: #72b4ff; font-weight: 700;"));
    m_summaryToggleBtn = new QPushButton(tr("裁判录入数据"), refereeGroup);
    m_summaryToggleBtn->setObjectName(QStringLiteral("summaryToggle"));
    m_summaryToggleBtn->setEnabled(false);
    connect(m_declareRedBtn, &QPushButton::clicked, this, &MainWindow::onDeclareRedWin);
    connect(m_declareBlueBtn, &QPushButton::clicked, this, &MainWindow::onDeclareBlueWin);
    connect(m_summaryToggleBtn, &QPushButton::clicked, this, &MainWindow::onToggleSummary);
    verdictActions->addWidget(m_declareRedBtn, 1);
    verdictActions->addWidget(m_declareBlueBtn, 1);
    verdictActions->addWidget(m_summaryToggleBtn, 1);
    refereeLayout->addLayout(verdictActions);
    auto *refereeHint = new QLabel(
        tr("本局击杀对方 +1 分，常规时间内可复活。时间到积分高者胜；"
           "同分进入 SUDDEN DEATH，下一次单方击杀定胜负。同时击杀各 +1，继续加时。"
           "裁判判定、红黄牌及离线判负仍优先。"),
        refereeGroup);
    refereeHint->setWordWrap(true);
    refereeHint->setStyleSheet(QStringLiteral("color: #65717c;"));
    refereeGroup->setToolTip(refereeHint->text());
    refereeHint->hide();

    auto *teamGroup = createGroup(tr("队伍、比分与牌面"));
    auto *teamLayout = new QVBoxLayout(teamGroup);
    teamLayout->setContentsMargins(8, 10, 8, 6);
    teamLayout->setSpacing(8);
    auto *teamNames = new QGridLayout;
    teamNames->setHorizontalSpacing(8);
    teamNames->setVerticalSpacing(6);
    auto *redLabel = new QLabel(tr("红方队名"), teamGroup);
    redLabel->setStyleSheet(QStringLiteral("color: #ff6872; font-weight: 700;"));
    auto *blueLabel = new QLabel(tr("蓝方队名"), teamGroup);
    blueLabel->setStyleSheet(QStringLiteral("color: #72b4ff; font-weight: 700;"));
    m_redTeamNameEdit = new QLineEdit(tr("红方"), teamGroup);
    m_blueTeamNameEdit = new QLineEdit(tr("蓝方"), teamGroup);
    auto *applyTeamNamesButton = new QPushButton(tr("应用队名"), teamGroup);
    connect(applyTeamNamesButton, &QPushButton::clicked,
            this, &MainWindow::onTeamNamesChanged);
    // Editing only previews names; applying is an explicit safe transaction.
    teamNames->addWidget(redLabel, 0, 0);
    teamNames->addWidget(m_redTeamNameEdit, 0, 1);
    teamNames->addWidget(blueLabel, 1, 0);
    teamNames->addWidget(m_blueTeamNameEdit, 1, 1);
    teamNames->addWidget(applyTeamNamesButton, 2, 0, 1, 2);
    teamNames->setColumnStretch(1, 1);

    teamLayout->addLayout(teamNames);

    auto *teamTableRow = new QGridLayout;
    m_importTeamsBtn = new QPushButton(tr("导入队伍表"), teamGroup);
    m_nextTeamBtn = new QPushButton(tr("下一组队伍"), teamGroup);
    m_teamPairLabel = new QLabel(teamGroup);
    m_teamPairLabel->setWordWrap(true);
    connect(m_importTeamsBtn, &QPushButton::clicked,
            this, &MainWindow::onImportTeamTable);
    connect(m_nextTeamBtn, &QPushButton::clicked,
            this, &MainWindow::onNextTeamPair);
    teamTableRow->addWidget(new QLabel(tr("队伍表"), teamGroup),0,0);
    teamTableRow->addWidget(m_importTeamsBtn,0,1);
    teamTableRow->addWidget(m_nextTeamBtn,0,2);
    m_teamPairEdit = new QComboBox(teamGroup);
    m_teamPairEdit->setObjectName(QStringLiteral("teamPairSelection"));
    m_applyTeamPairBtn = new QPushButton(tr("应用所选对阵"), teamGroup);
    m_applyTeamPairBtn->setObjectName(QStringLiteral("applyTeamPair"));
    connect(m_applyTeamPairBtn, &QPushButton::clicked, this, [this] {
        applyTeamPair(m_teamPairEdit->currentData().toInt(), true);
    });
    teamTableRow->addWidget(m_teamPairEdit,1,0,1,2);
    teamTableRow->addWidget(m_applyTeamPairBtn,1,2);
    teamTableRow->addWidget(m_teamPairLabel,2,0,1,3);
    teamLayout->addLayout(teamTableRow);

    auto *scoreRow = new QGridLayout;
    scoreRow->setHorizontalSpacing(8);
    scoreRow->setVerticalSpacing(6);

    auto *redScoreLabel = new QLabel(tr("红方 BO3 胜局"), teamGroup);
    m_redScoreLabel = redScoreLabel;
    redScoreLabel->setStyleSheet(QStringLiteral("color: #ff6872;"));
    auto *blueScoreLabel = new QLabel(tr("蓝方 BO3 胜局"), teamGroup);
    m_blueScoreLabel = blueScoreLabel;
    blueScoreLabel->setStyleSheet(QStringLiteral("color: #72b4ff;"));
    scoreRow->addWidget(redScoreLabel, 0, 0);
    m_redScoreEdit = new QSpinBox(teamGroup);
    m_redScoreEdit->setRange(0, 9999);
    m_redScoreEdit->setValue(0);
    scoreRow->addWidget(m_redScoreEdit, 0, 1);
    scoreRow->addWidget(blueScoreLabel, 1, 0);
    m_blueScoreEdit = new QSpinBox(teamGroup);
    m_blueScoreEdit->setRange(0, 9999);
    m_blueScoreEdit->setValue(0);
    scoreRow->addWidget(m_blueScoreEdit, 1, 1);
    connect(m_redScoreEdit, qOverload<int>(&QSpinBox::valueChanged),
            this, &MainWindow::onScoresChanged);
    connect(m_blueScoreEdit, qOverload<int>(&QSpinBox::valueChanged),
            this, &MainWindow::onScoresChanged);

    m_redCardBtn = new QPushButton(tr("红牌"), teamGroup);
    m_yellowCardBtn = new QPushButton(tr("黄牌"), teamGroup);
    m_clearCardsBtn = new QPushButton(tr("清除牌面"), teamGroup);
    connect(m_redCardBtn, &QPushButton::clicked, this, &MainWindow::onAwardRedCard);
    connect(m_yellowCardBtn, &QPushButton::clicked, this, &MainWindow::onAwardYellowCard);
    connect(m_clearCardsBtn, &QPushButton::clicked, this, &MainWindow::onClearCards);
    scoreRow->addWidget(m_redCardBtn, 2, 0);
    scoreRow->addWidget(m_yellowCardBtn, 2, 1);
    scoreRow->addWidget(m_clearCardsBtn, 3, 0, 1, 2);

    teamLayout->addLayout(scoreRow);
    m_cardTargetLabel = new QLabel(tr("判罚对象：未选择 · 请到「状态」页选择机器人"), teamGroup);
    m_cardTargetLabel->setObjectName(QStringLiteral("cardTarget"));
    m_cardTargetLabel->setWordWrap(true);
    teamLayout->addWidget(m_cardTargetLabel);
    auto *selectCardTarget = new QPushButton(tr("选择判罚机器人 → 状态页"), teamGroup);
    connect(selectCardTarget, &QPushButton::clicked, this, [this] {
        m_controlTabs->setCurrentIndex(0);
        if (m_compactTabs->isVisible()) m_compactTabs->setCurrentWidget(m_controlTabs);
        m_table->setFocus();
        showActionResult(tr("请在中央状态表选择判罚对象"), QStringLiteral("info"));
    });
    teamLayout->addWidget(selectCardTarget);

    auto *programGroup = createGroup(tr("节目输出与字幕"));
    auto *programLayout = new QFormLayout(programGroup);
    programLayout->setRowWrapPolicy(QFormLayout::WrapLongRows);
    programLayout->setContentsMargins(8, 10, 8, 6);
    programLayout->setHorizontalSpacing(12);
    programLayout->setVerticalSpacing(8);
    m_programModeEdit = new QComboBox(programGroup);
    m_programModeEdit->addItem(tr("手动切换"), false);
    m_programModeEdit->addItem(tr("自动切换"), true);
    connect(m_programModeEdit, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::onProgramModeChanged);
    connect(m_programModeEdit, qOverload<int>(&QComboBox::activated), this, [this](int) {
        showActionResult(tr("导播模式已应用：%1").arg(m_programModeEdit->currentText()));
    });
    m_programSourceEdit = new QComboBox(programGroup);
    m_programSourceEdit->setObjectName(QStringLiteral("programSource"));
    m_programSourceEdit->setMinimumWidth(220);
    connect(m_programSourceEdit, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::onProgramSourceChanged);
    connect(m_programSourceEdit, qOverload<int>(&QComboBox::activated), this, [this](int) {
        showActionResult(tr("节目视角已切换：%1").arg(m_programSourceEdit->currentText()));
    });
    m_autoSwitchIntervalEdit = new QSpinBox(programGroup);
    m_autoSwitchIntervalEdit->setRange(1, 60);
    m_autoSwitchIntervalEdit->setValue(5);
    m_autoSwitchIntervalEdit->setSuffix(tr(" 秒"));
    connect(m_autoSwitchIntervalEdit, qOverload<int>(&QSpinBox::valueChanged),
            this, [this](int seconds) {
                if (m_autoSwitchTimer && m_autoSwitchTimer->isActive())
                    m_autoSwitchTimer->start(seconds * 1000);
                showActionResult(tr("自动切换间隔已设为 %1 秒").arg(seconds));
            });
    auto *modeRow = new QHBoxLayout;
    modeRow->addWidget(m_programModeEdit, 1);
    modeRow->addWidget(new QLabel(tr("自动间隔"),programGroup));
    modeRow->addWidget(m_autoSwitchIntervalEdit);
    programLayout->addRow(tr("导播模式"), modeRow);
    programLayout->addRow(tr("当前视角"), m_programSourceEdit);

    m_tickerPresetEdit = new QComboBox(programGroup);
    m_tickerPresetEdit->setMinimumWidth(140);
    m_tickerPresetEdit->addItem(tr("欢迎词"), tr("欢迎来到赛事转播现场 · 比赛即将开始"));
    m_tickerPresetEdit->addItem(tr("比赛进行中"), tr("红方 vs 蓝方 · 精彩对决进行中"));
    m_tickerPresetEdit->addItem(tr("秩序提示"), tr("请各参赛队伍注意比赛秩序，听从裁判指示"));
    m_tickerPresetEdit->addItem(tr("现场编写"), QString());
    connect(m_tickerPresetEdit, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::onTickerPresetChanged);

    m_tickerTextEdit = new QLineEdit(programGroup);
    m_tickerTextEdit->setPlaceholderText(tr("输入要展示的字幕内容"));
    m_tickerTextEdit->setMinimumWidth(80);

    m_tickerApplyBtn = new QPushButton(tr("发布字幕"), programGroup);
    m_tickerApplyBtn->setToolTip(tr("将当前文字发布到节目画面底部"));
    connect(m_tickerApplyBtn, &QPushButton::clicked, this, &MainWindow::onApplyTicker);

    m_tickerToggleBtn = new QPushButton(tr("显示字幕"), programGroup);
    m_tickerToggleBtn->setToolTip(tr("显示或隐藏节目画面底部字幕条"));
    connect(m_tickerToggleBtn, &QPushButton::clicked, this, &MainWindow::onToggleTicker);

    auto *tickerLabel = new QLabel(tr("底部跑马字幕"), programGroup);
    auto *tickerBlock = new QVBoxLayout;
    tickerBlock->setSpacing(6);
    auto *tickerInputRow = new QHBoxLayout;
    tickerInputRow->addWidget(m_tickerPresetEdit);
    tickerInputRow->addWidget(m_tickerTextEdit, 1);
    tickerInputRow->addWidget(m_tickerApplyBtn);
    tickerInputRow->addWidget(m_tickerToggleBtn);
    tickerBlock->addLayout(tickerInputRow);
    programLayout->addRow(tickerLabel, tickerBlock);

    auto *cameraGroup = createGroup(tr("本地全场摄像头（USB / 内置）"));
    cameraGroup->setObjectName(QStringLiteral("fieldCameraGroup"));
    auto *cameraLayout = new QFormLayout(cameraGroup);
    cameraLayout->setRowWrapPolicy(QFormLayout::WrapLongRows);
    cameraLayout->setContentsMargins(8, 10, 8, 6);
    m_fieldCameraEdit = new QComboBox(cameraGroup);
    m_fieldCameraEdit->setObjectName(QStringLiteral("fieldCameraDevice"));
    m_fieldCameraEdit->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_fieldCameraEdit->setMinimumContentsLength(12);
    connect(m_fieldCameraEdit, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &MainWindow::onFieldCameraSelected);
    cameraLayout->addRow(tr("摄像头设备"), m_fieldCameraEdit);
    auto *cameraActions = new QHBoxLayout;
    auto *refreshCamera = new QPushButton(tr("刷新设备"), cameraGroup);
    refreshCamera->setObjectName(QStringLiteral("fieldCameraRefresh"));
    connect(refreshCamera, &QPushButton::clicked, this, [this] {
        refreshFieldCameraDevices();
        showActionResult(tr("摄像头列表已刷新 · 检测到 %1 台设备").arg(m_fieldCamera->devices().size()));
    });
    m_fieldCameraToggle = new QPushButton(tr("启动采集"), cameraGroup);
    m_fieldCameraToggle->setObjectName(QStringLiteral("fieldCameraToggle"));
    connect(m_fieldCameraToggle, &QPushButton::clicked, this, &MainWindow::onToggleFieldCamera);
    auto *takeCamera = new QPushButton(tr("切到全场"), cameraGroup);
    takeCamera->setObjectName(QStringLiteral("fieldCameraTake"));
    connect(takeCamera, &QPushButton::clicked, this, [this] {
        m_programSourceEdit->setCurrentIndex(m_programSourceEdit->findData(QStringLiteral("field")));
        showActionResult(tr("节目输出已切换到全场视角"));
    });
    cameraActions->addWidget(refreshCamera);
    cameraActions->addWidget(m_fieldCameraToggle);
    cameraActions->addWidget(takeCamera);
    cameraLayout->addRow(QString(), cameraActions);
    m_fieldCameraStatus = new QLabel(cameraGroup);
    m_fieldCameraStatus->setObjectName(QStringLiteral("fieldCameraStatus"));
    m_fieldCameraStatus->setWordWrap(true);
    cameraLayout->addRow(QString(), m_fieldCameraStatus);
    auto *cameraHint = new QLabel(tr("独立全场机位，不占红蓝选手身份；启动后可点击下方全场缩略图导播。"
                                     "默认不采集，断开后仅重连同一设备。"), cameraGroup);
    cameraHint->setWordWrap(true);
    cameraGroup->setToolTip(cameraHint->text());
    cameraHint->hide();

    // Per-source live preview strip. Each registered client shows up here as
    // a clickable thumbnail so the operator can see every feed at a glance.
    m_sourcePreviewGroup = createGroup(tr("已接入视频源预览"));
    auto *previewGroupLayout = new QVBoxLayout(m_sourcePreviewGroup);
    previewGroupLayout->setContentsMargins(8, 10, 8, 6);
    previewGroupLayout->setSpacing(6);
    auto *previewContent = new QWidget(m_sourcePreviewGroup);
    m_sourcePreviewLayout = new QGridLayout(previewContent);
    m_sourcePreviewLayout->setContentsMargins(0, 0, 0, 0);
    m_sourcePreviewLayout->setSpacing(10);
    previewGroupLayout->addWidget(previewContent);

    auto *layoutGroup = createGroup(tr("HUD 布局"));
    auto *layoutForm = new QFormLayout(layoutGroup);
    layoutForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    layoutForm->setContentsMargins(8, 10, 8, 6);
    layoutForm->setHorizontalSpacing(12);
    layoutForm->setVerticalSpacing(8);
    m_layoutElementEdit = new QComboBox(layoutGroup);
    m_layoutElementEdit->setMinimumWidth(180);
    const QList<QPair<QString, QString>> layoutElements = {
        {tr("红方信息面板"), QStringLiteral("red_team_panel")},
        {tr("中央整体计时 / 比分 HUD"), QStringLiteral("center_hud")},
        {tr("蓝方信息面板"), QStringLiteral("blue_team_panel")},

        {tr("比赛标题"), QStringLiteral("match_title")},
        {tr("比赛计时"), QStringLiteral("match_timer")},
        {tr("左侧小局比分"), QStringLiteral("red_round_score")},
        {tr("右侧小局比分"), QStringLiteral("blue_round_score")},
        {tr("比赛状态提示"), QStringLiteral("match_state")},
        {tr("当前视角提示"), QStringLiteral("source_label")}
    };
    for (const auto &element : layoutElements)
        m_layoutElementEdit->addItem(element.first, element.second);
    connect(m_layoutElementEdit, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::onLayoutElementChanged);
    layoutForm->addRow(tr("布局元素"), m_layoutElementEdit);
    auto *positionRow = new QHBoxLayout;
    m_layoutXEdit = new QSpinBox(layoutGroup);
    m_layoutXEdit->setRange(-2000, 2000);
    m_layoutXEdit->setSuffix(tr(" 基准 px"));
    connect(m_layoutXEdit, qOverload<int>(&QSpinBox::valueChanged),
            this, &MainWindow::onLayoutPositionChanged);
    positionRow->addWidget(new QLabel(tr("X"), layoutGroup));
    positionRow->addWidget(m_layoutXEdit, 1);
    m_layoutYEdit = new QSpinBox(layoutGroup);
    m_layoutYEdit->setRange(-2000, 2000);
    m_layoutYEdit->setSuffix(tr(" 基准 px"));
    connect(m_layoutYEdit, qOverload<int>(&QSpinBox::valueChanged),
            this, &MainWindow::onLayoutPositionChanged);
    positionRow->addWidget(new QLabel(tr("Y"), layoutGroup));
    positionRow->addWidget(m_layoutYEdit, 1);
    layoutForm->addRow(tr("位置"), positionRow);
    auto *sizeRow = new QHBoxLayout;
    m_layoutWidthEdit = new QSpinBox(layoutGroup);
    m_layoutWidthEdit->setRange(1, 4000);
    m_layoutWidthEdit->setSuffix(tr(" 基准 px"));
    connect(m_layoutWidthEdit, qOverload<int>(&QSpinBox::valueChanged),
            this, &MainWindow::onLayoutSizeChanged);
    sizeRow->addWidget(new QLabel(tr("宽"), layoutGroup));
    sizeRow->addWidget(m_layoutWidthEdit, 1);
    m_layoutHeightEdit = new QSpinBox(layoutGroup);
    m_layoutHeightEdit->setRange(1, 4000);
    m_layoutHeightEdit->setSuffix(tr(" 基准 px"));
    connect(m_layoutHeightEdit, qOverload<int>(&QSpinBox::valueChanged),
            this, &MainWindow::onLayoutSizeChanged);
    sizeRow->addWidget(new QLabel(tr("高"), layoutGroup));
    sizeRow->addWidget(m_layoutHeightEdit, 1);
    layoutForm->addRow(tr("尺寸"), sizeRow);
    m_resetLayoutBtn = new QPushButton(tr("恢复默认布局"), layoutGroup);
    m_saveLayoutBtn = new QPushButton(tr("保存布局"), layoutGroup);
    m_loadLayoutBtn = new QPushButton(tr("加载布局"), layoutGroup);
    connect(m_resetLayoutBtn, &QPushButton::clicked, this, &MainWindow::onResetLayout);
    connect(m_saveLayoutBtn, &QPushButton::clicked, this, &MainWindow::onSaveLayout);
    connect(m_loadLayoutBtn, &QPushButton::clicked, this, &MainWindow::onLoadLayout);
    auto *layoutActions = new QHBoxLayout;
    layoutActions->addWidget(m_resetLayoutBtn);
    layoutActions->addWidget(m_saveLayoutBtn);
    layoutActions->addWidget(m_loadLayoutBtn);
    layoutForm->addRow(QString(), layoutActions);
    auto *layoutHint = new QLabel(
        tr("位置和尺寸使用 1920×1080 基准像素；输出到其他分辨率时会按比例换算；"
           "中央底板为整体素材，比分文字仍可调整；边栏默认贴合中央宽度；布局文件保存在程序目录"),
        layoutGroup);
    layoutHint->setStyleSheet(QStringLiteral("color: #65717c;"));
    layoutHint->setWordWrap(true);
    layoutForm->addRow(QString(), layoutHint);
    refreshLayoutPositionEditors();

    auto *networkGroup = createGroup(tr("网络服务"));
    auto *networkLayout = new QFormLayout(networkGroup);
    networkLayout->setRowWrapPolicy(QFormLayout::WrapLongRows);
    networkLayout->setContentsMargins(8, 10, 8, 6);
    networkLayout->setHorizontalSpacing(12);
    networkLayout->setVerticalSpacing(8);
    auto *udpRow = new QHBoxLayout;
    m_portEdit = new QSpinBox(networkGroup);
    m_portEdit->setObjectName(QStringLiteral("serverUdpPort"));
    m_portEdit->setRange(1, 65535);
    m_portEdit->setValue(proto::kUplinkPort);
    m_listenBtn = new QPushButton(tr("启动监听"), networkGroup);
    connect(m_listenBtn, &QPushButton::clicked, this, &MainWindow::onToggleListen);
    udpRow->addWidget(m_portEdit);
    udpRow->addWidget(m_listenBtn);
    networkLayout->addRow(tr("机器人 UDP"), udpRow);

    auto *clientRow = new QHBoxLayout;
    m_clientPortEdit = new QSpinBox(networkGroup);
    m_clientPortEdit->setRange(1, 65535);
    m_clientPortEdit->setValue(matchproto::kControlPort);
    m_clientListenBtn = new QPushButton(tr("启动登记服务"), networkGroup);
    connect(m_clientListenBtn, &QPushButton::clicked, this, &MainWindow::onToggleClientServer);
    m_clientStateLabel = new QLabel(tr("未启动"), networkGroup);
    m_clientStateLabel->setWordWrap(true);
    clientRow->addWidget(m_clientPortEdit);
    clientRow->addWidget(m_clientListenBtn);
    clientRow->addWidget(m_clientStateLabel, 1);
    networkLayout->addRow(tr("选手端 TCP"), clientRow);

    // 设备管理面板：V2 中 team 不再来自数据包，需服务器侧分配
    auto *deviceGroup = new QGroupBox(tr("设备管理"), central);
    auto *deviceLayout = new QVBoxLayout(deviceGroup);
    deviceLayout->setContentsMargins(8, 10, 8, 8);
    auto *deviceGrid = new QGridLayout;
    auto *deviceSelectLabel = new QLabel(tr("ESP32 设备:"), deviceGroup);
    m_deviceSelectEdit = new QComboBox(deviceGroup);
    m_deviceSelectEdit->setObjectName(QStringLiteral("deviceSelect"));
    m_deviceSelectEdit->setMinimumWidth(200);
    m_deviceSelectEdit->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_deviceSelectEdit->setMinimumContentsLength(16);
    auto *deviceRobotLabel = new QLabel(tr("机器人 ID:"), deviceGroup);
    m_deviceRobotEdit = new QSpinBox(deviceGroup);
    m_deviceRobotEdit->setObjectName(QStringLiteral("deviceRobotId"));
    m_deviceRobotEdit->setRange(1, 255);
    auto *deviceTeamLabel = new QLabel(tr("分配到:"), deviceGroup);
    m_deviceTeamEdit = new QComboBox(deviceGroup);
    m_deviceTeamEdit->setObjectName(QStringLiteral("deviceTeam"));
    m_deviceTeamEdit->addItem(tr("未分配"), 0);
    m_deviceTeamEdit->addItem(tr("红方"), 1);
    m_deviceTeamEdit->addItem(tr("蓝方"), 2);
    auto *deviceAssignBtn = new QPushButton(tr("分配 ID + 手柄"), deviceGroup);
    connect(deviceAssignBtn, &QPushButton::clicked, this,
            &MainWindow::onAssignTeam);
    auto *devicePowerOnBtn = new QPushButton(tr("通电"), deviceGroup);
    connect(devicePowerOnBtn, &QPushButton::clicked, this,
            &MainWindow::onForcePowerOn);
    auto *devicePowerOffBtn = new QPushButton(tr("断电"), deviceGroup);
    connect(devicePowerOffBtn, &QPushButton::clicked, this,
            &MainWindow::onForcePowerOff);
    auto *deviceHpBtn = new QPushButton(tr("设 HP"), deviceGroup);
    connect(deviceHpBtn, &QPushButton::clicked, this,
            &MainWindow::onSetHp);
    auto *deviceHpEdit = new QSpinBox(deviceGroup);
    deviceHpEdit->setRange(0, 300);
    deviceHpEdit->setValue(300);
    m_deviceHpEdit = deviceHpEdit;
    auto *deviceStatusBtn = new QPushButton(tr("请求状态"), deviceGroup);
    connect(deviceStatusBtn, &QPushButton::clicked, this,
            &MainWindow::onRequestStatus);
    m_deviceControllerMacEdit = new QLineEdit(deviceGroup);
    m_deviceControllerMacEdit->setObjectName(QStringLiteral("deviceControllerMac"));
    m_deviceControllerMacEdit->setPlaceholderText(tr("手柄 MAC，例如 8D:23:AB:A5:3C:C9"));
    deviceGrid->addWidget(deviceSelectLabel,0,0,1,2);
    deviceGrid->addWidget(m_deviceSelectEdit,1,0,1,2);
    deviceGrid->addWidget(deviceRobotLabel,2,0);
    deviceGrid->addWidget(m_deviceRobotEdit,2,1);
    deviceGrid->addWidget(deviceTeamLabel,3,0);
    deviceGrid->addWidget(m_deviceTeamEdit,3,1);
    deviceGrid->addWidget(m_deviceControllerMacEdit,4,0,1,2);
    deviceGrid->addWidget(deviceAssignBtn,5,0,1,2);
    deviceGrid->addWidget(deviceHpEdit,6,0);
    deviceGrid->addWidget(deviceHpBtn,6,1);
    deviceGrid->addWidget(devicePowerOnBtn,7,0);
    deviceGrid->addWidget(devicePowerOffBtn,7,1);
    deviceGrid->addWidget(deviceStatusBtn,8,0,1,2);
    deviceGrid->setColumnStretch(0,1);
    deviceGrid->setColumnStretch(1,1);
    deviceLayout->addLayout(deviceGrid);

    connect(m_deviceSelectEdit, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) {
        if (!m_robots || !m_deviceSelectEdit || !m_deviceControllerMacEdit
            || !m_deviceRobotEdit) {
            return;
        }
        RobotManager::DeviceInfo device;
        if (!m_robots->deviceForMac(m_deviceSelectEdit->currentData().toString(),
                                    &device)) {
            return;
        }
        m_deviceControllerMacEdit->setText(device.controllerMac);
        m_deviceControllerMacEdit->setProperty("reportedMac", device.controllerMac);
        m_deviceRobotEdit->setProperty("reportedId", device.robotId);
        if (device.robotId != 0)
            m_deviceRobotEdit->setValue(device.robotId);
    });
    auto *deviceHint = new QLabel(
        tr("等待 ESP32 DEVICE_ANNOUNCE 后选择设备，填写手柄 BLE MAC；"
           "服务器按 21 字节 ASSIGNMENT 分配 ID，并通过 UDP 5006 下发。"),
        deviceGroup);
    deviceHint->setStyleSheet(QStringLiteral("color: #6c7886;"));
    QFont deviceHintFont = deviceHint->font();
    deviceHintFont.setPointSize(11);
    deviceHint->setFont(deviceHintFont);
    deviceHint->setWordWrap(true);
    deviceGroup->setToolTip(deviceHint->text());
    deviceHint->hide();

    auto *tableGroup = new QGroupBox(tr("机器人状态"), central);
    auto *tableLayout = new QVBoxLayout(tableGroup);
    tableLayout->setContentsMargins(8, 10, 8, 8);
    m_table = new QTableWidget(0, 11, tableGroup);
    m_table->setObjectName(QStringLiteral("robotTable"));
    m_table->setHorizontalHeaderLabels({
        tr("机器人ID"), tr("队伍"), tr("血量 HP"), tr("热量"),
        tr("功率 W"), tr("状态"), tr("射击"), tr("供电"), tr("链路"),
        tr("协议"), tr("来源地址")});
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] {
        const int row = m_table ? m_table->currentRow() : -1;
        const auto *id = row >= 0 ? m_table->item(row, kColId) : nullptr;
        const auto *team = row >= 0 ? m_table->item(row, kColTeam) : nullptr;
        if (m_cardTargetLabel)
            m_cardTargetLabel->setText(id && team
                ? tr("判罚对象：机器人 %1 · 队伍 %2").arg(id->text(), team->text())
                : tr("判罚对象：未选择 · 请到「状态」页选择机器人"));
    });
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(kColAddr, QHeaderView::Stretch);
    m_table->setMinimumHeight(85);
    tableLayout->addWidget(m_table);

    auto *logRow = new QHBoxLayout;
    auto *logGroup = new QGroupBox(tr("事件日志"), central);
    auto *logLayout = new QVBoxLayout(logGroup);
    logLayout->setContentsMargins(8, 10, 8, 8);
    auto *logTitle = new QLabel(tr("近期事件 · 登记 / 战斗 / 设备回执"), logGroup);
    QFont logTitleFont = logTitle->font();
    logTitleFont.setBold(true);
    logTitle->setFont(logTitleFont);
    auto *clearBtn = new QPushButton(tr("清空日志"), logGroup);
    connect(clearBtn, &QPushButton::clicked, this, [this] {
        if (m_log) m_log->clear();
        showActionResult(tr("事件日志已清空"));
    });
    logRow->addWidget(logTitle, 1);
    logRow->addStretch();
    logRow->addWidget(clearBtn);
    logLayout->addLayout(logRow);

    m_log = new QPlainTextEdit(logGroup);
    m_log->setObjectName(QStringLiteral("serverLog"));
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);
    logLayout->addWidget(m_log, 1);
    logGroup->setObjectName(QStringLiteral("serverEventLog"));
    logGroup->setMinimumHeight(110);
    logGroup->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

    const auto makeTabs = [this](const QString &name) {
        auto *tabs = new QTabWidget(m_workspace);
        tabs->setObjectName(name);
        tabs->setDocumentMode(true);
        tabs->setUsesScrollButtons(false);
        return tabs;
    };
    const auto makePage = [](const QList<QWidget *> &widgets, bool stretch) {
        auto *page = new QWidget;
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(4, 6, 4, 4);
        layout->setSpacing(8);
        for (QWidget *widget : widgets) layout->addWidget(widget);
        if (stretch) layout->addStretch(1);
        return page;
    };
    m_leftTabs = makeTabs(QStringLiteral("serverLeftSidebar"));
    m_controlTabs = makeTabs(QStringLiteral("serverControlTabs"));
    m_rightTabs = makeTabs(QStringLiteral("serverRightSidebar"));
    auto *leftMatchPage = makePage({matchGroup,refereeGroup,teamGroup},true);
    m_leftTabs->addTab(leftMatchPage,tr("比赛 / 裁判 / 队伍"));
    auto *livePage = makePage({},false);
    auto *liveLayout = qobject_cast<QVBoxLayout *>(livePage->layout());
    m_matchOverview = new QLabel(tr("等待比赛开始 · 红方 0 : 0 蓝方"),livePage);
    m_matchOverview->setObjectName(QStringLiteral("serverMatchOverview"));
    m_matchOverview->setAlignment(Qt::AlignCenter);
    m_matchOverview->setMinimumHeight(48);
    liveLayout->addWidget(m_matchOverview);
    m_programMonitor = new QLabel(tr("节目监看 · 暂无视频信号"),livePage);
    m_programMonitor->setObjectName(QStringLiteral("serverProgramMonitor"));
    m_programMonitor->setAlignment(Qt::AlignCenter);
    m_programMonitor->setMinimumHeight(100);
    m_programMonitor->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Expanding);
    liveLayout->addWidget(m_programMonitor,3);
    liveLayout->addWidget(m_sourcePreviewGroup);
    liveLayout->addWidget(programGroup);
    tableGroup->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);
    m_table->setMinimumHeight(85);
    liveLayout->addWidget(tableGroup,1);
    m_controlTabs->addTab(livePage,tr("实时监看 / 导播"));
    m_controlTabs->addTab(makePage({layoutGroup},true),tr("HUD 布局"));
    m_refereeEntryPanel = new RefereeEntryPanel;
    m_refereeEntryPanel->setObjectName(QStringLiteral("refereeEntryPanel"));
    m_refereeEntryTabIndex = m_controlTabs->addTab(m_refereeEntryPanel, tr("裁判录入辅助"));
    auto *rightPage = makePage({deviceGroup,networkGroup,cameraGroup},false);
    rightPage->setObjectName(QStringLiteral("serverRightOperationsPage"));
    auto *rightLayout = qobject_cast<QVBoxLayout *>(rightPage->layout());
    rightLayout->addWidget(logGroup, 1);
    m_rightTabs->addTab(rightPage,tr("设备 / 网络 / 摄像头 / 日志"));
    m_workspace->addWidget(m_leftTabs);
    m_workspace->addWidget(m_controlTabs);
    m_workspace->addWidget(m_rightTabs);
    m_workspace->setStretchFactor(0,1);
    m_workspace->setStretchFactor(1,2);
    m_workspace->setStretchFactor(2,1);
    root->addWidget(m_workspace,1);
    m_compactTabs = new QTabWidget(central);
    m_compactTabs->setObjectName(QStringLiteral("serverCompactWorkspace"));
    m_compactTabs->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_compactTabs->setMinimumSize(0, 0);
    m_workspace->setMinimumSize(0, 0);
    m_compactTabs->hide();
    root->addWidget(m_compactTabs,1);

    auto *fullscreen = new QPushButton(tr("全屏 · F11"),header);
    fullscreen->setObjectName(QStringLiteral("serverFullscreen"));
    const auto toggleFullscreen = [this] { isFullScreen() ? showNormal() : showFullScreen(); };
    connect(fullscreen,&QPushButton::clicked,this,toggleFullscreen);
    connect(new QShortcut(QKeySequence(Qt::Key_F11),this),&QShortcut::activated,this,toggleFullscreen);
    headerLayout->addWidget(fullscreen);
    connect(new QShortcut(QKeySequence(Qt::Key_Escape),this),&QShortcut::activated,this,[this] {
        if (isFullScreen()) showNormal();
    });

    setCentralWidget(central);
    // Start at a proportion of the available screen instead of fixed pixels so
    // the console stays usable on both small and high-resolution displays.
    const QRect available = screen() ? screen()->availableGeometry()
                                     : QRect(0, 0, 1280, 800);
    const QSize proportional(qMax(1, qRound(available.width() * 0.72)),
                             qMax(1, qRound(available.height() * 0.80)));
    const QSize initialSize = responsiveWindowSizeFromEnvironment(
        "SCOMP_MAIN_SIZE",
        QSize(qMin(proportional.width(), available.width()),
              qMin(proportional.height(), available.height())));
    m_uiScaler = new ResponsiveUiScaler;
    m_uiScaler->capture(this, QSize(1920, 1080), 0.60, 1.4);
    resize(initialSize);
    applyResponsiveUi();
    onTickerPresetChanged(0);
    updateTeamPairControls();
}

void MainWindow::startListen()
{
    if (m_listening) return;
    const quint16 port = static_cast<quint16>(m_portEdit->value());
    if (!m_socket->bind(QHostAddress::AnyIPv4, port)) {
        qWarning().noquote()
            << QStringLiteral("[网络] 绑定 UDP %1 失败: %2")
                   .arg(port).arg(m_socket->errorString());
        showActionResult(tr("绑定 UDP %1 失败：%2").arg(port).arg(m_socket->errorString()), QStringLiteral("error"));
        m_connLabel->setText(tr("绑定 UDP 端口 %1 失败: %2\n请更换端口或关闭占用该端口的程序后重试。")
                                 .arg(port)
                                 .arg(m_socket->errorString()));
        return;
    }
    m_listening = true;
    showActionResult(tr("UDP 监听已启动 · 端口 %1").arg(port));
    m_portEdit->setEnabled(false);
    m_listenBtn->setText(tr("停止监听"));
    updateListenInfo();
    onLogMessage(tr("已在 UDP 端口 %1 开始监听").arg(port));
    qInfo().noquote() << QStringLiteral("[网络] 已开始监听 UDP %1").arg(port);
}

void MainWindow::stopListen()
{
    if (!m_listening) return;
    if (competitionMode() && m_competition.locked()) {
        showActionResult(tr("当前 BO3 仍需要设备回执，请先终止并确认设备状态，再停止监听"), QStringLiteral("error")); return;
    }
    m_commander->cancelPending();
    m_pendingAssignments.clear();
    if (!m_actionCommands.isEmpty())
        showActionResult(tr("监听已停止，尚未确认的设备命令已取消"), QStringLiteral("error"));
    else
        showActionResult(tr("UDP 监听已停止"));
    m_actionCommands.clear();
    m_socket->close();
    m_listening = false;
    m_portEdit->setEnabled(true);
    m_listenBtn->setText(tr("启动监听"));
    m_connLabel->setText(tr("未在监听。"));
    onLogMessage(tr("已停止监听"));
    qInfo().noquote() << QStringLiteral("[网络] 已停止监听 UDP");
}

void MainWindow::populateScreens()
{
    if (!m_screenEdit) return;

    m_screenEdit->clear();
    const auto screens = QGuiApplication::screens();
    for (int i = 0; i < screens.size(); ++i) {
        const QScreen *screen = screens.at(i);
        const QRect geometry = screen->geometry();
        const QString name = screen->name().isEmpty()
                                 ? tr("屏幕 %1").arg(i + 1)
                                 : screen->name();
        m_screenEdit->addItem(tr("%1 (%2x%3)").arg(name).arg(geometry.width()).arg(geometry.height()),
                              i);
    }

    if (m_screenEdit->count() == 0)
        m_screenEdit->addItem(tr("主屏幕"), 0);

    // Automated layout captures run on a secondary display so the operator's
    // primary monitor remains untouched. Normal launches keep the user's
    // current screen selection unchanged.
    const QString requested = qEnvironmentVariable("SCOMP_BROADCAST_SCREEN")
                                  .trimmed().toLower();
    if (!requested.isEmpty() && m_screenEdit->count() > 0) {
        int requestedIndex = -1;
        bool numeric = false;
        const int parsed = requested.toInt(&numeric);
        if (numeric && parsed >= 0 && parsed < m_screenEdit->count()) {
            requestedIndex = parsed;
        } else if (requested == QStringLiteral("secondary")
                   || requested == QStringLiteral("non-primary")) {
            const QScreen *primary = QGuiApplication::primaryScreen();
            const auto screens = QGuiApplication::screens();
            for (int i = 0; i < screens.size(); ++i) {
                if (screens.at(i) != primary) {
                    requestedIndex = i;
                    break;
                }
            }
        }
        if (requestedIndex >= 0)
            m_screenEdit->setCurrentIndex(requestedIndex);
    }
}

QScreen *MainWindow::selectedScreen() const
{
    const auto screens = QGuiApplication::screens();
    if (screens.isEmpty()) return QGuiApplication::primaryScreen();

    // Test hook: keep unattended resolution captures off the user's primary
    // display.  "secondary" selects the first non-primary screen; a numeric
    // value selects the same zero-based index shown in the screen combo box.
    const QByteArray requested = qgetenv("SCOMP_BROADCAST_SCREEN").trimmed().toLower();
    if (!requested.isEmpty()) {
        if (requested == QByteArrayLiteral("secondary")) {
            QScreen *primary = QGuiApplication::primaryScreen();
            for (QScreen *candidate : screens) {
                if (candidate != primary)
                    return candidate;
            }
        } else {
            bool ok = false;
            const int requestedIndex = requested.toInt(&ok);
            if (ok && requestedIndex >= 0 && requestedIndex < screens.size())
                return screens.at(requestedIndex);
        }
    }

    const int index = m_screenEdit ? m_screenEdit->currentData().toInt() : 0;
    if (index >= 0 && index < screens.size())
        return screens.at(index);
    return QGuiApplication::primaryScreen();
}

void MainWindow::onShowBroadcast()
{
    if (!m_broadcast) return;

    if (m_broadcast->isVisible()) {
        m_broadcast->hide();
        showActionResult(tr("转播画面已隐藏"));
        return;
    }

    m_broadcast->showOnScreen(selectedScreen());
    showActionResult(m_broadcast->isVisible() ? tr("转播画面已显示") : tr("转播画面未能显示"),
                     m_broadcast->isVisible() ? QStringLiteral("success") : QStringLiteral("error"));
}

void MainWindow::onBroadcastScreenChanged(int)
{
    if (m_broadcast && m_broadcast->isVisible())
        m_broadcast->showOnScreen(selectedScreen());
}

QVector<RobotManager::RobotInfo> MainWindow::activeMatchRobots() const
{
    QVector<RobotManager::RobotInfo> result;
    if (!m_robots) return result;
    for (const auto &robot : m_robots->robots())
        if (robot.protocolVersion == proto::kVersion2 && m_activeRobotIds.value(robot.team) == robot.robotId)
            result.append(robot);
    return result;
}

bool MainWindow::bindCompetitionRobots(QString *error)
{
    if (m_noDeviceDebug) {
        // No robots are bound: activeMatchRobots() stays empty and
        // sendGameCommand() finds no eligible device to command.
        m_activeRobotIds.clear(); m_activeDeviceMacs.clear();
        return true;
    }
    if (!m_listening || !m_robots) { *error = tr("请先启动 UDP 监听并确认红蓝双方设备"); return false; }
    QHash<quint8, quint8> ids;
    QHash<quint8, QString> macs;
    for (quint8 team : {quint8(1), quint8(2)}) {
        QVector<RobotManager::RobotInfo> candidates;
        for (const auto &r : m_robots->robots())
            if (r.team == team && r.protocolVersion == proto::kVersion2 && r.online) candidates.append(r);
        if (candidates.size() != 1 || !candidates[0].linkUp || candidates[0].hp < 0) {
            *error = tr("%1需要恰好一台在线 V2 机器人，且状态/业务链路已确认").arg(team == 1 ? tr("红方") : tr("蓝方")); return false;
        }
        const auto &r = candidates[0];
        QVector<RobotManager::DeviceInfo> devices;
        for (const auto &d : m_robots->devices()) if (d.robotId == r.robotId) devices.append(d);
        if (devices.size() != 1 || devices[0].deviceMac.isEmpty() || devices[0].addressText != r.addressText) {
            *error = tr("机器人 %1 的设备 MAC/ID/地址不唯一，请重新确认分配").arg(r.robotId); return false;
        }
        if (!m_activeRobotIds.isEmpty() && (m_activeRobotIds.value(team) != r.robotId
            || m_activeDeviceMacs.value(r.robotId) != devices[0].deviceMac)) {
            *error = tr("本场 BO3 的机器人身份已锁定，不能中途替换"); return false;
        }
        ids.insert(team, r.robotId); macs.insert(r.robotId, devices[0].deviceMac);
    }
    if (ids.value(1) == ids.value(2)) { *error = tr("红蓝机器人 ID 必须不同"); return false; }
    m_activeRobotIds = ids; m_activeDeviceMacs = macs;
    return true;
}

bool MainWindow::persistCompetitionLedger()
{
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope,
                       QStringLiteral("Scompetition"), QStringLiteral("RevivalLedger"));
    settings.setValue(QStringLiteral("ledger"), QJsonDocument(m_competition.ledgerJson()).toJson(QJsonDocument::Compact));
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        m_ledgerHealthy = false;
        showActionResult(tr("复活审批账本保存失败，禁止发送新的复活批准，请人工核实"), QStringLiteral("error"));
        return false;
    }
    return m_ledgerHealthy;
}

void MainWindow::onReviveRequested(quint8 id, quint8 sequence, quint32 txid, quint32 ipv4, quint16 port)
{
    Q_UNUSED(port);
    if (!competitionMode() || !m_ledgerHealthy || !m_activeDeviceMacs.contains(id)) return;
    RobotManager::DeviceInfo device;
    if (!m_robots->deviceForMac(m_activeDeviceMacs.value(id), &device)
        || device.robotId != id || device.ipv4 != ipv4) return;
    quint8 team = 0;
    for (auto it = m_activeRobotIds.cbegin(); it != m_activeRobotIds.cend(); ++it)
        if (it.value() == id) team = it.key();
    const auto decision = m_competition.requestApproval(device.deviceMac, id, team, sequence, txid, refereeNow());
    if (decision.action == CompetitionFlow::RequestAction::Approve
        || decision.action == CompetitionFlow::RequestAction::Replay) {
        if (!persistCompetitionLedger()) return;
        if (!m_commander->sendReviveApproval(id, sequence, txid)) {
            m_competition.approvalTimeout(id, txid); persistCompetitionLedger();
            showActionResult(tr("机器人 %1 复活批准发送失败，保留次数占用并等待核实").arg(id), QStringLiteral("error"));
        }
    }
    updateCompetition();
}

QJsonObject MainWindow::competitionSnapshot() const
{
    const qint64 now = refereeNow();
    const bool resultAvailable = !m_lastRoundSummary.isEmpty()
        && m_lastRoundSummary.value(QStringLiteral("roundId")).toString() == m_competition.roundId()
        && (m_competition.phase() == CompetitionFlow::Phase::Intermission
            || m_competition.phase() == CompetitionFlow::Phase::MatchFinished
            || m_competition.phase() == CompetitionFlow::Phase::Cancelled);
    QString notice = m_referee.awaitingDecision() ? m_referee.liveNotice() : m_competition.phaseText();
    if (resultAvailable) notice = m_lastRoundSummary.value(QStringLiteral("reasonText")).toString();
    return {{"type", "match_state"}, {"protocolVersion", 2}, {"scoringRule", "remaining_resources"},
        {"phase", m_competition.phaseKey()}, {"remainingSeconds", m_competition.remainingSeconds(now)},
        {"roundRemainingSeconds", m_competition.roundRemainingSeconds(now)},
        {"roundIndex", m_competition.roundIndex()}, {"matchId", m_competition.matchId()},
        {"roundId", m_competition.roundId()}, {"stateRevision", double(m_competitionRevision)},
        {"serverTimestamp", double(QDateTime::currentMSecsSinceEpoch())},
        {"running", m_competition.phase() == CompetitionFlow::Phase::Running}, {"suddenDeath", false},
        {"redScore", m_competition.wins(1)}, {"blueScore", m_competition.wins(2)},
        {"redRemainingRevives", m_competition.remainingRevives(1)},
        {"blueRemainingRevives", m_competition.remainingRevives(2)},
        {"redRevivePending", m_competition.revivePending(1)}, {"blueRevivePending", m_competition.revivePending(2)},
        {"redName", m_broadcast ? m_broadcast->redTeamName() : tr("红方")},
        {"blueName", m_broadcast ? m_broadcast->blueTeamName() : tr("蓝方")},
        {"notice", notice}, {"ended", resultAvailable},
        {"settlement", resultAvailable ? m_lastRoundSummary.value("settlement") : QJsonValue(QString())},
        {"summary", resultAvailable ? m_lastRoundSummary : QJsonObject()}, {"summaryExpanded", false}};
}

void MainWindow::updateCompetition()
{
    if (!competitionMode() || !m_broadcast || !m_ruleProfileEdit) return;
    const qint64 now = refereeNow();
    if (m_competition.phase() == CompetitionFlow::Phase::Running) {
        const auto robots = activeMatchRobots();
        m_referee.updateRobots(robots, now);
        for (const auto &r : robots) if (r.online && r.hp >= 0)
            m_competition.observeLife(r.team, r.alive && r.hp > 0, now);
    }
    for (quint8 team : {quint8(1), quint8(2)})
        m_referee.setReviveState(team, m_competition.remainingRevives(team), m_competition.revivePending(team));
    const auto event = m_competition.tick(now);
    if (event == CompetitionFlow::Event::StartDevices) {
        QString error;
        if (!bindCompetitionRobots(&error)) { m_competition.cancelStart(); showActionResult(error, QStringLiteral("error")); }
        else if (m_noDeviceDebug) {
            // No devices to command or wait for: confirm the start immediately.
            m_competition.confirmStart(now);
            m_referee.beginRound({}, now);
            for (quint8 side : {quint8(1), quint8(2)}) m_referee.setReviveState(side, 3, false);
            m_lastRoundSummary = {};
            m_broadcast->clearMatchSummary(); m_broadcast->clearSettlement(); m_broadcast->clearCards();
            persistCompetitionLedger();
            showActionResult(tr("[无车调试] 小局开始 · 180 秒 · 未发送设备命令，需裁判手动判定"), QStringLiteral("info"));
        } else {
            m_startAcknowledged.clear();
            m_startRequestEpochMs = QDateTime::currentMSecsSinceEpoch(); m_startRequestMonotonicMs = now;
            sendGameCommand(true);
        }
    }
    if (m_competition.phase() == CompetitionFlow::Phase::Starting) {
        bool ready = m_startAcknowledged.size() == 2;
        const auto robots = activeMatchRobots();
        ready = ready && robots.size() == 2;
        for (const auto &r : robots)
            ready = ready && m_startAcknowledged.value(r.robotId) && r.online && r.alive
                    && r.hp == 300 && r.lastSeenMs >= m_startRequestEpochMs;
        if (ready && m_competition.confirmStart(now)) {
            m_referee.beginRound(robots, now);
            for (quint8 side : {quint8(1), quint8(2)}) m_referee.setReviveState(side, 3, false);
            m_lastRoundSummary = {};
            m_broadcast->clearMatchSummary(); m_broadcast->clearSettlement(); m_broadcast->clearCards();
            persistCompetitionLedger();
            showActionResult(tr("双方设备已确认，正式小局开始 · 180 秒"));
        } else if (now - m_startRequestMonotonicMs > 3000) {
            m_competition.cancelStart(); sendGameCommand(false);
            showActionResult(tr("设备开赛状态未确认，已发送结束命令，请检查后重新开赛"), QStringLiteral("error"));
        }
    }
    if (event == CompetitionFlow::Event::TimeExpired) {
        m_commander->cancelPendingType(proto::TypeReviveApprove);
        m_referee.timeExpired(now);
        sendGameCommand(false);
        persistCompetitionLedger();
    }
    if (event == CompetitionFlow::Event::PreparationComplete)
        showActionResult(tr("2 分钟局间准备结束，请裁判确认后开始下一局"));
    ++m_competitionRevision;
    refreshCompetitionUi();
    publishMatchState();
}

void MainWindow::refreshCompetitionUi()
{
    if (!m_competitionPhaseLabel || !m_broadcast) return;
    const bool formal = competitionMode();
    m_competitionPhaseLabel->setVisible(formal);
    m_redRevivesLabel->setVisible(formal); m_blueRevivesLabel->setVisible(formal);
    if (!formal) return;
    const int seconds = m_competition.remainingSeconds(refereeNow());
    m_competitionPhaseLabel->setText((m_noDeviceDebug ? tr("[无车调试] ") : QString())
        + tr("BO3 · 第 %1 局 · %2 · %3:%4")
        .arg(m_competition.roundIndex()).arg(m_competition.phaseText())
        .arg(seconds/60,2,10,QLatin1Char('0')).arg(seconds%60,2,10,QLatin1Char('0')));
    m_redRevivesLabel->setText(tr("红方剩余复活：%1 / 3%2").arg(m_competition.remainingRevives(1))
        .arg(m_competition.revivePending(1) ? tr(" · 确认中") : QString()));
    m_blueRevivesLabel->setText(tr("蓝方剩余复活：%1 / 3%2").arg(m_competition.remainingRevives(2))
        .arg(m_competition.revivePending(2) ? tr(" · 确认中") : QString()));
    const bool locked = m_competition.locked();
    const auto phase = m_competition.phase();
    m_matchBtn->setEnabled(phase == CompetitionFlow::Phase::Ready || phase == CompetitionFlow::Phase::Cancelled || phase == CompetitionFlow::Phase::Countdown);
    m_matchBtn->setText(phase == CompetitionFlow::Phase::Countdown ? tr("取消开赛倒计时") : tr("开始本局 · 5秒倒计时"));
    m_resetMatchBtn->setEnabled(!locked); m_resetMatchBtn->setText(tr("新建 / 重置 BO3"));
    m_importTeamsBtn->setEnabled(!locked); m_nextTeamBtn->setEnabled(!locked && !m_teamPairs.isEmpty());
    m_applyTeamPairBtn->setEnabled(!locked && !m_teamPairs.isEmpty());
    m_redTeamNameEdit->setReadOnly(locked); m_blueTeamNameEdit->setReadOnly(locked);
    m_ruleProfileEdit->setEnabled(!locked);
    if (m_noDeviceDebugEdit)
        m_noDeviceDebugEdit->setEnabled(phase != CompetitionFlow::Phase::Countdown && phase != CompetitionFlow::Phase::Starting
            && phase != CompetitionFlow::Phase::Running && phase != CompetitionFlow::Phase::AwaitingDecision);
    m_broadcast->setCompetitionState(competitionSnapshot());
}

void MainWindow::saveRoundResult(const QJsonObject &summary)
{
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope,
                       QStringLiteral("Scompetition"), QStringLiteral("RoundResults"));
    const QString base = settings.format() == QSettings::IniFormat
        ? QFileInfo(settings.fileName()).absolutePath()
        : QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    const QString folder = QDir(base).filePath(QStringLiteral("round-results"));
    QDir().mkpath(folder);
    const QString id = summary.value("roundId").toString(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSaveFile file(QDir(folder).filePath(id + QStringLiteral(".json")));
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(summary).toJson()) < 0 || !file.commit()) {
        onLogMessage(tr("[归档失败] 本局数据仍可在裁判面板复制，请立即人工保存")); return;
    }
    onLogMessage(tr("[裁判数据] 本局结果已归档：%1").arg(file.fileName()));
}

void MainWindow::onToggleMatch()
{
    if (!m_broadcast) return;
    if (competitionMode()) {
        if (m_competition.phase() == CompetitionFlow::Phase::Countdown) {
            m_competition.cancelCountdown();
            showActionResult(tr("已取消开赛倒计时，未发送开赛指令"));
            updateCompetition(); return;
        }
        QString error;
        if (m_competition.phase() != CompetitionFlow::Phase::Ready
            && m_competition.phase() != CompetitionFlow::Phase::Cancelled) {
            showActionResult(tr("当前阶段不能开赛；局间准备结束后由裁判确认，无技术暂停"), QStringLiteral("info")); return;
        }
        if (!m_ledgerHealthy || !bindCompetitionRobots(&error)) {
            showActionResult(m_ledgerHealthy ? error : tr("审批账本异常，禁止正式开赛"), QStringLiteral("error")); return;
        }
        m_competition.beginCountdown(refereeNow());
        m_broadcast->clearMatchSummary(); m_broadcast->clearSettlement();
        persistCompetitionLedger(); updateCompetition();
        showActionResult(tr("5 秒开赛倒计时已开始；归零后等待双方设备确认"));
        return;
    }

    const qint64 now = refereeNow();
    if (m_broadcast->isMatchRunning()) {
        m_broadcast->pauseMatch();
        m_referee.pause(now);
        showActionResult(tr("比赛已暂停 · 未向机器人发送重置命令"));
        // V2: 比赛暂停只影响 HUD，不向 ESP32 发命令
    } else if (m_referee.phase() == MatchReferee::Phase::Paused
               && !m_broadcast->roundEnded()
               && (m_broadcast->remainingSeconds() > 0 || m_referee.suddenDeath())) {
        // Resume: GAME_START would reset the robots to full HP mid-round.
        m_broadcast->startMatch();
        m_referee.resume(now);
        showActionResult(tr("比赛已继续"));
    } else if (m_referee.isRunning() && m_broadcast->remainingSeconds() == 0) {
        // Buzzer confirmation/SD is still this round, never send GAME_START.
        showActionResult(tr("正在进行结束判定 / 加时，无需重复开始"), QStringLiteral("info"));
        return;
    } else {
        m_broadcast->startMatch();
        m_referee.beginRound(m_robots ? m_robots->robots()
                                      : QVector<RobotManager::RobotInfo>(), now);
        if (m_summaryToggleBtn) {
            m_summaryToggleBtn->setEnabled(false);
            m_summaryToggleBtn->setText(tr("展开赛后数据"));
        }
        // V2: 对所有已分配 robot_id 单播 GAME_START，ESP32 收到后自设 HP=300
        sendGameCommand(true);
    }
    syncRefereeState();
    refreshRefereeStatus();
    publishMatchState();
}

void MainWindow::sendGameCommand(bool start)
{
    if (!m_commander || !m_robots)
        return;
    if (m_actionCommands.isEmpty())
        m_commandTotal = m_commandConfirmed = m_commandFailed = 0;
    m_collectingCommandBatch = true;
    int sent = 0;
    int eligible = 0;
    for (const auto &robot : m_robots->robots()) {
        if (competitionMode() && !m_activeDeviceMacs.contains(robot.robotId)) continue;
        if (robot.protocolVersion == proto::kVersion2 && robot.robotId != 0
            && robot.online) {
            ++eligible;
            const quint32 txid = start ? m_commander->sendGameStart(robot.robotId)
                                       : m_commander->sendGameEnd(robot.robotId);
            if (competitionMode() && start) m_startAcknowledged[robot.robotId] = false;
            trackCommand(txid, start ? tr("开始比赛") : tr("结束比赛"));
            if (txid) ++sent;
        }
    }
    m_collectingCommandBatch = false;
    if (eligible == 0)
        reportAction(start ? tr("本地比赛已开始 · 无在线已分配 V2 机器人，未发送设备命令")
                           : tr("本地比赛已结束 · 无在线已分配 V2 机器人，未发送设备命令"), QStringLiteral("info"));
    if (sent > 0)
        onLogMessage(QStringLiteral("[比赛] 已向 %1 台 V2 机器人发送 %2")
                         .arg(sent)
                         .arg(start ? QStringLiteral("GAME_START") : QStringLiteral("GAME_END")));
}

void MainWindow::onTerminateMatch()
{
    if (!m_broadcast) return;
    if (competitionMode() && (m_competition.phase() == CompetitionFlow::Phase::Countdown
        || m_competition.phase() == CompetitionFlow::Phase::Starting)) {
        if (!m_competition.cancelCountdown()) { m_competition.cancelStart(); sendGameCommand(false); }
        updateCompetition(); return;
    }
    if (m_broadcast->roundEnded()) return;
    m_referee.terminate(refereeNow());
    applyVerdict();
}

void MainWindow::onDeclareRedWin()
{
    if (!m_broadcast || m_broadcast->roundEnded())
        return;
    m_referee.declareWinner(MatchReferee::kRed, refereeNow());
    applyVerdict();
}

void MainWindow::onDeclareBlueWin()
{
    if (!m_broadcast || m_broadcast->roundEnded())
        return;
    m_referee.declareWinner(MatchReferee::kBlue, refereeNow());
    applyVerdict();
}

void MainWindow::onTimeExpired()
{
    if (competitionMode()) {
        // Only the server flow owns the official deadline. HUD expiry is display-only.
        if (m_competition.phase() != CompetitionFlow::Phase::AwaitingDecision) return;
        for (quint8 side : {quint8(1), quint8(2)})
            m_referee.setReviveState(side, m_competition.remainingRevives(side), m_competition.revivePending(side));
        m_commander->cancelPendingType(proto::TypeReviveApprove);
        sendGameCommand(false);
    }
    m_referee.timeExpired(refereeNow());
    onRefereeTick();
}

void MainWindow::onRefereeTick()
{
    if (competitionMode()) updateCompetition();
    if (m_referee.tick(refereeNow())) {
        applyVerdict();
        return;
    }
    if (competitionMode() && m_referee.awaitingDecision()) {
        if (m_competition.phase() == CompetitionFlow::Phase::Running) {
            m_competition.awaitDecision(refereeNow());
            m_commander->cancelPendingType(proto::TypeReviveApprove);
            sendGameCommand(false); persistCompetitionLedger();
        }
        QJsonObject pending = m_referee.summaryJson();
        pending.insert("redName", m_broadcast->redTeamName()); pending.insert("blueName", m_broadcast->blueTeamName());
        pending.insert("matchId", m_competition.matchId()); pending.insert("roundId", m_competition.roundId());
        pending.insert("roundIndex", m_competition.roundIndex());
        pending.insert("reasonText", m_referee.liveNotice());
        if (m_refereeEntryPanel) m_refereeEntryPanel->setResult(pending);
        refreshCompetitionUi(); publishMatchState();
    }
    syncRefereeState();
    refreshRefereeStatus();
}

void MainWindow::syncRefereeState()
{
    if (!m_broadcast) return;
    const int red = competitionMode() ? m_competition.wins(1) : m_referee.team(MatchReferee::kRed).kills;
    const int blue = competitionMode() ? m_competition.wins(2) : m_referee.team(MatchReferee::kBlue).kills;
    {
        const QSignalBlocker redBlock(m_redScoreEdit), blueBlock(m_blueScoreEdit);
        m_redScoreEdit->setValue(red);
        m_blueScoreEdit->setValue(blue);
    }
    if (m_broadcast->redScore() != red || m_broadcast->blueScore() != blue)
        m_broadcast->setScores(red, blue);
    if (m_referee.suddenDeath() && !m_broadcast->suddenDeath()
        && m_referee.phase() != MatchReferee::Phase::Finished) {
        m_broadcast->setMatchState(0, m_referee.isRunning(), true);
        onLogMessage(tr("[判定] 常规时间同分，进入 SUDDEN DEATH · 一命定胜负"));
    }
    if (m_referee.isRunning())
        m_broadcast->setRefereeNotice(m_referee.liveNotice());
}

void MainWindow::applyVerdict()
{
    // tick() reports a verdict exactly once; drain it so a verdict produced
    // directly by terminate()/declareWinner() is not reported again.
    m_referee.tick(refereeNow());
    const MatchReferee::Verdict &verdict = m_referee.verdict();
    if (!verdict.valid || !m_broadcast || m_broadcast->roundEnded())
        return;

    syncRefereeState(); // The final kill is the point; no extra winner point.
    // Order matters: the settlement clip must be running before the summary
    // arrives, so the panel waits for the clip instead of covering it.
    m_broadcast->concludeRound(verdict.settlementType);
    QJsonObject summary = m_referee.summaryJson();
    summary.insert(QStringLiteral("redName"), m_broadcast->redTeamName());
    summary.insert(QStringLiteral("blueName"), m_broadcast->blueTeamName());
    if (competitionMode()) {
        m_commander->cancelPendingType(proto::TypeReviveApprove);
        m_competition.finishRound(verdict.winner, refereeNow());
        summary.insert(QStringLiteral("matchId"), m_competition.matchId());
        summary.insert(QStringLiteral("roundId"), m_competition.roundId());
        summary.insert(QStringLiteral("roundIndex"), m_competition.roundIndex());
        summary.insert(QStringLiteral("redSeriesWins"), m_competition.wins(1));
        summary.insert(QStringLiteral("blueSeriesWins"), m_competition.wins(2));
        persistCompetitionLedger();
        syncRefereeState();
    }
    m_lastRoundSummary = summary;
    m_broadcast->setMatchSummary(summary);
    if (m_refereeEntryPanel) m_refereeEntryPanel->setResult(summary);
    saveRoundResult(summary);
    if (competitionMode() && m_controlTabs && m_refereeEntryTabIndex >= 0) {
        m_controlTabs->setCurrentIndex(m_refereeEntryTabIndex);
        if (m_compactTabs->isVisible()) m_compactTabs->setCurrentWidget(m_controlTabs);
    }
    sendGameCommand(false);
    onLogMessage(tr("[判定] %1（用时 %2 秒）")
                     .arg(verdict.reasonText)
                     .arg(verdict.elapsedMs / 1000.0, 0, 'f', 1));
    if (m_summaryToggleBtn) {
        m_summaryToggleBtn->setEnabled(true);
        m_summaryToggleBtn->setText(tr("裁判录入数据"));
    }
    if (competitionMode()) refreshCompetitionUi();
    refreshRefereeStatus();
    publishMatchState();
}

void MainWindow::onToggleSummary()
{
    if (competitionMode()) {
        if (m_controlTabs && m_refereeEntryTabIndex >= 0) {
            m_controlTabs->setCurrentIndex(m_refereeEntryTabIndex);
            if (m_compactTabs->isVisible()) m_compactTabs->setCurrentWidget(m_controlTabs);
        }
        showActionResult(tr("已显示本局裁判录入数据，可复制到外部成绩系统")); return;
    }
    if (!m_broadcast || m_broadcast->matchSummary().isEmpty())
        return;
    const bool expand = !m_broadcast->summaryExpanded();
    m_broadcast->setSummaryExpanded(expand);
    showActionResult(expand ? tr("赛后数据已展开") : tr("赛后数据已收起"));
    if (m_summaryToggleBtn)
        m_summaryToggleBtn->setText(expand ? tr("收起赛后数据") : tr("展开赛后数据"));
    publishMatchState();
}

void MainWindow::loadRefereeRules()
{
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope,
                       QStringLiteral("Scompetition"), QStringLiteral("MatchReferee"));
    const MatchReferee::Rules defaults;
    const QSignalBlocker bp(m_ruleProfileEdit);
    m_useCompetition = settings.value(QStringLiteral("competitionMode"), true).toBool();
    m_ruleProfileEdit->setCurrentIndex(m_useCompetition ? 0 : 1);
    const QSignalBlocker b2(m_redCardDisqualifiesEdit);
    const QSignalBlocker b4(m_offlineGraceEdit);
    m_redCardDisqualifiesEdit->setChecked(
        settings.value(QStringLiteral("redCardDisqualifies"), defaults.redCardDisqualifies).toBool());
    m_offlineGraceEdit->setValue(m_useCompetition ? 0 : settings.value(QStringLiteral("offlineForfeitSeconds"), defaults.offlineForfeitMs / 1000).toInt());
    onRefereeRulesChanged();
}

void MainWindow::onRefereeRulesChanged()
{
    if (!m_redCardDisqualifiesEdit || !m_offlineGraceEdit) {
        return;
    }
    const bool requested = m_ruleProfileEdit ? m_ruleProfileEdit->currentData().toBool() : true;
    if (m_competition.locked() || m_referee.isRunning() || m_referee.phase() == MatchReferee::Phase::Paused) {
        const QSignalBlocker blocker(m_ruleProfileEdit);
        m_ruleProfileEdit->setCurrentIndex(m_useCompetition ? 0 : 1);
        showActionResult(tr("比赛中不能切换规则"), QStringLiteral("error")); return;
    }
    const bool changed = requested != m_useCompetition;
    m_useCompetition = requested;
    if (changed) {
        m_competition.resetSeries(); m_referee.reset(); m_activeRobotIds.clear(); m_activeDeviceMacs.clear();
        if (m_broadcast) m_broadcast->resetMatch();
        if (m_matchServer) m_matchServer->clearRegistrations(tr("比赛规则模式已变更，请重新登记"));
    }
    MatchReferee::Rules rules = m_referee.rules();
    rules.scoringMode = competitionMode() ? MatchReferee::ScoringMode::RemainingResources : MatchReferee::ScoringMode::KillScore;
    rules.redCardDisqualifies = m_redCardDisqualifiesEdit->isChecked();
    rules.offlineForfeitMs = competitionMode() ? 0 : m_offlineGraceEdit->value() * 1000;
    m_offlineGraceEdit->setEnabled(!competitionMode());
    m_redScoreEdit->setReadOnly(competitionMode()); m_blueScoreEdit->setReadOnly(competitionMode());
    if (m_redScoreLabel) m_redScoreLabel->setText(competitionMode() ? tr("红方 BO3 胜局") : tr("红方击杀积分"));
    if (m_blueScoreLabel) m_blueScoreLabel->setText(competitionMode() ? tr("蓝方 BO3 胜局") : tr("蓝方击杀积分"));
    if (m_broadcast) m_broadcast->setClockDuration(competitionMode() ? 180 : 60);
    m_referee.setRules(rules);

    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope,
                       QStringLiteral("Scompetition"), QStringLiteral("MatchReferee"));
    settings.setValue(QStringLiteral("competitionMode"), competitionMode());
    settings.setValue(QStringLiteral("redCardDisqualifies"), rules.redCardDisqualifies);
    settings.setValue(QStringLiteral("offlineForfeitSeconds"), m_offlineGraceEdit->value());
    settings.sync();
    reportAction(settings.status() == QSettings::NoError ? tr("裁判规则已应用并保存") : tr("裁判规则已应用，但保存失败"),
                 settings.status() == QSettings::NoError ? QStringLiteral("success") : QStringLiteral("error"));
    refreshCompetitionUi();
    refreshRefereeStatus();
}

void MainWindow::onNoDeviceDebugToggled(bool enabled)
{
    if (enabled == m_noDeviceDebug) return;
    // Allowed between rounds (intermission included, the series is reset);
    // blocked only while a round is live.
    const auto phase = m_competition.phase();
    const bool liveRound = phase == CompetitionFlow::Phase::Countdown || phase == CompetitionFlow::Phase::Starting
        || phase == CompetitionFlow::Phase::Running || phase == CompetitionFlow::Phase::AwaitingDecision;
    if (liveRound || m_referee.isRunning() || m_referee.phase() == MatchReferee::Phase::Paused) {
        const QSignalBlocker blocker(m_noDeviceDebugEdit);
        m_noDeviceDebugEdit->setChecked(m_noDeviceDebug);
        showActionResult(tr("比赛中不能切换无车调试"), QStringLiteral("error")); return;
    }
    m_noDeviceDebug = enabled;
    // Debug and real rounds must never share one BO3 score / revival ledger.
    m_commander->cancelPendingType(proto::TypeReviveApprove);
    m_competition.resetSeries(); m_referee.reset(); m_activeRobotIds.clear(); m_activeDeviceMacs.clear();
    persistCompetitionLedger();
    if (m_broadcast) m_broadcast->resetMatch();
    onLogMessage(enabled ? tr("[调试] 已开启无车调试：开赛跳过设备检查，不发送设备命令；BO3 已重置")
                         : tr("[调试] 已关闭无车调试，恢复正式设备检查；BO3 已重置"));
    showActionResult(enabled ? tr("无车调试已开启 · 仅用于测试，正式比赛前请关闭")
                             : tr("无车调试已关闭 · 开赛需双方设备在线"),
                     enabled ? QStringLiteral("info") : QStringLiteral("success"));
    syncRefereeState();
    refreshCompetitionUi();
    refreshRefereeStatus();
    publishMatchState();
}

void MainWindow::refreshRefereeStatus()
{
    if (!m_refereeStatusLabel)
        return;
    const auto side = [this](quint8 team) {
        const MatchReferee::TeamStats &s = m_referee.team(team);
        const QString hp = s.hp >= 0 && s.maxHp > 0
                               ? QStringLiteral("%1%").arg(qRound(s.hp * 100.0 / s.maxHp))
                               : QStringLiteral("--");
        QString state = !s.online ? tr("离线") : (s.alive ? tr("存活") : tr("阵亡"));
        if (competitionMode()) return tr("剩余复活 %1 / 3%2 · HP %3 · %4")
            .arg(m_competition.remainingRevives(team))
            .arg(m_competition.revivePending(team) ? tr("（确认中）") : QString())
            .arg(s.hp >= 0 ? QString::number(s.hp) : tr("未知"), state);
        return tr("击杀积分 %1 · HP %2 · 伤害 %3 · %4")
                   .arg(s.kills).arg(hp).arg(m_referee.damageDealt(team)).arg(state);
    };
    QString headline;
    switch (m_referee.phase()) {
    case MatchReferee::Phase::Idle: headline = tr("等待开始"); break;
    case MatchReferee::Phase::Running:
        headline = m_referee.suddenDeath() ? tr("SUDDEN DEATH · 一命定胜负") : tr("击杀积分赛"); break;
    case MatchReferee::Phase::Paused: headline = tr("已暂停"); break;
    case MatchReferee::Phase::Finished: headline = m_referee.verdict().reasonText; break;
    case MatchReferee::Phase::AwaitingDecision: headline = m_referee.liveNotice(); break;
    }
    if (competitionMode() && m_referee.phase() != MatchReferee::Phase::Finished) headline = m_competition.phaseText();
    const QString notice = (m_referee.isRunning() || m_referee.awaitingDecision()) ? m_referee.liveNotice() : QString();
    const QString text = tr("<b>%1</b>%2<br><span style='color:#d0404c'>红方</span> %3"
                            "<br><span style='color:#2f7fd8'>蓝方</span> %4")
                             .arg(headline.toHtmlEscaped(),
                                  notice.isEmpty() ? QString()
                                                   : QStringLiteral(" · <span style='color:#b86a12'>%1</span>")
                                                         .arg(notice.toHtmlEscaped()),
                                  side(MatchReferee::kRed).toHtmlEscaped(),
                                  side(MatchReferee::kBlue).toHtmlEscaped());
    if (m_refereeStatusLabel->text() != text)
        m_refereeStatusLabel->setText(text);
    if (m_matchOverview && m_broadcast)
        m_matchOverview->setText(tr("%1  %2  :  %3  %4  ·  %5  ·  %6 秒")
            .arg(m_broadcast->redTeamName()).arg(m_broadcast->redScore()).arg(m_broadcast->blueScore())
            .arg(m_broadcast->blueTeamName(),headline).arg(m_broadcast->remainingSeconds()));
    const bool live = m_referee.phase() == MatchReferee::Phase::Running
                      || m_referee.phase() == MatchReferee::Phase::Paused
                       || m_referee.awaitingDecision();
    if (m_declareRedBtn)
        m_declareRedBtn->setEnabled(live);
    if (m_declareBlueBtn)
        m_declareBlueBtn->setEnabled(live);
}

void MainWindow::onResetMatch()
{
    if (competitionMode() && m_competition.locked()) {
        showActionResult(tr("请先终止当前小局；BO3 局间准备不能误重置或换队"), QStringLiteral("error")); return;
    }
    if (competitionMode()) {
        m_commander->cancelPendingType(proto::TypeReviveApprove);
        m_competition.resetSeries(); m_activeRobotIds.clear(); m_activeDeviceMacs.clear();
        persistCompetitionLedger();
    }
    if (m_broadcast)
        m_broadcast->resetMatch();
    m_referee.reset();
    showActionResult(tr("比赛已重置 · 等待下一局"));
    if (m_summaryToggleBtn) {
        m_summaryToggleBtn->setEnabled(false);
        m_summaryToggleBtn->setText(tr("展开赛后数据"));
    }
    if (m_redScoreEdit)
        m_redScoreEdit->setValue(0);
    if (m_blueScoreEdit)
        m_blueScoreEdit->setValue(0);
    refreshCompetitionUi();
    refreshRefereeStatus();
    publishMatchState();
}

void MainWindow::onTestVictoryAnimation()
{
    if (!m_broadcast)
        return;

    if (!m_broadcast->isVisible())
        m_broadcast->showOnScreen(selectedScreen());

    const QString type = m_settlementPreviewTypeEdit
                             ? m_settlementPreviewTypeEdit->currentData().toString()
                             : QStringLiteral("redwin");
    m_broadcast->playSettlementPreview(type);
    const QString name = m_settlementPreviewTypeEdit ? m_settlementPreviewTypeEdit->currentText()
                                                     : tr("红方胜利");
    reportAction(tr("[动画] 已开始播放%1动画预览").arg(name));
}

void MainWindow::onToggleClientServer()
{
    if (!m_matchServer)
        return;

    if (m_matchServer->isListening())
        stopClientServer();
    else
        startClientServer();
}

void MainWindow::startClientServer()
{
    if (!m_matchServer || !m_clientPortEdit)
        return;

    const quint16 port = static_cast<quint16>(m_clientPortEdit->value());
    if (!m_matchServer->start(port)) {
        showActionResult(tr("选手端登记服务启动失败 · 端口 %1 可能已被占用").arg(port), QStringLiteral("error"));
        return;
    }

    showActionResult(tr("选手端登记服务已启动 · TCP %1").arg(port));
    m_clientPortEdit->setEnabled(false);
}

void MainWindow::stopClientServer()
{
    if (!m_matchServer)
        return;

    m_matchServer->stop();
    showActionResult(tr("选手端登记服务已停止"));
    if (m_clientPortEdit)
        m_clientPortEdit->setEnabled(true);
}

void MainWindow::publishMatchState()
{
    if (!m_matchServer || !m_broadcast)
        return;

    if (competitionMode()) {
        ++m_competitionRevision;
        m_matchServer->publishMatchState(competitionSnapshot());
        return;
    }
    m_matchServer->publishMatchState({
        {QStringLiteral("type"), QStringLiteral("match_state")},
        {QStringLiteral("remainingSeconds"), m_broadcast->remainingSeconds()},
        {QStringLiteral("running"), m_broadcast->isMatchRunning()},
        {QStringLiteral("suddenDeath"), m_broadcast->suddenDeath()},
        {QStringLiteral("scoringRule"), QStringLiteral("kills")},
        {QStringLiteral("redScore"), m_broadcast->redScore()},
        {QStringLiteral("blueScore"), m_broadcast->blueScore()},
        {QStringLiteral("ended"), m_broadcast->roundEnded()},
        {QStringLiteral("settlement"), m_broadcast->settlementType()},
        {QStringLiteral("redName"), m_broadcast->redTeamName()},
        {QStringLiteral("blueName"), m_broadcast->blueTeamName()},
        {QStringLiteral("notice"), m_broadcast->refereeNotice()},
        {QStringLiteral("summary"), m_broadcast->roundEnded() ? m_broadcast->matchSummary()
                                                              : QJsonObject()},
        {QStringLiteral("summaryExpanded"), m_broadcast->summaryExpanded()}
    });
}

void MainWindow::refreshFieldCameraDevices()
{
    if (!m_fieldCameraEdit || !m_fieldCamera) return;
    const QSignalBlocker blocker(m_fieldCameraEdit);
    const QByteArray selected = m_fieldCamera->selectedDevice();
    m_fieldCameraEdit->clear();
    m_fieldCameraEdit->addItem(tr("请选择摄像头"), QByteArray());
    for (const auto &device : m_fieldCamera->devices())
        m_fieldCameraEdit->addItem(device.name, device.id);
    int index = m_fieldCameraEdit->findData(selected);
    if (!selected.isEmpty() && index < 0) {
        m_fieldCameraEdit->addItem(tr("上次所选设备（未连接）"), selected);
        index = m_fieldCameraEdit->count() - 1;
    }
    m_fieldCameraEdit->setCurrentIndex(qMax(0, index));
    m_fieldCameraEdit->setToolTip(tr("按设备 ID 区分同名摄像头；未检测到设备时，请检查 USB 连接及 Windows 摄像头权限。"));
}

void MainWindow::onFieldCameraSelected()
{
    if (!m_fieldCamera || !m_fieldCameraEdit) return;
    const QByteArray id = m_fieldCameraEdit->currentData().toByteArray();
    m_fieldCamera->selectDevice(id);
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope,
                       QStringLiteral("Scompetition"), QStringLiteral("FieldCamera"));
    settings.setValue(QStringLiteral("deviceId"), id);
}

void MainWindow::onToggleFieldCamera()
{
    if (m_fieldCamera->requested()) m_fieldCamera->stop();
    else m_fieldCamera->start();
}

void MainWindow::onFieldCameraStateChanged()
{
    if (!m_fieldCameraStatus) return;
    m_fieldCameraStatus->setText(m_fieldCamera->status());
    const QString status = m_fieldCamera->status();
    const bool failed = status.contains(tr("失败")) || status.contains(tr("错误")) || status.contains(tr("请检查"));
    showActionResult(status, failed ? QStringLiteral("error")
                     : !m_fieldCamera->requested() ? QStringLiteral("info")
                     : m_fieldCamera->live() ? QStringLiteral("success") : QStringLiteral("pending"));
    m_fieldCameraToggle->setText(m_fieldCamera->requested() ? tr("停止采集") : tr("启动采集"));
    updateProgramSourceList();
    rebuildSourcePreviewStrip();
}

void MainWindow::onProgramModeChanged(int)
{
    const bool automatic = m_programModeEdit
                               && m_programModeEdit->currentData().toBool();
    if (m_autoSwitchIntervalEdit)
        m_autoSwitchIntervalEdit->setEnabled(automatic);
    if (!automatic || !m_autoSwitchTimer || !m_programSourceEdit
        || m_programSourceEdit->count() < 2) {
        if (m_autoSwitchTimer)
            m_autoSwitchTimer->stop();
        return;
    }

    m_autoSwitchTimer->start((m_autoSwitchIntervalEdit
                                  ? m_autoSwitchIntervalEdit->value()
                                  : 5) * 1000);
}

void MainWindow::onProgramSourceChanged(int)
{
    if (!m_broadcast || !m_programSourceEdit || m_programSourceEdit->currentIndex() < 0)
        return;

    const QString sourceId = m_programSourceEdit->currentData().toString();
    const QString sourceTitle = m_programSourceEdit->currentText();
    m_broadcast->setActiveSource(sourceId, sourceTitle);
    refreshSourcePreviewBadges();
    onSourceFrameUpdated(sourceId);

    if (m_programModeEdit && m_programModeEdit->currentData().toBool()
        && m_autoSwitchTimer && !m_autoSwitchTimer->isActive()) {
        onProgramModeChanged(m_programModeEdit->currentIndex());
    }
}

void MainWindow::onAutoSwitchTimeout()
{
    if (!m_programSourceEdit || m_programSourceEdit->count() < 2)
        return;

    const int count = m_programSourceEdit->count();
    for (int step = 1; step <= count; ++step) {
        const int next = (m_programSourceEdit->currentIndex() + step) % count;
        const QString id = m_programSourceEdit->itemData(next).toString();
        if (id == QStringLiteral("field") && (!m_fieldCamera || !m_fieldCamera->live()))
            continue; // A placeholder/offline local camera must not blank auto-program.
        m_programSourceEdit->setCurrentIndex(next);
        break;
    }
}

void MainWindow::onTickerPresetChanged(int index)
{
    if (!m_tickerPresetEdit || !m_tickerTextEdit || index < 0)
        return;

    const QString preset = m_tickerPresetEdit->itemData(index).toString();
    if (!preset.isEmpty())
        m_tickerTextEdit->setText(preset);
    else
        m_tickerTextEdit->clear();
}

void MainWindow::onApplyTicker()
{
    if (!m_broadcast || !m_tickerTextEdit)
        return;

    const QString text = m_tickerTextEdit->text().trimmed();
    if (text.isEmpty()) {
        reportAction(tr("[字幕] 发布失败：字幕内容不能为空"), QStringLiteral("error"));
        return;
    }

    m_broadcast->setTickerText(text);
    m_broadcast->setTickerVisible(true);
    if (m_tickerToggleBtn)
        m_tickerToggleBtn->setText(tr("隐藏字幕"));
    reportAction(tr("[字幕] 已发布：%1").arg(text));
}

void MainWindow::onToggleTicker()
{
    if (!m_broadcast)
        return;

    const bool visible = !m_broadcast->tickerVisible();
    if (visible && m_broadcast->tickerText().trimmed().isEmpty()) {
        reportAction(tr("[字幕] 请先输入或选择字幕内容"), QStringLiteral("error"));
        return;
    }

    m_broadcast->setTickerVisible(visible);
    showActionResult(visible ? tr("字幕已显示") : tr("字幕已隐藏"));
    if (m_tickerToggleBtn)
        m_tickerToggleBtn->setText(visible ? tr("隐藏字幕") : tr("显示字幕"));
}

void MainWindow::onTeamNamesChanged()
{
    if (competitionMode() && m_competition.locked()) {
        m_redTeamNameEdit->setText(m_broadcast->redTeamName());
        m_blueTeamNameEdit->setText(m_broadcast->blueTeamName());
        showActionResult(tr("当前 BO3 不能更换参赛队伍"), QStringLiteral("error")); return;
    }
    if (!m_broadcast)
        return;

    const QString redName = m_redTeamNameEdit ? m_redTeamNameEdit->text() : tr("红方");
    const QString blueName = m_blueTeamNameEdit ? m_blueTeamNameEdit->text() : tr("蓝方");
    if (redName.trimmed().isEmpty() || blueName.trimmed().isEmpty() || redName.trimmed() == blueName.trimmed()) {
        showActionResult(tr("请选择两个不同的非空队伍名称"), QStringLiteral("error")); return;
    }
    if (competitionMode() && (redName != m_broadcast->redTeamName() || blueName != m_broadcast->blueTeamName())) {
        onResetMatch();
        if (m_matchServer) m_matchServer->clearRegistrations(tr("对阵已变更，请重新登记"));
    }
    m_broadcast->setTeamNames(redName, blueName);
    reportAction(tr("队名已应用 · %1 对 %2").arg(m_broadcast->redTeamName(), m_broadcast->blueTeamName()));
    updateProgramSourceList();
    updateTeamPairControls();
    publishMatchState();
}

void MainWindow::onImportTeamTable()
{
    if (competitionMode() && m_competition.locked()) {
        showActionResult(tr("当前 BO3 期间不能导入并替换对阵"), QStringLiteral("error")); return;
    }
    const QString filePath = QFileDialog::getOpenFileName(
        this,
        tr("导入队伍名称表"),
        QCoreApplication::applicationDirPath(),
        tr("队伍名称表 (*.csv *.tsv *.txt *.json);;CSV 文件 (*.csv);;TSV 文件 (*.tsv);;JSON 文件 (*.json);;所有文件 (*.*)"));
    if (filePath.isEmpty())
        return;

    QString errorMessage;
    if (!importTeamTable(filePath, &errorMessage)) {
        QMessageBox::warning(this, tr("导入失败"), errorMessage);
        reportAction(tr("[队伍表] 导入失败: %1").arg(errorMessage), QStringLiteral("error"));
        return;
    }

    updateTeamPairControls();
    refreshCompetitionUi();
    reportAction(tr("[队伍表] 已导入 %1 组，请手动选择并应用对阵: %2")
                     .arg(m_teamPairs.size())
                     .arg(QFileInfo(filePath).fileName()));
}

void MainWindow::onNextTeamPair()
{
    if (m_teamPairs.isEmpty()) {
        reportAction(tr("[队伍表] 尚未导入队伍名称表"), QStringLiteral("error"));
        return;
    }

    const int nextIndex = m_currentTeamPairIndex < 0
                              ? 0
                              : (m_currentTeamPairIndex + 1) % m_teamPairs.size();
    applyTeamPair(nextIndex, true);
    reportAction(tr("[队伍表] 已切换到第 %1 / %2 组")
                     .arg(nextIndex + 1)
                     .arg(m_teamPairs.size()));
}

bool MainWindow::importTeamTable(const QString &filePath, QString *errorMessage)
{
    if (competitionMode() && m_competition.locked()) {
        if (errorMessage) *errorMessage = tr("当前 BO3 已锁定对阵"); return false;
    }
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage)
            *errorMessage = tr("无法打开文件: %1").arg(file.errorString());
        return false;
    }

    const QByteArray data = file.readAll();
    const QString suffix = QFileInfo(filePath).suffix().toLower();
    QVector<TeamPair> parsedPairs;

    if (suffix == QStringLiteral("json")) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            if (errorMessage)
                *errorMessage = tr("JSON 格式错误: %1").arg(parseError.errorString());
            return false;
        }

        QJsonArray entries;
        if (document.isArray()) {
            entries = document.array();
        } else if (document.isObject()) {
            const QJsonObject root = document.object();
            for (const QString &key : {QStringLiteral("teams"),
                                       QStringLiteral("matches"),
                                       QStringLiteral("pairs"),
                                       QStringLiteral("teamPairs")}) {
                if (root.value(key).isArray()) {
                    entries = root.value(key).toArray();
                    break;
                }
            }
            if (entries.isEmpty())
                entries.append(root);
        }

        for (const QJsonValue &entry : entries) {
            if (entry.isArray()) {
                const QJsonArray pair = entry.toArray();
                if (pair.size() >= 2 && pair.at(0).isString() && pair.at(1).isString())
                    parsedPairs.append({pair.at(0).toString().trimmed(),
                                        pair.at(1).toString().trimmed()});
                continue;
            }
            if (!entry.isObject())
                continue;
            const QJsonObject object = entry.toObject();
            const QString red = jsonString(object, {QStringLiteral("red"),
                                                     QStringLiteral("redName"),
                                                     QStringLiteral("redTeam"),
                                                     QStringLiteral("red_team")});
            const QString blue = jsonString(object, {QStringLiteral("blue"),
                                                      QStringLiteral("blueName"),
                                                      QStringLiteral("blueTeam"),
                                                      QStringLiteral("blue_team")});
            if (!red.isEmpty() && !blue.isEmpty())
                parsedPairs.append({red, blue});
        }
    } else {
        QString text = QString::fromUtf8(data);
        if (text.startsWith(QChar(0xFEFF)))
            text.remove(0, 1);

        const QChar delimiter = text.contains(QLatin1Char('\t'))
                                    ? QLatin1Char('\t')
                                    : (text.contains(QLatin1Char(','))
                                           ? QLatin1Char(',')
                                           : QLatin1Char('\t'));
        const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        bool firstDataLine = true;
        for (QString line : lines) {
            line.remove(QLatin1Char('\r'));
            if (line.trimmed().isEmpty())
                continue;
            const QString trimmed = line.trimmed();
            if (trimmed.startsWith(QLatin1Char('#'))
                || trimmed.startsWith(QStringLiteral("//"))) {
                continue;
            }

            const QStringList fields = parseDelimitedLine(line, delimiter);
            if (firstDataLine && isTeamHeader(fields)) {
                firstDataLine = false;
                continue;
            }
            firstDataLine = false;
            if (fields.size() < 2)
                continue;

            const QString red = fields.at(0).trimmed();
            const QString blue = fields.at(1).trimmed();
            if (!red.isEmpty() && !blue.isEmpty())
                parsedPairs.append({red, blue});
        }
    }

    if (parsedPairs.isEmpty()) {
        if (errorMessage)
            *errorMessage = tr("没有找到有效的红方/蓝方队伍名称。CSV/TSV/TXT 每行应为：红方队名,蓝方队名；JSON 可使用 red/blue 或 redName/blueName 字段。");
        return false;
    }

    m_teamPairs = parsedPairs;
    m_currentTeamPairIndex = -1;
    if (m_teamPairEdit) {
        m_teamPairEdit->clear();
        for (int i = 0; i < m_teamPairs.size(); ++i)
            m_teamPairEdit->addItem(tr("%1 · %2 vs %3").arg(i+1).arg(m_teamPairs[i].redName, m_teamPairs[i].blueName), i);
    }
    updateTeamPairControls();
    refreshCompetitionUi();
    return true;
}

void MainWindow::updateTeamPairControls()
{
    if (!m_teamPairLabel || !m_nextTeamBtn)
        return;

    if (m_teamPairs.isEmpty() || m_currentTeamPairIndex < 0
        || m_currentTeamPairIndex >= m_teamPairs.size()) {
        m_teamPairLabel->setText(m_teamPairs.isEmpty() ? tr("未导入队伍表") : tr("已导入，请选择对阵并点击应用"));
        m_nextTeamBtn->setEnabled(!m_teamPairs.isEmpty());
        return;
    }

    const TeamPair &pair = m_teamPairs.at(m_currentTeamPairIndex);
    m_teamPairLabel->setText(tr("当前第 %1 / %2 组 · 红方：%3 · 蓝方：%4")
                                 .arg(m_currentTeamPairIndex + 1)
                                 .arg(m_teamPairs.size())
                                 .arg(pair.redName)
                                 .arg(pair.blueName));
    m_nextTeamBtn->setEnabled(true);
}

void MainWindow::applyTeamPair(int index, bool resetMatch)
{
    if (!m_broadcast || index < 0 || index >= m_teamPairs.size())
        return;

    if (competitionMode() && m_competition.locked()) {
        showActionResult(tr("当前 BO3 已锁定，选中只预览，不能应用新对阵"), QStringLiteral("error")); return;
    }
    const TeamPair &pair = m_teamPairs.at(index);
    if (pair.redName == pair.blueName) { showActionResult(tr("同一队伍不能对战自己"), QStringLiteral("error")); return; }
    if (competitionMode() && resetMatch) {
        m_competition.resetSeries(); m_activeRobotIds.clear(); m_activeDeviceMacs.clear();
        persistCompetitionLedger();
        if (m_matchServer) m_matchServer->clearRegistrations(tr("已切换对阵，请重新登记"));
        if (m_checkinPanel) m_checkinPanel->startNewSession();
    }
    m_currentTeamPairIndex = index;
    if (m_teamPairEdit) m_teamPairEdit->setCurrentIndex(index);
    {
        const QSignalBlocker redBlocker(m_redTeamNameEdit);
        const QSignalBlocker blueBlocker(m_blueTeamNameEdit);
        m_redTeamNameEdit->setText(pair.redName);
        m_blueTeamNameEdit->setText(pair.blueName);
    }

    m_broadcast->setTeamNames(pair.redName, pair.blueName);
    if (resetMatch) {
        m_broadcast->resetMatch();
        m_referee.reset();
        if (m_summaryToggleBtn)
            m_summaryToggleBtn->setEnabled(false);
        refreshRefereeStatus();
        if (m_redScoreEdit)
            m_redScoreEdit->setValue(0);
        if (m_blueScoreEdit)
            m_blueScoreEdit->setValue(0);
    }
    updateProgramSourceList();
    updateTeamPairControls();
    refreshCompetitionUi();
    publishMatchState();
}

void MainWindow::onScoresChanged()
{
    if (competitionMode()) { syncRefereeState(); return; }
    if (!m_broadcast)
        return;
    const int redScore = m_redScoreEdit ? m_redScoreEdit->value() : 0;
    const int blueScore = m_blueScoreEdit ? m_blueScoreEdit->value() : 0;
    m_referee.setScores(redScore, blueScore);
    m_broadcast->setScores(redScore, blueScore);
    showActionResult(tr("击杀积分已更新 · 红方 %1 / 蓝方 %2").arg(redScore).arg(blueScore));
}

void MainWindow::onAwardRedCard()
{
    if (!m_table || !m_broadcast) {
        return;
    }
    const int row = m_table->currentRow();
    auto *teamItem = row >= 0 ? m_table->item(row, kColTeam) : nullptr;
    if (!teamItem) {
        reportAction(tr("请先在机器人状态表中选中需要判罚的机器人"), QStringLiteral("error"));
        return;
    }
    const quint8 team = static_cast<quint8>(teamItem->text().toInt());
    m_broadcast->awardCard(team, true);
    reportAction(tr("[牌面] 已为队伍 %1 显示红牌").arg(teamItem->text()));
    m_referee.recordPenalty(team, true, refereeNow());
    onRefereeTick();
}

void MainWindow::onAwardYellowCard()
{
    if (!m_table || !m_broadcast) {
        return;
    }
    const int row = m_table->currentRow();
    auto *teamItem = row >= 0 ? m_table->item(row, kColTeam) : nullptr;
    auto *idItem = row >= 0 ? m_table->item(row, kColId) : nullptr;
    if (!teamItem || !idItem) {
        reportAction(tr("请先在机器人状态表中选中需要判罚的机器人"), QStringLiteral("error"));
        return;
    }
    const quint8 team = static_cast<quint8>(teamItem->text().toInt());
    const quint8 robotId = static_cast<quint8>(idItem->text().toInt());
    m_broadcast->awardCard(team, false);
    onLogMessage(tr("[牌面] 已为队伍 %1 显示黄牌").arg(team));
    // Record before sending: the third card's HP drop must not be read as a
    // knockout, and its 50 HP must not be credited to the opponent.
    m_referee.recordPenalty(team, false, refereeNow());
    // V2: 同时向 ESP32 下发黄牌命令（L431 内部扣分，第 3 次判负）
    if (m_commander) {
        const quint32 txid = m_commander->sendYellowCard(robotId);
        trackCommand(txid, tr("机器人 %1 黄牌处罚").arg(robotId));
        if (txid == 0)
            onLogMessage(tr("[牌面] 机器人 %1 未连接 ESP32，黄牌仅显示未下发")
                             .arg(robotId));
    }
    onRefereeTick();
}

void MainWindow::onClearCards()
{
    if (m_broadcast)
        m_broadcast->clearCards();
    reportAction(tr("[牌面] 已清除红牌/黄牌显示"));
}

void MainWindow::onLayoutElementChanged(int)
{
    refreshLayoutPositionEditors();
}

void MainWindow::onLayoutPositionChanged()
{
    if (!m_broadcast || !m_layoutElementEdit || !m_layoutXEdit || !m_layoutYEdit)
        return;

    const QString elementId = m_layoutElementEdit->currentData().toString();
    if (elementId.isEmpty())
        return;

    m_broadcast->setLayoutElementPosition(
        elementId, QPoint(m_layoutXEdit->value(), m_layoutYEdit->value()));
    showActionResult(tr("HUD 元素位置已应用"));
}

void MainWindow::onLayoutSizeChanged()
{
    if (!m_broadcast || !m_layoutElementEdit || !m_layoutWidthEdit || !m_layoutHeightEdit)
        return;

    const QString elementId = m_layoutElementEdit->currentData().toString();
    if (elementId.isEmpty())
        return;

    m_broadcast->setLayoutElementSize(
        elementId, QSize(m_layoutWidthEdit->value(), m_layoutHeightEdit->value()));
    showActionResult(tr("HUD 元素尺寸已应用"));
}

void MainWindow::onResetLayout()
{
    if (!m_broadcast)
        return;

    m_broadcast->resetLayoutPositions();
    refreshLayoutPositionEditors();
    reportAction(tr("[布局] 已恢复默认位置和尺寸"));
}

void MainWindow::onSaveLayout()
{
    if (!m_broadcast)
        return;

    if (m_broadcast->saveLayout(layoutFilePath()))
        reportAction(tr("[布局] 已保存到 %1").arg(layoutFilePath()));
    else
        reportAction(tr("[布局] 保存失败: %1").arg(layoutFilePath()), QStringLiteral("error"));
}

void MainWindow::onLoadLayout()
{
    if (!m_broadcast)
        return;

    if (m_broadcast->loadLayout(layoutFilePath())) {
        refreshLayoutPositionEditors();
        reportAction(tr("[布局] 已从 %1 加载").arg(layoutFilePath()));
    } else {
        reportAction(tr("[布局] 未找到可用布局文件: %1").arg(layoutFilePath()), QStringLiteral("error"));
    }
}

void MainWindow::onVideoSourcesChanged(const QVector<MatchServer::VideoSourceInfo> &sources)
{
    m_videoSources = sources;
    updateProgramSourceList();
    rebuildSourcePreviewStrip();
}

void MainWindow::rebuildSourcePreviewStrip()
{
    if (!m_sourcePreviewLayout)
        return;

    // Remove every existing thumbnail; rebuilding is cheap because the source list changes
    // far less often than the frame rate.
    while (QLayoutItem *item = m_sourcePreviewLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->setParent(nullptr); // Exclude deferred old cells from lookups immediately.
            widget->deleteLater();
        }
        delete item;
    }
    m_sourcePreviewViews.clear();

    const QString redName = m_redTeamNameEdit && !m_redTeamNameEdit->text().trimmed().isEmpty()
                                ? m_redTeamNameEdit->text().trimmed()
                                : tr("红方");
    const QString blueName = m_blueTeamNameEdit && !m_blueTeamNameEdit->text().trimmed().isEmpty()
                                  ? m_blueTeamNameEdit->text().trimmed()
                                  : tr("蓝方");

    int added = 0;
    QVector<MatchServer::VideoSourceInfo> previewSources = m_videoSources;
    MatchServer::VideoSourceInfo field;
    field.sourceId = QStringLiteral("field");
    field.sourceName = tr("本地全场摄像头");
    field.online = true;
    previewSources.prepend(field);
    for (const auto &source : previewSources) {
        if (!source.online || source.sourceId.isEmpty())
            continue;

        auto *cell = new QWidget;
        auto *cellLayout = new QVBoxLayout(cell);
        cellLayout->setContentsMargins(0, 0, 0, 0);
        cellLayout->setSpacing(qMax(1, qRound(4 * m_controlUiScale)));

        auto *thumb = new QLabel(cell);
        thumb->setFixedSize(qMax(1, qRound(140 * m_controlUiScale)),
                            qMax(1, qRound(79 * m_controlUiScale)));
        thumb->setAlignment(Qt::AlignCenter);
        thumb->setStyleSheet(QStringLiteral(
            "QLabel { background: #101418; border: 2px solid #2c333b;"
            " border-radius: 4px; color: #6c7886; }"));
        thumb->setText(source.sourceId == QStringLiteral("field") ? tr("全场摄像头\n未启动 / 无信号") : tr("无信号"));
        if (source.sourceId == QStringLiteral("field"))
            thumb->setObjectName(QStringLiteral("fieldCameraThumbnail"));
        thumb->setCursor(Qt::PointingHandCursor);
        thumb->setToolTip(tr("点击切换到该视角"));

        const QString sourceId = source.sourceId;
        // Tap-to-switch: clicking a thumbnail flips the program feed to it.
        thumb->installEventFilter(this);
        thumb->setProperty("sourceId", sourceId);
        thumb->setProperty("sourceName", source.sourceName);

        const QString teamName = source.team == 1 ? redName : blueName;
        const QString display = source.displayName.isEmpty()
                                    ? tr("选手%1").arg(source.robotId > 0 ? source.robotId : 1)
                                    : source.displayName;
        auto *caption = new QLabel(
            source.sourceId == QStringLiteral("field")
                ? tr("全场 · 本地摄像头\n%1").arg(m_fieldCamera && m_fieldCamera->live() ? tr("采集中") : tr("无信号"))
                : tr("%1 · %2号\n%3").arg(teamName).arg(source.robotId > 0 ? source.robotId : 1)
                               .arg(display), cell);
        caption->setAlignment(Qt::AlignCenter);
        caption->setStyleSheet(QStringLiteral("color: #cfd7df;"));
        QFont captionFont = caption->font();
        captionFont.setPointSizeF(qMax(7.0, 9.0 * m_controlUiScale));
        caption->setFont(captionFont);
        caption->setWordWrap(true);

        cellLayout->addWidget(thumb);
        cellLayout->addWidget(caption);

        m_sourcePreviewViews.insert(sourceId, thumb);
        const int columns = qMax(1, m_controlTabs->width() / qMax(1, qRound(155 * m_controlUiScale)));
        m_sourcePreviewLayout->addWidget(cell, added / columns, added % columns, Qt::AlignTop | Qt::AlignLeft);
        ++added;
        onSourceFrameUpdated(sourceId); // Restore cached frames after list/scale changes.
    }

    if (added == 0) {
        auto *empty = new QLabel(tr("暂无已登记选手端"), m_sourcePreviewGroup);
        empty->setAlignment(Qt::AlignCenter);
        empty->setStyleSheet(QStringLiteral("color: #6c7886; padding: %1px;")
                                 .arg(qMax(1, qRound(24 * m_controlUiScale))));
        m_sourcePreviewLayout->addWidget(empty, 0, 0);
    }
    refreshSourcePreviewBadges();
}

void MainWindow::refreshSourcePreviewBadges()
{
    if (!m_programSourceEdit)
        return;
    const QString activeId = m_programSourceEdit->currentData().toString();
    for (auto it = m_sourcePreviewViews.begin(); it != m_sourcePreviewViews.end(); ++it) {
        QLabel *thumb = it.value();
        if (!thumb)
            continue;
        const bool isActive = it.key() == activeId;
        thumb->setStyleSheet(QStringLiteral(
            "QLabel { background: #101418; border: %1px solid %2;"
            " border-radius: %3px; color: #6c7886; }")
                                 .arg(qMax(1, qRound(2 * m_controlUiScale)))
                                 .arg(isActive ? QStringLiteral("#ffd76a")
                                               : QStringLiteral("#2c333b"))
                                 .arg(qMax(1, qRound(4 * m_controlUiScale))));
    }
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    applyResponsiveUi();
    QTimer::singleShot(0, this, [this] { rebuildSourcePreviewStrip(); });
}

void MainWindow::applyResponsiveUi()
{
    if (!m_uiScaler)
        return;

    m_controlUiScale = m_uiScaler->apply(size());
    if (QWidget *central = centralWidget())
        central->setStyleSheet(controlStyleSheet(m_controlUiScale));

    if (!m_workspace)
        return;
    const bool compact = width() < 1500 || height() < 850;
    if (compact != m_dashboardCompact) {
        m_workspace->setMinimumSize(0, 0);
        m_compactTabs->setMinimumSize(0, 0);
        m_dashboardCompact = compact;
        if (compact) {
            m_compactTabs->addTab(m_leftTabs,tr("比赛与队伍"));
            m_compactTabs->addTab(m_controlTabs,tr("监看与导播"));
            m_compactTabs->addTab(m_rightTabs,tr("设备与网络"));
            m_workspace->hide();
            m_compactTabs->show();
        } else {
            while (m_compactTabs->count()) m_compactTabs->removeTab(0);
            m_workspace->addWidget(m_leftTabs);
            m_workspace->addWidget(m_controlTabs);
            m_workspace->addWidget(m_rightTabs);
            m_compactTabs->hide();
            m_workspace->show();
            m_leftTabs->show();
            m_controlTabs->show();
            m_rightTabs->show();
        }
    }
    if (!compact) {
        m_workspace->show();
        m_leftTabs->show();
        m_controlTabs->show();
        m_rightTabs->show();
        m_workspace->setSizes({width()*24/100,width()*52/100,width()*24/100});
    }
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // Clicking a source thumbnail switches the program feed to it.
    if (event->type() == QEvent::MouseButtonRelease) {
        const QString sourceId = watched->property("sourceId").toString();
        if (!sourceId.isEmpty() && m_programSourceEdit) {
            const int idx = m_programSourceEdit->findData(sourceId);
            if (idx >= 0)
                m_programSourceEdit->setCurrentIndex(idx);
            showActionResult(tr("节目源已切换：%1").arg(watched->property("sourceName").toString()));
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::onSourceFrameUpdated(const QString &sourceId)
{
    QLabel *thumb = m_sourcePreviewViews.value(sourceId);
    if (!m_broadcast) return;
    if (m_programMonitor && m_programSourceEdit && m_programSourceEdit->currentData().toString() == sourceId) {
        const QImage programFrame = m_broadcast->sourceFrame(sourceId);
        if (programFrame.isNull()) {
            m_programMonitor->setPixmap(QPixmap());
            m_programMonitor->setText(tr("节目监看 · %1 · 无信号").arg(m_programSourceEdit->currentText()));
        } else {
            m_programMonitor->setPixmap(QPixmap::fromImage(programFrame).scaled(m_programMonitor->size(),Qt::KeepAspectRatio,Qt::SmoothTransformation));
        }
    }
    if (!thumb) return;
    const QImage frame = m_broadcast->sourceFrame(sourceId);
    if (frame.isNull()) {
        thumb->setPixmap(QPixmap());
        thumb->setText(tr("无信号"));
        return;
    }
    thumb->setPixmap(QPixmap::fromImage(frame).scaled(
        thumb->size(), Qt::KeepAspectRatioByExpanding, Qt::FastTransformation));
}

void MainWindow::updateProgramSourceList()
{
    if (!m_programSourceEdit)
        return;

    const QString previousId = m_programSourceEdit->currentData().toString();
    QSignalBlocker blocker(m_programSourceEdit);
    m_programSourceEdit->clear();
    m_programSourceEdit->addItem(tr("全场视角"), QStringLiteral("field"));

    const QString redName = m_redTeamNameEdit && !m_redTeamNameEdit->text().trimmed().isEmpty()
                                ? m_redTeamNameEdit->text().trimmed()
                                : tr("红方");
    const QString blueName = m_blueTeamNameEdit && !m_blueTeamNameEdit->text().trimmed().isEmpty()
                                  ? m_blueTeamNameEdit->text().trimmed()
                                  : tr("蓝方");

    for (const auto &source : m_videoSources) {
        if (!source.online || source.sourceId.isEmpty() || (source.team != 1 && source.team != 2))
            continue;
        const QString teamName = source.team == 1 ? redName : blueName;
        const QString sourceName = source.sourceName.isEmpty()
                                        ? tr("未命名视频源")
                                        : source.sourceName;
        m_programSourceEdit->addItem(tr("%1 · %2 号 · %3")
                                         .arg(teamName)
                                         .arg(source.robotId > 0 ? source.robotId : 1)
                                         .arg(sourceName),
                                     source.sourceId);
    }

    int selected = m_programSourceEdit->findData(previousId);
    if (selected < 0)
        selected = 0;
    m_programSourceEdit->setCurrentIndex(selected);
    blocker.unblock();
    onProgramSourceChanged(selected);
    onProgramModeChanged(m_programModeEdit ? m_programModeEdit->currentIndex() : 0);
}

void MainWindow::refreshLayoutPositionEditors()
{
    if (!m_broadcast || !m_layoutElementEdit || !m_layoutXEdit || !m_layoutYEdit
        || !m_layoutWidthEdit || !m_layoutHeightEdit)
        return;

    const QString elementId = m_layoutElementEdit->currentData().toString();
    const QPoint position = m_broadcast->layoutElementPosition(elementId);
    const QSize size = m_broadcast->layoutElementSize(elementId);
    const QSignalBlocker xBlocker(m_layoutXEdit);
    const QSignalBlocker yBlocker(m_layoutYEdit);
    const QSignalBlocker widthBlocker(m_layoutWidthEdit);
    const QSignalBlocker heightBlocker(m_layoutHeightEdit);
    m_layoutXEdit->setValue(position.x());
    m_layoutYEdit->setValue(position.y());
    m_layoutWidthEdit->setValue(qMax(1, size.width()));
    m_layoutHeightEdit->setValue(qMax(1, size.height()));
}

QString MainWindow::layoutFilePath() const
{
    return QCoreApplication::applicationDirPath()
           + QStringLiteral("/broadcast_layout.ini");
}

void MainWindow::onToggleListen()
{
    if (m_listening)
        stopListen();
    else
        startListen();
}

void MainWindow::updateListenInfo()
{
    const quint16 port = static_cast<quint16>(m_portEdit->value());
    QString addrs = localIpv4List();
    m_connLabel->setText(tr("正在监听 UDP 端口 %1。小车端服务器地址应填以下任一 IPv4: %2")
                             .arg(port)
                             .arg(addrs.isEmpty() ? tr("(未检测到 IPv4 地址)") : addrs));
}

QString MainWindow::localIpv4List() const
{
    QStringList list;
    const auto addrs = QNetworkInterface::allAddresses();
    for (const QHostAddress &a : addrs) {
        if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback())
            list.append(a.toString());
    }
    return list.join(QStringLiteral("  /  "));
}

void MainWindow::onReadyRead()
{
    while (m_socket->hasPendingDatagrams()) {
        const QNetworkDatagram dg = m_socket->receiveDatagram();
        if (!dg.isValid())
            continue;
        try {
            proto::Frame health;
            if (m_checkinPanel && dg.senderAddress().protocol() == QAbstractSocket::IPv4Protocol
                && proto::parseDatagram(dg.data(), health) && health.version == proto::kVersion2
                && health.type == proto::TypeDeviceHealth && health.robotId != 0) {
                m_checkinPanel->observeDeviceHealth(health.robotId, health.componentOnline,
                                                    dg.senderAddress().toString());
            }
            m_robots->handleDatagram(dg.data(), dg.senderAddress(), dg.senderPort());
        } catch (const std::exception &error) {
            qCritical().noquote()
                << QStringLiteral("[UDP] 处理 %1:%2 的数据报发生异常: %3")
                       .arg(dg.senderAddress().toString()).arg(dg.senderPort())
                       .arg(QString::fromUtf8(error.what()));
        }
    }
}

void MainWindow::onReliableEventNeedsAck(quint8 robotId, quint8 type,
                                       quint32 txid,
                                       const QHostAddress &addr, quint16 port)
{
    // V2 可靠事件（死亡/复活）：必须回 ACK。result=0 表示已收到。
    if (m_robots)
        m_robots->sendAck(m_socket, addr, port, type, txid, 0);
    Q_UNUSED(robotId);
}

void MainWindow::onRobotAck(quint8 ackedType, quint32 txid, quint8 result,
                            quint8 robotId)
{
    if (m_commander)
        m_commander->onAck(ackedType, txid, result, robotId);
}

void MainWindow::onEndpointLearned(quint8 robotId, quint32 ipv4,
                                   quint16 port)
{
    if (m_commander)
        m_commander->learnEndpoint(robotId, ipv4, port);
}

// ---------- V2 设备管理 ----------

void MainWindow::onAssignTeam()
{
    if (competitionMode() && m_competition.locked()) {
        reportAction(tr("当前 BO3 的设备身份已锁定，请先完成或终止整场后再重新分配"), QStringLiteral("error")); return;
    }
    if (!m_robots || !m_commander || !m_deviceRobotEdit || !m_deviceTeamEdit
        || !m_deviceSelectEdit || !m_deviceControllerMacEdit)
        return;

    RobotManager::DeviceInfo device;
    const QString deviceMac = m_deviceSelectEdit->currentData().toString();
    if (!m_robots->deviceForMac(deviceMac, &device)) {
        reportAction(tr("[设备] 尚未收到可分配的 ESP32 DEVICE_ANNOUNCE"), QStringLiteral("error"));
        return;
    }

    QString controllerMacText = m_deviceControllerMacEdit->text().trimmed();
    if (controllerMacText.isEmpty())
        controllerMacText = device.controllerMac;
    QByteArray controllerMac;
    QString macError;
    if (!parseMac6(controllerMacText, &controllerMac, &macError)
        || controllerMac == QByteArray(6, char(0))) {
        if (macError.isEmpty())
            macError = tr("手柄 MAC 不能全部为零");
        reportAction(tr("[设备] 手柄 MAC 无效: %1").arg(macError), QStringLiteral("error"));
        return;
    }

    const quint8 robotId = static_cast<quint8>(m_deviceRobotEdit->value());
    const quint8 team = static_cast<quint8>(m_deviceTeamEdit->currentData().toInt());
    if (!m_listening) {
        reportAction(tr("[设备] 请先启动 UDP 监听，再分配队伍"), QStringLiteral("error"));
        return;
    }
    for (const auto &other : m_robots->devices()) {
        if (other.deviceMac != deviceMac && other.robotId == robotId
            && QDateTime::currentMSecsSinceEpoch() - other.lastSeenMs < 5000) {
            reportAction(tr("[设备] 此机器人 ID 已由另一台在线设备使用"), QStringLiteral("error"));
            return;
        }
    }
    for (const auto &pending : std::as_const(m_pendingAssignments)) {
        if (pending.deviceMac == deviceMac || pending.robotId == robotId) {
            reportAction(tr("[设备] 此设备或机器人 ID 正在等待分配回执，请稍后重试"), QStringLiteral("pending"));
            return;
        }
    }
    const QByteArray deviceBytes = macBytes(device.deviceMac);
    if (deviceBytes.size() != 6 || device.ipv4 == 0) {
        reportAction(tr("[设备] ESP32 %1 的网络地址或 MAC 无效").arg(device.deviceMac), QStringLiteral("error"));
        return;
    }

    const quint32 txid = m_commander->sendAssignment(
        device.ipv4, robotId, deviceBytes, controllerMac);
    if (txid == 0) {
        reportAction(tr("[设备] ASSIGNMENT 发送失败，请检查 UDP 5006 和网络地址"), QStringLiteral("error"));
        return;
    }

    m_pendingAssignments.insert(txid, {robotId, team, device.deviceMac});
    onLogMessage(tr("[设备] 已请求 ESP32 %1 分配为机器人 %2（%3），手柄 %4，txid=%5；等待 ACK")
                     .arg(device.deviceMac)
                     .arg(robotId)
                     .arg(team == 0 ? tr("未分配") : (team == 1 ? tr("红方") : tr("蓝方")))
                     .arg(controllerMacText)
                     .arg(txid));
    trackCommand(txid, tr("分配 ID 与手柄"));
    refreshTable();
}

void MainWindow::refreshDeviceList()
{
    if (!m_robots || !m_deviceSelectEdit)
        return;

    const QString previousMac = m_deviceSelectEdit->currentData().toString();
    const QVector<RobotManager::DeviceInfo> devices = m_robots->devices();
    QSignalBlocker blocker(m_deviceSelectEdit);
    m_deviceSelectEdit->clear();
    if (devices.isEmpty()) {
        m_deviceSelectEdit->addItem(tr("等待 ESP32 DEVICE_ANNOUNCE..."), QString());
        return;
    }

    for (const RobotManager::DeviceInfo &device : devices) {
        const QString robotText = device.robotId == 0
                                      ? tr("未分配")
                                      : tr("机器人 %1").arg(device.robotId);
        const QString controller = device.controllerMac.isEmpty()
                                       ? tr("手柄未分配")
                                       : device.controllerMac;
        m_deviceSelectEdit->addItem(
            tr("%1 | %2 | %3 | %4").arg(device.deviceMac, robotText,
                                        device.addressText, controller),
            device.deviceMac);
    }

    int index = m_deviceSelectEdit->findData(previousMac);
    if (index < 0 && m_deviceSelectEdit->count() > 0)
        index = 0;
    if (index >= 0)
        m_deviceSelectEdit->setCurrentIndex(index);

    RobotManager::DeviceInfo selected;
    if (m_robots->deviceForMac(m_deviceSelectEdit->currentData().toString(), &selected)) {
        const bool selectionChanged = previousMac != selected.deviceMac;
        if (m_deviceControllerMacEdit
            && (selectionChanged || m_deviceControllerMacEdit->property("reportedMac").toString() != selected.controllerMac))
            m_deviceControllerMacEdit->setText(selected.controllerMac);
        if (m_deviceRobotEdit && selected.robotId != 0
            && (selectionChanged || m_deviceRobotEdit->property("reportedId").toUInt() != selected.robotId))
            m_deviceRobotEdit->setValue(selected.robotId);
        m_deviceControllerMacEdit->setProperty("reportedMac", selected.controllerMac);
        m_deviceRobotEdit->setProperty("reportedId", selected.robotId);
    }
}

void MainWindow::onForcePowerOn()
{
    if (!m_commander || !m_deviceRobotEdit)
        return;
    const quint8 robotId = static_cast<quint8>(m_deviceRobotEdit->value());
    const quint32 txid = m_commander->sendForcePowerOn(robotId);
    if (txid)
        onLogMessage(tr("[设备] 已向机器人 %1 发送通电命令 txid=%2").arg(robotId).arg(txid));
    else
        onLogMessage(tr("[设备] 机器人 %1 未知端点，无法发送通电命令").arg(robotId));
    trackCommand(txid, tr("通电"));
}

void MainWindow::onForcePowerOff()
{
    if (!m_commander || !m_deviceRobotEdit)
        return;
    const quint8 robotId = static_cast<quint8>(m_deviceRobotEdit->value());
    const quint32 txid = m_commander->sendForcePowerOff(robotId);
    if (txid)
        onLogMessage(tr("[设备] 已向机器人 %1 发送断电命令 txid=%2").arg(robotId).arg(txid));
    else
        onLogMessage(tr("[设备] 机器人 %1 未知端点，无法发送断电命令").arg(robotId));
    trackCommand(txid, tr("断电"));
}

void MainWindow::onSetHp()
{
    if (competitionMode() && m_competition.locked()) {
        reportAction(tr("正式 BO3 中不能管理员改写 HP；复活必须通过配额审批"), QStringLiteral("error")); return;
    }
    if (!m_commander || !m_deviceRobotEdit || !m_deviceHpEdit)
        return;
    const quint8 robotId = static_cast<quint8>(m_deviceRobotEdit->value());
    const quint16 hp = static_cast<quint16>(m_deviceHpEdit->value());
    const quint32 txid = m_commander->sendSetHp(robotId, hp);
    if (txid)
        onLogMessage(tr("[设备] 已向机器人 %1 发送 SET_HP=%2 txid=%3").arg(robotId).arg(hp).arg(txid));
    else
        onLogMessage(tr("[设备] 机器人 %1 未知端点，无法发送 SET_HP").arg(robotId));
    trackCommand(txid, tr("设置 HP=%1").arg(hp));
}

void MainWindow::onRequestStatus()
{
    if (!m_commander || !m_deviceRobotEdit)
        return;
    const quint8 robotId = static_cast<quint8>(m_deviceRobotEdit->value());
    const bool sent = m_commander->sendStatusRequest(robotId);
    reportAction(sent ? tr("请求机器人 %1 状态已发送；此指令无 ACK，请观察状态更新").arg(robotId)
                      : tr("请求机器人 %1 状态发送失败，请检查监听与设备端点").arg(robotId),
                 sent ? QStringLiteral("info") : QStringLiteral("error"));
}

void MainWindow::refreshTable()
{
    if (!m_table) return;

    const auto allBots = m_robots->robots();
    QVector<RobotManager::RobotInfo> bots;
    for (const auto &robot : allBots) {
        // V2 中 team 可能为 0（未分配）；仍显示，但表格里 team=0 行
        // 由用户在设备管理面板分配后再进入红/蓝。
        bots.append(robot);
    }
    std::sort(bots.begin(), bots.end(), [](const RobotManager::RobotInfo &left,
                                           const RobotManager::RobotInfo &right) {
        if (left.team != right.team)
            return left.team < right.team;
        return left.robotId < right.robotId;
    });
    const auto *selectedIdItem = m_table->currentRow() >= 0 ? m_table->item(m_table->currentRow(), kColId) : nullptr;
    const int selectedId = selectedIdItem ? selectedIdItem->text().toInt() : -1;
    m_table->setSortingEnabled(false);
    m_table->setRowCount(bots.size());

    QFont bold = m_table->font();
    bold.setBold(true);

    int onlineCount = 0;
    for (int row = 0; row < bots.size(); ++row) {
        const RobotManager::RobotInfo &r = bots.at(row);
        if (r.online) ++onlineCount;

        const QColor green(0, 140, 0);
        const QColor red(200, 0, 0);
        const QColor gray(150, 150, 150);

        QString stateText;
        QColor stateColor;
        QString shootText;
        QColor shootColor;

        if (!r.online) {
            stateText = tr("离线");
            stateColor = gray;
            shootText = "-";
            shootColor = gray;
        } else {
            if (r.alive) {
                stateText = tr("存活");
                stateColor = green;
            } else {
                stateText = tr("死亡");
                stateColor = red;
            }
            if (r.shootEnabled) {
                shootText = tr("允许");
                shootColor = green;
            } else {
                shootText = tr("禁止");
                shootColor = red;
            }
        }

        auto idItem = new QTableWidgetItem(QString::number(r.robotId));
        idItem->setData(Qt::DisplayRole, int(r.robotId));
        idItem->setTextAlignment(Qt::AlignCenter);
        auto teamItem = new QTableWidgetItem(QString::number(r.team));
        teamItem->setData(Qt::DisplayRole, int(r.team));
        teamItem->setTextAlignment(Qt::AlignCenter);
        auto hpItem = new QTableWidgetItem(r.hp >= 0 ? QString::number(r.hp) : tr("-"));
        if (r.hp >= 0)
            hpItem->setData(Qt::DisplayRole, r.hp);
        hpItem->setTextAlignment(Qt::AlignCenter);
        auto heatItem = new QTableWidgetItem(r.heat >= 0 ? QString::number(r.heat) : tr("-"));
        if (r.heat >= 0)
            heatItem->setData(Qt::DisplayRole, r.heat);
        heatItem->setTextAlignment(Qt::AlignCenter);
        // V2 only: power / powerOn / linkUp / version
        auto powerItem = new QTableWidgetItem(
            r.protocolVersion == proto::kVersion2 && r.power >= 0
                ? QString::number(r.power) : tr("-"));
        powerItem->setTextAlignment(Qt::AlignCenter);
        auto powerOnItem = new QTableWidgetItem(
            r.online && r.protocolVersion == proto::kVersion2
                ? (r.powerOn ? tr("已通电") : tr("已断电")) : tr("-"));
        powerOnItem->setForeground(r.powerOn ? green : red);
        powerOnItem->setTextAlignment(Qt::AlignCenter);
        auto linkItem = new QTableWidgetItem(
            r.online && r.protocolVersion == proto::kVersion2
                ? (r.linkUp ? tr("正常") : tr("断开")) : tr("-"));
        linkItem->setForeground(r.linkUp ? green : red);
        linkItem->setTextAlignment(Qt::AlignCenter);
        auto verItem = new QTableWidgetItem(
            r.protocolVersion > 0 ? QStringLiteral("V%1").arg(r.protocolVersion) : tr("-"));
        verItem->setTextAlignment(Qt::AlignCenter);
        auto stateItem = new QTableWidgetItem(stateText);
        stateItem->setForeground(stateColor);
        stateItem->setFont(bold);
        stateItem->setTextAlignment(Qt::AlignCenter);
        auto shootItem = new QTableWidgetItem(shootText);
        shootItem->setForeground(shootColor);
        shootItem->setTextAlignment(Qt::AlignCenter);
        const QString src = r.addressText.isEmpty()
                                 ? QString()
                                 : QStringLiteral("%1:%2").arg(r.addressText).arg(r.sourcePort);
        auto addrItem = new QTableWidgetItem(src);

        m_table->setItem(row, kColId, idItem);
        m_table->setItem(row, kColTeam, teamItem);
        m_table->setItem(row, kColHp, hpItem);
        m_table->setItem(row, kColHeat, heatItem);
        m_table->setItem(row, kColPower, powerItem);
        m_table->setItem(row, kColState, stateItem);
        m_table->setItem(row, kColShoot, shootItem);
        m_table->setItem(row, kColPowerOn, powerOnItem);
        m_table->setItem(row, kColLink, linkItem);
        m_table->setItem(row, kColVer, verItem);
        m_table->setItem(row, kColAddr, addrItem);
    }
    m_table->setSortingEnabled(true);
    if (selectedId >= 0) {
        m_table->clearSelection();
        m_table->setCurrentItem(nullptr);
        for (int row = 0; row < m_table->rowCount(); ++row)
            if (m_table->item(row, kColId)->text().toInt() == selectedId) {
                m_table->setCurrentCell(row, kColId);
                m_table->selectRow(row);
                break;
            }
    }
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    if (m_cardTargetLabel) {
        const int selectedRow = m_table->currentRow();
        auto *id = selectedRow >= 0 ? m_table->item(selectedRow, kColId) : nullptr;
        auto *team = selectedRow >= 0 ? m_table->item(selectedRow, kColTeam) : nullptr;
        m_cardTargetLabel->setText(id && team ? tr("判罚对象：机器人 %1 · 队伍 %2").arg(id->text(), team->text())
                                            : tr("判罚对象：未选择 · 请到「状态」页选择机器人"));
    }

    statusBar()->showMessage(tr("在线 %1 / %2 台机器人").arg(onlineCount).arg(kExpectedRobots));

    m_referee.updateRobots(competitionMode() ? activeMatchRobots() : allBots, refereeNow());
    syncRefereeState();
    if (m_broadcast)
        m_broadcast->updateRobots(bots);
    if (m_matchServer)
        m_matchServer->publishRobots(bots);
}

void MainWindow::showActionResult(const QString &message, const QString &state)
{
    if (!m_actionStatusLabel)
        return;
    const QString prefix = state == QStringLiteral("error") ? tr("失败 / 未确认")
                         : state == QStringLiteral("pending") ? tr("等待确认")
                         : state == QStringLiteral("info") ? tr("提示") : tr("已完成");
    m_actionStatusLabel->setText(tr("%1  ·  %2").arg(prefix, message));
    m_actionStatusLabel->setProperty("state", state);
    m_actionStatusLabel->setToolTip(message);
    m_actionStatusLabel->style()->unpolish(m_actionStatusLabel);
    m_actionStatusLabel->style()->polish(m_actionStatusLabel);
}

void MainWindow::reportAction(const QString &message, const QString &state)
{
    onLogMessage(message);
    showActionResult(message, state);
}

void MainWindow::finishCommand(quint32 txid, const QString &detail, bool success)
{
    const QString action = m_actionCommands.take(txid);
    if (action.isEmpty()) return;
    if (success) ++m_commandConfirmed; else ++m_commandFailed;
    const QString summary = tr("%1 · %2 · 已确认 %3/%4，未确认/失败 %5，等待 %6")
        .arg(action, detail).arg(m_commandConfirmed).arg(m_commandTotal)
        .arg(m_commandFailed).arg(m_actionCommands.size());
    reportAction(summary, m_commandFailed ? QStringLiteral("error")
                          : m_actionCommands.isEmpty() ? QStringLiteral("success") : QStringLiteral("pending"));
}

void MainWindow::trackCommand(quint32 txid, const QString &action)
{
    if (m_actionCommands.isEmpty() && !m_collectingCommandBatch) {
        m_commandTotal = m_commandConfirmed = m_commandFailed = 0;
    }
    ++m_commandTotal;
    if (txid == 0) {
        ++m_commandFailed;
        reportAction(tr("%1 · 无法发送，请检查监听状态与设备端点").arg(action), QStringLiteral("error"));
        return;
    }
    m_actionCommands.insert(txid, action);
    reportAction(tr("%1 · 已发送，等待设备 ACK（事务 %2）；等待 %3 项").arg(action).arg(txid).arg(m_actionCommands.size()),
                 m_commandFailed ? QStringLiteral("error") : QStringLiteral("pending"));
}

void MainWindow::onLogMessage(const QString &message)
{
    if (!m_log) return;
    m_log->appendPlainText(QStringLiteral("[%1] %2")
                               .arg(QTime::currentTime().toString(QStringLiteral("hh:mm:ss.zzz")),
                                    message));
}
