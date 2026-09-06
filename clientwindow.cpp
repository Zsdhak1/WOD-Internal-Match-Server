#include "clientwindow.h"

#include "matchprotocol.h"

#include <QAbstractSocket>
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
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QStyle>
#include <QTcpSocket>
#include <QToolButton>
#include <QVBoxLayout>
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

QString sourceIdForDevice(const QString &instanceId)
{
    const QByteArray digest = QCryptographicHash::hash(instanceId.toUtf8(),
                                                        QCryptographicHash::Sha1)
                                  .toHex();
    return QStringLiteral("camera://windows/%1").arg(QString::fromLatin1(digest));
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

    auto *root = new QHBoxLayout(m_registrationOverlay);
    root->setContentsMargins(30, 30, 30, 30);
    root->addStretch(1);

    auto *card = new QFrame(m_registrationOverlay);
    card->setObjectName(QStringLiteral("registrationCard"));
    card->setMaximumWidth(700);
    card->setMinimumWidth(520);
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

    auto *sourceTitle = new QLabel(tr("机器人视频源"), card);
    QFont sourceTitleFont = sourceTitle->font();
    sourceTitleFont.setBold(true);
    sourceTitle->setFont(sourceTitleFont);
    cardLayout->addWidget(sourceTitle);

    auto *sourceRow = new QHBoxLayout;
    m_registrationSourceEdit = new QComboBox(card);
    m_registrationSourceEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(m_registrationSourceEdit, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &ClientWindow::onRegistrationSourceChanged);
    auto *refreshButton = new QPushButton(tr("刷新"), card);
    connect(refreshButton, &QPushButton::clicked, this, &ClientWindow::onRefreshVideoDevices);
    auto *previewButton = new QPushButton(tr("预览"), card);
    connect(previewButton, &QPushButton::clicked, this, &ClientWindow::onPreviewVideo);
    sourceRow->addWidget(m_registrationSourceEdit, 1);
    sourceRow->addWidget(refreshButton);
    sourceRow->addWidget(previewButton);
    cardLayout->addLayout(sourceRow);

    m_registrationPreview = new QLabel(card);
    m_registrationPreview->setObjectName(QStringLiteral("registrationPreview"));
    m_registrationPreview->setAlignment(Qt::AlignCenter);
    m_registrationPreview->setFixedSize(520, 250);
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

    root->addWidget(card, 0, Qt::AlignCenter);
    root->addStretch(1);
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
    m_controlPanel->setFixedWidth(430);
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

    auto *sourceLabel = new QLabel(tr("当前机器人视频源"), m_controlPanel);
    sourceLabel->setStyleSheet(QStringLiteral("color: #c9d4da; font-weight: 600;"));
    panelLayout->addWidget(sourceLabel);

    auto *sourceRow = new QHBoxLayout;
    m_videoDeviceEdit = new QComboBox(m_controlPanel);
    connect(m_videoDeviceEdit, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &ClientWindow::onSessionSourceChanged);
    auto *refreshButton = new QPushButton(tr("刷新"), m_controlPanel);
    connect(refreshButton, &QPushButton::clicked, this, &ClientWindow::onRefreshVideoDevices);
    sourceRow->addWidget(m_videoDeviceEdit, 1);
    sourceRow->addWidget(refreshButton);
    panelLayout->addLayout(sourceRow);

    m_videoPreview = new QLabel(m_controlPanel);
    m_videoPreview->setObjectName(QStringLiteral("clientPreview"));
    m_videoPreview->setAlignment(Qt::AlignCenter);
    m_videoPreview->setFixedSize(394, 220);
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
    m_log->setFixedHeight(86);
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
    const QString previousSourceId = m_activeSourceId;
    m_registered = false;
    m_selectedTeam = 0;
    m_selectedRobotId = kDefaultRobotId;
    m_displayName.clear();
    m_activeSourceId.clear();
    m_activeSourceName.clear();
    m_videoBuffer.clear();
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
    if (m_loginButton)
        m_loginButton->setEnabled(false);
    if (m_loginStatus)
        m_loginStatus->setText(tr("正在连接 %1:%2 ...").arg(server).arg(port));
    if (m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->abort();
    m_socket->connectToHost(server, port);
}

void ClientWindow::onSocketConnected()
{
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

void ClientWindow::onLogoutClicked()
{
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
        if (!message.value(QStringLiteral("ok")).toBool()) {
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
        m_activeSourceId = message.value(QStringLiteral("sourceId")).toString().trimmed();
        m_activeSourceName = message.value(QStringLiteral("sourceName")).toString().trimmed();

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

    setScores(message.value(QStringLiteral("redScore")).toInt(0),
              message.value(QStringLiteral("blueScore")).toInt(0));
    setMatchState(message.value(QStringLiteral("remainingSeconds")).toInt(60),
                  message.value(QStringLiteral("running")).toBool(false));
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

    m_activeSourceId = selectedSourceId(m_videoDeviceEdit);
    m_activeSourceName = selectedSourceName(m_videoDeviceEdit);
    updateActiveRobotSource();
}

void ClientWindow::onToggleControls()
{
    if (!m_controlPanel || !m_controlsButton)
        return;

    const bool visible = !m_controlPanel->isVisible();
    m_controlPanel->setVisible(visible);
    m_controlsButton->setText(visible ? tr("隐藏设置") : tr("视频设置"));
}

void ClientWindow::appendLog(const QString &message)
{
    if (m_log)
        m_log->appendPlainText(message);
}

void ClientWindow::refreshVideoDevices()
{
    m_videoDevices.clear();

#ifdef Q_OS_WIN
    QProcess process;
    process.start(QStringLiteral("powershell.exe"), {
        QStringLiteral("-NoProfile"), QStringLiteral("-Command"),
        QStringLiteral("Get-PnpDevice -PresentOnly | Where-Object { $_.Class -eq 'Camera' -or $_.Class -eq 'Image' } | ForEach-Object { \"$($_.InstanceId)`t$($_.FriendlyName)\" }")
    });
    if (process.waitForFinished(1500)) {
        const QStringList lines = QString::fromLocal8Bit(process.readAllStandardOutput())
                                      .split(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                                             Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            const QStringList parts = line.split(QChar('\t'));
            const QString instanceId = parts.value(0).trimmed();
            const QString name = parts.mid(1).join(QStringLiteral("\t")).trimmed();
            if (instanceId.isEmpty() || name.isEmpty())
                continue;

            const QString sourceId = sourceIdForDevice(instanceId);
            bool duplicate = false;
            for (const auto &device : m_videoDevices) {
                if (device.second == sourceId) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate)
                m_videoDevices.append({name, sourceId});
        }
    }
#endif

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
        combo->setToolTip(tr("未检测到摄像头；可安装 ffmpeg 后进行本地预览。"));

    const int selected = combo->findData(selectedId);
    combo->setCurrentIndex(selected >= 0 ? selected : 0);
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
    QComboBox *combo = m_registered ? m_videoDeviceEdit : m_registrationSourceEdit;
    const QString sourceId = selectedSourceId(combo);
    const QString sourceName = selectedSourceName(combo);

    m_activeSourceId = sourceId;
    m_activeSourceName = sourceName;
    m_videoBuffer.clear();

    stopVideoPreview();
    updateActiveRobotSource();

    if (sourceId.isEmpty() || sourceName.isEmpty()) {
        showFramePreview(QImage());
        setVideoStatus(tr("未选择视频源。"));
        return;
    }

    const QString executable = ffmpegExecutable();
    if (executable.isEmpty()) {
        showFramePreview(QImage());
        setVideoStatus(tr("未找到 ffmpeg，无法启动本地预览；视频源仍可登记。"),
                       QColor(QStringLiteral("#f0aa36")));
        return;
    }

    m_videoProcess = new QProcess(this);
    QProcess *process = m_videoProcess;
    process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(process, &QProcess::readyReadStandardOutput,
            this, &ClientWindow::consumeVideoOutput);
    connect(process, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError) {
        if (m_videoProcess != process)
            return;
        setVideoStatus(tr("视频预览启动失败：%1").arg(process->errorString()),
                       QColor(QStringLiteral("#ff6872")));
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process](int, QProcess::ExitStatus) {
        if (m_videoProcess != process)
            return;
        if (m_videoBuffer.isEmpty())
            setVideoStatus(tr("视频预览已停止，请检查摄像头是否被其他程序占用。"),
                           QColor(QStringLiteral("#f0aa36")));
    });

    process->start(executable, {
        QStringLiteral("-hide_banner"),
        QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-f"), QStringLiteral("dshow"),
        QStringLiteral("-framerate"), QStringLiteral("30"),
        QStringLiteral("-video_size"), QStringLiteral("1280x720"),
        QStringLiteral("-i"), QStringLiteral("video=%1").arg(sourceName),
        QStringLiteral("-an"),
        QStringLiteral("-f"), QStringLiteral("mjpeg"),
        QStringLiteral("-q:v"), QStringLiteral("5"),
        QStringLiteral("pipe:1")
    });

    if (!process->waitForStarted(800)) {
        setVideoStatus(tr("无法启动 ffmpeg：%1").arg(process->errorString()),
                       QColor(QStringLiteral("#ff6872")));
        stopVideoPreview();
        return;
    }

    setVideoStatus(tr("正在预览：%1").arg(sourceName),
                   QColor(QStringLiteral("#72d39a")));
}

void ClientWindow::stopVideoPreview()
{
    if (!m_videoProcess)
        return;

    QProcess *process = m_videoProcess;
    m_videoProcess = nullptr;
    process->disconnect(this);
    if (process->state() != QProcess::NotRunning) {
        process->kill();
        process->waitForFinished(300);
    }
    process->deleteLater();
}

void ClientWindow::consumeVideoOutput()
{
    if (!m_videoProcess)
        return;

    m_videoBuffer.append(m_videoProcess->readAllStandardOutput());
    const QByteArray jpegStart = QByteArray::fromHex("ffd8");
    const QByteArray jpegEnd = QByteArray::fromHex("ffd9");

    while (true) {
        int start = m_videoBuffer.indexOf(jpegStart);
        if (start < 0) {
            if (m_videoBuffer.size() > 2 * 1024 * 1024)
                m_videoBuffer.clear();
            return;
        }
        if (start > 0)
            m_videoBuffer.remove(0, start);

        const int end = m_videoBuffer.indexOf(jpegEnd, 2);
        if (end < 0) {
            if (m_videoBuffer.size() > 2 * 1024 * 1024)
                m_videoBuffer.remove(0, m_videoBuffer.size() - 2);
            return;
        }

        const QByteArray jpeg = m_videoBuffer.left(end + jpegEnd.size());
        m_videoBuffer.remove(0, end + jpegEnd.size());
        QImage frame;
        if (!frame.loadFromData(jpeg, "JPG"))
            continue;

        showFramePreview(frame);
        if (!m_activeSourceId.isEmpty())
            setSourceFrame(m_activeSourceId, frame);
        if (m_registered)
            setActiveSource(m_activeSourceId.isEmpty()
                               ? QStringLiteral("client://team%1/robot%2")
                                     .arg(m_selectedTeam).arg(selectedRobotId())
                               : m_activeSourceId,
                           ownViewTitle());
        setVideoStatus(tr("预览中：%1 × %2").arg(frame.width()).arg(frame.height()),
                       QColor(QStringLiteral("#72d39a")));
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
        const QPixmap pixmap = QPixmap::fromImage(frame).scaled(
            label->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
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
    if (m_lastVideoFrame.isNull())
        return;
    showFramePreview(m_lastVideoFrame);
}
