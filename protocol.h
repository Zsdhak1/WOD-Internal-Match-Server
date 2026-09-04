#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <QtGlobal>
#include <QByteArray>
#include <cstring>

// 小车(ESP32) -> 服务器 的 UDP 上行协议，定义与字段详见 proto.markdown。
// 所有字段为小端序、packed 结构体，解析时按字节偏移读取，不依赖主机对齐。

namespace proto {

constexpr quint16 kMagic   = 0x5254;   // 固定 magic：0x52 0x54 ('R''T')
constexpr quint8  kVersion = 1;        // 当前协议版本
constexpr quint16 kBindPort = 5005;    // 服务器监听端口，与小车端 sdkconfig 的 SERVER_PORT 一致

enum FrameType : quint8 {
    TypeStatus        = 1,   // 状态帧，10 Hz，10 字节
    TypeDeath         = 2,   // 死亡帧，6 字节
    TypeRevive        = 3,   // 复活帧，6 字节
    TypeHit           = 4,   // 受击帧(含受击后血量)，8 字节
    TypeAttack        = 5,   // 攻击帧，6 字节
    TypeShootEnabled  = 6,   // 恢复射击帧，6 字节
    TypeShootDisabled = 7,   // 禁止射击帧，6 字节
};

// 每个 frame_type 对应的预期帧长；未知类型返回 -1，用于拒绝非法帧。
inline int expectedSize(int type)
{
    switch (type) {
    case TypeStatus:        return 10;
    case TypeHit:           return 8;
    case TypeDeath:
    case TypeRevive:
    case TypeAttack:
    case TypeShootEnabled:
    case TypeShootDisabled: return 6;
    default:                return -1;
    }
}

struct Frame {
    quint8  type = 0;
    quint8  robotId = 0;
    quint8  team = 0;
    quint16 hp = 0;            // 状态帧/受击帧有效
    bool    alive = false;     // 仅状态帧有效
    bool    shootEnabled = false;
    bool    fromStatus = false; // 是否来自 10 Hz 状态帧
};

inline quint16 readU16(const char *p)
{
    quint16 v;
    std::memcpy(&v, p, 2);
    return v;
}

// 解析一个 UDP 数据报。返回 false 表示帧非法(magic/version/帧长/类型不符)，此时不修改 out。
inline bool parseDatagram(const QByteArray &dg, Frame &out)
{
    const char *p = dg.constData();
    const int n = dg.size();
    if (n < 6) return false;
    if (readU16(p) != kMagic) return false;
    if (static_cast<quint8>(p[2]) != kVersion) return false;

    const quint8 type = static_cast<quint8>(p[3]);
    if (expectedSize(type) != n) return false;   // 未知类型 expectedSize=-1 也会不匹配

    out.type         = type;
    out.robotId      = static_cast<quint8>(p[4]);
    out.team         = static_cast<quint8>(p[5]);
    out.fromStatus   = (type == TypeStatus);
    out.hp           = 0;
    out.alive        = false;
    out.shootEnabled = false;

    if (type == TypeStatus || type == TypeHit) {
        out.hp = readU16(p + 6);
        if (type == TypeStatus) {
            out.alive        = p[8] != 0;
            out.shootEnabled = p[9] != 0;
        }
    }
    return true;
}

} // namespace proto

#endif // PROTOCOL_H
