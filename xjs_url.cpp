/* xjs_url.cpp — snailsearch:// URL 协议 (P1)
 *
 * 传输: 中完整性侧 wWinMain 命中单实例 mutex → SendMessageTimeout(WM_COPYDATA, SNAIL_URL_MAGIC, COPYDATASTRUCT*);
 *       高完整性侧 WndProc WM_COPYDATA 分支校验魔数 + 大小 → UTF-8 URL → 本文件 XjsUrlApplyFromText。
 *       冷启动 (无实例): XjsAppMain 主窗建完读 g_startupUrl 应用一次。
 *
 * 语法: snailsearch://<action>[?<query>]  (P1 只支持 new/open/focus + q|mode|view|skin|zoom|preview|save)
 *       q 支持 "a|b|c" 多重搜索 (引擎 XJS_KEYWORD_MULTI 链式过滤): 每段可带模式前缀
 *       wild:/re:/sql:/lua: (无前缀=本窗当前模式); 单段无前缀回落普通搜索并记历史。
 *       save=0 缺省: OpenNew(-1) 建临时窗 (w->urlTransient=true), 关窗走 DestroyAndFree 时移除档案槽;
 *       save=1 P1 暂按 save=0 走, 打日志"P2 支持" (方案 六.P1)。
 *
 * 校验: 一律按"不可信输入"处理 — 枚举 clamp, 未知键忽略, 载荷 >32KB 拒, scheme 不匹配丢。
 *       任何解析错误不弹窗, 不建窗; 只在 debug 输出计数。
 *
 * HKCU 挂钩: 幂等写 Software\Classes\snailsearch\shell\open\command; 从"设置-通用"开关触发。 */
#include "xjs_app.h"
#include <shlwapi.h>
#include "picojson.h"
#pragma comment(lib, "shlwapi.lib")

/* 进程共享 (方案 五.xjs_app.h): 冷启动 URL 暂存 + 顶层开关 */
std::wstring g_startupUrl;
bool g_urlProtocolEnabled = false;

/* ============== 内部工具 ============== */

/* UTF-8 → UTF-16; 复用 xjs_util 的 Utf8ToUtf16 (声明在 xjs_app.h, 定义在 xjs_util.cpp) */

/* 百分号解码 (RFC 3986 minimal): 只处理 %XX, 保留其它字节; 无效序列原样透传。
 * 输出 UTF-8 字节串, 供上层 Utf8ToUtf16 统一转宽。 '+' 不视为空格 (query 规范不强制)。 */
static void UrlPctDecode(const std::string& in, std::string* out) {
    out->clear();
    out->reserve(in.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == '%' && i + 2 < in.size()) {
            int hi = hex(in[i + 1]), lo = hex(in[i + 2]);
            if (hi >= 0 && lo >= 0) { *out += (char)((hi << 4) | lo); i += 2; continue; }
        }
        *out += in[i];
    }
}

/* 拆 k=v 对 (v 内可含 %, 不含裸 '&' — 上游 split 已按 '&' 断开) */
static bool SplitKv(const std::string& pair, std::string* k, std::string* v) {
    size_t eq = pair.find('=');
    if (eq == std::string::npos) { *k = pair; v->clear(); return !pair.empty(); }
    *k = pair.substr(0, eq);
    *v = pair.substr(eq + 1);
    return !k->empty();
}

/* ASCII 小写化 (仅 host / 键名 归一化用, 不动 UTF-8 高位) */
static std::string ToLowerAscii(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return s;
}

/* 位值解析: 1/true→1, 0/false→0, 其它→-1 (未指定) */
static int UrlBit(const std::wstring& v) {
    if (v == L"1" || v == L"true") return 1;
    if (v == L"0" || v == L"false") return 0;
    return -1;
}
/* 洗控制字符 + 限长 (模式名/描述/类型等短文本用) */
static std::wstring UrlClean(const std::wstring& v, size_t maxLen) {
    std::wstring s; for (wchar_t c : v) if (c >= 32 && c != 127) s += c;
    if (s.size() > maxLen) s.resize(maxLen);
    return s;
}
/* 解析 "a,b" 两个整数 (可带负号, 用于 pos=x,y / size=w,h)。真=两数皆合法。 */
static bool UrlPair(const std::wstring& v, int* a, int* b) {
    size_t c = v.find(L',');
    if (c == std::wstring::npos || c == 0 || c + 1 >= v.size()) return false;
    auto num = [](const std::wstring& s, int* out) {
        size_t i = (s[0] == L'-') ? 1 : 0;
        if (i >= s.size()) return false;
        for (; i < s.size(); i++) if (s[i] < L'0' || s[i] > L'9') return false;
        *out = _wtoi(s.c_str()); return true;
    };
    return num(v.substr(0, c), a) && num(v.substr(c + 1), b);
}
/* 路径列表 → 引擎 SearchFilterConfig JSON (UTF-8 窄串); 递归默认 true (与 ai-assistant 口径一致) */
static std::string UrlScopeJson(const std::vector<std::wstring>& paths) {
    picojson::array arr;
    for (auto& wp : paths) {
        picojson::object o;
        o["路径"] = picojson::value(Utf16ToUtf8(wp.c_str()));
        o["递归子目录"] = picojson::value(true);
        arr.push_back(picojson::value(o));
    }
    picojson::object root;
    root["搜索范围"] = picojson::value(arr);
    return picojson::value(root).serialize();
}
/* 按名称查找本窗可见的自定义模式 (名称忽略大小写; 作用范围经 XjsCmApplies) */
static const XjsCustomMode* UrlFindCustomMode(const std::wstring& name, const std::wstring& winName) {
    for (auto& cm : g_customModes)
        if (XjsCmApplies(cm, winName) && _wcsicmp(cm.name.c_str(), name.c_str()) == 0) return &cm;
    return NULL;
}

/* 解析结果 (P1 只装必要字段) */
struct XjsUrlReq {
    std::wstring action;              /* 归一化小写: new/open/focus (其它 → new) */
    /* q / qs: '|' 拆分多重段, 每段 = (模式索引 0..3 或 -1=本窗默认, 搜索词) */
    std::vector<std::pair<int, std::wstring>> stages;
    int mode = -1;                    /* -1 = 未指定; 0..4 内置有档 */
    std::wstring modeName;            /* 非空 = 按名称选用户自定义模式 (与 mode 互斥) */
    std::wstring view;                /* list/details, 空 = 未指定 */
    std::wstring skin;                /* 皮肤标识, 空 = 未指定 */
    int zoom = -1;                    /* -1 = 未指定; 5..20 */
    int preview = -1;                 /* -1 = 未指定; 0/1 */
    int save = 0;                     /* 0=临时 (缺省), 1=请求落盘 (P1 只打日志) */
    std::vector<std::wstring> scopePaths;   /* 搜索范围路径 (scope= 按 | 拆分, 应用时生成引擎 JSON) */
    /* URL 添加自定义搜索模式 (addmode/mtype/tpl/mdesc/priv): addMode 非空即触发新增 */
    std::wstring addMode, addDesc, addType, addTpl;
    int addPriv = 0;                        /* 1=窗口私有 (ownerName=本窗档案名), 0=全局共享 */
    /* 窗口初始几何 (像素): pos=x,y / size=w,h; -1 或未填 = 不改该项 */
    int winX = -1, winY = -1, winW = -1, winH = -1;
    std::string cfgJson;   /* cfg=<JSON>: 原样 UTF-8 配置对象, 交 XjsUrlApplyWindowConfig 写入本窗档案 */
    /* 匹配开关三态 (-1 未指定 / 0 关 / 1 开), 对齐 XjsMatchSettings 8 字段 */
    int mPf = -1, mPi = -1, mPe = -1, mWs = -1, mWq = -1, mFw = -1, mCs = -1, mEa = -1;
};

static bool UrlParse(const std::wstring& raw, XjsUrlReq* out) {
    /* scheme 前置: 大小写不敏感匹配 "snailsearch://" */
    const wchar_t* kScheme = L"snailsearch://";
    size_t kLen = wcslen(kScheme);
    if (raw.size() < kLen) return false;
    for (size_t i = 0; i < kLen; i++) {
        wchar_t a = raw[i], b = kScheme[i];
        if (a >= L'A' && a <= L'Z') a = (wchar_t)(a + 32);
        if (a != b) return false;
    }
    /* 载荷上限 (方案 四.2): URL 转 UTF-8 后 ≤ 32KB, 提前拦 */
    std::string u8 = Utf16ToUtf8(raw.c_str());
    if (u8.size() > 32 * 1024) return false;
    /* host + query */
    size_t pos = kLen;
    size_t qmark = raw.find(L'?', pos);
    std::string hostLow;
    {
        std::wstring hostW = (qmark == std::wstring::npos) ? raw.substr(pos) : raw.substr(pos, qmark - pos);
        hostLow = ToLowerAscii(Utf16ToUtf8(hostW.c_str()));
    }
    /* 动作白名单 (P1); 未知 → new (方案 四.4) */
    if (hostLow.empty() || hostLow == "new" || hostLow == "open") out->action = L"new";
    else if (hostLow == "focus") out->action = L"focus";
    else out->action = L"new";   /* reuse/search P2; 冷落 new 保持不报错 */

    /* query 拆对 */
    if (qmark != std::wstring::npos && qmark + 1 < raw.size()) {
        std::wstring tail = raw.substr(qmark + 1);
        std::string qs = Utf16ToUtf8(tail.c_str());
        size_t i = 0;
        while (i < qs.size()) {
            size_t amp = qs.find('&', i);
            std::string pair = qs.substr(i, (amp == std::string::npos) ? std::string::npos : amp - i);
            i = (amp == std::string::npos) ? qs.size() : amp + 1;
            std::string k, vEnc;
            if (!SplitKv(pair, &k, &vEnc)) continue;
            std::string vRaw;
            UrlPctDecode(vEnc, &vRaw);
            std::wstring keyL = Utf8ToUtf16(ToLowerAscii(k).c_str());
            std::wstring val = Utf8ToUtf16(vRaw.c_str());
            if (keyL == L"q" || keyL == L"qs") {
                /* 多重段分隔: 裸 '|' 或 '%7C'(大小写不敏感) 都认 —— 浏览器/命令行会把裸 '|' 编码成 '%7C',
                   两者必须等价。限制: 多重段内不再支持字面 '|' (通配符"或"/SQL || 请用单段)。
                   每段解码后取可选模式前缀 wild:/re:/sql:/lua: (大小写不敏感) → 该段模式 0/1/2/3;
                   无前缀 = -1 (本窗当前模式); 冒号前须精确命中白名单才当前缀 (Windows 路径 C:\ 不受影响)。 */
                std::vector<std::string> segsEnc;
                size_t start = 0, p = 0;
                while (p < vEnc.size()) {
                    size_t dl = 0;
                    if (vEnc[p] == '|') dl = 1;
                    else if (vEnc[p] == '%' && p + 2 < vEnc.size() && vEnc[p + 1] == '7' && (vEnc[p + 2] == 'C' || vEnc[p + 2] == 'c')) dl = 3;
                    if (dl) { segsEnc.push_back(vEnc.substr(start, p - start)); p += dl; start = p; }
                    else p++;
                }
                segsEnc.push_back(vEnc.substr(start));
                for (auto& segEnc : segsEnc) {
                    std::string dec; UrlPctDecode(segEnc, &dec);
                    std::wstring one = Utf8ToUtf16(dec.c_str());
                    if (one.empty()) continue;
                    int smode = -1;
                    size_t colon = one.find(L':');
                    if (colon != std::wstring::npos) {
                        std::wstring pre = one.substr(0, colon);
                        for (auto& c : pre) if (c >= L'A' && c <= L'Z') c = (wchar_t)(c + 32);
                        if      (pre == L"wild") smode = 0;
                        else if (pre == L"re")   smode = 1;
                        else if (pre == L"sql")  smode = 2;
                        else if (pre == L"lua")  smode = 3;
                        if (smode >= 0) one.erase(0, colon + 1);   /* 命中前缀才剥, 否则整段留作词 */
                    }
                    if (!one.empty()) out->stages.emplace_back(smode, one);
                }
            } else if (keyL == L"mode") {
                bool numeric = !val.empty();
                for (wchar_t c : val) if (c < L'0' || c > L'9') { numeric = false; break; }
                if (numeric) {
                    int n = _wtoi(val.c_str()); if (n >= 0 && n < XMODE_COUNT) out->mode = n;
                } else {
                    std::wstring nm; for (wchar_t c : val) if (c >= 32 && c != 127) nm += c;
                    if (nm.size() > 64) nm.resize(64);
                    out->modeName = nm;   /* 非纯数字 → 自定义模式名 */
                }
            } else if (keyL == L"view") {
                /* 五种视图 (对齐 XjsViewMode): dense 严密 / list 紧凑 / details 详情 / medium 中等图标 / large 大图标 */
                if (val == L"dense" || val == L"list" || val == L"details" || val == L"medium" || val == L"large") out->view = val;
            } else if (keyL == L"skin") {
                /* 长度限 64, 剥控制字符 */
                std::wstring clean;
                for (wchar_t c : val) if (c >= 32 && c != 127) clean += c;
                if (clean.size() > 64) clean.resize(64);
                out->skin = clean;
            } else if (keyL == L"zoom") {
                int n = _wtoi(val.c_str()); if (n >= 5 && n <= 20) out->zoom = n;
            } else if (keyL == L"preview") {
                if (val == L"0" || val == L"false") out->preview = 0;
                else if (val == L"1" || val == L"true") out->preview = 1;
            } else if (keyL == L"save") {
                if (val == L"1" || val == L"true") out->save = 1;
                else if (val == L"0" || val == L"false") out->save = 0;
            } else if (keyL == L"scope") {
                /* '|' 分隔多路径; 只收对对应路径 (含: 或 \) 且无通配符 *? */
                size_t j = 0;
                while (j <= val.size()) {
                    size_t bar = val.find(L'|', j);
                    std::wstring one = (bar == std::wstring::npos) ? val.substr(j) : val.substr(j, bar - j);
                    if (!one.empty()
                        && one.find_first_of(L"*?") == std::wstring::npos
                        && (one.find(L':') != std::wstring::npos || one.find(L'\\') != std::wstring::npos))
                        out->scopePaths.push_back(one);
                    if (bar == std::wstring::npos) break;
                    j = bar + 1;
                }
            } else if (keyL == L"pinyinfull")   { out->mPf  = UrlBit(val);
            } else if (keyL == L"pinyininitial"){ out->mPi  = UrlBit(val);
            } else if (keyL == L"pinyinexact")  { out->mPe  = UrlBit(val);
            } else if (keyL == L"wildstar")     { out->mWs  = UrlBit(val);
            } else if (keyL == L"wildq")        { out->mWq  = UrlBit(val);
            } else if (keyL == L"fullwidth")    { out->mFw  = UrlBit(val);
            } else if (keyL == L"casesensitive"){ out->mCs = UrlBit(val);
            } else if (keyL == L"emptyall")     { out->mEa = UrlBit(val);
            } else if (keyL == L"addmode")      { out->addMode = UrlClean(val, 64);
            } else if (keyL == L"mdesc")        { out->addDesc = UrlClean(val, 128);
            } else if (keyL == L"mtype")        { out->addType = UrlClean(val, 16);
            } else if (keyL == L"tpl")          { out->addTpl = val;   /* 模板原样 (含 <keyword> 占位符, 不洗控制字符) */
            } else if (keyL == L"priv")         { out->addPriv = (UrlBit(val) == 1) ? 1 : 0;
            } else if (keyL == L"pos")          { UrlPair(val, &out->winX, &out->winY);
            } else if (keyL == L"size")         { UrlPair(val, &out->winW, &out->winH);
            } else if (keyL == L"cfg")          { out->cfgJson = vRaw; }   /* 原样 UTF-8 JSON (含中文键), 不转 UTF-16 */
            /* 未知键: 忽略 (方案 四.4) */
        }
    }
    return true;
}

/* ============== 应用 ============== */

/* focus: 复用现有 XjsSummonActivate(主窗) 口径 (等价 WM_WAKEUP 处理路径) */
static void UrlFocusMain() {
    HWND h = XjsSearchWindow::MainHwnd();
    if (!h) return;
    ShowWindow(h, SW_RESTORE);
    SetForegroundWindow(h);
}

/* new (P1 唯一建窗路径, save=0/1 都走临时窗; save=1 时打日志) */
static void UrlDoNew(const XjsUrlReq& req, std::wstring* err) {
    if (g_isScanning) { if (err) *err = L"扫描中禁止新建窗口"; return; }
    XjsSearchWindow::OpenNew(-1);              /* 走既有 slot 分配 = XjsUiProfileCount() */
    XjsSearchWindow* w = NULL;
    int n = XjsSearchWindow::Count();
    if (n > 0) w = XjsSearchWindow::At(n - 1); /* 最新入表 = 刚建 */
    if (!w || !w->hWnd) { if (err) *err = L"窗口创建失败 (16 顶/资源不足)"; return; }
    w->urlTransient = true;                    /* 关窗即删的底座 (方案 一.4) */
    if (req.save == 1 && err && err->empty()) *err = L"save=1 暂按 save=0 处理 (P2 支持持久档案)";

    /* 字段覆写 (每窗): mode / view / preview / zoom; 皮肤走 XjsApplySkinToWindow 唯一入口 */
    XjsWindowScope scope(w);                   /* Cur() 切到 w, 后续 g_* 宏落到本窗 */
    if (req.mode >= 0) w->mode = req.mode;
    if (!req.view.empty()) {
        int vm = (int)w->viewMode;
        if      (req.view == L"dense")   vm = VM_DENSE;
        else if (req.view == L"list")    vm = VM_LIST;
        else if (req.view == L"details") vm = VM_DETAILS;
        else if (req.view == L"medium")  vm = VM_MEDIUM;
        else if (req.view == L"large")   vm = VM_LARGE;
        w->viewMode = (XjsViewMode)vm;
    }
    /* 预览开合走唯一正确路径 (同菜单 IDM 预览项 / 设置 ACT_PREVIEW): 裸赋值 previewVisible 不算完 ——
       关面板要顺带收灯箱/停媒体/结束插件接管会话 + XjsClampScroll 重排 + 落盘 + 全窗重绘,
       否则面板视觉残留 (表象 = "preview=0 关无效")。本函数已在 XjsWindowScope(w) 内, Cur()==w,
       XjsPreviewToggle 经 g_previewVisible 宏落到本窗。 */
    if (req.preview >= 0) {
        bool wantPrev = (req.preview != 0);
        if (w->previewVisible != wantPrev) XjsPreviewToggle();
    }
    if (req.zoom >= 5 && req.zoom <= 20 && req.zoom != w->uiZoom) {
        w->uiZoom = req.zoom;
        XjsRecreateTextFormats();              /* 缩放变化 = 文本格式按新倍率重建 (与设置页同口径) */
    }
    if (!req.skin.empty() && req.skin != w->skinName) {
        XjsApplySkinToWindow(w, req.skin);     /* 内部含 XjsSaveConfig (方案 一.4: 关窗即删仍会短暂污染, 已 P1 折中) */
    }
    /* 窗口初始几何: pos=x,y / size=w,h (任一给定即 SetWindowPos, 未给项沿用当前矩形) */
    if (req.winX >= 0 || req.winY >= 0 || req.winW > 0 || req.winH > 0) {
        RECT rc; GetWindowRect(w->hWnd, &rc);
        int x  = req.winX >= 0 ? req.winX : rc.left;
        int y  = req.winY >= 0 ? req.winY : rc.top;
        int ww = req.winW > 0  ? req.winW : (int)(rc.right - rc.left);
        int hh = req.winH > 0  ? req.winH : (int)(rc.bottom - rc.top);
        SetWindowPos(w->hWnd, NULL, x, y, ww, hh, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    /* cfg=<JSON>: 常规窗口设置项写入本窗档案并即时生效 (在几何应用后、搜索执行前) */
    if (!req.cfgJson.empty()) XjsUrlApplyWindowConfig(w, req.cfgJson);
    /* 搜索范围: 路径列表 → 引擎 JSON, 存每窗字段 (必须在下方 SetText 触发搜索前设好) */
    if (!req.scopePaths.empty()) w->searchScope = UrlScopeJson(req.scopePaths);
    /* 匹配开关: 三态命中才覆写, 有变更则下推引擎 (XjsApplyMatchSettingsFor 直传 w, 不依赖 Cur) */
    {
        XjsMatchSettings& mt = w->match; bool dirty = false;
        if (req.mPf >= 0) { mt.pinyinFull       = req.mPf != 0; dirty = true; }
        if (req.mPi >= 0) { mt.pinyinInitial    = req.mPi != 0; dirty = true; }
        if (req.mPe >= 0) { mt.pinyinExact      = req.mPe != 0; dirty = true; }
        if (req.mWs >= 0) { mt.wildcardStar     = req.mWs != 0; dirty = true; }
        if (req.mWq >= 0) { mt.wildcardQuestion = req.mWq != 0; dirty = true; }
        if (req.mFw >= 0) { mt.matchFullWidth   = req.mFw != 0; dirty = true; }
        if (req.mCs >= 0) { mt.caseSensitive    = req.mCs != 0; dirty = true; }
        if (req.mEa >= 0) { mt.emptyShowsAll    = req.mEa != 0; dirty = true; }
        if (dirty) XjsApplyMatchSettingsFor(w);
    }

    /* 搜索执行 (优先自定义模式名 → 多重/单段 q → 仅切模式):
       - 单段无前缀: 普通搜索 (本窗模式), XjsSearchSetText 即搜并记历史;
       - 单段带模式前缀: 先设本窗模式再普通搜索;
       - 多段: 组引擎 XJS_KEYWORD_MULTI JSON 链式过滤 (每段独立模式, 缺省用本窗当前模式)。 */
    if (!req.addMode.empty()) {
        /* URL 添加自定义搜索模式: 先建模式 (共享/私有) → 添加即用 (输入词静默入框 → 模式名转托管标签) */
        std::wstring owner = req.addPriv ? w->name : std::wstring();
        std::wstring mtype = req.addType.empty() ? L"wildcard" : req.addType;
        if (XjsCustomModeAdd(req.addMode, req.addDesc, mtype, req.addTpl, req.addPriv, owner)) {
            if (!req.stages.empty()) XjsSearchSetTextQuiet(req.stages.front().second);
            XjsHostedWordAsTag(req.addMode, false);
        } else if (err && err->empty()) {
            *err = L"添加搜索模式失败 (名称/模板为空或类型非法)";
        }
    } else if (!req.modeName.empty()) {
        const XjsCustomMode* cm = UrlFindCustomMode(req.modeName, w->name);
        if (cm) {
            if (!req.stages.empty()) XjsSearchSetTextQuiet(req.stages.front().second);
            XjsHostedWordAsTag(cm->name, false);
        } else if (!req.stages.empty()) {
            XjsSearchSetText(req.stages.front().second);   /* 模式名未命中 → 回落普通搜索 */
        }
    } else if (req.stages.size() >= 2) {
        int defMode = (req.mode >= 0) ? req.mode : (int)w->mode;   /* 无前缀段的缺省模式 */
        XjsUrlRunMultiStages(req.stages, defMode);   /* 每段做成内联托管标签显示于搜索框 + 引擎 JSON 链式提交 */
    } else if (req.stages.size() == 1) {
        if (req.stages[0].first >= 0) w->mode = req.stages[0].first;   /* 段前缀指定模式: 落到本窗再普通搜索 */
        XjsSearchSetText(req.stages[0].second);
    } else if (req.mode >= 0) {
        XjsSearchNow(false);                   /* 只切模式: 按新模式重搜现词 */
    }
    w->Invalidate();
}

bool XjsUrlApplyFromText(const wchar_t* url, std::wstring* err) {
    if (!url || !*url) return false;
    if (err) err->clear();
    /* 剥命令行/Shell 包裹的首尾双引号与空白: 注册表 command = `"%1"`, 第二实例/冷启动从命令行抓到的
       URL 尾部会粘一个 `"`, 使最后一个参数值被污染 (如 preview=0" 解析不上 → preview 维 -1)。
       冷启动与 WM_COPYDATA 两条送达均经此入口, 一处剥净即可。 */
    std::wstring clean = url;
    while (!clean.empty() && (clean.front() == L'"' || clean.front() == L'\'' || clean.front() == L' ' || clean.front() == L'\t'))
        clean.erase(clean.begin());
    while (!clean.empty() && (clean.back() == L'"' || clean.back() == L'\'' || clean.back() == L' ' || clean.back() == L'\t' || clean.back() == L'\r' || clean.back() == L'\n'))
        clean.pop_back();
    XjsUrlReq req;
    if (!UrlParse(clean, &req)) {
        if (err) *err = L"URL 格式非法";
        return false;
    }
    if (req.action == L"focus") { UrlFocusMain(); return true; }
    UrlDoNew(req, err);
    return true;
}

/* ============== 送达入口 (中 IL 侧调用) ============== */
bool XjsUrlSendCopyData(HWND target, const wchar_t* url) {
    if (!target || !url) return false;
    std::string u8 = Utf16ToUtf8(url);
    if (u8.size() > 32 * 1024) return false;
    u8 += '\0';                                 /* 保底终止 */
    COPYDATASTRUCT cds = { (ULONG_PTR)SNAIL_URL_MAGIC, (DWORD)u8.size(), (LPVOID)u8.data() };
    DWORD_PTR res = 0;
    BOOL ok = SendMessageTimeoutW(target, WM_COPYDATA, (WPARAM)target, (LPARAM)&cds,
        SMTO_ABORTIFHUNG | SMTO_NOTIMEOUTIFNOTHUNG, 3000, &res);
    return ok != 0;
}

/* ============== HKCU 挂钩 (方案 四.11 幂等) ============== */
static const wchar_t* kClsKey = L"Software\\Classes\\snailsearch";

bool XjsUrlIsRegisteredScheme() {
    HKEY h = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kClsKey, 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    wchar_t probe[8] = {0};
    DWORD sz = sizeof(probe), type = 0;
    LONG r = RegQueryValueExW(h, L"URL Protocol", NULL, &type, (LPBYTE)probe, &sz);
    RegCloseKey(h);
    return r == ERROR_SUCCESS;
}

void XjsUrlRegisterScheme(bool enable) {
    if (!enable) {
        /* 幂等删除 (整机键不存在也返回成功) */
        RegDeleteTreeW(HKEY_CURRENT_USER, kClsKey);
        return;
    }
    wchar_t exe[MAX_PATH * 2] = {0};
    if (!GetModuleFileNameW(NULL, exe, (DWORD)(sizeof(exe) / sizeof(wchar_t)))) return;
    HKEY hRoot = NULL, hShell = NULL, hOpen = NULL, hCmd = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kClsKey, 0, NULL, 0, KEY_WRITE, NULL, &hRoot, NULL) != ERROR_SUCCESS) return;
    RegSetValueExW(hRoot, NULL, 0, REG_SZ, (const BYTE*)L"URL:Snail Search Protocol", (DWORD)(sizeof(L"URL:Snail Search Protocol")));
    RegSetValueExW(hRoot, L"URL Protocol", 0, REG_SZ, (const BYTE*)L"", 2);
    if (RegCreateKeyExW(hRoot, L"shell", 0, NULL, 0, KEY_WRITE, NULL, &hShell, NULL) == ERROR_SUCCESS &&
        RegCreateKeyExW(hShell, L"open", 0, NULL, 0, KEY_WRITE, NULL, &hOpen, NULL) == ERROR_SUCCESS &&
        RegCreateKeyExW(hOpen, L"command", 0, NULL, 0, KEY_WRITE, NULL, &hCmd, NULL) == ERROR_SUCCESS) {
        std::wstring cmd = L"\"" ;
        cmd += exe;
        cmd += L"\" \"%1\"";
        RegSetValueExW(hCmd, NULL, 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    }
    if (hCmd) RegCloseKey(hCmd);
    if (hOpen) RegCloseKey(hOpen);
    if (hShell) RegCloseKey(hShell);
    if (hRoot) RegCloseKey(hRoot);
}
