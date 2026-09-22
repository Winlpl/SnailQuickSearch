/*
 * xjs_plugin.cpp — 原生插件系统宿主端 (注册表/扫描/加载/闸门/票号/宿主 API)
 * 设计稿: 插件系统设计.md (2026-09-18); 插件作者 SDK = xjs_plugin_sdk.h (只有本文件 include 它,
 * manifest 的 picojson 解析收口在 xjs_engine.cpp — picojson 全库唯一 include 红线不破)。
 *
 * 事实源口径:
 *   - 插件 = 进程级资源: 注册表住本文件 static, 不进 XjsSearchWindow (对齐"进程共享的才用全局")。
 *   - 禁用 = 宿主闸门 (每个 API 入口查 enabled 位), 不卸载 DLL (照正式版 Extension 口径);
 *     已注入菜单在下次构建时自然消失 (不置灰, 直接不渲染)。
 *   - 生命周期: 启动扫描(不加载) → 对照配置加载已启用 → 运行时开关 = 闸门 + 首次启用立即加载;
 *     替换 DLL 需重启 (重扫只比对 WriteTime 显示"需重启生效"); 退出逆序通知, 不 FreeLibrary。
 *   - 窗口令牌 = (代<<32)|At 序号: 宿主回调时产生, 插件原样回传; 槽位复用由代号防串窗。
 *   - 票号表: 插件菜单项 id = IDM_PLUGIN_BASE+slot, 构建前清表重登记 (同"按 id 快照"防线,
 *     防插件启停/动态菜单造成下标漂移)。
 *   - 线程: 宿主→插件回调只在本进程 UI 线程; 插件→宿主 文件/存储/Log/Subscribe 任意线程,
 *     界面/对话框/Toast/皮肤 仅 UI 线程, 错线程返回 ERR_THREAD 而非崩溃。
 *   - 无任何插件时全部热路径 s_plugins.empty() 零开销短路, 行为与今天完全一致。
 */
#include "xjs_app.h"
#include "xjs_plugin_sdk.h"
#include <shellapi.h>
#include <shobjidl.h>
#include <cwctype>
#include <algorithm>
#include <thread>

/* ==================== 注册表 (实现细节, 只有本文件能碰) ==================== */

static const unsigned XJS_CTX_MAGIC = 0x584A5047;   /* "XJPG" */

/* 插件身份 (堆分配, 指针终身稳定 — vector 扩缩容不影响已发给插件的 ctx) */
struct XjsPluginCtx {
    unsigned magic;
    int idx;               /* s_plugins 下标 (重扫后在 SyncCtxIdx 统一刷新) */
};

struct XjsPluginUserState {
    std::wstring id;
    bool enabled = false;
    std::wstring confirmedVer;
};

struct XjsPluginEntry {
    XjsPluginManifest mf;         /* 清单解析产物 (mf.ok=false = 清单错误, 永不加载) */
    std::wstring dir;             /* plugins\<id> 绝对路径 */
    std::wstring dllPath;         /* 空 = 纯声明式插件 */
    bool enabled = false;         /* 用户开关 (宿主闸门依据; 加载失败不写回 false — 设计稿 §2.2) */
    std::wstring confirmedVer;    /* 已确认版本 (风险确认框记忆) */
    bool loaded = false;          /* DLL 已加载且 Init 成功 (纯声明式插件随 enabled 视为可用) */
    std::wstring loadErr;         /* 最近一次加载失败原因 */
    bool staleDll = false;        /* 重扫发现 DLL 比加载时新 → "需重启生效" */
    FILETIME dllWrite{};          /* 加载时的 DLL WriteTime (stale 比对基准) */
    HMODULE mod = NULL;           /* 一经加载不卸载; 失败也保留 (引擎宿主口径: 卸载第三方 CRT 不安全) */
    XjsPluginCtx* ctx = NULL;     /* 堆身份 (进程终身) */
    unsigned evtMask = 0;         /* Subscribe 掩码 */
    XjsPluginInfo const* info = NULL;
    /* 导出面 (固定三个必须, 能力回调可缺 — GetProcAddress 探测) */
    const XjsPluginInfo* (XJS_PLUGIN_CALL *fnGetInfo)(void) = NULL;
    int  (XJS_PLUGIN_CALL *fnInit)(XjsPluginCtx*, const XjsPluginHost*) = NULL;
    void (XJS_PLUGIN_CALL *fnShutdown)(XjsPluginCtx*) = NULL;
    void (XJS_PLUGIN_CALL *fnOnCommand)(XjsPluginCtx*, const char*, XjsWindowToken, const int*, int, int) = NULL;
    int  (XJS_PLUGIN_CALL *fnBuildMenu)(XjsPluginCtx*, const char*, XjsWindowToken, const int*, int, const char*, char*, int) = NULL;
    int  (XJS_PLUGIN_CALL *fnOnSearchMode)(XjsPluginCtx*, const char*, XjsWindowToken, const char*) = NULL;
    int  (XJS_PLUGIN_CALL *fnOnInput)(XjsPluginCtx*, XjsWindowToken, const char*) = NULL;
    int  (XJS_PLUGIN_CALL *fnOnPreview)(XjsPluginCtx*, int, XjsWindowToken, int) = NULL;
    void (XJS_PLUGIN_CALL *fnOnEvent)(XjsPluginCtx*, int, XjsWindowToken, void*) = NULL;
    void (XJS_PLUGIN_CALL *fnOnPanelEvent)(XjsPluginCtx*, XjsWindowToken, const XjsPanelEvent*) = NULL;
    void (XJS_PLUGIN_CALL *fnOnHostGone)(XjsPluginCtx*) = NULL;
};

/* 镜像对值 (xjs_app.h XJS_HPANEL_* ↔ SDK XJS_PANEL_*): 两套枚举必须逐项同值, 单边改号此处必炸 */
static_assert(XJS_HPANEL_OPEN == XJS_PANEL_OPEN && XJS_HPANEL_CLOSE == XJS_PANEL_CLOSE &&
              XJS_HPANEL_RESIZE == XJS_PANEL_RESIZE && XJS_HPANEL_MOUSE_MOVE == XJS_PANEL_MOUSE_MOVE &&
              XJS_HPANEL_LDOWN == XJS_PANEL_LDOWN && XJS_HPANEL_LUP == XJS_PANEL_LUP &&
              XJS_HPANEL_RDOWN == XJS_PANEL_RDOWN && XJS_HPANEL_RUP == XJS_PANEL_RUP &&
              XJS_HPANEL_DBLCLK == XJS_PANEL_DBLCLK && XJS_HPANEL_WHEEL == XJS_PANEL_WHEEL &&
              XJS_HPANEL_KEY_DOWN == XJS_PANEL_KEY_DOWN && XJS_HPANEL_KEY_CHAR == XJS_PANEL_KEY_CHAR &&
              XJS_HPANEL_FOCUS == XJS_PANEL_FOCUS && XJS_HPANEL_CAPTURE_LOST == XJS_PANEL_CAPTURE_LOST &&
              XJS_HPANEL_KEY_BLUR == XJS_PANEL_KEY_BLUR,
              "XJS_HPANEL_* mirror of XJS_PANEL_* (xjs_app.h) drifted");

static std::vector<XjsPluginEntry> s_plugins;      /* 按 mf.id 升序, 槽位稳定 */
static std::vector<XjsPluginUserState> s_user;     /* 配置 "插件" 键镜像 (Load/Save 经 Xjs*UserState*) */
static bool s_started = false;
static bool s_scanned = false;
static unsigned s_uiThread = 0;                    /* Startup 时的线程 = 宿主 UI 线程 */

/* 票号表: 一次菜单会话的插件项登记 (IDM_PLUGIN_BASE+slot → 内容) */
struct XjsPluginTicket {
    int plugin = -1;
    std::string cmd;                 /* utf8 */
    unsigned long long window = 0;
    std::vector<int> ids;            /* 文件上下文 (引擎 FileId, 菜单目标选中集) */
    int kind = XJS_PLUGIN_CTX_NONE;  /* XJS_PLUGIN_CTX_* */
};
static std::vector<XjsPluginTicket> s_tickets;
static const int XJS_TICKET_MAX = 200;

/* 窗口令牌代 (槽位复用防串窗): WM_DESTROY 递增该槽 */
static const int XJS_TOKEN_SLOTS = 128;
static unsigned long long s_winGen[XJS_TOKEN_SLOTS] = {};

/* 存储写入互斥 (临时文件名按 key 定, 同插件并发写同 key 才竞争) */
static CRITICAL_SECTION s_storageCs;

/* ==================== 小工具 ==================== */

/* 调用方缓冲输出: buf/cap 为空 = 返回所需字节数; 否则写入(截断)并返回实际写入数 (均不含 NUL) */
static int PluginBufOut(char* buf, int cap, const std::string& s) {
    int need = (int)s.size();
    if (!buf || cap <= 0) return need;
    int n = (cap - 1 < need) ? cap - 1 : need;
    if (n > 0) memcpy(buf, s.data(), (size_t)n);
    buf[n] = 0;
    return n;
}

/* JSON 字符串转义 (输入按 UTF-8 字节, <0x20 控制字符转 \u00XX; 其余原样) */
static void PluginJsonEscape(const std::string& s, std::string* out) {
    out->push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"':  *out += "\\\""; break;
            case '\\': *out += "\\\\"; break;
            case '\n': *out += "\\n"; break;
            case '\r': *out += "\\r"; break;
            case '\t': *out += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; sprintf_s(b, "\\u%04X", c); *out += b; }
                else out->push_back((char)c);
        }
    }
    out->push_back('"');
}
static std::string PluginJsonStr(const std::string& s) { std::string r; PluginJsonEscape(s, &r); return r; }

static std::string PluginPathsJson(const std::vector<std::wstring>& paths) {
    std::string j = "[";
    for (size_t i = 0; i < paths.size(); i++) {
        if (i) j += ",";
        j += PluginJsonStr(Utf16ToUtf8(paths[i].c_str()));
    }
    j += "]";
    return j;
}

static FILETIME PluginFileWriteTime(const std::wstring& path) {
    FILETIME ft{};
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        GetFileTime(h, NULL, NULL, &ft);
        CloseHandle(h);
    }
    return ft;
}

static bool PluginIdOk(const std::wstring& id) {   /* 目录名/插件 id 白名单 [a-z0-9-]{1,48} */
    if (id.empty() || id.size() > 48) return false;
    for (wchar_t c : id)
        if (!((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-')) return false;
    return true;
}

static bool PluginReadSmallFile(const std::wstring& path, std::string* out) {
    out->clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[8192]; DWORD rd = 0;
    while (ReadFile(h, buf, sizeof(buf), &rd, NULL) && rd) out->append(buf, rd);
    CloseHandle(h);
    return true;
}

static bool PluginUiThread() { return s_uiThread && GetCurrentThreadId() == s_uiThread; }

static const XjsPluginHost* PluginHostTable();   /* 定义在函数表处 (表引用 Fn*, 见文件尾) */

/* 窗口令牌 ↔ 窗口 */
static int PluginSlotOfHwnd(HWND hwnd) {
    for (int i = 0; i < XjsSearchWindow::Count(); i++) {
        XjsSearchWindow* w = XjsSearchWindow::At(i);
        if (w && w->hWnd == hwnd) return i;
    }
    return -1;
}
static unsigned long long PluginTokenOf(HWND hwnd) {
    int i = PluginSlotOfHwnd(hwnd);
    if (i < 0 || i >= XJS_TOKEN_SLOTS) return 0;
    return (s_winGen[i] << 32) | (unsigned)i;
}
static XjsSearchWindow* PluginWindowOfToken(unsigned long long tok) {
    if (!tok) return XjsSearchWindow::Main();   /* 0 = 主窗缺省 */
    unsigned idx = (unsigned)(tok & 0xffffffffu);
    if ((int)idx >= XJS_TOKEN_SLOTS) return NULL;
    XjsSearchWindow* w = XjsSearchWindow::At((int)idx);
    if (!w || (tok >> 32) != s_winGen[idx]) return NULL;   /* 槽位已换窗 = 旧令牌失效 */
    return w;
}

/* s_plugins 本体重建 (PluginScan, UI 线程) 与插件线程取条目 (PluginApiCheck) 的同步。
   共享锁只护"读 size + 取下标"瞬间 (vector 内部指针撕裂读 = 唯一内存险), 条目本体由
   退役注册表保活, 解锁后 p 的后续读取内存安全; 锁绝不横跨插件代码, 无重入死锁面 */
static SRWLOCK s_regLock = SRWLOCK_INIT;

/* 宿主 API 入口统一校验: ctx 身份 → 宿主闸门(enabled) → 权限位 → 线程 */
static int PluginApiCheck(XjsPluginCtx* ctx, unsigned perm, bool uiOnly, XjsPluginEntry** out) {
    *out = NULL;
    if (!ctx || ctx->magic != XJS_CTX_MAGIC) return XJS_PLUGIN_ERR_ARG;
    AcquireSRWLockShared(&s_regLock);   /* 护取下标瞬间: 重扫 (PluginScan 排他锁) 不得并发读 vector 本体 */
    bool inRange = ctx->idx >= 0 && ctx->idx < (int)s_plugins.size();
    XjsPluginEntry* p = inRange ? &s_plugins[ctx->idx] : NULL;
    ReleaseSRWLockShared(&s_regLock);
    /* 解锁后读 p 安全: 条目缓冲重扫时整体移入退役注册表保活 (值可能已 stale, ctx↔下标双向对表兜底) */
    if (!p || p->ctx != ctx) return XJS_PLUGIN_ERR_ARG;  /* 指针↔下标双向对表 (重扫后 idx 刷新前的旧 ctx) */
    if (!p->enabled) return XJS_PLUGIN_ERR_PERM;       /* 禁用 = 宿主强制闸门 (照正式版口径) */
    if (perm && !(p->mf.perms & perm)) return XJS_PLUGIN_ERR_PERM;
    if (uiOnly && !PluginUiThread()) return XJS_PLUGIN_ERR_THREAD;
    *out = p;
    return XJS_PLUGIN_OK;
}

static bool PluginActive(const XjsPluginEntry& e) {   /* "启用且可用": 声明式看 enabled, DLL 插件还要加载成功 */
    if (!e.enabled || !e.mf.ok) return false;
    return e.mf.dllName.empty() ? true : e.loaded;
}

static void PluginUserSync(const XjsPluginEntry& e) {   /* 开关/确认版本 → 配置镜像 (调用方随后 XjsSaveConfig) */
    for (auto& u : s_user)
        if (u.id == e.mf.id) { u.enabled = e.enabled; u.confirmedVer = e.confirmedVer; return; }
    s_user.push_back({ e.mf.id, e.enabled, e.confirmedVer });
}

/* ==================== 配置用户状态 (Load/Save 经此) ==================== */

void XjsPluginUserStateSet(const wchar_t* id, bool enabled, const wchar_t* confirmedVer) {
    if (!id || !*id) return;
    for (auto& u : s_user)
        if (u.id == id) { u.enabled = enabled; u.confirmedVer = confirmedVer ? confirmedVer : L""; return; }
    s_user.push_back({ id, enabled, confirmedVer ? confirmedVer : L"" });
}
int  XjsPluginUserStateCount() { return (int)s_user.size(); }
bool XjsPluginUserStateAt(int i, std::wstring* id, bool* enabled, std::wstring* confirmedVer) {
    if (i < 0 || i >= (int)s_user.size()) return false;
    *id = s_user[i].id; *enabled = s_user[i].enabled; *confirmedVer = s_user[i].confirmedVer;
    return true;
}

/* ==================== 扫描 / 加载 ==================== */

std::wstring XjsPluginRootDir() { return XjsGetExeDir() + L"\\plugins"; }

/* 重扫换下的旧注册表缓冲: 工作线程在 PluginApiCheck 之后仍持条目指针 p 使用整段调用
   (SDK 契约: 文件/存储/Log/Subscribe 任意线程), 没有安全的回收点 — 退役缓冲保存到
   进程退出 (手动重扫低频, 每次几 KB), 换取工作线程绝不摸到已释放内存 */
static std::vector<std::vector<XjsPluginEntry>*> s_retiredRegistries;

/* 重扫: 目录 → 清单 → 与既有条目按 id 合并 (已加载的 DLL 状态原样保留, 清单刷新) */
static void PluginScan() {
    AcquireSRWLockExclusive(&s_regLock);   /* 重建期间挡住插件线程的取条目共享锁 */
    std::vector<XjsPluginEntry>* old = new std::vector<XjsPluginEntry>(std::move(s_plugins));
    s_plugins.clear();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((XjsPluginRootDir() + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            std::wstring id = fd.cFileName;
            if (id == L"." || id == L".." || !PluginIdOk(id)) continue;
            XjsPluginEntry e;
            e.dir = XjsPluginRootDir() + L"\\" + id;
            std::string json;
            if (PluginReadSmallFile(e.dir + L"\\manifest.json", &json) && XjsPluginManifestParse(json.c_str(), &e.mf)) {
                if (e.mf.id != id) { e.mf.ok = false; e.mf.err = L"id 与目录名不一致"; }
            }
            e.mf.id = id;   /* 解析失败也保目录名 (管理页列表能显示) */
            for (auto& u : s_user)
                if (u.id == id) { e.enabled = u.enabled; e.confirmedVer = u.confirmedVer; break; }
            if (!e.mf.dllName.empty()) {
                e.dllPath = e.dir + L"\\" + e.mf.dllName;
                FILETIME now = PluginFileWriteTime(e.dllPath);
                if (e.mf.ok) e.dllWrite = now;
            }
            for (auto& o : *old) {   /* 同 id 既有条目: 保留加载态/身份/订阅 */
                if (o.mf.id != id) continue;
                e.loaded = o.loaded; e.mod = o.mod; e.ctx = o.ctx; e.info = o.info;
                e.evtMask = o.evtMask; e.loadErr = o.loadErr;
                e.fnGetInfo = o.fnGetInfo; e.fnInit = o.fnInit; e.fnShutdown = o.fnShutdown;
                e.fnOnCommand = o.fnOnCommand; e.fnBuildMenu = o.fnBuildMenu; e.fnOnSearchMode = o.fnOnSearchMode;
                e.fnOnInput = o.fnOnInput; e.fnOnPreview = o.fnOnPreview; e.fnOnEvent = o.fnOnEvent;
                e.fnOnPanelEvent = o.fnOnPanelEvent;
                e.fnOnHostGone = o.fnOnHostGone;
                if (o.loaded && e.mf.ok && !e.dllPath.empty()) {   /* 已加载插件: DLL 被换过 → 需重启生效 */
                    FILETIME now = PluginFileWriteTime(e.dllPath);
                    e.staleDll = CompareFileTime(&now, &o.dllWrite) > 0;
                    e.dllWrite = o.dllWrite;
                }
                break;
            }
            s_plugins.push_back(e);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(s_plugins.begin(), s_plugins.end(),
              [](const XjsPluginEntry& a, const XjsPluginEntry& b) { return a.mf.id < b.mf.id; });
    for (int i = 0; i < (int)s_plugins.size(); i++) {   /* ctx 下标统一刷新 (ctx 指针本体保持稳定) */
        if (!s_plugins[i].ctx) s_plugins[i].ctx = new XjsPluginCtx{ XJS_CTX_MAGIC, i };
        else s_plugins[i].ctx->idx = i;
    }
    if (old->empty()) delete old;
    else s_retiredRegistries.push_back(old);   /* 退役缓冲不释放 (见 s_retiredRegistries 注) */
    s_scanned = true;
    ReleaseSRWLockExclusive(&s_regLock);
}

/* 加载单个插件 (幂等): 校验 abi/id → LoadLibrary → 解析导出 → Init。失败返回 false+原因 (不卸载) */
static bool PluginLoadOne(XjsPluginEntry& e, std::wstring* err) {
    err->clear();
    if (e.loaded) return true;
    if (!e.mf.ok) { *err = e.mf.err; return false; }
    if (e.mf.dllName.empty()) { e.loaded = true; return true; }   /* 纯声明式: 无代码, 开闸即可用 */
    if (!e.dllPath.empty() && GetFileAttributesW(e.dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        *err = L"找不到 DLL 文件"; return false;
    }
    if (e.mf.abi != XJS_PLUGIN_ABI_VERSION) { *err = L"abi 版本不符 (清单 接口版本 必须与 SDK 头 XJS_PLUGIN_ABI_VERSION 一致)"; return false; }
    if (!e.mod) {
        /* 阶段标记: static 缓冲 (g_phase 要活到崩溃时刻, 局部数组出块即悬垂 — 同 main.cpp 口径) */
        static wchar_t ph[72];
        swprintf(ph, 72, L"plugin-load:%s", e.mf.id.c_str());
        XjsSetPhase(ph);
        e.mod = LoadLibraryW(e.dllPath.c_str());
        if (!e.mod) { *err = L"DLL 加载失败 (位数不符或依赖缺失)"; return false; }
    }
    if (!e.fnGetInfo) {
        e.fnGetInfo = (const XjsPluginInfo* (XJS_PLUGIN_CALL*)(void))GetProcAddress(e.mod, "XjsPlugin_GetInfo");
        e.fnInit = (int (XJS_PLUGIN_CALL*)(XjsPluginCtx*, const XjsPluginHost*))GetProcAddress(e.mod, "XjsPlugin_Init");
        e.fnShutdown = (void (XJS_PLUGIN_CALL*)(XjsPluginCtx*))GetProcAddress(e.mod, "XjsPlugin_Shutdown");
        if (!e.fnGetInfo || !e.fnInit || !e.fnShutdown) { *err = L"缺少固定导出 (GetInfo/Init/Shutdown)"; return false; }
        e.info = e.fnGetInfo();
        /* ABI 精确匹配 (v2 起事件回调签名与宿主表布局强耦合, 旧版插件混载 = 调用约定错位必崩) */
        if (!e.info || e.info->abiVersion != XJS_PLUGIN_ABI_VERSION || !e.info->id ||
            Utf8ToUtf16(e.info->id) != e.mf.id) {
            *err = L"GetInfo 缺失或 ABI/标识不符"; return false;
        }
        e.fnOnCommand = (void (XJS_PLUGIN_CALL*)(XjsPluginCtx*, const char*, XjsWindowToken, const int*, int, int))GetProcAddress(e.mod, "XjsPlugin_OnCommand");
        e.fnBuildMenu = (int (XJS_PLUGIN_CALL*)(XjsPluginCtx*, const char*, XjsWindowToken, const int*, int, const char*, char*, int))GetProcAddress(e.mod, "XjsPlugin_BuildMenu");
        e.fnOnSearchMode = (int (XJS_PLUGIN_CALL*)(XjsPluginCtx*, const char*, XjsWindowToken, const char*))GetProcAddress(e.mod, "XjsPlugin_OnSearchMode");
        e.fnOnInput = (int (XJS_PLUGIN_CALL*)(XjsPluginCtx*, XjsWindowToken, const char*))GetProcAddress(e.mod, "XjsPlugin_OnInput");
        e.fnOnPreview = (int (XJS_PLUGIN_CALL*)(XjsPluginCtx*, int, XjsWindowToken, int))GetProcAddress(e.mod, "XjsPlugin_OnPreview");
        e.fnOnEvent = (void (XJS_PLUGIN_CALL*)(XjsPluginCtx*, int, XjsWindowToken, void*))GetProcAddress(e.mod, "XjsPlugin_OnEvent");
        e.fnOnPanelEvent = (void (XJS_PLUGIN_CALL*)(XjsPluginCtx*, XjsWindowToken, const XjsPanelEvent*))GetProcAddress(e.mod, "XjsPlugin_OnPanelEvent");
        e.fnOnHostGone = (void (XJS_PLUGIN_CALL*)(XjsPluginCtx*))GetProcAddress(e.mod, "XjsPlugin_OnHostGone");
    }
    if (!e.ctx) { *err = L"内部状态错误 (ctx 未初始化)"; return false; }   /* 扫描后置已保证分配 */
    if (e.fnInit(e.ctx, PluginHostTable()) != XJS_PLUGIN_OK) { *err = L"Init 返回失败"; return false; }
    e.dllWrite = PluginFileWriteTime(e.dllPath);
    e.staleDll = false;
    e.loaded = true;
    XjsSetPhase(L"plugin-load:done");
    return true;
}

void XjsPluginStartup() {
    if (s_started) return;
    s_started = true;
    s_uiThread = GetCurrentThreadId();
    InitializeCriticalSectionAndSpinCount(&s_storageCs, 100);
    XjsSetPhase(L"plugin-scan");
    PluginScan();
    for (int i = 0; i < (int)s_plugins.size(); i++) {
        XjsPluginEntry& e = s_plugins[i];
        if (!e.enabled || !e.mf.ok) continue;
        std::wstring err;
        if (!PluginLoadOne(e, &err)) e.loadErr = err;   /* 失败不写回 enabled (设计稿 §2.2), 状态列显示原因 */
    }
    XjsSetPhase(L"plugin-scan:done");
}

void XjsPluginShutdown() {
    if (!s_started) return;
    for (int i = (int)s_plugins.size() - 1; i >= 0; i--) {   /* 逆加载序 */
        XjsPluginEntry& e = s_plugins[i];
        if (!e.loaded || !e.ctx) continue;
        XjsSetPhase(L"plugin-shutdown");
        if (e.fnOnHostGone) e.fnOnHostGone(e.ctx);   /* 引擎尚未销毁的最后通知 */
        if (e.fnShutdown) e.fnShutdown(e.ctx);   /* 纯声明式插件 loaded=true 但无代码, fnShutdown 为 NULL */
        e.loaded = false;
    }
}

void XjsPluginRescan() {
    if (!s_started) { XjsPluginStartup(); return; }
    XjsSetPhase(L"plugin-scan");
    PluginScan();
    for (auto& e : s_plugins) {   /* 新发现且已启用的 (重启前手工放进目录的) 立即补载 */
        if (!e.enabled || !e.mf.ok || e.loaded) continue;
        std::wstring err;
        if (!PluginLoadOne(e, &err)) e.loadErr = err;
    }
    XjsPluginPanelValidateOwners();   /* 重扫可能移除/清空能力: owner 失效的接管会话立即结束 */
}

void XjsPluginOnWindowDestroyed(HWND hwnd) {
    int i = PluginSlotOfHwnd(hwnd);
    if (i >= 0 && i < XJS_TOKEN_SLOTS) s_winGen[i]++;
}

void XjsPluginOnWindowsCompacted(int fromSlot) {
    /* 窗口注册表压缩 (DestroyAndFree 摘除后整体前移) 会让后窗"换槽"而令牌未变 —
       旧令牌按旧槽号会命中压缩后占住该槽的别的窗。从摘除槽起全部递增代号:
       受影响窗口的旧令牌一律失效 (插件重取 ERR_NOTFOUND, 好过串窗) */
    if (fromSlot < 0) fromSlot = 0;
    for (int i = fromSlot; i < XJS_TOKEN_SLOTS; i++) s_winGen[i]++;
}

/* ==================== 启用 / 禁用 (设置页) ==================== */

bool XjsPluginNeedsConfirm(int i) {
    if (i < 0 || i >= (int)s_plugins.size()) return false;
    const XjsPluginEntry& e = s_plugins[i];
    return e.confirmedVer.empty() || e.confirmedVer != e.mf.version;
}
void XjsPluginMarkConfirmed(int i) {
    if (i < 0 || i >= (int)s_plugins.size()) return;
    s_plugins[i].confirmedVer = s_plugins[i].mf.version;
    PluginUserSync(s_plugins[i]);
}
bool XjsPluginEnable(int i, std::wstring* err) {
    if (i < 0 || i >= (int)s_plugins.size()) { if (err) *err = L"插件不存在"; return false; }
    XjsPluginEntry& e = s_plugins[i];
    e.enabled = true;
    PluginUserSync(e);
    if (!PluginLoadOne(e, err)) { e.loadErr = *err; return false; }   /* enabled 保持 true, 状态列显示原因 */
    e.loadErr.clear();
    return true;
}
void XjsPluginDisable(int i) {
    if (i < 0 || i >= (int)s_plugins.size()) return;
    XjsPluginEntry& e = s_plugins[i];
    e.enabled = false;
    PluginUserSync(e);
    XjsPluginPanelValidateOwners();   /* 面板接管会话的 owner 失效 → 立即结束并恢复预览 */
    /* 不卸载不释放 (照源样式口径); 其搜索模式/托管来源由调用方剔除并重搜。
       evtMask 不清: 事件派发本就按 PluginActive (enabled) 闸住, 清了则再启用时
       loaded=true 短路 Init 不重跑、Subscribe 不会再调 = 插件永久收不到事件 */
}
void XjsPluginOpenDir(int i) {
    std::wstring dir = (i >= 0 && i < (int)s_plugins.size()) ? s_plugins[i].dir : XjsPluginRootDir();
    if ((uintptr_t)ShellExecuteW(NULL, L"open", dir.c_str(), NULL, NULL, SW_SHOWNORMAL) <= 32)
        ShellExecuteW(NULL, L"open", XjsGetExeDir().c_str(), NULL, NULL, SW_SHOWNORMAL);   /* 目录被删 → 退回 exe 目录 */
}

/* ==================== 设置页数据源 ==================== */

int XjsPluginCount() { return s_scanned ? (int)s_plugins.size() : 0; }

bool XjsPluginBriefAt(int i, XjsPluginBrief* out) {
    if (i < 0 || i >= (int)s_plugins.size() || !out) return false;
    const XjsPluginEntry& e = s_plugins[i];
    out->id = e.mf.id;
    out->name = e.mf.ok ? e.mf.name : e.mf.id;
    out->version = e.mf.version;
    out->author = e.mf.author;
    out->description = e.mf.description;
    out->declared = e.mf.ok;
    out->manifestErr = e.mf.err;
    out->enabled = e.enabled;
    out->loaded = e.loaded;
    out->staleDll = e.staleDll;
    out->loadErr = e.loadErr;
    out->dir = e.dir;
    out->caps = e.mf.caps;
    out->perms = e.mf.perms;
    out->type = e.mf.type;
    out->hasDll = !e.mf.dllName.empty();
    out->dllFile = e.mf.dllName;
    return true;
}

/* ==================== 宿主 API: 文件组 (任意线程) ==================== */

static int FnPathInfo(XjsPluginCtx* ctx, const char* path, char* buf, int cap) {
    XjsPluginEntry* p; int e;
    if ((e = PluginApiCheck(ctx, XPP_READ, false, &p)) != XJS_PLUGIN_OK) return e;
    if (!path || !*path) return XJS_PLUGIN_ERR_ARG;
    std::wstring w = Utf8ToUtf16(path);
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &fad)) return XJS_PLUGIN_ERR_NOTFOUND;
    auto ms = [](const FILETIME& t) {
        return (long long)(((unsigned long long)t.dwLowDateTime | ((unsigned long long)t.dwHighDateTime << 32)) / 10000);
    };
    DWORD attrs = GetFileAttributesW(w.c_str());
    char j[512];
    sprintf_s(j, "{\"exists\":true,\"dir\":%d,\"size\":%lld,\"mtime\":%lld,\"ctime\":%lld,\"atime\":%lld,\"attrs\":%lu}",
              (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0,
              (long long)(((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow),
              ms(fad.ftLastWriteTime), ms(fad.ftCreationTime), ms(fad.ftLastAccessTime),
              attrs == INVALID_FILE_ATTRIBUTES ? 0ul : attrs);
    return PluginBufOut(buf, cap, j);
}

static int FnReadFile(XjsPluginCtx* ctx, const char* path, long long offset, int len,
                      char* buf, int cap, int* eof) {
    XjsPluginEntry* p; int e;
    if ((e = PluginApiCheck(ctx, XPP_READ, false, &p)) != XJS_PLUGIN_OK) return e;
    if (!path || !*path || len < 0 || !buf || cap <= 0 || len > cap) return XJS_PLUGIN_ERR_ARG;
    if (eof) *eof = 0;
    HANDLE h = CreateFileW(Utf8ToUtf16(path).c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return XJS_PLUGIN_ERR_NOTFOUND;
    LARGE_INTEGER li; li.QuadPart = offset;
    int rc = XJS_PLUGIN_ERR_IO;
    if (SetFilePointerEx(h, li, NULL, FILE_BEGIN)) {
        DWORD rd = 0;
        if (ReadFile(h, buf, (DWORD)len, &rd, NULL)) {
            if (eof) *eof = (rd < (DWORD)len) ? 1 : 0;
            rc = (int)rd;
        }
    }
    CloseHandle(h);
    return rc;
}

static int FnWriteFile(XjsPluginCtx* ctx, const char* path, const void* data, int len) {
    XjsPluginEntry* p; int e;
    if ((e = PluginApiCheck(ctx, XPP_WRITE, false, &p)) != XJS_PLUGIN_OK) return e;
    if (!path || !*path || len < 0 || (!data && len > 0)) return XJS_PLUGIN_ERR_ARG;
    HANDLE h = CreateFileW(Utf8ToUtf16(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                           NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return XJS_PLUGIN_ERR_IO;
    DWORD wr = 0;
    BOOL ok = TRUE;
    if (len > 0) ok = WriteFile(h, data, (DWORD)len, &wr, NULL) && wr == (DWORD)len;
    CloseHandle(h);
    return ok ? len : XJS_PLUGIN_ERR_IO;
}

static int FnListDir(XjsPluginCtx* ctx, const char* path, char* buf, int cap) {
    XjsPluginEntry* p; int e;
    if ((e = PluginApiCheck(ctx, XPP_READ, false, &p)) != XJS_PLUGIN_OK) return e;
    if (!path || !*path) return XJS_PLUGIN_ERR_ARG;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((Utf8ToUtf16(path) + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return XJS_PLUGIN_ERR_NOTFOUND;
    std::string j = "[";
    bool first = true;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (!first) j += ",";
        first = false;
        j += "{\"name\":";
        j += PluginJsonStr(Utf16ToUtf8(fd.cFileName));
        char it[96];
        sprintf_s(it, ",\"dir\":%d,\"size\":%lld}",
                  (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0,
                  (long long)(((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow));
        j += it;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    j += "]";
    return PluginBufOut(buf, cap, j);
}

/* pathsJson → 路径表 (解析收口 xjs_engine.cpp 的 XjsJsonStringArray; 坏 JSON = 空) */
static bool PluginPathsFromJson(const char* pathsJson, std::vector<std::wstring>* out) {
    out->clear();
    if (!pathsJson || !*pathsJson) return false;
    XjsJsonStringArray(pathsJson, out);
    return !out->empty();
}

static int FnDeleteRecycle(XjsPluginCtx* ctx, const char* pathsJson, char* buf, int cap) {
    XjsPluginEntry* p; int e;
    if ((e = PluginApiCheck(ctx, XPP_WRITE, false, &p)) != XJS_PLUGIN_OK) return e;
    std::vector<std::wstring> paths;
    if (!PluginPathsFromJson(pathsJson, &paths)) return XJS_PLUGIN_ERR_ARG;
    std::wstring from;
    for (auto& s : paths) { from += s; from += L'\0'; }
    from += L'\0';
    SHFILEOPSTRUCTW fo = {};
    fo.hwnd = NULL; fo.wFunc = FO_DELETE; fo.pFrom = from.c_str();
    fo.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    int rc = SHFileOperationW(&fo);
    char j[96];
    sprintf_s(j, "{\"ok\":%d,\"fail\":%d}", (rc == 0 && !fo.fAnyOperationsAborted) ? (int)paths.size() : 0,
              (rc == 0 && !fo.fAnyOperationsAborted) ? 0 : (int)paths.size());
    return PluginBufOut(buf, cap, j);
}

static int FnMoveCopy(XjsPluginCtx* ctx, const char* pathsJson, const char* destDir, int copy, char* buf, int cap) {
    XjsPluginEntry* p; int e;
    if ((e = PluginApiCheck(ctx, XPP_WRITE, false, &p)) != XJS_PLUGIN_OK) return e;
    std::vector<std::wstring> paths;
    if (!PluginPathsFromJson(pathsJson, &paths) || !destDir || !*destDir) return XJS_PLUGIN_ERR_ARG;
    std::wstring from, dest = Utf8ToUtf16(destDir);
    dest += L'\0';
    for (auto& s : paths) { from += s; from += L'\0'; }
    from += L'\0';
    SHFILEOPSTRUCTW fo = {};
    fo.hwnd = NULL; fo.wFunc = copy ? FO_COPY : FO_MOVE;
    fo.pFrom = from.c_str(); fo.pTo = dest.c_str();
    fo.fFlags = FOF_NOCONFIRMMKDIR | FOF_RENAMEONCOLLISION | FOF_NOCONFIRMATION | FOF_NOERRORUI;   /* 同名不静默覆盖 */
    int rc = SHFileOperationW(&fo);
    char j[96];
    sprintf_s(j, "{\"ok\":%d,\"fail\":%d}", (rc == 0 && !fo.fAnyOperationsAborted) ? (int)paths.size() : 0,
              (rc == 0 && !fo.fAnyOperationsAborted) ? 0 : (int)paths.size());
    return PluginBufOut(buf, cap, j);
}

static int FnRenameTo(XjsPluginCtx* ctx, const char* path, const char* newPath, char* buf, int cap) {
    XjsPluginEntry* p; int e;
    if ((e = PluginApiCheck(ctx, XPP_WRITE, false, &p)) != XJS_PLUGIN_OK) return e;
    if (!path || !*path || !newPath || !*newPath) return XJS_PLUGIN_ERR_ARG;
    std::wstring wNew = Utf8ToUtf16(newPath);
    if (GetFileAttributesW(wNew.c_str()) != INVALID_FILE_ATTRIBUTES) return XJS_PLUGIN_ERR_NOTFOUND;   /* 不覆盖 */
    if (!MoveFileExW(Utf8ToUtf16(path).c_str(), wNew.c_str(), MOVEFILE_COPY_ALLOWED)) return XJS_PLUGIN_ERR_IO;
    return PluginBufOut(buf, cap, "{\"ok\":true}");
}

/* ==================== 宿主 API: 系统 ==================== */

static int FnExec(XjsPluginCtx* ctx, const char* exe, const char* argsJson, const char* cwd,
                  int timeoutMs, char* buf, int cap) {
    XjsPluginEntry* p; int e;
    if ((e = PluginApiCheck(ctx, XPP_EXEC, false, &p)) != XJS_PLUGIN_OK) return e;
    if (!exe || !*exe) return XJS_PLUGIN_ERR_ARG;
    std::vector<std::wstring> args;
    if (argsJson && *argsJson) XjsJsonStringArray(argsJson, &args);
    /* 命令行: exe 与参数各自整体加引号, 参数内部的引号剥除 (CreateProcessW 直启, 不走 cmd.exe 防注入) */
    auto q = [](const std::wstring& s) {
        std::wstring r = L"\"";
        for (wchar_t c : s) if (c != L'"') r += c;
        return r + L"\"";
    };
    std::wstring cl = q(Utf8ToUtf16(exe));
    for (auto& a : args) cl += L" " + q(a);
    std::wstring cw = (cwd && *cwd) ? Utf8ToUtf16(cwd) : L"";

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE outR = NULL, outW = NULL, errR = NULL, errW = NULL;
    if (!CreatePipe(&outR, &outW, &sa, 0) || !CreatePipe(&errR, &errW, &sa, 0)) {
        if (outR) CloseHandle(outR); if (outW) CloseHandle(outW);
        if (errR) CloseHandle(errR); if (errW) CloseHandle(errW);
        return XJS_PLUGIN_ERR_IO;
    }
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);   /* 读端不继承, 否则读不到 EOF */
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = outW; si.hStdError = errW;
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> clBuf(cl.begin(), cl.end()); clBuf.push_back(L'\0');
    /* 作业对象收口整棵进程树: 孙进程继承管道写端时不杀它, 读线程 ReadFile 永不 EOF,
       t1.join() 永久挂死 (插件在 UI 线程同步调用 = 全程序冻结) */
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl = {};
        jl.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jl, sizeof(jl));
    }
    BOOL ok = CreateProcessW(NULL, clBuf.data(), NULL, NULL, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW, NULL,
                             cw.empty() ? NULL : cw.c_str(), &si, &pi);
    CloseHandle(outW); CloseHandle(errW);   /* 父端写端先关, 读端才能 EOF */
    if (!ok) {
        CloseHandle(outR); CloseHandle(errR);
        if (job) CloseHandle(job);
        return XJS_PLUGIN_ERR_NOTFOUND;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);   /* 先入作业再放行 (挂起态防逃逸) */
    ResumeThread(pi.hThread);
    const int XJS_EXEC_CAP = 4 * 1024 * 1024;
    auto reader = [&](HANDLE rd, std::string* s) {
        char ch[8192]; DWORD n = 0;
        while (ReadFile(rd, ch, sizeof(ch), &n, NULL) && n > 0)
            if ((int)s->size() < XJS_EXEC_CAP) s->append(ch, n);   /* 超限继续读(丢弃), 保证管道排空 */
    };
    std::string so, se;
    std::thread t1(reader, outR, &so), t2(reader, errR, &se);
    bool timedOut = false;
    if (WaitForSingleObject(pi.hProcess, timeoutMs > 0 ? (DWORD)timeoutMs : INFINITE) == WAIT_TIMEOUT) {
        timedOut = true;
        if (job) TerminateJobObject(job, (UINT)-1);   /* 杀整棵树 (孙进程一并), 管道写端随之关闭 */
        else TerminateProcess(pi.hProcess, (UINT)-1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    /* 子进程已退出仍可能有孙进程持有管道写端: 不收割则 join 挂死 — 终结作业放行 EOF。
       TerminateJobObject 对已退出成员无害, exit code 已在上方取出 */
    if (job) TerminateJobObject(job, (UINT)code);
    t1.join(); t2.join();
    CloseHandle(outR); CloseHandle(errR); CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);
    bool trunc = so.size() >= (size_t)XJS_EXEC_CAP || se.size() >= (size_t)XJS_EXEC_CAP;
    std::string j = "{\"exitCode\":";
    j += std::to_string((long long)(int)code);
    j += ",\"stdout\":"; j += PluginJsonStr(so);
    j += ",\"stderr\":"; j += PluginJsonStr(se);
    j += timedOut ? ",\"timedOut\":true" : ",\"timedOut\":false";
    if (trunc) j += ",\"truncated\":true";
    j += "}";
    return PluginBufOut(buf, cap, j);
}

static int FnClipGet(XjsPluginCtx* ctx, char* buf, int cap) {
    XjsPluginEntry* p; int e;
    if ((e = PluginApiCheck(ctx, XPP_READ, false, &p)) != XJS_PLUGIN_OK) return e;
    if (!OpenClipboard(NULL)) return XJS_PLUGIN_ERR_STATE;
    std::string utf8;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) if (const wchar_t* w = (const wchar_t*)GlobalLock(h)) {
        utf8 = Utf16ToUtf8(w);
        GlobalUnlock(h);
    }
    CloseClipboard();
    return PluginBufOut(buf, cap, utf8);
}

static int FnClipSet(XjsPluginCtx* ctx, const char* utf8) {
    XjsPluginEntry* p; int e;
    if ((e = PluginApiCheck(ctx, XPP_WRITE, false, &p)) != XJS_PLUGIN_OK) return e;
    if (!utf8) return XJS_PLUGIN_ERR_ARG;
    std::wstring w = Utf8ToUtf16(utf8);
    size_t bytes = (w.size() + 1) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!g) return XJS_PLUGIN_ERR_FAIL;
    void* lk = GlobalLock(g);   /* 极端内存压力下可为 NULL, 直写 NULL = 崩溃 */
    if (!lk) { GlobalFree(g); return XJS_PLUGIN_ERR_FAIL; }
    memcpy(lk, w.c_str(), bytes);
    GlobalUnlock(g);
    if (!OpenClipboard(NULL)) { GlobalFree(g); return XJS_PLUGIN_ERR_STATE; }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);   /* 成功后剪贴板接管 g; 失败句柄仍归我, 不放 = 泄漏 */
    CloseClipboard();
    return XJS_PLUGIN_OK;
}

/* ---- 文件/目录选择对话框 (仅 UI 线程; COM 由主线程 OleInitialize 就绪) ---- */

static int FnDialog(XjsPluginCtx* ctx, const char* kind, const char* optsJson, char* buf, int cap) {
    XjsPluginEntry* p; int e;
    if ((e = PluginApiCheck(ctx, 0, true, &p)) != XJS_PLUGIN_OK) return e;
    if (!kind || !*kind) return XJS_PLUGIN_ERR_ARG;
    XjsPluginDialogOpts o;
    if (optsJson && *optsJson) XjsPluginDialogOptsParse(optsJson, &o);
    std::string ks = kind;
    bool multi = (ks == "files"), folder = (ks == "folder"), save = (ks == "save");
    if (!multi && !folder && !save && ks != "file") return XJS_PLUGIN_ERR_ARG;
    HWND owner = XjsSearchWindow::MainHwnd();
    if (!owner) return XJS_PLUGIN_ERR_STATE;
    std::vector<std::wstring> results;
    if (!save) {
        IFileOpenDialog* d = NULL;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d))) || !d)
            return XJS_PLUGIN_ERR_FAIL;
        DWORD opt = FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR;
        if (folder) opt |= FOS_PICKFOLDERS;
        if (multi) opt |= FOS_ALLOWMULTISELECT;
        d->SetOptions(opt);
        if (!o.title.empty()) d->SetTitle(o.title.c_str());
        if (!o.initialName.empty() && !folder) d->SetFileName(o.initialName.c_str());
        if (!o.filter.empty()) {
            std::vector<COMDLG_FILTERSPEC> fs;
            std::vector<std::wstring> keep(o.filter.size() * 2);
            for (size_t i = 0; i < o.filter.size(); i++) {
                keep[i * 2] = o.filter[i].first; keep[i * 2 + 1] = o.filter[i].second;
                fs.push_back({ keep[i * 2].c_str(), keep[i * 2 + 1].c_str() });
            }
            fs.push_back({ NULL, NULL });
            d->SetFileTypes((UINT)fs.size(), fs.data());
        }
        if (!o.initialDir.empty()) {
            IShellItem* it = NULL;
            if (SUCCEEDED(SHCreateItemFromParsingName(o.initialDir.c_str(), NULL, IID_PPV_ARGS(&it)) )) {
                d->SetFolder(it); it->Release();
            }
        }
        HRESULT hr = d->Show(owner);
        if (SUCCEEDED(hr)) {
            if (multi) {
                IShellItemArray* arr = NULL;
                if (SUCCEEDED(d->GetResults(&arr)) && arr) {
                    DWORD n = 0; arr->GetCount(&n);
                    for (DWORD i = 0; i < n; i++) {
                        IShellItem* it = NULL;
                        if (SUCCEEDED(arr->GetItemAt(i, &it)) && it) {
                            PWSTR ps = NULL;
                            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &ps)) && ps) {
                                results.push_back(ps); CoTaskMemFree(ps);
                            }
                            it->Release();
                        }
                    }
                    arr->Release();
                }
            } else {
                IShellItem* it = NULL;
                if (SUCCEEDED(d->GetResult(&it)) && it) {
                    PWSTR ps = NULL;
                    if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &ps)) && ps) {
                        results.push_back(ps); CoTaskMemFree(ps);
                    }
                    it->Release();
                }
            }
        }
        d->Release();
    } else {
        IFileSaveDialog* d = NULL;
        if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d))) || !d)
            return XJS_PLUGIN_ERR_FAIL;
        d->SetOptions(FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_NOCHANGEDIR);
        if (!o.title.empty()) d->SetTitle(o.title.c_str());
        if (!o.initialName.empty()) d->SetFileName(o.initialName.c_str());
        HRESULT hr = d->Show(owner);
        if (SUCCEEDED(hr)) {
            IShellItem* it = NULL;
            if (SUCCEEDED(d->GetResult(&it)) && it) {
                PWSTR ps = NULL;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &ps)) && ps) {
                    results.push_back(ps); CoTaskMemFree(ps);
                }
                it->Release();
            }
        }
        d->Release();
    }
    std::string j;
    if (results.empty()) j = "{}";
    else if (results.size() == 1 && !multi) j = "{\"path\":" + PluginJsonStr(Utf16ToUtf8(results[0].c_str())) + "}";
    else j = "{\"paths\":" + PluginPathsJson(results) + "}";
    return PluginBufOut(buf, cap, j);
}

/* ==================== 宿主 API: 界面 (仅 UI 线程) ==================== */

static int FnSearchSetText(XjsPluginCtx* ctx, XjsWindowToken window, const char* kw, const char* mode, int execute) {
    XjsPluginEntry* p; int e;
    if ((e = PluginApiCheck(ctx, XPP_UI, true, &p)) != XJS_PLUGIN_OK) return e;
    XjsSearchWindow* w = PluginWindowOfToken(window);
    if (!w) return XJS_PLUGIN_ERR_NOTFOUND;
    XjsWindowScope scope(w);
    if (kw && *kw) {
        if (execute) XjsSearchSetText(Utf8ToUtf16(kw));   /* 置入即搜 (输入即搜口径) */
        else XjsSearchSetTextQuiet(Utf8ToUtf16(kw));      /* execute=0 只填不搜 (SDK 契约) */
    }
    else if (execute) XjsSearchNow(false);               /* 只重搜当前词 */
    (void)mode;   /* 模式切换预留 (v1 不接线) */
    return XJS_PLUGIN_OK;
}

static int FnOpenFile(XjsPluginCtx* ctx, XjsWindowToken window, int fileId, int reveal) {
    XjsPluginEntry* p; int e;
    if ((e = PluginApiCheck(ctx, XPP_UI, true, &p)) != XJS_PLUGIN_OK) return e;
    if (fileId < 0) return XJS_PLUGIN_ERR_ARG;
    XjsSearchWindow* w = PluginWindowOfToken(window);
    if (!w) return XJS_PLUGIN_ERR_NOTFOUND;
    XjsWindowScope scope(w);   /* 打开行为 (提权/异步/隐藏) 按令牌窗口的每窗设置 */
    const char* p8 = g_engine ? xjs_db_GetPath(g_engine, fileId) : NULL;   /* 路径宿主按 ID 自取 (v3 口径) */
    if (!p8 || !*p8) return XJS_PLUGIN_ERR_NOTFOUND;
    std::wstring pw = Utf8ToUtf16(p8);
    if (reveal) XjsOpenFolderAndSelect(pw); else XjsOpenFile(pw);
    return XJS_PLUGIN_OK;
}

static int FnShowMain(XjsPluginCtx* ctx) {
    XjsPluginEntry* p; int e;
    if ((e = PluginApiCheck(ctx, XPP_UI, true, &p)) != XJS_PLUGIN_OK) return e;
    HWND h = XjsSearchWindow::MainHwnd();
    if (!h) return XJS_PLUGIN_ERR_STATE;
    XjsSummonActivate(h);
    return XJS_PLUGIN_OK;
}

static int FnToast(XjsPluginCtx* ctx, XjsWindowToken window, const char* utf8, int type) {
    XjsPluginEntry* p; int e;
    if ((e = PluginApiCheck(ctx, 0, true, &p)) != XJS_PLUGIN_OK) return e;   /* 免权限 (照正式版 notify) */
    if (!utf8) return XJS_PLUGIN_ERR_ARG;
    XjsSearchWindow* w = PluginWindowOfToken(window);
    if (!w) return XJS_PLUGIN_ERR_NOTFOUND;
    XjsWindowScope scope(w);
    if (type < 0 || type > 3) type = 0;
    XjsToastShow(w->hWnd, Utf8ToUtf16(utf8).c_str(), type, XSF(1.0f));
    return XJS_PLUGIN_OK;
}

static int FnSummon(XjsPluginCtx* ctx, void* hwnd) {
    XjsPluginEntry* p; int e;
    if ((e = PluginApiCheck(ctx, 0, true, &p)) != XJS_PLUGIN_OK) return e;
    HWND h = (HWND)hwnd;
    if (!h || !IsWindow(h)) return XJS_PLUGIN_ERR_NOTFOUND;
    XjsSummonActivate(h);
    return XJS_PLUGIN_OK;
}

static void PluginHex(const XjsColor& c, char out[8]) {
    sprintf_s(out, 8, "#%02X%02X%02X",
              (int)(c.r * 255.f + 0.5f), (int)(c.g * 255.f + 0.5f), (int)(c.b * 255.f + 0.5f));
}

static int SkinJsonOfWindow(XjsSearchWindow* cur, char* buf, int cap) {
    if (!cur) return XJS_PLUGIN_ERR_NOTFOUND;
    XjsWindowScope scope(cur);   /* g_skin 是"当前窗皮肤"镜像: 先认回所属窗再取 */
    cur->SyncSkin();
    char b1[8], b2[8], panel[8], text[8], dim[8], accent[8], line[8];
    PluginHex(g_skin.bg1, b1); PluginHex(g_skin.bg2, b2); PluginHex(g_skin.panel, panel);
    PluginHex(g_skin.text, text); PluginHex(g_skin.textDim, dim);
    PluginHex(g_skin.accent, accent); PluginHex(g_skin.border, line);
    char j[512];
    sprintf_s(j, "{\"dpi\":%u,\"zoom\":%.2f,\"font\":\"Segoe UI\",\"fontCjk\":\"Microsoft YaHei UI\","
              "\"bg1\":\"%s\",\"bg2\":\"%s\",\"panel\":\"%s\",\"text\":\"%s\",\"dim\":\"%s\","
              "\"accent\":\"%s\",\"line\":\"%s\"}",
              XjsWindowDpi(cur->hWnd), cur->uiZoom / 10.0,
              b1, b2, panel, text, dim, accent, line);
    return PluginBufOut(buf, cap, j);
}

static int FnSkinJson(XjsPluginCtx* ctx, char* buf, int cap) {
    XjsPluginEntry* p; int e;
    if ((e = PluginApiCheck(ctx, 0, true, &p)) != XJS_PLUGIN_OK) return e;   /* 免权限: 只读色板 */
    XjsSearchWindow* cur = XjsSearchWindow::Alive(XjsSearchWindow::Cur()) ? XjsSearchWindow::Cur()
                                                                          : XjsSearchWindow::Main();
    return SkinJsonOfWindow(cur, buf, cap);
}

static int FnSkinJsonOf(XjsPluginCtx* ctx, XjsWindowToken window, char* buf, int cap) {
    XjsPluginEntry* p; int e;
    if ((e = PluginApiCheck(ctx, 0, true, &p)) != XJS_PLUGIN_OK) return e;
    return SkinJsonOfWindow(PluginWindowOfToken(window), buf, cap);   /* window=0 = 默认窗口 (PluginWindowOfToken 缺省) */
}

static int FnPrevBitmap(XjsPluginCtx* ctx, int requestId, int w, int h, const void* bgra, int stride) {
    XjsPluginEntry* p; int e;
    if ((e = PluginApiCheck(ctx, 0, true, &p)) != XJS_PLUGIN_OK) return e;
    /* 外部输入硬上限: 先限 w/h 再算 stride (先判 stride < w*4 时 w 大会符号溢出),
       总字节数再封顶 — 曾只挡下限, stride=h=大开可击出数百 GB 分配直接压垮宿主 */
    if (!bgra || w <= 0 || h <= 0 || w > 32768 || h > 32768) return XJS_PLUGIN_ERR_ARG;
    if (stride < w * 4) return XJS_PLUGIN_ERR_ARG;
    if ((long long)stride * (long long)h > 256LL * 1024 * 1024) return XJS_PLUGIN_ERR_ARG;
    return XjsPreviewPluginDeliverBitmap(requestId, w, h, bgra, stride) ? XJS_PLUGIN_OK : XJS_PLUGIN_ERR_STATE;
}
static int FnPrevText(XjsPluginCtx* ctx, int requestId, const char* utf8) {
    XjsPluginEntry* p; int e;
    if ((e = PluginApiCheck(ctx, 0, true, &p)) != XJS_PLUGIN_OK) return e;
    if (!utf8) return XJS_PLUGIN_ERR_ARG;
    return XjsPreviewPluginDeliverText(requestId, utf8) ? XJS_PLUGIN_OK : XJS_PLUGIN_ERR_STATE;
}

/* ==================== 宿主 API: 面板接管 (preview-panel 能力, v4) ==================== */

static int FnPanelOpen(XjsPluginCtx* ctx, XjsWindowToken window) {
    XjsPluginEntry* p; int e;
    if ((e = PluginApiCheck(ctx, 0, true, &p)) != XJS_PLUGIN_OK) return e;
    if (!(p->mf.caps & XPC_PANEL) || !p->fnOnPanelEvent) return XJS_PLUGIN_ERR_PERM;   /* 未声明面板能力不得开 */
    XjsSearchWindow* w = PluginWindowOfToken(window);
    if (!w) return XJS_PLUGIN_ERR_NOTFOUND;
    XjsWindowScope scope(w);
    return XjsPreviewPanelOpen(w, window, p->mf.id.c_str()) ? XJS_PLUGIN_OK : XJS_PLUGIN_ERR_STATE;
}

static int FnPanelClose(XjsPluginCtx* ctx, XjsWindowToken window) {
    XjsPluginEntry* p; int e;
    if ((e = PluginApiCheck(ctx, 0, true, &p)) != XJS_PLUGIN_OK) return e;
    XjsSearchWindow* w = PluginWindowOfToken(window);
    if (!w) return XJS_PLUGIN_ERR_NOTFOUND;
    if (!w->plugPanelOn || w->plugPanelPluginId != p->mf.id) return XJS_PLUGIN_ERR_STATE;   /* 不是本插件的会话 */
    XjsWindowScope scope(w);
    XjsPreviewPanelClose(w, window, true);   /* 插件主动关 = 正常收尾, 按打开前状态恢复预览 */
    return XJS_PLUGIN_OK;
}

static int FnPanelGetInfo(XjsPluginCtx* ctx, XjsWindowToken window, long long* serial, int* w, int* h, float* scale) {
    XjsPluginEntry* p; int e;
    if ((e = PluginApiCheck(ctx, 0, false, &p)) != XJS_PLUGIN_OK) return e;   /* 任意线程 (流式渲染前取尺寸) */
    XjsSearchWindow* win = PluginWindowOfToken(window);
    if (!win || !win->plugPanelOn || win->plugPanelPluginId != p->mf.id) {
        if (serial) *serial = 0;
        if (w) *w = 0;
        if (h) *h = 0;
        if (scale) *scale = 1.0f;
        return XJS_PLUGIN_ERR_STATE;
    }
    XjsPreviewPanelInfo(win, serial, w, h, scale);
    return XJS_PLUGIN_OK;
}

static int FnPanelDeliverBitmap(XjsPluginCtx* ctx, XjsWindowToken window, long long serial,
                                int w, int h, const void* bgra, int stride) {
    XjsPluginEntry* p; int e;
    if ((e = PluginApiCheck(ctx, 0, false, &p)) != XJS_PLUGIN_OK) return e;   /* 任意线程 (流式交付) */
    XjsSearchWindow* win = PluginWindowOfToken(window);
    if (!win || !win->plugPanelOn || win->plugPanelPluginId != p->mf.id) return XJS_PLUGIN_ERR_STATE;
    return XjsPreviewPanelDeliver(win, serial, w, h, bgra, stride) ? XJS_PLUGIN_OK : XJS_PLUGIN_ERR_STATE;
}

static int FnPanelSetFocus(XjsPluginCtx* ctx, XjsWindowToken window, int want) {
    XjsPluginEntry* p; int e;
    if ((e = PluginApiCheck(ctx, 0, true, &p)) != XJS_PLUGIN_OK) return e;
    XjsSearchWindow* win = PluginWindowOfToken(window);
    if (!win || !win->plugPanelOn || win->plugPanelPluginId != p->mf.id) return XJS_PLUGIN_ERR_STATE;
    XjsWindowScope scope(win);
    win->plugPanelKey = (want != 0);
    if (win->plugPanelKey) XjsSearchYieldKeys();   /* 键盘让给面板: 搜索框先交出路由/选区 */
    return XJS_PLUGIN_OK;
}

static int FnPanelSetCaret(XjsPluginCtx* ctx, XjsWindowToken window, int x, int y) {
    XjsPluginEntry* p; int e;
    if ((e = PluginApiCheck(ctx, 0, true, &p)) != XJS_PLUGIN_OK) return e;
    XjsSearchWindow* win = PluginWindowOfToken(window);
    if (!win || !win->plugPanelOn || win->plugPanelPluginId != p->mf.id) return XJS_PLUGIN_ERR_STATE;
    win->plugPanelCaretX = x;
    win->plugPanelCaretY = y;
    { XjsWindowScope scope(win); XjsPreviewPanelUpdateIme(win->hWnd); }
    return XJS_PLUGIN_OK;
}

/* ==================== 宿主 API: 事件 / 存储 / 日志 ==================== */

static int FnSubscribe(XjsPluginCtx* ctx, unsigned mask) {
    XjsPluginEntry* p; int e;
    if ((e = PluginApiCheck(ctx, 0, false, &p)) != XJS_PLUGIN_OK) return e;
    p->evtMask = mask;
    return XJS_PLUGIN_OK;
}

static bool PluginStorageKeyOk(const char* k) {
    /* 键 = UTF-8 任意文字 (中文主键口径, 2026-09-19; 键即 data\ 下的文件名):
       仅禁控制字符与 Windows 文件名禁用符, 长度 ≤128 字节 (约 42 个汉字)。
       另禁 Win32 路径规范化会折叠的形态 — 尾随点/空格 (写盘被剥) 与保留设备名
       (CON/NUL/COM1… 含 ".ext" 形态, 命中设备): 否则不同键落到同一文件/设备串数据 */
    if (!k) return false;
    size_t n = strlen(k);
    if (!n || n > 128) return false;
    if (!strcmp(k, ".") || !strcmp(k, "..")) return false;
    for (const char* c = k; *c; c++) {
        unsigned char ch = (unsigned char)*c;
        if (ch < 0x20 || ch == 0x7F) return false;                        /* 控制字符 */
        if (ch < 0x80 && strchr("\\/:*?\"<>|", (char)ch)) return false;   /* 路径/文件名禁用符 */
    }
    if (k[n - 1] == '.' || k[n - 1] == ' ') return false;                 /* 尾随点/空格 */
    static const char* const DEV[] = { "CON", "PRN", "AUX", "NUL",
        "COM1","COM2","COM3","COM4","COM5","COM6","COM7","COM8","COM9",
        "LPT1","LPT2","LPT3","LPT4","LPT5","LPT6","LPT7","LPT8","LPT9" };
    char base[5] = {};   /* 设备基名 (首个'.'前) 最长 4 字符且纯 ASCII; 超 = 不可能命中 */
    size_t bn = 0;
    while (bn < n && k[bn] != '.' && bn < 4) {
        unsigned char ch = (unsigned char)k[bn];
        if (ch >= 0x80) return true;                                      /* 非 ASCII 基名不可能是设备名 */
        base[bn++] = (char)((ch >= 'a' && ch <= 'z') ? ch - 0x20 : ch);
    }
    if (bn > 0 && bn <= 4 && (bn == n || k[bn] == '.'))
        for (const char* d : DEV) if (!strcmp(base, d)) return false;
    return true;
}

static int FnStorageGet(XjsPluginCtx* ctx, const char* key, char* buf, int cap) {
    XjsPluginEntry* p; int e;
    if ((e = PluginApiCheck(ctx, 0, false, &p)) != XJS_PLUGIN_OK) return e;   /* 免权限 (照正式版 storage) */
    if (!PluginStorageKeyOk(key)) return XJS_PLUGIN_ERR_ARG;
    std::string data;
    HANDLE h = CreateFileW((p->dir + L"\\data\\" + Utf8ToUtf16(key)).c_str(), GENERIC_READ,
                           FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return XJS_PLUGIN_ERR_NOTFOUND;
    char ch[4096]; DWORD rd = 0;
    while (ReadFile(h, ch, sizeof(ch), &rd, NULL) && rd) data.append(ch, rd);
    CloseHandle(h);
    return PluginBufOut(buf, cap, data);
}

static int FnStorageSet(XjsPluginCtx* ctx, const char* key, const char* data, int len) {
    XjsPluginEntry* p; int e;
    if ((e = PluginApiCheck(ctx, 0, false, &p)) != XJS_PLUGIN_OK) return e;
    if (!PluginStorageKeyOk(key) || len < 0 || (!data && len > 0)) return XJS_PLUGIN_ERR_ARG;
    std::wstring dir = p->dir + L"\\data";
    CreateDirectoryW(dir.c_str(), NULL);   /* 已存在 = 忽略 */
    EnterCriticalSection(&s_storageCs);
    std::wstring tmp = dir + L"\\" + Utf8ToUtf16(key) + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { LeaveCriticalSection(&s_storageCs); return XJS_PLUGIN_ERR_IO; }
    DWORD wr = 0;
    BOOL ok = TRUE;
    if (len > 0) ok = WriteFile(h, data, (DWORD)len, &wr, NULL) && wr == (DWORD)len;
    CloseHandle(h);
    /* 临时文件 + 替换: 半写不会损坏旧值 */
    if (ok) ok = MoveFileExW(tmp.c_str(), (dir + L"\\" + Utf8ToUtf16(key)).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    LeaveCriticalSection(&s_storageCs);
    return ok ? XJS_PLUGIN_OK : XJS_PLUGIN_ERR_IO;
}

static int FnStorageRemove(XjsPluginCtx* ctx, const char* key) {
    XjsPluginEntry* p; int e;
    if ((e = PluginApiCheck(ctx, 0, false, &p)) != XJS_PLUGIN_OK) return e;
    if (!PluginStorageKeyOk(key)) return XJS_PLUGIN_ERR_ARG;
    DeleteFileW((p->dir + L"\\data\\" + Utf8ToUtf16(key)).c_str());
    return XJS_PLUGIN_OK;
}

static void FnLog(XjsPluginCtx* ctx, int level, const char* utf8) {
    XjsPluginEntry* p = NULL;
    if (PluginApiCheck(ctx, 0, false, &p) != XJS_PLUGIN_OK || !p || !utf8) return;
    static const wchar_t* const LV[4] = { L"D", L"I", L"W", L"E" };
    if (level < 0 || level > 3) level = 1;
    std::wstring line = std::wstring(L"[xjs-plugin:") + p->mf.id + L"," + LV[level] + L"] " + Utf8ToUtf16(utf8);
    OutputDebugStringW(line.c_str());   /* 只进调试器, 不落盘 (用户口径: 日志落盘面从简) */
}

/* ==================== 宿主 API 函数表 (Init 发给插件, 指针终身有效) ==================== */

static const XjsPluginHost s_host = {
    XJS_PLUGIN_ABI_VERSION,
    sizeof(XjsPluginHost),
    FnPathInfo,
    FnReadFile,
    FnWriteFile,
    FnListDir,
    FnDeleteRecycle,
    FnMoveCopy,
    FnRenameTo,
    FnExec,
    FnClipGet,
    FnClipSet,
    FnDialog,
    FnSearchSetText,
    FnOpenFile,
    FnShowMain,
    FnToast,
    FnSummon,
    FnSkinJson,
    FnPrevBitmap,
    FnPrevText,
    FnSubscribe,
    FnStorageGet,
    FnStorageSet,
    FnStorageRemove,
    FnLog,
    FnPanelOpen,
    FnPanelClose,
    FnPanelGetInfo,
    FnPanelDeliverBitmap,
    FnPanelSetFocus,
    FnPanelSetCaret,
    FnSkinJsonOf,
};

static const XjsPluginHost* PluginHostTable() { return &s_host; }

/* ==================== 菜单追加 / 票号 ==================== */

static bool PluginMenuMatches(const XjsPluginMenuDef& d, const std::vector<int>& ids,
                              bool isDir, bool isDrive) {
    int kind = isDrive ? 3 : isDir ? 2 : 1;
    if (d.when == 1 && kind != 1) return false;
    if (d.when == 2 && kind != 2) return false;
    if (d.when == 3 && kind != 3) return false;
    if (!d.exts.empty() && d.exts[0] != L"*") {
        if (kind != 1) return false;   /* 目录/驱动器不匹配 ext (照正式版口径) */
        if (!g_engine) return false;
        for (int id : ids) {           /* 多选 = 全部文件都要命中扩展名 (名称经引擎按 ID 自取) */
            const char* name = xjs_db_GetName(g_engine, id);
            if (!name || !*name) return false;
            std::wstring wname = Utf8ToUtf16(name);
            std::wstring ext;
            size_t dot = wname.find_last_of(L'.');
            if (dot != std::wstring::npos) ext = wname.substr(dot + 1);
            for (auto& c : ext) c = towlower(c);
            bool hit = false;
            for (auto& x : d.exts) if (x == ext) { hit = true; break; }
            if (!hit) return false;
        }
    }
    return true;
}

/* 构建插件菜单项区: 单插件 = 分隔线 + 项; 多插件 = 分隔线 + 每插件 header 小字分组。
   项 = manifest 静态菜单 + 动态菜单 (BuildMenu 导出, 同步询问合并) 按 order 稳定排序。
   项 id = IDM_PLUGIN_BASE+票号; 一次菜单会话先清票号表再登记 (防启停/动态菜单下标漂移)。
   文件上下文 = 引擎 FileId 数组 (v3 口径: 宿主不传路径, 插件直连引擎自取) */
static void PluginAppendMenuItems(std::vector<XjsPopupItem>& items,
                                  const std::vector<int>& ids, bool isDir, bool isDrive,
                                  bool searchBox, unsigned long long window, const std::wstring& inputText) {
    if (s_plugins.empty() || (ids.empty() && !searchBox)) return;
    struct Pick { int plugin; std::wstring cmd, text; int order; };
    std::vector<Pick> picks;
    for (int pi = 0; pi < (int)s_plugins.size(); pi++) {
        const XjsPluginEntry& e = s_plugins[pi];
        if (!PluginActive(e)) continue;
        unsigned cap = searchBox ? XPC_SEARCHBOXMENU : XPC_FILECTX;
        if (!(e.mf.caps & cap)) continue;
        const std::vector<XjsPluginMenuDef>& list = searchBox ? e.mf.searchBoxMenus : e.mf.menus;
        for (auto& d : list)
            if (searchBox || PluginMenuMatches(d, ids, isDir, isDrive))
                picks.push_back({ pi, d.cmd, d.text, d.order });
        /* 动态菜单 (照正式版 buildMenu): 构建期同步询问插件, 返回项并入 (无导出/0 项 = 仅静态) */
        if (e.fnBuildMenu) {
            char buf[16384];
            int n = e.fnBuildMenu(e.ctx, searchBox ? "searchBox" : "fileCtx", window,
                                  searchBox ? NULL : ids.data(), searchBox ? 0 : (int)ids.size(),
                                  searchBox ? Utf16ToUtf8(inputText.c_str()).c_str() : NULL,
                                  buf, (int)sizeof(buf));
            if (n > 0 && n < (int)sizeof(buf)) {
                std::vector<XjsPluginMenuDef> dyn;
                XjsPluginDynMenuParse(buf, &dyn);
                for (auto& d : dyn)
                    picks.push_back({ pi, d.cmd, d.text, d.order });
            }
        }
    }
    if (picks.empty()) return;
    std::stable_sort(picks.begin(), picks.end(),
                     [](const Pick& a, const Pick& b) { return a.order < b.order; });
    if (!items.empty()) items.push_back({ 0, L"", L"", false, true });   /* 分隔线 */
    int lastPlugin = -1, distinct = 0;
    for (auto& k : picks) if (k.plugin != lastPlugin) { distinct++; lastPlugin = k.plugin; }
    lastPlugin = -1;
    for (auto& k : picks) {
        const XjsPluginEntry& e = s_plugins[k.plugin];
        if (distinct > 1 && k.plugin != lastPlugin) {   /* 多插件分组小字 (无悬停不可点) */
            items.push_back({ 0, e.mf.name, L"", false, false, true });
            lastPlugin = k.plugin;
        }
        if ((int)s_tickets.size() >= XJS_TICKET_MAX) break;
        s_tickets.push_back({ k.plugin, Utf16ToUtf8(k.cmd.c_str()), window, ids,
                              searchBox ? XJS_PLUGIN_CTX_NONE : (isDrive ? XJS_PLUGIN_CTX_DRIVE
                                                         : isDir ? XJS_PLUGIN_CTX_DIR : XJS_PLUGIN_CTX_FILE) });
        XjsPopupItem it;
        it.id = IDM_PLUGIN_BASE + (int)s_tickets.size() - 1;
        it.title = k.text;
        items.push_back(it);
    }
}

void XjsPluginAppendFileMenuItems(std::vector<XjsPopupItem>& items,
                                  const std::vector<int>& ids, bool isDir, bool isDrive) {
    s_tickets.clear();
    PluginAppendMenuItems(items, ids, isDir, isDrive, false, PluginTokenOf(XjsSearchWindow::Cur()->hWnd), L"");
}

void XjsPluginAppendSearchBoxMenuItems(std::vector<XjsPopupItem>& items, const std::wstring& inputText) {
    s_tickets.clear();
    PluginAppendMenuItems(items, {}, false, false, true, PluginTokenOf(XjsSearchWindow::Cur()->hWnd), inputText);
}

void XjsPluginOnMenuTicket(int slot) {
    if (slot < 0 || slot >= (int)s_tickets.size()) return;
    XjsPluginTicket t = s_tickets[slot];   /* 值拷贝: OnCommand 里可能再开菜单 (票号表会被重建) */
    if (t.plugin < 0 || t.plugin >= (int)s_plugins.size()) return;
    XjsPluginEntry& e = s_plugins[t.plugin];
    if (!PluginActive(e) || !e.fnOnCommand) return;
    static wchar_t ph[72];
    swprintf(ph, 72, L"plugin-call:%s", e.mf.id.c_str());
    XjsSetPhase(ph);
    e.fnOnCommand(e.ctx, t.cmd.c_str(), t.window,
                  t.ids.empty() ? NULL : t.ids.data(), (int)t.ids.size(), t.kind);
    XjsSetPhase(L"plugin-call:done");
}

bool XjsPluginActiveCap(unsigned capMask) {
    if (s_plugins.empty()) return false;   /* 零插件零开销短路 */
    for (auto& e : s_plugins)
        if (PluginActive(e) && (e.mf.caps & capMask)) return true;
    return false;
}

unsigned long long XjsPluginCurWindowToken() {
    XjsSearchWindow* cur = XjsSearchWindow::Cur();
    return (cur && cur->hWnd) ? PluginTokenOf(cur->hWnd) : 0;
}

/* ==================== P1: 搜索模式 / 输入拦截 / 状态栏 / 事件 ==================== */

int XjsPluginModeCount() {
    int n = 0;
    for (auto& e : s_plugins)
        if (PluginActive(e) && (e.mf.caps & XPC_SEARCHMODES)) n += (int)e.mf.modes.size();
    return n;
}
bool XjsPluginModeAt(int i, XjsPluginModeRef* out) {
    if (!out) return false;
    for (int pi = 0; pi < (int)s_plugins.size(); pi++) {
        const XjsPluginEntry& e = s_plugins[pi];
        if (!PluginActive(e) || !(e.mf.caps & XPC_SEARCHMODES)) continue;
        if (i < (int)e.mf.modes.size()) { *out = { pi, i }; return true; }
        i -= (int)e.mf.modes.size();
    }
    return false;
}
const XjsPluginModeDef* XjsPluginModeDefAt(const XjsPluginModeRef& r) {
    if (r.plugin < 0 || r.plugin >= (int)s_plugins.size() || r.modeIdx < 0) return NULL;
    const XjsPluginEntry& e = s_plugins[r.plugin];
    if (!PluginActive(e) || r.modeIdx >= (int)e.mf.modes.size()) return NULL;   /* 已禁用/已消失 = NULL */
    return &e.mf.modes[r.modeIdx];
}
void XjsPluginFireSearchMode(const XjsPluginModeRef& r, unsigned long long window, const std::wstring& input) {
    const XjsPluginModeDef* d = XjsPluginModeDefAt(r);
    if (!d) return;
    XjsPluginEntry& e = s_plugins[r.plugin];
    if (!e.fnOnSearchMode) return;
    std::string modeId = Utf16ToUtf8(d->id.c_str());
    e.fnOnSearchMode(e.ctx, modeId.c_str(), window, Utf16ToUtf8(input.c_str()).c_str());
}

static int s_interceptDepth = 0;   /* 拦截重入闸: 插件 OnInput 内再触发搜索 (SearchSetText) 不再二次询问 */

bool XjsPluginInputIntercept(const std::wstring& text) {
    if (s_plugins.empty() || !PluginUiThread() || s_interceptDepth > 0) return false;
    std::string inUtf8 = Utf16ToUtf8(text.c_str());
    unsigned long long tok = PluginTokenOf(XjsSearchWindow::Cur()->hWnd);
    for (auto& e : s_plugins) {
        if (!PluginActive(e) || !(e.mf.caps & XPC_INPUTINTERCEPT) || !e.fnOnInput) continue;
        ++s_interceptDepth;
        int r = e.fnOnInput(e.ctx, tok, inUtf8.c_str());
        --s_interceptDepth;
        if (r == 1) return true;   /* 首个返回 1 的插件接管本次搜索 */
    }
    return false;
}

int XjsPluginStatusBarCount() {
    int n = 0;
    for (auto& e : s_plugins)
        if (PluginActive(e) && (e.mf.caps & XPC_STATUSBAR)) n += (int)e.mf.statusBar.size();
    return n;
}
bool XjsPluginStatusBarAt(int i, XjsPluginStatusBarDef* out) {
    for (int pi = 0; pi < (int)s_plugins.size(); pi++) {
        const XjsPluginEntry& e = s_plugins[pi];
        if (!PluginActive(e) || !(e.mf.caps & XPC_STATUSBAR)) continue;
        if (i < (int)e.mf.statusBar.size()) { *out = e.mf.statusBar[i]; return true; }
        i -= (int)e.mf.statusBar.size();
    }
    return false;
}
void XjsPluginStatusBarCommand(int i, unsigned long long window) {
    for (int pi = 0; pi < (int)s_plugins.size(); pi++) {
        XjsPluginEntry& e = s_plugins[pi];
        if (!PluginActive(e) || !(e.mf.caps & XPC_STATUSBAR)) continue;
        if (i >= (int)e.mf.statusBar.size()) { i -= (int)e.mf.statusBar.size(); continue; }
        if (!e.fnOnCommand) continue;   /* 声明了 statusBar 但无 OnCommand 导出: 跳过该插件继续找 (return 会吞掉后续插件的项) */
        std::string cmd = Utf16ToUtf8(e.mf.statusBar[i].cmd.c_str());
        e.fnOnCommand(e.ctx, cmd.c_str(), window, NULL, 0, XJS_PLUGIN_CTX_NONE);
        return;
    }
}

/* 订阅事件派发 — 纯信号口径 (v2): 只传 (事件类型, 窗口令牌, 结果对象), 不打包任何数据。
   曾经的 selectionChanged 把全部选中打成 paths JSON: 全选 451 万 = 451 万次索引反查 +
   逐行路径查询 + 几百 MB 字符串拼接, 且构建发生在订阅过滤之前 (没订阅也照拼) = 全选卡顿
   真凶; 结果对象本来就是引擎对象, 插件直连 xunjieso 自取 (GetSelectedCount/CopySelectedFileId…) */
void XjsPluginOnSearchComplete() {
    if (s_plugins.empty()) return;
    unsigned long long tok = PluginTokenOf(XjsSearchWindow::Cur()->hWnd);
    for (auto& e : s_plugins)
        if (PluginActive(e) && (e.evtMask & XJS_PLUGIN_EVT_SEARCH_COMPLETE) && e.fnOnEvent)
            e.fnOnEvent(e.ctx, XJS_PLUGIN_EVT_SEARCH_COMPLETE, tok, g_result);
}
void XjsPluginOnSelectionChanged() {
    if (s_plugins.empty()) return;
    /* 订阅者预检: 没人订阅 selection 事件 = 零成本返回 (取令牌都不必) */
    bool any = false;
    for (auto& e : s_plugins)
        if (PluginActive(e) && (e.evtMask & XJS_PLUGIN_EVT_SELECTION)) { any = true; break; }
    if (!any) return;
    unsigned long long tok = PluginTokenOf(XjsSearchWindow::Cur()->hWnd);
    for (auto& e : s_plugins)
        if (PluginActive(e) && (e.evtMask & XJS_PLUGIN_EVT_SELECTION) && e.fnOnEvent)
            e.fnOnEvent(e.ctx, XJS_PLUGIN_EVT_SELECTION, tok, g_result);
}
void XjsPluginOnSyncAfter() {
    if (s_plugins.empty()) return;
    for (auto& e : s_plugins)   /* 进程级事件: 文件同步影响全部窗口, window=0/result=NULL */
        if (PluginActive(e) && (e.evtMask & XJS_PLUGIN_EVT_SYNC) && e.fnOnEvent)
            e.fnOnEvent(e.ctx, XJS_PLUGIN_EVT_SYNC, 0, NULL);
}

void XjsPluginOnSkinChanged(unsigned long long windowToken) {
    if (s_plugins.empty()) return;
    for (auto& e : s_plugins)   /* 纯信号: 只报哪个窗口换了皮肤, 新配色插件经 GetSkinJsonOf 自取 */
        if (PluginActive(e) && (e.evtMask & XJS_PLUGIN_EVT_SKIN) && e.fnOnEvent)
            e.fnOnEvent(e.ctx, XJS_PLUGIN_EVT_SKIN, windowToken, NULL);
}

/* ==================== P2: 预览接管 / 批量重命名 ==================== */

/* 预览接管: 只传 (requestId, 窗口令牌, fileId) — 路径/名称/扩展名插件经 xjs_db_GetPath/GetName
   自取 (v3 口径); manifest 扩展名过滤仍由宿主用名称完成 (不命中不回调) */
bool XjsPluginPreviewTake(int fileId, int requestId, unsigned long long window) {
    if (s_plugins.empty() || !PluginUiThread()) return false;
    /* 清单 "预览.扩展名" 过滤 (宿主侧执行, 不命中不回调 — 见开发指南"接管的扩展名"):
       按文件名扩展名 (小写无点, 与收录口径一致) 比对; 空表 = 未声明, 保持原行为全回调 */
    std::wstring ext;
    {
        const char* nm = g_engine ? xjs_db_GetName(g_engine, fileId) : NULL;
        if (nm && *nm) {
            std::wstring name = Utf8ToUtf16(nm);
            size_t dot = name.find_last_of(L'.');
            if (dot != std::wstring::npos && dot + 1 < name.size())
                ext = name.substr(dot + 1);
            for (auto& c : ext) c = towlower(c);
        }
    }
    for (auto& e : s_plugins) {
        if (!PluginActive(e) || !(e.mf.caps & XPC_PREVIEW) || !e.fnOnPreview) continue;
        if (!e.mf.previewExts.empty()) {
            if (ext.empty()) continue;   /* 声明了扩展名表而目标无扩展名 (目录/无后缀) = 不命中 */
            bool hit = false;
            for (auto& x : e.mf.previewExts) if (x == ext) { hit = true; break; }
            if (!hit) continue;
        }
        if (e.fnOnPreview(e.ctx, requestId, window, fileId) == 1) return true;   /* 已接管, 异步交付 */
    }
    return false;
}

bool XjsPluginBatchRenameAvailable() {
    if (!XjsPluginActiveCap(XPC_BATCHRENAME)) return false;
    for (auto& e : s_plugins)
        if (PluginActive(e) && (e.mf.caps & XPC_BATCHRENAME) && e.fnOnCommand) return true;
    return false;
}
void XjsPluginBatchRename(const std::vector<int>& ids, unsigned long long window) {
    for (auto& e : s_plugins) {
        if (!PluginActive(e) || !(e.mf.caps & XPC_BATCHRENAME) || !e.fnOnCommand) continue;
        e.fnOnCommand(e.ctx, "batchRename", window,   /* 约定命令 id (见开发指南) */
                      ids.empty() ? NULL : ids.data(), (int)ids.size(), XJS_PLUGIN_CTX_FILE);
        return;   /* 多个接管插件取首个 (管理页顺序) */
    }
}

/* ==================== P3: 面板接管 (preview-panel) ==================== */

/* 面板事件派发 (xjs_preview.cpp 转发路径的唯一出口): 按会话 owner 插件 id 找插件 (重扫换槽
   不串), 打包 SDK 事件回调 OnPanelEvent。形参 = 纯 C 值 (SDK 头只有本文件 include, 红线不破) */
void XjsPluginPanelDispatch(unsigned long long window, int type, long long serial,
                            int w, int h, float scale, int x, int y, int delta,
                            unsigned flags, unsigned ch) {
    if (s_plugins.empty()) return;
    XjsSearchWindow* win = PluginWindowOfToken(window);
    if (!win || !win->plugPanelOn || win->plugPanelPluginId.empty()) return;
    for (auto& e : s_plugins) {
        if (!PluginActive(e) || !(e.mf.caps & XPC_PANEL)) continue;
        if (e.mf.id != win->plugPanelPluginId || !e.fnOnPanelEvent) continue;
        XjsPanelEvent ev{};
        ev.structSize = sizeof(ev);
        ev.type = type;
        ev.serial = serial;
        ev.w = w;
        ev.h = h;
        ev.scale = scale;
        ev.x = x;
        ev.y = y;
        ev.delta = delta;
        ev.flags = flags;
        ev.ch = ch;
        static wchar_t ph[72];
        swprintf(ph, 72, L"plugin-panel:%s", e.mf.id.c_str());
        XjsSetPhase(ph);
        e.fnOnPanelEvent(e.ctx, window, &ev);
        XjsSetPhase(L"plugin-panel:done");
        return;
    }
}

/* owner 失效校验 (禁用/重扫后调): 接管会话的插件已禁用/消失 → 结束会话并恢复预览。
   ForEach 回调无捕获, 用文件级静态传参 (鼠标互斥类瞬态同口径, 单线程 UI) */
static void XjsPluginPanelValidateOne(XjsSearchWindow* w) {
    if (!w->plugPanelOn) return;
    for (auto& e : s_plugins)
        if (PluginActive(e) && (e.mf.caps & XPC_PANEL) && e.mf.id == w->plugPanelPluginId) return;   /* owner 仍在 */
    XjsPreviewPanelClose(w, PluginTokenOf(w->hWnd), true);
}
void XjsPluginPanelValidateOwners() {
    XjsSearchWindow::ForEach(&XjsPluginPanelValidateOne);   /* 空注册表时所有会话都视为 owner 失效 */
}
