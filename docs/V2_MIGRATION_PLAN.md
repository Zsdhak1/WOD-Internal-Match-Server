# ESP32 V1 → V2 协议跟进计划

源仓库：`https://github.com/2470519590/ESP32-S3-Module-for-Judgement-System`
最新提交：`9957856 修复一些影响通信稳定的隐患`（2026-09-09）

ESP32 端已**完全切换到 V2**，不再发送 V1 帧。当前服务端仅实现 V1，**完全不兼容**新固件。

## 协议差异速查

| 项 | V1（当前） | V2（ESP32 现行） |
|---|---|---|
| 版本号 | `version=1` | `version=2` |
| `team` 字段 | 每帧含 | **移除** |
| 状态帧 | 10/12 字节 | **14 字节**（+`power`、`power_on`） |
| 死亡/复活 | 6 字节 | **9 字节**（+`transaction_id`），必须 ACK |
| 受击/攻击/射击 | 6/8 字节 | **5 字节** |
| 链路事件 | 无 | `0x08/0x09` L431 链路断开/恢复 |
| 下行 | 预留 | **完整命令集**（`0x81`–`0x88`） |
| ACK | 无 | `0xF0`，10 字节 |
| ESP32 下行端口 | 未定义 | **5006** |

## 实施阶段

### P0：`protocol.h` 重写（无依赖）
新增 V2 全套结构和构造函数，**保留 V1 解析路径**便于回退测试。

```cpp
namespace proto {
  constexpr quint8  kVersion1 = 1;
  constexpr quint8  kVersion2 = 2;
  constexpr quint16 kUplinkPort   = 5005;  // ESP32 -> server
  constexpr quint16 kDownlinkPort = 5006;  // server -> ESP32

  enum FrameType : quint8 {
    TypeStatus        = 0x01,
    TypeDeath         = 0x02,   // V2: 9B reliable
    TypeRevive        = 0x03,   // V2: 9B reliable
    TypeHit           = 0x04,   // V2: 5B
    TypeAttack        = 0x05,
    TypeShootEnabled  = 0x06,
    TypeShootDisabled = 0x07,
    TypeLinkDown      = 0x08,   // V2 new
    TypeLinkUp        = 0x09,
    TypeDeviceAnnounce= 0x0A,   // 25B, ESP32/controller MAC registry
    TypeCombatEnd     = 0x0B,   // 5B
    TypeDeviceHealth  = 0x0C,   // 6B, component bitmap
    TypeGameStart     = 0x81,
    TypeGameEnd       = 0x82,
    TypeAssignment    = 0x83,
    TypeStatusRequest = 0x84,
    TypeSetHp         = 0x85,
    TypeYellowCard    = 0x86,
    TypeForcePowerOff = 0x87,
    TypeForcePowerOn  = 0x88,
    TypeAck           = 0xF0,
  };

  struct Frame {
    quint8  version;
    quint8  type;
    quint8  robotId;
    quint8  team;            // V1 only
    quint16 hp;
    quint16 heat;
    quint16 power;           // V2 only
    bool    alive;
    bool    shootEnabled;
    bool    powerOn;         // V2 only
    quint32 transactionId;   // V2 reliable + ACK
    quint8  ackedFrameType;  // V2 ACK only
    quint8  result;          // V2 ACK only: 0=ok 1=rejected 2=failed
    quint8  componentOnline; // V2 health bitmap
    QByteArray deviceMac;    // V2 announce/assignment
    QByteArray controllerMac;// V2 announce/assignment
    bool    fromStatus;
  };

  bool parseDatagram(const QByteArray &dg, Frame &out);

  // V2 上行构造（模拟器用）
  QByteArray makeStatusV2(quint8 robotId, quint16 hp, quint16 heat,
                          quint16 power, bool alive, bool shoot, bool powerOn);
  QByteArray makeEventV2(quint8 type, quint8 robotId);                     // 5B
  QByteArray makeReliableEventV2(quint8 type, quint8 robotId, quint32 txid); // 9B

  // V2 下行构造（服务端用）
  QByteArray makeGameStart(quint8 target, quint32 txid);                   // 9B
  QByteArray makeGameEnd(quint8 target, quint32 txid);
  QByteArray makeDeviceAnnounce(...);                                      // 25B
  QByteArray makeDeviceHealth(quint8 robotId, quint8 componentOnline);       // 6B
  QByteArray makeAssignment(quint8 robotId, const QByteArray &deviceMac6,
                            const QByteArray &controllerMac6, quint32 txid);  // 21B
  QByteArray makeStatusRequest(quint8 target);                              // 5B
  QByteArray makeSetHp(quint8 target, quint16 hp, quint32 txid);            // 11B
  QByteArray makeYellowCard(quint8 target, quint32 txid);
  QByteArray makeForcePowerOff(quint8 target, quint32 txid);
  QByteArray makeForcePowerOn(quint8 target, quint32 txid);
  QByteArray makeAck(quint8 ackedType, quint32 txid, quint8 result);        // 10B
}
```

**校验规则**：
- `version=1`：按 V1 长度表解析（状态 10/12、事件 6/8）
- `version=2`：严格按 V2 长度表（status=14、health=6、announce=25、reliable=9、event=5、assignment=21、ack=10）
- 死亡/复活回 ACK 到 `sender:senderPort`

### P1：`RobotManager` V2 支持
**改动点**：
- `RobotInfo` 加字段：`power`、`powerOn`、`protocolVersion`、`componentOnline`、`lastTxid`；网络地址保存为 IP 文本 + 32 位 IPv4，避免跨事件长期持有 `QHostAddress`
- 维护 `ESP32 Wi-Fi MAC -> DeviceInfo` 设备表：当前 IP、手柄 BLE MAC、robot_id、丢包/发送失败计数
- `m_robotTeamMap: QHash<quint8, quint8>` —— `robot_id → team` 服务器映射，QSettings 持久化
- `m_processedTxids: QHash<quint8, QSet<quint64>>` —— `(robotId, type<<32|txid)` 去重，QSettings 持久化
- 收到 V2 死亡/复活 → 回 ACK + 去重
- 收到 V1 帧 → 用 `frame.team`；收到 V2 帧 → 查 `m_robotTeamMap`
- 新事件 `TypeLinkDown/TypeLinkUp` 在 UI 显示链路状态
- **新信号**：`robotIpLearned(robotId, ip, port)` 供下行用

### P2：`RobotCommander`（新类，新文件）
```cpp
// robotcommander.h / robotcommander.cpp
class RobotCommander : public QObject {
  Q_OBJECT
public:
  explicit RobotCommander(QUdpSocket *uplink, QObject *parent);

  void learnEndpoint(quint8 robotId, quint32 ipv4, quint16 port);

  // 所有返回 transaction_id；自动重传相同 txid 最多 2 次（100ms 间隔）
  quint32 sendGameStart(quint8 target);
  quint32 sendGameEnd(quint8 target);
  quint32 sendYellowCard(quint8 target);
  quint32 sendForcePowerOff(quint8 target);
  quint32 sendForcePowerOn(quint8 target);
  quint32 sendSetHp(quint8 target, quint16 hp);
  quint32 sendAssignment(quint32 ipv4, quint8 robotId,
                         const QByteArray &deviceMac6,
                         const QByteArray &controllerMac6);
  void    sendStatusRequest(quint8 target);

  // 由 RobotManager 在收到 ACK 帧时调用
  void onAckReceived(quint8 ackedType, quint32 txid, quint8 result, quint8 robotId);

signals:
  void commandAcked(quint32 txid, quint8 type, quint8 result, quint8 robotId);
  void commandTimeout(quint32 txid, quint8 type, quint8 robotId);

private:
  struct Pending {
    QByteArray datagram;
    quint8     type;
    quint8     targetId;
    QHostAddress ip;
    quint16    port;
    int        attempts;
    qint64     deadline;
  };
  QHash<quint32, Pending> m_pending;
  QHash<quint8, QPair<QHostAddress, quint16>> m_robotEndpoints;
  QTimer *m_retryTimer;
  quint32 m_nextTxid;
};
```

### P3：`MatchServer` 集成
- `RobotCommander` 实例化，绑定 `m_socket`（共享 5005 socket）
- `publishMatchState` 时调用 `sendGameStart`/`sendGameEnd` 广播（target=0）+ 跟踪每车 ACK
- `awardCard` 时调用 `sendYellowCard(targetId)`
- 新增"开始/结束比赛"业务逻辑：
  - `startMatch()` → 对每个已知 robotId 发 `GAME_START`，等所有 ACK 后才 `m_broadcast->startMatch()`
  - `terminateMatch()` → 发 `GAME_END`
- 黄牌：UI 按钮 → `sendYellowCard(robotId)` → 等 ACK → `m_broadcast->awardCard(team, false)`

### P4：`MainWindow` UI
新增"设备管理"分组：
- 列出已收到 `DEVICE_ANNOUNCE` 的设备：ESP32 Wi-Fi MAC、当前 IP、手柄 BLE MAC、当前 `robotId`
- 每行可选队伍（红/蓝）→ 更新 `m_robotTeamMap`
- 选择未分配设备，输入机器人 ID + 手柄 MAC，点击分配 → 发 21 字节 `ASSIGNMENT` 到设备 IP:UDP 5006
- "通电"/"断电"/"黄牌" 每行操作按钮

新增"比赛控制"分组：
- "开始比赛" → 遍历 `m_robotTeamMap` 发 `GAME_START`（每车单播）
- "结束比赛" → 同上 `GAME_END`
- 等待所有 ACK 的进度条

### P5：`BroadcastWindow` HUD
- 机器人面板加 `power`、`powerOn` 显示
- `powerOn=0` 时面板灰显 + "已断电"标记
- `protocolVersion=1` 显示 "V1 旧固件" 警告
- `TypeLinkDown` 时显示 "L431 链路断开"

### P6：`RobotSimulator` V2 升级
- 14 字节状态帧
- 9 字节死亡/复活（txid 随机，等 ACK 重传）
- 监听 5006 接收下行命令
- 模拟 L431：`GAME_START` → HP=300/alive=1；`SET_HP` → 直接改；`YELLOW_CARD` → 内部计数；`FORCE_POWER_OFF/ON` → 改 `powerOn`
- 所有需要 ACK 的命令回 `ACK(result=0)`

### P7：文档同步
- `proto.markdown` 全面重写为 V2（保留 V1 章节作为历史参考）
- `README.md` 加"协议版本"说明
- 新增 `docs/NETWORK_SETUP.md`：Wi-Fi/IP/端口/防火墙配置

## 兼容性策略

**采用方案 A：纯 V2**

- ESP32 端已纯 V2，无 V1 代码
- 服务端 V1 代码仅做解析回退，不再发送 V1
- `RobotSimulator` 提供 `--v1` 命令行开关，用于回归测试 V1 路径
- 正式上线时移除 V1 解析

## 部署配置

正式比赛网络（ESP32 README §8.3）：
```
Wi-Fi SSID: RM_GAME
Wi-Fi 密码: 12345678
服务器 IP: 192.168.1.3
ESP32 上行: UDP 5005
ESP32 下行: UDP 5006
```

服务端需要在防火墙放行 UDP 5005 入站 + 5006 出站。

## 风险点

1. **`robot_id → team` 无协议下发**：必须服务器侧持久化映射，重启不丢
2. **设备表来自内存中的 `DEVICE_ANNOUNCE`**：服务器重启后必须等待 ESP32 再次上报，才能下发命令
3. **ID 分配必须带 ESP32 Wi-Fi MAC**：不能在多台未分配设备中猜测目标，也不能复用旧的 15 字节帧
4. **事务号去重需持久化**：QSettings 存 `(robotId, type, txid) → result`，重启后 ESP32 重发不重复执行
5. **黄牌语义变化**：V1 是服务器显示，V2 是命令 ESP32 扣血。UI 上"红牌" 不再单独存在，由 L431 累计 3 次黄牌后判负
6. **状态帧功率字段**：`power` 是整数瓦特，UI 需要量程选择（如 0-500W）
7. **`power_on` 与 `shoot_enabled` 无关**：分别表示底盘供电和射击许可，UI 不要混淆

## 测试策略

1. **协议单元测试**：`RobotProtocolSmokeTest` 覆盖 V2 设备注册、状态、健康、战斗结束和 21 字节 ASSIGNMENT 往返
2. **模拟器对测**：`RobotSimulator --v2` ↔ `Scompetition` 完整流程
3. **ESP32 真机**：单台 ESP32 烧录最新固件，跑通登记→状态→开始→黄牌→结束→复活链路
4. **多车**：2 台 ESP32 + 2 台 RobotSimulator 混合测试

## 2026-09-25 模拟器修复验收

已修复模拟器缺少 DEVICE_ANNOUNCE、仍按旧 15 字节读取 ASSIGNMENT、未更新分配身份、未接收上行源端口 ACK 的问题。现在按本机 IPv4 保存独立模拟设备 MAC/ID/手柄绑定，两个环回地址可同时模拟红蓝车。服务端等待成功 ACK 后才更新队伍；周期设备刷新保留正在编辑的分配表单。

补齐命令去重、非法 MAC/目标 ID 拒绝、端口占用回滚、重启事务号持久化、ID 冲突检测、重新分配后的旧状态/端点清理。停止监听会取消尚未确认的命令，避免关闭后的 socket 因重试重新绑定随机端口。

HUD 的 V2 血量上限为 300，V1 为 5000；TCP 快照携带协议版本、功率、供电和链路字段。离线状态不再把未知供电当作实时状态。COMBAT_END 只表示停止交火，不改射击许可。模拟器准备模式 HP=200、正式模式 HP=300，普通受击扣 20，黄牌扣 50/50/判负。手动状态编辑仍用于注入测试快照；自动复活、BLE 和硬件保护不属于该模拟器的完整仿真范围。

### 上游核对

GitHub commits/HEAD 在 2026-09-25 返回 0b190aa9c47c14290f3bb46934aca4ce41cf632e（2026-09-22）。已取得该提交相对本地 39853c85308afb7acd6df5a7df51d203b7ef78c1 的完整 compare，只有 1 次提交，修改 README、手柄接收机说明、l431_link.c 和 main.c；没有修改 robot_protocol.h 或 ASSIGNMENT 处理函数。改动主要为 L431 链路超时、B7 接收及手柄控制策略，UDP V2 固定帧格式不变。原始响应保存在 build/upstream-latest.json 和 build/upstream-compare.json。

### 验证结果与边界

- MSVC2022 / Qt 6.8.3 Debug 全量构建通过。
- SimulatorIntegrationTest：真实双设备 UDP、服务端、TCP 选手端及重新分配/重启流程通过。
- QT_SCALE_FACTOR=1、1.25、1.5、2 均通过；每档验证 4 种 HUD 窗口尺寸、5 档 HP 和 5 档热量的像素边界、状态文字及裁切，另验证服务端、选手端和模拟器的指定窗口尺寸。每档生成 50 张 PNG，路径 build/msvc2022_64-Debug/simulator-verification/scale-倍率/。
- 测试配置使用临时 INI，且验证实际文件路径。早期未隔离测试写入的已确认 ID 71/72 分配和对应死亡事务已清理；注册表备份位于 build/pre-test-settings-cleanup.reg，原有 ID 2 数据保留。
- 此次新增验证全部离屏运行，不控制用户显示器或鼠标键盘；真实显示器跨屏、GPU 视频和实机协议验收仍需在对应环境进行，不能由离屏通过推断。

## 2026-10-01 自动判定、赛后数据与动效

- 新增 MatchReferee（matchreferee.h/.cpp）：纯逻辑判定，时间由调用方注入；规则与优先级见 README“自动胜负判定”。MatchRefereeTest 18 个用例覆盖同时击毁、复活取消、开局 0 HP、黄牌扣血不计伤害、DQ 优先于击毁、离线宽限（暂停不计/重连清零/双方离线不判）、V1/V2 血量比例、各级平局规则。
- 导播台倒计时归零不再按手填比分结算，改为 timeExpired → MatchReferee；选手端同样不在本地结算，只跟随服务端 match_state（新增 notice / summary / summaryExpanded 字段）。
- 修复：暂停后继续会重发 GAME_START 导致机器人回满血；开始下一局会清零小局积分（现在仅“重置比赛”清零）。
- HUD：可展开赛后数据面板（折叠条 → 统计、血量曲线、关键事件；鼠标/Enter/Space/Esc 可切换）；阵亡/离线面板变暗、受击边缘闪光、胜者边框与“本局胜者”标记；最后 10 秒计时脉冲、暂停时计时变灰；比分变化闪金；跑马灯改为按时间匀速滚动；修复标题与热量文字颜色、视角标签初始为空。
- 控制台：新增“胜负判定”区（实时状态、规则、裁判判定、展开赛后数据）；4:3 窗口不再上下堆叠导致控制区只有约 90 px 高。
- 验证：MSVC2022 / Qt 6.8.3 Debug 全量构建，ctest 4/4 通过（离屏）。未验证：真实显示器与 GPU 视频下的动画流畅度、ESP32 实机上 DEATH/STATUS 的实际时序。
