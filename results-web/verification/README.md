# 验证记录（独立 results-web）

## 后端

`http-report.json` 记录此次 **9/9** HTTP + CLI 测试通过，最新素材版日志为 `http-tests.log`。使用真实 ThreadingHTTPServer、独立临时 SQLite 与随机本机端口；包括登录、身份失效、CSRF/Origin/Host、CRUD/版本、持久化、中文BOM/重复/导入原子性与顺序、5000队上限、SD互杀3:4合法而分差2拒绝、积分无胜场奖励、导出公式安全、空环境密码拒绝、超大ID400。Python及JS语法检查通过。

## 真实浏览器渲染

`tests/browser_qa.mjs` 使用 **现有 Chromium headless-shell + Node原生WebSocket/CDP**，无需安装依赖。浏览器实际读取HTTP服务、执行HTML/CSS/JS并截图，不是仅检查CSS字符串或宽度。37支超长中英文名与单位通过真实导入提交写入独立SQLite。

| 分辨率 | 每页行数 | 页数 | 全周期去重队伍 | 横/纵溢出 | 默认12秒周期 |
|---|---:|---:|---:|---|---:|
| 1440×1080（4:3） | 7 | 6 | 37/37 | 无/无 | 72秒 |
| 1920×1440（4:3） | 7 | 6 | 37/37 | 无/无 | 72秒 |
| 1920×1080（16:9） | 7 | 6 | 37/37 | 无/无 | 72秒 |
| 2560×1440（16:9） | 8 | 5 | 37/37 | 无/无 | 60秒 |

每页检测行底边在表格空间内、footer在视口内，逐页确认37支队伍无遗漏；四种尺寸同一页面连续resize。另实测不加速的8秒定时翻页（鼠标停在表格内仍翻页）、空态、模拟网络断连保留旧结果与重新分页、真实后台登录、延迟预览后修改可见文本拒绝旧token、当前内容提交及按钮保持禁用、SD前端分差校验、后台实际SD表单保存、登出身份状态。运行报告见 `browser-report.json`，截图为此目录 `display-*.png`、`admin-*.png`。最终managed job正常exit 0，JS运行异常0。

复现：

```powershell
node tests/browser_qa.mjs --python <现有Python路径> --browser <现有Chromium-headless-shell路径>
```

脚本finally终止自己创建的服务和浏览器，并校验精确路径后清理专属临时数据库/profile；未使用或关闭用户已有浏览器，未触碰DSH19387。

## 尚未实测与边界

- Kimi WebBridge `127.0.0.1:10086/status` 无法连接；未启动或重启其daemon，**用户真实浏览器会话未操作**。
- 已实测的是隔离Chromium headless渲染与工作流；F11/真实显示器、OS缩放、触屏、其他浏览器与现场局域网不在本次验证范围。
- 超长名称在大屏最多2行省略，完整名称保存在title、后台和CSV；“显示所有队伍”是完整分页覆盖，不表示120字符队名每字在单屏全部展开。
- 首次CDP常规Chromium未建立端口；改用现有headless-shell。一次覆盖断言将未trim原始测试名与合法trim后的入库名比较导致假失败，按服务规范归一化fixture后通过；未删除覆盖断言。
- Git只保存本说明和可复现测试脚本，不提交生成截图、JSON报告或日志。默认源码分发包不附本机证据并标记未附验证；`tools/package.py --include-verification` 显式读取并附本机JSON报告/截图，缺失时明确未验证；始终排除日志、.learnings、临时或正式数据库、profile和真实凭据。
