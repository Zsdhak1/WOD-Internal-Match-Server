#include "manualdamageserver.h"

#include "third_party/qrcodegen/qrcodegen.hpp"

#include <QHostAddress>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QPainter>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>

namespace {
constexpr int kMaxHeaderBytes = 8 * 1024;
constexpr int kMaxBodyBytes = 4 * 1024;
constexpr int kMaxResults = 256;
constexpr int kMaxConnections = 64;
constexpr int kIdleTimeoutMs = 10000;

QByteArray reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 503: return "Service Unavailable";
    default: return "Error";
    }
}

quint8 teamFrom(const QString &text)
{
    return text == QLatin1String("red") ? 1 : text == QLatin1String("blue") ? 2 : 0;
}

bool validRequestId(const QString &id)
{
    if (id.size() < 8 || id.size() > 64) return false;
    for (const QChar c : id)
        if (!(c.isLetterOrNumber() && c.unicode() < 128) && c != QLatin1Char('-') && c != QLatin1Char('_'))
            return false;
    return true;
}
}

ManualDamageServer::ManualDamageServer(QObject *parent) : QObject(parent)
{
    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &ManualDamageServer::onNewConnection);
}

ManualDamageServer::~ManualDamageServer()
{
    // The owner may already be partially destroyed: never emit from here.
    blockSignals(true);
    stop();
}

bool ManualDamageServer::start(quint16 port)
{
    stop();
    if (!m_server->listen(QHostAddress::AnyIPv4, port)) {
        emit logMessage(tr("[扣血网页] 启动失败：端口 %1 %2").arg(port).arg(m_server->errorString()));
        return false;
    }
    emit logMessage(tr("[扣血网页] 已启动 · HTTP %1 · 局域网内无口令访问").arg(m_server->serverPort()));
    return true;
}

void ManualDamageServer::stop()
{
    if (!m_server) return;
    const bool was = m_server->isListening();
    m_server->close();
    for (QTcpSocket *s : findChildren<QTcpSocket *>()) s->abort();
    // Pending tickets stay owned by MainWindow; their sockets are gone.
    if (was) emit logMessage(tr("[扣血网页] 已停止"));
}

bool ManualDamageServer::isListening() const { return m_server && m_server->isListening(); }
quint16 ManualDamageServer::port() const { return m_server ? m_server->serverPort() : 0; }

QStringList ManualDamageServer::lanAddresses()
{
    QStringList list;
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning)
            || (flags & QNetworkInterface::IsLoopBack))
            continue;
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            const QHostAddress a = entry.ip();
            if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback()
                && !a.isInSubnet(QHostAddress(QStringLiteral("169.254.0.0")), 16))
                list.append(a.toString());
        }
    }
    list.removeDuplicates();
    // Phones need a real LAN address first. 198.18.0.0/15 is the benchmark
    // range proxy TUN adapters (Clash etc.) use; it is unreachable from Wi-Fi.
    const auto rank = [](const QString &text) {
        const QHostAddress a(text);
        if (a.isInSubnet(QHostAddress(QStringLiteral("192.168.0.0")), 16)) return 0;
        if (a.isInSubnet(QHostAddress(QStringLiteral("10.0.0.0")), 8)) return 1;
        if (a.isInSubnet(QHostAddress(QStringLiteral("172.16.0.0")), 12)) return 2;
        if (a.isInSubnet(QHostAddress(QStringLiteral("198.18.0.0")), 15)) return 4;
        return 3;
    };
    std::stable_sort(list.begin(), list.end(), [&](const QString &l, const QString &r) { return rank(l) < rank(r); });
    return list;
}

QImage ManualDamageServer::qrCode(const QString &text, int moduleSize, int border)
{
    using qrcodegen::QrCode;
    const QrCode qr = QrCode::encodeText(text.toUtf8().constData(), QrCode::Ecc::MEDIUM);
    const int size = qr.getSize();
    const int px = (size + border * 2) * moduleSize;
    QImage image(px, px, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter p(&image);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::black);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            if (qr.getModule(x, y))
                p.drawRect((x + border) * moduleSize, (y + border) * moduleSize, moduleSize, moduleSize);
    return image;
}

void ManualDamageServer::onNewConnection()
{
    while (QTcpSocket *socket = m_server->nextPendingConnection()) {
        if (findChildren<QTcpSocket *>().size() > kMaxConnections) { socket->abort(); socket->deleteLater(); continue; }
        socket->setParent(this);
        auto *idle = new QTimer(socket);
        idle->setSingleShot(true);
        idle->start(kIdleTimeoutMs);
        connect(idle, &QTimer::timeout, socket, &QTcpSocket::abort);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }
}

void ManualDamageServer::onReadyRead(QTcpSocket *socket)
{
    if (socket->property("handled").toBool()) { socket->readAll(); return; }
    QByteArray buffer = socket->property("buffer").toByteArray() + socket->readAll();
    const int headerEnd = buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        if (buffer.size() > kMaxHeaderBytes) { send(socket, 413, "text/plain", "too large"); return; }
        socket->setProperty("buffer", buffer);
        return;
    }
    const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');
    const QList<QByteArray> start = lines.value(0).trimmed().split(' ');
    if (start.size() < 2) { send(socket, 400, "text/plain", "bad request"); return; }
    int contentLength = 0;
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        if (line.toLower().startsWith("content-length:")) contentLength = line.mid(15).trimmed().toInt();
    }
    if (contentLength < 0 || contentLength > kMaxBodyBytes) { send(socket, 413, "text/plain", "too large"); return; }
    if (buffer.size() < headerEnd + 4 + contentLength) { socket->setProperty("buffer", buffer); return; }
    Request request;
    request.method = start.at(0);
    const QByteArray target = start.at(1);
    const int q = target.indexOf('?');
    request.path = q < 0 ? target : target.left(q);
    request.query = q < 0 ? QByteArray() : target.mid(q + 1);
    request.body = buffer.mid(headerEnd + 4, contentLength);
    socket->setProperty("handled", true);
    socket->setProperty("buffer", QVariant());
    handle(socket, request);
}

void ManualDamageServer::handle(QTcpSocket *socket, const Request &r)
{
    if (r.path == "/" || r.path == "/red" || r.path == "/blue" || r.path == "/index.html") {
        if (r.method != "GET") { send(socket, 405, "text/plain", "method not allowed"); return; }
        send(socket, 200, "text/html; charset=utf-8", pageHtml());
        return;
    }
    if (r.path == "/api/state") {
        const quint8 team = teamFrom(QUrlQuery(QString::fromUtf8(r.query)).queryItemValue(QStringLiteral("team")));
        if (!team) { sendJson(socket, 400, {{"ok", false}, {"message", tr("未知队伍")}}); return; }
        QJsonObject state = m_stateProvider ? m_stateProvider(team) : QJsonObject{};
        state.insert(QStringLiteral("ok"), true);
        sendJson(socket, 200, state);
        return;
    }
    if (r.path == "/api/damage") {
        if (r.method != "POST") { send(socket, 405, "text/plain", "method not allowed"); return; }
        handleDamage(socket, r.body);
        return;
    }
    if (r.path == "/favicon.ico") { send(socket, 404, "text/plain", ""); return; }
    send(socket, 404, "text/plain; charset=utf-8", "not found");
}

void ManualDamageServer::handleDamage(QTcpSocket *socket, const QByteArray &body)
{
    QJsonParseError error{};
    const QJsonObject o = QJsonDocument::fromJson(body, &error).object();
    const quint8 team = teamFrom(o.value(QStringLiteral("team")).toString());
    const QString requestId = o.value(QStringLiteral("requestId")).toString();
    if (error.error != QJsonParseError::NoError || !team || !validRequestId(requestId)) {
        sendJson(socket, 400, {{"ok", false}, {"message", tr("请求无效")}});
        return;
    }
    const QString key = QString::number(team) + QLatin1Char(':') + requestId;
    if (m_results.contains(key)) { sendJson(socket, 200, m_results.value(key)); return; } // replay
    if (m_inFlight.contains(key)) {                                                      // retry joins
        m_tickets[m_inFlight.value(key)].sockets.append(socket);
        return;
    }
    const quint64 ticket = m_nextTicket++;
    m_tickets.insert(ticket, Ticket{key, {socket}});
    m_inFlight.insert(key, ticket);
    emit damageRequested(ticket, team, socket->peerAddress().toString());
}

void ManualDamageServer::completeDamage(quint64 ticket, const QJsonObject &result)
{
    const auto it = m_tickets.find(ticket);
    if (it == m_tickets.end()) return;
    const Ticket t = it.value();
    m_tickets.erase(it);
    m_inFlight.remove(t.requestId);
    rememberResult(t.requestId, result);
    for (const QPointer<QTcpSocket> &s : t.sockets)
        if (s) sendJson(s, 200, result);
}

void ManualDamageServer::rememberResult(const QString &requestId, const QJsonObject &result)
{
    m_results.insert(requestId, result);
    m_resultOrder.append(requestId);
    while (m_resultOrder.size() > kMaxResults) m_results.remove(m_resultOrder.takeFirst());
}

void ManualDamageServer::sendJson(QTcpSocket *socket, int status, const QJsonObject &object)
{
    send(socket, status, "application/json; charset=utf-8", QJsonDocument(object).toJson(QJsonDocument::Compact));
}

void ManualDamageServer::send(QTcpSocket *socket, int status, const QByteArray &contentType, const QByteArray &body)
{
    if (!socket || socket->state() != QAbstractSocket::ConnectedState) return;
    QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + ' ' + reason(status) + "\r\n"
        "Content-Type: " + contentType + "\r\n"
        "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
        "Cache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Connection: close\r\n\r\n" + body;
    socket->write(response);
    socket->disconnectFromHost();
}
