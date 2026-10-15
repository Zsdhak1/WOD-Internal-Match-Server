// No-device debug mode: the formal BO3 flow can be exercised on the server
// without any robot, while the default (formal) mode still refuses to start.
#include "mainwindow.h"
#include "broadcastwindow.h"
#include "clientwindow.h"
#include "fake_field_camera.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
#include <stdexcept>

static void check(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
template<class T> static T *child(QObject &o, const char *name)
{ auto *p = o.findChild<T *>(QString::fromLatin1(name)); check(p, "missing UI object"); return p; }
static void call(QObject &o, const char *name)
{ check(QMetaObject::invokeMethod(&o, name, Qt::DirectConnection), "slot unavailable"); }
static void settle(int ms = 40)
{ QElapsedTimer t; t.start(); do { QApplication::processEvents(); QThread::msleep(2); } while (t.elapsed() < ms); }
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
    for (const QString &font : {QStringLiteral("arial.ttf"), QStringLiteral("msyh.ttc")})
        QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR") + QStringLiteral("/Fonts/") + font);
#endif
    QTemporaryDir settings; QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());
    try {
        check(settings.isValid(), "settings isolation failed");
        qint64 now = 1000;
        MainWindow main(nullptr, new FakeFieldCameraBackend, [&] { return now; });
        main.resize(1920, 1080); main.show(); settle();
        auto *bw = broadcast();
        auto *debug = child<QCheckBox>(main, "noDeviceDebug");
        auto *phase = child<QLabel>(main, "competitionPhase");
        check(child<QComboBox>(main, "ruleProfile")->currentData().toBool(), "formal mode is not default");
        check(!debug->isChecked(), "debug mode must be off by default");

        // 1. Default formal mode: no UDP listener, no robots -> start refused.
        call(main, "onToggleMatch"); now += 6000; call(main, "onRefereeTick"); settle();
        check(!bw->isMatchRunning(), "formal mode started without devices");
        check(!phase->text().contains(QStringLiteral("倒计时")), "formal mode entered countdown without devices");

        // 2. Debug mode: countdown -> running with no devices at all.
        debug->setChecked(true); settle();
        check(phase->text().contains(QStringLiteral("无车调试")), "debug state not visible");
        const QString qa = qEnvironmentVariable("SCOMP_INTEGRATION_QA_DIR");
        const auto capture = [&](const char *name) {
            if (qa.isEmpty()) return;
            check(QDir().mkpath(qa), "QA folder"); settle();
            check(main.grab().save(qa + "/" + name + ".png"), "UI capture");
        };
        capture("server-no-device-debug-ready");
        call(main, "onToggleMatch"); settle();
        check(!bw->isMatchRunning(), "countdown skipped");
        check(!debug->isEnabled(), "debug toggle not locked during a live round");
        now += 4999; call(main, "onRefereeTick"); check(!bw->isMatchRunning(), "started before 5 s");
        now += 1; call(main, "onRefereeTick"); settle();
        check(bw->isMatchRunning(), "debug round did not start without devices");
        check(bw->remainingSeconds() == 180, "debug round is not 180 s");
        capture("server-no-device-debug-running");

        // Toggling mid-round (bypassing the disabled widget) is rejected and
        // keeps the round running.
        debug->setEnabled(true); debug->setChecked(false); settle();
        check(debug->isChecked() && bw->isMatchRunning(), "debug toggled off mid-round");
        call(main, "onRefereeTick"); // refreshes the UI and re-locks the toggle
        check(!debug->isEnabled(), "debug toggle not re-locked");

        // 3. Time up with no telemetry -> waits for the referee, never auto-decides.
        now += 180000; call(main, "onRefereeTick"); settle();
        check(!bw->roundEnded(), "auto verdict without telemetry");
        check(phase->text().contains(QStringLiteral("判定")) || phase->text().contains(QStringLiteral("裁判")),
              "not awaiting referee decision");

        // 4. Manual verdict advances the BO3 into intermission.
        call(main, "onDeclareRedWin"); settle();
        check(bw->roundEnded(), "manual verdict not applied");
        check(bw->redScore() == 1 && bw->blueScore() == 0, "BO3 score wrong after manual verdict");
        check(debug->isEnabled(), "debug toggle locked during intermission");

        // 5. Turning debug off between rounds resets the series and restores checks.
        debug->setChecked(false); settle();
        check(!debug->isChecked(), "debug could not be turned off in intermission");
        check(bw->redScore() == 0 && bw->blueScore() == 0, "debug results leaked into the formal series");
        call(main, "onToggleMatch"); now += 6000; call(main, "onRefereeTick"); settle();
        check(!bw->isMatchRunning(), "device checks not restored after debug off");

        std::puts("PASS: no-device debug runs the formal BO3 flow without robots; formal mode still requires devices");
        return 0;
    } catch (const std::exception &e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
