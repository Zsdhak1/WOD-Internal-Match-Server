#include "playercombatoverlay.h"
#include <QApplication>
#include <QDir>
#include <QImage>
#include <QThread>
#include <cstdio>
#include <stdexcept>

static void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    try {
        playercombat::HpTracker tracker;
        playercombat::HpSample sample;
        sample.present = sample.online = sample.linkUp = true;
        sample.team = 1; sample.robotId = 71; sample.protocol = 2; sample.hp = 300;
        sample.address = "127.0.0.2";
        check(tracker.observe(sample) == 0, "First sample triggered damage");
        sample.hp = 280; check(tracker.observe(sample) == 20, "HP delta not detected");
        check(tracker.observe(sample) == 0, "Duplicate snapshot triggered damage");
        sample.hp = 300; check(tracker.observe(sample) == 0, "Healing triggered damage");
        sample.online = false; sample.hp = 100; check(tracker.observe(sample) == 0, "Offline sample triggered damage");
        sample.online = true; sample.hp = 80; check(tracker.observe(sample) == 0, "Reconnect triggered damage");
        sample.robotId = 72; sample.hp = 50; check(tracker.observe(sample) == 0, "New robot triggered damage");
        sample.protocol = 1; sample.hp = 40; check(tracker.observe(sample) == 0, "Protocol change triggered damage");
        sample.address = "127.0.0.3"; sample.hp = 30; check(tracker.observe(sample) == 0, "Address change triggered damage");
        sample.linkUp = false; check(tracker.observe(sample) == 0, "Broken link triggered damage");
        sample.linkUp = true; sample.hp = 20; check(tracker.observe(sample) == 0, "Link restoration triggered damage");
        sample.hp = -1; check(tracker.observe(sample) == 0, "Unknown HP triggered damage");
        sample.hp = 10; check(tracker.observe(sample) == 0, "Known HP restoration triggered damage");
        sample.hp = 0; check(tracker.observe(sample) == 10, "Fatal hit not detected");
        tracker.reset(); check(tracker.observe(sample) == 0, "Round reset did not clear baseline");

        playercombat::HeatTracker heat;
        check(!heat.valid(0) && heat.availableShots(0) == -1, "Unknown heat became ready");
        heat.observe(0, true, 0);
        check(heat.availableShots(0) == 20 && heat.coolingProgress(0) == 1, "Cold heat budget incorrect");
        heat.observe(80, true, 100);
        check(heat.availableShots(100) == 12 && heat.coolingProgress(600) == 0.5, "Heat/cooling calculation wrong");
        heat.observe(80, true, 500);
        check(heat.coolingProgress(600) == 0.5, "Duplicate heat reset cooling phase");
        heat.observe(90, true, 600);
        check(heat.availableShots(600) == 11 && heat.coolingProgress(600) == 0.5, "Shot reset cooling phase");
        heat.observe(80, true, 1100);
        check(heat.availableShots(1100) == 12 && heat.coolingProgress(1100) == 0, "Cooling telemetry did not resync");
        heat.observe(200, true, 1200);
        check(heat.availableShots(1200) == 0, "Overheat became ready");
        heat.observe(190, true, 1300);
        check(heat.availableShots(1300) == 1, "Exact heat limit shot budget wrong");
        heat.observe(191, true, 1400);
        check(heat.availableShots(1400) == 0, "Partial heat budget rounded up");
        heat.observe(80, false, 1500);
        check(heat.availableShots(1500) == 0, "Permission denial ignored");
        heat.observe(80, true, 1600);
        check(heat.coolingProgress(2700) == 1 && heat.availableShots(2700) == 12,
              "Clock invented additional cooling/ammo");
        check(!heat.valid(3101) && heat.availableShots(3101) == -1, "Stale heat remained live");
        heat.observe(80, true, 3200);
        check(heat.coolingProgress(3200) == 0, "Reconnect inherited old phase");
        for (int invalid : {-1, 201, 1000}) {
            heat.observe(invalid, true, 3300);
            check(!heat.valid(3300), "Out of range heat accepted");
        }
        for (int value = 0; value <= 200; ++value) {
            heat.observe(value, true, 4000 + value);
            check(heat.availableShots(4000 + value) == (200-value)/10, "Heat boundary budget wrong");
        }
        heat.reset(); check(!heat.valid(4300), "Reset retained heat");

        QWidget host;
        host.resize(1280, 720);
        host.setStyleSheet(QStringLiteral("background: #101416;"));
        PlayerCombatOverlay overlay(&host);
        overlay.setStyleSheet(QStringLiteral("background: transparent;"));
        overlay.setGeometry(host.rect()); overlay.setHudScale(1);
        host.show(); overlay.show(); app.processEvents();
        check(overlay.testAttribute(Qt::WA_TransparentForMouseEvents), "Overlay blocks input");
        const QString out = QCoreApplication::applicationDirPath()+QStringLiteral("/player-combat-verification");
        check(QDir().mkpath(out), "Cannot create preview directory");
        const QImage idle = host.grab().toImage();
        check(idle.save(out+QStringLiteral("/crosshair.png")), "Cannot save reticle preview");
        overlay.pulseOpponentDamage();
        const QImage hit = host.grab().toImage();
        check(hit != idle, "Opponent hit effect not drawn");
        check(hit.save(out+QStringLiteral("/hit.png")), "Cannot save hit preview");
        overlay.clearFeedback(); overlay.pulseOwnDamage();
        const QImage damage = host.grab().toImage();
        check(damage.pixelColor(1, 100).red() > idle.pixelColor(1, 100).red(), "Damage edge not drawn");
        check(damage.pixelColor(400, 300) == idle.pixelColor(400, 300), "Damage effect covers central view");
        check(damage.save(out+QStringLiteral("/damage.png")), "Cannot save damage preview");
        overlay.clearFeedback();
        check(host.grab().toImage() == idle, "Clearing feedback left visual residue");
        for (int value : {0, 80, 180, 200}) {
            overlay.setHeatTelemetry(value, true);
            check(overlay.availableShots() == (200-value)/10, "Overlay heat not bound");
            check(host.grab().save(out+QStringLiteral("/heat-%1.png").arg(value)), "Cannot save heat arc preview");
        }
        overlay.clearFeedback(); overlay.setHeatTelemetry(80, true);
        QElapsedTimer previewClock; previewClock.start();
        while (previewClock.elapsed() < 500) { app.processEvents(); QThread::msleep(5); }
        overlay.setHeatTelemetry(80, true); // Refresh without resetting phase.
        check(host.grab().save(out+QStringLiteral("/heat-80-cooling-half.png")), "Cannot save cooling half-cycle");
        overlay.pulseOpponentDamage();
        check(host.grab().save(out+QStringLiteral("/heat-80-hit.png")), "Cannot save hit plus arcs");
        overlay.setHeatTelemetry(80, false);
        check(overlay.availableShots() == 0, "Overlay permission ignored");
        check(host.grab().save(out+QStringLiteral("/heat-disabled.png")), "Cannot save blocked arcs");
        overlay.clearFeedback();
        check(host.grab().toImage() == idle, "Reset retained heat arcs");
        std::puts("PASS: HP baselines, heat budget/phase/boundaries/staleness, reticle, hit and damage rendering");
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1;
    }
}
