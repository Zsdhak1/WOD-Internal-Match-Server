# WOD-Internal-Match-Server

基于 Qt/C++ 的校内赛事服务端、选手端、车载端模拟器和赛事转播控制台，当前面向 **1v1 步兵机器人对抗赛**。

## 项目功能

- 接收并解析机器人通过 UDP 上报的状态和比赛事件
- 按 `(team, robotId)` 识别机器人，不使用发送 IP 作为机器人身份
- 机器人在线、血量、热量、存活状态和射击权限监控
- 死亡、复活、受击、攻击、允许射击和禁止射击事件日志
- 独立的全屏无边框转播输出窗口和赛事控制台窗口
- 1 分钟比赛倒计时，支持开始、暂停和重置
- 以实时视频为背景，叠加队伍名称、机器人血条、热量条、比分和比赛状态
- 机器人血量下降时显示血条闪烁和受击伤害提示
- 红方血条镜像显示，并从右向左填充
- 支持中央 HUD、比分背景、标题、计时和信息面板的位置及尺寸调节
- 支持布局保存和加载，文件名为 `broadcast_layout.ini`
- 支持导入队伍名称表，并在每局结束后手动切换下一组队伍
- 每小局结束自动播放红胜、蓝胜或平局结算动画；可终止本局并播放终止动画
- 选手端无需密码，只登记红方/蓝方身份并绑定视频流来源
- 支持手动切换和按时间自动切换全场、红方、蓝方视频视角
- 提供 ESP32 车载端模拟器，便于没有实机时联调

## 程序组成

- `Scompetition.exe`：赛事服务端、机器人 UDP 监控、转播控制台和选手端登记服务
- `ScompetitionClient.exe`：选手端登记窗口、视频设备选择和全屏 HUD
- `RobotSimulator.exe`：模拟 ESP32 节点，向 UDP `5005` 发送真实机器人协议数据

机器人身份由协议中的队伍编号和机器人编号共同决定。例如，在同一台电脑上启动两个模拟器时，可以都使用 `127.0.0.1`，但需要使用不同的队伍编号。

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

1. 启动 `Scompetition.exe`。默认监听机器人 UDP `5005` 和选手端登记 TCP `5010`。
2. 启动 `RobotSimulator.exe`，选择 **V2 协议**（默认），机器人 ID 任意。
3. 在"设备管理"面板输入机器人 ID，选择队伍并点 **分配队伍**——V2 协议中队伍由服务器维护。
4. 在模拟器中启动状态发送，或使用自动演示生成攻击、受击、死亡、复活和射击权限事件。
5. 启动 `ScompetitionClient.exe`，选择红方或蓝方，选择视频设备并完成登记。
6. 在控制台打开转播画面，选择输出屏幕和当前导播视角。控制台下方"已接入视频源预览"区会实时显示所有在线源的缩略图，点击任一缩略图切换为该视角。

选手端通过 `QMediaDevices::videoInputs()` 枚举 Windows 摄像头，使用 `QCamera + QVideoSink` 直接解码显示本地预览（无需 FFmpeg）。登记成功后，同一摄像头帧经 rawvideo 管道喂给 FFmpeg 编码为 H.264，通过 MPEG-TS/UDP 发送到服务端；服务端按红蓝方独立接收、解码，再交给 `BroadcastWindow::setSourceFrame()` 渲染。点击视频源旁的 **内置摄像头** 可自动选中系统默认摄像头（通常为笔记本内置 Integrated Camera）。

结算动画资源位于 `Assets/gamefinishvideo`。每个动画由同名的 `_rgb.mp4` 和 `_alpha.mp4` 组成，播放器使用 FFmpeg 将灰度蒙版作为透明度逐帧合成。导播台播放实际胜负动画，选手端则根据登记队伍将对手胜利动画映射为 `defeated`。

客户端和服务端都会优先使用程序目录中的 `tools/ffmpeg/ffmpeg.exe`，因此正式发布包不依赖系统安装 FFmpeg。程序目录或系统 `PATH` 中的 FFmpeg 仍作为开发环境兼容路径。客户端推流参数为 1280×720、30 FPS、H.264 `ultrafast/zerolatency`，服务端输出 960×540 BGRA 帧供 Qt 显示。

## 网络协议

- 机器人到服务端（上行）：UDP `5005`
- 服务端到机器人（下行）：UDP `5006`（ESP32 监听）
- 选手端到服务端：TCP `5010`
- 红方视频：服务端 TCP 端口 + `1`，默认 UDP `5011`
- 蓝方视频：服务端 TCP 端口 + `2`，默认 UDP `5012`

### V2 协议（当前正式版）

- `version=2`，与 ESP32 仓库 `2470519590/ESP32-S3-Module-for-Judgement-System` 同步
- 状态帧 14 字节：`robot_id + hp + heat + power + alive + shoot_enabled + power_on`
- 普通事件 5 字节：受击、攻击、允许/禁止射击、L431 链路断开/恢复
- 可靠事件 9 字节：死亡、复活（含 `transaction_id`，服务器必须 ACK + 按 `(robot_id, type, txid)` 去重）
- 下行命令（服务器→ESP32:5006）：
  - `0x81 GAME_START` / `0x82 GAME_END`：9 B + ACK
  - `0x83 ASSIGNMENT`：分配 robot_id + 手柄 MAC，15 B + ACK
  - `0x84 STATUS_REQUEST`：5 B，无 ACK
  - `0x85 SET_HP`：11 B + ACK（合法 0..300）
  - `0x86 YELLOW_CARD`：9 B + ACK（前 2 次 -50 HP，第 3 次判负）
  - `0x87 FORCE_POWER_OFF` / `0x88 FORCE_POWER_ON`：9 B + ACK
- ACK `0xF0`：10 B，回 `result`：`0` 成功 / `1` 拒绝 / `2` 失败
- `robot_id → team` 映射由服务器在"设备管理"面板分配并持久化（QSettings）
- 死亡/复活事务去重表也持久化，防止服务器重启后 ESP32 重发旧事务被重复执行

### V1 协议（旧版兼容，仅解析）

服务端仍识别 `version=1` 帧（10/12 B 状态 + 6/8 B 事件，含 `team` 字段），便于旧固件回归测试。**新部署一律使用 V2**。

协议实现见 [protocol.h](protocol.h)、[matchprotocol.h](matchprotocol.h)，V2 迁移细节见 [docs/V2_MIGRATION_PLAN.md](docs/V2_MIGRATION_PLAN.md)，原始规范见 ESP32 仓库 README 和 `docs/v2更新说明.md`。

## Windows 正式发布

项目的 Windows 发布内容包括 `Scompetition.exe`、`ScompetitionClient.exe`、`RobotSimulator.exe`，以及 `tools/ffmpeg/ffmpeg.exe`、`tools/ffmpeg/ffprobe.exe` 和许可证文件。配置并构建后执行以下命令生成安装目录：

```powershell
cmake --install build/msvc2022_64-Debug --config Debug --prefix dist
```

也可以使用 CPack 生成 Windows 安装包。发布目录中的 FFmpeg 文件必须与可执行文件一同分发，不能只复制三个 Qt 程序。

## 编译

项目使用 CMake、Ninja 和 Qt 6，需要 `Widgets` 与 `Network` 组件。

```text
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=<Qt 安装目录>
cmake --build build
```

Windows 下编译完成后，可以使用 Qt 提供的 `windeployqt` 部署运行所需的 Qt DLL。
