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

/* UI 编组在飞登记 (声明提前 — AgentToolInit/Shutdown 在此就触碰; 机制本体见下方"宿主扩展 API"节) */
static CRITICAL_SECTION s_uiCs;
static std::vector<AiUiJob*> s_inflight;

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
    InitializeCriticalSectionAndSpinCount(&s_uiCs, 100);
}
void AgentToolShutdown() {
    if (g_agentRes) { xjs_result_Destroy(g_agentRes); g_agentRes = NULL; }
    g_agentTopIds.clear();
    g_emitBuf.clear();   /* 到此工作线程已全部 join, 无并发 */
    g_emitCut = false;
    for (auto* jb : s_inflight) {   /* 投递后无人认领的编组作业 (消息窗口先没了) 代为回收 */
        if (jb->done) CloseHandle(jb->done);
        delete jb;
    }
    s_inflight.clear();
    DeleteCriticalSection(&s_uiCs);
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

/* ==================== 宿主扩展 API (QueryApi) + UI 编组 ====================
 * settings/window/modes/plugins/skins 这组扩展 API 全部仅 UI 线程 (宿主闸门查线程,
 * 错线程 = ERR_THREAD), 而 agent 工具执行在工作者线程 — 一律经 AiUiJob 编组到
 * g_msgwnd (UI 线程) 执行: worker 投递+轮询等完成 (abort 可退, 15 秒兜底), UI 侧
 * wndproc 调 AgentUiDispatch 分派。所有权仲裁走 s_uiCs: worker 放弃时登记在飞表,
 * UI 执行完发现 orphan 代为 delete — 两侧都不悬垂不双删。 */

HostApi g_api;

void ApiResolveAll() {   /* UI 线程 (Init); 旧宿主 = 全 NULL, 相关工具报"不支持" */
    g_api = HostApi{};
    if (!g_host || !HOST_QAPI_OK || !g_host->QueryApi) return;
    g_api.settingsGet  = (XjsApiSettingsGet)g_host->QueryApi(g_ctx, XJS_API_SETTINGS_GET);
    g_api.settingsSet  = (XjsApiSettingsSet)g_host->QueryApi(g_ctx, XJS_API_SETTINGS_SET);
    g_api.globalGet    = (XjsApiGlobalGet)g_host->QueryApi(g_ctx, XJS_API_GLOBAL_GET);
    g_api.globalSet    = (XjsApiGlobalSet)g_host->QueryApi(g_ctx, XJS_API_GLOBAL_SET);
    g_api.windowsEnum  = (XjsApiWindowsEnum)g_host->QueryApi(g_ctx, XJS_API_WINDOWS_ENUM);
    g_api.windowState  = (XjsApiWindowState)g_host->QueryApi(g_ctx, XJS_API_WINDOW_STATE);
    g_api.windowCmd    = (XjsApiWindowCmd)g_host->QueryApi(g_ctx, XJS_API_WINDOW_CMD);
    g_api.windowCreate = (XjsApiWindowCreate)g_host->QueryApi(g_ctx, XJS_API_WINDOW_CREATE);
    g_api.modesList    = (XjsApiModesList)g_host->QueryApi(g_ctx, XJS_API_MODES_LIST);
    g_api.modesAdd     = (XjsApiModesAdd)g_host->QueryApi(g_ctx, XJS_API_MODES_ADD);
    g_api.modesRemove  = (XjsApiModesRemove)g_host->QueryApi(g_ctx, XJS_API_MODES_REMOVE);
    g_api.modesApply   = (XjsApiModesApply)g_host->QueryApi(g_ctx, XJS_API_MODES_APPLY);
    g_api.pluginsList  = (XjsApiPluginsList)g_host->QueryApi(g_ctx, XJS_API_PLUGINS_LIST);
    g_api.pluginsState = (XjsApiPluginsState)g_host->QueryApi(g_ctx, XJS_API_PLUGINS_STATE);
    g_api.msgSend      = (XjsApiMsgSend)g_host->QueryApi(g_ctx, XJS_API_MSG_SEND);
    g_api.skinsList    = (XjsApiSkinsList)g_host->QueryApi(g_ctx, XJS_API_SKINS_LIST);
    g_api.windowSel    = (XjsApiWindowSelection)g_host->QueryApi(g_ctx, XJS_API_WINDOW_SELECTION);
    g_api.langsList    = (XjsApiLangsList)g_host->QueryApi(g_ctx, XJS_API_LANGS_LIST);
}

/* UIW_* 分派码 (AiUiJob::kind) */
enum {
    UIW_LIST_WINDOWS = 1, UIW_WINDOW_STATE, UIW_GET_GLOBAL,
    UIW_SET_SEARCH, UIW_LIST_MODES, UIW_APPLY_MODE,
    UIW_ADD_MODE, UIW_REMOVE_MODE, UIW_LIST_PLUGINS, UIW_PLUGIN_STATE, UIW_SEND_MSG,
    UIW_LIST_SKINS, UIW_OPEN_FILE, UIW_WINDOW_SELECTION, UIW_GET_LANGUAGE, UIW_LIST_LANGS,
};

static std::wstring ApiErrText(int rc) {
    switch (rc) {
        case XJS_PLUGIN_OK:         return L"";
        case XJS_PLUGIN_ERR_ARG:     return L"参数非法 (键名/取值不被接受)";
        case XJS_PLUGIN_ERR_PERM:    return L"权限未声明或插件已被禁用 (manifest 权限闸)";
        case XJS_PLUGIN_ERR_THREAD:  return L"线程契约错误";
        case XJS_PLUGIN_ERR_STATE:   return L"状态不允许 (引擎扫描中/目标不可用)";
        case XJS_PLUGIN_ERR_NOTFOUND: return L"目标不存在 (窗口已关/名单里没有)";
        case XJS_PLUGIN_ERR_IO:      return L"读写失败";
        case XJS_PLUGIN_ERR_FAIL:    return L"操作失败";
        default:                     return L"宿主返回错误码 " + std::to_wstring(rc);
    }
}

/* 调用方缓冲两段式取 JSON (fn = 输出约定同宿主: buf==NULL 报所需长度) */
template <class F> static void ApiCallOut(std::string* out, std::wstring* err, F fn) {
    int need = fn(NULL, 0);
    if (need < 0) { *err = ApiErrText(need); return; }
    std::string s((size_t)need + 1, 0);
    int n = need ? fn(&s[0], need + 1) : 0;
    if (n < 0) { *err = ApiErrText(n); return; }
    s.resize((size_t)(n > need ? need : n));
    *out = s;
}
template <class F> static void ApiCallRc(std::wstring* err, F fn) {
    int rc = fn();
    if (rc != XJS_PLUGIN_OK) *err = ApiErrText(rc);
}

/* Jv → 宽 JSON 文本 (合并读结果 / 发给同伴插件的载荷序列化用) */
static void JvAppend(const Jv& v, std::wstring* out) {
    switch (v.t) {
        case 0: *out += L"null"; break;
        case 1: *out += v.b ? L"true" : L"false"; break;
        case 2: {
            wchar_t nb[40];
            swprintf(nb, 40, L"%.17g", v.num);
            *out += nb;
            break;
        }
        case 3: *out += W8(JsonEscapeUtf8(v.str).c_str()); break;
        case 4: {
            *out += L"[";
            bool f = true;
            for (auto& e : v.arr) { if (!f) *out += L","; f = false; JvAppend(e, out); }
            *out += L"]";
            break;
        }
        case 5: {
            *out += L"{";
            bool f = true;
            for (auto& kv : v.obj) {
                if (!f) *out += L",";
                f = false;
                *out += W8(JsonEscapeUtf8(kv.first).c_str());
                *out += L":";
                JvAppend(kv.second, out);
            }
            *out += L"}";
            break;
        }
    }
}

/* 窗口名称 → 令牌 (UI 线程; 名称空 = 默认令牌=对话所在窗)。查无 = *err 设描述, 返 0 */
static long long UiWindowToken(const std::wstring& name, long long defTok, std::wstring* err) {
    if (name.empty()) return defTok;
    if (!g_api.windowsEnum) { *err = L"当前宿主不支持该操作"; return 0; }
    std::string j8;
    ApiCallOut(&j8, err, [&](char* b, int c) { return g_api.windowsEnum(g_ctx, b, c); });
    if (!err->empty()) return 0;
    Jv v = JsonParseW(W8(j8.c_str()));
    for (auto& w : v.arr) {
        if (w.t == 5 && w.S(L"名称") == name) {
            const Jv* t = w.Get(L"令牌");
            if (t && t->t == 2) return (long long)t->num;
        }
    }
    *err = L"窗口不存在: " + name + L" (用 list_windows 查有效名称)";
    return 0;
}

/* window.state + settings.get 合并 (后者独有键: 失焦行为/显示类开关/任务栏图标/鼠标打开/默认选中) */
static void UiStateMerged(long long tok, std::string* out, std::wstring* err) {
    std::string st8, se8;
    ApiCallOut(&st8, err, [&](char* b, int c) { return g_api.windowState(g_ctx, (XjsWindowToken)tok, b, c); });
    if (!err->empty()) return;
    std::wstring e2;
    ApiCallOut(&se8, &e2, [&](char* b, int c) { return g_api.settingsGet(g_ctx, (XjsWindowToken)tok, b, c); });
    Jv a = JsonParseW(W8(st8.c_str()));
    Jv b = e2.empty() ? Jv() : JsonParseW(W8(se8.c_str()));
    std::wstring m = L"{";
    bool first = true;
    auto put = [&](const std::wstring& k, const Jv& val) {
        if (!first) m += L",";
        first = false;
        m += W8(JsonEscapeUtf8(k).c_str());
        m += L":";
        JvAppend(val, &m);
    };
    if (a.t == 5)
        for (auto& kv : a.obj) put(kv.first, kv.second);
    if (b.t == 5)
        for (auto& kv : b.obj)
            if (a.t != 5 || !a.Get(kv.first.c_str())) put(kv.first, kv.second);
    m += L"}";
    *out = U8(m);
}

/* UI 线程分派 (g_msgwnd wndproc 调; 全部宿主扩展 API/宿主表 UI 函数都在这条线上) */
static void UiDispatchRun(AiUiJob* jb) {
    const XjsWindowToken defTok = (XjsWindowToken)jb->tok;
    std::wstring errw;
    long long tok = 0;
    switch (jb->kind) {
        case UIW_LIST_WINDOWS:
            if (!g_api.windowsEnum) { jb->err = L"当前宿主不支持该操作"; return; }
            ApiCallOut(&jb->out8, &jb->err, [&](char* b, int c) { return g_api.windowsEnum(g_ctx, b, c); });
            return;
        case UIW_WINDOW_STATE:
            if (!g_api.windowState) { jb->err = L"当前宿主不支持该操作"; return; }
            tok = UiWindowToken(jb->s1, (long long)defTok, &errw);
            if (!errw.empty()) { jb->err = errw; return; }
            UiStateMerged(tok, &jb->out8, &jb->err);
            return;
        case UIW_GET_GLOBAL:
            if (!g_api.globalGet) { jb->err = L"当前宿主不支持该操作"; return; }
            ApiCallOut(&jb->out8, &jb->err, [&](char* b, int c) { return g_api.globalGet(g_ctx, b, c); });
            return;
        case UIW_SET_SEARCH:
            tok = UiWindowToken(jb->s1, (long long)defTok, &errw);
            if (!errw.empty()) { jb->err = errw; return; }
            if (!g_host) { jb->err = L"宿主不可用"; return; }
            ApiCallRc(&jb->err, [&] {
                return g_host->SearchSetText(g_ctx, (XjsWindowToken)tok,
                                             jb->s2.empty() ? NULL : U8(jb->s2).c_str(),
                                             jb->s3.empty() ? NULL : U8(jb->s3).c_str(), (int)jb->n1);
            });
            return;
        case UIW_LIST_MODES:
            if (!g_api.modesList) { jb->err = L"当前宿主不支持该操作"; return; }
            tok = UiWindowToken(jb->s1, (long long)defTok, &errw);
            if (!errw.empty()) { jb->err = errw; return; }
            ApiCallOut(&jb->out8, &jb->err, [&](char* b, int c) { return g_api.modesList(g_ctx, (XjsWindowToken)tok, b, c); });
            return;
        case UIW_APPLY_MODE:
            if (!g_api.modesApply) { jb->err = L"当前宿主不支持该操作"; return; }
            tok = UiWindowToken(jb->s1, (long long)defTok, &errw);
            if (!errw.empty()) { jb->err = errw; return; }
            ApiCallRc(&jb->err, [&] {
                return g_api.modesApply(g_ctx, (XjsWindowToken)tok, U8(jb->s2).c_str(),
                                        jb->s3.empty() ? NULL : U8(jb->s3).c_str());
            });
            return;
        case UIW_ADD_MODE: {
            /* modes.add 有副作用 (每次调用真加一个模式), 不能走两段式取长 — 单次调用大缓冲 */
            if (!g_api.modesAdd) { jb->err = L"当前宿主不支持该操作"; return; }
            std::string s(4096, 0);
            int n = g_api.modesAdd(g_ctx, 0, U8(jb->s1).c_str(), &s[0], (int)s.size() - 1);
            if (n < 0) { jb->err = ApiErrText(n); return; }
            s.resize((size_t)n);
            jb->out8 = s;
            return;
        }
        case UIW_REMOVE_MODE:
            if (!g_api.modesRemove) { jb->err = L"当前宿主不支持该操作"; return; }
            ApiCallRc(&jb->err, [&] { return g_api.modesRemove(g_ctx, U8(jb->s1).c_str()); });
            return;
        case UIW_LIST_PLUGINS:
            if (!g_api.pluginsList) { jb->err = L"当前宿主不支持该操作"; return; }
            ApiCallOut(&jb->out8, &jb->err, [&](char* b, int c) { return g_api.pluginsList(g_ctx, b, c); });
            return;
        case UIW_PLUGIN_STATE:
            if (!g_api.pluginsState) { jb->err = L"当前宿主不支持该操作"; return; }
            ApiCallOut(&jb->out8, &jb->err, [&](char* b, int c) { return g_api.pluginsState(g_ctx, U8(jb->s1).c_str(), b, c); });
            return;
        case UIW_SEND_MSG: {
            /* msg.send 有副作用 (每次真发一条消息), 不能两段式取长 — 单次调用 64KB 收回复 */
            if (!g_api.msgSend) { jb->err = L"当前宿主不支持该操作"; return; }
            std::string s(64 * 1024, 0);
            int n = g_api.msgSend(g_ctx, U8(jb->s1).c_str(), U8(jb->s2).c_str(), &s[0], (int)s.size() - 1);
            if (n < 0) { jb->err = ApiErrText(n); return; }
            s.resize((size_t)n);
            jb->out8 = s.empty() ? "{\"回复\":null}" : s;
            return;
        }
        case UIW_LIST_SKINS:
            if (!g_api.skinsList) { jb->err = L"当前宿主不支持该操作"; return; }
            ApiCallOut(&jb->out8, &jb->err, [&](char* b, int c) { return g_api.skinsList(g_ctx, b, c); });
            return;
        case UIW_OPEN_FILE:
            tok = UiWindowToken(jb->s1, (long long)defTok, &errw);
            if (!errw.empty()) { jb->err = errw; return; }
            if (!g_host) { jb->err = L"宿主不可用"; return; }
            ApiCallRc(&jb->err, [&] {
                return g_host->OpenFile(g_ctx, (XjsWindowToken)tok, (int)jb->n1, (int)jb->n2);
            });
            return;
        case UIW_WINDOW_SELECTION:
            if (!g_api.windowSel) { jb->err = L"当前宿主不支持该操作"; return; }
            tok = UiWindowToken(jb->s1, (long long)defTok, &errw);
            if (!errw.empty()) { jb->err = errw; return; }
            ApiCallOut(&jb->out8, &jb->err, [&](char* b, int c) {
                return g_api.windowSel(g_ctx, (XjsWindowToken)tok, (int)jb->n1, b, c);
            });
            return;
        case UIW_GET_LANGUAGE: {
            if (!g_api.settingsGet) { jb->err = L"当前宿主不支持该操作"; return; }
            tok = UiWindowToken(jb->s1, (long long)defTok, &errw);
            if (!errw.empty()) { jb->err = errw; return; }
            std::string se8;
            ApiCallOut(&se8, &jb->err, [&](char* b, int c) { return g_api.settingsGet(g_ctx, (XjsWindowToken)tok, b, c); });
            if (!jb->err.empty()) return;
            Jv o = JsonParseW(W8(se8.c_str()));
            std::wstring wname = o.S(L"窗口名称");
            std::wstring code = o.S(L"语言");
            if (code.empty()) { jb->err = L"宿主未返回语言设置 (宿主版本过旧?)"; return; }
            std::wstring j2 = L"{\"窗口名称\":" + W8(JsonEscapeUtf8(wname).c_str());
            j2 += L",\"语言\":" + W8(JsonEscapeUtf8(code).c_str());
            j2 += L",\"说明\":\"auto=跟随系统; 切换用 set_language\"}";
            jb->out8 = U8(j2);
            return;
        }
        case UIW_LIST_LANGS:
            if (!g_api.langsList) { jb->err = L"当前宿主不支持该操作"; return; }
            ApiCallOut(&jb->out8, &jb->err, [&](char* b, int c) { return g_api.langsList(g_ctx, b, c); });
            return;
        default:
            jb->err = L"未知编组作业";
            return;
    }
}

void AgentUiDispatch(AiUiJob* jb) {   /* UI 线程 (g_msgwnd XJS_AI_UIJOB 分派) */
    if (!jb) return;
    UiDispatchRun(jb);
    EnterCriticalSection(&s_uiCs);
    jb->doneSignaled = 1;
    bool orphan = jb->orphan != 0;
    if (orphan) {   /* worker 已放弃: 摘出在飞表并由本线程代删 (不再 SetEvent, 句柄已被关) */
        for (size_t i = 0; i < s_inflight.size(); i++)
            if (s_inflight[i] == jb) { s_inflight.erase(s_inflight.begin() + i); break; }
    }
    LeaveCriticalSection(&s_uiCs);
    if (orphan) delete jb;
    else SetEvent(jb->done);
}

std::wstring AgentUiCall(AiJob* j, AiUiJob* jb, std::string* out8) {
    /* jb 所有权归本函数: 正常路径此处 delete; 放弃路径登记在飞由 UI 代删 */
    if (!g_msgwnd) { delete jb; return L"消息窗口未就绪 (面板已关闭?)"; }
    jb->done = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!jb->done) { delete jb; return L"同步对象创建失败"; }
    if (!PostMessageW(g_msgwnd, XJS_AI_UIJOB, 0, (LPARAM)jb)) {
        CloseHandle(jb->done);
        delete jb;
        return L"界面任务投递失败";
    }
    ULONGLONG t0 = GetTickCount64();
    for (;;) {
        if (WaitForSingleObject(jb->done, 40) == WAIT_OBJECT_0) break;
        bool stop = InterlockedCompareExchange(&j->abort, 0, 0) != 0;
        bool slow = GetTickCount64() - t0 > 15000;   /* 界面线程被模态/长活卡住的兜底 */
        if (!stop && !slow) continue;
        bool doneByUi = false;
        EnterCriticalSection(&s_uiCs);   /* 清理权仲裁: UI 已完成 = 走正常收尾 */
        if (jb->doneSignaled) doneByUi = true;
        else { jb->orphan = 1; s_inflight.push_back(jb); }
        LeaveCriticalSection(&s_uiCs);
        if (doneByUi) break;
        CloseHandle(jb->done);
        return stop ? L"已停止" : L"界面操作超时 (主界面 15 秒未响应)";
    }
    *out8 = jb->out8;
    std::wstring err = jb->err;
    CloseHandle(jb->done);
    delete jb;
    return err;
}

long long AgentUiWindowToken(const std::wstring& name, long long defTok, std::wstring* err) {
    return UiWindowToken(name, defTok, err);   /* 仅 UI 线程 (内部调 windows.enum 扩展 API) */
}

std::wstring AgentApiErrText(int rc) { return ApiErrText(rc); }

/* 调整项展示值: 键+JSON 值 → 用户视角文案 (未识别的键原样透出; 与工具 description 同一套取值) */
static std::wstring AdjustValueText(const std::wstring& key, const Jv& v) {
    if (v.t == 1) return v.b ? L"开" : L"关";
    if (v.t == 2) {
        wchar_t b[40];
        if (key == L"页面缩放") { swprintf(b, 40, L"%.0f%%", v.num); return b; }
        if (key == L"失焦行为") return v.num == 0 ? L"无操作" : L"失焦时关闭窗口";
        if (key == L"鼠标打开") return v.num == 0 ? L"双击打开" : L"单击打开";
        if (key == L"默认选中") return v.num == 0 ? L"不自动选" : L"自动选第一项";
        swprintf(b, 40, L"%.0f", v.num);
        return b;
    }
    std::wstring s = v.t == 3 ? v.str : L"";
    if (key == L"视图") {
        if (s == L"list") return L"列表";
        if (s == L"details") return L"详情";
        if (s == L"medium") return L"中图标";
        if (s == L"large") return L"大图标";
    } else if (key == L"语言") {
        if (s == L"auto") return L"跟随系统";
        if (s == L"zh") return L"简体中文";
        if (s == L"zh-TW") return L"繁體中文";
        if (s == L"en") return L"English";
        if (s == L"ko") return L"한국어";
        if (s == L"th") return L"ไทย";
        if (s == L"ms") return L"Bahasa Melayu";
    } else if (key == L"绘制引擎") {
        if (s == L"d2d") return L"标准模式";
        if (s == L"gdiplus") return L"兼容模式 (重启生效)";
    }
    return s;
}

/* 把代办改动列成"待应用的调整"卡片 (不直接执行 — 用户在卡上逐项 应用/忽略,
 * 应用动作发生在用户点击后的 UI 线程, 见 ai_web.cpp WebCommand "adj")。
 * worker 只负责挂提案 + 给模型回执; 返回错误描述 (空=成功)。 */
static std::wstring ProposeAdjustCard(AiToolStep* st, int kind, const std::wstring& win,
                                      std::vector<AiAdjustItem> items) {
    if (items.empty()) return L"没有可提交的调整项";
    if (items.size() > 16) return L"一次提交的调整项过多 (≤16 项)";
    st->adj.kind = kind;
    st->adj.win = win;
    st->adj.items = std::move(items);
    std::wstring list;
    for (auto& it : st->adj.items) {
        if (!list.empty()) list += L"、";
        list += it.key + L" → " + it.val;
    }
    st->argz = L"待应用: " + list;
    if (st->argz.size() > 200) { st->argz.resize(200); st->argz += L"…"; }   /* 卡头截 200 (同 ArgzCut, 定义在其后故内联) */
    std::wstring j = L"{\"状态\":\"待用户应用\",\"条目\":[";
    for (size_t i = 0; i < st->adj.items.size(); i++) {
        if (i) j += L",";
        j += L"\"";
        j += W8(JsonEscapeUtf8(st->adj.items[i].key + L" → " + st->adj.items[i].val).c_str());
        j += L"\"";
    }
    j += L"],\"说明\":\"改动已列在\\\"待应用的调整\\\"卡片, 用户点\\\"应用\\\"才会生效;"
         L"用一句话请用户到卡片上确认 (可逐项应用或忽略), 绝不宣称已生效, 不要重复提交相同调整\"}";
    st->res8 = U8(j);
    return L"";
}

/* 代办类工具统一执行: 建 UI 作业 → 编组执行 → 结果/错误进 step。返回错误描述 (空=成功)。 */
static std::wstring AgentToolUi(AiJob* j, int uiKind, XjsWindowToken defTok,
                                const std::wstring& s1, const std::wstring& s2,
                                const std::wstring& s3, long long n1, long long n2,
                                AiToolStep* st) {
    AiUiJob* jb = new AiUiJob();
    jb->kind = uiKind;
    jb->tok = (long long)defTok;
    jb->s1 = s1;
    jb->s2 = s2;
    jb->s3 = s3;
    jb->n1 = n1;
    jb->n2 = n2;
    std::string out8;
    std::wstring err = AgentUiCall(j, jb, &out8);
    st->res8 = out8;
    return err;
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

/* open_file 实体: 打开/定位最近一次 run_search 样本中的文件 (走宿主打开行为)。
 * 宿主 OpenFile 是界面类 API (仅 UI 线程) — 经 UI 编组执行 (曾在工作者线程直调必吃 ERR_THREAD)。 */
static std::wstring AgentToolOpenFile(AiJob* j, int index, bool reveal, XjsWindowToken tok, AiToolStep* st) {
    if (index < 1 || index > (int)g_agentTopIds.size())
        return L"序号越界 (最近一次搜索样本共 " + std::to_wstring((unsigned long long)g_agentTopIds.size()) + L" 条)";
    int fid = g_agentTopIds[index - 1];
    return AgentToolUi(j, UIW_OPEN_FILE, tok, L"", L"", L"", fid, reveal ? 1 : 0, st);
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

/* 参数小工具 */
static std::wstring JWin(const Jv& v) {   /* window 参数 (窗口名称; 空 = 对话所在窗) */
    return TrimW(v.S(L"window"));
}
static bool JFlagTrue(const Jv& v, const wchar_t* key) {   /* 布尔参数缺省 = true */
    const Jv* b = v.Get(key);
    return !(b && b->t == 1 && !b->b);
}
static void ArgzCut(std::wstring* s) {
    if (s->size() > 200) { s->resize(200); *s += L"…"; }
}

/* 工具统一入口 (agent 工作线程调用): 解析参数 → 执行 → 结果填进 step; 返回错误描述 (空=成功)。
 * 串行锁轮询占用, 被停止打断返回"已停止"。
 * 搜索类 (run_search/copy_paths) 直连引擎/剪贴板任意线程; 其余一律 AgentToolUi 编组到 UI 线程。 */
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
        std::wstring err = AgentToolOpenFile(j, index, reveal, tok, st);
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
    if (name8 == "get_lua_spec") {
        /* Lua 规范按需展开 (提示词防膨胀): 引擎内嵌文本工作者线程直读, 无需 UI 编组。
         * 优先合集 (promptType 2 = 0+1 合并去重版, 一次投喂即可生成两种模式脚本, 体积更小);
         * 旧引擎无合集 = 按 mode 单取一份 (此时 mode 必填)。 */
        st->kind = 9;
        const char* p = xjs_LUA_GetPprompt(2);
        if (p && *p) {
            st->argz = L"Lua 规范合集";
        } else {
            std::wstring wm = TrimW(v.S(L"mode"));
            if (wm == L"lua_filter" || wm == L"lua_exec")
                p = xjs_LUA_GetPprompt(wm == L"lua_exec" ? 1 : 0);
            if (!p || !*p) return L"mode 必填 lua_filter | lua_exec (引擎无合集提示词)";
            st->argz = wm + L" 规范";
        }
        st->res8 = std::string(p);   /* 规范原文回喂 (纯文本) */
        return L"";
    }

    if (name8 == "get_author_and_donate") {
        /* 关于作者和捐赠: 作者/软件介绍 = README 权威事实 (模型据此回答, 不编造);
         * 二维码图片本体 (几百 KB base64) 绝不进对话通道烧 token — 只回引用语法,
         * md 渲染层把 xjs://donate?kind=.. 换成本地缓存真图 (DonateQrDataUrl; 工作者线程直调) */
        st->kind = 10;
        st->argz = L"作者与捐赠";
        bool w = !DonateQrDataUrl(0).empty();
        bool a = !DonateQrDataUrl(1).empty();
        std::wstring j = L"{\"软件\":\"蜗牛快搜 SnailQuickSearch — Windows 本地文件即时搜索工具:"
                         L"全盘索引, 输入即搜, 秒级定位, 同规模索引下常驻内存更低;"
                         L"纯 Direct2D 自绘界面, 内置可编程插件系统\"";
        j += L",\"授权\":\"免费软件: 个人与商业环境均可免费使用; 成品层源码开放查看/修改/二次开发;"
             L"内置 xunjieso 搜索引擎为版权人自研闭源组件, 随软件免费授权使用\"";
        j += L",\"特性\":\"输入即搜(无防抖)/全盘索引/低内存占用/五种搜索模式(wildcard,regex,sql,lua过滤,lua执行)/"
             L"多搜索窗口(每窗独立设置)/原生插件系统/六种语言界面/预览面板/文件操作(重命名,别名,剪切粘贴,拖出,定位打开)\"";
        j += L",\"捐赠话术\":\"如果蜗牛快搜帮到了你, 欢迎请作者喝杯咖啡 — 捐赠完全自愿, 金额随意, 软件本身永久免费\"";
        if (w || a) {
            j += L",\"可用二维码\":[";
            if (w) j += L"\"微信\"";
            if (a) j += (w ? L"," : L"") + std::wstring(L"\"支付宝\"");
            j += L"],\"二维码如何展示\":\"在回答正文里用图片语法引用 (只引上面列出的可用项):"
                 L" ![微信捐赠码](xjs://donate?kind=wechat) 或 ![支付宝捐赠码](xjs://donate?kind=alipay)。"
                 L"二维码会竖排显示在对话页, 微信优先放最前; 用户没点名要支付宝时可以只放微信。"
                 L"不要把 base64/文件路径写进回答, 不要用普通链接语法\"";
        } else {
            j += L",\"二维码\":\"暂不可用 (安装目录缺二维码图片), 如实告知用户即可\"";
        }
        j += L"}";
        st->res8 = U8(j);
        return L"";
    }

    /* ---- 代办类 (宿主扩展 API, 全部经 UI 编组; 宿主能力面见 xjs_plugin_sdk.h) ---- */
    if (!HOST_QAPI_OK) return L"宿主版本过旧, 不支持窗口/设置代办工具";
    std::wstring win = JWin(v);

    if (name8 == "list_windows") {
        st->kind = 4;
        st->argz = L"列出全部搜索窗口";
        return AgentToolUi(j, UIW_LIST_WINDOWS, tok, L"", L"", L"", 0, 0, st);
    }
    if (name8 == "get_window_state") {
        st->kind = 3;
        st->argz = win.empty() ? L"(当前窗口) 状态与设置" : (win + L" 状态与设置");
        return AgentToolUi(j, UIW_WINDOW_STATE, tok, win, L"", L"", 0, 0, st);
    }
    if (name8 == "set_window_settings") {
        st->kind = 3;
        const Jv* sv = v.Get(L"settings");
        if (!sv || sv->t != 5 || sv->obj.empty()) return L"settings 参数缺失 (须为非空对象)";
        std::vector<AiAdjustItem> items;
        for (auto& kv : sv->obj) {
            AiAdjustItem it;
            it.key = kv.first;
            it.val = AdjustValueText(kv.first, kv.second);
            std::wstring one = L"{\"" + W8(JsonEscapeUtf8(kv.first).c_str()) + L"\":";
            JvAppend(kv.second, &one);
            one += L"}";
            it.json = U8(one);
            items.push_back(std::move(it));
        }
        return ProposeAdjustCard(st, 0, win, std::move(items));
    }
    if (name8 == "get_global_settings") {
        st->kind = 3;
        st->argz = L"读取全局设置";
        return AgentToolUi(j, UIW_GET_GLOBAL, tok, L"", L"", L"", 0, 0, st);
    }
    if (name8 == "set_global_settings") {
        st->kind = 3;
        const Jv* sv = v.Get(L"settings");
        if (!sv || sv->t != 5 || sv->obj.empty()) return L"settings 参数缺失 (须为非空对象)";
        std::vector<AiAdjustItem> items;
        for (auto& kv : sv->obj) {
            AiAdjustItem it;
            it.key = kv.first;
            it.val = AdjustValueText(kv.first, kv.second);
            std::wstring one = L"{\"" + W8(JsonEscapeUtf8(kv.first).c_str()) + L"\":";
            JvAppend(kv.second, &one);
            one += L"}";
            it.json = U8(one);
            items.push_back(std::move(it));
        }
        return ProposeAdjustCard(st, 1, L"", std::move(items));
    }
    if (name8 == "list_skins") {
        st->kind = 8;
        st->argz = L"列出可用皮肤";
        return AgentToolUi(j, UIW_LIST_SKINS, tok, L"", L"", L"", 0, 0, st);
    }
    if (name8 == "get_window_selection") {
        st->kind = 3;
        st->argz = (win.empty() ? L"(当前窗口)" : win) + L" 选中文件";
        long long limit = 200;
        const Jv* lv = v.Get(L"limit");
        if (lv && lv->t == 2 && lv->num >= 1 && lv->num <= 10000) limit = (long long)lv->num;
        std::wstring err = AgentToolUi(j, UIW_WINDOW_SELECTION, tok, win, L"", L"", limit, 0, st);
        if (!err.empty()) return err;
        /* worker 直连引擎补路径/名称 (GetPath/GetName 指针为线程本地缓存, 必须立即拷贝);
           UI 编组只回 FileId (宿主口径: 引擎数据是事实源)。解析不出引擎时原样透传 ID 列表 */
        Jv o = JsonParseW(W8(st->res8.c_str()));
        const Jv* arr = o.Get(L"文件ID");
        xjs_engine* eng = xjs_GetDefaultEngine();
        if (arr == NULL || arr->t != 4 || arr->arr.empty() || !eng) return L"";
        long long total = 0;
        const Jv* tv = o.Get(L"选中数");
        if (tv && tv->t == 2) total = (long long)tv->num;
        std::wstring out = L"{\"窗口名称\":\"" + W8(JsonEscapeUtf8(o.S(L"窗口名称")).c_str()) + L"\"";
        out += L",\"选中数\":" + std::to_wstring(total);
        out += L",\"说明\":\"选中数>返回数时只详列了前若干条, 其余未展开\"";
        out += L",\"文件\":[";
        bool first = true;
        for (auto& f : arr->arr) {
            if (f.t != 2) continue;
            int fid = (int)f.num;
            const char* p = xjs_db_GetPath(eng, fid);
            const char* nm = xjs_db_GetName(eng, fid);
            if (!first) out += L",";
            first = false;
            wchar_t nb[32];
            swprintf(nb, 32, L"%d", fid);
            out += L"{\"ID\":" + std::wstring(nb);
            out += L",\"路径\":" + W8(JsonEscapeUtf8(W8(p ? p : "")).c_str());
            out += L",\"名称\":" + W8(JsonEscapeUtf8(W8(nm ? nm : "")).c_str()) + L"}";
        }
        out += L"]}";
        st->res8 = U8(out);
        return L"";
    }
    if (name8 == "list_languages") {
        st->kind = 8;
        st->argz = L"列出界面语言";
        return AgentToolUi(j, UIW_LIST_LANGS, tok, L"", L"", L"", 0, 0, st);
    }
    if (name8 == "get_language") {
        st->kind = 3;
        st->argz = (win.empty() ? L"(当前窗口)" : win) + L" 界面语言";
        return AgentToolUi(j, UIW_GET_LANGUAGE, tok, win, L"", L"", 0, 0, st);
    }
    if (name8 == "set_language") {
        st->kind = 3;
        std::wstring code = TrimW(v.S(L"language"));
        if (code != L"auto" && code != L"zh" && code != L"zh-TW" && code != L"en" &&
            code != L"ko" && code != L"th" && code != L"ms")
            return L"language 只接受 auto | zh | zh-TW | en | ko | th | ms (list_languages 查母语名)";
        AiAdjustItem it;
        it.key = L"语言";
        Jv lv; lv.t = 3; lv.str = code;
        it.val = AdjustValueText(L"语言", lv);
        std::wstring one = L"{\"语言\":\"" + code + L"\"}";
        it.json = U8(one);
        std::vector<AiAdjustItem> items;
        items.push_back(std::move(it));
        return ProposeAdjustCard(st, 0, win, std::move(items));
    }
    if (name8 == "control_window") {
        st->kind = 4;
        std::wstring act = TrimW(v.S(L"action"));
        if (act != L"show" && act != L"dismiss" && act != L"openSettings")
            return L"action 只接受 show | dismiss | openSettings";
        AiAdjustItem it;
        it.key = act == L"show" ? L"唤起窗口" : (act == L"dismiss" ? L"收起窗口" : L"打开设置窗口");
        it.val = win.empty() ? L"(当前窗口)" : win;
        it.json = U8(act);
        std::vector<AiAdjustItem> items;
        items.push_back(std::move(it));
        return ProposeAdjustCard(st, 2, win, std::move(items));
    }
    if (name8 == "create_window") {
        st->kind = 4;
        std::wstring profile = TrimW(v.S(L"profile"));
        std::wstring inherit = TrimW(v.S(L"inherit"));
        AiAdjustItem it;
        it.key = L"新建窗口";
        it.val = profile.empty() ? L"(空白档案)" : profile;
        it.json = U8(profile + L"|" + inherit);   /* 档案名|继承尺寸的窗名 (应用时拆) */
        std::vector<AiAdjustItem> items;
        items.push_back(std::move(it));
        return ProposeAdjustCard(st, 3, L"", std::move(items));
    }
    if (name8 == "set_search") {
        st->kind = 5;
        std::wstring kw = v.S(L"keyword");
        std::wstring mode = TrimW(v.S(L"mode"));
        if (!mode.empty() && mode != L"wildcard" && mode != L"regex" && mode != L"sql" &&
            mode != L"lua" && mode != L"lua-exec")
            return L"mode 只接受 wildcard | regex | sql | lua | lua-exec";
        st->argz = (win.empty() ? L"(当前窗口)" : win) + L" 搜: " + (kw.empty() ? L"(保持现词)" : kw);
        if (!mode.empty()) st->argz += L" [" + mode + L"]";
        ArgzCut(&st->argz);
        return AgentToolUi(j, UIW_SET_SEARCH, tok, win, kw, mode, JFlagTrue(v, L"execute") ? 1 : 0, 0, st);
    }
    if (name8 == "list_modes") {
        st->kind = 6;
        st->argz = (win.empty() ? L"(当前窗口)" : win) + L" 可用模式";
        return AgentToolUi(j, UIW_LIST_MODES, tok, win, L"", L"", 0, 0, st);
    }
    if (name8 == "apply_mode") {
        st->kind = 6;
        std::wstring mid = TrimW(v.S(L"mode_id"));
        if (mid.empty()) return L"mode_id 不能为空 (list_modes 查\"标识\")";
        std::wstring input = v.S(L"input");
        st->argz = mid + (input.empty() ? L"" : (L" 输入: " + input));
        ArgzCut(&st->argz);
        return AgentToolUi(j, UIW_APPLY_MODE, tok, win, mid, input, 0, 0, st);
    }
    if (name8 == "add_search_mode") {
        st->kind = 6;
        std::wstring mname = TrimW(v.S(L"name"));
        std::wstring mtpl = TrimW(v.S(L"template"));
        std::wstring mtype = TrimW(v.S(L"type"));
        std::wstring mdesc = v.S(L"desc");
        if (mname.empty()) return L"name 不能为空";
        if (mtpl.empty()) return L"template 不能为空";
        if (mtpl.find(L"<keyword>") == std::wstring::npos)
            return L"template 必须包含 <keyword> 占位符 (执行时替换为搜索框输入)";
        if (mtype.empty()) mtype = L"wildcard";
        if (mtype != L"wildcard" && mtype != L"regex" && mtype != L"sql" && mtype != L"lua")
            return L"type 只接受 wildcard | regex | sql | lua";
        Jv d;
        d.t = 5;
        auto put = [&d](const wchar_t* k, const std::wstring& val) {
            Jv x;
            x.t = 3;
            x.str = val;
            d.obj.push_back({ k, x });
        };
        put(L"名称", mname);
        if (!mdesc.empty()) put(L"简介", mdesc);
        put(L"类型", mtype);
        put(L"模板", mtpl);
        std::wstring dj;
        JvAppend(d, &dj);
        st->argz = mname + L" (" + mtype + L")";
        return AgentToolUi(j, UIW_ADD_MODE, tok, dj, L"", L"", 0, 0, st);
    }
    if (name8 == "remove_search_mode") {
        st->kind = 6;
        std::wstring mid = TrimW(v.S(L"mode_id"));
        if (mid.empty()) return L"mode_id 不能为空";
        st->argz = mid;
        return AgentToolUi(j, UIW_REMOVE_MODE, tok, mid, L"", L"", 0, 0, st);
    }
    if (name8 == "list_plugins") {
        st->kind = 7;
        st->argz = L"列出全部插件";
        return AgentToolUi(j, UIW_LIST_PLUGINS, tok, L"", L"", L"", 0, 0, st);
    }
    if (name8 == "send_plugin_message") {
        st->kind = 7;
        std::wstring pid = TrimW(v.S(L"plugin_id"));
        if (pid.empty()) return L"plugin_id 不能为空 (list_plugins 查\"标识\")";
        const Jv* pv = v.Get(L"payload");
        std::wstring pay;
        if (pv && pv->t == 5) JvAppend(*pv, &pay);
        else if (pv && pv->t == 3) pay = pv->str;   /* 字符串载荷原样 (约定为 JSON 文本) */
        else return L"payload 参数缺失 (JSON 对象)";
        st->argz = pid;
        return AgentToolUi(j, UIW_SEND_MSG, tok, pid, pay, L"", 0, 0, st);
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
    } else if (!st.res8.empty()) {
        return st.res8;   /* 代办类: 宿主扩展 API 的结果 JSON 原样回喂 */
    } else {
        j = L"{\"ok\":true}";
    }
    return U8(j);
}

/* 系统提示词 — 按"常驻骨架 + 按需展开"拆分 (提示词防膨胀口径):
 *   常驻 = 角色目标 / 工作方式 / 工具目录(只有名字+一句话, 细节在各工具 description 里) /
 *          跨工具规则 / 可点击输出 / 语法速查 / Lua 速查 — 全是"每轮都要用"的行为契约。
 *   按需 = 引擎内嵌的两份 Lua 规范全文 (体量最大且只有写脚本时才用) 不再拼进每轮请求,
 *          改经 get_lua_spec 工具由模型在写 lua_filter/lua_exec 前自取 (工具结果纯文本回喂)。
 *   新增工具一律: 加 tools JSON (描述里写全参数语义) + 工具目录加一行; 别再往这里堆细节。 */
static const wchar_t* AI_INSTRUCTIONS =
    L"## 角色与目标\n"
    L"你是“蜗牛快搜”内置的 AI 助手(agent 模式)。蜗牛快搜是 Windows 本地文件极速搜索工具：全盘秒级索引，"
    L"支持文件名/大小/时间/类型/别名/内容等搜索。全程用简体中文回答（包括所有面向用户的文字），简洁、准确、直接。\n"
    L"\n"
    L"## 工作方式\n"
    L"- 找文件/查文件/统计类任务：**先调用 run_search 实际执行搜索**，根据返回的条数/样本/ai.print 输出判断结果，"
    L"不符合就换口径再搜（先粗筛再精筛），绝不凭空编造路径；需要给用户看文件用 open_file，给路径清单用 copy_paths。\n"
    L"- 统计类必须**自己跑到出数**再回答（用户要的是数字与结论，不是脚本）；只有确实多次失败，"
    L"才把可粘贴的搜索式/脚本交给用户并说明卡在哪一步。\n"
    L"- 改设置/换皮肤/切语言/窗口管理：用对应代办工具提交——这类改动**不会直接生效**，而是列进\"待应用的调整\"卡片，"
    L"用户逐项点\"应用\"才执行。提交后用一句话请用户到卡片上确认，**绝不宣称已生效**，"
    L"**用户没让改就不要替用户提交任何调整**。搜索模式管理与任务类操作（搜索/打开文件/复制路径）仍直接执行。\n"
    L"- 工具调用轮数有限，别在一种写法上反复试错：同一口径连续两次拿不到有效数据，立即换搜索模式（或先取 Lua 规范）。\n"
    L"- 得到足够信息后用最终答复总结：找到什么、在哪、关键数据；推荐执行的搜索用 xjs:// 搜索链接给出（见《可点击输出》）。\n"
    L"- 与任务无关的问题直接回答，不要调用工具。\n"
    L"\n"
    L"## 工具目录（只有名字与一句话；参数细节看各工具的 description，用前先读）\n"
    L"- get_lua_spec：取 Lua 脚本规范全文（默认返回合集，取一次即可写两种模式的脚本）——"
    L"**写 lua_filter/lua_exec 脚本前必须先取**，脚本报错后也先重读规范再改。\n"
    L"- run_search：引擎内执行一次搜索（5 种模式，语法见《搜索语法速查》；Lua 统计数字经 ai.print 回传）。\n"
    L"- open_file / copy_paths：把搜索样本中的文件打开/定位给用户看 / 复制路径清单到剪贴板。\n"
    L"- get_author_and_donate：关于作者/软件背景的权威介绍；用户想捐赠/赞赏时也用它取二维码引用（竖排显示在对话页）。\n"
    L"- list_windows / get_window_state / set_window_settings / control_window / create_window：窗口查看与代办（改动经\"待应用的调整\"卡片，用户点应用才生效）。\n"
    L"- get_global_settings / set_global_settings / list_skins：全局设置读写（改经卡片）/ 皮肤名清单。\n"
    L"- get_window_selection：读某窗口当前选中的文件 (ID/路径/名称)。用户指\"选中的/这些文件\"要做判断、统计或批量操作建议时用它。\n"
    L"- list_languages / get_language / set_language：界面语言清单 / 查询 / 切换（代码 auto|zh|zh-TW|en|ko|th|ms，切换经卡片，用户点应用才生效）。\n"
    L"- set_search：把关键词置入用户窗口的搜索框并执行（run_search 是你的私有搜索，不动用户界面）。\n"
    L"- list_modes / apply_mode / add_search_mode / remove_search_mode：搜索模式查看/执行/增删。\n"
    L"- list_plugins / send_plugin_message：发现其它插件 / 与它们互发 JSON 消息。\n"
    L"\n"
    L"## 代办工具跨条规则\n"
    L"- window 参数一律填窗口名称（list_windows 查），留空 = 当前对话所在的窗口。\n"
    L"- 换皮肤先 list_skins 拿有效名；settings 里的搜索模式值是 lua|lua-exec（与 run_search 的 lua_filter|lua_exec 不同名，别混）。\n"
    L"- control_window 的 dismiss：主窗=藏托盘（不是退出），子窗=真关闭；create_window 在索引扫描期会被拒绝，让用户稍后再试。\n"
    L"- send_plugin_message 需对方插件已启用并实现收信口；对方无回复属正常（载荷约定取决于对方插件）。\n"
    L"\n"
    L"## 回答格式（GFM）\n"
    L"- 一律用 GFM（GitHub Flavored Markdown）输出：**表格前空一行**、表头下一行是 |---|---| 分隔行、表格后再空一行；\n"
    L"  支持删除线 ~~…~~、任务列表 - [x]、``` 围栏代码块；不要输出 HTML 标签（渲染层会丢弃）。\n"
    L"\n"
    L"## 可点击输出（你的回答会被渲染成交互界面）\n"
    L"你的回答里有四种元素会被渲染成用户可直接点击的控件。前两种一律用**标准 Markdown 链接语法**，\n"
    L"链接文字写成给用户看的动作指引（不要裸放搜索词/路径当文字）：\n"
    L"1. 搜索指令：[链接文字](xjs://search?text=<URL编码的搜索词>&mode=<wildcard|regex|sql|lua|lua-exec>)。\n"
    L"   用户点击即把搜索词置入搜索框并按该模式执行。mode 可省略 = 沿用窗口当前模式；\n"
    L"   **mode 只接受 wildcard|regex|sql|lua|lua-exec 五个值**（lua=逐文件过滤的 Lua 脚本；\n"
    L"   lua-exec=执行模式，脚本自主遍历 db 表并必须 return ID 数组——跨文件聚合/统计/自定义排序用这个）；\n"
    L"   lua_filter/lua_exec 是 run_search 工具的枚举值：lua_filter 链接里写 lua、lua_exec 链接里写 lua-exec。\n"
    L"   text 的值要 URL 编码（空格=%20，&=%26，#=%23；中文/通配符可直接写）。\n"
    L"   示例：[🔍 搜索所有 PNG 图片](xjs://search?text=*.png&mode=wildcard)、"
    L"[🔍 找出大于 100MB 的视频](xjs://search?text=SELECT%20Path%20FROM%20alltable%20WHERE%20Size%20%3E%20'100M'&mode=sql)。\n"
    L"   凡要给\"可执行的搜索式\"一律用这种链接；链接里的 lua 脚本写成**单行紧凑形式**（语句用分号衔接，\n"
    L"   不用 -- 行注释——搜索框是单行显示）；过长塞不进链接的脚本改用普通代码块给出。\n"
    L"2. 文件动作：[打开 xxx](xjs://open?path=<完整路径>)、[在资源管理器中定位 xxx](xjs://reveal?path=<完整路径>)。\n"
    L"   path 的值同样要 URL 编码（空格=%20、&=%26、括号最好也编码=%28 %29）。表格清单里链接文字用文件名即可，\n"
    L"   完整路径放进 path（悬停可见），别把几百字符的整条路径铺在表格里。\n"
    L"3. 路径是精确数据：必须**逐字复制 run_search 返回的原文**（盘符/空格/括号/间隔点/扩展名一个字符都不能变），\n"
    L"   绝不凭印象改写、意译或补全——差一个字符，用户点击就打不开。链接文字照抄原文件名，不要自造名称。\n"
    L"4. 直接写出完整绝对路径（含盘符）也会自动渲染为可点击链接：单击=打开，右键=打开/定位/复制路径。\n"
    L"5. 网页链接照常 [标题](https://...)，点击用系统浏览器打开。\n"
    L"\n"
    L"## 搜索语法速查（run_search 的 mode）\n"
    L"- wildcard 通配符（日常默认）：* 任意长度、? 单个字符；不含 * ? 时自动按包含匹配；支持拼音首拼/全拼（wd 命中 文档.docx）；"
    L"空格=且，|=或；搜索词含 \\\\ 或 / 时按完整路径匹配。\n"
    L"- regex 正则（PCRE2）：如 ^[0-9]{4}-报告.*\\.docx$。\n"
    L"- sql（功能最强）：SELECT Path FROM alltable WHERE Size > '100M' AND ModTime > NOW() - INTERVAL '7 days' ORDER BY Size DESC LIMIT 100；"
    L"支持 LIKE/ILIKE/~/GROUP BY/COUNT/CASE WHEN/CTE 等；尺寸简写 '100M'；常用字段：ID、Path、FName、Ext、Size、CreateTime、ModTime、"
    L"AccessTime、FileType、IsDir（1=目录 0=文件）、Alias、Score、FileContent（不区分大小写）；不支持窗口函数(OVER)/多表 FROM/EXISTS/DDL。\n"
    L"- lua_filter 过滤模式：对每个文件做一次真值判断的 Lua 脚本；**每个文件一条线程并发求值，文件间顺序不定**——"
    L"脚本必须无状态，聚合统计一律换 lua_exec。\n"
    L"- lua_exec 执行模式：脚本全权遍历数据库/跨文件聚合/自定义排序，return 的 ID 数组即结果。\n"
    L"选择建议：找名字用 wildcard/regex；按字段组合筛选用 sql；逐文件自定义判断用 lua_filter；"
    L"**计数/分组/排名/占比等一切统计类问题直接用 lua_exec**（统计数字经 ai.print 拿回来）。\n"
    L"- SQL 做不了统计：聚合数值经工具通道拿不到（COUNT(*) 只回 1 行，数值本身不回传），GROUP BY 形态受限、不支持子查询/CASE。\n"
    L"- SQL 的 LIKE 里 \\ 是转义字符，`Path LIKE 'C:\\%'` 实测匹配 0 条；含 \\ 的路径前缀筛选改用 lua_exec（f.fpath() 判断前缀）。\n"
    L"\n"
    L"## 文件与目录必须区分（分析口径）\n"
    L"索引同时收录**文件和目录（文件夹/盘符）**，搜索结果默认两类混排，count 与 top 样本都是混合口径。"
    L"分析时必须分清对象究竟是文件还是目录，禁止把目录当文件、把文件当目录：\n"
    L"- 类型只能靠字段判断：SQL 用 `IsDir`，Lua 用 `f.isdir()`。**不要凭后缀或路径长相猜**——目录名可以带点，"
    L"无后缀的路径不一定是目录；top 样本只有路径字符串，本身不带类型标志。\n"
    L"- 用户问\"文件\"=必须排除目录：SQL 加 `AND IsDir=0`；lua_filter 脚本开头 `if f.isdir() then return false end`；"
    L"lua_exec 统计时按 `f.isdir()` 把文件/目录分开计数。\n"
    L"- 用户问\"文件夹/目录\"=只算目录：SQL `IsDir=1`，Lua `f.isdir()`。\n"
    L"- 用户没指明类型（如\"这里有多少东西/占多大空间\"）时，把文件与目录分开说明或注明口径，别混作一个数。\n"
    L"- wildcard/regex 不能按类型过滤，其 count 是文件+目录混算，**不能直接当\"文件数/文件清单\"回答**；要文件口径就换 sql 或 lua 重查。\n"
    L"- 统计文件数/总大小/最大文件一律先排除目录（IsDir=0），目录条目不参与\"文件\"的计数与求和；"
    L"给用户的清单里目录行要标明是目录（如 📁 前缀），不要一律写成\"文件\"。\n"
    L"\n"
    L"## Lua 脚本速查（全文规范用 get_lua_spec 取合集，写脚本前必读规范）\n"
    L"- db 表只有 count/ids/files/get 四个成员，没有 db.ext/db.isdir/db.size 之类的快捷函数——"
    L"取文件属性必须先 `local f = db.get(id)` 再 f.ext()/f.isdir()/f.size()/f.fpath()（虚构 API 脚本必报错）。\n"
    L"- 过程输出：引擎规范里\"数据走 print\"的说法对你不适用（print 进引擎调试输出，工具结果拿不到）；"
    L"你的工具环境注册了 **ai.print(...)**（与 print 同款多参数，可多次调用，参数可为字符串/数字/表），"
    L"输出会作为本次工具结果 JSON 的 output 字段原样回传——统计数字/逐目录计数/过程日志一律经它；"
    L"return 仍按规范（执行模式=ID 数组，过滤模式=逐文件真值）。\n"
    L"- 脚本沙箱删除了 io/os 等库；API 全集以 get_lua_spec 返回的规范为准，绝不虚构函数。\n"
    L"- **交给用户运行的脚本**（写在回答里的代码块或 xjs://search 链接，不经你执行）：必须写 return ID 数组"
    L"（漏写 return 界面一条结果都不显示）；**禁止调用 ai.print**（用户侧没有这个函数，调用即报错）；"
    L"过滤模式脚本没有用户侧入口（搜索框的 lua 模式就是执行模式），别生成 lua_filter 的搜索链接；"
    L"把脚本交给用户仅限用户明确要脚本本身的场合。\n";

/* 工具定义 (Responses API tools 数组; 与 AgentToolExec 的名字/参数一一对应) */
static const char* AI_TOOLS_JSON = R"json([
  {"type":"function","name":"run_search","description":"在蜗牛快搜索引中执行一次搜索, 返回命中总数与前 20 条路径样本。结果同时含文件与目录(文件夹), count/top 均为混合口径: 涉及\"文件\"口径的分析必须先按 IsDir=0 / f.isdir() 过滤, 不得拿混合 count 当文件数。可多次调用逐步逼近目标 (先粗筛再精筛)。5 种 mode 的搜索词语法以系统提示词中的说明为准; lua 两种模式写脚本前先调 get_lua_spec 取规范。Lua 模式脚本内用 ai.print(...) 输出的统计/过程信息附在结果 JSON 的 output 字段。","parameters":{"type":"object","properties":{"mode":{"type":"string","enum":["wildcard","regex","sql","lua_filter","lua_exec"],"description":"wildcard=通配符 regex=PCRE2正则 sql=SELECT语句 lua_filter=过滤模式(Lua 逐文件判断) lua_exec=执行模式(Lua 程序接管搜索)"},"query":{"type":"string","description":"搜索词/脚本全文 (lua 两种模式传完整脚本文本)"}},"required":["mode","query"]}},
  {"type":"function","name":"get_lua_spec","description":"获取引擎内嵌的 Lua 脚本规范全文 (纯文本)。写 lua_filter 或 lua_exec 脚本前必须先取, 脚本报错时也先重读规范再修改 — 规范里有全部可用 API 与硬性规则, 绝不虚构函数。默认返回合集 (两种模式合并去重版, 取一次即可写两种模式的脚本); 仅当引擎没有合集时才需要用 mode 单取一份。","parameters":{"type":"object","properties":{"mode":{"type":"string","enum":["lua_filter","lua_exec"],"description":"仅引擎无合集时才需要: 单取哪一份规范"}},"required":[]}},
  {"type":"function","name":"get_author_and_donate","description":"关于作者/软件背景的问题 (作者是谁/这是什么软件/授权与特性), 或用户想捐赠/赞赏/请作者喝咖啡时调用。返回软件与授权的权威介绍 (据此回答, 不编造) 与捐赠二维码的引用方式: 在回答正文里用图片语法 ![微信捐赠码](xjs://donate?kind=wechat) / ![支付宝捐赠码](xjs://donate?kind=alipay), 二维码竖排显示在对话页 (微信优先放最前)。只引用返回中列出的可用项; 图片本体不经过对话文本, 不要把 base64/文件路径写进回答。","parameters":{"type":"object","properties":{},"required":[]}},
  {"type":"function","name":"open_file","description":"打开最近一次 run_search 样本列表中的某个文件 (在用户屏幕上打开/定位), 用于让用户直接看到该文件。","parameters":{"type":"object","properties":{"index":{"type":"integer","description":"样本列表序号 (1 起)"},"reveal":{"type":"boolean","description":"true=只在资源管理器中定位, 不打开"}},"required":["index"]}},
  {"type":"function","name":"copy_paths","description":"把最近一次 run_search 的前 100 条完整路径 (每行一条) 复制到剪贴板, 供用户粘贴。","parameters":{"type":"object","properties":{},"required":[]}},
  {"type":"function","name":"list_windows","description":"列出当前全部搜索窗口 (令牌/名称/是否主窗/档案槽)。其它代办工具的 window 参数都填这里的\"名称\"。","parameters":{"type":"object","properties":{},"required":[]}},
  {"type":"function","name":"get_window_state","description":"查看一个搜索窗口的完整状态与设置 (视图/页面缩放/皮肤/预览/预览宽度/置顶/搜索模式/搜索词/结果数/选中数/失焦行为/显示开关/任务栏图标/鼠标打开/默认选中/窗口矩形等)。","parameters":{"type":"object","properties":{"window":{"type":"string","description":"窗口名称 (list_windows 查; 留空=当前对话所在窗口)"}},"required":[]}},
  {"type":"function","name":"set_window_settings","description":"提交对一个搜索窗口的设置修改。**不会直接生效**: 每个键列成\"待应用的调整\"卡片, 用户点\"应用\"才逐项执行 (可忽略)。settings 对象的键全部可选但必须合法, 一个未知键/非法值在应用时该键失败: 视图=list|details|medium|large; 页面缩放=50~200(百分数); 皮肤=皮肤名(先 list_skins 查); 预览=布尔; 预览宽度=160~2000; 置顶=布尔; 失焦行为=0(无)|1(失焦关闭窗口); 显示控制按钮/显示筛选框/显示状态栏/任务栏图标=布尔; 鼠标打开=0(双击)|1(单击); 默认选中=0(不选)|1(自动选第一个); 搜索模式=wildcard|regex|sql|lua|lua-exec; 语言=auto|zh|zh-TW|en|ko|th|ms。","parameters":{"type":"object","properties":{"window":{"type":"string","description":"窗口名称 (留空=当前对话所在窗口)"},"settings":{"type":"object","description":"要修改的设置键值对 (子集随意)"}},"required":["settings"]}},
  {"type":"function","name":"get_global_settings","description":"读取全局设置 (双击Ctrl目标/绘制引擎)。","parameters":{"type":"object","properties":{},"required":[]}},
  {"type":"function","name":"set_global_settings","description":"提交对全局设置的修改。**不会直接生效**: 列成\"待应用的调整\"卡片, 用户点\"应用\"才逐项执行。键: 双击Ctrl目标=\"\"(禁用)|\"默认窗口\"|档案名; 绘制引擎=\"d2d\"|\"gdiplus\"(应用后重启生效)。","parameters":{"type":"object","properties":{"settings":{"type":"object","description":"要修改的全局设置键值对"}},"required":["settings"]}},
  {"type":"function","name":"list_skins","description":"列出全部可用皮肤名 (set_window_settings 的\"皮肤\"键只接受这些名字)。","parameters":{"type":"object","properties":{},"required":[]}},
  {"type":"function","name":"get_window_selection","description":"读取一个搜索窗口当前选中的文件清单 (引擎 FileId + 完整路径 + 文件名)。用户说\"我选中的这些/当前选中的文件\"要做判断、统计或给出批量操作建议时调用; 没有选中时选中数为 0。","parameters":{"type":"object","properties":{"window":{"type":"string","description":"窗口名称 (留空=当前对话所在窗口)"},"limit":{"type":"integer","description":"最多详列多少条 (默认 200; 选中数为全量, 超出部分不展开)"}},"required":[]}},
  {"type":"function","name":"list_languages","description":"列出全部可用界面语言 (代码 + 母语名称)。set_language 的 language 参数只接受这些代码 (另加 auto=跟随系统)。","parameters":{"type":"object","properties":{},"required":[]}},
  {"type":"function","name":"get_language","description":"查询一个搜索窗口当前的界面语言设置 (语言代码; auto=跟随系统)。","parameters":{"type":"object","properties":{"window":{"type":"string","description":"窗口名称 (留空=当前对话所在窗口)"}},"required":[]}},
  {"type":"function","name":"set_language","description":"提交切换一个搜索窗口的界面语言。**不会直接生效**: 列成\"待应用的调整\"卡片, 用户点\"应用\"才切换 (应用后所有窗口标题各自按新语言刷新并落盘)。语言代码先 list_languages 查 (用户说的是\"中文/英文/泰语\"这类母语名, 映射成代码再调)。","parameters":{"type":"object","properties":{"window":{"type":"string","description":"窗口名称 (留空=当前对话所在窗口)"},"language":{"type":"string","enum":["auto","zh","zh-TW","en","ko","th","ms"],"description":"语言代码 (auto=跟随系统)"}},"required":["language"]}},
  {"type":"function","name":"control_window","description":"提交对一个搜索窗口的界面动作。**不会直接生效**: 列成\"待应用的调整\"卡片, 用户点\"应用\"才执行。show=唤起到前台; dismiss=窗口消失 (主窗藏托盘/子窗真关闭); openSettings=打开设置窗口并绑定该窗口。","parameters":{"type":"object","properties":{"window":{"type":"string","description":"窗口名称 (留空=当前对话所在窗口)"},"action":{"type":"string","enum":["show","dismiss","openSettings"],"description":"界面动作"}},"required":["action"]}},
  {"type":"function","name":"create_window","description":"提交一个创建搜索窗口的提案。**不会直接生效**: 列成\"待应用的调整\"卡片, 用户点\"应用\"才创建 (该档案已打开则只激活; 索引扫描期应用会失败)。","parameters":{"type":"object","properties":{"profile":{"type":"string","description":"窗口档案名 (留空=新建空白档案)"},"inherit":{"type":"string","description":"继承尺寸的窗口名称 (留空=默认窗口)"}},"required":[]}},
  {"type":"function","name":"set_search","description":"把关键词置入用户搜索窗口的搜索框并执行搜索 (用户立即可见)。与 run_search 的区别: run_search 是你私有的搜索, 不动用户界面; 要把某个搜索放进用户的窗口时用它。","parameters":{"type":"object","properties":{"window":{"type":"string","description":"窗口名称 (留空=当前对话所在窗口)"},"keyword":{"type":"string","description":"搜索词 (留空=保持现词)"},"mode":{"type":"string","enum":["wildcard","regex","sql","lua","lua-exec"],"description":"搜索模式 (留空=沿用窗口当前模式)"},"execute":{"type":"boolean","description":"false=只填词不搜索 (默认 true=立即搜索)"}},"required":[]}},
  {"type":"function","name":"list_modes","description":"列出一个窗口可用的全部搜索模式 (用户自定义+插件提供), 含标识/名称/类型/模板/来源。","parameters":{"type":"object","properties":{"window":{"type":"string","description":"窗口名称 (留空=当前对话所在窗口)"}},"required":[]}},
  {"type":"function","name":"apply_mode","description":"按搜索模式执行搜索 (语义=用户在药丸菜单点了该模式): 模板型把 input 置入搜索框转标签链执行; 插件接管型触发该插件。","parameters":{"type":"object","properties":{"window":{"type":"string","description":"窗口名称 (留空=当前对话所在窗口)"},"mode_id":{"type":"string","description":"模式标识 (list_modes 返回的\"标识\")"},"input":{"type":"string","description":"输入词 (留空=窗口现词)"}},"required":["mode_id"]}},
  {"type":"function","name":"add_search_mode","description":"添加一个会话级模板型搜索模式 (本次运行内有效, 重启后消失; 返回其\"标识\")。template 必须含 <keyword> 占位符, 执行时替换为搜索框输入文字。","parameters":{"type":"object","properties":{"name":{"type":"string","description":"模式名 (≤64字)"},"type":{"type":"string","enum":["wildcard","regex","sql","lua"],"description":"模式类型 (默认 wildcard)"},"template":{"type":"string","description":"模板, 必须含 <keyword> 占位符, 如 FileContent LIKE '%<keyword>%'"},"desc":{"type":"string","description":"简介 (≤256字)"}},"required":["name","template"]}},
  {"type":"function","name":"remove_search_mode","description":"删除你自己经 add_search_mode 添加的运行时搜索模式 (用户自定义/清单声明的模式删不了)。","parameters":{"type":"object","properties":{"mode_id":{"type":"string","description":"add_search_mode 返回的\"标识\""}},"required":["mode_id"]}},
  {"type":"function","name":"list_plugins","description":"列出全部已扫描插件 (标识/名称/版本/作者/启用/已加载)。","parameters":{"type":"object","properties":{},"required":[]}},
  {"type":"function","name":"send_plugin_message","description":"向另一个插件发送 JSON 消息并等它的同步回复 (消息经宿主中转; 对方需已启用并实现收信口, 载荷结构约定看对方插件)。","parameters":{"type":"object","properties":{"plugin_id":{"type":"string","description":"目标插件标识 (list_plugins 查)"},"payload":{"type":"object","description":"消息载荷 (JSON 对象)"}},"required":["plugin_id","payload"]}}
])json";

/* 系统提示词组装 (进程一次): 骨架已含全部常驻内容; Lua 两份规范不再拼入 —
   改经 get_lua_spec 工具按需取 (见 AI_INSTRUCTIONS 头注释的拆分口径) */
static std::string g_instrA;
void BuildInstructions() {
    g_instrA = U8(AI_INSTRUCTIONS);
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
    s += L"- 代办工具的 window 参数留空 = 作用于当前对话所在的窗口。\n";
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
                    dst.argz = local.argz;
                    dst.count = local.count;
                    dst.elapsedMs = local.elapsedMs;
                    dst.emit = local.emit;
                    dst.err = local.err;
                    dst.top = local.top;   /* open 展开态归泵/用户, 不覆盖 */
                    dst.adj = local.adj;   /* 待应用的调整 (提案数据; 漏拷 = 卡片按钮区不渲染) */
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

