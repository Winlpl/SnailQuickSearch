/*
 * open_folders.cpp — 蜗牛快搜(D2D) 插件「打开的文件夹」
 * 形态 = manifest.json + 本 DLL (type "library", 无自建界面): 后台监听 + 运行时搜索模式。
 *
 * 做什么:
 *   监听所有打开着的 Windows 资源管理器窗口 (IShellWindows 枚举, Win11 标签页每个标签
 *   是独立条目; 按 CabinetWClass 过滤掉桌面等非文件管理器宿主), 取每扇窗当前位置的
 *   真实目录路径 (IShellBrowser→活动视图→IPersistFolder2::GetCurFolder, 虚拟位置如
 *   此电脑/回收站取不出路径自动跳过) 与标题 (IWebBrowser2::LocationName)。
 *
 * 怎么联动 (全部复用宿主既有链路, 宿主零改动):
 *   每个目录注册一个**会话级通配符模板模式** (扩展 API modes.add, 模式名=目录末段名,
 *   简介=完整路径, 模板=路径通配符词「路径\*」含空格包双引号, 与宿主右键文件夹标签同款
 *   执行词)。用户在搜索框完整输入目录名/窗口标题并提交时, 宿主"输入词命中模式名→转
 *   托管标签"链路自动把它变成搜索框标签 —— 标签阶段即把本次搜索限定在该目录内, 之后的
 *   输入作为链尾阶段过滤 (XJS_KEYWORD_MULTI 链式); 同名目录多个窗口 = 同名多来源标签,
 *   悬停标签可切换是哪个文件夹, 点 × 解除限定。窗口关闭/导航离开 → 撤销对应模式
 *   (modes.remove, 宿主自动剔除引用它的标签并重搜)。
 *
 * 线程模型:
 *   Init (UI 线程) 建一个 message-only 通知窗 + 启动后台线程; 后台线程 MTA 每 ~0.9s
 *   枚举一次 ShellWindows, 快照变化才 PostMessage 通知 UI 线程; UI 线程在通知窗过程里
 *   对账注册表 (modes.add/remove 仅 UI 线程可调)。Shutdown 里停线程 join、撤窗。
 */
#include <windows.h>
#include <objbase.h>
#include <shlguid.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <exdisp.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>

#include "../../xjs_plugin_sdk.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

/* ==================== 基础工具 ==================== */

static XjsPluginCtx*  g_ctx  = NULL;
static const XjsPluginHost* g_host = NULL;
static XjsApiModesAdd    g_add = NULL;
static XjsApiModesRemove g_rem = NULL;

static void OfLog(int level, const char* msg) {
    if (g_host) g_host->Log(g_ctx, level, msg);
}

static std::wstring OfUtf16(const char* s) {
    std::wstring w;
    if (!s || !*s) return w;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n > 1) {
        w.resize((size_t)n - 1);
        MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    }
    return w;
}

static std::string OfUtf8(const std::wstring& w) {
    std::string s;
    if (w.empty()) return s;
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    if (n > 0) {
        s.resize((size_t)n);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    }
    return s;
}

/* JSON 字符串字面量转义 (路径里的 " 与 \ 必转; 控制字符理论上出现不了, 兜底 \u00XX) */
static std::string OfJsonEsc(const std::string& utf8) {
    static const char* const HEX = "0123456789abcdef";
    std::string out;
    out.reserve(utf8.size() + 8);
    for (size_t i = 0; i < utf8.size(); i++) {
        unsigned char c = (unsigned char)utf8[i];
        if (c == '"') { out += "\\\""; continue; }
        if (c == '\\') { out += "\\\\"; continue; }
        if (c < 0x20) {
            out += "\\u00";
            out += HEX[(c >> 4) & 0xF];
            out += HEX[c & 0xF];
            continue;
        }
        out += (char)c;
    }
    return out;
}

static std::wstring OfTrim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return std::wstring();
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

/* ==================== 路径/词法 (与宿主右键文件夹标签同款口径) ==================== */

/* 目录名末段 (剥尾斜杠取最后一段; 根目录 "C:\" → "C:") */
static std::wstring OfLeafName(const std::wstring& dir) {
    std::wstring leaf = dir;
    while (!leaf.empty() && leaf.back() == L'\\') leaf.pop_back();
    size_t bs = leaf.find_last_of(L'\\');
    if (bs != std::wstring::npos) leaf = leaf.substr(bs + 1);
    if (leaf.empty()) leaf = dir;
    return leaf;
}

/* 路径归一: 去多余尾斜杠, 盘根 "C:\" 保留 */
static std::wstring OfNormDir(std::wstring p) {
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    return p;
}

/* 执行词 = 完整路径通配符 (路径+\*; 根路径已带 \ 不重复补; 含空格包双引号 —— 引擎按
   空格分"且", 引号内为一段)。与宿主右键文件夹标签的执行词完全同款。 */
static std::wstring OfFolderWord(const std::wstring& dir) {
    std::wstring w = dir;
    if (!w.empty() && w.back() == L'\\') w += L'*';
    else w += L"\\*";
    if (dir.find(L' ') != std::wstring::npos) w = L"\"" + w + L"\"";
    return w;
}

/* 标题别名模式资格: 非路径样标题 (含 \ / : 的标题 = "显示完整路径"设置, 用户直接输路径
   原生就按路径匹配, 不重复注册) 且与目录名不同 (匹配忽略大小写, 仅大小写不同=同词) */
static bool OfAliasOk(const struct OfEntry& e);

/* ==================== 快照条目 ==================== */

struct OfEntry {
    std::wstring path;    /* 归一目录路径 (去重键) */
    std::wstring name;    /* 模式名 = 目录末段名 */
    std::wstring title;   /* 窗口标题 (首个该路径窗口的; 与 name 不同才作别名模式) */
    std::wstring word;    /* 模板执行词 (路径通配符) */
};

static bool OfNameOk(const std::wstring& n) {
    std::wstring t = OfTrim(n);
    return !t.empty() && t.size() <= 64;   /* 宿主 modes.add 限制: 名称 ≤64 宽字符 */
}

static bool OfAliasOk(const OfEntry& e) {
    if (e.title.size() > 64) return false;
    std::wstring t = OfTrim(e.title);
    if (t.empty()) return false;
    if (t.find_first_of(L"\\/:") != std::wstring::npos) return false;
    if (_wcsicmp(t.c_str(), OfTrim(e.name).c_str()) == 0) return false;
    return true;
}

static bool OfEntryLess(const OfEntry& a, const OfEntry& b) {
    return _wcsicmp(a.path.c_str(), b.path.c_str()) < 0;
}

static bool OfEntrySame(const OfEntry& a, const OfEntry& b) {
    return _wcsicmp(a.path.c_str(), b.path.c_str()) == 0 &&
           a.name == b.name && a.title == b.title && a.word == b.word;
}

/* 两份快照是否完全一致 (均已按 path 排序) */
static bool OfSetSame(const std::vector<OfEntry>& a, const std::vector<OfEntry>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (!OfEntrySame(a[i], b[i])) return false;
    return true;
}

/* ==================== 资源管理器枚举 (后台线程, MTA) ==================== */

/* IShellBrowser → 活动视图 → 当前文件夹 PIDL → 文件系统路径; 虚拟位置/异常 = 空串 */
static std::wstring OfShellFolderPath(IWebBrowser2* wb) {
    std::wstring path;
    IServiceProvider* sp = NULL;
    if (FAILED(wb->QueryInterface(IID_IServiceProvider, (void**)&sp)) || !sp) return path;
    IShellBrowser* sb = NULL;
    if (SUCCEEDED(sp->QueryService(SID_STopLevelBrowser, IID_IShellBrowser, (void**)&sb)) && sb) {
        IShellView* sv = NULL;
        if (SUCCEEDED(sb->QueryActiveShellView(&sv)) && sv) {
            IFolderView* fv = NULL;
            if (SUCCEEDED(sv->QueryInterface(IID_IFolderView, (void**)&fv)) && fv) {
                IPersistFolder2* pf = NULL;
                if (SUCCEEDED(fv->GetFolder(IID_IPersistFolder2, (void**)&pf)) && pf) {
                    PIDLIST_ABSOLUTE pidl = NULL;
                    if (SUCCEEDED(pf->GetCurFolder(&pidl)) && pidl) {
                        wchar_t buf[1024];
                        if (SHGetPathFromIDListEx(pidl, buf, 1024, GPFIDL_DEFAULT)) {
                            DWORD attrs = GetFileAttributesW(buf);
                            /* zip 等以文件形式打开的位置不注册 (搜索引擎进不去压缩包内部);
                               属性取不到 (网络位置暂不可达) 时保留, 搜索仍可命中索引内旧数据 */
                            if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY))
                                path = OfNormDir(buf);
                        }
                        ILFree(pidl);
                    }
                    pf->Release();
                }
                fv->Release();
            }
            sv->Release();
        }
        sb->Release();
    }
    sp->Release();
    return path;
}

/* 枚举全部打开着的资源管理器窗口, 按 path 去重 (多标签/多窗同目录=一条), 结果按 path 排序。
   枚举失败 (shell COM 暂不可用) 返回 false, 保留上一份快照不动。 */
static bool OfEnumerate(std::vector<OfEntry>* out) {
    IShellWindows* sw = NULL;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, NULL, CLSCTX_ALL, IID_IShellWindows, (void**)&sw)) || !sw)
        return false;
    std::vector<OfEntry> list;
    long n = 0;
    sw->get_Count(&n);
    for (long i = 0; i < n; i++) {
        VARIANT vi;
        VariantInit(&vi);
        vi.vt = VT_I4;
        vi.lVal = i;
        IDispatch* d = NULL;
        if (FAILED(sw->Item(vi, &d)) || !d) { VariantClear(&vi); continue; }
        VariantClear(&vi);
        IWebBrowser2* wb = NULL;
        if (SUCCEEDED(d->QueryInterface(IID_IWebBrowser2, (void**)&wb)) && wb) {
            SHANDLE_PTR hp = 0;
            wb->get_HWND(&hp);
            wchar_t cls[64] = L"";
            bool isExplorer = hp &&
                              GetClassNameW((HWND)hp, cls, 64) > 0 &&
                              wcscmp(cls, L"CabinetWClass") == 0;   /* 桌面等其它 shell 宿主剔除 */
            if (isExplorer) {
                std::wstring path = OfShellFolderPath(wb);
                if (!path.empty()) {
                    OfEntry e;
                    e.path = path;
                    e.name = OfLeafName(path);
                    BSTR b = NULL;
                    if (SUCCEEDED(wb->get_LocationName(&b)) && b) {
                        e.title = b;   /* 每个标签自己的标题 (Win11 多标签同窗不同名) */
                        SysFreeString(b);
                    }
                    e.word = OfFolderWord(path);
                    bool dup = false;
                    for (size_t k = 0; k < list.size(); k++)
                        if (_wcsicmp(list[k].path.c_str(), path.c_str()) == 0) { dup = true; break; }
                    if (!dup) list.push_back(e);
                }
            }
            wb->Release();
        }
        d->Release();
    }
    sw->Release();
    std::sort(list.begin(), list.end(), OfEntryLess);
    *out = list;
    return true;
}

/* ==================== 注册表对账 (UI 线程) ==================== */

struct OfReg {
    std::wstring path;
    std::wstring name;
    std::wstring title;
    std::vector<std::string> ids;   /* modes.add 回传的来源 id (UTF-8) */
};

static std::vector<OfReg> g_reg;   /* UI 线程私有 */

static const UINT WM_OF_CHANGE = WM_APP + 1;   /* 后台快照变化 → UI 对账 (只 poke, 数据走 g_desired) */

static CRITICAL_SECTION g_cs;
static std::vector<OfEntry> g_desired;   /* g_cs 保护: 后台写, UI 读 */
static HWND g_msgHwnd = NULL;            /* g_cs 保护: 后台读 (PostMessage 目标) */
static HANDLE g_stopEvent = NULL;
static HANDLE g_thread = NULL;

/* UTF-8 按字节截断到 ≤cap, 不劈开多字节序列 (简介进宿主 modes.add 有 256 字节上限,
   深层中文目录的 UTF-8 路径可能超) */
static std::string OfUtf8Clamp(const std::string& utf8, size_t cap) {
    if (utf8.size() <= cap) return utf8;
    size_t n = cap;
    while (n > 0 && ((unsigned char)utf8[n] & 0xC0) == 0x80) n--;   /* 回退续字节 */
    return utf8.substr(0, n);
}

/* 向宿主注册一个运行时模式; 成功回传来源 id */
static bool OfAddMode(const std::wstring& name, const std::wstring& pathWordDesc,
                      const std::wstring& tpl, std::string* idOut) {
    if (!g_add || !OfNameOk(name)) return false;
    std::string tplU8 = OfUtf8(tpl);
    if (tplU8.empty() || tplU8.size() > 2048) return false;   /* 宿主 modes.add 模板上限 */
    std::string j = "{\"名称\":\"" + OfJsonEsc(OfUtf8(OfTrim(name))) +
                    "\",\"简介\":\"" + OfJsonEsc(OfUtf8Clamp(OfUtf8(pathWordDesc), 256)) +
                    "\",\"类型\":\"wildcard\",\"模板\":\"" + OfJsonEsc(tplU8) + "\"}";
    char buf[320];
    int n = g_add(g_ctx, 0, j.c_str(), buf, (int)sizeof(buf));
    if (n <= 0 || n >= (int)sizeof(buf)) return false;   /* 负值=错误码; 超缓冲=未完整写入 */
    buf[n] = 0;
    /* 回包 = 宿主生成的 {"标识":"p:open-folders:<序>"} — 取值段 (冒号后的第一对引号) */
    const char* colon = strchr(buf, ':');
    if (!colon) return false;
    const char* q1 = strchr(colon, '"');
    if (!q1) return false;
    const char* q2 = strchr(q1 + 1, '"');
    if (!q2) return false;
    *idOut = std::string(q1 + 1, q2 - q1 - 1);
    return !idOut->empty();
}

/* UI 线程: 拿最新快照对账注册表 — 新目录注册模式; 消失/改名 (重命名目录) 的撤销模式 */
static void OfReconcile() {
    if (!g_add || !g_rem) return;
    std::vector<OfEntry> want;
    EnterCriticalSection(&g_cs);
    want = g_desired;
    LeaveCriticalSection(&g_cs);

    for (size_t i = g_reg.size(); i-- > 0;) {
        OfReg& r = g_reg[i];
        const OfEntry* hit = NULL;
        for (size_t k = 0; k < want.size(); k++)
            if (_wcsicmp(want[k].path.c_str(), r.path.c_str()) == 0) { hit = &want[k]; break; }
        if (hit && hit->name == r.name && hit->title == r.title) continue;   /* 未变, 保留 */
        for (size_t s = 0; s < r.ids.size(); s++)
            g_rem(g_ctx, r.ids[s].c_str());   /* 宿主自动剔除引用该来源的标签并重搜 */
        g_reg.erase(g_reg.begin() + i);
    }
    for (size_t k = 0; k < want.size(); k++) {
        const OfEntry& e = want[k];
        bool have = false;
        for (size_t i = 0; i < g_reg.size(); i++)
            if (_wcsicmp(g_reg[i].path.c_str(), e.path.c_str()) == 0) { have = true; break; }
        if (have) continue;
        OfReg r;
        r.path = e.path;
        r.name = e.name;
        r.title = e.title;
        std::string id;
        if (OfAddMode(e.name, e.path, e.word, &id))
            r.ids.push_back(id);
        if (OfAliasOk(e) && OfAddMode(OfTrim(e.title), e.path, e.word, &id))
            r.ids.push_back(id);   /* 标题别名模式: 同路径第二个名字也能命中 */
        if (!r.ids.empty()) g_reg.push_back(r);
    }
}

/* ==================== 通知窗 (UI 线程) 与后台线程 ==================== */

static LRESULT CALLBACK OfMsgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_OF_CHANGE) {
        OfReconcile();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* 插件启停/重扫信号 → 自催一次对账: 禁用期间的快照变化注册不上 (闸门 ERR_PERM),
   再启用/重扫后靠这个信号把欠下的注册补齐, 不必等下一次资源管理器变化 */
extern "C" __declspec(dllexport) void XJS_PLUGIN_CALL XjsPlugin_OnEvent(XjsPluginCtx*, int eventType,
                                                                        XjsWindowToken window, void* result) {
    (void)window;
    (void)result;
    if (eventType != XJS_PLUGIN_EVT_PLUGINS) return;
    HWND poke = NULL;
    EnterCriticalSection(&g_cs);
    poke = g_msgHwnd;
    LeaveCriticalSection(&g_cs);
    if (poke) PostMessageW(poke, WM_OF_CHANGE, 0, 0);
}

static DWORD WINAPI OfWorkerProc(LPVOID) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool com = SUCCEEDED(hr);
    std::vector<OfEntry> last;
    bool hasLast = false;
    for (;;) {
        DWORD w = WaitForSingleObject(g_stopEvent, 900);
        if (w == WAIT_OBJECT_0) break;
        std::vector<OfEntry> now;
        if (!com || !OfEnumerate(&now)) continue;   /* 枚举失败保留旧快照 */
        if (hasLast && OfSetSame(now, last)) continue;
        last = now;
        hasLast = true;
        HWND poke = NULL;
        EnterCriticalSection(&g_cs);
        g_desired = now;
        poke = g_msgHwnd;
        LeaveCriticalSection(&g_cs);
        if (poke) PostMessageW(poke, WM_OF_CHANGE, 0, 0);
    }
    if (com) CoUninitialize();
    return 0;
}

/* ==================== 插件固定导出 ==================== */

extern "C" __declspec(dllexport) const XjsPluginInfo* XJS_PLUGIN_CALL XjsPlugin_GetInfo(void) {
    static const XjsPluginInfo info = { XJS_PLUGIN_ABI_VERSION, sizeof(XjsPluginInfo), "open-folders", "1.0.0" };
    return &info;
}

extern "C" __declspec(dllexport) int XJS_PLUGIN_CALL XjsPlugin_Init(XjsPluginCtx* ctx, const XjsPluginHost* host) {
    g_ctx = ctx;
    g_host = host;
    /* 本插件唯一职能 = 运行时搜索模式管理; 旧宿主没有扩展 API = 干净失败 */
    if (!host || host->size < offsetof(XjsPluginHost, QueryApi) + sizeof(void*))
        return XJS_PLUGIN_ERR_FAIL;
    g_add = (XjsApiModesAdd)host->QueryApi(ctx, XJS_API_MODES_ADD);
    g_rem = (XjsApiModesRemove)host->QueryApi(ctx, XJS_API_MODES_REMOVE);
    if (!g_add || !g_rem) return XJS_PLUGIN_ERR_FAIL;

    InitializeCriticalSection(&g_cs);
    g_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_stopEvent) { DeleteCriticalSection(&g_cs); return XJS_PLUGIN_ERR_FAIL; }

    WNDCLASSW wc;
    if (!GetClassInfoW(GetModuleHandleW(NULL), L"XjsOpenFoldersMsg", &wc)) {
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = OfMsgProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"XjsOpenFoldersMsg";
        RegisterClassW(&wc);
    }
    g_msgHwnd = CreateWindowExW(0, L"XjsOpenFoldersMsg", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL,
                                GetModuleHandleW(NULL), NULL);
    if (!g_msgHwnd) {
        CloseHandle(g_stopEvent);
        g_stopEvent = NULL;
        DeleteCriticalSection(&g_cs);
        return XJS_PLUGIN_ERR_FAIL;
    }
    g_thread = CreateThread(NULL, 0, OfWorkerProc, NULL, 0, NULL);
    if (!g_thread) {
        DestroyWindow(g_msgHwnd);
        g_msgHwnd = NULL;
        CloseHandle(g_stopEvent);
        g_stopEvent = NULL;
        DeleteCriticalSection(&g_cs);
        return XJS_PLUGIN_ERR_FAIL;
    }
    host->Subscribe(g_ctx, XJS_PLUGIN_EVT_PLUGINS);   /* 启停/重扫信号 → OfReconcile 自催 (OnEvent) */
    OfLog(1, "open-folders 1.0.0 initialized (watching Explorer windows -> runtime path-scope modes)");
    return XJS_PLUGIN_OK;
}

extern "C" __declspec(dllexport) void XJS_PLUGIN_CALL XjsPlugin_Shutdown(XjsPluginCtx* ctx) {
    (void)ctx;
    if (g_stopEvent) SetEvent(g_stopEvent);
    if (g_thread) {
        WaitForSingleObject(g_thread, 8000);   /* SDK 契约: Shutdown 里 join; 轮询周期有界 */
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    EnterCriticalSection(&g_cs);
    HWND h = g_msgHwnd;
    g_msgHwnd = NULL;
    LeaveCriticalSection(&g_cs);
    if (h) DestroyWindow(h);   /* 窗属 UI 线程, Shutdown 也在 UI 线程 */
    if (g_stopEvent) { CloseHandle(g_stopEvent); g_stopEvent = NULL; }
    /* 已注册的运行时模式不逐个 remove: 模式属主=插件 id, 宿主按属主管生命周期
       (禁用/重扫剪枝, 进程退出随会话消亡), 与"运行时模式会话级"口径一致 */
    DeleteCriticalSection(&g_cs);
}
