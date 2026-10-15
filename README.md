# WOD-Internal-Match-Server

校内 1v1 步兵机器人对抗赛的比赛系统，基于 Qt 6 / C++。包含赛事服务端、选手端、车载端模拟器和转播画面。

> [!NOTE]
> 阅读方式：正文是正常使用需要的内容。蓝色 **Note** 是补充说明，绿色 **Tip** 是能省事的做法，紫色 **Important** 是必须遵守的约定，橙色 **Warning** 是容易出错的地方。带 ▶ 的折叠块是参考资料，需要时再展开。

## 程序组成

| 程序 | 用途 |
| --- | --- |
| `Scompetition.exe` | 服务端：接收机器人数据、判定胜负、导播控制台和转播输出窗口 |
| `ScompetitionClient.exe` | 选手端：登记红/蓝方身份，推送摄像头画面，显示全屏 HUD |
| `RobotSimulator.exe` | 模拟 ESP32 车载端，没有实车时用来联调 |

## 比赛规则

默认使用正式 BO3 规则：

- 每局开始前 5 秒倒计时，双方设备确认后进入 180 秒小局
- 每小局每方 3 次复活，阵亡 5 秒后由车载端申请，服务端批准后复活
- 小局之间 120 秒准备时间，先赢 2 局的一方获胜
- 时间到时先比剩余复活次数，再比剩余血量
- 没有技术暂停；数据相同、同时耗尽或数据不可信时，交给裁判手动判定，不会自动加时

相关文档：[正式比赛流程](<docs/FORMAL_MATCH_FLOW.md>)

> [!NOTE]
> 旧的 60 秒击杀积分 + SUDDEN DEATH 规则仍然保留。在控制台“胜负判定”里切换到“旧调试 · 击杀积分”即可使用，只用于调试。

> [!WARNING]
> 目前还没有实现小组获胜点总榜和自动晋级。模拟器测试通过不代表实车安全，也不代表完全符合比赛手册。规则对照见 [规则符合性审查](<docs/COMPETITION_RULE_COMPLIANCE.md>)。

## 快速上手（用模拟器）

1. 启动 `Scompetition.exe`，点击“启动监听”。
2. 启动两个 `RobotSimulator.exe`，选择 V2 协议，服务器地址填服务端 IP（同一台电脑填 `127.0.0.1`）。
3. 在服务端“设备管理”里选中设备，填机器人 ID、红/蓝方和手柄 MAC，点“分配 ID + 手柄”。
4. 启动 `ScompetitionClient.exe`，选择红方或蓝方和摄像头，完成登记。
5. 在控制台点“显示转播画面”，选好输出屏幕，再点“开始本局”。

> [!TIP]
> 同一台电脑跑两台模拟器时，把两边的“本机绑定 IPv4”分别填成 `127.0.0.2` 和 `127.0.0.3`，否则它们会抢同一个下行端口 `5006`。

> [!NOTE]
> 第 3 步点了分配后，要等设备回 ACK 才算生效，列表里的队伍才会更新。

### 没有车时测试开赛

正式模式要求双方设备都在线才能开赛。只想走一遍服务端流程时，勾选“比赛与转播”里的“无车调试”：

- 开赛时不检查设备、不发 GAME_START，倒计时结束后直接进入小局
- 没有血量数据，时间到后用“判红方胜 / 判蓝方胜”手动判定

> [!IMPORTANT]
> 无车调试仅用于测试。每次启动默认关闭，比赛进行中不能切换，切换时会重置当前 BO3。

## 控制台常用功能

**队伍表**：点“导入队伍表”，选一组对阵后点“应用所选对阵”。换队会清空比分、牌面和计时，已登记的选手需要重新登记。文件格式见下面的[队伍表格式](#队伍表格式)。

**判罚**：先在“状态”页选中机器人，再发红牌或黄牌。黄牌前两张各扣 50 HP，第三张判负；红牌是否直接判负可以在“胜负判定”里设置。

**导播**：可以手动切换全场、红方、蓝方视角，也可以按设定时间自动轮换。点击“已接入视频源预览”里的缩略图可直接切到对应画面。

**本地全场摄像头**：服务端可以直接采集本机 USB 摄像头作为全场视角，不占红蓝选手的位置。
相关文档：[USB 全场摄像头](<docs/SERVER_USB_CAMERA.md>)

**车辆检录**：“赛前检查”会打开一个只读窗口，查看各车的连接状态、模块健康和遥测数据，不发送任何命令。
相关文档：[车辆检录](<docs/CHECKIN_MODE.md>)

**裁判录入**：每小局结束后自动切到“裁判录入辅助”页，可以复制胜方、双方剩余血量和复活次数、结束方式，填到外部成绩系统里。

**选手端 HUD**：登记后显示准星、受击红边和命中标记，准星两侧的弧线显示热量允许的可发弹数和冷却进度。
相关文档：[选手热量弧](<docs/PLAYER_HEAT_ARCS.md>)

**转播布局**：在“HUD 布局”页调整各元素的位置和大小，可以保存或加载 `broadcast_layout.ini`。非 16:9 输出会自动留黑边，不会被拉伸。

> [!IMPORTANT]
> 正式 BO3 进行中不能换队，一场打完再换。

> [!NOTE]
> 程序不会自动上传成绩，裁判录入需要人工完成。选手端的命中标记只是根据血量变化推断出来的，不能作为判罚依据。

## 队伍表格式

每行一组对阵，第一列红方、第二列蓝方，第一行可以写表头。支持 CSV、TSV、TXT 和 JSON。

模板文件：[team_names_template.csv](team_names_template.csv)、[team_names_template.json](team_names_template.json)

<details>
<summary>各格式的详细写法</summary>

文本文件使用 UTF-8 编码。空行和以 `#`、`//` 开头的行会被忽略。

CSV 或 TXT（逗号分隔）：

```csv
红方队名,蓝方队名
赤焰战队,蓝盾战队
Alpha,Bravo
```

TSV 的格式相同，只是两列之间用 Tab 分隔。

JSON 使用数组，字段名可以是 `red`/`blue`，也可以是 `redName`/`blueName`。数组也可以放在 `teams`、`matches`、`pairs` 或 `teamPairs` 字段里：

```json
[
  { "redName": "赤焰战队", "blueName": "蓝盾战队" },
  { "red": "Alpha", "blue": "Bravo" }
]
```

</details>

## 网络与协议

| 方向 | 端口 |
| --- | --- |
| 机器人 → 服务端 | UDP `5005` |
| 服务端 → 机器人 | UDP `5006`（ESP32 监听） |
| 选手端 → 服务端（登记） | TCP `5010` |
| 红方 / 蓝方视频 | UDP `5011` / `5012`（TCP 端口 +1 / +2） |

当前使用 V2 协议，与 ESP32 仓库 `2470519590/ESP32-S3-Module-for-Judgement-System` 保持同步。设备先用 Wi-Fi MAC 上报自己，再由服务端分配机器人 ID 和队伍；机器人身份不按 IP 识别。

相关文档：[protocol.h](protocol.h)、[matchprotocol.h](matchprotocol.h)、[V2 迁移说明](<docs/V2_MIGRATION_PLAN.md>)

<details>
<summary>命令可靠性和 V1 兼容</summary>

- 比赛控制命令只单播给指定的机器人 ID，设备需要回 ACK。
- 死亡、复活这类可靠事件按事务号去重，服务端重启后也不会重复执行。
- 服务端仍能解析旧的 V1 帧，方便测试旧固件。新部署请使用 V2。

</details>

## 视频链路

选手端读取摄像头，经 FFmpeg 编码成低延迟 H.264 通过 UDP 发给服务端，服务端分别解码红蓝两路画面后显示在转播窗口。两端都优先使用程序目录下的 `tools/ffmpeg/ffmpeg.exe`，发布包不需要另外安装 FFmpeg。

> [!TIP]
> 测试视频性能请使用 Release 版，Debug 版会明显卡顿。

<details>
<summary>编码参数和结算动画</summary>

- 编码为 960×540、2.5 Mbps；编码积压时主动丢帧，避免延迟越积越大。
- 结算动画放在 `Assets/gamefinishvideo`，每个动画由同名的 `_rgb.mp4` 和 `_alpha.mp4` 组成。

</details>

## 构建

需要 Visual Studio（MSVC）和 Qt 6.8.3（含 Multimedia 模块）。使用项目自带的构建脚本：

```powershell
# Release 构建，运行测试，生成服务端和选手端两个独立 ZIP
.\tools\build.ps1 -Configuration Release -Test -SplitPackages -QtDirectory <Qt 安装目录>

# Debug 增量编译，不导出
.\tools\build.ps1 -Configuration Debug -BuildOnly -QtDirectory <Qt 安装目录>

# 只显示构建计划，不实际执行
.\tools\build.ps1 -Configuration Release -Plan
```

> [!IMPORTANT]
> 版本号只以 `CMakeLists.txt` 中的 `project(... VERSION ...)` 为准，当前为 0.2.0。

<details>
<summary>常用参数</summary>

| 参数 | 说明 |
| --- | --- |
| `-QtDirectory` | Qt 安装目录，默认 `G:\Dev\Qt\6.8.3\msvc2022_64` |
| `-VisualStudioDirectory` | VS 安装目录，不填时按脚本里写死的几个路径查找 |
| `-WindowsSdkEnvironment` | 使用项目内置的 Windows SDK（`build/tools/windows-sdk/Enable-WindowsSdk.ps1`） |
| `-PythonExecutable` | 打包校验用的 Python，可以用 `build/tools/release-python` 里的虚拟环境 |
| `-Jobs` | 并行编译数，默认 4 |

</details>

### 构建产物

```text
build/
  cache/msvc2026_64-Debug/     # 开发缓存（VS 2022 时为 build/msvc2022_64-*）
  releases/<构建 ID>-split/    # 展开的发布目录
  packages/                    # ZIP 和 SHA256SUMS
  deprecated/                  # 不再使用的旧构建，按版本分组
  latest-split.json            # 最近一次发布的索引
```

构建 ID 的格式是 `v版本_yyyyMMdd-HHmmss_win64_配置`，时间为 UTC。每个发布目录里的 `build-info.json` 记录了编译器、Qt 版本、Git 提交，以及工作区是否有未提交的改动。

> [!WARNING]
> - 不要重命名正在使用的 CMake 缓存目录。缓存里记录的是绝对路径，改名后就不能增量编译。
> - 分发时要带上整个目录，不能只复制 EXE。Qt DLL、FFmpeg 和结算素材都要一起带上。

> [!NOTE]
> 每次导出都会新建目录，不会覆盖旧的。旧构建确认不用后，移到 `deprecated/`，不要删除。

### 测试

项目共有 14 项 CTest，覆盖判定逻辑、复活协议、正式 HUD、真实 UDP/TCP 集成和无车调试等场景。界面测试在 offscreen 下运行，使用临时配置，不会影响本机的比赛设置。

```powershell
ctest --test-dir build\cache\msvc2026_64-Debug -C Debug --output-on-failure
```

> [!WARNING]
> `simulator_integration` 等集成测试会占用 UDP `5006`。运行前先关掉正在运行的模拟器，否则测试会报 “Simulator failed to bind”。

> [!TIP]
> 如果提示找不到 offscreen 插件，先把 `QT_QPA_PLATFORM_PLUGIN_PATH` 设为 Qt 目录下的 `plugins\platforms`。

## GitHub 自动发布

push 到任意分支后，GitHub Actions 会自动构建、测试，并发布服务端和选手端两个 Windows x64 ZIP。只有 main 上的发布会被标为最新版，PR 只构建、不发布。

相关文档：[GitHub 发布说明](<docs/GITHUB_RELEASE.md>)

## 日志

服务端和选手端都在程序目录下的 `logs/` 里写日志（`Scompetition-日期.log`、`ScompetitionClient-日期.log`）。程序崩溃时会在同一目录生成 `.dmp` 文件，可以用 Visual Studio 或 WinDbg 打开。

> [!NOTE]
> 程序目录不可写时，日志会写到系统的应用数据目录。

## 已弃用

`results-web` 成绩网页已停止维护，也不再打包发布，代码暂时保留在仓库里。