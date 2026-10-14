#include "protocol.h"
#include "matchserver.h"
#include "robotmanager.h"

#include <QCoreApplication>
#include <QDebug>
#include <QHostAddress>
#include <QSettings>
#include <QTemporaryDir>

namespace {

int fail(const QString &message)
{
    qCritical().noquote() << message;
    return 1;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) return fail(QStringLiteral("Cannot isolate test settings"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());
    const QHostAddress sender(QStringLiteral("127.0.0.1"));
    const QByteArray deviceMac = QByteArray::fromHex("112233445566");
    const QByteArray controllerMac = QByteArray::fromHex("8D23ABA53CC9");

    RobotManager manager;
    manager.handleDatagram(proto::makeDeviceAnnounce(0, deviceMac, controllerMac,
                                                     3, 4), sender, 51000);
    const QVector<RobotManager::DeviceInfo> devices = manager.devices();
    if (devices.size() != 1 || devices.first().deviceMac != QStringLiteral("11:22:33:44:55:66")
        || devices.first().controllerMac != QStringLiteral("8D:23:AB:A5:3C:C9")
        || devices.first().ipv4 == 0) {
        return fail(QStringLiteral("DEVICE_ANNOUNCE 状态未正确登记"));
    }

    if (devices.first().firstSeenMs <= 0 || devices.first().firstSeenMs != devices.first().lastSeenMs)
        return fail(QStringLiteral("Device first uplink timestamp not recorded"));
    manager.handleDatagram(proto::makeDeviceAnnounce(0, deviceMac, controllerMac, 3, 4), sender, 51000);
    if (manager.devices().first().firstSeenMs != devices.first().firstSeenMs)
        return fail(QStringLiteral("Device first uplink timestamp overwritten"));

    manager.handleDatagram(proto::makeStatusV2(7, 300, 120, 45, true, true, true),
                           sender, 51000);
    QVector<RobotManager::RobotInfo> robots = manager.robots();
    if (robots.size() != 1 || robots.first().robotId != 7
        || robots.first().addressText != QStringLiteral("127.0.0.1")
        || !robots.first().powerOn || !robots.first().shootEnabled) {
        return fail(QStringLiteral("V2 STATUS 状态未正确更新"));
    }

    if (robots.first().firstSeenMs <= 0 || robots.first().healthReceived
        || robots.first().lastHealthSeenMs != 0)
        return fail(QStringLiteral("Unknown health mistaken for a received frame"));
    const qint64 firstRobotUplink = robots.first().firstSeenMs;
    manager.handleDatagram(proto::makeDeviceHealth(7, 0x1f), sender, 51000);
    robots = manager.robots();
    if (robots.first().componentOnline != 0x1f || !robots.first().linkUp
        || !robots.first().healthReceived || robots.first().lastHealthSeenMs <= 0
        || robots.first().firstSeenMs != firstRobotUplink)
        return fail(QStringLiteral("DEVICE_HEALTH 状态未正确更新"));

    manager.handleDatagram(proto::makeEventV2(proto::TypeCombatEnd, 7),
                           sender, 51000);
    robots = manager.robots();
    if (!robots.first().shootEnabled)
        return fail(QStringLiteral("COMBAT_END 不应改变射击许可"));

    // This is the former crash path: publishRobots() serializes RobotInfo and
    // used to call the stale QHostAddress copy after it had become invalid.
    MatchServer publisher;
    publisher.publishRobots(robots);

    const QByteArray assignment = proto::makeAssignment(7, deviceMac, controllerMac, 42);
    proto::Frame frame;
    if (assignment.size() != 21 || !proto::parseDatagram(assignment, frame)
        || frame.deviceMac != deviceMac || frame.controllerMac != controllerMac
        || frame.transactionId != 42) {
        return fail(QStringLiteral("21 字节 ASSIGNMENT 编解码失败"));
    }

    QByteArray malformed = proto::makeEventV2(proto::TypeAttack, 7);
    malformed.chop(1);
    proto::Frame ignored;
    if (proto::parseDatagram(malformed, ignored))
        return fail(QStringLiteral("错误长度帧未被拒绝"));

    const QByteArray reviveRequest = proto::makeReviveRequest(7, 0xa5, 0x12345678u);
    const QByteArray reviveApprove = proto::makeReviveApprove(7, 0xa5, 0x12345678u);
    if (reviveRequest != QByteArray::fromHex("5452020d07a578563412")
        || reviveApprove != QByteArray::fromHex("5452028907a578563412")
        || !proto::parseDatagram(reviveRequest, frame) || frame.requestSequence != 0xa5
        || frame.transactionId != 0x12345678u || frame.fromStatus
        || !proto::parseDatagram(reviveApprove, frame)
        || !proto::isDownlinkCommand(frame.type)
        || proto::isReliableEvent(proto::TypeReviveRequest))
        return fail(QStringLiteral("10B revive request/approval golden bytes or parsing failed"));
    for (const QByteArray &valid : {reviveRequest, reviveApprove}) {
        QByteArray extra = valid; extra.append(char(0));
        QByteArray wrongVersion = valid; wrongVersion[2] = char(proto::kVersion1);
        if (proto::parseDatagram(valid.left(9), frame)
            || proto::parseDatagram(extra, frame) || proto::parseDatagram(wrongVersion, frame))
            return fail(QStringLiteral("Revive frames must be strictly V2 10B"));
    }
    int requests = 0;
    bool requestFieldsCorrect = true;
    QObject::connect(&manager, &RobotManager::reviveRequested, &application,
                     [&](quint8 id, quint8 seq, quint32 tx, quint32 ip, quint16 port) {
        ++requests;
        requestFieldsCorrect = requestFieldsCorrect && id == 7 && seq == 0xa5
            && tx == 0x12345678u && ip == sender.toIPv4Address() && port == 51000;
    });
    manager.markTransactionProcessed(7, proto::TypeDeath, 0x12345678u);
    manager.markTransactionProcessed(7, proto::TypeReviveRequest, 0x12345678u);
    const auto beforeRequest = manager.robots().first();
    manager.handleDatagram(reviveRequest, sender, 51000);
    manager.handleDatagram(reviveRequest, sender, 51000);
    const auto afterRequest = manager.robots().first();
    if (requests != 2 || !requestFieldsCorrect || beforeRequest.hp != afterRequest.hp
        || beforeRequest.alive != afterRequest.alive
        || beforeRequest.lastSeenMs != afterRequest.lastSeenMs)
        return fail(QStringLiteral("Repeated requests must bypass event dedup and not mutate telemetry"));

    qInfo().noquote() << QStringLiteral("Robot protocol smoke test passed");
    return 0;
}
