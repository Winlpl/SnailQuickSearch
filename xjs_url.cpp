/* xjs_url.cpp — snailsearch:// URL 协议 (P1)
 *
 * 传输: 中完整性侧 wWinMain 命中单实例 mutex → SendMessageTimeout(WM_COPYDATA, SNAIL_URL_MAGIC, COPYDATASTRUCT*);
 *       高完整性侧 WndProc WM_COPYDATA 分支校验魔数 + 大小 → UTF-8 URL → 本文件 XjsUrlApplyFromText。
 *       冷启动 (无实例): XjsAppMain 主窗建完读 g_startupUrl 应用一次。
 *
 * 语法: snailsearch://<action>[?<query>]  (动作: new/open(=new)/focus)
 *       (支持参数: q|ms|mode|view|skin|zoom|preview|save|win|sort|order|scope|cfg|匹配开关|addmode|pos|size 等)
 *       q = 单一主关键词 (原样, 不再按 '|' 分段/解 wild:/re:/sql:/lua: 模式前缀 —— 旧式多重写法已废弃)。
 *       ms=<原样 JSON> = 唯一的多重搜索链入口 (引擎 XJS_KEYWORD_MULTI 链式过滤): 每段 {"q":词,"mode":..},
 *       段词可含任意字符 (含 '|'/':'); 各段做成内联托管标签显示于搜索框, q 若同时给出则作为链末节追加在最后
 *       (见 UrlParseStagesJson / XjsUrlRunMultiMs)。mode 可为整数 0..4 或别名 wild/re/sql/lua/lua-exec (缺省回落本窗当前模式)。
 *       save=0 缺省: OpenNew(-1) 建临时窗 (w->urlTransient=true), 关窗走 DestroyAndFree 时移除档案槽;
 *       save=1 持久命名档案窗: win=<名> 命中同名存活窗则提到前台, 命中未打开档案则按槽重建, 否则新建并改名;
 *             无 win= 则建 GUID 自动名持久窗 (关窗档案保留, ☰启动器可按槽重建)。
 *       sort=<字段>&order=asc|desc: 建窗即套用初始排序并写入本窗档案默认排序 (字段=引擎口径中文串)。
 *       scope=<JSON>: 搜索范围白名单, 与 ms 同口径 — [{"p":路径,"rec":bool}..] 或 ["路径"..] (rec 缺省 true=递归, false=仅本层);
 *       逐路径独立设递归, 只收绝对路径 (含: 或 \, 无通配 *?)。旧式 '|' 分隔写法已废弃 (见 UrlParseScopeJson)。
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
/* pos= 坐标哨兵: 屏幕坐标可为负 (主屏左/上方的副屏), 不能用 -1 当"未给"; 取 INT_MIN。 */
static const int kNoCoord = -2147483647 - 1;
/* 路径列表(各带递归标志) → 引擎 SearchFilterConfig JSON (UTF-8 窄串); 每路径独立"递归子目录" */
static std::string UrlScopeJson(const std::vector<std::pair<std::wstring,bool>>& paths) {
    picojson::array arr;
    for (auto& pr : paths) {
        picojson::object o;
        o["路径"] = picojson::value(Utf16ToUtf8(pr.first.c_str()));
        o["递归子目录"] = picojson::value(pr.second);
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

/* 多重搜索原样 JSON (ms= 的 JSON 形式): 段词可含 '|'/':'/中文等任意字符, 不受 '|' 分隔符口径限制。
   载荷 = percent-decode 后的 UTF-8 JSON, 顶层可为数组 (逐元素) 或 {"stages":[..]} 包裹, 每元素:
     - 字符串                → 该段搜索词, 模式回落本窗当前 (mode=-1);
     - {"q":"词","mode":..}  → 词取 q|word|kw|搜索词 首个命中; mode 取 mode|m|搜索模式,
                               可为整数 0..4 或别名 wild/wildcard|re/regex|sql|lua|lua-exec (缺省/非法=-1)。
   解析失败 / 非上述结构 / 无有效段 → 返回 false, 调用方(ms)忽略该值(不再回落 '|' 分段, 旧式分段入口已删)。 */
static bool UrlParseStagesJson(const std::string& utf8, std::vector<std::pair<int, std::wstring>>* out) {
    picojson::value v;
    if (!picojson::parse(v, utf8).empty()) return false;
    const picojson::array* arrp = NULL;
    if (v.is<picojson::array>()) arrp = &v.get<picojson::array>();
    else if (v.is<picojson::object>()) {           /* 容错: 也接受 {"stages":[..]} 包裹 */
        picojson::object& o = v.get<picojson::object>();
        for (const char* kk : { "stages", "\u6bb5", "\u591a\u91cd" }) {   /* stages / 段 / 多重 */
            auto it = o.find(kk);
            if (it != o.end() && it->second.is<picojson::array>()) { arrp = &it->second.get<picojson::array>(); break; }
        }
    }
    if (!arrp) return false;
    auto resolveMode = [](const picojson::value& mv) -> int {
        if (mv.is<double>()) { int n = (int)mv.get<double>(); return (n >= 0 && n < XMODE_COUNT) ? n : -1; }
        if (mv.is<std::string>()) {
            std::string s = mv.get<std::string>();
            for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            if (s == "wild" || s == "wildcard") return XMODE_WILDCARD;
            if (s == "re" || s == "regex")      return XMODE_REGEX;
            if (s == "sql")                     return XMODE_SQL;
            if (s == "lua" || s == "lua-filter" || s == "luafilter") return XMODE_LUA;
            if (s == "lua-exec" || s == "luaexec" || s == "exec")    return XMODE_LUA_EXEC;
            bool num = !s.empty(); for (char c : s) if (c < '0' || c > '9') num = false;
            if (num) { int n = atoi(s.c_str()); return (n >= 0 && n < XMODE_COUNT) ? n : -1; }
        }
        return -1;
    };
    std::vector<std::pair<int, std::wstring>> stages;
    for (auto& e : *arrp) {
        std::string wordUtf8; int mode = -1;
        if (e.is<std::string>()) wordUtf8 = e.get<std::string>();
        else if (e.is<picojson::object>()) {
            const picojson::object& o = e.get<picojson::object>();
            for (const char* wk : { "q", "word", "kw", "\u641c\u7d22\u8bcd" }) {   /* q/word/kw/搜索词 */
                auto it = o.find(wk);
                if (it != o.end() && it->second.is<std::string>()) { wordUtf8 = it->second.get<std::string>(); break; }
            }
            for (const char* mk : { "mode", "m", "\u641c\u7d22\u6a21\u5f0f" }) {   /* mode/m/搜索模式 */
                auto it = o.find(mk);
                if (it != o.end()) { mode = resolveMode(it->second); break; }
            }
        }
        if (wordUtf8.empty()) continue;
        stages.emplace_back(mode, Utf8ToUtf16(wordUtf8.c_str()));
    }
    if (stages.empty()) return false;
    *out = std::move(stages);
    return true;
}

/* 搜索范围 JSON (scope= 的 JSON 形式, 与 ms 同口径): 载荷 = percent-decode 后的 UTF-8 JSON,
   顶层可为数组 (逐元素) 或 {"搜索范围"/"范围"/"scope":[..]} 包裹, 每元素:
     - 字符串              → 路径, 递归子目录缺省 true;
     - {"p":"路径",...}  → 路径取 p|path|路径|word; 递归取 rec|recursive|递归|递归子目录 (bool/0/1/false, 缺省 true)。
   只收绝对路径 (含: 或 \) 且无通配符 *?。解析失败 / 无有效路径 → 返回 false。 */
static bool UrlParseScopeJson(const std::string& utf8, std::vector<std::pair<std::wstring,bool>>* out) {
    picojson::value v;
    if (!picojson::parse(v, utf8).empty()) return false;
    const picojson::array* arrp = NULL;
    if (v.is<picojson::array>()) arrp = &v.get<picojson::array>();
    else if (v.is<picojson::object>()) {           /* 容错: 也接受 {"搜索范围":[..]} 包裹 */
        picojson::object& o = v.get<picojson::object>();
        for (const char* kk : { "\u641c\u7d22\u8303\u56f4", "\u8303\u56f4", "scope" }) {   /* 搜索范围 / 范围 / scope */
            auto it = o.find(kk);
            if (it != o.end() && it->second.is<picojson::array>()) { arrp = &it->second.get<picojson::array>(); break; }
        }
    }
    if (!arrp) return false;
    auto resolveRec = [](const picojson::value& rv) -> bool {
        if (rv.is<bool>()) return rv.get<bool>();
        if (rv.is<double>()) return rv.get<double>() != 0.0;
        if (rv.is<std::string>()) {
            std::string s = rv.get<std::string>();
            for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            if (s == "0" || s == "false" || s == "no" || s == "off") return false;
        }
        return true;
    };
    std::vector<std::pair<std::wstring,bool>> paths;
    for (auto& e : *arrp) {
        std::string pathUtf8; bool rec = true;
        if (e.is<std::string>()) pathUtf8 = e.get<std::string>();
        else if (e.is<picojson::object>()) {
            const picojson::object& o = e.get<picojson::object>();
            for (const char* wk : { "p", "path", "\u8def\u5f84", "word" }) {   /* p/path/路径/word */
                auto it = o.find(wk);
                if (it != o.end() && it->second.is<std::string>()) { pathUtf8 = it->second.get<std::string>(); break; }
            }
            for (const char* rk : { "rec", "recursive", "\u9012\u5f52", "\u9012\u5f52\u5b50\u76ee\u5f55" }) {   /* rec/recursive/递归/递归子目录 */
                auto it = o.find(rk);
                if (it != o.end()) { rec = resolveRec(it->second); break; }
            }
        }
        if (pathUtf8.empty()) continue;
        std::wstring wp = Utf8ToUtf16(pathUtf8.c_str());
        if (wp.find_first_of(L"*?") == std::wstring::npos
            && (wp.find(L':') != std::wstring::npos || wp.find(L'\\') != std::wstring::npos))
            paths.emplace_back(wp, rec);
    }
    if (paths.empty()) return false;
    *out = std::move(paths);
    return true;
}

/* 解析结果 (P1 只装必要字段) */
struct XjsUrlReq {
    std::wstring action;              /* 归一化小写: new/open/focus (其它 → new) */
    std::wstring q;                   /* 主关键词 q/qs: 单一、原样 (旧式 '|' 分段/模式前缀已废弃; 多重过滤一律走 ms JSON) */
    int mode = -1;                    /* -1 = 未指定; 0..4 内置有档 */
    std::wstring modeName;            /* 非空 = 按名称选用户自定义模式 (与 mode 互斥) */
    std::wstring view;                /* list/details, 空 = 未指定 */
    std::wstring skin;                /* 皮肤标识, 空 = 未指定 */
    int zoom = -1;                    /* -1 = 未指定; 5..20 */
    int preview = -1;                 /* -1 = 未指定; 0/1 */
    int save = 0;                     /* 0=临时窗 (缺省, 关窗即删), 1=持久命名档案窗 (win= 指定名, 关窗档案保留) */
    std::vector<std::pair<std::wstring,bool>> scopePaths;   /* 搜索范围 (scope= JSON): 每项 = (路径, 是否递归子目录 rec 缺省 true) */
    /* URL 添加自定义搜索模式 (addmode/mtype/tpl/mdesc/priv): addMode 非空即触发新增 */
    std::wstring addMode, addDesc, addType, addTpl;
    int addPriv = 0;                        /* 1=窗口私有 (ownerName=本窗档案名), 0=全局共享 */
    /* 窗口初始几何 (像素): pos=x,y / size=w,h; winX/winY 哨兵 kNoCoord(坐标可负), winW/winH 哨兵 -1(尺寸必正) = 不改该项 */
    int winX = kNoCoord, winY = kNoCoord, winW = -1, winH = -1;
    /* save=1 持久命名档案窗的窗口名 (win=/name=); 空 = GUID 自动名 */
    std::wstring winName;
    /* 初始排序 (sort=字段 & order=asc/desc): sortField 空=不改; sortAsc -1=未指定/0=降序/1=升序 */
    std::wstring sortField; int sortAsc = -1;
    std::string cfgJson;   /* cfg=<JSON>: 原样 UTF-8 配置对象, 交 XjsUrlApplyWindowConfig 写入本窗档案 */
    /* ms=<多重搜索链, 独立于 q>: 链先执行, UrlDoNew 把 q 作末段追加; 每段 = (模式 0..4 或 -1=本窗默认, 搜索词) */
    std::vector<std::pair<int, std::wstring>> msStages;
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
    /* 动作白名单; 未知 → new (方案 四.4) */
    if (hostLow.empty() || hostLow == "new" || hostLow == "open") out->action = L"new";
    else if (hostLow == "focus") out->action = L"focus";
    else out->action = L"new";   /* 未知动作回落 new, 保持不报错 */

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
                out->q = val;                     /* 单一主关键词, 原样 (不再按 '|' 分段/解模式前缀) */
            } else if (keyL == L"ms" || keyL == L"multi") {
                /* ms = 唯一的多重搜索链入口 (JSON, 独立于 q): 链先执行, q(若给)作末段追加在最后 (见 UrlDoNew)。
                   仅接受原样 JSON [{"q","mode"}] (段词可含 '|'); 旧式 '|' 分段入口已删除。 */
                UrlParseStagesJson(vRaw, &out->msStages);
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
                /* scope = JSON (与 ms 同口径): [{"p":路径,"rec":bool}..] 或 ["路径"..], 顶层可 {"搜索范围":[..]} 包裹;
                   每路径独立"递归子目录"(rec 缺省 true)。旧式 '|' 分隔 + '!' 前缀已废弃。 */
                UrlParseScopeJson(vRaw, &out->scopePaths);
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
            } else if (keyL == L"win" || keyL == L"name") { out->winName = UrlClean(val, 40);   /* save=1 持久命名档案窗名 (同重命名上限 40 字) */
            } else if (keyL == L"sort")         { out->sortField = UrlClean(val, 24);           /* 排序字段 (引擎口径: 文件名/文件大小/修改时间/创建时间/访问时间/文件评分/别名/文件夹/文件属性) */
            } else if (keyL == L"order") {
                std::wstring lo; for (wchar_t c : val) lo += (c >= L'A' && c <= L'Z') ? (wchar_t)(c + 32) : c;
                if (lo == L"asc" || lo == L"1" || lo == L"true") out->sortAsc = 1;
                else if (lo == L"desc" || lo == L"0" || lo == L"false") out->sortAsc = 0;
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

/* new: 建窗主路径 — save=0 临时窗 / save=1 持久命名档案窗 (复用存活窗、按槽重建或新建改名) */
static void UrlDoNew(const XjsUrlReq& req, std::wstring* err) {
    if (g_isScanning) { if (err) *err = L"扫描中禁止新建窗口"; return; }
    XjsSearchWindow* w = NULL;
    if (req.save == 1) {
        /* 持久命名档案窗: 同名存活窗复用(提到前台) → 同名未打开档案按槽重建 → 否则新建并按 win= 改名。
           不设 urlTransient (关窗档案保留, ☰启动器可按槽重建)。 */
        if (!req.winName.empty()) {
            for (int i = 0; i < XjsSearchWindow::Count(); i++) {           /* ① 存活窗同名 */
                XjsSearchWindow* c = XjsSearchWindow::At(i);
                if (c && c->hWnd && c->name == req.winName) { w = c; XjsSummonActivate(c->hWnd); break; }
            }
            if (!w) {
                int slot = -1;
                for (int i = 1; i < XjsUiProfileCount(); i++) {            /* ② 未打开档案同名 (主窗槽0名固定不参与) */
                    XjsUiProfile* p = XjsUiProfileAt(i);
                    if (p && p->name == req.winName) { slot = i; break; }
                }
                if (slot > 0) { XjsSearchWindow::OpenNew(slot); w = XjsSearchWindow::AtSlot(slot); }
                else {                                                       /* ③ 全新: 建后改名 */
                    XjsSearchWindow::OpenNew(-1);
                    int n = XjsSearchWindow::Count();
                    if (n > 0) w = XjsSearchWindow::At(n - 1);
                    if (w && w->hWnd && !XjsSearchWindow::NameInUse(req.winName.c_str(), w)) {
                        XjsUiProfile* p = XjsUiProfileAt(w->uiIndex);
                        std::wstring old = w->name;
                        if (p) p->name = req.winName;
                        w->name = req.winName;
                        XjsCmRenameWindow(old, req.winName);
                    }
                }
            }
        } else {
            XjsSearchWindow::OpenNew(-1);                                  /* 无名持久窗 (GUID) */
            int n = XjsSearchWindow::Count();
            if (n > 0) w = XjsSearchWindow::At(n - 1);
        }
    } else {
        XjsSearchWindow::OpenNew(-1);              /* 临时窗: 走既有 slot 分配 = XjsUiProfileCount() */
        int n = XjsSearchWindow::Count();
        if (n > 0) w = XjsSearchWindow::At(n - 1); /* 最新入表 = 刚建 */
        if (w && w->hWnd) w->urlTransient = true;  /* 关窗即删的底座 (方案 一.4) */
    }
    if (!w || !w->hWnd) { if (err) *err = L"窗口创建失败 (16 顶/资源不足)"; return; }

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
    if (req.winX != kNoCoord || req.winY != kNoCoord || req.winW > 0 || req.winH > 0) {
        RECT rc; GetWindowRect(w->hWnd, &rc);
        int x  = req.winX != kNoCoord ? req.winX : rc.left;
        int y  = req.winY != kNoCoord ? req.winY : rc.top;
        int ww = req.winW > 0  ? req.winW : (int)(rc.right - rc.left);
        int hh = req.winH > 0  ? req.winH : (int)(rc.bottom - rc.top);
        SetWindowPos(w->hWnd, NULL, x, y, ww, hh, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    /* cfg=<JSON>: 常规窗口设置项写入本窗档案并即时生效 (在几何应用后、搜索执行前) */
    if (!req.cfgJson.empty()) XjsUrlApplyWindowConfig(w, req.cfgJson);
    /* cfg 可能改写了 appearPos(出现位置), 但建窗时的 WM_ACTIVATE 早于此步、已按旧档(=0)定过位 ——
       未显式给 pos= 时按新档立即重定位一次 (等价于"关闭→回托盘→再唤起"才生效的那步, 提前到 URL 应用当下;
       appearPos 无效/为 0 时 ApplyAppearPos 内部自会 return; 与 WM_ACTIVATE 侧同口径跳过最大化/最小化) */
    if (req.winX == kNoCoord && req.winY == kNoCoord && !IsIconic(w->hWnd) && !IsZoomed(w->hWnd))
        w->ApplyAppearPos();
    /* 初始排序 (sort=字段 & order=asc/desc): 写入本窗档案默认排序 + 立即对结果对象套用。
       sortField 用引擎口径 UTF-8 串 (文件名/文件大小/修改时间/创建时间/访问时间/文件评分/别名/文件夹/文件属性);
       order 缺省 (-1) 按降序 (与新建窗口档案默认档一致)。须在下方 SetText 触发搜索前设好, 使首搜即按此序出结果。 */
    if (!req.sortField.empty()) {
        bool asc = (req.sortAsc >= 0) ? (req.sortAsc == 1) : false;
        XjsUiProfile* sp = XjsUiProfileAt(w->uiIndex);
        if (sp) { sp->sortField = req.sortField; sp->sortWay = asc; }   /* 落档: ☰启动器按槽重建时沿用 */
        XjsEngineEnsureResult();
        std::string sf = Utf16ToUtf8(req.sortField.c_str());
        XjsResultSetSortField(g_result, sf.c_str(), asc ? TRUE : FALSE);
    }
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

    /* 搜索执行 (优先自定义模式名 → ms JSON 多重链 → 单主词 q → 仅切模式):
       - ms 多重链: 引擎 XJS_KEYWORD_MULTI JSON 链式过滤 (每段独立模式), q 作链末主词;
       - 单主词 q: 普通搜索 (本窗模式), XjsSearchSetText 即搜并记历史。
       (旧式 q='a|b' 竖线分段 / 段前缀 wild:/re:/sql:/lua: 入口已删除, 多重一律走 ms) */
    if (!req.addMode.empty()) {
        /* URL 添加自定义搜索模式: 先建模式 (共享/私有) → 添加即用 (输入词静默入框 → 模式名转托管标签) */
        std::wstring owner = req.addPriv ? w->name : std::wstring();
        std::wstring mtype = req.addType.empty() ? L"wildcard" : req.addType;
        if (XjsCustomModeAdd(req.addMode, req.addDesc, mtype, req.addTpl, req.addPriv, owner)) {
            if (!req.q.empty()) XjsSearchSetTextQuiet(req.q);
            XjsHostedWordAsTag(req.addMode, false);
        } else if (err && err->empty()) {
            *err = L"添加搜索模式失败 (名称/模板为空或类型非法)";
        }
    } else if (!req.modeName.empty()) {
        const XjsCustomMode* cm = UrlFindCustomMode(req.modeName, w->name);
        if (cm) {
            if (!req.q.empty()) XjsSearchSetTextQuiet(req.q);
            XjsHostedWordAsTag(cm->name, false);
        } else if (!req.q.empty()) {
            XjsSearchSetText(req.q);   /* 模式名未命中 → 回落普通搜索 */
        }
    } else if (!req.msStages.empty()) {
        /* ms 多重链 (JSON) 先执行, q(主词)作为链末节追加 ("先多重、q 放最后") */
        int defMode = (req.mode >= 0) ? req.mode : (int)w->mode;   /* 无前缀段/q 的缺省模式 */
        XjsUrlRunMultiMs(req.msStages, req.q, defMode, defMode);
    } else if (!req.q.empty()) {
        XjsSearchSetText(req.q);                   /* 单主词普通搜索 (本窗模式), 即搜并记历史 */
    } else if (req.mode >= 0) {
        XjsSearchNow(false);                       /* 只切模式: 按新模式重搜现词 */
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
