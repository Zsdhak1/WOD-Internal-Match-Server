#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <QtGlobal>
#include <QByteArray>
#include <cstring>

// 小车(ESP32) <-> 服务器 的 UDP 协议。
// V1 是早期测试协议（仍支持解析，便于回归测试）。
// V2 是正式协议，详见 docs/V2_MIGRATION_PLAN.md 与 ESP32 仓库 README。
// 所有字段为小端序、packed 结构体，解析时按字节偏移读取，不依赖主机对齐。

namespace proto {

constexpr quint16 kMagic        = 0x5254;   // 固定 magic：0x52 0x54 ('R''T')
constexpr quint8  kVersion1     = 1;
constexpr quint8  kVersion2     = 2;
constexpr quint16 kUplinkPort   = 5005;     // ESP32 -> 服务器
constexpr quint16 kDownlinkPort = 5006;     // 服务器 -> ESP32

enum FrameType : quint8 {
    // 上行（ESP32 -> server）
    TypeStatus        = 0x01,   // 状态帧：V1=10/12B，V2=14B
    TypeDeath         = 0x02,   // 死亡帧：V1=6B，V2=9B reliable
    TypeRevive        = 0x03,   // 复活帧：V1=6B，V2=9B reliable
    TypeHit           = 0x04,   // 受击帧：V1=8B，V2=5B
    TypeAttack        = 0x05,   // 攻击帧：V1=6B，V2=5B
    TypeShootEnabled  = 0x06,   // 允许射击：V1=6B，V2=5B
    TypeShootDisabled = 0x07,   // 禁止射击：V1=6B，V2=5B
    TypeLinkDown      = 0x08,   // V2 新增：上游业务链路断开
    TypeLinkUp        = 0x09,   // V2 新增：上游业务链路恢复
    // 下行（server -> ESP32）
    TypeGameStart     = 0x81,   // 比赛开始：9B + ACK
    TypeGameEnd       = 0x82,   // 比赛结束：9B + ACK
    TypeAssignment    = 0x83,   // 分配 ID+MAC：15B + ACK
    TypeStatusRequest = 0x84,   // 请求状态：5B，无 ACK
    TypeSetHp         = 0x85,   // 设置 HP：11B + ACK
    TypeYellowCard    = 0x86,   // 黄牌处罚：9B + ACK
    TypeForcePowerOff = 0x87,   // 强制断电：9B + ACK
    TypeForcePowerOn  = 0x88,   // 请求通电：9B + ACK
    TypeAck           = 0xF0,   // ACK 回执：10B
};

// V1 长度表
inline int expectedSizeV1(int type)
{
    switch (type) {
    case TypeStatus:        return -1;   // 10 或 12，由 isExpectedSize 特判
    case TypeHit:           return 8;
    case TypeDeath:
    case TypeRevive:
    case TypeAttack:
    case TypeShootEnabled:
    case TypeShootDisabled: return 6;
    default:                return -1;
    }
}

// V2 长度表
inline int expectedSizeV2(int type)
{
    switch (type) {
    case TypeStatus:        return 14;
    case TypeDeath:
    case TypeRevive:        return 9;
    case TypeHit:
    case TypeAttack:
    case TypeShootEnabled:
    case TypeShootDisabled:
    case TypeLinkDown:
    case TypeLinkUp:        return 5;
    case TypeGameStart:
    case TypeGameEnd:
    case TypeYellowCard:
    case TypeForcePowerOff:
    case TypeForcePowerOn:  return 9;
    case TypeAssignment:    return 15;
    case TypeStatusRequest: return 5;
    case TypeSetHp:         return 11;
    case TypeAck:           return 10;
    default:                return -1;
    }
}

inline bool isExpectedSize(quint8 version, int type, int size)
{
    if (version == kVersion1) {
        if (type == TypeStatus) return size == 10 || size == 12;
        return expectedSizeV1(type) == size;
    }
    if (version == kVersion2)
        return expectedSizeV2(type) == size;
    return false;
}

struct Frame {
    quint8  version        = 0;
    quint8  type           = 0;
    quint8  robotId        = 0;
    quint8  team           = 0;             // V1 only; V2 移除
    quint16 hp             = 0;
    quint16 heat           = 0;
    quint16 power          = 0;             // V2 only
    bool    alive          = false;
    bool    shootEnabled   = false;
    bool    powerOn        = false;         // V2 only
    quint32 transactionId  = 0;             // V2 reliable events + ACK
    quint8  ackedFrameType = 0;             // V2 ACK only
    quint8  result         = 0;             // V2 ACK only: 0=ok 1=rejected 2=failed
    bool    fromStatus     = false;
    QByteArray controllerMac;               // V2 ASSIGNMENT only, 6 bytes
};

inline quint16 readU16(const char *p)
{
    quint16 v;
    std::memcpy(&v, p, 2);
    return v;
}

inline quint32 readU32(const char *p)
{
    quint32 v;
    std::memcpy(&v, p, 4);
    return v;
}

inline void writeU16(QByteArray &data, int offset, quint16 value)
{
    data[offset]     = static_cast<char>(value & 0xff);
    data[offset + 1] = static_cast<char>((value >> 8) & 0xff);
}

inline void writeU32(QByteArray &data, int offset, quint32 value)
{
    data[offset]     = static_cast<char>(value & 0xff);
    data[offset + 1] = static_cast<char>((value >> 8) & 0xff);
    data[offset + 2] = static_cast<char>((value >> 16) & 0xff);
    data[offset + 3] = static_cast<char>((value >> 24) & 0xff);
}

// ---------- V1 上行构造（模拟器回归测试用） ----------

inline QByteArray makeEventV1(quint8 type, quint8 robotId, quint8 team)
{
    QByteArray data(6, Qt::Uninitialized);
    writeU16(data, 0, kMagic);
    data[2] = static_cast<char>(kVersion1);
    data[3] = static_cast<char>(type);
    data[4] = static_cast<char>(robotId);
    data[5] = static_cast<char>(team);
    return data;
}

inline QByteArray makeStatusV1(quint8 robotId, quint8 team, quint16 hp,
                               bool alive, bool shootEnabled)
{
    QByteArray data(10, Qt::Uninitialized);
    writeU16(data, 0, kMagic);
    data[2] = static_cast<char>(kVersion1);
    data[3] = static_cast<char>(TypeStatus);
    data[4] = static_cast<char>(robotId);
    data[5] = static_cast<char>(team);
    writeU16(data, 6, hp);
    data[8] = alive ? 1 : 0;
    data[9] = shootEnabled ? 1 : 0;
    return data;
}

inline QByteArray makeStatusV1WithHeat(quint8 robotId, quint8 team, quint16 hp,
                                       quint16 heat, bool alive, bool shootEnabled)
{
    QByteArray data(12, Qt::Uninitialized);
    writeU16(data, 0, kMagic);
    data[2] = static_cast<char>(kVersion1);
    data[3] = static_cast<char>(TypeStatus);
    data[4] = static_cast<char>(robotId);
    data[5] = static_cast<char>(team);
    writeU16(data, 6, hp);
    data[8] = alive ? 1 : 0;
    data[9] = shootEnabled ? 1 : 0;
    writeU16(data, 10, heat);
    return data;
}

inline QByteArray makeHitV1(quint8 robotId, quint8 team, quint16 hpAfterHit)
{
    QByteArray data(8, Qt::Uninitialized);
    writeU16(data, 0, kMagic);
    data[2] = static_cast<char>(kVersion1);
    data[3] = static_cast<char>(TypeHit);
    data[4] = static_cast<char>(robotId);
    data[5] = static_cast<char>(team);
    writeU16(data, 6, hpAfterHit);
    return data;
}

// ---------- V2 上行构造（模拟器用） ----------

inline QByteArray makeStatusV2(quint8 robotId, quint16 hp, quint16 heat,
                               quint16 power, bool alive, bool shootEnabled,
                               bool powerOn)
{
    QByteArray data(14, Qt::Uninitialized);
    writeU16(data, 0, kMagic);
    data[2] = static_cast<char>(kVersion2);
    data[3] = static_cast<char>(TypeStatus);
    data[4] = static_cast<char>(robotId);
    writeU16(data, 5, hp);
    writeU16(data, 7, heat);
    writeU16(data, 9, power);
    data[11] = alive ? 1 : 0;
    data[12] = shootEnabled ? 1 : 0;
    data[13] = powerOn ? 1 : 0;
    return data;
}

inline QByteArray makeEventV2(quint8 type, quint8 robotId)
{
    QByteArray data(5, Qt::Uninitialized);
    writeU16(data, 0, kMagic);
    data[2] = static_cast<char>(kVersion2);
    data[3] = static_cast<char>(type);
    data[4] = static_cast<char>(robotId);
    return data;
}

inline QByteArray makeReliableEventV2(quint8 type, quint8 robotId, quint32 txid)
{
    QByteArray data(9, Qt::Uninitialized);
    writeU16(data, 0, kMagic);
    data[2] = static_cast<char>(kVersion2);
    data[3] = static_cast<char>(type);
    data[4] = static_cast<char>(robotId);
    writeU32(data, 5, txid);
    return data;
}

// ---------- V2 下行构造（服务端用） ----------

inline QByteArray makeDownlinkBase(quint8 type, quint8 targetId, quint32 txid)
{
    QByteArray data(9, Qt::Uninitialized);
    writeU16(data, 0, kMagic);
    data[2] = static_cast<char>(kVersion2);
    data[3] = static_cast<char>(type);
    data[4] = static_cast<char>(targetId);
    writeU32(data, 5, txid);
    return data;
}

inline QByteArray makeGameStart(quint8 targetId, quint32 txid)
{ return makeDownlinkBase(TypeGameStart, targetId, txid); }

inline QByteArray makeGameEnd(quint8 targetId, quint32 txid)
{ return makeDownlinkBase(TypeGameEnd, targetId, txid); }

inline QByteArray makeYellowCard(quint8 targetId, quint32 txid)
{ return makeDownlinkBase(TypeYellowCard, targetId, txid); }

inline QByteArray makeForcePowerOff(quint8 targetId, quint32 txid)
{ return makeDownlinkBase(TypeForcePowerOff, targetId, txid); }

inline QByteArray makeForcePowerOn(quint8 targetId, quint32 txid)
{ return makeDownlinkBase(TypeForcePowerOn, targetId, txid); }

inline QByteArray makeAssignment(quint8 robotId, const QByteArray &mac6,
                                  quint32 txid)
{
    QByteArray data(15, Qt::Uninitialized);
    writeU16(data, 0, kMagic);
    data[2] = static_cast<char>(kVersion2);
    data[3] = static_cast<char>(TypeAssignment);
    data[4] = static_cast<char>(robotId);
    for (int i = 0; i < 6; ++i)
        data[5 + i] = i < mac6.size() ? mac6[i] : char(0);
    writeU32(data, 11, txid);
    return data;
}

inline QByteArray makeStatusRequest(quint8 targetId)
{
    QByteArray data(5, Qt::Uninitialized);
    writeU16(data, 0, kMagic);
    data[2] = static_cast<char>(kVersion2);
    data[3] = static_cast<char>(TypeStatusRequest);
    data[4] = static_cast<char>(targetId);
    return data;
}

inline QByteArray makeSetHp(quint8 targetId, quint16 hp, quint32 txid)
{
    QByteArray data(11, Qt::Uninitialized);
    writeU16(data, 0, kMagic);
    data[2] = static_cast<char>(kVersion2);
    data[3] = static_cast<char>(TypeSetHp);
    data[4] = static_cast<char>(targetId);
    writeU16(data, 5, hp);
    writeU32(data, 7, txid);
    return data;
}

inline QByteArray makeAck(quint8 ackedType, quint32 txid, quint8 result)
{
    QByteArray data(10, Qt::Uninitialized);
    writeU16(data, 0, kMagic);
    data[2] = static_cast<char>(kVersion2);
    data[3] = static_cast<char>(TypeAck);
    data[4] = static_cast<char>(ackedType);
    writeU32(data, 5, txid);
    data[9] = static_cast<char>(result);
    return data;
}

// ---------- 解析 ----------

// 解析一个 UDP 数据报。返回 false 表示帧非法。
// 自动识别 V1/V2；V1 帧走旧字段，V2 帧走新字段。
inline bool parseDatagram(const QByteArray &dg, Frame &out)
{
    const char *p = dg.constData();
    const int n = dg.size();
    if (n < 5) return false;
    if (readU16(p) != kMagic) return false;

    const quint8 version = static_cast<quint8>(p[2]);
    const quint8 type = static_cast<quint8>(p[3]);
    if (version != kVersion1 && version != kVersion2) return false;
    if (!isExpectedSize(version, type, n)) return false;

    out = Frame{};
    out.version    = version;
    out.type       = type;
    out.fromStatus = (type == TypeStatus);

    if (version == kVersion1) {
        if (n < 6) return false;
        out.robotId = static_cast<quint8>(p[4]);
        out.team    = static_cast<quint8>(p[5]);
        if (type == TypeStatus) {
            out.hp           = readU16(p + 6);
            out.alive        = p[8] != 0;
            out.shootEnabled = p[9] != 0;
            if (n >= 12)
                out.heat = readU16(p + 10);
        } else if (type == TypeHit) {
            out.hp = readU16(p + 6);
        }
        return true;
    }

    // V2
    out.robotId = static_cast<quint8>(p[4]);
    switch (type) {
    case TypeStatus:
        out.hp           = readU16(p + 5);
        out.heat         = readU16(p + 7);
        out.power        = readU16(p + 9);
        out.alive        = p[11] != 0;
        out.shootEnabled = p[12] != 0;
        out.powerOn      = p[13] != 0;
        break;
    case TypeDeath:
    case TypeRevive:
        out.transactionId = readU32(p + 5);
        break;
    case TypeAck:
        out.ackedFrameType = static_cast<quint8>(p[4]);
        out.robotId        = 0;         // ACK 无 robotId
        out.transactionId  = readU32(p + 5);
        out.result         = static_cast<quint8>(p[9]);
        break;
    case TypeAssignment:
        out.controllerMac = QByteArray(p + 5, 6);
        out.transactionId = readU32(p + 11);
        break;
    case TypeGameStart:
    case TypeGameEnd:
    case TypeYellowCard:
    case TypeForcePowerOff:
    case TypeForcePowerOn:
        out.transactionId = readU32(p + 5);
        break;
    case TypeSetHp:
        out.hp            = readU16(p + 5);
        out.transactionId = readU32(p + 7);
        break;
    default:
        break;
    }
    return true;
}

// 该帧类型是否需要回 ACK（V2 only）
inline bool isReliableEvent(quint8 type)
{ return type == TypeDeath || type == TypeRevive; }

inline bool isDownlinkCommand(quint8 type)
{
    return type == TypeGameStart || type == TypeGameEnd
           || type == TypeAssignment || type == TypeSetHp
           || type == TypeYellowCard || type == TypeForcePowerOff
           || type == TypeForcePowerOn;
}

} // namespace proto

#endif // PROTOCOL_H
