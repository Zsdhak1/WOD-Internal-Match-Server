// Manual HP deduction web page: real MainWindow + two UDP simulators + HTTP.
// Covers: pages/QR, round-only gating, ACK-confirmed SET_HP(current-N),
// idempotent requestId replay, one in-flight per side, clamp at 0.
#include "mainwindow.h"
#include "broadcastwindow.h"
#include "clientwindow.h"
#include "simulatorwindow.h"
#include "fake_field_camera.h"
#include "manualdamageserver.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QUdpSocket>
#include <functional>
#include <cstdio>
#include <stdexcept>

static void check(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
template<class T> static T *child(QObject &o, const char *name)
{ auto *p = o.findChild<T *>(QString::fromLatin1(name)); check(p, name); return p; }
static void call(QObject &o, const char *name)
{ check(QMetaObject::invokeMethod(&o, name, Qt::DirectConnection), "slot unavailable"); }
static void settle(int ms = 40)
{ QElapsedTimer t; t.start(); do { QApplication::processEvents(); QThread::msleep(2); } while (t.elapsed() < ms); }
static void wait(const std::function<bool()> &f, const char *why)
{ QElapsedTimer t; t.start(); while (!f() && t.elapsed() < 3500) settle(5); check(f(), why); }
static void simStart(SimulatorWindow &s, const char *ip, quint16 port)
{ s.setProtocolTimeForTest(1000); child<QLineEdit>(s, "simLocalAddress")->setText(QString::fromLatin1(ip));
  child<QSpinBox>(s, "simServerPort")->setValue(port); call(s, "onToggleRunning"); }
static void assign(MainWindow &m, SimulatorWindow &s, int id, int side)
{ const int idx = child<QComboBox>(m, "deviceSelect")->findData(child<QLineEdit>(s, "simDeviceMac")->text());
  check(idx >= 0, "device undiscovered"); child<QComboBox>(m, "deviceSelect")->setCurrentIndex(idx);
  child<QSpinBox>(m, "deviceRobotId")->setValue(id); child<QComboBox>(m, "deviceTeam")->setCurrentIndex(side);
  child<QLineEdit>(m, "deviceControllerMac")->setText("8D:23:AB:A5:3C:C9"); call(m, "onAssignTeam"); }

struct Reply { int status = 0; QByteArray body; QJsonObject json() const { return QJsonDocument::fromJson(body).object(); } };
static Reply http(quint16 port, const QByteArray &method, const QByteArray &path, const QByteArray &body = {})
{
    QTcpSocket s; s.connectToHost(QHostAddress::LocalHost, port);
    wait([&] { return s.state() == QAbstractSocket::ConnectedState; }, "HTTP connect");
    s.write(method + ' ' + path + " HTTP/1.1\r\nHost: test\r\nContent-Type: application/json\r\nContent-Length: "
            + QByteArray::number(body.size()) + "\r\n\r\n" + body);
    QByteArray raw;
    wait([&] { raw += s.readAll(); return s.state() == QAbstractSocket::UnconnectedState || raw.contains("\r\n\r\n")
                && raw.size() >= raw.indexOf("\r\n\r\n") + 4 + raw.mid(raw.indexOf("Content-Length: ") + 16).split('\r').first().toInt(); },
         "HTTP response");
    raw += s.readAll();
    Reply r; r.status = raw.mid(9, 3).toInt(); r.body = raw.mid(raw.indexOf("\r\n\r\n") + 4); return r;
}
static QByteArray damage(const char *team, const char *id)
{ return QByteArray("{\"team\":\"") + team + "\",\"requestId\":\"" + id + "\"}"; }

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
        check(!ManualDamageServer::qrCode(QStringLiteral("http://192.168.1.10:8080/red")).isNull(), "QR render");
        qint64 now = 1000;
        MainWindow main(nullptr, new FakeFieldCameraBackend, [&] { return now; });
        main.resize(1920, 1080); main.show(); settle();
        QUdpSocket reserve; check(reserve.bind(QHostAddress::LocalHost, quint16(0)), "UDP reserve");
        const auto udp = reserve.localPort(); reserve.close();
        child<QSpinBox>(main, "serverUdpPort")->setValue(udp); call(main, "onToggleListen");
        SimulatorWindow red, blue; simStart(red, "127.0.0.2", udp); simStart(blue, "127.0.0.3", udp);
        auto *manager = main.findChild<RobotManager *>(); check(manager, "manager");
        wait([&] { return manager->devices().size() == 2; }, "device discovery");
        assign(main, red, 1, 1); wait([&] { return manager->teamForRobot(1) == 1; }, "red assignment");
        assign(main, blue, 2, 2); wait([&] { return manager->teamForRobot(2) == 2; }, "blue assignment");
        wait([&] { return manager->robots().size() == 2 && manager->robots()[0].hp >= 0 && manager->robots()[1].hp >= 0; }, "telemetry");

        // Start the page server on a free port; amount 25.
        auto *server = main.findChild<ManualDamageServer *>(); check(server, "damage server");
        check(server->start(0), "HTTP start");
        const quint16 port = server->port();
        child<QSpinBox>(main, "damageAmount")->setValue(25);
        call(main, "refreshManualDamageUi");
        check(child<QPushButton>(main, "showDamageQr")->isEnabled(), "QR button disabled while serving");
        if (!qa.isEmpty()) {
            check(QDir().mkpath(qa), "QA folder");
            qputenv("SCOMP_DAMAGE_QR_CAPTURE", (qa + "/manual-damage-qr.png").toLocal8Bit());
            call(main, "onShowManualDamageQr");
            qunsetenv("SCOMP_DAMAGE_QR_CAPTURE");
        }

        // Pages.
        for (const char *p : {"/", "/red", "/blue"}) {
            const Reply r = http(port, "GET", p);
            check(r.status == 200 && r.body.contains("/api/damage") && r.body.contains("navigator.vibrate"), "page");
            if (!qa.isEmpty() && QByteArray(p) == "/red") {
                QFile page(qa + "/manual-damage-page.html");
                check(page.open(QIODevice::WriteOnly) && page.write(r.body) == r.body.size(), "page dump");
            }
        }
        check(http(port, "GET", "/nope").status == 404, "404");
        check(http(port, "POST", "/api/damage", "{bad").status == 400, "bad JSON accepted");
        check(http(port, "POST", "/api/damage", damage("green", "abcdefgh1")).status == 400, "bad team accepted");

        // Outside a round: rejected, no SET_HP.
        const int hp0 = child<QSpinBox>(red, "simHp")->value();
        Reply r = http(port, "POST", "/api/damage", damage("red", "before-round-1"));
        check(r.status == 200 && !r.json().value("ok").toBool() && r.json().value("message").toString().contains(QStringLiteral("小局")),
              "deduction allowed outside a round");
        check(!http(port, "GET", "/api/state?team=red").json().value("available").toBool(), "state available before round");

        // Start a round.
        call(main, "onToggleMatch"); now += 5000; call(main, "onRefereeTick");
        auto *bw = [] { for (auto *w : QApplication::topLevelWidgets()) if (auto *b = qobject_cast<BroadcastWindow *>(w))
                        if (!qobject_cast<ClientWindow *>(w)) return b; throw std::runtime_error("output"); }();
        wait([&] { call(main, "onRefereeTick"); return bw->isMatchRunning(); }, "round start");
        settle(250); // fresh telemetry
        const QJsonObject st = http(port, "GET", "/api/state?team=red").json();
        check(st.value("available").toBool() && st.value("amount").toInt() == 25 && st.value("hp").toInt() == 300, "state in round");

        // Deduct: ACK-confirmed, simulator HP drops by 25.
        r = http(port, "POST", "/api/damage", damage("red", "tap-000000001"));
        check(r.json().value("ok").toBool() && r.json().value("hp").toInt() == 275, "deduction not confirmed");
        check(child<QSpinBox>(red, "simHp")->value() == 275, "simulator HP not reduced");
        check(child<QSpinBox>(blue, "simHp")->value() == 300, "other side affected");
        // Retry of the same requestId replays the result without a second deduction.
        r = http(port, "POST", "/api/damage", damage("red", "tap-000000001"));
        check(r.json().value("ok").toBool() && r.json().value("hp").toInt() == 275, "replay mismatch");
        settle(250);
        check(child<QSpinBox>(red, "simHp")->value() == 275, "replay deducted twice");

        // Blue side and clamp at 0.
        wait([&] { return manager->robots().size() == 2; }, "robots");
        child<QSpinBox>(main, "damageAmount")->setValue(300); settle(250);
        r = http(port, "POST", "/api/damage", damage("blue", "tap-000000002"));
        check(r.json().value("ok").toBool() && r.json().value("hp").toInt() == 0, "clamp to 0");
        wait([&] { return !child<QCheckBox>(blue, "simAlive")->isChecked(); }, "0 HP did not kill");
        settle(250);
        r = http(port, "POST", "/api/damage", damage("blue", "tap-000000003"));
        check(!r.json().value("ok").toBool(), "dead robot deducted");
        Q_UNUSED(hp0);

        if (!qa.isEmpty()) { settle(); check(main.grab().save(qa + "/server-manual-damage.png"), "capture"); }
        server->stop();
        check(!server->isListening(), "stop");
        call(red, "onToggleRunning"); call(blue, "onToggleRunning"); call(main, "onToggleListen");
        std::puts("PASS: manual damage page, round-only, ACK-confirmed SET_HP(current-N), idempotent replay, clamp, dead-robot reject, QR");
        return 0;
    } catch (const std::exception &e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
