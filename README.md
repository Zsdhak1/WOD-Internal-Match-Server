# WOD-Internal-Match-Server

基于 Qt/C++ 的校内赛事服务端、选手端、车载端模拟器和赛事转播控制台，当前面向 **1v1 步兵机器人对抗赛**。

## 当前规则与适用范围

**本版本仍是 60 秒、击杀 +1、同分 SUDDEN DEATH 模式，不是中俄大学生机甲挑战赛手册的正式 BO3 裁判版本。** 180 秒、有限复活、按复活次数/血量判胜、小组获胜点及晋级管理尚未实现；规则缺口、裁判组待确认事项和实车验收清单见 [规则符合性审查](<docs/COMPETITION_RULE_COMPLIANCE.md>)。检录模块在线、命令 ACK 或模拟器回归通过不等于实车安全和赛事合规。

独立结果网页的启动、人工维护、击杀积分排序和分辨率适配见 [results-web 使用说明](<results-web/README.md>)；该网页不自动同步 Qt 服务端，也不是该手册要求的获胜点榜。

## 项目功能

- 接收并解析机器人通过 UDP 上报的状态和比赛事件
- 按 `(team, robotId)` 识别机器人，不使用发送 IP 作为机器人身份
- 机器人在线、血量、热量、存活状态和射击权限监控
- 死亡、复活、受击、攻击、允许射击和禁止射击事件日志
- 独立的全屏无边框转播输出窗口和赛事控制台窗口
- 1 分钟比赛倒计时，支持开始、暂停和重置
- 比赛画面使用无外框的紧凑顶部 HUD：白色队名、蜂窝纹理红蓝血条和条内血量数字，与中央倒计时对齐
- 不在比赛画面显示在线、存活、射击、热量、功率、链路等监控文字；相关数据保留在赛事控制台，判罚和赛后数据仍可显示
- 机器人血量下降时保留血条伤害缓冲效果，不在顶部悬浮额外伤害文字
- 红方血条镜像显示，并从右向左填充
- 中央计时 / 左右比分底板使用单张合并透明素材，整体缩放；文字保持动态，左右信息面板默认贴合中央宽度
- 倒计时和比分采用内置原创棱角数码字形（矢量绘制，不依赖系统字体）；文字层始终位于素材上方，暂停使用亮冷白而非暗灰
- 支持整体中央 HUD、标题、计时、比分文字和信息面板的位置及尺寸调节；旧布局自动迁移至 V4（重置旧信息面板尺寸和偏移）
- 支持布局保存和加载，文件名为 `broadcast_layout.ini`
- 支持导入队伍名称表，并在每局结束后手动切换下一组队伍
- 本局击杀积分：击杀对方 +1，常规时间内允许复活继续；时间到积分高者胜，同分进入 SUDDEN DEATH（下一次单方击杀定胜负）。胜利不额外加分，新局清零；仅最终结算发送 GAME_END，判罚与离线判负保留
- 每小局结束自动播放红胜、蓝胜或平局结算动画；可终止本局或由裁判手动判定
- 结算动画后显示可展开的赛后数据面板（剩余血量、伤害、受击、攻击、阵亡、判罚、离线时长、血量曲线、关键事件），导播台展开状态同步到选手端
- 选手端无需密码，只登记红方/蓝方身份并绑定视频流来源
- 登记后的选手端显示细小静态准星；比赛进行中己方血量下降触发短暂淡红边，对方血量下降触发小型命中标记。首次快照、重连、换身份、暂停及结束不触发；血量快照不含扣血原因，因此对方黄牌扣血也可能被显示为命中反馈，不能当作命中裁判依据
- 选手端准星左右显示开放双弧：左侧为热量预算可发弹数 / 最大 20 发，低于等于 3 发为橙色、不可发为红色；右侧估计恢复下一发的 1 秒冷却周期。数字只按己方真实热量快照更新，不因动画提前增加；无效/超时/暂停/离线显示未知，射击许可关闭或死亡显示 0
- 热量规则来自枪管 `main_xjx` 分支 `6222c4c5655d2722a224d0e4541ccd624a6b5161` 的 `Core/Inc/thermal.h`、`Core/Src/thermal.c` 与 `docs/CAN_PROTOCOL.md`：上限 200、每发 +10、每秒阶梯减 10（默认 main 是旧调试代码）。左侧用 `floor((200-heat)/10)` 计算完整热量预算，不代表弹仓余弹或硬件射击授权；右弧没有设备冷却时钟，首次仅估计，热量下降时校准，周期满后等待遥测确认、不虚构连续恢复。连续 1500 ms 未收到己方有效热量则显示 `--/20`
- 支持手动切换和按时间自动切换全场、红方、蓝方视频视角
- 提供 ESP32 车载端模拟器，便于没有实机时联调
- 所有窗口按参考分辨率统一缩放，支持 16:9、4:3 和超宽屏；转播 HUD 在非 16:9 输出中保持比例并自动留白

## 程序组成

- `Scompetition.exe`：赛事服务端、机器人 UDP 监控、转播控制台和选手端登记服务
- `ScompetitionClient.exe`：选手端登记窗口、视频设备选择和全屏 HUD
- `RobotSimulator.exe`：模拟 ESP32 节点，向 UDP `5005` 发送真实机器人协议数据

V2 使用设备 Wi-Fi MAC 发现设备，再由服务端分配唯一机器人 ID 和红蓝队伍；V2 数据包不携带队伍编号。V1 才使用包内的队伍与机器人编号。

### 用模拟器分配红蓝队伍

1. 启动服务端的 UDP 监听（默认 5005）。
2. 启动 RobotSimulator.exe，选择 V2。服务器地址填服务端 IPv4；同机测试填 127.0.0.1。
3. 单车的“本机绑定 IPv4”可保留 0.0.0.0；同机两车分别使用 127.0.0.2 和 127.0.0.3，避免争用下行 UDP 5006。启动后，服务端设备列表会显示模拟 ESP32 MAC。
4. 在服务端“设备管理”选择设备，填写不同的机器人 ID、红/蓝方及有效的手柄 MAC（6 字节、不能全零），点击分配。收到成功 ACK 后才保存队伍映射。模拟器内的 ID 和手柄 MAC 会同步显示。
5. 分别分配红蓝设备后开始比赛。模拟器准备模式 HP 为 200，GAME_START 后为 300；普通受击扣 20，前两张黄牌各扣 50，第三张判负。复活通过模拟器按钮触发；它不模拟硬件的自动 5 秒复活、电源保护或 BLE 实际连接。

模拟设备身份按本机 IPv4 分别保存，重启后保留；V2 的队伍和 ID 不再通过模拟器下拉框手动指定。可以在运行时修改 HP、热量、功率等状态输入，以测试异常快照，输入框本身不强制模拟完整 L431 状态机。服务端停止监听时禁止发送分配；重新分配 ID 后会清理旧 ID 的比赛状态。

### 协议与显示回归

SimulatorIntegrationTest 运行真实 Qt 窗口类、两台环回 UDP 模拟设备和 TCP 选手端，使用临时 INI 配置，不读写比赛用的注册表配置，也不在桌面显示窗口。覆盖设备发现、21 字节分配、ACK、重试去重、重启、重新分配、掉线、供电、链路、射击状态，以及 V1 热量归零。

离屏渲染检查 HUD 的 1024×768、1280×720、1920×1080、2560×1080；血量 0/1/30/150/300 的像素填充边界、条内数字与几何布局，并检查监控文字/热量条不出现在比赛画面。另检查服务端/选手端 1024×768、1600×900，以及模拟器 640×480、1024×768。

SimulatorIntegrationTest 还覆盖完整击杀积分链路：两台模拟器开赛、受击、击杀后 +1 但继续比赛、时间到按分数判胜、下发 GAME_END、选手端收到赛后数据并同步展开，以及下一局清零、暂停/继续不重发 GAME_START、终止不改积分。同分加时检查服务端/选手端在 00:00 仍进行比赛、热量反馈有效、不回满血、加时暂停恢复及决胜击杀。赛后面板在四种 HUD 尺寸下分别检查折叠和展开状态的边界，截图为 `summary-*.png`。

PlayerCombatOverlayTest 覆盖 HP 基线、首次/重复/回血/未知/掉线/身份变化和准星/红边/命中标记绘制；SimulatorIntegrationTest 增加红蓝双方真实 TCP 反馈、暂停恢复、重连、协议变化、回合时钟重置、720p/1080p/4K 准星居中和退出登录回归。另有 FieldCameraTest（可注入采集后端、首帧/换源/断连/有界恢复）和 CheckinModelTest（首连会话/健康证据/模块异常），集成测试覆盖服务端本地全场、缩略图与检录界面。当前共注册 7 项 CTest；可设 SCOMP_TEST_PLAYER_ONLY=1 仅运行集成测试中的选手反馈场景。

构建后可运行 `ctest --test-dir build/msvc2022_64-Debug --output-on-failure`。若开发机找不到离屏插件，先设置 QT_QPA_PLATFORM_PLUGIN_PATH 为 Qt 安装目录下的 plugins/platforms。用 QT_SCALE_FACTOR=1、1.25、1.5、2 分别运行集成测试可复验缩放矩阵；PNG 保存在测试程序目录的 simulator-verification/scale-倍率/。此验证不覆盖真实显示器跨屏迁移、GPU 视频和硬件固件联调。

## 自动胜负判定

判定逻辑在 `matchreferee.h/.cpp`，不依赖 UI 和系统时钟，由 `MatchRefereeTest` 覆盖。控制台“胜负判定”区可调整规则，设置保存在本机 QSettings（`Scompetition/MatchReferee`）。同一时刻多个条件成立时按以下顺序取第一条：

1. 裁判终止 / 判红方胜 / 判蓝方胜
2. 红牌判负（可关闭）、累计 3 张黄牌判负；双方同时满足判平局
3. 离线判负：比赛进行中一方连续离线超过设定秒数（默认 10 秒，0 为关闭）且另一方在线；暂停时间不计，双方都离线时不自动判
4. 常规时间（1 分钟）：一方从存活变为阵亡（`alive=0` 或 HP=0，或收到 DEATH）对方击杀积分 +1。重复死亡事件/状态不重复加分，复活不撤销积分，下一次阵亡可再次得分。开局已死亡、暂停期间、离线/设备移除和可识别的判罚阵亡不计击杀；协议没有击杀者/伤害原因，1v1 正常阵亡归对方，管理员 SET_HP=0 等不带原因的情况仍需裁判纠正比分。
5. 时间到：只比较本局击杀积分，不比较血量、伤害或判罚次数。更高者胜；同分保持当前 HP 进入 SUDDEN DEATH，无限时、一命定胜负。中央计时冻结 00:00，状态栏标明加时，射击和热量反馈保持有效。边界击杀先等待 0.3 秒同时击杀确认。
6. SUDDEN DEATH：单方击杀确认 0.3 秒后判胜；双方在窗口内同时击杀各得 1 分但不判平局，继续等待复活后的下一次单方击杀。加时不发送 GAME_START/GAME_END，不回满血；只有最终裁决才发送 GAME_END。

中央比分和控制台积分均为本局击杀分（裁判可手动纠正）；结算不追加胜利积分，新局/重置清零。旧“击毁即结束”和“胜方小局积分自动 +1”选项及其旧设置不再生效。伤害统计按 HP 下降累计，黄牌自身的 50 HP 扣减不计入对手伤害。常规/加时暂停后继续均不发送 GAME_START，避免机器人回满血。

## 队伍名称表

在 `Scompetition.exe` 控制台的队名区域点击 **导入队伍表**，导入后会自动应用第 1 组队伍。点击 **下一组队伍** 可以循环切换到下一组。

切换队伍时会自动：

- 更新红方和蓝方队伍名称
- 重置 1 分钟比赛计时
- 重置本局比分
- 清除红牌和黄牌显示
- 清除血量变化记录，避免上一局的血量变化触发受击特效
- 将新的比赛状态重新发布给已登记的选手端

支持的文件格式如下：

- **CSV**：使用英文逗号 `,` 分隔两列
- **TSV**：使用 Tab 字符分隔两列
- **TXT**：每行使用英文逗号或 Tab 字符分隔两列
- **JSON**：使用数组保存多组红蓝队伍

文本文件使用 UTF-8 编码。每行的第 1 列是红方队名，第 2 列是蓝方队名；首行可以写列标题，也可以省略。空行以及以 `#` 或 `//` 开头的注释行会被跳过。

### CSV 模板

```csv
红方队名,蓝方队名
赤焰战队,蓝盾战队
Alpha,Bravo
```

可直接使用项目根目录中的 [team_names_template.csv](team_names_template.csv)。

### TSV 模板

下面两列之间必须是实际的 **Tab 字符**，不是多个空格：

```text
红方队名	蓝方队名
赤焰战队	蓝盾战队
Alpha	Bravo
```

### TXT 模板

TXT 文件可以使用逗号分隔，格式与 CSV 相同：

```text
红方队名,蓝方队名
赤焰战队,蓝盾战队
Alpha,Bravo
```

### JSON 模板

JSON 支持 `red` / `blue` 或 `redName` / `blueName` 字段：

```json
[
  {
    "redName": "赤焰战队",
    "blueName": "蓝盾战队"
  },
  {
    "red": "Alpha",
    "blue": "Bravo"
  }
]
```

也可以把数组放在 `teams`、`matches`、`pairs` 或 `teamPairs` 字段中。可直接使用项目根目录中的 [team_names_template.json](team_names_template.json)。

## 本地测试流程

1. 启动 `Scompetition.exe`，在控制台点击“启动监听”。默认监听机器人 UDP `5005`，选手端登记服务端口为 TCP `5010`。
2. 启动 `RobotSimulator.exe`，选择 **V2 协议**（默认），机器人 ID 任意。
3. 等待 ESP32 上报 `0x0A DEVICE_ANNOUNCE`，在“设备管理”面板选择设备，输入手柄 BLE MAC，设置机器人 ID 与队伍，然后点击“分配 ID + 手柄”。服务器会发送 21 字节 `0x83 ASSIGNMENT`，并等待 ESP32 的 ACK。
4. 在模拟器或 ESP32 端启动状态发送，生成状态、受击、攻击、死亡、复活、设备健康和射击权限事件。
5. 启动 `ScompetitionClient.exe`，选择红方或蓝方，选择视频设备并完成登记。
6. 在控制台打开转播画面，选择输出屏幕和当前导播视角。控制台下方“已接入视频源预览”区会实时显示所有在线源的缩略图，点击任一缩略图切换为该视角。

### 多显示器和分辨率验证

控制台的“转播输出屏幕”选择框使用系统屏幕列表。选择副屏后，转播窗口会在副屏全屏显示，控制台仍可留在主屏。布局会根据窗口宽高比例缩放；4:3 输出上下留白，超宽输出左右留白，避免 HUD 被拉伸。

自动化验证可使用 `tools/capture-broadcast.ps1`，脚本默认选择第一块非主屏并不会把验证窗口移到 `(0,0)`：

```powershell
.\tools\capture-broadcast.ps1 -Size 1024x768 -OutFile build\layout-verification\secondary-1024x768.png -Screen secondary
.\tools\capture-broadcast.ps1 -Size 2560x1080 -OutFile build\layout-verification\secondary-2560x1080.png -Screen secondary
```

也可以直接为三个程序指定验证窗口：`SCOMP_MAIN_SCREEN` / `SCOMP_MAIN_SIZE`、`SCOMP_CLIENT_SCREEN` / `SCOMP_CLIENT_SIZE`、`SCOMP_SIMULATOR_SCREEN` / `SCOMP_SIMULATOR_SIZE`。`secondary` 表示第一块非主屏，数字值表示从 0 开始的屏幕索引。未设置这些变量时，生产启动行为保持不变。

视频链路性能测试请使用 **Release** 构建。Qt Debug DLL、断言和未优化代码会明显放大摄像头转 QImage、H.264 编码和解码开销。

选手端通过 `QMediaDevices::videoInputs()` 枚举 Windows 摄像头，使用 `QCamera + QVideoSink` 直接解码显示本地预览（无需 FFmpeg）。登记成功后，同一摄像头帧经 rawvideo 管道喂给 FFmpeg 编码为 H.264，通过 MPEG-TS/UDP 发送到服务端；服务端按红蓝方独立接收、解码，再交给 `BroadcastWindow::setSourceFrame()` 渲染。点击视频源旁的 **内置摄像头** 可自动选中系统默认摄像头（通常为笔记本内置 Integrated Camera）。

选手端全屏画面使用 GPU 加速的 `QOpenGLWidget` 绘制，不再对每一帧执行全屏 `QPixmap` 缩放，也不再人为限制全屏渲染帧率。推流编码使用摄像头实际选择的帧率和 960×540、2.5 Mbps 的 H.264 低延迟配置，并在 FFmpeg 管道积压时主动丢帧。服务端解码器积压超过 128 KB 时会重启解码器清除陈旧管道。选手端日志每 2 秒记录实际 FPS、推流积压和累计丢帧，便于定位性能问题。

结算动画资源位于 `Assets/gamefinishvideo`。每个动画由同名的 `_rgb.mp4` 和 `_alpha.mp4` 组成，播放器使用 FFmpeg 将灰度蒙版作为透明度逐帧合成。导播台播放实际胜负动画，选手端则根据登记队伍将对手胜利动画映射为 `defeated`。

客户端和服务端都会优先使用程序目录中的 `tools/ffmpeg/ffmpeg.exe`，因此正式发布包不依赖系统安装 FFmpeg。程序目录或系统 `PATH` 中的 FFmpeg 仍作为开发环境兼容路径。客户端相机输入按 1280×720 和摄像头支持的最高帧率处理，H.264 编码为 960×540、2.5 Mbps 的 `ultrafast/zerolatency` 流，服务端输出 640×360 BGRA 帧供 Qt 显示。

## 车辆检录模式

控制台「比赛与转播 → 赛前检查」可打开独立只读检录窗口，不下发命令、不暂停比赛或摄像头。显示车辆/未分配设备的连接状态、模块摘要、服务端进程首次有效上行时间、本检录会话首次观察连接时间、最近上报、设备/手柄 MAC、遥测和诊断提示。六个健康位分别为 L431PM、枪管、装甲板1～4；只有合法、新鲜、来源匹配的 V2 健康帧才判定在线/未在线，V1、未上报、过期及离线明确显示未知。首连历史保留至新检录会话；服务端首个上行时间只由真实记录显示，不用最近时间推算。详情见 `docs/CHECKIN_MODE.md`。模块在线不等于硬件验收通过。

## 服务端本地 USB 全场摄像头

服务端现可直接采集本机 USB/内置摄像头，作为独立 `field` 全场视角，不占红蓝选手身份。在「本地全场摄像头」选择设备、点击「启动采集」，再「切到全场」或点击全场缩略图。默认不打开相机，只记忆设备 ID；支持同名设备区分、刷新、换源、停止释放、断开清帧、同设备重连及有界异常重试。摄像头独立于比赛暂停/结束/重置。详情与硬件验证边界见 `docs/SERVER_USB_CAMERA.md`。

## 网络协议

- 机器人到服务端（上行）：UDP `5005`
- 服务端到机器人（下行）：UDP `5006`（ESP32 监听）
- 选手端到服务端：TCP `5010`
- 红方视频：服务端 TCP 端口 + `1`，默认 UDP `5011`
- 蓝方视频：服务端 TCP 端口 + `2`，默认 UDP `5012`

### V2 协议（当前正式版）

- `version=2`，与 ESP32 仓库 `2470519590/ESP32-S3-Module-for-Judgement-System` 同步
- 状态帧 `0x01`：14 字节，`robot_id + hp + heat + power + alive + shoot_enabled + power_on`
- 普通事件 5 字节：受击 `0x04`、攻击 `0x05`、允许/禁止射击 `0x06/0x07`、L431 链路断开/恢复 `0x08/0x09`、脱离战斗 `0x0B`
- 可靠事件 9 字节：死亡、复活（含 `transaction_id`，服务器必须 ACK + 按 `(robot_id, type, txid)` 去重）
- 设备注册 `0x0A`：25 字节，`robot_id + ESP32 Wi-Fi MAC + controller BLE MAC + event_queue_drops + udp_send_failures`
- 设备健康 `0x0C`：6 字节，`robot_id + component_online`（bit0=L431PM，bit1=枪管，bit2~5=装甲板 1~4）
- 下行命令（服务器→ESP32:5006）：
  - `0x81 GAME_START` / `0x82 GAME_END`：9 B + ACK
  - `0x83 ASSIGNMENT`：分配 robot_id + ESP32 Wi-Fi MAC + 手柄 BLE MAC，21 B + ACK
  - `0x84 STATUS_REQUEST`：5 B，无 ACK
  - `0x85 SET_HP`：11 B + ACK（正式比赛 0..300；准备模式 0..200）
  - `0x86 YELLOW_CARD`：9 B + ACK（前 2 次 -50 HP，第 3 次判负）
  - `0x87 FORCE_POWER_OFF` / `0x88 FORCE_POWER_ON`：9 B + ACK
- 所有比赛控制命令只支持按非零 `target_robot_id` 单播，不支持广播；下行必须发往 ESP32 的 UDP `5006`。
- ACK `0xF0`：10 B，回 `result`：`0` 成功 / `1` 拒绝 / `2` 失败
- `robot_id → team` 映射由服务器在“设备管理”面板维护并持久化（QSettings）
- 死亡/复活事务去重表也持久化，防止服务器重启后 ESP32 重发旧事务被重复执行

### V1 协议（旧版兼容，仅解析）

服务端仍识别 `version=1` 帧（10/12 B 状态 + 6/8 B 事件，含 `team` 字段），便于旧固件回归测试。**新部署一律使用 V2**。

协议实现见 [protocol.h](protocol.h)、[matchprotocol.h](matchprotocol.h)，V2 迁移细节见 [docs/V2_MIGRATION_PLAN.md](docs/V2_MIGRATION_PLAN.md)，原始规范见 ESP32 仓库 README 和 `docs/v2更新说明.md`。

### 运行日志与崩溃转储

服务端启动后会在可执行文件目录的 `logs/` 下创建 `Scompetition-YYYYMMDD.log`；选手端会创建 `ScompetitionClient-YYYYMMDD.log`，其中包含实际 FPS、推流积压和累计丢帧。若发生未处理异常，会在同一目录生成 `Scompetition-crash-*.dmp`，可直接交给 Visual Studio 或 WinDbg 分析。若程序目录不可写，日志会回退到 Qt 的应用本地数据目录。

## GitHub 自动构建与独立发布包

提交并 **push 到 GitHub 任意分支**后，自动构建、测试并发布三个 Windows 10/11 x64 ZIP：**服务端、选手端、结果展示网页**。各包带运行依赖、素材、启动入口和SHA256清单；网页内置Python/SQLite，不需目标电脑安装Python。PR只构建不发布；支持手动运行。仅本地commit不会触发GitHub。

部署、工作流权限、三包内容与本地复现见 [GitHub Release说明](<docs/GITHUB_RELEASE.md>)。使用统一入口加 `-SplitPackages` 生成三包；不带该参数保留旧合并包导出方式。

## Windows 正式发布与构建目录

当前源码版本为 **0.2.0**，版本号以 `CMakeLists.txt` 的 `project(... VERSION ...)` 为唯一来源。历史二进制不会因源码升级而自动变成新版本。

推荐在项目目录运行统一入口：

```powershell
# 默认使用 MSVC 2022，构建 Release、安装完整运行目录并生成 ZIP
.\tools\build.ps1 -Configuration Release

# Debug 构建并运行已注册的 CTest，再导出
.\tools\build.ps1 -Configuration Debug -Test

# 只做增量编译，不导出；查看计划不会配置或构建
.\tools\build.ps1 -Configuration Debug -BuildOnly
.\tools\build.ps1 -Configuration Release -Plan
```

Qt 默认路径为 `G:\Dev\Qt\6.8.3\msvc2022_64`，可用 `-QtDirectory` 覆盖；可用 `-VisualStudioDirectory`、`-BuildDirectory`、`-Jobs` 指定工具链、缓存和并行度。旧的 `tools/build-msvc-debug.cmd` 转调统一入口，不再生成旧工具链目录。发布依赖 Widgets、Network、Multimedia、OpenGLWidgets，以及 Windows 的 dbghelp。

### 命名和分区约定

发布名称使用 `v版本号_yyyyMMdd-HHmmss_平台_配置`，时间为 **UTC 构建会话开始时间**。同一构建的目录、ZIP 与元数据使用相同时间，不使用归档时间冒充构建时间。例如：

```text
build/
  msvc2022_64-Debug/                         # 保留稳定路径的最新开发缓存
  msvc2022_64-VideoRelease/                  # 保留稳定路径的最新 Release 缓存
  releases/v0.2.0_20261009-073000_win64_Release/
    bin/build-info.json
  packages/WOD-Internal-Match-Server_v0.2.0_20261009-073000_win64_Release.zip
  deprecated/v0.1/                          # 已废弃的历史构建
  deprecated/legacy-unknown/                # 无法证明软件版本的历史产物
  verification/legacy/                     # 历史截图、渲染和视频验证数据
  logs/legacy/                             # 历史构建与测试日志
  references/                              # 上游源码与协议核对材料
  backups/                                 # 注册表等恢复备份
  latest-Release.json                       # 最近一次成功导出的索引
  latest-Debug.json
```

- 不重命名正在使用的 CMake 缓存：缓存/Ninja 含绝对路径，移动后不能直接增量编译。已归档缓存只作历史保存，需要重新配置才能构建。
- 每次成功导出创建新的版本/时间目录，不覆盖旧发布。脚本不会自动判定旧发布为 deprecated；确认不再使用后再归档，避免误收纳仍在使用的版本。
- `bin/build-info.json` 保存版本、会话时间、编译器、Qt、Git 提交及脏工作区标记；`latest-*.json` 另记录完成时间、导出位置、ZIP SHA256 和本次是否执行测试。
- 历史整理使用 `tools/organize-legacy-builds.ps1`，可先加 `-WhatIf` 查看，只移动不删除；构建版本来自原 CPack 元数据，历史时间来自程序或产物修改时间并在迁移清单中明确来源。无法证明版本的项目标为 `legacy-unknown`，不编造版本号。
- FFmpeg、Qt DLL、结算素材和布局文件必须随发布目录分发，不能只复制三个 EXE。

## 手动编译

项目使用 CMake、Ninja 和 Qt 6。手动编译仍支持稳定缓存路径；配置时 CPack 自动加入版本号、UTC 时间、平台和配置：

```powershell
cmake -S . -B build/cache/manual-Release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<Qt 安装目录>
cmake --build build/cache/manual-Release
cpack --config build/cache/manual-Release/CPackConfig.cmake -B build/packages
```

手动增量构建若不重新配置，元数据时间保持上次配置会话的时间；需要每次构建准确命名时请使用统一入口。也可在配置时显式传入 `-DSCOMP_BUILD_TIMESTAMP=yyyyMMdd-HHmmss`（UTC）。
