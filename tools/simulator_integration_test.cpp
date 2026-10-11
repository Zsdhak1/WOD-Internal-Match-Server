#include "simulatorwindow.h"
#include "mainwindow.h"
#include "clientwindow.h"
#include "robotcommander.h"
#include "matchreferee.h"
#include "protocol.h"
#include "fake_field_camera.h"
#include "checkinpanel.h"
#include <QScrollArea>

#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QDir>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFrame>
#include <QFontDatabase>
#include <QFileInfo>
#include <QImage>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QTabWidget>
#include <QTabBar>
#include <QAbstractButton>
#include <QTemporaryDir>
#include <QThread>
#include <QUdpSocket>
#include <QDebug>
#include <functional>
#include <stdexcept>
#include <cstdio>

namespace {
void check(bool condition, const QString &message)
{
    if (!condition) {
        qCritical() << "Assertion failed:" << message;
        throw std::runtime_error(message.toStdString());
    }
}
template<class T> T *child(QObject &parent, const char *name)
{
    auto *result = parent.findChild<T *>(QString::fromLatin1(name));
    check(result != nullptr, QStringLiteral("Missing control: %1").arg(name));
    return result;
}
void invoke(QObject &object, const char *slot)
{
    check(QMetaObject::invokeMethod(&object, slot, Qt::DirectConnection),
          QStringLiteral("Cannot invoke %1").arg(slot));
}
void settle(int ms = 30)
{
    QElapsedTimer timer; timer.start();
    do { QApplication::processEvents(); QThread::msleep(2); } while (timer.elapsed() < ms);
}
void waitFor(const std::function<bool()> &condition, const QString &message, int ms = 2500)
{
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < ms) settle(5);
    check(condition(), message);
}
BroadcastWindow *serverBroadcast()
{
    for (QWidget *widget : QApplication::topLevelWidgets()) {
        auto *window = qobject_cast<BroadcastWindow *>(widget);
        if (window && !qobject_cast<ClientWindow *>(widget))
            return window;
    }
    throw std::runtime_error("Missing server broadcast window");
}
QFrame *panel(BroadcastWindow &window, const QString &team)
{
    for (auto *frame : window.findChildren<QFrame *>(QStringLiteral("teamHud")))
        if (frame->property("team").toString() == team) return frame;
    throw std::runtime_error("Missing HUD team panel");
}
void configureSimulator(SimulatorWindow &sim, const QString &ip, quint16 port)
{
    child<QLineEdit>(sim, "simLocalAddress")->setText(ip);
    child<QSpinBox>(sim, "simServerPort")->setValue(port);
    invoke(sim, "onToggleRunning");
    check(!child<QLineEdit>(sim, "simLocalAddress")->isEnabled(), "Simulator failed to bind");
}
QByteArray deviceMac(SimulatorWindow &sim)
{
    return QByteArray::fromHex(child<QLineEdit>(sim, "simDeviceMac")->text().remove(':').toLatin1());
}
void assign(MainWindow &server, SimulatorWindow &sim, int id, int team)
{
    auto *devices = child<QComboBox>(server, "deviceSelect");
    const QString mac = child<QLineEdit>(sim, "simDeviceMac")->text();
    const int index = devices->findData(mac);
    check(index >= 0, "Simulator not discoverable by server");
    devices->setCurrentIndex(index);
    child<QSpinBox>(server, "deviceRobotId")->setValue(id);
    child<QComboBox>(server, "deviceTeam")->setCurrentIndex(team);
    child<QLineEdit>(server, "deviceControllerMac")->setText(QStringLiteral("8D:23:AB:A5:3C:C9"));
    invoke(server, "onAssignTeam");
}
int rowFor(QTableWidget &table, int id)
{
    for (int row = 0; row < table.rowCount(); ++row)
        if (table.item(row, 0)->text().toInt() == id) return row;
    return -1;
}
void verifyRenderedFill(const QImage &empty, const QImage &full, const QImage &actual,
                        double fraction, bool reverse)
{
    int changedPixels = 0, mismatches = 0;
    // Qt clips at a rounded logical-pixel boundary before applying DPR.
    // Compare physical pixels against that boundary, not a second rounding
    // of the physical width (which differs for 125%/150% displays).
    const qreal dpr = full.devicePixelRatio();
    const int logicalWidth = qRound(full.width()/dpr);
    const int fillWidth = qRound(logicalWidth * fraction);
    const qreal boundary = (reverse ? logicalWidth-fillWidth : fillWidth) * dpr;
    for (int y = 0; y < full.height(); ++y) for (int x = 0; x < full.width(); ++x) {
        const QColor a = empty.pixelColor(x, y), b = full.pixelColor(x, y), c = actual.pixelColor(x, y);
        const int delta = qAbs(b.red()-a.red()) + qAbs(b.green()-a.green()) + qAbs(b.blue()-a.blue());
        if (delta < 12) continue;
        if (qAbs(x+0.5-boundary) < 1.0) continue;
        ++changedPixels;
        const bool filled = reverse ? x+0.5 >= boundary : x+0.5 < boundary;
        const QColor expected = filled ? b : a;
        if (qAbs(c.red()-expected.red()) + qAbs(c.green()-expected.green())
              + qAbs(c.blue()-expected.blue()) > 12) ++mismatches;
    }
    check(changedPixels > 20, "Progress bar has no rendered fill/asset");
    check(mismatches <= qMax(2, changedPixels/100),
          QStringLiteral("Wrong fill boundary at %1: %2/%3 pixels").arg(fraction).arg(mismatches).arg(changedPixels));
}
void verifyHudRendering(const QString &outDir)
{
    BroadcastWindow hud;
    RobotManager::RobotInfo red, blue;
    red.robotId = 71; red.team = 1; red.protocolVersion = 2; red.hp = 300; red.heat = 0;
    red.power = 45; red.online = true; red.powerOn = true; red.linkUp = true;
    blue = red; blue.robotId = 72; blue.team = 2;
    hud.setTeamNames(QStringLiteral("红方测试"), QStringLiteral("蓝方测试"));
    hud.setScores(2, 1); hud.setMatchState(37, false);
    hud.setTickerText(QStringLiteral("协议与状态回归验证")); hud.setTickerVisible(true, false);
    {
        QTemporaryDir migrationDir;
        const QString file = migrationDir.path()+QStringLiteral("/legacy-layout.ini");
        QSettings legacy(file, QSettings::IniFormat);
        legacy.setValue(QStringLiteral("broadcast_layout/version"), 2);
        legacy.setValue(QStringLiteral("broadcast_layout/center_hud/x"), 0);
        legacy.setValue(QStringLiteral("broadcast_layout/center_hud/y"), 30);
        legacy.setValue(QStringLiteral("broadcast_layout/center_hud/width"), 786);
        legacy.setValue(QStringLiteral("broadcast_layout/center_hud/height"), 225);
        legacy.setValue(QStringLiteral("broadcast_layout/match_timer/y"), -50);
        legacy.setValue(QStringLiteral("broadcast_layout/left_score_panel/x"), 60);
        legacy.setValue(QStringLiteral("broadcast_layout/right_score_panel/x"), -60);
        legacy.sync();
        check(hud.loadLayout(file), "Cannot migrate V2 layout");
        check(hud.layoutElementPosition(QStringLiteral("center_hud")) == QPoint()
                  && hud.layoutElementPosition(QStringLiteral("match_timer")) == QPoint(),
              "Legacy central anchors were not migrated");
        check(hud.saveLayout(file), "Cannot save composite V3 layout");
        legacy.sync();
        check(legacy.value(QStringLiteral("broadcast_layout/version")).toInt() == 4
                  && !legacy.contains(QStringLiteral("broadcast_layout/left_score_panel/x")),
              "Legacy movable plate keys survived V3 save");
    }
    for (const QSize size : {QSize(1024,768), QSize(1280,720), QSize(1920,1080), QSize(2560,1080)}) {
        hud.resize(size); hud.show(); settle();
        hud.updateRobots({red,blue}); settle();
        auto *center = child<QWidget>(hud, "centerHud");
        check(center->property("compositeAssetLoaded").toBool(), "Composite HUD resource missing");
        for (const char *name : {"timer", "redRoundScore", "blueRoundScore"}) {
            check(child<QLabel>(*center,name)->property("angularDigits").toBool(), "Angular numeric face not applied");
            check(child<QLabel>(*center,name)->property("boldDigits").toBool(), "Bold numeric face not applied");
        }
        check(child<QLabel>(*center,"timer")->palette().color(QPalette::WindowText).lightness() > 180,
              "Paused countdown appears occluded/dark");
        check(!hud.layoutElementIds().contains(QStringLiteral("left_score_panel"))
                  && !hud.layoutElementIds().contains(QStringLiteral("right_score_panel")),
              "Composite still exposes separately movable artwork plates");
        const qreal scale = qMin(size.width()/1920.0, size.height()/1080.0);
        const int gap = qRound(18 * scale);
        check(qAbs(center->width() - qRound(600 * scale)) <= 1
                  && qAbs(center->height() - qRound(177 * scale)) <= 1,
              "Composite aspect/size mismatch");
        check(qAbs(center->geometry().left() - panel(hud, QStringLiteral("red"))->geometry().right() - 1 - gap) <= 1
                  && qAbs(panel(hud, QStringLiteral("blue"))->geometry().left() - center->geometry().right() - 1 - gap) <= 1,
              "Side panels do not join the composite HUD width");
        for (const char *name : {"redRoundScore", "blueRoundScore", "matchTitleLayer", "matchTimerLayer"}) {
            auto *text = child<QWidget>(*center, name);
            const QRect bounds(text->mapTo(center, QPoint()), text->size());
            check(center->rect().contains(bounds), QStringLiteral("Composite text clipped: %1 at %2x%3, center=%4x%5 bounds=%6,%7 %8x%9")
                      .arg(name).arg(size.width()).arg(size.height()).arg(center->width()).arg(center->height())
                      .arg(bounds.x()).arg(bounds.y()).arg(bounds.width()).arg(bounds.height()));
        }
        auto *timerText = child<QLabel>(*center,"timer");
        const QPoint timerOrigin = timerText->mapTo(center,QPoint());
        check(qAbs(timerOrigin.x() + timerText->width()/2.0 - center->width()/2.0) <= 1.5
                  && qAbs(timerOrigin.y() + timerText->height()/2.0 - center->height()*94.0/177.0) <= 1.5,
              "Countdown not centred on main artwork face");
        // Resizing the whole artwork moves both text anchors together and
        // refills the side widths; no individual plate can drift away.
        hud.setLayoutElementSize(QStringLiteral("center_hud"), QSize(640, 189)); settle();
        check(qAbs(center->geometry().left() - panel(hud, QStringLiteral("red"))->geometry().right() - 1 - gap) <= 1
                  && qAbs(panel(hud, QStringLiteral("blue"))->geometry().left() - center->geometry().right() - 1 - gap) <= 1,
              "Side join broken after composite resize");
        hud.resetLayoutPositions(); settle();
        check(center->grab().save(outDir+QStringLiteral("/center-hud-%1x%2.png").arg(size.width()).arg(size.height())),
              "Cannot save composite HUD render");
        for (const QString &side : {QStringLiteral("red"),QStringLiteral("blue")}) {
            auto *p = panel(hud, side);
            auto *bar = child<QProgressBar>(*p, "healthBar");
            QImage empty, full;
            for (int hp : {0, 300, 150, 30, 1}) {
                hud.updateRobots({});
                auto r = red, b = blue; r.hp = hp; b.hp = hp;
                hud.updateRobots({r,b}); settle(15);
                check(bar->maximum()==300 && bar->value()==hp, "V2 HP range/value mismatch");
                check(child<QLabel>(*p, "healthText")->text() == QString::number(hp), "HP label mismatch");
                const QImage rendered = bar->grab().toImage();
                if (hp==0) empty=rendered;
                else if (hp==300) full=rendered;
                else {
                    verifyRenderedFill(empty,full,rendered,hp/300.0,side==QStringLiteral("red"));
                }
                const QString name=QStringLiteral("%1-hp%2-%3x%4.png").arg(side).arg(hp).arg(size.width()).arg(size.height());
                check(rendered.save(outDir+QLatin1Char('/')+name), "Cannot save bar render");
            }
            for (int heat : {0,1000,500,100,1}) {
                auto r=red,b=blue; r.heat=heat; b.heat=heat; hud.updateRobots({r,b});
                check(child<QProgressBar>(*p, "heatBar")->value()==heat, "Hidden heat binding failed");
            }
            for (const char *name : {"heatText","lifeState","shootState","deviceDiagnostics","connectionStatus"})
                check(!child<QLabel>(*p,name)->isVisibleTo(p), QStringLiteral("Telemetry leaked into clean HUD: %1").arg(name));
            check(!child<QProgressBar>(*p,"heatBar")->isVisibleTo(p), "Extra heat ribbon visible");
            auto *hp = child<QLabel>(*p,"healthText");
            check(qAbs(bar->height() - qRound(64*scale)) <= 1, "Health ribbon thickness regressed");
            check(bar->geometry().contains(hp->geometry()), "HP number not inset in health ribbon");
            check(hp->fontMetrics().horizontalAdvance(hp->text()) <= hp->width()+3, "Clipped HP number");
            check(qAbs(bar->mapTo(center->parentWidget(),QPoint()).y() - center->geometry().y() - qRound(71*scale)) <= 2,
                  "HP ribbon does not align with countdown height");
            check(p->height() <= qRound(145*scale), "Team header retained tall diagnostic panel");
            check(p->frameShape() == QFrame::NoFrame, "Team header has an enclosing frame");
        }
        hud.updateRobots({red,blue}); hud.awardCard(1,false); hud.awardCard(2,true); settle(800);
        check(hud.grab().save(outDir+QStringLiteral("/hud-%1x%2.png").arg(size.width()).arg(size.height())), "Cannot save HUD render");
        hud.clearCards();
    }
    {
        // Post-match panel: collapsed bar and expanded panel at every size.
        MatchReferee referee;
        referee.beginRound({red, blue}, 0);
        auto r = red, b = blue;
        for (int t = 1; t <= 12; ++t) {
            r.hp = 300 - t * 10; b.hp = 300 - t * 20;
            referee.recordCombatEvent(2, 72, proto::TypeHit, t * 1000);
            referee.updateRobots({r, b}, t * 1000);
            referee.tick(t * 1000);
        }
        referee.recordPenalty(2, false, 12500);
        referee.setScores(2, 1);
        referee.timeExpired(13000);
        check(referee.tick(13000) && referee.verdict().winner == 1, "Synthetic verdict failed");
        hud.updateRobots({r, b});
        hud.concludeRound(referee.verdict().settlementType);
        hud.setMatchSummary(referee.summaryJson());
        auto *summary = child<QWidget>(hud, "matchSummaryPanel");
        QElapsedTimer revealClock; revealClock.start();
        check(hud.settlementPlaying() && !summary->isVisible(), "Summary must wait for the settlement clip");
        waitFor([&]{ return summary->isVisible(); }, "Summary panel not revealed after settlement", 30000);
        qInfo() << "Summary revealed after settlement in" << revealClock.elapsed() << "ms";
        check(panel(hud, QStringLiteral("red"))->findChild<QWidget *>(QStringLiteral("hudStateLayer")) != nullptr,
              "Team state layer missing");
        for (const QSize size : {QSize(1024,768), QSize(1280,720), QSize(1920,1080), QSize(2560,1080)}) {
            hud.resize(size); settle(60);
            const int teamBottom = panel(hud, QStringLiteral("red"))->geometry().bottom();
            const int stateTop = child<QLabel>(hud, "matchState")->mapTo(&hud, QPoint()).y();
            for (bool expanded : {false, true}) {
                hud.setSummaryExpanded(expanded, false); settle(420);
                const QRect g = summary->geometry();
                check(hud.rect().contains(g), QStringLiteral("Summary outside window at %1x%2").arg(size.width()).arg(size.height()));
                check(g.bottom() < stateTop, QStringLiteral("Summary overlaps status row at %1x%2").arg(size.width()).arg(size.height()));
                if (expanded)
                    check(g.top() > teamBottom && g.height() > g.width() / 3,
                          QStringLiteral("Expanded summary overlaps HUD at %1x%2").arg(size.width()).arg(size.height()));
                else
                    check(g.height() < hud.height() / 6, "Collapsed summary too tall");
                check(hud.grab().save(outDir+QStringLiteral("/summary-%1-%2x%3.png")
                                          .arg(expanded ? QStringLiteral("expanded") : QStringLiteral("collapsed"))
                                          .arg(size.width()).arg(size.height())), "Cannot save summary render");
            }
        }
        hud.resetMatch();
        check(!summary->isVisible() && hud.matchSummary().isEmpty(), "Reset did not clear summary");
        hud.resize(1920, 1080); settle();
    }
    auto *p=panel(hud,QStringLiteral("red"));
    red.alive=false; red.shootEnabled=false; red.hp=0; red.powerOn=false; red.linkUp=false;
    hud.updateRobots({red,blue}); settle(800);
    check(child<QLabel>(*p,"lifeState")->text()==QStringLiteral("阵亡"), "Death not displayed");
    check(child<QLabel>(*p,"shootState")->text()==QStringLiteral("射击 禁止"), "Shoot state not displayed");
    check(child<QLabel>(*p,"deviceDiagnostics")->text().contains(QStringLiteral("已断电"))
          && child<QLabel>(*p,"deviceDiagnostics")->text().contains(QStringLiteral("链路断开")), "Diagnostics not displayed");
    red.online=false; hud.updateRobots({red,blue});
    check(child<QLabel>(*p,"connectionStatus")->text()==QStringLiteral("离线"), "Offline not displayed");
    hud.updateRobots({});
    check(child<QLabel>(*p,"healthText")->text()==QStringLiteral("--"), "Disconnected HP not cleared");
    red.online=true;red.alive=true;red.hp=2500;red.protocolVersion=1;hud.updateRobots({red});
    check(child<QProgressBar>(*p,"healthBar")->maximum()==5000, "Legacy HP range changed");
    hud.hide();
    qInfo() << "PASS: offscreen HUD at four resolutions, HP fill pixels and status labels";
}
void verifyPlayerFeedback(const QString &outDir)
{
    MatchServer match;
    check(match.start(0), "Combat feedback TCP server failed");
    for (int team : {1, 2}) {
        qInfo() << "Player feedback: registering team" << team;
        ClientWindow client;
        child<QLineEdit>(client, "clientServerPort")->setText(QString::number(match.port()));
        child<QComboBox>(client, "clientVideoSource")->setCurrentIndex(0);
        child<QComboBox>(client, "clientTeam")->setCurrentIndex(team - 1);
        client.resize(1280, 720); client.show();
        invoke(client, "onLoginClicked");
        auto *overlay = child<QWidget>(client, "playerCombatOverlay");
        waitFor([&]{return overlay->isVisible();}, "Player registration not completed");
        const auto unchanged = [overlay] {
            check(overlay->property("ownDamagePulseCount").toInt() == 1
                      && overlay->property("opponentDamagePulseCount").toInt() == 1,
                  "Player inherited stale HP baseline or retriggered feedback");
        };
        RobotManager::RobotInfo own, other;
        own.team = team; own.robotId = 1; own.hp = 300; own.protocolVersion = 2;
        own.online = own.linkUp = own.alive = true; own.addressText = QStringLiteral("own");
        other = own; other.team = team == 1 ? 2 : 1; other.robotId = 2;
        other.addressText = QStringLiteral("other");
        own.heat = 80; other.heat = 190;
        auto *combat = static_cast<PlayerCombatOverlay *>(overlay);
        const auto publish = [&] { match.publishRobots({own, other}); settle(35); };
        const auto phase = [&](bool running, int seconds = 60) {
            match.publishMatchState({{QStringLiteral("running"), running},
                                    {QStringLiteral("remainingSeconds"), seconds},
                                    {QStringLiteral("ended"), false}});
            settle(35);
        };
        phase(true); publish();
        qInfo() << "Player feedback: initial baseline" << team;
        check(overlay->property("ownDamagePulseCount").toInt() == 0
                  && overlay->property("opponentDamagePulseCount").toInt() == 0,
              "First player snapshot flashed");
        check(combat->heatTelemetryValid() && combat->availableShots() == 12,
              "Player heat did not bind selected team or inherited opponent heat");
        own.heat = 200; publish();
        check(combat->availableShots() == 0, "Player full heat budget incorrect");
        own.heat = 190; publish();
        check(combat->availableShots() == 1, "Player cooling did not recover one shot");
        own.heat = 80; own.shootEnabled = false; publish();
        check(combat->availableShots() == 0, "Player permission denial ignored");
        own.shootEnabled = true; own.heat = -1; publish();
        check(!combat->heatTelemetryValid(), "Unknown player heat remained ready");
        own.heat = 500; publish();
        check(!combat->heatTelemetryValid(), "Out of range player heat treated as ammunition");
        own.heat = 80; publish();
        check(combat->availableShots() == 12, "Valid player heat did not restore");
        other.hp = 280; publish();
        qInfo() << "Player feedback: opponent pulse" << overlay->property("opponentDamagePulseCount");
        check(overlay->property("opponentDamagePulseCount").toInt() == 1
                  && overlay->property("ownDamagePulseCount").toInt() == 0,
              "Opponent HP drop did not trigger only hit feedback");
        check(client.grab().save(outDir+QStringLiteral("/player-team%1-hit.png").arg(team)),
              "Cannot save player hit preview");
        qInfo() << "Player feedback: hit captured" << team;
        qInfo() << "Player feedback: sending own drop";
        own.hp = 280; publish();
        qInfo() << "Player feedback: own drop received";
        waitFor([&]{return overlay->property("ownDamagePulseCount").toInt() == 1;},
                "Own HP drop did not trigger damage feedback");
        unchanged();
        check(client.grab().save(outDir+QStringLiteral("/player-team%1-damage.png").arg(team)),
              "Cannot save player damage preview");
        qInfo() << "Player feedback: damage captured";
        settle(300); publish(); unchanged();
        qInfo() << "Player feedback: duplicate done";
        phase(false); own.hp = 260; other.hp = 260; publish();
        check(!combat->heatTelemetryValid(), "Pause retained live heat");
        phase(true); own.hp = 240; other.hp = 240; publish(); unchanged();
        qInfo() << "Player feedback: resume done";
        own.online = false; publish();
        check(!combat->heatTelemetryValid(), "Offline robot retained heat budget");
        own.online = true; own.hp = 220; other.hp = 220; publish(); unchanged();
        qInfo() << "Player feedback: reconnect done";
        other.hp = -1; publish(); other.hp = 200; publish();
        qInfo() << "Player feedback: unknown done";
        other.protocolVersion = 1; other.hp = 180; publish();
        qInfo() << "Player feedback: protocol done";
        other.addressText = QStringLiteral("replacement"); other.hp = 160; publish();
        qInfo() << "Player feedback: address done";
        own.hp = 300; other.hp = 300; publish(); unchanged();
        qInfo() << "Player feedback: healing done";
        phase(false); publish(); phase(true, 50); publish();
        phase(true, 60); own.hp = 200; other.hp = 200; publish(); unchanged();
        qInfo() << "Player feedback: reset done";
        match.publishMatchState({{QStringLiteral("type"), QStringLiteral("match_state")},
                                 {QStringLiteral("running"), true}, {QStringLiteral("ended"), false},
                                 {QStringLiteral("remainingSeconds"), 0}, {QStringLiteral("suddenDeath"), true}});
        settle(40); publish();
        check(client.suddenDeath() && client.isMatchRunning() && combat->availableShots()==12,
              "SD at zero clock disabled own heat feedback");
        for (const QSize size : {QSize(1280,720), QSize(1920,1080), QSize(3840,2160)}) {
            qInfo() << "Player feedback: resizing" << size;
            client.resize(size); settle(40);
            check(overlay->size() == client.centralWidget()->size(), "Reticle off-center after resize");
            check(child<QWidget>(client,"centerHud")->property("compositeAssetLoaded").toBool(),
                  "Player not using shared HUD artwork");
            check(overlay->testAttribute(Qt::WA_TransparentForMouseEvents), "Feedback blocks controls");
        }
        client.resize(1920,1080); publish(); settle(400);
        check(client.grab().save(outDir+QStringLiteral("/player-team%1-idle.png").arg(team)),
              "Cannot save full-size player reticle");
        invoke(client,"onLogoutClicked"); settle(40);
        check(!overlay->isVisible(), "Logged-out reticle remained visible");
        client.hide();
    }
    match.stop();
    qInfo()<<"PASS: real TCP player feedback for red/blue, pause, reconnect, replacement, healing, reset and resize";
}
void verifyWindowSizing(QMainWindow &window, const QString &name, const QString &outDir,
                        const QList<QSize> &sizes)
{
    for (const QSize &size : sizes) {
        window.resize(size); window.show(); settle(80);
        check(window.size()==size,QStringLiteral("%1 cannot fit %2x%3 (actual %4x%5)")
              .arg(name).arg(size.width()).arg(size.height()).arg(window.width()).arg(window.height()));
        check(window.grab().save(outDir+QStringLiteral("/%1-%2x%3.png").arg(name).arg(size.width()).arg(size.height())),
              "Cannot save window rendering");
    }
    window.hide();
}
}

int main(int argc,char **argv)
{
    std::fputs("Starting offscreen simulator test", stderr); std::fputc(10, stderr); std::fflush(stderr);
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
        std::fputs(message.toUtf8().constData(), stderr); std::fputc(10, stderr); std::fflush(stderr);
    });
    qputenv("QT_QPA_PLATFORM","offscreen");
    QTemporaryDir fontDir;
#ifdef Q_OS_WIN
    qputenv("QT_QPA_FONTDIR",fontDir.path().toUtf8());
#endif
    qunsetenv("SCOMP_AUTO_BROADCAST");
    QApplication app(argc,argv);
#ifdef Q_OS_WIN
    QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR") + QStringLiteral("/Fonts/msyh.ttc"));
    QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR") + QStringLiteral("/Fonts/arial.ttf"));
#endif
    qInfo() << "QApplication ready";
    QTemporaryDir settingsDir;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settingsDir.path());
    QSettings::setPath(QSettings::IniFormat,QSettings::SystemScope,settingsDir.path());
    try {
        check(settingsDir.isValid(),"Cannot isolate test settings");
        QSettings probe(QSettings::defaultFormat(), QSettings::UserScope,
                        QStringLiteral("Scompetition"), QStringLiteral("TestProbe"));
        check(QDir::cleanPath(probe.fileName()).startsWith(QDir::cleanPath(settingsDir.path()) + QLatin1Char('/')),
              "Settings escaped the temporary test directory");
        probe.setValue(QStringLiteral("isolated"), true); probe.sync();
        check(QFileInfo::exists(probe.fileName()), "Isolated settings file was not created");
        check(!QFontDatabase::families().isEmpty(), "No fonts available for HUD rendering");
        app.setFont(QFont(QStringLiteral("Microsoft YaHei"), 9));
        const QString outDir=QCoreApplication::applicationDirPath()+QStringLiteral("/simulator-verification/scale-%1")
            .arg(qEnvironmentVariable("QT_SCALE_FACTOR", "1"));
        check(QDir().mkpath(outDir),"Cannot create test artifacts");
        if (qEnvironmentVariableIsSet("SCOMP_TEST_PLAYER_ONLY")) {
            verifyPlayerFeedback(outDir);
            return 0;
        }
        if (qEnvironmentVariableIsSet("SCOMP_TEST_FAIL_HUD")) {
            BroadcastWindow failureHud; failureHud.show(); settle();
            check(false,"Intentional HUD failure to verify clean exception teardown");
        }
        qInfo() << "Constructing server";
        auto *fieldBackend = new FakeFieldCameraBackend;
        MainWindow server(nullptr, fieldBackend); settle();
        {
            auto *camera = child<FieldCamera>(server,"fieldCameraController");
            auto *devices = child<QComboBox>(server,"fieldCameraDevice");
            check(!camera->requested() && fieldBackend->starts==0, "Server auto-opened camera");
            check(devices->count()==3, "Same-name USB devices collapsed");
            devices->setCurrentIndex(devices->findData(QByteArray("usb-B")));
            invoke(server,"onToggleFieldCamera");
            check(fieldBackend->opened=="usb-B", "Server opened wrong USB device");
            QImage field(640,480,QImage::Format_RGB32); field.fill(QColor("#265c42"));
            fieldBackend->push(field); settle();
            check(camera->live() && !serverBroadcast()->sourceFrame("field").isNull(), "Local field frame not routed");
            check(!child<QLabel>(server,"fieldCameraThumbnail")->pixmap().isNull(), "Local thumbnail missing");
            for (auto *button : server.findChildren<QPushButton *>("fieldCameraTake")) button->click();
            check(child<QComboBox>(server,"programSource")->currentData().toString()==QStringLiteral("field"), "Take field failed");
            check(!child<QLabel>(server,"serverProgramMonitor")->pixmap().isNull(), "Central program monitor missing live camera frame");
            invoke(server,"onResetMatch");
            check(camera->live(), "Match reset stopped independent camera");
            auto *inspector = static_cast<CheckinPanel *>(child<QWidget>(server,"checkinPanel"));
            child<QPushButton>(server,"openCheckin")->click();
            check(inspector->inspectionEnabled() && camera->live(), "Check-in interrupted camera");
            const qint64 stamp = QDateTime::currentMSecsSinceEpoch();
            RobotManager::RobotInfo inspected;
            inspected.robotId=71; inspected.team=1; inspected.protocolVersion=2;
            inspected.online=true; inspected.hp=300; inspected.lastSeenMs=stamp;
            inspected.firstSeenMs=stamp-60000;
            inspected.addressText=QStringLiteral("127.0.0.2");
            inspected.componentOnline=0x3d;
            inspector->observeDeviceHealth(71, 0x3d, inspected.addressText, stamp);
            inspector->updateSnapshot({inspected}, {}, stamp);
            check(inspector->stateModel().rows().size()==1
                      && inspector->stateModel().rows().first().modules[1]==checkin::Health::Fault
                      && inspector->stateModel().rows().first().serverFirstSeenMs==stamp-60000,
                  "Check-in module anomaly not shown");
            inspector->resize(1400,850); inspector->show(); settle();
            check(inspector->grab().save(outDir+QStringLiteral("/server-checkin.png")), "Cannot save check-in UI");
            inspector->setInspectionEnabled(false); inspector->hide();
            auto *sourceChoice = child<QComboBox>(server,"programSource");
            sourceChoice->addItem(QStringLiteral("Test red source"), QStringLiteral("test-red"));
            sourceChoice->setCurrentIndex(sourceChoice->findData(QStringLiteral("test-red")));
            invoke(server,"onAutoSwitchTimeout");
            check(sourceChoice->currentData().toString()==QStringLiteral("field"), "Auto switch skipped live USB camera");
            server.resize(1600,900); server.show(); settle();
            for (auto *area : server.findChildren<QScrollArea *>())
                if (area->widget() && area->widget()->isAncestorOf(child<QWidget>(server,"fieldCameraGroup")))
                    area->ensureWidgetVisible(child<QWidget>(server,"fieldCameraGroup"));
            settle();
            check(server.grab().save(outDir+QStringLiteral("/server-local-usb-camera.png")), "Cannot save camera UI");
            fieldBackend->changeDevices({}); settle();
            check(camera->requested() && !camera->live() && serverBroadcast()->sourceFrame("field").isNull(),
                  "Unplug retained stale field frame");
            sourceChoice->addItem(QStringLiteral("Test red source"), QStringLiteral("test-red"));
            sourceChoice->setCurrentIndex(sourceChoice->findData(QStringLiteral("test-red")));
            invoke(server,"onAutoSwitchTimeout");
            check(sourceChoice->currentData().toString()==QStringLiteral("test-red"), "Auto switch selected offline USB camera");
            invoke(server,"onToggleFieldCamera");
            check(!camera->requested(), "Stop while disconnected failed");
            fieldBackend->changeDevices({{"usb-A",QStringLiteral("USB Camera")},{"usb-B",QStringLiteral("USB Camera")}});
            invoke(server,"refreshFieldCameraDevices");
            check(devices->currentData().toByteArray()=="usb-B", "Refresh lost stable device selection");
            invoke(server,"onToggleFieldCamera"); fieldBackend->push(field);
            invoke(server,"onToggleFieldCamera"); settle();
            check(fieldBackend->opened.isEmpty() && serverBroadcast()->sourceFrame("field").isNull(),
                  "Camera stop failed to release/clear");
            qInfo()<<"PASS: server independent USB field source, device identity, thumbnail, take, match independence and signal loss";
        }
        {
            auto *tabs = child<QTabWidget>(server, "serverControlTabs");
            auto *left = child<QTabWidget>(server,"serverLeftSidebar");
            auto *right = child<QTabWidget>(server,"serverRightSidebar");
            auto *compact = child<QTabWidget>(server,"serverCompactWorkspace");
            check(left->count()==1 && tabs->count()==2 && right->count()==1,"Dashboard tools missing");
            check(server.findChild<QWidget *>(QStringLiteral("serverBottomDock")) == nullptr,
                  "Legacy bottom log dock is still present");
            check(child<QWidget>(server, "serverRightOperationsPage")
                      ->findChild<QWidget *>(QStringLiteral("serverEventLog")) != nullptr,
                  "Event log was not moved into the right sidebar page");
            auto *feedback = child<QLabel>(server, "serverActionStatus");
            for (const QSize size : {QSize(1920,1080), QSize(2560,1440), QSize(1600,900), QSize(1280,720), QSize(1024,768), QSize(800,600), QSize(800,900)}) {
                server.resize(size); server.show(); settle(100);
                check(server.width() <= size.width() && server.height() <= size.height(), QStringLiteral("Dashboard exceeds requested viewport %1x%2 actual %3x%4").arg(size.width()).arg(size.height()).arg(server.width()).arg(server.height()));
                const bool isCompact = size.width()<1500 || size.height()<850;
                check(compact->isVisible()==isCompact,"Wrong dashboard breakpoint");
                if (!isCompact)
                    check(left->isVisible() && tabs->isVisible() && right->isVisible(),"Three dashboard columns not simultaneously visible");
                for (auto *pane : {left,tabs,right}) {
                    if (isCompact) compact->setCurrentWidget(pane);
                    for (int page=0;page<pane->count();++page) {
                        pane->setCurrentIndex(page); settle(40);
                        auto *workspace=pane->currentWidget();
                        const QRect bounds(QPoint(0,0),workspace->size());
                        for (auto *button : workspace->findChildren<QAbstractButton *>()) {
                            if (!button->isVisible() || button->width()==0) continue;
                            const QRect rect(button->mapTo(workspace,QPoint(0,0)),button->size());
                            check(bounds.contains(rect),QStringLiteral("Dashboard clipped %1 in %2 page %3 at %4x%5 rect %6,%7 %8x%9 bounds %10x%11")
                                .arg(button->text(),pane->objectName()).arg(page).arg(size.width()).arg(size.height()).arg(rect.x()).arg(rect.y()).arg(rect.width()).arg(rect.height()).arg(bounds.width()).arg(bounds.height()));
                        }
                    }
                    pane->setCurrentIndex(0);
                }
                if (isCompact) compact->setCurrentWidget(right);
                right->setCurrentIndex(0);
                settle();
                check(feedback->isVisible(),"Header action result hidden");
                check(child<QPlainTextEdit>(server,"serverLog")->isVisible(),"Right sidebar log hidden");
                check(child<QWidget>(server,"serverEventLog")->isVisible(),"Right sidebar log panel hidden");
                if (isCompact) compact->setCurrentWidget(tabs);
                check(server.grab().save(outDir+QStringLiteral("/server-dashboard-%1x%2.png").arg(size.width()).arg(size.height())),"Cannot save dashboard");
            }
            invoke(server,"onForcePowerOn");
            check(feedback->property("state").toString()==QStringLiteral("error"), "Missing endpoint not shown as failure");
            invoke(server,"onAssignTeam");
            check(feedback->property("state").toString()==QStringLiteral("error"), "Assignment validation lacks visible error");
            invoke(server,"onResetMatch");
            check(feedback->property("state").toString()==QStringLiteral("success"), "Match reset lacks visible success");
            tabs->setCurrentIndex(0); server.resize(1280,720); settle();
            qInfo()<<"PASS: full-screen three-column dashboard, compact fallback and all tool pages at seven sizes; right-sidebar log and header result visible";
        }
        qInfo() << "Server ready";
        auto *manager=server.findChild<RobotManager *>();
        auto *commander=server.findChild<RobotCommander *>();
        auto *socket=child<QUdpSocket>(server,"serverUdpSocket");
        auto *table=child<QTableWidget>(server,"robotTable");
        check(manager && commander,"Missing controller components");
        QUdpSocket reserve; check(reserve.bind(QHostAddress::LocalHost,quint16(0)),"No UDP port");
        const quint16 port=reserve.localPort(); reserve.close();
        child<QSpinBox>(server,"serverUdpPort")->setValue(port); invoke(server,"onToggleListen");
        check(socket->state()==QAbstractSocket::BoundState,"Server bind failed");
        {
            auto *feedback=child<QLabel>(server,"serverActionStatus");
            commander->learnEndpoint(250,QHostAddress(QStringLiteral("127.0.0.9")).toIPv4Address(),proto::kDownlinkPort);
            commander->learnEndpoint(251,QHostAddress(QStringLiteral("127.0.0.10")).toIPv4Address(),proto::kDownlinkPort);
            const auto lastTx=[] {
                QSettings settings(QSettings::defaultFormat(),QSettings::UserScope,QStringLiteral("Scompetition"),QStringLiteral("RobotCommands"));
                return settings.value(QStringLiteral("lastTxid")).toUInt();
            };
            child<QSpinBox>(server,"deviceRobotId")->setValue(250);
            invoke(server,"onForcePowerOn"); const quint32 first=lastTx();
            check(feedback->property("state").toString()==QStringLiteral("pending"),"Sent command not pending");
            child<QSpinBox>(server,"deviceRobotId")->setValue(251);
            invoke(server,"onForcePowerOn"); const quint32 second=lastTx();
            commander->onAck(proto::TypeForcePowerOn,first,0,250);
            check(feedback->property("state").toString()==QStringLiteral("pending"),"Partial ACK marked batch success");
            waitFor([&]{return commander->pendingCount()==0;},"Command timeout missing");
            check(feedback->property("state").toString()==QStringLiteral("error") && feedback->text().contains(QStringLiteral("超时")),"Timeout not visibly reported");
            invoke(server,"onForcePowerOn"); const quint32 rejected=lastTx();
            commander->onAck(proto::TypeForcePowerOn,rejected,2,251);
            check(feedback->property("state").toString()==QStringLiteral("error"),"Rejected command shown as success");
            invoke(server,"onForcePowerOn"); const quint32 successful=lastTx();
            commander->onAck(proto::TypeForcePowerOn,successful,0,251);
            check(feedback->property("state").toString()==QStringLiteral("success"),"ACK success not reported");
            invoke(server,"onRequestStatus");
            check(commander->pendingCount()==0 && feedback->property("state").toString()==QStringLiteral("info"),"No-ACK status request incorrectly failed or queued");
            Q_UNUSED(second);
            commander->forgetEndpoint(250); commander->forgetEndpoint(251);
            child<QSpinBox>(server,"deviceRobotId")->setValue(1);
            qInfo()<<"PASS: command pending, partial ACK, timeout, rejection, success and no-ACK request feedback";
        }
        SimulatorWindow red,blue;
        configureSimulator(red,QStringLiteral("127.0.0.2"),port);
        configureSimulator(blue,QStringLiteral("127.0.0.3"),port);
        waitFor([&]{return manager->devices().size()==2;},"DEVICE_ANNOUNCE discovery failed");
        check(manager->robots().isEmpty(),"Unassigned simulator reported robot 0");
        auto *controllerEdit=child<QLineEdit>(server,"deviceControllerMac");
        controllerEdit->setText(QStringLiteral("8D:23:AB:A5:3C:C9"));
        child<QSpinBox>(server,"deviceRobotId")->setValue(71);
        invoke(red,"sendDeviceAnnounce");invoke(blue,"sendDeviceAnnounce");settle();
        check(controllerEdit->text()==QStringLiteral("8D:23:AB:A5:3C:C9")
              && child<QSpinBox>(server,"deviceRobotId")->value()==71,"Announce overwrote assignment form");
        assign(server,red,71,1);
        check(manager->teamForRobot(71)==0,"Team changed before ACK");
        check(child<QLabel>(server,"serverActionStatus")->property("state").toString()==QStringLiteral("pending"), "Assignment was marked successful before ACK");
        waitFor([&]{return manager->teamForRobot(71)==1 && rowFor(*table,71)>=0;},"Red assignment failed");
        assign(server,blue,72,2);
        waitFor([&]{return manager->teamForRobot(72)==2 && rowFor(*table,72)>=0;},"Blue assignment failed");
        check(child<QSpinBox>(red,"simRobotId")->value()==71 && child<QSpinBox>(blue,"simRobotId")->value()==72,"Assignment applied wrong ID");
        waitFor([&]{return commander->pendingCount()==0;},"Assignment ACK did not close transaction");
        qInfo()<<"PASS: two-device discovery, form editing, assignment ACK and red/blue mapping";
        {
            server.resize(1920,1080); settle();
            auto *compact=child<QTabWidget>(server,"serverCompactWorkspace");
            auto *left=child<QTabWidget>(server,"serverLeftSidebar");
            auto *center=child<QTabWidget>(server,"serverControlTabs");
            auto *right=child<QTabWidget>(server,"serverRightSidebar");
            if (compact->isVisible()) compact->setCurrentWidget(center);
            left->setCurrentIndex(0); center->setCurrentIndex(0); right->setCurrentIndex(0);
            child<QComboBox>(server,"fieldCameraDevice")->setCurrentIndex(child<QComboBox>(server,"fieldCameraDevice")->findData(QByteArray("usb-B")));
            invoke(server,"onToggleFieldCamera");
            QImage liveFrame(1280,720,QImage::Format_RGB32); liveFrame.fill(QColor("#265c42"));
            fieldBackend->push(liveFrame);
            for (auto *button : server.findChildren<QPushButton *>("fieldCameraTake")) button->click();
            table->setCurrentCell(rowFor(*table,71),0); table->selectRow(rowFor(*table,71));
            settle();
            check(server.grab().save(outDir+QStringLiteral("/server-dashboard-live-1920x1080.png")),"Cannot save populated dashboard");
            left->setCurrentIndex(0); settle();
            check(server.grab().save(outDir+QStringLiteral("/server-dashboard-teams-1920x1080.png")),"Cannot save team sidebar");
            invoke(server,"onToggleFieldCamera");
        }

        QHash<quint32,int> acks;
        QObject ackObserver;
        QObject::connect(manager,&RobotManager::ackReceived,&ackObserver,[&](quint8,quint32 tx,quint8 result,quint8){acks.insert(tx,result);});
        const auto send=[&](const QByteArray &data,quint32 tx,int expected=0){
            check(socket->writeDatagram(data,QHostAddress(QStringLiteral("127.0.0.2")),proto::kDownlinkPort)==data.size(),"UDP send failed");
            if(expected>=0)waitFor([&]{return acks.contains(tx);},QStringLiteral("Missing ACK %1").arg(tx));else settle(100);
            if(expected>=0)check(acks.value(tx)==expected,"Wrong ACK result");else check(!acks.contains(tx),"Foreign/malformed frame was ACKed");
        };
        const QByteArray controller=QByteArray::fromHex("8d23aba53cc9");
        check(child<QSpinBox>(red,"simHp")->value()==200,"Preparation HP must be 200");
        send(proto::makeSetHp(71,201,9991),9991,2);
        send(proto::makeYellowCard(71,9992),9992,1);
        send(proto::makeAssignment(99,deviceMac(blue),controller,10001),10001,-1);
        send(proto::makeAssignment(99,deviceMac(red),controller,10002).left(15),10002,-1);
        send(proto::makeAssignment(0,deviceMac(red),controller,10003),10003,2);
        send(proto::makeAssignment(99,deviceMac(red),QByteArray(6,char(0)),10004),10004,2);
        send(proto::makeGameStart(72,10005),10005,-1);
        send(proto::makeGameStart(0,10006),10006,-1);
        check(child<QSpinBox>(red,"simRobotId")->value()==71,"Rejected assignment changed identity");
        send(proto::makeGameStart(71,10007),10007);
        send(proto::makeYellowCard(71,10008),10008);
        check(child<QSpinBox>(red,"simHp")->value()==250,"First yellow card wrong HP");
        acks.remove(10008);send(proto::makeYellowCard(71,10008),10008);
        check(child<QSpinBox>(red,"simHp")->value()==250,"Retry applied yellow card twice");
        send(proto::makeYellowCard(71,10009),10009);send(proto::makeYellowCard(71,10010),10010);
        waitFor([&]{return table->item(rowFor(*table,71),2)->text()==QStringLiteral("0");},"Third yellow card not fatal");
        settle(500);
        check(!child<QPlainTextEdit>(red,"simLog")->toPlainText().contains(QStringLiteral("未获服务器 ACK")),"Reliable event ACK not received on uplink");
        invoke(red,"onSendRevive");check(child<QSpinBox>(red,"simHp")->value()==0,"Disqualified robot revived");
        send(proto::makeSetHp(71,300,9993),9993,1);
        send(proto::makeGameStart(71,10011),10011);
        send(proto::makeSetHp(71,301,10012),10012,2);
        for(int hp:{300,150,30,0}) {
            send(proto::makeSetHp(71,quint16(hp),10020+hp),10020+hp);
            waitFor([&]{return table->item(rowFor(*table,71),2)->text().toInt()==hp;},"UI HP not updated");
        }
        send(proto::makeGameStart(71,11000),11000);
        manager->handleDatagram(proto::makeEventV2(proto::TypeCombatEnd,71),QHostAddress("127.0.0.2"),proto::kDownlinkPort);
        check(table->item(rowFor(*table,71),6)->text()==QStringLiteral("允许"),"COMBAT_END changed shooting permission");
        check(child<QCheckBox>(red,"simShoot")->isChecked(),"GAME_START did not enable shooting");
        invoke(red,"onSendHit");check(child<QSpinBox>(red,"simHp")->value()==280,"Hit damage must be 20 HP");
        send(proto::makeSetHp(71,0,10997),10997);send(proto::makeSetHp(71,300,10998),10998);
        check(child<QCheckBox>(red,"simAlive")->isChecked() && child<QCheckBox>(red,"simShoot")->isChecked(),"Positive SET_HP did not restore alive/shoot state");
        send(proto::makeForcePowerOff(71,11001),11001);
        waitFor([&]{return table->item(rowFor(*table,71),7)->text()==QStringLiteral("已断电");},"Power off not displayed");
        send(proto::makeForcePowerOn(71,11002),11002);
        invoke(red,"onSendLinkDown");settle(300);
        check(table->item(rowFor(*table,71),8)->text()==QStringLiteral("断开"),"Health heartbeat overwrote link down");
        invoke(red,"onSendLinkUp");invoke(red,"onSendShootDisabled");
        waitFor([&]{return table->item(rowFor(*table,71),6)->text()==QStringLiteral("禁止");},"Shoot prohibition not displayed");
        child<QSpinBox>(red,"simHeat")->setValue(500);child<QSpinBox>(red,"simPower")->setValue(45);
        waitFor([&]{return table->item(rowFor(*table,71),3)->text()==QStringLiteral("500");},"Heat not displayed");
        qInfo()<<"PASS: rejection, target filtering, deduplication, yellow cards, HP, power, link and shoot state";

        auto *match=server.findChild<MatchServer *>();check(match && match->start(0),"TCP server failed");
        ClientWindow client;
        child<QLineEdit>(client,"clientServerPort")->setText(QString::number(match->port()));
        child<QComboBox>(client,"clientVideoSource")->setCurrentIndex(0);
        invoke(client,"onLoginClicked");
        auto *cp=panel(client,QStringLiteral("red"));
        waitFor([&]{return child<QLabel>(*cp,"healthText")->text()==QStringLiteral("300");},"Client TCP snapshot missing");
        check(child<QProgressBar>(*cp,"healthBar")->maximum()==300,"Client lost V2 HP range");
        waitFor([&]{return child<QLabel>(*cp,"deviceDiagnostics")->text().contains(QStringLiteral("45 W"));},"Client lost power diagnostics");
        invoke(client,"onLogoutClicked");match->stop();
        verifyWindowSizing(client,QStringLiteral("client"),outDir,{QSize(1024,768),QSize(1600,900)});
        qInfo()<<"PASS: production TCP client receives V2 status and diagnostic fields";

        assign(server,red,72,1); settle();
        check(child<QSpinBox>(red,"simRobotId")->value()==71,"Duplicate live robot ID was assigned");
        assign(server,red,74,1);
        waitFor([&]{return manager->teamForRobot(74)==1 && rowFor(*table,74)>=0;},"Reassignment failed");
        check(manager->teamForRobot(71)==0 && rowFor(*table,71)<0,"Reassignment left old team/robot");
        check(commander->sendGameStart(71)==0,"Old command endpoint survived reassignment");
        assign(server,red,71,1);
        waitFor([&]{return manager->teamForRobot(71)==1 && rowFor(*table,71)>=0;},"Reassignment back failed");
        check(rowFor(*table,74)<0,"Old robot row remained visible");
        {
            quint32 firstTx=0,secondTx=0;
            { RobotCommander first(socket); firstTx=first.sendAssignment(QHostAddress("127.0.0.2").toIPv4Address(),75,deviceMac(red),controller); }
            waitFor([&]{return child<QSpinBox>(red,"simRobotId")->value()==75;},"First commander assignment failed");
            { RobotCommander restarted(socket); secondTx=restarted.sendAssignment(QHostAddress("127.0.0.2").toIPv4Address(),71,deviceMac(red),controller); }
            check(firstTx!=0 && secondTx!=0 && firstTx!=secondTx,"Restart reused transaction ID");
            waitFor([&]{return child<QSpinBox>(red,"simRobotId")->value()==71;},"Restarted commander assignment was treated as a duplicate");
            assign(server,red,71,1);
            waitFor([&]{return manager->teamForRobot(71)==1;},"Restoring team mapping failed");
        }
        qInfo()<<"PASS: ID conflicts, old-ID cleanup and command IDs across server restarts";

        {
            // Full automatic-verdict path: real UDP simulators, GAME_START /
            // GAME_END, knockout confirmation, auto score and TCP summary sync.
            auto *bw = serverBroadcast();
            child<QSpinBox>(server,"ruleOfflineGrace")->setValue(10);
            invoke(server,"onResetMatch");
            check(match->start(0),"TCP server restart failed");
            child<QLineEdit>(client,"clientServerPort")->setText(QString::number(match->port()));
            invoke(client,"onLoginClicked");
            waitFor([&]{return rowFor(*table,71)>=0 && rowFor(*table,72)>=0
                               && table->item(rowFor(*table,71),5)->text()!=QStringLiteral("离线")
                               && table->item(rowFor(*table,72),5)->text()!=QStringLiteral("离线");},
                    "Robots not online before match");
            invoke(server,"onToggleMatch");
            check(bw->isMatchRunning(),"Match did not start");
            waitFor([&]{return child<QSpinBox>(red,"simHp")->value()==300
                               && child<QSpinBox>(blue,"simHp")->value()==300;},"GAME_START not applied");
            waitFor([&]{return client.isMatchRunning();}, "Client not running before combat preview");
            settle(250); // Establish player HP baselines before the first drop.
            invoke(blue,"onSendHit");
            waitFor([&]{return table->item(rowFor(*table,72),2)->text()==QStringLiteral("280");},"Hit not reported");
            client.resize(1920,1080); client.show(); settle(30);
            auto *playerOverlay = child<QWidget>(client, "playerCombatOverlay");
            check(playerOverlay->isVisible() && playerOverlay->testAttribute(Qt::WA_TransparentForMouseEvents),
                  "Registered player overlay missing or blocking input");
            check(playerOverlay->size() == client.centralWidget()->size(),
                  "Player reticle remained at previous window size after resize");
            check(child<QWidget>(client, "centerHud")->property("compositeAssetLoaded").toBool(),
                  "Player client not using shared composite HUD");
            check(client.grab().save(outDir+QStringLiteral("/client-playing-1920x1080.png")),
                  "Cannot save registered player UI");
            invoke(red,"onSendDeath");
            waitFor([&]{return bw->blueScore()==1;},"Kill point not applied");
            settle(400);
            check(!bw->roundEnded() && bw->isMatchRunning(), "Regulation kill ended match");
            waitFor([&]{return client.blueScore()==1;}, "Kill point not mirrored to client");
            invoke(server,"onTimeExpired");
            waitFor([&]{return bw->roundEnded();},"Score timeout produced no verdict");
            const QJsonObject summary=bw->matchSummary();
            check(bw->settlementType()==QStringLiteral("bluewin"),"Wrong knockout settlement");
            check(summary.value(QStringLiteral("winner")).toInt()==2
                  && summary.value(QStringLiteral("reason")).toString()==QStringLiteral("kill_score"),"Wrong summary verdict");
            check(summary.value(QStringLiteral("red")).toObject().value(QStringLiteral("damageDealt")).toInt()==20
                  && summary.value(QStringLiteral("blue")).toObject().value(QStringLiteral("damageDealt")).toInt()==300,
                  "Wrong damage statistics");
            check(bw->blueScore()==1 && bw->redScore()==0,"Auto score not applied");
            waitFor([&]{return !child<QCheckBox>(blue,"simShoot")->isChecked();},"GAME_END not sent after verdict");
            waitFor([&]{return client.roundEnded() && client.settlementType()==QStringLiteral("defeated")
                               && client.matchSummary().value(QStringLiteral("winner")).toInt()==2;},
                    "Client did not receive verdict summary");
            check(child<QPushButton>(server,"summaryToggle")->isEnabled(),"Summary toggle disabled");
            invoke(server,"onToggleSummary");
            check(bw->summaryExpanded(),"Summary did not expand");
            waitFor([&]{return client.summaryExpanded();},"Client did not mirror summary expansion");

            // Next round keeps the series score; pause/resume must not resend
            // GAME_START (it would heal the robots mid-round).
            invoke(server,"onToggleMatch");
            check(bw->isMatchRunning() && !bw->roundEnded() && bw->blueScore()==0 && bw->redScore()==0,"Next round retained kill score");
            check(bw->matchSummary().isEmpty(),"Previous summary survived next round");
            waitFor([&]{return child<QSpinBox>(red,"simHp")->value()==300;},"Second GAME_START not applied");
            invoke(blue,"onSendHit");
            waitFor([&]{return table->item(rowFor(*table,72),2)->text()==QStringLiteral("280");},"Second hit not reported");
            invoke(server,"onToggleMatch");check(!bw->isMatchRunning(),"Pause failed");
            invoke(server,"onToggleMatch");settle(300);
            check(bw->isMatchRunning() && child<QSpinBox>(blue,"simHp")->value()==280,"Resume re-sent GAME_START");
            invoke(server,"onTerminateMatch");
            check(bw->roundEnded() && bw->settlementType()==QStringLiteral("termination")
                  && bw->blueScore()==0 && bw->redScore()==0,"Termination changed score or settlement");
            // Score tie enters SD without restart commands, healing or end.
            invoke(server,"onToggleMatch");
            waitFor([&]{return child<QSpinBox>(red,"simHp")->value()==300
                               && child<QSpinBox>(blue,"simHp")->value()==300;}, "SD round start failed");
            invoke(blue,"onSendHit");
            waitFor([&]{return table->item(rowFor(*table,72),2)->text()==QStringLiteral("280");}, "SD pre-hit failed");
            bw->setMatchState(0, false);
            invoke(server,"onTimeExpired");
            waitFor([&]{return bw->suddenDeath() && bw->isMatchRunning()
                               && client.suddenDeath() && client.isMatchRunning();}, "SD not synced to client");
            check(!bw->roundEnded() && bw->remainingSeconds()==0
                      && child<QSpinBox>(blue,"simHp")->value()==280
                      && child<QCheckBox>(blue,"simShoot")->isChecked(), "SD healed/stopped robot");
            child<QSpinBox>(red,"simHeat")->setValue(80); settle(200);
            // This console scenario registers robot 1 but simulates 71/72;
            // identity-specific combat feedback is checked in the player test.
            check(client.grab().save(outDir+QStringLiteral("/client-sudden-death-1920x1080.png")), "Cannot save SD HUD");
            invoke(server,"onToggleMatch");
            check(!bw->isMatchRunning() && bw->suddenDeath(), "SD pause reset phase");
            invoke(server,"onToggleMatch"); settle(200);
            check(bw->isMatchRunning() && bw->suddenDeath() && bw->remainingSeconds()==0
                      && child<QSpinBox>(blue,"simHp")->value()==280, "SD resume resent GAME_START");
            invoke(red,"onSendDeath");
            waitFor([&]{return bw->roundEnded() && client.roundEnded();}, "SD kill did not finish");
            check(bw->blueScore()==1 && bw->redScore()==0
                      && bw->matchSummary().value(QStringLiteral("reason")).toString()==QStringLiteral("sudden_death_kill"),
                  "SD verdict/score wrong");
            invoke(client,"onLogoutClicked");match->stop();
            invoke(server,"onResetMatch");
            check(bw->redScore()==0 && bw->blueScore()==0 && !bw->roundEnded(),"Reset failed");
        }
        qInfo()<<"PASS: kill points, score timeout, sudden death, GAME_END, summary sync, resume without GAME_START";

        const QByteArray savedMac=deviceMac(red);invoke(red,"onToggleRunning");
        waitFor([&]{return table->item(rowFor(*table,71),5)->text()==QStringLiteral("离线");},"Offline status did not expire");
        check(table->item(rowFor(*table,71),7)->text()==QStringLiteral("-"),"Offline power incorrectly reported live");
        SimulatorWindow restored;configureSimulator(restored,QStringLiteral("127.0.0.2"),port);
        check(deviceMac(restored)==savedMac && child<QSpinBox>(restored,"simRobotId")->value()==71,"Assignment not persisted");
        { SimulatorWindow conflict;child<QLineEdit>(conflict,"simLocalAddress")->setText(QStringLiteral("127.0.0.2"));invoke(conflict,"onToggleRunning");
          check(child<QLineEdit>(conflict,"simLocalAddress")->isEnabled(),"Bind failure reported running"); }
        invoke(restored,"onToggleRunning");invoke(blue,"onToggleRunning");
        SimulatorWindow v1;child<QComboBox>(v1,"simProtocolVersion")->setCurrentIndex(0);
        child<QComboBox>(v1,"simTeam")->setCurrentIndex(2);child<QSpinBox>(v1,"simRobotId")->setValue(73);
        configureSimulator(v1,QStringLiteral("127.0.0.4"),port);
        child<QSpinBox>(v1,"simHeat")->setValue(200);settle(150);child<QSpinBox>(v1,"simHeat")->setValue(0);settle(150);
        check(rowFor(*table,73)>=0 && table->item(rowFor(*table,73),3)->text()==QStringLiteral("0"),"V1 heat did not reset to zero");
        invoke(v1,"onToggleRunning");invoke(server,"onToggleListen");
        assign(server,red,76,1);
        check(socket->state()==QAbstractSocket::UnconnectedState && commander->pendingCount()==0,"Assignment while stopped rebound the server socket");
        QSettings saved(QSettings::defaultFormat(),QSettings::UserScope,
                        QStringLiteral("Scompetition"),QStringLiteral("RobotSimulator"));
        check(QDir::cleanPath(saved.fileName()).startsWith(QDir::cleanPath(settingsDir.path())+QLatin1Char('/'))
              && QFileInfo::exists(saved.fileName()),"Simulator settings were not isolated on disk");
        qInfo()<<"PASS: persistent identity, port conflict, offline status and V1 compatibility";
        verifyWindowSizing(server,QStringLiteral("server"),outDir,{QSize(1024,768),QSize(1600,900)});
        verifyWindowSizing(red,QStringLiteral("simulator"),outDir,{QSize(640,480),QSize(1024,768)});
        verifyHudRendering(outDir);
        verifyPlayerFeedback(outDir);
        qInfo()<<"SimulatorIntegrationTest passed; artifacts:"<<outDir;
        return 0;
    } catch(const std::exception &error) { qCritical()<<"FAIL:"<<error.what();return 1; }
}
