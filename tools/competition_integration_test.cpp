#include "mainwindow.h"
#include "broadcastwindow.h"
#include "clientwindow.h"
#include "simulatorwindow.h"
#include "refereeentrypanel.h"
#include "fake_field_camera.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QThread>
#include <QUdpSocket>
#include <functional>
#include <cstdio>
#include <stdexcept>

static void check(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
template<class T> static T *child(QObject &o,const char *name)
{ auto *p=o.findChild<T *>(QString::fromLatin1(name)); check(p,"missing UI object"); return p; }
static void call(QObject &o,const char *name)
{ check(QMetaObject::invokeMethod(&o,name,Qt::DirectConnection),"slot unavailable"); }
static void settle(int ms=40)
{ QElapsedTimer t;t.start();do{QApplication::processEvents();QThread::msleep(2);}while(t.elapsed()<ms); }
static void wait(const std::function<bool()> &f,const char *why)
{ QElapsedTimer t;t.start();while(!f()&&t.elapsed()<3500)settle(5);check(f(),why); }
static BroadcastWindow *broadcast()
{ for(auto *w:QApplication::topLevelWidgets())if(auto *b=qobject_cast<BroadcastWindow *>(w))if(!qobject_cast<ClientWindow *>(w))return b;throw std::runtime_error("missing output"); }
static void simStart(SimulatorWindow &s,const char *ip,quint16 port)
{ s.setProtocolTimeForTest(1000);child<QLineEdit>(s,"simLocalAddress")->setText(QString::fromLatin1(ip));child<QSpinBox>(s,"simServerPort")->setValue(port);call(s,"onToggleRunning"); }
static void assign(MainWindow &m,SimulatorWindow &s,int id,int side)
{ const int idx=child<QComboBox>(m,"deviceSelect")->findData(child<QLineEdit>(s,"simDeviceMac")->text());check(idx>=0,"device undiscovered");child<QComboBox>(m,"deviceSelect")->setCurrentIndex(idx);child<QSpinBox>(m,"deviceRobotId")->setValue(id);child<QComboBox>(m,"deviceTeam")->setCurrentIndex(side);child<QLineEdit>(m,"deviceControllerMac")->setText("8D:23:AB:A5:3C:C9");call(m,"onAssignTeam"); }
int main(int argc,char **argv)
{
    qputenv("QT_QPA_PLATFORM","offscreen");
    QApplication app(argc,argv);
#ifdef Q_OS_WIN
    for (const QString &font : {QStringLiteral("arial.ttf"), QStringLiteral("msyh.ttc"), QStringLiteral("msyhbd.ttc")})
        QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR") + QStringLiteral("/Fonts/") + font);
#endif
    app.setFont(QFont(QStringLiteral("Microsoft YaHei"),10));
    QTemporaryDir settings; QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    QSettings::setPath(QSettings::IniFormat,QSettings::SystemScope,settings.path());
    try {
        check(settings.isValid(),"settings isolation failed");
        qint64 now=1000;
        MainWindow main(nullptr,new FakeFieldCameraBackend,[&]{return now;}); main.resize(1920,1080);main.show();settle();
        check(child<QComboBox>(main,"ruleProfile")->currentData().toBool(),"formal mode is not default");
        check(child<QSpinBox>(main,"ruleOfflineGrace")->value()==0,"formal inherited automatic offline loss");
        QFile teams(settings.path()+"/teams.csv");check(teams.open(QIODevice::WriteOnly),"team fixture");teams.write("red,blue\nA1,A2\nB1,B2\n");teams.close();QString error;
        check(main.importTeamTable(teams.fileName(),&error),"team import failed");
        child<QComboBox>(main,"teamPairSelection")->setCurrentIndex(1);
        child<QPushButton>(main,"applyTeamPair")->click();
        auto *bw=broadcast();check(bw->redTeamName()=="B1"&&bw->blueTeamName()=="B2","manual row selection not applied");
        QUdpSocket reserve;check(reserve.bind(QHostAddress::LocalHost,quint16(0)),"UDP reserve");const auto port=reserve.localPort();reserve.close();
        child<QSpinBox>(main,"serverUdpPort")->setValue(port);call(main,"onToggleListen");
        SimulatorWindow red,blue;simStart(red,"127.0.0.2",port);simStart(blue,"127.0.0.3",port);
        auto *manager=main.findChild<RobotManager *>();check(manager,"manager unavailable");
        wait([&]{return manager->devices().size()==2;},"device discovery");
        assign(main,red,1,1);wait([&]{return manager->teamForRobot(1)==1;},"red assignment");
        assign(main,blue,2,2);wait([&]{return manager->teamForRobot(2)==2;},"blue assignment");
        wait([&]{return manager->robots().size()==2&&manager->robots()[0].hp>=0&&manager->robots()[1].hp>=0;},"startup telemetry");
        auto *tcp=main.findChild<MatchServer *>();check(tcp&&tcp->start(0),"TCP startup");
        ClientWindow client; client.resize(1280,720); client.show(); settle();
        child<QLineEdit>(client,"clientServerPort")->setText(QString::number(tcp->port()));
        child<QComboBox>(client,"clientVideoSource")->setCurrentIndex(0);call(client,"onLoginClicked");
        wait([&]{return !child<QWidget>(client,"registrationOverlay")->isVisible();},"client registration");
        QImage frame(1920,1080,QImage::Format_RGB32);frame.fill(QColor("#102430"));bw->setProgramFrame(frame);
        const QString qa=qEnvironmentVariable("SCOMP_INTEGRATION_QA_DIR");if(!qa.isEmpty())check(QDir().mkpath(qa),"QA folder");
        const auto capture=[&](const char *name){if(!qa.isEmpty()){settle();check(main.grab().save(qa+"/"+name+".png"),"UI capture");}};
        const auto captureClient=[&](const char *name){if(!qa.isEmpty()){settle(300);check(client.grab().save(qa+"/client-"+name+".png"),"client capture");}};
        bool countdownCaptured=false;
        const auto startRound=[&] {
            call(main,"onToggleMatch");check(!bw->isMatchRunning(),"countdown started combat early");
            qint64 elapsed=0;
            if(!countdownCaptured){countdownCaptured=true;elapsed=2000;now+=elapsed;call(main,"onRefereeTick");captureClient("formal-countdown");}
            check(!child<QPushButton>(main,"applyTeamPair")->isEnabled(),"active team application allowed");
            check(!main.importTeamTable(teams.fileName(),&error),"active import not rejected");
            now+=4999-elapsed;call(main,"onRefereeTick");check(!bw->isMatchRunning(),"4.999s starts combat");
            now+=1;call(main,"onRefereeTick");
            wait([&]{return bw->isMatchRunning()&&client.isMatchRunning();},"confirmed formal start failed");
            check(bw->remainingSeconds()==180&&client.remainingSeconds()==180,"180 seconds not synchronized");
        };
        startRound();capture("server-formal-running");
        const auto deathAndRevive=[&] {
            call(red,"onSendDeath");wait([&]{return !child<QCheckBox>(red,"simAlive")->isChecked();},"death missing");
            settle(150);now+=5000;red.advanceProtocolTimeForTest(5000);call(main,"onRefereeTick");
            wait([&]{return child<QCheckBox>(red,"simAlive")->isChecked();},"revival approval not delivered");
            wait([&]{return !child<QLabel>(main,"redRemainingRevives")->text().contains(QStringLiteral("确认中"));},"revival ACK not accounted");
            settle(150);
        };
        deathAndRevive();
        check(child<QLabel>(main,"redRemainingRevives")->text().contains("2 / 3"),"revival quota not displayed");
        wait([&]{return child<QLabel>(client,"redReviveCount")->text().contains("2");},"client quota not mirrored");
        capture("server-revival-accounted");captureClient("formal-running");
        now+=qint64(bw->remainingSeconds())*1000;call(main,"onRefereeTick");
        wait([&]{return bw->roundEnded()&&client.roundEnded();},"timeout no settlement");
        captureClient("formal-settlement");
        auto *entry=static_cast<RefereeEntryPanel *>(child<QWidget>(main,"refereeEntryPanel"));
        check(entry->result().value("reason").toString()=="remaining_revives","wrong timeout reason");
        check(entry->result().value("winner").toInt()==2,"resource winner wrong");
        check(entry->result().value("red").toObject().value("remainingRevives").toInt()==2,"entry quota wrong");
        check(entry->plainText().contains(QStringLiteral("剩余")),"copy data missing resources");
        // The main clock freezes on the finished round (timed out -> 00:00); the
        // 120 s preparation countdown is owned by the overlay intermission bar.
        check(bw->blueScore()==1&&bw->remainingSeconds()==0,"BO3/intermission display wrong");
        capture("server-referee-after-timeout");
        call(main,"onToggleMatch");check(!bw->isMatchRunning(),"next round skipped preparation");
        now+=60000;call(main,"onRefereeTick");
        wait([&]{return child<QLabel>(client,"matchState")->text().startsWith(QStringLiteral("局间准备"));},"client shows preparation as finished");
        captureClient("formal-intermission");
        now+=59999;call(main,"onRefereeTick");check(!child<QPushButton>(main,"startMatch")->isEnabled(),"prep ended early");
        now+=1;call(main,"onRefereeTick");check(child<QPushButton>(main,"startMatch")->isEnabled(),"prep failed to unlock");
        check(!bw->isMatchRunning(),"automatic unsafe next round");
        startRound();check(bw->blueScore()==1&&child<QLabel>(main,"redRemainingRevives")->text().contains("3 / 3"),"new round lost wins or kept old quota");
        for(int i=0;i<3;++i)deathAndRevive();
        check(child<QLabel>(main,"redRemainingRevives")->text().contains("0 / 3"),"three approvals not exhausted");
        call(red,"onSendDeath");settle(150);now+=301;call(main,"onRefereeTick");
        wait([&]{return bw->roundEnded();},"exhausted death not settled");
        check(entry->result().value("reason").toString()=="revive_exhausted"&&bw->blueScore()==2,"BO3 2:0 wrong");
        check(!child<QPushButton>(main,"startMatch")->isEnabled(),"2:0 allowed third round");
        capture("server-referee-after-exhaustion");
        child<QComboBox>(main,"teamPairSelection")->setCurrentIndex(0);child<QPushButton>(main,"applyTeamPair")->click();
        wait([&]{return child<QWidget>(client,"registrationOverlay")->isVisible();},"old player seats survived team switch");
        check(bw->redTeamName()=="A1"&&bw->blueTeamName()=="A2"&&bw->blueScore()==0,"new matchup reset wrong");
        call(red,"onToggleRunning");call(blue,"onToggleRunning");tcp->stop();call(main,"onToggleListen");
        std::puts("PASS: real Qt/UDP/TCP formal countdown, finite revival, quotas, timeout/exhaustion, BO3, preparation, referee transcription and team-switch isolation");
        return 0;
    } catch(const std::exception &e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
