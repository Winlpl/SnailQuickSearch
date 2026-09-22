/*
 * ai_plugin.cpp — 插件边界: GetInfo / Init / Shutdown / OnCommand / OnPanelEvent。
 * 宿主全局指针定义也在此 (Init 时存下, 指针终身有效)。
 */
#include "ai_assistant.h"

const XjsPluginHost* g_host = NULL;
XjsPluginCtx*        g_ctx  = NULL;
ULONG_PTR            g_gdipToken = 0;
unsigned             g_uiThread = 0;

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "winhttp.lib")

/* ==================== 插件导出面 ==================== */

static const XjsPluginInfo* XJS_PLUGIN_CALL XjsPlugin_GetInfo(void) {
    static const XjsPluginInfo info = { XJS_PLUGIN_ABI_VERSION, sizeof(XjsPluginInfo), "ai-assistant", "1.1.0" };
    return &info;
}

static int XJS_PLUGIN_CALL XjsPlugin_Init(XjsPluginCtx* ctx, const XjsPluginHost* host) {
    g_ctx = ctx;
    g_host = host;
    g_uiThread = GetCurrentThreadId();
    if (!HOST_PANEL_OK) return XJS_PLUGIN_ERR_FAIL;   /* 旧宿主 (无 v4 Panel* 表) = 干净失败 */
    AgentToolInit();
    BuildInstructions();   /* 系统提示词 = 角色说明 + 引擎内嵌 Lua 两规范 (进程一次) */
    Gdiplus::GdiplusStartupInput si;
    if (GdiplusStartup(&g_gdipToken, &si, NULL) != Gdiplus::Ok) return XJS_PLUGIN_ERR_FAIL;
    if (!AiMsgWndCreate()) return XJS_PLUGIN_ERR_FAIL;
    SetTimer(g_msgwnd, 1, 530, NULL);   /* 光标闪烁节拍 (仅聚焦会话重渲染) */
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
    if (g_msgwnd) KillTimer(g_msgwnd, 1);
    AbortAndJoinAll(2000);   /* SDK 契约: Shutdown 时线程必须已收尾 (宽限 2 秒) */
    AgentToolShutdown();     /* 在途工具线程已 join, 结果对象安全销毁 */
    if (g_msgwnd) { DestroyWindow(g_msgwnd); g_msgwnd = NULL; }
    if (g_gdipToken) { Gdiplus::GdiplusShutdown(g_gdipToken); g_gdipToken = 0; }
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
            RenderDeliver(s);
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
            s->layDirty = true;
            RenderDeliver(s);
            return;
        }
        case XJS_PANEL_MOUSE_MOVE:
        case XJS_PANEL_LDOWN:
        case XJS_PANEL_LUP:
        case XJS_PANEL_RDOWN:
        case XJS_PANEL_RUP:
        case XJS_PANEL_DBLCLK: {
            AiSess* s = SessByTok(window);
            if (s) PanelMouse(s, ev->type, (float)ev->x, (float)ev->y, ev->flags);
            return;
        }
        case XJS_PANEL_WHEEL: {
            AiSess* s = SessByTok(window);
            if (s) PanelWheel(s, (float)ev->x, (float)ev->y, ev->delta);
            return;
        }
        case XJS_PANEL_KEY_DOWN: {
            AiSess* s = SessByTok(window);
            if (s) PanelKey(s, (unsigned)ev->delta, ev->flags);
            return;
        }
        case XJS_PANEL_KEY_CHAR: {
            AiSess* s = SessByTok(window);
            if (s) PanelChar(s, ev->ch);
            return;
        }
        case XJS_PANEL_FOCUS: {
            AiSess* s = SessByTok(window);
            if (s && s->winActive != (ev->delta != 0)) {
                s->winActive = ev->delta != 0;
                RenderDeliver(s);
            }
            return;
        }
        case XJS_PANEL_CAPTURE_LOST: {
            AiSess* s = SessByTok(window);
            if (s) { s->press = HIT_NONE; s->pressGrab = 0; }
            return;
        }
        case XJS_PANEL_KEY_BLUR: {
            /* 宿主收回键盘让渡 (用户点了面板以外的宿主 UI): 字段失焦熄光标,
               不用回发 PanelSetFocus(0) — 宿主已自行清闸 */
            AiSess* s = SessByTok(window);
            if (s && (s->inputFocus || s->dFocus)) {
                s->inputFocus = false;
                s->dFocus = 0;
                RenderDeliver(s);
            }
            return;
        }
        default:
            return;
    }
}

/* 固定导出: OnHostGone (引擎尚未销毁的最后通知; 只收线程, GDI+/窗口留给随后的 Shutdown) */
extern "C" __declspec(dllexport) void XJS_PLUGIN_CALL XjsPlugin_OnHostGone(XjsPluginCtx* ctx) {
    (void)ctx;
    AbortAndJoinAll(2000);
}







