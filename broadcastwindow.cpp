#include "broadcastwindow.h"

#include <QCloseEvent>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QScreen>
#include <QShowEvent>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QWindow>

namespace {
constexpr quint8 kRedTeam = 1;
constexpr quint8 kBlueTeam = 2;
constexpr quint8 kRobotOne = 1;
constexpr quint8 kRobotTwo = 2;

QLabel *makeValueLabel(QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    label->setMinimumWidth(72);
    return label;
}

void setValueColor(QLabel *label, const QColor &color)
{
    label->setStyleSheet(QStringLiteral("color: %1; font-weight: 600;").arg(color.name()));
}
} // namespace

BroadcastWindow::BroadcastWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("赛事转播画面"));
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_DeleteOnClose, false);
    buildUi();
}

void BroadcastWindow::buildUi()
{
    auto *central = new QWidget(this);
    central->setStyleSheet(QStringLiteral(
        "QWidget { background: #111820; color: #f3f6f8; }"
        "QFrame#robotCard { background: #1b2630; border: 1px solid #41515e; border-radius: 6px; }"
        "QFrame#videoFrame { background: #080b0e; border: 1px solid #41515e; }"
        "QLabel#teamRed { color: #ff7168; }"
        "QLabel#teamBlue { color: #70a8ff; }"
        "QLabel#programTitle { color: #aebdca; }"));

    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(30, 24, 30, 24);
    root->setSpacing(18);

    auto *header = new QHBoxLayout;
    auto *title = new QLabel(tr("2v2 步兵对抗赛"), central);
    QFont titleFont = title->font();
    titleFont.setPointSize(22);
    titleFont.setBold(true);
    title->setFont(titleFont);
    m_matchState = new QLabel(tr("等待比赛数据"), central);
    m_matchState->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    header->addWidget(title);
    header->addStretch();
    header->addWidget(m_matchState);
    root->addLayout(header);

    auto *matchLayout = new QHBoxLayout;
    matchLayout->setSpacing(18);

    auto *redLayout = new QVBoxLayout;
    auto *redTitle = new QLabel(tr("红方"), central);
    redTitle->setObjectName(QStringLiteral("teamRed"));
    QFont teamFont = redTitle->font();
    teamFont.setPointSize(16);
    teamFont.setBold(true);
    redTitle->setFont(teamFont);
    redLayout->addWidget(redTitle);
    RobotCard redOne;
    redLayout->addWidget(createRobotCard(tr("红方 1 号步兵"), &redOne));
    m_cards.insert(RobotManager::keyOf(kRedTeam, kRobotOne), redOne);
    RobotCard redTwo;
    redLayout->addWidget(createRobotCard(tr("红方 2 号步兵"), &redTwo));
    m_cards.insert(RobotManager::keyOf(kRedTeam, kRobotTwo), redTwo);
    redLayout->addStretch();

    auto *programLayout = new QVBoxLayout;
    auto *programTitle = new QLabel(tr("主画面"), central);
    programTitle->setObjectName(QStringLiteral("programTitle"));
    programLayout->addWidget(programTitle);
    m_programView = new QLabel(tr("等待视频流\n主画面占位"), central);
    m_programView->setObjectName(QStringLiteral("videoFrame"));
    m_programView->setAlignment(Qt::AlignCenter);
    m_programView->setMinimumSize(640, 360);
    m_programView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    programLayout->addWidget(m_programView, 1);

    auto *blueLayout = new QVBoxLayout;
    auto *blueTitle = new QLabel(tr("蓝方"), central);
    blueTitle->setObjectName(QStringLiteral("teamBlue"));
    blueTitle->setFont(teamFont);
    blueLayout->addWidget(blueTitle);
    RobotCard blueOne;
    blueLayout->addWidget(createRobotCard(tr("蓝方 1 号步兵"), &blueOne));
    m_cards.insert(RobotManager::keyOf(kBlueTeam, kRobotOne), blueOne);
    RobotCard blueTwo;
    blueLayout->addWidget(createRobotCard(tr("蓝方 2 号步兵"), &blueTwo));
    m_cards.insert(RobotManager::keyOf(kBlueTeam, kRobotTwo), blueTwo);
    blueLayout->addStretch();

    matchLayout->addLayout(redLayout, 1);
    matchLayout->addLayout(programLayout, 4);
    matchLayout->addLayout(blueLayout, 1);
    root->addLayout(matchLayout, 1);

    auto *previewLayout = new QHBoxLayout;
    previewLayout->setSpacing(18);
    m_leftPreview = new QLabel(tr("全场视角\n等待视频流"), central);
    m_rightPreview = new QLabel(tr("备用画面\n等待视频流"), central);
    for (QLabel *preview : {m_leftPreview, m_rightPreview}) {
        preview->setObjectName(QStringLiteral("videoFrame"));
        preview->setAlignment(Qt::AlignCenter);
        preview->setMinimumHeight(105);
        preview->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        previewLayout->addWidget(preview);
    }
    root->addLayout(previewLayout);

    setCentralWidget(central);
}

QWidget *BroadcastWindow::createRobotCard(const QString &title, RobotCard *card)
{
    auto *frame = new QFrame(this);
    frame->setObjectName(QStringLiteral("robotCard"));
    frame->setMinimumHeight(132);

    auto *layout = new QGridLayout(frame);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setHorizontalSpacing(10);
    layout->setVerticalSpacing(7);

    card->name = new QLabel(title, frame);
    QFont nameFont = card->name->font();
    nameFont.setBold(true);
    card->name->setFont(nameFont);
    card->name->setWordWrap(true);
    card->hp = makeValueLabel(frame);
    card->state = makeValueLabel(frame);
    card->shoot = makeValueLabel(frame);

    layout->addWidget(card->name, 0, 0, 1, 2);
    layout->addWidget(new QLabel(tr("HP"), frame), 1, 0);
    layout->addWidget(card->hp, 1, 1);
    layout->addWidget(new QLabel(tr("状态"), frame), 2, 0);
    layout->addWidget(card->state, 2, 1);
    layout->addWidget(new QLabel(tr("射击"), frame), 3, 0);
    layout->addWidget(card->shoot, 3, 1);

    updateCard(nullptr, *card);
    return frame;
}

QString BroadcastWindow::teamName(quint8 team)
{
    if (team == kRedTeam) return tr("红方");
    if (team == kBlueTeam) return tr("蓝方");
    return tr("队伍 %1").arg(team);
}

void BroadcastWindow::updateCard(const RobotManager::RobotInfo *robot, RobotCard &card)
{
    if (!robot) {
        card.hp->setText(tr("-"));
        card.state->setText(tr("等待连接"));
        card.shoot->setText(tr("-"));
        setValueColor(card.hp, QColor(QStringLiteral("#aebdca")));
        setValueColor(card.state, QColor(QStringLiteral("#aebdca")));
        setValueColor(card.shoot, QColor(QStringLiteral("#aebdca")));
        return;
    }

    if (!robot->online) {
        card.hp->setText(robot->hp >= 0 ? QString::number(robot->hp) : tr("-"));
        card.state->setText(tr("离线"));
        card.shoot->setText(tr("-"));
        setValueColor(card.state, QColor(QStringLiteral("#aebdca")));
        setValueColor(card.shoot, QColor(QStringLiteral("#aebdca")));
        return;
    }

    card.hp->setText(robot->hp >= 0 ? QString::number(robot->hp) : tr("-"));
    card.state->setText(robot->alive ? tr("存活") : tr("阵亡"));
    card.shoot->setText(robot->shootEnabled ? tr("允许") : tr("禁止"));
    setValueColor(card.hp, QColor(QStringLiteral("#f3f6f8")));
    setValueColor(card.state, robot->alive ? QColor(QStringLiteral("#72d39a"))
                                           : QColor(QStringLiteral("#ff7168")));
    setValueColor(card.shoot, robot->shootEnabled ? QColor(QStringLiteral("#72d39a"))
                                                  : QColor(QStringLiteral("#ff7168")));
}

void BroadcastWindow::updateRobots(const QVector<RobotManager::RobotInfo> &robots)
{
    QHash<quint64, RobotManager::RobotInfo> current;
    for (const auto &robot : robots)
        current.insert(RobotManager::keyOf(robot.team, robot.robotId), robot);

    int onlineCount = 0;
    for (auto it = m_cards.begin(); it != m_cards.end(); ++it) {
        const auto found = current.constFind(it.key());
        if (found == current.constEnd()) {
            updateCard(nullptr, it.value());
        } else {
            updateCard(&found.value(), it.value());
            if (found->online) ++onlineCount;
        }
    }

    m_matchState->setText(onlineCount == 4
                              ? tr("4 台机器人已连接")
                              : tr("机器人在线 %1 / 4").arg(onlineCount));
}

void BroadcastWindow::showOnScreen(QScreen *screen)
{
    QScreen *target = screen ? screen : QGuiApplication::primaryScreen();
    if (!target) return;

    showNormal();
    if (windowHandle())
        windowHandle()->setScreen(target);
    setGeometry(target->geometry());
    showFullScreen();
    raise();
    activateWindow();
}

void BroadcastWindow::closeEvent(QCloseEvent *event)
{
    event->ignore();
    hide();
}

void BroadcastWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    emit visibilityChanged(true);
}

void BroadcastWindow::hideEvent(QHideEvent *event)
{
    QMainWindow::hideEvent(event);
    emit visibilityChanged(false);
}
