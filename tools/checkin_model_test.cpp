#include "checkinmodel.h"

#include <QCoreApplication>
#include <QDebug>

namespace {
int failures = 0;
void require(bool condition, const char *description)
{
    if (!condition) {
        ++failures;
        qCritical().noquote() << "FAIL:" << description;
    }
}
checkin::RobotSnapshot robot(quint8 id, qint64 seen, bool online = true)
{
    checkin::RobotSnapshot sample;
    sample.robotId = id;
    sample.protocolVersion = 2;
    sample.online = online;
    sample.lastSeenMs = seen;
    sample.addressText = QStringLiteral("192.0.2.7");
    sample.sourcePort = 51000;
    return sample;
}
checkin::DeviceSnapshot device(quint8 id, qint64 seen)
{
    checkin::DeviceSnapshot sample;
    sample.deviceMac = QStringLiteral("11:22:33:44:55:66");
    sample.robotId = id;
    sample.lastSeenMs = seen;
    sample.addressText = QStringLiteral("192.0.2.7");
    sample.sourcePort = 51000;
    return sample;
}
bool allUnknown(const checkin::Row &row)
{
    for (const auto health : row.modules)
        if (health != checkin::Health::Unknown) return false;
    return true;
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    using namespace checkin;
    CheckinModel model;
    auto r = robot(7, 800);
    model.updateSnapshot({r}, {}, 1000);
    model.observeDeviceHealth(7, 0x3f, r.addressText, 1000);
    require(model.rows().isEmpty(), "disabled model must not record snapshots or evidence");
    model.setEnabled(true, 1000);
    model.updateSnapshot({r}, {}, 1000);
    require(model.rows().size() == 1, "online robot creates row");
    require(model.rows()[0].firstConnectedMs == 1000,
            "first connection is observation time, NOT older lastSeen");
    require(allUnknown(model.rows()[0]) && !model.rows()[0].healthFresh,
            "default zero bitmap / default linkUp must not mean fault or healthy");
    require(!model.rows()[0].notes.isEmpty(), "no status/health must have unknown explanation");

    r.lastSeenMs = 1100;
    r.componentOnline = 0x3f;
    model.updateSnapshot({r}, {}, 1100);
    require(allUnknown(model.rows()[0]), "nonzero snapshot bitmap still needs validated frame evidence");
    model.observeDeviceHealth(7, 0x3f, r.addressText, 1100);
    model.updateSnapshot({r}, {}, 1100);
    require(model.rows()[0].healthFresh, "validated fresh health evidence confirms bitmap");
    const auto healthyModules = model.rows()[0].modules;
    for (const auto health : healthyModules)
        require(health == Health::Healthy, "all six documented bits should be healthy for 0x3f");
    require(model.rows()[0].firstConnectedMs == 1000, "refresh cannot replace first connection");
    r.componentOnline = 0x1f;
    model.updateSnapshot({r}, {}, 1101);
    require(allUnknown(model.rows()[0]), "inconsistent snapshot and health evidence cannot be confirmed");

    r.componentOnline = 0;
    r.lastSeenMs = 1200;
    model.observeDeviceHealth(7, 0, r.addressText, 1200);
    model.updateSnapshot({r}, {}, 1200);
    require(model.rows()[0].healthFresh, "explicit all-zero health is distinguishable from missing health");
    const auto failedModules = model.rows()[0].modules;
    for (const auto health : failedModules)
        require(health == Health::Fault, "all-zero confirmed health marks six offline modules");
    require(model.rows()[0].issues.size() == 6, "each failing module has its own explanation");

    // Test each defined bit independently; high reserved bits never stand for extra modules.
    for (int bit = 0; bit < 6; ++bit) {
        const qint64 now = 1300 + bit;
        r.componentOnline = quint8(0x3fU & ~(1U << bit));
        r.lastSeenMs = now;
        model.observeDeviceHealth(7, r.componentOnline, r.addressText, now);
        model.updateSnapshot({r}, {}, now);
        for (int other = 0; other < 6; ++other)
            require(model.rows()[0].modules[std::size_t(other)]
                == (other == bit ? Health::Fault : Health::Healthy), "documented bit maps to correct module");
    }
    r.componentOnline = 0xff;
    r.lastSeenMs = 1400;
    model.observeDeviceHealth(7, 0xff, r.addressText, 1400);
    model.updateSnapshot({r}, {}, 1400);
    require(model.rows()[0].issues.isEmpty(), "reserved bits are not faults");
    require(model.rows()[0].modules.size() == 6, "reserved bits do not create invented modules");

    model.observeDeviceHealth(7, 0, r.addressText, 1399);
    model.updateSnapshot({r}, {}, 1400);
    require(model.rows()[0].confirmedBitmap == 0xff, "out-of-order health cannot overwrite latest evidence");
    model.updateSnapshot({r}, {}, 4400);
    require(model.rows()[0].healthFresh, "health is valid at the exact 3000ms boundary");
    model.updateSnapshot({r}, {}, 4401);
    require(allUnknown(model.rows()[0]), "expired health is unknown, not healthy history");

    r.componentOnline = 0x3f;
    r.lastSeenMs = 4500;
    model.observeDeviceHealth(7, 0x3f, QStringLiteral("192.0.2.99"), 4500);
    model.updateSnapshot({r}, {}, 4500);
    require(allUnknown(model.rows()[0]), "wrong endpoint cannot validate this robot's health");
    model.observeDeviceHealth(7, 0x3f, r.addressText, 4501);
    r.lastSeenMs = 4501;
    model.updateSnapshot({r}, {}, 4501);
    require(model.rows()[0].healthFresh, "correct endpoint restores health evidence");
    r.online = false;
    model.updateSnapshot({r}, {}, 4600);
    require(model.rows()[0].offlineObservedMs == 4600 && allUnknown(model.rows()[0]),
            "offline robot must not retain current healthy modules");
    r.online = true;
    r.lastSeenMs = 4700;
    model.updateSnapshot({r}, {}, 4700);
    require(model.rows()[0].firstConnectedMs == 1000 && allUnknown(model.rows()[0]),
            "reconnect keeps first connection but requires new health evidence");
    model.observeDeviceHealth(7, 0x3f, r.addressText, 4700);
    model.updateSnapshot({r}, {}, 4700);
    require(model.rows()[0].healthFresh, "new evidence confirms reconnected robot");
    r.team = 2;
    model.updateSnapshot({r}, {}, 4800);
    require(model.rows().size() == 1 && model.rows()[0].firstConnectedMs == 1000,
            "V2 team remapping does not change identity or first time");

    model.updateSnapshot({}, {}, 4900);
    require(model.rows().size() == 1 && model.rows()[0].connection == Connection::Offline,
            "removed snapshot retains history as offline");
    r.lastSeenMs = 5000;
    model.updateSnapshot({r}, {}, 5000);
    require(model.rows()[0].firstConnectedMs == 1000, "reappearance retains session history");
    model.setEnabled(false, 5100);
    model.updateSnapshot({}, {}, 5200);
    require(model.rows()[0].lastSeenMs == 5000, "exit freezes data rather than observing background changes");
    model.setEnabled(true, 5300);
    model.updateSnapshot({r}, {}, 5300);
    require(model.rows()[0].firstConnectedMs == 1000 && allUnknown(model.rows()[0]),
            "reopening same session keeps first connection, invalidates old health evidence");
    model.startNewSession(6000);
    r.lastSeenMs = 5990;
    model.updateSnapshot({r}, {}, 6000);
    require(model.rows()[0].firstConnectedMs == 6000 && model.sessionNumber() == 2,
            "new session explicitly resets first connection records");
    model.observeDeviceHealth(7, 0x3f, r.addressText, 5900);
    model.updateSnapshot({r}, {}, 6000);
    require(allUnknown(model.rows()[0]), "prior-session evidence must be ignored");

    CheckinModel registry;
    registry.startNewSession(1000);
    auto d = device(0, 900);
    registry.updateSnapshot({}, {d}, 1000);
    require(registry.rows().size() == 1 && registry.rows()[0].robotId == 0,
            "unassigned MAC remains visible without fake robot telemetry");
    require(registry.rows()[0].deviceMac == d.deviceMac
        && registry.rows()[0].connection == Connection::RegistryRecent
        && registry.rows()[0].firstConnectedMs == 1000 && allUnknown(registry.rows()[0]),
        "registry observation includes MAC, address, first observation and unknown health");
    registry.updateSnapshot({}, {d}, 6901);
    require(registry.rows()[0].connection == Connection::RegistryStale,
            "registration freshness uses separate longer window, not 800ms robot timeout");
    d.robotId = 7;
    d.lastSeenMs = 7000;
    r = robot(7, 7000);
    registry.observeDeviceHealth(7, 0x3f, r.addressText, 7000);
    r.componentOnline = 0x3f;
    registry.updateSnapshot({r}, {d}, 7000);
    require(registry.rows().size() == 1 && registry.rows()[0].firstConnectedMs == 1000,
            "stable MAC keeps first observation through assignment");
    require(registry.rows()[0].healthFresh, "validated health with new assignment can be confirmed");

    // Robot may precede registration; join later without losing first connection.
    CheckinModel lateRegistry;
    lateRegistry.startNewSession(1000);
    r = robot(7, 1000);
    lateRegistry.updateSnapshot({r}, {}, 1000);
    d = device(7, 1500);
    r.lastSeenMs = 1500;
    lateRegistry.updateSnapshot({r}, {d}, 1500);
    require(lateRegistry.rows().size() == 1 && lateRegistry.rows()[0].firstConnectedMs == 1000,
            "late registry merges provisional robot identity without duplicate row");

    auto secondDevice = d;
    secondDevice.deviceMac = QStringLiteral("AA:BB:CC:DD:EE:FF");
    CheckinModel conflict;
    conflict.startNewSession(1000);
    conflict.updateSnapshot({r}, {d, secondDevice}, 1500);
    require(conflict.rows().size() == 3, "ambiguous duplicate ID must expose both devices and robot");
    for (const auto &row : conflict.rows())
        require(!row.issues.isEmpty(), "duplicate ID must be explicitly reported, never silently pick a MAC");

    CheckinModel legacy;
    legacy.startNewSession(1000);
    r = robot(4, 1000);
    r.protocolVersion = 1;
    r.team = 1;
    r.componentOnline = 0xff;
    r.power = 99;
    r.heat = -1;
    legacy.observeDeviceHealth(4, 0xff, r.addressText, 1000);
    legacy.updateSnapshot({r}, {}, 1000);
    require(allUnknown(legacy.rows()[0]) && legacy.rows()[0].power == -1
        && legacy.rows()[0].heat == -1, "V1 unavailable health/power/optional heat cannot become normal defaults");
    auto blue = r;
    blue.team = 2;
    legacy.updateSnapshot({r, blue}, {}, 1100);
    require(legacy.rows().size() == 2, "V1 same robot ID in distinct teams must remain separate");

    // Server-process first uplink is separate from the session's observation time.
    CheckinModel serverFirst;
    serverFirst.startNewSession(1000);
    r = robot(7, 950);
    r.firstSeenMs = 600;
    d = device(7, 950);
    d.firstSeenMs = 500;
    serverFirst.updateSnapshot({r}, {d}, 1000);
    require(serverFirst.rows()[0].serverFirstSeenMs == 500
        && serverFirst.rows()[0].firstConnectedMs == 1000,
        "MAC-bound server first announce and inspection first observation stay distinct");
    serverFirst.startNewSession(2000);
    serverFirst.updateSnapshot({r}, {d}, 2000);
    require(serverFirst.rows()[0].serverFirstSeenMs == 500
        && serverFirst.rows()[0].firstConnectedMs == 2000,
        "new inspection session cannot reset server-process first uplink");
    r.firstSeenMs = 0;
    d.firstSeenMs = 0;
    serverFirst.updateSnapshot({r}, {d}, 2100);
    require(serverFirst.rows()[0].serverFirstSeenMs == 0,
        "missing server first uplink must not be manufactured from lastSeen");
    CheckinModel deviceFirst;
    deviceFirst.startNewSession(1000);
    d = device(0, 950);
    d.firstSeenMs = 400;
    deviceFirst.updateSnapshot({}, {d}, 1000);
    require(deviceFirst.rows()[0].serverFirstSeenMs == 400,
        "unassigned registry retains actual server first announce");

    CheckinModel firstHealth;
    firstHealth.startNewSession(1000);
    r = robot(7, 1100);
    r.componentOnline = 0x3f;
    firstHealth.observeDeviceHealth(7, 0x3f, r.addressText, 1100);
    firstHealth.updateSnapshot({r}, {}, 1101);
    require(firstHealth.rows()[0].healthFresh,
        "new identity accepts its first health when observe and snapshot cross a millisecond");

    // A stable MAC changing assigned ID/IP cannot reuse a pre-binding health frame.
    CheckinModel rebound;
    rebound.startNewSession(1000);
    r = robot(7, 1000);
    r.firstSeenMs = 900;
    r.componentOnline = 0x3f;
    d = device(7, 1000);
    d.firstSeenMs = 800;
    rebound.observeDeviceHealth(7, 0x3f, r.addressText, 1000);
    rebound.updateSnapshot({r}, {d}, 1000);
    require(rebound.rows()[0].healthFresh,
        "new row accepts validated first health frame before its first snapshot");
    auto reassigned = robot(8, 1500);
    reassigned.firstSeenMs = 1500;
    reassigned.componentOnline = 0x3f;
    d.robotId = 8;
    d.lastSeenMs = 2000;
    rebound.observeDeviceHealth(8, 0x3f, reassigned.addressText, 1500);
    rebound.updateSnapshot({reassigned}, {d}, 2000);
    require(rebound.rows().size() == 1 && rebound.rows()[0].robotId == 8
        && rebound.rows()[0].firstConnectedMs == 1000
        && rebound.rows()[0].serverFirstSeenMs == 800 && allUnknown(rebound.rows()[0]),
        "MAC reassignment preserves first times but rejects health cached before binding change");
    rebound.observeDeviceHealth(8, 0x3f, reassigned.addressText, 2000);
    rebound.updateSnapshot({reassigned}, {d}, 2000);
    require(rebound.rows()[0].healthFresh,
        "health at the assignment observation boundary can confirm the new identity");
    reassigned.addressText = QStringLiteral("192.0.2.8");
    reassigned.lastSeenMs = 2100;
    d.addressText = reassigned.addressText;
    rebound.observeDeviceHealth(8, 0x3f, reassigned.addressText, 2100);
    rebound.updateSnapshot({reassigned}, {d}, 2500);
    require(allUnknown(rebound.rows()[0]),
        "address change cannot reuse health received before its observation");
    rebound.observeDeviceHealth(8, 0x3f, reassigned.addressText, 2500);
    rebound.updateSnapshot({reassigned}, {d}, 2500);
    require(rebound.rows()[0].healthFresh,
        "fresh evidence after address change restores module status");

    CheckinModel offlineFirst;
    offlineFirst.startNewSession(1000);
    r = robot(9, 800, false);
    offlineFirst.updateSnapshot({r}, {}, 1000);
    require(offlineFirst.rows()[0].firstConnectedMs == 0,
            "first observed offline record cannot manufacture first connection from lastSeen");
    r.online = true;
    r.lastSeenMs = 2000;
    offlineFirst.updateSnapshot({r}, {}, 2000);
    require(offlineFirst.rows()[0].firstConnectedMs == 2000,
            "offline-first robot records first subsequent actual online observation");

    if (failures) {
        qCritical() << failures << "check-in model assertions failed";
        return 1;
    }
    qInfo() << "Check-in model deterministic tests passed";
    return 0;
}
