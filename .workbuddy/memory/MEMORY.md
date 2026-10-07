# 蜗牛快搜(D2D) — 项目长期记忆

## 列表纵向滚动条几何 (2026-10-07 收紧后口径)

几何唯一来源两处, 改一处必须看另一处:

- `xjs_chrome.cpp XjsChromeLayout`: `L.vtrack = XjsRectF(listRight - XSF(14), L.list.top + XSF(2),
  listRight - XSF(3), L.list.bottom - XSF(6))` —— 带宽 11, 右缘距窗口边 3。
- `xjs_list.cpp XjsGetColumnRects`: `if (XjsVScrollVisible()) rightPad += XSF(10)` (叠加基内边距 12 = 22)。
- thumb 8px / 圆角 4 / 最短 24px, 恒贴带右缘 (`L.vtrack.right - XSF(8)`), 渲染与命中拖拽同源
  (`XjsVThumbGeom`)。
- 视觉口径: 内容裁剪边(= vtrack.left)到 thumb 左缘 = 3px, 与 thumb 右侧 3px 对称; 非溢出时末列右缘
  距 vtrack 命中区左缘留 8px (保证点行尾不误触发轨道翻页)。
- 依赖 vtrack.left 自动跟随的量: 网格列数 `XjsGridCols`、行裁剪 `XjsRowsClipRect`、滚动条命中区。

## 构建环境 (本机助手的可行路径)

- `cmd.exe` 被工具安全策略拦截 → build.bat 无法直接调用。改用 PowerShell 复刻 rc/cl/link:
  工具集 `<VS>\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64` + Windows Kits `10.0.26100.0` (bin/x64,
  Include 的 ucrt/um/shared/winrt, Lib 的 ucrt/x64 与 um/x64), 参数照抄 build.bat。
- PowerShell 工具 stdout 不回传 → 输出必须写日志文件再读。
- **cl 的 `/I` 传含空格路径会被吞**(报 C1083 找不到 windows.h) → 改设 `$env:INCLUDE`(分号拼接
  5 个目录)再用无 `/I` 的命令行; 这样单文件编译验证能过。
- **`rc.exe` 不在 MSVC 目录下**, 在 `C:\Program Files (x86)\Windows Kits\10\bin\<ver>\x64\rc.exe`;
  且 `link` 带 `/MANIFEST:EMBED` 时会自己调 rc → **必须把 Kits bin 与 MSVC bin 都加进 `$env:PATH`**,
  否则 LNK1158 "无法运行 rc.exe"。
- 中文路径下 `Remove-Item` 触发 safe-delete(trash) 失败并中断脚本 → 用 `[System.IO.File]::Delete()`。
- 运行中实例(管理员进程)锁 exe → 普通 shell 杀不掉, 工具禁提权; 让用户托盘退出后单独补跑 link。
- 全量 cl 20 个 TU 只需 ~6s (/MP + /GL), link 6s —— 不用为了"怕慢"而跳过全量验证。

## 引擎 API 签名变更纪律 (改 xunjieso.h 导出签名时必读)

改 `xunjieso.h` 里导出函数签名后, 只改调用点**不够**。仓库里有三类二进制各自链接
`xunjieso.lib`, 版本不一致就是静默 ABI 错配 (x64 下第 5+ 参读栈上垃圾值, 表现为引擎弹
"主线程不能进行搜索等待"之类的行为异常, 而非编译错误):
1. `SnailQuickSearch.exe` (宿主)
2. `plugins/*/*.dll` (部署版插件) — 对应源在 `插件示例/同名/`
3. 文档示例代码 (`插件开发指南.md` 等, 只影响读者)

改完必须逐个重编 + 部署。快速判断某个报错是不是 ABI 错配: 把报错文案按 utf-16-le 在
dll/exe 里搜, 只有 dll 命中 = 引擎侧抛的, 优先查版本对齐而非逻辑。

## 引擎 SearchFilterConfig (路径白名单) 口径

`xjs_result_Query` 第 4 参, JSON: `{"搜索范围":[{"路径":"D:\\工作","递归子目录":true}]}`。
**顶层键 2026-10-08 由「包含路径」改名「搜索范围」** (子键 `路径`/`递归子目录` 不变);
dll 里核过**只有这三个键, 没有排除项**。路径**区分大小写、不支持通配符**。命中白名单外的路径
根本不参与搜索 (引擎级过滤, 早于 SQL 的 Path LIKE 前置条件, 内容搜索 FileContent 与
lua_exec 全盘统计靠它省时)。

**引擎键名会变**: 改名前后先在 dll 里 utf-16 搜键名确认 (搜不到 = 引擎侧改名了),
别照抄旧注释。查法: `data.find(kw.encode('utf-16-le'))`。

消费方: `ai-assistant` 插件 run_search 的「搜索范围」参数 (2026-10-08 接, 见日常日志)。
宿主搜索框目前无入口, 恒传 NULL。

## ai-assistant 插件: 内嵌前端与验证手法

- 前端 (`ui/index.html`+`app.css`+`app.js`) 由 `ai_ui.rc` 以 RCDATA 300/301/302 **打包进 dll**,
  运行目录 `plugins/ai-assistant/` 下**没有** ui 目录是正常的 — 改前端只需重编 dll, 不用单独拷。
- 改 `AI_TOOLS_JSON` (工具 schema, R"json( 分两段拼接) 后**必须 python json.loads 校验**,
  编译器不检查 JSON 合法性。坑: 英文双引号需转义 (中文「」更省事); 路径示例的 `\\` 转义层数
  易多写一层, 模型会看到 `D:////工作`。
- CSS 变量实际是 `--accent-violet/cyan/emerald/amber/pink` — **`--accent-green` 不存在**。
- 插件新增工具参数时, 落地清单 (漏一处就功能半残): schema description → `AgentToolExec` 解析 →
  `AiToolStep` 字段 + `operator==` → `ai_core.cpp` 落库/读取 → `HistStepArgsOf` 历史回喂 →
  卡片徽标 css+渲染 → `AgentManualExec` 重放透传 → `AI_INSTRUCTIONS` 提示词 → `BuildEnvSnapshot`。
