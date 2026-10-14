// Real Qt widget screenshots with synthetic video/telemetry. No referee flow,
// hardware, camera, TCP service, or local competition timer is needed here.
#include "broadcastwindow.h"
#include "competitionoverlay.h"
#include "playercombatoverlay.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFontDatabase>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLinearGradient>
#include <QPainter>
#include <QThread>
#include <QTimer>
#include <cstdio>
#include <stdexcept>

namespace {
void check(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}
void settle(int milliseconds = 30)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}
QLabel *label(BroadcastWindow &window, const char *name)
{
    auto *result = window.findChild<QLabel *>(QString::fromLatin1(name));
    check(result != nullptr, name);
    return result;
}
QJsonObject snapshot(const QString &phase, int seconds, int round = 1)
{
    return {
        {"scoringRule", "remaining_resources"}, {"phase", phase},
        {"remainingSeconds", seconds}, {"roundRemainingSeconds", phase == "running" ? seconds : 180},
        {"roundIndex", round}, {"matchId", "synthetic-bo3"},
        {"roundId", QStringLiteral("qa-round-%1").arg(round)}, {"stateRevision", 1},
        {"redScore", 0}, {"blueScore", 0},
        {"redRemainingRevives", 3}, {"blueRemainingRevives", 3},
        {"redRevivePending", false}, {"blueRevivePending", false},
        {"notice", ""}, {"ended", false}, {"summary", QJsonObject()}, {"settlement", ""}
    };
}
QJsonObject summary(const QString &reason, int winner = 1)
{
    return {{"valid", true}, {"winner", winner}, {"reasonText", reason}};
}
// Full referee-style summary whose evidence (resources/HP/series) agrees with
// the reason text. Synthetic authoritative data, never a UI-side verdict.
QJsonObject verdictSummary(const QString &reason, const QString &method, int winner,
                           int redRevives, int blueRevives, int redHp, int blueHp,
                           int round, int redWins, int blueWins, qint64 durationMs)
{
    const auto side = [](int revives, int hp) {
        return QJsonObject{{"remainingRevives", revives}, {"quotaKnown", true},
                           {"revivePending", false}, {"hp", hp}, {"maxHp", 300}};
    };
    return {{"valid", true}, {"winner", winner}, {"reasonText", reason},
            {"endingMethod", method}, {"roundIndex", round},
            {"redSeriesWins", redWins}, {"blueSeriesWins", blueWins},
            {"durationMs", double(durationMs)},
            {"red", side(redRevives, redHp)}, {"blue", side(blueRevives, blueHp)}};
}
QImage syntheticFrame()
{
    QImage frame(1920, 1080, QImage::Format_RGB32);
    QPainter painter(&frame);
    painter.setRenderHint(QPainter::Antialiasing);
    QLinearGradient sky(0, 0, 0, 1080);
    sky.setColorAt(0, QColor("#0f2436"));
    sky.setColorAt(0.48, QColor("#25475a"));
    sky.setColorAt(1, QColor("#13242c"));
    painter.fillRect(frame.rect(), sky);
    painter.fillRect(QRect(100, 220, 1720, 240), QColor("#1c3442"));
    painter.setPen(QPen(QColor("#537383"), 3));
    painter.drawLine(100, 460, 1820, 460);
    for (int x = -1000; x <= 3000; x += 250)
        painter.drawLine(QPointF(960 + (x - 960) * 0.10, 460), QPointF(x, 1080));
    for (int y : {500, 550, 620, 720, 870, 1050})
        painter.drawLine(0, y, 1920, y);
    painter.setPen(QPen(QColor("#879daa"), 6));
    painter.drawRect(110, 245, 1700, 790);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor("#a94b56"));
    painter.drawRoundedRect(QRectF(515, 620, 135, 84), 10, 10);
    painter.fillRect(QRect(552, 590, 60, 35), QColor("#d16a73"));
    painter.setBrush(QColor("#427aae"));
    painter.drawRoundedRect(QRectF(1250, 560, 120, 75), 10, 10);
    painter.fillRect(QRect(1283, 535, 53, 29), QColor("#78a4cf"));
    painter.setPen(QColor("#9fb2bd"));
    QFont font = painter.font();
    font.setPixelSize(20);
    painter.setFont(font);
    painter.drawText(QRect(130, 265, 1300, 40), "SYNTHETIC ARENA FRAME / REAL QT WIDGET QA");
    return frame;
}
} // namespace

int main(int argc, char **argv)
{
    // Always a real offscreen QApplication; the broadcast raster fallback is
    // used because QOpenGLWidget is unsupported by Qt's offscreen platform.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    const QString fonts = qEnvironmentVariable("SystemRoot") + QStringLiteral("/Fonts/");
    QFontDatabase::addApplicationFont(fonts + QStringLiteral("msyh.ttc"));
    QFontDatabase::addApplicationFont(fonts + QStringLiteral("msyhbd.ttc"));
    QFontDatabase::addApplicationFont(fonts + QStringLiteral("segoeui.ttf"));
    app.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 10));
    try {
        check(QThread::currentThread() == app.thread(), "Widget QA must run on the GUI main thread");
        const QString output = qEnvironmentVariable("SCOMP_COMPETITION_QA_DIR");
        if (!output.isEmpty())
            check(QDir().mkpath(output), "Cannot create competition screenshot directory");
        QJsonArray screenshots;
        BroadcastWindow window;
        check(window.remainingSeconds() == 60, "Legacy clock no longer defaults to 60 seconds");
        window.setMatchState(180, false);
        check(window.remainingSeconds() == 60, "Default legacy clock cap changed");
        window.setClockDuration(180);
        window.setMatchState(180, false);
        check(window.remainingSeconds() == 180, "Instance clock duration still capped at 60 seconds");
        window.resetMatch();
        check(window.remainingSeconds() == 180, "Reset did not preserve instance clock duration");
        window.setClockDuration(60);
        window.setTeamNames(QStringLiteral("红方 · 赤焰"), QStringLiteral("蓝方 · 深蓝"));
        window.setProgramFrame(syntheticFrame());
        window.setActiveSource(QStringLiteral("field"), QStringLiteral("合成场地 · QA"));
        window.setTickerVisible(false, false);

        // Reuse the approved playeroverlay2x implementation without editing it.
        // This proves that formal running HUD leaves the actual player reticle
        // visible, not just an empty centre of a broadcast-only screenshot.
        QWidget *interaction = window.interactionLayer();
        interaction->setAttribute(Qt::WA_TransparentForMouseEvents);
        PlayerCombatOverlay combat(interaction);
        combat.setStyleSheet(QStringLiteral("background: transparent;"));
        interaction->show();
        combat.show();
        RobotManager::RobotInfo red, blue;
        red.team = 1; blue.team = 2;
        red.robotId = 71; blue.robotId = 72;
        red.protocolVersion = blue.protocolVersion = 2;
        red.online = blue.online = red.linkUp = blue.linkUp = true;
        red.hp = 235; blue.hp = 180;
        red.heat = 30; blue.heat = 80;
        red.addressText = blue.addressText = QStringLiteral("127.0.0.1");
        QVector<RobotManager::RobotInfo> robots{red, blue};
        qint64 revision = 0;
        int localExpiries = 0, localVerdicts = 0;
        QObject::connect(&window, &BroadcastWindow::timeExpired, &window, [&] { ++localExpiries; });
        QObject::connect(&window, &BroadcastWindow::roundFinished, &window,
                         [&](const QString &) { ++localVerdicts; });
        QString passMatchId = QStringLiteral("synthetic-bo3");
        const auto apply = [&](QJsonObject state) {
            state.insert(QStringLiteral("stateRevision"), ++revision);
            state.insert(QStringLiteral("matchId"), passMatchId); // fresh launch key per resolution pass
            window.setCompetitionState(state);
            window.updateRobots(robots);
            settle();
        };
        const auto capture = [&](const QString &scenario, const QSize &size) {
            check(window.size() == size, "HUD expanded beyond requested resolution");
            auto *background = label(window, "videoBackground");
            check(background->text().isEmpty(), "No-video placeholder covers synthetic field");
            check(!background->pixmap().isNull(), "Synthetic video frame is not rendered");
            if (!output.isEmpty()) {
                const QString name = QStringLiteral("competition-%1-%2x%3.png")
                                         .arg(scenario).arg(size.width()).arg(size.height());
                const QImage image = window.grab().toImage();
                check(image.size() == size, "Screenshot resolution does not match widget size");
                check(image.save(QDir(output).filePath(name)), "Cannot save competition screenshot");
                screenshots.append(name);
            }
        };
        for (const QSize size : {QSize(1920, 1080), QSize(1280, 720)}) {
            window.resize(size);
            window.show();
            settle(60);
            combat.setGeometry(interaction->rect());
            combat.setHudScale(qMin(size.width() / 1920.0, size.height() / 1080.0));
            passMatchId = QStringLiteral("synthetic-bo3-%1").arg(size.width());
            // Neither class declares Q_OBJECT, so findChild<T*> (qobject_cast)
            // cannot target them directly: find by name, then dynamic_cast.
            auto *overlay = dynamic_cast<CompetitionOverlay *>(
                window.findChild<QWidget *>(QStringLiteral("competitionOverlay")));
            check(overlay != nullptr, "CompetitionOverlay is not part of the broadcast HUD");
            auto *timer = label(window, "timer");
            auto *redRevives = dynamic_cast<ReviveIndicator *>(label(window, "redReviveCount"));
            auto *blueRevives = dynamic_cast<ReviveIndicator *>(label(window, "blueReviveCount"));
            check(redRevives && blueRevives, "Revive counts are not ReviveIndicator icons");
            // Deterministic animation time for every screenshot.
            overlay->setManualClockForTest(true);
            redRevives->setManualClockForTest(true);
            blueRevives->setManualClockForTest(true);
            const auto step = [&](int milliseconds) {
                overlay->advanceForTest(milliseconds);
                redRevives->advanceForTest(milliseconds);
                blueRevives->advanceForTest(milliseconds);
                settle(10);
            };
            const qreal hudScale = qMin(size.width() / 1920.0, size.height() / 1080.0);
            const auto px = [hudScale](int v) { return qRound(v * hudScale); };
            // Player reticle zone (centre 400x400 reference px of the 16:9 HUD).
            const QRect reticle(size.width() / 2 - px(200), size.height() / 2 - px(200), px(400), px(400));
            const auto captureTop = [&](const QString &scenario) {
                if (output.isEmpty())
                    return;
                const QString name = QStringLiteral("competition-revive-%1-%2x%3.png")
                                         .arg(scenario).arg(size.width()).arg(size.height());
                const QImage image = window.grab(QRect(0, 0, size.width(), px(260))).toImage();
                check(image.save(QDir(output).filePath(name)), "Cannot save revive screenshot");
                screenshots.append(name);
            };
            const auto revives = [&](QJsonObject state, int red, int blue,
                                     bool redPending = false, bool bluePending = false) {
                state.insert("redRemainingRevives", red < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(red));
                state.insert("blueRemainingRevives", blue < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(blue));
                state.insert("redRevivePending", redPending);
                state.insert("blueRevivePending", bluePending);
                return state;
            };

            apply(snapshot("ready", 180));
            check(!window.isMatchRunning() && overlay->currentMode() == CompetitionOverlay::Mode::Hidden,
                  "Ready phase runs or obscures centre");
            // The previous resolution's run ended in "cancelled"; let its
            // ~320 ms cross-fade finish so the ready frame shows the settled HUD.
            step(400);
            check(overlay->occupiedRect().isEmpty(), "Ready phase still shows a previous card");
            capture("ready", size);
            for (int seconds = 5; seconds >= 1; --seconds) {
                apply(snapshot("countdown", seconds));
                check(overlay->currentMode() == CompetitionOverlay::Mode::Countdown
                          && overlay->countdownDigit() == seconds,
                      "Countdown does not display authoritative 5-to-1 digit");
                check(!window.isMatchRunning() && window.remainingSeconds() == seconds,
                      "Countdown starts a combat clock");
                const int before = overlay->transitionCount();
                apply(snapshot("countdown", seconds));
                check(overlay->transitionCount() == before, "Repeated countdown snapshot restarted the digit");
                step(500); // settle the digit entrance
                capture(QStringLiteral("countdown-%1").arg(seconds), size);
            }
            apply(snapshot("countdown", 0));
            check(overlay->currentMode() == CompetitionOverlay::Mode::Launch && overlay->launchFlashActive(),
                  "Countdown zero does not launch");
            step(240);
            capture("launch", size);
            step(1000);
            apply(snapshot("starting", 0));
            check(overlay->currentMode() == CompetitionOverlay::Mode::Starting
                      && !overlay->statusText().isEmpty(), "Starting lacks device confirmation");
            step(400);
            capture("starting", size);
            auto running = snapshot("running", 180);
            running = revives(running, 2, 1);
            apply(running);
            step(500);
            check(window.isMatchRunning() && timer->text() == "03:00", "Formal running clock is capped at 60 seconds");
            check(overlay->currentMode() == CompetitionOverlay::Mode::Hidden && !window.summaryVisible(),
                  "Formal running overlays centre");
            check(!overlay->occupiedRect().intersects(reticle), "Running overlay intersects the reticle area");
            check(redRevives->isVisible() && blueRevives->isVisible(), "Resource icons are hidden");
            check(redRevives->text().contains("2 / 3") && blueRevives->text().contains("1 / 3")
                      && redRevives->remainingRevives() == 2 && blueRevives->remainingRevives() == 1,
                  "Resource icons do not display authoritative counts");
            check(redRevives->y() > window.findChild<QLabel *>("healthText")->y(), "Resource row is not below HP");
            capture("running-180", size);
            running.insert("remainingSeconds", 137);
            running.insert("roundRemainingSeconds", 137);
            running = revives(running, -1, 1, true, false);
            apply(running);
            check(timer->text() == "02:17", "Server seconds snapshot is not displayed");
            check(!redRevives->known() && redRevives->pending()
                      && redRevives->text().contains(QStringLiteral("未知"))
                      && redRevives->text().contains(QStringLiteral("确认中")), "Unknown/pending revives are misleading");
            capture("running-pending", size);
            if (size.width() == 1920) {
                settle(1150);
                check(window.remainingSeconds() == 137 && window.isMatchRunning(), "Formal clock advanced locally");
                window.startMatch(); window.pauseMatch(); window.terminateMatch(); window.concludeRound("redwin");
                check(window.remainingSeconds() == 137 && window.isMatchRunning() && !window.roundEnded(),
                      "Local legacy controls advanced/settled the formal match");
            }
            for (int count = 0; count <= 3; ++count) {
                apply(revives(snapshot("running", 137), count, 3 - count));
                check(redRevives->text().contains(QStringLiteral("%1 / 3").arg(count))
                          && blueRevives->text().contains(QStringLiteral("%1 / 3").arg(3 - count))
                          && redRevives->remainingRevives() == count && redRevives->known(),
                      "A resource boundary 0..3 is displayed incorrectly");
            }
            apply(revives(snapshot("running", 137), 4, -1, false, true));
            check(!redRevives->known() && !blueRevives->known() && blueRevives->pending()
                      && redRevives->text().contains(QStringLiteral("未知"))
                      && blueRevives->text().contains(QStringLiteral("未知"))
                      && blueRevives->text().contains(QStringLiteral("确认中")), "Invalid resources displayed as known counts");

            // Revive icon gallery (HUD top strip). Unknown -> known snaps.
            const auto settled = [&](int red, int blue, bool redPending, const QString &name) {
                apply(revives(snapshot("running", 120), red, blue, redPending));
                step(1200); // finish any break/light animation; pending stays at phase 0
                captureTop(name);
            };
            settled(3, 3, false, "3of3");
            check(!redRevives->animating(), "Idle revive icons keep animating");
            // Consume one: 3 -> 2 shows a break mid-frame.
            apply(revives(snapshot("running", 120), 2, 3));
            check(redRevives->animating(), "Revive consumption does not animate");
            step(180);
            captureTop("break-midframe");
            step(600);
            check(!redRevives->animating(), "Break animation never ends");
            captureTop("2of3");
            // Pending approval: the reserved core breathes (half a cycle = brightest).
            apply(revives(snapshot("running", 120), 1, 3, true));
            check(redRevives->pending() && redRevives->animating(), "Pending revive does not breathe");
            step(600);
            captureTop("pending");
            settled(1, 3, false, "1of3");
            settled(0, 3, false, "0of3");
            check(redRevives->remainingRevives() == 0 && redRevives->known(), "Exhausted revives not shown as 0");
            settled(-1, -1, false, "unknown");
            check(!redRevives->known() && !blueRevives->known(), "Unknown revives shown as a count");
            apply(revives(snapshot("running", 120), 0, 0));
            step(800);
            apply(revives(snapshot("running", 120), 3, 3)); // e.g. next-round refill
            step(180); // first core lit, second lighting, third waiting
            captureTop("refill-midframe");
            step(1000);
            check(!redRevives->animating() && redRevives->remainingRevives() == 3, "Refill did not finish");

            apply(snapshot("running", 0));
            settle(50);
            check(window.isMatchRunning() && !window.roundEnded(), "Widget inferred expiry/verdict at server zero");
            capture("running-zero-await-server", size);
            apply(snapshot("awaiting_decision", 0));
            step(500);
            check(!window.isMatchRunning()
                      && overlay->currentMode() == CompetitionOverlay::Mode::AwaitingDecision,
                  "Awaiting decision not presented");
            capture("awaiting-decision", size);
            struct Verdict {
                const char *name; QString reason; const char *method; int winner;
                int redRev, blueRev, redHp, blueHp, redWins, blueWins; qint64 ms;
            };
            // Each reason agrees with its resource/HP evidence.
            const Verdict verdicts[] = {
                {"time-revives", QStringLiteral("时间耗尽，按剩余复活次数判定：红方2次，蓝方1次，红方获胜"),
                 "time_decision", 1, 2, 1, 235, 180, 1, 0, 180000},
                {"time-hp", QStringLiteral("时间耗尽，剩余复活次数相同（各1次），按血量判定：红方 HP 235 > 蓝方 HP 180，红方获胜"),
                 "time_decision", 1, 1, 1, 235, 180, 1, 0, 180000},
                {"revive-exhausted", QStringLiteral("蓝方复活次数耗尽且血量归零，红方获胜"),
                 "revive_exhausted", 1, 2, 0, 235, 0, 1, 0, 89000},
                {"violation", QStringLiteral("裁判判罚：蓝方违规，判红方获胜"),
                 "violation", 1, 2, 2, 235, 180, 1, 0, 105000},
                {"no-winner", QStringLiteral("安全停止：场地异常，本局终止，无胜方"),
                 "safety_stop", 0, 2, 2, 235, 180, 0, 0, 64000},
            };
            QJsonObject lastResult;
            for (const Verdict &v : verdicts) {
                auto result = revives(snapshot("intermission", 120), v.redRev, v.blueRev);
                const QJsonObject sum = verdictSummary(v.reason, QString::fromLatin1(v.method), v.winner,
                                                       v.redRev, v.blueRev, v.redHp, v.blueHp,
                                                       1, v.redWins, v.blueWins, v.ms);
                result.insert("ended", true);
                result.insert("redScore", v.redWins);
                result.insert("blueScore", v.blueWins);
                result.insert("summary", sum);
                result.insert("settlement", "qa-no-clip");
                // Separate round ids keep each card's 7 s hold independent.
                result.insert("roundId", QStringLiteral("qa-verdict-%1").arg(QString::fromLatin1(v.name)));
                result.insert("roundRemainingSeconds", int(qMax<qint64>(0, 180 - v.ms / 1000)));
                robots[0].hp = v.redHp;
                robots[1].hp = v.blueHp;
                robots[0].alive = robots[0].shootEnabled = true;
                robots[1].alive = robots[1].shootEnabled = v.blueHp > 0;
                apply(result);
                check(redRevives->remainingRevives() == v.redRev && blueRevives->remainingRevives() == v.blueRev
                          && redRevives->text().contains(QStringLiteral("%1 / 3").arg(v.redRev)),
                      "Victory resource evidence does not match its fixture");
                check(redRevives->parentWidget()->findChild<QLabel *>("healthText")->text() == QString::number(v.redHp)
                          && blueRevives->parentWidget()->findChild<QLabel *>("healthText")->text()
                                 == QString::number(v.blueHp),
                      "Victory HP evidence does not match its fixture");
                // The main clock freezes on the finished round; the 120 s
                // preparation is shown by the overlay bar (checked below).
                const int frozen = int(qMax<qint64>(0, 180 - v.ms / 1000));
                check(timer->text() == QStringLiteral("%1:%2").arg(frozen / 60, 2, 10, QLatin1Char('0'))
                                           .arg(frozen % 60, 2, 10, QLatin1Char('0')),
                      "Main clock is not frozen on the finished round during intermission");
                check(!window.summaryVisible(), "Formal flow automatically revealed the legacy statistics panel");
                // Settlement clip playing: the verdict card must not cover it.
                // (QA has no clip/FFmpeg, so the playing state is simulated on
                // the overlay exactly as playSettlement() would set it.)
                overlay->setSettlementBlocking(true);
                step(200);
                check(!overlay->resultVisible() && overlay->isHidden() && overlay->occupiedRect().isEmpty(),
                      "Verdict card covers the settlement clip");
                if (v.winner == 1 && QByteArray(v.name) == "time-revives")
                    capture("settlement-playing", size);
                overlay->setSettlementBlocking(false); // clip ended
                check(overlay->resultVisible() && overlay->currentMode() == CompetitionOverlay::Mode::Result,
                      "Verdict card not revealed after the clip");
                check(overlay->reasonText().contains(v.reason), "Victory reason is not shown");
                check(v.winner ? overlay->headlineText().contains(v.winner == 1 ? QStringLiteral("红方")
                                                                                : QStringLiteral("蓝方"))
                               : !overlay->headlineText().contains(QStringLiteral("获胜")),
                      "Verdict headline disagrees with the winner");
                if (QByteArray(v.name) == "time-revives") {
                    step(650);
                    capture("result-scoreflip-midframe", size);
                }
                step(1600);
                // Repeated periodic ticks: no restarted clip, summary or animation.
                window.stopSettlement();
                const int before = overlay->transitionCount();
                for (int tick = 0; tick < 4; ++tick)
                    apply(result);
                check(overlay->transitionCount() == before && overlay->resultVisible(),
                      "Repeated identical snapshots restarted a transition");
                check(!window.settlementPlaying() && window.matchSummary() == sum,
                      "Repeated snapshots restarted settlement or cleared summary");
                capture(QStringLiteral("result-%1").arg(QString::fromLatin1(v.name)), size);
                if (QByteArray(v.name) == "time-revives")
                    lastResult = result; // a decided round leads into intermission
            }
            // Card hold ends -> compact intermission bar.
            apply(lastResult);
            step(7200);
            check(overlay->currentMode() == CompetitionOverlay::Mode::Intermission
                      && overlay->intermissionClockText() == "02:00", "Intermission bar missing after result hold");
            // The main clock freezes on the finished round (180 s used -> 00:00);
            // the 120 s preparation countdown lives only in the intermission bar.
            check(window.remainingSeconds() == 0,
                  "Main HUD clock shows the preparation countdown instead of the finished round");
            // The result card cross-fades out for ~320 ms after the 7 s hold;
            // only the settled intermission bar must clear the reticle.
            step(400);
            check(!overlay->occupiedRect().intersects(reticle), "Intermission bar covers the reticle");
            capture("intermission-0200", size);
            auto preparation = lastResult;
            preparation.insert("remainingSeconds", 0);
            preparation.insert("summary", QJsonObject());
            // Empty periodic summary must not erase the completed same-round verdict.
            apply(preparation);
            step(600);
            check(!window.matchSummary().isEmpty(), "Same-round tick erased summary");
            check(overlay->intermissionClockText() == "00:00"
                      && overlay->statusText().contains(QStringLiteral("等待裁判")), "Preparation expiry lacks referee wait");
            capture("intermission-0000", size);
            auto next = revives(snapshot("countdown", 5, 2), 3, 3);
            next.insert("redScore", 1);
            apply(next);
            step(500);
            check(!overlay->resultVisible() && window.matchSummary().isEmpty()
                      && overlay->currentMode() == CompetitionOverlay::Mode::Countdown,
                  "New round inherited previous verdict");
            check(label(window, "matchTitle")->text().contains(QStringLiteral("第 2 小局")), "BO3 round index is absent");
            capture("round-2-countdown", size);
            // BO3 final 2:1 (round 3 decided by remaining revives at time-up).
            auto finished = revives(snapshot("match_finished", 0, 3), 2, 1);
            finished.insert("ended", true);
            finished.insert("redScore", 2);
            finished.insert("blueScore", 1);
            finished.insert("summary", verdictSummary(
                QStringLiteral("时间耗尽，按剩余复活次数判定：红方2次，蓝方1次，红方获胜；红方 2:1 赢得 BO3"),
                QStringLiteral("time_decision"), 1, 2, 1, 235, 180, 3, 2, 1, 180000));
            apply(finished);
            step(7200 + 2000); // result card hold, then the BO3 final sweep
            check(window.redScore() == 2 && window.blueScore() == 1
                      && overlay->currentMode() == CompetitionOverlay::Mode::MatchFinished,
                  "BO3 final result is wrong");
            capture("match-finished-2-1", size);
            auto cancelled = revives(snapshot("cancelled", 0, 2), 2, 2);
            cancelled.insert("roundId", QStringLiteral("qa-cancelled"));
            cancelled.insert("ended", true);
            cancelled.insert("summary", summary(QStringLiteral("裁判取消比赛"), 0));
            apply(cancelled);
            step(7200 + 600);
            check(overlay->currentMode() == CompetitionOverlay::Mode::Cancelled, "Cancelled state not presented");
            check(overlay->headlineText() == QStringLiteral("比赛已取消"),
                  "Cancelled card shows contradictory wording");
            capture("cancelled", size);
        }
        check(localExpiries == 0 && localVerdicts == 0, "Formal widget emitted a local expiry or verdict");
        // Old kills/sudden-death/debug summary APIs remain available.
        window.setClockDuration(60);
        window.setMatchState(120, false);
        check(window.remainingSeconds() == 60 && !label(window, "redReviveCount")->isVisible(), "Legacy mode did not reset formal HUD");
        {
            auto *overlay = dynamic_cast<CompetitionOverlay *>(
                window.findChild<QWidget *>(QStringLiteral("competitionOverlay")));
            check(overlay && !overlay->isVisible() && overlay->currentMode() == CompetitionOverlay::Mode::Hidden
                      && overlay->occupiedRect().isEmpty() && !label(window, "blueReviveCount")->isVisible(),
                  "Legacy mode keeps the formal overlay or revive icons");
        }
        window.setMatchState(0, true, true);
        check(window.isMatchRunning() && window.suddenDeath(), "Legacy sudden-death compatibility lost");
        window.setMatchState(0, false);
        window.playSettlement("qa-no-clip");
        window.setMatchSummary(summary(QStringLiteral("旧模式赛后总结")));
        settle(450);
        check(window.summaryVisible(), "Legacy summary API no longer reveals its panel");
        window.setSummaryExpanded(true, false);
        check(window.summaryExpanded(), "Legacy expanded summary API no longer works");
        window.stopSettlement();
        window.resetMatch();
        if (!output.isEmpty()) {
            const QJsonObject manifest{
                {"syntheticData", true}, {"renderer", "real Qt offscreen widgets / raster synthetic video"},
                {"sourceCommit", qEnvironmentVariable("SCOMP_COMPETITION_QA_COMMIT", "working-tree")},
                {"capturedUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                {"screenshots", screenshots}, {"thread", "QApplication main thread"}
            };
            QFile file(QDir(output).filePath(QStringLiteral("competition-ui-manifest.json")));
            check(file.open(QIODevice::WriteOnly), "Cannot write screenshot manifest");
            check(file.write(QJsonDocument(manifest).toJson()) > 0, "Cannot serialize screenshot manifest");
        }
        std::puts("PASS: CompetitionOverlay + ReviveIndicator, settlement never covered, formal snapshots, 5-to-0 countdown, 180-second round, 120-second preparation, BO3, resources/pending, persistent reasons, no local expiry/replay, legacy summary; real widgets at 1920x1080 and 1280x720.");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
