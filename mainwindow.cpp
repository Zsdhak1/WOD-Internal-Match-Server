#include "mainwindow.h"
#include "broadcastwindow.h"
#include "matchprotocol.h"
#include "matchserver.h"
#include "protocol.h"
#include "robotcommander.h"
#include "robotmanager.h"

#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
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
#include <QScreen>
#include <QShortcut>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QSplitter>
#include <QSpinBox>
#include <QStatusBar>
#include <QStringList>
#include <QTableWidget>
#include <QTime>
#include <QTimer>
#include <QUdpSocket>
#include <QVBoxLayout>

#include <algorithm>
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
} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    m_socket = new QUdpSocket(this);
    connect(m_socket, &QUdpSocket::readyRead, this, &MainWindow::onReadyRead);

    m_commander = new RobotCommander(m_socket, this);
    connect(m_commander, &RobotCommander::logMessage, this,
            &MainWindow::onLogMessage);
    connect(m_commander, &RobotCommander::commandAcked, this,
            [this](quint32 txid, quint8 type, quint8 result, quint8 robotId) {
        Q_UNUSED(txid);
        if (result != 0)
            onLogMessage(QStringLiteral("[警告] 机器人%1 执行 0x%2 失败 result=%3")
                             .arg(robotId).arg(type, 2, 16).arg(result));
    });
    connect(m_commander, &RobotCommander::commandTimeout, this,
            [this](quint32 txid, quint8 type, quint8 robotId) {
        Q_UNUSED(txid);
        onLogMessage(QStringLiteral("[超时] 机器人%1 命令 0x%2 未获 ACK")
                         .arg(robotId).arg(type, 2, 16));
    });

    m_robots = new RobotManager(this);
    connect(m_robots, &RobotManager::robotsChanged, this, &MainWindow::refreshTable);
    connect(m_robots, &RobotManager::logMessage, this, &MainWindow::onLogMessage);
    connect(m_robots, &RobotManager::reliableEventNeedsAck, this,
            &MainWindow::onReliableEventNeedsAck);
    connect(m_robots, &RobotManager::ackReceived, this,
            &MainWindow::onRobotAck);
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

    m_broadcast = new BroadcastWindow;
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

    buildUi();
    populateScreens();
    connect(m_broadcast, &BroadcastWindow::visibilityChanged, this, [this](bool visible) {
        if (m_broadcastBtn)
            m_broadcastBtn->setText(visible ? tr("隐藏转播画面") : tr("显示转播画面"));
    });
    connect(m_broadcast, &BroadcastWindow::matchStateChanged, this, [this](bool running) {
        if (m_matchBtn)
            m_matchBtn->setText(running ? tr("暂停比赛") : tr("开始比赛"));
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
}

void MainWindow::buildUi()
{
    setWindowTitle(tr("赛事转播控制台"));

    auto *central = new QWidget(this);
    setMinimumSize(1080, 700);
    central->setStyleSheet(QStringLiteral(
        "QGroupBox { border: 1px solid #43515e; border-radius: 6px; margin-top: 10px; "
        "padding: 12px 10px 10px; font-weight: 700; color: #dce6eb; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 5px; "
        "color: #72b4ff; }"
        "QPushButton { min-height: 30px; padding: 0 11px; }"
        "QComboBox, QLineEdit, QSpinBox { min-height: 28px; }"
        "QTableWidget { alternate-background-color: #101a23; gridline-color: #2d3c47; }"
        "QHeaderView::section { background: #1b2a35; color: #dce6eb; padding: 5px; }"));

    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(10);

    auto *header = new QFrame(central);
    header->setObjectName(QStringLiteral("controlHeader"));
    header->setStyleSheet(QStringLiteral(
        "QFrame#controlHeader { background: #101b24; border: 1px solid #334652; "
        "border-radius: 6px; }"));
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(14, 9, 14, 9);
    auto *title = new QLabel(tr("赛事转播控制台"), header);
    QFont titleFont = title->font();
    titleFont.setPointSize(16);
    titleFont.setBold(true);
    title->setFont(titleFont);
    headerLayout->addWidget(title);

    m_connLabel = new QLabel(header);
    m_connLabel->setWordWrap(true);
    m_connLabel->setStyleSheet(QStringLiteral("color: #aebdc6;"));
    headerLayout->addWidget(m_connLabel, 1);
    root->addWidget(header);

    auto *workspace = new QSplitter(Qt::Horizontal, central);
    workspace->setChildrenCollapsible(false);

    auto *controlScroll = new QScrollArea(workspace);
    controlScroll->setWidgetResizable(true);
    controlScroll->setFrameShape(QFrame::NoFrame);
    controlScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *controlPage = new QWidget;
    auto *controlLayout = new QVBoxLayout(controlPage);
    controlLayout->setContentsMargins(2, 2, 10, 2);
    controlLayout->setSpacing(8);

    const auto createGroup = [controlPage](const QString &titleText) {
        auto *group = new QGroupBox(titleText, controlPage);
        group->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
        return group;
    };

    auto *matchGroup = createGroup(tr("比赛与转播"));
    auto *matchLayout = new QFormLayout(matchGroup);
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
    auto *matchActions = new QHBoxLayout;
    matchActions->setSpacing(6);
    matchActions->addWidget(m_broadcastBtn, 1);
    m_matchBtn = new QPushButton(tr("开始比赛"), matchGroup);
    connect(m_matchBtn, &QPushButton::clicked, this, &MainWindow::onToggleMatch);
    m_terminateMatchBtn = new QPushButton(tr("终止本局"), matchGroup);
    connect(m_terminateMatchBtn, &QPushButton::clicked,
            this, &MainWindow::onTerminateMatch);
    m_resetMatchBtn = new QPushButton(tr("重置比赛"), matchGroup);
    connect(m_resetMatchBtn, &QPushButton::clicked, this, &MainWindow::onResetMatch);
    matchActions->addWidget(m_matchBtn, 1);
    matchActions->addWidget(m_terminateMatchBtn, 1);
    matchActions->addWidget(m_resetMatchBtn, 1);
    matchLayout->addRow(tr("比赛操作"), matchActions);
    auto *animationActions = new QHBoxLayout;
    m_settlementPreviewTypeEdit = new QComboBox(matchGroup);
    m_settlementPreviewTypeEdit->addItem(tr("红方胜利"), QStringLiteral("redwin"));
    m_settlementPreviewTypeEdit->addItem(tr("蓝方胜利"), QStringLiteral("bluewin"));
    animationActions->addWidget(m_settlementPreviewTypeEdit);
    m_testVictoryAnimationBtn = new QPushButton(tr("测试胜利动画"), matchGroup);
    connect(m_testVictoryAnimationBtn, &QPushButton::clicked,
            this, &MainWindow::onTestVictoryAnimation);
    animationActions->addWidget(m_testVictoryAnimationBtn);
    animationActions->addStretch(1);
    matchLayout->addRow(tr("动画预览"), animationActions);
    controlLayout->addWidget(matchGroup);

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
    connect(m_redTeamNameEdit, &QLineEdit::editingFinished,
            this, &MainWindow::onTeamNamesChanged);
    connect(m_blueTeamNameEdit, &QLineEdit::editingFinished,
            this, &MainWindow::onTeamNamesChanged);
    teamNames->addWidget(redLabel, 0, 0);
    teamNames->addWidget(m_redTeamNameEdit, 0, 1);
    teamNames->addWidget(blueLabel, 0, 2);
    teamNames->addWidget(m_blueTeamNameEdit, 0, 3);
    teamNames->addWidget(applyTeamNamesButton, 0, 4);
    teamNames->setColumnStretch(1, 1);
    teamNames->setColumnStretch(3, 1);
    teamLayout->addLayout(teamNames);

    auto *teamTableRow = new QHBoxLayout;
    m_importTeamsBtn = new QPushButton(tr("导入队伍表"), teamGroup);
    m_nextTeamBtn = new QPushButton(tr("下一组队伍"), teamGroup);
    m_teamPairLabel = new QLabel(teamGroup);
    m_teamPairLabel->setWordWrap(true);
    connect(m_importTeamsBtn, &QPushButton::clicked,
            this, &MainWindow::onImportTeamTable);
    connect(m_nextTeamBtn, &QPushButton::clicked,
            this, &MainWindow::onNextTeamPair);
    teamTableRow->addWidget(new QLabel(tr("队伍表"), teamGroup));
    teamTableRow->addWidget(m_importTeamsBtn);
    teamTableRow->addWidget(m_nextTeamBtn);
    teamTableRow->addWidget(m_teamPairLabel, 1);
    teamLayout->addLayout(teamTableRow);

    auto *scoreRow = new QGridLayout;
    scoreRow->setHorizontalSpacing(8);
    scoreRow->setVerticalSpacing(6);
    auto *redScoreLabel = new QLabel(tr("红方小局积分"), teamGroup);
    redScoreLabel->setStyleSheet(QStringLiteral("color: #ff6872;"));
    auto *blueScoreLabel = new QLabel(tr("蓝方小局积分"), teamGroup);
    blueScoreLabel->setStyleSheet(QStringLiteral("color: #72b4ff;"));
    scoreRow->addWidget(redScoreLabel, 0, 0);
    m_redScoreEdit = new QSpinBox(teamGroup);
    m_redScoreEdit->setRange(0, 99);
    m_redScoreEdit->setValue(0);
    scoreRow->addWidget(m_redScoreEdit, 0, 1);
    scoreRow->addWidget(blueScoreLabel, 0, 2);
    m_blueScoreEdit = new QSpinBox(teamGroup);
    m_blueScoreEdit->setRange(0, 99);
    m_blueScoreEdit->setValue(0);
    scoreRow->addWidget(m_blueScoreEdit, 0, 3);
    connect(m_redScoreEdit, qOverload<int>(&QSpinBox::valueChanged),
            this, &MainWindow::onScoresChanged);
    connect(m_blueScoreEdit, qOverload<int>(&QSpinBox::valueChanged),
            this, &MainWindow::onScoresChanged);
    scoreRow->addWidget(new QLabel(tr("牌面"), teamGroup), 1, 0);
    m_redCardBtn = new QPushButton(tr("红牌"), teamGroup);
    m_yellowCardBtn = new QPushButton(tr("黄牌"), teamGroup);
    m_clearCardsBtn = new QPushButton(tr("清除牌面"), teamGroup);
    connect(m_redCardBtn, &QPushButton::clicked, this, &MainWindow::onAwardRedCard);
    connect(m_yellowCardBtn, &QPushButton::clicked, this, &MainWindow::onAwardYellowCard);
    connect(m_clearCardsBtn, &QPushButton::clicked, this, &MainWindow::onClearCards);
    scoreRow->addWidget(m_redCardBtn, 1, 1);
    scoreRow->addWidget(m_yellowCardBtn, 1, 2);
    scoreRow->addWidget(m_clearCardsBtn, 1, 3);
    scoreRow->setColumnStretch(4, 1);
    teamLayout->addLayout(scoreRow);
    controlLayout->addWidget(teamGroup);

    auto *programGroup = createGroup(tr("节目输出与字幕"));
    auto *programLayout = new QFormLayout(programGroup);
    programLayout->setContentsMargins(8, 10, 8, 6);
    programLayout->setHorizontalSpacing(12);
    programLayout->setVerticalSpacing(8);
    m_programModeEdit = new QComboBox(programGroup);
    m_programModeEdit->addItem(tr("手动切换"), false);
    m_programModeEdit->addItem(tr("自动切换"), true);
    connect(m_programModeEdit, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::onProgramModeChanged);
    m_programSourceEdit = new QComboBox(programGroup);
    m_programSourceEdit->setMinimumWidth(220);
    connect(m_programSourceEdit, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::onProgramSourceChanged);
    m_autoSwitchIntervalEdit = new QSpinBox(programGroup);
    m_autoSwitchIntervalEdit->setRange(1, 60);
    m_autoSwitchIntervalEdit->setValue(5);
    m_autoSwitchIntervalEdit->setSuffix(tr(" 秒"));
    connect(m_autoSwitchIntervalEdit, qOverload<int>(&QSpinBox::valueChanged),
            this, [this](int seconds) {
                if (m_autoSwitchTimer && m_autoSwitchTimer->isActive())
                    m_autoSwitchTimer->start(seconds * 1000);
            });
    auto *modeRow = new QHBoxLayout;
    modeRow->addWidget(m_programModeEdit, 1);
    programLayout->addRow(tr("导播模式"), modeRow);
    programLayout->addRow(tr("当前视角"), m_programSourceEdit);
    programLayout->addRow(tr("自动间隔"), m_autoSwitchIntervalEdit);

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
    tickerBlock->addLayout(tickerInputRow);
    auto *tickerActions = new QHBoxLayout;
    tickerActions->addStretch(1);
    tickerActions->addWidget(m_tickerApplyBtn);
    tickerActions->addWidget(m_tickerToggleBtn);
    tickerBlock->addLayout(tickerActions);
    programLayout->addRow(tickerLabel, tickerBlock);
    controlLayout->addWidget(programGroup);

    // Per-source live preview strip. Each registered client shows up here as
    // a clickable thumbnail so the operator can see every feed at a glance.
    m_sourcePreviewGroup = createGroup(tr("已接入视频源预览"));
    auto *previewGroupLayout = new QVBoxLayout(m_sourcePreviewGroup);
    previewGroupLayout->setContentsMargins(8, 10, 8, 6);
    previewGroupLayout->setSpacing(6);
    auto *previewScrollContent = new QWidget(m_sourcePreviewGroup);
    m_sourcePreviewLayout = new QHBoxLayout(previewScrollContent);
    m_sourcePreviewLayout->setContentsMargins(0, 0, 0, 0);
    m_sourcePreviewLayout->setSpacing(10);
    m_sourcePreviewLayout->addStretch(1);
    previewScrollContent->setLayout(m_sourcePreviewLayout);
    auto *previewScroll = new QScrollArea(m_sourcePreviewGroup);
    previewScroll->setWidgetResizable(true);
    previewScroll->setWidget(previewScrollContent);
    previewScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    previewScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    previewScroll->setMinimumHeight(160);
    previewScroll->setMaximumHeight(200);
    previewGroupLayout->addWidget(previewScroll);
    controlLayout->addWidget(m_sourcePreviewGroup);

    auto *layoutGroup = createGroup(tr("HUD 布局"));
    auto *layoutForm = new QFormLayout(layoutGroup);
    layoutForm->setContentsMargins(8, 10, 8, 6);
    layoutForm->setHorizontalSpacing(12);
    layoutForm->setVerticalSpacing(8);
    m_layoutElementEdit = new QComboBox(layoutGroup);
    m_layoutElementEdit->setMinimumWidth(180);
    const QList<QPair<QString, QString>> layoutElements = {
        {tr("红方信息面板"), QStringLiteral("red_team_panel")},
        {tr("中央计时 HUD"), QStringLiteral("center_hud")},
        {tr("蓝方信息面板"), QStringLiteral("blue_team_panel")},
        {tr("左侧比分背景板"), QStringLiteral("left_score_panel")},
        {tr("右侧比分背景板"), QStringLiteral("right_score_panel")},
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
    m_layoutXEdit->setSuffix(tr(" px"));
    connect(m_layoutXEdit, qOverload<int>(&QSpinBox::valueChanged),
            this, &MainWindow::onLayoutPositionChanged);
    positionRow->addWidget(new QLabel(tr("X"), layoutGroup));
    positionRow->addWidget(m_layoutXEdit, 1);
    m_layoutYEdit = new QSpinBox(layoutGroup);
    m_layoutYEdit->setRange(-2000, 2000);
    m_layoutYEdit->setSuffix(tr(" px"));
    connect(m_layoutYEdit, qOverload<int>(&QSpinBox::valueChanged),
            this, &MainWindow::onLayoutPositionChanged);
    positionRow->addWidget(new QLabel(tr("Y"), layoutGroup));
    positionRow->addWidget(m_layoutYEdit, 1);
    layoutForm->addRow(tr("位置"), positionRow);
    auto *sizeRow = new QHBoxLayout;
    m_layoutWidthEdit = new QSpinBox(layoutGroup);
    m_layoutWidthEdit->setRange(1, 4000);
    m_layoutWidthEdit->setSuffix(tr(" px"));
    connect(m_layoutWidthEdit, qOverload<int>(&QSpinBox::valueChanged),
            this, &MainWindow::onLayoutSizeChanged);
    sizeRow->addWidget(new QLabel(tr("宽"), layoutGroup));
    sizeRow->addWidget(m_layoutWidthEdit, 1);
    m_layoutHeightEdit = new QSpinBox(layoutGroup);
    m_layoutHeightEdit->setRange(1, 4000);
    m_layoutHeightEdit->setSuffix(tr(" px"));
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
    auto *layoutHint = new QLabel(tr("正值向右/下；宽高为组件实际像素尺寸；布局文件保存在程序目录"), layoutGroup);
    layoutHint->setStyleSheet(QStringLiteral("color: #65717c;"));
    layoutHint->setWordWrap(true);
    layoutForm->addRow(QString(), layoutHint);
    controlLayout->addWidget(layoutGroup);
    refreshLayoutPositionEditors();

    auto *networkGroup = createGroup(tr("网络服务"));
    auto *networkLayout = new QFormLayout(networkGroup);
    networkLayout->setContentsMargins(8, 10, 8, 6);
    networkLayout->setHorizontalSpacing(12);
    networkLayout->setVerticalSpacing(8);
    auto *udpRow = new QHBoxLayout;
    m_portEdit = new QSpinBox(networkGroup);
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
    controlLayout->addWidget(networkGroup);
    controlLayout->addStretch(1);
    controlScroll->setWidget(controlPage);

    auto *statusSplitter = new QSplitter(Qt::Vertical, workspace);
    statusSplitter->setChildrenCollapsible(false);

    // 设备管理面板：V2 中 team 不再来自数据包，需服务器侧分配
    auto *deviceGroup = new QGroupBox(tr("设备管理"), statusSplitter);
    auto *deviceLayout = new QVBoxLayout(deviceGroup);
    deviceLayout->setContentsMargins(8, 10, 8, 8);
    auto *deviceRow = new QHBoxLayout;
    auto *deviceRobotLabel = new QLabel(tr("机器人 ID:"), deviceGroup);
    m_deviceRobotEdit = new QSpinBox(deviceGroup);
    m_deviceRobotEdit->setRange(1, 255);
    auto *deviceTeamLabel = new QLabel(tr("分配到:"), deviceGroup);
    m_deviceTeamEdit = new QComboBox(deviceGroup);
    m_deviceTeamEdit->addItem(tr("未分配"), 0);
    m_deviceTeamEdit->addItem(tr("红方"), 1);
    m_deviceTeamEdit->addItem(tr("蓝方"), 2);
    auto *deviceAssignBtn = new QPushButton(tr("分配队伍"), deviceGroup);
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
    deviceRow->addWidget(deviceRobotLabel);
    deviceRow->addWidget(m_deviceRobotEdit);
    deviceRow->addWidget(deviceTeamLabel);
    deviceRow->addWidget(m_deviceTeamEdit);
    deviceRow->addWidget(deviceAssignBtn);
    deviceRow->addStretch(1);
    deviceRow->addWidget(deviceHpBtn);
    deviceRow->addWidget(deviceHpEdit);
    deviceRow->addWidget(deviceStatusBtn);
    deviceRow->addWidget(devicePowerOnBtn);
    deviceRow->addWidget(devicePowerOffBtn);
    deviceLayout->addLayout(deviceRow);
    auto *deviceHint = new QLabel(
        tr("V2 设备需先通过上行状态帧学习 IP，再分配队伍和手柄 MAC"),
        deviceGroup);
    deviceHint->setStyleSheet(QStringLiteral("color: #6c7886; font-size: 11px;"));
    deviceLayout->addWidget(deviceHint);
    statusSplitter->addWidget(deviceGroup);

    auto *tableGroup = new QGroupBox(tr("机器人状态"), statusSplitter);
    auto *tableLayout = new QVBoxLayout(tableGroup);
    tableLayout->setContentsMargins(8, 10, 8, 8);
    m_table = new QTableWidget(0, 11, tableGroup);
    m_table->setHorizontalHeaderLabels({
        tr("机器人ID"), tr("队伍"), tr("血量 HP"), tr("热量"),
        tr("功率 W"), tr("状态"), tr("射击"), tr("供电"), tr("链路"),
        tr("协议"), tr("来源地址")});
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->horizontalHeader()->setSectionResizeMode(kColState, QHeaderView::Fixed);
    m_table->horizontalHeader()->resizeSection(kColState, 64);
    m_table->horizontalHeader()->setSectionResizeMode(kColAddr, QHeaderView::Stretch);
    m_table->setMinimumHeight(120);
    tableLayout->addWidget(m_table);

    auto *logRow = new QHBoxLayout;
    auto *logGroup = new QGroupBox(tr("事件日志"), statusSplitter);
    auto *logLayout = new QVBoxLayout(logGroup);
    logLayout->setContentsMargins(8, 10, 8, 8);
    auto *logTitle = new QLabel(tr("登记、死亡、复活、受击、攻击、禁射和离线事件"), logGroup);
    QFont logTitleFont = logTitle->font();
    logTitleFont.setBold(true);
    logTitle->setFont(logTitleFont);
    auto *clearBtn = new QPushButton(tr("清空日志"), logGroup);
    connect(clearBtn, &QPushButton::clicked, this, [this] {
        if (m_log) m_log->clear();
    });
    logRow->addWidget(logTitle, 1);
    logRow->addStretch();
    logRow->addWidget(clearBtn);
    logLayout->addLayout(logRow);

    m_log = new QPlainTextEdit(logGroup);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);
    logLayout->addWidget(m_log);

    workspace->addWidget(controlScroll);
    workspace->addWidget(statusSplitter);
    statusSplitter->addWidget(tableGroup);
    statusSplitter->addWidget(logGroup);
    workspace->setStretchFactor(0, 0);
    workspace->setStretchFactor(1, 1);
    workspace->setSizes({380, 900});
    statusSplitter->setStretchFactor(0, 3);
    statusSplitter->setStretchFactor(1, 2);
    root->addWidget(workspace, 1);

    setCentralWidget(central);
    resize(1280, 800);
    onTickerPresetChanged(0);
    updateTeamPairControls();
}

void MainWindow::startListen()
{
    if (m_listening) return;
    const quint16 port = static_cast<quint16>(m_portEdit->value());
    if (!m_socket->bind(QHostAddress::AnyIPv4, port)) {
        m_connLabel->setText(tr("绑定 UDP 端口 %1 失败: %2\n请更换端口或关闭占用该端口的程序后重试。")
                                 .arg(port)
                                 .arg(m_socket->errorString()));
        return;
    }
    m_listening = true;
    m_portEdit->setEnabled(false);
    m_listenBtn->setText(tr("停止监听"));
    updateListenInfo();
    onLogMessage(tr("已在 UDP 端口 %1 开始监听").arg(port));
}

void MainWindow::stopListen()
{
    if (!m_listening) return;
    m_socket->close();
    m_listening = false;
    m_portEdit->setEnabled(true);
    m_listenBtn->setText(tr("启动监听"));
    m_connLabel->setText(tr("未在监听。"));
    onLogMessage(tr("已停止监听"));
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
}

QScreen *MainWindow::selectedScreen() const
{
    const auto screens = QGuiApplication::screens();
    if (screens.isEmpty()) return QGuiApplication::primaryScreen();

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
        return;
    }

    m_broadcast->showOnScreen(selectedScreen());
}

void MainWindow::onBroadcastScreenChanged(int)
{
    if (m_broadcast && m_broadcast->isVisible())
        m_broadcast->showOnScreen(selectedScreen());
}

void MainWindow::onToggleMatch()
{
    if (!m_broadcast) return;

    if (m_broadcast->isMatchRunning()) {
        m_broadcast->pauseMatch();
        // V2: 比赛暂停只影响 HUD，不向 ESP32 发命令
    } else {
        m_broadcast->startMatch();
        // V2: 对所有已分配 robot_id 单播 GAME_START，ESP32 收到后自设 HP=300
        if (m_commander && m_robots) {
            int sent = 0;
            for (const auto &robot : m_robots->robots()) {
                if (robot.protocolVersion == proto::kVersion2 && robot.robotId != 0
                    && robot.online) {
                    m_commander->sendGameStart(robot.robotId);
                    ++sent;
                }
            }
            if (sent > 0)
                onLogMessage(QStringLiteral("[比赛] 已向 %1 台 V2 机器人发送 GAME_START")
                                 .arg(sent));
        }
    }
}

void MainWindow::onTerminateMatch()
{
    if (m_broadcast)
        m_broadcast->terminateMatch();
    // V2: 向所有 V2 机器人发送 GAME_END
    if (m_commander && m_robots) {
        int sent = 0;
        for (const auto &robot : m_robots->robots()) {
            if (robot.protocolVersion == proto::kVersion2 && robot.robotId != 0
                && robot.online) {
                m_commander->sendGameEnd(robot.robotId);
                ++sent;
            }
        }
        if (sent > 0)
            onLogMessage(QStringLiteral("[比赛] 已向 %1 台 V2 机器人发送 GAME_END")
                             .arg(sent));
    }
}

void MainWindow::onResetMatch()
{
    if (m_broadcast)
        m_broadcast->resetMatch();
    if (m_redScoreEdit)
        m_redScoreEdit->setValue(0);
    if (m_blueScoreEdit)
        m_blueScoreEdit->setValue(0);
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
    onLogMessage(tr("[动画] 已开始播放%1动画预览")
                     .arg(type == QStringLiteral("bluewin") ? tr("蓝方胜利")
                                                               : tr("红方胜利")));
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
    if (!m_matchServer->start(port))
        return;

    m_clientPortEdit->setEnabled(false);
}

void MainWindow::stopClientServer()
{
    if (!m_matchServer)
        return;

    m_matchServer->stop();
    if (m_clientPortEdit)
        m_clientPortEdit->setEnabled(true);
}

void MainWindow::publishMatchState()
{
    if (!m_matchServer || !m_broadcast)
        return;

    m_matchServer->publishMatchState({
        {QStringLiteral("type"), QStringLiteral("match_state")},
        {QStringLiteral("remainingSeconds"), m_broadcast->remainingSeconds()},
        {QStringLiteral("running"), m_broadcast->isMatchRunning()},
        {QStringLiteral("redScore"), m_broadcast->redScore()},
        {QStringLiteral("blueScore"), m_broadcast->blueScore()},
        {QStringLiteral("ended"), m_broadcast->roundEnded()},
        {QStringLiteral("settlement"), m_broadcast->settlementType()},
        {QStringLiteral("redName"), m_broadcast->redTeamName()},
        {QStringLiteral("blueName"), m_broadcast->blueTeamName()}
    });
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

    if (m_programModeEdit && m_programModeEdit->currentData().toBool()
        && m_autoSwitchTimer && !m_autoSwitchTimer->isActive()) {
        onProgramModeChanged(m_programModeEdit->currentIndex());
    }
}

void MainWindow::onAutoSwitchTimeout()
{
    if (!m_programSourceEdit || m_programSourceEdit->count() < 2)
        return;

    const int nextIndex = (m_programSourceEdit->currentIndex() + 1)
                          % m_programSourceEdit->count();
    m_programSourceEdit->setCurrentIndex(nextIndex);
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
        onLogMessage(tr("[字幕] 发布失败：字幕内容不能为空"));
        return;
    }

    m_broadcast->setTickerText(text);
    m_broadcast->setTickerVisible(true);
    if (m_tickerToggleBtn)
        m_tickerToggleBtn->setText(tr("隐藏字幕"));
    onLogMessage(tr("[字幕] 已发布：%1").arg(text));
}

void MainWindow::onToggleTicker()
{
    if (!m_broadcast)
        return;

    const bool visible = !m_broadcast->tickerVisible();
    if (visible && m_broadcast->tickerText().trimmed().isEmpty()) {
        onLogMessage(tr("[字幕] 请先输入或选择字幕内容"));
        return;
    }

    m_broadcast->setTickerVisible(visible);
    if (m_tickerToggleBtn)
        m_tickerToggleBtn->setText(visible ? tr("隐藏字幕") : tr("显示字幕"));
}

void MainWindow::onTeamNamesChanged()
{
    if (!m_broadcast)
        return;

    const QString redName = m_redTeamNameEdit ? m_redTeamNameEdit->text() : tr("红方");
    const QString blueName = m_blueTeamNameEdit ? m_blueTeamNameEdit->text() : tr("蓝方");
    m_broadcast->setTeamNames(redName, blueName);
    updateProgramSourceList();
    updateTeamPairControls();
    publishMatchState();
}

void MainWindow::onImportTeamTable()
{
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
        onLogMessage(tr("[队伍表] 导入失败: %1").arg(errorMessage));
        return;
    }

    applyTeamPair(0, true);
    onLogMessage(tr("[队伍表] 已导入 %1 组队伍: %2")
                     .arg(m_teamPairs.size())
                     .arg(QFileInfo(filePath).fileName()));
}

void MainWindow::onNextTeamPair()
{
    if (m_teamPairs.isEmpty()) {
        onLogMessage(tr("[队伍表] 尚未导入队伍名称表"));
        return;
    }

    const int nextIndex = m_currentTeamPairIndex < 0
                              ? 0
                              : (m_currentTeamPairIndex + 1) % m_teamPairs.size();
    applyTeamPair(nextIndex, true);
    onLogMessage(tr("[队伍表] 已切换到第 %1 / %2 组")
                     .arg(nextIndex + 1)
                     .arg(m_teamPairs.size()));
}

bool MainWindow::importTeamTable(const QString &filePath, QString *errorMessage)
{
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
    return true;
}

void MainWindow::updateTeamPairControls()
{
    if (!m_teamPairLabel || !m_nextTeamBtn)
        return;

    if (m_teamPairs.isEmpty() || m_currentTeamPairIndex < 0
        || m_currentTeamPairIndex >= m_teamPairs.size()) {
        m_teamPairLabel->setText(tr("未导入队伍表"));
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

    m_currentTeamPairIndex = index;
    const TeamPair &pair = m_teamPairs.at(index);
    {
        const QSignalBlocker redBlocker(m_redTeamNameEdit);
        const QSignalBlocker blueBlocker(m_blueTeamNameEdit);
        m_redTeamNameEdit->setText(pair.redName);
        m_blueTeamNameEdit->setText(pair.blueName);
    }

    m_broadcast->setTeamNames(pair.redName, pair.blueName);
    if (resetMatch) {
        m_broadcast->resetMatch();
        if (m_redScoreEdit)
            m_redScoreEdit->setValue(0);
        if (m_blueScoreEdit)
            m_blueScoreEdit->setValue(0);
    }
    updateProgramSourceList();
    updateTeamPairControls();
    publishMatchState();
}

void MainWindow::onScoresChanged()
{
    if (!m_broadcast)
        return;
    const int redScore = m_redScoreEdit ? m_redScoreEdit->value() : 0;
    const int blueScore = m_blueScoreEdit ? m_blueScoreEdit->value() : 0;
    m_broadcast->setScores(redScore, blueScore);
}

void MainWindow::onAwardRedCard()
{
    if (!m_table || !m_broadcast) {
        return;
    }
    const int row = m_table->currentRow();
    auto *teamItem = row >= 0 ? m_table->item(row, kColTeam) : nullptr;
    if (!teamItem) {
        onLogMessage(tr("请先在机器人状态表中选中需要判罚的机器人"));
        return;
    }
    m_broadcast->awardCard(static_cast<quint8>(teamItem->text().toInt()), true);
    onLogMessage(tr("[牌面] 已为队伍 %1 显示红牌").arg(teamItem->text()));
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
        onLogMessage(tr("请先在机器人状态表中选中需要判罚的机器人"));
        return;
    }
    const quint8 team = static_cast<quint8>(teamItem->text().toInt());
    const quint8 robotId = static_cast<quint8>(idItem->text().toInt());
    m_broadcast->awardCard(team, false);
    onLogMessage(tr("[牌面] 已为队伍 %1 显示黄牌").arg(team));
    // V2: 同时向 ESP32 下发黄牌命令（L431 内部扣分，第 3 次判负）
    if (m_commander) {
        const quint32 txid = m_commander->sendYellowCard(robotId);
        if (txid == 0)
            onLogMessage(tr("[牌面] 机器人 %1 未连接 ESP32，黄牌仅显示未下发")
                             .arg(robotId));
    }
}

void MainWindow::onClearCards()
{
    if (m_broadcast)
        m_broadcast->clearCards();
    onLogMessage(tr("[牌面] 已清除红牌/黄牌显示"));
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
}

void MainWindow::onResetLayout()
{
    if (!m_broadcast)
        return;

    m_broadcast->resetLayoutPositions();
    refreshLayoutPositionEditors();
    onLogMessage(tr("[布局] 已恢复默认位置和尺寸"));
}

void MainWindow::onSaveLayout()
{
    if (!m_broadcast)
        return;

    if (m_broadcast->saveLayout(layoutFilePath()))
        onLogMessage(tr("[布局] 已保存到 %1").arg(layoutFilePath()));
    else
        onLogMessage(tr("[布局] 保存失败: %1").arg(layoutFilePath()));
}

void MainWindow::onLoadLayout()
{
    if (!m_broadcast)
        return;

    if (m_broadcast->loadLayout(layoutFilePath())) {
        refreshLayoutPositionEditors();
        onLogMessage(tr("[布局] 已从 %1 加载").arg(layoutFilePath()));
    } else {
        onLogMessage(tr("[布局] 未找到可用布局文件: %1").arg(layoutFilePath()));
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

    // Remove every existing thumbnail; the layout only ever holds stretch +
    // per-source cells. Rebuilding is cheap because the source list changes
    // far less often than the frame rate.
    while (QLayoutItem *item = m_sourcePreviewLayout->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }
    m_sourcePreviewViews.clear();
    m_sourcePreviewLayout->addStretch(1);

    const QString redName = m_redTeamNameEdit && !m_redTeamNameEdit->text().trimmed().isEmpty()
                                ? m_redTeamNameEdit->text().trimmed()
                                : tr("红方");
    const QString blueName = m_blueTeamNameEdit && !m_blueTeamNameEdit->text().trimmed().isEmpty()
                                  ? m_blueTeamNameEdit->text().trimmed()
                                  : tr("蓝方");

    int added = 0;
    for (const auto &source : m_videoSources) {
        if (!source.online || source.sourceId.isEmpty()
            || (source.team != 1 && source.team != 2))
            continue;

        auto *cell = new QWidget;
        auto *cellLayout = new QVBoxLayout(cell);
        cellLayout->setContentsMargins(0, 0, 0, 0);
        cellLayout->setSpacing(4);

        auto *thumb = new QLabel(cell);
        thumb->setFixedSize(192, 108);
        thumb->setAlignment(Qt::AlignCenter);
        thumb->setStyleSheet(QStringLiteral(
            "QLabel { background: #101418; border: 2px solid #2c333b;"
            " border-radius: 4px; color: #6c7886; }"));
        thumb->setText(tr("无信号"));
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
            tr("%1 · %2号\n%3").arg(teamName).arg(source.robotId > 0 ? source.robotId : 1)
                               .arg(display), cell);
        caption->setAlignment(Qt::AlignCenter);
        caption->setStyleSheet(QStringLiteral("color: #cfd7df; font-size: 11px;"));
        caption->setWordWrap(true);

        cellLayout->addWidget(thumb);
        cellLayout->addWidget(caption);

        m_sourcePreviewViews.insert(sourceId, thumb);
        m_sourcePreviewLayout->insertWidget(m_sourcePreviewLayout->count() - 1,
                                            cell);
        ++added;
    }

    if (added == 0) {
        auto *empty = new QLabel(tr("暂无已登记选手端"), m_sourcePreviewGroup);
        empty->setAlignment(Qt::AlignCenter);
        empty->setStyleSheet(QStringLiteral("color: #6c7886; padding: 24px;"));
        m_sourcePreviewLayout->insertWidget(0, empty);
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
            "QLabel { background: #101418; border: 2px solid %1;"
            " border-radius: 4px; color: #6c7886; }")
                                 .arg(isActive ? QStringLiteral("#ffd76a")
                                               : QStringLiteral("#2c333b")));
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
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::onSourceFrameUpdated(const QString &sourceId)
{
    QLabel *thumb = m_sourcePreviewViews.value(sourceId);
    if (!thumb || !m_broadcast)
        return;
    const QImage frame = m_broadcast->sourceFrame(sourceId);
    if (frame.isNull())
        return;
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
        if (dg.isValid())
            m_robots->handleDatagram(dg.data(), dg.senderAddress(), dg.senderPort());
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

void MainWindow::onEndpointLearned(quint8 robotId, const QHostAddress &ip,
                                   quint16 port)
{
    if (m_commander)
        m_commander->learnEndpoint(robotId, ip, port);
}

// ---------- V2 设备管理 ----------

void MainWindow::onAssignTeam()
{
    if (!m_robots || !m_deviceRobotEdit || !m_deviceTeamEdit)
        return;
    const quint8 robotId = static_cast<quint8>(m_deviceRobotEdit->value());
    const quint8 team = static_cast<quint8>(m_deviceTeamEdit->currentData().toInt());
    m_robots->setTeamForRobot(robotId, team);
    onLogMessage(tr("[设备] 机器人 %1 分配到 %2")
                     .arg(robotId)
                     .arg(team == 0 ? tr("未分配") : (team == 1 ? tr("红方") : tr("蓝方"))));
    refreshTable();
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
}

void MainWindow::onSetHp()
{
    if (!m_commander || !m_deviceRobotEdit || !m_deviceHpEdit)
        return;
    const quint8 robotId = static_cast<quint8>(m_deviceRobotEdit->value());
    const quint16 hp = static_cast<quint16>(m_deviceHpEdit->value());
    const quint32 txid = m_commander->sendSetHp(robotId, hp);
    if (txid)
        onLogMessage(tr("[设备] 已向机器人 %1 发送 SET_HP=%2 txid=%3").arg(robotId).arg(hp).arg(txid));
    else
        onLogMessage(tr("[设备] 机器人 %1 未知端点，无法发送 SET_HP").arg(robotId));
}

void MainWindow::onRequestStatus()
{
    if (!m_commander || !m_deviceRobotEdit)
        return;
    const quint8 robotId = static_cast<quint8>(m_deviceRobotEdit->value());
    m_commander->sendStatusRequest(robotId);
    onLogMessage(tr("[设备] 已向机器人 %1 发送 STATUS_REQUEST").arg(robotId));
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
            r.protocolVersion == proto::kVersion2
                ? (r.powerOn ? tr("已通电") : tr("已断电")) : tr("-"));
        powerOnItem->setForeground(r.powerOn ? green : red);
        powerOnItem->setTextAlignment(Qt::AlignCenter);
        auto linkItem = new QTableWidgetItem(
            r.protocolVersion == proto::kVersion2
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
        const QString src = r.addr.isNull()
                                 ? QString()
                                 : QStringLiteral("%1:%2").arg(r.addr.toString()).arg(r.port);
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
    m_table->resizeColumnsToContents();
    m_table->horizontalHeader()->setSectionResizeMode(kColAddr, QHeaderView::Stretch);

    statusBar()->showMessage(tr("在线 %1 / %2 台机器人").arg(onlineCount).arg(kExpectedRobots));

    if (m_broadcast)
        m_broadcast->updateRobots(bots);
    if (m_matchServer)
        m_matchServer->publishRobots(bots);
}

void MainWindow::onLogMessage(const QString &message)
{
    if (!m_log) return;
    m_log->appendPlainText(QStringLiteral("[%1] %2")
                               .arg(QTime::currentTime().toString(QStringLiteral("hh:mm:ss.zzz")),
                                    message));
}
