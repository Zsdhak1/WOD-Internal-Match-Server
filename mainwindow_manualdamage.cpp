// Manual HP deduction from phones on the LAN.
// Firmware only supports SET_HP (absolute), so a deduction is
// "fresh telemetry HP - amount" sent as SET_HP and reported to the phone only
// after the car ACKs it. To limit races with real hits: one in-flight
// deduction per side, and the robot's last uplink must be <= 300 ms old.
#include "mainwindow.h"
#include "broadcastwindow.h"
#include "protocol.h"
#include "robotcommander.h"
#include "robotmanager.h"

#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

namespace {
constexpr qint64 kFreshTelemetryMs = 300;
QString sideName(quint8 team) { return team == 1 ? QObject::tr("红方") : QObject::tr("蓝方"); }
}

QString MainWindow::manualDamageBlockReason(quint8 team, RobotManager::RobotInfo *robot) const
{
    if (!competitionMode()) return tr("仅正式比赛模式可用");
    if (m_noDeviceDebug) return tr("无车调试中没有设备可扣血");
    if (m_competition.phase() != CompetitionFlow::Phase::Running) return tr("仅小局进行中可扣血");
    const quint8 robotId = m_activeRobotIds.value(team);
    if (!robotId) return tr("本方未绑定机器人");
    RobotManager::RobotInfo found;
    bool exists = false;
    for (const auto &r : activeMatchRobots())
        if (r.team == team && r.robotId == robotId) { found = r; exists = true; }
    if (!exists || !found.online) return tr("机器人离线");
    if (robot) *robot = found;
    if (found.hp < 0) return tr("尚未收到血量数据");
    if (found.hp == 0 || !found.alive) return tr("机器人已阵亡");
    if (m_competition.revivePending(team)) return tr("复活确认中，暂不能扣血");
    if (QDateTime::currentMSecsSinceEpoch() - found.lastSeenMs > kFreshTelemetryMs)
        return tr("车辆状态不是最新，请稍后重试");
    return {};
}

QJsonObject MainWindow::manualDamageState(quint8 team) const
{
    RobotManager::RobotInfo robot;
    const QString blocked = manualDamageBlockReason(team, &robot);
    const bool busy = m_manualDamage.contains(team);
    const int seconds = m_competition.remainingSeconds(refereeNow());
    QJsonObject o{
        {"team", team == 1 ? "red" : "blue"},
        {"teamName", m_broadcast ? (team == 1 ? m_broadcast->redTeamName() : m_broadcast->blueTeamName()) : QString()},
        {"amount", m_damageAmountEdit ? m_damageAmountEdit->value() : 0},
        {"hp", robot.robotId ? robot.hp : -1},
        {"available", blocked.isEmpty() && !busy},
        {"reason", busy ? tr("上一次扣血确认中") : blocked},
        {"message", competitionMode()
             ? tr("第 %1 局 · %2 · %3:%4").arg(m_competition.roundIndex()).arg(m_competition.phaseText())
                   .arg(seconds / 60, 2, 10, QLatin1Char('0')).arg(seconds % 60, 2, 10, QLatin1Char('0'))
             : tr("非正式比赛模式")},
    };
    return o;
}

void MainWindow::onManualDamageRequested(quint64 ticket, quint8 team, const QString &peer)
{
    const auto fail = [&](const QString &message) {
        m_damageServer->completeDamage(ticket, {{"ok", false}, {"message", message}});
        onLogMessage(tr("[手动扣血] %1 拒绝 · %2 · 来自 %3").arg(sideName(team), message, peer));
    };
    if (m_manualDamage.contains(team)) { fail(tr("上一次扣血还在等车辆确认，请稍候")); return; }
    RobotManager::RobotInfo robot;
    const QString blocked = manualDamageBlockReason(team, &robot);
    if (!blocked.isEmpty()) { fail(blocked); return; }
    const int amount = m_damageAmountEdit ? m_damageAmountEdit->value() : 0;
    const int toHp = qMax(0, robot.hp - amount);
    const quint32 txid = m_commander ? m_commander->sendSetHp(robot.robotId, quint16(toHp)) : 0;
    if (!txid) { fail(tr("命令发送失败，请检查 UDP 监听")); return; }
    m_manualDamage.insert(team, {ticket, team, robot.robotId, txid, robot.hp, toHp, peer});
    // Backstop: if the commander drops the pending command without a timeout
    // signal (listener stopped, pending cancelled), never leave the phone hanging.
    QTimer::singleShot(1500, this, [this, team, txid, robotId = robot.robotId] {
        const auto it = m_manualDamage.constFind(team);
        if (it != m_manualDamage.constEnd() && it->txid == txid)
            finishManualDamage(txid, proto::TypeSetHp, robotId, -1);
    });
    onLogMessage(tr("[手动扣血] %1 机器人 %2 · HP %3 → %4 · 已发送，等待 ACK（txid=%5）· 来自 %6")
                     .arg(sideName(team)).arg(robot.robotId).arg(robot.hp).arg(toHp).arg(txid).arg(peer));
    refreshManualDamageUi();
}

bool MainWindow::finishManualDamage(quint32 txid, quint8 type, quint8 robotId, int result)
{
    if (type != proto::TypeSetHp) return false;
    for (auto it = m_manualDamage.begin(); it != m_manualDamage.end(); ++it) {
        if (it->txid != txid || it->robotId != robotId) continue;
        const ManualDamage d = it.value();
        m_manualDamage.erase(it);
        QJsonObject reply;
        if (result == 0) {
            reply = {{"ok", true}, {"hp", d.toHp},
                     {"message", tr("已扣 %1 血 · HP %2 → %3").arg(d.fromHp - d.toHp).arg(d.fromHp).arg(d.toHp)}};
            onLogMessage(tr("[手动扣血] %1 机器人 %2 已确认 · HP %3 → %4")
                             .arg(sideName(d.team)).arg(d.robotId).arg(d.fromHp).arg(d.toHp));
        } else if (result < 0) {
            reply = {{"ok", false}, {"message", tr("车辆确认超时，扣血结果未知，请看控制台血量")}};
            onLogMessage(tr("[手动扣血] %1 机器人 %2 确认超时，结果未知（txid=%3）")
                             .arg(sideName(d.team)).arg(d.robotId).arg(txid));
        } else {
            reply = {{"ok", false}, {"message", tr("车辆拒绝扣血（错误码 %1）").arg(result)}};
            onLogMessage(tr("[手动扣血] %1 机器人 %2 拒绝，错误码 %3")
                             .arg(sideName(d.team)).arg(d.robotId).arg(result));
        }
        if (m_damageServer) m_damageServer->completeDamage(d.ticket, reply);
        refreshManualDamageUi();
        return true;
    }
    return false;
}

void MainWindow::refreshManualDamageUi()
{
    if (!m_damageServerBtn || !m_damageServer) return;
    const bool on = m_damageServer->isListening();
    m_damageServerBtn->setText(on ? tr("停止扣血网页") : tr("启动扣血网页"));
    m_damagePortEdit->setEnabled(!on);
    m_damageQrBtn->setEnabled(on);
    if (!on) { m_damageStateLabel->setText(tr("未启动")); return; }
    const QStringList addresses = ManualDamageServer::lanAddresses();
    const QString host = addresses.isEmpty() ? QStringLiteral("127.0.0.1") : addresses.first();
    QString text = tr("http://%1:%2 · 无口令，同网可访问").arg(host).arg(m_damageServer->port());
    if (!m_manualDamage.isEmpty()) text += tr(" · 确认中 %1").arg(m_manualDamage.size());
    m_damageStateLabel->setText(text);
}

void MainWindow::onToggleManualDamageServer()
{
    if (!m_damageServer) return;
    if (m_damageServer->isListening()) {
        m_damageServer->stop();
        showActionResult(tr("扣血网页已停止"));
    } else {
        const quint16 port = quint16(m_damagePortEdit->value());
        if (!m_damageServer->start(port)) {
            showActionResult(tr("扣血网页启动失败 · 端口 %1 可能已被占用").arg(port), QStringLiteral("error"));
        } else {
            QSettings s(QSettings::defaultFormat(), QSettings::UserScope,
                        QStringLiteral("Scompetition"), QStringLiteral("ManualDamage"));
            s.setValue(QStringLiteral("port"), port);
            showActionResult(tr("扣血网页已启动 · HTTP %1 · 同局域网任何人都可访问，建议使用独立比赛 Wi-Fi").arg(port),
                             QStringLiteral("info"));
        }
    }
    refreshManualDamageUi();
}

void MainWindow::onShowManualDamageQr()
{
    if (!m_damageServer || !m_damageServer->isListening()) return;
    QStringList addresses = ManualDamageServer::lanAddresses();
    if (addresses.isEmpty()) addresses << QStringLiteral("127.0.0.1");

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("manualDamageQrDialog"));
    dialog.setWindowTitle(tr("扣血网页 · 扫码进入"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *hint = new QLabel(tr("手机连接同一局域网后扫码。安卓确认扣血后振动；iPhone 显示提示条。"), &dialog);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *address = new QComboBox(&dialog);
    address->setObjectName(QStringLiteral("damageQrAddress"));
    address->addItems(addresses);
    address->setToolTip(tr("本机有多个网卡时，选择手机所在网络的地址"));
    layout->addWidget(address);
    auto *grid = new QGridLayout;
    QLabel *codes[2] = {}, *urls[2] = {};
    for (int i = 0; i < 2; ++i) {
        auto *title = new QLabel(i == 0 ? tr("红方") : tr("蓝方"), &dialog);
        title->setAlignment(Qt::AlignCenter);
        title->setStyleSheet(i == 0 ? QStringLiteral("color:#ff5a63;font-size:20px;font-weight:bold;")
                                    : QStringLiteral("color:#5aa0ff;font-size:20px;font-weight:bold;"));
        codes[i] = new QLabel(&dialog);
        codes[i]->setObjectName(i == 0 ? QStringLiteral("damageQrRed") : QStringLiteral("damageQrBlue"));
        codes[i]->setAlignment(Qt::AlignCenter);
        urls[i] = new QLabel(&dialog);
        urls[i]->setAlignment(Qt::AlignCenter);
        urls[i]->setTextInteractionFlags(Qt::TextSelectableByMouse);
        grid->addWidget(title, 0, i);
        grid->addWidget(codes[i], 1, i);
        grid->addWidget(urls[i], 2, i);
    }
    layout->addLayout(grid);
    const auto render = [&, this] {
        for (int i = 0; i < 2; ++i) {
            const QString url = QStringLiteral("http://%1:%2/%3")
                .arg(address->currentText()).arg(m_damageServer->port())
                .arg(i == 0 ? QStringLiteral("red") : QStringLiteral("blue"));
            codes[i]->setPixmap(QPixmap::fromImage(ManualDamageServer::qrCode(url, 7, 3)));
            urls[i]->setText(url);
        }
    };
    connect(address, &QComboBox::currentIndexChanged, &dialog, render);
    render();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    buttons->button(QDialogButtonBox::Close)->setText(tr("关闭"));
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (qEnvironmentVariableIsSet("SCOMP_DAMAGE_QR_CAPTURE")) {
        // Test hook: render the dialog to an image instead of blocking.
        dialog.adjustSize();
        dialog.grab().save(qEnvironmentVariable("SCOMP_DAMAGE_QR_CAPTURE"));
        return;
    }
    dialog.exec();
}
