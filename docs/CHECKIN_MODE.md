# 只读车辆检录模式

## 范围与验证状态

本组件是独立 QWidget 检录面板和可测试的 QtCore 纯状态 reducer；Lead 已接入 MainWindow 的“打开车辆检录（只读）”按钮，默认打开 1400×850 独立窗口。它没有 RobotManager 实例、socket、RobotCommander、比赛裁判或摄像头控制入口，不发送 `GAME_START`、罚牌、状态请求、设备分配等命令，不修改赛况，不暂停现有比赛/摄像头。面板自身 500 ms 定时器仅刷新本地展示与健康证据时效。

**验证分阶段记录：Lead 在本轮 review 修改前已串行构建并通过 7/7 CTest，集成测试生成了实际检录窗口截图并检查开启检录不打断 USB 摄像头。review 本身没有运行主 Qt 构建、没有修改活动 CMake cache；本轮新增双首连/身份变更/摘要改动仍须 Lead 重建并复测。未连接真实硬件，不能把单一窗口截图等同于全部 DPI 视觉验收。** 软件检查不能替代枪管、装甲板、供电或安全现场验收。

## 文件与职责

- [checkinmodel.h](../checkinmodel.h)、[checkinmodel.cpp](../checkinmodel.cpp)：不依赖 QObject/Widgets/Network、无隐式时钟/持久化的纯状态模型；时间由调用方注入。
- [checkinpanel.h](../checkinpanel.h)、[checkinpanel.cpp](../checkinpanel.cpp)：RobotManager 快照到纯模型的适配、模式开关、滚动表格、选中详情。无 `Q_OBJECT`，不需生成本组件 moc。
- [checkin_model_test.cpp](../tools/checkin_model_test.cpp)：仅 QtCore 的确定性测试程序，无实机、网络、注册表、睡眠或随机数。

## Lead 接入接口

当前接入是受 MainWindow 生命周期管理的独立窗口；按钮 objectName 为 `openCheckin`。panel/model 四个源文件已加入服务端和编译 MainWindow 的集成测试 target；不需要加入客户端/模拟器。下列代码只说明已有接入接口，实际实现以源码为准。

```cpp
#include "checkinpanel.h"

m_checkinPanel = new CheckinPanel(parent);
// rightTabs->addTab(m_checkinPanel, tr("车辆检录"));

const auto refreshCheckin = [this] {
    m_checkinPanel->updateSnapshot(m_robotManager->robots(),
                                  m_robotManager->devices());
};
connect(m_robotManager, &RobotManager::robotsChanged, this, refreshCheckin);
connect(m_robotManager, &RobotManager::devicesChanged, this, refreshCheckin);
refreshCheckin(); // 未开启时只缓存，不开始计时
```

实际成员名称按现有 MainWindow 调整。`updateSnapshot(robots, devices, observedAtMs = -1)` 的时间默认当前 epoch ms；测试可传固定时间。每次应推送完整 robots/devices 集合；在 GUI 线程调用面板接口。RobotManager 为遥测在线/离线的权威来源，面板不复制其 800 ms sweep 判定。即使没有注册变化，也应推送 `robotsChanged` 的离线快照；不要只在健康帧触发刷新。

### 为什么还需要被动健康证据

[RobotInfo](../robotmanager.h) 的 `componentOnline` 默认 `0`，`linkUp` 默认 `true`；Lead 已补 `healthReceived`、`lastHealthSeenMs` 和 `firstSeenMs`，DeviceInfo 也有 `firstSeenMs`。面板仍采用本检录会话启用期间的被动健康证据，不从快照重新构造健康接收事件，避免开启/继续/新会话复用既往帧。仅有原始位图时仍显示“未确认”和逐模块“未知”，绝不把缺状态/默认值判正常。

现有 MainWindow UDP 接收路径在成功 `proto::parseDatagram` 后，对有效的 V2 `TypeDeviceHealth` 被动记录：

```cpp
proto::Frame frame;
if (proto::parseDatagram(data, frame)
    && frame.version == proto::kVersion2
    && frame.type == proto::TypeDeviceHealth
    && frame.robotId != 0
    && sender.protocol() == QAbstractSocket::IPv4Protocol) {
    m_checkinPanel->observeDeviceHealth(frame.robotId, frame.componentOnline,
                                        sender.toString(),
                                        QDateTime::currentMSecsSinceEpoch());
}
// 随后保留原有 RobotManager::handleDatagram(data, sender, port) 路径。
// 在 RobotManager 的 robotsChanged 回调推送新快照，不能用旧位图立即验收。
```

当前接入先 observe 再 handleDatagram，避免同步 `robotsChanged` 已刷新但证据还未入库。接口不会 parse/ACK/发送任何内容，不取代现有处理；只有成功验证的上行健康帧才允许作为证据。不要从 `RobotInfo.componentOnline` 调用 observe 来伪造“已收到”信息。快照适配器只读取真实 `firstSeenMs` 来展示服务端首个上行，不读取 `lastSeenMs` 补造首连，也不放宽健康证据要求。

外部操作接口：

```cpp
m_checkinPanel->setInspectionEnabled(true);  // 开启/继续同一会话
m_checkinPanel->setInspectionEnabled(false); // 退出，冻结记录，不动比赛
m_checkinPanel->startNewSession();           // 明确清空所有首连/健康证据并开启
bool active = m_checkinPanel->inspectionEnabled();
const auto &model = m_checkinPanel->stateModel(); // const，只读诊断
```

面板按钮也提供这些操作，“新检录会话”按钮带确认对话框；程序化 `startNewSession()` 不弹框，由调用方确认意图。

## 会话与首次连接语义

1. 默认未开启；后台快照仅供缓存，不产生首连历史。
2. 第一次开启建立会话；在线机器人（有正的 lastSeen）和近期注册设备的首连时间 = **本检录会话首次观察到连接的调用时刻**，不是 lastSeen，更不是设备真实开机时间。
3. 首次看到离线/未知的机器人，不从历史 lastSeen 反推首连；其后首次观察在线才记录。
4. 刷新、队伍重新映射、离线重连不重写首连。V2 以 robot ID 识别，唯一关联的注册设备则以 MAC 识别；V1 身份包含 team + ID。双时间独立展示：`serverFirstSeenMs` 是本服务器进程首次记录有效上行/注册（唯一 MAC 关联时取机器人与设备正值最早者），不等于物理设备开机时间；`firstConnectedMs` 是本检录会话首次观察连接。服务端首个上行未知时保持未知，不用 lastSeen 反推。
5. MAC 注册晚于遥测时合并先前 robot 记录，保持较早的本会话首连。未分配 ID 的 MAC 以后分配 ID，首连仍保留。
6. 从快照消失的记录保留为历史离线，避免重现丢失首连；退出停止观察，保留冻结记录，再开启同一面板继续原会话，并清除旧健康证据。
7. **“新检录会话”是唯一主动清空方式**，清空面板内会话记录/证据并开启新会话，不清空 RobotManager 的服务端首个上行记录。从新会话当前在线快照观察到的连接以新时刻计。组件销毁/程序退出自然丢失全部内存会话，没有 QSettings 写入。

## 字段与健康解释

| 展示 | 含义 |
|---|---|
| Robot ID / team / V1,V2 | 快照中 ID、队伍及最近协议。team=0 未分配；1/2 红/蓝。仅设备注册没有队伍信息，显示未分配/未知。 |
| 在线/离线 | 机器人使用 RobotManager `online`，并要求已有正的 lastSeen。注册-only 不伪造遥测在线，而显示“注册近期/遥测未知”或“注册超时/遥测未知”。 |
| 服务端首个上行 | RobotInfo/DeviceInfo 的真实 firstSeenMs；唯一 MAC 关联时取正值中最早者。是当前服务器进程的首次有效上行/注册，不是硬件实际开机时间，不随新检录会话清空。未记录则明确未知。 |
| 本会话首连 | 本会话首次观察到机器人在线或设备近期注册的时刻（本机本地时间显示，精确毫秒），与服务端首个上行独立。 |
| 最近上报 / 最近注册 | 机器人任意有效上行的 lastSeen 或设备 DEVICE_ANNOUNCE 的 lastSeen；不是首连或特定状态/健康帧时间。 |
| 观察到离线 | 本面板观察到从在线/注册近期变为离线/注册超时的时刻，不是精确断线时刻；首次就是离线则保持未观察，重连清除当前离线时间。 |
| MAC / 地址 / 源端口 | 注册的 ESP32 Wi-Fi MAC、手柄 MAC，最近来源地址和上行端口；缺 MAC 明确未知。源端口不是下行命令端口。 |
| HP / 热量 / 功率 | 显示已存遥测；负值明确未知。V1 没有功率和健康位图，功率强制未知；V1 10 B STATUS 没有热量，只有12 B STATUS 会设置热量。历史遥测不能证明模块正常。 |
| 原始位图 | 快照原样显示。没有有效被动证据时标未确认，不参与通过判断。 |
| 健康证据时间 | 专门收到合法 V2 DEVICE_HEALTH 的本机接收时刻，不借用 lastSeen。 |
| 诊断计数 | 注册中的 eventQueueDrops / udpSendFailures 是累计值，提示需复核，不断言为当前故障。 |

模块定义来自本项目 [README 协议说明](../README.md#L187-L194)，[protocol.h](../protocol.h#L268-L277) 负责6 B帧构造/解析，[RobotManager](../robotmanager.cpp#L270-L280) 用 bit0 更新 L431 链路。未创造新位定义：

| 位 | 模块 | 新鲜且明确健康帧 |
|---|---|---|
| bit0 | L431PM | 1 在线 / 0 未在线异常 |
| bit1 | 枪管 | 同上 |
| bit2..5 | 装甲板1..4 | 同上 |
| bit6..7 | 保留 | 原样保留，不判断、不创建模块 |

健康有效需同时满足：V2机器人快照存在且在线；证据来源 IP 与快照一致；证据位图与快照一致；接收时间不在未来、距观察时间不超过3000 ms（恰好3000 ms仍有效）；不早于会话/身份更换/已观察断线后的要求。无证据、过期、离线、V1、仅注册、不一致均逐模块 **未知**。明确全0健康帧为6个异常，0x3f仅为6模块在线，**不表示现场检录通过**。重连需断线后的健康证据，旧健康不能复用为当前正常。

新建记录允许消费同会话中先 observe、后 snapshot 的首个合法健康帧。已有记录的 robot ID、协议或来源地址改变，则要求证据时间不早于**本次快照观察变更时刻**，不再仅与旧 lastSeen 比较，避免借用绑定变更前缓存的新 ID 健康帧。真实 observe → RobotManager → snapshot 若跨毫秒，变更触发的首帧可能保守显示未知，下一合法健康帧恢复；不为消除该短暂未知放宽证据或回填旧帧。稳定 MAC 的首连时间仍保留。

仅注册设备 freshness 默认6000 ms（本项目模拟器注册周期2000 ms）；它是 UI 的“近期注册”提示而非链路定论/RobotManager 离线阈值。可通过 `CheckinModel::Timing` 在纯模型层调整；与健康帧3000 ms时效是两个独立窗口。

## UI 与边界

- 原生垂直布局适应独立窗口或窄嵌入；表格/详情 splitter 可调整高度，逐像素横纵滚动、清晰至少10 pt字体、无单元格编辑。身份/连接后前置紧凑模块摘要，显示未知、6/6在线（非验收通过）或未在线模块数与名称；服务端首个上行和本会话首连并列，逐位模块/完整时间与异常保留详情，避免首屏全被注册/历史列占满。
- 刷新保持选中行、表格滚动条和详情滚动位置；字段带 tooltip，异常与未知均有文字，不能仅靠颜色判断。
- 重复注册 robot ID 不随意选择 MAC：显示机器人及各设备并标出冲突。来源 IP 不匹配也不合并。
- 没有 MAC 时只能依据 V1 team/ID 或 V2 ID识别；同协议同ID被另一实体复用且身份不可辨时无法可靠区分物理车辆，显式新会话/稳定MAC是清理手段。MAC缺失设备使用地址+端口回退，也不是保证稳定身份。
- 离线期间可能漏掉短重连（快照观察限制）；若未见断线则无法推断所有实际连接事件。原始默认 `alive/shootEnabled/powerOn/linkUp` 不作为“已测正常”证据，也不将射击禁止、未通电等赛况状态误判模块损坏。
- 观察时间使用 wall-clock epoch；系统时钟回拨时健康证据变未知，无法重建真实单调连接时间。本会话历史不会因回拨自动重写。

## 确定性测试与 Lead 构建建议

Lead 已在 CMake 加入下列独立模型测试，勿重复添加或另开并行主构建：

```cmake
add_executable(CheckinModelTest
    tools/checkin_model_test.cpp
    checkinmodel.cpp checkinmodel.h)
target_include_directories(CheckinModelTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(CheckinModelTest PRIVATE Qt${QT_VERSION_MAJOR}::Core)
add_test(NAME checkin_model COMMAND CheckinModelTest)
```

添加测试需要现有 CTest 开启区域（项目已经有同类测试）。组件加至所有编译 MainWindow 的 target 的 source 列表，避免集成测试链接缺符号。Lead 可在既有构建脚本管理/正确配置的稳定 cache上串行执行：

```powershell
# 先由 tools/build.ps1 管理工具链配置；具体稳定 cache 路径由 Lead 确认。
cmake --build <stable-cache> --target CheckinModelTest
ctest --test-dir <stable-cache> -R '^checkin_model$' --output-on-failure
```

覆盖源码断言：默认未开启；首连与lastSeen区分；刷新/重连/退出/继续/新会话；无健康证据与非零原始位图未知；明确全0和六模块逐位解码；保留位；3000 ms边界/过期/不同来源/位图不一致/乱序/旧会话证据；队伍映射；移除/重现；未分配注册MAC及注册时效；后续分配和迟到注册的首连保持；重复ID冲突；V1健康/功率/可选热量未知；首次观察离线不制造首连；服务器首个上行与会话首连独立、新会话不重写服务端首个上行、未知不回填；稳定MAC变更ID/IP拒绝变更前证据、保留双首连、后续健康恢复；新建记录接收先observe再snapshot首帧。

后续仍应由 Lead 实际编译/运行测试，并检查 QWidget在窄tab、独立窗口和100%/125%/150%/200% DPI下滚动/字体、退出检录时比赛视频不断流，以及真实ESP32的注册/健康发送周期、健康0/部分缺位/断线恢复和没有健康帧的旧固件。
