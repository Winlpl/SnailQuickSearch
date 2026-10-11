/* xjs_url.cpp — snailsearch:// URL 协议 (P1)
 *
 * 传输: 中完整性侧 wWinMain 命中单实例 mutex → SendMessageTimeout(WM_COPYDATA, SNAIL_URL_MAGIC, COPYDATASTRUCT*);
 *       高完整性侧 WndProc WM_COPYDATA 分支校验魔数 + 大小 → UTF-8 URL → 本文件 XjsUrlApplyFromText。
 *       冷启动 (无实例): XjsAppMain 主窗建完读 g_startupUrl 应用一次。
 *
 * 语法: snailsearch://<action>[?<query>]
 *       动作 new/open(=new) = 建窗 (缺省临时窗) 或复用 win= 同名档案窗;
 *            focus = 唤起主窗; search/find = 唤起并就地应用到已有窗 (win= 指名, 未给/未命中回落主窗, 绝不建窗)。
 *       动作段取到第一个 '/' 为止 (尾斜杠不改语义), '#' 之后整段丢弃 (参数值里的 # 须写 %23)。
 *       focus/search 带任何窗口或搜索参数 = 唤起后把参数应用到那扇窗 (裸 focus 仍是纯唤起, 参数不再被静默吃掉)。
 *       参数 q|ms|mode|view|skin|zoom|preview|save|win|sort|order|scope|filter|cfg|匹配开关|
 *             addmode|mdesc|mtype|tpl|priv|delmode|cleartags|hotkey|settings|pos|size|run|reveal|props
 *       q = 单一主关键词 (原样, 不再按 '|' 分段/解 wild:/re:/sql:/lua: 模式前缀 —— 旧式多重写法已废弃)。
 *       ms=<原样 JSON> = 唯一的多重搜索链入口 (引擎 XJS_KEYWORD_MULTI 链式过滤): 每段 {"q":词,"mode":..},
 *       段词可含任意字符 (含 '|'/':'); 各段做成内联托管标签显示于搜索框, q 若同时给出则作为链末节追加在最后
 *       (见 UrlParseStagesJson / XjsUrlRunMultiMs)。mode 可为整数 0..4 或别名 wild/re/sql/lua/lua-exec (缺省回落本窗当前模式)。
 *       save=0 缺省: OpenNew(-1) 建临时窗 (w->urlTransient=true), 关窗走 DestroyAndFree 时移除档案槽;
 *       save=1 持久命名档案窗: win=<名> 命中同名存活窗则提到前台, 命中未打开档案则按槽重建, 否则新建并改名;
 *             无 win= 则建 GUID 自动名持久窗 (关窗档案保留, ☰启动器可按槽重建)。
 *       hotkey=<组合> (如 alt+a) 只对持久窗生效: 临时窗关窗即删, 把全局热键注册给它 = 死窗口占着组合键,
 *       故 hotkey= 一律忽略并弹提示告知用户 (写法非法 / 被别的程序占用 同样提示, 不静默丢弃)。
 *       sort=<字段>&order=asc|desc: 建窗即套用初始排序并写入本窗档案默认排序 (字段=引擎口径中文串)。
 *       scope=<JSON>: 搜索范围白名单, 与 ms 同口径 — [{"p":路径,"rec":bool}..] 或 ["路径"..] (rec 缺省 true=递归, false=仅本层);
 *       逐路径独立设递归, 只收绝对路径 (含: 或 \, 无通配 *?)。写每窗结构化白名单 (设置-搜索匹配"搜索范围"
 *       同一事实源, 随档案持久化)。旧式 '|' 分隔写法已废弃 (见 UrlParseScopeJson)。
 *
 * 校验: 一律按"不可信输入"处理 — 枚举 clamp, 未知键忽略, 载荷 >32KB 拒, scheme 不匹配丢。
 *       总口径 = 协议能搜、能改界面, 但**不能让本机运行它递来的程序**:
 *         · Lua 过滤/执行开放 —— 引擎虚拟机是纯检索沙箱 (危险函数已在引擎侧移除, 窗口结果对象也没有任何
 *           宿主注册的 C 函数可供脚本调用; 插件的 ai.write/ai.saveas 只挂在它自己的私有结果对象上),
 *           脚本再怎么写也只能查索引。**若将来有代码向进程/窗口结果对象注册 Lua C 函数, 必须回来重估这条**。
 *         · run= 两级 —— 可执行/脚本/宏/挂载类扩展名 (UrlExecExtension) 硬拒且不弹确认 (脚本内容用户读不懂,
 *           点"打开"等于盲签); 其余文档类才弹确认。reveal=/props= 只定位/看属性, 不执行内容, 不询问。
 *         · addmode/delmode 改动用户持久配置, 一律先弹确认摊开内容 (UrlConfirm)。
 *         · cfg 白名单不含 "打开提权" (决定子进程是否继承管理员令牌); skin 未知名不应用 (否则垃圾名落进档案);
 *           addmode 重名或满 100 = 拒绝 (不静默淘汰用户既有模式)。
 *         · 路径参数只收绝对路径、无通配、剥控制字符 (UrlAbsPath); scope 非 JSON 时回落单条绝对路径直写。
 *       任何解析错误不弹窗, 不建窗; 失败原因经 XjsUrlApplyFromText 的 err 出参回传 (调用方负责显示)。
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
/* (UrlScopeJson 已删: 白名单→引擎 JSON 序列化收口 xjs_engine.cpp XjsScopeFilterJson, 与设置页共用) */
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
        for (auto& c : wp) if (c == L'/') c = L'\\';   /* 同 UrlAbsPath 口径: 斜杠归一后再交给引擎 */
        if (wp.find_first_of(L"*?") == std::wstring::npos
            && (wp.find(L':') != std::wstring::npos || wp.find(L'\\') != std::wstring::npos))
            paths.emplace_back(wp, rec);
    }
    if (paths.empty()) return false;
    *out = std::move(paths);
    return true;
}

/* 文件动作的路径参数 (run=/reveal=/props=): 剥控制字符, 只收绝对路径 (盘符 "X:\" 或 UNC "\\") 且无通配符。
   长度上限 4000 字 (远超 MAX_PATH, 但 URL 载荷本就有 32KB 闸); 不合口径返回空 = 该动作不执行。 */
static std::wstring UrlAbsPath(const std::wstring& v) {
    std::wstring s;
    for (wchar_t c : v) if (c >= 32 && c != 127) s += c;
    /* 斜杠归一: 浏览器会把 URL 里的 '\' 规整成 '/' (chip 里写 C:\Windows\x.exe 实测就中招),
       而 ILCreateFromPathW / ShellExecute 只认反斜杠 —— 不归一就是"点了没反应" */
    for (auto& c : s) if (c == L'/') c = L'\\';
    s = XjsTrimWs(s);
    if (s.empty() || s.size() > 4000) return std::wstring();
    if (s.find_first_of(L"*?") != std::wstring::npos) return std::wstring();
    bool drive = s.size() >= 3 && s[1] == L':' && ((s[0] >= L'A' && s[0] <= L'Z') || (s[0] >= L'a' && s[0] <= L'z'))
              && s[2] == L'\\';
    bool unc = s.size() >= 2 && s[0] == L'\\' && s[1] == L'\\';
    return (drive || unc) ? s : std::wstring();
}

/* 名称列表 (filter= 等): JSON 字符串数组 ["图片","视频"] (解析走收口 XjsJsonStringArray, 不在 UI 模块摸 picojson),
   或 「、，,;；|」分隔单串; 去空白、去重保序、单项 ≤32 字。 */
static std::vector<std::wstring> UrlParseNames(const std::wstring& v) {
    std::vector<std::wstring> out;
    auto push = [&out](const std::wstring& raw) {
        std::wstring s = XjsTrimWs(raw);
        if (s.empty() || s.size() > 32) return;
        for (auto& p : out) if (p == s) return;
        out.push_back(s);
    };
    std::string u8 = Utf16ToUtf8(v.c_str());
    if (!u8.empty() && u8[0] == '[') {
        std::vector<std::wstring> arr;
        XjsJsonStringArray(u8.c_str(), &arr);
        for (auto& s : arr) push(s);
        if (!out.empty()) return out;
    }
    std::wstring cur;
    for (wchar_t c : v) {
        if (c == L'、' || c == L'，' || c == L',' || c == L';' || c == L'；' || c == L'|') { push(cur); cur.clear(); }
        else cur += c;
    }
    push(cur);
    return out;
}

/* hotkey=<修饰键+...+主键> (alt+a / ctrl+alt+f12 / win+space;分隔符 + 或 -,大小写不敏感)。
   口径与设置页录制完全一致: 必须带修饰键 —— 裸键 RegisterHotKey 也会成功, 但那等于把该键从全系统拦下来。
   出参 act: 1=设组合 2=清除 (none/off/0/无/清除) 3=写法不认识、缺主键或缺修饰键 (mod/vk 仅在 act=1 有意义)。
   减号键请写 minus (单独的 - 会被当成分隔符)。 */
static void UrlHotkey(const std::wstring& v, int* act, UINT* mod, UINT* vk) {
    std::wstring s; for (wchar_t c : v) if (c >= 32 && c != 127) s += c;
    std::wstring lo; for (wchar_t c : XjsTrimWs(s)) lo += (c >= L'A' && c <= L'Z') ? (wchar_t)(c + 32) : c;
    if (lo.empty() || lo == L"none" || lo == L"off" || lo == L"0" || lo == L"clear" || lo == L"无" || lo == L"清除") { *act = 2; return; }
    auto keyVk = [](const std::wstring& k) -> UINT {
        if (k.size() == 1) {
            wchar_t c = k[0];
            if (c >= L'a' && c <= L'z') return (UINT)(c - L'a' + L'A');
            if (c >= L'0' && c <= L'9') return (UINT)c;
            if (c == L',') return VK_OEM_COMMA;
            if (c == L'.') return VK_OEM_PERIOD;
        }
        if (k.size() > 1 && k[0] == L'f') { int n = _wtoi(k.c_str() + 1); if (n >= 1 && n <= 24) return (UINT)(VK_F1 + n - 1); }
        static const struct { const wchar_t* n; UINT vk; } T[] = {
            { L"space", VK_SPACE }, { L"enter", VK_RETURN }, { L"return", VK_RETURN }, { L"tab", VK_TAB },
            { L"esc", VK_ESCAPE }, { L"escape", VK_ESCAPE }, { L"backspace", VK_BACK }, { L"back", VK_BACK },
            { L"delete", VK_DELETE }, { L"del", VK_DELETE }, { L"insert", VK_INSERT }, { L"ins", VK_INSERT },
            { L"home", VK_HOME }, { L"end", VK_END }, { L"pageup", VK_PRIOR }, { L"pgup", VK_PRIOR },
            { L"pagedown", VK_NEXT }, { L"pgdn", VK_NEXT }, { L"up", VK_UP }, { L"down", VK_DOWN },
            { L"left", VK_LEFT }, { L"right", VK_RIGHT }, { L"minus", VK_OEM_MINUS }, { L"equals", VK_OEM_PLUS },
            { L"comma", VK_OEM_COMMA }, { L"period", VK_OEM_PERIOD }, { L"pause", VK_PAUSE },
            { L"capslock", VK_CAPITAL }, { L"numlock", VK_NUMLOCK }, { L"scrolllock", VK_SCROLL },
        };
        for (auto& e : T) if (k == e.n) return e.vk;
        return 0;
    };
    UINT m = 0, main = 0;
    size_t i = 0;
    while (i <= lo.size()) {
        size_t j = lo.find_first_of(L"+-", i);
        std::wstring tok = XjsTrimWs(lo.substr(i, (j == std::wstring::npos) ? std::wstring::npos : j - i));
        if (tok.empty()) { /* 空段 (连续分隔符或结尾) 跳过 */ }
        else if (tok == L"ctrl" || tok == L"control") m |= MOD_CONTROL;
        else if (tok == L"alt" || tok == L"menu") m |= MOD_ALT;
        else if (tok == L"shift") m |= MOD_SHIFT;
        else if (tok == L"win" || tok == L"windows" || tok == L"meta") m |= MOD_WIN;
        else { main = keyVk(tok); if (!main) { *act = 3; return; } }
        if (j == std::wstring::npos) break;
        i = j + 1;
    }
    if (!main || !m) { *act = 3; return; }   /* 缺主键 / 缺修饰键: 不猜, 交给调用方告知用户 */
    *act = 1; *mod = m; *vk = main;
}

/* 解析结果 (P1 只装必要字段) */
struct XjsUrlReq {
    std::wstring action;              /* 归一化小写: new/open/focus/search (其它 → new) */
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
    /* filter=<分类名..>: 文件分类多选 (「全部」= 不加筛选); 名称须取自引擎分类表, 未知名丢弃 */
    std::vector<std::wstring> filterNames;
    /* 文件动作 (按绝对路径, 与搜索可并存): run=用关联程序打开 (须用户确认), reveal=资源管理器定位, props=属性框 */
    std::wstring runPath, revealPath, propsPath;
    /* settings=<分类中文短名|1>: 打开设置窗直达该分类 (1=通用); delmode=<模式名>: 删除自定义搜索模式;
       cleartags=1: 清空本窗搜索框的托管标签链 (回到普通搜索) */
    std::wstring settingsCat, delMode;
    int clearTags = 0;
    /* hotkey=<组合>: 本窗全局快捷键。hkAct 0=未给 1=设 2=清除 3=写法非法/缺修饰键 (必须告知用户);
       只有持久窗能设 —— 临时窗关窗即删, 见 UrlApply 里的 urlTransient 判定 */
    int hkAct = 0; UINT hkMod = 0, hkVk = 0; std::wstring hkRaw;
    /* ms=<多重搜索链, 独立于 q>: 链先执行, UrlApply 把 q 作末段追加; 每段 = (模式 0..4 或 -1=本窗默认, 搜索词) */
    std::vector<std::pair<int, std::wstring>> msStages;
    /* 匹配开关三态 (-1 未指定 / 0 关 / 1 开), 对齐 XjsMatchSettings 8 字段 */
    int mPf = -1, mPi = -1, mPe = -1, mWs = -1, mWq = -1, mFw = -1, mCs = -1, mEa = -1;
};

static bool UrlParse(const std::wstring& rawIn, XjsUrlReq* out) {
    /* 片段 (#...) 先整段剥掉 (RFC 3986): 参数值里的 '#' 必须写成 %23, 裸 '#' 之后不属于载荷 */
    std::wstring raw = rawIn;
    size_t hash = raw.find(L'#');
    if (hash != std::wstring::npos) raw.erase(hash);
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
        /* 动作段只取到第一个 '/' — 手写与浏览器补的尾斜杠 ("focus/?q=x") 不得改变语义,
           否则 "focus/" 落进"未知动作回落 new", 唤起变成了新建临时窗 */
        size_t slash = hostW.find(L'/');
        if (slash != std::wstring::npos) hostW.erase(slash);
        hostLow = ToLowerAscii(Utf16ToUtf8(hostW.c_str()));
    }
    /* 动作白名单; 未知 → new (方案 四.4) */
    if (hostLow.empty() || hostLow == "new" || hostLow == "open") out->action = L"new";
    else if (hostLow == "focus") out->action = L"focus";
    else if (hostLow == "search" || hostLow == "find") out->action = L"search";
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
                    /* 先按模式别名定档 (与 ms 段 resolveMode 同口径) —— 否则 `mode=sql` 会被当成"名叫 sql 的自定义模式" */
                    std::wstring lo; for (wchar_t c : nm) lo += (c >= L'A' && c <= L'Z') ? (wchar_t)(c + 32) : c;
                    if (lo == L"wild" || lo == L"wildcard") out->mode = XMODE_WILDCARD;
                    else if (lo == L"re" || lo == L"regex") out->mode = XMODE_REGEX;
                    else if (lo == L"sql") out->mode = XMODE_SQL;
                    else if (lo == L"lua" || lo == L"lua-filter" || lo == L"luafilter") out->mode = XMODE_LUA;
                    else if (lo == L"lua-exec" || lo == L"luaexec" || lo == L"exec") out->mode = XMODE_LUA_EXEC;
                    else out->modeName = nm;   /* 其余 = 自定义模式名 */
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
                if (!UrlParseScopeJson(vRaw, &out->scopePaths)) {
                    /* 容错: 不是 JSON 就当单条绝对路径 —— `scope=D:\电影` 这种手写最常见, 同 UrlAbsPath 口径
                       (只收绝对路径、无通配符), 不放宽任何校验 */
                    std::wstring one = UrlAbsPath(val);
                    if (!one.empty()) out->scopePaths.push_back(std::make_pair(one, true));
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
            } else if (keyL == L"win" || keyL == L"name") { out->winName = UrlClean(val, 40);   /* save=1 持久命名档案窗名 (同重命名上限 40 字) */
            } else if (keyL == L"sort")         { out->sortField = UrlClean(val, 24);           /* 排序字段 (引擎口径: 文件名/文件大小/修改时间/创建时间/访问时间/文件评分/别名/文件夹/文件属性) */
            } else if (keyL == L"order") {
                std::wstring lo; for (wchar_t c : val) lo += (c >= L'A' && c <= L'Z') ? (wchar_t)(c + 32) : c;
                if (lo == L"asc" || lo == L"1" || lo == L"true") out->sortAsc = 1;
                else if (lo == L"desc" || lo == L"0" || lo == L"false") out->sortAsc = 0;
            } else if (keyL == L"cfg")          { out->cfgJson = vRaw; }   /* 原样 UTF-8 JSON (含中文键), 不转 UTF-16 */
            else if (keyL == L"filter" || keyL == L"cat") {
                /* filter = 文件分类 (筛选下拉, 多选): JSON ["图片","视频"] 或 图片、视频 分隔串, 「全部」= 不加筛选 */
                out->filterNames = UrlParseNames(val);
            } else if (keyL == L"run")          { out->runPath = UrlAbsPath(val);      /* 用关联程序打开 (须确认) */
            } else if (keyL == L"reveal")       { out->revealPath = UrlAbsPath(val);   /* 资源管理器定位选中 */
            } else if (keyL == L"props")        { out->propsPath = UrlAbsPath(val);    /* 系统属性框 */
            } else if (keyL == L"settings" || keyL == L"set") {
                out->settingsCat = UrlClean(val, 24);          /* 分类中文短名, 1/true = 通用页 */
            } else if (keyL == L"delmode")      { out->delMode = UrlClean(val, 64);
            } else if (keyL == L"cleartags")    { out->clearTags = (UrlBit(val) == 1) ? 1 : 0;
            } else if (keyL == L"hotkey")       {
                out->hkRaw = UrlClean(val, 32);
                UrlHotkey(val, &out->hkAct, &out->hkMod, &out->hkVk);
            }
            /* 未知键: 忽略 (方案 四.4) */
        }
    }
    return true;
}

/* ============== 应用 ============== */

/* URL 是否带"要改动窗口/搜索"的内容 — 决定 focus 是纯唤起, 还是唤起后在主窗就地应用
   (裸 focus 与带参 focus 的区别只在下方字段, save/win 属建窗维度, 不计入) */
static bool UrlHasPayload(const XjsUrlReq& r) {
    return !r.q.empty() || !r.msStages.empty() || !r.modeName.empty() || r.mode >= 0 || !r.view.empty()
        || !r.skin.empty() || r.zoom >= 0 || r.preview >= 0 || !r.scopePaths.empty() || !r.sortField.empty()
        || !r.cfgJson.empty() || !r.addMode.empty() || !r.filterNames.empty()
        || !r.runPath.empty() || !r.revealPath.empty() || !r.propsPath.empty()
        || !r.settingsCat.empty() || !r.delMode.empty() || r.clearTags != 0 || r.hkAct != 0
        || r.winX != kNoCoord || r.winY != kNoCoord || r.winW > 0 || r.winH > 0
        || r.mPf >= 0 || r.mPi >= 0 || r.mPe >= 0 || r.mWs >= 0 || r.mWq >= 0 || r.mFw >= 0 || r.mCs >= 0 || r.mEa >= 0;
}

/* 无槽新建并核实真的建出来了: OpenNew(-1) 在 16 窗上限时会 toast 后静默 return (一个窗都不建),
   也可能建了上下文但 CreateWindowExW 失败 (hWnd 空) —— 不核实的话 At(n-1) 拿到的是无辜的
   既有窗口, 随后被打 urlTransient 标记 (关窗 = 删掉用户真实档案) 或按 win= 改名 (档案名被夺)。
   核实口径 = 计数增长 (RegisterPending 是唯一追加点, 追加必在末位); 计数不增/末位 hWnd 空 = 未建成。
   (OpenNew 顶部会清扫历史失败残留的 hWnd 空上下文, 计数一减一增相抵时宁 可误报失败 —— 失败安全) */
static XjsSearchWindow* UrlOpenNewVerified(std::wstring* err) {
    int before = XjsSearchWindow::Count();
    XjsSearchWindow::OpenNew(-1);
    int after = XjsSearchWindow::Count();
    XjsSearchWindow* w = after > before ? XjsSearchWindow::At(after - 1) : NULL;
    if (!w || !w->hWnd) { if (err) *err = L"窗口创建失败 (16 顶/资源不足)"; return NULL; }
    return w;
}

/* new/open 的目标窗解析: save=0 临时窗 / save=1 持久命名档案窗 (复用存活窗、按槽重建或新建改名)。
   返回 NULL = 未建成 (err 已给原因)。 */
static XjsSearchWindow* UrlResolveWindow(const XjsUrlReq& req, std::wstring* err) {
    if (g_isScanning) { if (err) *err = L"扫描中禁止新建窗口"; return NULL; }
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
                    w = UrlOpenNewVerified(err);
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
            w = UrlOpenNewVerified(err);                                   /* 无名持久窗 (GUID) */
        }
    } else {
        w = UrlOpenNewVerified(err);               /* 临时窗: 走既有 slot 分配 = XjsUiProfileCount() */
        if (w) w->urlTransient = true;             /* 关窗即删的底座 (方案 一.4) */
    }
    if (!w || !w->hWnd) { if (err) *err = L"窗口创建失败 (16 顶/资源不足)"; return NULL; }
    return w;
}

/* 可执行 / 脚本 / 宏文档 / 挂载载体类扩展名 —— 双击就是运行代码 (lnk/url 是"指向别处的启动器",
   docm/xlsm 带宏, iso/vhd 挂载后 autorun 玩法)。URL 是不可信外部输入, 这类目标 **一律拒绝, 连确认框都不弹**:
   确认框只该用于用户能一眼判断价值的文档;脚本类内容用户读不懂, 点"打开"等于盲签。 */
static bool UrlExecExtension(const std::wstring& path) {
    size_t dot = path.rfind(L'.');
    if (dot == std::wstring::npos || dot + 1 >= path.size()) return false;
    std::wstring ext;
    for (wchar_t c : path.substr(dot + 1)) ext += (c >= L'A' && c <= L'Z') ? (wchar_t)(c + 32) : c;
    static const wchar_t* const kDeny[] = {
        L"exe", L"com", L"bat", L"cmd", L"msi", L"msp", L"msm", L"ps1", L"psm1", L"ps1xml", L"psc1",
        L"scr", L"cpl", L"hta", L"js", L"jse", L"vbs", L"vbe", L"ws", L"wsh", L"wsf", L"jar", L"lnk",
        L"url", L"reg", L"chm", L"msh", L"msh1", L"msh2", L"application", L"gadget", L"cab", L"iso",
        L"img", L"vhd", L"vhdx", L"docm", L"dotm", L"xlsm", L"xltm", L"xlam", L"pptm", L"potm", L"ppam",
        L"xll", L"settingcontent-ms", L"library-ms", L"searchconnector-ms", L"computerlauncher"
    };
    for (auto* d : kDeny) if (ext == d) return true;
    return false;
}

/* 追加一条回执: 被忽略/被拒的参数必须说出来 (已有消息就用分隔符续上, 后到的不覆盖前一条, 也不被前一条顶掉) */
static void UrlNote(std::wstring* err, const std::wstring& msg) {
    if (!err) return;
    if (!err->empty()) *err += L" · ";
    *err += msg;
}

/* 窗口名显示用: 自动名窗口是 GUID, 摊在确认框/菜单里根本读不动 → 截前 8 位 (想要像样的名字去 设置-窗口管理 改名) */
static std::wstring UrlWinLabel(const std::wstring& name) {
    if (name.size() == 36 && name[8] == L'-') return name.substr(0, 8) + L"…";
    return name;
}

/* 模式作用范围的人话 (确认框要摊开给用户看的东西不能只给 1/0): 私有模式带上归属窗口名 */
static std::wstring UrlModeScopeText(int scope, const std::wstring& owner) {
    return scope == 1 ? std::wstring(XjsT(L"菜单.仅本窗口")) + L" · " + UrlWinLabel(owner)
                      : std::wstring(XjsT(L"模式对话框.全局共享"));
}

/* 一条"添加自定义模式"的请求 (确认框上用户点了什么就怎么落地) */
struct XjsUrlAddReq { std::wstring name, desc, type, tpl, q; };

/* 落地添加 (目标窗 = t): 唤起该窗 → 建模式 (scope/owner 按用户选) → 填词 → 模式名转托管标签执行。
   失败只写 err (回执口径), 不弹窗。 */
static void UrlCommitAddMode(XjsSearchWindow* t, const XjsUrlAddReq& a, int scope, std::wstring* err) {
    if (!t || !t->hWnd) return;
    XjsSummonActivate(t->hWnd);
    XjsWindowScope sc(t);                       /* g_* 宏与搜索执行都落到这扇窗 */
    std::wstring owner = (scope == 1) ? t->name : std::wstring();
    if (!XjsCustomModeAdd(a.name, a.desc, a.type, a.tpl, scope, owner)) {
        UrlNote(err, L"添加搜索模式失败 (名称/模板为空、类型非法、同名已存在或模式数已达上限)");
        return;
    }
    if (!a.q.empty()) XjsSearchSetTextQuiet(a.q);
    XjsHostedWordAsTag(a.name, false);
    t->Invalidate();
}

/* "添加到别的窗口" 的两段式: 询问框是同步的, 窗口清单是异步弹窗菜单 (WM_POPUP_RESULT → XjsUrlPopupResult),
   中间把这条请求暂存下来。同一时刻只挂一条 (URL 送达本身串行), 新请求直接覆盖。 */
static XjsUrlAddReq g_pendAdd;
static bool g_pendAddOpen = false;

static void UrlOpenWindowPicker(XjsSearchWindow* from) {
    std::vector<XjsPopupItem> items;
    XjsPopupItem hd; hd.title = XjsT(L"URL协议.选窗口标题"); hd.header = true;
    items.push_back(hd);
    for (int i = 0; i < XjsSearchWindow::Count(); i++) {
        XjsSearchWindow* c = XjsSearchWindow::At(i);
        if (!c || !c->hWnd) continue;
        XjsPopupItem it;
        it.id = IDM_URL_BASE + i;
        it.title = UrlWinLabel(c->name);
        it.checked = (c == from);
        if (c->urlTransient) { it.sub = XjsT(L"URL协议.临时窗标记"); it.disabled = true; }   /* 私有模式随临时窗即删, 存进去等于丢进黑洞 */
        items.push_back(it);
    }
    RECT rc; GetWindowRect(from->hWnd, &rc);
    /* XSF 返 float, POINT 成员是 LONG —— 聚合初始化会报窄化警告, 显式取整 */
    POINT pt = { (LONG)(rc.left + (rc.right - rc.left) / 2 - XSF(120)), (LONG)(rc.top + (rc.bottom - rc.top) / 2) };
    XjsShowPopupMenu(from->hWnd, pt, items, XSF(260));
}

/* 外部输入要"改用户数据 / 打开文件" = 先把内容摊开给用户看再问一次; 真=用户同意。
   拒绝/取消也要留原因 (静默跳过 = 集成方不知道被谁拒了)。 */
static bool UrlConfirm(XjsSearchWindow* w, const wchar_t* titleKey, const std::wstring& body,
                       const wchar_t* okKey, const wchar_t* denyMsg, std::wstring* err) {
    int pick = XjsShowAskDialog(w->hWnd, XjsT(titleKey), body.c_str(),
        ("[{\"text\":\"" + XjsTUtf8(okKey) + "\",\"style\":\"primary\"},{\"text\":\"" +
          XjsTUtf8(L"通用词.取消") + "\"}]").c_str());
    if (pick == 0) return true;
    if (err && err->empty()) *err = XjsT(denyMsg);
    return false;
}

/* 把 req 携带的窗口覆写 + 搜索执行落到目标窗 (new/open 新建或复用的窗、focus 的主窗共用同一段) */
static void UrlApply(const XjsUrlReq& req, std::wstring* err, XjsSearchWindow* w) {
    if (!w || !w->hWnd) return;

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
        /* 未知名不应用: XjsSkinLoad 只是视觉回落默认, 名字却会落进本窗档案持久化
           (与插件 settings.set "皮肤" 的未知名 ERR_NOTFOUND 同一严格度, 校验口径取自 XjsSkinEnumerate) */
        bool known = false;
        for (auto& k : XjsSkinEnumerate()) if (k == req.skin) { known = true; break; }
        if (known) XjsApplySkinToWindow(w, req.skin);   /* 内部含 XjsSaveConfig (方案 一.4: 关窗即删仍会短暂污染, 已 P1 折中) */
        else if (err && err->empty()) *err = L"皮肤名不存在: " + req.skin;
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
    /* 全局快捷键 (hotkey=): 只对持久窗开放 —— 临时窗 (urlTransient) 关窗即删, 把组合键注册给它等于
       让一个不在了的窗口占着全局热键 (按下去没人认领), 所以直接忽略并告知用户:
       命令行里写了 hotkey= 却没进配置, 必须说出来, 不能静默丢掉。写法非法同口径。 */
    if (req.hkAct == 3) {
        UrlNote(err, L"hotkey 写法无法识别 (要 修饰键+主键, 如 alt+a / ctrl+alt+f12): " + req.hkRaw);
    } else if (req.hkAct && w->urlTransient) {
        UrlNote(err, L"临时窗口不保存快捷键, 已忽略 hotkey=" + req.hkRaw + L" (要持久请用 save=1&win=窗口名)");
    } else if (req.hkAct == 2) {
        if (!XjsHotkeyApplyToWindow(w, 0, 0)) UrlNote(err, L"快捷键清除失败 (窗口或主窗不可用)");
    } else if (req.hkAct == 1) {
        if (XjsHotkeyApplyToWindow(w, req.hkMod, req.hkVk)) { /* 成功: 窗口出现本身就是回执 */ }
        else UrlNote(err, L"快捷键注册失败 (已被其它程序占用): " + XjsHotkeyText(req.hkMod, req.hkVk));
    }
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
    /* 搜索范围: 路径列表 → 每窗结构化白名单 (与设置页"搜索范围"同一事实源, 随档案持久化),
       重建派生 JSON 并落盘; 必须在下方 SetText 触发搜索前设好, 使首搜即带上范围 */
    if (!req.scopePaths.empty()) {
        w->scopePaths = req.scopePaths;
        w->scopeOn = true;
        XjsScopeSyncFor(w);
        XjsSaveConfig();
    }
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
    /* 筛选分类 (filter=): 名称经引擎分类表校验后多选下发 (XjsApplyFilterNames 与筛选下拉同一落点, 自带重搜)。
       须在下方搜索执行之前 — 首搜即带上分类; 结果对象未就绪时静默跳过 (库未加载, 分类表也还没填)。 */
    if (!req.filterNames.empty()) {
        XjsEngineEnsureResult();
        if (XjsApplyFilterNames(req.filterNames) == 0 && err && err->empty())
            *err = L"筛选分类名未识别 (取自设置/下拉的分类表, 或传 \"全部\")";
    }
    /* 模式删除: delmode=<名> 命中范围 = 本窗可见的自定义模式 (名称是数据串不译, 与 addmode 写的名字直接对照)。
       摘表会令 UrlFindCustomMode 返回的指针失效, 故先取 id 再删。
       投毒闸: 删除是破坏性操作 (链接连发就能清空用户全部模式), 一律先确认。 */
    if (!req.delMode.empty()) {
        const XjsCustomMode* cm = UrlFindCustomMode(req.delMode, w->name);
        /* 摘表会令指针失效: id/类型/作用范围 先取成值再往下走 —— 同名可能同时存在"全局共享"与
           "仅本窗口"两条, 摊开这三样用户才知道要删的是哪一条 */
        std::wstring id, ctype, cscope;
        if (cm) { id = cm->id; ctype = cm->type; cscope = UrlModeScopeText(cm->scope, cm->ownerName); }
        if (id.empty()) {
            if (err && err->empty()) *err = L"搜索模式不存在 (本窗可见范围): " + req.delMode;
        } else {
            std::wstring body = XjsFmt(XjsT(L"URL协议.删除模式确认.说明"), req.delMode, ctype, cscope);
            if (UrlConfirm(w, L"URL协议.删除模式确认", body, L"通用词.删除", L"URL协议.已取消", err))
                XjsCustomModeRemoveById(id);
        }
    }
    /* 清空托管标签链 (回到普通搜索): 下方没有新搜索要执行时, 按现词重搜一次让结果集跟上 */
    if (req.clearTags) XjsHostedClearTags();

    /* 搜索执行 (优先自定义模式名 → ms JSON 多重链 → 单主词 q → 仅切模式):
       - ms 多重链: 引擎 XJS_KEYWORD_MULTI JSON 链式过滤 (每段独立模式), q 作链末主词;
       - 单主词 q: 普通搜索 (本窗模式), XjsSearchSetText 即搜并记历史。
       (旧式 q='a|b' 竖线分段 / 段前缀 wild:/re:/sql:/lua: 入口已删除, 多重一律走 ms) */
    if (!req.addMode.empty()) {
        /* 投毒闸: 模式一旦落盘就长期住在用户配置里, 用户之后每次点它都在执行这条模板 ——
           外部链接要植入, 必须把 名称/类型/作用范围/模板 摊开让用户亲自点头 */
        std::wstring mtype = req.addType.empty() ? L"wildcard" : req.addType;
        std::wstring body = XjsFmt(XjsT(L"URL协议.添加模式确认.说明"),
                                   req.addMode, mtype, UrlModeScopeText(req.addPriv, w->name),
                                   req.addTpl.empty() ? std::wstring(L"-") : req.addTpl);
        /* 去处由用户在确认框上定: 链接只能写死一个目标窗, 而"这窗口不是我要的"往往正是看到这张卡才想到。
           多窗时加第三个钮 = 弹窗口清单另选 (临时窗置灰, 选了也存不住)。
           按钮 JSON 逐项拼再统一收尾 —— 手写漏掉收尾 ']' 会让整串解析失败, 询问框退化成单个"确定",
           选择项和"取消"一起消失 (2026-10-10 实锤) */
        std::vector<std::pair<std::string, const char*>> bs;      /* {按钮文字, style} */
        bs.push_back({ XjsTUtf8(L"菜单.仅本窗口"), "primary" });
        bs.push_back({ XjsTUtf8(L"模式对话框.全局共享"), "default" });
        int idxOther = -1;
        if (XjsSearchWindow::Count() > 1) { bs.push_back({ XjsTUtf8(L"URL协议.选其它窗口"), "default" }); idxOther = 2; }
        bs.push_back({ XjsTUtf8(L"通用词.取消"), "default" });
        std::string btns = "[";
        for (size_t i = 0; i < bs.size(); i++) {
            if (i) btns += ",";
            btns += "{\"text\":\"" + bs[i].first + "\",\"style\":\"" + bs[i].second + "\"}";
        }
        btns += "]";
        XjsUrlAddReq a; a.name = req.addMode; a.desc = req.addDesc; a.type = mtype; a.tpl = req.addTpl; a.q = req.q;
        int pick = XjsShowAskDialog(w->hWnd, XjsT(L"URL协议.添加模式确认"), body.c_str(), btns.c_str());
        if (pick == 0)      UrlCommitAddMode(w, a, 1, err);
        else if (pick == 1) UrlCommitAddMode(w, a, 0, err);
        else if (idxOther >= 0 && pick == idxOther) { g_pendAdd = a; g_pendAddOpen = true; UrlOpenWindowPicker(w); }
        else UrlNote(err, XjsT(L"URL协议.已取消"));
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
    /* 只清标签、没给任何搜索内容 = 按现词重搜一次 (XjsHostedClearTags 本身静默不触发搜索,
       不清这一下结果集还停在被清掉的链上) */
    if (req.clearTags && req.q.empty() && req.msStages.empty() && req.modeName.empty()
        && req.addMode.empty() && req.delMode.empty() && req.mode < 0) XjsSearchNow(false);
    /* 文件动作 (与搜索可并存): reveal/props 只定位/看属性, 不执行内容 = 直接做;
       run= 是"用关联程序打开", 而本进程与宿主同权限 (管理员) —— 可执行/脚本/宏/挂载类目标硬拒且不弹确认
       (脚本内容用户读不懂, 点"打开"等于盲签), 其余文档类才询问一次 */
    if (!req.revealPath.empty()) {
        /* 存在性先验 + 落地返回值都要说话: 路径不在 / shell 没接住 = 用户看到的是"点了没反应" */
        if (GetFileAttributesW(req.revealPath.c_str()) == INVALID_FILE_ATTRIBUTES)
            UrlNote(err, L"定位失败, 路径不存在: " + req.revealPath);
        else if (!XjsOpenFolderAndSelect(req.revealPath))
            UrlNote(err, L"定位失败 (取不到该路径的位置): " + req.revealPath);
    }
    if (!req.propsPath.empty()) {
        if (GetFileAttributesW(req.propsPath.c_str()) != INVALID_FILE_ATTRIBUTES) XjsShowPropertiesPath(req.propsPath);
        else UrlNote(err, L"看属性失败, 路径不存在: " + req.propsPath);
    }
    if (!req.runPath.empty()) {
        if (UrlExecExtension(req.runPath)) {
            if (err && err->empty()) *err = L"已拒绝打开: 该文件属可执行/脚本类, URL 协议不运行任何程序";
        } else {
            std::wstring body = XjsFmt(XjsT(L"URL协议.打开确认.说明"), req.runPath);
            if (UrlConfirm(w, L"URL协议.打开确认", body, L"通用词.打开", L"URL协议.已取消", err)) {
                XjsWindowScope s2(w); XjsOpenFile(req.runPath);   /* 询问框同步泵会重绑"当前窗", 打开前重新认窗 (打开行为三项是每窗设置) */
            }
        }
    }
    /* settings=<分类中文短名|1>: 打开设置窗并绑定本窗直达该页 (分类下标经 XjsSettingsCatByName 现取,
       外部不碰 SC_* 枚举); XjsSettingsShow 认 Cur() 作 owner, 故在重新认窗的作用域里调 */
    if (!req.settingsCat.empty()) {
        std::wstring cat = (req.settingsCat == L"1" || req.settingsCat == L"true") ? std::wstring(L"通用") : req.settingsCat;
        int sc = -1;
        if (XjsSettingsCatByName(cat, &sc)) { XjsWindowScope s3(w); XjsSettingsShow(sc); }
        else if (err && err->empty())
            *err = L"设置分类名未识别: " + cat + L" (可用: 通用/窗口/外观/列表/搜索匹配/打开/窗口管理/索引/分类与别名/性能与内存/捐赠/搜索模式/关于)";
    }
    w->Invalidate();
}

/* 唤起一个已有窗并把参数落到它 (focus/search 共用 —— 这两个动作都不建窗)。
   裸唤起与带参唤起都走 XjsSummonActivate 借前台口径 (裸 ShowWindow+SetForegroundWindow 是红线禁写,
   且跨完整性/前台锁场景不稳); 带参数时参数不得静默丢弃 (旧 focus 在此 return, q 整条被吃掉) */
static void UrlApplyToExisting(const XjsUrlReq& req, std::wstring* err, XjsSearchWindow* w) {
    if (!w || !w->hWnd) { if (err) *err = L"没有可用窗口"; return; }
    XjsSummonActivate(w->hWnd);
    if (UrlHasPayload(req)) UrlApply(req, err, w);
}

/* search/find 的目标窗: win=<档案名> 命中存活窗, 未给或未命中回落主窗 (不建窗: 建窗是 new 的语义) */
static XjsSearchWindow* UrlPickLiveWindow(const std::wstring& winName) {
    if (!winName.empty()) {
        for (int i = 0; i < XjsSearchWindow::Count(); i++) {
            XjsSearchWindow* c = XjsSearchWindow::At(i);
            if (c && c->hWnd && c->name == winName) return c;
        }
    }
    return XjsSearchWindow::Main();
}

/* IDM_URL_BASE 段菜单结果 = 确认框上选了"其它窗口…"的那条添加, 落到所选窗口 (私有)。
   真 = 属本段并已处理 (main.cpp 据此结束分发); 越界 = false 交回其它分支。 */
bool XjsUrlPopupResult(int id) {
    if (id < IDM_URL_BASE || id >= IDM_URL_BASE + 64) return false;
    if (!g_pendAddOpen) return true;                 /* 菜单重影/迟到的结果: 吞掉, 不落到别的分支 */
    g_pendAddOpen = false;
    XjsUrlAddReq a = g_pendAdd;
    XjsSearchWindow* t = XjsSearchWindow::At(id - IDM_URL_BASE);
    if (!t || !t->hWnd || t->urlTransient) return true;   /* 窗口已关 或 临时窗 (菜单里本就置灰) */
    std::wstring err;
    UrlCommitAddMode(t, a, 1, &err);
    if (!err.empty()) XjsToastShow(XjsSearchWindow::MainHwnd(), err.c_str(), XTOAST_WARN, XSF(1));
    return true;
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
    if (req.action == L"focus") { UrlApplyToExisting(req, err, XjsSearchWindow::Main()); return true; }
    if (req.action == L"search") { UrlApplyToExisting(req, err, UrlPickLiveWindow(req.winName)); return true; }
    UrlApply(req, err, UrlResolveWindow(req, err));
    return true;
}

/* ============== 送达入口 (中 IL 侧调用) ============== */

/* 从命令行里剥出 snailsearch:// URL (scheme 大小写不敏感)。
   截断口径 = 引号优先: 注册表 command 形如 "%1", URL 常整段被双引号包住, 内含未编码空格时
   (批处理 start "" "snailsearch://new?q=a b" / 运行对话框手打) 按"第一个空格"截会丢后半参数。
   故: scheme 前紧邻 '"' = 引号段, 收在闭合引号 (空格照收); 否则收在第一个空白或引号。 */
std::wstring XjsUrlExtractFromCmdLine(const wchar_t* cmd) {
    if (!cmd || !*cmd) return std::wstring();
    std::wstring s = cmd;
    const wchar_t* kScheme = L"snailsearch://";
    const size_t kLen = 14;
    size_t at = std::wstring::npos;
    for (size_t i = 0; i + kLen <= s.size(); i++) {
        if (_wcsnicmp(s.c_str() + i, kScheme, kLen) == 0) { at = i; break; }
    }
    if (at == std::wstring::npos) return std::wstring();
    bool quoted = at > 0 && s[at - 1] == L'"';
    size_t end = s.size();
    for (size_t i = at; i < s.size(); i++) {
        wchar_t c = s[i];
        if (c == L'"') { end = i; break; }
        if (!quoted && (c == L' ' || c == L'\t')) { end = i; break; }
    }
    return s.substr(at, end - at);
}

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
