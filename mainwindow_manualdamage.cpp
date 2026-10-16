// Manual HP control: phones on the LAN (deduct) and the referee console
// (deduct or set an absolute HP). Firmware only supports SET_HP (absolute),
// so a deduction is "fresh telemetry HP - amount" sent as SET_HP and reported
// only after the car ACKs it. To limit races with real hits: one in-flight
// change per side, and the robot's last uplink must be <= 300 ms old.
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
    if (m_competition.phase() != CompetitionFlow::Phase::Running) return tr("仅小局进行中可扣血");
    const quint8 robotId = m_activeRobotIds.value(team);
    // No-device debug binds only the cars that are online; a side without
    // one simply has nothing to control.
    if (!robotId) return m_noDeviceDebug ? tr("无车调试：本方没有连接车辆") : tr("本方未绑定机器人");
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
        {"message", tr("第 %1 局 · %2 · %3:%4").arg(m_competition.roundIndex()).arg(m_competition.phaseText())
                        .arg(seconds / 60, 2, 10, QLatin1Char('0')).arg(seconds % 60, 2, 10, QLatin1Char('0'))},
    };
    return o;
}

void MainWindow::onManualDamageRequested(quint64 ticket, quint8 team, const QString &peer)
{
    const auto fail = [&](const QString &message) {
        m_damageServer->completeDamage(ticket, {{"ok", false}, {"message", message}});
        onLogMessage(tr("[手动扣血] %1 拒绝 · %2 · 来自 %3").arg(sideName(team), message, peer));
    };
    const QString error = startHpChange(team, -1, ticket, peer);
    if (!error.isEmpty()) fail(error);
}

QString MainWindow::startHpChange(quint8 team, int setHp, quint64 ticket, const QString &source)
{
    if (m_manualDamage.contains(team)) return tr("上一次血量修改还在等车辆确认，请稍候");
    RobotManager::RobotInfo robot;
    const QString blocked = manualDamageBlockReason(team, &robot);
    if (!blocked.isEmpty()) return blocked;
    // setHp < 0: deduct the configured amount from the latest telemetry HP.
    const int amount = m_damageAmountEdit ? m_damageAmountEdit->value() : 0;
    const int toHp = setHp >= 0 ? qBound(0, setHp, 300) : qMax(0, robot.hp - amount);
    const quint32 txid = m_commander ? m_commander->sendSetHp(robot.robotId, quint16(toHp)) : 0;
    if (!txid) return tr("命令发送失败，请检查 UDP 监听");
    m_manualDamage.insert(team, {ticket, team, robot.robotId, txid, robot.hp, toHp, source});
    // Backstop: if the commander drops the pending command without a timeout
    // signal (listener stopped, pending cancelled), never leave a request hanging.
    QTimer::singleShot(1500, this, [this, team, txid, robotId = robot.robotId] {
        const auto it = m_manualDamage.constFind(team);
        if (it != m_manualDamage.constEnd() && it->txid == txid)
            finishManualDamage(txid, proto::TypeSetHp, robotId, -1);
    });
    onLogMessage(tr("[%1] %2 机器人 %3 · HP %4 → %5 · 已发送，等待 ACK（txid=%6）· 来自 %7")
                     .arg(setHp >= 0 ? tr("设定血量") : tr("手动扣血"), sideName(team))
                     .arg(robot.robotId).arg(robot.hp).arg(toHp).arg(txid).arg(source));
    refreshManualDamageUi();
    return {};
}

void MainWindow::requestRefereeHp(quint8 team, bool deduct)
{
    if (team != 1 && team != 2) return;
    const int setHp = deduct ? -1 : (m_refHpEdit[team - 1] ? m_refHpEdit[team - 1]->value() : -1);
    if (!deduct && setHp < 0) return;
    const QString error = startHpChange(team, setHp, 0, tr("裁判控制台"));
    if (!error.isEmpty()) {
        showActionResult(tr("%1%2失败 · %3").arg(sideName(team), deduct ? tr("扣血") : tr("设定血量"), error),
                         QStringLiteral("error"));
        onLogMessage(tr("[裁判血量] %1 拒绝 · %2").arg(sideName(team), error));
    } else {
        showActionResult(tr("%1%2已发送，等待车辆确认").arg(sideName(team), deduct ? tr("扣血") : tr("设定血量")),
                         QStringLiteral("info"));
    }
    refreshRefereeHpUi();
}

void MainWindow::refreshRefereeHpUi()
{
    if (!m_refHpStateLabel) return;
    const int amount = m_damageAmountEdit ? m_damageAmountEdit->value() : 0;
    QStringList states;
    for (int i = 0; i < 2; ++i) {
        const quint8 team = quint8(i + 1);
        RobotManager::RobotInfo robot;
        const QString blocked = manualDamageBlockReason(team, &robot);
        const bool busy = m_manualDamage.contains(team);
        const bool ok = blocked.isEmpty() && !busy;
        m_refDamageBtn[i]->setText(tr("扣 %1 血").arg(amount));
        m_refDamageBtn[i]->setEnabled(ok);
        m_refSetHpBtn[i]->setEnabled(ok);
        m_refHpEdit[i]->setEnabled(ok);
        const QString hp = robot.robotId && robot.hp >= 0 ? QString::number(robot.hp) : tr("--");
        states << tr("%1 HP %2 · %3").arg(sideName(team), hp, busy ? tr("确认中") : (ok ? tr("可操作") : blocked));
    }
    const QString text = states.join(QStringLiteral("   "));
    if (m_refHpStateLabel->text() != text) m_refHpStateLabel->setText(text);
}

bool MainWindow::finishManualDamage(quint32 txid, quint8 type, quint8 robotId, int result)
{
    if (type != proto::TypeSetHp) return false;
    for (auto it = m_manualDamage.begin(); it != m_manualDamage.end(); ++it) {
        if (it->txid != txid || it->robotId != robotId) continue;
        const ManualDamage d = it.value();
        m_manualDamage.erase(it);
        QJsonObject reply;
        QString text;
        if (result == 0) {
            text = d.toHp <= d.fromHp ? tr("已扣 %1 血 · HP %2 → %3").arg(d.fromHp - d.toHp).arg(d.fromHp).arg(d.toHp)
                                      : tr("已设定血量 · HP %1 → %2").arg(d.fromHp).arg(d.toHp);
            reply = {{"ok", true}, {"hp", d.toHp}, {"message", text}};
            onLogMessage(tr("[血量修改] %1 机器人 %2 已确认 · HP %3 → %4 · 来自 %5")
                             .arg(sideName(d.team)).arg(d.robotId).arg(d.fromHp).arg(d.toHp).arg(d.peer));
        } else if (result < 0) {
            text = tr("车辆确认超时，血量修改结果未知，请看控制台血量");
            reply = {{"ok", false}, {"message", text}};
            onLogMessage(tr("[血量修改] %1 机器人 %2 确认超时，结果未知（txid=%3）")
                             .arg(sideName(d.team)).arg(d.robotId).arg(txid));
        } else {
            text = tr("车辆拒绝修改血量（错误码 %1）").arg(result);
            reply = {{"ok", false}, {"message", text}};
            onLogMessage(tr("[血量修改] %1 机器人 %2 拒绝，错误码 %3")
                             .arg(sideName(d.team)).arg(d.robotId).arg(result));
        }
        // ticket 0 = referee console; phone requests are answered over HTTP.
        if (d.ticket && m_damageServer) m_damageServer->completeDamage(d.ticket, reply);
        else showActionResult(sideName(d.team) + QStringLiteral(" · ") + text,
                              result == 0 ? QStringLiteral("success") : QStringLiteral("error"));
        refreshManualDamageUi();
        refreshRefereeHpUi();
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
