// Armor dual-threshold (ESP32 f1c17e0): real MainWindow + UDP simulator.
// Covers: 0x0E/0x0F/0x8A wire layout, motion column, per-board apply with
// safe ordering, apply-to-all, defaults, still<=moving guard, APPLIED state.
#include "mainwindow.h"
#include "armorthresholddialog.h"
#include "simulatorwindow.h"
#include "fake_field_camera.h"
#include "protocol.h"
#include "robotmanager.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QUdpSocket>
#include <cstdio>
#include <functional>
#include <stdexcept>

static void check(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
template<class T> static T *child(QObject &o, const char *name)
{ auto *p = o.findChild<T *>(QString::fromLatin1(name)); check(p, name); return p; }
static void call(QObject &o, const char *name)
{ check(QMetaObject::invokeMethod(&o, name, Qt::DirectConnection), "slot unavailable"); }
static void settle(int ms = 40)
{ QElapsedTimer t; t.start(); do { QApplication::processEvents(); QThread::msleep(2); } while (t.elapsed() < ms); }
static void wait(const std::function<bool()> &f, const char *why, int ms = 4000)
{ QElapsedTimer t; t.start(); while (!f() && t.elapsed() < ms) settle(5); check(f(), why); }

static void wireLayout()
{
    const QByteArray set = proto::makeArmorThresholdSet(2, 1, 1, 1000000, 0x11223344);
    check(set.size() == 15 && set.toHex() == "5452028a020101" "40420f00" "44332211", "0x8A layout");
    proto::Frame f; check(proto::parseDatagram(set, f) && f.armorNode == 1 && f.armorProfile == 1
                          && f.thresholdValue == 1000000 && f.transactionId == 0x11223344, "0x8A parse");
    check(proto::isDownlinkCommand(proto::TypeArmorThresholdSet), "0x8A downlink");
    const QByteArray m = proto::makeMotion(2, 2, 7);
    check(m.toHex() == "5452020e020207" && proto::parseDatagram(m, f) && f.motion == 2 && f.motionSequence == 7, "0x0E");
    const QByteArray s = proto::makeArmorThresholdState(2, 3, 1, 0x05, 0x02, 1, 700000, 1000000, 700000);
    check(s.size() == 22 && proto::parseDatagram(s, f) && f.armorNode == 3 && f.motion == 1 && f.appliedMask == 5
          && f.failedMask == 2 && f.storageStatus == 1 && f.stillThreshold == 700000
          && f.movingThreshold == 1000000 && f.appliedThreshold == 700000, "0x0F");
    check(!proto::parseDatagram(s.left(21), f), "short 0x0F accepted");
    // Safe ordering keeps still <= moving after each single write.
    using D = ArmorThresholdDialog;
    auto plan = D::planNode(1, 700000, 1000000, 1500000, 2000000);   // raise both
    check(plan.size() == 2 && plan[0].profile == 1 && plan[1].profile == 0, "raise order");
    plan = D::planNode(1, 700000, 1000000, 5000, 6000);               // lower both
    check(plan.size() == 2 && plan[0].profile == 0 && plan[1].profile == 1, "lower order");
    check(D::planNode(1, 700000, 1000000, 700000, 1000000).isEmpty(), "no-op plan");
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
        wireLayout();
        MainWindow main(nullptr, new FakeFieldCameraBackend);
        main.resize(1920, 1080); main.show(); settle();
        QUdpSocket reserve; check(reserve.bind(QHostAddress::LocalHost, quint16(0)), "UDP reserve");
        const auto udp = reserve.localPort(); reserve.close();
        child<QSpinBox>(main, "serverUdpPort")->setValue(udp); call(main, "onToggleListen");
        SimulatorWindow sim; sim.setProtocolTimeForTest(1000);
        child<QLineEdit>(sim, "simLocalAddress")->setText(QStringLiteral("127.0.0.2"));
        child<QSpinBox>(sim, "simServerPort")->setValue(udp); call(sim, "onToggleRunning");
        auto *manager = main.findChild<RobotManager *>(); check(manager, "manager");
        wait([&] { return manager->devices().size() == 1; }, "device discovery");
        auto *select = child<QComboBox>(main, "deviceSelect");
        select->setCurrentIndex(select->findData(child<QLineEdit>(sim, "simDeviceMac")->text()));
        child<QSpinBox>(main, "deviceRobotId")->setValue(2); child<QComboBox>(main, "deviceTeam")->setCurrentIndex(2);
        child<QLineEdit>(main, "deviceControllerMac")->setText(QStringLiteral("8D:23:AB:A5:3C:C9"));
        call(main, "onAssignTeam");
        wait([&] { return manager->teamForRobot(2) == 2; }, "assignment");
        const auto robot = [&] { for (const auto &r : manager->robots()) if (r.robotId == 2) return r;
                                 return RobotManager::RobotInfo{}; };
        const auto slotsFresh = [&] { const auto r = robot(); const qint64 now = QDateTime::currentMSecsSinceEpoch();
            for (const auto &s : r.armor) if (!RobotManager::armorSlotFresh(s, now)) return false; return true; };
        wait(slotsFresh, "threshold snapshots");
        check(robot().armor[0].still == 700000 && robot().armor[0].moving == 1000000 && robot().armor[0].applied, "defaults");

        // Motion column follows 0x0E.
        auto *table = child<QTableWidget>(main, "robotTable");
        check(table->horizontalHeaderItem(9)->text() == QStringLiteral("运动"), "motion column");
        child<QComboBox>(sim, "simMotion")->setCurrentIndex(2);
        wait([&] { return robot().motion == 2 && table->item(0, 9)->text() == QStringLiteral("运动"); }, "moving not shown");

        // Dialog.
        call(main, "onShowArmorThresholds");
        auto *dlg = main.findChild<ArmorThresholdDialog *>(QStringLiteral("armorThresholdDialog"));
        check(dlg && dlg->isVisible() && dlg->robotId() == 2, "dialog");
        auto *status = child<QLabel>(*dlg, "armorStatus");
        wait([&] { return child<QLabel>(*dlg, "armorMotion")->text().contains(QStringLiteral("运动")); }, "dialog motion");

        // Single board, raise both: moving first, then still.
        child<QSpinBox>(*dlg, "armorStill1")->setValue(1500000);
        child<QSpinBox>(*dlg, "armorMoving1")->setValue(2000000);
        child<QPushButton>(*dlg, "armorApply1")->click();
        wait([&] { return !dlg->busy(); }, "board 1 apply stuck");
        check(status->text().contains(QStringLiteral("已保存")), "board 1 not saved");
        wait([&] { const auto s = robot().armor[0]; return s.still == 1500000 && s.moving == 2000000; }, "board 1 snapshot");
        check(robot().armor[1].still == 700000, "board 2 changed");

        // Guard: still > moving is never sent.
        child<QSpinBox>(*dlg, "armorStill2")->setValue(3000000);
        child<QSpinBox>(*dlg, "armorMoving2")->setValue(2000000);
        child<QPushButton>(*dlg, "armorApply2")->click(); settle(300);
        check(status->text().contains(QStringLiteral("≤")) && robot().armor[1].still == 700000, "invalid pair sent");

        // Apply to all four (lowering board 1, raising others).
        child<QSpinBox>(*dlg, "armorAllStill")->setValue(900000);
        child<QSpinBox>(*dlg, "armorAllMoving")->setValue(1200000);
        child<QPushButton>(*dlg, "armorApplyAll")->click();
        wait([&] { return !dlg->busy(); }, "apply all stuck", 8000);
        check(status->text().contains(QStringLiteral("4 块板")), "apply all not saved");
        wait([&] { const auto r = robot(); for (const auto &s : r.armor) if (s.still != 900000 || s.moving != 1200000) return false; return true; },
             "apply all snapshot");

        // Not applied by armor: shown as pending, not APPLIED.
        child<QCheckBox>(sim, "simArmorApplied")->setChecked(false);
        wait([&] { return !robot().armor[0].applied; }, "applied mask");
        call(*dlg, "refresh");
        check(child<QTableWidget>(*dlg, "armorTable")->item(0, 4)->text().contains(QStringLiteral("未生效")), "pending text");
        child<QCheckBox>(sim, "simArmorApplied")->setChecked(true);
        wait([&] { const auto r = robot(); for (const auto &s : r.armor) if (!s.applied) return false; return true; },
             "re-applied");
        call(*dlg, "refresh");
        if (!qa.isEmpty()) {
            check(QDir().mkpath(qa), "QA folder"); settle(300);
            check(dlg->grab().save(qa + "/armor-threshold-dialog.png"), "dialog capture");
            check(main.grab().save(qa + "/server-armor-motion.png"), "main capture");
        }

        // Restore firmware defaults.
        child<QPushButton>(*dlg, "armorRestoreDefaults")->click();
        wait([&] { return !dlg->busy(); }, "defaults stuck", 8000);
        wait([&] { const auto r = robot(); for (const auto &s : r.armor) if (s.still != 700000 || s.moving != 1000000) return false; return true; },
             "defaults snapshot");

        // Offline: everything unknown, apply refused without sending.
        call(sim, "onToggleRunning");
        wait([&] { return !robot().online; }, "offline");
        call(*dlg, "refresh");
        child<QPushButton>(*dlg, "armorApply1")->click(); settle(100);
        check(status->text().contains(QStringLiteral("快照")) && !dlg->busy(), "offline apply");
        dlg->close(); call(main, "onToggleListen");
        std::puts("PASS: armor dual threshold 0x0E/0x0F/0x8A, motion column, per-board safe order, apply-all, defaults, guard, applied state, offline");
        std::fflush(stdout);
        return 0;
    } catch (const std::exception &e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
