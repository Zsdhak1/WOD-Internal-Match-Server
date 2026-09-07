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
2. 启动两个 `RobotSimulator.exe`，分别选择红方和蓝方，机器人编号均可设为 `1`。
3. 在模拟器中启动状态发送，或使用自动演示生成攻击、受击、死亡、复活和射击权限事件。
4. 启动 `ScompetitionClient.exe`，选择红方或蓝方，选择视频设备并完成登记。
5. 在控制台打开转播画面，选择输出屏幕和当前导播视角。

选手端会枚举 Windows 摄像头设备，并为选择的设备生成稳定的视频源标识。登记成功后，客户端使用 FFmpeg 将摄像头编码为 H.264，并通过 MPEG-TS/UDP 发送到服务端。服务端按红蓝方独立接收、解码，再通过 `BroadcastWindow::setSourceFrame()` 更新转播画面。

客户端和服务端都会优先使用程序目录中的 `tools/ffmpeg/ffmpeg.exe`，因此正式发布包不依赖系统安装 FFmpeg。程序目录或系统 `PATH` 中的 FFmpeg 仍作为开发环境兼容路径。客户端推流参数为 1280×720、30 FPS、H.264 `ultrafast/zerolatency`，服务端输出 960×540 BGRA 帧供 Qt 显示。

## 网络协议

- 机器人到服务端：UDP `5005`
- 选手端到服务端：TCP `5010`
- 红方视频：服务端 TCP 端口 + `1`，默认 UDP `5011`
- 蓝方视频：服务端 TCP 端口 + `2`，默认 UDP `5012`
- 机器人身份：`team + robotId`
- 机器人状态帧：兼容基础 10 字节状态帧和带热量的 12 字节扩展状态帧
- 比赛事件：死亡、复活、受击、攻击、允许射击和禁止射击使用独立事件帧

协议实现见 [protocol.h](protocol.h)、[matchprotocol.h](matchprotocol.h)，详细说明见 [proto.markdown](proto.markdown)。

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
