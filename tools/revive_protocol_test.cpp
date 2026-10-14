// Standalone Qt Widgets/Network test. CMake integration is owned by the main agent.
// Sources: this file, robotmanager.cpp, robotcommander.cpp, simulatorwindow.cpp.
// Enable AUTOMOC for their Q_OBJECT headers; responsiveui.h is header-only.
#include "protocol.h"
#include "robotcommander.h"
#include "robotmanager.h"
#include "simulatorwindow.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QLineEdit>
#include <QNetworkDatagram>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QUdpSocket>
#include <functional>
#include <memory>
#include <stdexcept>

namespace {
void check(bool ok, const char *message)
{
    if (!ok) throw std::runtime_error(message);
}
void waitFor(const std::function<bool()> &condition, const char *message)
{
    QElapsedTimer elapsed;
    elapsed.start();
    // Only await actual loopback I/O. Protocol delays use the injected clock,
    // never a five-second sleep or a guessed fixed network-settling sleep.
    while (!condition() && elapsed.elapsed() < 2000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    check(condition(), message);
}
void invoke(QObject &object, const char *slot)
{
    check(QMetaObject::invokeMethod(&object, slot, Qt::DirectConnection), "Missing simulator slot");
}
template<class T> T *control(QObject &object, const char *name)
{
    auto *result = object.findChild<T *>(QString::fromLatin1(name));
    check(result != nullptr, "Missing simulator control");
    return result;
}
struct Received {
    QByteArray bytes;
    proto::Frame frame;
    QHostAddress addr;
    quint16 port = 0;
};
struct Request {
    quint8 id = 0;
    quint8 sequence = 0;
    quint32 tx = 0;
    quint32 ipv4 = 0;
    quint16 port = 0;
};
struct Ack {
    quint32 tx = 0;
    quint8 type = 0;
    quint8 result = 0;
    quint8 target = 0;
};
class Server : public QObject {
public:
    QUdpSocket socket;
    RobotManager manager;
    RobotCommander commander{&socket};
    QVector<Received> received;
    QVector<Request> requests;
    QVector<Ack> confirmed;
    int rawAcks = 0;
    Server()
    {
        check(socket.bind(QHostAddress::LocalHost, quint16(0), QUdpSocket::DontShareAddress),
              "Cannot bind loopback server");
        connect(&manager, &RobotManager::endpointLearned, &commander, &RobotCommander::learnEndpoint);
        connect(&manager, &RobotManager::ackReceivedFrom, &commander, &RobotCommander::onAckFrom);
        connect(&manager, &RobotManager::ackReceived, this,
                [this](quint8, quint32, quint8, quint8) { ++rawAcks; });
        connect(&manager, &RobotManager::reviveRequested, this,
                [this](quint8 id, quint8 seq, quint32 tx, quint32 ip, quint16 port) {
            requests.append(Request{id, seq, tx, ip, port});
        });
        connect(&manager, &RobotManager::reliableEventNeedsAck, this,
                [this](quint8, quint8 type, quint32 tx, const QHostAddress &addr, quint16 port) {
            manager.sendAck(&socket, addr, port, type, tx, 0);
        });
        connect(&commander, &RobotCommander::commandAcked, this,
                [this](quint32 tx, quint8 type, quint8 result, quint8 target) {
            confirmed.append(Ack{tx, type, result, target});
        });
        connect(&socket, &QUdpSocket::readyRead, this, [this] {
            while (socket.hasPendingDatagrams()) {
                const auto dg = socket.receiveDatagram();
                proto::Frame frame;
                if (!dg.isValid() || !proto::parseDatagram(dg.data(), frame)) continue;
                received.append(Received{dg.data(), frame, dg.senderAddress(), static_cast<quint16>(dg.senderPort())});
                manager.handleDatagram(dg.data(), dg.senderAddress(), dg.senderPort());
            }
        });
    }
    void sendTo(const QByteArray &bytes, const QHostAddress &ip, quint16 port)
    {
        check(socket.writeDatagram(bytes, ip, port) == bytes.size(), "Loopback send failed");
    }
    void command(const QByteArray &bytes, quint8 type, quint32 tx, quint8 result,
                 const QHostAddress &ip = QHostAddress(QStringLiteral("127.0.0.2")))
    {
        const int before = received.size();
        sendTo(bytes, ip, proto::kDownlinkPort);
        waitFor([&] {
            for (int i = before; i < received.size(); ++i) {
                const auto &f = received[i].frame;
                if (f.type == proto::TypeAck && f.ackedFrameType == type
                    && f.transactionId == tx && f.result == result) return true;
            }
            return false;
        }, "Expected simulator ACK was not received");
    }
    int countType(quint8 type) const
    {
        int count = 0;
        for (const auto &r : received) if (r.frame.type == type) ++count;
        return count;
    }
};

void testCodecAndManager()
{
    const auto request = proto::makeReviveRequest(7, 0xa5, 0x12345678u);
    const auto approval = proto::makeReviveApprove(7, 0xa5, 0x12345678u);
    check(request == QByteArray::fromHex("5452020d07a578563412"), "Wrong request wire bytes");
    check(approval == QByteArray::fromHex("5452028907a578563412"), "Wrong approval wire bytes");
    proto::Frame f;
    for (const auto &bytes : {request, approval}) {
        check(bytes.size() == 10 && proto::parseDatagram(bytes, f), "10B parse failed");
        check(f.robotId == 7 && f.requestSequence == 0xa5 && f.transactionId == 0x12345678u
              && !f.fromStatus && !f.alive, "Revive request fields corrupted");
        for (int n = 0; n < 10; ++n)
            check(!proto::parseDatagram(bytes.left(n), f), "Truncated revive accepted");
        auto oversized = bytes; oversized.append(char(0));
        check(!proto::parseDatagram(oversized, f), "Oversized revive accepted");
        auto invalid = bytes; invalid[0] = 0;
        check(!proto::parseDatagram(invalid, f), "Bad magic accepted");
        invalid = bytes; invalid[2] = char(proto::kVersion1);
        check(!proto::parseDatagram(invalid, f), "V1 revive request accepted");
        invalid[2] = 3;
        check(!proto::parseDatagram(invalid, f), "Unknown version accepted");
    }
    check(proto::isDownlinkCommand(proto::TypeReviveApprove)
          && !proto::isDownlinkCommand(proto::TypeReviveRequest)
          && !proto::isReliableEvent(proto::TypeReviveRequest), "Direction/ACK classification wrong");
    check(proto::parseDatagram(proto::makeEventV1(proto::TypeRevive, 7, 1), f)
          && f.version == 1 && f.team == 1, "Legacy V1 revive broken");

    RobotManager manager;
    int requests = 0, events = 0, ackNeeds = 0;
    QObject::connect(&manager, &RobotManager::combatEvent, &manager,
                     [&](quint8, quint8, quint8) { ++events; });
    QObject::connect(&manager, &RobotManager::reliableEventNeedsAck, &manager,
                     [&](quint8, quint8, quint32, const QHostAddress &, quint16) { ++ackNeeds; });
    QObject::connect(&manager, &RobotManager::reviveRequested, &manager,
                     [&](quint8 id, quint8 seq, quint32 tx, quint32 ip, quint16 port) {
        check(id == 7 && seq == 0xa5 && tx == 0x12345678u
              && ip == QHostAddress(QStringLiteral("127.0.0.2")).toIPv4Address()
              && port == 51000, "Request source/identity lost");
        ++requests;
    });
    const QHostAddress source(QStringLiteral("127.0.0.2"));
    manager.handleDatagram(request, source, 51000);
    check(requests == 1 && manager.robots().isEmpty(), "Request invented an alive robot");
    manager.handleDatagram(proto::makeStatusV2(7, 0, 17, 20, false, false, true), source, 51000);
    const auto before = manager.robots().first();
    manager.markTransactionProcessed(7, proto::TypeDeath, 0x12345678u);
    manager.markTransactionProcessed(7, proto::TypeRevive, 0x12345678u);
    manager.markTransactionProcessed(7, proto::TypeReviveRequest, 0x12345678u);
    manager.handleDatagram(request, source, 51000);
    manager.handleDatagram(request, source, 51000);
    const auto after = manager.robots().first();
    check(requests == 3 && events == 0 && ackNeeds == 0, "Request mixed with event dedup/ACK");
    check(after.hp == before.hp && after.alive == before.alive
          && after.shootEnabled == before.shootEnabled && after.lastTxid == before.lastTxid
          && after.lastSeenMs == before.lastSeenMs, "Request mutated robot combat/telemetry");
    manager.handleDatagram(approval, source, 51000);
    check(requests == 3 && manager.robots().first().hp == 0, "Downlink approve became telemetry");
    manager.handleDatagram(proto::makeReviveRequest(0, 1, 2), source, 51000);
    manager.handleDatagram(proto::makeReviveRequest(7, 1, 0), source, 51000);
    manager.handleDatagram(request, QHostAddress::LocalHostIPv6, 51000);
    check(requests == 3, "Invalid request source/identity reached business logic");
}

void testCommanderLoopback()
{
    Server server;
    QUdpSocket robotA, robotB, foreign;
    const QHostAddress a(QStringLiteral("127.0.0.3")), b(QStringLiteral("127.0.0.4"));
    check(robotA.bind(a, quint16(0), QUdpSocket::DontShareAddress), "Robot A bind failed");
    check(robotB.bind(b, quint16(0), QUdpSocket::DontShareAddress), "Robot B bind failed");
    check(foreign.bind(QHostAddress::LocalHost, quint16(0), QUdpSocket::DontShareAddress),
          "Foreign bind failed");
    server.commander.learnEndpoint(11, a.toIPv4Address(), robotA.localPort());
    server.commander.learnEndpoint(12, b.toIPv4Address(), robotB.localPort());
    const quint32 tx = 0xa1b2c3d4u;
    QSettings commands(QSettings::defaultFormat(), QSettings::UserScope,
                       QStringLiteral("Scompetition"), QStringLiteral("RobotCommands"));
    commands.setValue(QStringLiteral("lastTxid"), tx - 1u); commands.sync();
    check(server.commander.sendForcePowerOn(11) == tx, "Normal command transaction allocation failed");
    check(server.commander.sendReviveApproval(11, 0x45, tx) == tx
          && server.commander.sendReviveApproval(12, 0x46, tx) == tx,
          "Approval did not reuse original transaction");
    check(server.commander.pendingCount() == 3, "Composite pending key collided");
    commands.sync();
    check(commands.value(QStringLiteral("lastTxid")).toUInt() == tx, "Approval allocated a new transaction");
    QVector<QByteArray> fromA, fromB;
    waitFor([&] {
        while (robotA.hasPendingDatagrams()) fromA.append(robotA.receiveDatagram().data());
        while (robotB.hasPendingDatagrams()) fromB.append(robotB.receiveDatagram().data());
        return fromA.size() >= 2 && !fromB.isEmpty();
    }, "Real approval datagrams missing");
    check(fromA[0] == proto::makeForcePowerOn(11, tx)
          && fromA[1] == proto::makeReviveApprove(11, 0x45, tx)
          && fromB[0] == proto::makeReviveApprove(12, 0x46, tx),
          "Commander changed original approval bytes/offset");
    server.commander.onAck(proto::TypeReviveApprove, tx, 0, 0);
    server.commander.onAckFrom(proto::TypeReviveApprove, tx, 0, QHostAddress(), 40000);
    check(server.commander.pendingCount() == 3 && server.confirmed.isEmpty(), "Unknown source confirmed");
    const auto sendAck = [&](QUdpSocket &from, quint8 type, quint8 result) {
        const int before = server.rawAcks;
        const auto bytes = proto::makeAck(type, tx, result);
        check(from.writeDatagram(bytes, QHostAddress::LocalHost, server.socket.localPort()) == bytes.size(),
              "Real ACK send failed");
        waitFor([&] { return server.rawAcks > before; }, "Real ACK not routed through manager");
    };
    sendAck(foreign, proto::TypeReviveApprove, 0);
    sendAck(robotA, proto::TypeGameEnd, 0);
    sendAck(robotA, proto::TypeReviveApprove, 3);
    check(server.commander.pendingCount() == 3 && server.confirmed.isEmpty(), "Foreign/type/invalid ACK accepted");
    sendAck(robotB, proto::TypeReviveApprove, 2);
    check(server.commander.pendingCount() == 2 && server.confirmed.last().target == 12
          && server.confirmed.last().result == 2, "Result2/source target mapping wrong");
    sendAck(robotA, proto::TypeForcePowerOn, 0);
    check(server.commander.pendingCount() == 1 && server.confirmed.last().type == proto::TypeForcePowerOn,
          "Normal ACK consumed same-tx approval");
    sendAck(robotA, proto::TypeReviveApprove, 0);
    check(server.commander.pendingCount() == 0 && server.confirmed.last().target == 11,
          "Approval ACK did not consume original composite key");
    sendAck(robotA, proto::TypeReviveApprove, 0);
    check(server.confirmed.size() == 3, "Duplicate ACK emitted repeated completion");

    // Preserve the trusted explicit-ID API used by existing normal tests.
    const quint32 normalTx = server.commander.sendForcePowerOn(11);
    server.commander.onAck(proto::TypeForcePowerOn, normalTx, 0, 11);
    check(server.commander.pendingCount() == 0, "Normal explicit-ID ACK compatibility broken");
    server.commander.sendReviveApproval(11, 1, tx);
    server.commander.sendReviveApproval(12, 2, tx);
    server.commander.sendGameEnd(11);
    server.commander.cancelPendingType(proto::TypeReviveApprove);
    check(server.commander.pendingCount() == 1, "Cancel type touched another type");
    server.commander.cancelPendingForTarget(11);
    check(server.commander.pendingCount() == 0, "Cancel target did not remove command");
    check(server.commander.sendReviveApproval(0, 1, tx) == 0
          && server.commander.sendReviveApproval(11, 1, 0) == 0
          && server.commander.sendReviveApproval(99, 1, tx) == 0, "Invalid approval sent");
    // Same-IP different targets with same type/tx cannot be attributed by F0.
    server.commander.learnEndpoint(12, a.toIPv4Address(), robotA.localPort());
    server.commander.sendReviveApproval(11, 1, tx);
    server.commander.sendReviveApproval(12, 2, tx);
    sendAck(robotA, proto::TypeReviveApprove, 0);
    check(server.commander.pendingCount() == 2, "Ambiguous F0 target was guessed");
    server.commander.cancelPending();
    qInfo() << "PASS: real UDP composite pending/echoed tx/source checks/result2/cancellation";
}

void startSimulator(SimulatorWindow &sim, Server &server, qint64 now = 1000)
{
    sim.setProtocolTimeForTest(now);
    control<QLineEdit>(sim, "simLocalAddress")->setText(QStringLiteral("127.0.0.2"));
    control<QSpinBox>(sim, "simServerPort")->setValue(server.socket.localPort());
    invoke(sim, "onToggleRunning");
    check(!control<QLineEdit>(sim, "simLocalAddress")->isEnabled(), "Simulator failed to bind");
}
int hp(SimulatorWindow &sim) { return control<QSpinBox>(sim, "simHp")->value(); }
void checkDead(SimulatorWindow &sim)
{
    check(hp(sim) == 0 && !control<QCheckBox>(sim, "simAlive")->isChecked()
          && !control<QCheckBox>(sim, "simShoot")->isChecked(), "Approval wait did not stay dead/HP0");
}
void statusBarrier(SimulatorWindow &sim, Server &server)
{
    const int before = server.countType(proto::TypeStatus);
    invoke(sim, "sendStatus");
    waitFor([&] { return server.countType(proto::TypeStatus) > before; }, "Status barrier failed");
}
void testSimulatorLoopback()
{
    Server server;
    auto sim = std::make_unique<SimulatorWindow>();
    startSimulator(*sim, server);
    waitFor([&] { return !server.manager.devices().isEmpty(); }, "Simulator announce missing");
    const auto device = server.manager.devices().first();
    QString macText = device.deviceMac;
    const auto mac = QByteArray::fromHex(macText.remove(QLatin1Char(':')).toLatin1());
    server.command(proto::makeAssignment(7, mac, QByteArray::fromHex("112233445566"), 10),
                   proto::TypeAssignment, 10, 0);
    check(hp(*sim) == 200, "Preparation initial HP wrong");
    invoke(*sim, "onSendDeath");
    sim->advanceProtocolTimeForTest(4999); checkDead(*sim);
    sim->advanceProtocolTimeForTest(1);
    check(hp(*sim) == 200, "Preparation did not auto-revive at exactly 5s");
    check(server.requests.isEmpty(), "Preparation sent formal request");

    server.command(proto::makeGameStart(7, 20), proto::TypeGameStart, 20, 0);
    check(hp(*sim) == 300, "Formal GAME_START HP wrong");
    invoke(*sim, "onSendDeath");
    invoke(*sim, "onSendRevive"); checkDead(*sim);
    sim->advanceProtocolTimeForTest(4999); statusBarrier(*sim, server);
    checkDead(*sim); check(server.requests.isEmpty(), "Formal request before 5s");
    sim->advanceProtocolTimeForTest(1);
    waitFor([&] { return server.requests.size() == 1; }, "Formal request missing at 5s");
    const Request original = server.requests.first();
    check(original.id == 7 && original.tx && original.ipv4
          == QHostAddress(QStringLiteral("127.0.0.2")).toIPv4Address(), "Request identity/source wrong");
    checkDead(*sim);
    server.sendTo(proto::makeAck(proto::TypeReviveRequest, original.tx, 0),
                  QHostAddress(original.ipv4), original.port);
    statusBarrier(*sim, server);
    const int beforeRetry = server.requests.size();
    sim->advanceProtocolTimeForTest(100);
    waitFor([&] { return server.requests.size() > beforeRetry; }, "F0 improperly cleared request");
    check(server.requests.last().sequence == original.sequence && server.requests.last().tx == original.tx,
          "Retry replaced request identity");
    server.command(proto::makeSetHp(7, 300, 21), proto::TypeSetHp, 21, 1); checkDead(*sim);
    server.command(proto::makeReviveApprove(7, quint8(original.sequence + 1), original.tx),
                   proto::TypeReviveApprove, original.tx, 1); checkDead(*sim);
    server.command(proto::makeReviveApprove(7, original.sequence, original.tx + 1),
                   proto::TypeReviveApprove, original.tx + 1, 1); checkDead(*sim);

    // Destroy/recreate the device: the exact request survives in isolated INI settings.
    const int beforeRestart = server.requests.size();
    sim.reset();
    sim = std::make_unique<SimulatorWindow>();
    startSimulator(*sim, server, 11100);
    checkDead(*sim);
    sim->advanceProtocolTimeForTest(0);
    waitFor([&] { return server.requests.size() > beforeRestart; }, "Restart lost persisted request");
    check(server.requests.last().tx == original.tx && server.requests.last().sequence == original.sequence,
          "Restart allocated replacement request tx/seq");
    const int reviveBefore = server.countType(proto::TypeRevive);
    check(server.commander.sendReviveApproval(7, original.sequence, original.tx) == original.tx,
          "Approval send failed");
    waitFor([&] { return hp(*sim) == 300 && server.commander.pendingCount() == 0; },
            "Matching approval failed to revive/ACK");
    waitFor([&] { return server.countType(proto::TypeRevive) > reviveBefore; }, "No reliable revive event");
    const int reviveAfter = server.countType(proto::TypeRevive);
    invoke(*sim, "onSendHit"); check(hp(*sim) == 280, "Post-revive hit failed");
    server.command(proto::makeReviveApprove(7, original.sequence, original.tx),
                   proto::TypeReviveApprove, original.tx, 0);
    check(hp(*sim) == 280 && server.countType(proto::TypeRevive) == reviveAfter,
          "Duplicate approval healed/executed twice");
    const int requestsBefore = server.requests.size();
    invoke(*sim, "onSendDeath"); sim->advanceProtocolTimeForTest(5000);
    waitFor([&] { return server.requests.size() > requestsBefore; }, "Second death did not request");
    const Request second = server.requests.last();
    check(second.tx != original.tx && second.sequence != original.sequence, "New death reused old identity");
    const int beforeSecondRestart = server.requests.size();
    sim.reset();
    sim = std::make_unique<SimulatorWindow>();
    startSimulator(*sim, server, 16100);
    sim->advanceProtocolTimeForTest(0);
    waitFor([&] { return server.requests.size() > beforeSecondRestart; }, "Second persisted request lost");
    check(server.requests.last().tx == second.tx && server.requests.last().sequence == second.sequence,
          "Second restart changed request identity");
    server.command(proto::makeReviveApprove(7, original.sequence, original.tx),
                   proto::TypeReviveApprove, original.tx, 0); checkDead(*sim);
    // END cancels the request and rejects even previously successful approvals.
    server.command(proto::makeGameEnd(7, 30), proto::TypeGameEnd, 30, 0);
    const int endedRequests = server.requests.size();
    sim->advanceProtocolTimeForTest(10000); statusBarrier(*sim, server);
    check(server.requests.size() == endedRequests, "END did not cancel local request retries");
    sim.reset();
    sim = std::make_unique<SimulatorWindow>();
    startSimulator(*sim, server, 26100);
    checkDead(*sim);
    sim->advanceProtocolTimeForTest(1000); statusBarrier(*sim, server);
    check(server.requests.size() == endedRequests, "Restart resurrected END-cancelled request");
    server.command(proto::makeReviveApprove(7, second.sequence, second.tx),
                   proto::TypeReviveApprove, second.tx, 1); checkDead(*sim);
    server.command(proto::makeReviveApprove(7, original.sequence, original.tx),
                   proto::TypeReviveApprove, original.tx, 1); checkDead(*sim);
    server.command(proto::makeGameStart(7, 31), proto::TypeGameStart, 31, 0);
    server.command(proto::makeReviveApprove(7, second.sequence, second.tx),
                   proto::TypeReviveApprove, second.tx, 1);
    check(hp(*sim) == 300, "Old-epoch approval affected new GAME_START");

    // Quotas are owned by the main thread: four approvals here must all work.
    for (quint32 i = 0; i < 4; ++i) {
        const int count = server.requests.size();
        invoke(*sim, "onSendDeath"); sim->advanceProtocolTimeForTest(5000);
        waitFor([&] { return server.requests.size() > count; }, "Simulator hardcoded revive quota");
        const auto r = server.requests.last();
        server.command(proto::makeReviveApprove(7, r.sequence, r.tx), proto::TypeReviveApprove, r.tx, 0);
        check(hp(*sim) == 300, "Fourth revive blocked by simulator quota");
    }
    server.command(proto::makeYellowCard(7, 40), proto::TypeYellowCard, 40, 0);
    server.command(proto::makeYellowCard(7, 41), proto::TypeYellowCard, 41, 0);
    server.command(proto::makeYellowCard(7, 42), proto::TypeYellowCard, 42, 0);
    const int disqualifiedRequests = server.requests.size();
    sim->advanceProtocolTimeForTest(5000); statusBarrier(*sim, server);
    invoke(*sim, "onSendRevive"); checkDead(*sim);
    check(server.requests.size() == disqualifiedRequests, "Third yellow card still requested revive");
    server.command(proto::makeSetHp(7, 300, 43), proto::TypeSetHp, 43, 1);

    // V1 direct revive stays compatible, with no request/approval gate.
    invoke(*sim, "onToggleRunning");
    control<QComboBox>(*sim, "simProtocolVersion")->setCurrentIndex(0);
    control<QSpinBox>(*sim, "simRobotId")->setValue(7);
    invoke(*sim, "onToggleRunning");
    invoke(*sim, "onSendDeath"); invoke(*sim, "onSendRevive");
    check(hp(*sim) == 5000 && control<QCheckBox>(*sim, "simAlive")->isChecked(), "V1 direct revive regressed");
    qInfo() << "PASS: real UDP simulator 5s gate/persistence/F0/non-bypass/dedup/END/new epoch/no quota/V1";
}
} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QTemporaryDir settings;
    if (!settings.isValid()) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());
    try {
        testCodecAndManager();
        testCommanderLoopback();
        testSimulatorLoopback();
    } catch (const std::exception &error) {
        qCritical() << "Revive protocol test failed:" << error.what();
        return 1;
    }
    qInfo() << "Revive protocol test passed";
    return 0;
}
