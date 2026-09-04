#include "mainwindow.h"
#include "broadcastwindow.h"
#include "protocol.h"
#include "robotmanager.h"

#include <QColor>
#include <QDateTime>
#include <QComboBox>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHostAddress>
#include <QLabel>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QSpinBox>
#include <QStatusBar>
#include <QStringList>
#include <QTableWidget>
#include <QTime>
#include <QUdpSocket>
#include <QVBoxLayout>

namespace {
constexpr int kColId    = 0;
constexpr int kColTeam  = 1;
constexpr int kColHp    = 2;
constexpr int kColState = 3;
constexpr int kColShoot = 4;
constexpr int kColAddr  = 5;
} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    m_socket = new QUdpSocket(this);
    connect(m_socket, &QUdpSocket::readyRead, this, &MainWindow::onReadyRead);

    m_robots = new RobotManager(this);
    connect(m_robots, &RobotManager::robotsChanged, this, &MainWindow::refreshTable);
    connect(m_robots, &RobotManager::logMessage, this, &MainWindow::onLogMessage);

    m_broadcast = new BroadcastWindow;

    buildUi();
    populateScreens();
    connect(m_broadcast, &BroadcastWindow::visibilityChanged, this, [this](bool visible) {
        if (m_broadcastBtn)
            m_broadcastBtn->setText(visible ? tr("隐藏转播画面") : tr("显示转播画面"));
    });
    startListen();
}

MainWindow::~MainWindow()
{
    if (m_broadcast) {
        m_broadcast->close();
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
    broadcastRow->addWidget(m_screenEdit);
    broadcastRow->addWidget(m_broadcastBtn);
    broadcastRow->addStretch();
    root->addLayout(broadcastRow);

    auto *tableTitle = new QLabel(tr("机器人状态"), central);
    QFont tableTitleFont = tableTitle->font();
    tableTitleFont.setBold(true);
    tableTitle->setFont(tableTitleFont);
    root->addWidget(tableTitle);

    m_table = new QTableWidget(0, 6, central);
    m_table->setHorizontalHeaderLabels({tr("机器人ID"), tr("队伍"), tr("血量 HP"),
                                        tr("状态"), tr("射击"), tr("来源地址")});
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

    const auto bots = m_robots->robots();
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
        auto stateItem = new QTableWidgetItem(stateText);
        stateItem->setForeground(stateColor);
        stateItem->setFont(bold);
        stateItem->setTextAlignment(Qt::AlignCenter);
        auto shootItem = new QTableWidgetItem(shootText);
        shootItem->setForeground(shootColor);
        shootItem->setTextAlignment(Qt::AlignCenter);
        QString src = r.online
                          ? QStringLiteral("%1:%2").arg(r.addr.toString()).arg(r.port)
                          : QString();
        auto addrItem = new QTableWidgetItem(src);

        m_table->setItem(row, kColId, idItem);
        m_table->setItem(row, kColTeam, teamItem);
        m_table->setItem(row, kColHp, hpItem);
        m_table->setItem(row, kColState, stateItem);
        m_table->setItem(row, kColShoot, shootItem);
        m_table->setItem(row, kColAddr, addrItem);
    }
    m_table->setSortingEnabled(true);
    m_table->resizeColumnsToContents();
    m_table->horizontalHeader()->setSectionResizeMode(kColAddr, QHeaderView::Stretch);

    statusBar()->showMessage(tr("在线 %1 / %2 台机器人").arg(onlineCount).arg(bots.size()));

    if (m_broadcast)
        m_broadcast->updateRobots(bots);
}

void MainWindow::onLogMessage(const QString &message)
{
    if (!m_log) return;
    m_log->appendPlainText(QStringLiteral("[%1] %2")
                               .arg(QTime::currentTime().toString(QStringLiteral("hh:mm:ss.zzz")),
                                    message));
}
