#ifndef CHECKINMODEL_H
#define CHECKINMODEL_H

#include <QHash>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>
#include <array>
#include <cstddef>

// Pure snapshot reducer: no QObject, clocks, sockets, settings, or match state.
// All times are explicitly supplied Unix epoch milliseconds by the caller.
namespace checkin {

enum class Health { Unknown, Healthy, Fault };
enum class Connection { Unknown, Online, Offline, RegistryRecent, RegistryStale };

struct RobotSnapshot {
    quint8 robotId = 0;
    quint8 team = 0;
    quint8 protocolVersion = 0;
    bool online = false;
    qint64 lastSeenMs = 0;
    qint64 firstSeenMs = 0; // First valid uplink recorded by this server process.
    QString addressText;
    quint16 sourcePort = 0;
    int hp = -1;
    int heat = -1;
    int power = -1;
    quint8 componentOnline = 0; // Raw snapshot only; NOT proof a health frame arrived.
};

struct DeviceSnapshot {
    QString deviceMac;
    QString controllerMac;
    QString addressText;
    quint16 sourcePort = 0;
    quint8 robotId = 0;
    qint64 lastSeenMs = 0;
    qint64 firstSeenMs = 0; // First announce recorded by this server process.
    quint32 eventQueueDrops = 0;
    quint32 udpSendFailures = 0;
};

struct Row {
    QString key;
    quint8 robotId = 0;
    quint8 team = 0;
    quint8 protocolVersion = 0;
    bool hasRobotSnapshot = false;
    QString deviceMac;
    QString controllerMac;
    QString addressText;
    quint16 sourcePort = 0;
    Connection connection = Connection::Unknown;
    qint64 firstConnectedMs = 0; // First observed connected IN THIS SESSION, never lastSeen.
    qint64 serverFirstSeenMs = 0; // First valid uplink/announce in the server process, not boot time.
    qint64 lastSeenMs = 0;       // Robot uplink if present, otherwise device announce.
    qint64 lastAnnounceMs = 0;
    qint64 offlineObservedMs = 0;
    int hp = -1;
    int heat = -1;
    int power = -1;
    quint8 rawBitmap = 0;
    quint8 confirmedBitmap = 0;
    qint64 healthReceivedMs = 0;
    bool healthFresh = false;
    std::array<Health, 6> modules {{Health::Unknown, Health::Unknown, Health::Unknown,
                                   Health::Unknown, Health::Unknown, Health::Unknown}};
    QStringList issues;
    QStringList notes;
    quint32 eventQueueDrops = 0;
    quint32 udpSendFailures = 0;
};

class CheckinModel
{
public:
    struct Timing {
        qint64 healthFreshnessMs = 3000;
        qint64 announceFreshnessMs = 6000;
    };

    CheckinModel();
    explicit CheckinModel(const Timing &timing);
    void setEnabled(bool enabled, qint64 observedAtMs);
    bool isEnabled() const { return m_enabled; }
    void startNewSession(qint64 observedAtMs); // Clears history/evidence and enables inspection.
    qint64 sessionStartedMs() const { return m_sessionStartedMs; }
    quint64 sessionNumber() const { return m_sessionNumber; }
    qint64 lastUpdatedMs() const { return m_lastUpdatedMs; }

    void updateSnapshot(const QVector<RobotSnapshot> &robots,
                        const QVector<DeviceSnapshot> &devices, qint64 observedAtMs);
    // Only call after a validated V2 DEVICE_HEALTH uplink, never from a default bitmap.
    void observeDeviceHealth(quint8 robotId, quint8 bitmap, const QString &addressText,
                             qint64 receivedAtMs);
    QVector<Row> rows() const;
    static QString moduleName(int bit);
    static QString healthText(Health health);
    static QString connectionText(Connection connection);

private:
    struct Record {
        Row row;
        qint64 minimumHealthMs = 0;
    };
    struct Evidence {
        quint8 bitmap = 0;
        QString addressText;
        qint64 receivedAtMs = 0;
    };
    void finishRow(Record &record, qint64 observedAtMs);
    Timing m_timing;
    bool m_enabled = false;
    qint64 m_sessionStartedMs = 0;
    qint64 m_lastUpdatedMs = 0;
    quint64 m_sessionNumber = 0;
    QMap<QString, Record> m_records;
    QHash<quint8, Evidence> m_health;
};

} // namespace checkin
#endif // CHECKINMODEL_H
