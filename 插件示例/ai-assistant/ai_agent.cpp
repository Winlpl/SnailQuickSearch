/*
 * ai_agent.cpp — agent 大脑: 引擎直连工具执行 / 系统提示词组装 / 工具定义 /
 * 请求体构建 / SSE 轮 (function calling) / 工作线程循环
 * 私有结果对象 = 进程级单例 + 互斥串行, 不碰窗口结果对象; Query/ExecuteLua 用
 * waitComplete=TRUE 阻塞等完成 (主线程被引擎拒绝, 工作线程正是推荐用法)。
 */
#include "ai_assistant.h"

/* ==================== 引擎直连 (agent 工具执行层) ====================
 * 插件进程 = 宿主进程, 加载器按模块名绑到宿主已加载的 xunjieso.dll 同一实例;
 * xjs_GetDefaultEngine() 取句柄。agent 的搜索全部打到插件私有结果对象 g_agentRes
 * (不碰窗口结果对象 = 不打扰用户正在看的搜索)。工具执行一律在 agent 工作线程 +
 * g_agentCs 串行 (多窗口同时让 AI 跑工具时排队), Query/ExecuteLua 用 waitComplete=TRUE
 * 阻塞等完成 (主线程被引擎拒绝, 工作线程正是推荐用法; Lua 看门狗 10 秒兜底)。
 * 结果对象必须引擎空闲态创建 (忙时创建会被永久定成文件名序), 懒建 + 失效重建。 */

static CRITICAL_SECTION g_agentCs;
static xjs_result* g_agentRes = NULL;     /* agent 私有结果对象 (进程级一份, 串行使用) */
static std::vector<int> g_agentTopIds;    /* 最近一次 run_search 样本 fileId (open_file 按 1-based 序号取用) */

void AgentToolInit() { InitializeCriticalSectionAndSpinCount(&g_agentCs, 100); }
void AgentToolShutdown() {
    if (g_agentRes) { xjs_result_Destroy(g_agentRes); g_agentRes = NULL; }
    g_agentTopIds.clear();
    DeleteCriticalSection(&g_agentCs);
}

/* run_search 实体; 返回空串 = 成功, 否则 = 错误描述 (调用方持 g_agentCs) */
static std::wstring AgentToolRunSearch(const std::wstring& mode, const std::wstring& query, AiToolStep* st) {
    xjs_engine* eng = xjs_GetDefaultEngine();
    if (!eng) return L"搜索引擎未就绪";
    if (g_agentRes && !xjs_result_IsEffective(g_agentRes)) g_agentRes = NULL;
    if (!g_agentRes) {
        if (xjs_db_GetEngineState(eng) != XJS_DB_STATE_IDLE) return L"索引正忙 (加载/扫描中), 请稍后重试";
        g_agentRes = xjs_result_Create(eng);
        if (!g_agentRes) return L"结果对象创建失败";
    }
    std::string q8 = U8(query);
    int fp = -1;
    if (mode == L"wildcard")      fp = xjs_result_Query(g_agentRes, q8.c_str(), 0, TRUE);
    else if (mode == L"regex")    fp = xjs_result_Query(g_agentRes, q8.c_str(), 1, TRUE);
    else if (mode == L"sql")      fp = xjs_result_Query(g_agentRes, q8.c_str(), 2, TRUE);
    else if (mode == L"lua_filter") fp = xjs_result_Query(g_agentRes, q8.c_str(), XJS_KEYWORD_LUA, TRUE);
    else if (mode == L"lua_exec") fp = xjs_result_ExecuteLua(g_agentRes, q8.c_str(), TRUE);
    else return L"未知搜索模式: " + mode;
    if (fp < 0) {
        std::wstring e = W8(xjs_GetLastErrorMsg(eng));
        return L"搜索发起失败: " + (e.empty() ? std::wstring(L"引擎拒绝") : e);
    }
    if (!xjs_result_IsCompleted(g_agentRes, fp)) return L"搜索等待超时";   /* waitComplete=TRUE 正常不发生 */
    st->count = xjs_result_GetCount(g_agentRes);
    st->elapsedMs = xjs_result_GetElapsed(g_agentRes);
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

/* 工具统一入口 (agent 工作线程调用): 解析参数 → 执行 → 结果填进 step; 返回错误描述 (空=成功) */
static std::wstring AgentToolExec(const std::string& name8, const std::string& args8,
                                  XjsWindowToken tok, AiToolStep* st) {
    Jv v = JsonParseW(W8(args8.c_str()));
    if (name8 == "run_search") {
        st->kind = 0;
        st->mode = v.S(L"mode");
        st->query = v.S(L"query");
        if (st->query.empty()) return L"query 不能为空";
        EnterCriticalSection(&g_agentCs);
        std::wstring err = AgentToolRunSearch(st->mode, st->query, st);
        LeaveCriticalSection(&g_agentCs);
        return err;
    }
    if (name8 == "open_file") {
        st->kind = 1;
        const Jv* rv = v.Get(L"reveal");
        bool reveal = rv && ((rv->t == 1 && rv->b) || (rv->t == 2 && rv->num != 0));
        int index = (int)wcstol(TrimW(v.S(L"index")).c_str(), NULL, 10);
        EnterCriticalSection(&g_agentCs);
        std::wstring err = AgentToolOpenFile(index, reveal, tok);
        LeaveCriticalSection(&g_agentCs);
        return err;
    }
    if (name8 == "copy_paths") {
        st->kind = 2;
        EnterCriticalSection(&g_agentCs);
        std::wstring err = AgentToolCopyPaths();
        LeaveCriticalSection(&g_agentCs);
        return err;
    }
    return L"未知工具: " + W8(name8.c_str());
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
        j += L"]}";
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
    L"支持文件名/大小/时间/类型/别名/内容等搜索。请用简体中文回答，简洁、准确、直接。\n"
    L"\n"
    L"## 工作方式\n"
    L"你有工具可用：涉及找文件/查文件/统计文件的任务，**先调用 run_search 工具实际执行搜索**，"
    L"根据返回的条数与路径样本判断结果是否符合，不符合就换口径再搜（逐步逼近：先粗筛再精筛），"
    L"不要凭空编造文件路径；需要给用户看文件时用 open_file，需要给出路径清单时用 copy_paths。"
    L"得到足够信息后，用最终答复总结：找到什么、在哪、关键数据；并给出可粘贴到搜索框的搜索式（注明模式）。\n"
    L"与搜索无关的问题直接回答，不要调用工具。\n"
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
    L"选择建议：找名字用 wildcard/regex；按字段组合筛选/统计用 sql；逐文件自定义判断用 lua_filter；"
    L"跨文件聚合/自定义排序/树形下钻用 lua_exec。\n"
    L"\n"
    L"以下两份规范来自搜索引擎内置提示词，写 lua_filter / lua_exec 脚本时必须严格遵守其中的 API 全集与硬性规则"
    L"（脚本沙箱删除了 io/os 等库，API 以规范所列为准，绝不虚构函数）：\n"
    L"\n"
    L"════════════ 《Lua 过滤模式脚本规范》(mode=lua_filter) ════════════\n";

static const wchar_t* AI_INSTRUCTIONS_TAIL =
    L"\n════════════ 《Lua 执行模式脚本规范》(mode=lua_exec) ════════════\n";

/* 工具定义 (Responses API tools 数组; 与 AgentToolExec 的名字/参数一一对应) */
static const char* AI_TOOLS_JSON = R"json([
  {"type":"function","name":"run_search","description":"在蜗牛快搜索引中执行一次搜索, 返回命中总数与前 20 条路径样本。可多次调用逐步逼近目标 (先粗筛再精筛)。5 种 mode 的搜索词语法以系统提示词中的说明与两份 Lua 规范为准。","parameters":{"type":"object","properties":{"mode":{"type":"string","enum":["wildcard","regex","sql","lua_filter","lua_exec"],"description":"wildcard=通配符 regex=PCRE2正则 sql=SELECT语句 lua_filter=过滤模式Lua脚本 lua_exec=执行模式Lua脚本"},"query":{"type":"string","description":"搜索词/脚本全文 (lua 两种模式传完整脚本文本)"}},"required":["mode","query"]}},
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

/* 一轮 SSE 事件里攒下的 function_call (模型要求执行的工具) */
struct AiCall {
    std::string callId, name, args;
};

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
    body += W8(JsonEscapeUtf8(W8(g_instrA.c_str())).c_str());
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
                        AiCall c;
                        c.name = U8(item->S(L"name"));
                        c.callId = U8(item->S(L"call_id"));
                        const Jv* oi = ev.Get(L"output_index");
                        int outIdx = (oi && oi->t == 2) ? (int)oi->num : (int)turnCalls->size();
                        if (outIdx < 0) outIdx = (int)turnCalls->size();
                        while ((int)turnCalls->size() <= outIdx) turnCalls->push_back(AiCall());
                        (*turnCalls)[outIdx] = c;
                    }
                } else if (type == L"response.function_call_arguments.delta" ||
                           type == L"response.function_call_arguments.done") {
                    const Jv* oi = ev.Get(L"output_index");
                    size_t p = (oi && oi->t == 2 && oi->num >= 0 && oi->num < (double)turnCalls->size())
                                   ? (size_t)oi->num : (turnCalls->empty() ? 0 : turnCalls->size() - 1);
                    if (!turnCalls->empty()) {
                        const Jv* d = ev.Get(L"delta");
                        const Jv* a = ev.Get(L"arguments");
                        if (type == L"response.function_call_arguments.done")
                            (*turnCalls)[p].args = a && a->t == 3 ? U8(a->str) : (*turnCalls)[p].args;
                        else if (d)
                            (*turnCalls)[p].args += (d->t == 3 ? U8(d->str) : std::string());
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
            bool okTurn = AgentRunTurn(j, hc, accCalls, accOuts, !lastTurn, &turnCalls,
                                       &aborted, &truncated, &failed, &errMsg);
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
                std::wstring err = AgentToolExec(c.name, c.args, j->tok, &local);
                local.state = err.empty() ? 2 : 3;
                local.err = err;
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
                    dst.err = local.err;
                    dst.top = local.top;   /* open 展开态归泵/用户, 不覆盖 */
                    j->stepsVersion++;
                }
                LeaveCriticalSection(&j->cs);
                accCalls.push_back(c);
                accOuts.push_back(output);
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

