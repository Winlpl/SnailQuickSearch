/*
 * space_map.cpp — 蜗牛快搜(D2D) 插件示例「空间地图」(GDI+ 一比一复刻源样式)
 * 形态 = manifest.json + 本 DLL (type "app"): 自建窗口 + 纯 GDI+ 绘制, 宿主不参与窗口生命周期。
 * 源样式参照: CSS/JS 逐条对照, 值/几何/文案同源 (参照落盘位置记于会话记忆, 不写入源码)。
 * 数据源 = 引擎直连 (与宿主同进程 xunjieso.dll): xjs_db_GetFileIdByPath/GetChildrenIds/
 *          TraverseChildrenIds 递归回调统计 + GetFileSize — 索引内存库查询, 与源样式 dirChildren 同语义。
 * 演示点: ①清单 menus(dir/drive)+statusBar → OnCommand (状态栏项 = CTX_NONE 无文件上下文, 文件菜单项带 FileId)
 *         ②引擎直连只读查询(读锁) ③ui 权限宿主 API
 *         (OpenPath 定位 / GetSkinJson 皮肤 / Toast / Storage 记忆) ④自绘窗口全套: 标题栏/导航条/
 *         嵌套矩形树图/侧边栏评估面板/图例/菜单/Tooltip/OLE 拖出拖入/多选框选。
 */
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <shlobj.h>
#include <gdiplus.h>
#include <shlwapi.h>
#include <winhttp.h>
#include <imm.h>
#include <dwmapi.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <atomic>
#include <thread>
#include <functional>
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../../xjs_plugin_sdk.h"   /* 插件 SDK (纯 C ABI) */
#include "../../xunjieso.h"         /* 引擎直连: 与宿主进程内 xunjieso.dll 同一实例 */

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "imm32.lib")

using namespace Gdiplus;

/* ==================== 宿主接口 (Init 时存下, 指针终身有效) ==================== */
static const XjsPluginHost* g_host = NULL;
static XjsPluginCtx*        g_ctx  = NULL;
static ULONG_PTR             g_gdipToken = 0;

/* ==================== 基础工具 ==================== */

static std::wstring W8(const char* s) {
    if (!s || !*s) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);   /* n 含终止 NUL */
    std::wstring w(n > 0 ? n : 1, 0);                          /* 缓冲必须容纳 n 个 wchar, 否则 API 越界写 2 字节 (曾堆损坏) */
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    while (!w.empty() && w.back() == 0) w.pop_back();          /* 去掉终止 NUL */
    return w;
}
static std::string U8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s(n > 0 ? n : 0, 0);
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}
/* 去尾部反斜杠 ("C:\" -> "C:", 与引擎存路径同形) */
static std::wstring TrimBs(std::wstring p) {
    while (!p.empty() && (p[p.size() - 1] == L'\\' || p[p.size() - 1] == L'/')) p.erase(p.size() - 1);
    return p;
}
static std::wstring NormDir(std::wstring p) { p = TrimBs(p); p += L'\\'; return p; }
static std::wstring PathName(std::wstring p) {
    p = TrimBs(p);
    size_t i = p.find_last_of(L"\\/");
    return (i == std::wstring::npos) ? p : p.substr(i + 1);
}
static bool IsDrivePath(const std::wstring& s) {   /* "C:" 或 "C:\" */
    return s.size() >= 2 && s[1] == L':' && (s.size() == 2 || (s.size() == 3 && (s[2] == L'\\' || s[2] == L'/')));
}

/* 尺寸格式化 (与源样式 fmtSize 逐位一致): B / KB 1位 / MB 1位 / GB 2位 / TB 2位 */
static std::wstring FmtSize(unsigned long long n) {
    wchar_t b[64];
    if (n < 1024ull) { swprintf(b, 64, L"%llu B", n); return b; }
    if (n < 1048576ull) { swprintf(b, 64, L"%.1f KB", n / 1024.0); return b; }
    if (n < 1073741824ull) { swprintf(b, 64, L"%.1f MB", n / 1048576.0); return b; }
    if (n < 1099511627776ull) { swprintf(b, 64, L"%.2f GB", n / 1073741824.0); return b; }
    swprintf(b, 64, L"%.2f TB", n / 1099511627776.0); return b;
}
/* 千分位整数 (fmtCount <1万 段) */
static std::wstring FmtInt(long long n) {
    wchar_t raw[32]; swprintf(raw, 32, L"%lld", n);
    std::wstring s = raw, out; int c = 0;
    for (int i = (int)s.size() - 1; i >= 0; i--) {
        out.insert(out.begin(), s[i]);
        if (++c % 3 == 0 && i > 0) out.insert(out.begin(), L',');
    }
    return out;
}
/* 子项数量 (与源样式 fmtCount 一致): <1万 千分位; >=1万 x.x万; >=100万 取整万去 .0 */
static std::wstring FmtCount(long long n) {
    if (n < 10000) return FmtInt(n);
    double v = n / 10000.0;
    wchar_t b[48];
    if (v >= 100) swprintf(b, 48, L"%lld万", (long long)(v + 0.5));
    else {
        swprintf(b, 48, L"%.1f万", v);
        std::wstring s = b;
        size_t p = s.find(L".0万");
        if (p != std::wstring::npos) s.erase(p, 2);
        return s;
    }
    return b;
}

/* 极简 JSON 字段提取 (顶层扁平对象, 字符串值带转义还原): 返回 UTF-16; 缺失返回空 */
static std::wstring JsonFieldW(const std::string& j, const char* key) {
    std::string k = std::string("\"") + key + "\":";
    size_t p = j.find(k);
    if (p == std::string::npos) return L"";
    p += k.size();
    while (p < j.size() && (j[p] == ' ' || j[p] == '\t')) p++;
    if (p >= j.size()) return L"";
    std::string out;
    if (j[p] == '"') {
        p++;
        while (p < j.size() && j[p] != '"') {
            char c = j[p];
            if (c == '\\' && p + 1 < j.size()) {
                char e = j[++p];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        if (p + 4 < j.size()) {
                            wchar_t wc = 0;
                            for (int i = 1; i <= 4 && p + i < (int)j.size(); i++) {
                                char h = j[p + i]; wc <<= 4;
                                if (h >= '0' && h <= '9') wc |= h - '0';
                                else if (h >= 'a' && h <= 'f') wc |= h - 'a' + 10;
                                else if (h >= 'A' && h <= 'F') wc |= h - 'A' + 10;
                            }
                            p += 4;
                            out += (char)wc;   /* 代理对不拆: 示例数据无 emoji, 单元直转 */
                        }
                        break;
                    }
                    default: out += e;
                }
                p++;
            } else { out += c; p++; }
        }
    } else {   /* 数字 / 布尔原样 */
        size_t e = p;
        while (e < j.size() && j[e] != ',' && j[e] != '}' && j[e] != ' ') e++;
        out = j.substr(p, e - p);
    }
    return W8(out.c_str());
}
static long long JsonFieldN(const std::string& j, const char* key) {
    std::wstring v = JsonFieldW(j, key);
    return _wtoi64(v.c_str());
}

/* ==================== 皮肤 (GetSkinJson, 免权限) ==================== */
struct Skin {
    COLORREF bg1 = RGB(10, 8, 36), bg2 = RGB(3, 2, 8), panel = RGB(20, 16, 40);
    COLORREF text = RGB(232, 230, 255), dim = RGB(138, 134, 184), accent = RGB(124, 92, 255);
    COLORREF border = RGB(124, 92, 255);
    float dpi = 1.0f;
};
static Skin g_sk;
static Gdiplus::Color GC(COLORREF c, int a = 255) { return Gdiplus::Color((BYTE)a, GetRValue(c), GetGValue(c), GetBValue(c)); }
/* CSS #RRGGBB 字面量 → COLORREF (0x00BBGGRR): 色值一律经此包装, 直接写 0xRRGGBB 会被按 BGR
   解释红蓝互换 (曾导致全图色相错乱: 目录金黄变蓝青/文件蓝变橙黄) */
static constexpr COLORREF CSS(unsigned css) { return RGB((css >> 16) & 0xff, (css >> 8) & 0xff, css & 0xff); }
static COLORREF Mix(COLORREF a, COLORREF b, double t) {   /* a*(1-t) + b*t */
    return RGB((int)(GetRValue(a) * (1 - t) + GetRValue(b) * t + 0.5),
               (int)(GetGValue(a) * (1 - t) + GetGValue(b) * t + 0.5),
               (int)(GetBValue(a) * (1 - t) + GetBValue(b) * t + 0.5));
}
static bool HostHasSkinJsonOf() {   /* 宿主表追加指针: 取用前先校验 host->size (SDK v4 追加口径) */
    return g_host && g_host->size >= offsetof(XjsPluginHost, GetSkinJsonOf) + sizeof(g_host->GetSkinJsonOf);
}
static void ApplySkin() {   /* 跟随默认窗口的皮肤 (window=0); 打开时取一次, EVT_SKIN 到达再重取 */
    char buf[512] = {};
    int n = HostHasSkinJsonOf() ? g_host->GetSkinJsonOf(g_ctx, 0, buf, sizeof(buf))
                                : (g_host ? g_host->GetSkinJson(g_ctx, buf, sizeof(buf)) : XJS_PLUGIN_ERR_FAIL);
    if (n > 0) {   /* 缓冲类 API 返回写入字节数 (负数=错误), 不返回 XJS_PLUGIN_OK */
        std::string j = buf;
        auto cv = [&](const char* k, COLORREF def) {
            std::wstring s = JsonFieldW(j, k);
            if (s.size() == 7 && s[0] == L'#') {
                auto hb = [&](wchar_t c) -> int {
                    if (c >= L'0' && c <= L'9') return c - L'0';
                    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
                    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
                    return -1; };
                int r = hb(s[1]) * 16 + hb(s[2]), g = hb(s[3]) * 16 + hb(s[4]), b = hb(s[5]) * 16 + hb(s[6]);
                if (r >= 0 && g >= 0 && b >= 0) return RGB(r, g, b);
            }
            return def;
        };
        g_sk.bg1 = cv("bg1", g_sk.bg1);
        g_sk.bg2 = cv("bg2", g_sk.bg2);
        g_sk.panel = cv("panel", g_sk.panel);
        g_sk.text = cv("text", g_sk.text);
        g_sk.dim = cv("dim", g_sk.dim);
        g_sk.accent = cv("accent", g_sk.accent);
        g_sk.border = cv("line", g_sk.border);
    }
}

/* 存取插件存储 (免权限): 键规则见 SDK (UTF-8 文字, 禁控制字符与路径符) */
static std::wstring StorageGet(const char* key) {
    char buf[256] = {};
    if (g_host && g_host->StorageGet(g_ctx, key, buf, sizeof(buf)) > 0) return W8(buf);   /* >0 = 字节数 (负=错误) */
    return L"";
}
static void StorageSet(const char* key, const std::wstring& v) {
    if (g_host) { std::string u = U8(v); g_host->StorageSet(g_ctx, key, u.c_str(), (int)u.size()); }
}

/* ==================== 数据模型 ==================== */
struct N {
    std::wstring path;                  /* 完整路径, 无尾反斜杠 (引擎同形); 盘符根 = "C:" */
    std::wstring name;
    int cid = -1;                       /* 引擎文件ID (宿主 OpenFile 按 ID 打开; 根/占位节点 = -1) */
    unsigned long long size = 0;
    long long childCount = 0, fileCount = 0, dirCount = 0;
    bool isDir = false, isDrive = false;
    bool expanded = false;
    bool snapEmpty = false;             /* 快照补查过仍无子项 (防死循环) */
    unsigned long long total = 0, free = 0;   /* 驱动器信息 (tooltip 用, 地图内无驱动器瓦片) */
    std::vector<N> children;
};
/* 排序方式: 0=按大小 1=按子项数 2=按子文件数 3=按子文件夹数 (源样式 sortSel 四项) */
static int g_sortBy = 0;
static long long SortKeyOf(const N& n) {
    if (g_sortBy == 1) return n.childCount;
    if (g_sortBy == 2) return n.fileCount;
    if (g_sortBy == 3) return n.dirCount;
    return (long long)n.size;
}
/* 源样式 cmpItems: 排序键不同的先按键; 相同的按大小 (size 模式纯按大小) */
static bool CmpItems(const N& a, const N& b) {
    long long ka = SortKeyOf(a), kb = SortKeyOf(b);
    if (ka != kb) return ka > kb;
    return a.size > b.size;
}
/* 格子布局权重 (面积基准): 最小 1 防 0 值 NaN */
static unsigned long long TileWeight(const N& n) {
    long long w = g_sortBy ? SortKeyOf(n) : (long long)n.size;
    return (unsigned long long)(w > 0 ? w : 1);
}

/* ==================== 引擎直连查询 (只读锁, UI 线程) ==================== */
static bool EngReady(xjs_engine* eng) {
    int st = eng ? xjs_db_GetEngineState(eng) : -1;
    return st == 0 || st == 4 || st == 5;   /* 空闲/同步/搜索可查; 加载/保存/扫描忙 */
}
struct EngReadLock {
    xjs_engine* e;
    explicit EngReadLock(xjs_engine* e) : e(e) { if (e) xjs_Lock(e, TRUE); }
    ~EngReadLock() { if (e) xjs_Unlock(e, TRUE); }
};
/* 目录统计 (用户口径 2026-09-19): GetFileIdByPath + TraverseChildrenIds 一次递归遍历, 统计全在回调里做
   (代替每目录 GetChildrenCount×3 + 聚合兜底的 4 趟子树遍历); 直接子项用 数组[200] + 最小值变量
   逐个对比、满了淘汰最小值, 只保留渲染需要的前 200 (地图最多画 200), 最后对入选项排序一次 */
struct DirTally { unsigned long long size = 0; long long files = 0, dirs = 0; };   /* childCount = files+dirs */
static int XJS_CALL TallyCb(xjs_engine* eng, int fileId, void* ud, void*, void*) {
    DirTally* t = (DirTally*)ud;
    if (xjs_db_IsDir(eng, fileId)) t->dirs++;
    else { t->files++; long long s = xjs_db_GetFileSize(eng, fileId); if (s > 0) t->size += (unsigned long long)s; }
    return 0;
}
static void EngTally(xjs_engine* eng, int dirId, DirTally* t) {
    t->size = 0; t->files = 0; t->dirs = 0;
    xjs_db_TraverseChildrenIds(eng, dirId, TRUE, TallyCb, t, NULL, NULL);
}
/* 引擎 dirChildren 等价: 直接子项 (路径/名称/大小/目录/递归三项计数), 已按当前排序降序;
   rootOut = 本目录全子树合计 (含被淘汰子项 — 总占用/根标题用, 不随前 200 截断失真) */
static std::vector<N> EngLoadDir(const std::wstring& dirPath, DirTally* rootOut) {
    std::vector<N> out;
    xjs_engine* eng = xjs_GetDefaultEngine();
    if (!eng || !EngReady(eng)) { if (rootOut) *rootOut = DirTally(); return out; }
    EngReadLock lk(eng);
    std::string p8 = U8(TrimBs(dirPath));
    int rootId = xjs_db_GetFileIdByPath(eng, p8.c_str());
    if (rootId < 0) { if (rootOut) *rootOut = DirTally(); return out; }
    std::vector<int> ids(1024);
    int n = xjs_db_GetChildrenIds(eng, rootId, FALSE, ids.data(), (int)ids.size());
    while (n == (int)ids.size() && n > 0) { ids.resize(ids.size() * 2); n = xjs_db_GetChildrenIds(eng, rootId, FALSE, ids.data(), (int)ids.size()); }
    /* 数组[200] + 最小值淘汰: 池存 {子项id, 统计, 权重}; 权重 = 排序键<<44 | 大小 (与 cmpItems 同序) */
    static const int XSM_TOP = 200;
    struct Slot { int cid; DirTally t; unsigned long long w; };
    Slot pool[XSM_TOP];
    int count = 0, minIdx = -1;
    unsigned long long minW = ULLONG_MAX;
    DirTally root;   /* 根合计 (含被淘汰子项) */
    for (int i = 0; i < n; i++) {
        int cid = ids[i];
        bool d = xjs_db_IsDir(eng, cid) != 0;
        Slot s; s.cid = cid;
        unsigned long long key = 0;
        if (d) {
            EngTally(eng, cid, &s.t);              /* 一次递归遍历, 统计在回调里做 */
            s.w = s.t.size;
            key = (unsigned long long)(s.t.files + s.t.dirs);
            root.dirs += 1 + s.t.dirs;             /* 根合计: 子目录自身 + 其子树 */
            root.files += s.t.files;
        } else {
            long long sz = xjs_db_GetFileSize(eng, cid);
            s.t.size = sz > 0 ? (unsigned long long)sz : 0;
            s.w = s.t.size;
            key = 0;                               /* 文件无递归计数 (cmpItems 计数为 0, 同批按大小排) */
            root.files += 1;
        }
        root.size += s.w;
        s.w = (key << 44) | (s.w & ((1ULL << 44) - 1));
        if (count < XSM_TOP) {                     /* 池未满: 入池, 维护最小值变量 */
            pool[count] = s;
            if (minIdx < 0 || s.w < minW) { minW = s.w; minIdx = count; }
            count++;
        } else if (s.w > minW) {                   /* 比最小值大: 淘汰最小值槽, 循环重扫求新最小值 */
            pool[minIdx] = s;
            minW = ULLONG_MAX; minIdx = -1;
            for (int k = 0; k < XSM_TOP; k++) if (pool[k].w < minW) { minW = pool[k].w; minIdx = k; }
        }
    }
    /* 物化入选子项 (被淘汰的零字符串开销), 最后只排序这一次 */
    for (int k = 0; k < count; k++) {
        int cid = pool[k].cid;
        const char* pp = xjs_db_GetPath(eng, cid);
        if (!pp || !*pp) continue;
        N it;
        it.path = W8(pp);
        it.cid = cid;
        const char* nm = xjs_db_GetName(eng, cid);
        it.name = nm && *nm ? W8(nm) : PathName(it.path);
        it.isDir = xjs_db_IsDir(eng, cid) != 0;
        it.size = pool[k].t.size;
        it.fileCount = pool[k].t.files;
        it.dirCount = pool[k].t.dirs;
        it.childCount = pool[k].t.files + pool[k].t.dirs;
        out.push_back(std::move(it));
    }
    std::sort(out.begin(), out.end(), CmpItems);
    if (rootOut) *rootOut = root;
    return out;
}

/* 可用盘符 (A-Z 探测 GetDiskFreeSpaceExW, 与源样式 loadDrivesByEnumeration 同口径: 按 已用 降序) */
struct DriveInfo { std::wstring name, path; unsigned long long used, total, free; };
static std::vector<DriveInfo> EnumDrives() {
    std::vector<DriveInfo> v;
    for (wchar_t c = L'A'; c <= L'Z'; c++) {
        wchar_t root[4] = { c, L':', L'\\', 0 };
        ULARGE_INTEGER total = {}, freeq = {};
        if (GetDiskFreeSpaceExW(root, NULL, &total, &freeq) && total.QuadPart > 0) {
            DriveInfo d;
            d.name = std::wstring(1, c) + L":";
            d.path = d.name + L"\\";
            d.total = total.QuadPart; d.free = freeq.QuadPart;
            d.used = d.total - d.free;
            v.push_back(d);
        }
    }
    std::sort(v.begin(), v.end(), [](const DriveInfo& a, const DriveInfo& b) { return a.used > b.used; });
    return v;
}

/* ==================== 视图 / 导航状态 (单窗口, 状态全局同源样式 JS) ==================== */
static HWND g_hwnd = NULL;
static float g_dpi = 1.0f;
#define S(v) ((v) * g_dpi)

static std::vector<std::wstring> g_pathStack;    /* [0]=盘符根, 末位=当前视图根 */
static std::vector<std::wstring> g_forwardStack; /* 鼠标侧键前进栈 */
static std::map<std::wstring, std::vector<N>> g_cache;           /* 视图根 -> 子项树 (导航返回零查询) */
static std::map<std::wstring, std::set<std::wstring>> g_expSnap; /* 视图根 -> 展开路径快照 */
static std::vector<N>* g_items = NULL;           /* 当前视图 (指向 g_cache 槽; 变树后立即重排) */
static std::wstring g_viewRoot;
static bool g_booted = false;
static bool g_loading = false;
static std::wstring g_loadText = L"正在统计目录占用…";
static std::wstring g_pendingOpen;               /* boot 前到达的定位目标 */
static std::wstring g_lastOpenTarget;
static unsigned long long g_winToken = 0;        /* 发起窗口令牌 (OpenPath 用) */

/* 选择 / 标记 */
static std::set<std::wstring> g_sel;
static std::vector<std::wstring> g_selOrder;
static int g_selAnchor = -1;
static std::wstring g_pinned;                    /* 上次点击标记 (pinKey: 去尾反斜杠小写) */
static std::wstring g_hlPath;                    /* 定位高亮 (文件路径) */
static std::wstring g_statusBase, g_status;
static DirTally g_viewTally;                     /* 当前视图根的全子树合计 (总占用/根标题, 不随前 200 截断) */
static std::map<std::wstring, DirTally> g_cacheTally;   /* 视图根 -> 根合计 (与 g_cache 平行) */
static std::wstring g_toastText;
static bool g_toastShow = false;
static std::wstring g_waitView;                  /* 引擎忙时待装载视图 (定时器轮询) */
static std::wstring g_bootStatus;                /* boot 装载完成后的状态文本 (DoLoadView 消费) */

static std::wstring PinKey(const std::wstring& p) {
    std::wstring s = TrimBs(p);
    for (auto& c : s) c = towlower(c);
    return s;
}

/* 前置声明 (Part 3/4 定义) */
struct LRectf { float x, y, w, h; };
static RECT StageRect();
static void RefreshSelVisual();
static void SetPathInput(const std::wstring& p);
static void BootWith(const std::wstring& target);
static void TipStateReset();   /* 定义在 Tip 状态声明处 (RebuildTiles 前置调用) */
static void InvalidateAll() { if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE); }

/* ==================== 装载: 目录树 (与源样式 loadTree 三层预载同构) ==================== */
static void CollapseDescendants(N& n) {
    for (auto& c : n.children) {
        c.expanded = false;
        CollapseDescendants(c);
    }
}
static void CollectExpanded(const std::vector<N>& list, std::set<std::wstring>* out) {
    for (auto& it : list) {
        if (it.expanded) out->insert(it.path);
        if (!it.children.empty()) CollectExpanded(it.children, out);
    }
}
static void MarkAndCollect(std::vector<N>& list, const std::set<std::wstring>& snap, std::vector<std::wstring>* need) {
    for (auto& it : list) {
        if (snap.count(it.path)) {
            it.expanded = true;
            if (it.children.empty() && !it.snapEmpty) need->push_back(it.path);
        }
        if (!it.children.empty()) MarkAndCollect(it.children, snap, need);
    }
}
static N* FindNode(std::vector<N>& list, const std::wstring& path) {
    for (auto& it : list) {
        if (it.path == path) return &it;
        if (!it.children.empty()) { N* f = FindNode(it.children, path); if (f) return f; }
    }
    return NULL;
}
/* 默认展开 (源样式 markDefaultExpanded): 第1层前3 / 第2层各前2 / 第3层各前1; 返回待补查路径 */
static std::vector<std::wstring> MarkDefaultExpanded(std::vector<N>& items) {
    std::vector<std::wstring> need;
    auto pickDirs = [](std::vector<N>& list, int n, std::vector<N*>* out, std::vector<std::wstring>* nd) {
        std::vector<N*> dirs;
        for (auto& it : list) if (it.isDir && !it.isDrive) dirs.push_back(&it);
        std::stable_sort(dirs.begin(), dirs.end(), [](const N* a, const N* b) { return CmpItems(*a, *b); });
        for (int i = 0; i < (int)dirs.size() && i < n; i++) {
            if (!dirs[i]->children.empty()) dirs[i]->expanded = true;
            else nd->push_back(dirs[i]->path);
            out->push_back(dirs[i]);
        }
    };
    std::vector<N*> l1, l2;
    pickDirs(items, 3, &l1, &need);
    for (auto* d : l1) if (!d->children.empty()) pickDirs(d->children, 2, &l2, &need);
    {   /* 第三层: 遍历 l2 的副本 — pickDirs 会向 l2 追加导致重分配, 原容器上遍历 = 悬垂迭代器 (14:35 崩溃根因) */
        std::vector<N*> l2copy = l2;
        for (auto* d : l2copy) if (!d->children.empty()) pickDirs(d->children, 1, &l2, &need);
    }
    return need;
}
/* 展开补齐 (默认展开/快照恢复共用): need 里的目录逐个查子项, 查到即标记展开; 快照恢复可能层层递归 */
static void FillExpanded(std::vector<N>& items, std::vector<std::wstring>& need, const std::set<std::wstring>* snap) {
    int guard = 0;
    while (!need.empty() && guard++ < 8) {
        std::vector<std::wstring> round = need;
        need.clear();
        for (auto& p : round) {
            N* n = FindNode(items, p);
            if (!n) continue;
            if (!n->children.empty()) { if (snap) n->expanded = true; continue; }
            n->children = EngLoadDir(p, NULL);
            if (!n->children.empty()) n->expanded = true;
            else if (snap) n->snapEmpty = true;
        }
        if (!snap) break;
        MarkAndCollect(items, *snap, &need);
    }
}
/* 目录树装载: 当前层 + 前 8 大子目录 + 前 12 个孙目录预载 (源样式 loadTree 三轮; 引擎查询微秒级) */
static std::vector<N> LoadTree(const std::wstring& dirPath, DirTally* rootOut) {
    std::vector<N> items = EngLoadDir(dirPath, rootOut);
    /* 第一轮: 按当前排序取前 8 个达阈值 (最大子目录 1%) 的大子目录, 各预载直接子项 */
    {
        std::vector<N*> dirs;
        for (auto& it : items) if (it.isDir && !it.isDrive) dirs.push_back(&it);
        std::stable_sort(dirs.begin(), dirs.end(), [](const N* a, const N* b) { return CmpItems(*a, *b); });
        unsigned long long maxSize = dirs.empty() ? 0 : dirs[0]->size;
        int nested = 0;
        for (auto* d : dirs) {
            if (nested >= 8) break;
            if (maxSize && d->size < maxSize / 100) break;
            d->children = EngLoadDir(d->path, NULL);
            nested++;
        }
        /* 第二轮: 孙目录按权重取前 12 (收集路径重查, 避免 vector 指针干扰) */
        std::vector<std::pair<std::wstring, unsigned long long>> sub;
        for (auto* d : dirs) {
            if (d->children.empty()) continue;
            for (auto& k : d->children) if (k.isDir) sub.push_back({ k.path, TileWeight(k) });
        }
        std::stable_sort(sub.begin(), sub.end(), [](auto& a, auto& b) { return a.second > b.second; });
        int n2 = 0;
        for (auto& sw : sub) {
            if (n2 >= 12) break;
            for (auto* d : dirs) {
                bool hit = false;
                for (auto& k : d->children)
                    if (k.path == sw.first) { k.children = EngLoadDir(k.path, NULL); n2++; hit = true; break; }
                if (hit) break;
            }
        }
    }
    return items;
}

/* ==================== 导航 ==================== */
static void RebuildTiles();
static void ApplySortResort(std::vector<N>& list) {
    std::sort(list.begin(), list.end(), CmpItems);
    for (auto& it : list) if (!it.children.empty()) ApplySortResort(it.children);
}
static void SetStatus(const std::wstring& s) { g_status = s; }
static void RefreshSelStatus() {
    g_status = g_statusBase + (g_selOrder.empty() ? L"" : (L" · 已选 " + FmtInt((long long)g_selOrder.size()) + L" 项"));
}
/* 视图切换 (源样式 transitionTo): 缓存命中零查询直绘; 未命中先画遮罩, 定时器里做引擎装载 */
static void TransitionTo(const std::wstring& view) {
    g_viewRoot = view;
    auto hit = g_cache.find(view);
    if (hit != g_cache.end()) {
        g_sel.clear(); g_selOrder.clear(); g_selAnchor = -1;
        g_items = &hit->second;
        auto t = g_cacheTally.find(view);
        g_viewTally = (t != g_cacheTally.end()) ? t->second : DirTally();
        ApplySortResort(*g_items);
        RebuildTiles();
        RefreshSelStatus();
        InvalidateAll();
        return;
    }
    g_loading = true;
    g_loadText = L"正在统计目录占用…";
    g_waitView = view;
    SetTimer(g_hwnd, 7, 30, NULL);   /* 遮罩先画, WM_TIMER 里执行引擎装载 */
    InvalidateAll();
}
static void DoLoadView(const std::wstring& view) {
    xjs_engine* eng = xjs_GetDefaultEngine();
    if (!eng || !EngReady(eng)) {
        /* 文案区分两种等待: 引擎实例未注册 (宿主未设默认引擎) vs 索引忙 (加载/保存/扫描中) */
        g_loadText = eng ? L"正在等待索引就绪…" : L"引擎不可用 (默认引擎未注册)";
        SetTimer(g_hwnd, 7, 500, NULL);
        InvalidateAll();
        return;
    }
    DirTally rootTally;
    std::vector<N> items = LoadTree(view, &rootTally);
    auto slot = g_cache.find(view);
    if (slot == g_cache.end()) slot = g_cache.emplace(view, std::vector<N>()).first;
    slot->second = std::move(items);
    g_items = &slot->second;
    g_viewTally = rootTally;
    g_cacheTally[view] = rootTally;
    auto snapIt = g_expSnap.find(view);
    if (snapIt != g_expSnap.end()) {   /* 返回上级: 恢复展开快照 (逐层补齐缺失数据) */
        std::set<std::wstring> snap = snapIt->second;
        g_expSnap.erase(snapIt);
        std::vector<std::wstring> need;
        MarkAndCollect(*g_items, snap, &need);
        FillExpanded(*g_items, need, &snap);
    } else {
        std::vector<std::wstring> need = MarkDefaultExpanded(*g_items);
        FillExpanded(*g_items, need, NULL);
    }
    g_loading = false;
    KillTimer(g_hwnd, 7);
    g_sel.clear(); g_selOrder.clear(); g_selAnchor = -1;
    RebuildTiles();
    std::wstring rootLabel = TrimBs(g_pathStack[0]);
    std::wstring curName = PathName(TrimBs(view));
    if (curName.empty()) curName = rootLabel;
    g_statusBase = L"磁盘 " + rootLabel + (g_pathStack.size() > 1 ? (L" › " + curName) : L"");
    if (!g_bootStatus.empty()) { SetStatus(g_bootStatus); g_bootStatus.clear(); }
    else RefreshSelStatus();
    InvalidateAll();
}
static void SaveExpandedSnapshot() {
    if (g_items && !g_viewRoot.empty()) {
        std::set<std::wstring> s;
        CollectExpanded(*g_items, &s);
        g_expSnap[g_viewRoot] = s;
    }
}
static void GoInto(const std::wstring& dirPath) {
    SaveExpandedSnapshot();
    SetPathInput(dirPath);
    g_forwardStack.clear();
    g_pathStack.push_back(NormDir(dirPath));
    TransitionTo(g_pathStack.back());
}
static void BackToParent() {
    if (g_pathStack.size() <= 1) return;
    g_pinned = PinKey(g_pathStack.back());   /* 正在返回的视图 = 刚放大进入的目录 */
    SaveExpandedSnapshot();
    g_pathStack.pop_back();
    TransitionTo(g_pathStack.back());
}
/* 收起最深层展开目录; 无展开层且放大进入过 → 返回上级 (Esc/侧键←, 源样式 collapseDeepest) */
static void CollapseDeepest() {
    if (!g_items) return;
    N* deepest = NULL; int deepDepth = -1;
    std::function<void(std::vector<N>&, int)> walk = [&](std::vector<N>& list, int depth) {
        for (auto& it : list) {
            if (it.expanded && depth > deepDepth) { deepDepth = depth; deepest = &it; }
            if (!it.children.empty()) walk(it.children, depth + 1);
        }
    };
    walk(*g_items, 0);
    if (deepest) {
        std::wstring p = deepest->path;
        deepest->expanded = false;
        g_pinned = PinKey(p);
        RebuildTiles();
        SetStatus(L"已收起 " + PathName(p));
        RefreshSelVisual();
        InvalidateAll();
        return;
    }
    if (g_pathStack.size() > 1) BackToParent();
}

/* ==================== Squarified 布局 (逐行 worst-ratio, 与源样式 squarify 同算法) ==================== */
struct SqItem { N* node; double area; };
static double WorstRatio(const std::vector<SqItem>& row, double shortSide) {
    if (row.empty()) return 1e18;
    double sum = 0, mx = 0, mn = 1e18;
    for (auto& r : row) { sum += r.area; if (r.area > mx) mx = r.area; if (r.area < mn) mn = r.area; }
    if (sum <= 0 || mn <= 0) return 1e18;
    double w2 = shortSide * shortSide;
    double a = w2 * mx / (sum * sum), b = sum * sum / (w2 * mn);
    return a > b ? a : b;
}
static void Squarify(std::vector<N*>& items, double x, double y, double w, double h,
                     std::vector<std::pair<N*, LRectf>>* out) {
    /* items 必须是指向真实树 (g_cache 内 N) 的指针: g_tiles 会长期持有这些指针,
       禁止传局部拷贝 — 曾因拷贝导致全表悬垂, 绘制/命中读释放内存必崩 (2026-09-19 实锤) */
    if (items.empty() || w < 2 || h < 2) return;
    double totalSize = 0;
    for (auto* c : items) totalSize += (double)TileWeight(*c);
    if (totalSize <= 0) return;
    std::vector<N*> sorted = items;
    std::stable_sort(sorted.begin(), sorted.end(), [](const N* a, const N* b) { return TileWeight(*a) > TileWeight(*b); });
    double scale = (w * h) / totalSize;
    std::vector<SqItem> remaining;
    for (auto* c : sorted) remaining.push_back({ c, (double)TileWeight(*c) * scale });
    double rx = x, ry = y, rw = w, rh = h;
    size_t idx = 0;
    while (idx < remaining.size()) {
        double shortSide = rw < rh ? rw : rh;
        std::vector<SqItem> row{ remaining[idx] };
        size_t i = idx + 1;
        double curWorst = WorstRatio(row, shortSide);
        while (i < remaining.size()) {
            std::vector<SqItem> nr = row;
            nr.push_back(remaining[i]);
            double nw = WorstRatio(nr, shortSide);
            if (nw <= curWorst) { row = nr; curWorst = nw; i++; }
            else break;
        }
        double rowSum = 0;
        for (auto& r : row) rowSum += r.area;
        if (rw >= rh) {
            double stripW = rowSum / rh, cy = ry;
            for (auto& item : row) {
                double ih = item.area / stripW;
                out->push_back({ item.node, { (float)rx, (float)cy, (float)stripW, (float)ih } });
                cy += ih;
            }
            rx += stripW; rw -= stripW;
        } else {
            double stripH = rowSum / rw, cx = rx;
            for (auto& item : row) {
                double iw = item.area / stripH;
                out->push_back({ item.node, { (float)cx, (float)ry, (float)iw, (float)stripH } });
                cx += iw;
            }
            ry += stripH; rh -= stripH;
        }
        idx = i;
    }
}

/* ==================== 瓦片平面化 (绘制/命中同源; 父先子后 = 命中取最后一个包含者) ==================== */
struct Tile {
    N* n;
    LRectf rc;
    int depth;
    float titleH;
    bool expanded;
    unsigned long long total;   /* 本层占比基准 */
};
static std::vector<Tile> g_tiles;
static int g_hover = -1;
static void BuildNode(N& node, const LRectf& rc, unsigned long long total, int depth) {
    bool isDir = node.isDir || node.isDrive;
    bool canExpand = rc.w >= S(52) && rc.h >= S(34);
    bool exp = isDir && node.expanded && !node.children.empty() && canExpand;
    if (isDir && node.expanded && !exp) node.expanded = false;
    Tile t{ &node, rc, depth, 0, false, total };
    if (exp) {
        float th = (float)std::round(rc.h * 0.18);
        if (th < S(12)) th = S(12);
        if (th > S(22)) th = S(22);
        t.titleH = th;
        t.expanded = true;
    }
    g_tiles.push_back(t);
    if (exp) {
        float pad = (float)std::round(std::min(rc.w, rc.h) * 0.06);
        if (pad < S(1)) pad = S(1);
        if (pad > S(4)) pad = S(4);
        unsigned long long subTotal = 1, s = 0;
        for (auto& k : node.children) s += TileWeight(k);
        if (s) subTotal = s;
        std::vector<N*> sub;   /* 指向真实树的子项指针 (前 50), 禁拷贝 — 见 Squarify 注释 */
        for (size_t k = 0; k < node.children.size() && k < 50; k++) sub.push_back(&node.children[k]);
        std::vector<std::pair<N*, LRectf>> raw;
        Squarify(sub, pad, t.titleH + pad, std::max(rc.w - pad * 2, 4.0f), std::max(rc.h - t.titleH - pad * 2, 4.0f), &raw);
        for (auto& r : raw) {
            int x = (int)std::lround(r.second.x), y = (int)std::lround(r.second.y);
            int w = (int)std::lround(r.second.x + r.second.w) - x, h = (int)std::lround(r.second.y + r.second.h) - y;
            if (w < 1 || h < 1) continue;
            BuildNode(*r.first, { rc.x + (float)x, rc.y + (float)y, (float)w, (float)h }, subTotal, depth + 1);
        }
    }
}
/* 整图重排 (源样式 renderMap 布局段): 前 200 项 squarify, 顶层取整吸附边缘 */
static void RebuildTiles() {
    g_tiles.clear();
    g_hover = -1;
    TipStateReset();   /* 树可能被整体替换: 旧 Tooltip 节点指针作废 (定义在 Tip 状态声明处) */
    if (!g_hwnd || !g_items) return;
    std::vector<N>& items = *g_items;
    if (items.empty()) return;
    RECT sr = StageRect();
    float sx = (float)sr.left, sy = (float)sr.top, sw = (float)(sr.right - sr.left), sh = (float)(sr.bottom - sr.top);
    if (sw < 10 || sh < 10) return;
    float rootTitleH = (g_pathStack.size() > 1) ? S(24) : 0;
    std::vector<N*> renderList;   /* 指向真实树 (g_cache) 的指针, 前 200 项 — 禁拷贝 */
    size_t n200 = std::min(items.size(), (size_t)200);
    for (size_t i = 0; i < n200; i++) renderList.push_back(&items[i]);
    unsigned long long total = 1, s = 0;
    for (auto* it : renderList) s += TileWeight(*it);
    if (s) total = s;
    std::vector<std::pair<N*, LRectf>> raw;
    Squarify(renderList, sx, sy + rootTitleH, sw, sh - rootTitleH, &raw);
    for (auto& r : raw) {
        int x = (int)std::lround(r.second.x), y = (int)std::lround(r.second.y);
        int w = (int)std::lround(r.second.x + r.second.w) - x, h = (int)std::lround(r.second.y + r.second.h) - y;
        if (x + w >= (int)(sx + sw) - 2) w = (int)(sx + sw) - x;
        if (y + h >= (int)(sy + sh) - 2) h = (int)(sy + sh) - y;
        if (w < 1 || h < 1) continue;
        BuildNode(*r.first, { (float)x, (float)y, (float)w, (float)h }, total, 0);
    }
}

/* ==================== 交互状态声明 (绘制/命中共用) ==================== */
static bool g_focusInput = false;
static std::wstring g_pathInput;      /* 路径编辑框内容 */
static int g_inputCaret = 0, g_inputSelA = -1;
static float g_inputScroll = 0;
static bool g_caretOn = true;

struct MItem { std::wstring text, sub; int icon; bool disabled, current, sep; int act; };
enum { MI_NONE = 0, MI_DRIVE, MI_DIR, MI_FILE, MI_OPEN, MI_REVEAL, MI_COPYPATH, MI_COPYNAME, MI_REFRESH };
static bool g_menuOpen = false;
static std::vector<MItem> g_menu;
static RectF g_menuRc;
static int g_menuHover = -1;
static std::wstring g_menuNodePath;   /* 右键对象路径 (动作执行时按路径查回) */
static bool g_menuNodeIsDir = false;
static int g_menuKind = 0;            /* 1=瓦片右键 2=选择磁盘 3=排序 */

static int g_tipTile = -1;
static bool g_tipVisible = false;
static POINT g_tipPos;
static RectF g_tipRc;
static N* g_tipNode = NULL;
static unsigned long long g_tipTotal = 1;
static bool s_tipWait = false;        /* tooltip 延迟等待中 */
/* 树整体替换 (装载/切盘/返回) 后旧节点指针失效: RebuildTiles 前置调用作废 Tip 状态 */
static void TipStateReset() {
    g_tipNode = NULL;
    s_tipWait = false;
    g_tipVisible = false;
    if (g_hwnd) { KillTimer(g_hwnd, 1); KillTimer(g_hwnd, 2); }   /* 无条件清 (挂起的延迟定时器到点也会显示) */
}

static bool g_marqueeOn = false;
static POINT g_marqueeStart, g_marqueeCur;
static bool g_sbDrag = false;
static bool s_suppressClick = false;  /* 拖出完成抑制本次 click */
static int g_spinTick = 0;            /* 旋转动画节拍 (定时器驱动) */
static RectF g_hmRc1, g_hmRc2;        /* 隐私页两按钮命中区 (绘制时回填) */
static RectF g_backBtnRc;             /* 空态返回按钮命中区 */

struct EvalData {
    bool ok = false;
    std::wstring err;
    std::wstring name, cat, product, vendor, desc, site, note, action, reliability;
    int score = -1;
    int rebuild = -1;   /* 0=不会 1=会 */
};
enum { SB_PRIVACY = 0, SB_GUIDE, SB_LOADING, SB_RESULT, SB_FAIL, SB_ERROR };
static int g_sbPage = SB_PRIVACY;
static float g_sbScroll = 0, g_sbContentH = 0, g_sbMaxScroll = 0;
static std::wstring g_evalPath;
static EvalData g_evalData;
static RectF g_siteRc;   /* 官网链接命中区 (绘制时回填) */

/* 字体缓存前置 (InputXOf 先于绘制基础设施使用) */
static Gdiplus::Font* F(float px, bool bold);

/* 路径输入光标 x (供绘制/命中) */
static float InputXOf(Gdiplus::Graphics& g, int idx) {
    if (g_pathInput.empty() || idx <= 0) return 0;
    if (idx > (int)g_pathInput.size()) idx = (int)g_pathInput.size();
    RectF box(0, 0, 10000, 100);
    RectF out;
    Gdiplus::StringFormat sf;
    sf.SetFormatFlags(Gdiplus::StringFormatFlagsMeasureTrailingSpaces | Gdiplus::StringFormatFlagsNoWrap);
    g.MeasureString(g_pathInput.c_str(), idx, F(S(10.9f), false), box, &sf, &out);
    return out.Width;
}

/* ==================== GDI+ 绘制基础设施 ==================== */
static std::map<unsigned, Gdiplus::Font*> g_fonts;
static Gdiplus::Font* F(float px, bool bold) {
    unsigned key = (unsigned)(px * 4.0f) * 2 + (bold ? 1 : 0);
    auto it = g_fonts.find(key);
    if (it != g_fonts.end()) return it->second;
    Gdiplus::Font* f = new Gdiplus::Font(L"Microsoft YaHei UI", px,
                                         bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    g_fonts[key] = f;
    return f;
}
static void ClearFonts() {
    for (auto& kv : g_fonts) delete kv.second;
    g_fonts.clear();
}
static Gdiplus::GraphicsPath* RoundPath(const RectF& rc, float r) {
    Gdiplus::GraphicsPath* p = new Gdiplus::GraphicsPath();
    if (r <= 0.5f) { p->AddRectangle(rc); return p; }
    float d = r * 2;
    if (d > rc.Width) d = rc.Width;
    if (d > rc.Height) d = rc.Height;
    p->AddArc(rc.X, rc.Y, d, d, 180, 90);
    p->AddArc(rc.X + rc.Width - d, rc.Y, d, d, 270, 90);
    p->AddArc(rc.X + rc.Width - d, rc.Y + rc.Height - d, d, d, 0, 90);
    p->AddArc(rc.X, rc.Y + rc.Height - d, d, d, 90, 90);
    p->CloseFigure();
    return p;
}
static void FillRc(Gdiplus::Graphics& g, const RectF& rc, Gdiplus::Brush* br, float r = 0) {
    if (rc.Width < 0.5f || rc.Height < 0.5f) return;
    if (r <= 0.5f) { g.FillRectangle(br, rc); return; }
    Gdiplus::GraphicsPath* p = RoundPath(rc, r);
    g.FillPath(br, p);
    delete p;
}
static void StrokeRc(Gdiplus::Graphics& g, const RectF& rc, Gdiplus::Pen* pen, float r = 0) {
    if (rc.Width < 0.5f || rc.Height < 0.5f) return;
    if (r <= 0.5f) { g.DrawRectangle(pen, rc); return; }
    Gdiplus::GraphicsPath* p = RoundPath(rc, r);
    g.DrawPath(pen, p);
    delete p;
}
static void DrawStr(Gdiplus::Graphics& g, const std::wstring& s, Gdiplus::Font* f, const RectF& rc,
                    Gdiplus::Brush* br, int align = 1, int valign = 1, bool wrap = false, bool ellipsis = true) {
    if (s.empty() || rc.Width < 1 || rc.Height < 1) return;
    /* 文本原点取整: ClearType 落在分数坐标上会发虚 (居中布局常产生 .5 偏移) */
    RectF rc2(std::floor(rc.X), std::floor(rc.Y), rc.Width, rc.Height);
    Gdiplus::StringFormat sf;
    sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoFitBlackBox | Gdiplus::StringFormatFlagsMeasureTrailingSpaces |
                      (wrap ? 0 : Gdiplus::StringFormatFlagsNoWrap));
    sf.SetTrimming(ellipsis ? Gdiplus::StringTrimmingEllipsisWord : Gdiplus::StringTrimmingNone);
    sf.SetAlignment(align == 0 ? Gdiplus::StringAlignmentNear : align == 2 ? Gdiplus::StringAlignmentFar : Gdiplus::StringAlignmentCenter);
    sf.SetLineAlignment(valign == 0 ? Gdiplus::StringAlignmentNear : valign == 2 ? Gdiplus::StringAlignmentFar : Gdiplus::StringAlignmentCenter);
    g.DrawString(s.c_str(), (INT)s.size(), f, rc2, &sf, br);
}
static float MeasureStr(Gdiplus::Graphics& g, const std::wstring& s, Gdiplus::Font* f, bool wrap = false, float layoutW = 0) {
    if (s.empty()) return 0;
    Gdiplus::StringFormat sf;
    sf.SetFormatFlags(Gdiplus::StringFormatFlagsMeasureTrailingSpaces | (wrap ? 0 : Gdiplus::StringFormatFlagsNoWrap));
    RectF box(0, 0, layoutW > 0 ? layoutW : 10000, 10000);
    RectF out;
    g.MeasureString(s.c_str(), (INT)s.size(), f, box, &sf, &out);
    return out.Width;
}
static float MeasureH(Gdiplus::Graphics& g, const std::wstring& s, Gdiplus::Font* f, float layoutW) {
    if (s.empty() || layoutW <= 0) return 0;
    Gdiplus::StringFormat sf;
    sf.SetFormatFlags(Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
    RectF box(0, 0, layoutW, 10000);
    RectF out;
    g.MeasureString(s.c_str(), (INT)s.size(), f, box, &sf, &out);
    return out.Height;
}
/* 复用单例画刷 (同一语句内只取一支, 全帧序列化使用) */
/* 复用单例画笔 (同语句内只取一支) */
static Gdiplus::Pen* PenP(COLORREF c, int a, float w) {
    static Gdiplus::Pen p(Gdiplus::Color(255, 0, 0, 0), 1.0f);
    p.SetColor(GC(c, a));
    p.SetWidth(w);
    return &p;
}
static Gdiplus::SolidBrush* Br(COLORREF c, int a = 255) {
    static Gdiplus::SolidBrush b(Gdiplus::Color(255, 0, 0, 0));
    b.SetColor(GC(c, a));
    return &b;
}

/* ==================== UI 布局几何 (px = CSS px × DPI, 值与源样式 CSS 同源) ==================== */
struct UiLayout {
    RectF titlebar, navbar, body, stage, resizer, sidebar, sbHead, sbBody, legend;
    RectF btnPin, btnMin, btnMax, btnClose;
    RectF driveBtn, pathInput, sortBtn, stats, sbToggle, sbClose;
};
static UiLayout g_lo;
static bool g_sbCollapsed = false;
static float g_sbWidth = 280;      /* 侧边栏宽度 (CSS px) */
static int g_hotUi = 0;
enum HotId {
    HOT_NONE = 0, HOT_PIN, HOT_MIN, HOT_MAX, HOT_CLOSE, HOT_DRIVE, HOT_SORT, HOT_INPUT,
    HOT_SBTOGGLE, HOT_SBCLOSE, HOT_SBRESIZER, HOT_BACKBTN, HOT_AGREE, HOT_DECLINE, HOT_SITE, HOT_SBTRACK
};
static RECT StageRect() {
    RECT rc = { (LONG)g_lo.stage.X, (LONG)g_lo.stage.Y,
                (LONG)(g_lo.stage.X + g_lo.stage.Width), (LONG)(g_lo.stage.Y + g_lo.stage.Height) };
    return rc;
}
static void ComputeLayout() {
    if (!g_hwnd) return;
    RECT crc; GetClientRect(g_hwnd, &crc);
    float W = (float)(crc.right - crc.left), H = (float)(crc.bottom - crc.top);
    g_lo.titlebar = RectF(0, 0, W, S(40));
    float bx = W - S(8) - S(30);
    g_lo.btnClose = RectF(bx, S(6), S(30), S(28)); bx -= S(40);
    g_lo.btnMax   = RectF(bx, S(6), S(30), S(28)); bx -= S(40);
    g_lo.btnMin   = RectF(bx, S(6), S(30), S(28)); bx -= S(40);
    g_lo.btnPin   = RectF(bx, S(6), S(30), S(28));
    g_lo.navbar   = RectF(S(12), S(40) + S(10), W - S(24), S(44));
    float ly = H - S(12) - S(34);
    g_lo.legend   = RectF(S(12), ly, W - S(24), S(34));
    float bodyY = g_lo.navbar.Y + g_lo.navbar.Height + S(10);
    g_lo.body     = RectF(S(12), bodyY, W - S(24), ly - S(10) - bodyY);
    float sbW = g_sbCollapsed ? 0 : g_sbWidth * g_dpi;
    float resW = g_sbCollapsed ? 0 : S(3);
    g_lo.sidebar  = RectF(g_lo.body.X + g_lo.body.Width - sbW, g_lo.body.Y, sbW, g_lo.body.Height);
    g_lo.resizer  = RectF(g_lo.sidebar.X - resW, g_lo.body.Y, resW, g_lo.body.Height);
    g_lo.stage    = RectF(g_lo.body.X, g_lo.body.Y, g_lo.body.Width - resW - sbW, g_lo.body.Height);
    g_lo.sbHead   = RectF(g_lo.sidebar.X, g_lo.sidebar.Y, sbW, S(40));
    g_lo.sbBody   = RectF(g_lo.sidebar.X, g_lo.sidebar.Y + S(40), sbW, sbW > 0 ? g_lo.sidebar.Height - S(40) : 0);
    g_lo.sbClose  = RectF(g_lo.sbHead.X + sbW - S(8) - S(24), g_lo.sbHead.Y + S(8), S(24), S(24));
    /* 导航条内部: [盘符][路径框 弹性][排序][统计][展开侧栏钮] 间距 16 */
    Graphics gi(g_hwnd);
    Gdiplus::Graphics* g = &gi;
    float pad = S(14), gap = S(16), y = g_lo.navbar.Y + S(9), h = S(26);
    float x = g_lo.navbar.X + pad;
    float driveW = S(10) + S(11) + S(5) + MeasureStr(*g, TrimBs(g_pathStack.empty() ? L"C:" : g_pathStack[0]), F(S(10.9f), true)) + S(10);
    g_lo.driveBtn = RectF(x, y, driveW, h); x += driveW + gap;
    float sortW = S(6) + MeasureStr(*g, L"按子文件夹数", F(S(10.9f), false)) + S(6) + S(10) + S(6);
    float stW = 0;
    {
        unsigned long long total = g_viewTally.size;   /* 根合计 (不随前 200 截断) */
        float vw = MeasureStr(*g, FmtSize(total), F(S(13.6f), true));
        float lw = MeasureStr(*g, L"总占用", F(S(9), false));
        stW = (vw > lw ? vw : lw) + S(4);
    }
    float togW = g_sbCollapsed ? S(26) + gap : 0;
    float inputR = g_lo.navbar.X + g_lo.navbar.Width - pad - stW - togW - gap - sortW - gap;
    g_lo.pathInput = RectF(x, y, inputR > x ? inputR - x : S(40), h);
    x = g_lo.pathInput.X + g_lo.pathInput.Width + gap;
    g_lo.sortBtn = RectF(x, y, sortW, h); x += sortW + gap;
    g_lo.stats = RectF(x, g_lo.navbar.Y + S(6), stW, g_lo.navbar.Height - S(12));
    if (g_sbCollapsed) g_lo.sbToggle = RectF(g_lo.navbar.X + g_lo.navbar.Width - pad - S(26), y, S(26), h);
    else g_lo.sbToggle = RectF(0, 0, 0, 0);
}

/* ==================== 瓦片配色 (源样式 DIR_DEPTH_COLORS / 文件蓝 / 驱动器粉) ==================== */
static const COLORREF DIR_DEPTH_COLORS[4] = { CSS(0xe8b93a), CSS(0xe0c98c), CSS(0xdfd3ae), CSS(0xe2d9c2) };
static const COLORREF TILE_FILE = CSS(0x8fb0da), TILE_DRIVE = CSS(0xe86a92), TILE_DIR_FG = CSS(0x4a3a12);
static COLORREF TileBg(const N& n, int depth) {
    if (n.isDrive) return TILE_DRIVE;
    if (n.isDir) return DIR_DEPTH_COLORS[std::min(depth, 3)];
    return TILE_FILE;
}
static COLORREF TileFg(const N& n) { return (n.isDir || n.isDrive) ? TILE_DIR_FG : RGB(255, 255, 255); }

/* ==================== 图标 (简化线稿, 与源样式内联 SVG 同形) ==================== */
static void DrawDriveIcon(Gdiplus::Graphics& g, const RectF& rc, COLORREF c, int a = 255) {
    Gdiplus::Pen pen(GC(c, a), S(1.3f));
    float w = rc.Width, h = rc.Height * 0.62f, y = rc.Y + rc.Height * 0.19f;
    StrokeRc(g, RectF(rc.X, y, w, h), &pen, S(1.4f));
    g.DrawLine(&pen, rc.X + w * 0.21f, y + h * 0.65f, rc.X + w * 0.79f, y + h * 0.65f);
    g.FillEllipse(Br(c, a), rc.X + w * 0.15f - S(0.6f), y + h * 0.28f - S(0.6f), S(1.2f), S(1.2f));
}
static void DrawFolderIcon(Gdiplus::Graphics& g, const RectF& rc, COLORREF c, int a = 255) {
    Gdiplus::Pen pen(GC(c, a), S(1.3f));
    float w = rc.Width, h = rc.Height;
    g.DrawLine(&pen, rc.X, rc.Y + h * 0.18f, rc.X + w * 0.38f, rc.Y + h * 0.18f);
    g.DrawLine(&pen, rc.X + w * 0.38f, rc.Y + h * 0.18f, rc.X + w * 0.52f, rc.Y + h * 0.36f);
    StrokeRc(g, RectF(rc.X, rc.Y + h * 0.18f, w - S(1), h * 0.64f), &pen, S(1.5f));
}
static void DrawFileIcon(Gdiplus::Graphics& g, const RectF& rc, COLORREF c, int a = 255) {
    Gdiplus::Pen pen(GC(c, a), S(1.3f));
    float w = rc.Width, h = rc.Height;
    PointF pts[6] = { PointF(rc.X, rc.Y), PointF(rc.X + w * 0.62f, rc.Y), PointF(rc.X + w, rc.Y + h * 0.3f),
                      PointF(rc.X + w, rc.Y + h), PointF(rc.X, rc.Y + h), PointF(rc.X, rc.Y) };
    g.DrawLines(&pen, pts, 6);
    g.DrawLine(&pen, rc.X + w * 0.62f, rc.Y, rc.X + w * 0.62f, rc.Y + h * 0.3f);
    g.DrawLine(&pen, rc.X + w * 0.62f, rc.Y + h * 0.3f, rc.X + w, rc.Y + h * 0.3f);
}
static void DrawMenuIcon(Gdiplus::Graphics& g, int icon, const RectF& rc, COLORREF c) {
    switch (icon) {
        case MI_DRIVE: DrawDriveIcon(g, rc, c); break;
        case MI_DIR: case MI_FILE:
            if (icon == MI_DIR) DrawFolderIcon(g, rc, c); else DrawFileIcon(g, rc, c);
            break;
        case MI_OPEN: {   /* 打开: 方框+外向箭头 */
            Gdiplus::Pen pen(GC(c), S(1.3f));
            StrokeRc(g, RectF(rc.X, rc.Y + rc.Height * 0.15f, rc.Width * 0.7f, rc.Height * 0.7f), &pen, S(2));
            g.DrawLine(&pen, rc.X + rc.Width * 0.45f, rc.Y + rc.Height * 0.5f, rc.X + rc.Width * 0.92f, rc.Y + rc.Height * 0.5f);
            PointF pts[3] = { PointF(rc.X + rc.Width * 0.78f, rc.Y + rc.Height * 0.32f),
                              PointF(rc.X + rc.Width * 0.95f, rc.Y + rc.Height * 0.5f),
                              PointF(rc.X + rc.Width * 0.78f, rc.Y + rc.Height * 0.68f) };
            g.DrawLines(&pen, pts, 3);
            break;
        }
        case MI_REVEAL: {   /* 定位: 托盘+横线 */
            Gdiplus::Pen pen(GC(c), S(1.3f));
            g.DrawLine(&pen, rc.X, rc.Y + rc.Height * 0.2f, rc.X + rc.Width, rc.Y + rc.Height * 0.2f);
            g.DrawLine(&pen, rc.X + rc.Width * 0.28f, rc.Y + rc.Height * 0.08f, rc.X + rc.Width * 0.45f, rc.Y + rc.Height * 0.2f);
            StrokeRc(g, RectF(rc.X + rc.Width * 0.1f, rc.Y + rc.Height * 0.2f, rc.Width * 0.8f, rc.Height * 0.7f), &pen, S(2));
            break;
        }
        case MI_COPYPATH: {   /* 复制路径: 列表 */
            Gdiplus::Pen pen(GC(c), S(1.3f));
            StrokeRc(g, RectF(rc.X + rc.Width * 0.35f, rc.Y, rc.Width * 0.55f, rc.Height * 0.75f), &pen, S(1.5f));
            g.DrawLine(&pen, rc.X + rc.Width * 0.1f, rc.Y + rc.Height * 0.28f, rc.X + rc.Width * 0.24f, rc.Y + rc.Height * 0.28f);
            g.DrawLine(&pen, rc.X + rc.Width * 0.1f, rc.Y + rc.Height * 0.52f, rc.X + rc.Width * 0.24f, rc.Y + rc.Height * 0.52f);
            g.DrawLine(&pen, rc.X + rc.Width * 0.1f, rc.Y + rc.Height * 0.76f, rc.X + rc.Width * 0.24f, rc.Y + rc.Height * 0.76f);
            break;
        }
        case MI_COPYNAME: {   /* 复制名称: 笔 */
            Gdiplus::Pen pen(GC(c), S(1.3f));
            g.DrawLine(&pen, rc.X + rc.Width * 0.1f, rc.Y + rc.Height * 0.85f, rc.X + rc.Width * 0.3f, rc.Y + rc.Height * 0.85f);
            g.DrawLines(&pen, std::vector<PointF>{ PointF(rc.X + rc.Width * 0.42f, rc.Y + rc.Height * 0.18f),
                        PointF(rc.X + rc.Width * 0.85f, rc.Y + rc.Height * 0.55f),
                        PointF(rc.X + rc.Width * 0.42f, rc.Y + rc.Height * 0.9f) }.data(), 3);
            break;
        }
        case MI_REFRESH: {   /* 刷新: 圆弧箭头 */
            Gdiplus::Pen pen(GC(c), S(1.3f));
            RectF arc(rc.X + rc.Width * 0.1f, rc.Y + rc.Height * 0.1f, rc.Width * 0.8f, rc.Height * 0.8f);
            g.DrawArc(&pen, arc, -60, 300);
            PointF pts[3] = { PointF(rc.X + rc.Width * 0.9f, rc.Y + rc.Height * 0.08f),
                              PointF(rc.X + rc.Width * 0.92f, rc.Y + rc.Height * 0.38f),
                              PointF(rc.X + rc.Width * 0.62f, rc.Y + rc.Height * 0.36f) };
            g.DrawLines(&pen, pts, 3);
            break;
        }
    }
}
/* 窗口控制小图标 (12 viewBox 线稿, 与源样式 SVG 同形) */
static void DrawWinIcon(Gdiplus::Graphics& g, int id, const RectF& rc, COLORREF c, bool pinActive, bool maxed) {
    Gdiplus::Pen pen(GC(c), S(1.5f));
    pen.SetStartCap(Gdiplus::LineCapRound); pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    float u = rc.Width / 12.0f;
    auto P = [&](float x, float y) { return PointF(rc.X + x * u, rc.Y + y * u); };
    if (id == 0) {   /* 图钉 */
        Gdiplus::GraphicsPath p;
        PointF pts[9] = { P(3.2f,1.5f), P(8.8f,1.5f), P(8.3f,4.2f), P(10.2f,6.0f), P(10.2f,7.2f),
                          P(1.8f,7.2f), P(1.8f,6.0f), P(3.7f,4.2f), P(3.2f,1.5f) };
        p.AddLines(pts, 9);
        if (pinActive) {
            Gdiplus::Matrix m;
            m.RotateAt(45, PointF(rc.X + rc.Width / 2, rc.Y + rc.Height / 2));
            p.Transform(&m);
        }
        g.DrawPath(&pen, &p);
        g.DrawLine(&pen, P(6, 7.2f), P(6, 10.5f));
    } else if (id == 1) {
        g.DrawLine(&pen, P(2.5f, 6), P(9.5f, 6));
    } else if (id == 2) {
        if (maxed) {
            StrokeRc(g, RectF(P(2.5f, 4).X, P(2.5f, 4).Y, 5.5f * u, 5.5f * u), &pen, u);
            PointF pts[5] = { P(4.5f, 4), P(4.5f, 2.5f), P(10.f, 2.5f), P(10.f, 8), P(8.f, 8) };
            g.DrawLines(&pen, pts, 5);
        } else {
            StrokeRc(g, RectF(P(2.5f, 2.5f).X, P(2.5f, 2.5f).Y, 7 * u, 7 * u), &pen, u);
        }
    } else {
        g.DrawLine(&pen, P(3, 3), P(9, 9));
        g.DrawLine(&pen, P(9, 3), P(3, 9));
    }
}
static bool IsWinMaxed() { return g_hwnd && IsZoomed(g_hwnd); }
static bool TopmostOn() { return g_hwnd && (GetWindowLongW(g_hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0; }

/* ==================== 标题栏 ==================== */
static void DrawTitlebar(Gdiplus::Graphics& g) {
    FillRc(g, g_lo.titlebar, Br(g_sk.panel));
    RectF logo(S(12), (S(40) - S(20)) / 2, S(20), S(20));
    {
        RectF gr(logo.X, logo.Y, logo.Width, logo.Height);
        LinearGradientBrush lg(gr, GC(g_sk.accent), GC(CSS(0x00d4ff)), 45.0f);
        Gdiplus::GraphicsPath* p = RoundPath(gr, S(6));
        g.FillPath(&lg, p);
        delete p;
    }
    Gdiplus::SolidBrush white(Gdiplus::Color(255, 255, 255, 255));
    float u = S(11) / 24.0f, ox = logo.X + S(4.5f), oy = logo.Y + S(4.5f);
    g.FillRectangle(&white, ox + 3 * u, oy + 3 * u, 8 * u, 8 * u);
    g.FillRectangle(&white, ox + 13 * u, oy + 3 * u, 8 * u, 5 * u);
    g.FillRectangle(&white, ox + 13 * u, oy + 10 * u, 8 * u, 11 * u);
    g.FillRectangle(&white, ox + 3 * u, oy + 13 * u, 8 * u, 8 * u);
    DrawStr(g, L"蜗牛快搜 · 空间地图", F(S(11.5f), true),
            RectF(logo.X + logo.Width + S(8), 0, S(300), S(40)), Br(g_sk.text), 0, 1);
    struct B { RectF rc; int icon; int hot; bool danger; };
    B bs[4] = { { g_lo.btnPin, 0, HOT_PIN, false }, { g_lo.btnMin, 1, HOT_MIN, false },
                { g_lo.btnMax, 2, HOT_MAX, false }, { g_lo.btnClose, 3, HOT_CLOSE, true } };
    for (auto& b : bs) {
        bool hot = (g_hotUi == b.hot);
        COLORREF ic = g_sk.dim;
        if (b.danger) {
            if (hot) { FillRc(g, b.rc, Br(CSS(0xe0442e)), S(8)); ic = RGB(255, 255, 255); }
        } else if (hot || (b.icon == 0 && TopmostOn())) {
            FillRc(g, b.rc, Br(g_sk.accent, 38), S(8));
            ic = g_sk.accent;
        }
        DrawWinIcon(g, b.icon, RectF(b.rc.X + (b.rc.Width - S(12)) / 2, b.rc.Y + (b.rc.Height - S(12)) / 2, S(12), S(12)),
                    ic, b.icon == 0 && TopmostOn(), IsWinMaxed());
    }
    g.FillRectangle(Br(g_sk.border, 70), RectF(0, S(40) - 1, g_lo.titlebar.Width, 1));
}

/* ==================== 导航条 ==================== */
static void DrawNavbar(Gdiplus::Graphics& g) {
    FillRc(g, g_lo.navbar, Br(g_sk.panel), S(13));
    Gdiplus::Pen nb(GC(g_sk.border, 70), 1.0f);
    StrokeRc(g, RectF(g_lo.navbar.X + 0.5f, g_lo.navbar.Y + 0.5f, g_lo.navbar.Width - 1, g_lo.navbar.Height - 1), &nb, S(13));
    {   /* 盘符按钮 */
        bool hot = g_hotUi == HOT_DRIVE;
        FillRc(g, g_lo.driveBtn, Br(hot ? Mix(g_sk.panel, g_sk.accent, 0.14f)
                                         : Mix(g_sk.panel, RGB(255, 255, 255), 0.05f)), S(7));
        Gdiplus::Pen bp(GC(hot ? g_sk.accent : g_sk.border, hot ? 200 : 70), 1.0f);
        StrokeRc(g, g_lo.driveBtn, &bp, S(7));
        RectF ic(g_lo.driveBtn.X + S(10), g_lo.driveBtn.Y + (S(26) - S(11)) / 2, S(11), S(11));
        DrawDriveIcon(g, ic, hot ? g_sk.accent : g_sk.dim);
        DrawStr(g, TrimBs(g_pathStack.empty() ? L"C:" : g_pathStack[0]), F(S(10.9f), true),
                RectF(ic.X + ic.Width + S(5), g_lo.driveBtn.Y, g_lo.driveBtn.Width - S(10) - S(11) - S(5) - S(10), S(26)),
                Br(g_sk.accent), 0, 1);
    }
    {   /* 路径输入框 */
        bool focus = g_focusInput;
        FillRc(g, g_lo.pathInput, Br(Mix(g_sk.panel, RGB(255, 255, 255), 0.05f)), 0);
        Gdiplus::Pen ip(GC(focus ? g_sk.accent : g_sk.border, focus ? 255 : 80), 1.0f);
        StrokeRc(g, g_lo.pathInput, &ip, 0);
        RectF tr(g_lo.pathInput.X + S(8), g_lo.pathInput.Y, g_lo.pathInput.Width - S(16), g_lo.pathInput.Height);
        if (g_pathInput.empty() && !focus) {
            DrawStr(g, L"点击/双击显示路径, 输入路径或盘符回车查看", F(S(10.9f), false), tr, Br(g_sk.dim, 150), 0, 1);
        } else {
            int selA = g_inputSelA, selB = g_inputCaret;
            if (selA >= 0 && selA != selB) {
                if (selA > selB) std::swap(selA, selB);
                float x0 = InputXOf(g, selA), x1 = InputXOf(g, selB);
                g.FillRectangle(Br(g_sk.accent, 70), RectF(tr.X + x0, tr.Y + S(4), x1 - x0, tr.Height - S(8)));
            }
            float tw = MeasureStr(g, g_pathInput, F(S(10.9f), false));
            float viewW = tr.Width;
            float caretX = InputXOf(g, g_inputCaret);
            if (caretX - g_inputScroll > viewW - S(4)) g_inputScroll = caretX - viewW + S(4);
            if (caretX - g_inputScroll < S(4)) g_inputScroll = caretX - S(4);
            if (g_inputScroll < 0) g_inputScroll = 0;
            if (g_inputScroll > tw) g_inputScroll = tw;
            Gdiplus::GraphicsState st = g.Save();
            g.SetClip(tr);
            DrawStr(g, g_pathInput, F(S(10.9f), false), RectF(tr.X - g_inputScroll, tr.Y, tw + S(40), tr.Height), Br(g_sk.text), 0, 1);
            if (focus && g_caretOn) {
                float cx = tr.X + caretX - g_inputScroll;
                g.FillRectangle(Br(g_sk.text), RectF(cx, tr.Y + S(5), std::max(1.0f, S(1)), tr.Height - S(10)));
            }
            g.Restore(st);
        }
    }
    {   /* 排序下拉 */
        bool hot = g_hotUi == HOT_SORT;
        FillRc(g, g_lo.sortBtn, Br(Mix(g_sk.panel, RGB(255, 255, 255), 0.05f)), S(7));
        Gdiplus::Pen bp(GC(hot ? g_sk.accent : g_sk.border, hot ? 200 : 70), 1.0f);
        StrokeRc(g, g_lo.sortBtn, &bp, S(7));
        static const wchar_t* SORT_NAMES[4] = { L"按大小", L"按子项数", L"按子文件数", L"按子文件夹数" };
        DrawStr(g, SORT_NAMES[g_sortBy], F(S(10.9f), false),
                RectF(g_lo.sortBtn.X + S(6), g_lo.sortBtn.Y, g_lo.sortBtn.Width - S(12) - S(8), S(26)),
                Br(hot ? g_sk.accent : g_sk.dim), 0, 1);
        float ax = g_lo.sortBtn.X + g_lo.sortBtn.Width - S(12), ay = g_lo.sortBtn.Y + S(12);
        Gdiplus::Pen ap(GC(hot ? g_sk.accent : g_sk.dim), S(1.3f));
        PointF pts[3] = { PointF(ax, ay - S(2)), PointF(ax + S(3.5f), ay + S(2)), PointF(ax + S(7), ay - S(2)) };
        g.DrawLines(&ap, pts, 3);
    }
    {   /* 统计 (根合计, 不随前 200 截断) */
        unsigned long long total = g_viewTally.size;
        DrawStr(g, FmtSize(total), F(S(13.6f), true),
                RectF(g_lo.stats.X, g_lo.stats.Y, g_lo.stats.Width, g_lo.stats.Height / 2 + S(3)), Br(g_sk.accent), 2, 0);
        DrawStr(g, L"总占用", F(S(9), false),
                RectF(g_lo.stats.X, g_lo.stats.Y + g_lo.stats.Height / 2, g_lo.stats.Width, g_lo.stats.Height / 2),
                Br(g_sk.dim, 170), 2, 0);
    }
    if (g_sbCollapsed && g_lo.sbToggle.Width > 0) {   /* 展开侧边栏小钮 */
        bool hot = g_hotUi == HOT_SBTOGGLE;
        FillRc(g, g_lo.sbToggle, Br(Mix(g_sk.panel, RGB(255, 255, 255), hot ? 0.10f : 0.05f)), S(7));
        Gdiplus::Pen bp(GC(hot ? g_sk.accent : g_sk.border, hot ? 200 : 70), 1.0f);
        StrokeRc(g, g_lo.sbToggle, &bp, S(7));
        Gdiplus::Pen ap(GC(hot ? g_sk.accent : g_sk.dim), S(1.5f));
        float cx = g_lo.sbToggle.X + S(9), cy = g_lo.sbToggle.Y + S(13);
        PointF pts[3] = { PointF(cx + S(4), cy - S(4)), PointF(cx, cy), PointF(cx + S(4), cy + S(4)) };
        g.DrawLines(&ap, pts, 3);
    }
}

/* ==================== 地图绘制 (瓦片/覆盖层/根标题/空态/加载/框选) ==================== */
static void DrawTiles(Gdiplus::Graphics& g) {
    Gdiplus::Pen linePen(Gdiplus::Color(140, 0, 0, 0), 1.0f);
    for (auto& t : g_tiles) {
        N& n = *t.n;
        float x = t.rc.x, y = t.rc.y, w = t.rc.w, h = t.rc.h;
        g.FillRectangle(Br(TileBg(n, t.depth)), RectF(x, y, w, h));
        bool isDir = n.isDir || n.isDrive;
        if (t.expanded) {
            /* 展开态: 顶部标题条 (源样式 .t-title) + 下方嵌套子项已由递归绘制 */
            float th = t.titleH;
            g.FillRectangle(Br(RGB(0,0,0), 115), RectF(x, y, w, th));
            g.FillRectangle(Br(Mix(TileBg(n, t.depth), RGB(0, 0, 0), 0.55f)), RectF(x, y + th, w, 1));
            float innerW = w - S(10);
            /* 标题条大小文案: "size · N 项" (目录; 数量字段跟随排序模式, 0 不追加) */
            std::wstring cntLbl = L" 项";
            long long cntVal = n.childCount;
            if (g_sortBy == 2) { cntLbl = L" 文件"; cntVal = n.fileCount; }
            if (g_sortBy == 3) { cntLbl = L" 个文件夹"; cntVal = n.dirCount; }
            std::wstring sizeTxt = FmtSize(n.size);
            if (cntVal > 0) sizeTxt += L" · " + FmtCount(cntVal) + cntLbl;
            float sizeW = MeasureStr(g, sizeTxt, F(S(9), true));
            float foldW = S(10);
            Gdiplus::SolidBrush sizeBr(Gdiplus::Color(217, 255, 255, 255));
            /* 标题条内容整体裁剪在本格内 (源样式 .t-title overflow hidden): 窄格放不下
               "size · N 项" 时右对齐文本向左溢出到兄弟格标题条上 = 文字重叠 (2026-09-19 实锤) */
            Gdiplus::GraphicsState tst = g.Save();
            g.SetClip(RectF(x + 1, y, w - 2, th));
            DrawStr(g, sizeTxt, F(S(9), true), RectF(x + w - S(5) - foldW - sizeW, y, sizeW, th), &sizeBr, 2, 1);
            DrawStr(g, L"▾", F(S(9.6f), false), RectF(x + w - S(5) - foldW, y, foldW, th),
                    Br(RGB(255,255,255), 178), 1, 1, false, false);
            DrawStr(g, n.name, F(S(9.9f), true), RectF(x + S(5), y, std::max(innerW - sizeW - foldW - S(6), 8.0f), th),
                    Br(RGB(255, 255, 255)), 0, 1);
            g.Restore(tst);
        } else {
            /* 折叠态: 名称(≤2行) + 大小徽章 + 数量行 (源样式 .tile-content + overflow hidden) */
            COLORREF fg = TileFg(n);
            bool isSmall = (w < S(60) || h < S(34));
            bool isTiny = (w < S(30) || h < S(18));
            if (!isTiny) {
                /* 内容整体裁剪在格子内: 块高超出格高时溢出到相邻格 = 文字重叠 (2026-09-19 实锤) */
                Gdiplus::GraphicsState cst = g.Save();
                g.SetClip(RectF(x + 1, y + 1, w - 2, h - 2));
                if (isSmall) {
                    DrawStr(g, n.name, F(S(9.6f), true), RectF(x + S(2), y, w - S(4), h), Br(fg), 1, 1, true);
                } else {
                    float lh = S(14.2f);
                    float nameW = w * 0.95f;
                    float nameH = MeasureH(g, n.name, F(S(10.9f), true), nameW);
                    if (nameH > lh * 2) nameH = lh * 2;
                    std::wstring cntTxt;
                    if (n.isDir) {
                        long long cntVal = g_sortBy == 2 ? n.fileCount : g_sortBy == 3 ? n.dirCount : n.childCount;
                        if (cntVal > 0) {
                            std::wstring lbl = g_sortBy == 2 ? L" 文件" : g_sortBy == 3 ? L" 个文件夹" : L" 项";
                            cntTxt = FmtCount(cntVal) + lbl;
                        }
                    }
                    float pillH = S(15), pillW = MeasureStr(g, FmtSize(n.size), F(S(9.3f), true)) + S(10);
                    float cntH = cntTxt.empty() ? 0 : S(11);
                    /* 渐进隐藏: 块高超过格高先去数量行, 再去徽章, 只保名称 (源样式 overflow hidden 观感) */
                    bool showCnt = !cntTxt.empty(), showPill = true;
                    float blockH = nameH + S(2) + pillH + (showCnt ? S(2) + cntH : 0);
                    if (blockH > h && showCnt) { showCnt = false; blockH = nameH + S(2) + pillH; }
                    if (blockH > h) { showPill = false; blockH = std::min(nameH, h); }
                    float by = y + (h - blockH) / 2;
                    {   /* 名称 ≤2 行: 裁剪 + 居中折行 */
                        Gdiplus::GraphicsState st = g.Save();
                        g.SetClip(RectF(x + S(2), by, w - S(4), nameH + 2));
                        Gdiplus::StringFormat sf;
                        sf.SetAlignment(Gdiplus::StringAlignmentCenter);
                        sf.SetLineAlignment(Gdiplus::StringAlignmentNear);
                        sf.SetTrimming(Gdiplus::StringTrimmingEllipsisWord);
                        g.DrawString(n.name.c_str(), (INT)n.name.size(), F(S(10.9f), true),
                                     RectF(x + w * 0.025f, by - S(1), nameW, nameH + 4), &sf, Br(fg));
                        g.Restore(st);
                    }
                    if (showPill) {
                        float py = by + nameH + S(2);
                        float px = x + (w - pillW) / 2;
                        g.FillRectangle(Br(RGB(0,0,0), 64), RectF(px, py, pillW, pillH));
                        DrawStr(g, FmtSize(n.size), F(S(9.3f), true), RectF(px, py, pillW, pillH), Br(fg), 1, 1);
                        if (showCnt) {
                            Gdiplus::SolidBrush cb(Gdiplus::Color(204, GetRValue(fg), GetGValue(fg), GetBValue(fg)));
                            DrawStr(g, cntTxt, F(S(8.6f), false), RectF(x, py + pillH + S(1), w, cntH), &cb, 1, 0);
                        }
                    }
                }
                g.Restore(cst);
            }
        }
        /* 单线边框 (相邻格子边框重叠成一条线, 源样式 1px rgba(0,0,0,.55)) */
        g.DrawRectangle(&linePen, RectF(x, y, w - 1, h - 1));
    }
}
/* 覆盖层: 上次点击(红) / 多选(紫+白描边+✓) / 悬停(青) / 定位高亮(橙) — 依瓦片序绘制, 子块盖父块 */
static void DrawTileOverlays(Gdiplus::Graphics& g) {
    for (int i = 0; i < (int)g_tiles.size(); i++) {
        const Tile& t = g_tiles[i];
        N& n = *t.n;
        RectF rc(t.rc.x, t.rc.y, t.rc.w, t.rc.h);
        bool sel = g_sel.count(n.path) != 0;
        if (PinKey(n.path) == g_pinned && !g_pinned.empty()) {
            g.DrawRectangle(PenP(CSS(0xff3b30), 255, S(2)), RectF(rc.X + 1, rc.Y + 1, rc.Width - 2, rc.Height - 2));
            Gdiplus::Pen glow(GC(CSS(0xff3b30), 70), S(6));
            g.DrawRectangle(&glow, RectF(rc.X - 2, rc.Y - 2, rc.Width + 4, rc.Height + 4));
        }
        if (sel) {
            g.FillRectangle(Br(g_sk.accent, 77), rc);
            g.DrawRectangle(PenP(RGB(255,255,255), 255, S(2)), RectF(rc.X + 1, rc.Y + 1, rc.Width - 2, rc.Height - 2));
            Gdiplus::Pen ring(GC(g_sk.accent), S(3));
            g.DrawRectangle(&ring, RectF(rc.X + 3.5f, rc.Y + 3.5f, rc.Width - 7 < 1 ? 1.0f : rc.Width - 7, rc.Height - 7 < 1 ? 1.0f : rc.Height - 7));
            RectF b(rc.X + S(2), rc.Y + S(2), S(14), S(14));
            g.FillRectangle(Br(g_sk.accent), b);
            g.DrawRectangle(PenP(RGB(255,255,255), 230, S(2)), b);
            DrawStr(g, L"✓", F(S(10), true), b, Br(RGB(255, 255, 255)), 1, 1, false, false);
        }
        if (i == g_hover) {
            g.FillRectangle(Br(RGB(255,255,255), 26), rc);
            g.DrawRectangle(PenP(CSS(0x00d4ff), 255, S(2)), RectF(rc.X + 1, rc.Y + 1, rc.Width - 2, rc.Height - 2));
        }
        if (!g_hlPath.empty() && TrimBs(n.path) == g_hlPath) {
            g.FillRectangle(Br(CSS(0xff9500), 64), rc);
            g.DrawRectangle(PenP(CSS(0xff9500), 255, S(3)), RectF(rc.X + 1, rc.Y + 1, rc.Width - 2, rc.Height - 2));
        }
    }
}
static void DrawStage(Gdiplus::Graphics& g) {
    RECT sr = StageRect();
    RectF st((float)sr.left, (float)sr.top, (float)(sr.right - sr.left), (float)(sr.bottom - sr.top));
    if (st.Width < 2 || st.Height < 2) return;
    if (!(g_loading) && g_items && !g_items->empty()) g_backBtnRc = RectF(0, 0, 0, 0);
    /* 放大进入: 顶部根标题条 (源样式 .root-title, 24px, 点击返回上级) */
    if (g_pathStack.size() > 1) {
        RectF rt(st.X, st.Y, st.Width, S(24));
        g.FillRectangle(Br(RGB(0,0,0), 140), rt);
        std::wstring nm = PathName(TrimBs(g_pathStack.back()));
        if (nm.empty()) nm = TrimBs(g_pathStack.back());
        unsigned long long tot = g_viewTally.size;   /* 根合计 */
        Gdiplus::SolidBrush sizeBr(Gdiplus::Color(217, 255, 255, 255));
        float nw = MeasureStr(g, L"▾ " + nm, F(S(11.2f), true));
        DrawStr(g, L"▾ " + nm, F(S(11.2f), true), RectF(rt.X + S(10), rt.Y, st.Width - S(20), S(24)), Br(RGB(255, 255, 255)), 0, 1);
        DrawStr(g, FmtSize(tot), F(S(9.6f), true), RectF(rt.X + S(10) + nw + S(10), rt.Y, st.Width - nw - S(40), S(24)), &sizeBr, 0, 1);
        DrawStr(g, L"◂ 返回上级", F(S(9.9f), false), RectF(rt.X + rt.Width - S(110), rt.Y, S(100), S(24)),
                Br(RGB(255,255,255), 204), 2, 1);
    }
    if (g_loading) {   /* 加载遮罩 (panel 78% + 旋转弧 + 文案) */
        Gdiplus::SolidBrush mask(Gdiplus::Color(199, GetRValue(g_sk.panel), GetGValue(g_sk.panel), GetBValue(g_sk.panel)));
        g.FillRectangle(&mask, st);
        float cx = st.X + st.Width / 2, cy = st.Y + st.Height / 2 - S(14);
        float ang = (g_spinTick % 60) * 6.0f;
        Gdiplus::Pen sp(GC(g_sk.accent), S(2));
        g.DrawArc(&sp, RectF(cx - S(11), cy - S(11), S(22), S(22)), ang, 300);
        DrawStr(g, g_loadText, F(S(12.8f), false), RectF(st.X, cy + S(20), st.Width, S(22)), Br(g_sk.dim), 1, 1);
    } else if (!g_items || g_items->empty()) {
        g_backBtnRc = RectF(0, 0, 0, 0);
        /* 空态: 图标 + 文案 (+ 返回按钮) */
        float cx = st.X + st.Width / 2, cy = st.Y + st.Height / 2;
        DrawFileIcon(g, RectF(cx - S(20), cy - S(34), S(40), S(40)), g_sk.dim, 77);
        std::wstring txt = g_status.find(L"失败") != std::wstring::npos ? g_status : L"此目录为空";
        DrawStr(g, txt, F(S(13.6f), false), RectF(st.X, cy + S(12), st.Width, S(22)), Br(g_sk.dim, 128), 1, 1);
        if (g_pathStack.size() > 1) {
            std::wstring bl = L"◂ 返回上级";
            float bw = MeasureStr(g, bl, F(S(12.5f), true)) + S(40), bh = S(32);
            RectF b(cx - bw / 2, cy + S(42), bw, bh);
            g_backBtnRc = b;
            bool hot = g_hotUi == HOT_BACKBTN;
            FillRc(g, b, Br(hot ? Mix(g_sk.accent, RGB(255, 255, 255), 0.15f) : g_sk.accent), 0);
            StrokeRc(g, b, PenP(g_sk.accent, 255, 1.0f), 0);
            DrawStr(g, bl, F(S(12.5f), true), b, Br(RGB(255, 255, 255)), 1, 1);
        }
    }
    if (g_marqueeOn) {   /* Shift 框选选区 */
        RectF mq((float)std::min(g_marqueeStart.x, g_marqueeCur.x), (float)std::min(g_marqueeStart.y, g_marqueeCur.y),
                 (float)std::abs(g_marqueeCur.x - g_marqueeStart.x), (float)std::abs(g_marqueeCur.y - g_marqueeStart.y));
        g.FillRectangle(Br(g_sk.accent, 46), mq);
        g.DrawRectangle(PenP(g_sk.accent, 255, 1.0f), mq);
    }
}

/* ==================== 侧边栏 (评估面板) ==================== */
/* 内容构造: draw=false 仅累计高度 (滚动条), draw=true 绘制在 y-offset */
static float SbPrivacy(Gdiplus::Graphics* g, float x, float y, float w, bool draw) {
    float h = 0;
    auto iconBox = [&](float cy) {
        if (draw) {
            RectF ic(x + (w - S(34)) / 2, cy, S(34), S(34));
            FillRc(*g, ic, Br(g_sk.accent, 36), S(10));
            Gdiplus::Pen pen(GC(g_sk.accent), S(1.4f));
            float ix = ic.X + ic.Width * 0.25f, iw = ic.Width * 0.5f;
            StrokeRc(*g, RectF(ix, ic.Y + ic.Height * 0.45f, iw, ic.Height * 0.35f), &pen, S(2));
            g->DrawArc(&pen, RectF(ix + iw * 0.15f, ic.Y + ic.Height * 0.12f, iw * 0.7f, iw * 0.7f), 180, 180);
        }
        return S(34) + S(10);
    };
    h += iconBox(y + h);
    auto title = [&](const std::wstring& s, float fs, COLORREF c, bool bold) {
        if (draw) DrawStr(*g, s, F(S(fs), bold), RectF(x, y + h, w, S(fs * 1.6f)), Br(c), 1, 0);
        return S(fs * 1.6f) + S(6);
    };
    auto para = [&](const std::wstring& s, float fs, COLORREF c, float inset = 0) {
        float tw = w - inset;
        float ph = MeasureH(*g, s, F(S(fs), false), tw);
        if (draw) DrawStr(*g, s, F(S(fs), false), RectF(x + inset, y + h, tw, ph + S(2)), Br(c), 0, 0, true);
        return ph + S(8);
    };
    h += title(L"使用前请先确认隐私条款", 11.5f, g_sk.text, true);
    h += para(L"「路径评估」功能会将文件/文件夹的路径名称发送至云端 (不含数据), 用于识别所属软件与删除风险, 并给出评估结果。", 10.6f, g_sk.dim);
    {   /* 这个功能有什么用? 列表 (先量后画: 盒高随内容折行, 曾用固定盒高导致文字溢出盒外) */
        const wchar_t* lis[3] = {
            L"点击地图上的任意文件/文件夹, 就能识别它属于哪个软件、是做什么用的。",
            L"给出推荐操作与安全指引: 哪些能安全清理、哪些绝不能删, 一目了然。",
            L"清理前先评估, 避免误删系统文件与重要数据。" };
        float pad = S(9.6f), textW = w - pad * 2 - S(11) - S(6);
        float lhs[3] = { 0, 0, 0 };
        float listH = 0;
        for (int i = 0; i < 3; i++) { lhs[i] = MeasureH(*g, lis[i], F(S(10.2f), false), textW); listH += lhs[i] + (i < 2 ? S(4.8f) : 0); }
        float boxH = S(8.8f) * 2 + S(13.8f) + S(6.4f) + listH;
        float by2 = y + h;
        if (draw) {
            RectF box(x, by2, w, boxH);
            FillRc(*g, box, Br(Mix(g_sk.panel, RGB(255, 255, 255), 0.03f)), S(9.6f));
            Gdiplus::Pen pb(GC(g_sk.border, 40), 1.0f);
            StrokeRc(*g, box, &pb, S(9.6f));
            DrawStr(*g, L"这个功能有什么用?", F(S(10.6f), true), RectF(x + pad, box.Y + S(8.8f), w - pad * 2, S(14)), Br(g_sk.accent), 0, 0);
            /* 三枚 11px 线稿小图标 (源样式 SVG: 放大镜 / 标签 / 盾牌), 青 */
            float iy = box.Y + S(8.8f) + S(13.8f) + S(6.4f);
            for (int i = 0; i < 3; i++) {
                RectF ic(x + pad, iy + S(2), S(11), S(11));
                Gdiplus::Pen ip(GC(CSS(0x00d4ff)), S(1.2f));
                if (i == 0) {   /* 放大镜 */
                    g->DrawEllipse(&ip, RectF(ic.X, ic.Y, S(7), S(7)));
                    g->DrawLine(&ip, ic.X + S(6.5f), ic.Y + S(6.5f), ic.X + S(10.5f), ic.Y + S(10.5f));
                } else if (i == 1) {   /* 标签 */
                    PointF pts[5] = { PointF(ic.X, ic.Y + S(1)), PointF(ic.X + S(7), ic.Y + S(1)),
                                      PointF(ic.X + S(10.5f), ic.Y + S(5.5f)), PointF(ic.X + S(5.5f), ic.Y + S(10.5f)),
                                      PointF(ic.X, ic.Y + S(5.5f)) };
                    g->DrawPolygon(&ip, pts, 5);
                    g->FillEllipse(Br(g_sk.panel), RectF(ic.X + S(2), ic.Y + S(2.5f), S(2), S(2)));
                } else {   /* 盾牌 */
                    PointF pts[5] = { PointF(ic.X + S(5.5f), ic.Y), PointF(ic.X + S(10.5f), ic.Y + S(2)),
                                      PointF(ic.X + S(10.5f), ic.Y + S(6)), PointF(ic.X + S(5.5f), ic.Y + S(11)),
                                      PointF(ic.X, ic.Y + S(6)) };
                    g->DrawPolygon(&ip, pts, 5);
                }
                DrawStr(*g, lis[i], F(S(10.2f), false), RectF(x + pad + S(11) + S(6), iy, textW, lhs[i] + S(2)), Br(g_sk.dim), 0, 0, true);
                iy += lhs[i] + S(4.8f);
            }
        }
        h += boxH + S(8);
    }
    {   /* 警示框 */
        std::wstring wn = L"仅上传路径名称, 不上传任何文件内容或数据。路径中可能包含用户名等敏感信息(无数据泄漏风险), 请知悉并谨慎同意。AI提供的信息仅供参考.本插件不提供删除功能.删除需谨慎.";
        float tw = w - S(24);
        float ph = MeasureH(*g, wn, F(S(10.2f), false), tw);
        float bh = ph + S(16);
        if (draw) {
            RectF box(x, y + h, w, bh);
            FillRc(*g, box, Br(CSS(0xff5c8a), 20), S(9));
            StrokeRc(*g, box, PenP(CSS(0xff5c8a), 46, 1.0f), S(9));
            DrawStr(*g, wn, F(S(10.2f), false), RectF(x + S(10), y + h + S(8), tw, ph + S(2)), Br(CSS(0xc9a8bc)), 0, 0, true);
        }
        h += bh + S(10);
    }
    h += para(L"服务器有效期至 2026-11-11。到期后或 DeepSeek 额度不足时，服务可能不可用", 10.2f, g_sk.dim);
    {   /* 按钮: 同意 / 不同意 */
        float bw = w * 0.6f, bh = S(30);
        RectF b1(x + (w - bw) / 2, y + h, bw, bh);
        if (draw) {
            RectF gr(b1.X, b1.Y, b1.Width, b1.Height);
            LinearGradientBrush lg(gr, GC(g_sk.accent), GC(CSS(0x00d4ff)), 35.0f);
            Gdiplus::GraphicsPath* p = RoundPath(gr, S(8));
            g->FillPath(&lg, p);
            delete p;
            DrawStr(*g, L"同意并开始使用", F(S(11.2f), true), b1, Br(RGB(255, 255, 255)), 1, 1);
        }
        g_hmRc1 = b1;
        h += bh + S(8);
        RectF b2(x + (w - bw) / 2, y + h, bw, bh);
        if (draw) {
            FillRc(*g, b2, Br(Mix(g_sk.panel, RGB(255, 255, 255), 0.04f)), S(8));
            Gdiplus::Pen pb(GC(g_sk.border, 90), 1.0f);
            StrokeRc(*g, b2, &pb, S(8));
            DrawStr(*g, L"不同意, 收起面板", F(S(11.2f), false), b2, Br(g_sk.dim), 1, 1);
        }
        g_hmRc2 = b2;
        h += bh + S(6);
    }
    return h;
}
static float SbGuide(Gdiplus::Graphics* g, float x, float y, float w, bool draw) {
    float h = S(20);
    if (draw) {
        RectF ic(x + (w - S(34)) / 2, y + h, S(34), S(34));
        FillRc(*g, ic, Br(g_sk.accent, 36), S(10));
        Gdiplus::Pen pen(GC(g_sk.accent), S(1.4f));
        g->DrawEllipse(&pen, RectF(ic.X + S(8), ic.Y + S(8), S(15), S(15)));
        g->DrawLine(&pen, ic.X + S(21), ic.Y + S(21), ic.X + S(27), ic.Y + S(27));
        g->DrawLines(&pen, std::vector<PointF>{ PointF(ic.X + S(12), ic.Y + S(16)), PointF(ic.X + S(14.5f), ic.Y + S(18.5f)),
                    PointF(ic.X + S(19), ic.Y + S(13)) }.data(), 3);
    }
    h += S(34) + S(12);
    if (draw) DrawStr(*g, L"路径评估", F(S(12.5f), true), RectF(x, y + h, w, S(20)), Br(g_sk.text), 1, 0);
    h += S(20) + S(8);
    {
        std::wstring d = L"点击地图上的任意文件或文件夹, 将在此显示它属于哪个软件、有什么风险, 以及能否安全清理。";
        float ph = MeasureH(*g, d, F(S(11.2f), false), std::min(w, S(220)));
        if (draw) DrawStr(*g, d, F(S(11.2f), false), RectF(x + (w - std::min(w, S(220))) / 2, y + h, std::min(w, S(220)), ph + S(2)), Br(g_sk.dim), 0, 0, true);
        h += ph + S(10);
    }
    {   /* 红点提示 */
        std::wstring t = L"仅上传路径名称, 不上传任何文件内容";
        float tw = MeasureStr(*g, t, F(S(9.9f), false)) + S(12);
        if (draw) {
            g->FillEllipse(Br(CSS(0xff5c8a)), RectF(x + (w - tw) / 2, y + h + S(6), S(5), S(5)));
            DrawStr(*g, t, F(S(9.9f), false), RectF(x + (w - tw) / 2 + S(9), y + h, tw, S(18)), Br(g_sk.dim, 140), 0, 1);
        }
        h += S(22);
    }
    return h + S(10);
}
static float SbEvalLoading(Gdiplus::Graphics* g, float x, float y, float w, bool draw) {
    if (draw) {
        float cy = y + S(30);
        float ang = (g_spinTick % 60) * 6.0f;
        Gdiplus::Pen sp(GC(g_sk.dim), S(2));
        g->DrawArc(&sp, RectF(x + w / 2 - S(7.5f), cy - S(7.5f), S(15), S(15)), ang, 300);
        DrawStr(*g, L"正在评估「" + PathName(g_evalPath) + L"」...", F(S(11.5f), false),
                RectF(x, cy + S(18), w, S(20)), Br(g_sk.dim), 1, 0);
    }
    return S(70);
}
static float SbEvalResult(Gdiplus::Graphics* g, float x, float y, float w, bool draw) {
    const EvalData& d = g_evalData;
    float pad = S(11);
    float cw = w - S(2);
    /* 卡片底需要真实高度: draw 模式先自测内容高 */
    float hTotal = draw ? SbEvalResult(NULL, x, y, w, false) : 0;
    Gdiplus::Graphics mscratch(g_hwnd ? g_hwnd : GetDesktopWindow());
    Gdiplus::Graphics& gg = g ? *g : mscratch;
    if (draw) {
        RectF card(x, y, cw, hTotal - S(12));
        FillRc(gg, card, Br(Mix(g_sk.panel, RGB(255, 255, 255), 0.03f)), S(11));
        Gdiplus::Pen cbp(GC(g_sk.border, 40), 1.0f);
        StrokeRc(gg, card, &cbp, S(11));
    }
    float h = 0;
    float ix = x + pad, iw = cw - pad * 2;
    auto line = [&](const std::wstring& s, float fs, COLORREF c, bool bold, float lh = 1.6f) {
        float ph = MeasureH(gg, s, F(S(fs), bold), iw);
        if (draw) DrawStr(gg, s, F(S(fs), bold), RectF(ix, y + h, iw, ph + S(2)), Br(c), 0, 0, true);
        h += ph + S(6) * (lh / 1.6f);
    };
    {   /* 名称 + 类别 pill (源样式 .ev-head flex-wrap: 名称超宽折行, pill 放不下换行自占一行) */
        float nw = MeasureStr(gg, d.name, F(S(14), true));
        float ctw = d.cat.empty() ? 0 : MeasureStr(gg, d.cat, F(S(10.9f), false)) + S(16);
        if (ctw && nw + S(8) + ctw <= iw) {   /* 同行放得下: 名称 + pill 一行 */
            if (draw) {
                DrawStr(gg, d.name, F(S(14), true), RectF(ix, y + h, nw + S(4), S(22)), Br(g_sk.text), 0, 0);
                RectF pc(ix + nw + S(8), y + h + S(3), ctw, S(16));
                FillRc(gg, pc, Br(g_sk.accent, 36), S(8));
                Gdiplus::Pen pb(GC(g_sk.border, 70), 1.0f);
                StrokeRc(gg, pc, &pb, S(8));
                DrawStr(gg, d.cat, F(S(10.9f), false), pc, Br(g_sk.accent), 1, 1);
            }
            h += S(24);
        } else {   /* 名称折行 (长 GUID/长名), pill 换行自占一行 */
            float nh = MeasureH(gg, d.name, F(S(14), true), iw);
            if (draw) DrawStr(gg, d.name, F(S(14), true), RectF(ix, y + h, iw, nh + S(2)), Br(g_sk.text), 0, 0, true);
            h += nh + S(4);
            if (ctw) {
                if (draw) {
                    RectF pc(ix, y + h + S(3), ctw, S(16));
                    FillRc(gg, pc, Br(g_sk.accent, 36), S(8));
                    Gdiplus::Pen pb(GC(g_sk.border, 70), 1.0f);
                    StrokeRc(gg, pc, &pb, S(8));
                    DrawStr(gg, d.cat, F(S(10.9f), false), pc, Br(g_sk.accent), 1, 1);
                }
                h += S(21);
            }
        }
    }
    if (!d.product.empty()) line(L"产品: " + d.product, 11.2f, g_sk.dim, false, 1.3f);
    if (!d.vendor.empty()) line(L"厂商: " + d.vendor, 11.2f, g_sk.dim, false, 1.3f);
    if (!d.desc.empty()) line(d.desc, 12, g_sk.dim, false, 1.7f);
    {   /* 指标块: 风险等级 / 推荐操作 / 可靠性 / 删除后重建 */
        struct M { std::wstring l, v; COLORREF c; };
        std::vector<M> ms;
        if (d.score >= 0) {
            ms.push_back({ L"风险等级", d.score <= 1 ? L"高" : (d.score <= 3 ? L"中" : L"低"),
                           d.score <= 1 ? (COLORREF)CSS(0xff4d5e) : (d.score <= 3 ? (COLORREF)CSS(0xe8b93a) : (COLORREF)CSS(0x22c55e)) });
        }
        if (!d.action.empty()) ms.push_back({ L"推荐操作", d.action, g_sk.text });
        if (!d.reliability.empty()) ms.push_back({ L"可靠性", d.reliability, g_sk.text });
        if (d.rebuild >= 0) ms.push_back({ L"删除后重建", d.rebuild ? L"会" : L"不会", d.rebuild ? (COLORREF)CSS(0xe8b93a) : (COLORREF)CSS(0x22c55e) });
        if (!ms.empty()) {
            /* 源样式 .ev-metric flex-wrap + min-width 86px: 侧栏宽度下自然折行 (4 项 = 2×2 网格) */
            float gap = S(7);
            float mh = S(48);
            int perRow = (int)std::max(1.0f, std::floor((iw + gap) / (S(86) + gap)));
            int i = 0;
            while (i < (int)ms.size()) {
                int rn = std::min(perRow, (int)ms.size() - i);
                float mw = (iw - gap * (rn - 1)) / rn;
                float mx = ix;
                for (int k = 0; k < rn; k++, i++) {
                    auto& m = ms[i];
                    if (draw) {
                        RectF box(mx, y + h, mw, mh);
                        FillRc(gg, box, Br(Mix(g_sk.panel, RGB(255, 255, 255), 0.02f)), S(9));
                        Gdiplus::Pen pb(GC(g_sk.border, 40), 1.0f);
                        StrokeRc(gg, box, &pb, S(9));
                        DrawStr(gg, m.l, F(S(10.6f), false), RectF(mx, box.Y + S(6), mw, S(14)), Br(g_sk.dim, 150), 1, 0);
                        DrawStr(gg, m.v, F(S(14.4f), true), RectF(mx + S(2), box.Y + S(22), mw - S(4), S(18)), Br(m.c), 1, 0);
                    }
                    mx += mw + gap;
                }
                h += mh;
            }
            h += S(8);
        }
    }
    if (!d.site.empty()) {   /* 官网链接 (可点击; 宽度钳剩余空间, 长 URL 靠默认省略号截断) */
        std::wstring lbl = L"官网: ";
        float lw = MeasureStr(gg, lbl, F(S(11.2f), false));
        float sw2 = std::min(MeasureStr(gg, d.site, F(S(11.2f), false)), iw - lw);
        if (draw) {
            DrawStr(gg, lbl, F(S(11.2f), false), RectF(ix, y + h, lw, S(18)), Br(g_sk.dim, 150), 0, 0);
            RectF link(ix + lw, y + h, sw2 + S(4), S(18));
            DrawStr(gg, d.site, F(S(11.2f), false), link, Br(g_sk.accent), 0, 0);
            g_siteRc = link;
        } else g_siteRc = RectF(0, 0, 0, 0);
        h += S(20);
    }
    if (!d.note.empty()) {
        if (draw) gg.FillRectangle(Br(g_sk.border, 40), RectF(ix, y + h, iw, 1));
        h += S(6);
        line(L"备注: " + d.note, 11.2f, g_sk.dim, false, 1.6f);
    }
    {   /* AI 提示 */
        if (draw) DrawStr(gg, L"AI 生成, 仅供参考", F(S(9.6f), false), RectF(ix, y + h, iw, S(16)), Br(g_sk.dim, 120), 1, 0);
        h += S(16);
    }
    return h + pad + S(12);
}
static void DrawSidebar(Gdiplus::Graphics& g) {
    if (g_sbCollapsed || g_lo.sidebar.Width < 2) return;
    FillRc(g, g_lo.sidebar, Br(g_sk.panel), S(14));
    Gdiplus::Pen bp(GC(g_sk.border, 70), 1.0f);
    StrokeRc(g, RectF(g_lo.sidebar.X + 0.5f, g_lo.sidebar.Y + 0.5f, g_lo.sidebar.Width - 1, g_lo.sidebar.Height - 1), &bp, S(14));
    /* 头部: 渐变强调底 + 标题 + 收起钮 */
    {
        RectF hd(g_lo.sbHead.X + 1, g_lo.sbHead.Y + 1, g_lo.sidebar.Width - 2, S(40) - 1);
        LinearGradientBrush lg(RectF(hd.X, hd.Y, hd.Width * 0.72f, hd.Height), GC(g_sk.accent, 40), GC(g_sk.accent, 0), 0.0f);
        g.FillRectangle(&lg, hd);
        g.FillRectangle(Br(g_sk.border, 30), RectF(hd.X, hd.Y + hd.Height - 1, hd.Width, 1));
        /* 文档图标 */
        Gdiplus::Pen pen(GC(g_sk.accent), S(1.3f));
        float ix = hd.X + S(14), iy = hd.Y + S(13);
        StrokeRc(g, RectF(ix, iy, S(11), S(15)), &pen, S(1.5f));
        for (int i = 0; i < 4; i++) g.DrawLine(&pen, ix + S(2), iy + S(3) + i * S(3), ix + S(9), iy + S(3) + i * S(3));
        DrawStr(g, L"评估面板", F(S(11.5f), true), RectF(ix + S(16), hd.Y, S(160), S(40)), Br(g_sk.text), 0, 1);
        /* 收起钮 (chevron >) */
        bool hot = g_hotUi == HOT_SBCLOSE;
        if (hot) FillRc(g, g_lo.sbClose, Br(g_sk.accent, 40), S(7));
        Gdiplus::Pen cp(GC(hot ? g_sk.accent : g_sk.dim), S(1.5f));
        float cx = g_lo.sbClose.X + S(10), cy = g_lo.sbClose.Y + S(12);
        PointF pts[3] = { PointF(cx - S(2.5f), cy - S(4)), PointF(cx + S(2.5f), cy), PointF(cx - S(2.5f), cy + S(4)) };
        g.DrawLines(&cp, pts, 3);
    }
    /* 内容 (裁剪 + 滚动) */
    if (g_lo.sbBody.Height < 2) return;
    Gdiplus::GraphicsState st = g.Save();
    g.SetClip(g_lo.sbBody);
    float pad = S(12), x = g_lo.sbBody.X + pad, w = g_lo.sbBody.Width - pad * 2;
    float y = g_lo.sbBody.Y - g_sbScroll + S(6);
    float ch = 0;
    switch (g_sbPage) {
        case SB_PRIVACY: ch = SbPrivacy(&g, x, y, w, true); break;
        case SB_GUIDE: ch = SbGuide(&g, x, y, w, true); break;
        case SB_LOADING: ch = SbEvalLoading(&g, x, y, w, true); break;
        case SB_RESULT: ch = SbEvalResult(&g, x, y, w, true); break;
        case SB_FAIL: {
            std::wstring t = L"「" + PathName(g_evalPath) + L"」评估失败, 请稍后重试; 若持续失败请检查网络或服务可用性";
            DrawStr(g, t, F(S(11.2f), false), RectF(x, y, w, S(80)), Br(CSS(0xff5c8a)), 1, 0, true);
            ch = S(80);
            break;
        }
        case SB_ERROR: {
            RectF box(x, y, w, S(60));
            FillRc(g, box, Br(CSS(0xff4d5e), 20), S(9));
            StrokeRc(g, box, PenP(CSS(0xff4d5e), 50, 1.0f), S(9));
            DrawStr(g, g_evalData.err, F(S(11.5f), false), RectF(x + S(8), y + S(8), w - S(16), S(44)), Br(CSS(0xff4d5e)), 0, 0, true);
            ch = S(70);
            break;
        }
    }
    g.Restore(st);
    g_sbContentH = ch + S(12);
    g_sbMaxScroll = g_sbContentH > g_lo.sbBody.Height ? g_sbContentH - g_lo.sbBody.Height : 0;
    if (g_sbScroll > g_sbMaxScroll) g_sbScroll = g_sbMaxScroll;
    if (g_sbScroll < 0) g_sbScroll = 0;
    /* 滚动条 */
        if (g_sbMaxScroll > 0) {
            float trackH = g_lo.sbBody.Height - S(8);
            float thumbH = std::max(S(24), trackH * g_lo.sbBody.Height / g_sbContentH);
            float t = trackH - thumbH;
            float ty = g_lo.sbBody.Y + S(4) + t * (g_sbScroll / g_sbMaxScroll);
            Gdiplus::GraphicsPath* p = RoundPath(RectF(g_lo.sbBody.X + g_lo.sbBody.Width - S(8), ty, S(6), thumbH), S(3));
            g.FillPath(Br(g_sk.border, 90), p);
            delete p;
        }
}

/* ==================== 图例 / 状态栏 ==================== */
static void DrawLegend(Gdiplus::Graphics& g) {
    FillRc(g, g_lo.legend, Br(g_sk.panel), S(11));
    Gdiplus::Pen bp(GC(g_sk.border, 70), 1.0f);
    StrokeRc(g, RectF(g_lo.legend.X + 0.5f, g_lo.legend.Y + 0.5f, g_lo.legend.Width - 1, g_lo.legend.Height - 1), &bp, S(11));
    float x = g_lo.legend.X + S(14);
    float cy = g_lo.legend.Y + S(17);
    {   /* 色块: 目录 / 文件 */
        g.FillRectangle(Br(DIR_DEPTH_COLORS[0]), RectF(x, cy - S(6.5f), S(13), S(13)));
        DrawStr(g, L"目录", F(S(10.9f), false), RectF(x + S(17), cy - S(9), S(34), S(18)), Br(g_sk.dim), 0, 1);
        x += S(17) + S(34) + S(14);
        g.FillRectangle(Br(TILE_FILE), RectF(x, cy - S(6.5f), S(13), S(13)));
        DrawStr(g, L"文件", F(S(10.9f), false), RectF(x + S(17), cy - S(9), S(34), S(18)), Br(g_sk.dim), 0, 1);
        x += S(17) + S(34) + S(20);
    }
    {   /* 快捷键提示 (kbd 芯片), 超宽裁剪 */
        struct Seg { const wchar_t* t; bool kbd; };
        static const Seg segs[] = {
            { L"单击", true }, { L"展开/收起", false }, { L"双击标题", true }, { L"放大", false },
            { L"Ctrl", true }, { L"多选", false }, { L"Shift", true }, { L"框选·按住拖出", false },
            { L"Esc", true }, { L"/", true }, { L"侧键←", true }, { L"返回", false },
            { L"选择磁盘", true }, { L"/", true }, { L"输入盘符回车", true }, { L"换盘", false } };
        Gdiplus::GraphicsState st = g.Save();
        g.SetClip(RectF(x, g_lo.legend.Y, g_lo.legend.Width - (x - g_lo.legend.X) - S(330), g_lo.legend.Height));
        float fx = x;
        for (auto& sg : segs) {
            if (sg.kbd) {
                float tw = MeasureStr(g, sg.t, F(S(9.9f), false)) + S(10);
                RectF kb(fx, cy - S(9), tw, S(18));
                FillRc(g, kb, Br(Mix(g_sk.panel, RGB(255, 255, 255), 0.06f)), S(4));
                Gdiplus::Pen kbPen(Gdiplus::Color(26, 255, 255, 255), 1.0f);
                StrokeRc(g, kb, &kbPen, S(4));
                DrawStr(g, sg.t, F(S(9.9f), false), kb, Br(g_sk.text), 1, 1);
                fx += tw + S(4);
            } else {
                float tw = MeasureStr(g, sg.t, F(S(9.9f), false));
                DrawStr(g, sg.t, F(S(9.9f), false), RectF(fx, cy - S(9), tw + S(4), S(18)), Br(g_sk.dim), 0, 1);
                fx += tw + S(4) + S(6);
            }
        }
        g.Restore(st);
    }
    {   /* 状态文本 (右) */
        float sw = S(320);
        DrawStr(g, g_status, F(S(10.2f), false),
                RectF(g_lo.legend.X + g_lo.legend.Width - S(14) - sw, cy - S(9), sw, S(18)), Br(g_sk.dim, 140), 2, 1);
    }
}

/* ==================== 弹出菜单 / Tooltip / Toast ==================== */
static void DrawMenu(Gdiplus::Graphics& g) {
    if (!g_menuOpen) return;
    Gdiplus::SolidBrush shadow(Gdiplus::Color(70, 0, 0, 0));
    FillRc(g, RectF(g_menuRc.X + S(3), g_menuRc.Y + S(4), g_menuRc.Width, g_menuRc.Height), &shadow, S(10));
    FillRc(g, g_menuRc, Br(g_sk.panel, 248), S(10));
    StrokeRc(g, g_menuRc, PenP(g_sk.accent, 64, 1.0f), S(10));
    float y = g_menuRc.Y + S(5);
    for (int i = 0; i < (int)g_menu.size(); i++) {
        MItem& m = g_menu[i];
        if (m.sep) {
            g.FillRectangle(Br(g_sk.text, 20), RectF(g_menuRc.X + S(8), y + S(4), g_menuRc.Width - S(16), 1));
            y += S(9);
            continue;
        }
        RectF ir(g_menuRc.X + S(5), y, g_menuRc.Width - S(10), S(32));
        if ((int)i == g_menuHover && !m.disabled) {
            FillRc(g, ir, Br(g_sk.accent, 46), S(7));
        } else if (m.current) {
            FillRc(g, ir, Br(g_sk.accent, 61), S(7));
            StrokeRc(g, ir, PenP(g_sk.accent, 128, 1.0f), S(7));
        }
        COLORREF tc = m.disabled ? g_sk.dim : (i == g_menuHover ? g_sk.accent : (m.current ? g_sk.accent : g_sk.text));
        if (m.icon) DrawMenuIcon(g, m.icon, RectF(ir.X + S(7), ir.Y + S(7), S(18), S(18)), tc);
        DrawStr(g, m.text, F(S(11.8f), false), RectF(ir.X + S(31), ir.Y, ir.Width - S(40), S(32)), Br(tc), 0, 1);
        if (!m.sub.empty()) {
            float sw = MeasureStr(g, m.sub, F(S(9.9f), false));
            DrawStr(g, m.sub, F(S(9.9f), false), RectF(ir.X + ir.Width - sw - S(12) - (m.current ? S(36) : 0), ir.Y, sw + S(4), S(32)),
                    Br(g_sk.dim, 153), 2, 1);
        }
        if (m.current && !m.sub.empty()) {
            DrawStr(g, L"当前", F(S(9.9f), false), RectF(ir.X + ir.Width - S(44), ir.Y, S(36), S(32)), Br(CSS(0x22c55e)), 2, 1);
        }
        y += S(32);
    }
}
static void DrawTooltip(Gdiplus::Graphics& g) {
    if (!g_tipVisible || !g_tipNode) return;
    N& n = *g_tipNode;
    float x = (float)g_tipPos.x + S(15), y = (float)g_tipPos.y + S(15);
    /* 内容测量 */
    float w = S(240);
    float rows = 0;
    float pad = S(12);
    std::vector<std::pair<std::wstring, std::wstring>> rws;
    rws.push_back({ L"类型", n.isDrive ? L"驱动器" : (n.isDir ? L"文件夹" : L"文件") });
    double ratio = g_tipTotal > 0 ? std::min((double)TileWeight(n) / (double)g_tipTotal, 1.0) : 0;
    if (n.isDrive && n.total) {
        rws.push_back({ L"已用", FmtSize(n.size) });
        rws.push_back({ L"可用", FmtSize(n.free) });
        rws.push_back({ L"总容量", FmtSize(n.total) });
        rws.push_back({ L"占用率", std::to_wstring((int)((double)n.size / (double)n.total * 100.0 + 0.5)) + L"%" });
    } else {
        rws.push_back({ L"大小", FmtSize(n.size) });
        wchar_t rb[32]; swprintf(rb, 32, L"%.2f%%", ratio * 100);
        rws.push_back({ L"占比", rb });
        if (n.isDir && n.childCount > 0) {
            rws.push_back({ L"子项", FmtInt(n.childCount) + L" (文件 " + FmtInt(n.fileCount) + L" · 文件夹 " + FmtInt(n.dirCount) + L")" });
        }
    }
    std::wstring pathTxt = n.isDrive ? L"" : n.path;
    float maxRowW = 0;
    Graphics gi(g_hwnd);
    for (auto& r : rws) maxRowW = std::max(maxRowW, S(52) + MeasureStr(gi, r.second, F(S(11.2f), false)));
    w = std::min(std::max(maxRowW + pad * 2, S(200)), S(340));
    float headH = S(26);
    float pathH = pathTxt.empty() ? 0 : MeasureH(gi, pathTxt, F(S(11.2f), false), w - pad * 2) + S(4);
    float h = pad + headH + rows * 0 + rws.size() * S(19) + (pathH ? pathH : 0) + S(4 + 10) + pad;
    /* 贴视口翻转 */
    RECT crc; GetClientRect(g_hwnd, &crc);
    if (x + w > crc.right - S(10)) x = (float)g_tipPos.x - w - S(15);
    if (y + h > crc.bottom - S(10)) y = (float)g_tipPos.y - h - S(15);
    g_tipRc = RectF(x, y, w, h);
    Gdiplus::SolidBrush shadow(Gdiplus::Color(90, 0, 0, 0));
    FillRc(g, RectF(x + S(3), y + S(4), w, h), &shadow, S(10));
    FillRc(g, g_tipRc, Br(g_sk.panel, 242), S(10));
    StrokeRc(g, g_tipRc, PenP(g_sk.accent, 64, 1.0f), S(10));
    float cy2 = y + pad;
    {   /* 头: 图标 + 名称 */
        RectF ic(x + pad, cy2, S(15), S(15));
        if (n.isDrive) DrawDriveIcon(g, ic, g_sk.text);
        else if (n.isDir) DrawFolderIcon(g, ic, g_sk.text);
        else DrawFileIcon(g, ic, g_sk.text);
        DrawStr(g, n.name, F(S(13.1f), true), RectF(x + pad + S(20), cy2 - S(3), w - pad * 2 - S(20), S(22)), Br(g_sk.text), 0, 1);
        cy2 += headH;
        g.FillRectangle(Br(g_sk.text, 15), RectF(x + pad, cy2, w - pad * 2, 1));
        cy2 += S(6);
    }
    for (auto& r : rws) {
        DrawStr(g, r.first, F(S(11.2f), false), RectF(x + pad, cy2, w - pad * 2, S(18)), Br(g_sk.dim), 0, 0);
        DrawStr(g, r.second, F(S(11.2f), false), RectF(x + pad, cy2, w - pad * 2 - S(2), S(18)), Br(g_sk.text), 2, 0, true);
        cy2 += S(19);
    }
    if (pathH) {
        DrawStr(g, L"路径", F(S(11.2f), false), RectF(x + pad, cy2, S(52), S(18)), Br(g_sk.dim), 0, 0);
        DrawStr(g, pathTxt, F(S(11.2f), false), RectF(x + pad + S(52), cy2, w - pad * 2 - S(52), pathH), Br(g_sk.text), 0, 0, true);
        cy2 += pathH;
    }
    {   /* 占比进度条 */
        float by = y + h - pad - S(4);
        g.FillRectangle(Br(g_sk.text, 15), RectF(x + pad, by, w - pad * 2, S(4)));
        if (ratio > 0) {
            RectF fill(x + pad, by, (float)((w - pad * 2) * ratio), S(4));
            LinearGradientBrush lg(fill, GC(g_sk.accent), GC(CSS(0x00d4ff)), 0.0f);
            g.FillRectangle(&lg, fill);
        }
    }
}
static void DrawToast(Gdiplus::Graphics& g) {
    if (!g_toastShow) return;
    RECT sr = StageRect();
    float cx = sr.left + (sr.right - sr.left) / 2;
    float cy = sr.top + (sr.bottom - sr.top) * 0.42f;
    float tw = MeasureStr(g, g_toastText, F(S(15.2f), true)) + S(52);
    float th = S(46);
    RectF rc(cx - tw / 2, cy - th / 2, tw, th);
    Gdiplus::SolidBrush shadow(Gdiplus::Color(120, 0, 0, 0));
    FillRc(g, RectF(rc.X + S(4), rc.Y + S(6), tw, th), &shadow, 0);
    FillRc(g, rc, Br(g_sk.accent), 0);
    StrokeRc(g, rc, PenP(RGB(255,255,255), 255, S(2)), 0);
    DrawStr(g, g_toastText, F(S(15.2f), true), rc, Br(RGB(255, 255, 255)), 1, 1);
}

/* ==================== 整帧绘制 (双缓冲) ==================== */
static void DrawFrame(HDC dc) {
    RECT crc; GetClientRect(g_hwnd, &crc);
    int w = crc.right - crc.left, h = crc.bottom - crc.top;
    if (w < 1 || h < 1) return;
    Gdiplus::Bitmap mem(w, h, PixelFormat32bppPARGB);
    Gdiplus::Graphics g(&mem);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    /* 背景: 160° 渐变 bg1→bg2 (源样式 body linear-gradient) */
    {
        LinearGradientBrush lg(RectF(0, 0, (REAL)w, (REAL)h), GC(g_sk.bg1), GC(g_sk.bg2), 160.0f);
        g.FillRectangle(&lg, 0, 0, w, h);
    }
    ComputeLayout();
    DrawTitlebar(g);
    DrawNavbar(g);
    DrawTiles(g);
    DrawTileOverlays(g);
    DrawStage(g);
    {   /* 侧边栏分隔拖拽条 */
        if (g_lo.resizer.Width > 0) {
            bool hot = g_hotUi == HOT_SBRESIZER || g_sbDrag;
            g.FillRectangle(Br(hot ? g_sk.accent : g_sk.border, hot ? 200 : 90),
                            RectF(g_lo.resizer.X, g_lo.resizer.Y, hot ? S(4) : S(3), g_lo.resizer.Height));
        }
        DrawSidebar(g);
    }
    DrawLegend(g);
    DrawMenu(g);
    if (!g_menuOpen) DrawTooltip(g);   /* 菜单打开期间不画 Tooltip (无论状态如何都不压菜单) */
    DrawToast(g);
    Gdiplus::Graphics scr(dc);
    scr.DrawImage(&mem, 0, 0);   /* 1:1 直拷: 带目标宽高的重载走双线性重采样, 整帧文字/边框发虚 */
}

/* ==================== 交互动作 ==================== */
static void ShowToast(const std::wstring& t) {
    g_toastText = t;
    g_toastShow = true;
    SetTimer(g_hwnd, 5, 2500, NULL);
    InvalidateAll();
}
static void SetPathInput(const std::wstring& p) {
    g_pathInput = TrimBs(p);
    g_inputCaret = (int)g_pathInput.size();
    g_inputSelA = -1;
    g_inputScroll = 0;
}
static void RefreshSelVisual() {
    RefreshSelStatus();
    InvalidateAll();
}
static void ToggleSelect(N& n) {
    if (g_sel.count(n.path)) {
        g_sel.erase(n.path);
        std::vector<std::wstring> keep;
        for (auto& p : g_selOrder) if (p != n.path) keep.push_back(p);
        g_selOrder = keep;
    } else {
        g_sel.insert(n.path);
        g_selOrder.push_back(n.path);
    }
    g_selAnchor = -1;
    RefreshSelVisual();
}
static void SingleSelect(N& n) {
    g_sel.clear(); g_selOrder.clear();
    g_sel.insert(n.path);
    g_selOrder.push_back(n.path);
    g_selAnchor = -1;
    SetPathInput(n.path);
    RefreshSelVisual();
}
static void RangeSelect(N& n) {
    if (!g_items) return;
    int cur = -1;
    for (int i = 0; i < (int)g_items->size(); i++) if ((*g_items)[i].path == n.path) { cur = i; break; }
    if (cur < 0) return;
    if (g_selAnchor < 0) g_selAnchor = cur;
    int a = std::min(g_selAnchor, cur), b = std::max(g_selAnchor, cur);
    g_sel.clear(); g_selOrder.clear();
    for (int i = a; i <= b && i < (int)g_items->size(); i++) {
        g_sel.insert((*g_items)[i].path);
        g_selOrder.push_back((*g_items)[i].path);
    }
    RefreshSelVisual();
}
/* 展开/收起 (源样式 toggleExpand): 有子项直接展开 (内部子层收起, 视图停在当前层); 无则查询后展开 */
static void ToggleExpand(N& n) {
    if (n.expanded) {
        n.expanded = false;
        RebuildTiles();
        RefreshSelVisual();
        InvalidateAll();
        return;
    }
    if (!n.children.empty()) {
        n.expanded = true;
        CollapseDescendants(n);
    } else {
        SetStatus(L"正在统计 " + n.name + L" …");
        n.children = EngLoadDir(n.path, NULL);
        if (!n.children.empty()) {
            n.expanded = true;
            CollapseDescendants(n);
        }
    }
    RebuildTiles();
    RefreshSelVisual();
    InvalidateAll();
}
/* 菜单构建 */
static void OpenTileMenu(const std::wstring& path, bool isDir, bool isDrive, POINT pt) {
    g_menu.clear();
    g_menuNodePath = path;
    g_menuNodeIsDir = isDir || isDrive;
    g_menuKind = 1;
    g_menu.push_back({ isDrive ? L"进入此磁盘" : L"进入此目录", L"", isDrive ? MI_DRIVE : MI_DIR, false, false, false, 1 });
    g_menu.push_back({ (isDir || isDrive) ? L"打开所在位置" : L"打开", L"", MI_OPEN, false, false, false, 2 });
    g_menu.push_back({ L"在资源管理器中定位", L"", MI_REVEAL, false, false, false, 3 });
    g_menu.push_back({ L"", L"", 0, false, false, true, 0 });
    g_menu.push_back({ L"复制路径", L"", MI_COPYPATH, false, false, false, 4 });
    g_menu.push_back({ L"复制名称", L"", MI_COPYNAME, false, false, false, 5 });
    g_menu.push_back({ L"", L"", 0, false, false, true, 0 });
    g_menu.push_back({ L"刷新当前视图", L"", MI_REFRESH, false, false, false, 6 });
    /* 尺寸: 宽 = 最长项 + 图标, 高 = 项累计 */
    Graphics gi(g_hwnd);
    float mw = S(120);
    float mh = S(10);
    for (auto& m : g_menu) {
        if (m.sep) { mh += S(9); continue; }
        float tw = S(31) + MeasureStr(gi, m.text, F(S(11.8f), false)) + S(20);
        mw = std::max(mw, tw);
        mh += S(32);
    }
    mw = std::min(mw + S(10), S(300));
    RECT crc; GetClientRect(g_hwnd, &crc);
    POINT c = pt; ScreenToClient(g_hwnd, &c);
    float mx = std::min((float)c.x, (float)crc.right - mw - S(8));
    float my = std::min(std::max((float)c.y, S(4)), (float)crc.bottom - mh - S(8));   /* 跟随鼠标 */
    g_menuRc = RectF(mx, my, mw, mh);
    TipStateReset();   /* 菜单压过悬停提示: 不清则 Tooltip 绘制在菜单之后盖住菜单 (2026-09-19 实锤) */
    g_menuOpen = true;
    g_menuHover = -1;
    SetCapture(g_hwnd);
    InvalidateAll();
}
static void OpenDriveMenu() {
    g_menu.clear();
    g_menuKind = 2;
    for (auto& d : EnumDrives()) {
        MItem m;
        m.text = d.name;
        m.sub = L"已用 " + FmtSize(d.used) + (d.total ? (L" · 总 " + FmtSize(d.total)) : L"");
        m.icon = MI_DRIVE;
        m.current = (!g_pathStack.empty() && TrimBs(g_pathStack[0]) == d.name);
        m.disabled = m.current;
        m.act = 0;
        m.sep = false;
        g_menu.push_back(m);
    }
    if (g_menu.empty()) g_menu.push_back({ L"未检测到可用磁盘", L"", 0, true, false, false, 0 });
    Graphics gi(g_hwnd);
    float mw = S(150), mh = S(10);
    for (auto& m : g_menu) {
        float tw = S(31) + MeasureStr(gi, m.text, F(S(11.8f), false)) + S(10) + MeasureStr(gi, m.sub, F(S(9.9f), false)) + S(30);
        mw = std::max(mw, tw);
        mh += m.sep ? S(9) : S(32);
    }
    mw = std::min(mw + S(10), S(380));
    RECT crc; GetClientRect(g_hwnd, &crc);
    /* 主程序下拉口径: 菜单左上角贴触发按钮左下角 = 按钮正下方弹出 (同 OpenSortMenu) */
    float mx = g_lo.driveBtn.X;
    float my = g_lo.driveBtn.Y + g_lo.driveBtn.Height + S(2);
    mx = std::min(std::max(mx, S(4)), (float)crc.right - mw - S(8));
    my = std::min(std::max(my, S(4)), (float)crc.bottom - mh - S(8));
    g_menuRc = RectF(mx, my, mw, mh);
    TipStateReset();   /* 菜单压过悬停提示: 不清则 Tooltip 绘制在菜单之后盖住菜单 (2026-09-19 实锤) */
    g_menuOpen = true;
    g_menuHover = -1;
    SetCapture(g_hwnd);
    InvalidateAll();
}
static void OpenSortMenu() {
    g_menu.clear();
    g_menuKind = 3;
    static const wchar_t* NAMES[4] = { L"按大小", L"按子项数", L"按子文件数", L"按子文件夹数" };
    for (int i = 0; i < 4; i++)
        g_menu.push_back({ NAMES[i], L"", 0, false, i == g_sortBy, false, i + 1 });
    Graphics gi(g_hwnd);
    float mw = S(150), mh = S(10);
    for (auto& m : g_menu) {
        float tw = S(31) + MeasureStr(gi, m.text, F(S(11.8f), false)) + S(20);
        mw = std::max(mw, tw);
        mh += S(32);
    }
    mw = std::min(mw + S(10), S(260));
    RECT crc; GetClientRect(g_hwnd, &crc);
    /* 主程序下拉口径 (xjs_popup XjsShowPopupMenu 锚点同款): 菜单左上角贴触发按钮的
       左下角 = 按钮正下方弹出、左对齐; 越界按窗口钳回 (右缘/底缘留 8px) */
    float mx = g_lo.sortBtn.X;
    float my = g_lo.sortBtn.Y + g_lo.sortBtn.Height + S(2);
    mx = std::min(std::max(mx, S(4)), (float)crc.right - mw - S(8));
    my = std::min(std::max(my, S(4)), (float)crc.bottom - mh - S(8));
    g_menuRc = RectF(mx, my, mw, mh);
    TipStateReset();   /* 菜单压过悬停提示: 不清则 Tooltip 绘制在菜单之后盖住菜单 (2026-09-19 实锤) */
    g_menuOpen = true;
    g_menuHover = -1;
    SetCapture(g_hwnd);
    InvalidateAll();
}
static void CloseMenu() {
    if (!g_menuOpen) return;
    g_menuOpen = false;
    g_menu.clear();
    g_menuHover = -1;
    if (GetCapture() == g_hwnd) ReleaseCapture();
    InvalidateAll();
}
static void RunMenuAction(int act) {
    if (g_menuKind == 2) {   /* 换盘: act 无用, 用 g_menuHover */
        if (g_menuHover < 0 || g_menuHover >= (int)g_menu.size()) return;
        std::wstring name = g_menu[g_menuHover].text;
        CloseMenu();
        if (name.size() < 2) return;
        SaveExpandedSnapshot();
        g_forwardStack.clear();
        g_pathStack.assign(1, NormDir(name + L"\\"));
        SetStatus(L"");
        TransitionTo(g_pathStack[0]);
        return;
    }
    if (g_menuKind == 3) {
        int sel = act - 1;
        CloseMenu();
        if (sel < 0 || sel > 3 || sel == g_sortBy) return;
        g_sortBy = sel;
        StorageSet("排序", std::to_wstring(sel));
        if (g_items) {
            ApplySortResort(*g_items);
            RebuildTiles();
            RefreshSelVisual();
            static const wchar_t* MSG[4] = { L"", L"已按子项数排序", L"已按子文件数排序", L"已按子文件夹数排序" };
            SetStatus(sel ? MSG[sel] : L"已按大小排序");
        }
        InvalidateAll();
        return;
    }
    /* 瓦片右键 */
    N* node = g_items ? FindNode(*g_items, g_menuNodePath) : NULL;
    std::wstring path = g_menuNodePath;
    CloseMenu();
    switch (act) {
        case 1:   /* 进入 */
            if (IsDrivePath(path)) {
                SaveExpandedSnapshot();
                g_forwardStack.clear();
                g_pathStack.assign(1, NormDir(path));
                TransitionTo(g_pathStack[0]);
            } else GoInto(path);
            break;
        case 2:   /* 打开 / 打开所在位置 (宿主 OpenFile 按 ID; 节点无 ID = 不在索引, 跳过) */
            if (g_host && node && node->cid >= 0)
                g_host->OpenFile(g_ctx, g_winToken, node->cid, (g_menuNodeIsDir || IsDrivePath(path)) ? 1 : 0);
            break;
        case 3:   /* 定位 */
            if (g_host && node && node->cid >= 0)
                g_host->OpenFile(g_ctx, g_winToken, node->cid, 1);
            break;
        case 4:   /* 复制路径 */
        case 5: { /* 复制名称 */
            if (g_host) {
                std::wstring txt = (act == 4) ? path : PathName(path);
                g_host->ClipboardSetText(g_ctx, U8(txt).c_str());
                SetStatus(act == 4 ? L"已复制路径" : L"已复制名称");
                InvalidateAll();
            }
            break;
        }
        case 6: { /* 刷新 */
            /* erase 释放槽内 N: g_items 与 g_tiles[].n 全部悬空, 必须立即失效 —
               加载完成 (30ms 定时器) 前的 WM_PAINT (DrawTiles 无条件遍历) / 鼠标命中
               都会解引用旧指针 = UAF; 空瓦片帧被 DrawStage 的加载遮罩盖住, 视觉无感 */
            g_cache.erase(g_viewRoot);
            g_items = NULL;
            g_tiles.clear();
            TransitionTo(g_viewRoot);
            break;
        }
    }
    (void)node;
}

/* ==================== 路径输入框编辑 ==================== */
static void InputEnsureCaretVisible() { InvalidateAll(); }
static void InputInsertText(const std::wstring& t) {
    int a = g_inputSelA >= 0 ? g_inputSelA : g_inputCaret;
    int b = g_inputCaret;
    if (a > b) std::swap(a, b);
    if (a < 0) a = 0;
    if (b > (int)g_pathInput.size()) b = (int)g_pathInput.size();
    g_pathInput = g_pathInput.substr(0, a) + t + g_pathInput.substr(b);
    g_inputCaret = a + (int)t.size();
    g_inputSelA = -1;
    InputEnsureCaretVisible();
}
static void InputKey(WPARAM vk, bool ctrl, bool shift) {
    int n = (int)g_pathInput.size();
    if (ctrl && vk == 'A') { g_inputSelA = 0; g_inputCaret = n; InvalidateAll(); return; }
    if (ctrl && vk == 'C' && g_inputSelA >= 0 && g_inputSelA != g_inputCaret) {
        int a = g_inputSelA, b = g_inputCaret;
        if (a > b) std::swap(a, b);
        if (g_host && OpenClipboard(g_hwnd)) {
            EmptyClipboard();
            std::wstring t = g_pathInput.substr(a, b - a);
            size_t bytes = (t.size() + 1) * sizeof(wchar_t);
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (h) { memcpy(GlobalLock(h), t.c_str(), bytes); GlobalUnlock(h); SetClipboardData(CF_UNICODETEXT, h); }
            CloseClipboard();
        }
        return;
    }
    if (ctrl && vk == 'X' && g_inputSelA >= 0 && g_inputSelA != g_inputCaret) {
        int a = g_inputSelA, b = g_inputCaret;
        if (a > b) std::swap(a, b);
        std::wstring t = g_pathInput.substr(a, b - a);
        if (g_host && OpenClipboard(g_hwnd)) {
            EmptyClipboard();
            size_t bytes = (t.size() + 1) * sizeof(wchar_t);
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (h) { memcpy(GlobalLock(h), t.c_str(), bytes); GlobalUnlock(h); SetClipboardData(CF_UNICODETEXT, h); }
            CloseClipboard();
        }
        g_pathInput = g_pathInput.substr(0, a) + g_pathInput.substr(b);
        g_inputCaret = a; g_inputSelA = -1;
        InvalidateAll();
        return;
    }
    if (ctrl && vk == 'V') {
        if (OpenClipboard(g_hwnd)) {
            HANDLE h = GetClipboardData(CF_UNICODETEXT);
            if (h) if (const wchar_t* w = (const wchar_t*)GlobalLock(h)) {
                std::wstring t = w;
                for (auto& c : t) if (c == L'\r' || c == L'\n') c = L' ';
                GlobalUnlock(h);
                InputInsertText(t);
            }
            CloseClipboard();
        }
        return;
    }
    if (vk == VK_LEFT) {
        int base = (g_inputSelA >= 0 && !shift) ? std::min(g_inputSelA, g_inputCaret) : g_inputCaret;
        int step = ctrl ? 1 : 1;
        int np = std::max(0, base - step);
        if (ctrl) { while (np > 0 && g_pathInput[np - 1] != L'\\' && g_pathInput[np - 1] != L' ' && g_pathInput[np - 1] != L':') np--; }
        g_inputCaret = np;
        if (!shift) g_inputSelA = -1;
        else if (g_inputSelA < 0) g_inputSelA = base;
        InvalidateAll();
        return;
    }
    if (vk == VK_RIGHT) {
        int base = (g_inputSelA >= 0 && !shift) ? std::max(g_inputSelA, g_inputCaret) : g_inputCaret;
        int np = std::min(n, base + 1);
        if (ctrl) { while (np < n && g_pathInput[np] != L'\\' && g_pathInput[np] != L' ' && g_pathInput[np] != L':') np++; }
        g_inputCaret = np;
        if (!shift) g_inputSelA = -1;
        else if (g_inputSelA < 0) g_inputSelA = base;
        InvalidateAll();
        return;
    }
    if (vk == VK_HOME) { g_inputCaret = 0; if (!shift) g_inputSelA = -1; else if (g_inputSelA < 0) g_inputSelA = n; InvalidateAll(); return; }
    if (vk == VK_END) { g_inputCaret = n; if (!shift) g_inputSelA = -1; else if (g_inputSelA < 0) g_inputSelA = n; InvalidateAll(); return; }
    if (vk == VK_BACK) {
        if (g_inputSelA >= 0 && g_inputSelA != g_inputCaret) {
            int a = g_inputSelA, b = g_inputCaret;
            if (a > b) std::swap(a, b);
            g_pathInput = g_pathInput.substr(0, a) + g_pathInput.substr(b);
            g_inputCaret = a; g_inputSelA = -1;
        } else if (g_inputCaret > 0) {
            g_pathInput.erase(g_inputCaret - 1, 1);
            g_inputCaret--;
        }
        InvalidateAll();
        return;
    }
    if (vk == VK_DELETE) {
        if (g_inputSelA >= 0 && g_inputSelA != g_inputCaret) {
            int a = g_inputSelA, b = g_inputCaret;
            if (a > b) std::swap(a, b);
            g_pathInput = g_pathInput.substr(0, a) + g_pathInput.substr(b);
            g_inputCaret = a; g_inputSelA = -1;
        } else if (g_inputCaret < n) {
            g_pathInput.erase(g_inputCaret, 1);
        }
        InvalidateAll();
        return;
    }
}
/* 回车: 盘符 → 换盘; 完整路径 → 进入 (源样式 pathInput keydown) */
static void InputSubmit() {
    std::wstring v = TrimBs(g_pathInput);
    if (v.empty()) return;
    g_focusInput = false;
    if (IsDrivePath(v)) {
        SaveExpandedSnapshot();
        g_forwardStack.clear();
        g_pathStack.assign(1, NormDir(v));
        SetStatus(L"");
        TransitionTo(g_pathStack[0]);
        InvalidateAll();
        return;
    }
    if (v.size() >= 3 && v[1] == L':') {
        std::wstring drive = NormDir(v.substr(0, 2));
        std::wstring target = NormDir(v);
        SaveExpandedSnapshot();
        g_forwardStack.clear();
        g_pathStack.assign(1, drive);
        g_pathStack.push_back(target);
        TransitionTo(target);
        InvalidateAll();
        return;
    }
    SetStatus(L"请输入有效的路径 (如 D:\\xxx 或 D:)");
    InvalidateAll();
}
/* 点位 → 光标下标 (输入框命中时) */
static int InputCaretAt(Gdiplus::Graphics& g, float localX) {
    float prev = 0;
    for (int i = 0; i <= (int)g_pathInput.size(); i++) {
        float cx = InputXOf(g, i);
        if (localX <= (prev + cx) / 2) return i;
        prev = cx;
    }
    return (int)g_pathInput.size();
}

/* ==================== 评估上报 (WinHTTP, 源样式 Evaluation 接口同款) ==================== */
static std::atomic<bool> g_evalBusy{ false };
static std::thread g_evalTh;   /* 工作线程句柄 (Shutdown 里 join — SDK 契约: 不裸 detach 活到进程退出) */
static const wchar_t EVAL_HOST[32] = L"www.xunjieso.com";
static const wchar_t EVAL_HOST2[40] = L"ditu.snailquicksearch.com";
static std::map<std::wstring, std::pair<long long, std::string>> g_evalCache;   /* 路径 -> {时间, 原始JSON} */
static const long long EVAL_ERR_TTL = 5 * 60 * 1000;

static void ApplyEvalData(const std::string& json) {
    EvalData d;
    d.ok = true;
    d.err = JsonFieldW(json, "错误");
    d.name = JsonFieldW(json, "名称");
    d.cat = JsonFieldW(json, "类别");
    d.product = JsonFieldW(json, "产品名称");
    d.vendor = JsonFieldW(json, "厂商");
    d.desc = JsonFieldW(json, "描述");
    d.site = JsonFieldW(json, "官网");
    d.note = JsonFieldW(json, "备注");
    d.action = JsonFieldW(json, "推荐操作");
    d.reliability = JsonFieldW(json, "可靠性");
    if (!d.reliability.empty() && d.reliability.find(L'%') == std::wstring::npos) d.reliability += L"%";
    d.score = (int)JsonFieldN(json, "可删除评分");
    /* 删除后重建: bool */
    {
        std::string k = "\"删除后重建\":";
        size_t p = json.find(k);
        if (p != std::string::npos) {
            p += k.size();
            while (p < json.size() && json[p] == ' ') p++;
            if (json.compare(p, 4, "true") == 0) d.rebuild = 1;
            else if (json.compare(p, 5, "false") == 0) d.rebuild = 0;
        }
    }
    g_evalData = d;
    g_sbPage = d.err.empty() ? SB_RESULT : SB_ERROR;
    InvalidateAll();
}
static std::string HttpPost(const wchar_t* host, int port, const std::string& bodyA, bool* netOk) {
    *netOk = false;
    HINTERNET ses = WinHttpOpen(L"XjsSpaceMap/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return "";
    WinHttpSetTimeouts(ses, 10000, 10000, 10000, 10000);
    HINTERNET con = WinHttpConnect(ses, host, (INTERNET_PORT)port, 0);
    std::string out;
    if (con) {
        HINTERNET req = WinHttpOpenRequest(con, L"POST", L"/Evaluation", NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if (req) {
            const wchar_t* hdrs = L"Content-Type: application/json\r\n";
            if (WinHttpSendRequest(req, hdrs, (DWORD)-1, (LPVOID)bodyA.data(), (DWORD)bodyA.size(), (DWORD)bodyA.size(), 0) &&
                WinHttpReceiveResponse(req, NULL)) {
                DWORD st = 0, sz = sizeof(st);
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &st, &sz, WINHTTP_NO_HEADER_INDEX);
                if (st >= 200 && st < 300) {
                    char buf[4096];
                    DWORD rd = 0;
                    while (WinHttpReadData(req, buf, sizeof(buf), &rd) && rd) out.append(buf, rd);
                    *netOk = true;
                }
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(con);
    }
    WinHttpCloseHandle(ses);
    return out;
}
/* 服务端 GBK/UTF-8 双编码: UTF-8 严格解失败回退 GBK (源样式 decodeEvalJson) */
static void EvalThread(std::wstring path, bool isDir, std::string bodyUtf8) {
    bool netOk = false;
    std::string json = HttpPost(EVAL_HOST, 8096, bodyUtf8, &netOk);
    if (!netOk) json = HttpPost(EVAL_HOST2, 8096, bodyUtf8, &netOk);
    /* 回包投递到 UI 线程 */
    struct EvalMsg { std::wstring path; std::string json; bool ok; };
    EvalMsg* m = new EvalMsg{ path, json, netOk };
    /* 窗口已关 (g_hwnd=NULL) 或投递失败 = 消息无人消费, 就地释放 (否则 EvalMsg 泄漏) */
    if (!g_hwnd || !PostMessageW(g_hwnd, WM_APP + 4, 0, (LPARAM)m)) delete m;
    g_evalBusy = false;
}
static void ReportEvalClick(const std::wstring& path, bool isDir) {
    if (path.empty()) return;
    if (g_sbPage != SB_GUIDE && g_sbPage != SB_RESULT && g_sbPage != SB_FAIL) return;
    if (g_evalBusy.load()) return;
    auto it = g_evalCache.find(path);
    if (it != g_evalCache.end()) {
        bool isErr = JsonFieldW(it->second.second, "错误").empty() == false;
        if (!isErr || (GetTickCount64() - (unsigned long long)it->second.first) < (unsigned long long)EVAL_ERR_TTL) {
            g_evalPath = path;
            ApplyEvalData(it->second.second);
            return;
        }
        g_evalCache.erase(it);
    }
    g_evalPath = path;
    g_sbPage = SB_LOADING;
    SetTimer(g_hwnd, 3, 33, NULL);   /* 评估加载动画 */
    InvalidateAll();
    g_evalBusy = true;
    std::string body = "{\"路径\":\"" + U8(path) + "\",\"文件夹\":" + (isDir ? "true" : "false") +
                       ",\"操作系统\":\"windows\",\"语言\":\"zh\"}";
    /* 路径 JSON 转义 (反斜杠) */
    std::string esc;
    for (char c : body) { if (c == '\\') esc += "\\\\"; else esc += c; }
    if (g_evalTh.joinable()) g_evalTh.join();   /* 上一发必然已结束 (g_evalBusy 闸): 收掉句柄 */
    g_evalTh = std::thread(EvalThread, path, isDir, esc);
}

/* ==================== OLE 拖出 (CF_HDROP, 源样式按住拖出) ==================== */
class DropSource : public IDropSource {
    LONG ref = 1;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropSource) { *ppv = this; AddRef(); return S_OK; }
        *ppv = NULL; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref; }
    ULONG STDMETHODCALLTYPE Release() override { LONG r = --ref; if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL esc, DWORD state) override {
        if (esc) return DRAGDROP_S_CANCEL;
        if (!(state & MK_LBUTTON)) return DRAGDROP_S_DROP;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};
static void StartDragOut(const std::wstring& pressedPath) {
    std::vector<std::wstring> paths;
    if (g_sel.count(pressedPath)) {
        for (auto& p : g_selOrder) if (g_sel.count(p)) paths.push_back(p);
    } else paths.push_back(pressedPath);
    if (paths.empty()) return;
    /* CF_HDROP 数据对象: SHCreateDataObject 按路径建 pidl 列表 */
    std::vector<PIDLIST_ABSOLUTE> pids;
    for (auto& p : paths) {
        PIDLIST_ABSOLUTE pi = ILCreateFromPathW(p.c_str());
        if (pi) pids.push_back(pi);
    }
    IDataObject* dto = NULL;
    if (!pids.empty())
        SHCreateDataObject(NULL, (UINT)pids.size(), (PCUITEMID_CHILD_ARRAY)pids.data(), NULL, IID_PPV_ARGS(&dto));
    for (auto& pi : pids) ILFree(pi);
    if (!dto) return;
    IDropSource* src = new DropSource();
    DWORD eff = 0;
    DoDragDrop(dto, src, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &eff);
    dto->Release();
    src->Release();
}

/* ==================== 窗口过程 ==================== */
static int HitTile(POINT pt) {
    int hit = -1;
    for (int i = 0; i < (int)g_tiles.size(); i++) {
        const Tile& t = g_tiles[i];
        if (pt.x >= t.rc.x && pt.x < t.rc.x + t.rc.w && pt.y >= t.rc.y && pt.y < t.rc.y + t.rc.h) hit = i;
    }
    return hit;
}
static int HitUi(POINT pt) {
    auto in = [&](const RectF& r) {
        return pt.x >= r.X && pt.x < r.X + r.Width && pt.y >= r.Y && pt.y < r.Y + r.Height && r.Width > 0;
    };
    if (in(g_lo.btnPin)) return HOT_PIN;
    if (in(g_lo.btnMin)) return HOT_MIN;
    if (in(g_lo.btnMax)) return HOT_MAX;
    if (in(g_lo.btnClose)) return HOT_CLOSE;
    if (in(g_lo.driveBtn)) return HOT_DRIVE;
    if (in(g_lo.sortBtn)) return HOT_SORT;
    if (in(g_lo.pathInput)) return HOT_INPUT;
    if (in(g_lo.sbToggle)) return HOT_SBTOGGLE;
    if (in(g_lo.sbClose)) return HOT_SBCLOSE;
    if (in(g_lo.resizer) && g_lo.resizer.Width > 0) return HOT_SBRESIZER;
    if (!g_sbCollapsed && pt.x >= g_lo.sbBody.X && pt.x < g_lo.sbBody.X + g_lo.sbBody.Width &&
        pt.y >= g_lo.sbBody.Y && pt.y < g_lo.sbBody.Y + g_lo.sbBody.Height) {
        if (in(g_hmRc1)) return HOT_AGREE;
        if (in(g_hmRc2)) return HOT_DECLINE;
        if (in(g_siteRc)) return HOT_SITE;
    }
    if (in(g_backBtnRc)) return HOT_BACKBTN;
    return HOT_NONE;
}
static N* TileNodeAt(POINT pt) {
    int i = HitTile(pt);
    return i >= 0 ? g_tiles[i].n : NULL;
}

static LRESULT CALLBACK MapWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static POINT s_downPt; static int s_downTile = -1; static bool s_dragCand = false;
    static bool s_titleClick = false; static std::wstring s_titlePath;
    static bool s_pressedUi = 0; static int s_pressedHot = 0;
    switch (msg) {
        case WM_CREATE: {
            SetTimer(hwnd, 6, 530, NULL);   /* 输入框光标闪烁 */
            PostMessageW(hwnd, WM_APP + 2, 0, 0);   /* 启动装载 (遮罩先画) */
            return 0;
        }
        case WM_SIZE: {
            ComputeLayout();
            RebuildTiles();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            DefWindowProcW(hwnd, msg, wp, lp);
            MINMAXINFO* mmi = (MINMAXINFO*)lp;
            UINT dpi = GetDpiForWindow(hwnd);
            float sc = dpi / 96.0f;
            mmi->ptMinTrackSize.x = (LONG)(820 * sc);
            mmi->ptMinTrackSize.y = (LONG)(560 * sc);
            /* WS_POPUP 最大化默认铺满整屏盖住任务栏: 钳到最近显示器的工作区 (同宿主主窗) */
            MONITORINFO mi = { sizeof(mi) };
            if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
                mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
                mmi->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
                mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
                mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
            }
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            DrawFrame(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DPICHANGED: {
            g_dpi = HIWORD(wp) / 96.0f;
            ClearFonts();
            ComputeLayout();
            RebuildTiles();
            RECT* sug = (RECT*)lp;
            SetWindowPos(hwnd, NULL, sug->left, sug->top, sug->right - sug->left, sug->bottom - sug->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case WM_TIMER: {
            if (wp == 7) {   /* 视图装载 (含引擎忙轮询) */
                KillTimer(hwnd, 7);
                if (!g_waitView.empty()) { std::wstring v = g_waitView; DoLoadView(v); }
                return 0;
            }
            if (wp == 3) {   /* 加载/评估旋转动画 (遮罩态全帧低频刷新, 可接受) */
                g_spinTick++;
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (wp == 5) { KillTimer(hwnd, 5); g_toastShow = false; InvalidateRect(hwnd, NULL, FALSE); return 0; }
            if (wp == 6) { g_caretOn = !g_caretOn; if (g_focusInput) { RECT r = { (LONG)g_lo.pathInput.X, (LONG)g_lo.pathInput.Y,
                             (LONG)(g_lo.pathInput.X + g_lo.pathInput.Width), (LONG)(g_lo.pathInput.Y + g_lo.pathInput.Height) };
                             InvalidateRect(hwnd, &r, FALSE); } return 0; }
            if (wp == 4) {   /* 标题条单击收起 (双击放大优先, 源样式 240ms 延迟) */
                KillTimer(hwnd, 4);
                if (g_items) if (N* n = FindNode(*g_items, s_titlePath)) {
                    n->expanded = false;
                    RebuildTiles();
                    RefreshSelVisual();
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                return 0;
            }
            if (wp == 1) {   /* Tooltip 延迟显示 */
                KillTimer(hwnd, 1);
                if (s_tipWait && g_tipNode) { g_tipVisible = true; SetTimer(hwnd, 2, 3000, NULL); InvalidateRect(hwnd, NULL, FALSE); }
                return 0;
            }
            if (wp == 2) { KillTimer(hwnd, 2); g_tipVisible = false; s_tipWait = false; InvalidateRect(hwnd, NULL, FALSE); return 0; }
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (g_menuOpen) {   /* 菜单悬停 */
                int h = -1;
                if (pt.x >= g_menuRc.X && pt.x < g_menuRc.X + g_menuRc.Width && pt.y >= g_menuRc.Y && pt.y < g_menuRc.Y + g_menuRc.Height) {
                    float y = g_menuRc.Y + S(5);
                    for (int i = 0; i < (int)g_menu.size(); i++) {
                        if (g_menu[i].sep) { y += S(9); continue; }
                        if (pt.y >= y && pt.y < y + S(32) && !g_menu[i].disabled) { h = i; break; }
                        y += S(32);
                    }
                }
                if (h != g_menuHover) { g_menuHover = h; InvalidateRect(hwnd, NULL, FALSE); }
                return 0;
            }
            if (s_dragCand && s_downTile >= 0) {   /* 拖出手势 (阈值 6px, 源样式 dragGesture) */
                int dx = pt.x - s_downPt.x, dy = pt.y - s_downPt.y;
                if (dx * dx + dy * dy >= 36) {
                    s_dragCand = false;
                    N* n = TileNodeAt(s_downPt);
                    if (n && !n->isDrive && g_items) {
                        N* cur = FindNode(*g_items, n->path);   /* 当前树节点 (拖出期间树不变) */
                        std::wstring p = n->path;
                        ReleaseCapture();
                        StartDragOut(p);
                        s_suppressClick = true;
                        (void)cur;
                    }
                }
                return 0;
            }
            if (g_sbDrag) {   /* 侧边栏宽度拖拽 */
                float w = (float)(g_lo.body.X + g_lo.body.Width - pt.x);
                w = std::max(S(180), std::min(g_lo.body.Width * 0.55f, w));
                g_sbWidth = w / g_dpi;
                ComputeLayout();
                RebuildTiles();
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (g_marqueeOn) {
                g_marqueeCur = pt;
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            /* 悬停: 瓦片 / UI 控件 */
            int tile = HitTile(pt);
            int ui = HitUi(pt);
            int newHot = 0;
            if (tile >= 0 && ui == HOT_NONE) newHot = HOT_NONE, ui = 0;
            if (ui != HOT_NONE) newHot = ui;
            if (newHot != g_hotUi) { g_hotUi = newHot; InvalidateRect(hwnd, NULL, FALSE); }
            if (tile != g_hover) {
                g_hover = tile;
                g_tipVisible = false;
                s_tipWait = false;
                KillTimer(hwnd, 1);
                KillTimer(hwnd, 2);
                g_tipNode = NULL;
                if (tile >= 0) {
                    g_tipNode = g_tiles[tile].n;
                    g_tipTotal = g_tiles[tile].total;
                    g_tipPos = pt;
                    s_tipWait = true;
                    SetTimer(hwnd, 1, 450, NULL);   /* 源样式: 静止 450ms 才显示 */
                }
                InvalidateRect(hwnd, NULL, FALSE);
            } else if (g_tipVisible || s_tipWait) {
                int dx = pt.x - g_tipPos.x, dy = pt.y - g_tipPos.y;
                if (dx * dx + dy * dy >= 16) {   /* 移动 ≥4px: 隐藏并重新延迟 */
                    g_tipVisible = false;
                    s_tipWait = true;
                    g_tipPos = pt;
                    KillTimer(hwnd, 2);
                    SetTimer(hwnd, 1, 450, NULL);
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            /* 输入框光标形状 */
            if (ui == HOT_INPUT || (g_focusInput && ui == HOT_NONE && pt.y < g_lo.navbar.Y + g_lo.navbar.Height)) {
                SetCursor(LoadCursorW(NULL, IDC_IBEAM));
            } else if (ui == HOT_SBRESIZER) {
                SetCursor(LoadCursorW(NULL, IDC_SIZEWE));
            } else {
                SetCursor(LoadCursorW(NULL, IDC_ARROW));
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_hover = -1;
            g_hotUi = 0;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            SetFocus(hwnd);
            if (g_menuOpen) {   /* 点外按下即关 (含点在菜单外) */
                bool inMenu = pt.x >= g_menuRc.X && pt.x < g_menuRc.X + g_menuRc.Width && pt.y >= g_menuRc.Y && pt.y < g_menuRc.Y + g_menuRc.Height;
                if (!inMenu) { CloseMenu(); return 0; }
                return 0;
            }
            /* 侧边栏滚轮/拖拽条 */
            if (!g_sbCollapsed && pt.x >= g_lo.resizer.X - S(2) && pt.x < g_lo.resizer.X + g_lo.resizer.Width + S(2) &&
                pt.y >= g_lo.resizer.Y && pt.y < g_lo.resizer.Y + g_lo.resizer.Height) {
                g_sbDrag = true;
                SetCapture(g_hwnd);
                return 0;
            }
            /* 隐私页按钮 / 官网链接 */
            int ui = HitUi(pt);
            if (ui == HOT_AGREE || ui == HOT_DECLINE || ui == HOT_SITE) {
                s_pressedUi = true; s_pressedHot = ui; s_downPt = pt;
                SetCapture(g_hwnd);
                return 0;
            }
            /* 侧边栏内其它点击: 收起钮 */
            if (ui == HOT_SBCLOSE) { s_pressedUi = true; s_pressedHot = ui; s_downPt = pt; SetCapture(g_hwnd); return 0; }
            /* 导航条 */
            if (ui == HOT_DRIVE || ui == HOT_SORT || ui == HOT_SBTOGGLE) {
                s_pressedUi = true; s_pressedHot = ui; s_downPt = pt; SetCapture(g_hwnd);
                return 0;
            }
            if (ui == HOT_INPUT) {
                g_focusInput = true;
                g_caretOn = true;
                Graphics gi(g_hwnd);
                float localX = (float)pt.x - g_lo.pathInput.X - S(8) + g_inputScroll;
                int idx = InputCaretAt(gi, localX);
                g_inputCaret = idx;
                g_inputSelA = -1;
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (ui == HOT_PIN || ui == HOT_MIN || ui == HOT_MAX || ui == HOT_CLOSE) {
                s_pressedUi = true; s_pressedHot = ui; s_downPt = pt; SetCapture(g_hwnd);
                return 0;
            }
            /* 空态返回按钮 */
            if (ui == HOT_BACKBTN) { s_pressedUi = true; s_pressedHot = ui; s_downPt = pt; SetCapture(g_hwnd); return 0; }
            /* 地图: Shift 框选 / 拖出待定 / 点击 */
            if (pt.x >= g_lo.stage.X && pt.x < g_lo.stage.X + g_lo.stage.Width &&
                pt.y >= g_lo.stage.Y && pt.y < g_lo.stage.Y + g_lo.stage.Height && g_booted) {
                if (GetKeyState(VK_SHIFT) & 0x8000) {
                    g_marqueeOn = true;
                    g_marqueeStart = pt;
                    g_marqueeCur = pt;
                    SetCapture(g_hwnd);
                    return 0;
                }
                int t = HitTile(pt);
                s_downPt = pt;
                s_downTile = t;
                s_dragCand = (t >= 0);
                SetCapture(g_hwnd);
                return 0;
            }
            /* 侧边栏滚动点击: 略 (滚轮足够) */
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (GetCapture() == g_hwnd) ReleaseCapture();
            if (g_menuOpen) {   /* 菜单项: 松开才触发 (拖离 = 取消) */
                int idx = -1;
                if (pt.x >= g_menuRc.X && pt.x < g_menuRc.X + g_menuRc.Width && pt.y >= g_menuRc.Y && pt.y < g_menuRc.Y + g_menuRc.Height) {
                    float y = g_menuRc.Y + S(5);
                    for (int i = 0; i < (int)g_menu.size(); i++) {
                        if (g_menu[i].sep) { y += S(9); continue; }
                        if (pt.y >= y && pt.y < y + S(32) && !g_menu[i].disabled) { idx = i; break; }
                        y += S(32);
                    }
                }
                if (idx < 0) CloseMenu();
                else {
                    g_menuHover = idx;
                    int act = g_menu[idx].act;
                    if (g_menuKind == 2 || g_menuKind == 3) RunMenuAction(act);
                    else RunMenuAction(act);
                }
                return 0;
            }
            if (g_marqueeOn) {   /* 框选落地: 命中的顶层瓦片全选 */
                g_marqueeOn = false;
                RECT mq = { std::min(g_marqueeStart.x, pt.x), std::min(g_marqueeStart.y, pt.y),
                            std::max(g_marqueeStart.x, pt.x), std::max(g_marqueeStart.y, pt.y) };
                g_sel.clear(); g_selOrder.clear(); g_selAnchor = -1;
                for (auto& t : g_tiles) {
                    if (t.depth != 0) continue;
                    RECT tr = { (LONG)t.rc.x, (LONG)t.rc.y, (LONG)(t.rc.x + t.rc.w), (LONG)(t.rc.y + t.rc.h) };
                    RECT is;
                    if (IntersectRect(&is, &mq, &tr)) {
                        g_sel.insert(t.n->path);
                        g_selOrder.push_back(t.n->path);
                    }
                }
                RefreshSelVisual();
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (g_sbDrag) { g_sbDrag = false; StorageSet("侧栏宽度", std::to_wstring((int)g_sbWidth)); return 0; }
            /* 按下-松开配对 (拖离 = 取消, 命令类控件松开触发口径) */
            if (s_pressedUi) {
                bool wasPressed = s_pressedUi;
                int hot = s_pressedHot;
                s_pressedUi = false;
                if (!wasPressed) return 0;
                int uiNow = HitUi(pt);
                if (uiNow != hot) return 0;   /* 拖离取消 */
                switch (hot) {
                    case HOT_PIN: {
                        SetWindowPos(hwnd, TopmostOn() ? HWND_NOTOPMOST : HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                        InvalidateRect(hwnd, NULL, FALSE);
                        break;
                    }
                    case HOT_MIN: ShowWindow(hwnd, SW_MINIMIZE); break;
                    case HOT_MAX: ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE); break;
                    case HOT_CLOSE: DestroyWindow(hwnd); break;
                    case HOT_DRIVE: OpenDriveMenu(); break;
                    case HOT_SORT: OpenSortMenu(); break;
                    case HOT_SBTOGGLE:
                    case HOT_SBCLOSE: {
                        g_sbCollapsed = !g_sbCollapsed;
                        StorageSet("侧栏收起", g_sbCollapsed ? L"1" : L"0");
                        ComputeLayout();
                        RebuildTiles();
                        InvalidateRect(hwnd, NULL, FALSE);
                        break;
                    }
                    case HOT_AGREE:
                        StorageSet("已同意隐私条款", L"1");
                        g_sbPage = SB_GUIDE;
                        g_sbScroll = 0;
                        InvalidateRect(hwnd, NULL, FALSE);
                        break;
                    case HOT_DECLINE:
                        g_sbCollapsed = true;
                        StorageSet("侧栏收起", L"1");
                        ComputeLayout();
                        RebuildTiles();
                        InvalidateRect(hwnd, NULL, FALSE);
                        break;
                    case HOT_SITE:
                        if (!g_evalData.site.empty())
                            ShellExecuteW(hwnd, L"open", g_evalData.site.c_str(), NULL, NULL, SW_SHOWNORMAL);
                        break;
                    case HOT_BACKBTN: BackToParent(); break;
                }
                return 0;
            }
            if (s_suppressClick) { s_suppressClick = false; return 0; }   /* 拖出收尾 */
            if (!g_booted) return 0;
            /* 地图点击 (源样式 click 语义, 松开生效) */
            if (pt.x >= g_lo.stage.X && pt.x < g_lo.stage.X + g_lo.stage.Width &&
                pt.y >= g_lo.stage.Y && pt.y < g_lo.stage.Y + g_lo.stage.Height) {
                if (!g_hlPath.empty()) { g_hlPath.clear(); InvalidateRect(hwnd, NULL, FALSE); }   /* 定位高亮: 点击即清 */
                /* 放大进入的根标题条 (24px): 整条可点, 点击返回上级 (源样式 root-title click) */
                if (g_pathStack.size() > 1 && pt.y < g_lo.stage.Y + S(24)) { BackToParent(); return 0; }
                int t = HitTile(pt);
                if (t < 0) {   /* 空白: 清空选中 */
                    if (!g_selOrder.empty()) { g_sel.clear(); g_selOrder.clear(); g_selAnchor = -1; RefreshSelVisual(); }
                    return 0;
                }
                N& n = *g_tiles[t].n;
                if (GetKeyState(VK_CONTROL) & 0x8000) { ToggleSelect(n); return 0; }
                if (GetKeyState(VK_SHIFT) & 0x8000) { RangeSelect(n); return 0; }
                g_pinned = PinKey(n.path);
                SetPathInput(n.path);
                bool isDir = n.isDir || n.isDrive;
                bool canExpand = g_tiles[t].rc.w >= S(52) && g_tiles[t].rc.h >= S(34);
                if (isDir) {
                    if (canExpand) ToggleExpand(n);
                    else {
                        ShowToast(L"双击放大查看");
                        SetStatus(L"双击放大进入 " + n.name);
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                    /* 评估上报 (目录) */
                    if (g_sbPage != SB_PRIVACY) ReportEvalClick(n.path, true);
                } else {
                    SingleSelect(n);
                    if (g_sbPage != SB_PRIVACY) ReportEvalClick(n.path, false);
                }
                return 0;
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            s_dragCand = false;
            return 0;
        case WM_LBUTTONDBLCLK: {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (pt.x >= g_lo.stage.X && pt.x < g_lo.stage.X + g_lo.stage.Width &&
                pt.y >= g_lo.stage.Y && pt.y < g_lo.stage.Y + g_lo.stage.Height && g_booted) {
                int t = HitTile(pt);
                if (t < 0) return 0;
                N& n = *g_tiles[t].n;
                /* 根标题条双击/单击: 返回上级 */
                if (g_pathStack.size() > 1 && pt.y < g_lo.stage.Y + S(24)) return 0;
                if ((n.isDir || n.isDrive) && !g_tiles[t].expanded) {
                    bool canExpand = g_tiles[t].rc.w >= S(52) && g_tiles[t].rc.h >= S(34);
                    if (!canExpand) { KillTimer(hwnd, 4); GoInto(n.path); }   /* 小格子双击放大 */
                } else if ((n.isDir || n.isDrive) && g_tiles[t].expanded && pt.y < g_tiles[t].rc.y + g_tiles[t].titleH) {
                    KillTimer(hwnd, 4);
                    GoInto(n.path);   /* 标题条双击放大 */
                } else if (!n.isDir && !n.isDrive) {
                    if (g_host && n.cid >= 0) g_host->OpenFile(g_ctx, g_winToken, n.cid, 1);   /* 文件双击: 定位 */
                }
                return 0;
            }
            return 0;
        }
        case WM_XBUTTONUP: {   /* 侧键: 后退/前进 (源样式 mouseNavBack/Forward) */
            int btn = HIWORD(wp);
            if (btn == XBUTTON1) {
                if (g_pathStack.size() > 1) {
                    if (!g_loading) g_forwardStack.push_back(g_pathStack.back());
                    BackToParent();
                } else CollapseDeepest();
            } else if (btn == XBUTTON2) {
                if (!g_forwardStack.empty() && !g_loading) {
                    std::wstring target = g_forwardStack.back();
                    g_forwardStack.pop_back();
                    SaveExpandedSnapshot();
                    SetPathInput(target);
                    g_pathStack.push_back(NormDir(target));
                    TransitionTo(g_pathStack.back());
                }
            }
            return TRUE;
        }
        case WM_CONTEXTMENU: {   /* 瓦片右键菜单 */
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (pt.x == -1 && pt.y == -1) return 0;
            POINT c = pt; ScreenToClient(hwnd, &c);
            RECT sr = StageRect();
            if (c.x < sr.left || c.x >= sr.right || c.y < sr.top || c.y >= sr.bottom) return 0;
            int t = HitTile(c);
            if (t < 0) return 0;
            N& n = *g_tiles[t].n;
            OpenTileMenu(n.path, n.isDir, n.isDrive, pt);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd, &pt);
            if (!g_sbCollapsed && pt.x >= g_lo.sbBody.X && pt.x < g_lo.sbBody.X + g_lo.sbBody.Width &&
                pt.y >= g_lo.sbBody.Y && pt.y < g_lo.sbBody.Y + g_lo.sbBody.Height) {
                int delta = GET_WHEEL_DELTA_WPARAM(wp);
                g_sbScroll -= delta / 120.0f * S(48);
                if (g_sbScroll < 0) g_sbScroll = 0;
                if (g_sbScroll > g_sbMaxScroll) g_sbScroll = g_sbMaxScroll;
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            return 0;
        }
        case WM_KEYDOWN: {
            bool ctrl = GetKeyState(VK_CONTROL) & 0x8000;
            bool shift = GetKeyState(VK_SHIFT) & 0x8000;
            if (g_focusInput) {
                if (wp == VK_RETURN) { InputSubmit(); return 0; }
                if (wp == VK_ESCAPE) { g_focusInput = false; InvalidateRect(hwnd, NULL, FALSE); return 0; }
                InputKey(wp, ctrl, shift);
                return 0;
            }
            if (wp == VK_ESCAPE) { CloseMenu(); CollapseDeepest(); return 0; }
            return 0;
        }
        case WM_CHAR: {
            if (g_focusInput && wp >= 32 && wp != 127) {
                if (wp == VK_RETURN) return 0;
                std::wstring t(1, (wchar_t)wp);
                InputInsertText(t);
            }
            return 0;
        }
        case WM_IME_COMPOSITION: {
            if (g_focusInput && (lp & GCS_RESULTSTR)) {
                HIMC imc = ImmGetContext(hwnd);
                if (imc) {
                    LONG sz = ImmGetCompositionStringW(imc, GCS_RESULTSTR, NULL, 0);
                    if (sz > 0) {
                        std::vector<wchar_t> buf(sz / 2 + 1, 0);
                        ImmGetCompositionStringW(imc, GCS_RESULTSTR, buf.data(), sz);
                        InputInsertText(buf.data());
                    }
                    ImmReleaseContext(hwnd, imc);
                }
                return 0;
            }
            break;
        }
        case WM_APP + 2: {   /* 启动装载 / boot */
            if (!g_booted) {
                std::wstring target = g_pendingOpen.empty() ? L"C:\\" : g_pendingOpen;
                g_pendingOpen.clear();
                BootWith(target);
            }
            return 0;
        }
        case WM_APP + 4: {   /* 评估回包 */
            struct EvalMsg { std::wstring path; std::string json; bool ok; };
            EvalMsg* m = (EvalMsg*)lp;
            if (!m) return 0;
            if (m->ok) {
                g_evalCache[m->path] = { (long long)GetTickCount64(), m->json };
                if (g_evalPath == m->path) ApplyEvalData(m->json);
            } else if (g_evalPath == m->path) {
                g_sbPage = SB_FAIL;
            }
            KillTimer(hwnd, 3);
            delete m;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wp & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_NCCALCSIZE: {
            if (wp) {
                if (IsZoomed(hwnd)) return 0;   /* 最大化: GETMINMAXINFO 已钳到工作区, 客户区=整窗 */
                /* 非最大化保留 1px NC 边距: DWM 圆角遮罩需要它才有抗锯齿 (同宿主主窗) */
                RECT* r = (RECT*)lp;
                r->left += 1; r->top += 1; r->right -= 1; r->bottom -= 1;
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        /* NCCALCSIZE 去框后激活切换时系统会把标准标题栏盖画进客户区顶部: lParam=-1 跳过非客户区重绘 */
        case WM_NCACTIVATE:
            return DefWindowProcW(hwnd, msg, wp, -1);
        case WM_NCPAINT:
            return 0;   /* 去框窗口不重画标准框架 (DWM 圆角/阴影由系统独立绘制) */
        case WM_NCHITTEST: {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            RECT wr; GetWindowRect(hwnd, &wr);
            int b = (int)S(8);
            bool l = pt.x < wr.left + b, r = pt.x >= wr.right - b, t = pt.y < wr.top + b, bm = pt.y >= wr.bottom - b;
            if (!IsZoomed(hwnd)) {
                if (l && t) return HTTOPLEFT;
                if (r && t) return HTTOPRIGHT;
                if (l && bm) return HTBOTTOMLEFT;
                if (r && bm) return HTBOTTOMRIGHT;
                if (l) return HTLEFT;
                if (r) return HTRIGHT;
                if (t) return HTTOP;
                if (bm) return HTBOTTOM;
            }
            POINT c = pt; ScreenToClient(hwnd, &c);
            int ui = HitUi(c);
            /* 最大化按钮与其他按钮一样走 HTCLIENT 客户区点击 (宿主无 HTMAXBUTTON 口径):
               返回 HTMAXBUTTON 会让点击变成非客户区消息, 客户区的最大化/还原切换永远收不到 */
            if (ui == HOT_CLOSE || ui == HOT_MIN || ui == HOT_MAX || ui == HOT_PIN) return HTCLIENT;
            if (c.y < S(40)) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_DESTROY: {
            KillTimer(hwnd, 1); KillTimer(hwnd, 2); KillTimer(hwnd, 3);
            KillTimer(hwnd, 4); KillTimer(hwnd, 5); KillTimer(hwnd, 6); KillTimer(hwnd, 7);
            /* 浮层瞬态随窗口归零: 隐藏定时器已随窗口销毁, 残留 true 会在下次开窗第一帧原样
               画出且再无定时器清除 — Toast 永驻 (实锤: "双击放大查看" 出现后 2.5s 内关窗,
               定时器 5 到点前 WM_DESTROY 不清 g_toastShow); Tooltip 的节点指针更会随下方
               g_tiles.clear() 悬垂, 复现即崩 */
            TipStateReset();   /* Tooltip 三态 (定时器 1/2 上面已清) */
            g_toastShow = false;  g_toastText.clear();
            g_menuOpen = false;   g_menu.clear();  g_menuHover = -1;  g_menuNodePath.clear();
            g_marqueeOn = false;  g_sbDrag = false;
            g_focusInput = false; g_hlPath.clear();
            g_hwnd = NULL;
            g_booted = false;
            g_items = NULL;
            g_cacheTally.clear();
            g_cache.clear();
            g_expSnap.clear();
            g_tiles.clear();
            /* 注意: 本窗口是宿主进程内的第二个窗口, 消息循环归宿主所有 —
               绝不能 PostQuitMessage (曾导致关地图窗 = 整个蜗牛快搜退出) */
            ClearFonts();
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ==================== boot / 定位 (源样式 boot/bootWith/handleOpenTarget) ==================== */
static void BootWith(const std::wstring& target) {
    std::wstring s = TrimBs(target);
    if (s.empty()) s = L"C:";
    g_loading = true;
    InvalidateAll();
    auto doBoot = [&](const std::wstring& root, std::vector<std::wstring> stack) {
        g_pathStack = stack;
        std::wstring label = IsDrivePath(TrimBs(root)) ? (std::wstring(1, root[0]) + L" 盘") : PathName(TrimBs(root));
        g_loadText = L"正在统计 " + label + L" 占用…";
        g_lastOpenTarget = s;
        g_bootStatus = label + L"空间占用 (点击/选择硬盘,可切换盘符)";
        g_booted = true;
        TransitionTo(root);
    };
    if (IsDrivePath(s) && s.size() <= 3) {
        doBoot(NormDir(s), { NormDir(s) });
        return;
    }
    std::wstring dir = s;
    bool isFile = false;
    {
        xjs_engine* eng = xjs_GetDefaultEngine();
        if (eng && EngReady(eng)) {
            EngReadLock lk(eng);
            int id = xjs_db_GetFileIdByPath(eng, U8(s).c_str());
            if (id >= 0) isFile = !xjs_db_IsDir(eng, id);
        }
    }
    if (isFile) {   /* 文件: 取父目录作视图根 + 定位高亮 (源样式 bootWith) */
        size_t i = dir.find_last_of(L'\\');
        if (i > 0) dir = dir.substr(0, i);
        g_hlPath = s;
    }
    std::wstring root = NormDir(dir);
    std::vector<std::wstring> stack;
    if (root.size() >= 3 && root[1] == L':') { stack.push_back(root.substr(0, 3)); stack.push_back(root); }
    else stack.push_back(root);
    doBoot(root, stack);
}
/* 主页面"空间地图"再次触发: 已开窗时定位到目标 (源样式 handleOpenTarget) */
static void HandleOpenTarget(const std::wstring& p) {
    if (!p.empty() == false) return;
    std::wstring s = TrimBs(p);
    if (s.empty()) return;
    if (!g_booted) { g_pendingOpen = p; return; }
    if (g_lastOpenTarget == s) return;   /* 同一路径重复点击: 只激活窗口 */
    g_lastOpenTarget = s;
    if (IsDrivePath(s) && s.size() <= 3) { GoInto(s + L"\\"); return; }
    std::wstring dir = s;
    bool isFile = false;
    {
        xjs_engine* eng = xjs_GetDefaultEngine();
        if (eng && EngReady(eng)) {
            EngReadLock lk(eng);
            int id = xjs_db_GetFileIdByPath(eng, U8(s).c_str());
            if (id >= 0) isFile = !xjs_db_IsDir(eng, id);
        }
    }
    if (isFile) {
        size_t i = dir.find_last_of(L'\\');
        if (i > 0) dir = dir.substr(0, i);
        g_hlPath = s;
    }
    if (NormDir(dir) == g_viewRoot) { InvalidateAll(); return; }
    GoInto(dir);
}

/* ==================== 窗口创建 ==================== */
static void OpenMapWindow(const std::wstring& path, unsigned long long token) {
    ApplySkin();
    g_winToken = token;
    if (g_hwnd) {   /* 单实例: 已开 = 唤起 + 定位 (源样式 openFile 消息口径) */
        ShowWindow(g_hwnd, SW_SHOW);
        if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
        if (g_host) g_host->Summon(g_ctx, g_hwnd);
        if (!path.empty()) HandleOpenTarget(path);
        return;
    }
    /* 存储记忆 (源样式 localStorage) */
    {
        std::wstring v = StorageGet("排序");
        int sv = _wtoi(v.c_str());
        if (sv >= 0 && sv <= 3) g_sortBy = sv;
        g_sbCollapsed = StorageGet("侧栏收起") == L"1";
        std::wstring w = StorageGet("侧栏宽度");
        int wv = _wtoi(w.c_str());
        if (wv >= 180 && wv <= 640) g_sbWidth = (float)wv;
        std::wstring pr = StorageGet("已同意隐私条款");
        g_sbPage = (pr == L"1") ? SB_GUIDE : SB_PRIVACY;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = MapWndProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));
        wc.lpszClassName = L"XjsSpaceMapWnd";
        wc.hIconSm = wc.hIcon;
        if (!RegisterClassExW(&wc)) return;
        registered = true;
    }
    /* 无边框(有阴影)窗口, 同宿主主窗口径: 样式不带 WS_CAPTION — 系统没有标准标题栏可盖画
       (WS_OVERLAPPEDWINDOW 自带 WS_CAPTION, 系统标题栏会叠在自绘标题栏上方 = 双控制栏);
       WS_THICKFRAME 保留 DWM 阴影+拖边调整+Snap, WM_NCCALCSIZE 1px 内缩保圆角抗锯齿 */
    HWND hwnd = CreateWindowExW(0, L"XjsSpaceMapWnd", L"蜗牛快搜 · 空间地图",
                                WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1150, 800,
                                NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!hwnd) return;
    UINT dpi = GetDpiForWindow(hwnd);
    g_dpi = dpi / 96.0f;
    /* 尺寸按 DPI 折算并居中到主屏工作区 */
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int w = (int)(1150 * g_dpi), h = (int)(800 * g_dpi);
    SetWindowPos(hwnd, NULL, wa.left + ((wa.right - wa.left) - w) / 2, wa.top + ((wa.bottom - wa.top) - h) / 2, w, h, SWP_NOZORDER);
    /* Win11 圆角 + DWM 阴影 (1px 边距保住圆角遮罩抗锯齿) */
    {
        INT pref = 2;   /* DWMWCP_ROUND */
        DwmSetWindowAttribute(hwnd, 33, &pref, sizeof(pref));
        MARGINS m = { 0, 0, 1, 0 };
        DwmExtendFrameIntoClientArea(hwnd, &m);
    }
    if (g_host) {   /* 窗口图标 = 宿主 exe 图标 (任务栏/悬停预览) */
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1)));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1)));
    }
    g_hwnd = hwnd;
    ComputeLayout();
    ShowWindow(hwnd, SW_SHOW);
    if (g_host) g_host->Summon(g_ctx, hwnd);   /* 借前台唤起 (与宿主同实现) */
    if (!path.empty()) g_pendingOpen = path;   /* boot 消费 (WM_CREATE 已投 WM_APP+2) */
    else if (!g_booted) g_pendingOpen = L"C:\\";
}

/* ==================== 插件固定导出 ==================== */
extern "C" __declspec(dllexport) const XjsPluginInfo* XJS_PLUGIN_CALL XjsPlugin_GetInfo(void) {
    static const XjsPluginInfo info = { XJS_PLUGIN_ABI_VERSION, sizeof(XjsPluginInfo), "space-map", "1.3.0" };
    return &info;
}
extern "C" __declspec(dllexport) int XJS_PLUGIN_CALL XjsPlugin_Init(XjsPluginCtx* ctx, const XjsPluginHost* host) {
    g_ctx = ctx;
    g_host = host;
    static Gdiplus::GdiplusStartupInput gsi;
    if (Gdiplus::GdiplusStartup(&g_gdipToken, &gsi, NULL) != Gdiplus::Ok)
        return XJS_PLUGIN_ERR_FAIL;
    if (g_host) {
        g_host->Subscribe(g_ctx, XJS_PLUGIN_EVT_SKIN);   /* 皮肤变化 → 窗口配色跟随默认窗口 (OnEvent) */
        g_host->Log(g_ctx, 1, "space-map 1.3.0 initialized (GDI+ replica of c-disk-cleaner, engine direct-link)");
    }
    return XJS_PLUGIN_OK;
}
extern "C" __declspec(dllexport) void XJS_PLUGIN_CALL XjsPlugin_Shutdown(XjsPluginCtx* ctx) {
    (void)ctx;
    if (g_evalTh.joinable()) g_evalTh.join();   /* SDK 契约 "Shutdown 里 join"; HttpPost 有 10s 级超时, 有界 */
    if (g_hwnd) DestroyWindow(g_hwnd);
    if (g_gdipToken) { Gdiplus::GdiplusShutdown(g_gdipToken); g_gdipToken = 0; }
}
/* 菜单/状态栏点击: 宿主只给 (命令id, 窗口令牌, 引擎FileId数组, 类别) — 路径自己直连引擎取 (v3 口径);
   状态栏项 kind=CTX_NONE 且 fileIds=NULL (无文件上下文), 与文件菜单共用 OnCommand */
extern "C" __declspec(dllexport) void XJS_PLUGIN_CALL XjsPlugin_OnCommand(
    XjsPluginCtx* ctx, const char* cmdId, XjsWindowToken window, const int* fileIds, int count, int kind) {
    (void)ctx; (void)kind;
    if (!cmdId) return;
    if (strcmp(cmdId, "open-app") == 0) {   /* 状态栏项: 无路径参数 — 已开窗只唤起不改视图, 首次打开默认 C 盘 */
        OpenMapWindow(L"", window);
        return;
    }
    if (strcmp(cmdId, "open-dir") != 0 && strcmp(cmdId, "open-drive") != 0) return;
    if (!fileIds || count <= 0) return;
    xjs_engine* eng = xjs_GetDefaultEngine();   /* 单根语义: 取第一个文件上下文 */
    if (!eng) return;
    const char* p = xjs_db_GetPath(eng, fileIds[0]);
    if (!p || !*p) return;
    std::wstring wpath = W8(p);
    OpenMapWindow(wpath, window);
}
/* 皮肤变化 (纯信号): 重取默认窗口皮肤并整帧重绘; 换的是别的窗口时取回同色, 重绘无观感变化 */
extern "C" __declspec(dllexport) void XJS_PLUGIN_CALL XjsPlugin_OnEvent(
    XjsPluginCtx* ctx, int eventType, XjsWindowToken window, void* result) {
    (void)ctx; (void)window; (void)result;
    if (eventType != XJS_PLUGIN_EVT_SKIN) return;
    ApplySkin();
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}
extern "C" __declspec(dllexport) void XJS_PLUGIN_CALL XjsPlugin_OnHostGone(XjsPluginCtx* ctx) {
    (void)ctx;   /* 引擎即将销毁: 无常驻引擎资源, 窗口随 Shutdown 关闭 */
}
