/*
 * ai_agent.cpp — agent 大脑: 引擎直连工具执行 / 系统提示词组装 / 工具定义 /
 * 请求体构建 / SSE 轮 (function calling) / 工作线程循环
 * 私有结果对象 = 进程级单例 + 互斥串行, 不碰窗口结果对象; 五种模式统一 xjs_result_Query
 * 提交 (-4 执行模式也走 Query), waitComplete=FALSE 异步发起 + 轮询等完成 (80ms 一查) —— 不能用 waitComplete=TRUE
 * 一路阻塞: 阻在引擎里时"停止"按钮的 abort 无人看, 工具卡片永远"执行中"。
 * 轮询期 abort → CancelSearch 取消在途搜索; 120 秒兜底超时; 串行锁也轮询占用
 * (多窗排队等锁同样可被停止打断)。
 */
#include "ai_assistant.h"

/* ==================== 引擎直连 (agent 工具执行层) ====================
 * 插件进程 = 宿主进程, 加载器按模块名绑到宿主已加载的 xunjieso.dll 同一实例;
 * xjs_GetDefaultEngine() 取句柄。agent 的搜索全部打到插件私有结果对象 g_agentRes
 * (不碰窗口结果对象 = 不打扰用户正在看的搜索)。工具执行一律在 agent 工作线程 +
 * g_agentCs 串行 (多窗口同时让 AI 跑工具时排队, 排队等锁可被停止打断), 搜索用
 * waitComplete=FALSE 异步发起 + 轮询 IsCompleted, abort 即 CancelSearch。
 * 结果对象必须引擎空闲态创建 (忙时创建会被永久定成文件名序), 懒建 + 失效重建。 */

static CRITICAL_SECTION g_agentCs;
static xjs_result* g_agentRes = NULL;     /* agent 私有结果对象 (进程级一份, 串行使用) */
static std::vector<int> g_agentTopIds;    /* 最近一次 run_search 样本 fileId (open_file 按 1-based 序号取用) */

/* 搜索失败信息槽 (XJS_RESULT_EVENT_FAILED 在引擎搜索线程触发, 只带回当前指纹的;
 * 独立小锁 —— 回调若等 g_agentCs 会与轮询线程互等死锁)。SQL 解析/正则编译失败只
 * 触发 FAILED 不触发 COMPLETE, 靠它把真实错误喂回模型, 否则伪装成"命中 0 条"。 */
static CRITICAL_SECTION g_srchErrCs;
static std::wstring g_srchErr;            /* 错误 JSON 原文 */
static int g_srchErrFp = -1;              /* 所属搜索指纹 */

static int AgentOnSearchFailed(void* userData, xjs_engine* eng, xjs_result* res,
                               int searchFingerprint, const char* errorJson) {
    (void)userData; (void)eng; (void)res;
    EnterCriticalSection(&g_srchErrCs);
    g_srchErr = W8(errorJson ? errorJson : "");
    g_srchErrFp = searchFingerprint;
    LeaveCriticalSection(&g_srchErrCs);
    return 0;
}

/* ---- Lua 过程输出通道 (结果对象级注册 ai.print) ----
 * 引擎规范让统计型脚本"数据走 print", 但 print 只进引擎调试输出、SDK 无读取口 —— 统计
 * 数字物理上回不到模型。故在 g_agentRes 上注册 ai.print(...)(xjs_result_LuaRegisterFunction,
 * 仅本结果对象的 -3/-4 虚拟机生效, 随对象销毁自动消失), 脚本的过程/统计文本经它进缓冲,
 * 完成后并入工具结果 JSON 的 output 字段回喂模型。回调在引擎搜索/执行线程触发 —— 独立小锁,
 * 禁止碰 g_agentCs (轮询线程持锁等完成会互等死锁, 同 AgentOnSearchFailed 口径)。
 * 缓冲发起前清空、完成后整体取走 = 恰好本次运行 (工具串行锁保证期间无第二个 agent 搜索);
 * 16KB 封顶防失控脚本。 */
static CRITICAL_SECTION g_emitCs;
static std::string g_emitBuf;
static bool g_emitCut = false;
static std::string g_srchSet8;   /* g_agentRes 搜索设置 JSON 快照 (创建时取一次, 注入系统提示词; 经 g_emitCs 护) */

static void EmitAppendLine(const std::string& line) {
    EnterCriticalSection(&g_emitCs);
    if (g_emitBuf.size() + line.size() <= 16384) {
        g_emitBuf += line;
    } else if (!g_emitCut) {
        g_emitCut = true;
        g_emitBuf += "\n... (ai.print 输出过长, 已截断)";
    }
    LeaveCriticalSection(&g_emitCs);
}

/* ai.print 的 C 实现: 多参数按 print 惯例以 \t 连接; 字符串/数字直取 (ToString 指针只在
 * 本次调用内有效, 立即拷贝), 表等其余值经 xjs_lua_ToJson 序列化, 都不行退化为 true/nil。 */
static int AgentLuaPrint(void* L) {
    std::string line;
    int n = xjs_lua_GetTop(L);
    for (int i = 1; i <= n; i++) {
        if (i > 1) line += '\t';
        const char* s = xjs_lua_ToString(L, i);
        if (s) { line += s; continue; }
        int need = xjs_lua_ToJson(L, i, NULL, 0);
        if (need > 1 && need <= 8192) {
            std::string buf((size_t)need, 0);
            if (xjs_lua_ToJson(L, i, &buf[0], need) > 0) {
                line += buf.c_str();   /* c_str 吃掉结尾 '\0' */
                continue;
            }
        }
        line += xjs_lua_ToBoolean(L, i) ? "true" : "nil";
    }
    line += '\n';
    EmitAppendLine(line);
    return 0;
}

void AgentToolInit() {
    InitializeCriticalSectionAndSpinCount(&g_agentCs, 100);
    InitializeCriticalSectionAndSpinCount(&g_srchErrCs, 100);
    InitializeCriticalSectionAndSpinCount(&g_emitCs, 100);
}
void AgentToolShutdown() {
    if (g_agentRes) { xjs_result_Destroy(g_agentRes); g_agentRes = NULL; }
    g_agentTopIds.clear();
    g_emitBuf.clear();   /* 到此工作线程已全部 join, 无并发 */
    g_emitCut = false;
    DeleteCriticalSection(&g_srchErrCs);
    DeleteCriticalSection(&g_emitCs);
    DeleteCriticalSection(&g_agentCs);
}

/* 占用 agent 串行锁: 多窗同时跑工具时在此排队, 轮询等待期间响应停止。
 * 返回 false = 等锁期间被停止 (未持锁)。 */
static bool AgentCsEnter(AiJob* j) {
    while (!TryEnterCriticalSection(&g_agentCs)) {
        if (InterlockedCompareExchange(&j->abort, 0, 0)) return false;
        Sleep(40);
    }
    return true;
}

/* run_search 实体; 返回空串 = 成功, 否则 = 错误描述 (调用方持 g_agentCs) */
static std::wstring AgentToolRunSearch(AiJob* j, const std::wstring& mode, const std::wstring& query, AiToolStep* st) {
    xjs_engine* eng = xjs_GetDefaultEngine();
    if (!eng) return L"搜索引擎未就绪";
    if (g_agentRes && !xjs_result_IsEffective(g_agentRes)) g_agentRes = NULL;
    if (!g_agentRes) {
        if (xjs_db_GetEngineState(eng) != XJS_DB_STATE_IDLE) return L"索引正忙 (加载/扫描中), 请稍后重试";
        g_agentRes = xjs_result_Create(eng);
        if (!g_agentRes) return L"结果对象创建失败";
        xjs_result_SetCallback(g_agentRes, XJS_RESULT_EVENT_FAILED, (const void*)AgentOnSearchFailed, NULL);
        xjs_result_LuaRegisterFunction(g_agentRes, "ai", "print", AgentLuaPrint);
        const char* ss = xjs_result_GetSearchSettings(g_agentRes);
        EnterCriticalSection(&g_emitCs);
        g_srchSet8 = ss ? ss : "";
        LeaveCriticalSection(&g_emitCs);
    }
    std::string q8 = U8(query);
    EnterCriticalSection(&g_srchErrCs);
    g_srchErr.clear();
    g_srchErrFp = -1;
    LeaveCriticalSection(&g_srchErrCs);
    bool luaMode = (mode == L"lua_filter" || mode == L"lua_exec");
    if (luaMode) {   /* 清缓冲必须在发起前: Query 返回后 VM 可能立刻开跑并 ai.print */
        EnterCriticalSection(&g_emitCs);
        g_emitBuf.clear();
        g_emitCut = false;
        LeaveCriticalSection(&g_emitCs);
    }
    int fp = -1;
    if (mode == L"wildcard")      fp = xjs_result_Query(g_agentRes, q8.c_str(), 0, FALSE);
    else if (mode == L"regex")    fp = xjs_result_Query(g_agentRes, q8.c_str(), 1, FALSE);
    else if (mode == L"sql")      fp = xjs_result_Query(g_agentRes, q8.c_str(), 2, FALSE);
    else if (mode == L"lua_filter") fp = xjs_result_Query(g_agentRes, q8.c_str(), XJS_KEYWORD_LUA, FALSE);
    else if (mode == L"lua_exec") fp = xjs_result_Query(g_agentRes, q8.c_str(), XJS_KEYWORD_LUA_EXEC, FALSE);
    else return L"未知搜索模式: " + mode;
    if (fp < 0) {
        std::wstring e = W8(xjs_GetLastErrorMsg(eng));
        return L"搜索发起失败: " + (e.empty() ? std::wstring(L"引擎拒绝") : e);
    }
    /* 轮询等完成 (阻塞式 waitComplete=TRUE 会把"停止"冻死在引擎调用里):
     * abort → CancelSearch 在途搜索即停; 120 秒兜底防引擎侧万一永不完成;
     * IsCompleted 对"指纹被覆盖/从未开始"也返回 TRUE, 串行使用下不会误判。 */
    ULONGLONG t0 = GetTickCount64();
    for (;;) {
        if (InterlockedCompareExchange(&j->abort, 0, 0)) {
            if (xjs_result_IsEffective(g_agentRes)) xjs_result_CancelSearch(g_agentRes, fp, FALSE);
            return L"已停止";
        }
        if (xjs_result_IsCompleted(g_agentRes, fp)) break;
        if (!xjs_result_IsEffective(g_agentRes)) return L"结果对象已失效";
        if (GetTickCount64() - t0 > 120000) {
            if (xjs_result_IsEffective(g_agentRes)) xjs_result_CancelSearch(g_agentRes, fp, FALSE);
            return L"搜索超时 (120 秒未完成, 已取消) — 请缩小范围 (加目录前缀/限定字段) 重试";
        }
        Sleep(80);
    }
    EnterCriticalSection(&g_srchErrCs);
    bool failed = g_srchErrFp == fp && !g_srchErr.empty();
    std::wstring serr = g_srchErr;
    g_srchErr.clear();
    g_srchErrFp = -1;
    LeaveCriticalSection(&g_srchErrCs);
    if (failed) {   /* SQL/正则等编译失败: 抽"错误信息"喂回模型便于自行纠正 */
        Jv v = JsonParseW(serr);
        std::wstring msg = v.S(L"错误信息");
        if (msg.empty()) msg = v.S(L"错误类型");
        return msg.empty() ? (L"搜索失败: " + serr) : (L"搜索失败: " + msg);
    }
    st->count = xjs_result_GetCount(g_agentRes);
    st->elapsedMs = xjs_result_GetElapsed(g_agentRes);
    if (luaMode) {   /* 取走本次 ai.print 过程/统计输出 (整段并入工具结果) */
        EnterCriticalSection(&g_emitCs);
        st->emit = W8(g_emitBuf.c_str());
        g_emitBuf.clear();
        g_emitCut = false;
        LeaveCriticalSection(&g_emitCs);
    }
    /* 样本 TOP 20: fileId 留给 open_file, 路径给模型 (GetPath 指针为线程本地缓存, 必须立即拷贝) */
    g_agentTopIds.clear();
    st->top.clear();
    int n = st->count < 20 ? st->count : 20;
    for (int i = 0; i < n; i++) {
        int fid = xjs_result_GetFileId(g_agentRes, i);
        if (fid < 0) continue;
        g_agentTopIds.push_back(fid);
        const char* p = xjs_db_GetPath(eng, fid);
        st->top.push_back(p ? W8(p) : L"(路径不可用)");
    }
    return L"";
}

/* open_file 实体: 打开/定位最近一次 run_search 样本中的文件 (走宿主打开行为) */
static std::wstring AgentToolOpenFile(int index, bool reveal, XjsWindowToken tok) {
    if (index < 1 || index > (int)g_agentTopIds.size())
        return L"序号越界 (最近一次搜索样本共 " + std::to_wstring((unsigned long long)g_agentTopIds.size()) + L" 条)";
    int fid = g_agentTopIds[index - 1];
    if (!g_host) return L"宿主不可用";
    int rc = g_host->OpenFile(g_ctx, tok, fid, reveal ? 1 : 0);
    if (rc != XJS_PLUGIN_OK) return L"打开失败 (窗口已关闭或未声明 ui 权限)";
    return L"";
}

/* copy_paths 实体: 最近一次结果前 100 条路径 (每行一条) 复制到剪贴板 */
static std::wstring AgentToolCopyPaths() {
    if (!g_agentRes || !xjs_result_IsEffective(g_agentRes)) return L"还没有已完成的搜索";
    xjs_engine* eng = xjs_result_GetXjsEngine(g_agentRes);
    int count = xjs_result_GetCount(g_agentRes);
    if (count <= 0) return L"当前结果为空";
    std::wstring text;
    int n = count < 100 ? count : 100;
    for (int i = 0; i < n; i++) {
        int fid = xjs_result_GetFileId(g_agentRes, i);
        if (fid < 0) continue;
        const char* p = xjs_db_GetPath(eng, fid);
        if (p) { text += W8(p); text += L"\r\n"; }
    }
    if (text.empty()) return L"没有可复制的路径";
    if (!g_host || g_host->ClipboardSetText(g_ctx, U8(text).c_str()) != XJS_PLUGIN_OK)
        return L"剪贴板写入失败";
    return L"";
}

/* 工具统一入口 (agent 工作线程调用): 解析参数 → 执行 → 结果填进 step; 返回错误描述 (空=成功)。
 * 串行锁轮询占用, 被停止打断返回"已停止"。 */
static std::wstring AgentToolExec(AiJob* j, const std::string& name8, const std::string& args8,
                                  XjsWindowToken tok, AiToolStep* st) {
    Jv v = JsonParseW(W8(args8.c_str()));
    if (name8 == "run_search") {
        st->kind = 0;
        st->mode = v.S(L"mode");
        st->query = v.S(L"query");
        if (st->query.empty()) return L"query 不能为空";
        if (!AgentCsEnter(j)) return L"已停止";
        std::wstring err = AgentToolRunSearch(j, st->mode, st->query, st);
        LeaveCriticalSection(&g_agentCs);
        return err;
    }
    if (name8 == "open_file") {
        st->kind = 1;
        const Jv* rv = v.Get(L"reveal");
        bool reveal = rv && ((rv->t == 1 && rv->b) || (rv->t == 2 && rv->num != 0));
        int index = (int)wcstol(TrimW(v.S(L"index")).c_str(), NULL, 10);
        if (!AgentCsEnter(j)) return L"已停止";
        std::wstring err = AgentToolOpenFile(index, reveal, tok);
        LeaveCriticalSection(&g_agentCs);
        return err;
    }
    if (name8 == "copy_paths") {
        st->kind = 2;
        if (!AgentCsEnter(j)) return L"已停止";
        std::wstring err = AgentToolCopyPaths();
        LeaveCriticalSection(&g_agentCs);
        return err;
    }
    return name8.empty() ? L"未知工具 (模型未给出工具名, 请从 tools 列表中选择)"
                         : (L"未知工具: " + W8(name8.c_str()));
}

/* 工具结果 → function_call_output 文本 (喂回模型的 JSON) */
static std::string AgentToolOutput(const std::wstring& err, const AiToolStep& st) {
    std::wstring j;
    if (!err.empty()) {
        j = L"{\"error\":";
        j += W8(JsonEscapeUtf8(err).c_str());
        j += L"}";
        return U8(j);
    }
    if (st.kind == 0) {
        wchar_t head[128];
        swprintf(head, 128, L"{\"count\":%d,\"elapsedMs\":%lld,\"top\":[", st.count, st.elapsedMs);
        j = head;
        for (size_t i = 0; i < st.top.size(); i++) {
            if (i) j += L",";
            j += W8(JsonEscapeUtf8(st.top[i]).c_str());
        }
        j += L"]";
        if (!st.emit.empty()) {   /* lua 脚本 ai.print 的过程/统计输出 */
            j += L",\"output\":";
            j += W8(JsonEscapeUtf8(st.emit).c_str());
        }
        j += L"}";
    } else {
        j = L"{\"ok\":true}";
    }
    return U8(j);
}

/* 系统提示词: 产品能力内建 (搜索模式/语法/API 摘要取自 搜索模式简介.md 提炼) —
   助手要能直接给出可粘贴的搜索式, 而不是泛泛的通用回答 */
/* 角色/产品说明 (agent 循环里的"我有什么工具、怎么用"部分; Lua 脚本规范由引擎内嵌提示词提供) */
static const wchar_t* AI_INSTRUCTIONS =
    L"你是“蜗牛快搜”内置的 AI 助手(agent 模式)。蜗牛快搜是 Windows 本地文件极速搜索工具：全盘秒级索引，"
    L"支持文件名/大小/时间/类型/别名/内容等搜索。全程用简体中文回答（包括所有面向用户的文字），简洁、准确、直接。\n"
    L"\n"
    L"## 工作方式\n"
    L"你有工具可用：涉及找文件/查文件/统计文件的任务，**先调用 run_search 工具实际执行搜索**，"
    L"根据返回的条数/路径样本/ai.print 输出判断结果是否符合，不符合就换口径再搜（逐步逼近：先粗筛再精筛），"
    L"不要凭空编造文件路径；需要给用户看文件时用 open_file，需要给出路径清单时用 copy_paths。\n"
    L"- 工具调用轮数有限，别在一种写法上反复试错：同一口径连续两次拿不到有效数据，立即换搜索模式。\n"
    L"- 统计类任务必须**自己跑到出数**再回答（用户要的是数字与结论，不是脚本）；只有确实多次失败，\n"
    L"  才把可粘贴的搜索式/脚本交给用户并说明卡在哪一步。\n"
    L"得到足够信息后，用最终答复总结：找到什么、在哪、关键数据；推荐执行的搜索用 xjs:// 搜索链接给出（见下节）。\n"
    L"与搜索无关的问题直接回答，不要调用工具。\n"
    L"\n"
    L"## 可点击输出（你的回答会被渲染成交互界面）\n"
    L"你的回答里有四种元素会被渲染成用户可直接点击的控件。前两种一律用**标准 Markdown 链接语法**，\n"
    L"链接文字写成给用户看的动作指引（不要裸放搜索词/路径当文字）：\n"
    L"1. 搜索指令：[链接文字](xjs://search?text=<URL编码的搜索词>&mode=<wildcard|regex|sql|lua>)。\n"
    L"   用户点击即把搜索词置入搜索框并按该模式执行。mode 可省略 = 沿用窗口当前模式；\n"
    L"   text 的值要 URL 编码（空格=%20，&=%26，#=%23；中文/通配符可直接写）。\n"
    L"   示例：[🔍 搜索所有 PNG 图片](xjs://search?text=*.png&mode=wildcard)、"
    L"[🔍 找出大于 100MB 的视频](xjs://search?text=SELECT%20Path%20FROM%20alltable%20WHERE%20Size%20%3E%20'100M'&mode=sql)。\n"
    L"   凡要给\"可执行的搜索式\"一律用这种链接；很长的 lua 脚本塞不进链接，改用普通代码块给出。\n"
    L"2. 文件动作：[打开 xxx](xjs://open?path=<完整路径>)、[在资源管理器中定位 xxx](xjs://reveal?path=<完整路径>)。\n"
    L"   path 的值同样要 URL 编码（空格=%20、&=%26、括号最好也编码=%28 %29）。表格清单里链接文字用文件名即可，\n"
    L"   完整路径放进 path（悬停可见），别把几百字符的整条路径铺在表格里。\n"
    L"3. 路径是精确数据：必须**逐字复制 run_search 返回的原文**（盘符/空格/括号/间隔点/扩展名一个字符都不能变），\n"
    L"   绝不凭印象改写、意译或补全——差一个字符，用户点击就打不开。链接文字照抄原文件名，不要自造名称。\n"
    L"4. 直接写出完整绝对路径（含盘符）也会自动渲染为可点击链接：单击=打开，右键=打开/定位/复制路径。\n"
    L"5. 网页链接照常 [标题](https://...)，点击用系统浏览器打开。\n"
    L"\n"
    L"## 搜索模式选择（run_search 的 mode 参数）\n"
    L"- wildcard 通配符（日常默认）：* 任意长度、? 单个字符；不含 * ? 时自动按包含匹配；支持拼音首拼/全拼（wd 命中 文档.docx）；"
    L"空格=且，|=或；搜索词含 \\\\ 或 / 时按完整路径匹配。\n"
    L"- regex 正则（PCRE2）：如 ^[0-9]{4}-报告.*\\.docx$。\n"
    L"- sql（功能最强）：SELECT Path FROM alltable WHERE Size > '100M' AND ModTime > NOW() - INTERVAL '7 days' ORDER BY Size DESC LIMIT 100；"
    L"支持 LIKE/ILIKE/~/GROUP BY/COUNT/CASE WHEN/CTE 等；尺寸简写 '100M'；常用字段：ID、Path、FName、Ext、Size、CreateTime、ModTime、"
    L"AccessTime、FileType、IsDir、Alias、Score、FileContent（不区分大小写）；不支持窗口函数(OVER)/多表 FROM/EXISTS/DDL。\n"
    L"- lua_filter 过滤模式：对每个文件做一次真值判断的 Lua 脚本（多线程快、增量同步）；脚本规范见下方《Lua 过滤模式脚本规范》。\n"
    L"- lua_exec 执行模式：脚本全权遍历数据库/跨文件聚合/自定义排序，return 的 ID 数组即结果；脚本规范见下方《Lua 执行模式脚本规范》。\n"
    L"选择建议：找名字用 wildcard/regex；按字段组合筛选文件用 sql；逐文件自定义判断用 lua_filter；"
    L"**计数/分组/排名/占比等一切统计类问题直接用 lua_exec**（统计数字经 ai.print 拿回来）。\n"
    L"- SQL 做不了统计：聚合数值经工具通道拿不到（COUNT(*) 只回 1 行，数值本身不回传），GROUP BY 形态受限、不支持子查询/CASE。\n"
    L"- SQL 的 LIKE 里 \\ 是转义字符，`Path LIKE 'C:\\%'` 实测匹配 0 条；含 \\ 的路径前缀筛选改用 lua_exec（f.fpath() 判断前缀）。\n"
    L"\n"
    L"## 过程输出通道（Lua 脚本统计必读）\n"
    L"- 两份规范里\"数据走 print\"的说法对本助手不适用：print 的输出进引擎调试输出，工具结果里拿不到。\n"
    L"- 本助手为脚本注册了 ai.print(...)（与 print 同款多参数拼接，可多次调用；参数可为字符串/数字/表），\n"
    L"  其输出会作为本次工具结果 JSON 的 output 字段原样回传。统计数字、逐目录/逐扩展名计数、过程日志\n"
    L"  等一切要给助手看的数据一律经 ai.print 输出；return 仍按规范（执行模式=结果 ID 数组，过滤模式=逐文件真值）。\n"
    L"\n"
    L"以下两份规范来自搜索引擎内置提示词，写 lua_filter / lua_exec 脚本时必须严格遵守其中的 API 全集与硬性规则"
    L"（脚本沙箱删除了 io/os 等库，API 以规范所列为准，绝不虚构函数）：\n"
    L"\n"
    L"════════════ 《Lua 过滤模式脚本规范》(mode=lua_filter) ════════════\n";

static const wchar_t* AI_INSTRUCTIONS_TAIL =
    L"\n════════════ 《Lua 执行模式脚本规范》(mode=lua_exec) ════════════\n";

/* 工具定义 (Responses API tools 数组; 与 AgentToolExec 的名字/参数一一对应) */
static const char* AI_TOOLS_JSON = R"json([
  {"type":"function","name":"run_search","description":"在蜗牛快搜索引中执行一次搜索, 返回命中总数与前 20 条路径样本。可多次调用逐步逼近目标 (先粗筛再精筛)。5 种 mode 的搜索词语法以系统提示词中的说明与两份 Lua 规范为准。Lua 模式脚本内用 ai.print(...) 输出的统计/过程信息附在结果 JSON 的 output 字段。","parameters":{"type":"object","properties":{"mode":{"type":"string","enum":["wildcard","regex","sql","lua_filter","lua_exec"],"description":"wildcard=通配符 regex=PCRE2正则 sql=SELECT语句 lua_filter=过滤模式Lua脚本 lua_exec=执行模式Lua脚本"},"query":{"type":"string","description":"搜索词/脚本全文 (lua 两种模式传完整脚本文本)"}},"required":["mode","query"]}},
  {"type":"function","name":"open_file","description":"打开最近一次 run_search 样本列表中的某个文件 (在用户屏幕上打开/定位), 用于让用户直接看到该文件。","parameters":{"type":"object","properties":{"index":{"type":"integer","description":"样本列表序号 (1 起)"},"reveal":{"type":"boolean","description":"true=只在资源管理器中定位, 不打开"}},"required":["index"]}},
  {"type":"function","name":"copy_paths","description":"把最近一次 run_search 的前 100 条完整路径 (每行一条) 复制到剪贴板, 供用户粘贴。","parameters":{"type":"object","properties":{},"required":[]}}
])json";

/* 系统提示词组装: 角色说明 + 引擎内嵌两份 Lua 规范 (进程级构建一次) */
static std::string g_instrA;
void BuildInstructions() {
    std::wstring s = AI_INSTRUCTIONS;
    const char* p0 = xjs_LUA_GetPprompt(0);   /* 过滤模式 (-3) */
    if (p0 && *p0) s += W8(p0);
    s += AI_INSTRUCTIONS_TAIL;
    const char* p1 = xjs_LUA_GetPprompt(1);   /* 执行模式 (-4) */
    if (p1 && *p1) s += W8(p1);
    g_instrA = U8(s);
}

/* 运行环境快照 (每次请求实时采集, 拼在 instructions 尾部): 当前时间 / 索引规模与状态 /
 * 可选字段开关 / 搜索设置。引擎 7 个可选字段 (大小/时间×3/评分/别名/属性) 未必全开,
 * 不注入这份清单, 模型就会对未开启字段照常写 SQL/Lua —— 用户问"文件何时创建"而
 * 创建时间字段未开启 = 查询报错或空结果, 模型只能瞎猜。全部为引擎只读查询, 工作线程可调。 */
static std::wstring BuildEnvSnapshot() {
    std::wstring s = L"\n## 运行环境快照 (每次请求实时采集, 时间与字段口径以此为准)\n";
    SYSTEMTIME st;
    GetLocalTime(&st);
    static const wchar_t* WK[7] = { L"日", L"一", L"二", L"三", L"四", L"五", L"六" };
    wchar_t tb[80];
    swprintf(tb, 80, L"- 当前时间: %04u-%02u-%02u %02u:%02u:%02u 星期%s (本地时间)\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, WK[st.wDayOfWeek % 7]);
    s += tb;
    xjs_engine* eng = xjs_GetDefaultEngine();
    if (!eng) return s;
    const wchar_t* stTxt;
    switch (xjs_db_GetEngineState(eng)) {
        case XJS_DB_STATE_LOADING:   stTxt = L"数据库加载中 (暂无可用索引)"; break;
        case XJS_DB_STATE_SAVING:    stTxt = L"正在保存数据库"; break;
        case XJS_DB_STATE_SCANNING:  stTxt = L"正在扫描建索引 (结果不完整)"; break;
        case XJS_DB_STATE_SYNCING:   stTxt = L"正在同步文件变化"; break;
        case XJS_DB_STATE_SEARCHING: stTxt = L"正在搜索"; break;
        default:                     stTxt = L"空闲"; break;
    }
    wchar_t ib[128];
    swprintf(ib, 128, L"- 索引库: %d 个条目 (含文件夹与盘符), 引擎状态=%s\n",
             xjs_db_GetFileCount(eng), stTxt);
    s += ib;
    /* 7 个可选字段: 中文名 = xjs_db_IsFieldEnabled 的键, 括号内 = SQL/Lua 侧列名 */
    static const struct { const wchar_t* cn; const char* key; const wchar_t* col; } OPT[] = {
        { L"文件大小", "文件大小", L"Size" },
        { L"修改时间", "修改时间", L"ModTime" },
        { L"创建时间", "创建时间", L"CreateTime" },
        { L"访问时间", "访问时间", L"AccessTime" },
        { L"文件评分", "文件评分", L"Score" },
        { L"别名",     "别名",     L"Alias" },
        { L"文件属性", "文件属性", L"FAttr" },
    };
    std::wstring on, off;
    for (const auto& f : OPT) {
        std::wstring item = std::wstring(f.cn) + L"(" + f.col + L")";
        if (xjs_db_IsFieldEnabled(eng, f.key)) { if (!on.empty()) on += L"、"; on += item; }
        else                                   { if (!off.empty()) off += L"、"; off += item; }
    }
    s += L"- 已开启字段: " + (on.empty() ? std::wstring(L"(以上必建字段之外一个都没开)") : on) + L"\n";
    s += L"- 未开启字段: " + (off.empty() ? std::wstring(L"(无)") : off) + L"\n";
    s += L"- 未开启字段在 SQL 条件/排序、Lua 取值中均不可用 (查询报错或无此数据), 绝不要编造此类数值;\n";
    s += L"  用户问题依赖未开启字段时, 如实说明\"索引未开启该字段, 需在设置中开启并重建索引后再查\"。\n";
    EnterCriticalSection(&g_emitCs);
    std::wstring setW = g_srchSet8.empty() ? std::wstring() : W8(g_srchSet8.c_str());
    LeaveCriticalSection(&g_emitCs);
    if (!setW.empty())
        s += L"- 搜索设置: " + setW + L" (首拼/全拼/大小写口径以此为准)\n";
    return s;
}

/* 一轮 SSE 事件里攒下的 function_call (模型要求执行的工具) */
struct AiCall {
    std::string callId, name, args;
    int outIdx = -1;   /* SSE output_index: 一轮输出里 reasoning 等其它项也占号,
                          只能按值匹配槽位, 禁止当数组下标扩容 (会造出空名幻影调用) */
};

/* 按 output_index 找/建调用槽位; oi 缺失 = 归入最后一槽 (单调用流兼容口径)。
 * 返回指针仅在本次事件内立即使用, 跨事件可能因扩容失效。 */
static AiCall* AgentCallSlot(std::vector<AiCall>* v, const Jv* oi) {
    if (oi && oi->t == 2) {
        int idx = (int)oi->num;
        for (auto& c : *v) if (c.outIdx == idx) return &c;
        AiCall c;
        c.outIdx = idx;
        v->push_back(c);
        return &v->back();
    }
    if (v->empty()) { AiCall c; v->push_back(c); return &v->back(); }
    return &v->back();
}

/* 每轮请求体: input = 对话快照 + 已发生的工具往返 (call→output 交错) +
   本轮模型产出; withTools=false = 收尾轮 (省略 tools 并明示直接回答) */
static std::string AgentBuildBody(AiJob* j, const std::vector<AiCall>& accCalls,
                                  const std::vector<std::string>& accOuts, bool withTools) {
    std::wstring body = L"{\"model\":";
    body += W8(JsonEscapeUtf8(g_cfg.model).c_str());
    body += L",\"input\":[";
    bool first = true;
    auto sep = [&]() { if (!first) body += L","; first = false; };
    for (auto& m : j->hist) {
        sep();
        body += m.role ? L"{\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":"
                       : L"{\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":";
        body += W8(JsonEscapeUtf8(m.text).c_str());
        body += L"}]}";
    }
    for (size_t i = 0; i < accCalls.size() && i < accOuts.size(); i++) {
        sep();
        body += L"{\"type\":\"function_call\",\"call_id\":";
        body += W8(JsonEscapeUtf8(W8(accCalls[i].callId.c_str())).c_str());
        body += L",\"name\":";
        body += W8(JsonEscapeUtf8(W8(accCalls[i].name.c_str())).c_str());
        body += L",\"arguments\":";
        body += W8(JsonEscapeUtf8(W8(accCalls[i].args.c_str())).c_str());
        body += L"}";
        sep();
        body += L"{\"type\":\"function_call_output\",\"call_id\":";
        body += W8(JsonEscapeUtf8(W8(accCalls[i].callId.c_str())).c_str());
        body += L",\"output\":";
        body += W8(JsonEscapeUtf8(W8(accOuts[i].c_str())).c_str());
        body += L"}";
    }
    if (!withTools) {
        sep();
        body += L"{\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":";
        body += W8(JsonEscapeUtf8(L"(工具调用次数已达上限，请直接根据已获得的信息回答)").c_str());
        body += L"}]}";
    }
    body += L"],\"stream\":true,\"instructions\":";
    std::wstring instr = W8(g_instrA.c_str()) + BuildEnvSnapshot();   /* 静态说明(UTF-8→宽) + 每轮实时环境快照 */
    body += W8(JsonEscapeUtf8(instr).c_str());
    if (withTools) {
        body += L",\"tools\":";
        body += W8(AI_TOOLS_JSON);
    }
    body += L",\"reasoning\":{\"effort\":\"";
    body += g_cfg.reasoning ? L"high" : L"none";
    body += L"\"}}";
    return U8(body);
}

/* 一轮对话: 发请求 → SSE 读流 → 文本增量进 j->out/reason, function_call 攒进 turnCalls。
   返回 false = 网络/HTTP 失败 (errMsg 已设); *failed = 流内协议失败 */
static bool AgentRunTurn(AiJob* j, HINTERNET hc, const std::vector<AiCall>& accCalls,
                         const std::vector<std::string>& accOuts, bool withTools,
                         std::vector<AiCall>* turnCalls, bool* aborted, bool* truncated,
                         bool* failed, std::string* errMsg) {
    turnCalls->clear();
    std::string body = AgentBuildBody(j, accCalls, accOuts, withTools);
    wchar_t wpath[1024] = {};
    MultiByteToWideChar(CP_UTF8, 0, j->pathA.c_str(), -1, wpath, 1024);
    HINTERNET hr = WinHttpOpenRequest(hc, L"POST", wpath, NULL, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      j->secure ? WINHTTP_FLAG_SECURE : 0);
    if (!hr) { *errMsg = "open request failed"; return false; }
    bool ok = true;
    EnterCriticalSection(&j->cs);
    j->hReq = hr;   /* UI"停止"并发关句柄打断阻塞读 */
    LeaveCriticalSection(&j->cs);
    std::wstring hdr = L"Content-Type: application/json\r\nAuthorization: Bearer ";
    hdr += W8(j->keyA.c_str());
    do {
        if (!WinHttpAddRequestHeaders(hr, hdr.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD) ||
            !WinHttpSendRequest(hr, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) ||
            !WinHttpReceiveResponse(hr, NULL)) {
            *errMsg = "send/receive failed";
            ok = false;
            break;
        }
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &sz, NULL);
        if (status != 200) {
            char eb[8192] = {};
            DWORD erd = 0, eofc = 0;
            while (erd < sizeof(eb) - 1 && WinHttpReadData(hr, eb + erd, sizeof(eb) - 1 - erd, &eofc) && eofc)
                erd += eofc;
            eb[erd] = 0;
            Jv v = JsonParseW(W8(eb));
            std::wstring msg;
            const Jv* em = v.Get(L"message");
            if (em && em->t == 3) msg = em->str;
            if (msg.empty()) {
                const Jv* eo = v.Get(L"error");
                if (eo && eo->t == 5) msg = eo->S(L"message");
            }
            std::string detail;
            if (!msg.empty()) detail = U8(msg);
            else {
                std::string ebStr = eb;
                detail = ebStr.size() > 200 ? ebStr.substr(0, 200) : ebStr;
            }
            char mb[512];
            _snprintf_s(mb, sizeof(mb), _TRUNCATE, "HTTP %lu: %s", (unsigned long)status, detail.c_str());
            *errMsg = mb;
            ok = false;
            break;
        }
        /* SSE 流式: 文本增量照旧; function_call 按 output_index 分组攒参 */
        std::string buf;
        ULONGLONG lastPost = 0;
        for (;;) {
            if (InterlockedCompareExchange(&j->abort, 0, 0)) { *aborted = true; break; }
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(hr, &avail)) break;
            if (!avail) break;
            std::string chunk((size_t)avail, 0);
            DWORD rd = 0;
            if (!WinHttpReadData(hr, &chunk[0], avail, &rd)) break;
            if (InterlockedCompareExchange(&j->abort, 0, 0)) { *aborted = true; break; }
            chunk.resize(rd);
            buf += chunk;
            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.rfind("data:", 0) != 0) continue;
                std::string payload = line.substr(5);
                while (!payload.empty() && (payload[0] == ' ')) payload.erase(0, 1);
                if (payload == "[DONE]") continue;
                Jv ev = JsonParseW(W8(payload.c_str()));
                if (ev.t != 5) continue;
                std::wstring type = ev.S(L"type");
                EnterCriticalSection(&j->cs);
                if (type == L"response.output_text.delta") {
                    const Jv* d = ev.Get(L"delta");
                    if (d) j->out += (d->t == 3 ? d->str : (d->t == 5 ? d->S(L"text") : L""));
                } else if (type == L"response.reasoning_text.delta") {
                    const Jv* d = ev.Get(L"delta");
                    if (d) j->reason += (d->t == 3 ? d->str : (d->t == 5 ? d->S(L"text") : L""));
                } else if (type == L"response.output_item.added") {
                    const Jv* item = ev.Get(L"item");
                    if (item && item->t == 5 && item->S(L"type") == L"function_call") {
                        AiCall* slot = AgentCallSlot(turnCalls, ev.Get(L"output_index"));
                        slot->name = U8(item->S(L"name"));
                        slot->callId = U8(item->S(L"call_id"));
                    }
                } else if (type == L"response.output_item.done") {
                    /* 收尾事件携带权威全量字段: 个别模型 added 缺 name/call_id, 从这里补齐 */
                    const Jv* item = ev.Get(L"item");
                    if (item && item->t == 5 && item->S(L"type") == L"function_call") {
                        AiCall* slot = AgentCallSlot(turnCalls, ev.Get(L"output_index"));
                        const Jv* n = item->Get(L"name");
                        const Jv* ci = item->Get(L"call_id");
                        const Jv* a = item->Get(L"arguments");
                        if (n && n->t == 3 && !n->str.empty()) slot->name = U8(n->str);
                        if (ci && ci->t == 3 && !ci->str.empty()) slot->callId = U8(ci->str);
                        if (a && a->t == 3 && !a->str.empty()) slot->args = U8(a->str);
                    }
                } else if (type == L"response.function_call_arguments.delta" ||
                           type == L"response.function_call_arguments.done") {
                    AiCall* slot = AgentCallSlot(turnCalls, ev.Get(L"output_index"));
                    /* added/done 之外的兜底补采 */
                    if (slot->name.empty()) {
                        const Jv* n = ev.Get(L"name");
                        if (n && n->t == 3) slot->name = U8(n->str);
                    }
                    if (slot->callId.empty()) {
                        const Jv* ci = ev.Get(L"call_id");
                        if (ci && ci->t == 3) slot->callId = U8(ci->str);
                    }
                    const Jv* d = ev.Get(L"delta");
                    const Jv* a = ev.Get(L"arguments");
                    if (type == L"response.function_call_arguments.done")
                        slot->args = a && a->t == 3 ? U8(a->str) : slot->args;
                    else if (d)
                        slot->args += (d->t == 3 ? U8(d->str) : std::string());
                } else if (type == L"response.completed") {
                    /* 用量统计 (对齐参考实现): input=计费输入, cached=前缀缓存命中 */
                    const Jv* rsp = ev.Get(L"response");
                    const Jv* us = (rsp && rsp->t == 5) ? rsp->Get(L"usage") : NULL;
                    if (us && us->t == 5) {
                        const Jv* x;
                        j->turnUsage.has = true;
                        if ((x = us->Get(L"input_tokens")) && x->t == 2) j->turnUsage.prompt = (long long)x->num;
                        if ((x = us->Get(L"output_tokens")) && x->t == 2) j->turnUsage.completion = (long long)x->num;
                        if ((x = us->Get(L"total_tokens")) && x->t == 2) j->turnUsage.total = (long long)x->num;
                        const Jv* dt = us->Get(L"input_tokens_details");
                        if (dt && dt->t == 5 && (x = dt->Get(L"cached_tokens")) && x->t == 2)
                            j->turnUsage.cacheHit = (long long)x->num;
                    }
                } else if (type == L"response.failed") {
                    const Jv* rsp = ev.Get(L"response");
                    if (rsp && rsp->t == 5) {
                        const Jv* er = rsp->Get(L"error");
                        if (er && er->t == 5) *errMsg = U8(er->S(L"message").c_str());
                    }
                    *failed = true;
                } else if (type == L"response.incomplete") {
                    *truncated = true;
                } else if (type == L"error") {
                    const Jv* er = ev.Get(L"error");
                    *errMsg = U8((er && er->t == 5 ? er->S(L"message") : ev.S(L"message")).c_str());
                    *failed = true;
                }
                LeaveCriticalSection(&j->cs);
            }
            ULONGLONG now = GetTickCount64();
            if (now - lastPost > 40 && g_msgwnd) {   /* 节流回泵 (UI 抽增量渲染) */
                lastPost = now;
                PostMessageW(g_msgwnd, XJS_AI_STREAM, 0, (LPARAM)j);
            }
        }
    } while (0);
    EnterCriticalSection(&j->cs);
    if (j->hReq) { WinHttpCloseHandle(j->hReq); j->hReq = NULL; }
    LeaveCriticalSection(&j->cs);
    WinHttpCloseHandle(hr);
    return ok;
}

void WorkerMain(AiJob* j) {   /* agent 循环: SSE → 工具执行 → 结果回填 → 下一轮, 直到最终答复 */
    wchar_t whost[512] = {};
    MultiByteToWideChar(CP_UTF8, 0, j->hostA.c_str(), -1, whost, 512);
    HINTERNET hs = WinHttpOpen(L"snail-quicksearch-ai-assistant", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!hs) hs = WinHttpOpen(L"snail-quicksearch-ai-assistant", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    HINTERNET hc = NULL;
    if (hs) {
        WinHttpSetTimeouts(hs, 15000, 30000, 30000, 120000);   /* 单轮超时; 多轮总时长由轮数×超时构成 */
        hc = WinHttpConnect(hs, whost, j->port, 0);
    }
    bool aborted = false, truncated = false, failed = false;
    std::string errMsg;
    if (!hs || !hc) { failed = true; errMsg = hs ? "connect failed" : "network init failed"; }
    else {
        std::vector<AiCall> accCalls;       /* 已执行的工具往返 (input 回填; 与 accOuts 一一对应) */
        std::vector<std::string> accOuts;
        for (int turn = 0; turn < AI_AGENT_MAX_TURNS; turn++) {
            if (InterlockedCompareExchange(&j->abort, 0, 0)) { aborted = true; break; }
            bool lastTurn = turn == AI_AGENT_MAX_TURNS - 1;
            std::vector<AiCall> turnCalls;
            ULONGLONG turnT0 = GetTickCount64();
            bool okTurn = AgentRunTurn(j, hc, accCalls, accOuts, !lastTurn, &turnCalls,
                                       &aborted, &truncated, &failed, &errMsg);
            EnterCriticalSection(&j->cs);
            j->turnOutMs = GetTickCount64() - turnT0;   /* 速度 = 本轮输出 / 本轮耗时 (含首 token 等待) */
            LeaveCriticalSection(&j->cs);
            if (!okTurn) failed = true;
            if (aborted || failed) break;
            if (turnCalls.empty()) break;   /* 没有工具调用 = 最终答复完成 */
            /* 工具执行 (仍在本线程, 串行; 步骤镜像实时推进给泵渲染) */
            EnterCriticalSection(&j->cs);
            j->phase = 1;   /* 泵冻结当前文本气泡 (下一轮答复另起新气泡) */
            LeaveCriticalSection(&j->cs);
            if (g_msgwnd) PostMessageW(g_msgwnd, XJS_AI_STREAM, 0, (LPARAM)j);
            for (auto& c : turnCalls) {
                if (InterlockedCompareExchange(&j->abort, 0, 0)) break;
                AiToolStep local;
                local.state = 1;
                local.name = W8(c.name.c_str());
                int sidx = -1;
                EnterCriticalSection(&j->cs);
                j->steps.push_back(local);
                sidx = (int)j->steps.size() - 1;
                j->stepsVersion++;
                LeaveCriticalSection(&j->cs);
                if (g_msgwnd) PostMessageW(g_msgwnd, XJS_AI_STREAM, 0, (LPARAM)j);
                /* 文件操作权限闸 (对齐参考实现"命令行权限"): 禁用/只读直接拒绝并回喂模型;
                   询问 = 先拒绝 + 卡片转询问态给确认按钮, 用户点"允许"后本作业后续调用放行 */
                int pol = InterlockedCompareExchange(&j->policy, 0, 0);
                bool fileTool = (c.name == "open_file" || c.name == "copy_paths");
                std::wstring err;
                if (fileTool && pol == 0) {
                    err = L"权限策略为「禁用」, 已拒绝文件操作";
                    local.state = 3;
                } else if (fileTool && pol == 1) {
                    err = L"权限策略为「只读」, 已拒绝文件操作";
                    local.state = 3;
                } else if (fileTool && pol == 2) {
                    err = L"等待用户确认文件操作 (在下方卡片选择「允许」后我会重试)";
                    local.state = 4;
                } else {
                    err = AgentToolExec(j, c.name, c.args, j->tok, &local);
                }
                local.err = err;
                if (local.state == 1) local.state = err.empty() ? 2 : 3;
                std::string output = AgentToolOutput(err, local);
                EnterCriticalSection(&j->cs);
                if (sidx >= 0 && sidx < (int)j->steps.size()) {
                    AiToolStep& dst = j->steps[sidx];
                    dst.kind = local.kind;
                    dst.state = local.state;
                    dst.mode = local.mode;
                    dst.query = local.query;
                    dst.count = local.count;
                    dst.elapsedMs = local.elapsedMs;
                    dst.emit = local.emit;
                    dst.err = local.err;
                    dst.top = local.top;   /* open 展开态归泵/用户, 不覆盖 */
                    j->stepsVersion++;
                }
                LeaveCriticalSection(&j->cs);
                /* 缺 name/call_id 的调用不回喂 (API 校验 call_id 非空且与 output 成对,
                 * 喂空串整轮 400 连累同轮正常调用); 卡片照常显示错误 */
                if (!c.name.empty() && !c.callId.empty()) {
                    accCalls.push_back(c);
                    accOuts.push_back(output);
                }
                if (g_msgwnd) PostMessageW(g_msgwnd, XJS_AI_STREAM, 0, (LPARAM)j);
            }
            EnterCriticalSection(&j->cs);
            j->phase = 0;       /* 下一轮答复另起新文本气泡 */
            j->out.clear();
            j->reason.clear();
            LeaveCriticalSection(&j->cs);
        }
    }
    if (hc) WinHttpCloseHandle(hc);
    if (hs) WinHttpCloseHandle(hs);
    EnterCriticalSection(&j->cs);
    if (j->hReq) { WinHttpCloseHandle(j->hReq); j->hReq = NULL; }
    j->truncated = truncated;
    j->err = W8(errMsg.c_str());
    j->state = aborted ? 3 : (failed ? 2 : 1);
    LeaveCriticalSection(&j->cs);
    if (g_msgwnd) PostMessageW(g_msgwnd, XJS_AI_STREAM, 0, (LPARAM)j);
}

