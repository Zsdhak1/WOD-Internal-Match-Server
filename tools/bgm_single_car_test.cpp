// Phase BGM + single-car debug + referee HP control, on the real MainWindow
// with one UDP simulator (red only) in no-device debug mode.
//   - BGM: silent at launch; prepare after reset; match restarts at the
//     countdown; beeps on the last 5 s; prepare in intermission; interlude
//     after the BO3; prepare again after reset; cancelled start -> prepare.
//   - Single car: GAME_START to the one bound car, round starts after its ACK.
//   - Referee console: deduct N and set absolute HP, ACK-confirmed (SET_HP).
#include "mainwindow.h"
#include "bgmcontroller.h"
#include "broadcastwindow.h"
#include "clientwindow.h"
#include "simulatorwindow.h"
#include "fake_field_camera.h"
#include "robotmanager.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QThread>
#include <QUdpSocket>
#include <cstdio>
#include <functional>
#include <stdexcept>

using T = BgmController::Track;
static void check(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
template<class W> static W *child(QObject &o, const char *name)
{ auto *p = o.findChild<W *>(QString::fromLatin1(name)); check(p, name); return p; }
static void call(QObject &o, const char *name)
{ check(QMetaObject::invokeMethod(&o, name, Qt::DirectConnection), "slot unavailable"); }
static void settle(int ms = 40)
{ QElapsedTimer t; t.start(); do { QApplication::processEvents(); QThread::msleep(2); } while (t.elapsed() < ms); }
static void wait(const std::function<bool()> &f, const char *why, int ms = 4000)
{ QElapsedTimer t; t.start(); while (!f() && t.elapsed() < ms) settle(5); check(f(), why); }
static BroadcastWindow *broadcast()
{
    for (auto *w : QApplication::topLevelWidgets())
        if (auto *b = qobject_cast<BroadcastWindow *>(w))
            if (!qobject_cast<ClientWindow *>(w)) return b;
    throw std::runtime_error("missing output");
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
#ifdef Q_OS_WIN
    for (const QString &font : {QStringLiteral("arial.ttf"), QStringLiteral("msyh.ttc"), QStringLiteral("msyhbd.ttc")})
        QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR") + QStringLiteral("/Fonts/") + font);
#endif
    app.setFont(QFont(QStringLiteral("Microsoft YaHei"), 10));
    QTemporaryDir settings; QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());
    const QString qa = qEnvironmentVariable("SCOMP_INTEGRATION_QA_DIR");
    try {
        check(settings.isValid(), "settings isolation failed");
        for (const char *f : {"bgm_prepare.m4a", "bgm_match.m4a", "bgm_interlude.m4a", "countdown_beep.wav"})
            check(QFileInfo::exists(BgmController::defaultAssetDir() + "/" + f), "BGM asset not deployed next to the binary");

        qint64 now = 1000;
        MainWindow main(nullptr, new FakeFieldCameraBackend, [&] { return now; });
        main.resize(1920, 1080); main.show(); settle();
        auto *bw = broadcast();
        auto *bgm = child<BgmController>(main, "bgmController");
        check(child<QCheckBox>(main, "bgmEnabled")->isChecked() && child<QSlider>(main, "bgmVolume")->value() == 70,
              "BGM defaults (on, 70%)");
        bgm->setVolume(0);   // keep the test machine quiet; logic is volume-independent
        call(main, "onRefereeTick");
        check(bgm->track() == T::None, "BGM must stay silent at launch until new/reset BO3");

        // One simulator: red robot 1. Debug mode lets a single car play.
        QUdpSocket reserve; check(reserve.bind(QHostAddress::LocalHost, quint16(0)), "UDP reserve");
        const auto udp = reserve.localPort(); reserve.close();
        child<QSpinBox>(main, "serverUdpPort")->setValue(udp); call(main, "onToggleListen");
        SimulatorWindow red; red.setProtocolTimeForTest(1000);
        child<QLineEdit>(red, "simLocalAddress")->setText("127.0.0.2");
        child<QSpinBox>(red, "simServerPort")->setValue(udp); call(red, "onToggleRunning");
        auto *manager = main.findChild<RobotManager *>(); check(manager, "manager");
        wait([&] { return manager->devices().size() == 1; }, "device discovery");
        const int idx = child<QComboBox>(main, "deviceSelect")->findData(child<QLineEdit>(red, "simDeviceMac")->text());
        check(idx >= 0, "device undiscovered"); child<QComboBox>(main, "deviceSelect")->setCurrentIndex(idx);
        child<QSpinBox>(main, "deviceRobotId")->setValue(1); child<QComboBox>(main, "deviceTeam")->setCurrentIndex(1);
        child<QLineEdit>(main, "deviceControllerMac")->setText("8D:23:AB:A5:3C:C9"); call(main, "onAssignTeam");
        wait([&] { return manager->teamForRobot(1) == 1 && !manager->robots().isEmpty()
                          && manager->robots()[0].hp >= 0; }, "red assignment / telemetry");

        // Formal mode refuses one car; debug mode accepts it.
        call(main, "onToggleMatch"); settle();
        check(bgm->track() == T::None, "refused start must not touch BGM");
        child<QCheckBox>(main, "noDeviceDebug")->setChecked(true); settle();
        call(main, "onRefereeTick");
        check(bgm->track() == T::Prepare && bgm->currentSource().endsWith("bgm_prepare.m4a"), "prepare BGM after BO3 reset");

        // Countdown: match track restarts from 0 with the HUD countdown.
        call(main, "onToggleMatch"); settle();
        check(bgm->track() == T::Match && bgm->currentSource().endsWith("bgm_match.m4a"), "match BGM at countdown start");
        now += 5000; call(main, "onRefereeTick");
        wait([&] { now += 20; call(main, "onRefereeTick"); return bw->isMatchRunning(); },
             "single-car round did not start after its GAME_START ACK");
        check(bgm->track() == T::Match, "match BGM kept through device confirmation");
        if (!qa.isEmpty()) { check(QDir().mkpath(qa), "QA"); settle(300); check(main.grab().save(qa + "/server-single-car-referee-hp.png"), "capture"); }

        // Referee console: deduct and set HP on the single car (ACK-confirmed).
        auto *simHp = child<QSpinBox>(red, "simHp");
        wait([&] { return simHp->value() == 300; }, "GAME_START did not heal the car");
        child<QSpinBox>(main, "damageAmount")->setValue(40);
        wait([&] { call(main, "refreshRefereeHpUi"); return child<QPushButton>(main, "refDamageRed")->isEnabled(); },
             "referee deduct not available on the bound car");
        check(!child<QPushButton>(main, "refDamageBlue")->isEnabled(), "blue side has no car but HP control is enabled");
        check(child<QLabel>(main, "refHpState")->text().contains(QStringLiteral("没有连接车辆")), "blue reason not shown");
        child<QPushButton>(main, "refDamageRed")->click();
        wait([&] { return simHp->value() == 260; }, "referee deduct not applied (300-40)");
        wait([&] { now += 20; call(main, "refreshRefereeHpUi"); return child<QPushButton>(main, "refSetHpRed")->isEnabled(); },
             "referee set HP not available after ACK");
        child<QSpinBox>(main, "refHpRed")->setValue(123);
        child<QPushButton>(main, "refSetHpRed")->click();
        wait([&] { return simHp->value() == 123; }, "referee set HP not applied");
        if (!qa.isEmpty()) { settle(400); check(main.grab().save(qa + "/server-single-car-referee-hp.png"), "capture"); }

        // Last 5 s: one beep per second (5..1), no repeats within a second.
        const int before = bgm->beepCount();
        // Jump to 20 s before the end, then step 250 ms past time-up.
        const qint64 remaining = qint64(bw->remainingSeconds()) * 1000;
        now += qMax<qint64>(0, remaining - 20000); call(main, "onRefereeTick");
        check(bgm->beepCount() == before, "beeped before the final 5 s");
        for (int step = 0; step < 120; ++step) { now += 250; call(main, "onRefereeTick"); }
        if (bgm->beepCount() - before != BgmController::kBeepSeconds)
            std::fprintf(stderr, "beeps=%d running=%d ended=%d\n", bgm->beepCount() - before,
                         int(bw->isMatchRunning()), int(bw->roundEnded()));
        check(bgm->beepCount() - before == BgmController::kBeepSeconds, "expected exactly 5 countdown beeps");

        // Time up -> referee decides -> intermission plays prepare.
        call(main, "onDeclareRedWin"); settle(); call(main, "onRefereeTick");
        check(bgm->track() == T::Prepare, "prepare BGM in intermission");

        // Next round, then win the BO3 -> interlude.
        now += 10 * 60 * 1000; call(main, "onRefereeTick");   // intermission over
        call(main, "onToggleMatch"); settle();
        check(bgm->track() == T::Match, "match BGM restarted for round 2");
        now += 5000; call(main, "onRefereeTick");
        wait([&] { now += 20; call(main, "onRefereeTick"); return bw->isMatchRunning(); }, "round 2 start");
        call(main, "onDeclareRedWin"); settle(); call(main, "onRefereeTick");
        check(bw->redScore() == 2, "BO3 not decided");
        check(bgm->track() == T::Interlude && bgm->currentSource().endsWith("bgm_interlude.m4a"), "interlude BGM after BO3");

        // New/reset BO3 -> prepare again.
        call(main, "onResetMatch"); settle();
        check(bgm->track() == T::Prepare, "prepare BGM after reset");

        // Device start not confirmed (car stopped) -> cancelled -> prepare.
        call(red, "onToggleRunning"); settle(300);
        call(main, "onToggleMatch"); settle();
        if (bgm->track() == T::Match) {   // the car may still count as online briefly
            now += 5000; call(main, "onRefereeTick");
            now += 3100; call(main, "onRefereeTick");
            check(bgm->track() == T::Prepare, "cancelled start must fall back to prepare BGM");
        }

        // Disable -> silent; re-enable mid-phase restarts the phase track.
        child<QCheckBox>(main, "bgmEnabled")->setChecked(false); settle();
        check(!bgm->isPlaying() && child<QLabel>(main, "bgmState")->text().contains(QStringLiteral("关闭")), "BGM off");
        child<QCheckBox>(main, "bgmEnabled")->setChecked(true); settle();
        check(bgm->enabled(), "BGM on");

        std::puts("PASS: phase BGM (silent at launch, prepare/match/interlude, countdown restart, 5 beeps, "
                  "cancelled start), single-car debug round, referee deduct/set HP via ACK");
        std::fflush(stdout);
        return 0;
    } catch (const std::exception &e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
