/*
 * sql_generator.cpp — 蜡牛快搜(D2D) 插件示例「SQL 生成器」(GDI+ 纯自绘, 一比一复刻源页面)
 * 形态 = manifest.json + 本 DLL (类型 "app"): 自建独立无边框窗口 + GDI+ 全自绘, 宿主不参与窗口生命周期
 * (窗口口径同 space-map: 无 WS_CAPTION 防系统标题栏叠画, WS_THICKFRAME 保留 DWM 阴影/拖边/Snap,
 *  WM_NCCALCSIZE 1px 内缩保圆角抗锯齿, WM_NCACTIVATE lParam=-1 / WM_NCPAINT 吞标准框架重绘)。
 * 源样式 = 原版插件页面 index.html (参照 CSS 类名/文案键即对齐锚点, 参照落盘位置记于会话记忆):
 *   布局/几何/字号/圆角/悬停色 全部按 CSS 值折算 (px = CSS px × dpi), 文案 = 原版语言包 Sg 段
 *   (RCDATA 301..305 五语言, SG(key,def) 回退链与页面一致: 当前窗语言 → 键 → 页面硬编码中文)。
 * 功能链:
 *   - 入口 = 搜索框右键菜单"SQL 生成器"(BuildMenu 动态出项, 文字表照抄原版 sqlmenu.js NAMES 全表);
 *   - 可视化配置 SELECT/WHERE/GROUP BY/ORDER BY/LIMIT 生成 SQL (构建逻辑 = 页面 buildSQL/wrowExpr
 *     的 C++ 移植, 转义口径一致: 单引号翻倍, LIKE 模式反斜杠双写);
 *   - "填入搜索框执行" = SearchSetText(mode="sql", execute=1), owner = 打开菜单的发起窗口令牌
 *     (辅助窗口绑定 owner 口径; owner 失效回落主窗);
 *   - 接管型搜索模式 kw-sql: 窗口开着 = 填预览框并执行 (原版宿主转发语义); 没开 = 直接执行;
 *   - 皮肤 = GetSkinJsonOf(owner) 七色 + 派生 (panel-2/border-strong/text-faint 混合系数按默认皮肤
 *     实测值标定), EVT_SKIN 重取重绘;
 *   - SQL 预览 = 自绘多行编辑器 (等宽字体/光标/选区/剪贴板/IME/滚动), 文本可改后执行。
 * 线程: 宿主回调全部 UI 线程, 无自建线程。
 */
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <imm.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../../xjs_plugin_sdk.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "shlwapi.lib")

using namespace Gdiplus;

/* ==================== 宿主接口 (Init 存下, 指针终身有效) ==================== */
static const XjsPluginHost* g_host = NULL;
static XjsPluginCtx*        g_ctx  = NULL;
static XjsWindowToken       g_ownerToken = 0;   /* 打开插件窗口的发起搜索窗 (辅助窗口绑定 owner 口径) */
static ULONG_PTR            g_gdipToken = 0;

/* ==================== 基础工具 ==================== */
static std::wstring W8(const char* s) {
    if (!s || !*s) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    std::wstring w(n > 0 ? n : 1, 0);
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    while (!w.empty() && w.back() == 0) w.pop_back();
    return w;
}
static std::string U8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s(n > 0 ? n : 0, 0);
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}
/* CSS #RRGGBB 字面量 → COLORREF (0x00BBGGRR): 色值一律经此包装, 直接写 0xRRGGBB 会被按 BGR 解释 */
static constexpr COLORREF CSS(unsigned css) { return RGB((css >> 16) & 0xff, (css >> 8) & 0xff, css & 0xff); }
static COLORREF Mix(COLORREF a, COLORREF b, double t) {
    return RGB((int)(GetRValue(a) * (1 - t) + GetRValue(b) * t + 0.5),
               (int)(GetGValue(a) * (1 - t) + GetGValue(b) * t + 0.5),
               (int)(GetBValue(a) * (1 - t) + GetGValue(b) * t + 0.5));
}

/* ==================== 插件目录 / 语言包资源 ==================== */
static std::wstring PluginDirOf() {
    static std::wstring dir;
    if (!dir.empty()) return dir;
    HMODULE mod = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&PluginDirOf, &mod))
        return dir;
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(mod, path, MAX_PATH)) return dir;
    dir.assign(path);
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash);
    return dir;
}
/* RCDATA → UTF-8 字节串 (301=zh-CN 302=zh-TW 303=en 304=ko 305=th; 原版语言包 Sg 段原文) */
static std::string LoadRcUtf8(int id) {
    HMODULE hMod = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&LoadRcUtf8, &hMod))
        return std::string();
    HRSRC rs = FindResourceW(hMod, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!rs) return std::string();
    HGLOBAL h = LoadResource(hMod, rs);
    if (!h) return std::string();
    DWORD n = SizeofResource(hMod, rs);
    const void* p = n ? LockResource(h) : NULL;
    if (!p) return std::string();
    return std::string((const char*)p, n);
}

/* ==================== 界面语言 (原版页面 SG(key, def) 同构查表) ==================== */
/* 语言包 = {"Sg":{"键":"译文",...}} 扁平对象; 打开窗口时按发起窗生效语言装载一次。
   无 ms 包 (原版无马来语): ms 与未知代码回落英文, 与原版回退链一致。 */

static int LangIndexFromCode(const std::string& code) {
    if (code == "zh") return 0;
    if (code == "zh-TW") return 1;
    if (code == "en") return 2;
    if (code == "ko") return 3;
    if (code == "th") return 4;
    return -1;   /* ms/未知 → 英文 */
}
static int LangRcOf(int idx) { static const int RC[5] = { 301, 302, 303, 304, 305 }; return RC[idx]; }
/* 页面 lang 值 (仅记账; 渲染文案全走 SG 表) */
static const char* LangPageCodeOf(int idx) {
    static const char* PC[5] = { "zh-CN", "zh-TW", "en", "ko", "th" };
    return PC[idx];
}

/* auto → 系统 UI 语言 (与宿主 XjsI18nSystemLang 同规则: 中文按子语言分简/繁) */
static std::string SystemLangCode() {
    LANGID ui = GetUserDefaultUILanguage();
    WORD prim = PRIMARYLANGID(ui), sub = SUBLANGID(ui);
    if (prim == LANG_CHINESE)
        return (sub == SUBLANG_CHINESE_TRADITIONAL || sub == SUBLANG_CHINESE_HONGKONG || sub == SUBLANG_CHINESE_MACAU)
                   ? "zh-TW" : "zh";
    if (prim == LANG_KOREAN) return "ko";
    if (prim == LANG_THAI)   return "th";
    if (prim == LANG_MALAY)  return "ms";
    return "en";
}
/* 发起窗口语言代码 (settings.get "语言"; auto 按系统规则解析; 失败 = 英文) */
static std::string WindowLangCode(XjsWindowToken window) {
    if (!g_host || !g_host->QueryApi) return "en";
    auto get = (XjsApiSettingsGet)g_host->QueryApi(g_ctx, XJS_API_SETTINGS_GET);
    if (!get) return "en";
    char j[1024] = {};
    if (get(g_ctx, window, j, sizeof(j)) <= 0) return "en";
    const char* k = strstr(j, "\"语言\":\"");
    if (!k) return "en";
    k += strlen("\"语言\":\"");   /* 中文键 UTF-8 = 汉字 6 字节 + 引号/冒号/引号 3 字节 */
    std::string code;
    while (*k && *k != '"') code.push_back(*k++);
    if (code.empty() || code == "auto") return SystemLangCode();
    return code;
}

/* Sg 段装载: 在 {"Sg":{...}} 里逐对提取 "键":"译文" (转义还原)。专用小解析器, 不引 JSON 库。 */
static void LoadLangTable(const std::string& json8, std::map<std::wstring, std::wstring>* out) {
    out->clear();
    size_t p = json8.find("\"Sg\"");
    if (p == std::string::npos) return;
    p = json8.find('{', p);
    if (p == std::string::npos) return;
    p++;
    auto skipWs = [&](size_t& i) { while (i < json8.size() && (json8[i] == ' ' || json8[i] == '\n' || json8[i] == '\r' || json8[i] == '\t')) i++; };
    auto readStr = [&](size_t& i, std::wstring* s) -> bool {
        skipWs(i);
        if (i >= json8.size() || json8[i] != '"') return false;
        i++;
        std::string raw;
        while (i < json8.size() && json8[i] != '"') {
            char c = json8[i];
            if (c == '\\' && i + 1 < json8.size()) {
                char e = json8[++i];
                switch (e) {
                    case '"': raw += '"'; break;
                    case '\\': raw += '\\'; break;
                    case '/': raw += '/'; break;
                    case 'n': raw += '\n'; break;
                    case 'r': raw += '\r'; break;
                    case 't': raw += '\t'; break;
                    case 'u': {
                        if (i + 4 >= json8.size()) return false;
                        unsigned cp = 0;
                        for (int q = 1; q <= 4; q++) {
                            char h = json8[i + q]; cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                            else return false;
                        }
                        i += 4;
                        if (cp < 0x80) raw += (char)cp;
                        else if (cp < 0x800) {
                            raw += (char)(0xC0 | (cp >> 6));
                            raw += (char)(0x80 | (cp & 0x3F));
                        } else {
                            raw += (char)(0xE0 | (cp >> 12));
                            raw += (char)(0x80 | ((cp >> 6) & 0x3F));
                            raw += (char)(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: return false;
                }
                i++;
            } else { raw += c; i++; }
        }
        if (i >= json8.size()) return false;
        i++;   /* 收尾引号 */
        *s = W8(raw.c_str());
        return true;
    };
    for (;;) {
        std::wstring key, val;
        skipWs(p);
        if (p >= json8.size() || json8[p] == '}') break;
        if (!readStr(p, &key)) break;
        skipWs(p);
        if (p >= json8.size() || json8[p] != ':') break;
        p++;
        if (!readStr(p, &val)) break;
        (*out)[key] = val;
    }
}

/* 生效语言表 (窗口打开时装载; g_langIdx 记语言档, -1 = 未装载全回落 def) */
static std::map<std::wstring, std::wstring> g_sg[5];
static int g_langIdx = -1;
/* 页面 SG(key, def) 同构: 当前语言表 → 键缺失回 def (页面硬编码中文) */
static std::wstring SG(const char* key, const wchar_t* def) {
    if (g_langIdx >= 0) {
        auto& tab = g_sg[g_langIdx];
        auto it = tab.find(W8(key));
        if (it != tab.end() && !it->second.empty()) return it->second;
    }
    return def;
}

/* 搜索框菜单项文字表 (照抄原版 sqlmenu.js 的 NAMES 全表; 精确命中 → 前缀回退 → 英文, 同原版 menuText) */
static const struct { const char* code; const wchar_t* name; } MENU_NAMES[] = {
    { "zh-CN", L"SQL 生成器" },      { "zh-TW", L"SQL 產生器" },   { "en", L"SQL Generator" },
    { "ja", L"SQL ジェネレーター" }, { "ko", L"SQL 생성기" },      { "de", L"SQL-Generator" },
    { "fr", L"Générateur SQL" },     { "es", L"Generador SQL" },   { "pt", L"Gerador SQL" },
    { "ru", L"SQL-генератор" },      { "it", L"Generatore SQL" },  { "ar", L"مولّد SQL" },
    { "hi", L"SQL जनरेटर" },         { "th", L"ตัวสร้าง SQL" },    { "vi", L"Trình tạo SQL" },
    { "nl", L"SQL-generator" },      { "pl", L"Generator SQL" },   { "tr", L"SQL Üretici" },
    { "id", L"Pembuat SQL" },
};
static std::wstring MenuTextForLang(const std::string& code) {
    for (auto& e : MENU_NAMES)
        if (code == e.code) return e.name;
    std::string prefix = code.substr(0, code.find('-'));
    for (auto& e : MENU_NAMES) {
        std::string ec = e.code;
        if (ec.substr(0, ec.find('-')) == prefix) return e.name;
    }
    return L"SQL Generator";
}

/* ==================== 皮肤 (GetSkinJsonOf, 仅 UI 线程) ==================== */
struct Skin {
    COLORREF bg1 = CSS(0xf6f7fb), bg2 = CSS(0xeaedf5), panel = RGB(255, 255, 255), panel2 = CSS(0xf7f8fc);
    COLORREF text = CSS(0x1d2230), dim = CSS(0x646b7a), faint = CSS(0x98a0ae);
    COLORREF accent = CSS(0x4a63e7), border = CSS(0xe4e7f0), borderStrong = CSS(0xd3d9e6);
    COLORREF hoverBg = CSS(0xe9edfb);       /* 行悬停 (菜单项) */
    COLORREF danger = CSS(0xe0442e);        /* 关闭钮悬停红 (页面 .close-btn:hover 原值) */
    float dpi = 1.0f;
};
static Skin g_sk;
static bool HexCol(const std::string& s, COLORREF* out) {
    if (s.size() != 7 || s[0] != '#') return false;
    auto hb = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1; };
    int r = hb(s[1]) * 16 + hb(s[2]), g = hb(s[3]) * 16 + hb(s[4]), b = hb(s[5]) * 16 + hb(s[6]);
    if (r < 0 || g < 0 || b < 0) return false;
    *out = RGB(r, g, b);
    return true;
}
/* 皮肤 JSON 取字段 (扁平 {"bg1":"#..",..}) */
static std::string SkinField(const char* j, const char* key) {
    std::string k = std::string("\"") + key + "\":\"";
    const char* p = strstr(j, k.c_str());
    if (!p) return std::string();
    p += k.size();
    std::string v;
    while (*p && *p != '"') v.push_back(*p++);
    return v;
}
/* ApplySkin: 七色直出 + 派生 (panel-2/border-strong/text-faint 宿主皮肤 JSON 无对应键,
   混合系数按默认皮肤实测值标定:
   panel-2 = panel 混 bg1 0.70 (#ffffff×#f6f7fb → #f8f9fc ≈ 原 #f7f8fc)
   border-strong = line 混 text 0.20 (#e4e7f0×#1d2230 → #d4dae6 ≈ 原 #d3d9e6)
   text-faint = dim 混 bg1 0.42 (#646b7a×#f6f7fb → #9aa2b0 ≈ 原 #98a0ae)) */
static void ApplySkin(XjsWindowToken window) {
    char j[512] = {};
    bool ok = false;
    if (g_host && g_host->size >= offsetof(XjsPluginHost, GetSkinJsonOf) + sizeof(g_host->GetSkinJsonOf))
        ok = g_host->GetSkinJsonOf(g_ctx, window, j, sizeof(j)) > 0;
    if (!ok && g_host) ok = g_host->GetSkinJson(g_ctx, j, sizeof(j)) > 0;
    if (ok) {
        HexCol(SkinField(j, "bg1"), &g_sk.bg1);
        HexCol(SkinField(j, "bg2"), &g_sk.bg2);
        HexCol(SkinField(j, "panel"), &g_sk.panel);
        HexCol(SkinField(j, "text"), &g_sk.text);
        HexCol(SkinField(j, "dim"), &g_sk.dim);
        HexCol(SkinField(j, "accent"), &g_sk.accent);
        HexCol(SkinField(j, "line"), &g_sk.border);
    }
    g_sk.panel2 = Mix(g_sk.panel, g_sk.bg1, 0.70);
    g_sk.borderStrong = Mix(g_sk.border, g_sk.text, 0.20);
    g_sk.faint = Mix(g_sk.dim, g_sk.bg1, 0.42);
    g_sk.hoverBg = Mix(g_sk.panel, g_sk.accent, 0.10);
    /* 重绘由调用方负责 (OnEvent/OpenWindow) — 本函数在 g_hwnd 声明之前 */
}

/* ==================== 数据模型 (页面状态 1:1) ==================== */

/* SELECT 模式: 0=star 1=cols 2=agg (selMode 下拉三项) */
static int g_selMode = 0;
static bool g_colChk[12] = {};            /* cols-grid 12 项 (Path..MD5) */
static const wchar_t* const COL_NAMES[12] = {
    L"Path", L"ParentPath", L"AnyParent", L"FName", L"Ext", L"Size",
    L"ModTime", L"FileType", L"IsDir", L"FAttr", L"FileContent", L"MD5(FileContent)"
};
static bool g_aggChip[6] = { true, false, false, false, false, false };   /* COUNT(*) 固定 on */
static const wchar_t* const AGG_EXPR[6] = {
    L"COUNT(*)", L"SUM(Size)", L"AVG(Size)", L"MIN(ModTime)", L"MAX(Size)", L"COUNT(DISTINCT Ext)"
};
static int g_groupBy = 0;                 /* 0=(不分组) 1..6 */
static std::wstring g_having;             /* HAVING 输入 */

/* WHERE 条件类型 (页面 WT_OPTIONS 原序) */
enum WT { WT_FILETYPE = 0, WT_SIZE, WT_PATH, WT_EXT, WT_FNAME, WT_MTIME, WT_CONTENT, WT_ATTR,
          WT_ISDIR, WT_PARENTNAME, WT_PARENTPATH, WT_ANYPARENT, WT_SCORE, WT_FILENUMBER, WT_CUSTOM, WT_N };
struct WRow {
    int conn = 0;          /* 0=AND 1=OR (首行连接词隐藏但值保留, 同原版) */
    int type = WT_FILETYPE;
    int op = 0;            /* wop 下拉序号 (类型切换重置 = 页面 innerHTML 重建) */
    int preset = 2;        /* mtime 预设序 (页面默认第 3 项 selected) */
    std::wstring v, v2, v3;
    int unit = 2, unit2 = 2;   /* 0=B 1=KB 2=MB 3=G (页面默认 MB) */
    bool sub = true;           /* path 含子目录 (默认勾选) */
};
static std::vector<WRow> g_wrows;
/* 大小单位后缀 (页面 wunit option value: ""/"K"/"M"/"G"; SQL 拼接 = 值+后缀, 如 '100M') */
static const wchar_t* SizeUnitSuf(int u) {
    static const wchar_t* const SUF[4] = { L"", L"K", L"M", L"G" };
    return (u >= 0 && u < 4) ? SUF[u] : L"";
}
static std::wstring JoinW(const std::vector<std::wstring>& v, const wchar_t* sep) {
    std::wstring o;
    for (size_t i = 0; i < v.size(); i++) {
        if (i) o += sep;
        o += v[i];
    }
    return o;
}
/* 各类型 wop 选项表 (页面 wctlHtml 的 <option> 1:1):
   m = 显示模式 — 0 纯文本(d), 1 = val+" "+SG(k,d), 2 = 纯 SG(k,d) (attr/isdir/mtime 的文案自带符号) */
struct Opt { const char* k; const wchar_t* d; const wchar_t* val; unsigned char m; };
static const Opt OP_FILETYPE[] = { { NULL, L"=", L"=", 0 }, { NULL, L"!=", L"!=", 0 },
    { "IsOneOf", L"是其一", L"IN", 1 }, { "Exclude", L"排除", L"NOT IN", 1 } };
static const Opt OP_SIZE[] = { { NULL, L">", L">", 0 }, { NULL, L">=", L">=", 0 },
    { NULL, L"<", L"<", 0 }, { NULL, L"<=", L"<=", 0 }, { NULL, L"=", L"=", 0 },
    { NULL, L"!=", L"!=", 0 }, { NULL, L"<>", L"<>", 0 }, { NULL, L"BETWEEN", L"BETWEEN", 0 } };
static const Opt OP_LIKE5[] = { { "Contains", L"包含", L"LIKE", 1 }, { "NotContains", L"不含", L"NOT LIKE", 1 },
    { "IgnoreCase", L"忽略大小写", L"ILIKE", 1 }, { "Exact", L"精确", L"=", 1 }, { "Exclude", L"排除", L"!=", 1 } };
static const Opt OP_EXT[] = { { "IsOneOf", L"是其一", L"IN", 1 }, { "Exclude", L"排除", L"NOT IN", 1 },
    { NULL, L"=", L"=", 0 }, { NULL, L"!=", L"!=", 0 } };
static const Opt OP_FNAME[] = { { "Contains", L"包含", L"LIKE", 1 }, { "NotContains", L"不含", L"NOT LIKE", 1 },
    { "IgnoreCase", L"忽略大小写", L"ILIKE", 1 }, { "NotIgnoreCase", L"不区分大小写不含", L"NOT ILIKE", 1 },
    { "Regex", L"正则", L"~", 1 }, { "RegexIgnoreCase", L"正则忽略大小写", L"~*", 1 },
    { "Exact", L"精确", L"=", 1 }, { "Exclude", L"排除", L"!=", 1 } };
static const Opt OP_MTIME[] = { { "TimePreset", L"预设区间", L"preset", 2 }, { "TimeAfter", L"指定日期之后", L"after", 2 },
    { "TimeBefore", L"指定日期之前", L"before", 2 }, { "TimeBetween", L"指定日期之间", L"between", 2 } };
static const Opt OP_CONTENT[] = { { "Regex", L"正则", L"~", 1 }, { "RegexIgnoreCase", L"正则忽略大小写", L"~*", 1 },
    { "Contains", L"包含", L"LIKE", 1 }, { "NotContains", L"不含", L"NOT LIKE", 1 },
    { "IgnoreCase", L"忽略大小写", L"ILIKE", 1 } };
static const Opt OP_ATTR[] = { { "AttrExclude", L"排除 [!~]", L"!~", 2 }, { "AttrMatch", L"匹配 [~]", L"~", 2 },
    { "AttrExcludeIgnore", L"排除忽略大小写", L"!~*", 2 }, { "AttrMatchIgnore", L"匹配忽略大小写", L"~*", 2 },
    { "Exact", L"精确", L"=", 2 }, { "Exclude", L"排除", L"!=", 2 } };
static const Opt OP_ISDIR[] = { { "IsDir1", L"目录 (IsDir = 1)", L"1", 2 }, { "IsDir0", L"文件 (IsDir = 0)", L"0", 2 } };
static const Opt OP_SCORE[] = { { NULL, L">=", L">=", 0 }, { NULL, L">", L">", 0 },
    { NULL, L"<", L"<", 0 }, { NULL, L"<=", L"<=", 0 }, { NULL, L"=", L"=", 0 },
    { NULL, L"!=", L"!=", 0 }, { NULL, L"<>", L"<>", 0 } };
static const Opt OP_FNUM[] = { { NULL, L"=", L"=", 0 }, { NULL, L"!=", L"!=", 0 },
    { NULL, L">", L">", 0 }, { NULL, L"<", L"<", 0 }, { NULL, L">=", L">=", 0 }, { NULL, L"<=", L"<=", 0 } };
static const Opt* OpListOf(int type, int* n) {
    switch (type) {
        case WT_FILETYPE: *n = 4; return OP_FILETYPE;
        case WT_SIZE:     *n = 8; return OP_SIZE;
        case WT_PATH: case WT_PARENTNAME: case WT_PARENTPATH: case WT_ANYPARENT:
                          *n = 5; return OP_LIKE5;
        case WT_EXT:      *n = 4; return OP_EXT;
        case WT_FNAME:    *n = 8; return OP_FNAME;
        case WT_MTIME:    *n = 4; return OP_MTIME;
        case WT_CONTENT:  *n = 5; return OP_CONTENT;
        case WT_ATTR:     *n = 6; return OP_ATTR;
        case WT_ISDIR:    *n = 2; return OP_ISDIR;
        case WT_SCORE:    *n = 7; return OP_SCORE;
        case WT_FILENUMBER: *n = 6; return OP_FNUM;
        default:          *n = 0; return NULL;   /* custom: 无 wop */
    }
}
/* 选项显示文本 (页面 option 文本同构) */
static std::wstring OptText(const Opt& o) {
    if (o.m == 0 || !o.k) return o.d;
    std::wstring t = SG(o.k, o.d);
    return (o.m == 1) ? (std::wstring(o.val) + L" " + t) : t;
}

/* 条件类型中文名 (WT_OPTIONS.d) */
static const struct { const wchar_t* d; const char* k; } WT_DEFS[WT_N] = {
    { L"文件类型", "WTFileType" }, { L"大小", "WTSize" }, { L"路径", "WTPath" },
    { L"扩展名", "WTExt" }, { L"文件名", "WTFName" }, { L"修改时间", "WTModTime" },
    { L"文件内容", "WTContent" }, { L"属性", "WTAttr" }, { L"目录标记", "WTIsDir" },
    { L"父目录名称", "WTParentName" }, { L"父目录路径", "WTParentPath" }, { L"任意父目录", "WTAnyParent" },
    { L"评分", "WTScore" }, { L"文件编号", "WTFileNumber" }, { L"自定义", "WTCustom" },
};
static std::wstring WTypeName(int t) { return SG(WT_DEFS[t].k, WT_DEFS[t].d); }

/* 文件类型 9 类 (FILE_TYPES / FILE_TYPE_KEYS) */
static const wchar_t* const FILE_TYPES[9] = { L"视频", L"音频", L"图片", L"文档", L"办公", L"程序", L"压缩", L"系统", L"其他" };
static const char* const FILE_TYPE_KEYS[9] = { "FTVideo", "FTAudio", "FTImage", "FTDoc", "FTOffice", "FTProgram", "FTArchive", "FTSystem", "FTOther" };
static std::wstring FileTypeLabel(int i) { return SG(FILE_TYPE_KEYS[i], FILE_TYPES[i]); }

/* 时间预设 6 项 (TIME_PRESETS; 第 3 项默认选中) */
struct TimePreset { const char* k; const wchar_t* d; const wchar_t* expr; };
static const TimePreset TIME_PRESETS[6] = {
    { "TPToday",  L"今天",       L"ModTime >= CURRENT_DATE" },
    { "TP7d",     L"最近 7 天",   L"ModTime >= CURRENT_DATE - INTERVAL '7 days'" },
    { "TP30d",    L"最近 30 天",  L"ModTime >= CURRENT_DATE - INTERVAL '30 days'" },
    { "TP90d",    L"最近 90 天",  L"ModTime >= CURRENT_DATE - INTERVAL '90 days'" },
    { "TP180d",   L"最近 180 天", L"ModTime >= CURRENT_DATE - INTERVAL '180 days'" },
    { "TP1y",     L"最近 1 年",   L"ModTime >= CURRENT_DATE - INTERVAL '365 days'" },
};
static std::wstring TimePresetName(int i) { return SG(TIME_PRESETS[i].k, TIME_PRESETS[i].d); }

/* 排序 (orderField 0=无; 方向 0=DESC 1=ASC) */
static int g_orderField = 0, g_orderDir = 0, g_orderField2 = 0, g_orderDir2 = 0;
static const struct { const wchar_t* d; const char* k; const wchar_t* f; } ORDER_FIELDS[7] = {
    { L"(无)", "None", L"" }, { L"Size 大小", "OrderSize", L"Size" },
    { L"ModTime 修改时间", "OrderModTime", L"ModTime" }, { L"FName 文件名", "OrderFName", L"FName" },
    { L"Ext 扩展名", "OrderExt", L"Ext" }, { L"Path 路径", "OrderPath", L"Path" },
    { L"FileType 类型", "OrderFileType", L"FileType" },
};
static std::wstring OrderFieldLabel(int i) { return SG(ORDER_FIELDS[i].k, ORDER_FIELDS[i].d); }

/* 分组 6 项 (groupBy 0=(不分组)) */
static const struct { const wchar_t* d; const char* k; const wchar_t* f; } GROUP_FIELDS[7] = {
    { L"(不分组)", "GroupNone", L"" }, { L"FileType 类型", "GroupFileType", L"FileType" },
    { L"Ext 扩展名", "GroupExt", L"Ext" }, { L"IsDir 目录标记", "GroupIsDir", L"IsDir" },
    { L"ParentPath 父目录", "GroupParent", L"ParentPath" }, { L"FName 文件名", "GroupFName", L"FName" },
};
static std::wstring GroupLabel(int i) { return SG(GROUP_FIELDS[i].k, GROUP_FIELDS[i].d); }

static std::wstring g_limit;      /* LIMIT 数字输入 */
static bool g_noSH = true;        /* 排除系统/隐藏文件 (默认勾选) */
static std::wstring g_status;     /* 状态栏 (status div) */
static std::wstring g_sqlText;    /* SQL 预览编辑器内容 (可编辑) */

/* 常用模板 15 项 (TEMPLATES 原序原文) */
struct Tpl { const char* k; const wchar_t* d; const wchar_t* sql; };
static const Tpl TEMPLATES[15] = {
    { "TplTop50", L"前 50 个大文件", L"SELECT * FROM alltable WHERE Size > '100M' AND FAttr !~ '[SH]' ORDER BY Size DESC LIMIT 50" },
    { "TplBigVideo", L"大视频 (>500M)", L"SELECT * FROM alltable WHERE FileType='视频' AND Size > '500M' AND FAttr !~ '[SH]' ORDER BY Size DESC" },
    { "TplRecent30", L"最近 30 天修改", L"SELECT * FROM alltable WHERE ModTime >= CURRENT_DATE - INTERVAL '30 days' AND FAttr !~ '[SH]' ORDER BY ModTime DESC" },
    { "TplByType", L"按类型统计", L"SELECT FileType, COUNT(*) AS cnt FROM alltable WHERE FAttr !~ '[SH]' GROUP BY FileType ORDER BY cnt DESC" },
    { "TplByExtTop", L"按扩展名统计 TOP10", L"SELECT Ext, COUNT(*) AS cnt FROM alltable WHERE FAttr !~ '[SH]' GROUP BY Ext ORDER BY cnt DESC LIMIT 10" },
    { "TplTopDirs", L"前 20 大目录", L"SELECT * FROM alltable WHERE IsDir=1 AND FAttr !~ '[SH]' ORDER BY Size DESC LIMIT 20" },
    { "TplDupName", L"重名+同大小查重", L"SELECT FName, Size, COUNT(*) FROM alltable WHERE FAttr !~ '[SH]' GROUP BY FName, Size HAVING COUNT(*) > 1" },
    { "TplDupMd5", L"MD5 内容查重", L"SELECT MD5(FileContent), COUNT(*) FROM alltable WHERE FAttr !~ '[SH]' GROUP BY MD5(FileContent) HAVING COUNT(*) > 1" },
    { "TplEmptyDir", L"空目录", L"SELECT * FROM alltable WHERE IsDir=1 AND Size=0 AND FAttr !~ '[SH]'" },
    { "TplTempFiles", L"临时/缓存文件", L"SELECT * FROM alltable WHERE Ext IN ('tmp','temp','log','bak','cache','db-journal') AND FAttr !~ '[SH]'" },
    { "TplColdData", L"冷数据 (>1G 且 180 天未动)", L"SELECT * FROM alltable WHERE Size > '1G' AND ModTime < CURRENT_DATE - INTERVAL '180 days' AND FAttr !~ '[SH]' ORDER BY ModTime" },
    { "TplArchives", L"压缩包", L"SELECT * FROM alltable WHERE Ext IN ('zip','rar','7z','tar','gz','bz2','xz') AND FAttr !~ '[SH]' ORDER BY Size DESC" },
    { "TplDocsInDir", L"指定目录下的文档", L"SELECT * FROM alltable WHERE Path LIKE 'D:\\\\Work\\\\%' AND FileType IN ('文档','办公') AND FAttr !~ '[SH]'" },
    { "TplFNameRegex", L"文件名正则 (测试/备份)", L"SELECT * FROM alltable WHERE FName ~ 'test|backup|\\.bak$' AND FAttr !~ '[SH]'" },
    { "TplDirSize", L"按目录统计大小", L"SELECT ParentPath, COUNT(*) AS cnt, SUM(Size) AS total FROM alltable WHERE FAttr !~ '[SH]' GROUP BY ParentPath ORDER BY total DESC LIMIT 20" },
};
static std::wstring TplName(int i) { return SG(TEMPLATES[i].k, TEMPLATES[i].d); }

/* ==================== SQL 生成 (页面 escStr/escLike/wrowExpr/buildSQL 的 C++ 移植) ==================== */
/* 字符串字面量: 单引号翻倍 ('' 表示字面 '); 反斜杠是普通字符不转义 (=/!= 精确与正则 ~/~* 用) */
static std::wstring EscStr(const std::wstring& s) {
    std::wstring o = L"'";
    for (wchar_t c : s) { if (c == L'\'') o += L"''"; else o += c; }
    return o + L"'";
}
/* LIKE/ILIKE 模式串: 反斜杠是模式转义字符须双写 (\\ 表示字面 \); 单引号仍由 EscStr 处理 */
static std::wstring EscLike(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) { if (c == L'\\') o += L"\\\\"; else o += c; }
    return o;
}
static std::wstring TrimW(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}
/* 读取一行条件生成 SQL 片段 (wrowExpr 移植; 空值 = 空串 = 该行不参与) */
static std::wstring WrowExpr(const WRow& r) {
    const Opt* ops; int nop = 0;
    ops = OpListOf(r.type, &nop);
    std::wstring opv = (r.op >= 0 && r.op < nop) ? ops[r.op].val : L"";
    switch (r.type) {
        case WT_FILETYPE: {
            if (r.v.empty()) return L"";
            if (opv == L"IN" || opv == L"NOT IN") return L"FileType " + opv + L" (" + EscStr(r.v) + L")";
            return L"FileType " + opv + L" " + EscStr(r.v);
        }
        case WT_SIZE: {
            if (opv == L"BETWEEN") {
                if (r.v.empty() || r.v2.empty()) return L"";
                return L"Size BETWEEN " + EscStr(r.v + SizeUnitSuf(r.unit)) + L" AND " + EscStr(r.v2 + SizeUnitSuf(r.unit2));
            }
            if (r.v.empty()) return L"";
            return L"Size " + opv + L" " + EscStr(r.v + SizeUnitSuf(r.unit));
        }
        case WT_PATH: {
            if (r.v.empty()) return L"";
            std::wstring v = r.v;
            if (opv == L"LIKE" || opv == L"NOT LIKE" || opv == L"ILIKE") {
                /* 尾部反斜杠/斜杠剥掉 (页面 v.replace(/[\\/]+$/,'')) */
                while (!v.empty() && (v.back() == L'\\' || v.back() == L'/')) v.pop_back();
                v = EscLike(v) + (r.sub ? L"\\\\%" : L"");
            }
            /* =/!= 精确匹配: 反斜杠是普通字符不双写 */
            return L"Path " + opv + L" " + EscStr(v);
        }
        case WT_EXT: {
            if (r.v.empty()) return L"";
            /* 逗号/空白/全角逗号切分, 去前导点 (页面 v.split(/[,\s，]+/)) */
            std::wstring list, cur;
            auto flush = [&]() {
                if (cur.empty()) return;
                while (!cur.empty() && cur[0] == L'.') cur.erase(0, 1);
                if (!list.empty()) list += L", ";
                list += EscStr(cur);
                cur.clear();
            };
            for (wchar_t c : r.v) {
                if (c == L',' || c == L'，' || c == L' ' || c == L'\t') { flush(); continue; }
                cur += c;
            }
            flush();
            if (opv == L"IN" || opv == L"NOT IN") return L"Ext " + opv + L" (" + list + L")";
            std::wstring v = r.v;
            while (!v.empty() && v[0] == L'.') v.erase(0, 1);
            return L"Ext " + opv + L" " + EscStr(v);
        }
        case WT_FNAME: {
            if (r.v.empty()) return L"";
            if (opv == L"LIKE" || opv == L"NOT LIKE" || opv == L"ILIKE" || opv == L"NOT ILIKE")
                return L"FName " + opv + L" " + EscStr(L"%" + EscLike(r.v) + L"%");
            /* 正则 (~/~*) 与精确 (=/!=): 反斜杠是正则/普通字符不双写 */
            return L"FName " + opv + L" " + EscStr(r.v);
        }
        case WT_MTIME: {
            if (opv == L"preset") return r.preset >= 0 && r.preset < 6 ? TIME_PRESETS[r.preset].expr : L"";
            if (opv == L"after" || opv == L"before") {
                if (r.v2.empty()) return L"";
                return std::wstring(L"ModTime ") + (opv == L"after" ? L">=" : L"<=") + L" CAST(" + EscStr(r.v2) + L" AS int)";
            }
            if (opv == L"between") {
                if (r.v2.empty() || r.v3.empty()) return L"";
                return L"ModTime BETWEEN CAST(" + EscStr(r.v2) + L" AS int) AND CAST(" + EscStr(r.v3) + L" AS int)";
            }
            return L"";
        }
        case WT_CONTENT: {
            if (r.v.empty()) return L"";
            if (opv == L"LIKE" || opv == L"NOT LIKE" || opv == L"ILIKE")
                return L"FileContent " + opv + L" " + EscStr(L"%" + EscLike(r.v) + L"%");
            /* 正则类: 原样按正则匹配, 反斜杠是正则转义不双写, 单引号由 EscStr 转义 */
            return L"FileContent " + opv + L" " + EscStr(r.v);
        }
        case WT_ATTR: {
            if (r.v.empty()) return L"";
            return L"FAttr " + opv + L" " + EscStr(r.v);
        }
        case WT_ISDIR:
            return std::wstring(L"IsDir = ") + opv;
        case WT_PARENTNAME: case WT_PARENTPATH: case WT_ANYPARENT: {
            const wchar_t* pf = r.type == WT_PARENTNAME ? L"ParentName" : r.type == WT_PARENTPATH ? L"ParentPath" : L"AnyParent";
            if (r.v.empty()) return L"";
            if (opv == L"LIKE" || opv == L"NOT LIKE" || opv == L"ILIKE")
                return std::wstring(pf) + L" " + opv + L" " + EscStr(L"%" + EscLike(r.v) + L"%");
            return std::wstring(pf) + L" " + opv + L" " + EscStr(r.v);
        }
        case WT_SCORE: case WT_FILENUMBER: {
            const wchar_t* nf = r.type == WT_SCORE ? L"Score" : L"FileNumber";
            if (r.v.empty()) return L"";
            return std::wstring(nf) + L" " + opv + L" " + r.v;
        }
        default:   /* custom: 直接写表达式 */
            return r.v;
    }
}

/* buildSQL 移植 (逐段同页面): SELECT → WHERE → GROUP BY → HAVING → ORDER BY → LIMIT, \n 连接 */
static std::wstring BuildSQL() {
    std::vector<std::wstring> o;
    std::wstring sel;
    if (g_selMode == 1) {   /* cols: 勾选字段逗号连接, 无勾选 = * */
        std::wstring cols;
        for (int i = 0; i < 12; i++) if (g_colChk[i]) { if (!cols.empty()) cols += L", "; cols += COL_NAMES[i]; }
        sel = cols.empty() ? L"*" : cols;
    } else if (g_selMode == 2) {   /* agg: COUNT(*) + 勾选聚合 */
        sel = L"COUNT(*)";
        for (int i = 1; i < 6; i++) if (g_aggChip[i]) { sel += L", "; sel += AGG_EXPR[i]; }
    } else {
        sel = L"*";
    }
    o.push_back(L"SELECT " + sel + L" FROM alltable");
    std::vector<std::wstring> w;
    for (size_t i = 0; i < g_wrows.size(); i++) {
        std::wstring e = WrowExpr(g_wrows[i]);
        if (!e.empty()) {
            if (!w.empty()) w.push_back(g_wrows[i].conn ? L"OR" : L"AND");
            w.push_back(e);
        }
    }
    if (g_noSH) {
        if (!w.empty()) w.push_back(L"AND");   /* 已有条件时必须补连接词, 否则拼出语法错误 (页面同注释) */
        w.push_back(L"FAttr !~ '[SH]'");
    }
    if (!w.empty()) o.push_back(L"WHERE " + JoinW(w, L" "));
    if (g_selMode == 2 && g_groupBy > 0) o.push_back(L"GROUP BY " + std::wstring(GROUP_FIELDS[g_groupBy].f));
    std::wstring having = TrimW(g_having);
    if (!having.empty()) o.push_back(L"HAVING " + having);
    std::vector<std::wstring> ob;
    if (g_orderField > 0) ob.push_back(std::wstring(ORDER_FIELDS[g_orderField].f) + (g_orderDir ? L" ASC" : L" DESC"));
    if (g_orderField2 > 0) ob.push_back(std::wstring(ORDER_FIELDS[g_orderField2].f) + (g_orderDir2 ? L" ASC" : L" DESC"));
    if (!ob.empty()) o.push_back(L"ORDER BY " + JoinW(ob, L", "));
    std::wstring lm = TrimW(g_limit);
    if (!lm.empty()) o.push_back(L"LIMIT " + lm);
    return JoinW(o, L"\n");
}

/* 配置变化 → 重建预览 (页面 refresh(): preview.value = buildSQL(), 无条件覆盖 —
   手动改过的预览在下一次配置变化时同样被重建, 原版行为) */
static void RefreshSql();

/* ==================== GDI+ 绘制基建 (space-map 同款) ==================== */
static HWND g_hwnd = NULL;
static float g_dpi = 1.0f;
#define S(v) ((v) * g_dpi)
static void InvalidateAll() { if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE); }

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
static Gdiplus::Font* FM(float px, bool bold) {   /* 编辑器等宽 (页面 textarea: "Cascadia Code","Consolas") */
    unsigned key = 0x10000000u + (unsigned)(px * 4.0f) * 2 + (bold ? 1 : 0);
    auto it = g_fonts.find(key);
    if (it != g_fonts.end()) return it->second;
    Gdiplus::Font* f = new Gdiplus::Font(L"Consolas", px,
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
static Gdiplus::SolidBrush* Br(COLORREF c, int a = 255) {
    static Gdiplus::SolidBrush b(Gdiplus::Color(255, 0, 0, 0));
    b.SetColor(Gdiplus::Color((BYTE)a, GetRValue(c), GetGValue(c), GetBValue(c)));
    return &b;
}
static Gdiplus::Color GC(COLORREF c, int a = 255) { return Gdiplus::Color((BYTE)a, GetRValue(c), GetGValue(c), GetBValue(c)); }
static Gdiplus::Pen* PenP(COLORREF c, int a, float w) {
    static Gdiplus::Pen p(Gdiplus::Color(255, 0, 0, 0), 1.0f);
    p.SetColor(Gdiplus::Color((BYTE)a, GetRValue(c), GetGValue(c), GetBValue(c)));
    p.SetWidth(w);
    p.SetDashStyle(Gdiplus::DashStyleSolid);
    return &p;
}
static void DrawStr(Gdiplus::Graphics& g, const std::wstring& s, Gdiplus::Font* f, const RectF& rc,
                    Gdiplus::Brush* br, int align = 1, int valign = 1, bool wrap = false, bool ellipsis = true) {
    if (s.empty() || rc.Width < 1 || rc.Height < 1) return;
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

/* ==================== 编辑器基础 (拆行/测量) ==================== */
static std::vector<std::wstring> g_sqlLines;
static int  g_caretLine = 0, g_caretCol = 0;    /* 光标 */
static int  g_selLine = -1, g_selCol = -1;      /* 选区锚点 (-1 = 无选区) */
static float g_edScrollY = 0, g_edScrollX = 0;
static float g_goalX = 0;                        /* 上下移动的目标 x (标准编辑器行为) */
static bool  g_edFocus = false;
static bool  g_caretOn = true;
static float g_edLineH() { return S(20.4f); }    /* 12px × 1.7 (页面 line-height) */
static float g_cfgScroll = 0, g_cfgMaxScroll = 0, g_cfgContentH = 0;

static void SqlSplit() {
    g_sqlLines.clear();
    const std::wstring& t = g_sqlText;
    size_t a = 0;
    for (size_t i = 0; i <= t.size(); i++) {
        if (i == t.size() || t[i] == L'\n') {
            g_sqlLines.push_back(t.substr(a, i - a));
            a = i + 1;
        }
    }
    if (g_sqlLines.empty()) g_sqlLines.push_back(L"");
}
/* 光标所在行内列 → 前缀宽 (编辑器等宽字体) */
static float EdColX(Gdiplus::Graphics& g, const std::wstring& line, int col) {
    if (col <= 0) return 0;
    if (col > (int)line.size()) col = (int)line.size();
    return MeasureStr(g, line.substr(0, col), FM(S(12), false));
}
/* 点击 x → 列 (按字符中点归属) */
static int EdColAt(Gdiplus::Graphics& g, const std::wstring& line, float x) {
    float prev = 0;
    for (int i = 0; i <= (int)line.size(); i++) {
        float cx = EdColX(g, line, i);
        if (x <= (prev + cx) / 2) return i;
        prev = cx;
    }
    return (int)line.size();
}

/* ==================== 控件命中表 (ComputeLayout 产出, 绘制/命中共用) ====================
   坐标系: cfg 内容流控件 = 内容坐标 (相对 cfg 内容原点, rel=true, 绘制/命中统一经
   cfg.Y - cfgScroll 变换); bar/标题栏/预览控件 = 窗口视口坐标 (rel=false, 直接用)。 */
struct Layout {
    RectF titlebar, cfg, preview, bar;
    RectF phead, editor, tpl, status;
    RectF btnRun, btnCopy;
    /* cfg 卡片 (sec) 与标题/hint 矩形 (绘制用; 内容流由 ComputeLayout 现算) */
    RectF sec1, sec2, sec3, hintRc;
    std::wstring hintText;
    float contentW;
    float cfgMaxY;   /* cfg 视口底 */
};
static Layout g_lo;
enum CtlId {
    CI_NONE = 0, CI_CLOSE,
    CI_SELMODE, CI_COLCHK, CI_AGGCHIP, CI_GROUPBY, CI_HAVING,
    CI_WCONN, CI_WTYPE, CI_WOP, CI_WV, CI_WV2, CI_WV3, CI_WUNIT, CI_WUNIT2, CI_WSUB, CI_WDEL,
    CI_ADDCOND, CI_ORDERF, CI_ORDERD, CI_ORDERF2, CI_ORDERD2, CI_LIMIT, CI_NOSH,
    CI_EDITOR, CI_TPL, CI_RUN, CI_COPY
};
struct CtlRect { int id; RectF rc; int row; int idx; bool rel; };
static std::vector<CtlRect> g_ctls;
/* 控件视口矩形 (命中/悬停共用的变换落点) */
static RectF CtlViewRc(const CtlRect& c) {
    RectF rc = c.rc;
    if (c.rel) { rc.Y += g_lo.cfg.Y - g_cfgScroll; }
    return rc;
}
static const CtlRect* HitCtl(POINT pt) {
    for (int i = (int)g_ctls.size() - 1; i >= 0; i--) {
        RectF rc = CtlViewRc(g_ctls[i]);
        if (pt.x >= rc.X && pt.x < rc.X + rc.Width && pt.y >= rc.Y && pt.y < rc.Y + rc.Height)
            return &g_ctls[i];
    }
    return NULL;
}
static CtlRect* CtlOf(int id, int row = -1) {
    for (auto& c : g_ctls)
        if (c.id == id && (row < 0 || c.row == row)) return &c;
    return NULL;
}
/* 焦点/悬停的控件编码 (id + row) */
static int  g_focusId = 0, g_focusRow = -1;
static int  g_pressId = 0, g_pressRow = -1;   /* 按压中的按钮 (btn-main active 观感) */
static int  g_hotId = 0, g_hotRow = -1;
static bool SameCtl(int id, int row, const CtlRect* c) { return c && c->id == id && (row < 0 || c->row == row); }

/* ==================== 布局 (页面 CSS 值折算; 控件矩形全量产出) ==================== */

static float DropW(Gdiplus::Graphics& g, const std::vector<std::wstring>& items, float minW) {
    float w = minW;
    for (auto& s : items) w = std::max(w, MeasureStr(g, s, F(S(12), false)) + S(26));
    return w;
}
static std::vector<std::wstring> FTItems() {
    std::vector<std::wstring> v;
    for (int i = 0; i < 9; i++) v.push_back(FileTypeLabel(i));
    return v;
}
static std::vector<std::wstring> OpItems(const Opt* ops, int n) {
    std::vector<std::wstring> v;
    for (int i = 0; i < n; i++) v.push_back(OptText(ops[i]));
    return v;
}
static std::vector<std::wstring> UnitItems() { return { L"B", L"KB", L"MB", L"GB" }; }
static std::vector<std::wstring> PresetItems() {
    std::vector<std::wstring> v;
    for (int i = 0; i < 6; i++) v.push_back(TimePresetName(i));
    return v;
}
static std::vector<std::wstring> GroupItems() {
    std::vector<std::wstring> v;
    for (int i = 0; i < 6; i++) v.push_back(GroupLabel(i));
    return v;
}
static std::vector<std::wstring> OrderItems() {
    std::vector<std::wstring> v;
    for (int i = 0; i < 7; i++) v.push_back(OrderFieldLabel(i));
    return v;
}

static void ComputeLayout() {
    if (!g_hwnd) return;
    RECT crc; GetClientRect(g_hwnd, &crc);
    float W = (float)(crc.right - crc.left), H = (float)(crc.bottom - crc.top);
    Graphics gi(g_hwnd);
    g_ctls.clear();
    g_lo.titlebar = RectF(0, 0, W, S(40));
    float barH = S(48);
    float cfgW = W * 0.54f;
    if (cfgW < S(380)) cfgW = S(380);
    if (W - cfgW < S(320) && W > S(320)) cfgW = W - S(320);
    g_lo.cfg = RectF(0, S(40), cfgW, H - S(40) - barH);
    g_lo.preview = RectF(cfgW, S(40), W - cfgW, H - S(40) - barH);
    g_lo.bar = RectF(0, H - barH, W, barH);
    /* --- 预览区三段 (页面 .preview: phead / textarea(flex) / tpl / status) --- */
    float px = g_lo.preview.X, pw = g_lo.preview.Width;
    g_lo.phead = RectF(px, S(40), pw, S(34));
    float tplH = S(46), statusH = S(25);
    g_lo.status = RectF(px, S(40) + g_lo.preview.Height - statusH, pw, statusH);
    g_lo.tpl = RectF(px, g_lo.status.Y - tplH, pw, tplH);
    g_lo.editor = RectF(px, g_lo.phead.Y + g_lo.phead.Height, pw, g_lo.tpl.Y - (S(40) + S(34)));
    /* --- cfg 内容流 (页面 .cfg padding 12/14; 卡片间 gap 12) ---
     y 为内容坐标 (相对 cfg 内容原点; 绘制/命中经 cfg.Y - cfgScroll 变换) */
    float cw = cfgW - S(28);
    g_lo.contentW = cw;
    float y = S(12);
    float x0 = S(14);
    float xEnd = cfgW - S(14);
    auto ctl = [&](int id, const RectF& rc, int row = -1, int idx = -1) {
        g_ctls.push_back({ id, rc, row, idx, true });
    };
    auto lblW = [&](const std::wstring& t) { return MeasureStr(gi, t, F(S(12), false)); };
    auto newCard = [&](const char* key, const wchar_t* def, RectF* secOut) {
        *secOut = RectF(x0, y, cw, 0);
        y += S(10);            /* sec padding-top */
        /* sec-title (accent 竖条 + 12px 加粗) 占 18px + margin-bottom 8 */
        y += S(18) + S(8);
    };
    auto endCard = [&](RectF* secOut) {
        secOut->Height = y + S(10) - secOut->Y;
        y += S(10) + S(12);    /* sec padding-bottom + 卡片间 gap */
    };

    /* ---- 卡片 1: SELECT 输出 (sec) ---- */
    newCard("SecSelect", L"SELECT 输出", &g_lo.sec1);
    {   /* 模式 row: lbl + 下拉 (gap 8) */
        float lw = lblW(SG("Mode", L"模式"));
        std::vector<std::wstring> modes = { SG("ModeStar", L"SELECT * (全部字段)"), SG("ModeCols", L"指定字段"), SG("ModeAgg", L"聚合统计") };
        float mw = DropW(gi, modes, S(60));
        ctl(CI_SELMODE, RectF(x0 + lw + S(8), y, mw, S(28)));
        y += S(28) + S(7);
    }
    if (g_selMode == 1) {   /* cols-grid: 3 列 (页面 grid repeat(3,1fr) gap 4px 10px) */
        float colW = cw / 3.0f;
        for (int i = 0; i < 12; i++) {
            int r = i / 3, c = i % 3;
            ctl(CI_COLCHK, RectF(x0 + c * colW, y + r * S(24), colW, S(20)), -1, i);
        }
        y += 4 * S(24) + S(7);
    }
    if (g_selMode == 2) {   /* agg: chips (可折行) + 分组 + HAVING (页面 #aggGrid) */
        {   /* chips: lbl 后流式排, 超右缘折行 */
            float cx = x0 + lblW(SG("Agg", L"聚合")) + S(8);
            float cy = y;
            for (int i = 0; i < 6; i++) {
                float w = MeasureStr(gi, AGG_EXPR[i], F(S(11), false)) + S(20);
                if (cx + w > xEnd && cx > x0) { cx = x0 + lblW(SG("Agg", L"聚合")) + S(8); cy += S(26); }
                if (i > 0) ctl(CI_AGGCHIP, RectF(cx, cy, w, S(22)), -1, i);   /* COUNT(*) 固定项不可点 */
                cx += w + S(6);
            }
            y = cy + S(22) + S(7);
        }
        {   /* 分组 row */
            float lw = lblW(SG("GroupBy", L"分组"));
            std::vector<std::wstring> gitems = GroupItems();
            float gw = DropW(gi, gitems, S(70));
            ctl(CI_GROUPBY, RectF(x0 + lw + S(8), y, gw, S(28)));
            y += S(28) + S(7);
        }
        {   /* HAVING row: lbl + 输入 (flex 1) */
            float lw = lblW(L"HAVING");
            ctl(CI_HAVING, RectF(x0 + lw + S(8), y, xEnd - (x0 + lw + S(8)), S(28)));
            y += S(28);
        }
    }
    endCard(&g_lo.sec1);

    /* ---- 卡片 2: WHERE 条件 (sec) ---- */
    newCard("SecWhere", L"WHERE 条件", &g_lo.sec2);
    for (size_t r = 0; r < g_wrows.size(); r++) {
        WRow& w = g_wrows[r];
        float rx = x0;
        if (r != 0) {   /* 首行连接词隐藏 (页面 updateConnSelectors), 布局随之收紧 */
            ctl(CI_WCONN, RectF(rx, y, S(62), S(28)), (int)r);
            rx += S(62) + S(6);
        }
        ctl(CI_WTYPE, RectF(rx, y, S(96), S(28)), (int)r);
        rx += S(96) + S(6);
        /* wctl 控件区 (wctlHtml 各 case; px 值照 CSS) */
        const Opt* ops; int nop = 0;
        ops = OpListOf(w.type, &nop);
        std::vector<std::wstring> opItems = OpItems(ops, nop);
        float ow = nop ? DropW(gi, opItems, S(40)) : 0;
        float flexEnd = xEnd - S(24) - S(6);   /* 预留删除钮 */
        switch (w.type) {
            case WT_FILETYPE: {
                ctl(CI_WOP, RectF(rx, y, ow, S(28)), (int)r); rx += ow + S(6);
                ctl(CI_WV, RectF(rx, y, std::min(std::max(S(60), xEnd - rx), S(120)), S(28)), (int)r);
                break;
            }
            case WT_SIZE: {
                ctl(CI_WOP, RectF(rx, y, ow, S(28)), (int)r); rx += ow + S(6);
                ctl(CI_WV, RectF(rx, y, S(90), S(28)), (int)r); rx += S(90) + S(6);
                float uw = S(52);
                ctl(CI_WUNIT, RectF(rx, y, uw, S(28)), (int)r); rx += uw + S(6);
                if (ops[w.op].val && std::wstring(ops[w.op].val) == L"BETWEEN") {
                    ctl(CI_WV2, RectF(rx, y, S(90), S(28)), (int)r); rx += S(90) + S(6);
                    ctl(CI_WUNIT2, RectF(rx, y, uw, S(28)), (int)r);
                }
                break;
            }
            case WT_PATH: {
                ctl(CI_WOP, RectF(rx, y, ow, S(28)), (int)r); rx += ow + S(6);
                float vw = std::max(S(60), flexEnd - rx - S(84));
                ctl(CI_WV, RectF(rx, y, vw, S(28)), (int)r); rx += vw + S(6);
                ctl(CI_WSUB, RectF(rx, y, flexEnd - rx, S(28)), (int)r);
                break;
            }
            case WT_MTIME: {
                ctl(CI_WOP, RectF(rx, y, ow, S(28)), (int)r); rx += ow + S(6);
                std::wstring ov = ops[w.op].val;
                if (ov == L"preset") {
                    ctl(CI_WV, RectF(rx, y, std::max(S(80), flexEnd - rx), S(28)), (int)r);
                } else if (ov == L"after" || ov == L"before") {
                    ctl(CI_WV2, RectF(rx, y, S(128), S(28)), (int)r);
                } else if (ov == L"between") {
                    ctl(CI_WV2, RectF(rx, y, S(128), S(28)), (int)r); rx += S(128) + S(6);
                    ctl(CI_WV3, RectF(rx, y, S(128), S(28)), (int)r);
                }
                break;
            }
            case WT_ATTR: {
                ctl(CI_WOP, RectF(rx, y, ow, S(28)), (int)r); rx += ow + S(6);
                ctl(CI_WV, RectF(rx, y, S(90), S(28)), (int)r);
                break;
            }
            case WT_ISDIR: {
                ctl(CI_WOP, RectF(rx, y, ow, S(28)), (int)r);
                break;
            }
            case WT_SCORE: case WT_FILENUMBER: {
                ctl(CI_WOP, RectF(rx, y, ow, S(28)), (int)r); rx += ow + S(6);
                ctl(CI_WV, RectF(rx, y, S(90), S(28)), (int)r);
                break;
            }
            default: {   /* ext / fname / content / parentname / parentpath / anyparent / custom: 主值 flex */
                ctl(CI_WOP, RectF(rx, y, ow, S(28)), (int)r); rx += ow + S(6);
                ctl(CI_WV, RectF(rx, y, std::max(S(60), flexEnd - rx), S(28)), (int)r);
                break;
            }
        }
        ctl(CI_WDEL, RectF(xEnd - S(24), y + S(2), S(24), S(24)), (int)r);
        y += S(28) + S(6);
    }
    {   /* + 添加条件 (dashed, mt 2) */
        ctl(CI_ADDCOND, RectF(x0, y + S(2), S(88), S(26)));
        y += S(26) + S(2);
    }
    endCard(&g_lo.sec2);

    /* ---- 卡片 3: 排序与限制 (sec) ---- */
    newCard("SecOrder", L"排序与限制", &g_lo.sec3);
    {   /* 排序 row (6 控件, 超宽折行 — 页面 flex-wrap) */
        float cx = x0;
        auto put = [&](float w, int id) {
            if (cx + w > xEnd && cx > x0) { cx = x0; y += S(28) + S(7); }
            if (id != CI_NONE) ctl(id, RectF(cx, y, w, S(28)));
            cx += w + S(8);
        };
        put(lblW(SG("Order", L"排序")), CI_NONE);
        std::vector<std::wstring> oi = OrderItems();
        float fw = DropW(gi, oi, S(60)), dw = S(64);
        put(fw, CI_ORDERF);
        put(dw, CI_ORDERD);
        put(lblW(SG("Secondary", L"次级")), CI_NONE);
        put(fw, CI_ORDERF2);
        put(dw, CI_ORDERD2);
        y += S(28) + S(7);
    }
    {   /* LIMIT + noSH row */
        float cx = x0;
        cx += lblW(L"LIMIT") + S(8);
        ctl(CI_LIMIT, RectF(cx, y, S(120), S(28)));
        cx += S(120) + S(12);
        g_lo.hintText = SG("NoSH", L"排除系统/隐藏文件 (FAttr !~ '[SH]')");
        float tw = S(18) + lblW(g_lo.hintText);
        if (cx + tw > xEnd && cx > x0) { cx = x0; y += S(28) + S(7); }
        ctl(CI_NOSH, RectF(cx, y, tw, S(20)));
        y += S(28);
    }
    endCard(&g_lo.sec3);
    /* ---- hint (wrap 全宽, 11px) ---- */
    {
        std::wstring hint = SG("Hint", L"搜索词原样交给蜗牛快搜, 自动识别为 SQL 后按 SQL 执行 (无视通配符/正则设置); 匹配文件直接显示在主窗口文件列表。可手动修改右侧预览后执行。");
        float hh = MeasureH(gi, hint, F(S(11), false), cw);
        g_lo.hintRc = RectF(x0, y, cw, hh);
        y += hh + S(12);
    }
    g_cfgContentH = y;
    g_cfgMaxScroll = std::max(0.0f, g_cfgContentH - g_lo.cfg.Height);
    if (g_cfgScroll > g_cfgMaxScroll) g_cfgScroll = g_cfgMaxScroll;
    if (g_cfgScroll < 0) g_cfgScroll = 0;
    /* --- 编辑器滚动范围 (内容变化后钳制) --- */
    float edTextH = (float)g_sqlLines.size() * g_edLineH();
    float edViewH = g_lo.editor.Height - S(24);
    float maxEdY = std::max(0.0f, edTextH - edViewH);
    if (g_edScrollY > maxEdY) g_edScrollY = maxEdY;
    if (g_edScrollY < 0) g_edScrollY = 0;
    float edLineW = 0;
    for (auto& ln : g_sqlLines) edLineW = std::max(edLineW, MeasureStr(gi, ln, FM(S(12), false)));
    float maxEdX = std::max(0.0f, edLineW - (pw - S(28)));
    if (g_edScrollX > maxEdX) g_edScrollX = maxEdX;
    if (g_edScrollX < 0) g_edScrollX = 0;
    /* --- bar 按钮 (btn-main / btn; 视口坐标 rel=false) --- */
    {
        float by = g_lo.bar.Y + S(8);
        float rw = MeasureStr(gi, SG("Run", L"填入搜索框执行"), F(S(13), true)) + S(44);
        g_lo.btnRun = RectF(S(14), by, rw, S(32));
        g_ctls.push_back({ CI_RUN, g_lo.btnRun, -1, -1, false });
        float cw2 = MeasureStr(gi, SG("Copy", L"复制 SQL"), F(S(12), false)) + S(32);
        g_lo.btnCopy = RectF(g_lo.btnRun.X + g_lo.btnRun.Width + S(8), by, cw2, S(32));
        g_ctls.push_back({ CI_COPY, g_lo.btnCopy, -1, -1, false });
    }
    /* 标题栏关闭钮 (页面 .close-btn: 32×32, 右缘 6px; 视口坐标) */
    g_ctls.push_back({ CI_CLOSE, RectF(W - S(6) - S(32), S(4), S(32), S(32)), -1, -1, false });
    /* 模板下拉 (页面 .tpl: padding 8/12, select 宽 100% 高 30; 视口坐标) */
    g_ctls.push_back({ CI_TPL, RectF(g_lo.tpl.X + S(12), g_lo.tpl.Y + S(8), pw - S(24), S(30)), -1, -1, false });
    /* SQL 预览编辑器命中矩形 (整块 textarea 区; 视口坐标) */
    g_ctls.push_back({ CI_EDITOR, g_lo.editor, -1, -1, false });
}

static std::vector<std::wstring> TplItems() {
    std::vector<std::wstring> v;
    for (int i = 0; i < 15; i++) v.push_back(TplName(i));
    return v;
}

/* ==================== 下拉浮层 (自绘 select 的弹出层) ==================== */
static bool g_ddOpen = false;
static int  g_ddSrc = 0, g_ddRow = -1, g_ddIdx = -1;
static RectF g_ddAnchor;
static std::vector<std::wstring> g_ddItems;
static int  g_ddCur = 0, g_ddHover = -1;
static RectF g_ddRc;

static void CloseDropdown() {
    if (!g_ddOpen) return;
    g_ddOpen = false;
    g_ddHover = -1;
    InvalidateAll();
}
static void OpenDropdown(int srcId, int row, const RectF& anchor, const std::vector<std::wstring>& items, int cur) {
    g_ddOpen = true;
    g_ddSrc = srcId; g_ddRow = row; g_ddIdx = -1;
    g_ddAnchor = anchor;
    g_ddItems = items;
    g_ddCur = cur;
    g_ddHover = cur;
    /* 浮层矩形: 锚下方, 宽 = max(锚宽, 最宽项+22), 底部放不下向上翻 (页面原生 select 行为) */
    Graphics gi(g_hwnd);
    float w = anchor.Width;
    for (auto& s : items) w = std::max(w, MeasureStr(gi, s, F(S(12), false)) + S(22));
    float ih = S(26);
    float h = (float)items.size() * ih + S(8);
    RECT crc; GetClientRect(g_hwnd, &crc);
    float x = anchor.X;
    float yy = anchor.Y + anchor.Height + S(2);
    if (yy + h > crc.bottom - S(4)) yy = anchor.Y - h - S(2);
    if (yy < S(4)) yy = S(4);
    if (x + w > crc.right - S(4)) x = crc.right - S(4) - w;
    g_ddRc = RectF(x, yy, w, h);
    InvalidateAll();
}
/* 选中项写回源控件 (open 下拉时的 srcId 路由) */
static void RefreshSql();   /* 前置 (实现在交互动作节) */
static void SetStatus(const std::wstring& s);
static void FeedDropdown(int idx) {
    if (idx < 0 || idx >= (int)g_ddItems.size()) { CloseDropdown(); return; }
    int src = g_ddSrc, row = g_ddRow;
    CloseDropdown();
    switch (src) {
        case CI_SELMODE:  if (g_selMode != idx) { g_selMode = idx; RefreshSql(); } break;
        case CI_GROUPBY:  if (g_groupBy != idx) { g_groupBy = idx; RefreshSql(); } break;
        case CI_WCONN:    if (g_wrows[(size_t)row].conn != idx) { g_wrows[(size_t)row].conn = idx; RefreshSql(); } break;
        case CI_WTYPE:
            if (g_wrows[(size_t)row].type != idx) {
                g_wrows[(size_t)row].type = idx;
                g_wrows[(size_t)row].op = 0;          /* 类型切换重建控件 (页面 innerHTML 重建, 值清空) */
                g_wrows[(size_t)row].v.clear(); g_wrows[(size_t)row].v2.clear(); g_wrows[(size_t)row].v3.clear();
                g_wrows[(size_t)row].preset = 2;
                g_wrows[(size_t)row].unit = 2; g_wrows[(size_t)row].unit2 = 2;
                g_wrows[(size_t)row].sub = true;
                /* 页面 value 属性 = 真实初始值 (DOM input 初始就有, 生成 SQL 直接读) */
                switch (idx) {
                    case WT_SIZE: g_wrows[(size_t)row].v = L"100"; break;
                    case WT_ATTR: g_wrows[(size_t)row].v = L"[SH]"; break;
                    case WT_SCORE: g_wrows[(size_t)row].v = L"80"; break;
                }
                if (g_focusId == CI_WV && g_focusRow == row) { g_focusId = 0; g_focusRow = -1; }
                RefreshSql();
            }
            break;
        case CI_WOP:      if (g_wrows[(size_t)row].op != idx) { g_wrows[(size_t)row].op = idx; RefreshSql(); } break;
        case CI_WV:
            if (g_wrows[(size_t)row].type == WT_FILETYPE) {   /* 文件类型下拉 */
                if (g_wrows[(size_t)row].v != FileTypeLabel(idx)) { g_wrows[(size_t)row].v = FileTypeLabel(idx); RefreshSql(); }
            } else if (g_wrows[(size_t)row].type == WT_MTIME) {
                if (g_wrows[(size_t)row].preset != idx) { g_wrows[(size_t)row].preset = idx; RefreshSql(); }
            }
            break;
        case CI_WUNIT:    if (g_wrows[(size_t)row].unit != idx) { g_wrows[(size_t)row].unit = idx; RefreshSql(); } break;
        case CI_WUNIT2:   if (g_wrows[(size_t)row].unit2 != idx) { g_wrows[(size_t)row].unit2 = idx; RefreshSql(); } break;
        case CI_ORDERF:   if (g_orderField != idx) { g_orderField = idx; RefreshSql(); } break;
        case CI_ORDERD:   if (g_orderDir != idx) { g_orderDir = idx; RefreshSql(); } break;
        case CI_ORDERF2:  if (g_orderField2 != idx) { g_orderField2 = idx; RefreshSql(); } break;
        case CI_ORDERD2:  if (g_orderDir2 != idx) { g_orderDir2 = idx; RefreshSql(); } break;
        case CI_TPL: {
            /* 模板 = 直接覆盖预览文本 (页面 tplSel change: preview.value = sql; 下拉回显复位) */
            g_sqlText = TEMPLATES[idx].sql;
            SqlSplit();
            g_caretLine = g_caretCol = 0; g_selLine = g_selCol = -1;
            std::wstring st = SG("StatusTpl", L"已应用模板: {0}");
            size_t p = st.find(L"{0}");
            if (p != std::wstring::npos) st.replace(p, 3, TplName(idx));
            g_status = st;
            InvalidateAll();
            break;
        }
    }
}
static std::wstring SelectTextOf(int srcId, int row) {
    switch (srcId) {
        case CI_SELMODE: {
            static const char* MK[3] = { "ModeStar", "ModeCols", "ModeAgg" };
            static const wchar_t* MD[3] = { L"SELECT * (全部字段)", L"指定字段", L"聚合统计" };
            return SG(MK[g_selMode], MD[g_selMode]);
        }
        case CI_GROUPBY: return GroupLabel(g_groupBy);
        case CI_WCONN:   return row >= 0 ? (g_wrows[(size_t)row].conn ? L"OR" : L"AND") : L"";
        case CI_WTYPE:   return row >= 0 ? WTypeName(g_wrows[(size_t)row].type) : L"";
        case CI_WOP: {
            if (row < 0) return L"";
            const Opt* ops; int n = 0;
            ops = OpListOf(g_wrows[(size_t)row].type, &n);
            return (ops && g_wrows[(size_t)row].op < n) ? OptText(ops[g_wrows[(size_t)row].op]) : L"";
        }
        case CI_WV:
            if (row < 0) return L"";
            if (g_wrows[(size_t)row].type == WT_FILETYPE) {
                int sel = 0;
                for (int i = 0; i < 9; i++) if (FileTypeLabel(i) == g_wrows[(size_t)row].v) { sel = i; break; }
                return FileTypeLabel(sel);
            }
            if (g_wrows[(size_t)row].type == WT_MTIME) return TimePresetName(g_wrows[(size_t)row].preset);
            return g_wrows[(size_t)row].v;
        case CI_WUNIT:  return row >= 0 ? std::wstring(UnitItems()[g_wrows[(size_t)row].unit]) : L"";
        case CI_WUNIT2: return row >= 0 ? std::wstring(UnitItems()[g_wrows[(size_t)row].unit2]) : L"";
        case CI_ORDERF:  return OrderFieldLabel(g_orderField);
        case CI_ORDERD:  return g_orderDir ? SG("Asc", L"升序") : SG("Desc", L"降序");
        case CI_ORDERF2: return OrderFieldLabel(g_orderField2);
        case CI_ORDERD2: return g_orderDir2 ? SG("Asc", L"升序") : SG("Desc", L"降序");
        case CI_TPL:     return SG("TplPlaceholder", L"-- 常用模板 --");
    }
    return L"";
}
/* 展开某下拉 (按命中控件路由选项表) */
static void ExpandDropdown(const CtlRect& c) {
    switch (c.id) {
        case CI_SELMODE:
            OpenDropdown(c.id, c.row, c.rc, { SG("ModeStar", L"SELECT * (全部字段)"), SG("ModeCols", L"指定字段"), SG("ModeAgg", L"聚合统计") }, g_selMode);
            break;
        case CI_GROUPBY:
            OpenDropdown(c.id, c.row, c.rc, GroupItems(), g_groupBy);
            break;
        case CI_WCONN:
            OpenDropdown(c.id, c.row, c.rc, { L"AND", L"OR" }, g_wrows[(size_t)c.row].conn);
            break;
        case CI_WTYPE: {
            std::vector<std::wstring> tn;
            for (int t = 0; t < WT_N; t++) tn.push_back(WTypeName(t));
            OpenDropdown(c.id, c.row, c.rc, tn, g_wrows[(size_t)c.row].type);
            break;
        }
        case CI_WOP: {
            const Opt* ops; int n = 0;
            ops = OpListOf(g_wrows[(size_t)c.row].type, &n);
            OpenDropdown(c.id, c.row, c.rc, OpItems(ops, n), g_wrows[(size_t)c.row].op);
            break;
        }
        case CI_WV:
            if (g_wrows[(size_t)c.row].type == WT_FILETYPE) {
                std::vector<std::wstring> ft = FTItems();
                int sel = 0;
                for (int i = 0; i < 9; i++) if (FileTypeLabel(i) == g_wrows[(size_t)c.row].v) { sel = i; break; }
                OpenDropdown(c.id, c.row, c.rc, ft, sel);
            } else if (g_wrows[(size_t)c.row].type == WT_MTIME) {
                OpenDropdown(c.id, c.row, c.rc, PresetItems(), g_wrows[(size_t)c.row].preset);
            }
            break;
        case CI_WUNIT:  OpenDropdown(c.id, c.row, c.rc, UnitItems(), g_wrows[(size_t)c.row].unit); break;
        case CI_WUNIT2: OpenDropdown(c.id, c.row, c.rc, UnitItems(), g_wrows[(size_t)c.row].unit2); break;
        case CI_ORDERF:  OpenDropdown(c.id, c.row, c.rc, OrderItems(), g_orderField); break;
        case CI_ORDERD:  OpenDropdown(c.id, c.row, c.rc, { SG("Desc", L"降序"), SG("Asc", L"升序") }, g_orderDir); break;
        case CI_ORDERF2: OpenDropdown(c.id, c.row, c.rc, OrderItems(), g_orderField2); break;
        case CI_ORDERD2: OpenDropdown(c.id, c.row, c.rc, { SG("Desc", L"降序"), SG("Asc", L"升序") }, g_orderDir2); break;
        case CI_TPL:     OpenDropdown(c.id, c.row, c.rc, TplItems(), -1); break;
    }
}

/* ==================== 交互动作 ==================== */
static void SetStatus(const std::wstring& s) { g_status = s; }
static void RefreshSql() {
    g_sqlText = BuildSQL();
    SqlSplit();
    if (g_caretLine >= (int)g_sqlLines.size()) g_caretLine = (int)g_sqlLines.size() - 1;
    if (g_caretCol > (int)g_sqlLines[(size_t)g_caretLine].size()) g_caretCol = (int)g_sqlLines[(size_t)g_caretLine].size();
    InvalidateAll();
}
static void AddRow() {
    WRow w;   /* 默认 = 文件类型 AND (= 页面 wrowHtml('AND','filetype')) */
    g_wrows.push_back(w);
    ComputeLayout();
    RefreshSql();
}
static void DelRow(int row) {
    if (row < 0 || row >= (int)g_wrows.size()) return;
    g_wrows.erase(g_wrows.begin() + row);
    if (g_focusRow == row) { g_focusId = 0; g_focusRow = -1; }
    ComputeLayout();
    RefreshSql();
}
/* 填入搜索框执行 (页面 btnRun → xjs.searchSql: SearchSetText mode=sql execute=1;
   owner 已关回落主窗 — owner 悬垂校验回落口径) */
static void DoRun() {
    std::wstring sql = TrimW(g_sqlText);
    if (sql.empty()) { SetStatus(SG("StatusEmpty", L"SQL 为空")); InvalidateAll(); return; }
    if (g_host) {
        std::string sql8 = U8(sql);
        int e = g_host->SearchSetText(g_ctx, g_ownerToken, sql8.c_str(), "sql", 1);
        if (e == XJS_PLUGIN_ERR_NOTFOUND)
            e = g_host->SearchSetText(g_ctx, 0, sql8.c_str(), "sql", 1);
        if (e == XJS_PLUGIN_OK)
            SetStatus(SG("StatusRun", L"已填入主窗口搜索框并执行 (蜗牛快搜自动识别 SQL)"));
        /* 失败 (发起窗失效等) 不改状态栏 — 下次成功路径再刷新 */
    }
    InvalidateAll();
}
/* 复制 SQL (页面 btnCopy; Win32 剪贴板, 与宿主一致) */
static void DoCopy() {
    std::wstring sql = TrimW(g_sqlText);
    if (sql.empty()) { SetStatus(SG("StatusEmpty", L"SQL 为空")); InvalidateAll(); return; }
    if (OpenClipboard(g_hwnd)) {
        EmptyClipboard();
        size_t bytes = (sql.size() + 1) * sizeof(wchar_t);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (h) {
            memcpy(GlobalLock(h), sql.c_str(), bytes);
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
        }
        CloseClipboard();
    }
    SetStatus(SG("StatusCopied", L"SQL 已复制到剪贴板"));
    InvalidateAll();
}

/* ==================== 单行输入 (绑定 WRow 字段/全局值; 值指针每次现取防 vector 重分配悬垂) ==================== */
static int g_lineCaret = 0, g_lineSelA = -1;
static float g_lineScroll = 0;
static std::wstring* BoundStr(int id, int row) {
    switch (id) {
        case CI_HAVING: return &g_having;
        case CI_LIMIT:  return &g_limit;
        case CI_WV:
            if (row < 0 || row >= (int)g_wrows.size()) return NULL;
            if (g_wrows[(size_t)row].type == WT_FILETYPE || g_wrows[(size_t)row].type == WT_MTIME) return NULL;   /* 下拉型 */
            return &g_wrows[(size_t)row].v;
        case CI_WV2: return (row >= 0 && row < (int)g_wrows.size()) ? &g_wrows[(size_t)row].v2 : NULL;
        case CI_WV3: return (row >= 0 && row < (int)g_wrows.size()) ? &g_wrows[(size_t)row].v3 : NULL;
    }
    return NULL;
}
/* 输入过滤 (页面 number/date input 的字符域): 数字型 / 日期型 (YYYY-MM-DD) / 自由文本 */
static bool LineFilterNum(int id, int row) {
    if (id == CI_LIMIT) return true;
    if (row < 0 || row >= (int)g_wrows.size()) return false;
    WRow& w = g_wrows[(size_t)row];
    if (w.type == WT_SIZE) return id == CI_WV || id == CI_WV2;
    if (w.type == WT_SCORE || w.type == WT_FILENUMBER) return id == CI_WV;
    return false;
}
static bool LineFilterDate(int id, int row) {
    if (row < 0 || row >= (int)g_wrows.size()) return false;
    if (g_wrows[(size_t)row].type != WT_MTIME) return false;
    return id == CI_WV2 || id == CI_WV3;
}
static std::wstring LinePlaceholder(int id, int row) {
    switch (id) {
        case CI_HAVING: return SG("HavingPh", L"如 COUNT(*) > 10 (可选)");
        case CI_LIMIT:  return SG("LimitPh", L"最多行数 (可选)");
        case CI_WV2: case CI_WV3:
            if (LineFilterDate(id, row)) return L"YYYY-MM-DD";
            return SG("And", L"和");
        case CI_WV: {
            if (row < 0 || row >= (int)g_wrows.size()) return L"";
            switch (g_wrows[(size_t)row].type) {
                case WT_PATH:       return SG("PhPath", L"如 D:\\Work");
                case WT_EXT:        return SG("PhExt", L"如 mp4,mkv,avi");
                case WT_FNAME:      return SG("PhKw", L"关键词或正则");
                case WT_CONTENT:    return SG("PhContent", L"正则表达式 (对文件内容匹配)");
                case WT_PARENTNAME: return SG("PhParentName", L"如 纪录片");
                case WT_PARENTPATH: return SG("PhParentPath", L"如 D:\\Work\\视频");
                case WT_ANYPARENT:  return SG("PhAnyParent", L"如 电影");
                case WT_FILENUMBER: return SG("PhFileNumber", L"如 100");
                case WT_CUSTOM:     return SG("PhCustom", L"直接写表达式, 如 Size > 100 AND Ext = 'txt'");
            }
            return L"";
        }
    }
    return L"";
}
/* 单行值占位显示 (size wv 默认 '100', attr '[SH]', score '80' — 页面 value 属性, 是真实初始值) */
static std::wstring LineInitValue(int id, int row) {
    if (id == CI_WV && row >= 0 && row < (int)g_wrows.size()) {
        switch (g_wrows[(size_t)row].type) {
            case WT_SIZE: return L"100";
            case WT_ATTR: return L"[SH]";
            case WT_SCORE: return L"80";
        }
    }
    return L"";
}

/* ==================== 绘制 ==================== */
static void DrawSelect(Gdiplus::Graphics& g, const RectF& rc, const std::wstring& text, bool hot, bool focus) {
    FillRc(g, rc, Br(g_sk.panel2), S(7));
    StrokeRc(g, rc, PenP(focus ? g_sk.accent : g_sk.border, 255, 1.0f), S(7));
    DrawStr(g, text, F(S(12), false), RectF(rc.X + S(8), rc.Y, rc.Width - S(24), rc.Height), Br(g_sk.text), 0, 1);
    /* 下拉箭头 ▾ (select 右侧) */
    float ax = rc.X + rc.Width - S(14), ay = rc.Y + rc.Height / 2;
    Gdiplus::Pen ap(Gdiplus::Color(255, GetRValue(g_sk.dim), GetGValue(g_sk.dim), GetBValue(g_sk.dim)), S(1.3f));
    PointF pts[3] = { PointF(ax - S(3.5f), ay - S(2)), PointF(ax + S(3.5f), ay - S(2)), PointF(ax, ay + S(2)) };
    g.DrawLines(&ap, pts, 3);
}
static void DrawInput(Gdiplus::Graphics& g, const RectF& rc, const std::wstring& text, const std::wstring& ph, bool focus, int caret, int selA, float* scroll) {
    FillRc(g, rc, Br(g_sk.panel2), S(7));
    StrokeRc(g, rc, PenP(focus ? g_sk.accent : g_sk.border, 255, 1.0f), S(7));
    RectF tr(rc.X + S(8), rc.Y, rc.Width - S(16), rc.Height);
    if (text.empty() && !focus) {
        if (!ph.empty()) DrawStr(g, ph, F(S(12), false), tr, Br(g_sk.faint), 0, 1);
        return;
    }
    Graphics gi(g_hwnd);
    float tw = MeasureStr(gi, text, F(S(12), false));
    auto colX = [&](int n) { return n <= 0 ? 0.0f : MeasureStr(gi, text.substr(0, n), F(S(12), false)); };
    float caretX = 0;
    if (focus) caretX = colX(caret);   /* 单行光标/选区宽 = UI 字体 (12px) 前缀实测 */
    if (caretX - *scroll > tr.Width - S(4)) *scroll = caretX - tr.Width + S(4);
    if (caretX - *scroll < S(4)) *scroll = caretX - S(4);
    if (*scroll < 0) *scroll = 0;
    if (*scroll > tw) *scroll = tw;
    Gdiplus::GraphicsState st = g.Save();
    g.SetClip(tr);
    if (focus && selA >= 0 && selA != caret) {
        int a = selA < caret ? selA : caret, b = selA < caret ? caret : selA;
        float xa = colX(a), xb = colX(b);
        g.FillRectangle(Br(g_sk.accent, 70), RectF(tr.X + xa - *scroll, tr.Y + S(4), xb - xa, tr.Height - S(8)));
    }
    DrawStr(g, text, F(S(12), false), RectF(tr.X - *scroll, tr.Y, tw + S(40), tr.Height), Br(g_sk.text), 0, 1);
    if (focus && g_caretOn)
        g.FillRectangle(Br(g_sk.text), RectF(tr.X + caretX - *scroll, tr.Y + S(5), std::max(1.0f, S(1)), tr.Height - S(10)));
    g.Restore(st);
}
static void DrawCheckbox(Gdiplus::Graphics& g, const RectF& rc, bool checked, const std::wstring& label, bool hot) {
    float bx = rc.X, by = rc.Y + (rc.Height - S(13)) / 2;
    RectF box(bx, by, S(13), S(13));
    FillRc(g, box, Br(checked ? g_sk.accent : g_sk.panel2), S(3));
    StrokeRc(g, box, PenP(checked ? g_sk.accent : g_sk.border, 255, 1.0f), S(3));
    if (checked) {   /* 白勾 (accent-color: accent 同款观感) */
        Gdiplus::Pen cp(Gdiplus::Color(255, 255, 255, 255), S(1.8f));
        cp.SetStartCap(Gdiplus::LineCapRound); cp.SetEndCap(Gdiplus::LineCapRound);
        float u = S(13) / 13.0f;
        PointF pts[3] = { PointF(box.X + 3 * u, box.Y + 7 * u), PointF(box.X + 5.5f * u, box.Y + 9.5f * u), PointF(box.X + 10 * u, box.Y + 3.5f * u) };
        g.DrawLines(&cp, pts, 3);
    }
    DrawStr(g, label, F(S(12), false), RectF(bx + S(18), rc.Y, rc.Width - S(18), rc.Height), Br(hot ? g_sk.text : g_sk.text), 0, 1);
}
static void DrawTitlebar(Gdiplus::Graphics& g) {
    g.FillRectangle(Br(g_sk.panel), g_lo.titlebar);
    DrawStr(g, SG("Title", L"SQL 生成器"), F(S(13), true),
            RectF(S(16), 0, S(200), S(40)), Br(g_sk.text), 0, 1);
    /* tag 徽章 (页面 .titlebar .tag) */
    std::wstring tag = L"蜗牛快搜 SQL";
    float tw = MeasureStr(g, tag, F(S(11), false)) + S(16);
    RectF tagRc(S(16) + MeasureStr(g, SG("Title", L"SQL 生成器"), F(S(13), true)) + S(12), S(40) / 2 - S(10), tw, S(20));
    FillRc(g, tagRc, Br(g_sk.panel2), S(10));
    StrokeRc(g, tagRc, PenP(g_sk.border, 255, 1.0f), S(10));
    DrawStr(g, tag, F(S(11), false), tagRc, Br(g_sk.faint), 1, 1);
    /* 关闭钮 (hover 红 #e0442e / 白 ✕, 页面 .close-btn) */
    const CtlRect* cc = CtlOf(CI_CLOSE);
    if (cc) {
        bool hot = g_hotId == CI_CLOSE;
        if (hot) FillRc(g, cc->rc, Br(g_sk.danger), S(8));
        COLORREF ic = hot ? RGB(255, 255, 255) : g_sk.dim;
        Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(ic), GetGValue(ic), GetBValue(ic)), S(1.5f));
        pen.SetStartCap(Gdiplus::LineCapRound); pen.SetEndCap(Gdiplus::LineCapRound);
        float u = S(12) / 12.0f;
        PointF a(cc->rc.X + (cc->rc.Width - S(12)) / 2 + 3 * u, cc->rc.Y + (cc->rc.Height - S(12)) / 2 + 3 * u);
        g.DrawLine(&pen, a.X, a.Y, a.X + 6 * u, a.Y + 6 * u);
        g.DrawLine(&pen, a.X + 6 * u, a.Y, a.X, a.Y + 6 * u);
    }
    g.FillRectangle(Br(g_sk.border, 255), RectF(0, S(40) - 1, g_lo.titlebar.Width, 1));
}
static void DrawSecTitle(Gdiplus::Graphics& g, const RectF& sec, float y, const std::wstring& text) {
    RectF bar(sec.X + S(12), y, S(3), S(12));
    FillRc(g, bar, Br(g_sk.accent), S(2));
    DrawStr(g, text, F(S(12), true), RectF(sec.X + S(12) + S(3) + S(6), y - S(3), sec.Width - S(40), S(18)), Br(g_sk.dim), 0, 1);
}
static void DrawCfg(Gdiplus::Graphics& g) {
    /* 背景已由整帧渐变出 (页面 body 渐变); 卡片区带滚动裁剪。
       cfg 控件表 = 内容坐标: 绘制统一 + (cfg.Y - cfgScroll) 变换 (与命中 CtlViewRc 同式) */
    Gdiplus::GraphicsState st = g.Save();
    g.SetClip(g_lo.cfg);
    float oy = g_lo.cfg.Y - g_cfgScroll;   /* 内容 → 视口偏移 */
    auto sec = [&](const RectF& r) {
        RectF rc(r.X, r.Y + oy, r.Width, r.Height);
        FillRc(g, rc, Br(g_sk.panel), S(8));
        StrokeRc(g, rc, PenP(g_sk.border, 255, 1.0f), S(8));
    };
    sec(g_lo.sec1);
    sec(g_lo.sec2);
    sec(g_lo.sec3);
    /* 标题 */
    DrawSecTitle(g, g_lo.sec1, g_lo.sec1.Y + oy + S(10), SG("SecSelect", L"SELECT 输出"));
    DrawSecTitle(g, g_lo.sec2, g_lo.sec2.Y + oy + S(10), SG("SecWhere", L"WHERE 条件"));
    DrawSecTitle(g, g_lo.sec3, g_lo.sec3.Y + oy + S(10), SG("SecOrder", L"排序与限制"));
    /* 控件 (统一按 g_ctls + oy 绘制; select/input/chip/checkbox 各自识别) */
    Graphics gi(g_hwnd);
    for (auto& c : g_ctls) {
        if (c.id == CI_CLOSE || c.id == CI_RUN || c.id == CI_COPY || c.id == CI_TPL || c.id == CI_EDITOR) continue;
        RectF rc(c.rc.X, c.rc.Y + oy, c.rc.Width, c.rc.Height);
        if (rc.Y + rc.Height < g_lo.cfg.Y - S(4) || rc.Y > g_lo.cfg.Y + g_lo.cfg.Height + S(4)) continue;   /* 视口外 */
        bool hot = g_hotId == c.id && g_hotRow == c.row && !g_ddOpen;
        bool focus = g_focusId == c.id && g_focusRow == c.row;
        switch (c.id) {
            case CI_SELMODE: DrawSelect(g, rc, SelectTextOf(c.id, c.row), hot, focus); break;
            case CI_GROUPBY: DrawSelect(g, rc, SelectTextOf(c.id, c.row), hot, focus); break;
            case CI_WCONN: case CI_WTYPE: case CI_WOP: case CI_WUNIT: case CI_WUNIT2:
                DrawSelect(g, rc, SelectTextOf(c.id, c.row), hot, focus); break;
            case CI_ORDERF: case CI_ORDERD: case CI_ORDERF2: case CI_ORDERD2:
                DrawSelect(g, rc, SelectTextOf(c.id, c.row), hot, focus); break;
            case CI_WV: {
                if (c.row >= 0 && (g_wrows[(size_t)c.row].type == WT_FILETYPE || g_wrows[(size_t)c.row].type == WT_MTIME)) {
                    DrawSelect(g, rc, SelectTextOf(c.id, c.row), hot, focus);
                    break;
                }
                std::wstring v = g_wrows[(size_t)c.row].v;
                if (v.empty() && !focus) {
                    std::wstring iv = LineInitValue(c.id, c.row);
                    if (!iv.empty()) v = iv;
                }
                DrawInput(g, rc, v, LinePlaceholder(c.id, c.row), focus,
                          focus ? g_lineCaret : 0, focus ? g_lineSelA : -1, &g_lineScroll);
                break;
            }
            case CI_WV2: case CI_WV3: {
                std::wstring v = g_wrows[(size_t)c.row].v2;
                if (c.id == CI_WV3) v = g_wrows[(size_t)c.row].v3;
                DrawInput(g, rc, v, LinePlaceholder(c.id, c.row), focus,
                          focus ? g_lineCaret : 0, focus ? g_lineSelA : -1, &g_lineScroll);
                break;
            }
            case CI_HAVING: case CI_LIMIT:
                DrawInput(g, rc, *BoundStr(c.id, c.row), LinePlaceholder(c.id, c.row), focus,
                          focus ? g_lineCaret : 0, focus ? g_lineSelA : -1, &g_lineScroll);
                break;
            case CI_COLCHK:
                DrawCheckbox(g, rc, g_colChk[c.idx], COL_NAMES[c.idx], hot);
                break;
            case CI_NOSH:
                DrawCheckbox(g, rc, g_noSH, g_lo.hintText, hot);
                break;
            case CI_WSUB:
                DrawCheckbox(g, RectF(rc.X, rc.Y, rc.Width, S(20)), g_wrows[(size_t)c.row].sub,
                             SG("IncSub", L"含子目录"), hot);
                break;
            case CI_AGGCHIP: {
                bool on = g_aggChip[c.idx];
                FillRc(g, rc, Br(on ? g_sk.accent : g_sk.panel2, on ? 46 : 255), S(14));
                StrokeRc(g, rc, PenP(on ? g_sk.accent : g_sk.border, 255, 1.0f), S(14));
                DrawStr(g, AGG_EXPR[c.idx], F(S(11), false), rc, Br(on ? g_sk.accent : g_sk.dim), 1, 1);
                break;
            }
            case CI_WDEL: {
                if (hot) FillRc(g, rc, Br(g_sk.danger, 38), S(6));
                COLORREF ic = hot ? g_sk.danger : g_sk.faint;
                DrawStr(g, L"×", F(S(14), false), rc, Br(ic), 1, 1);
                break;
            }
            case CI_ADDCOND: {
                /* dashed 边框 (页面 border: 1px dashed var(--border-strong)) */
                Gdiplus::Pen dp(Gdiplus::Color(255, GetRValue(hot ? g_sk.accent : g_sk.borderStrong),
                                                GetGValue(hot ? g_sk.accent : g_sk.borderStrong),
                                                GetBValue(hot ? g_sk.accent : g_sk.borderStrong)), 1.0f);
                dp.SetDashStyle(Gdiplus::DashStyleDash);
                Gdiplus::GraphicsPath* p = RoundPath(rc, S(7));
                g.DrawPath(&dp, p);
                delete p;
                DrawStr(g, SG("AddCond", L"+ 添加条件"), F(S(12), false), rc, Br(hot ? g_sk.accent : g_sk.dim), 1, 1);
                break;
            }
        }
    }
    /* hint 文本 */
    DrawStr(g, SG("Hint", L"搜索词原样交给蜗牛快搜, 自动识别为 SQL 后按 SQL 执行 (无视通配符/正则设置); 匹配文件直接显示在主窗口文件列表。可手动修改右侧预览后执行。"),
            F(S(11), false), RectF(g_lo.hintRc.X, g_lo.hintRc.Y + oy, g_lo.hintRc.Width, g_lo.hintRc.Height), Br(g_sk.faint), 0, 0, true);
    g.Restore(st);
    /* cfg 滚动条 (页面 ::-webkit-scrollbar 10px thumb rgba(128,128,128,.4)) */
    if (g_cfgMaxScroll > 0 && g_lo.cfg.Height > S(20)) {
        float trackH = g_lo.cfg.Height - S(4);
        float thumbH = std::max(S(24), trackH * g_lo.cfg.Height / std::max(g_cfgContentH, 1.0f));
        float ty = g_lo.cfg.Y + S(2) + (trackH - thumbH) * (g_cfgScroll / g_cfgMaxScroll);
        float tx = g_lo.cfg.Width - S(10);
        FillRc(g, RectF(tx, ty, S(6), thumbH), Br(g_sk.dim, 100), S(3));
    }
}
static float pw_of();
static void DrawPreview(Gdiplus::Graphics& g) {
    /* 预览面板 (页面 .preview: panel 底 + 左缘 1px border) */
    g.FillRectangle(Br(g_sk.panel), g_lo.preview);
    g.FillRectangle(Br(g_sk.border), RectF(g_lo.preview.X, g_lo.preview.Y, 1, g_lo.preview.Height));
    /* phead */
    g.FillRectangle(Br(g_sk.border), RectF(g_lo.preview.X, g_lo.phead.Y + g_lo.phead.Height - 1, pw_of(), 1));
    DrawStr(g, SG("PreviewTitle", L"SQL 预览 (可编辑)"), F(S(12), true),
            RectF(g_lo.preview.X + S(12), g_lo.phead.Y, g_lo.phead.Width - S(24), g_lo.phead.Height), Br(g_sk.dim), 0, 1);
    /* 编辑器 (textarea: padding 12/14, Consolas 12px, lh 1.7, pre) */
    {
        Gdiplus::GraphicsState st = g.Save();
        g.SetClip(g_lo.editor);
        float tx = g_lo.editor.X + S(14) - g_edScrollX;
        float ty = g_lo.editor.Y + S(12) - g_edScrollY;
        float lh = g_edLineH();
        /* 选区底 (焦点在编辑器才有浏览器选区观感; 用 accent 25%) */
        int a1 = g_selLine, a2 = g_selCol, b1 = g_caretLine, b2 = g_caretCol;
        if (a1 > b1 || (a1 == b1 && a2 > b2)) { std::swap(a1, b1); std::swap(a2, b2); }
        Graphics gi(g_hwnd);
        if (g_selLine >= 0 && (a1 != b1 || a2 != b2)) {
            for (int ln = a1; ln <= b1 && ln < (int)g_sqlLines.size(); ln++) {
                float lx = tx + (ln == a1 ? EdColX(gi, g_sqlLines[(size_t)ln], (ln == a1) ? a2 : 0) : 0);
                float rx = tx + EdColX(gi, g_sqlLines[(size_t)ln], (ln == b1) ? b2 : (int)g_sqlLines[(size_t)ln].size());
                g.FillRectangle(Br(g_sk.accent, 64), RectF(lx, ty + ln * lh, std::max(rx - lx, S(1)), lh));
            }
        }
        /* 文本行 */
        for (int ln = 0; ln < (int)g_sqlLines.size(); ln++) {
            float yy = ty + ln * lh;
            if (yy + lh < g_lo.editor.Y || yy > g_lo.editor.Y + g_lo.editor.Height) continue;
            DrawStr(g, g_sqlLines[(size_t)ln], FM(S(12), false),
                    RectF(tx, yy, g_lo.editor.Width - S(28) + g_edScrollX, lh), Br(g_sk.text), 0, 1, false, false);
        }
        /* 光标 */
        if (g_edFocus && g_caretOn && g_caretLine < (int)g_sqlLines.size()) {
            float cx = tx + EdColX(gi, g_sqlLines[(size_t)g_caretLine], g_caretCol);
            g.FillRectangle(Br(g_sk.text), RectF(cx, ty + g_caretLine * lh + S(2), std::max(1.0f, S(1)), lh - S(4)));
        }
        g.Restore(st);
        /* 编辑器滚动条 (纵向) */
        float edTextH = (float)g_sqlLines.size() * lh;
        float edViewH = g_lo.editor.Height - S(24);
        if (edTextH > edViewH && edViewH > S(20)) {
            float trackH = g_lo.editor.Height - S(4);
            float thumbH = std::max(S(24), trackH * edViewH / edTextH);
            float tyb = g_lo.editor.Y + S(2) + (trackH - thumbH) * (g_edScrollY / (edTextH - edViewH));
            FillRc(g, RectF(g_lo.editor.X + g_lo.editor.Width - S(8), tyb, S(6), thumbH), Br(g_sk.dim, 100), S(3));
        }
    }
    /* 模板下拉 (顶 border-top + select) */
    g.FillRectangle(Br(g_sk.border), RectF(g_lo.preview.X, g_lo.tpl.Y, g_lo.tpl.Width, 1));
    if (const CtlRect* tc = CtlOf(CI_TPL))
        DrawSelect(g, RectF(tc->rc.X, tc->rc.Y, tc->rc.Width, tc->rc.Height),
                   SelectTextOf(CI_TPL, -1), g_hotId == CI_TPL, false);
    /* 状态栏 (顶 border-top, 11px faint) */
    g.FillRectangle(Br(g_sk.border), RectF(g_lo.preview.X, g_lo.status.Y, g_lo.status.Width, 1));
    DrawStr(g, g_status, F(S(11), false),
            RectF(g_lo.preview.X + S(14), g_lo.status.Y + S(6), g_lo.status.Width - S(28), g_lo.status.Height - S(6)), Br(g_sk.faint), 0, 1);
}
static float pw_of() { return g_lo.phead.Width; }
static void DrawBar(Gdiplus::Graphics& g) {
    g.FillRectangle(Br(g_sk.panel), g_lo.bar);
    g.FillRectangle(Br(g_sk.border), RectF(0, g_lo.bar.Y, g_lo.bar.Width, 1));
    /* btn-main (accent 底白字; 按压 18% 黑叠层 = opacity .8) */
    {
        bool hot = g_hotId == CI_RUN;
        bool press = g_pressId == CI_RUN;
        FillRc(g, g_lo.btnRun, Br(g_sk.accent), S(8));
        if (press) FillRc(g, g_lo.btnRun, Br(RGB(0, 0, 0), 46), S(8));
        DrawStr(g, SG("Run", L"填入搜索框执行"), F(S(13), true), g_lo.btnRun, Br(RGB(255, 255, 255)), 1, 1);
        (void)hot;
    }
    /* btn (panel-2 底 border 边框 dim 12px; hover text 字 border-strong 边框) */
    {
        bool hot = g_hotId == CI_COPY;
        bool press = g_pressId == CI_COPY;
        FillRc(g, g_lo.btnCopy, Br(g_sk.panel2), S(8));
        if (press) FillRc(g, g_lo.btnCopy, Br(RGB(0, 0, 0), 26), S(8));
        StrokeRc(g, g_lo.btnCopy, PenP(hot ? g_sk.borderStrong : g_sk.border, 255, 1.0f), S(8));
        DrawStr(g, SG("Copy", L"复制 SQL"), F(S(12), false), g_lo.btnCopy, Br(hot ? g_sk.text : g_sk.dim), 1, 1);
    }
    /* tip (右下角 footer) */
    DrawStr(g, SG("FooterTip", L"SQL 生成器 · 蜗牛快搜 SQL 搜索"), F(S(11), false),
            RectF(g_lo.bar.Width - S(320), g_lo.bar.Y, S(306), g_lo.bar.Height), Br(g_sk.faint), 2, 1);
}
static void DrawDropdown(Gdiplus::Graphics& g) {
    if (!g_ddOpen) return;
    Gdiplus::SolidBrush shadow(Gdiplus::Color(70, 0, 0, 0));
    FillRc(g, RectF(g_ddRc.X + S(3), g_ddRc.Y + S(4), g_ddRc.Width, g_ddRc.Height), &shadow, S(10));
    FillRc(g, g_ddRc, Br(g_sk.panel, 248), S(10));
    StrokeRc(g, g_ddRc, PenP(g_sk.accent, 64, 1.0f), S(10));
    float ih = S(26);
    float y = g_ddRc.Y + S(4);
    for (int i = 0; i < (int)g_ddItems.size(); i++) {
        RectF ir(g_ddRc.X + S(4), y, g_ddRc.Width - S(8), ih);
        if (i == g_ddHover) FillRc(g, ir, Br(g_sk.hoverBg), S(6));
        DrawStr(g, g_ddItems[(size_t)i], F(S(12), false),
                RectF(ir.X + S(8), ir.Y, ir.Width - S(16), ih), Br(i == g_ddHover ? g_sk.accent : g_sk.text), 0, 1);
        y += ih;
    }
}
static void DrawFrame(HDC dc) {
    RECT crc; GetClientRect(g_hwnd, &crc);
    int w = crc.right - crc.left, h = crc.bottom - crc.top;
    if (w < 1 || h < 1) return;
    Gdiplus::Bitmap mem(w, h, PixelFormat32bppPARGB);
    Gdiplus::Graphics g(&mem);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    /* 背景: 180° 渐变 bg1→bg2 (页面 body linear-gradient(180deg, var(--bg1), var(--bg2))) */
    {
        LinearGradientBrush lg(RectF(0, 0, (REAL)w, (REAL)h), GC(g_sk.bg1), GC(g_sk.bg2), 90.0f);
        g.FillRectangle(&lg, 0, 0, w, h);
    }
    ComputeLayout();
    DrawTitlebar(g);
    DrawCfg(g);
    DrawPreview(g);
    DrawBar(g);
    DrawDropdown(g);
    Gdiplus::Graphics scr(dc);
    scr.DrawImage(&mem, 0, 0);   /* 1:1 直拷: 带目标宽高的重载走双线性重采样, 整帧文字发虚 */
}

/* ==================== 编辑器操作 (多行: 光标/选区/剪贴板/IME/滚动跟随) ==================== */
static bool g_edDrag = false, g_lineDrag = false;
static bool g_sbDrag = false;                /* cfg 滚动条拖动 */
static POINT g_downPt;

static void EdEnsureVisible() {
    if (!g_hwnd) return;
    Graphics gi(g_hwnd);
    float lh = g_edLineH();
    float caretY = g_caretLine * lh;
    float viewH = g_lo.editor.Height - S(24);
    if (caretY < g_edScrollY) g_edScrollY = caretY;
    if (caretY + lh > g_edScrollY + viewH) g_edScrollY = caretY + lh - viewH;
    float caretX = EdColX(gi, g_sqlLines[(size_t)g_caretLine], g_caretCol);
    float viewW = g_lo.editor.Width - S(28);
    if (caretX - g_edScrollX > viewW - S(20)) g_edScrollX = caretX - viewW + S(20);
    if (caretX - g_edScrollX < S(8)) g_edScrollX = caretX - S(8);
    if (g_edScrollX < 0) g_edScrollX = 0;
    /* 绘制帧会再钳制上限 */
}
/* IME 组字窗锚点 = 光标屏幕坐标 (自绘输入的 IME 体验) */
static void ImeAnchor(float x, float y) {
    if (!g_hwnd) return;
    HIMC imc = ImmGetContext(g_hwnd);
    if (!imc) return;
    POINT pt = { (LONG)x, (LONG)y };
    ClientToScreen(g_hwnd, &pt);
    COMPOSITIONFORM cf = {};
    cf.dwStyle = CFS_POINT;
    cf.ptCurrentPos = pt;
    ImmSetCompositionWindow(imc, &cf);
    ImmReleaseContext(g_hwnd, imc);
}
static void EdCaretMoved() {
    g_caretOn = true;
    EdEnsureVisible();
    if (g_hwnd) {
        Graphics gi(g_hwnd);
        float lh = g_edLineH();
        float cx = g_lo.editor.X + S(14) - g_edScrollX + EdColX(gi, g_sqlLines[(size_t)g_caretLine], g_caretCol);
        float cy = g_lo.editor.Y + S(12) - g_edScrollY + g_caretLine * lh + lh / 2;
        ImeAnchor(cx, cy);
    }
    InvalidateAll();
}
static void EdNormalizeSel() {
    if (g_selLine >= 0 && (g_selLine != g_caretLine || g_selCol != g_caretCol)) return;
    g_selLine = -1; g_selCol = -1;
}
static void EdInsertText(const std::wstring& t) {
    EdNormalizeSel();
    std::wstring& line = g_sqlLines[(size_t)g_caretLine];
    /* t 可能含 \n (粘贴多行) — 逐字符处理换行 */
    for (wchar_t c : t) {
        if (c == L'\n') {
            std::wstring tail = line.substr(g_caretCol);
            line = line.substr(0, g_caretCol);
            g_sqlLines.insert(g_sqlLines.begin() + g_caretLine + 1, tail);
            g_caretLine++;
            g_caretCol = 0;
        } else if (c == L'\r' || c == L'\t') {
            continue;   /* 单行域过滤口径: 回车经 WM_CHAR 另行处理, Tab 不插入 (浏览器 Tab = 焦点移动) */
        } else {
            line.insert(line.begin() + g_caretCol, c);
            g_caretCol++;
        }
    }
    g_sqlText.clear();
    for (size_t i = 0; i < g_sqlLines.size(); i++) {
        if (i) g_sqlText += L"\n";
        g_sqlText += g_sqlLines[i];
    }
    EdCaretMoved();
}
static void EdEraseSelection() {
    if (g_selLine < 0) return;
    int a1 = g_selLine, a2 = g_selCol, b1 = g_caretLine, b2 = g_caretCol;
    if (a1 > b1 || (a1 == b1 && a2 > b2)) { std::swap(a1, b1); std::swap(a2, b2); }
    if (a1 == b1) {
        std::wstring& line = g_sqlLines[(size_t)a1];
        line = line.substr(0, a2) + line.substr(b2);
    } else {
        g_sqlLines[(size_t)a1] = g_sqlLines[(size_t)a1].substr(0, a2);
        g_sqlLines[(size_t)b1] = g_sqlLines[(size_t)b1].substr(b2);
        g_sqlLines[(size_t)a1] += g_sqlLines[(size_t)b1];
        g_sqlLines.erase(g_sqlLines.begin() + a1 + 1, g_sqlLines.begin() + b1 + 1);
    }
    g_caretLine = a1; g_caretCol = a2;
    g_selLine = -1; g_selCol = -1;
}
static void EdReserialize() {
    g_sqlText.clear();
    for (size_t i = 0; i < g_sqlLines.size(); i++) {
        if (i) g_sqlText += L"\n";
        g_sqlText += g_sqlLines[i];
    }
}
static void EdCopy(bool cut) {
    if (g_selLine < 0) return;
    int a1 = g_selLine, a2 = g_selCol, b1 = g_caretLine, b2 = g_caretCol;
    if (a1 > b1 || (a1 == b1 && a2 > b2)) { std::swap(a1, b1); std::swap(a2, b2); }
    std::wstring txt;
    if (a1 == b1) txt = g_sqlLines[(size_t)a1].substr(a2, b2 - a2);
    else {
        txt = g_sqlLines[(size_t)a1].substr(a2) + L"\n";
        for (int i = a1 + 1; i < b1; i++) { txt += g_sqlLines[(size_t)i]; txt += L"\n"; }
        txt += g_sqlLines[(size_t)b1].substr(0, b2);
    }
    if (OpenClipboard(g_hwnd)) {
        EmptyClipboard();
        size_t bytes = (txt.size() + 1) * sizeof(wchar_t);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (h) { memcpy(GlobalLock(h), txt.c_str(), bytes); GlobalUnlock(h); SetClipboardData(CF_UNICODETEXT, h); }
        CloseClipboard();
    }
    if (cut) { EdEraseSelection(); EdReserialize(); EdCaretMoved(); }
}
static void EdPaste() {
    if (!OpenClipboard(g_hwnd)) return;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        if (const wchar_t* w = (const wchar_t*)GlobalLock(h)) {
            std::wstring t = w;
            GlobalUnlock(h);
            std::wstring t2;   /* \r\n / \r → \n (页面 textarea 同款换行归一) */
            for (size_t i = 0; i < t.size(); i++) {
                if (t[i] == L'\r') {
                    if (i + 1 < t.size() && t[i + 1] == L'\n') continue;
                    t2 += L'\n';
                } else t2 += t[i];
            }
            EdEraseSelection();
            EdInsertText(t2);
        }
    }
    CloseClipboard();
}
static void EdKey(WPARAM vk, bool ctrl, bool shift) {
    int lines = (int)g_sqlLines.size();
    if (ctrl && vk == 'A') { g_selLine = 0; g_selCol = 0; g_caretLine = lines - 1; g_caretCol = (int)g_sqlLines[(size_t)g_caretLine].size(); EdCaretMoved(); return; }
    if (ctrl && vk == 'C') { EdCopy(false); return; }
    if (ctrl && vk == 'X') { EdCopy(true); return; }
    if (ctrl && vk == 'V') { EdPaste(); return; }
    if (vk == VK_LEFT) {
        EdNormalizeSel();
        int baseCol = (g_selLine >= 0 && !shift) ? ((g_selLine < g_caretLine || (g_selLine == g_caretLine && g_selCol < g_caretCol)) ? g_selCol : g_caretCol) : g_caretCol;
        int baseLine = (g_selLine >= 0 && !shift) ? std::min(g_selLine, g_caretLine) : g_caretLine;
        if (g_selLine >= 0 && !shift) { g_caretLine = baseLine; g_caretCol = baseCol; }
        if (g_caretCol > 0) g_caretCol--;
        else if (g_caretLine > 0) { g_caretLine--; g_caretCol = (int)g_sqlLines[(size_t)g_caretLine].size(); }
        if (!shift) g_selLine = -1, g_selCol = -1;
        else if (g_selLine < 0) { g_selLine = baseLine; g_selCol = baseCol; }
        g_goalX = 0;
        EdCaretMoved();
        return;
    }
    if (vk == VK_RIGHT) {
        EdNormalizeSel();
        int baseLine = (g_selLine >= 0 && !shift) ? std::max(g_selLine, g_caretLine) : g_caretLine;
        if (g_selLine >= 0 && !shift) {
            g_caretLine = baseLine;
            g_caretCol = (g_selLine > g_caretLine) ? g_selCol : g_caretCol;
        }
        if (g_caretCol < (int)g_sqlLines[(size_t)g_caretLine].size()) g_caretCol++;
        else if (g_caretLine < lines - 1) { g_caretLine++; g_caretCol = 0; }
        if (!shift) g_selLine = -1, g_selCol = -1;
        else if (g_selLine < 0) { g_selLine = baseLine; g_selCol = g_caretCol; }
        g_goalX = 0;
        EdCaretMoved();
        return;
    }
    if (vk == VK_UP || vk == VK_DOWN) {
        EdNormalizeSel();
        if (g_selLine >= 0 && shift) { /* 保留锚 */ }
        else if (!shift) { g_selLine = -1; g_selCol = -1; }
        else if (g_selLine < 0) { g_selLine = g_caretLine; g_selCol = g_caretCol; }
        Graphics gi(g_hwnd);
        if (g_goalX <= 0) g_goalX = EdColX(gi, g_sqlLines[(size_t)g_caretLine], g_caretCol);
        if (vk == VK_UP && g_caretLine > 0) {
            g_caretLine--;
            g_caretCol = EdColAt(gi, g_sqlLines[(size_t)g_caretLine], g_goalX);
        } else if (vk == VK_DOWN && g_caretLine < lines - 1) {
            g_caretLine++;
            g_caretCol = EdColAt(gi, g_sqlLines[(size_t)g_caretLine], g_goalX);
        }
        EdCaretMoved();
        return;
    }
    if (vk == VK_HOME) {
        EdNormalizeSel();
        int old = g_caretCol;
        g_caretCol = ctrl ? 0 : 0;
        if (!shift) g_selLine = -1, g_selCol = -1;
        else if (g_selLine < 0) { g_selLine = g_caretLine; g_selCol = old; }
        if (ctrl) g_caretLine = 0;
        g_goalX = 0;
        EdCaretMoved();
        return;
    }
    if (vk == VK_END) {
        EdNormalizeSel();
        int old = g_caretCol;
        g_caretCol = (int)g_sqlLines[(size_t)g_caretLine].size();
        if (!shift) g_selLine = -1, g_selCol = -1;
        else if (g_selLine < 0) { g_selLine = g_caretLine; g_selCol = old; }
        if (ctrl) { g_caretLine = lines - 1; g_caretCol = (int)g_sqlLines[(size_t)g_caretLine].size(); }
        g_goalX = 0;
        EdCaretMoved();
        return;
    }
    if (vk == VK_BACK) {
        if (g_selLine >= 0) { EdEraseSelection(); EdReserialize(); EdCaretMoved(); }
        else if (g_caretCol > 0) {
            std::wstring& line = g_sqlLines[(size_t)g_caretLine];
            line.erase(g_caretCol - 1, 1);
            g_caretCol--;
            EdReserialize(); EdCaretMoved();
        } else if (g_caretLine > 0) {
            g_caretCol = (int)g_sqlLines[(size_t)g_caretLine - 1].size();
            g_sqlLines[(size_t)g_caretLine - 1] += g_sqlLines[(size_t)g_caretLine];
            g_sqlLines.erase(g_sqlLines.begin() + g_caretLine);
            g_caretLine--;
            EdReserialize(); EdCaretMoved();
        }
        return;
    }
    if (vk == VK_DELETE) {
        if (g_selLine >= 0) { EdEraseSelection(); EdReserialize(); EdCaretMoved(); }
        else {
            std::wstring& line = g_sqlLines[(size_t)g_caretLine];
            if (g_caretCol < (int)line.size()) { line.erase(g_caretCol, 1); EdReserialize(); EdCaretMoved(); }
            else if (g_caretLine < lines - 1) {
                line += g_sqlLines[(size_t)g_caretLine + 1];
                g_sqlLines.erase(g_sqlLines.begin() + g_caretLine + 1);
                EdReserialize(); EdCaretMoved();
            }
        }
        return;
    }
    if (vk == VK_PRIOR || vk == VK_NEXT) {   /* 翻页 */
        float page = (g_lo.editor.Height - S(24)) / g_edLineH();
        g_caretLine += (vk == VK_PRIOR) ? -(int)page : (int)page;
        g_caretLine = std::max(0, std::min(lines - 1, g_caretLine));
        g_caretCol = std::min(g_caretCol, (int)g_sqlLines[(size_t)g_caretLine].size());
        if (!shift) g_selLine = -1, g_selCol = -1;
        g_goalX = 0;
        EdCaretMoved();
    }
}

/* ==================== 单行输入操作 (值过滤 + 光标; space-map InputKey 同款骨架) ==================== */
static void LineInsertText(const std::wstring& t) {
    std::wstring* b = BoundStr(g_focusId, g_focusRow);
    if (!b) return;
    bool num = LineFilterNum(g_focusId, g_focusRow);
    bool date = LineFilterDate(g_focusId, g_focusRow);
    int a = g_lineSelA >= 0 ? g_lineSelA : g_lineCaret;
    int bb = g_lineCaret;
    if (a > bb) std::swap(a, bb);
    std::wstring ins;
    for (wchar_t c : t) {
        if (c == L'\r' || c == L'\n') continue;
        if (num && !(c >= L'0' && c <= L'9')) continue;   /* number input 字符域 */
        if (date && !((c >= L'0' && c <= L'9') || c == L'-')) continue;
        ins += c;
    }
    if (ins.empty() && t.size() > 1) return;
    *b = b->substr(0, a) + ins + b->substr(bb);
    g_lineCaret = a + (int)ins.size();
    g_lineSelA = -1;
    RefreshSql();
}
static void LineKey(WPARAM vk, bool ctrl, bool shift) {
    std::wstring* b = BoundStr(g_focusId, g_focusRow);
    if (!b) return;
    int n = (int)b->size();
    if (ctrl && vk == 'A') { g_lineSelA = 0; g_lineCaret = n; InvalidateAll(); return; }
    if (ctrl && vk == 'C' && g_lineSelA >= 0 && g_lineSelA != g_lineCaret) {
        int a = g_lineSelA, bb = g_lineCaret;
        if (a > bb) std::swap(a, bb);
        std::wstring t = b->substr(a, bb - a);
        if (OpenClipboard(g_hwnd)) {
            EmptyClipboard();
            size_t bytes = (t.size() + 1) * sizeof(wchar_t);
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (h) { memcpy(GlobalLock(h), t.c_str(), bytes); GlobalUnlock(h); SetClipboardData(CF_UNICODETEXT, h); }
            CloseClipboard();
        }
        return;
    }
    if (ctrl && vk == 'X' && g_lineSelA >= 0 && g_lineSelA != g_lineCaret) {
        int a = g_lineSelA, bb = g_lineCaret;
        if (a > bb) std::swap(a, bb);
        std::wstring t = b->substr(a, bb - a);
        if (OpenClipboard(g_hwnd)) {
            EmptyClipboard();
            size_t bytes = (t.size() + 1) * sizeof(wchar_t);
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (h) { memcpy(GlobalLock(h), t.c_str(), bytes); GlobalUnlock(h); SetClipboardData(CF_UNICODETEXT, h); }
            CloseClipboard();
        }
        *b = b->substr(0, a) + b->substr(bb);
        g_lineCaret = a; g_lineSelA = -1;
        RefreshSql();
        return;
    }
    if (ctrl && vk == 'V') {
        if (OpenClipboard(g_hwnd)) {
            HANDLE h = GetClipboardData(CF_UNICODETEXT);
            if (h) if (const wchar_t* w = (const wchar_t*)GlobalLock(h)) {
                std::wstring t = w;
                GlobalUnlock(h);
                g_lineSelA = -1;
                LineInsertText(t);
            }
            CloseClipboard();
        }
        return;
    }
    if (vk == VK_LEFT || vk == VK_RIGHT) {
        int base = (g_lineSelA >= 0 && !shift) ? ((vk == VK_LEFT) ? std::min(g_lineSelA, g_lineCaret) : std::max(g_lineSelA, g_lineCaret)) : g_lineCaret;
        if (vk == VK_LEFT) g_lineCaret = std::max(0, base - 1);
        else g_lineCaret = std::min(n, base + 1);
        if (!shift) g_lineSelA = -1;
        else if (g_lineSelA < 0) g_lineSelA = base;
        g_caretOn = true;
        InvalidateAll();
        return;
    }
    if (vk == VK_HOME) { g_lineCaret = 0; if (!shift) g_lineSelA = -1; else if (g_lineSelA < 0) g_lineSelA = n; InvalidateAll(); return; }
    if (vk == VK_END)  { g_lineCaret = n; if (!shift) g_lineSelA = -1; else if (g_lineSelA < 0) g_lineSelA = n; InvalidateAll(); return; }
    if (vk == VK_BACK) {
        if (g_lineSelA >= 0 && g_lineSelA != g_lineCaret) {
            int a = g_lineSelA, bb = g_lineCaret;
            if (a > bb) std::swap(a, bb);
            *b = b->substr(0, a) + b->substr(bb);
            g_lineCaret = a; g_lineSelA = -1;
            RefreshSql();
        } else if (g_lineCaret > 0) {
            b->erase(g_lineCaret - 1, 1);
            g_lineCaret--;
            RefreshSql();
        }
        return;
    }
    if (vk == VK_DELETE) {
        if (g_lineSelA >= 0 && g_lineSelA != g_lineCaret) {
            int a = g_lineSelA, bb = g_lineCaret;
            if (a > bb) std::swap(a, bb);
            *b = b->substr(0, a) + b->substr(bb);
            g_lineCaret = a; g_lineSelA = -1;
            RefreshSql();
        } else if (g_lineCaret < n) {
            b->erase(g_lineCaret, 1);
            RefreshSql();
        }
        return;
    }
}
/* 点击 x → 单行光标列 */
static int LineCaretAt(Gdiplus::Graphics& g, const std::wstring& text, float localX) {
    float prev = 0;
    for (int i = 0; i <= (int)text.size(); i++) {
        float cx = MeasureStr(g, text.substr(0, i), F(S(12), false));
        if (localX <= (prev + cx) / 2) return i;
        prev = cx;
    }
    return (int)text.size();
}

/* ==================== 鼠标交互 ==================== */
static bool TrackHover(HWND hwnd) {
    TRACKMOUSEEVENT te = { sizeof(te), TME_LEAVE, hwnd, 0 };
    return TrackMouseEvent(&te);
}
static bool InEditor(POINT pt) {
    return pt.x >= g_lo.editor.X && pt.x < g_lo.editor.X + g_lo.editor.Width &&
           pt.y >= g_lo.editor.Y && pt.y < g_lo.editor.Y + g_lo.editor.Height;
}
static bool InCfg(POINT pt) {
    return pt.x >= g_lo.cfg.X && pt.x < g_lo.cfg.X + g_lo.cfg.Width &&
           pt.y >= g_lo.cfg.Y && pt.y < g_lo.cfg.Y + g_lo.cfg.Height;
}
static void EditorPointToCaret(POINT pt) {
    Graphics gi(g_hwnd);
    float lh = g_edLineH();
    int ln = (int)((pt.y - g_lo.editor.Y - S(12) + g_edScrollY) / lh);
    ln = std::max(0, std::min((int)g_sqlLines.size() - 1, ln));
    float lx = pt.x - (g_lo.editor.X + S(14)) + g_edScrollX;
    g_caretLine = ln;
    g_caretCol = EdColAt(gi, g_sqlLines[(size_t)ln], lx);
}

static void OnLButtonDown(POINT pt) {
    SetFocus(g_hwnd);
    g_downPt = pt;
    if (g_ddOpen) {   /* 下拉浮层: 点内选项 / 点外关闭 */
        int idx = -1;
        if (pt.x >= g_ddRc.X && pt.x < g_ddRc.X + g_ddRc.Width && pt.y >= g_ddRc.Y && pt.y < g_ddRc.Y + g_ddRc.Height)
            idx = (int)((pt.y - g_ddRc.Y - S(4)) / S(26));
        if (idx >= 0 && idx < (int)g_ddItems.size()) FeedDropdown(idx);
        else CloseDropdown();
        return;
    }
    const CtlRect* c = HitCtl(pt);
    if (!c) {   /* 空白: 清焦点 */
        if (g_focusId || g_edFocus) { g_focusId = 0; g_focusRow = -1; g_edFocus = false; InvalidateAll(); }
        return;
    }
    switch (c->id) {
        case CI_CLOSE: g_pressId = CI_CLOSE; SetCapture(g_hwnd); return;
        case CI_RUN:   g_pressId = CI_RUN;   SetCapture(g_hwnd); return;
        case CI_COPY:  g_pressId = CI_COPY;  SetCapture(g_hwnd); return;
        case CI_SELMODE: case CI_GROUPBY: case CI_WCONN: case CI_WTYPE: case CI_WOP:
        case CI_WUNIT: case CI_WUNIT2: case CI_ORDERF: case CI_ORDERD: case CI_ORDERF2: case CI_ORDERD2:
            ExpandDropdown(*c);
            return;
        case CI_WV:
            if (c->row >= 0 && (g_wrows[(size_t)c->row].type == WT_FILETYPE || g_wrows[(size_t)c->row].type == WT_MTIME)) {
                ExpandDropdown(*c);
                return;
            }
            /* fallthrough — 文本输入 */
        case CI_WV2: case CI_WV3: case CI_HAVING: case CI_LIMIT: {
            g_focusId = c->id; g_focusRow = c->row; g_edFocus = false;
            g_lineSelA = -1;
            g_lineCaret = 0;
            Graphics gi(g_hwnd);
            std::wstring* b = BoundStr(c->id, c->row);
            if (b) g_lineCaret = LineCaretAt(gi, *b, (float)pt.x - c->rc.X - S(8) + g_lineScroll);
            TrackHover(g_hwnd);
            InvalidateAll();
            g_lineDrag = true;
            SetCapture(g_hwnd);
            return;
        }
        case CI_COLCHK: {
            g_colChk[c->idx] = !g_colChk[c->idx];
            if (g_selMode == 2 && c->idx == 5) { /* Path 勾选与 agg 无联动 (页面 selMode change 才联动) */ }
            RefreshSql();
            return;
        }
        case CI_NOSH:
            g_noSH = !g_noSH;
            RefreshSql();
            return;
        case CI_WSUB:
            g_wrows[(size_t)c->row].sub = !g_wrows[(size_t)c->row].sub;
            RefreshSql();
            return;
        case CI_AGGCHIP:
            g_aggChip[c->idx] = !g_aggChip[c->idx];   /* toggle (COUNT(*) 固定项不在命中表) */
            RefreshSql();
            return;
        case CI_WDEL:
            DelRow(c->row);
            return;
        case CI_ADDCOND:
            AddRow();
            return;
        case CI_EDITOR: {
            g_edFocus = true;
            g_focusId = 0; g_focusRow = -1;
            g_caretOn = true;
            EditorPointToCaret(pt);
            g_selLine = g_caretLine; g_selCol = g_caretCol;   /* 拖选锚 */
            g_edDrag = true;
            SetCapture(g_hwnd);
            InvalidateAll();
            return;
        }
        default:
            if (g_focusId || g_edFocus) { g_focusId = 0; g_focusRow = -1; g_edFocus = false; InvalidateAll(); }
            return;
    }
}
static void OnMouseMove(POINT pt) {
    if (g_ddOpen) {   /* 浮层悬停 */
        int h = -1;
        if (pt.x >= g_ddRc.X && pt.x < g_ddRc.X + g_ddRc.Width && pt.y >= g_ddRc.Y && pt.y < g_ddRc.Y + g_ddRc.Height)
            h = (int)((pt.y - g_ddRc.Y - S(4)) / S(26));
        if (h >= (int)g_ddItems.size()) h = -1;
        if (h != g_ddHover) { g_ddHover = h; InvalidateAll(); }
        return;
    }
    if (g_sbDrag) {   /* cfg 滚动条拖动 */
        float rel = (float)(pt.y - g_lo.cfg.Y - S(2)) / std::max(1.0f, g_lo.cfg.Height - S(4));
        g_cfgScroll = rel * g_cfgMaxScroll;
        if (g_cfgScroll < 0) g_cfgScroll = 0;
        if (g_cfgScroll > g_cfgMaxScroll) g_cfgScroll = g_cfgMaxScroll;
        InvalidateAll();
        return;
    }
    if (g_edDrag) {   /* 编辑器拖选 */
        EditorPointToCaret(pt);
        InvalidateAll();
        return;
    }
    if (g_lineDrag && g_focusId) {   /* 单行拖选 */
        const CtlRect* c = CtlOf(g_focusId, g_focusRow);
        if (c) {
            Graphics gi(g_hwnd);
            std::wstring* b = BoundStr(g_focusId, g_focusRow);
            if (b) g_lineCaret = LineCaretAt(gi, *b, (float)pt.x - c->rc.X - S(8) + g_lineScroll);
            InvalidateAll();
        }
        return;
    }
    /* 悬停态 */
    const CtlRect* c = HitCtl(pt);
    int hid = c ? c->id : 0, hrow = c ? c->row : -1;
    /* 聚焦输入框内 = 文本光标 */
    if (hid == CI_EDITOR || (SameCtl(g_focusId, g_focusRow, c) && BoundStr(g_focusId, g_focusRow)))
        SetCursor(LoadCursorW(NULL, IDC_IBEAM));
    else
        SetCursor(LoadCursorW(NULL, IDC_ARROW));
    if (hid != g_hotId || hrow != g_hotRow) {
        g_hotId = hid; g_hotRow = hrow;
        TrackHover(g_hwnd);
        InvalidateAll();
    }
}
static void OnLButtonUp(POINT pt) {
    (void)pt;
    /* 先取按压动作再释放捕获: ReleaseCapture 会同步发 WM_CAPTURECHANGED,
       CAPTURECHANGED 分支清 g_pressId — 若先释放后取值, 三个按钮动作全被吞 (实锤) */
    int pid = g_pressId;
    g_pressId = 0;
    if (GetCapture() == g_hwnd) ReleaseCapture();
    g_edDrag = false; g_lineDrag = false; g_sbDrag = false;
    switch (pid) {
        case CI_CLOSE: if (g_hwnd) DestroyWindow(g_hwnd); return;
        case CI_RUN:   DoRun(); return;
        case CI_COPY:  DoCopy(); return;
    }
    InvalidateAll();
}
static void OnWheel(int delta, POINT pt) {
    if (g_ddOpen) return;
    if (InEditor(pt)) {
        g_edScrollY -= delta / 120.0f * g_edLineH() * 3;
        if (g_edScrollY < 0) g_edScrollY = 0;
        InvalidateAll();
        return;
    }
    if (InCfg(pt)) {
        g_cfgScroll -= delta / 120.0f * S(48);
        if (g_cfgScroll < 0) g_cfgScroll = 0;
        if (g_cfgScroll > g_cfgMaxScroll) g_cfgScroll = g_cfgMaxScroll;
        InvalidateAll();
    }
}

/* ==================== 窗口过程 (space-map 无边框口径) ==================== */
static LRESULT CALLBACK SqlGenWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            SetTimer(hwnd, 1, 530, NULL);   /* 光标闪烁 */
            return 0;
        case WM_SIZE:
            ComputeLayout();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case WM_GETMINMAXINFO: {
            DefWindowProcW(hwnd, msg, wp, lp);
            MINMAXINFO* mmi = (MINMAXINFO*)lp;
            UINT dpi = GetDpiForWindow(hwnd);
            float sc = dpi / 96.0f;
            mmi->ptMinTrackSize.x = (LONG)(780 * sc);   /* 清单 window 档 minWidth/minHeight */
            mmi->ptMinTrackSize.y = (LONG)(520 * sc);
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
            RECT* sug = (RECT*)lp;
            SetWindowPos(hwnd, NULL, sug->left, sug->top, sug->right - sug->left, sug->bottom - sug->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case WM_TIMER:
            if (wp == 1) {
                g_caretOn = !g_caretOn;
                if (g_edFocus || (g_focusId && BoundStr(g_focusId, g_focusRow))) InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        case WM_MOUSEMOVE: {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            OnMouseMove(pt);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (g_hotId || g_hotRow) { g_hotId = 0; g_hotRow = -1; InvalidateRect(hwnd, NULL, FALSE); }
            return 0;
        case WM_LBUTTONDOWN: {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            OnLButtonDown(pt);
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            OnLButtonUp(pt);
            return 0;
        }
        case WM_CAPTURECHANGED:
            g_edDrag = false; g_lineDrag = false; g_sbDrag = false; g_pressId = 0;
            return 0;
        case WM_MOUSEWHEEL: {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd, &pt);
            OnWheel(GET_WHEEL_DELTA_WPARAM(wp), pt);
            return 0;
        }
        case WM_KEYDOWN: {
            bool ctrl = GetKeyState(VK_CONTROL) & 0x8000;
            bool shift = GetKeyState(VK_SHIFT) & 0x8000;
            if (g_ddOpen) {   /* 下拉键盘 (原生 select 同款) */
                if (wp == VK_ESCAPE) { CloseDropdown(); return 0; }
                if (wp == VK_RETURN || wp == VK_TAB) { FeedDropdown(g_ddHover >= 0 ? g_ddHover : g_ddCur); return 0; }
                if (wp == VK_UP || wp == VK_DOWN) {
                    int d = (wp == VK_UP) ? -1 : 1;
                    int h = g_ddHover + d;
                    if (h < 0) h = (int)g_ddItems.size() - 1;
                    if (h >= (int)g_ddItems.size()) h = 0;
                    g_ddHover = h;
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                return 0;
            }
            if (g_edFocus) { EdKey(wp, ctrl, shift); return 0; }
            if (g_focusId && BoundStr(g_focusId, g_focusRow)) {
                if (wp == VK_ESCAPE) { g_focusId = 0; g_focusRow = -1; InvalidateRect(hwnd, NULL, FALSE); return 0; }
                LineKey(wp, ctrl, shift);
                return 0;
            }
            if (wp == VK_ESCAPE) CloseDropdown();
            return 0;
        }
        case WM_CHAR: {
            if (wp == 3 || wp == 22 || wp == 24) return 0;   /* Ctrl+C/V/X 控制字符 (KEYDOWN 已处理) */
            if (g_edFocus) {
                if (wp == L'\r') { EdInsertText(L"\n"); return 0; }
                if (wp >= 32 && wp != 127) EdInsertText(std::wstring(1, (wchar_t)wp));
                return 0;
            }
            if (g_focusId && BoundStr(g_focusId, g_focusRow)) {
                if (wp >= 32 && wp != 127) LineInsertText(std::wstring(1, (wchar_t)wp));
                return 0;
            }
            return 0;
        }
        case WM_IME_COMPOSITION: {
            if (lp & GCS_RESULTSTR) {
                HIMC imc = ImmGetContext(hwnd);
                if (imc) {
                    LONG sz = ImmGetCompositionStringW(imc, GCS_RESULTSTR, NULL, 0);
                    if (sz > 0) {
                        std::vector<wchar_t> buf(sz / 2 + 1, 0);
                        ImmGetCompositionStringW(imc, GCS_RESULTSTR, buf.data(), sz);
                        if (g_edFocus) EdInsertText(buf.data());
                        else if (g_focusId) LineInsertText(buf.data());
                    }
                    ImmReleaseContext(hwnd, imc);
                }
                return 0;
            }
            break;
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
            /* 标题栏区 (40px) 拖动; 关闭钮保持客户区点击 (页面 click 语义) */
            const CtlRect* cc = CtlOf(CI_CLOSE);
            if (cc && c.x >= cc->rc.X && c.x < cc->rc.X + cc->rc.Width && c.y >= cc->rc.Y && c.y < cc->rc.Y + cc->rc.Height)
                return HTCLIENT;
            if (c.y < S(40)) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            if (hwnd == g_hwnd) g_hwnd = NULL;
            /* 禁 PostQuitMessage: 本窗口过程跑在宿主 UI 线程的消息循环上, 终止循环 = 宿主退出 */
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ==================== 打开窗口 (space-map 无边框口径; 初始 1100×680 / min 780×520) ==================== */
/* 接管型搜索模式模板 (原版清单声明原文; {keyword} 占位) */
static const wchar_t* const KW_SQL_TEMPLATE =
    L"SELECT * FROM alltable WHERE FileType = '视频' AND FName LIKE '%{keyword}%' "
    L"AND FAttr !~ '[SH]' ORDER BY Size DESC";
/* {keyword} 占位替换 (页面回调同语义: 输入词单引号翻倍, 兼容旧写法 {kw}) */
static std::wstring ApplyKeywordTemplate(const std::wstring& input) {
    std::wstring kw;
    for (wchar_t c : input) {
        if (c == L'\'') { kw += L"''"; continue; }
        kw += c;
    }
    std::wstring out;
    const std::wstring tpl = KW_SQL_TEMPLATE;
    for (size_t i = 0; i < tpl.size();) {
        if (tpl[i] == L'{' && _wcsnicmp(tpl.c_str() + i, L"{keyword}", 9) == 0) { out += kw; i += 9; }
        else if (tpl[i] == L'{' && _wcsnicmp(tpl.c_str() + i, L"{kw}", 4) == 0) { out += kw; i += 4; }
        else out += tpl[i++];
    }
    return out;
}

static void CloseDropdownSilent() { g_ddOpen = false; g_ddHover = -1; }

static void ResetWindowState() {
    /* 每次打开 = 全新状态 (真关闭型窗口, 重开 = 页面 reload) */
    g_selMode = 0;
    for (int i = 0; i < 12; i++) g_colChk[i] = false;
    for (int i = 0; i < 6; i++) g_aggChip[i] = (i == 0);
    g_groupBy = 0;
    g_having.clear();
    g_wrows.clear();
    WRow def;   /* 初始一行条件: 文件类型 = 视频 (页面 boot IIFE) */
    def.v = FileTypeLabel(0);
    g_wrows.push_back(def);
    g_orderField = g_orderDir = g_orderField2 = g_orderDir2 = 0;
    g_limit.clear();
    g_noSH = true;
    g_cfgScroll = 0;
    g_edScrollY = g_edScrollX = 0;
    g_caretLine = g_caretCol = 0;
    g_selLine = g_selCol = -1;
    g_edFocus = false;
    g_focusId = 0; g_focusRow = -1;
    g_hotId = 0; g_hotRow = -1;
    CloseDropdownSilent();
    RefreshSql();
    SetStatus(SG("StatusInit", L"配置条件或选择模板生成 SQL, 点\"填入搜索框执行\""));
}

static void OpenWindow(XjsWindowToken ownerToken) {
    g_ownerToken = ownerToken;
    /* 语言/皮肤跟随发起窗 (打开时装载; EVT_SKIN 到达重取) */
    std::string code = WindowLangCode(ownerToken);
    int li = LangIndexFromCode(code);
    if (li < 0) li = 2;   /* ms/未知 → 英文 (原版回退链) */
    g_langIdx = li;
    LoadLangTable(LoadRcUtf8(LangRcOf(li)), &g_sg[li]);
    ApplySkin(ownerToken);
    if (g_hwnd && IsWindow(g_hwnd)) {
        if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
        ResetWindowState();   /* 唤起同页面 reload 口径 (原版窗口 hide/还原保留状态; D2D 真关闭型 = 重置) */
        InvalidateAll();
        if (g_host) g_host->Summon(g_ctx, g_hwnd);
        return;
    }
    static bool registered = false;
    if (!registered) {
        HMODULE mod = NULL;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCWSTR)&OpenWindow, &mod))
            return;
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);   /* 红线: 缺它注册静默失败 */
        wc.lpfnWndProc = SqlGenWndProc;
        wc.hInstance = mod;
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.lpszClassName = L"XjsSqlGenWnd";
        if (!RegisterClassExW(&wc)) return;
        registered = true;
    }
    /* 无边框(有阴影)窗口, 同宿主主窗口径: 样式不带 WS_CAPTION — 系统没有标准标题栏可盖画
       (带 WS_CAPTION 时系统标题栏会叠在自绘标题栏上方 = 双控制栏);
       WS_THICKFRAME 保留 DWM 阴影+拖边调整+Snap, WM_NCCALCSIZE 1px 内缩保圆角抗锯齿 */
    HWND hwnd = CreateWindowExW(0, L"XjsSqlGenWnd", L"SQL 生成器",
                                WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1100, 680,
                                NULL, NULL, NULL, NULL);
    if (!hwnd) {
        if (g_host) g_host->Toast(g_ctx, ownerToken, "SQL 生成器窗口创建失败", XJS_PLUGIN_TOAST_ERROR);
        return;
    }
    UINT dpi = GetDpiForWindow(hwnd);
    g_dpi = dpi / 96.0f;
    /* 尺寸按 DPI 折算并居中到主屏工作区 */
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int w = (int)(1100 * g_dpi), h = (int)(680 * g_dpi);
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
    ResetWindowState();
    ComputeLayout();    ShowWindow(hwnd, SW_SHOW);
    if (g_host) g_host->Summon(g_ctx, hwnd);   /* 借前台唤起 (与宿主同实现) */
}

/* ==================== 插件固定导出 ==================== */
extern "C" __declspec(dllexport) const XjsPluginInfo* XJS_PLUGIN_CALL XjsPlugin_GetInfo(void) {
    static const XjsPluginInfo info = { XJS_PLUGIN_ABI_VERSION, sizeof(XjsPluginInfo), "sql-generator", "1.0.0" };
    return &info;
}
extern "C" __declspec(dllexport) int XJS_PLUGIN_CALL XjsPlugin_Init(XjsPluginCtx* ctx, const XjsPluginHost* host) {
    g_ctx = ctx;
    g_host = host;
    static Gdiplus::GdiplusStartupInput gsi;
    if (Gdiplus::GdiplusStartup(&g_gdipToken, &gsi, NULL) != Gdiplus::Ok)
        return XJS_PLUGIN_ERR_FAIL;
    if (g_host) {
        g_host->Subscribe(g_ctx, XJS_PLUGIN_EVT_SKIN);   /* 皮肤变化 → 窗口配色跟随 (OnEvent) */
        g_host->Log(g_ctx, 1, "sql-generator 1.0.0 initialized (GDI+ full custom-draw replica of the original page)");
    }
    return XJS_PLUGIN_OK;
}
extern "C" __declspec(dllexport) void XJS_PLUGIN_CALL XjsPlugin_Shutdown(XjsPluginCtx* ctx) {
    (void)ctx;
    if (g_hwnd) DestroyWindow(g_hwnd);
    if (g_gdipToken) { Gdiplus::GdiplusShutdown(g_gdipToken); g_gdipToken = 0; }
}
/* 搜索框右键菜单: "SQL 生成器" (文字随发起窗口界面语言; 照抄原版 sqlmenu.js 注册语义) */
extern "C" __declspec(dllexport) int XJS_PLUGIN_CALL XjsPlugin_BuildMenu(XjsPluginCtx*, const char* kindUtf8,
                                                                        XjsWindowToken window,
                                                                        const int* fileIds, int count,
                                                                        const char* inputUtf8,
                                                                        char* buf, int cap) {
    (void)fileIds; (void)count; (void)inputUtf8;
    if (!kindUtf8 || strcmp(kindUtf8, "searchBox") != 0) return 0;
    std::string text = U8(MenuTextForLang(WindowLangCode(window)));
    std::string j = "[{\"标识\":";
    auto esc = [&](const std::string& s) {
        j += "\"";
        for (unsigned char ch : s) {
            switch (ch) {
                case '"': j += "\\\""; break;
                case '\\': j += "\\\\"; break;
                case '\n': j += "\\n"; break;
                default: if (ch < 0x20) { char b2[8]; sprintf_s(b2, 8, "\\u%04x", ch); j += b2; } else j += (char)ch;
            }
        }
        j += "\"";
    };
    esc("open-sqlgen");
    j += ",\"文字\":";
    esc(text);
    j += ",\"顺序\":0}]";
    if (!buf || cap <= 0) return (int)j.size();
    if (cap < (int)j.size() + 1) return XJS_PLUGIN_ERR_OVERFLOW;
    memcpy(buf, j.c_str(), j.size() + 1);
    return (int)j.size();
}
/* 搜索框菜单点击 → 打开 (或激活) 插件窗口 (原版 sqlmenu.js run → openPlugin 同语义) */
extern "C" __declspec(dllexport) void XJS_PLUGIN_CALL XjsPlugin_OnCommand(XjsPluginCtx*, const char* cmdIdUtf8,
                                                                          XjsWindowToken window,
                                                                          const int* fileIds, int count, int kind) {
    (void)fileIds; (void)count; (void)kind;
    if (!cmdIdUtf8 || strcmp(cmdIdUtf8, "open-sqlgen") != 0) return;
    OpenWindow(window);
}
/* 接管型搜索模式 kw-sql (清单声明无模板 = 本回调驱动):
   窗口开着 = 填预览框并由本窗执行 (原版宿主转发语义); 没开 = 直接 SearchSetText (不弹窗打扰)。 */
extern "C" __declspec(dllexport) int XJS_PLUGIN_CALL XjsPlugin_OnSearchMode(XjsPluginCtx*, const char* modeIdUtf8,
                                                                            XjsWindowToken window,
                                                                            const char* inputUtf8) {
    if (!modeIdUtf8 || strcmp(modeIdUtf8, "kw-sql") != 0) return 0;
    std::wstring input = W8(inputUtf8 ? inputUtf8 : "");
    if (input.empty()) return 1;   /* 空输入无可替换 (原版页面 evt.keyword 空即不处理) */
    if (g_hwnd && IsWindow(g_hwnd)) {
        if (window) g_ownerToken = window;   /* 跟随本次执行的发起窗 (填入目标随之) */
        g_sqlText = ApplyKeywordTemplate(input);
        SqlSplit();
        g_caretLine = g_caretCol = 0; g_selLine = g_selCol = -1;
        SetStatus(SG("StatusRun", L"已填入主窗口搜索框并执行 (蜗牛快搜自动识别 SQL)"));
        DoRun();   /* 填预览框后执行 (页面 onSearchMode 回调同序) */
        InvalidateAll();
        return 1;
    }
    std::string sql8 = U8(ApplyKeywordTemplate(input));
    if (sql8.empty()) return 1;
    if (g_host) {
        int e = g_host->SearchSetText(g_ctx, window ? window : g_ownerToken, sql8.c_str(), "sql", 1);
        if (e == XJS_PLUGIN_ERR_NOTFOUND && window)
            g_host->SearchSetText(g_ctx, 0, sql8.c_str(), "sql", 1);
    }
    return 1;
}
/* 事件: 换肤 (window = 换肤窗令牌) — 窗口开着且是发起窗 (或发起窗已失效) 重取皮肤重绘 */
extern "C" __declspec(dllexport) void XJS_PLUGIN_CALL XjsPlugin_OnEvent(XjsPluginCtx*, int eventType,
                                                                        XjsWindowToken window, void* result) {
    (void)result;
    if (eventType != XJS_PLUGIN_EVT_SKIN) return;
    if (g_hwnd && IsWindow(g_hwnd)) {
        if (window == 0 || window == g_ownerToken) {
            ApplySkin(g_ownerToken);
            InvalidateAll();
        }
    }
}
extern "C" __declspec(dllexport) void XJS_PLUGIN_CALL XjsPlugin_OnHostGone(XjsPluginCtx* ctx) {
    (void)ctx;   /* 引擎即将销毁: 无常驻引擎资源, 窗口随 Shutdown 关闭 */
}



