#include "mainwindow.h"
#include "broadcastwindow.h"
#include "matchprotocol.h"
#include "matchserver.h"
#include "protocol.h"
#include "robotmanager.h"

#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
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
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QShortcut>
#include <QSignalBlocker>
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
constexpr int kColState = 4;
constexpr int kColShoot = 5;
constexpr int kColAddr  = 6;

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

    m_robots = new RobotManager(this);
    connect(m_robots, &RobotManager::robotsChanged, this, &MainWindow::refreshTable);
    connect(m_robots, &RobotManager::logMessage, this, &MainWindow::onLogMessage);
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
    m_broadcast->loadLayout(layoutFilePath());
    refreshLayoutPositionEditors();
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
    startListen();
    startClientServer();
    publishMatchState();
    updateProgramSourceList();
    refreshTable();

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
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(10, 8, 10, 8);

    auto *title = new QLabel(tr("赛事转播控制台"), central);
    QFont titleFont = title->font();
    titleFont.setPointSize(15);
    titleFont.setBold(true);
    title->setFont(titleFont);
    root->addWidget(title);

    m_connLabel = new QLabel(central);
    m_connLabel->setWordWrap(true);
    root->addWidget(m_connLabel);

    auto *listenRow = new QHBoxLayout;
    listenRow->addWidget(new QLabel(tr("监听 UDP 端口:"), central));
    m_portEdit = new QSpinBox(central);
    m_portEdit->setRange(1, 65535);
    m_portEdit->setValue(proto::kBindPort);
    m_listenBtn = new QPushButton(tr("启动监听"), central);
    connect(m_listenBtn, &QPushButton::clicked, this, &MainWindow::onToggleListen);
    listenRow->addWidget(m_portEdit);
    listenRow->addWidget(m_listenBtn);
    listenRow->addStretch();
    root->addLayout(listenRow);

    auto *broadcastRow = new QHBoxLayout;
    broadcastRow->addWidget(new QLabel(tr("转播输出屏幕:"), central));
    m_screenEdit = new QComboBox(central);
    m_screenEdit->setMinimumWidth(220);
    connect(m_screenEdit, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::onBroadcastScreenChanged);
    m_broadcastBtn = new QPushButton(tr("显示转播画面"), central);
    connect(m_broadcastBtn, &QPushButton::clicked, this, &MainWindow::onShowBroadcast);
    m_matchBtn = new QPushButton(tr("开始比赛"), central);
    connect(m_matchBtn, &QPushButton::clicked, this, &MainWindow::onToggleMatch);
    m_resetMatchBtn = new QPushButton(tr("重置比赛"), central);
    connect(m_resetMatchBtn, &QPushButton::clicked, this, &MainWindow::onResetMatch);
    broadcastRow->addWidget(m_screenEdit);
    broadcastRow->addWidget(m_broadcastBtn);
    broadcastRow->addWidget(m_matchBtn);
    broadcastRow->addWidget(m_resetMatchBtn);
    broadcastRow->addStretch();
    root->addLayout(broadcastRow);

    auto *teamNameRow = new QHBoxLayout;
    teamNameRow->addWidget(new QLabel(tr("红方队名:"), central));
    m_redTeamNameEdit = new QLineEdit(tr("红方"), central);
    m_redTeamNameEdit->setMinimumWidth(150);
    teamNameRow->addWidget(m_redTeamNameEdit);
    teamNameRow->addWidget(new QLabel(tr("蓝方队名:"), central));
    m_blueTeamNameEdit = new QLineEdit(tr("蓝方"), central);
    m_blueTeamNameEdit->setMinimumWidth(150);
    teamNameRow->addWidget(m_blueTeamNameEdit);
    auto *applyTeamNamesButton = new QPushButton(tr("应用队名"), central);
    connect(applyTeamNamesButton, &QPushButton::clicked,
            this, &MainWindow::onTeamNamesChanged);
    connect(m_redTeamNameEdit, &QLineEdit::editingFinished,
            this, &MainWindow::onTeamNamesChanged);
    connect(m_blueTeamNameEdit, &QLineEdit::editingFinished,
            this, &MainWindow::onTeamNamesChanged);
    teamNameRow->addWidget(applyTeamNamesButton);
    teamNameRow->addStretch();
    root->addLayout(teamNameRow);

    auto *teamTableRow = new QHBoxLayout;
    teamTableRow->addWidget(new QLabel(tr("队伍名称表:"), central));
    m_importTeamsBtn = new QPushButton(tr("导入队伍表"), central);
    m_nextTeamBtn = new QPushButton(tr("下一组队伍"), central);
    m_teamPairLabel = new QLabel(central);
    m_teamPairLabel->setWordWrap(true);
    connect(m_importTeamsBtn, &QPushButton::clicked,
            this, &MainWindow::onImportTeamTable);
    connect(m_nextTeamBtn, &QPushButton::clicked,
            this, &MainWindow::onNextTeamPair);
    teamTableRow->addWidget(m_importTeamsBtn);
    teamTableRow->addWidget(m_nextTeamBtn);
    teamTableRow->addWidget(m_teamPairLabel, 1);
    root->addLayout(teamTableRow);

    auto *scoreRow = new QHBoxLayout;
    scoreRow->addWidget(new QLabel(tr("红方小局积分:"), central));
    m_redScoreEdit = new QSpinBox(central);
    m_redScoreEdit->setRange(0, 99);
    m_redScoreEdit->setValue(0);
    scoreRow->addWidget(m_redScoreEdit);
    scoreRow->addWidget(new QLabel(tr("蓝方小局积分:"), central));
    m_blueScoreEdit = new QSpinBox(central);
    m_blueScoreEdit->setRange(0, 99);
    m_blueScoreEdit->setValue(0);
    scoreRow->addWidget(m_blueScoreEdit);
    connect(m_redScoreEdit, qOverload<int>(&QSpinBox::valueChanged),
            this, &MainWindow::onScoresChanged);
    connect(m_blueScoreEdit, qOverload<int>(&QSpinBox::valueChanged),
            this, &MainWindow::onScoresChanged);
    scoreRow->addWidget(new QLabel(tr("牌面:"), central));
    m_redCardBtn = new QPushButton(tr("红牌"), central);
    m_yellowCardBtn = new QPushButton(tr("黄牌"), central);
    m_clearCardsBtn = new QPushButton(tr("清除牌面"), central);
    connect(m_redCardBtn, &QPushButton::clicked, this, &MainWindow::onAwardRedCard);
    connect(m_yellowCardBtn, &QPushButton::clicked, this, &MainWindow::onAwardYellowCard);
    connect(m_clearCardsBtn, &QPushButton::clicked, this, &MainWindow::onClearCards);
    scoreRow->addWidget(m_redCardBtn);
    scoreRow->addWidget(m_yellowCardBtn);
    scoreRow->addWidget(m_clearCardsBtn);
    scoreRow->addWidget(new QLabel(tr("先在下方表格选中机器人"), central));
    scoreRow->addStretch();
    root->addLayout(scoreRow);

    auto *programRow = new QHBoxLayout;
    programRow->addWidget(new QLabel(tr("导播模式:"), central));
    m_programModeEdit = new QComboBox(central);
    m_programModeEdit->addItem(tr("手动切换"), false);
    m_programModeEdit->addItem(tr("自动切换"), true);
    connect(m_programModeEdit, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::onProgramModeChanged);
    programRow->addWidget(m_programModeEdit);
    programRow->addWidget(new QLabel(tr("当前视角:"), central));
    m_programSourceEdit = new QComboBox(central);
    m_programSourceEdit->setMinimumWidth(260);
    connect(m_programSourceEdit, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::onProgramSourceChanged);
    programRow->addWidget(m_programSourceEdit, 1);
    programRow->addWidget(new QLabel(tr("自动间隔(秒):"), central));
    m_autoSwitchIntervalEdit = new QSpinBox(central);
    m_autoSwitchIntervalEdit->setRange(1, 60);
    m_autoSwitchIntervalEdit->setValue(5);
    m_autoSwitchIntervalEdit->setSuffix(tr(" 秒"));
    connect(m_autoSwitchIntervalEdit, qOverload<int>(&QSpinBox::valueChanged),
            this, [this](int seconds) {
                if (m_autoSwitchTimer && m_autoSwitchTimer->isActive())
                    m_autoSwitchTimer->start(seconds * 1000);
            });
    programRow->addWidget(m_autoSwitchIntervalEdit);
    root->addLayout(programRow);

    auto *tickerRow = new QHBoxLayout;
    tickerRow->addWidget(new QLabel(tr("底部跑马字幕:"), central));
    m_tickerPresetEdit = new QComboBox(central);
    m_tickerPresetEdit->setMinimumWidth(220);
    m_tickerPresetEdit->addItem(tr("欢迎词"), tr("欢迎来到赛事转播现场 · 比赛即将开始"));
    m_tickerPresetEdit->addItem(tr("比赛进行中"), tr("红方 vs 蓝方 · 精彩对决进行中"));
    m_tickerPresetEdit->addItem(tr("秩序提示"), tr("请各参赛队伍注意比赛秩序，听从裁判指示"));
    m_tickerPresetEdit->addItem(tr("现场编写"), QString());
    connect(m_tickerPresetEdit, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::onTickerPresetChanged);
    tickerRow->addWidget(m_tickerPresetEdit);

    m_tickerTextEdit = new QLineEdit(central);
    m_tickerTextEdit->setPlaceholderText(tr("输入要展示的字幕内容"));
    m_tickerTextEdit->setMinimumWidth(340);
    tickerRow->addWidget(m_tickerTextEdit, 1);

    m_tickerApplyBtn = new QPushButton(tr("发布字幕"), central);
    m_tickerApplyBtn->setToolTip(tr("将当前文字发布到节目画面底部"));
    connect(m_tickerApplyBtn, &QPushButton::clicked, this, &MainWindow::onApplyTicker);
    tickerRow->addWidget(m_tickerApplyBtn);

    m_tickerToggleBtn = new QPushButton(tr("显示字幕"), central);
    m_tickerToggleBtn->setToolTip(tr("显示或隐藏节目画面底部字幕条"));
    connect(m_tickerToggleBtn, &QPushButton::clicked, this, &MainWindow::onToggleTicker);
    tickerRow->addWidget(m_tickerToggleBtn);

    root->addLayout(tickerRow);

    auto *layoutRow = new QHBoxLayout;
    layoutRow->addWidget(new QLabel(tr("布局元素:"), central));
    m_layoutElementEdit = new QComboBox(central);
    m_layoutElementEdit->setMinimumWidth(190);
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
    layoutRow->addWidget(m_layoutElementEdit);
    layoutRow->addWidget(new QLabel(tr("X 偏移:"), central));
    m_layoutXEdit = new QSpinBox(central);
    m_layoutXEdit->setRange(-2000, 2000);
    m_layoutXEdit->setSuffix(tr(" px"));
    connect(m_layoutXEdit, qOverload<int>(&QSpinBox::valueChanged),
            this, &MainWindow::onLayoutPositionChanged);
    layoutRow->addWidget(m_layoutXEdit);
    layoutRow->addWidget(new QLabel(tr("Y 偏移:"), central));
    m_layoutYEdit = new QSpinBox(central);
    m_layoutYEdit->setRange(-2000, 2000);
    m_layoutYEdit->setSuffix(tr(" px"));
    connect(m_layoutYEdit, qOverload<int>(&QSpinBox::valueChanged),
            this, &MainWindow::onLayoutPositionChanged);
    layoutRow->addWidget(m_layoutYEdit);
    layoutRow->addWidget(new QLabel(tr("宽:"), central));
    m_layoutWidthEdit = new QSpinBox(central);
    m_layoutWidthEdit->setRange(1, 4000);
    m_layoutWidthEdit->setSuffix(tr(" px"));
    connect(m_layoutWidthEdit, qOverload<int>(&QSpinBox::valueChanged),
            this, &MainWindow::onLayoutSizeChanged);
    layoutRow->addWidget(m_layoutWidthEdit);
    layoutRow->addWidget(new QLabel(tr("高:"), central));
    m_layoutHeightEdit = new QSpinBox(central);
    m_layoutHeightEdit->setRange(1, 4000);
    m_layoutHeightEdit->setSuffix(tr(" px"));
    connect(m_layoutHeightEdit, qOverload<int>(&QSpinBox::valueChanged),
            this, &MainWindow::onLayoutSizeChanged);
    layoutRow->addWidget(m_layoutHeightEdit);
    m_resetLayoutBtn = new QPushButton(tr("恢复默认布局"), central);
    m_saveLayoutBtn = new QPushButton(tr("保存布局"), central);
    m_loadLayoutBtn = new QPushButton(tr("加载布局"), central);
    connect(m_resetLayoutBtn, &QPushButton::clicked, this, &MainWindow::onResetLayout);
    connect(m_saveLayoutBtn, &QPushButton::clicked, this, &MainWindow::onSaveLayout);
    connect(m_loadLayoutBtn, &QPushButton::clicked, this, &MainWindow::onLoadLayout);
    layoutRow->addWidget(m_resetLayoutBtn);
    layoutRow->addWidget(m_saveLayoutBtn);
    layoutRow->addWidget(m_loadLayoutBtn);
    auto *layoutHint = new QLabel(tr("正值向右/下；宽高为组件实际像素尺寸；布局文件保存在程序目录"), central);
    layoutHint->setStyleSheet(QStringLiteral("color: #65717c;"));
    layoutRow->addWidget(layoutHint, 1);
    root->addLayout(layoutRow);
    refreshLayoutPositionEditors();

    auto *clientRow = new QHBoxLayout;
    clientRow->addWidget(new QLabel(tr("选手端登记 TCP 端口:"), central));
    m_clientPortEdit = new QSpinBox(central);
    m_clientPortEdit->setRange(1, 65535);
    m_clientPortEdit->setValue(matchproto::kControlPort);
    m_clientListenBtn = new QPushButton(tr("启动登记服务"), central);
    connect(m_clientListenBtn, &QPushButton::clicked, this, &MainWindow::onToggleClientServer);
    m_clientStateLabel = new QLabel(tr("未启动"), central);
    m_clientStateLabel->setWordWrap(true);
    clientRow->addWidget(m_clientPortEdit);
    clientRow->addWidget(m_clientListenBtn);
    clientRow->addWidget(m_clientStateLabel, 1);
    root->addLayout(clientRow);

    auto *tableTitle = new QLabel(tr("机器人状态"), central);
    QFont tableTitleFont = tableTitle->font();
    tableTitleFont.setBold(true);
    tableTitle->setFont(tableTitleFont);
    root->addWidget(tableTitle);

    m_table = new QTableWidget(0, 7, central);
    m_table->setHorizontalHeaderLabels({tr("机器人ID"), tr("队伍"), tr("血量 HP"),
                                        tr("热量"), tr("状态"), tr("射击"), tr("来源地址")});
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->horizontalHeader()->setSectionResizeMode(kColState, QHeaderView::Fixed);
    m_table->horizontalHeader()->resizeSection(kColState, 64);
    m_table->horizontalHeader()->setSectionResizeMode(kColAddr, QHeaderView::Stretch);
    m_table->setMinimumHeight(220);
    root->addWidget(m_table, /*stretch=*/1);

    auto *logRow = new QHBoxLayout;
    auto *logTitle = new QLabel(tr("事件日志(登记/死亡/复活/受击/攻击/禁射/离线)"), central);
    QFont logTitleFont = logTitle->font();
    logTitleFont.setBold(true);
    logTitle->setFont(logTitleFont);
    auto *clearBtn = new QPushButton(tr("清空日志"), central);
    connect(clearBtn, &QPushButton::clicked, this, [this] {
        if (m_log) m_log->clear();
    });
    logRow->addWidget(logTitle);
    logRow->addStretch();
    logRow->addWidget(clearBtn);
    root->addLayout(logRow);

    m_log = new QPlainTextEdit(central);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);
    m_log->setMaximumHeight(150);
    root->addWidget(m_log);

    setCentralWidget(central);
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

    if (m_broadcast->isMatchRunning())
        m_broadcast->pauseMatch();
    else
        m_broadcast->startMatch();
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
        {QStringLiteral("redName"), m_broadcast->redTeamName()},
        {QStringLiteral("blueName"), m_broadcast->blueTeamName()}
    });
}

void MainWindow::onProgramModeChanged(int)
{
    const bool automatic = m_programModeEdit
                               && m_programModeEdit->currentData().toBool();
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
    if (!teamItem) {
        onLogMessage(tr("请先在机器人状态表中选中需要判罚的机器人"));
        return;
    }
    m_broadcast->awardCard(static_cast<quint8>(teamItem->text().toInt()), false);
    onLogMessage(tr("[牌面] 已为队伍 %1 显示黄牌").arg(teamItem->text()));
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

void MainWindow::refreshTable()
{
    if (!m_table) return;

    const auto allBots = m_robots->robots();
    QVector<RobotManager::RobotInfo> bots;
    for (const auto &robot : allBots) {
        // 机器人身份由协议中的 team + robotId 决定，不使用发送 IP 作为主键。
        // 不限制 robotId，方便在同一台电脑上用多个模拟节点联调。
        if (robot.team == kRedTeam || robot.team == kBlueTeam)
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
        m_table->setItem(row, kColState, stateItem);
        m_table->setItem(row, kColShoot, shootItem);
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
