# 独立比赛结果网页

> 当前仅支持原有击杀积分 + SD 规则，不支持赛事手册的 BO3、有限复活与正负获胜点排名；不要作为该赛事正式成绩榜。差异与后续验收见 [规则符合性审查](<../docs/COMPETITION_RULE_COMPLIANCE.md>)。

Python **3.10 或更新版本**，只用标准库 HTTP + SQLite；前端为原生 HTML / CSS / JavaScript。无需 pip、npm、外部字体、CDN 或联网运行。**只人工维护记录，不连接、不伪装自动同步 Qt 服务端。** 不修改 Qt、CMake 或主项目文件。

## Windows x64 发布包（无需安装Python）

GitHub自动Release提供独立 `Scompetition-ResultsWeb_..._win64_Release.zip`。完整解压后双击Start.cmd，内置Python/SQLite与全部网页素材；保持窗口打开，保存首次显示的管理员密码。展示页 `http://127.0.0.1:8765/`、后台 `/admin`。私有数据库在EXE旁data目录；升级时备份/迁移data，不随Release发布。

局域网例：`Start.cmd --host 0.0.0.0 --allowed-host 192.168.1.10`，IP换成本机真实地址并放行防火墙。端口、重置密码等参数与源码版一致。以下内容说明源码版启动方式。

## 1. 启动（Windows源码版）

在 PowerShell 进入本目录后执行：

```powershell
python server.py
# Windows 若 python 未在 PATH，可用：
py -3 server.py
```

默认地址：

- 只读展示：`http://127.0.0.1:8765/`
- 管理后台：`http://127.0.0.1:8765/admin`
- 管理员名：`admin`
- 首次启动自动生成高强度随机密码，**仅在终端显示一次**；请立即保存。没有固定公开密码。
- 默认数据库为本目录下 `data/results.sqlite3`，下次启动仍保留全部队伍、比赛、结果与密码哈希。
- `Ctrl+C` 停止服务；重启会让全部会话失效，但不会丢失已保存记录。

也可以在第一次启动前自设密码（12–128个字符）：

```powershell
$env:RESULTS_ADMIN_PASSWORD = Read-Host '请设置管理员密码（12至128字符）'
python server.py
Remove-Item Env:RESULTS_ADMIN_PASSWORD
```

环境变量只在初始化或显式重置时生效，正常重启不会偷偷改密码。忘记密码时，先停止现有服务，执行 `python server.py --reset-password` 生成新随机密码；也可结合环境变量设置。登录后可在“导出 / 安全”修改密码，修改会退出所有会话。

> 输入环境变量示例仅适合可信本机；`Read-Host` 这里不是隐藏输入。正式使用可让首次启动生成密码，然后在后台输入新密码。

## 2. 局域网只读展示

推荐直接绑定本机实际局域网 IP，例如：

```powershell
python server.py --host 192.168.1.20 --port 8765
# 或监听全部IPv4网卡，必须显式放行浏览器使用的主机名/IP：
python server.py --host 0.0.0.0 --allowed-host 192.168.1.20 --port 8765
# 主机名可另加 --allowed-host results-host
```

其他电脑访问 `http://192.168.1.20:8765/`。若 Windows 防火墙拦截，仅对可信专用网络允许此 Python/端口。默认不监听公网；不会占用 DSH 的 19387，命令行明确禁止该端口。

**HTTP 不加密密码。** 只适合可信本机/可信隔离局域网；不要直接暴露公网。公网必须自己配置 HTTPS 反向代理、访问限制与防火墙。HTTPS 代理场景可加 `--secure-cookie`，反向代理需保留允许的 Host；纯 HTTP 下不能打开此开关，否则浏览器不会发送会话 Cookie。

## 3. 管理操作

1. 登录后台，添加队伍或在“名单导入”读取 CSV / JSON。
2. 导入必须先预览。逐行显示新增、跳过与错误。默认重复导致整次拒绝；若明确选择“跳过重复”，保留原队伍，绝不覆盖。
3. 修正所有错误后，点击“确认新增队伍”；10分钟预览令牌只能使用一次。预览后若队伍名单有变化，会拒绝提交并要求重新预览。
4. 在“比赛与结果”选择双方、日期、轮次，录入常规击杀、状态；常规同分时勾选加时并录入加时击杀。
5. 只有“已完成”比赛计入积分与胜负。删除或编辑比赛时自动重新计算；没有重复累加的缓存积分。
6. 编辑 / 删除需带当前版本，若其他页面已经更新会提示冲突，不会静默覆盖。已有比赛的队伍禁止直接删除，需要先明确删除相关比赛。
7. “导出 / 安全”导出积分胜负表或比赛明细 CSV。

后台支持所有队伍搜索、比赛状态筛选、取消编辑、删除确认、输入错误提示、请求超时提示。后台不会自动刷新并覆盖正在填写的表单；有“刷新数据”按钮。请求超时可能已提交成功，应先刷新核对，再决定重试。

## 4. 本次计分规则

- 每次常规击杀 **+1分**；比分高的一方胜。
- **常规比分相同必须加时，不能自动判胜。** 加时仍同分只能维持未完成/加时中，不能保存为已完成。
- 加时采用 SD 突然死亡：只允许常规同分进入加时，最终加时分差必须恰好为1。双方在0.3秒窗口互杀各+1后同分继续，因此允许 n : n+1，不限制为0:1。可手动选择胜方校验；与常规+SD合计比分冲突会拒绝。
- 积分榜定义为：已完成比赛的常规击杀 + 加时击杀，每次击杀一分；没有臆造的胜场额外奖励。常规和加时击杀分别保存，导出可核对。
- 排序依次为：积分高、胜场多、净击杀高。三项相同为并列排名；队名仅用于稳定显示顺序，不打破并列。
- 场次、胜、负、被击杀、净击杀同时显示。未参赛队伍也保留在榜中。

## 5. CSV / JSON 模板

本目录 `templates/teams.csv`（带 UTF-8 BOM）、`templates/teams.json` 可直接复制后编辑。

CSV 支持 UTF-8 / UTF-8 BOM、CRLF、中文与英文、标准引号转义：

```csv
name,short_name,organization
星火队,星火,示例大学
Blue Robotics,Blue,Example College
```

中文表头可用 `队名,简称,单位`；队名别名包括“队伍”“队伍名称”“名称”，单位也可用“学校”。只允许 name / short_name / organization 三列，不接受未知列、重复表头或多出的字段。

JSON 支持数组，或 `{"teams": [...]}`：

```json
[
  {"name": "星火队", "short_name": "星火", "organization": "示例大学"},
  {"name": "Blue Robotics", "organization": "Example College"}
]
```

队名必填，1–120字符；简称0–32字符；单位0–120字符。文本去除首尾空格，禁止控制字符；重复判断忽略英文大小写。最多5000队；网页上传文件上限1MiB，HTTP JSON请求上限2MiB。**不提供 XLSX 原生导入**，因为不引入第三方运行依赖；Excel / WPS 请另存为“CSV UTF-8”，不要用系统 ANSI / GBK CSV。潜在表格公式在导出时加文本前缀，防止打开 CSV 执行公式。

## 6. 展示大屏

只读展示无需管理员权限，接口不返回会话、CSRF令牌或密码。每3秒轮询，6秒请求超时，断连显示红色提示并保留最后一次数据；首次无数据有空态。

针对 4:3 `1440×1080`、`1920×1440` 与16:9 `1920×1080`、`2560×1440` 设计：

- 标题、概览、表头、最新赛况与底栏使用固定职责布局；表格使用剩余高度。展示页不显示英文装饰、规则注释或队名下的单位小字；单位/简称仍保存在后台和导出中。顶部概览仅显示参赛队伍和已完成比赛。
- 用户提供的红蓝背景及黑底四角素材随包保存在 `static/assets`。背景左右两半均按原始比例缩放并锚定各自外边缘，4:3保留红蓝边框而不是将16:9整图压扁；4:3底栏改为两行，16:9横排。黑底角图使用screen混合去黑，不修改原素材，成绩面板使用深色半透明层保证可读性。
- 底部以醒目状态块显示“数据更新时间”及最近成功取得数据的时间，不使用当前时钟冒充更新。断连时变红并标记“最后成功更新”，保留原时间。
- ResizeObserver 根据真实表格剩余高度和行高自动算每页行数，队伍过多分多页，不强迫塞一屏。
- 每页默认12秒，可改8/20/30秒。所有队伍在 `页数 × 每页时长` 的完整周期内出现；暂停时不保证自动周期。
- 上/下页按钮、页码、范围标记、轮播进度、全屏按钮；方向键翻页，空格暂停/继续。
- 鼠标悬停表格不会阻止无人值守轮播。滚轮会明确暂停，需点击“继续轮播”；键盘聚焦控制区临时暂停。
- 中英文长名在队名区域最多2行，不撑坏布局；悬停可看到完整名称与单位，后台和CSV保留完整内容。
- 支持 `prefers-reduced-motion`，关闭过渡动画；不依赖花哨闪烁。
- 若浏览器拦截全屏权限，可手动按 F11。使用浏览器100%缩放，现场大屏应检查是否有 OS/浏览器缩放。

**本机已用现有Chromium headless-shell真实渲染四分辨率并截图验收。** 截图和JSON报告属于生成产物，Git只保存验证说明与可复现脚本；新克隆应自行执行QA，不能宣称已拥有本机报告。 37支超长中英文名队伍逐页完整覆盖；当前紧凑大字版1440×1080、1920×1440、1920×1080每页7队/6页，2560×1440每页8队/5页（随实际可用高度自适应），均无横纵溢出且底栏可见。已验证resize、真实8秒轮播、空态/断连、后台登录/异步预览修改/提交/SD表单保存/登出，详见 verification/browser-report.json 与截图。用户现场显示器、F11、OS缩放与其他浏览器尚未实测。Kimi WebBridge无法连接，未操作用户已有浏览器或启动其daemon。

## 7. 安全与备份边界

- 管理密码以 PBKDF2-HMAC-SHA256、随机盐、310000轮存储，不存明文。
- 随机会话、HttpOnly / SameSite=Strict Cookie、8小时绝对过期、改密码失效全部会话。
- 写入必须认证及 CSRF；检查 Origin、Sec-Fetch-Site、允许Host，限制跨站请求与常见DNS rebinding路径。
- 单IP连续5分钟最多10次失败登录，超限暂时拒绝；进程重启后限流状态清空。
- SQLite参数化写入、事务导入、版本冲突、长度/类型/范围校验；未知静态路径拒绝，不暴露本地数据库、源码、模板或任意文件。
- Content Security Policy 禁止第三方资源、内联脚本、嵌套 iframe；前端数据用 textContent，不把队名作为 HTML 注入。
- 这是单管理员本地应用，不是公网托管平台：没有多角色授权、审计日志、自动备份或TLS服务。展示公开队名、单位、比赛与排名；名单不要包含敏感个人信息。
- 备份：停止服务后整体复制 `data` 目录。运行中不要只复制 sqlite3 主文件忽略 WAL。恢复时停止服务、替换完整备份目录，再启动。不要把数据库或密码终端截图公开分发。

## 8. 测试与API

运行自动化测试：

```powershell
python -m unittest discover -s tests -v
```

测试实际启动本地HTTP服务（临时随机端口）并发送真实HTTP请求，覆盖公开页、管理员认证、Cookie、安全头、CSRF、Host/Origin、导入预览/中文/BOM/重复/原子性/过期状态、队伍和比赛 CRUD、版本冲突、日期/比分边界、同分加时、排名/并列、持久化、导出与公式防护、密码修改/会话过期/登录限流。使用独立临时数据库，测试结束关闭服务。

源码打包（排除数据库/缓存/凭据；发布时使用主项目 build/packages，与 Qt 发布包分离）：

```powershell
# 默认只打包源码、素材、模板、测试与验证说明，不附本机生成报告
python tools/package.py --output-dir ../build/packages

# 已执行QA时，显式附本机JSON报告与截图（仍排除日志与数据库）
python tools/package.py --output-dir ../build/packages --include-verification
```

包名带主项目真实版本、UTC时间、平台与配置，包内包含 BUILD_METADATA.json 与逐文件SHA256。默认源码包明确标为未附验证证据；只有显式 --include-verification 才读取并附本机生成报告/截图，报告缺失时仍标记未验证。日志、.learnings、数据库、缓存、凭据和浏览器profile不发布；不会通过Qt构建器构建本独立标准库应用。不要将自行创建的data目录放入分发包。

主要接口：

| 方法 | 路径 | 权限 / 用途 |
|---|---|---|
| GET | `/api/public` | 无需登录；队伍、比赛、积分榜、规则 |
| POST | `/api/login` | JSON username / password；设置会话Cookie并返回CSRF |
| GET | `/api/session` | 登录；读取CSRF与过期时间 |
| GET | `/api/admin` | 登录；后台数据 |
| POST | `/api/logout` | 登录 + X-CSRF-Token |
| POST | `/api/password` | 登录 + CSRF；current_password / new_password |
| POST / PUT / DELETE | `/api/teams` 或 `/api/teams/{id}` | 登录 + CSRF；新增/编辑/删除；编辑删除需要version |
| POST / PUT / DELETE | `/api/matches` 或 `/api/matches/{id}` | 登录 + CSRF；比赛与结果；编辑删除需要version |
| POST | `/api/import/preview` | 登录 + CSRF；format / content / duplicate_mode；返回一次性token与逐行预览 |
| POST | `/api/import/commit` | 登录 + CSRF；token；10分钟有效，名单变化须重新预览 |
| GET | `/api/export/standings.csv` | 登录；排名、积分与胜负 |
| GET | `/api/export/matches.csv` | 登录；比赛完整结果 |

写接口JSON Content-Type必须 `application/json`；错误返回 `{"error":"中文错误原因"}`。比赛字段：team_a / team_b（队伍ID）、date（YYYY-MM-DD）、round、score_a / score_b（整数0–100000）、overtime（boolean）、overtime_score_a / overtime_score_b、winner_id（双方ID之一或null）、status（scheduled / live / overtime / finished / cancelled）。
