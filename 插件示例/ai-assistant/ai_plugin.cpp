/*
 * ai_plugin.cpp — 插件边界: GetInfo / Init / Shutdown / OnCommand / OnPanelEvent / OnEvent。
 * 宿主全局指针定义也在此 (Init 时存下, 指针终身有效)。
 * 面板接管口径 (WebView2 版): 插件建真子窗口盖住面板内容区, 输入/渲染归浏览器 —
 * 宿主只发 OPEN/RESIZE/CLOSE 三种事件 (鼠标/键盘/IME 转发与位图交付均已不适用)。
 */
#include "ai_assistant.h"

const XjsPluginHost* g_host = NULL;
XjsPluginCtx*        g_ctx  = NULL;
unsigned             g_uiThread = 0;

#pragma comment(lib, "winhttp.lib")

/* ==================== 插件导出面 ==================== */

static const XjsPluginInfo* XJS_PLUGIN_CALL XjsPlugin_GetInfo(void) {
    static const XjsPluginInfo info = { XJS_PLUGIN_ABI_VERSION, sizeof(XjsPluginInfo), "ai-assistant", "2.0.0" };
    return &info;
}

static int XJS_PLUGIN_CALL XjsPlugin_Init(XjsPluginCtx* ctx, const XjsPluginHost* host) {
    g_ctx = ctx;
    g_host = host;
    g_uiThread = GetCurrentThreadId();
    if (!HOST_PANEL_OK) return XJS_PLUGIN_ERR_FAIL;   /* 旧宿主 (无 v4 Panel* 表) = 干净失败 */
    AgentToolInit();
    BuildInstructions();   /* 系统提示词 = 角色说明 + 引擎内嵌 Lua 两规范 (进程一次) */
    WebInit();             /* 子窗口类注册 */
    if (!AiMsgWndCreate()) return XJS_PLUGIN_ERR_FAIL;
    if (g_host->Subscribe) g_host->Subscribe(g_ctx, XJS_PLUGIN_EVT_SKIN);   /* 换肤 → 重推调色 */
    return XJS_PLUGIN_OK;
}

/* 在途/孤儿作业统一收尾 (abort → 并发关句柄打断读 → 宽限内 join) */
static void AbortAndJoinAll(ULONGLONG graceMs) {
    std::vector<AiJob*> all = s_orphans;
    s_orphans.clear();
    for (auto& s : g_sess)
        if (s.inUse && s.job) { all.push_back(s.job); s.job = NULL; }
    ULONGLONG deadline = GetTickCount64() + graceMs;
    for (AiJob* j : all) {
        InterlockedExchange(&j->abort, 1);
        EnterCriticalSection(&j->cs);
        if (j->hReq) { WinHttpCloseHandle(j->hReq); j->hReq = NULL; }
        LeaveCriticalSection(&j->cs);
    }
    for (AiJob* j : all) {
        if (j->th) {
            while (j->th->joinable()) {
                EnterCriticalSection(&j->cs);
                bool done = j->state != 0;
                LeaveCriticalSection(&j->cs);
                if (done) { j->th->join(); break; }
                if (GetTickCount64() > deadline) { j->th->detach(); break; }   /* 宽限到点放弃 (进程将退) */
                Sleep(30);
            }
            delete j->th;
            j->th = NULL;
        }
        delete j;
    }
}

static void XJS_PLUGIN_CALL XjsPlugin_Shutdown(XjsPluginCtx* ctx) {
    (void)ctx;
    static bool s_fini = false;
    if (s_fini) return;
    s_fini = true;
    AbortAndJoinAll(2000);   /* SDK 契约: Shutdown 时线程必须已收尾 (宽限 2 秒) */
    AgentToolShutdown();     /* 在途工具线程已 join, 结果对象安全销毁 */
    for (auto& s : g_sess)
        if (s.inUse) SessClose(&s);   /* 保存会话 + 收 WebView2 控制器/子窗 */
    WebShutdown();           /* 环境释放 (进程尾, 无人再用) */
    if (g_msgwnd) { DestroyWindow(g_msgwnd); g_msgwnd = NULL; }
}

static void XJS_PLUGIN_CALL XjsPlugin_OnCommand(XjsPluginCtx* ctx, const char* cmdIdUtf8,
                                                XjsWindowToken window, const int* fileIds, int count, int kind) {
    (void)ctx; (void)fileIds; (void)count; (void)kind;
    if (!g_host || !cmdIdUtf8) return;
    if (strcmp(cmdIdUtf8, "open-panel") == 0)
        g_host->PanelOpen(g_ctx, window);   /* 状态栏"AI 助手" → 接管预览面板 (OPEN 事件随后送达) */
}

static void XJS_PLUGIN_CALL XjsPlugin_OnPanelEvent(XjsPluginCtx* ctx, XjsWindowToken window,
                                                   const XjsPanelEvent* ev) {
    (void)ctx;
    if (!ev || ev->structSize < sizeof(XjsPanelEvent)) return;
    switch (ev->type) {
        case XJS_PANEL_OPEN: {
            AiSess* s = SessByTok(window);
            if (!s) s = SessFree();
            if (!s) return;
            SessOpen(s, window, ev->serial, ev->w, ev->h, ev->scale);
            return;
        }
        case XJS_PANEL_CLOSE: {
            AiSess* s = SessByTok(window);
            if (s) SessClose(s);
            return;
        }
        case XJS_PANEL_RESIZE: {
            AiSess* s = SessByTok(window);
            if (!s) return;
            s->serial = ev->serial;
            s->w = ev->w;
            s->h = ev->h;
            s->scale = ev->scale > 0 ? ev->scale : 1.0f;
            WebSessionRect(s);   /* 子窗口重定位 + 页面缩放档对齐 */
            return;
        }
        /* 鼠标/滚轮/键盘/IME/焦点转发与捕获丢失: WebView2 子窗口自带完整输入体系, 一律忽略 */
        default:
            return;
    }
}

/* 事件订阅: 皮肤变化 → 会话重取皮肤并重推调色 (前端 CSS 变量即时跟随) */
static void XJS_PLUGIN_CALL XjsPlugin_OnEvent(XjsPluginCtx* ctx, int eventType,
                                              XjsWindowToken window, void* result) {
    (void)ctx; (void)result;
    if (eventType != XJS_PLUGIN_EVT_SKIN) return;
    if (window == 0) {   /* 进程级广播: 全部活跃会话重取 */
        for (auto& s : g_sess)
            if (s.inUse && s.web) { SessLoadSkinOf(&s); WebPushSkin(&s); }
        return;
    }
    AiSess* s = SessByTok(window);
    if (s && s->web) {
        SessLoadSkinOf(s);
        WebPushSkin(s);
    }
}

/* 固定导出: OnHostGone (引擎尚未销毁的最后通知; 只收线程, 窗口/环境留给随后的 Shutdown) */
extern "C" __declspec(dllexport) void XJS_PLUGIN_CALL XjsPlugin_OnHostGone(XjsPluginCtx* ctx) {
    (void)ctx;
    AbortAndJoinAll(2000);
}
