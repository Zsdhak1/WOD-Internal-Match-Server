#include "clientwindow.h"

#include "matchprotocol.h"

#include <QAbstractSocket>
#include <QCamera>
#include <QCameraDevice>
#include <QCameraFormat>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QIntValidator>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScreen>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QStyle>
#include <QTcpSocket>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVideoFrame>
#include <QVideoSink>
#include <QDir>

namespace {
constexpr int kRedTeam = 1;
constexpr int kBlueTeam = 2;
constexpr int kDefaultRobotId = 1;

QString teamText(int team)
{
    if (team == kRedTeam)
        return QStringLiteral("红方");
    if (team == kBlueTeam)
        return QStringLiteral("蓝方");
    return QStringLiteral("未登记");
}

QColor teamColor(int team)
{
    return team == kRedTeam ? QColor(QStringLiteral("#ff6872"))
                            : QColor(QStringLiteral("#72b4ff"));
}

// A stable id derived from the OS-level device id. Two different Qt backends
// (FFmpeg vs WindowsMF) may describe the same physical camera with different
// names but the same persistent id, so we hash the id rather than the display
// name.
QString sourceIdForDevice(const QByteArray &deviceId)
{
    const QByteArray digest = QCryptographicHash::hash(deviceId,
                                                        QCryptographicHash::Sha1)
                                  .toHex();
    return QStringLiteral("camera://windows/%1").arg(QString::fromLatin1(digest));
}

int builtInCameraScore(const QString &name)
{
    const QString normalized = name.toLower();
    static const QStringList strongHints = {
        QStringLiteral("integrated"), QStringLiteral("built-in"),
        QStringLiteral("built in"), QStringLiteral("builtin"),
        QStringLiteral("internal"), QStringLiteral("内置"),
        QStringLiteral("内置摄像机"), QStringLiteral("笔记本"),
        QStringLiteral("笔记本电脑"), QStringLiteral("laptop"),
        QStringLiteral("notebook")
    };
    static const QStringList frontFacingHints = {
        QStringLiteral("user facing"), QStringLiteral("front camera"),
        QStringLiteral("front"), QStringLiteral("前置")
    };
    static const QStringList cameraHints = {
        QStringLiteral("camera"), QStringLiteral("webcam"),
        QStringLiteral("摄像头"), QStringLiteral("相机")
    };

    int score = 0;
    for (const QString &hint : strongHints) {
        if (normalized.contains(hint))
            score += 20;
    }
    for (const QString &hint : frontFacingHints) {
        if (normalized.contains(hint))
            score += 6;
    }
    for (const QString &hint : cameraHints) {
        if (normalized.contains(hint))
            score += 2;
    }
    return score;
}

void styleLabel(QLabel *label, const QColor &color, bool bold = false)
{
    if (!label)
        return;
    label->setStyleSheet(QStringLiteral("color: %1;%2")
                             .arg(color.name(), bold ? QStringLiteral(" font-weight: 700;")
                                                       : QString()));
}
} // namespace

ClientWindow::ClientWindow(QWidget *parent)
    : BroadcastWindow(parent)
{
    setWindowTitle(tr("赛事选手端"));

    m_socket = new QTcpSocket(this);
    connect(m_socket, &QTcpSocket::connected, this, &ClientWindow::onSocketConnected);
    connect(m_socket, &QTcpSocket::readyRead, this, &ClientWindow::onSocketReadyRead);
    connect(m_socket, &QTcpSocket::disconnected, this, &ClientWindow::onSocketDisconnected);
    connect(m_socket, &QTcpSocket::errorOccurred, this, &ClientWindow::onSocketError);

    m_loginTimeoutTimer = new QTimer(this);
    m_loginTimeoutTimer->setSingleShot(true);
    m_loginTimeoutTimer->setInterval(3000);
    connect(m_loginTimeoutTimer, &QTimer::timeout,
            this, &ClientWindow::onLoginTimeout);

    m_videoRestartTimer = new QTimer(this);
    m_videoRestartTimer->setSingleShot(true);
    m_videoRestartTimer->setInterval(2000);
    connect(m_videoRestartTimer, &QTimer::timeout, this, [this] {
        if (m_registered && !m_activeSourceId.isEmpty())
            startVideoPreview();
    });

    buildUi();
    refreshVideoDevices();
    showLoginPage();
}

void ClientWindow::buildUi()
{
    buildRegistrationOverlay();
    buildControlLayer();

    if (centralWidget()) {
        const QRect rect = centralWidget()->rect();
        if (m_registrationOverlay)
            m_registrationOverlay->setGeometry(rect);
        if (m_controlLayer)
            m_controlLayer->setGeometry(rect);
    }

    if (m_controlLayer)
        m_controlLayer->show();
    if (m_registrationOverlay)
        m_registrationOverlay->raise();
}

void ClientWindow::buildRegistrationOverlay()
{
    QWidget *host = centralWidget();
    if (!host)
        return;

    m_registrationOverlay = new QWidget(host);
    m_registrationOverlay->setObjectName(QStringLiteral("registrationOverlay"));
    m_registrationOverlay->setStyleSheet(QStringLiteral(
        "QWidget#registrationOverlay { background: rgba(3, 7, 11, 242); color: #f4f7fa; }"
        "QFrame#registrationCard { background: rgba(10, 18, 27, 250); border: 2px solid #3d4d5a; border-radius: 8px; }"
        "QLabel#registrationTitle { color: #f4f7fa; }"
        "QLabel#registrationSubtitle { color: #9eabb5; }"
        "QLabel#registrationPreview { background: #05080c; border: 1px solid #455661; color: #8d9aa4; }"
        "QLineEdit, QComboBox { min-height: 34px; background: #111c26; border: 1px solid #536574; border-radius: 4px; padding: 0 9px; color: #f4f7fa; }"
        "QLineEdit:focus, QComboBox:focus { border-color: #72b4ff; }"
        "QPushButton { min-height: 34px; background: #233746; border: 1px solid #5b7180; border-radius: 4px; padding: 0 14px; color: #f4f7fa; }"
        "QPushButton:hover { background: #2d4a5d; }"
        "QPushButton:pressed { background: #1a2d3b; }"
        "QPushButton#registerButton { background: #2f6d8e; border-color: #72b4ff; font-weight: 700; min-height: 42px; }"
        "QLabel#registrationStatus { color: #d5e0e6; }"));

    auto *root = new QVBoxLayout(m_registrationOverlay);
    root->setContentsMargins(24, 24, 24, 24);
    root->setSpacing(0);

    auto *registrationScroll = new QScrollArea(m_registrationOverlay);
    registrationScroll->setFrameShape(QFrame::NoFrame);
    registrationScroll->setWidgetResizable(true);
    registrationScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    registrationScroll->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    auto *registrationPage = new QWidget;
    registrationPage->setStyleSheet(QStringLiteral("background: transparent;"));
    auto *registrationPageLayout = new QVBoxLayout(registrationPage);
    registrationPageLayout->setContentsMargins(0, 0, 0, 0);
    registrationPageLayout->setSpacing(0);

    auto *card = new QFrame(registrationPage);
    card->setObjectName(QStringLiteral("registrationCard"));
    card->setMinimumWidth(0);
    card->setMaximumWidth(820);
    card->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(34, 30, 34, 30);
    cardLayout->setSpacing(13);

    auto *title = new QLabel(tr("赛事选手端"), card);
    title->setObjectName(QStringLiteral("registrationTitle"));
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSize(24);
    title->setFont(titleFont);
    cardLayout->addWidget(title);

    auto *subtitle = new QLabel(tr("登记完成后将进入与导播台一致的全屏比赛画面；视角固定为本机机器人。"),
                                card);
    subtitle->setObjectName(QStringLiteral("registrationSubtitle"));
    subtitle->setWordWrap(true);
    cardLayout->addWidget(subtitle);

    auto *networkForm = new QFormLayout;
    networkForm->setHorizontalSpacing(18);
    networkForm->setVerticalSpacing(10);
    m_serverEdit = new QLineEdit(QStringLiteral("127.0.0.1"), card);
    m_portEdit = new QLineEdit(QString::number(matchproto::kControlPort), card);
    m_portEdit->setValidator(new QIntValidator(1, 65535, m_portEdit));
    m_teamEdit = new QComboBox(card);
    m_teamEdit->addItem(tr("红方"), kRedTeam);
    m_teamEdit->addItem(tr("蓝方"), kBlueTeam);
    connect(m_teamEdit, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &ClientWindow::onTeamChanged);
    m_displayNameEdit = new QLineEdit(card);
    m_displayNameEdit->setPlaceholderText(tr("可选，例如：红方驾驶舱"));
    m_registrationRobotLabel = new QLabel(card);
    networkForm->addRow(tr("服务器地址"), m_serverEdit);
    networkForm->addRow(tr("TCP 端口"), m_portEdit);
    networkForm->addRow(tr("登记队伍"), m_teamEdit);
    networkForm->addRow(tr("自动绑定"), m_registrationRobotLabel);
    networkForm->addRow(tr("选手端名称"), m_displayNameEdit);
    cardLayout->addLayout(networkForm);

    auto *sourceTitle = new QLabel(tr("机器人视频源（USB 或内置摄像头）"), card);
    QFont sourceTitleFont = sourceTitle->font();
    sourceTitleFont.setBold(true);
    sourceTitle->setFont(sourceTitleFont);
    cardLayout->addWidget(sourceTitle);

    auto *sourceLayout = new QVBoxLayout;
    sourceLayout->setSpacing(7);
    m_registrationSourceEdit = new QComboBox(card);
    m_registrationSourceEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(m_registrationSourceEdit, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &ClientWindow::onRegistrationSourceChanged);
    auto *refreshButton = new QPushButton(tr("刷新"), card);
    connect(refreshButton, &QPushButton::clicked, this, &ClientWindow::onRefreshVideoDevices);
    auto *builtInButton = new QPushButton(tr("内置摄像头"), card);
    connect(builtInButton, &QPushButton::clicked, this, &ClientWindow::onUseBuiltInCamera);
    auto *previewButton = new QPushButton(tr("预览"), card);
    connect(previewButton, &QPushButton::clicked, this, &ClientWindow::onPreviewVideo);
    sourceLayout->addWidget(m_registrationSourceEdit);
    auto *sourceActions = new QHBoxLayout;
    sourceActions->setSpacing(7);
    sourceActions->addWidget(refreshButton, 1);
    sourceActions->addWidget(builtInButton, 1);
    sourceActions->addWidget(previewButton, 1);
    sourceLayout->addLayout(sourceActions);
    cardLayout->addLayout(sourceLayout);

    m_registrationPreview = new QLabel(card);
    m_registrationPreview->setObjectName(QStringLiteral("registrationPreview"));
    m_registrationPreview->setAlignment(Qt::AlignCenter);
    m_registrationPreview->setMinimumSize(280, 158);
    m_registrationPreview->setMaximumSize(720, 405);
    m_registrationPreview->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_registrationPreview->setText(tr("选择视频源后点击“预览”"));
    cardLayout->addWidget(m_registrationPreview, 0, Qt::AlignHCenter);

    m_loginButton = new QPushButton(tr("登记并进入全屏画面"), card);
    m_loginButton->setObjectName(QStringLiteral("registerButton"));
    connect(m_loginButton, &QPushButton::clicked, this, &ClientWindow::onLoginClicked);
    cardLayout->addWidget(m_loginButton);

    m_loginStatus = new QLabel(card);
    m_loginStatus->setObjectName(QStringLiteral("registrationStatus"));
    m_loginStatus->setWordWrap(true);
    cardLayout->addWidget(m_loginStatus);

    registrationPageLayout->addStretch(1);
    registrationPageLayout->addWidget(card, 0, Qt::AlignHCenter);
    registrationPageLayout->addStretch(1);
    registrationScroll->setWidget(registrationPage);
    root->addWidget(registrationScroll, 1);
}

void ClientWindow::buildControlLayer()
{
    m_controlLayer = interactionLayer();
    if (!m_controlLayer)
        return;

    m_controlLayer->setStyleSheet(QStringLiteral(
        "QToolButton#controlsButton { min-height: 36px; background: rgba(10, 18, 27, 230); border: 2px solid #5e7381; border-radius: 5px; padding: 0 13px; color: #f4f7fa; font-weight: 700; }"
        "QToolButton#controlsButton:hover { background: rgba(37, 66, 83, 245); }"
        "QFrame#clientControls { background: rgba(8, 15, 22, 245); border: 2px solid #536774; border-radius: 7px; color: #f4f7fa; }"
        "QLabel#clientIdentity { color: #f4f7fa; font-size: 17px; font-weight: 700; }"
        "QLabel#clientConnection { color: #72d39a; }"
        "QLabel#clientPreview { background: #05080c; border: 1px solid #455661; color: #8d9aa4; }"
        "QComboBox { min-height: 32px; background: #111c26; border: 1px solid #536574; border-radius: 4px; padding: 0 8px; color: #f4f7fa; }"
        "QPushButton { min-height: 32px; background: #233746; border: 1px solid #5b7180; border-radius: 4px; padding: 0 11px; color: #f4f7fa; }"
        "QPushButton:hover { background: #2d4a5d; }"
        "QPushButton#logoutButton { background: #512e36; border-color: #b96875; }"
        "QPlainTextEdit { background: rgba(3, 7, 11, 210); border: 1px solid #394b57; color: #c9d4da; }"));

    auto *root = new QVBoxLayout(m_controlLayer);
    root->setContentsMargins(26, 24, 26, 24);
    root->setSpacing(10);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->addStretch(1);
    m_controlsButton = new QToolButton(m_controlLayer);
    m_controlsButton->setObjectName(QStringLiteral("controlsButton"));
    m_controlsButton->setText(tr("视频设置"));
    m_controlsButton->setIcon(style()->standardIcon(QStyle::SP_FileDialogDetailedView));
    m_controlsButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_controlsButton->setToolTip(tr("打开或关闭视频源设置"));
    connect(m_controlsButton, &QToolButton::clicked, this, &ClientWindow::onToggleControls);
    buttonRow->addWidget(m_controlsButton, 0, Qt::AlignTop);
    root->addLayout(buttonRow);

    m_controlPanel = new QFrame(m_controlLayer);
    m_controlPanel->setObjectName(QStringLiteral("clientControls"));
    m_controlPanel->setMinimumWidth(360);
    m_controlPanel->setMaximumWidth(520);
    m_controlPanel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto *panelLayout = new QVBoxLayout(m_controlPanel);
    panelLayout->setContentsMargins(18, 16, 18, 16);
    panelLayout->setSpacing(9);

    m_identityLabel = new QLabel(m_controlPanel);
    m_identityLabel->setObjectName(QStringLiteral("clientIdentity"));
    panelLayout->addWidget(m_identityLabel);

    m_connectionLabel = new QLabel(m_controlPanel);
    m_connectionLabel->setObjectName(QStringLiteral("clientConnection"));
    panelLayout->addWidget(m_connectionLabel);

    auto *robotHint = new QLabel(tr("机器人 ID 按红/蓝方自动登记为 1 号；状态由导播台同步。"),
                                 m_controlPanel);
    robotHint->setWordWrap(true);
    robotHint->setStyleSheet(QStringLiteral("color: #9eabb5;"));
    panelLayout->addWidget(robotHint);

    auto *sourceLabel = new QLabel(tr("当前机器人视频源（USB 或内置摄像头）"), m_controlPanel);
    sourceLabel->setStyleSheet(QStringLiteral("color: #c9d4da; font-weight: 600;"));
    panelLayout->addWidget(sourceLabel);

    auto *sourceLayout = new QVBoxLayout;
    sourceLayout->setSpacing(7);
    m_videoDeviceEdit = new QComboBox(m_controlPanel);
    connect(m_videoDeviceEdit, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &ClientWindow::onSessionSourceChanged);
    auto *refreshButton = new QPushButton(tr("刷新"), m_controlPanel);
    connect(refreshButton, &QPushButton::clicked, this, &ClientWindow::onRefreshVideoDevices);
    auto *builtInButton = new QPushButton(tr("内置摄像头"), m_controlPanel);
    connect(builtInButton, &QPushButton::clicked, this, &ClientWindow::onUseBuiltInCamera);
    sourceLayout->addWidget(m_videoDeviceEdit);
    auto *sourceActions = new QHBoxLayout;
    sourceActions->setSpacing(7);
    sourceActions->addWidget(refreshButton, 1);
    sourceActions->addWidget(builtInButton, 1);
    sourceLayout->addLayout(sourceActions);
    panelLayout->addLayout(sourceLayout);

    m_videoPreview = new QLabel(m_controlPanel);
    m_videoPreview->setObjectName(QStringLiteral("clientPreview"));
    m_videoPreview->setAlignment(Qt::AlignCenter);
    m_videoPreview->setMinimumSize(280, 158);
    m_videoPreview->setMaximumSize(480, 270);
    m_videoPreview->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_videoPreview->setText(tr("尚未开始预览"));
    panelLayout->addWidget(m_videoPreview, 0, Qt::AlignHCenter);

    auto *actionRow = new QHBoxLayout;
    auto *previewButton = new QPushButton(tr("预览当前源"), m_controlPanel);
    connect(previewButton, &QPushButton::clicked, this, &ClientWindow::onPreviewVideo);
    auto *selectButton = new QPushButton(tr("更换并登记"), m_controlPanel);
    connect(selectButton, &QPushButton::clicked, this, &ClientWindow::onSelectVideoDevice);
    actionRow->addWidget(previewButton, 1);
    actionRow->addWidget(selectButton, 1);
    panelLayout->addLayout(actionRow);

    m_videoStatus = new QLabel(m_controlPanel);
    m_videoStatus->setWordWrap(true);
    m_videoStatus->setStyleSheet(QStringLiteral("color: #c9d4da;"));
    panelLayout->addWidget(m_videoStatus);

    m_videoSourceIdLabel = new QLabel(m_controlPanel);
    m_videoSourceIdLabel->setWordWrap(true);
    m_videoSourceIdLabel->setStyleSheet(QStringLiteral("color: #8d9aa4;"));
    panelLayout->addWidget(m_videoSourceIdLabel);

    m_log = new QPlainTextEdit(m_controlPanel);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(500);
    m_log->setMinimumHeight(54);
    m_log->setMaximumHeight(120);
    panelLayout->addWidget(m_log);

    auto *logoutButton = new QPushButton(tr("取消登记并返回"), m_controlPanel);
    logoutButton->setObjectName(QStringLiteral("logoutButton"));
    connect(logoutButton, &QPushButton::clicked, this, &ClientWindow::onLogoutClicked);
    panelLayout->addWidget(logoutButton);

    root->addWidget(m_controlPanel, 0, Qt::AlignTop | Qt::AlignRight);
    root->addStretch(1);
}

void ClientWindow::showLoginPage()
{
    m_loginInProgress = false;
    if (m_loginTimeoutTimer)
        m_loginTimeoutTimer->stop();
    clearSettlement();

    const QString previousSourceId = m_activeSourceId;
    m_registered = false;
    m_selectedTeam = 0;
    m_selectedRobotId = kDefaultRobotId;
    m_displayName.clear();
    m_activeSourceId.clear();
    m_activeSourceName.clear();
    m_videoPort = 0;
    if (!previousSourceId.isEmpty())
        setSourceFrame(previousSourceId, QImage());
    stopVideoPreview();

    if (m_teamEdit)
        m_teamEdit->setCurrentIndex(0);
    if (m_loginButton)
        m_loginButton->setEnabled(true);
    if (m_loginStatus)
        m_loginStatus->setText(tr("请选择红方或蓝方并登记。"));
    if (m_videoStatus)
        m_videoStatus->setText(tr("尚未登记视频源"));
    if (m_videoPreview) {
        m_videoPreview->setPixmap(QPixmap());
        m_videoPreview->setText(tr("尚未开始预览"));
    }
    if (m_registrationPreview) {
        m_registrationPreview->setPixmap(QPixmap());
        m_registrationPreview->setText(tr("选择视频源后点击“预览”"));
    }

    updateAutoRobotIdentity();
    setConnectionStatus(tr("未登记"));
    setActiveSource(QStringLiteral("field"), tr("等待选手端登记"));

    if (centralWidget() && m_registrationOverlay)
        m_registrationOverlay->setGeometry(centralWidget()->rect());
    if (centralWidget() && m_controlLayer)
        m_controlLayer->setGeometry(centralWidget()->rect());

    if (m_controlPanel)
        m_controlPanel->hide();
    if (m_controlsButton) {
        m_controlsButton->show();
        m_controlsButton->setText(tr("视频设置"));
    }
    if (m_controlLayer)
        m_controlLayer->show();
    if (m_registrationOverlay) {
        m_registrationOverlay->show();
        m_registrationOverlay->raise();
    }
}

void ClientWindow::showSessionPage(const QString &displayName, int team, int robotId)
{
    m_registered = true;
    m_displayName = displayName;
    m_selectedTeam = team;
    m_selectedRobotId = robotId >= 1 && robotId <= 255 ? robotId : kDefaultRobotId;
    updateAutoRobotIdentity();

    if (m_identityLabel)
        m_identityLabel->setText(tr("%1 · %2 · %3 号步兵")
                                     .arg(teamText(m_selectedTeam),
                                          m_displayName)
                                     .arg(m_selectedRobotId));
    setConnectionStatus(tr("TCP 已连接 · 比赛状态同步中"), QColor(QStringLiteral("#72d39a")));

    if (m_registrationOverlay)
        m_registrationOverlay->hide();
    if (m_controlLayer) {
        if (centralWidget())
            m_controlLayer->setGeometry(centralWidget()->rect());
        m_controlLayer->show();
        m_controlLayer->raise();
    }
    if (m_controlsButton)
        m_controlsButton->show();
    if (m_controlPanel)
        m_controlPanel->hide();

    updateActiveRobotSource();
    appendLog(tr("登记成功，已进入选手端全屏比赛画面"));
}

void ClientWindow::onLoginClicked()
{
    if (!m_socket || !m_teamEdit || !m_serverEdit || !m_portEdit)
        return;

    const QString server = m_serverEdit->text().trimmed();
    bool portOk = false;
    const quint16 port = static_cast<quint16>(m_portEdit->text().toUShort(&portOk));
    const int team = m_teamEdit->currentData().toInt();
    if (server.isEmpty() || !portOk || port == 0 || (team != kRedTeam && team != kBlueTeam)) {
        if (m_loginStatus)
            m_loginStatus->setText(tr("请填写有效的服务器地址、端口，并选择红方或蓝方。"));
        return;
    }

    m_selectedTeam = team;
    m_selectedRobotId = kDefaultRobotId;
    m_displayName = m_displayNameEdit ? m_displayNameEdit->text().trimmed() : QString();
    if (m_displayName.isEmpty())
        m_displayName = teamText(team) + tr("选手端");

    m_readBuffer.clear();
    m_loginInProgress = true;
    if (m_loginTimeoutTimer)
        m_loginTimeoutTimer->stop();
    if (m_loginButton)
        m_loginButton->setEnabled(false);
    if (m_loginStatus)
        m_loginStatus->setText(tr("正在连接 %1:%2 ...").arg(server).arg(port));
    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        const QSignalBlocker blocker(m_socket);
        m_socket->abort();
    }
    m_socket->connectToHost(server, port);
    if (m_loginTimeoutTimer)
        m_loginTimeoutTimer->start();
}

void ClientWindow::onSocketConnected()
{
    if (!m_loginInProgress || m_registered)
        return;

    if (m_loginTimeoutTimer)
        m_loginTimeoutTimer->start();
    if (m_loginStatus)
        m_loginStatus->setText(tr("TCP 已连接，正在登记 %1 ...").arg(teamText(m_selectedTeam)));

    const QString sourceId = selectedSourceId(m_registrationSourceEdit);
    const QString sourceName = selectedSourceName(m_registrationSourceEdit);
    sendMessage(matchproto::registrationRequest(m_selectedTeam,
                                                 m_selectedRobotId,
                                                 m_displayName,
                                                 sourceId,
                                                 sourceId.isEmpty() ? QString() : sourceName));
}

void ClientWindow::onSocketReadyRead()
{
    if (!m_socket)
        return;

    m_readBuffer.append(m_socket->readAll());
    while (true) {
        const int newline = m_readBuffer.indexOf('\n');
        if (newline < 0)
            break;

        const QByteArray line = m_readBuffer.left(newline);
        m_readBuffer.remove(0, newline + 1);
        QJsonObject message;
        QString error;
        if (matchproto::decode(line, message, &error))
            handleMessage(message);
        else
            appendLog(tr("收到无效服务器消息: %1").arg(error));
    }
}

void ClientWindow::onSocketDisconnected()
{
    if (m_loginTimeoutTimer)
        m_loginTimeoutTimer->stop();

    if (m_loginInProgress) {
        m_loginInProgress = false;
        if (m_loginButton)
            m_loginButton->setEnabled(true);
        if (m_loginStatus)
            m_loginStatus->setText(tr("连接已断开，请确认赛事服务器已启动。"));
        return;
    }

    const bool wasRegistered = m_registered;
    m_registered = false;
    if (wasRegistered)
        appendLog(tr("已与赛事服务器断开连接"));

    if (m_registrationOverlay && m_registrationOverlay->isVisible()) {
        if (m_loginButton)
            m_loginButton->setEnabled(true);
    } else {
        showLoginPage();
    }
    setConnectionStatus(tr("TCP 未连接"), QColor(QStringLiteral("#ff6872")));
}

void ClientWindow::onSocketError()
{
    if (!m_socket)
        return;

    const QString error = m_socket->errorString();
    if (m_loginInProgress) {
        m_loginInProgress = false;
        if (m_loginTimeoutTimer)
            m_loginTimeoutTimer->stop();
        if (m_socket->state() != QAbstractSocket::UnconnectedState) {
            const QSignalBlocker blocker(m_socket);
            m_socket->abort();
        }
        if (m_loginStatus)
            m_loginStatus->setText(tr("连接失败：%1").arg(error));
        if (m_loginButton)
            m_loginButton->setEnabled(true);
        return;
    }

    if (m_registrationOverlay && m_registrationOverlay->isVisible()) {
        if (m_loginStatus)
            m_loginStatus->setText(tr("连接失败：%1").arg(error));
        if (m_loginButton)
            m_loginButton->setEnabled(true);
    } else {
        appendLog(tr("网络错误：%1").arg(error));
        setConnectionStatus(tr("TCP 错误"), QColor(QStringLiteral("#ff6872")));
    }
}

void ClientWindow::onLoginTimeout()
{
    if (!m_loginInProgress)
        return;

    m_loginInProgress = false;
    if (m_socket && m_socket->state() != QAbstractSocket::UnconnectedState) {
        const QSignalBlocker blocker(m_socket);
        m_socket->abort();
    }
    if (m_loginButton)
        m_loginButton->setEnabled(true);
    if (m_loginStatus)
        m_loginStatus->setText(tr("连接超时，请确认赛事服务器地址和端口。"));
}

void ClientWindow::onLogoutClicked()
{
    m_loginInProgress = false;
    if (m_loginTimeoutTimer)
        m_loginTimeoutTimer->stop();
    if (m_registered)
        sendMessage(matchproto::simpleRequest(QStringLiteral("logout")));
    m_registered = false;
    if (m_socket)
        m_socket->disconnectFromHost();
    showLoginPage();
}

void ClientWindow::sendMessage(const QJsonObject &message)
{
    if (m_socket && m_socket->state() == QAbstractSocket::ConnectedState)
        m_socket->write(matchproto::encode(message));
}

void ClientWindow::handleMessage(const QJsonObject &message)
{
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("registration_result")) {
        if (!m_loginInProgress)
            return;
        if (!message.value(QStringLiteral("ok")).toBool()) {
            m_loginInProgress = false;
            if (m_loginTimeoutTimer)
                m_loginTimeoutTimer->stop();
            if (m_loginStatus)
                m_loginStatus->setText(message.value(QStringLiteral("message"))
                                            .toString(tr("登记失败")));
            if (m_loginButton)
                m_loginButton->setEnabled(true);
            if (m_socket)
                m_socket->disconnectFromHost();
            return;
        }

        const QString displayName = message.value(QStringLiteral("displayName"))
                                         .toString(m_displayName);
        const int team = message.value(QStringLiteral("team")).toInt(m_selectedTeam);
        const int robotId = message.value(QStringLiteral("robotId"))
                                .toInt(kDefaultRobotId);
        m_loginInProgress = false;
        if (m_loginTimeoutTimer)
            m_loginTimeoutTimer->stop();
        m_activeSourceId = message.value(QStringLiteral("sourceId")).toString().trimmed();
        m_activeSourceName = message.value(QStringLiteral("sourceName")).toString().trimmed();
        m_videoPort = static_cast<quint16>(
            qBound(0, message.value(QStringLiteral("videoPort")).toInt(), 65535));

        if (m_videoDeviceEdit) {
            const QSignalBlocker blocker(m_videoDeviceEdit);
            const int sourceIndex = m_videoDeviceEdit->findData(m_activeSourceId);
            if (sourceIndex >= 0) {
                m_videoDeviceEdit->setCurrentIndex(sourceIndex);
            } else if (!m_activeSourceId.isEmpty()) {
                m_videoDeviceEdit->addItem(m_activeSourceName.isEmpty()
                                               ? m_activeSourceId
                                               : m_activeSourceName,
                                           m_activeSourceId);
                m_videoDeviceEdit->setCurrentIndex(m_videoDeviceEdit->count() - 1);
            } else {
                m_videoDeviceEdit->setCurrentIndex(0);
            }
        }
        updateSourceLabels(m_videoDeviceEdit, m_videoSourceIdLabel);
        showSessionPage(displayName, team, robotId);

        if (!m_activeSourceId.isEmpty()) {
            startVideoPreview();
            setVideoStatus(tr("已登记视频源：%1").arg(m_activeSourceName));
        } else {
            setVideoStatus(tr("尚未登记视频源，可在右上角视频设置中更换。"));
        }
        return;
    }

    if (!m_registered)
        return;

    if (type == QStringLiteral("robot_snapshot")) {
        handleRobotSnapshot(message);
    } else if (type == QStringLiteral("match_state")) {
        handleMatchState(message);
    } else if (type == QStringLiteral("camera_ack")) {
        if (!message.value(QStringLiteral("ok")).toBool(true)) {
            setVideoStatus(message.value(QStringLiteral("message"))
                               .toString(tr("视频源登记失败")),
                           QColor(QStringLiteral("#ff6872")));
            return;
        }
        m_activeSourceId = message.value(QStringLiteral("sourceId")).toString().trimmed();
        m_activeSourceName = message.value(QStringLiteral("sourceName")).toString().trimmed();
        m_videoPort = static_cast<quint16>(
            qBound(0, message.value(QStringLiteral("videoPort")).toInt(m_videoPort), 65535));
        updateActiveRobotSource();
        setVideoStatus(message.value(QStringLiteral("message"))
                           .toString(tr("视频源登记成功")),
                       QColor(QStringLiteral("#72d39a")));
        updateSourceLabels(m_videoDeviceEdit, m_videoSourceIdLabel);
        appendLog(tr("视频源已登记：%1").arg(m_activeSourceName));
    } else if (type == QStringLiteral("error")) {
        appendLog(tr("服务端错误：%1").arg(message.value(QStringLiteral("message")).toString()));
    } else if (type == QStringLiteral("pong")) {
        appendLog(tr("服务器心跳正常"));
    }
}

void ClientWindow::handleRobotSnapshot(const QJsonObject &message)
{
    updateRobots(robotInfosFromSnapshot(message));
}

void ClientWindow::handleMatchState(const QJsonObject &message)
{
    const QString redName = message.value(QStringLiteral("redName")).toString();
    const QString blueName = message.value(QStringLiteral("blueName")).toString();
    if (!redName.isEmpty() || !blueName.isEmpty())
        setTeamNames(redName.isEmpty() ? tr("红方") : redName,
                     blueName.isEmpty() ? tr("蓝方") : blueName);

    const bool ended = message.value(QStringLiteral("ended")).toBool(false);
    const QString settlement = message.value(QStringLiteral("settlement")).toString().trimmed();
    QString localSettlement = settlement;
    if ((settlement == QStringLiteral("redwin") && m_selectedTeam == 2)
        || (settlement == QStringLiteral("bluewin") && m_selectedTeam == 1)) {
        localSettlement = QStringLiteral("defeated");
    }
    const bool sameSettlement = ended && roundEnded()
                                && localSettlement == settlementType();

    setScores(message.value(QStringLiteral("redScore")).toInt(0),
              message.value(QStringLiteral("blueScore")).toInt(0));

    if (!ended || settlement.isEmpty()) {
        setMatchState(message.value(QStringLiteral("remainingSeconds")).toInt(60),
                      message.value(QStringLiteral("running")).toBool(false));
        stopSettlement();
        return;
    }

    if (!sameSettlement) {
        setMatchState(message.value(QStringLiteral("remainingSeconds")).toInt(60),
                      message.value(QStringLiteral("running")).toBool(false));
        playSettlement(localSettlement);
    }
}

void ClientWindow::onRefreshVideoDevices()
{
    const QString registrationId = selectedSourceId(m_registrationSourceEdit);
    const QString sessionId = selectedSourceId(m_videoDeviceEdit);
    refreshVideoDevices();

    if (m_registrationSourceEdit) {
        const int index = m_registrationSourceEdit->findData(registrationId);
        m_registrationSourceEdit->setCurrentIndex(index >= 0 ? index : 0);
    }
    if (m_videoDeviceEdit) {
        const int index = m_videoDeviceEdit->findData(sessionId);
        m_videoDeviceEdit->setCurrentIndex(index >= 0 ? index : 0);
    }

    if (m_registered)
        setVideoStatus(tr("设备列表已刷新，请选择视频源并点击“预览当前源”。"));
    else if (m_loginStatus)
        m_loginStatus->setText(tr("设备列表已刷新。"));
}

void ClientWindow::onPreviewVideo()
{
    startVideoPreview();
}

void ClientWindow::onUseBuiltInCamera()
{
    QComboBox *combo = m_registered ? m_videoDeviceEdit : m_registrationSourceEdit;
    const int index = builtInCameraIndex(combo);
    if (index < 0) {
        const QString message = tr("未找到内置摄像头，请先点击刷新，并确认 Windows 已允许应用访问摄像头。");
        setVideoStatus(message, QColor(QStringLiteral("#f0aa36")));
        if (!m_registered && m_loginStatus)
            m_loginStatus->setText(message);
        return;
    }

    combo->setCurrentIndex(index);
    if (m_registered)
        onSelectVideoDevice();
    else
        startVideoPreview();
}

void ClientWindow::onSelectVideoDevice()
{
    if (!m_registered || !m_videoDeviceEdit)
        return;

    m_activeSourceId = selectedSourceId(m_videoDeviceEdit);
    m_activeSourceName = selectedSourceName(m_videoDeviceEdit);
    updateActiveRobotSource();
    startVideoPreview();
    sendMessage(matchproto::cameraSelectRequest(m_activeSourceId,
                                                m_activeSourceId.isEmpty()
                                                    ? QString()
                                                    : m_activeSourceName));
    setVideoStatus(tr("正在登记视频源…"));
}

void ClientWindow::onTeamChanged(int)
{
    updateAutoRobotIdentity();
}

void ClientWindow::onRegistrationSourceChanged(int)
{
    updateSourceLabels(m_registrationSourceEdit, nullptr);
}

void ClientWindow::onSessionSourceChanged(int)
{
    updateSourceLabels(m_videoDeviceEdit, m_videoSourceIdLabel);
    if (!m_registered)
        return;

    const QString nextId = selectedSourceId(m_videoDeviceEdit);
    const QString nextName = selectedSourceName(m_videoDeviceEdit);
    if (nextId == m_activeSourceId && nextName == m_activeSourceName)
        return;

    m_activeSourceId = nextId;
    m_activeSourceName = nextName;
    updateActiveRobotSource();
    // Switching the registered source takes effect immediately: the camera is
    // reopened and upstream streaming is restarted against the new device.
    startVideoPreview();
}

void ClientWindow::onToggleControls()
{
    if (!m_controlPanel || !m_controlsButton)
        return;

    const bool visible = !m_controlPanel->isVisible();
    m_controlPanel->setVisible(visible);
    m_controlsButton->setText(visible ? tr("隐藏设置") : tr("视频设置"));
    if (visible && !m_lastVideoFrame.isNull())
        showFramePreview(m_lastVideoFrame);
}

void ClientWindow::appendLog(const QString &message)
{
    if (m_log)
        m_log->appendPlainText(message);
}

void ClientWindow::refreshVideoDevices()
{
    m_videoDevices.clear();

    // QMediaDevices is the authoritative source on Windows: it maps to
    // MediaFoundation's device list and exposes a persistent id per camera.
    // FFmpeg's dshow enumeration produces different names for the same
    // hardware, so we no longer shell out to ffmpeg just to list devices.
    const auto appendDevice = [this](const QString &name, const QByteArray &deviceId) {
        const QString trimmedName = name.trimmed();
        const QString sourceId = sourceIdForDevice(deviceId);
        if (trimmedName.isEmpty() || sourceId.isEmpty())
            return;
        for (const auto &device : m_videoDevices) {
            if (device.first.compare(trimmedName, Qt::CaseInsensitive) == 0
                || device.second == sourceId) {
                return;
            }
        }
        m_videoDevices.append({trimmedName, sourceId});
    };

    for (const QCameraDevice &device : QMediaDevices::videoInputs())
        appendDevice(device.description(), device.id());

    populateVideoCombo(m_registrationSourceEdit);
    populateVideoCombo(m_videoDeviceEdit);
    updateSourceLabels(m_registrationSourceEdit, nullptr);
    updateSourceLabels(m_videoDeviceEdit, m_videoSourceIdLabel);
}

void ClientWindow::populateVideoCombo(QComboBox *combo, const QString &selectedId)
{
    if (!combo)
        return;

    const QSignalBlocker blocker(combo);
    combo->clear();
    combo->addItem(tr("不使用视频源"), QString());
    for (const auto &device : m_videoDevices)
        combo->addItem(device.first, device.second);
    if (m_videoDevices.isEmpty())
        combo->setToolTip(tr("未检测到摄像头；可点击“内置摄像头”重试，或检查 Windows 摄像头权限。"));

    const int selected = combo->findData(selectedId);
    combo->setCurrentIndex(selected >= 0 ? selected : 0);
}

int ClientWindow::builtInCameraIndex(const QComboBox *combo) const
{
    if (!combo)
        return -1;

    int bestIndex = -1;
    int bestScore = 0;
    for (int index = 1; index < combo->count(); ++index) {
        const int score = builtInCameraScore(combo->itemText(index));
        if (score >= 10 && score > bestScore) {
            bestScore = score;
            bestIndex = index;
        }
    }

    // A single camera with a vendor-specific name is normally the laptop's
    // internal camera, even when its name contains no standard hint.
    if (bestIndex < 0 && combo->count() == 2)
        bestIndex = 1;
    return bestIndex;
}

void ClientWindow::updateAutoRobotIdentity()
{
    const int team = m_teamEdit ? m_teamEdit->currentData().toInt() : m_selectedTeam;
    const QString text = tr("%1 · %2 号步兵（自动）")
                             .arg(teamText(team))
                             .arg(m_selectedRobotId > 0 ? m_selectedRobotId : kDefaultRobotId);
    if (m_registrationRobotLabel) {
        m_registrationRobotLabel->setText(text);
        styleLabel(m_registrationRobotLabel, teamColor(team), true);
    }
}

void ClientWindow::updateSourceLabels(QComboBox *combo, QLabel *idLabel)
{
    if (!combo || !idLabel)
        return;
    const QString sourceId = selectedSourceId(combo);
    idLabel->setText(tr("源标识：%1")
                         .arg(sourceId.isEmpty() ? tr("未选择") : sourceId));
}

void ClientWindow::updateActiveRobotSource()
{
    if (!m_registered) {
        setActiveSource(QStringLiteral("field"), tr("等待选手端登记"));
        return;
    }

    const QString sourceId = m_activeSourceId.isEmpty()
                                 ? QStringLiteral("client://team%1/robot%2")
                                       .arg(m_selectedTeam)
                                       .arg(selectedRobotId())
                                 : m_activeSourceId;
    setActiveSource(sourceId, ownViewTitle());
}

void ClientWindow::setConnectionStatus(const QString &message, const QColor &color)
{
    if (!m_connectionLabel)
        return;
    m_connectionLabel->setText(message);
    if (color.isValid())
        m_connectionLabel->setStyleSheet(QStringLiteral("color: %1; font-weight: 600;")
                                             .arg(color.name()));
    else
        m_connectionLabel->setStyleSheet(QString());
}

void ClientWindow::setVideoStatus(const QString &message, const QColor &color)
{
    if (!m_videoStatus)
        return;
    m_videoStatus->setText(message);
    if (color.isValid())
        m_videoStatus->setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));
    else
        m_videoStatus->setStyleSheet(QStringLiteral("color: #c9d4da;"));
}

void ClientWindow::startVideoPreview()
{
    if (m_videoRestartTimer)
        m_videoRestartTimer->stop();

    QComboBox *combo = m_registered ? m_videoDeviceEdit : m_registrationSourceEdit;
    const QString sourceId = selectedSourceId(combo);
    const QString sourceName = selectedSourceName(combo);

    m_activeSourceId = sourceId;
    m_activeSourceName = sourceName;

    stopVideoPreview();
    updateActiveRobotSource();

    if (sourceId.isEmpty() || sourceName.isEmpty()) {
        showFramePreview(QImage());
        setVideoStatus(tr("未选择视频源。"));
        return;
    }

    // === Local preview via Qt Multimedia (native MediaFoundation backend) ===
    // A dedicated QVideoSink receives camera frames and forwards each one to
    // showFramePreview + setSourceFrame. No ffmpeg fork, no JPEG round-trip,
    // no pipe buffer to manage.
    QCameraDevice selected;
    bool found = false;
    for (const QCameraDevice &dev : QMediaDevices::videoInputs()) {
        if (sourceIdForDevice(dev.id()) == sourceId) {
            selected = dev;
            found = true;
            break;
        }
    }
    if (!found) {
        setVideoStatus(tr("找不到视频源对应的本机摄像头，请刷新设备列表。"),
                       QColor(QStringLiteral("#ff6872")));
        return;
    }

    m_captureSession = new QMediaCaptureSession(this);
    m_camera = new QCamera(selected, this);
    m_videoSink = new QVideoSink(this);
    m_captureSession->setCamera(m_camera);
    m_captureSession->setVideoSink(m_videoSink);

    // Ask the camera for a 1280x720 feed so onCameraFrame is a cheap
    // pass-through. Qt6 calls this a QCameraFormat (not QVideoFrameFormat).
    // If no 720p mode is advertised we keep the device's default format and
    // let onCameraFrame do a cheap per-frame rescale instead.
    {
        const auto formats = selected.videoFormats();
        QCameraFormat preferred;
        for (const auto &fmt : formats) {
            if (fmt.resolution() == QSize(1280, 720)
                && fmt.pixelFormat() == QVideoFrameFormat::Format_ARGB8888) {
                preferred = fmt;
                break;
            }
        }
        if (preferred.isNull()) {
            for (const auto &fmt : formats) {
                if (fmt.resolution() == QSize(1280, 720)) {
                    preferred = fmt;
                    break;
                }
            }
        }
        if (!preferred.isNull())
            m_camera->setCameraFormat(preferred);
    }

    connect(m_videoSink, &QVideoSink::videoFrameChanged,
            this, &ClientWindow::onCameraFrame);
    connect(m_camera, &QCamera::errorOccurred, this,
            [this](QCamera::Error, const QString &errorString) {
        setVideoStatus(tr("摄像头错误：%1").arg(errorString),
                       QColor(QStringLiteral("#ff6872")));
        scheduleVideoRestart();
    });

    m_camera->start();

    // === Optional upstream: push H.264 to the server over UDP ===
    // Kept in a separate ffmpeg child so the local preview stays responsive
    // even when the network is congested.
    if (m_registered && m_videoPort > 0 && m_serverEdit)
        startStreaming();

    const bool streaming = m_streamProcess && m_streamProcess->state() != QProcess::NotRunning;
    setVideoStatus(streaming
                       ? tr("H.264 推流中：%1 · UDP %2").arg(sourceName).arg(m_videoPort)
                       : tr("正在预览：%1").arg(sourceName),
                   QColor(QStringLiteral("#72d39a")));
}

void ClientWindow::stopVideoPreview()
{
    if (m_videoRestartTimer)
        m_videoRestartTimer->stop();
    stopStreaming();

    if (m_camera) {
        m_camera->stop();
        m_camera->deleteLater();
        m_camera = nullptr;
    }
    if (m_captureSession) {
        m_captureSession->deleteLater();
        m_captureSession = nullptr;
    }
    if (m_videoSink) {
        m_videoSink->deleteLater();
        m_videoSink = nullptr;
    }
}

void ClientWindow::startStreaming()
{
    if (m_streamProcess)
        return; // already running

    const QString executable = ffmpegExecutable();
    if (executable.isEmpty()) {
        appendLog(tr("未找到 ffmpeg，无法推流；本地预览继续运行。"));
        return;
    }
    if (!m_camera || !m_camera->isActive()) {
        appendLog(tr("摄像头未激活，推流取消。"));
        return;
    }

    auto *process = new QProcess(this);
    m_streamProcess = process;
    process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(process, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError) {
        if (m_streamProcess != process)
            return;
        appendLog(tr("推流进程错误：%1").arg(process->errorString()));
        process->deleteLater();
        m_streamProcess = nullptr;
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process](int, QProcess::ExitStatus) {
        if (m_streamProcess != process)
            return;
        m_streamProcess = nullptr;
        process->deleteLater();
    });

    QString server = m_serverEdit->text().trimmed();
    if (server.contains(QLatin1Char(':')) && !server.startsWith(QLatin1Char('[')))
        server = QStringLiteral("[%1]").arg(server);
    const QString udpUrl = QStringLiteral(
        "udp://%1:%2?pkt_size=1316&buffer_size=65536&connect=1")
                               .arg(server)
                               .arg(m_videoPort);

    // Feed raw BGRA frames into ffmpeg's stdin. The camera is opened by Qt
    // (MediaFoundation), so we cannot also have ffmpeg open it via dshow —
    // most webcams only allow one exclusive handle. Encoding from a pipe
    // keeps both consumers on the same physical device.
    process->start(executable, {
        QStringLiteral("-hide_banner"),
        QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-f"), QStringLiteral("rawvideo"),
        QStringLiteral("-pixel_format"), QStringLiteral("bgra"),
        QStringLiteral("-video_size"), QStringLiteral("1280x720"),
        QStringLiteral("-framerate"), QStringLiteral("30"),
        QStringLiteral("-i"), QStringLiteral("pipe:0"),
        QStringLiteral("-an"),
        QStringLiteral("-c:v"), QStringLiteral("libx264"),
        QStringLiteral("-preset"), QStringLiteral("ultrafast"),
        QStringLiteral("-tune"), QStringLiteral("zerolatency"),
        QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
        QStringLiteral("-b:v"), QStringLiteral("3500k"),
        QStringLiteral("-maxrate"), QStringLiteral("3500k"),
        QStringLiteral("-bufsize"), QStringLiteral("7000k"),
        QStringLiteral("-g"), QStringLiteral("30"),
        QStringLiteral("-keyint_min"), QStringLiteral("30"),
        QStringLiteral("-sc_threshold"), QStringLiteral("0"),
        QStringLiteral("-bf"), QStringLiteral("0"),
        QStringLiteral("-flush_packets"), QStringLiteral("1"),
        QStringLiteral("-f"), QStringLiteral("mpegts"),
        udpUrl
    });

    if (!process->waitForStarted(800)) {
        appendLog(tr("无法启动 ffmpeg 推流：%1").arg(process->errorString()));
        process->deleteLater();
        m_streamProcess = nullptr;
    }
}

void ClientWindow::stopStreaming()
{
    if (!m_streamProcess)
        return;
    QProcess *process = m_streamProcess;
    m_streamProcess = nullptr;
    process->disconnect(this);
    if (process->state() != QProcess::NotRunning) {
        process->kill();
        process->waitForFinished(300);
    }
    process->deleteLater();
}

void ClientWindow::scheduleVideoRestart()
{
    if (m_registered && !m_activeSourceId.isEmpty() && m_videoRestartTimer
        && !m_videoRestartTimer->isActive()) {
        m_videoRestartTimer->start();
    }
}

void ClientWindow::onCameraFrame(const QVideoFrame &frame)
{
    if (!frame.isValid())
        return;
    const QImage image = frame.toImage();
    if (image.isNull())
        return;
    m_lastVideoFrame = image;
    showFramePreview(image);
    if (m_registered && !m_activeSourceId.isEmpty())
        setSourceFrame(m_activeSourceId, image);

    // Feed the same frame to the upstream encoder when streaming is active.
    // Frames arrive in whatever pixel format/resolution the camera negotiated;
    // we only convert when the format differs from ARGB32 or the size is not
    // 1280x720 — in the common case this is a single memcpy into the pipe.
    if (m_streamProcess && m_streamProcess->state() == QProcess::Running) {
        const bool needsConvert = image.format() != QImage::Format_ARGB32;
        const bool needsScale = image.width() != 1280 || image.height() != 720;
        if (!needsConvert && !needsScale) {
            m_streamProcess->write(reinterpret_cast<const char *>(image.constBits()),
                                   image.sizeInBytes());
        } else {
            QImage rgba = needsConvert
                              ? image.convertToFormat(QImage::Format_ARGB32)
                              : image;
            if (needsScale)
                rgba = rgba.scaled(1280, 720, Qt::IgnoreAspectRatio,
                                   Qt::FastTransformation);
            m_streamProcess->write(reinterpret_cast<const char *>(rgba.constBits()),
                                   rgba.sizeInBytes());
        }
    }
}

void ClientWindow::showFramePreview(const QImage &frame)
{
    if (frame.isNull()) {
        m_lastVideoFrame = QImage();
        if (m_registrationPreview) {
            m_registrationPreview->setPixmap(QPixmap());
            m_registrationPreview->setText(tr("暂无视频帧"));
        }
        if (m_videoPreview) {
            m_videoPreview->setPixmap(QPixmap());
            m_videoPreview->setText(tr("暂无视频帧"));
        }
        return;
    }

    m_lastVideoFrame = frame;

    const auto setPreview = [&frame](QLabel *label) {
        if (!label)
            return;
        if (!label->isVisible())
            return;
        const QPixmap pixmap = QPixmap::fromImage(frame).scaled(
            label->size(), Qt::KeepAspectRatio, Qt::FastTransformation);
        label->setText(QString());
        label->setPixmap(pixmap);
    };
    setPreview(m_registrationPreview);
    setPreview(m_videoPreview);
}

QString ClientWindow::ffmpegExecutable() const
{
    const QString bundled = QDir(QCoreApplication::applicationDirPath())
                                .filePath(QStringLiteral("ffmpeg.exe"));
    if (QFileInfo::exists(bundled))
        return bundled;

    const QString adjacent = QDir(QCoreApplication::applicationDirPath())
                                  .filePath(QStringLiteral("tools/ffmpeg.exe"));
    if (QFileInfo::exists(adjacent))
        return adjacent;

    const QString packaged = QDir(QCoreApplication::applicationDirPath())
                                  .filePath(QStringLiteral("tools/ffmpeg/ffmpeg.exe"));
    if (QFileInfo::exists(packaged))
        return packaged;

    return QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
}

QString ClientWindow::selectedSourceId(const QComboBox *combo) const
{
    return combo && combo->currentIndex() >= 0
               ? combo->currentData().toString().trimmed()
               : QString();
}

QString ClientWindow::selectedSourceName(const QComboBox *combo) const
{
    return combo && combo->currentIndex() >= 0
               ? combo->currentText().trimmed()
               : QString();
}

QString ClientWindow::ownViewTitle() const
{
    const QString source = m_activeSourceId.isEmpty()
                               ? tr("未选择视频源")
                               : (m_activeSourceName.isEmpty()
                                      ? m_activeSourceId
                                      : m_activeSourceName);
    return tr("我的机器人视角 · %1 %2 号 · %3")
        .arg(teamText(m_selectedTeam))
        .arg(selectedRobotId())
        .arg(source);
}

quint8 ClientWindow::selectedRobotId() const
{
    return static_cast<quint8>(qBound(1, m_selectedRobotId, 255));
}

QVector<RobotManager::RobotInfo> ClientWindow::robotInfosFromSnapshot(
    const QJsonObject &message) const
{
    QVector<RobotManager::RobotInfo> robots;
    const QJsonArray values = message.value(QStringLiteral("robots")).toArray();
    robots.reserve(values.size());

    for (const QJsonValue &value : values) {
        const QJsonObject object = value.toObject();
        const int robotId = object.value(QStringLiteral("robotId")).toInt();
        const int team = object.value(QStringLiteral("team")).toInt();
        if (robotId < 0 || robotId > 255 || team < 0 || team > 255)
            continue;

        RobotManager::RobotInfo robot;
        robot.robotId = static_cast<quint8>(robotId);
        robot.team = static_cast<quint8>(team);
        robot.hp = object.value(QStringLiteral("hp")).toInt(-1);
        robot.heat = object.value(QStringLiteral("heat")).toInt(-1);
        robot.alive = object.value(QStringLiteral("alive")).toBool(true);
        robot.shootEnabled = object.value(QStringLiteral("shootEnabled")).toBool(true);
        robot.online = object.value(QStringLiteral("online")).toBool(false);
        robot.addr = QHostAddress(object.value(QStringLiteral("address")).toString());
        robots.append(robot);
    }
    return robots;
}

void ClientWindow::closeEvent(QCloseEvent *event)
{
    stopVideoPreview();
    stopSettlement();
    if (m_socket && m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->disconnectFromHost();
    event->accept();
}

void ClientWindow::resizeEvent(QResizeEvent *event)
{
    BroadcastWindow::resizeEvent(event);
    if (m_registrationOverlay && centralWidget()) {
        m_registrationOverlay->setGeometry(centralWidget()->rect());
        if (m_registrationOverlay->isVisible())
            m_registrationOverlay->raise();
    }

    const auto resizePreview = [](QLabel *preview) {
        if (!preview || preview->width() <= 0)
            return;
        const int height = qBound(158, qRound(preview->width() * 9.0 / 16.0),
                                  preview->maximumHeight() > 0
                                      ? preview->maximumHeight()
                                      : 405);
        preview->setFixedHeight(height);
    };
    resizePreview(m_registrationPreview);
    resizePreview(m_videoPreview);
    if (m_lastVideoFrame.isNull())
        return;
    showFramePreview(m_lastVideoFrame);
}
