#ifndef MATCHPROTOCOL_H
#define MATCHPROTOCOL_H

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

// 选手端 <-> 赛事服务端的局域网控制协议。
// 每条消息都是一个紧凑 JSON 对象，以换行符分隔，便于在 TCP 流上增量解析。
namespace matchproto {

constexpr quint16 kControlPort = 5010;
constexpr int kMaxLineBytes = 64 * 1024;

inline QByteArray encode(const QJsonObject &object)
{
    QByteArray line = QJsonDocument(object).toJson(QJsonDocument::Compact);
    line.append('\n');
    return line;
}

inline bool decode(const QByteArray &line, QJsonObject &object, QString *error = nullptr)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line.trimmed(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = parseError.errorString();
        return false;
    }

    object = document.object();
    return true;
}

inline QJsonObject registrationRequest(int team,
                                       int robotId,
                                       const QString &displayName,
                                       const QString &sourceId,
                                       const QString &sourceName)
{
    return {
        {QStringLiteral("type"), QStringLiteral("register")},
        {QStringLiteral("team"), team},
        {QStringLiteral("robotId"), robotId},
        {QStringLiteral("displayName"), displayName},
        {QStringLiteral("sourceId"), sourceId},
        {QStringLiteral("sourceName"), sourceName}
    };
}

// Keep the original four-argument helper usable for older integrations.
inline QJsonObject registrationRequest(int team,
                                       const QString &displayName,
                                       const QString &sourceId,
                                       const QString &sourceName)
{
    return registrationRequest(team, 1, displayName, sourceId, sourceName);
}

inline QJsonObject cameraSelectRequest(const QString &sourceId, const QString &sourceName)
{
    return {
        {QStringLiteral("type"), QStringLiteral("camera_select")},
        {QStringLiteral("sourceId"), sourceId},
        {QStringLiteral("sourceName"), sourceName}
    };
}

inline QJsonObject simpleRequest(const QString &type)
{
    return {{QStringLiteral("type"), type}};
}

} // namespace matchproto

#endif // MATCHPROTOCOL_H
