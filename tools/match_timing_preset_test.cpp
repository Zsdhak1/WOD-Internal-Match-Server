// Formal timing presets: built-ins, validation, persistence, the 比赛时间 combo,
// locking during a BO3, the secondary editor and the removed jump button.
#include "mainwindow.h"
#include "broadcastwindow.h"
#include "clientwindow.h"
#include "fake_field_camera.h"
#include "matchtimingdialog.h"
#include "matchtimingpresets.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QTime>
#include <QTimeEdit>
#include <QTimer>
#include <cstdio>
#include <stdexcept>

static void check(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
template<class T> static T *child(QObject &o, const char *name)
{ auto *p = o.findChild<T *>(QString::fromLatin1(name)); check(p, name); return p; }
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
static void choose(QComboBox *combo, const QString &name)
{
    const int i = combo->findData(name); check(i >= 0, "preset missing from combo");
    combo->setCurrentIndex(i); emit combo->activated(i); settle();
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
    const QString qa = qEnvironmentVariable("SCOMP_INTEGRATION_QA_DIR");
    try {
        check(settings.isValid(), "settings isolation failed");
        // 1. Built-ins and validation.
        const auto defaults = matchtiming::defaultPresets();
        check(defaults.size() == 2 && defaults[0].name == QStringLiteral("小组赛")
              && defaults[0].roundSeconds == 120 && defaults[0].intermissionSeconds == 60
              && defaults[1].name == QStringLiteral("淘汰赛")
              && defaults[1].roundSeconds == 180 && defaults[1].intermissionSeconds == 120, "built-in presets wrong");
        check(!matchtiming::isValid({QStringLiteral(" "), 120, 60}), "blank name accepted");
        check(!matchtiming::isValid({QStringLiteral("x"), 5, 60}), "too-short round accepted");
        check(!matchtiming::isValid({QStringLiteral("x"), 120, -1}), "negative intermission accepted");
        check(matchtiming::isValid({QStringLiteral("x"), 120, 0}), "zero intermission rejected");
        check(!matchtiming::isValidList({{QStringLiteral("A"), 60, 0}, {QStringLiteral("a"), 90, 0}}), "duplicate names accepted");
        QString selected;
        check(matchtiming::load(&selected) == defaults && selected == QStringLiteral("淘汰赛"),
              "first run must default to 淘汰赛 (previous fixed 3+2)");

        qint64 now = 1000;
        MainWindow main(nullptr, new FakeFieldCameraBackend, [&] { return now; });
        main.resize(1920, 1080); main.show(); settle();
        auto *bw = broadcast();
        auto *combo = child<QComboBox>(main, "timingPreset");
        auto *phase = child<QLabel>(main, "competitionPhase");
        check(combo->count() == 2 && combo->currentData().toString() == QStringLiteral("淘汰赛"), "combo default");
        check(phase->text().endsWith(QStringLiteral("03:00")), "knockout idle clock not 03:00");

        // 2. The navigation button next to the card controls is gone.
        for (QPushButton *b : main.findChildren<QPushButton *>())
            check(!b->text().contains(QStringLiteral("选择判罚机器人")), "card target jump button still present");

        // 3. Switch to 小组赛: persisted, 2-minute clock shown before the round.
        choose(combo, QStringLiteral("小组赛"));
        check(phase->text().endsWith(QStringLiteral("02:00")), "group preset not applied to idle clock");
        check(matchtiming::load(&selected).size() == 2 && selected == QStringLiteral("小组赛"), "selection not persisted");
        if (!qa.isEmpty()) { check(QDir().mkpath(qa), "QA folder"); settle(); check(main.grab().save(qa + "/server-timing-preset.png"), "capture"); }

        // 4. Run a 小组赛 round with no devices: 120 s round, then 60 s intermission.
        child<QCheckBox>(main, "noDeviceDebug")->setChecked(true); settle();
        call(main, "onToggleMatch"); now += 5000; call(main, "onRefereeTick"); settle();
        check(bw->isMatchRunning() && bw->remainingSeconds() == 120, "group round is not 120 s");
        check(!combo->isEnabled(), "preset switch not locked during the round");
        choose(combo, QStringLiteral("淘汰赛")); // bypasses the disabled widget
        check(bw->remainingSeconds() == 120 && combo->currentData().toString() == QStringLiteral("小组赛"),
              "preset changed under a running round");
        now += 120000; call(main, "onRefereeTick"); settle();
        check(!bw->isMatchRunning(), "group round did not end at 120 s");
        call(main, "onDeclareRedWin"); settle();
        check(!combo->isEnabled(), "preset switch not locked during intermission");
        check(phase->text().endsWith(QStringLiteral("01:00")), "group intermission is not 60 s");
        now += 59999; call(main, "onRefereeTick");
        check(!child<QPushButton>(main, "startMatch")->isEnabled(), "intermission ended early");
        now += 1; call(main, "onRefereeTick"); settle();
        check(child<QPushButton>(main, "startMatch")->isEnabled(), "intermission did not end at 60 s");
        check(!combo->isEnabled(), "preset unlocked mid-BO3 (would mix durations)");

        // 5. After resetting the BO3 the switch unlocks again.
        call(main, "onResetMatch"); settle();
        check(combo->isEnabled(), "preset not unlocked after BO3 reset");

        // 6. Secondary editor: add a preset, edit it, save; the combo picks it up.
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<MatchTimingDialog *>(QApplication::activeModalWidget());
            check(dialog, "editor dialog not opened");
            check(child<QListWidget>(*dialog, "timingPresetList")->count() == 2, "editor list wrong");
            call(*dialog, "onAdd"); settle();
            auto *name = child<QLineEdit>(*dialog, "timingPresetName");
            name->setText(QStringLiteral("表演赛")); emit name->textEdited(name->text());
            child<QTimeEdit>(*dialog, "timingPresetRound")->setTime(QTime(0, 1, 30));
            child<QTimeEdit>(*dialog, "timingPresetIntermission")->setTime(QTime(0, 0, 30));
            settle();
            if (!qa.isEmpty()) check(dialog->grab().save(qa + "/timing-preset-editor.png"), "editor capture");
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        });
        child<QPushButton>(main, "editTimingPresets")->click(); settle();
        check(combo->count() == 3 && combo->findData(QStringLiteral("表演赛")) >= 0, "new preset missing from combo");
        const auto stored = matchtiming::load(&selected);
        check(stored.size() == 3 && stored[2] == MatchTimingPreset{QStringLiteral("表演赛"), 90, 30}, "edited preset not saved");
        check(selected == QStringLiteral("小组赛"), "editing changed the active preset");
        choose(combo, QStringLiteral("表演赛"));
        check(phase->text().endsWith(QStringLiteral("01:30")), "custom preset not applied");

        std::puts("PASS: timing presets (小组赛 2+1, 淘汰赛 3+2, custom), validation, persistence, BO3 lock, editor; card jump button removed"); std::fflush(stdout);
        return 0;
    } catch (const std::exception &e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
