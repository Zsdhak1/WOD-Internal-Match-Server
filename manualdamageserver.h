#ifndef MANUALDAMAGESERVER_H
#define MANUALDAMAGESERVER_H

#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QVector>
#include <functional>

class QTcpServer;
class QTcpSocket;
class QTimer;

// Minimal LAN HTTP service for the manual HP deduction page (no Qt HttpServer
// dependency). Routes:
//   GET  /, /red, /blue      mobile page (the path preselects a side)
//   GET  /api/state?team=red current side state (JSON, from the state provider)
//   POST /api/damage         {"team":"red","requestId":"..."}; answered only
//                            after the owner calls completeDamage().
// One request per connection. requestId makes phone retries idempotent: a
// retry joins the in-flight request or receives the cached result, and never
// triggers a second deduction. There is intentionally no authentication; the
// page is meant for an isolated match Wi-Fi.
class ManualDamageServer : public QObject
{
    Q_OBJECT
public:
    using StateProvider = std::function<QJsonObject(quint8 team)>;

    explicit ManualDamageServer(QObject *parent = nullptr);
    ~ManualDamageServer() override;

    bool start(quint16 port);   // port 0 picks a free port (tests)
    void stop();
    bool isListening() const;
    quint16 port() const;
    void setStateProvider(StateProvider provider) { m_stateProvider = std::move(provider); }

    // Finish a damageRequested ticket exactly once. result must contain "ok".
    void completeDamage(quint64 ticket, const QJsonObject &result);

    static QStringList lanAddresses();               // non-loopback IPv4
    static QImage qrCode(const QString &text, int moduleSize = 4, int border = 2);

signals:
    void damageRequested(quint64 ticket, quint8 team, const QString &peer);
    void logMessage(const QString &message);

private:
    struct Request {
        QByteArray method;
        QByteArray path;
        QByteArray query;
        QByteArray body;
    };
    struct Ticket {
        QString requestId;
        QVector<QPointer<QTcpSocket>> sockets;
    };

    void onNewConnection();
    void onReadyRead(QTcpSocket *socket);
    void handle(QTcpSocket *socket, const Request &request);
    void handleDamage(QTcpSocket *socket, const QByteArray &body);
    void send(QTcpSocket *socket, int status, const QByteArray &contentType, const QByteArray &body);
    void sendJson(QTcpSocket *socket, int status, const QJsonObject &object);
    void rememberResult(const QString &requestId, const QJsonObject &result);
    static QByteArray pageHtml();

    QTcpServer *m_server = nullptr;
    StateProvider m_stateProvider;
    quint64 m_nextTicket = 1;
    QHash<quint64, Ticket> m_tickets;
    QHash<QString, quint64> m_inFlight;     // requestId -> ticket
    QHash<QString, QJsonObject> m_results;  // requestId -> final result
    QStringList m_resultOrder;
};

#endif
