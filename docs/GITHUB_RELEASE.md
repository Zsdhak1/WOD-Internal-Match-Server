# GitHub 自动构建与三包发布

## 触发方式

将全部源码、素材、新工具脚本和 `.github/workflows/build-release.yml` **commit 并 push 到 GitHub 的任意分支**后，Actions 自动执行Windows x64构建与GitHub Release。仅本地commit不会触发GitHub。

- push任意分支：构建、测试、打包、发布正式Release；只有默认分支的发布成为Latest。
- pull_request：同样构建/测试/打包，但不发布Release（fork PR也没有发布写权限）。
- Actions → Build and Release Windows x64 → Run workflow：允许手动重新发布。
- 仅监听分支push，不监听自动Release创建的tag，避免循环触发。
- 每次run/attempt使用独立tag，发布目标为该次提交SHA。连续提交不取消已开始或等待的构建；每次提交独立运行，不使用会丢弃中间待执行任务的concurrency组。
- 构建、测试、包内启动验证任何一步失败，都不发布半成品。

仓库必须启用Actions，并允许GitHub Token写入Release。工作流仅在release job申请 `contents: write`，不需要个人PAT。组织策略若禁止写入，需由管理员放行。首次执行必须把所有未跟踪的新源码和素材一并提交，仅提交workflow会构建失败。

## Release文件

统一版本以 `CMakeLists.txt` project VERSION为准，时间为UTC构建会话开始时间，例如：

```text
Scompetition-Server_v0.2.0_20261010-170000_win64_Release.zip
Scompetition-Client_v0.2.0_20261010-170000_win64_Release.zip
Scompetition-ResultsWeb_v0.2.0_20261010-170000_win64_Release.zip
SHA256SUMS_v0.2.0_20261010-170000_win64_Release.txt
```

三个ZIP均带顶层组件目录、build-info.json和逐文件SHA256的PACKAGE_MANIFEST.json；完整解压，不要只取EXE。

| 包 | 启动方式 | 内含依赖 |
|---|---|---|
| 服务端 | Start.cmd / Scompetition.exe | Qt6.8.3 DLL、Windows平台/多媒体等插件、app-local VC143 CRT、静态FFmpeg/ffprobe、结算视频、默认布局、文档 |
| 选手端 | Start.cmd / ScompetitionClient.exe | 对应Qt DLL/插件、VC143 CRT、FFmpeg/ffprobe、结算视频、布局、文档 |
| 结果网页 | Start.cmd / ResultsWeb.exe | PyInstaller onedir内置x64 Python、SQLite/哈希等标准库运行依赖、HTML/CSS/JS及红蓝素材、模板、启动说明 |

服务端和选手端不是把整个开发目录复制后改名：分别复制自己的EXE并使用windeployqt解析依赖，再补app-local CRT、外部工具和视频。模拟器和测试EXE不进入三个正式包。无需安装Qt、Visual Studio或Python；Windows系统组件、摄像头/显卡驱动和现有浏览器仍由系统提供。目标是Windows10/11 x64，不支持32位Windows。

## 网页部署

1. 完整解压ResultsWeb包，双击Start.cmd，保持命令窗口开启。
2. 首次管理员账号admin与生成密码在窗口中显示，请保存；不在包中放默认固定密码。
3. 展示页 `http://127.0.0.1:8765/`；管理后台 `/admin`。
4. 数据写在EXE旁 `data/results.sqlite3`，不是_internal。升级前备份data并恢复到新包，**不把data重新上传为Release**。
5. 局域网启动例：`Start.cmd --host 0.0.0.0 --allowed-host 192.168.1.10`。换成本机真实IP，并放行Windows防火墙；客户端访问该IP的8765端口。多个地址可重复--allowed-host。
6. HTTP只用于可信局域网，公网需HTTPS和访问限制。程序不附浏览器，不操作用户已有浏览器。

## 本地复现

```powershell
# 使用独立环境安装构建依赖，示例使用已有python
python -m venv build/tools/release-python
build/tools/release-python/Scripts/python.exe -m pip install -r tools/requirements-release.txt

# 统一入口，构建+7项CTest+三包+包内启动验证；Qt/VS路径可覆盖
.\tools\build.ps1 -Configuration Release -Test -SplitPackages `
  -PythonExecutable .\build\tools\release-python\Scripts\python.exe
```

输出：`build/releases/<buildId>-split/`、`build/packages/`；索引`build/latest-split.json`。缓存仍保留原路径，不重命名。旧默认不带-SplitPackages的合并包导出保持兼容；GitHub只上传三个新独立包与校验文件。

## 验证及适用边界

- 7项桌面CTest、9项网页HTTP/CLI测试。
- 检查EXE和FFmpeg PE类型为AMD64。
- 清除Qt/Python环境变量并将PATH限制为Windows System32，分别运行Qt --package-smoke与包内FFmpeg。
- 网页EXE以随机端口/临时数据库启动，实测公开接口、管理页、JS/CSS和两张JPEG，退出后清理临时数据；不打印生成测试密码。
- ZIP CRC、包清单及不存在测试/正式数据库、日志、缓存/凭据检查。
- 不等于实车、摄像头或现场防火墙验收，也不是无任何软件的全新虚拟机测试。
- 当前仍是60秒击杀积分+SD模式，规则限制见COMPETITION_RULE_COMPLIANCE.md；本次只实现构建发布，不切换比赛规则。
