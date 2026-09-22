/*
 * ai_assistant.cpp — 蜗牛快搜(D2D) 插件「AI 助手」(preview-panel 面板接管, 一比一对齐正式版 ai-assistant)
 * 形态 = manifest.json + 本 DLL: 接管预览面板内容区, GDI+ 软渲染整块位图交付, 宿主转发鼠标/键盘/IME。
 * 正式版口径: 状态栏"AI 助手"打开; 预览面板没开自动展开 (关闭按打开前状态恢复); 聊天期间选中文件
 * 不覆盖聊天 (宿主守卫); 协议 = OpenAI Responses API (POST {baseUrl}/responses, SSE 流式)。
 * 演示点: ①面板接管全套 (交付位图世代对齐 / 鼠标滚轮键盘 IME / 尺寸自适应)
 *         ②WinHTTP SSE 流式 (工作线程只攒增量, 经消息窗口回 UI 线程渲染 — 渲染不进工作线程)
 *         ③存储 cfg (API Key 按机器码 XOR 混淆, 明文不落盘) + 多对话历史
 *         ④深度思考 (reasoning.effort=high) 可折叠推理过程块
 *         ⑤Agent 工具循环 (Responses API function calling): 模型可多轮调用 run_search/
 *           open_file/copy_paths 工具 — 引擎直连在插件私有结果对象上执行搜索, 工具结果
 *           回填对话继续推理, 直到给出最终答复; 系统提示词 = 角色说明 + 引擎内嵌的
 *           xjs_LUA_GetPprompt(0/1) 两份 Lua 脚本规范 (过滤模式/执行模式)。
 */
#define NOMINMAX
#include <windows.h>
#include <gdiplus.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <cstring>
#include <atomic>
#include <thread>

#include "../../xjs_plugin_sdk.h"
#include "../../xunjieso.h"   /* 引擎直连: 加载器按模块名绑到宿主进程已加载的同名 DLL 实例 */

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "winhttp.lib")

/* ==================== 宿主接口 (Init 时存下, 指针终身有效) ==================== */

static const XjsPluginHost* g_host = NULL;
static XjsPluginCtx*        g_ctx  = NULL;
static ULONG_PTR            g_gdipToken = 0;
static HWND                 g_msgwnd = NULL;    /* 消息窗口: 工作线程 PostMessage 回 UI 线程渲染 (渲染不进工作线程) */
static unsigned             g_uiThread = 0;

static const UINT XJS_AI_STREAM = WM_APP + 40;  /* 流式增量到达 (wParam=0, lParam=Job*) */
static const UINT XJS_AI_SWEEP  = WM_APP + 41;  /* 孤儿作业清扫 */

static bool HostHas(unsigned need) {   /* 宿主表按"只追加"扩字段: 取用前先验 size (旧宿主干净失败) */
    return g_host && g_host->size >= need;
}
#define HOST_PANEL_OK (HostHas(offsetof(XjsPluginHost, PanelSetCaret) + sizeof(void*)))

/* ==================== 基础工具 ==================== */

static std::wstring W8(const char* s) {
    if (!s || !*s) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);   /* n 含终止 NUL */
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
static std::wstring TrimW(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}
/* 代理对安全步进 (caret 按 UTF-16 单元计) */
static size_t PrevCp(const std::wstring& s, size_t i) {
    while (i > 0 && (s[i - 1] & 0xFC00) == 0xDC00 && i > 1 && (s[i - 2] & 0xFC00) == 0xD800) i -= 2;
    if (i > 0) i -= 1;
    return i;
}
static size_t NextCp(const std::wstring& s, size_t i) {
    if (i < s.size()) {
        i += 1;
        if (i < s.size() && (s[i - 1] & 0xFC00) == 0xD800 && (s[i] & 0xFC00) == 0xDC00) i += 1;
    }
    return i;
}

/* ---- base64 (API Key 混淆用) ---- */
static const char* B64C = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string B64Enc(const std::string& in) {
    std::string out;
    size_t i = 0;
    while (i + 2 < in.size()) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8) | (unsigned char)in[i + 2];
        out += B64C[(v >> 18) & 63]; out += B64C[(v >> 12) & 63];
        out += B64C[(v >> 6) & 63];  out += B64C[v & 63];
        i += 3;
    }
    if (i + 1 == in.size()) {
        unsigned v = (unsigned char)in[i] << 16;
        out += B64C[(v >> 18) & 63]; out += B64C[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8);
        out += B64C[(v >> 18) & 63]; out += B64C[(v >> 12) & 63]; out += B64C[(v >> 6) & 63]; out += "=";
    }
    return out;
}
static std::string B64Dec(const std::string& in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    int buf = 0, bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\r' || c == '\n') continue;
        int v = val(c);
        if (v < 0) return out;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out += (char)((buf >> bits) & 0xFF); }
    }
    return out;
}

/* ---- 简易 JSON (SSE 载荷/配置/历史 都是小型文档, 自带解析器零依赖) ---- */
struct Jv {
    int t = 0;   /* 0=null 1=bool 2=num 3=string 4=array 5=object */
    bool b = false;
    double num = 0;
    std::wstring str;
    std::vector<Jv> arr;
    std::vector<std::pair<std::wstring, Jv>> obj;
    const Jv* Get(const wchar_t* key) const {
        if (t != 5) return NULL;
        for (auto& kv : obj) if (kv.first == key) return &kv.second;
        return NULL;
    }
    std::wstring S(const wchar_t* key, const wchar_t* def = L"") const {
        const Jv* v = Get(key);
        return (v && v->t == 3) ? v->str : def;
    }
};
struct JParser {
    const wchar_t* p;
    const wchar_t* end;
    bool ok = false;
    explicit JParser(const std::wstring& s) : p(s.c_str()), end(s.c_str() + s.size()) {}
    void Ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++; }
    Jv Num() {
        Jv v; v.t = 2;
        wchar_t* stop = NULL;
        v.num = wcstod(p, &stop);
        if (stop == p) { p++; return v; }
        p = stop;
        ok = true;
        return v;
    }
    Jv Val() {
        Jv v;
        Ws();
        if (p >= end) return v;
        wchar_t c = *p;
        if (c == '{') {
            v.t = 5; p++;
            Ws();
            if (p < end && *p == '}') { p++; ok = true; return v; }
            for (;;) {
                Ws();
                Jv k = (p < end && *p == '"') ? Str() : Jv();
                Ws();
                if (p < end && *p == ':') p++;
                v.obj.push_back({ k.str, Val() });
                Ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == '}') p++;
                break;
            }
            ok = true;
            return v;
        }
        if (c == '[') {
            v.t = 4; p++;
            Ws();
            if (p < end && *p == ']') { p++; ok = true; return v; }
            for (;;) {
                v.arr.push_back(Val());
                Ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == ']') p++;
                break;
            }
            ok = true;
            return v;
        }
        if (c == '"') return Str();
        if (c == 't') { p += 4; v.t = 1; v.b = true; ok = true; return v; }
        if (c == 'f') { p += 5; v.t = 1; v.b = false; ok = true; return v; }
        if (c == 'n') { p += 4; return v; }
        /* 裸词容错: 旧版转义函数漏引号写出的存量 (配置/历史) 值是裸文本 — 此前单字符跳过
           会让对象/数组解析提前断掉, 后续字段 (apiKeyEnc/会话消息) 全丢 = key 与会话记录
           "消失"。吞到分隔符整段取值: 纯数字按数, 其余按字符串; 新版写入恒为带引号合法
           JSON, 不走此径 */
        {
            const wchar_t* q = p;
            while (q < end && *q != L',' && *q != L']' && *q != L'}' && *q != L'\n' && *q != L'\r') q++;
            if (q > p) {
                wchar_t* stop = NULL;
                v.num = wcstod(p, &stop);
                if (stop == q) { v.t = 2; p = q; ok = true; return v; }
                v.t = 3;
                v.str.assign(p, q);
                p = q;
                ok = true;
                return v;
            }
        }
        return Num();
    }
    Jv Str() {
        Jv v; v.t = 3; p++;   /* 跳开引号 */
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) {
                p++;
                switch (*p) {
                    case '"': v.str += '"'; break;
                    case '\\': v.str += '\\'; break;
                    case '/': v.str += '/'; break;
                    case 'n': v.str += '\n'; break;
                    case 't': v.str += '\t'; break;
                    case 'r': v.str += '\r'; break;
                    case 'b': v.str += '\b'; break;
                    case 'f': v.str += '\f'; break;
                    case 'u': {
                        if (p + 4 < end) {
                            wchar_t cp = (wchar_t)wcstol(std::wstring(p + 1, p + 5).c_str(), NULL, 16);
                            v.str += cp;
                            p += 4;
                        }
                        break;
                    }
                    default: v.str += *p; break;
                }
                p++;
            } else {
                v.str += *p++;
            }
        }
        if (p < end) p++;   /* 闭引号 */
        ok = true;
        return v;
    }
};
static std::string JsonEscapeUtf8(const std::wstring& s) {   /* → 完整 JSON 字符串字面量 (含首尾引号;
    全部调用点 (请求体/配置/历史) 均直接拼接不再手动包引号 — 曾漏引号发裸词, DeepSeek 400
    "model expected value at line 1 column 10" 即值起始处解析失败 */
    std::string u = U8(s);
    std::string out = "\"";
    for (unsigned char c : u) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; sprintf_s(b, "\\u%04X", c); out += b; }
                else out += (char)c;
        }
    }
    out += "\"";
    return out;
}
static Jv JsonParseW(const std::wstring& text) {
    JParser jp(text);
    return jp.Val();
}

/* ==================== 配置 (存储键 "cfg"; API Key 按机器码 XOR 混淆, 明文不落盘) ==================== */

struct AiCfg {
    std::wstring baseUrl = L"https://api.deepseek.com";
    std::wstring model = L"deepseek-flash";   /* Responses API 仅新模型名可用 (deepseek-flash / deepseek-v4-pro;
                                                 deepseek-chat 等旧名在此端点被拒) */
    std::wstring apiKey;
    bool reasoning = false;   /* 深度思考: true=effort high, false=none (DeepSeek 须显式传) */
};
static AiCfg g_cfg;

static std::wstring MachineKeyStr() {   /* 机器码: 注册表 MachineGuid, 缺失回退常量 (换机解密失败=重输) */
    wchar_t buf[128] = {};
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid",
                     RRF_RT_REG_SZ, NULL, buf, &sz) == ERROR_SUCCESS && buf[0])
        return std::wstring(buf) + L"|snail-ai-key";
    return std::wstring(L"snail-ai-key-fallback");
}
static std::string XorSecret(const std::string& plain, const std::string& key) {
    std::string out = plain;
    for (size_t i = 0; i < out.size(); i++) out[i] ^= key[i % key.size()];
    return out;
}
static bool ValidUtf8(const std::string& s) {
    if (s.empty()) return false;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), NULL, 0);
    return n > 0;
}
static void CfgSave() {
    if (!g_host) return;
    /* JSON 字面量拼接: 先 UTF-8 转义再转回宽字符 (纯 ASCII 字面量), 整体一次落盘 */
    auto lit = [](const std::wstring& s) { return W8(JsonEscapeUtf8(s).c_str()); };
    std::wstring j = L"{\"baseUrl\":" + lit(g_cfg.baseUrl) + L",\"model\":" + lit(g_cfg.model);
    if (!g_cfg.apiKey.empty()) {
        std::string enc = B64Enc(XorSecret(U8(g_cfg.apiKey), U8(MachineKeyStr())));
        j += L",\"apiKeyEnc\":\"enc:1:" + W8(enc.c_str()) + L"\"";
    }
    j += g_cfg.reasoning ? L",\"reasoning\":true}" : L",\"reasoning\":false}";
    std::string u8 = U8(j);
    g_host->StorageSet(g_ctx, "cfg", u8.c_str(), (int)u8.size());
}
static void CfgLoad() {
    if (!g_host) return;
    char buf[4096];
    int n = g_host->StorageGet(g_ctx, "cfg", buf, (int)sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = 0;
    Jv v = JsonParseW(W8(buf));
    if (v.t != 5) return;
    std::wstring b = v.S(L"baseUrl");
    if (!b.empty()) g_cfg.baseUrl = b;
    std::wstring m = v.S(L"model");
    if (!m.empty()) g_cfg.model = m;
    const Jv* r = v.Get(L"reasoning");
    if (r && r->t == 1) g_cfg.reasoning = r->b;
    std::wstring enc = v.S(L"apiKeyEnc");
    if (enc.rfind(L"enc:1:", 0) == 0) {
        std::string raw = B64Dec(U8(enc.substr(6)));
        std::string plain = XorSecret(raw, U8(MachineKeyStr()));
        g_cfg.apiKey = ValidUtf8(plain) ? W8(plain.c_str()) : L"";   /* 换机解出乱码 = 视为未配置 */
    }
}

/* ==================== 多对话历史 (存储键 "历史"; 上限 30 会话/每会话 200 条) ==================== */

struct AiToolStep {             /* 一次工具调用 (live 态, 只在当前会话消息流; 不落历史) */
    int kind = 0;               /* 0=run_search 1=open_file 2=copy_paths (未知工具照显 name) */
    std::wstring name;          /* 工具名 (模型传回; 未知工具也照显) */
    int state = 0;              /* 0=排队 1=执行中 2=完成 3=失败 */
    std::wstring mode, query;   /* run_search 参数 */
    int count = -1;             /* run_search 命中总数 */
    long long elapsedMs = -1;
    std::wstring err;           /* 失败原因 */
    std::vector<std::wstring> top;   /* 结果样本路径 (≤20; 展开显示) */
    bool open = false;          /* 样本列表展开态 (UI 态; 泵同步镜像时保留) */
};
struct AiMsg {
    int role = 0;               /* 0=user 1=assistant 2=工具步骤组 (role==2 不落历史/不重发) */
    std::wstring text;
    std::wstring reason;        /* 推理过程 (只在折叠块显示, 从不发送/入库发送体) */
    bool reasonOpen = false;    /* 折叠块展开态 (流式中自动展开, 完成后收起) */
    std::vector<AiToolStep> steps;   /* role==2: 本组工具步骤 */
};
struct AiConv {
    unsigned long long id = 0;
    long long t = 0;            /* 最后活动时间 (time_t 秒) */
    std::wstring title;         /* 首条用户消息 (≤30 字) */
    std::vector<AiMsg> msgs;
};
static std::vector<AiConv> g_hist;
static unsigned long long g_nextConvId = 1;
static const size_t AI_CONV_MAX = 30, AI_MSG_MAX = 200, AI_TEXT_MAX = 60000;
static const int AI_AGENT_MAX_TURNS = 8;   /* agent 工具循环上限 (最后一轮省略 tools 强制收尾) */

static std::wstring ConvTitleOf(const std::wstring& firstUser) {
    std::wstring t = TrimW(firstUser);
    if (t.size() > 30) { t = t.substr(0, 30); t += L"…"; }
    return t;
}
static void HistSave() {
    if (!g_host) return;
    std::wstring j = L"[";
    for (size_t i = 0; i < g_hist.size(); i++) {
        const AiConv& c = g_hist[i];
        if (i) j += L",";
        wchar_t head[96];
        swprintf(head, 96, L"{\"id\":%llu,\"t\":%lld,\"title\":", c.id, c.t);
        j += head;
        j += W8(JsonEscapeUtf8(c.title).c_str());
        j += L",\"msgs\":[";
        for (size_t k = 0; k < c.msgs.size(); k++) {
            const AiMsg& m = c.msgs[k];
            if (k) j += L",";
            j += m.role ? L"{\"r\":1,\"text\":" : L"{\"r\":0,\"text\":";
            j += W8(JsonEscapeUtf8(m.text).c_str());
            if (!m.reason.empty()) {
                j += L",\"reason\":";
                j += W8(JsonEscapeUtf8(m.reason).c_str());
            }
            j += L"}";
        }
        j += L"]}";
    }
    j += L"]";
    std::string u8 = U8(j);
    g_host->StorageSet(g_ctx, "历史", u8.c_str(), (int)u8.size());
}
static void HistLoad() {
    g_hist.clear();
    if (!g_host) return;
    /* 上限裁剪在装载侧做: 存储值无硬上限, 读入 4MB 足够 30 会话满载 */
    std::string buf;
    buf.resize(4 * 1024 * 1024);
    int n = g_host->StorageGet(g_ctx, "历史", &buf[0], (int)buf.size() - 1);
    if (n <= 0) return;
    buf.resize((size_t)n);
    buf.push_back(0);
    Jv v = JsonParseW(W8(buf.c_str()));
    if (v.t != 4) return;
    for (auto& jc : v.arr) {
        if (jc.t != 5) continue;
        AiConv c;
        c.id = (unsigned long long)jc.S(L"id").empty() ? (unsigned long long)(jc.Get(L"id") ? jc.Get(L"id")->num : 0)
                                                       : (unsigned long long)wcstoull(jc.S(L"id").c_str(), NULL, 10);
        const Jv* jt = jc.Get(L"t");
        if (jt && jt->t == 2) c.t = (long long)jt->num;
        c.title = jc.S(L"title");
        const Jv* jm = jc.Get(L"msgs");
        if (jm && jm->t == 4) {
            for (auto& jmsg : jm->arr) {
                if (jmsg.t != 5) continue;
                AiMsg m;
                const Jv* jr = jmsg.Get(L"r");
                m.role = (jr && jr->num == 1) ? 1 : 0;
                m.text = jmsg.S(L"text");
                m.reason = jmsg.S(L"reason");
                if (m.text.empty() && m.reason.empty()) continue;
                if (c.msgs.size() < AI_MSG_MAX) c.msgs.push_back(m);
            }
        }
        if (!c.msgs.empty()) {
            g_hist.push_back(c);
            if (c.id >= g_nextConvId) g_nextConvId = c.id + 1;
        }
    }
    while (g_hist.size() > AI_CONV_MAX) g_hist.erase(g_hist.begin());   /* 最旧丢弃 */
}
/* 当前会话落库 (curId=0 → 新建; 否则原位更新), 返回会话 id; 工具步骤组 (role==2) 是 live 态不落库 */
static unsigned long long HistUpsert(unsigned long long curId, const std::vector<AiMsg>& msgs) {
    std::wstring firstUser;
    for (auto& m : msgs)
        if (m.role == 0 && !m.text.empty()) { firstUser = m.text; break; }
    if (firstUser.empty()) return curId;
    AiConv c;
    c.id = curId ? curId : g_nextConvId++;
    c.t = (long long)time(NULL);
    c.title = ConvTitleOf(firstUser);
    for (auto& m : msgs)
        if (m.role != 2) c.msgs.push_back(m);
    while (c.msgs.size() > AI_MSG_MAX) c.msgs.erase(c.msgs.begin());
    for (size_t i = 0; i < g_hist.size(); i++) {
        if (g_hist[i].id == c.id) { g_hist[i] = c; HistSave(); return c.id; }
    }
    g_hist.push_back(c);
    while (g_hist.size() > AI_CONV_MAX) g_hist.erase(g_hist.begin());
    HistSave();
    return c.id;
}

/* ==================== 引擎直连 (agent 工具执行层) ====================
 * 插件进程 = 宿主进程, 加载器按模块名绑到宿主已加载的 xunjieso.dll 同一实例;
 * xjs_GetDefaultEngine() 取句柄。agent 的搜索全部打到插件私有结果对象 g_agentRes
 * (不碰窗口结果对象 = 不打扰用户正在看的搜索)。工具执行一律在 agent 工作线程 +
 * g_agentCs 串行 (多窗口同时让 AI 跑工具时排队), Query/ExecuteLua 用 waitComplete=TRUE
 * 阻塞等完成 (主线程被引擎拒绝, 工作线程正是推荐用法; Lua 看门狗 10 秒兜底)。
 * 结果对象必须引擎空闲态创建 (忙时创建会被永久定成文件名序), 懒建 + 失效重建。 */

static CRITICAL_SECTION g_agentCs;
static xjs_result* g_agentRes = NULL;     /* agent 私有结果对象 (进程级一份, 串行使用) */
static std::vector<int> g_agentTopIds;    /* 最近一次 run_search 样本 fileId (open_file 按 1-based 序号取用) */

static void AgentToolInit() { InitializeCriticalSectionAndSpinCount(&g_agentCs, 100); }
static void AgentToolShutdown() {
    if (g_agentRes) { xjs_result_Destroy(g_agentRes); g_agentRes = NULL; }
    g_agentTopIds.clear();
    DeleteCriticalSection(&g_agentCs);
}

/* run_search 实体; 返回空串 = 成功, 否则 = 错误描述 (调用方持 g_agentCs) */
static std::wstring AgentToolRunSearch(const std::wstring& mode, const std::wstring& query, AiToolStep* st) {
    xjs_engine* eng = xjs_GetDefaultEngine();
    if (!eng) return L"搜索引擎未就绪";
    if (g_agentRes && !xjs_result_IsEffective(g_agentRes)) g_agentRes = NULL;
    if (!g_agentRes) {
        if (xjs_db_GetEngineState(eng) != XJS_DB_STATE_IDLE) return L"索引正忙 (加载/扫描中), 请稍后重试";
        g_agentRes = xjs_result_Create(eng);
        if (!g_agentRes) return L"结果对象创建失败";
    }
    std::string q8 = U8(query);
    int fp = -1;
    if (mode == L"wildcard")      fp = xjs_result_Query(g_agentRes, q8.c_str(), 0, TRUE);
    else if (mode == L"regex")    fp = xjs_result_Query(g_agentRes, q8.c_str(), 1, TRUE);
    else if (mode == L"sql")      fp = xjs_result_Query(g_agentRes, q8.c_str(), 2, TRUE);
    else if (mode == L"lua_filter") fp = xjs_result_Query(g_agentRes, q8.c_str(), XJS_KEYWORD_LUA, TRUE);
    else if (mode == L"lua_exec") fp = xjs_result_ExecuteLua(g_agentRes, q8.c_str(), TRUE);
    else return L"未知搜索模式: " + mode;
    if (fp < 0) {
        std::wstring e = W8(xjs_GetLastErrorMsg(eng));
        return L"搜索发起失败: " + (e.empty() ? std::wstring(L"引擎拒绝") : e);
    }
    if (!xjs_result_IsCompleted(g_agentRes, fp)) return L"搜索等待超时";   /* waitComplete=TRUE 正常不发生 */
    st->count = xjs_result_GetCount(g_agentRes);
    st->elapsedMs = xjs_result_GetElapsed(g_agentRes);
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

/* open_file 实体: 打开/定位最近一次 run_search 样本中的文件 (走宿主打开行为) */
static std::wstring AgentToolOpenFile(int index, bool reveal, XjsWindowToken tok) {
    if (index < 1 || index > (int)g_agentTopIds.size())
        return L"序号越界 (最近一次搜索样本共 " + std::to_wstring((unsigned long long)g_agentTopIds.size()) + L" 条)";
    int fid = g_agentTopIds[index - 1];
    if (!g_host) return L"宿主不可用";
    int rc = g_host->OpenFile(g_ctx, tok, fid, reveal ? 1 : 0);
    if (rc != XJS_PLUGIN_OK) return L"打开失败 (窗口已关闭或未声明 ui 权限)";
    return L"";
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

/* 工具统一入口 (agent 工作线程调用): 解析参数 → 执行 → 结果填进 step; 返回错误描述 (空=成功) */
static std::wstring AgentToolExec(const std::string& name8, const std::string& args8,
                                  XjsWindowToken tok, AiToolStep* st) {
    Jv v = JsonParseW(W8(args8.c_str()));
    if (name8 == "run_search") {
        st->kind = 0;
        st->mode = v.S(L"mode");
        st->query = v.S(L"query");
        if (st->query.empty()) return L"query 不能为空";
        EnterCriticalSection(&g_agentCs);
        std::wstring err = AgentToolRunSearch(st->mode, st->query, st);
        LeaveCriticalSection(&g_agentCs);
        return err;
    }
    if (name8 == "open_file") {
        st->kind = 1;
        const Jv* rv = v.Get(L"reveal");
        bool reveal = rv && ((rv->t == 1 && rv->b) || (rv->t == 2 && rv->num != 0));
        int index = (int)wcstol(TrimW(v.S(L"index")).c_str(), NULL, 10);
        EnterCriticalSection(&g_agentCs);
        std::wstring err = AgentToolOpenFile(index, reveal, tok);
        LeaveCriticalSection(&g_agentCs);
        return err;
    }
    if (name8 == "copy_paths") {
        st->kind = 2;
        EnterCriticalSection(&g_agentCs);
        std::wstring err = AgentToolCopyPaths();
        LeaveCriticalSection(&g_agentCs);
        return err;
    }
    return L"未知工具: " + W8(name8.c_str());
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
        j += L"]}";
    } else {
        j = L"{\"ok\":true}";
    }
    return U8(j);
}

/* ---- 颜色 ---- */
static Gdiplus::Color HexCol(const std::wstring& hex, int alpha = 255) {   /* "#RRGGBB" */
    unsigned v = 0;
    for (size_t i = (hex.size() > 6 ? hex.size() - 6 : 0); i < hex.size(); i++) {
        wchar_t c = hex[i];
        v = v * 16 + (unsigned)((c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 0);
    }
    return Gdiplus::Color((BYTE)alpha, (BYTE)((v >> 16) & 255), (BYTE)((v >> 8) & 255), (BYTE)(v & 255));
}
static Gdiplus::Color MixCol(Gdiplus::Color a, Gdiplus::Color b, float t) {   /* t=0 → a */
    auto ch = [&](BYTE x, BYTE y) { return (BYTE)(x * (1 - t) + y * t + 0.5f); };
    return Gdiplus::Color((BYTE)(a.GetA() * (1 - t) + b.GetA() * t + 0.5f),
                          ch(a.GetR(), b.GetR()), ch(a.GetG(), b.GetG()), ch(a.GetB(), b.GetB()));
}
static Gdiplus::Color WithA(Gdiplus::Color c, BYTE a) { return Gdiplus::Color(a, c.GetR(), c.GetG(), c.GetB()); }
static float minf(float a, float b) { return a < b ? a : b; }
static float maxf(float a, float b) { return a > b ? a : b; }

struct FRect { float x = 0, y = 0, w = 0, h = 0;   /* 面板内坐标 (像素) */
               bool Hit(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; } };

/* 命中 id (渲染时回填矩形, 交互处查询 — 两处同源) */
enum {
    HIT_NONE = 0, HIT_STOPGEN = 1, HIT_HSET = 2, HIT_HHIST = 3, HIT_HNEW = 4,
    HIT_CLEAR = 5, HIT_INPUT = 6, HIT_SEND = 7, HIT_CLOSE = 8, HIT_INTHUMB = 9,
    HIT_SBROW = 20, HIT_SBDEL = 60,          /* +i: 侧栏行 / 行删除 */
    HIT_DURL = 100, HIT_DKEY = 101, HIT_DMODEL = 102, HIT_DCHK = 103, HIT_DCANCEL = 104, HIT_DSAVE = 105,
    HIT_THUMB = 120, HIT_TRACK = 121, HIT_REASON = 130,   /* +i: 推理块头 */
    HIT_STEPHEAD = 140                                    /* +i: 工具卡片头 (idxOut=消息下标; 点击=展开/收起样本) */
};

struct AiLine {           /* 排版产物: 一行 = 若干 run (自带文本与样式, 渲染期不依赖临时块) */
    float h = 0;
    float prefixW = 0;    /* 列表前缀宽 (首行 runs 整体右移) */
    std::wstring prefix;  /* 列表前缀文本 ("• " / "1. ") */
    bool hard = true;     /* 逻辑行界 (真换行: 块首/代码行首); false = 软折行续行 (复制时不插 \n) */
    bool leadSpace = false; /* 软折行时被丢的行首空格 (复制拼接处补回, "ORDER BY" 不粘成 "ORDERBY") */
    struct Run { float x = 0, w = 0; std::wstring text; bool bold = false, code = false, accent = false, cjk = false; };
    std::vector<Run> runs;
};
struct AiBlock {          /* markdown-lite 块 */
    int type = 0;         /* 0=段落 1=无序表 2=有序表 3=代码块 */
    std::wstring num;     /* 有序表序号 */
    struct Seg { std::wstring text; bool bold = false, code = false, accent = false; };
    std::vector<Seg> segs;
    std::vector<std::wstring> codeLines;   /* type=3 */
};
static void MdInline(const std::wstring& text, std::vector<AiBlock::Seg>* out) {
    /* 行内: `code` / **bold** / 裸 URL → accent; 其余普通 */
    std::wstring cur;
    auto flush = [&](bool bold = false, bool code = false, bool accent = false) {
        if (!cur.empty()) { AiBlock::Seg s; s.text = cur; s.bold = bold; s.code = code; s.accent = accent; out->push_back(s); }
        cur.clear();
    };
    for (size_t i = 0; i < text.size();) {
        if (text[i] == L'`') {
            size_t e = text.find(L'`', i + 1);
            if (e != std::wstring::npos) {
                flush();
                AiBlock::Seg s; s.text = text.substr(i + 1, e - i - 1); s.code = true; out->push_back(s);
                i = e + 1;
                continue;
            }
        }
        if (text[i] == L'*' && i + 1 < text.size() && text[i + 1] == L'*') {
            size_t e = text.find(L"**", i + 2);
            if (e != std::wstring::npos) {
                flush();
                AiBlock::Seg s; s.text = text.substr(i + 2, e - i - 2); s.bold = true; out->push_back(s);
                i = e + 2;
                continue;
            }
        }
        if ((text.compare(i, 8, L"https://") == 0) || (text.compare(i, 7, L"http://") == 0)) {
            size_t n = text.compare(i, 8, L"https://") == 0 ? 8 : 7;
            size_t e = i + n;
            while (e < text.size() && !iswspace((wint_t)text[e]) && text[e] != L')' && text[e] != L']') e++;
            flush();
            AiBlock::Seg s; s.text = text.substr(i, e - i); s.accent = true; out->push_back(s);
            i = e;
            continue;
        }
        cur += text[i++];
    }
    flush();
}
static void MdParse(const std::wstring& text, std::vector<AiBlock>* out) {
    out->clear();
    std::vector<std::wstring> lines;
    {
        std::wstring cur;
        for (wchar_t c : text) {
            if (c == L'\n') { lines.push_back(cur); cur.clear(); }
            else cur += c;
        }
        lines.push_back(cur);
    }
    bool inCode = false;
    std::vector<AiBlock> blocks;
    for (auto& raw : lines) {
        std::wstring line = raw;
        if (line.rfind(L"```", 0) == 0) {   /* 围栏切换 */
            if (inCode || !TrimW(line).empty()) inCode = !inCode;
            continue;
        }
        if (inCode) {
            if (blocks.empty() || blocks.back().type != 3) {
                AiBlock b; b.type = 3; blocks.push_back(b);
            }
            if (blocks.back().codeLines.size() < 400) blocks.back().codeLines.push_back(raw);
            continue;
        }
        std::wstring t = TrimW(line);
        if (t.empty()) continue;   /* 空行 = 段落分隔 */
        if (t.size() >= 2 && (t[0] == L'-' || t[0] == L'*' || t[0] == L'•') && t[1] == L' ') {
            AiBlock b; b.type = 1;
            MdInline(TrimW(t.substr(2)), &b.segs);
            blocks.push_back(b);
            continue;
        }
        size_t d = t.find(L". ");
        if (d != std::wstring::npos && d <= 3 && d > 0) {
            bool allDigit = true;
            for (size_t k = 0; k < d; k++) if (!iswdigit((wint_t)t[k])) allDigit = false;
            if (allDigit) {
                AiBlock b; b.type = 2; b.num = t.substr(0, d + 1);
                MdInline(TrimW(t.substr(d + 2)), &b.segs);
                blocks.push_back(b);
                continue;
            }
        }
        if (t[0] == L'#') {   /* 标题降级为加粗段 (轻量口径) */
            size_t k = 0;
            while (k < t.size() && t[k] == L'#') k++;
            std::wstring head = L"**" + TrimW(t.substr(k)) + L"**";
            AiBlock b;
            MdInline(head, &b.segs);
            blocks.push_back(b);
            continue;
        }
        /* 普通行: 并入上一段 (若上一段是普通段落), 否则开新段 */
        if (!blocks.empty() && blocks.back().type == 0) {
            std::vector<AiBlock::Seg> more;
            MdInline(t, &more);
            if (!more.empty()) {
                more[0].text = (blocks.back().segs.empty() ? L"" : L" ") + more[0].text;
                for (auto& s : more) blocks.back().segs.push_back(s);
            }
        } else {
            AiBlock b;
            MdInline(t, &b.segs);
            blocks.push_back(b);
        }
    }
    *out = blocks;
}

/* ==================== 会话 (每窗一份; 渲染/交互状态全在这) ==================== */

struct AiJob {            /* 一次 agent 请求 (堆分配; 工作线程只摸它, UI 经消息泵抽取) */
    CRITICAL_SECTION cs;
    std::wstring out, reason;      /* 当前轮的文本增量 (worker 写, UI 抽; 每轮工具执行后清空重开) */
    int state = 0;                 /* 0=进行中 1=完成 2=失败 3=已中止 */
    int phase = 0;                 /* 0=流式中 1=工具执行中 (泵据此冻结/新开文本气泡) */
    std::vector<AiToolStep> steps; /* 工具步骤镜像 (泵同步进 msgs 的 role==2 消息; 保留 open 态) */
    int stepsVersion = 0;          /* steps 每次内容变化 +1 (泵据版本号决定重排) */
    std::wstring err;
    bool truncated = false;
    std::string hostA, pathA, keyA;   /* 请求要素 (UTF-8; worker 自取; 请求体每轮在 worker 构建) */
    INTERNET_PORT port = 443;
    bool secure = true;
    XjsWindowToken tok = 0;        /* 发起窗口 (open_file 走宿主 OpenFile 用) */
    std::vector<AiMsg> hist;       /* 发送时对话快照 (只含 role 0/1; worker 构造每轮 body 用) */
    volatile LONG abort = 0;
    HANDLE hReq = NULL;            /* UI 线程"停止"用它打断阻塞读 (并发关句柄=中断语义) */
    std::thread* th = NULL;
    bool thJoined = false;
    AiJob() { InitializeCriticalSectionAndSpinCount(&cs, 100); }
    ~AiJob() { DeleteCriticalSection(&cs); }
};
static std::vector<AiJob*> s_orphans;   /* 会话已关而流未完的作业 (泵里清扫 join) */

struct AiSess {
    bool inUse = false;
    XjsWindowToken tok = 0;
    long long serial = 0;
    int w = 0, h = 0;
    float scale = 1.0f;
    bool winActive = true;
    /* 皮肤 (GetSkinJson; 打开会话时取一次) */
    Gdiplus::Color cBg, cPanel, cText, cDim, cAccent, cLine;
    bool skinOk = false;
    /* 字体 (按 scale 重建) */
    Gdiplus::Font* fBody = NULL;      /* 12.5px 正文 (CJK = 雅黑; 拉丁 = Segoe, 同 em 双族 —
                                         单族画中英混排时拉丁字形观感突兀, 主程序 GDI+ 后端同口径) */
    Gdiplus::Font* fBodyB = NULL;
    Gdiplus::Font* fBodyL = NULL;     /* 拉丁版 (Segoe UI) */
    Gdiplus::Font* fBodyBL = NULL;
    Gdiplus::Font* fMono = NULL;      /* 11.5px 代码 */
    Gdiplus::Font* fTiny = NULL;      /* 10.5px 提示 */
    Gdiplus::Font* fTinyL = NULL;
    Gdiplus::Font* fTitle = NULL;     /* 15px 标题 (加粗) */
    float madeScale = 0.0f;
    Gdiplus::FontFamily* famUI = NULL;
    Gdiplus::FontFamily* famLat = NULL;   /* 拉丁家族 (不可用时共享 famUI; owns=false 不二次删) */
    bool famLatOwn = false;
    Gdiplus::FontFamily* famMono = NULL;
    /* 交付表面 (BGRA, 顶层不透明) */
    std::vector<uint8_t> px;
    int stride = 0, bw = 0, bh = 0;
    /* 数据 */
    bool loaded = false;
    std::vector<AiMsg> msgs;
    unsigned long long curId = 0;
    bool sending = false;
    AiJob* job = NULL;
    int lastStepsVer = -1;            /* 泵已同步到卡片的 stepsVersion (变化才重排) */
    int netStatus = 0;                /* 0=未配置/未知(灰) 1=正常(绿) 2=失败(红) */
    /* 输入框 */
    std::wstring input;
    size_t caret = 0;
    bool inputFocus = false;
    /* 输入框编辑 (对齐宿主搜索框 XjsLineEdit 口径): 选区/拖选/行导航期望列/单档撤销/右键编辑菜单 */
    size_t anchor = 0;                /* 选区锚点 (== caret = 无选区) */
    bool selDragging = false;         /* 按住拖选进行中 (宿主捕获中 MOVE 持续转发) */
    float expectCol = -1.0f;          /* ↑↓ 行导航的期望列 x (<0 = 无, 取当前列) */
    std::wstring undoText;            /* 单档撤销快照 (交换式) */
    size_t undoCaret = 0, undoAnchor = 0;
    bool undoSnap = false;
    int inScroll = 0;                 /* 输入框滚动: 首个可视行 (可视窗口 4 行, 溢出才 >0) */
    float inGrab = 0;                 /* 输入框滚动条拖拽: 抓点偏移 (>0 = 拖拽中) */
    FRect hInThumb;                   /* 输入框滚动条 (渲染回填; 无溢出 = 零矩形) */
    float inTrackY = 0, inTrackH = 0;
    bool menuOpen = false;            /* 输入框右键编辑菜单 (自绘浮层; 几何渲染回填) */
    FRect menuBox; std::vector<FRect> menuRows;
    /* 消息文本选区 (拖选复制; 单条消息内, atom 粒度 — 行/atom 两端点) */
    bool txtSel = false;
    bool txtDragging = false;
    int selMsg = -1;
    int selALine = -1, selAAtom = -1, selBLine = -1, selBAtom = -1;
    bool msgKeyBorrow = false;        /* 拖选开始时向宿主借的键盘 (Ctrl+C 复制用; 清选区即归还) */
    /* 滚动 */
    double scrollY = 0;
    bool sticky = true;               /* 吸底 (流式跟随); 向上滚即解除 */
    /* 历史侧栏 */
    bool sideOpen = false;
    double sideScroll = 0;
    bool clearArm = false;
    FRect sidePanel;                  /* 侧栏浮层矩形 (渲染回填; 点浮层外 = 关闭) */
    /* 接口设置对话框 */
    bool dlg = false;
    std::wstring dUrl, dKey, dModel;
    int dFocus = 0;                   /* 0无 1=URL 2=Key 3=Model */
    size_t dCaret = 0, dAnchor = 0;   /* 焦点字段的光标/选区锚 (UTF-16 下标, 密钥 ● 显示与值 1:1) */
    bool dSelDragging = false;        /* 对话框字段拖选进行中 (按住拖动扩选区) */
    bool dReason = false;
    /* 命中矩形 (渲染回填; 面板内坐标) */
    FRect hSet, hHist, hNew, hClose, hInput, hSend, hClear;
    FRect hThumb; float thumbTrackY = 0, thumbTrackH = 0;
    std::vector<FRect> hRows, hRowDel;
    FRect dUrlR, dKeyR, dModelR, dChkR, dCancelR, dSaveR;
    FRect dCardR;                     /* 对话框卡片整体 (卡内空白 = 标题/标签/按钮带空档, 点它不关闭) */
    int hover = HIT_NONE;             /* 高亮 hover (低频: 值变化才重渲染) */
    int press = HIT_NONE;
    float pressX = -1, pressY = -1;   /* 按下点 (蒙层取消判定: 按下与松开都在卡外才关闭) */
    int pressGrab = 0;                /* 滚动条拖拽: 抓点偏移 */
    /* 排版缓存 */
    struct MsgLayout {
        std::vector<AiLine> lines;
        std::vector<AiLine> reasonLines;   /* 推理块 (reason 非空才有) */
        float bodyH = 0, reasonH = 0, totalH = 0, bubbleW = 0;
        /* role==2 工具卡片组 */
        std::vector<float> stepHs;                       /* 每 step 卡片总高 */
        std::vector<std::vector<AiLine>> stepErrLines;   /* 失败信息折行 */
    };
    std::vector<MsgLayout> lay;
    std::vector<float> layOffsets;    /* 每条消息 y 起点 (排版产物) */
    float contentH = 0;               /* 消息流总高 */
    bool layDirty = true;
    int layW = 0;
    float layScale = 0;
};
static AiSess g_sess[8];

static AiSess* SessByTok(XjsWindowToken tok) {
    for (auto& s : g_sess) if (s.inUse && s.tok == tok) return &s;
    return NULL;
}
static AiSess* SessFree() {
    for (auto& s : g_sess) if (!s.inUse) return &s;
    return NULL;
}

/* ---- 字体 / 表面 ---- */
static void SessFreeFonts(AiSess* s) {
    delete s->fBody; delete s->fBodyB; delete s->fBodyL; delete s->fBodyBL;
    delete s->fMono; delete s->fTiny; delete s->fTinyL; delete s->fTitle;
    s->fBody = s->fBodyB = s->fBodyL = s->fBodyBL = s->fMono = s->fTiny = s->fTinyL = s->fTitle = NULL;
    delete s->famUI; delete s->famMono;
    if (s->famLatOwn) delete s->famLat;   /* 回退共享 famUI 时只删一次 */
    s->famUI = s->famMono = NULL;
    s->famLat = NULL; s->famLatOwn = false;
    s->madeScale = 0.0f;
}
static void SessEnsureFonts(AiSess* s) {
    if (s->madeScale == s->scale && s->fBody) return;
    SessFreeFonts(s);
    s->famUI = new Gdiplus::FontFamily(L"Microsoft YaHei UI");
    if (!s->famUI->IsAvailable()) { delete s->famUI; s->famUI = new Gdiplus::FontFamily(L"Microsoft YaHei"); }
    if (!s->famUI->IsAvailable()) { delete s->famUI; s->famUI = new Gdiplus::FontFamily(L"Segoe UI"); }
    s->famLat = new Gdiplus::FontFamily(L"Segoe UI");
    s->famLatOwn = s->famLat->IsAvailable();
    if (!s->famLatOwn) { delete s->famLat; s->famLat = s->famUI; }   /* 无 Segoe = 拉丁同 CJK 族 */
    s->famMono = new Gdiplus::FontFamily(L"Consolas");
    if (!s->famMono->IsAvailable()) { delete s->famMono; s->famMono = new Gdiplus::FontFamily(L"Courier New"); }
    float k = s->scale;
    s->fBody = new Gdiplus::Font(s->famUI, 12.5f * k, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    s->fBodyB = new Gdiplus::Font(s->famUI, 12.5f * k, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    s->fBodyL = new Gdiplus::Font(s->famLat, 12.5f * k, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    s->fBodyBL = new Gdiplus::Font(s->famLat, 12.5f * k, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    s->fMono = new Gdiplus::Font(s->famMono, 11.0f * k, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    s->fTiny = new Gdiplus::Font(s->famUI, 10.5f * k, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    s->fTinyL = new Gdiplus::Font(s->famLat, 10.5f * k, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    s->fTitle = new Gdiplus::Font(s->famUI, 15.0f * k, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    s->madeScale = s->scale;
}
static void SessEnsureSurface(AiSess* s) {
    int bw = s->w > 0 ? s->w : 1, bh = s->h > 0 ? s->h : 1;
    if (s->bw == bw && s->bh == bh) return;
    s->stride = bw * 4;
    s->px.assign((size_t)s->stride * bh, 0);
    s->bw = bw;
    s->bh = bh;
}

/* ---- 皮肤 ---- */
static void SessLoadSkin(AiSess* s) {
    s->skinOk = false;
    if (!g_host) return;
    char buf[1024];
    int n = g_host->GetSkinJson(g_ctx, buf, (int)sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = 0;
    Jv v = JsonParseW(W8(buf));
    if (v.t != 5) return;
    s->cBg = HexCol(v.S(L"bg1", L"#14171f"));
    s->cPanel = HexCol(v.S(L"panel", L"#1b2030"));
    s->cText = HexCol(v.S(L"text", L"#e8eaf0"));
    s->cDim = HexCol(v.S(L"dim", L"#9aa3b5"));
    s->cAccent = HexCol(v.S(L"accent", L"#4f7cff"));
    s->cLine = HexCol(v.S(L"line", L"#2a3040"));
    s->skinOk = true;
}

/* ==================== 排版 (块 → 行; 量宽贪心换行, CJK 逐字/拉丁按词) ==================== */

static const float AI_HEAD_H = 42.0f;      /* ×scale */
static const float AI_LINE_H = 20.0f;      /* 正文行高 ×scale */
static const float AI_CODE_H = 16.5f;
static const float AI_TINY_H = 15.0f;

struct AiAtom {           /* 最小断行单元 */
    std::wstring t;
    float w = 0;
    bool code = false, bold = false, accent = false, cjk = false;
};

/* 文本测量/绘制统一格式: GDI+ 默认 StringFormat (GenericDefault) 带两侧布局 padding (~1/6 em),
   排版是逐 atom (逐字/逐词) 分段测量+分段绘制, 每段的 padding 都累计进 x 偏移 = 行内大片
   "空格"。GenericTypographic 无 padding — 但其去 padding 行为挂在原生格式内部状态上,
   按公开 flags 重建不等效, 必须 Clone 原生单例保留; 尾随空格宽需显式补标志 (空格是独立
   断行 atom, 量不出宽 = 单词粘连)。单例缓存, UI 线程串行使用 */
static Gdiplus::StringFormat* AiTextFmt() {
    static Gdiplus::StringFormat* fmt = NULL;
    if (!fmt) {
        fmt = Gdiplus::StringFormat::GenericTypographic()->Clone();
        fmt->SetFormatFlags(fmt->GetFormatFlags() | Gdiplus::StringFormatFlagsNoWrap |
                            Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
    }
    return fmt;
}

static float AiMeasure(Gdiplus::Graphics& g, Gdiplus::Font* f, const std::wstring& t) {
    if (t.empty()) return 0;
    Gdiplus::RectF r;
    g.MeasureString(t.c_str(), (INT)t.size(), f, Gdiplus::PointF(0, 0), AiTextFmt(), &r);
    return r.Width;
}

static Gdiplus::Font* AiFontOf(AiSess* s, bool code, bool bold, bool cjk) {
    if (code) return s->fMono;
    if (cjk) return bold ? s->fBodyB : s->fBody;      /* CJK = 雅黑 */
    return bold ? s->fBodyBL : s->fBodyL;             /* 拉丁 = Segoe UI (观感与主程序一致) */
}

/* 段落 segs → atoms (量宽; word/CJK 混排断行单元) */
static void AiSegsToAtoms(Gdiplus::Graphics& g, AiSess* s, const std::vector<AiBlock::Seg>& segs,
                          std::vector<AiAtom>* out) {
    for (int si = 0; si < (int)segs.size(); si++) {
        const std::wstring& t = segs[si].text;
        size_t i = 0;
        std::wstring word;
        auto flushWord = [&](const std::wstring& piece) {
            if (piece.empty()) return;
            AiAtom a; a.t = piece;
            a.code = segs[si].code; a.bold = segs[si].bold; a.accent = segs[si].accent;
            wchar_t c0 = piece[0];
            a.cjk = (c0 & 0xFC00) == 0xD800 || c0 >= 0x2E80;   /* 段级选族: CJK=雅黑, 拉丁=Segoe */
            a.w = AiMeasure(g, AiFontOf(s, a.code, a.bold, a.cjk), piece);
            out->push_back(a);
        };
        while (i < t.size()) {
            wchar_t c = t[i];
            if (c == L'\r' || c == L'\n') {   /* 行结构由 MdParse 分块; 漏网换行不进 runs —
                                                 测量宽 0 而绘制成实块字形 = 伸出气泡右缘 */
                flushWord(word); word.clear();
                i++;
                continue;
            }
            bool cjk = (c & 0xFC00) == 0xD800 || c >= 0x2E80;
            if (c == L' ') {
                flushWord(word); word.clear();
                flushWord(L" ");
                i++;
                continue;
            }
            if (cjk) {
                flushWord(word); word.clear();
                size_t n = ((c & 0xFC00) == 0xD800 && i + 1 < t.size()) ? 2 : 1;   /* 代理对整体 */
                flushWord(t.substr(i, n));
                i += n;
                continue;
            }
            word += c;
            i++;
        }
        flushWord(word);
    }
}

/* 行内容宽 = 最宽 run 行 (排版产物即行宽来源; bubble 宽按最宽行 + 内边距) */
static void AiPackLines(AiSess* s, Gdiplus::Graphics& g, const std::vector<AiBlock>& blocks,
                        float maxW, float k, std::vector<AiLine>* lines, float* contentH) {
    const float codePad = 6.0f * k;
    size_t i = 0;
    while (i < blocks.size()) {
        const AiBlock& b = blocks[i];
        if (b.type == 3) {   /* 代码块: 整块等宽排版 */
            lines->push_back(AiLine()); lines->back().h = codePad;
            lines->back().hard = false;   /* 纯视觉垫行: 复制不贡献换行 */
            *contentH += codePad;   /* 垫行必须计入 bodyH — 绘制 yCur 累加它, 漏计 = 内容沉出气泡底 */
            for (auto& cl : b.codeLines) {
                AiBlock::Seg seg; seg.text = cl; seg.code = true;
                std::vector<AiBlock::Seg> one{ seg };
                std::vector<AiAtom> atoms;
                AiSegsToAtoms(g, s, one, &atoms);
                AiLine ln; ln.h = AI_CODE_H * k;
                float x = 0;
                for (auto& a : atoms) {
                    if (x + a.w > maxW && x > 0) {   /* 代码行超宽必须换行 — 行宽无上限会溢出气泡右缘 */
                        lines->push_back(ln);
                        *contentH += ln.h;
                        ln = AiLine(); ln.h = AI_CODE_H * k;
                        ln.hard = false;   /* 同一代码行的软折行续行 (复制时不插 \n) */
                        x = 0;
                    }
                    AiLine::Run r; r.x = x; r.w = a.w; r.text = a.t; r.code = true;
                    ln.runs.push_back(r);
                    x += a.w;
                }
                lines->push_back(ln);
                *contentH += ln.h;
            }
            lines->push_back(AiLine()); lines->back().h = codePad;
            lines->back().hard = false;   /* 纯视觉垫行: 复制不贡献换行 */
            *contentH += codePad;
            i++;
            continue;
        }
        /* 段落/列表: atoms 贪心打包 */
        float indent = 0.0f;
        std::wstring prefix;
        if (b.type == 1) { indent = 14.0f * k; prefix = L"• "; }
        if (b.type == 2) { indent = 0; prefix = b.num + L" "; }
        std::vector<AiAtom> atoms;
        AiSegsToAtoms(g, s, b.segs, &atoms);
        AiLine cur;
        float x = indent;
        bool firstLine = true;
        auto emit = [&]() {
            if (firstLine && !prefix.empty()) {
                float pw = AiMeasure(g, s->fBody, prefix);
                for (auto& r : cur.runs) r.x = pw + (r.x - indent);   /* 首行正文让位前缀 */
                AiLine::Run pr; pr.x = 0; pr.w = pw;
                cur.runs.insert(cur.runs.begin(), pr);
                cur.prefix = prefix;
                cur.prefixW = pw;
            }
            cur.h = AI_LINE_H * k;   /* 段落行高必须落值 — AiLine::h 默认 0, 不设 = 整段所有行
                                        叠画在同一 y (错版叠字真因); 流式光标 y 也按此行高回退 */
            lines->push_back(cur);
            *contentH += cur.h;
            cur = AiLine();
            x = indent;
            firstLine = false;
        };
        for (auto& a : atoms) {
            if (a.t == L" " && cur.runs.empty() && !firstLine) { cur.leadSpace = true; continue; }   /* 行首空格丢弃 (复制时补回) */
            if (x + a.w > maxW && !cur.runs.empty()) { emit(); cur.hard = false; }   /* 软折行续行 */
            AiLine::Run r; r.x = x; r.w = a.w; r.text = a.t;
            r.bold = a.bold; r.code = a.code; r.accent = a.accent; r.cjk = a.cjk;
            cur.runs.push_back(r);
            x += a.w;
            if (x > maxW) { emit(); cur.hard = false; }   /* 超宽单词兜底逐段硬拆观感 (同词续行, 下一 atom 起新行) */
        }
        emit();
        lines->push_back(AiLine()); lines->back().h = 5.0f*k;   /* 块间距 (垫行高同样计入 bodyH, 漏计 = 末行溢出气泡) */
        lines->back().hard = false;   /* 纯视觉垫行: 复制不贡献换行 (块间 = 下一块首行的一个 \n) */
        *contentH += 5.0f*k;
        i++;
    }
}

/* ==================== 消息排版 + 渲染 ==================== */

static std::vector<AiLine> WrapPlain(AiSess* s, Gdiplus::Graphics& g, const std::wstring& text,
                                     float maxW, float k, float* outH) {
    std::vector<AiLine> lines;
    auto emitLine = [&](const std::wstring& piece) {
        std::vector<AiBlock::Seg> segs{ AiBlock::Seg{} };
        segs[0].text = piece;
        std::vector<AiAtom> atoms;
        AiSegsToAtoms(g, s, segs, &atoms);
        AiLine ln; ln.h = AI_TINY_H * k;
        float x = 0;
        for (auto& at : atoms) {
            AiLine::Run r; r.x = x; r.w = at.w; r.text = at.t; r.cjk = at.cjk;
            ln.runs.push_back(r);
            x += at.w;
        }
        lines.push_back(ln);
        *outH += ln.h;
    };
    std::wstring cur;
    for (size_t i = 0; i <= text.size(); i++) {
        if (i == text.size() || text[i] == L'\n') {
            emitLine(cur);
            cur.clear();
            continue;
        }
        cur += text[i];
    }
    return lines;
}

static void LayoutMsg(AiSess* s, int mi, Gdiplus::Graphics& mg) {
    if (mi < 0 || mi >= (int)s->msgs.size()) return;
    float k = s->scale;
    AiSess::MsgLayout& L = s->lay[mi];
    const AiMsg& m = s->msgs[mi];
    float avail = s->w - 12.0f * k * 2;
    float maxTextW = avail - 24.0f * k - 8.0f * k * 2 - 10.0f * k * 2;   /* 头像+间隙+气泡内边距 */
    if (maxTextW < 40.0f * k) maxTextW = 40.0f * k;

    if (m.role == 2) {
        /* 工具卡片组: 全宽气泡, 每 step = 头行 + (查询行) + (展开样本/错误折行) */
        L.lines.clear();
        L.reasonLines.clear();
        L.bodyH = 0;
        L.reasonH = 0;
        L.stepHs.assign(m.steps.size(), 0.0f);
        L.stepErrLines.assign(m.steps.size(), {});
        float h = 0;
        for (size_t si = 0; si < m.steps.size(); si++) {
            const AiToolStep& st = m.steps[si];
            float sh = 22.0f * k;                                   /* 头行 */
            if (st.kind == 0 && !st.query.empty()) sh += 15.0f * k; /* 查询行 (mono 截断) */
            if (st.state == 3 && !st.err.empty()) {                 /* 错误折行 */
                float eh = 0;
                L.stepErrLines[si] = WrapPlain(s, mg, st.err, maxTextW - 24.0f * k, k, &eh);
                sh += eh + 2.0f * k;
            }
            if (st.open && !st.top.empty())
                sh += 4.0f * k + (float)st.top.size() * 15.0f * k;  /* 样本列表 */
            L.stepHs[si] = sh;
            h += sh + (si + 1 < m.steps.size() ? 6.0f * k : 0.0f);
        }
        L.bubbleW = avail - 24.0f * k - 8.0f * k * 2;
        if (L.bubbleW < 60.0f * k) L.bubbleW = 60.0f * k;
        L.totalH = 10.0f * k * 2 + h;
        return;
    }

    L.lines.clear();
    L.reasonLines.clear();
    L.bodyH = 0;
    L.reasonH = 0;
    std::vector<AiBlock> blocks;
    MdParse(m.text, &blocks);
    AiPackLines(s, mg, blocks, maxTextW, k, &L.lines, &L.bodyH);
    if (!m.reason.empty() && m.reasonOpen) {
        L.reasonLines = WrapPlain(s, mg, m.reason, maxTextW - 8.0f * k, k, &L.reasonH);   /* 返回值必须接 — 曾丢弃 = 展开推理块空白 */
        L.reasonH += 26.0f * k;   /* 头行 + 内边距 */
    }
    float widest = 0;
    for (auto& ln : L.lines)
        for (auto& r : ln.runs)
            if (r.x + r.w > widest) widest = r.x + r.w;
    L.bubbleW = widest + 10.0f * k * 2;
    float maxBubble = avail - 24.0f * k - 8.0f * k * 2;
    if (L.bubbleW > maxBubble) L.bubbleW = maxBubble;
    if (L.bubbleW < 60.0f * k) L.bubbleW = 60.0f * k;
    L.totalH = 10.0f * k * 2 + L.bodyH
             + (m.reason.empty() ? 0.0f : (m.reasonOpen ? L.reasonH + 4.0f * k : 24.0f * k));   /* 折叠=只占头行 */
}

static void LayoutAll(AiSess* s, bool force) {
    int w = s->w;
    float k = s->scale;
    if (!force && s->layW == w && s->layScale == k && !s->layDirty) return;
    s->lay.assign(s->msgs.size(), AiSess::MsgLayout());
    s->layOffsets.assign(s->msgs.size(), 0.0f);
    Gdiplus::Bitmap tmp(1, 1, PixelFormat32bppPARGB);
    Gdiplus::Graphics mg(&tmp);
    float y = 0;
    for (int i = 0; i < (int)s->msgs.size(); i++) {
        LayoutMsg(s, i, mg);
        s->layOffsets[i] = y;
        y += s->lay[i].totalH + 14.0f * k;
    }
    s->contentH = y;
    s->layW = w;
    s->layScale = k;
    s->layDirty = false;
}

static void RelayoutOne(AiSess* s, int mi) {
    if (mi < 0 || mi >= (int)s->msgs.size()) return;
    /* 排版数组未同步 (本帧刚 push 过新消息, 比 msgs 短) 不得摸 lay[mi] —
       越界 = 对堆垃圾当 MsgLayout (含 vector 成员) 执行 clear/push_back, 堆写坏后整版叠画乱码 */
    if (mi >= (int)s->lay.size() || mi >= (int)s->layOffsets.size()) { s->layDirty = true; return; }
    Gdiplus::Bitmap tmp(1, 1, PixelFormat32bppPARGB);
    Gdiplus::Graphics mg(&tmp);
    float oldH = s->lay[mi].totalH;
    LayoutMsg(s, mi, mg);
    float k = s->scale;
    float d = s->lay[mi].totalH - oldH;
    if (d != 0) {
        for (int i = mi + 1; i < (int)s->layOffsets.size(); i++) s->layOffsets[i] += d;
        s->contentH += d;
    }
    (void)k;
}

/* ---- 绘制小件 ---- */
static void AiRoundRect(Gdiplus::Graphics& g, float x, float y, float w, float h, float r, Gdiplus::Brush* br) {
    Gdiplus::GraphicsPath p;
    float rr = r;
    if (rr * 2 > h) rr = h / 2;
    if (rr * 2 > w) rr = w / 2;
    p.AddArc(x, y, rr * 2, rr * 2, 180, 90);
    p.AddArc(x + w - rr * 2, y, rr * 2, rr * 2, 270, 90);
    p.AddArc(x + w - rr * 2, y + h - rr * 2, rr * 2, rr * 2, 0, 90);
    p.AddArc(x, y + h - rr * 2, rr * 2, rr * 2, 90, 90);
    p.CloseFigure();
    g.FillPath(br, &p);
}
static void AiRoundRect(Gdiplus::Graphics& g, float x, float y, float w, float h, float r, const Gdiplus::Brush& br) {
    AiRoundRect(g, x, y, w, h, r, const_cast<Gdiplus::Brush*>(&br));
}
static void AiRoundRectLine(Gdiplus::Graphics& g, float x, float y, float w, float h, float r, Gdiplus::Pen* pen) {
    Gdiplus::GraphicsPath p;
    float rr = r;
    if (rr * 2 > h) rr = h / 2;
    if (rr * 2 > w) rr = w / 2;
    p.AddArc(x, y, rr * 2, rr * 2, 180, 90);
    p.AddArc(x + w - rr * 2, y, rr * 2, rr * 2, 270, 90);
    p.AddArc(x + w - rr * 2, y + h - rr * 2, rr * 2, rr * 2, 0, 90);
    p.AddArc(x, y + h - rr * 2, rr * 2, rr * 2, 90, 90);
    p.CloseFigure();
    g.DrawPath(pen, &p);
}
static void AiRoundRectLine(Gdiplus::Graphics& g, float x, float y, float w, float h, float r, const Gdiplus::Pen& pen) {
    AiRoundRectLine(g, x, y, w, h, r, const_cast<Gdiplus::Pen*>(&pen));
}
static void AiText(Gdiplus::Graphics& g, const std::wstring& t, Gdiplus::Font* f, Gdiplus::Brush* br,
                   float x, float y) {
    g.DrawString(t.c_str(), (INT)t.size(), f, Gdiplus::PointF(x, y), AiTextFmt(), br);
}
static void AiText(Gdiplus::Graphics& g, const std::wstring& t, Gdiplus::Font* f, const Gdiplus::Brush& br,
                   float x, float y) {
    AiText(g, t, f, const_cast<Gdiplus::Brush*>(&br), x, y);
}
static void AiTextTrunc(Gdiplus::Graphics& g, std::wstring t, Gdiplus::Font* f, Gdiplus::Brush* br,
                        float x, float y, float maxW) {
    if (AiMeasure(g, f, t) <= maxW) { AiText(g, t, f, br, x, y); return; }
    while (t.size() > 1 && AiMeasure(g, f, t + L"…") > maxW) t.erase(t.size() - 1);
    AiText(g, t + L"…", f, br, x, y);
}
/* 垂直居中绘制 (按实测排版高度) — 对话框字段值/占位共用; 行高常量近似曾让两者基线不齐、● 点偏上 */
static void AiTextMid(Gdiplus::Graphics& g, const std::wstring& t, Gdiplus::Font* f, Gdiplus::Brush* br,
                      float x, float boxY, float boxH) {
    Gdiplus::RectF m;
    g.MeasureString(t.c_str(), (INT)t.size(), f, Gdiplus::PointF(0, 0), AiTextFmt(), &m);
    AiText(g, t, f, br, x, boxY + (boxH - m.Height) / 2);
}

/* ==================== 接口设置对话框字段编辑 (单行全模型: 点定位/框选/全选/剪贴板 — 主输入框同款) ==================== */

/* 字段显示串 (密钥字段 = 等长 ● 串, 一个 UTF-16 单元一点, 与编辑下标 1:1) */
static std::wstring FieldDispOf(AiSess* s, int id) {
    const std::wstring& v = id == 1 ? s->dUrl : id == 2 ? s->dKey : s->dModel;
    if (id != 2) return v;
    return std::wstring(minf((size_t)14, v.size()), L'●');
}

/* 逐 UTF-16 单元累计宽 (末点归一到整串实宽) — atom 词内按比例插值曾偏差 1-2 字符, 点定位/光标 x 不准 */
static void FieldCumWidths(AiSess* s, const std::wstring& t, std::vector<float>* cum) {
    cum->assign(t.size() + 1, 0.0f);
    if (t.empty()) return;
    Gdiplus::Bitmap tmp(1, 1, PixelFormat32bppPARGB);
    Gdiplus::Graphics mg(&tmp);
    float acc = 0;
    for (size_t i = 0; i < t.size(); ) {
        size_t n = ((t[i] & 0xFC00) == 0xD800 && i + 1 < t.size()) ? 2 : 1;   /* 代理对整体 */
        float w = AiMeasure(mg, s->fBody, t.substr(i, n));
        (*cum)[i + n] = acc + w;
        if (n == 2) (*cum)[i + 1] = acc + w * 0.5f;   /* 代理对中间点 (点击落在其内=取前) */
        acc += w;
        i += n;
    }
    float total = AiMeasure(mg, s->fBody, t);
    if (acc > 0 && total > 0) {
        float k = total / acc;
        for (auto& v : *cum) v *= k;   /* 逐字测量无排版微调 — 末点对齐整串实宽, 内部线性归一 */
    }
}

/* 显示串前 n 个 UTF-16 单元的宽 (选区底/光标 x) */
static float FieldWidthOf(AiSess* s, const std::wstring& t, size_t n) {
    if (n == 0) return 0;
    if (n > t.size()) n = t.size();
    std::vector<float> cum;
    FieldCumWidths(s, t, &cum);
    return cum[n];
}

/* 点定位: x (相对文本原点) → 字符边界 (逐字实测宽, 半宽取前/后) */
static size_t FieldIndexFromX(AiSess* s, const std::wstring& t, float px) {
    if (px <= 0) return 0;
    std::vector<float> cum;
    FieldCumWidths(s, t, &cum);
    for (size_t i = 0; i < t.size(); ) {
        size_t n = ((t[i] & 0xFC00) == 0xD800 && i + 1 < t.size()) ? 2 : 1;
        if (px < cum[i + n])
            return (px - cum[i]) * 2.0f < cum[i + n] - cum[i] ? i : i + n;
        i += n;
    }
    return t.size();
}

/* 可视窗口: 整串放得下就全显; 溢出左裁看尾 (密钥习惯); 光标被裁掉时改锚光标向右裁 — 渲染/点定位同源 */
static size_t FieldVisWindow(AiSess* s, const std::wstring& disp, float maxW, std::wstring* visOut) {
    Gdiplus::Bitmap tmp(1, 1, PixelFormat32bppPARGB);
    Gdiplus::Graphics mg(&tmp);
    std::wstring vis = disp;
    size_t skip = 0;
    while (vis.size() > 1 && AiMeasure(mg, s->fBody, vis) > maxW) { vis.erase(0, 1); skip++; }
    if (s->dCaret < skip) {   /* 光标在窗口左缘外: 改从光标起向右裁 */
        skip = s->dCaret;
        vis = disp.substr(skip);
        size_t tail = vis.size();
        while (tail > 1 && AiMeasure(mg, s->fBody, vis.substr(0, tail)) > maxW) tail = PrevCp(vis, tail);
        vis.erase(tail);
    }
    *visOut = vis;
    return skip;
}

/* 点定位: x (相对文本原点) → 字符边界 — FieldIndexFromX / FieldWidthOf 见上 (逐字实测宽同源) */

/* 对话框字段 IME 锚点 (面板坐标; 组字/候选窗跟随字段光标) */
static void DlgUpdateIme(AiSess* s) {
    if (!HOST_PANEL_OK || !g_host || s->dFocus < 1 || s->dFocus > 3) return;
    FRect* rc = s->dFocus == 1 ? &s->dUrlR : s->dFocus == 2 ? &s->dKeyR : &s->dModelR;
    std::wstring disp = FieldDispOf(s, s->dFocus);
    if (s->dCaret > disp.size()) s->dCaret = disp.size();
    std::wstring vis;
    size_t skip = FieldVisWindow(s, disp, rc->w - 16.0f * s->scale, &vis);
    float cx = FieldWidthOf(s, vis, s->dCaret - skip);
    g_host->PanelSetCaret(g_ctx, s->tok, (int)(rc->x + 8.0f * s->scale + cx), (int)(rc->y + rc->h / 2));
}
static void AiFillRect(Gdiplus::Graphics& g, const Gdiplus::Brush& br, float x, float y, float w, float h) {
    g.FillRectangle(const_cast<Gdiplus::Brush*>(&br), x, y, w, h);
}
static void AiFillRect(Gdiplus::Graphics& g, const Gdiplus::Brush& br, const Gdiplus::RectF& r) {
    g.FillRectangle(const_cast<Gdiplus::Brush*>(&br), r);
}

/* ==================== 渲染主流程 ==================== */

static void RenderSession(AiSess* s);   /* 前置 */

static std::wstring TimeTextOf(long long t) {
    time_t tt = (time_t)t;
    struct tm lt;
    if (localtime_s(&lt, &tt) != 0) return L"";
    wchar_t b[40];
    swprintf(b, 40, L"%02d:%02d", lt.tm_hour, lt.tm_min);
    return b;
}

/* ============ 输入框布局 (行/字符位置表) ============
   折行/光标/点定位/选区渲染/行导航五处同源 — 禁止各写一套折行累加 */
static bool InputSelRange(AiSess* s, size_t* a, size_t* b);   /* 选区 [a,b) (渲染段前置调用; 实现在输入编辑区) */
static void MsgSelNorm(AiSess* s, int* aL, int* aA, int* bL, int* bA);   /* 消息选区两端归一 (实现在面板交互区) */
static void MsgSelClear(AiSess* s);   /* 清消息选区并归还借用键盘 (实现在面板交互区) */
struct InpLine {
    size_t start = 0;             /* 行首字符偏移 */
    float w = 0;                  /* 行宽 */
    std::vector<AiAtom> atoms;
    std::vector<float> cum;       /* 各 atom 行内起点 x */
    size_t len() const { size_t n = 0; for (auto& a : atoms) n += a.t.size(); return n; }
};

/* 输入框布局: 显式 '\n' (Shift+Enter) = 硬换行切逻辑行, 行内再按宽软换行。
   '\n' 不产 atom (AiSegsToAtoms 是消息排版口径, 遇 \n 丢弃) 但记 1 个下标归上一逻辑行尾 —
   整串直喂会让显示下标比编辑下标少 (差 \n 个数), 光标显示位置与删除位置错位 */
static void InputLayout(AiSess* s, float boxInnerW, std::vector<InpLine>* out) {
    out->clear();
    Gdiplus::Bitmap tmp(1, 1, PixelFormat32bppPARGB);
    Gdiplus::Graphics mg(&tmp);
    size_t pos = 0, off = 0;
    while (true) {
        size_t nl = s->input.find(L'\n', pos);
        size_t end = nl == std::wstring::npos ? s->input.size() : nl;
        std::vector<AiBlock::Seg> segs{ AiBlock::Seg{} };
        segs[0].text = s->input.substr(pos, end - pos);
        std::vector<AiAtom> atoms;
        AiSegsToAtoms(mg, s, segs, &atoms);
        InpLine ln; ln.start = off;
        for (auto& a : atoms) {
            if (!ln.atoms.empty() && ln.w + a.w > boxInnerW) {
                out->push_back(ln);
                ln = InpLine(); ln.start = off;
            }
            ln.cum.push_back(ln.w);
            ln.atoms.push_back(a);
            ln.w += a.w;
            off += a.t.size();
        }
        out->push_back(ln);   /* 逻辑行至少落一行 (空行/空文本 = 空行占位) */
        if (nl == std::wstring::npos) break;
        off++;                /* '\n' 本身 (属上一逻辑行尾, 编辑下标含它) */
        pos = nl + 1;
    }
}

/* 行内 x (idx 必须已折算进本行窗口 [start, start+len]; ==start 取 0, ==start+len 取行宽) */
static float InputXInLine(const InpLine& ln, size_t idx) {
    size_t aOff = ln.start;
    for (size_t i = 0; i < ln.atoms.size(); i++) {
        const AiAtom& a = ln.atoms[i];
        if (idx <= aOff) return ln.cum[i];
        if (idx < aOff + a.t.size())
            return ln.cum[i] + a.w * (float)(idx - aOff) / (float)a.t.size();
        aOff += a.t.size();
    }
    return ln.w;
}

/* caret (或任意字符偏移) → 可视位置 (行号, 行内 x); 词内按比例插值。
   行归属/行内下标一律走 ln.start (InputLayout 记账, 含 '\n' 硬换行) — 禁按 len 累加另推一套 */
static void InputPosOf(AiSess* s, const std::vector<InpLine>& lay, float boxInnerW, size_t idx,
                       int* outLine, float* outX) {
    if (idx > s->input.size()) idx = s->input.size();
    int li = 0;
    while (li < (int)lay.size() - 1 && idx > lay[li].start + lay[li].len()) li++;
    *outLine = li;
    *outX = InputXInLine(lay[li], idx);
}

/* 事件点 → 字符偏移 (点定位/拖选; 行按 y, 行内按 x 就近, 词内比例)
   px/py = 面板坐标, boxX/boxY = 输入框左上角 — 文本原点 = 框内缩 10k (渲染同源) */
static size_t InputIndexFromPoint(AiSess* s, const std::vector<InpLine>& lay, float boxInnerW,
                                  float px, float py, float boxX, float boxY) {
    float k = s->scale;
    px -= boxX + 10.0f * k;   /* 面板 x → 行内 x (ln.cum 以文本原点为 0; 直用面板 x 恒大于行宽 = 永远命中行尾) */
    int li = (int)((py - boxY - 10.0f * k) / (AI_LINE_H * k));
    if (li < 0) li = 0;
    if (li >= (int)lay.size()) li = (int)lay.size() - 1;
    const InpLine& ln = lay[li];
    if (px <= 0 || ln.atoms.empty()) return ln.start;
    size_t idx = ln.start;
    for (size_t i = 0; i < ln.atoms.size(); i++) {
        const AiAtom& a = ln.atoms[i];
        if (px < ln.cum[i] + a.w) {   /* 落在本 atom 内: 词内比例细分 */
            if (a.t.size() > 1) idx += (size_t)((px - ln.cum[i]) / a.w * (float)a.t.size() + 0.5f);
            else if (px >= ln.cum[i] + a.w * 0.5f) idx += a.t.size();
            return idx > s->input.size() ? s->input.size() : idx;
        }
        idx += a.t.size();
    }
    return ln.start + ln.len();   /* 行尾 */
}

/* 输入框换行数 (1..4; 布局同源, 超出第 4 行裁剪不显) */
static int InputLineCount(AiSess* s, float boxInnerW) {
    std::vector<InpLine> lay;
    InputLayout(s, boxInnerW, &lay);
    int n = (int)lay.size();
    return n > 4 ? 4 : n;
}

/* 输入框内光标 (列, 行, 行内 x) — caret 折算到换行后的可视位置 */
static void InputCaretPos(AiSess* s, float boxInnerW, int* outLine, float* outX) {
    std::vector<InpLine> lay;
    InputLayout(s, boxInnerW, &lay);
    InputPosOf(s, lay, boxInnerW, s->caret, outLine, outX);
}

static void DrawIconGear(Gdiplus::Graphics& g, float cx, float cy, float r, Gdiplus::Pen* pen) {
    g.DrawEllipse(pen, cx - r, cy - r, r * 2, r * 2);
    for (int i = 0; i < 6; i++) {
        float a = i * 3.1415926f / 3.0f;
        g.DrawLine(pen, cx + cosf(a) * (r + 1.2f * (r / 5.0f + 0.5f)), cy + sinf(a) * (r + 1.2f * (r / 5.0f + 0.5f)),
                   cx + cosf(a) * (r + 3.0f * (r / 5.0f + 0.5f)), cy + sinf(a) * (r + 3.0f * (r / 5.0f + 0.5f)));
    }
}
static void DrawIconClock(Gdiplus::Graphics& g, float cx, float cy, float r, Gdiplus::Pen* pen) {
    g.DrawEllipse(pen, cx - r, cy - r, r * 2, r * 2);
    g.DrawLine(pen, cx, cy, cx, cy - r * 0.62f);
    g.DrawLine(pen, cx, cy, cx + r * 0.5f, cy + r * 0.28f);
}
static void DrawIconPlus(Gdiplus::Graphics& g, float cx, float cy, float r, Gdiplus::Pen* pen) {
    g.DrawLine(pen, cx - r, cy, cx + r, cy);
    g.DrawLine(pen, cx, cy - r, cx, cy + r);
}
static void DrawIconX(Gdiplus::Graphics& g, float cx, float cy, float r, Gdiplus::Pen* pen) {
    g.DrawLine(pen, cx - r, cy - r, cx + r, cy + r);
    g.DrawLine(pen, cx + r, cy - r, cx - r, cy + r);
}

/* 头部按钮 (compact = 窄面板只画图标); 返回按钮左缘供右对齐排布 */
static float DrawHeadBtn(Gdiplus::Graphics& g, AiSess* s, float xRight, float y, float k, int id,
                         const wchar_t* label, void (*icon)(Gdiplus::Graphics&, float, float, float, Gdiplus::Pen*),
                         bool compact, FRect* out) {
    Gdiplus::Bitmap tmp(1, 1, PixelFormat32bppPARGB);
    Gdiplus::Graphics mg(&tmp);
    float tw = compact ? 0 : AiMeasure(mg, s->fTiny, label);
    float bw = 16.0f * k + tw + (compact ? 12.0f * k : 10.0f * k);
    float bh = 26.0f * k;
    float x = xRight - bw;
    *out = FRect{ x, y, bw, bh };
    bool hot = (s->hover == id);
    if (hot) AiRoundRect(g, x, y, bw, bh, 6.0f * k, Gdiplus::SolidBrush(WithA(s->cText, 22)));
    Gdiplus::Pen pen(hot ? s->cText : s->cDim, 1.2f * k);
    icon(g, x + 9.0f * k, y + bh / 2, 5.0f * k, &pen);
    if (!compact) {
        Gdiplus::SolidBrush tb(hot ? s->cText : s->cDim);
        AiText(g, label, s->fTiny, &tb, x + 18.0f * k, y + (bh - AI_TINY_H * k) / 2);
    }
    return x;
}

static void DrawMsg(AiSess* s, Gdiplus::Graphics& g, int mi, float yTop) {
    const AiMsg& m = s->msgs[mi];
    const AiSess::MsgLayout& L = s->lay[mi];
    float k = s->scale;
    float pad = 12.0f * k, av = 24.0f * k, gap = 8.0f * k, bp = 10.0f * k;
    bool user = m.role == 0;
    float bubbleX = user ? (s->w - pad - av - gap - L.bubbleW) : (pad + av + gap);
    float bubbleY = yTop;
    float bubbleH = L.totalH;

    /* 头像 (渐变圆 + AI/我) */
    {
        float acx = user ? (s->w - pad - av / 2) : (pad + av / 2);
        float acy = bubbleY + av / 2 + 2.0f * k;
        Gdiplus::LinearGradientBrush br(Gdiplus::PointF(0, acy - av / 2), Gdiplus::PointF(0, acy + av / 2),
                                        s->cAccent, MixCol(s->cAccent, Gdiplus::Color(0, 0, 0), 0.35f));
        g.FillEllipse(&br, acx - av / 2, acy - av / 2, av, av);
        Gdiplus::SolidBrush wb(Gdiplus::Color(255, 255, 255, 255));
        std::wstring tag = user ? L"我" : L"AI";
        float tw = AiMeasure(g, s->fTiny, tag);
        AiText(g, tag, s->fTiny, &wb, acx - tw / 2, acy - AI_TINY_H * k / 2);
    }

    /* 工具卡片组 (AI 动作; role==2) */
    if (m.role == 2) {
        AiRoundRect(g, bubbleX, bubbleY, L.bubbleW, bubbleH, 10.0f * k, Gdiplus::SolidBrush(s->cPanel));
        AiRoundRectLine(g, bubbleX, bubbleY, L.bubbleW, bubbleH, 10.0f * k,
                        Gdiplus::Pen(s->cLine, 1.0f));
        float innerW = L.bubbleW - bp * 2;
        float yCur = bubbleY + bp;
        for (size_t si = 0; si < m.steps.size() && si < L.stepHs.size(); si++) {
            const AiToolStep& st = m.steps[si];
            float sh = L.stepHs[si];
            AiRoundRect(g, bubbleX + bp, yCur, innerW, sh, 6.0f * k,
                        Gdiplus::SolidBrush(MixCol(s->cPanel, s->cBg, 0.5f)));
            /* 状态图标: 排队=空心圈 执行中=accent 圈 完成=✓ 失败=✕ */
            float icx = bubbleX + bp + 10.0f * k, icy = yCur + 11.0f * k, ir = 4.0f * k;
            if (st.state <= 1) {
                Gdiplus::Pen pen(st.state == 1 ? s->cAccent : WithA(s->cDim, 160), 1.4f * k);
                g.DrawEllipse(&pen, icx - ir, icy - ir, ir * 2, ir * 2);
            } else if (st.state == 2) {
                Gdiplus::Pen pen(Gdiplus::Color(255, 52, 199, 89), 1.6f * k);
                g.DrawLine(&pen, icx - 3.0f * k, icy, icx - 1.0f * k, icy + 2.5f * k);
                g.DrawLine(&pen, icx - 1.0f * k, icy + 2.5f * k, icx + 3.5f * k, icy - 2.5f * k);
            } else {
                Gdiplus::Pen pen(Gdiplus::Color(255, 229, 72, 77), 1.6f * k);
                g.DrawLine(&pen, icx - 2.5f * k, icy - 2.5f * k, icx + 2.5f * k, icy + 2.5f * k);
                g.DrawLine(&pen, icx - 2.5f * k, icy + 2.5f * k, icx + 2.5f * k, icy - 2.5f * k);
            }
            /* 名称 + 模式 */
            Gdiplus::SolidBrush tb(s->cText);
            std::wstring label = st.name.empty() ? L"工具" : st.name;
            if (st.kind == 0 && !st.mode.empty()) label += L" · " + st.mode;
            AiText(g, label, s->fTiny, &tb, icx + 10.0f * k, yCur + 3.0f * k);
            /* 右侧状态文字 */
            Gdiplus::SolidBrush sb(st.state == 3 ? Gdiplus::Color(255, 229, 72, 77) : s->cDim);
            std::wstring stat;
            if (st.state <= 1) stat = st.state == 0 ? L"排队中…" : L"执行中…";
            else if (st.state == 2) {
                if (st.kind == 0 && st.count >= 0) {
                    wchar_t nb[64];
                    swprintf(nb, 64, L"✓ %d 条 · %lld ms", st.count, st.elapsedMs);
                    stat = nb;
                } else stat = L"✓ 完成";
            } else stat = L"✕ 失败";
            {
                float tw = AiMeasure(g, s->fTiny, stat);
                AiText(g, stat, s->fTiny, &sb, bubbleX + bp + innerW - 8.0f * k - tw, yCur + 3.0f * k);
            }
            float yRow = yCur + 22.0f * k;
            /* 查询行 (mono 截断; 完成态可点卡片头展开/收起) */
            if (st.kind == 0 && !st.query.empty()) {
                Gdiplus::SolidBrush qb(MixCol(s->cDim, s->cPanel, 0.25f));
                AiTextTrunc(g, st.query, s->fMono, &qb, bubbleX + bp + 8.0f * k, yRow, innerW - 16.0f * k);
                yRow += 15.0f * k;
            }
            /* 错误折行 */
            if (st.state == 3 && si < L.stepErrLines.size()) {
                Gdiplus::SolidBrush eb(Gdiplus::Color(255, 229, 72, 77));
                for (auto& ln : L.stepErrLines[si]) {
                    for (auto& r : ln.runs)
                        if (!r.text.empty()) AiText(g, r.text, AiFontOf(s, false, false, r.cjk), &eb,
                                                    bubbleX + bp + 8.0f * k + r.x, yRow);
                    yRow += ln.h;
                }
                yRow += 2.0f * k;
            }
            /* 展开的样本列表 */
            if (st.open && !st.top.empty()) {
                yRow += 4.0f * k;
                Gdiplus::SolidBrush pb(s->cDim);
                for (size_t ti = 0; ti < st.top.size(); ti++) {
                    wchar_t no[8];
                    swprintf(no, 8, L"%d.", (int)(ti + 1));
                    std::wstring noS = no;
                    AiText(g, noS, s->fTiny, &pb, bubbleX + bp + 8.0f * k, yRow);
                    float nx = bubbleX + bp + 8.0f * k + AiMeasure(g, s->fTiny, noS) + 4.0f * k;
                    AiTextTrunc(g, st.top[ti], s->fTiny, &pb, nx, yRow, bubbleX + bp + innerW - 8.0f * k - nx);
                    yRow += 15.0f * k;
                }
            }
            yCur += sh + (si + 1 < m.steps.size() ? 6.0f * k : 0.0f);
        }
        return;
    }

    /* 气泡底 */
    AiRoundRect(g, bubbleX, bubbleY, L.bubbleW, bubbleH, 10.0f * k,
                Gdiplus::SolidBrush(user ? MixCol(s->cAccent, s->cBg, 0.82f) : s->cPanel));
    AiRoundRectLine(g, bubbleX, bubbleY, L.bubbleW, bubbleH, 10.0f * k,
                    Gdiplus::Pen(user ? WithA(s->cAccent, 150) : s->cLine, 1.0f));

    float yCur = bubbleY + bp;
    /* 推理折叠块 (AI 消息且有推理) */
    if (!m.reason.empty()) {
        float rx = bubbleX + bp, rw = L.bubbleW - bp * 2;
        float rh = m.reasonOpen ? L.reasonH : 24.0f * k;
        AiRoundRect(g, rx, yCur, rw, rh, 6.0f * k, Gdiplus::SolidBrush(MixCol(s->cPanel, s->cBg, 0.55f)));
        Gdiplus::SolidBrush db(s->cDim);
        float chx = rx + 10.0f * k, chy = yCur + 12.0f * k;
        Gdiplus::Pen pen(s->cAccent, 1.4f * k);
        if (m.reasonOpen) {   /* ▾ */
            g.DrawLine(&pen, chx, chy - 2.0f * k, chx + 5.0f * k, chy - 2.0f * k);
            g.DrawLine(&pen, chx, chy - 2.0f * k, chx + 2.5f * k, chy + 1.5f * k);
            g.DrawLine(&pen, chx + 2.5f * k, chy + 1.5f * k, chx + 5.0f * k, chy - 2.0f * k);
        } else {              /* ▸ */
            g.DrawLine(&pen, chx, chy - 4.0f * k, chx + 4.0f * k, chy - 1.5f * k);
            g.DrawLine(&pen, chx + 4.0f * k, chy - 1.5f * k, chx, chy + 1.0f * k);
        }
        AiTextTrunc(g, L"已深度思考（推理过程）", s->fTiny, &db, rx + 22.0f * k, yCur + 4.0f * k, rw - 30.0f * k);
        if (m.reasonOpen) {
            float ty = yCur + 22.0f * k;
            for (auto& ln : L.reasonLines) {
                for (auto& r : ln.runs)
                    if (!r.text.empty()) AiText(g, r.text, AiFontOf(s, false, false, r.cjk), &db, rx + 6.0f * k + r.x, ty);
                ty += ln.h;
            }
        }
        yCur += (m.reasonOpen ? L.reasonH + 4.0f * k : 24.0f * k);
    }
    /* 正文行 (含拖选选区底) — 选择粒度 = runs (含前缀占位 run, text 空跳过) */
    int saL = 0, saA = 0, sbL = 0, sbA = 0;
    bool txtSelHere = s->txtSel && s->selMsg == mi;
    if (txtSelHere) MsgSelNorm(s, &saL, &saA, &sbL, &sbA);
    int li2 = 0;
    for (auto& ln : L.lines) {
        if (txtSelHere && li2 >= saL && li2 <= sbL && !ln.runs.empty()) {
            int lo = li2 == saL ? saA : 0;
            int hi = li2 == sbL ? sbA : (int)ln.runs.size();
            if (hi > (int)ln.runs.size()) hi = (int)ln.runs.size();
            if (hi > lo) {
                float x1 = ln.runs[lo].x;
                float x2 = ln.runs[hi - 1].x + ln.runs[hi - 1].w;
                if (x2 > x1)
                    AiFillRect(g, Gdiplus::SolidBrush(Gdiplus::Color(60, s->cAccent.GetR(), s->cAccent.GetG(), s->cAccent.GetB())),
                                    Gdiplus::RectF(bubbleX + bp + x1, yCur - 1.0f * k, x2 - x1, ln.h));
            }
        }
        if (!ln.prefix.empty()) AiText(g, ln.prefix, s->fBody, Gdiplus::SolidBrush(s->cDim), bubbleX + bp, yCur);
        for (auto& r : ln.runs) {
            if (r.text.empty()) continue;
            Gdiplus::Font* f = AiFontOf(s, r.code, r.bold, r.cjk);
            Gdiplus::Color c = r.code ? MixCol(s->cAccent, s->cText, 0.45f)
                              : r.accent ? s->cAccent : s->cText;
            Gdiplus::SolidBrush br(c);
            AiText(g, r.text, f, &br, bubbleX + bp + r.x, yCur);
        }
        yCur += ln.h;
        li2++;
    }
    /* 流式光标 (最后一条 AI 文本消息发送中; 工具卡片组不画 — 卡片自带"执行中…"状态) */
    if (s->sending && !user && m.role == 1 && mi == (int)s->msgs.size() - 1 && ((GetTickCount64() / 500) & 1)) {
        float lastW = 0;
        if (!L.lines.empty())
            for (auto& r : L.lines.back().runs) lastW = (r.x + r.w > lastW) ? r.x + r.w : lastW;
        AiRoundRect(g, bubbleX + bp + lastW + 2.0f * k, yCur - AI_LINE_H * k + 3.0f * k,
                    2.0f * k, AI_LINE_H * k - 6.0f * k, 1.0f * k, Gdiplus::SolidBrush(s->cAccent));
    }
}

static void RenderSession(AiSess* s) {
    SessEnsureFonts(s);
    SessEnsureSurface(s);
    float k = s->scale;
    Gdiplus::Bitmap bm(s->bw, s->bh, s->stride, PixelFormat32bppPARGB, s->px.data());
    Gdiplus::Graphics g(&bm);
    g.SetPageUnit(Gdiplus::UnitPixel);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    g.Clear(s->cBg);

    float pad = 12.0f * k;
    float headH = AI_HEAD_H * k;
    float sendW = 56.0f * k, sendH = 30.0f * k;
    float boxInnerW = s->w - pad * 2 - sendW - 8.0f * k - 10.0f * k * 2;
    int inLines = InputLineCount(s, boxInnerW);
    float boxH = inLines * AI_LINE_H * k + 10.0f * k * 2;
    float hintH = 16.0f * k;
    float inH = boxH + hintH + 12.0f * k;
    float msgsTop = headH;
    float msgsH = s->bh - headH - inH;
    if (msgsH < 40.0f * k) msgsH = 40.0f * k;

    /* ===== 头部 ===== */
    {
        Gdiplus::SolidBrush tb(s->cText);
        AiText(g, L"AI 助手", s->fTitle, &tb, pad, (headH - 21.0f * k) / 2);
        float tx = pad + AiMeasure(g, s->fTitle, L"AI 助手") + 8.0f * k;
        Gdiplus::Color dotC = s->netStatus == 1 ? Gdiplus::Color(255, 52, 199, 89)
                            : s->netStatus == 2 ? Gdiplus::Color(255, 229, 72, 77)
                                                : WithA(s->cDim, 200);
        Gdiplus::SolidBrush dotBr(dotC);
        g.FillEllipse(&dotBr, tx, headH / 2 - 3.5f * k, 7.0f * k, 7.0f * k);
        tx += 11.0f * k;
        std::wstring model = s->netStatus == 0 && g_cfg.apiKey.empty() ? L"未配置" : g_cfg.model;
        Gdiplus::SolidBrush db(s->cDim);
        float maxModelW = s->w * 0.42f - tx;
        if (maxModelW > 30.0f * k) AiTextTrunc(g, model, s->fTiny, &db, tx, (headH - AI_TINY_H * k) / 2, maxModelW);
        bool compact = s->w < 470.0f * k;
        float xr = s->w - pad;
        float by = (headH - 26.0f * k) / 2;
        /* ✕ 恒图标在最右 (宿主头部带已由本插件整块接管, 关闭会话入口在此; 标题栏 ✕ 惯例) */
        xr = DrawHeadBtn(g, s, xr, by, k, HIT_CLOSE, L"关闭", DrawIconX, true, &s->hClose);
        xr = DrawHeadBtn(g, s, xr, by, k, HIT_HNEW, L"新对话", DrawIconPlus, compact, &s->hNew);
        xr = DrawHeadBtn(g, s, xr, by, k, HIT_HHIST, L"历史对话", DrawIconClock, compact, &s->hHist);
        DrawHeadBtn(g, s, xr, by, k, HIT_HSET, L"接口设置", DrawIconGear, compact, &s->hSet);
        AiFillRect(g, Gdiplus::SolidBrush(WithA(s->cLine, 160)), 0.0f, headH - 1.0f, (float)s->bw, 1.0f);
    }

    LayoutAll(s, false);

    /* ===== 消息流 ===== */
    {
        double maxScroll = s->contentH - msgsH;
        if (maxScroll < 0) maxScroll = 0;
        if (s->sticky) s->scrollY = maxScroll;
        if (s->scrollY > maxScroll) s->scrollY = maxScroll;
        if (s->scrollY < 0) s->scrollY = 0;
        Gdiplus::GraphicsState gs = g.Save();
        g.SetClip(Gdiplus::RectF(0, msgsTop, (float)s->bw, msgsH));
        g.TranslateTransform(0, -s->scrollY);
        if (s->msgs.empty() && !s->sending) {
            /* 空态 */
            float cx = s->bw / 2.0f, cy = msgsTop + msgsH / 2.0f;
            float bs = 56.0f * k;
            Gdiplus::LinearGradientBrush br(Gdiplus::PointF(cx - bs / 2, cy - bs / 2 - 40.0f * k),
                                            Gdiplus::PointF(cx + bs / 2, cy + bs / 2 - 40.0f * k),
                                            s->cAccent, MixCol(s->cAccent, Gdiplus::Color(139, 92, 246), 0.75f));
            Gdiplus::GraphicsPath p;
            float rr = 16.0f * k;
            p.AddArc(cx - bs / 2, cy - bs / 2 - 40.0f * k, rr * 2, rr * 2, 180, 90);
            p.AddArc(cx + bs / 2 - rr * 2, cy - bs / 2 - 40.0f * k, rr * 2, rr * 2, 270, 90);
            p.AddArc(cx + bs / 2 - rr * 2, cy + bs / 2 - rr * 2 - 40.0f * k, rr * 2, rr * 2, 0, 90);
            p.AddArc(cx - bs / 2, cy + bs / 2 - rr * 2 - 40.0f * k, rr * 2, rr * 2, 90, 90);
            p.CloseFigure();
            g.FillPath(&br, &p);
            Gdiplus::SolidBrush wb(Gdiplus::Color(255, 255, 255, 255));
            float aiw = AiMeasure(g, s->fTitle, L"AI");
            AiText(g, L"AI", s->fTitle, &wb, cx - aiw / 2, cy - 40.0f * k - 12.0f * k);
            Gdiplus::SolidBrush tb(s->cText);
            std::wstring t1 = L"问我任何问题";
            float w1 = AiMeasure(g, s->fBodyB, t1);
            AiText(g, t1, s->fBodyB, &tb, cx - w1 / 2, cy + 32.0f * k);
            Gdiplus::SolidBrush db(s->cDim);
            std::wstring t2 = g_cfg.apiKey.empty() ? L"尚未配置接口密钥 — 点右上角 ⚙ 接口设置" : L"Enter 发送，Shift + Enter 换行";
            float w2 = AiMeasure(g, s->fTiny, t2);
            AiText(g, t2, s->fTiny, &db, cx - w2 / 2, cy + 52.0f * k);
        }
        for (int i = 0; i < (int)s->msgs.size(); i++) {
            float y = msgsTop + s->layOffsets[i] - (float)s->scrollY;   /* 屏幕域: 视口剔除判定用 */
            if (y > msgsTop + msgsH || y + s->lay[i].totalH < msgsTop) continue;
            /* 传内容域 y — translate 已做滚动偏移; 曾再减一次 scrollY = 双重平移,
               滚动越深偏差越大 (2 倍速跳变, 滚一点整条回答飞出视口) */
            DrawMsg(s, g, i, msgsTop + s->layOffsets[i]);
        }
        g.Restore(gs);
        /* 滚动条 */
        if (s->contentH > msgsH && msgsH > 10.0f * k) {
            float trackX = s->bw - 8.0f * k;
            float thumbH = msgsH * msgsH / s->contentH;
            if (thumbH < 24.0f * k) thumbH = 24.0f * k;
            float maxScroll = s->contentH - msgsH;
            float thumbY = msgsTop + (msgsH - thumbH) * (float)(s->scrollY / maxScroll);
            s->hThumb = FRect{ trackX, thumbY, 6.0f * k, thumbH };
            s->thumbTrackY = msgsTop;
            s->thumbTrackH = msgsH;
            BYTE a = (s->hover == HIT_THUMB || s->press == HIT_THUMB || s->pressGrab) ? 170 : 80;
            AiRoundRect(g, trackX, thumbY, 6.0f * k, thumbH, 3.0f * k, Gdiplus::SolidBrush(WithA(s->cDim, a)));
        } else {
            s->hThumb = FRect{};
        }
    }

    /* ===== 输入区 ===== */
    {
        float boxY = s->bh - inH + 4.0f * k;
        float boxW = s->w - pad * 2 - sendW - 8.0f * k;
        s->hInput = FRect{ pad, boxY, boxW, boxH };
        AiRoundRect(g, pad, boxY, boxW, boxH, 8.0f * k,
                    Gdiplus::SolidBrush(s->inputFocus ? MixCol(s->cPanel, s->cAccent, 0.06f) : s->cPanel));
        AiRoundRectLine(g, pad, boxY, boxW, boxH, 8.0f * k,
                        Gdiplus::Pen(s->inputFocus ? s->cAccent : s->cLine, s->inputFocus ? 1.3f : 1.0f));
        /* 文本 / 选区底 / 占位 / 光标 (布局同源 InputLayout; 空文本照常画光标 — 搜索框口径) */
        g.SetClip(Gdiplus::RectF(pad + 2.0f * k, boxY, boxW - 4.0f * k, boxH));
        std::vector<InpLine> lay;
        InputLayout(s, boxInnerW, &lay);
        size_t selA = 0, selB = 0;
        bool hasSel = InputSelRange(s, &selA, &selB);
        float ty = boxY + 10.0f * k;
        for (int li = s->inScroll; li < (int)lay.size() && li < s->inScroll + 4; li++) {
            const InpLine& ln = lay[li];
            if (hasSel) {
                size_t sLo = selA > ln.start ? selA : ln.start;
                size_t sHi = selB < ln.start + ln.len() ? selB : ln.start + ln.len();
                if (sLo < sHi) {
                    float x1 = InputXInLine(ln, sLo), x2 = InputXInLine(ln, sHi);
                    if (x2 > x1)
                        AiFillRect(g, Gdiplus::SolidBrush(Gdiplus::Color(70, s->cAccent.GetR(), s->cAccent.GetG(), s->cAccent.GetB())),
                                        Gdiplus::RectF(pad + 10.0f * k + x1, ty - 1.0f * k, x2 - x1, AI_LINE_H * k));
                }
            }
            Gdiplus::SolidBrush tb(s->cText);
            for (size_t i = 0; i < ln.atoms.size(); i++)
                AiText(g, ln.atoms[i].t, AiFontOf(s, ln.atoms[i].code, ln.atoms[i].bold, ln.atoms[i].cjk),
                       &tb, pad + 10.0f * k + ln.cum[i], ty);
            ty += AI_LINE_H * k;
        }
        if (s->input.empty()) {
            Gdiplus::SolidBrush db(WithA(s->cDim, 150));
            AiText(g, L"输入消息，Enter 发送…", s->fBody, &db, pad + 10.0f * k, boxY + 10.0f * k);
        }
        if (s->inputFocus && s->winActive && ((GetTickCount64() / 530) & 1)) {
            int cl = 0; float cx = 0;
            InputPosOf(s, lay, boxInnerW, s->caret, &cl, &cx);
            if (cl >= s->inScroll && cl < s->inScroll + 4)
                AiFillRect(g, Gdiplus::SolidBrush(s->cText),
                                Gdiplus::RectF(pad + 10.0f * k + cx, boxY + 10.0f * k + (cl - s->inScroll) * AI_LINE_H * k + 2.0f * k,
                                               1.4f * k, AI_LINE_H * k - 4.0f * k));
        }
        g.ResetClip();
        /* 溢出滚动条 (覆盖式, 样式随列表: 圆角细条; 可拖拽/悬停提亮) */
        {
            int over = (int)lay.size() - 4;
            if (over > 0) {
                float trackY = boxY + 4.0f * k, trackH = boxH - 8.0f * k;
                float thumbH = maxf(trackH * 4.0f / (float)lay.size(), 14.0f * k);
                float thumbY = trackY + (trackH - thumbH) * ((float)s->inScroll / (float)over);
                s->hInThumb = FRect{ pad + boxW - 9.0f * k, thumbY, 5.0f * k, thumbH };
                s->inTrackY = trackY;
                s->inTrackH = trackH;
                BYTE ta = (s->inGrab > 0) ? 170 : 90;
                AiRoundRect(g, s->hInThumb.x, thumbY, 5.0f * k, thumbH, 2.5f * k,
                            Gdiplus::SolidBrush(WithA(s->cDim, ta)));
            } else {
                s->hInThumb = FRect{};
                s->inTrackY = s->inTrackH = 0;
            }
        }
        /* 发送 / 停止 */
        float sendX = s->w - pad - sendW;
        /* 单行时与输入框垂直居中; 多行增高后与框底对齐 (恒贴底 = 单行观感下沉不对齐) */
        float sendY = boxH > 40.0f * k + 0.5f ? boxY + boxH - sendH : boxY + (boxH - sendH) / 2.0f;
        s->hSend = FRect{ sendX, sendY, sendW, sendH };
        bool stop = s->sending;
        bool enabled = stop || !s->input.empty();
        Gdiplus::Color c1 = stop ? Gdiplus::Color(255, 229, 72, 77) : s->cAccent;
        Gdiplus::Color c2 = stop ? Gdiplus::Color(255, 200, 50, 55) : MixCol(s->cAccent, Gdiplus::Color(255, 255, 255), 0.14f);
        Gdiplus::LinearGradientBrush sbr(Gdiplus::PointF(0, sendY), Gdiplus::PointF(0, sendY + sendH), c2, c1);
        if (!enabled) {
            Gdiplus::SolidBrush dimbr(MixCol(s->cPanel, s->cDim, 0.35f));
            AiRoundRect(g, sendX, sendY, sendW, sendH, 7.0f * k, &dimbr);
        } else {
            AiRoundRect(g, sendX, sendY, sendW, sendH, 7.0f * k, &sbr);
        }
        Gdiplus::SolidBrush wb(Gdiplus::Color(255, 255, 255, 255));
        std::wstring bl = stop ? L"停止" : L"发送";
        float blw = AiMeasure(g, s->fBodyB, bl);
        AiText(g, bl, s->fBodyB, &wb, sendX + (sendW - blw) / 2, sendY + (sendH - AI_LINE_H * k) / 2);
        /* 提示行 */
        Gdiplus::SolidBrush fb(WithA(s->cDim, 120));
        AiText(g, L"Enter 发送，Shift + Enter 换行", s->fTiny, &fb, pad, boxY + boxH + 4.0f * k);
    }

    /* ===== 历史侧栏 (覆盖输入区之上的右层) ===== */
    if (s->sideOpen) {
        float sideW = minf(320.0f * k, s->w * 0.62f);
        float sx = s->w - sideW;
        s->sidePanel = FRect{ sx, headH, sideW, s->bh - headH };
        AiFillRect(g, Gdiplus::SolidBrush(MixCol(s->cPanel, s->cBg, 0.5f)),
                        Gdiplus::RectF(sx, headH, sideW, s->bh - headH));
        AiFillRect(g, Gdiplus::SolidBrush(s->cLine), Gdiplus::RectF(sx, headH, 1.0f, s->bh - headH));
        Gdiplus::SolidBrush tb(s->cText);
        AiText(g, L"历史对话", s->fBodyB, &tb, sx + 14.0f * k, headH + 10.0f * k);
        std::wstring clearLbl = s->clearArm ? L"确认清空?" : L"清空记录";
        Gdiplus::SolidBrush cb(s->clearArm ? Gdiplus::Color(255, 229, 72, 77) : s->cDim);
        float cw = AiMeasure(g, s->fTiny, clearLbl);
        s->hClear = FRect{ s->w - 14.0f * k - cw, headH + 12.0f * k, cw + 8.0f * k, 18.0f * k };
        AiText(g, clearLbl, s->fTiny, &cb, s->w - 14.0f * k - cw, headH + 12.0f * k);
        /* 行 ( newest first ) */
        float rowH = 50.0f * k;
        float y = headH + 40.0f * k - (float)s->sideScroll;
        s->hRows.clear();
        s->hRowDel.clear();
        g.SetClip(Gdiplus::RectF(sx, headH + 36.0f * k, sideW, s->bh - headH - 36.0f * k));
        int vi = 0;
        for (int i = (int)g_hist.size() - 1; i >= 0; i--, vi++) {
            if (y > s->bh) break;
            float rh = rowH;
            if (y + rh < headH) { y += rh; continue; }
            const AiConv& c = g_hist[i];
            bool active = c.id == s->curId;
            if (active) AiFillRect(g, Gdiplus::SolidBrush(WithA(s->cAccent, 30)),
                                        Gdiplus::RectF(sx, y, sideW, rh));
            if (s->hover == HIT_SBROW + vi)
                AiFillRect(g, Gdiplus::SolidBrush(WithA(s->cText, 14)), Gdiplus::RectF(sx, y, sideW, rh));
            Gdiplus::SolidBrush tb2(active ? s->cText : MixCol(s->cText, s->cDim, 0.35f));
            AiTextTrunc(g, c.title, s->fBody, &tb2, sx + 14.0f * k, y + 8.0f * k, sideW - 60.0f * k);
            Gdiplus::SolidBrush db2(s->cDim);
            AiText(g, TimeTextOf(c.t), s->fTiny, &db2, sx + 14.0f * k, y + 28.0f * k);
            /* 删除 ✕ */
            float dx = s->w - 26.0f * k, dy = y + 8.0f * k;
            bool hotDel = s->hover == HIT_SBDEL + vi;
            Gdiplus::Pen xp(hotDel ? Gdiplus::Color(255, 229, 72, 77) : s->cDim, 1.2f * k);
            g.DrawLine(&xp, dx, dy, dx + 9.0f * k, dy + 9.0f * k);
            g.DrawLine(&xp, dx + 9.0f * k, dy, dx, dy + 9.0f * k);
            FRect rr; rr.x = sx; rr.y = y; rr.w = sideW; rr.h = rh;
            s->hRows.push_back(rr);
            s->hRowDel.push_back(FRect{ dx - 3.0f * k, dy - 3.0f * k, 15.0f * k, 15.0f * k });
            s->hRowDel.back().w = 15.0f * k;
            s->hRows.back().h = rh;
            s->hRows.back().y = y;
            s->hRows.back().x = sx;
            s->hRows.back().w = sideW;
            s->hRowDel.back().x = dx - 3.0f * k;
            s->hRowDel.back().y = dy - 3.0f * k;
            y += rh;
        }
        g.ResetClip();
    } else {
        s->hClear = FRect{};
        s->sidePanel = FRect{};
    }

    /* ===== 输入框右键编辑菜单 (浮层; 行几何回填 menuRows 供命中) ===== */
    if (s->menuOpen) {
        static const wchar_t* menuLbl[5] = { L"剪切", L"复制", L"粘贴", L"全选", L"删除" };
        bool sel = s->anchor != s->caret;
        bool en[5] = { sel, sel, true, !s->input.empty(), sel };
        AiRoundRect(g, s->menuBox.x, s->menuBox.y, s->menuBox.w, s->menuBox.h, 6.0f * k,
                    Gdiplus::SolidBrush(MixCol(s->cPanel, s->cBg, 0.35f)));
        AiRoundRectLine(g, s->menuBox.x, s->menuBox.y, s->menuBox.w, s->menuBox.h, 6.0f * k,
                        Gdiplus::Pen(s->cLine, 1.0f));
        s->menuRows.clear();
        float iy = s->menuBox.y + 4.0f * k;
        for (int i = 0; i < 5; i++) {
            s->menuRows.push_back(FRect{ s->menuBox.x + 4.0f * k, iy, s->menuBox.w - 8.0f * k, 26.0f * k });
            Gdiplus::SolidBrush ib(en[i] ? s->cText : WithA(s->cDim, 90));
            AiText(g, menuLbl[i], s->fBody, &ib, s->menuBox.x + 14.0f * k, iy + 4.0f * k);
            iy += 26.0f * k;
        }
    }

    /* ===== 接口设置对话框 (卡片高度随内容排; 点蒙层空白 = 取消) ===== */
    if (s->dlg) {
        AiFillRect(g, Gdiplus::SolidBrush(Gdiplus::Color(115, 0, 0, 0)),
                   Gdiplus::RectF(0, 0, (float)s->bw, (float)s->bh));
        float cw = minf(300.0f * k, s->w - 24.0f * k);
        float cx = (s->w - cw) / 2;
        /* 高度自内容排: 标题44 + 3×字段52 + 复选24 + 备注19 + 按钮带40 */
        float ch = 284.0f * k;
        float maxCh = s->bh - headH - 16.0f * k;
        if (ch > maxCh) ch = maxCh;   /* 极矮面板兜底 (内容可能溢出, 按钮仍可见) */
        float cy = headH + 8.0f * k + maxf(0.0f, (s->bh - headH - 16.0f * k - ch) / 2);
        s->dCardR = FRect{ cx, cy, cw, ch };
        AiRoundRect(g, cx, cy, cw, ch, 10.0f * k, Gdiplus::SolidBrush(s->cPanel));
        AiRoundRectLine(g, cx, cy, cw, ch, 10.0f * k, Gdiplus::Pen(s->cLine, 1.0f));
        Gdiplus::SolidBrush tb(s->cText);
        AiText(g, L"接口设置", s->fBodyB, &tb, cx + 16.0f * k, cy + 14.0f * k);
        struct Fld { const wchar_t* lab; std::wstring val; std::wstring ph; int id; bool mask; FRect* rc; };
        FRect r1, r2, r3;
        Fld flds[3] = {
            { L"接口地址", s->dUrl, L"https://api.deepseek.com", 1, false, &r1 },
            { L"API 密钥", s->dKey, L"sk-…", 2, true, &r2 },
            { L"模型", s->dModel, L"deepseek-chat", 3, false, &r3 },
        };
        float fy = cy + 44.0f * k;
        Gdiplus::SolidBrush lb(s->cDim);
        for (auto& f : flds) {
            AiText(g, f.lab, s->fTiny, &lb, cx + 16.0f * k, fy);
            float bx = cx + 16.0f * k, bw = cw - 32.0f * k, bh2 = 28.0f * k;
            fy += 16.0f * k;
            bool foc = s->dFocus == f.id;
            bool hotF = (s->hover == f.id) && !foc;
            AiRoundRect(g, bx, fy, bw, bh2, 6.0f * k, Gdiplus::SolidBrush(MixCol(s->cBg, s->cPanel, 0.5f)));
            AiRoundRectLine(g, bx, fy, bw, bh2, 6.0f * k,
                            Gdiplus::Pen(foc ? s->cAccent : hotF ? MixCol(s->cLine, s->cText, 0.4f) : s->cLine,
                                         foc ? 1.3f : 1.0f));
            std::wstring show = f.mask ? std::wstring(minf((size_t)14, f.val.size()), L'●') : f.val;
            if (show.empty()) {
                Gdiplus::SolidBrush pb(WithA(s->cDim, 130));
                AiTextMid(g, f.ph, s->fTiny, &pb, bx + 8.0f * k, fy, bh2);
            } else {
                std::wstring vis;
                size_t skip = FieldVisWindow(s, show, bw - 16.0f * k, &vis);   /* 裁剪窗 (渲染/点定位同源) */
                Gdiplus::RectF tm;
                g.MeasureString(vis.c_str(), (INT)vis.size(), s->fBody, Gdiplus::PointF(0, 0), AiTextFmt(), &tm);
                float ty2 = fy + (bh2 - tm.Height) / 2;
                if (foc && s->dAnchor != s->dCaret) {   /* 选区底 */
                    size_t lo = s->dAnchor < s->dCaret ? s->dAnchor : s->dCaret;
                    size_t hi = s->dAnchor < s->dCaret ? s->dCaret : s->dAnchor;
                    if (lo < skip) lo = skip;
                    if (hi > skip + vis.size()) hi = skip + vis.size();
                    if (lo < hi) {
                        float x1 = FieldWidthOf(s, vis, lo - skip);
                        float x2 = FieldWidthOf(s, vis, hi - skip);
                        if (x2 > x1)
                            AiFillRect(g, Gdiplus::SolidBrush(Gdiplus::Color(70, s->cAccent.GetR(), s->cAccent.GetG(), s->cAccent.GetB())),
                                            Gdiplus::RectF(bx + 8.0f * k + x1, ty2, x2 - x1, tm.Height));
                    }
                }
                AiText(g, vis, s->fBody, &tb, bx + 8.0f * k, ty2);
                if (foc && s->dAnchor == s->dCaret && s->winActive && ((GetTickCount64() / 530) & 1)) {   /* 光标 (无选区才画; 有选区 = 只见选区) */
                    float cx = FieldWidthOf(s, vis, s->dCaret - skip);
                    AiFillRect(g, Gdiplus::SolidBrush(s->cText),
                               Gdiplus::RectF(bx + 8.0f * k + cx, ty2, 1.4f * k, tm.Height));
                }
            }
            *f.rc = FRect{ bx, fy, bw, bh2 };
            fy += bh2 + 8.0f * k;
        }
        s->dUrlR = r1;
        s->dKeyR = r2;
        s->dModelR = r3;
        /* 深度思考 (整行命中; hover 提亮) */
        {
            float cbx = cx + 16.0f * k;
            float cby = fy + 2.0f * k;
            float rowW = cw - 32.0f * k;
            s->dChkR = FRect{ cbx, cby - 3.0f * k, rowW, 22.0f * k };
            if (s->hover == HIT_DCHK)
                AiRoundRect(g, cbx - 6.0f * k, cby - 3.0f * k, rowW + 12.0f * k, 22.0f * k, 6.0f * k,
                            Gdiplus::SolidBrush(WithA(s->cText, 22)));
            AiRoundRect(g, cbx, cby, 15.0f * k, 15.0f * k, 4.0f * k,
                        Gdiplus::SolidBrush(s->dReason ? s->cAccent : MixCol(s->cBg, s->cPanel, 0.5f)));
            AiRoundRectLine(g, cbx, cby, 15.0f * k, 15.0f * k, 4.0f * k,
                            Gdiplus::Pen(s->dReason ? s->cAccent : s->cLine, 1.0f));
            if (s->dReason) {
                Gdiplus::Pen chk(Gdiplus::Color(255, 255, 255, 255), 1.6f * k);
                g.DrawLine(&chk, cbx + 3.5f * k, cby + 8.0f * k, cbx + 6.5f * k, cby + 11.0f * k);
                g.DrawLine(&chk, cbx + 6.5f * k, cby + 11.0f * k, cbx + 11.5f * k, cby + 4.0f * k);
            }
            Gdiplus::SolidBrush lb2(s->cText);
            AiText(g, L"深度思考", s->fTiny, &lb2, cbx + 22.0f * k, cby);
            Gdiplus::SolidBrush db2(s->cDim);
            AiText(g, L"(reasoning.effort=high)", s->fTiny, &db2,
                   cbx + 22.0f * k + AiMeasure(g, s->fTiny, L"深度思考 ") + 4.0f * k, cby);
            fy += 24.0f * k;
        }
        Gdiplus::SolidBrush nb(WithA(s->cDim, 140));
        AiTextTrunc(g, L"兼容 OpenAI Responses 接口，保存后立即生效", s->fTiny, &nb, cx + 16.0f * k, fy + 1.0f * k, cw - 32.0f * k);
        /* 按钮 (hover 提亮; 底垫 12k) */
        Gdiplus::SolidBrush wb(Gdiplus::Color(255, 255, 255, 255));
        float by2 = cy + ch - 40.0f * k;
        float bw2 = 74.0f * k, bh3 = 28.0f * k;
        s->dSaveR = FRect{ cx + cw - 16.0f * k - bw2, by2, bw2, bh3 };
        AiRoundRect(g, s->dSaveR.x, s->dSaveR.y, bw2, bh3, 6.0f * k,
                    Gdiplus::SolidBrush(s->hover == HIT_DSAVE ? MixCol(s->cAccent, Gdiplus::Color(255, 255, 255), 0.18f)
                                                              : s->cAccent));
        float sw = AiMeasure(g, s->fBodyB, L"保存");
        AiText(g, L"保存", s->fBodyB, &wb, s->dSaveR.x + (bw2 - sw) / 2, by2 + (bh3 - AI_LINE_H * k) / 2);
        s->dCancelR = FRect{ s->dSaveR.x - 8.0f * k - bw2, by2, bw2, bh3 };
        AiRoundRect(g, s->dCancelR.x, s->dCancelR.y, bw2, bh3, 6.0f * k,
                    Gdiplus::SolidBrush(MixCol(s->cPanel, s->cBg, 0.4f)));
        AiRoundRectLine(g, s->dCancelR.x, s->dCancelR.y, bw2, bh3, 6.0f * k,
                        Gdiplus::Pen(s->hover == HIT_DCANCEL ? MixCol(s->cLine, s->cText, 0.45f) : s->cLine, 1.0f));
        float cw2 = AiMeasure(g, s->fBody, L"取消");
        AiText(g, L"取消", s->fBody, &tb, s->dCancelR.x + (bw2 - cw2) / 2, by2 + (bh3 - AI_LINE_H * k) / 2);
    }
}

/* 渲染 + 交付 (UI 线程) */
static void RenderDeliver(AiSess* s) {
    if (!s || !s->inUse || !s->w) return;
    RenderSession(s);
    if (!HOST_PANEL_OK || !g_host) return;
    g_host->PanelDeliverBitmap(g_ctx, s->tok, s->serial, s->bw, s->bh, s->px.data(), s->stride);
}

/* ==================== 交互 (命中 / 鼠标 / 滚轮 / 键盘) ==================== */

static void UpdateImeAnchor(AiSess* s);
static void SendCurrent(AiSess* s);

/* 输入框几何 (渲染/命中/IME 锚点三处同源) */
static void InputGeom(AiSess* s, float* bx, float* by, float* bw, float* bh, float* inH) {
    float k = s->scale;
    float pad = 12.0f * k, sendW = 56.0f * k;
    float boxInnerW = s->w - pad * 2 - sendW - 8.0f * k - 10.0f * k * 2;
    int inLines = InputLineCount(s, boxInnerW);
    float boxH = inLines * AI_LINE_H * k + 10.0f * k * 2;
    float ih = boxH + 16.0f * k + 12.0f * k;
    if (bx) *bx = pad;
    if (by) *by = s->bh - ih + 4.0f * k;
    if (bw) *bw = s->w - pad * 2 - sendW - 8.0f * k;
    if (bh) *bh = boxH;
    if (inH) *inH = ih;
}

/* 光标所在行滚入输入框可视窗口 (4 行); 未溢出归零。光标移动/内容变化路径经 UpdateImeAnchor 调 */
static void InEnsureCaret(AiSess* s) {
    float bx, by, bw2, bh2;
    InputGeom(s, &bx, &by, &bw2, &bh2, NULL);
    std::vector<InpLine> lay;
    InputLayout(s, bw2 - 20.0f * s->scale, &lay);
    int line; float x;
    InputPosOf(s, lay, bw2 - 20.0f * s->scale, s->caret, &line, &x);
    if ((int)lay.size() <= 4) { s->inScroll = 0; return; }
    if (line < s->inScroll) s->inScroll = line;
    if (line > s->inScroll + 3) s->inScroll = line - 3;
    if (s->inScroll > (int)lay.size() - 4) s->inScroll = (int)lay.size() - 4;
    if (s->inScroll < 0) s->inScroll = 0;
}

/* 输入框溢出行数 (>0 = 内容超出 4 行可视窗, 可滚动) */
static int InOverflowLines(AiSess* s) {
    float bx, by, bw2, bh2;
    InputGeom(s, &bx, &by, &bw2, &bh2, NULL);
    std::vector<InpLine> lay;
    InputLayout(s, bw2 - 20.0f * s->scale, &lay);
    int over = (int)lay.size() - 4;
    return over > 0 ? over : 0;
}

static int HitTest(AiSess* s, float x, float y, int* idxOut) {
    *idxOut = -1;
    if (s->dlg) {   /* 对话框开着: 只认对话框 */
        if (s->dSaveR.Hit(x, y)) return HIT_DSAVE;
        if (s->dCancelR.Hit(x, y)) return HIT_DCANCEL;
        if (s->dChkR.Hit(x, y)) return HIT_DCHK;
        if (s->dUrlR.Hit(x, y)) return HIT_DURL;
        if (s->dKeyR.Hit(x, y)) return HIT_DKEY;
        if (s->dModelR.Hit(x, y)) return HIT_DMODEL;
        return HIT_NONE;
    }
    if (s->hClose.Hit(x, y)) return HIT_CLOSE;
    if (s->hSet.Hit(x, y)) return HIT_HSET;
    if (s->hHist.Hit(x, y)) return HIT_HHIST;
    if (s->hNew.Hit(x, y)) return HIT_HNEW;
    if (s->sideOpen) {
        bool inSide = s->sidePanel.Hit(x, y);
        if (!inSide && y >= AI_HEAD_H * s->scale)
            return HIT_NONE;   /* 浮层开着: 浮层外的内容区不透 hover/点击 (底下的发送/消息/输入框不亮) */
        if (inSide) {
            if (s->hClear.Hit(x, y)) return HIT_CLEAR;
            for (int i = 0; i < (int)s->hRows.size(); i++) {
                if (s->hRows[i].Hit(x, y)) {
                    if (i < (int)s->hRowDel.size() && s->hRowDel[i].Hit(x, y)) { *idxOut = i; return HIT_SBDEL + i; }
                    *idxOut = i;
                    return HIT_SBROW + i;   /* 行号进命中码 — hover 高亮按码+行号比对 (曾裸返回基码 = 悬停永远亮第 0 行) */
                }
            }
            return HIT_NONE;   /* 浮层内空白: 无命中, 也不落到浮层底下 */
        }
    }
    if (s->hSend.Hit(x, y)) return HIT_SEND;
    if (s->hThumb.w > 0 && s->hThumb.Hit(x, y)) return HIT_THUMB;
    if (s->hInThumb.w > 0 && s->hInThumb.Hit(x, y)) return HIT_INTHUMB;
    float bx, by, bw, bh;
    InputGeom(s, &bx, &by, &bw, &bh, NULL);
    if (x >= bx && x < bx + bw && y >= by && y < by + bh) return HIT_INPUT;
    if (s->hThumb.w > 0 && x >= s->hThumb.x - 4.0f * s->scale &&
        x < s->hThumb.x + s->hThumb.w + 4.0f * s->scale &&
        y >= s->thumbTrackY && y < s->thumbTrackY + s->thumbTrackH) return HIT_TRACK;
    /* 推理块头: 消息流区域内, 按气泡几何反推 (宽=气泡, 高=头部行 24*scale) */
    if (y >= AI_HEAD_H * s->scale) {
        for (int i = 0; i < (int)s->msgs.size(); i++) {
            const AiMsg& m = s->msgs[i];
            if (m.reason.empty() || (size_t)i >= s->layOffsets.size()) continue;
            float k = s->scale;
            float gy = AI_HEAD_H * k + s->layOffsets[i] - (float)s->scrollY;
            if (y < gy + 10.0f * k || y > gy + 10.0f * k + 24.0f * k) continue;
            float bubW = s->lay[i].bubbleW;
            float bubX = m.role == 0 ? (s->w - 12.0f * k - 24.0f * k - 8.0f * k - bubW)
                                     : (12.0f * k + 24.0f * k + 8.0f * k);
            if (x >= bubX && x <= bubX + bubW) { *idxOut = i; return HIT_REASON; }
        }
        /* 工具卡片头 (点击=展开/收起样本列表): 遍历 role==2 消息, 气泡内逐 step 定位头行 */
        {
            float k = s->scale;
            for (int i = 0; i < (int)s->msgs.size(); i++) {
                const AiMsg& m = s->msgs[i];
                if (m.role != 2 || (size_t)i >= s->layOffsets.size()) continue;
                const AiSess::MsgLayout& L = s->lay[i];
                if ((int)L.stepHs.size() != (int)m.steps.size()) continue;   /* 排版未同步 */
                float gy = AI_HEAD_H * k + s->layOffsets[i] - (float)s->scrollY;
                float bubX = 12.0f * k + 24.0f * k + 8.0f * k;   /* AI 气泡恒左侧 */
                if (x < bubX || x > bubX + L.bubbleW) continue;
                float curY = gy + 10.0f * k;
                for (int si = 0; si < (int)m.steps.size(); si++) {
                    if (y >= curY && y < curY + 22.0f * k) { *idxOut = i; return HIT_STEPHEAD + si; }
                    curY += L.stepHs[si] + 6.0f * k;
                }
            }
        }
    }
    return HIT_NONE;
}

/* ---- 会话级操作 ---- */

static void AbortSend(AiSess* s) {
    if (!s->job) return;
    AiJob* j = s->job;
    InterlockedExchange(&j->abort, 1);
    EnterCriticalSection(&j->cs);
    if (j->hReq) { WinHttpCloseHandle(j->hReq); j->hReq = NULL; }   /* 并发关句柄 = 打断阻塞读 */
    LeaveCriticalSection(&j->cs);
}

static void SessSaveConv(AiSess* s) {
    if (s->msgs.empty()) return;
    s->curId = HistUpsert(s->curId, s->msgs);
}

static void SessOpen(AiSess* s, XjsWindowToken tok, long long serial, int w, int h, float scale) {
    s->inUse = true;
    s->tok = tok;
    s->serial = serial;
    s->w = w;
    s->h = h;
    s->scale = scale > 0 ? scale : 1.0f;
    s->winActive = true;
    s->layDirty = true;
    static bool s_dataLoaded = false;   /* cfg/历史进程级一份, 多窗共享 */
    if (!s_dataLoaded) { s_dataLoaded = true; CfgLoad(); HistLoad(); }
    SessLoadSkin(s);
}

static void SessClose(AiSess* s) {
    AbortSend(s);
    SessSaveConv(s);   /* 未落库的当前对话保存 (关面板不丢) */
    s->inUse = false;
    if (s->job) { s_orphans.push_back(s->job); s->job = NULL; }   /* 流未完 → 孤儿 (泵清扫 join) */
    s->msgs.clear();
    s->curId = 0;
    s->input.clear();
    s->lay.clear();
    s->layOffsets.clear();
    s->px.clear();
    s->px.shrink_to_fit();
    s->bw = s->bh = 0;
    SessFreeFonts(s);
}

/* ---- 对话框 ---- */
/* 对话框键盘让渡 (面板接管宿主闸: plugPanelKey 不开 = 键盘根本不进插件 —
   开对话框即请求, 关闭后按输入框聚焦态归还) */
static void DlgKeyboard(AiSess* s, int want) {
    if (HOST_PANEL_OK && g_host) g_host->PanelSetFocus(g_ctx, s->tok, want);
}
static int DlgKeyboardAfter(AiSess* s) {
    return s->inputFocus ? 1 : 0;   /* 关对话框后: 输入框仍聚焦则键盘留给输入框 */
}
static void DlgOpen(AiSess* s) {
    s->dlg = true;
    s->dUrl = g_cfg.baseUrl;
    s->dKey = g_cfg.apiKey;
    s->dModel = g_cfg.model;
    s->dReason = g_cfg.reasoning;
    s->dFocus = 1;   /* 默认聚焦接口地址 (键盘已让渡, Tab 可切) */
    s->dCaret = s->dAnchor = s->dUrl.size();
    DlgKeyboard(s, 1);
}
static void DlgClose(AiSess* s) {
    s->dlg = false;
    DlgKeyboard(s, DlgKeyboardAfter(s));
}
static void DlgSave(AiSess* s) {
    std::wstring url = TrimW(s->dUrl);
    std::wstring model = TrimW(s->dModel);
    if (!url.empty()) g_cfg.baseUrl = url;
    if (!model.empty()) g_cfg.model = model;
    g_cfg.apiKey = TrimW(s->dKey);
    g_cfg.reasoning = s->dReason;
    CfgSave();
    DlgClose(s);
    RenderDeliver(s);
}

/* ---- 历史 ---- */
/* 命中/悬停值 = 视觉行号 (最新在上), g_hist 按时间序存储 (最新在尾) — 视觉行号 → 数据下标 */
static int HistRowToData(int row) { return (int)g_hist.size() - 1 - row; }
static void LoadConv(AiSess* s, int i) {
    if (i < 0 || i >= (int)g_hist.size()) return;
    AbortSend(s);
    SessSaveConv(s);   /* 当前未存对话先存 */
    s->msgs = g_hist[i].msgs;
    MsgSelClear(s);   /* 会话切换, 选区失效 (含键盘归还) */
    s->curId = g_hist[i].id;
    s->layDirty = true;
    s->sticky = true;
    s->sideOpen = false;
    s->clearArm = false;
}
static void DeleteConv(AiSess* s, int i) {
    if (i < 0 || i >= (int)g_hist.size()) return;
    unsigned long long id = g_hist[i].id;
    g_hist.erase(g_hist.begin() + i);
    HistSave();
    if (s->curId == id) { s->curId = 0; s->msgs.clear(); s->layDirty = true; }
}
static void ClearHist(AiSess* s) {
    g_hist.clear();
    HistSave();
    s->curId = 0;
    s->msgs.clear();
    s->layDirty = true;
    s->clearArm = false;
    s->sideOpen = false;
}
static void NewConv(AiSess* s) {
    AbortSend(s);
    SessSaveConv(s);
    s->msgs.clear();
    s->curId = 0;
    s->layDirty = true;
    s->sticky = true;
    s->sideOpen = false;
}

/* ---- 输入编辑 (选区/撤销; 对齐搜索框: 输入替换选区, Backspace/Delete 删选区, 单档撤销) ---- */
static bool InputSelRange(AiSess* s, size_t* a, size_t* b) {
    if (s->anchor == s->caret) return false;
    *a = s->anchor < s->caret ? s->anchor : s->caret;
    *b = s->anchor < s->caret ? s->caret : s->anchor;
    return true;
}
static void InputSnapUndo(AiSess* s) {
    if (s->undoSnap) return;   /* 同一次修改序列只快照一次 (Ctrl+Z 恢复后置 false, 再改再快照) */
    s->undoText = s->input;
    s->undoCaret = s->caret;
    s->undoAnchor = s->anchor;
    s->undoSnap = true;
}
static void InsertText(AiSess* s, const std::wstring& t) {
    InputSnapUndo(s);
    size_t a, b;
    if (InputSelRange(s, &a, &b)) {   /* 有选区: 输入替换选区 */
        s->input.erase(a, b - a);
        s->caret = s->anchor = a;
    }
    if (s->input.size() + t.size() > 8000) return;
    s->input.insert(s->caret, t);
    s->caret += t.size();
    s->anchor = s->caret;
}
static void InputEraseSel(AiSess* s) {
    size_t a, b;
    if (!InputSelRange(s, &a, &b)) return;
    InputSnapUndo(s);
    s->input.erase(a, b - a);
    s->caret = s->anchor = a;
}
static void InputSelectAll(AiSess* s) { s->anchor = 0; s->caret = s->input.size(); }
static void InputCopySel(AiSess* s, bool cut) {
    size_t a, b;
    if (!InputSelRange(s, &a, &b)) return;
    if (HOST_PANEL_OK && g_host) g_host->ClipboardSetText(g_ctx, U8(s->input.substr(a, b - a)).c_str());
    if (cut) InputEraseSel(s);
}
static void InputPaste(AiSess* s) {
    char buf[65536];
    int n = g_host ? g_host->ClipboardGetText(g_ctx, buf, (int)sizeof(buf) - 1) : 0;
    if (n <= 0) return;
    buf[n] = 0;
    std::wstring t = W8(buf);
    for (auto& c : t) if (c == L'\r') c = L'\n';   /* CRLF→LF (粘贴保留换行, 搜索框口径) */
    InsertText(s, t);
}
static void InputUndo(AiSess* s) {
    if (!s->undoSnap) return;
    std::wstring ti = s->input; size_t c1 = s->caret, a1 = s->anchor;
    s->input = s->undoText; s->caret = s->undoCaret; s->anchor = s->undoAnchor;
    s->undoText = ti; s->undoCaret = c1; s->undoAnchor = a1;   /* 交换式: 再按 Z 回到当前 */
    s->undoSnap = false;
    if (s->caret > s->input.size()) s->caret = s->input.size();
    if (s->anchor > s->input.size()) s->anchor = s->input.size();
}
/* 双击选词: 码点类型 (0空白 1CJK/全角 2拉丁) 两侧扩展; 代理对经 PrevCp/NextCp 整体步进 */
static void InputSelectWord(AiSess* s, size_t idx) {
    if (s->input.empty()) return;
    if (idx >= s->input.size()) idx = s->input.size() - 1;
    if ((s->input[idx] & 0xFC00) == 0xDC00 && idx > 0) idx--;   /* 低代理归前对 */
    auto kind = [](wchar_t c) { return (c == L' ' || c == L'\n') ? 0 : ((c & 0xFC00) == 0xD800 || c >= 0x2E80) ? 1 : 2; };
    int k0 = kind(s->input[idx]);
    size_t lo = idx, hi = NextCp(s->input, idx);
    while (lo > 0 && k0 != 0 && kind(s->input[PrevCp(s->input, lo)]) == k0) lo = PrevCp(s->input, lo);
    while (hi < s->input.size() && k0 != 0 && kind(s->input[hi]) == k0) hi = NextCp(s->input, hi);
    s->anchor = lo;
    s->caret = hi;
}
/* 右键编辑菜单命令 (0剪切 1复制 2粘贴 3全选 4删除; 禁用态由调用方判) */
static void InputMenuCmd(AiSess* s, int mi) {
    switch (mi) {
        case 0: InputCopySel(s, true); break;
        case 1: InputCopySel(s, false); break;
        case 2: InputPaste(s); break;
        case 3: InputSelectAll(s); break;
        case 4: InputEraseSel(s); break;
    }
}

/* IME 锚点 → 宿主 (输入框内光标点位; 渲染几何同源换算)。
   兼作"光标滚入可视窗口"汇点 — 凡改光标/内容的路径都经这里 (漏接的路径 = 滚动不跟光标) */
static void UpdateImeAnchor(AiSess* s) {
    if (!HOST_PANEL_OK || !g_host || !s->inUse) return;
    InEnsureCaret(s);
    float bx, by, bw, bh;
    InputGeom(s, &bx, &by, &bw, &bh, NULL);
    int line = 0;
    float cx = 0;
    InputCaretPos(s, bw - 10.0f * s->scale * 2, &line, &cx);
    g_host->PanelSetCaret(g_ctx, s->tok, (int)(bx + 10.0f * s->scale + cx),
                          (int)(by + 10.0f * s->scale + (line - s->inScroll) * AI_LINE_H * s->scale));
}

/* ---- 消息文本选区 (拖选复制; 单条消息内 atom 粒度) ---- */
/* 事件点 → 消息正文 atom 定位 (行尾 = 行 atoms 数; 推理块/头像区不参与; 工具卡片组无正文) */
static bool MsgHitAtom(AiSess* s, float x, float y, int* mOut, int* lOut, int* aOut) {
    float k = s->scale;
    float pad = 12.0f * k, av = 24.0f * k, gap = 8.0f * k, bp = 10.0f * k;
    float msgsTop = AI_HEAD_H * k;
    for (int i = 0; i < (int)s->msgs.size(); i++) {
        if ((size_t)i >= s->lay.size() || (size_t)i >= s->layOffsets.size()) break;
        if (s->msgs[i].role == 2) continue;   /* 工具卡片组: 无正文 atom, 不参与文本选区 */
        float gy = msgsTop + s->layOffsets[i] - (float)s->scrollY;
        if (y < gy || y > gy + s->lay[i].totalH) continue;
        const AiSess::MsgLayout& L = s->lay[i];
        const AiMsg& m = s->msgs[i];
        float bubbleX = m.role == 0 ? (s->w - pad - av - gap - L.bubbleW) : (pad + av + gap);
        float yCur = gy + bp;
        if (!m.reason.empty()) yCur += (m.reasonOpen ? L.reasonH + 4.0f * k : 24.0f * k);
        for (int li = 0; li < (int)L.lines.size(); li++) {
            const AiLine& ln = L.lines[li];
            bool last = li == (int)L.lines.size() - 1;
            if (ln.runs.empty()) { yCur += ln.h; continue; }   /* 垫行并入下一可见行 */
            if (y <= yCur + ln.h || last) {
                float rel = x - (bubbleX + bp);
                *mOut = i; *lOut = li;
                int ai = (int)ln.runs.size();   /* 默认行尾 */
                for (int a2 = 0; a2 < (int)ln.runs.size(); a2++) {
                    if (rel < ln.runs[a2].x + ln.runs[a2].w * 0.5f) { ai = a2; break; }   /* 过半归后 (点定位同口径) */
                }
                *aOut = ai;
                return true;
            }
            yCur += ln.h;
        }
    }
    return false;
}
/* 选区两端归一 (小端在前) */
static void MsgSelNorm(AiSess* s, int* aL, int* aA, int* bL, int* bA) {
    if (s->selALine > s->selBLine || (s->selALine == s->selBLine && s->selAAtom > s->selBAtom)) {
        *aL = s->selBLine; *aA = s->selBAtom; *bL = s->selALine; *bA = s->selAAtom;
    } else {
        *aL = s->selALine; *aA = s->selAAtom; *bL = s->selBLine; *bA = s->selBAtom;
    }
}
/* 清消息选区: 借用的键盘一并归还宿主 (选区没了, 键盘回搜索框/列表) */
static void MsgSelClear(AiSess* s) {
    s->txtSel = false;
    if (s->msgKeyBorrow && !s->inputFocus) {
        s->msgKeyBorrow = false;
        if (HOST_PANEL_OK && g_host) g_host->PanelSetFocus(g_ctx, s->tok, 0);
    }
}
/* 复制选区文本 (行界加换行; runs 即渲染文本 — markdown 标记已剥离) */
static void MsgCopySel(AiSess* s) {
    if (!s->txtSel || s->selMsg < 0 || (size_t)s->selMsg >= s->lay.size() || !g_host) return;
    int aL, aA, bL, bA;
    MsgSelNorm(s, &aL, &aA, &bL, &bA);
    const AiSess::MsgLayout& L = s->lay[s->selMsg];
    std::wstring out;
    for (int li = aL; li <= bL && li < (int)L.lines.size(); li++) {
        const AiLine& ln = L.lines[li];
        int lo = li == aL ? aA : 0;
        int hi = li == bL ? bA : (int)ln.runs.size();
        if (li > aL) {
            /* 只在逻辑行界插 \n (块首/代码行首/垫行); 软折行续行直接拼接,
               排版时被丢的行首空格在此补回 ("ORDER BY" 不裂成两行/不粘成 "ORDERBY") */
            if (ln.hard) out += L"\n";
            else if (ln.leadSpace) out += L" ";
        }
        for (int ai = lo; ai < hi && ai < (int)ln.runs.size(); ai++) out += ln.runs[ai].text;
    }
    if (!out.empty()) {
        int rc = g_host->ClipboardSetText(g_ctx, U8(out).c_str());
        if (rc == XJS_PLUGIN_OK) g_host->Toast(g_ctx, s->tok, "已复制", XJS_PLUGIN_TOAST_SUCCESS);
        else g_host->Toast(g_ctx, s->tok, "复制失败 (剪贴板写入被拒)", XJS_PLUGIN_TOAST_WARN);
    }
}

/* 历史侧栏 = 浮层: 点内容区浮层之外 (头部按钮除外) = 点外关闭, 点击消费不透传底层 —
   同宿主菜单"点外按下即关"口径; 返回真 = 已关闭, 调用方渲染后直接返回 */
static bool SideDismissOutside(AiSess* s, float x, float y) {
    if (!s->sideOpen) return false;
    if (y < AI_HEAD_H * s->scale || s->sidePanel.Hit(x, y)) return false;
    s->sideOpen = false;
    s->clearArm = false;
    return true;
}

/* 鼠标/滚轮/键盘事件 (OnPanelEvent 转发; 全 UI 线程) */
static void PanelMouse(AiSess* s, int type, float x, float y, unsigned flags) {
    bool shift = (flags & 2) != 0;   /* Shift+点击 = 扩展选区 (位定义同 PanelKey) */
    int idx = -1;
    int hit = x < 0 ? HIT_NONE : HitTest(s, x, y, &idx);
    switch (type) {
        case XJS_PANEL_MOUSE_MOVE: {
            if (s->pressGrab && s->hThumb.w > 0) {   /* 滚动条拖拽 */
                float trackH = s->thumbTrackH - s->hThumb.h;
                float maxScroll = s->contentH - s->thumbTrackH;
                if (trackH > 0 && maxScroll > 0) {
                    s->scrollY = maxScroll * (double)((y - s->thumbTrackY - s->pressGrab) / trackH);
                    if (s->scrollY < 0) s->scrollY = 0;
                    if (s->scrollY > maxScroll) s->scrollY = maxScroll;
                    /* 手动滚动态: 拖离底部解除吸底 (sticky=true 时渲染每帧强制回底, 拖动会被覆盖 = 纹丝不动),
                       拖回底部恢复跟随 */
                    s->sticky = (s->scrollY >= maxScroll);
                }
                RenderDeliver(s);
                return;
            }
            if (s->inGrab > 0 && s->hInThumb.w > 0 && s->inTrackH > 0) {   /* 输入框滚动条拖拽 */
                float bx, by, bw2, bh2;
                InputGeom(s, &bx, &by, &bw2, &bh2, NULL);
                std::vector<InpLine> lay;
                InputLayout(s, bw2 - 20.0f * s->scale, &lay);
                int over = (int)lay.size() - 4;
                if (over > 0) {
                    float t = (y - s->inTrackY - s->inGrab) / maxf(1.0f, s->inTrackH - s->hInThumb.h);
                    s->inScroll = (int)(over * t + 0.5f);
                    if (s->inScroll < 0) s->inScroll = 0;
                    if (s->inScroll > over) s->inScroll = over;
                }
                RenderDeliver(s);
                return;
            }
            if (s->dSelDragging) {   /* 对话框字段拖选 (anchor 固定, caret 随点走; 拖出字段=夹到 0/末尾) */
                if (s->dFocus >= 1 && s->dFocus <= 3) {
                    FRect* rc = s->dFocus == 1 ? &s->dUrlR : s->dFocus == 2 ? &s->dKeyR : &s->dModelR;
                    std::wstring disp = FieldDispOf(s, s->dFocus);
                    std::wstring vis;
                    size_t skip = FieldVisWindow(s, disp, rc->w - 16.0f * s->scale, &vis);
                    size_t i = FieldIndexFromX(s, vis, x - rc->x - 8.0f * s->scale) + skip;
                    if (i > disp.size()) i = disp.size();
                    s->dCaret = i;
                    DlgUpdateIme(s);
                }
                RenderDeliver(s);
                return;
            }
            if (s->selDragging) {   /* 输入框拖选 (anchor 固定, caret 随点走) */
                float bx, by, bw2, bh2;
                InputGeom(s, &bx, &by, &bw2, &bh2, NULL);
                std::vector<InpLine> lay;
                InputLayout(s, bw2 - 20.0f * s->scale, &lay);
                s->caret = InputIndexFromPoint(s, lay, bw2 - 20.0f * s->scale, x, y, bx, by);
                s->expectCol = -1;
                UpdateImeAnchor(s);
                RenderDeliver(s);
                return;
            }
            if (s->txtDragging) {   /* 消息文本拖选 (起点固定, 终点随点走) */
                int mI, lI, aI;
                if (MsgHitAtom(s, x, y, &mI, &lI, &aI) && mI == s->selMsg) {
                    s->selBLine = lI;
                    s->selBAtom = aI;
                }
                RenderDeliver(s);
                return;
            }
            if (hit != s->hover) { s->hover = hit; RenderDeliver(s); }
            return;
        }
        case XJS_PANEL_LDOWN: {
            if (s->menuOpen) {   /* 编辑菜单开着: 点项执行 (禁用态忽略), 点外关闭; 不穿透 */
                int cmd = -1;
                for (int i = 0; i < (int)s->menuRows.size(); i++)
                    if (s->menuRows[i].Hit(x, y)) { cmd = i; break; }
                s->menuOpen = false;
                if (cmd >= 0) {
                    bool sel = s->anchor != s->caret;
                    bool en[5] = { sel, sel, true, !s->input.empty(), sel };
                    if (en[cmd]) InputMenuCmd(s, cmd);
                    UpdateImeAnchor(s);   /* 粘贴/剪切/删除动过光标: 滚入可见区 + 锚点跟手 */
                }
                RenderDeliver(s);
                return;
            }
            if (SideDismissOutside(s, x, y)) { RenderDeliver(s); return; }
            s->press = hit;
            s->pressX = x;
            s->pressY = y;
            if (s->dlg) {
                /* 对话框开着: 点字段 = 聚焦 + 点定位 (Shift+点 = 扩展选区; 主输入框同款) */
                if (hit == HIT_DURL) s->dFocus = 1;
                else if (hit == HIT_DKEY) s->dFocus = 2;
                else if (hit == HIT_DMODEL) s->dFocus = 3;
                if (hit == HIT_DURL || hit == HIT_DKEY || hit == HIT_DMODEL) {
                    FRect* rc = hit == HIT_DURL ? &s->dUrlR : hit == HIT_DKEY ? &s->dKeyR : &s->dModelR;
                    std::wstring disp = FieldDispOf(s, s->dFocus);
                    if (s->dAnchor > disp.size()) s->dAnchor = disp.size();
                    std::wstring vis;
                    size_t skip = FieldVisWindow(s, disp, rc->w - 16.0f * s->scale, &vis);
                    size_t i = FieldIndexFromX(s, vis, x - rc->x - 8.0f * s->scale) + skip;
                    if (i > disp.size()) i = disp.size();
                    s->dCaret = i;
                    if (!shift) s->dAnchor = i;
                    s->dSelDragging = true;   /* 按住拖动 = 扩选区 (LUP 收) */
                    DlgUpdateIme(s);
                }
            } else if (hit == HIT_INPUT) {
                s->inputFocus = true;
                if (HOST_PANEL_OK && g_host) g_host->PanelSetFocus(g_ctx, s->tok, 1);
                {   /* 点定位 (普通点击 = 定位+折叠; Shift+点 = 从 anchor 扩展到点击处) */
                    float bx, by, bw2, bh2;
                    InputGeom(s, &bx, &by, &bw2, &bh2, NULL);
                    std::vector<InpLine> lay;
                    InputLayout(s, bw2 - 20.0f * s->scale, &lay);
                    size_t hitIdx = InputIndexFromPoint(s, lay, bw2 - 20.0f * s->scale, x, y, bx, by);
                    if (shift) s->caret = hitIdx;
                    else { s->caret = hitIdx; s->anchor = hitIdx; }
                }
                s->selDragging = true;
                s->expectCol = -1;
                UpdateImeAnchor(s);
            } else if (hit == HIT_THUMB) {
                s->pressGrab = y - s->hThumb.y;
                s->sticky = false;   /* 抓住滑块 = 手动滚动态 (否则渲染吸底覆盖, 拖不动) */
            } else if (hit == HIT_TRACK && s->hThumb.w > 0) {
                float maxScroll = s->contentH - s->thumbTrackH;
                if (maxScroll > 0) {
                    s->scrollY = maxScroll * (double)((y - s->thumbTrackY) / s->thumbTrackH);
                    s->sticky = (y >= s->thumbTrackY + s->thumbTrackH - s->hThumb.h);   /* 点轨道底部 = 回底恢复吸底 */
                }
                s->pressGrab = s->hThumb.h / 2;
            } else if (hit == HIT_INTHUMB) {   /* 输入框滚动条: 记抓点拖拽 */
                s->inGrab = y - s->hInThumb.y;
            } else if (s->dlg) {
                /* 对话框开着: 蒙层/卡片空白按下只清输入焦点, 底层消息不可拖选 (背景惰性) */
                s->inputFocus = false;
            } else {
                /* 消息流/空白: 命中消息正文 = 拖选起点 (向宿主借键盘, Ctrl+C 可复制); 未命中 = 清选区归还键盘 */
                bool wasInput = s->inputFocus;
                s->inputFocus = false;
                int mI = -1, lI = -1, aI = -1;
                if (MsgHitAtom(s, x, y, &mI, &lI, &aI)) {
                    s->txtSel = true;
                    s->selMsg = mI;
                    s->selALine = s->selBLine = lI;
                    s->selAAtom = s->selBAtom = aI;
                    s->txtDragging = true;
                    s->msgKeyBorrow = true;
                    if (HOST_PANEL_OK && g_host) g_host->PanelSetFocus(g_ctx, s->tok, 1);
                } else if (s->msgKeyBorrow) {
                    MsgSelClear(s);
                } else if (wasInput && HOST_PANEL_OK && g_host) {
                    g_host->PanelSetFocus(g_ctx, s->tok, 0);
                }
            }
            RenderDeliver(s);
            return;
        }
        case XJS_PANEL_LUP: {
            int press = s->press;
            float pX = s->pressX, pY = s->pressY;
            s->press = HIT_NONE;
            s->pressX = s->pressY = -1.0f;
            s->pressGrab = 0;
            s->inGrab = 0;
            s->dSelDragging = false;   /* 对话框字段拖选结束 (选区保留待复制) */
            s->selDragging = false;   /* 输入框拖选结束 (caret 已在 MOVE 跟点) */
            s->txtDragging = false;   /* 消息拖选结束 (选区保留待复制) */
            /* 对话框开着点蒙层空白 (按下与松开都在卡片外) = 取消 (原版 ai-chat 点遮罩取消口径);
               卡内空白 (标题/字段标签/按钮带空档) 不得关闭 */
            if (s->dlg && press == HIT_NONE && hit == HIT_NONE &&
                !s->dCardR.Hit(x, y) && !s->dCardR.Hit(pX, pY)) {
                DlgClose(s);
                RenderDeliver(s);
                return;
            }
            bool same = (hit == press) || (press == HIT_TRACK);
            if (!same) { RenderDeliver(s); return; }
            /* 侧栏行命中码带行号 (HIT_SBROW+i / HIT_SBDEL+i, hover 同码) — 范围判别, 不能进 switch */
            if (press >= HIT_SBROW && press < HIT_SBROW + (int)AI_CONV_MAX) { LoadConv(s, HistRowToData(idx)); }
            else if (press >= HIT_SBDEL && press < HIT_SBDEL + (int)AI_CONV_MAX) { DeleteConv(s, HistRowToData(idx)); }
            else switch (press) {
                case HIT_CLOSE:
                    /* 结束接管会话 (宿主按打开前状态恢复预览; CLOSE 事件随后回来收尾存库) */
                    if (HOST_PANEL_OK && g_host) g_host->PanelClose(g_ctx, s->tok);
                    break;
                case HIT_HSET: DlgOpen(s); break;
                case HIT_HHIST: s->sideOpen = !s->sideOpen; s->clearArm = false; s->sideScroll = 0; break;
                case HIT_HNEW: NewConv(s); break;
                case HIT_CLEAR:
                    if (s->clearArm) ClearHist(s);
                    else s->clearArm = true;
                    break;
                case HIT_REASON:
                    if (idx >= 0 && idx < (int)s->msgs.size()) {
                        s->msgs[idx].reasonOpen = !s->msgs[idx].reasonOpen;
                        MsgSelClear(s);   /* 折叠切换重排行结构, 旧选区失效 */
                        RelayoutOne(s, idx);
                    }
                    break;
                default:
                    if (press >= HIT_STEPHEAD && press < HIT_STEPHEAD + 8 &&
                        idx >= 0 && idx < (int)s->msgs.size() &&
                        s->msgs[idx].role == 2) {
                        int si = press - HIT_STEPHEAD;
                        if (si >= 0 && si < (int)s->msgs[idx].steps.size()) {
                            AiToolStep& st = s->msgs[idx].steps[si];
                            if (st.state >= 2 && !st.top.empty()) {   /* 完成且带样本才有展开 */
                                st.open = !st.open;
                                MsgSelClear(s);
                                RelayoutOne(s, idx);
                            }
                        }
                    }
                    break;
                case HIT_SEND:
                    if (s->sending) AbortSend(s);
                    else SendCurrent(s);
                    break;
                /* 对话框命令类控件: 松开触发 (同全 UI 口径) */
                case HIT_DSAVE: DlgSave(s); return;   /* DlgSave 内已渲染交付 */
                case HIT_DCANCEL: DlgClose(s); break;
                case HIT_DCHK: s->dReason = !s->dReason; break;
                case HIT_DURL: s->dFocus = 1; break;
                case HIT_DKEY: s->dFocus = 2; break;
                case HIT_DMODEL: s->dFocus = 3; break;
            }
            RenderDeliver(s);
            return;
        }
        case XJS_PANEL_DBLCLK: {   /* 双击选词 (搜索框口径: 编辑框内双击不穿透) */
            if (SideDismissOutside(s, x, y)) { RenderDeliver(s); return; }   /* 侧栏浮层外 = 关闭 */
            if (hit == HIT_INPUT) {
                s->inputFocus = true;
                if (HOST_PANEL_OK && g_host) g_host->PanelSetFocus(g_ctx, s->tok, 1);
                float bx, by, bw2, bh2;
                InputGeom(s, &bx, &by, &bw2, &bh2, NULL);
                std::vector<InpLine> lay;
                InputLayout(s, bw2 - 20.0f * s->scale, &lay);
                InputSelectWord(s, InputIndexFromPoint(s, lay, bw2 - 20.0f * s->scale, x, y, bx, by));
                UpdateImeAnchor(s);
                RenderDeliver(s);
            }
            return;
        }
        case XJS_PANEL_RUP: {   /* 输入框右键 = 编辑菜单; 消息选区右键 = 直接复制 */
            if (SideDismissOutside(s, x, y)) { RenderDeliver(s); return; }   /* 侧栏浮层外 = 关闭 */
            if (hit == HIT_INPUT) {
                s->inputFocus = true;
                if (HOST_PANEL_OK && g_host) g_host->PanelSetFocus(g_ctx, s->tok, 1);
                float k = s->scale;
                float mw = 124.0f * k, mh = 5 * 26.0f * k + 8.0f * k;
                float mx = x + 4.0f * k, my = y + 4.0f * k;
                if (mx + mw > s->w - 4.0f * k) mx = s->w - 4.0f * k - mw;
                if (my + mh > s->bh - 4.0f * k) my = s->bh - 4.0f * k - mh;
                if (mx < 4.0f * k) mx = 4.0f * k;
                if (my < 4.0f * k) my = 4.0f * k;
                s->menuBox = FRect{ mx, my, mw, mh };
                s->menuRows.clear();
                s->menuOpen = true;
                RenderDeliver(s);
            } else if (s->txtSel) {   /* 消息选区右键 = 复制 */
                MsgCopySel(s);
                RenderDeliver(s);
            }
            return;
        }
        default: return;   /* RDOWN 预留 */
    }
}

static void PanelWheel(AiSess* s, float x, float y, int delta) {
    float k = s->scale;
    if (s->sideOpen && x >= s->w - minf(320.0f * k, s->w * 0.62f)) {
        s->sideScroll -= delta / 120.0 * 3 * 15.0f * k;
        float rows = (float)g_hist.size() * 50.0f * k - (s->bh - AI_HEAD_H * k - 40.0f * k);
        if (s->sideScroll < 0) s->sideScroll = 0;
        if (s->sideScroll > rows) s->sideScroll = rows;
    } else if (s->hInput.Hit(x, y)) {   /* 悬停输入框: 滚溢出内容 (一格 2 行; 未溢出不动) */
        int over = InOverflowLines(s);
        if (over > 0) {
            s->inScroll += delta < 0 ? 2 : -2;
            if (s->inScroll < 0) s->inScroll = 0;
            if (s->inScroll > over) s->inScroll = over;
        }
    } else {
        s->scrollY -= delta / 120.0 * 3 * (double)AI_LINE_H * k;
        if (s->scrollY < 0) s->scrollY = 0;
        /* 向上滚 (delta>0, 看历史) = 解除吸底 — 曾写反 (向上滚置 sticky=true), sticky=true 时
           渲染每帧强制回底, 滚动被整个吞掉 = "滚轮没反应"; 向下滚不改 sticky (滚到底后新消息
           跟随由发送/流式路径恢复) */
        if (delta > 0) s->sticky = false;
    }
    RenderDeliver(s);
}

static void PanelKey(AiSess* s, unsigned vk, unsigned flags) {
    bool ctrl = (flags & 1) != 0, shift = (flags & 2) != 0;
    if (vk == VK_PROCESSKEY) return;   /* IME 组合中的键 (宿主原样转发) */
    if (s->dlg) {
        /* 对话框字段编辑 (单行全模型: 点定位/框选/全选/剪贴板; 主输入框同款语义) */
        std::wstring* f = s->dFocus == 1 ? &s->dUrl : s->dFocus == 2 ? &s->dKey : s->dFocus == 3 ? &s->dModel : NULL;
        if (vk == VK_TAB) {
            s->dFocus = s->dFocus == 3 ? 1 : s->dFocus + 1;
            std::wstring* nf = s->dFocus == 1 ? &s->dUrl : s->dFocus == 2 ? &s->dKey : &s->dModel;
            s->dCaret = s->dAnchor = nf->size();
        } else if (vk == VK_ESCAPE) {
            DlgClose(s);
            RenderDeliver(s);
            return;
        } else if (vk == VK_RETURN) {
            DlgSave(s);
        } else if (f) {
            size_t sz = f->size();
            if (s->dCaret > sz) s->dCaret = sz;
            if (s->dAnchor > sz) s->dAnchor = sz;
            bool sel = s->dAnchor != s->dCaret;
            size_t lo = s->dAnchor < s->dCaret ? s->dAnchor : s->dCaret;
            size_t hi = s->dAnchor < s->dCaret ? s->dCaret : s->dAnchor;
            switch (vk) {
                case VK_LEFT:
                    if (shift) s->dCaret = s->dCaret ? PrevCp(*f, s->dCaret) : 0;
                    else { s->dCaret = sel ? lo : (s->dCaret ? PrevCp(*f, s->dCaret) : 0); s->dAnchor = s->dCaret; }
                    break;
                case VK_RIGHT:
                    if (shift) s->dCaret = s->dCaret < sz ? NextCp(*f, s->dCaret) : sz;
                    else { s->dCaret = sel ? hi : (s->dCaret < sz ? NextCp(*f, s->dCaret) : sz); s->dAnchor = s->dCaret; }
                    break;
                case VK_HOME: s->dCaret = 0; if (!shift) s->dAnchor = 0; break;
                case VK_END: s->dCaret = sz; if (!shift) s->dAnchor = sz; break;
                case VK_BACK:
                    if (sel) { f->erase(lo, hi - lo); s->dCaret = s->dAnchor = lo; }
                    else if (s->dCaret) { size_t p = PrevCp(*f, s->dCaret); f->erase(p, s->dCaret - p); s->dCaret = s->dAnchor = p; }
                    break;
                case VK_DELETE:
                    if (sel) { f->erase(lo, hi - lo); s->dCaret = s->dAnchor = lo; }
                    else if (s->dCaret < sz) { size_t n2 = NextCp(*f, s->dCaret); f->erase(s->dCaret, n2 - s->dCaret); s->dAnchor = s->dCaret; }
                    break;
                case 'A':
                    if (ctrl) { s->dAnchor = 0; s->dCaret = sz; }
                    break;
                case 'C':
                    if (ctrl && sel && HOST_PANEL_OK && g_host)
                        g_host->ClipboardSetText(g_ctx, U8(f->substr(lo, hi - lo)).c_str());
                    break;
                case 'X':
                    if (ctrl && sel) {
                        if (HOST_PANEL_OK && g_host) g_host->ClipboardSetText(g_ctx, U8(f->substr(lo, hi - lo)).c_str());
                        f->erase(lo, hi - lo);
                        s->dCaret = s->dAnchor = lo;
                    }
                    break;
                case 'V':
                    if (ctrl) {
                        char buf[8192];
                        int n = (HOST_PANEL_OK && g_host) ? g_host->ClipboardGetText(g_ctx, buf, (int)sizeof(buf) - 1) : 0;
                        if (n > 0) {
                            buf[n] = 0;
                            std::wstring clean;
                            for (wchar_t c : W8(buf)) if (c != L'\r' && c != L'\n') clean += c;   /* 单行字段: 剔除换行 */
                            if (f->size() + clean.size() > 2048) clean.erase(2048 > f->size() ? 2048 - f->size() : 0);
                            if (sel) { f->erase(lo, hi - lo); s->dCaret = s->dAnchor = lo; }
                            f->insert(s->dCaret, clean);
                            s->dCaret += clean.size();
                            s->dAnchor = s->dCaret;
                        }
                    }
                    break;
            }
            DlgUpdateIme(s);
        }
        RenderDeliver(s);
        return;
    }
    if (!s->inputFocus) {
        /* 拖选借用键盘期: Ctrl+C = 复制选区; Esc = 清选区归还; 其它键 = 归还键盘后丢弃
           (打字意图 → 键盘回宿主搜索框, 用户重按一次即正常) */
        if (s->msgKeyBorrow) {
            if (vk == 'C' && ctrl) { MsgCopySel(s); RenderDeliver(s); return; }
            s->msgKeyBorrow = false;
            s->txtSel = false;
            if (HOST_PANEL_OK && g_host) g_host->PanelSetFocus(g_ctx, s->tok, 0);
            if (vk != VK_ESCAPE) return;
        }
        if (vk == VK_ESCAPE && s->sideOpen) { s->sideOpen = false; s->clearArm = false; RenderDeliver(s); }
        else if (vk == 'C' && ctrl && s->txtSel) { MsgCopySel(s); RenderDeliver(s); }
        return;
    }
    if (s->menuOpen) {   /* 编辑菜单开着: Esc 关闭, 其余先关菜单再处理 */
        s->menuOpen = false;
        if (vk == VK_ESCAPE) { RenderDeliver(s); return; }
    }
    /* 可视行导航/行首尾共用的布局快照 (按需) */
    std::vector<InpLine> lay;
    auto needLay = [&]() { if (lay.empty()) InputLayout(s, s->hInput.w - 20.0f * s->scale, &lay); };
    switch (vk) {
        case VK_LEFT:
            if (shift) { s->caret = PrevCp(s->input, s->caret); }
            else { size_t a, b; if (InputSelRange(s, &a, &b)) s->caret = a; else s->caret = PrevCp(s->input, s->caret); s->anchor = s->caret; }
            s->expectCol = -1; UpdateImeAnchor(s); break;
        case VK_RIGHT:
            if (shift) { s->caret = NextCp(s->input, s->caret); }
            else { size_t a, b; if (InputSelRange(s, &a, &b)) s->caret = b; else s->caret = NextCp(s->input, s->caret); s->anchor = s->caret; }
            s->expectCol = -1; UpdateImeAnchor(s); break;
        case VK_UP: case VK_DOWN: {   /* 多行可视行导航 (保持期望列) */
            needLay();
            float boxInnerW = s->hInput.w - 20.0f * s->scale;
            int line; float x;
            InputPosOf(s, lay, boxInnerW, s->caret, &line, &x);
            if (s->expectCol >= 0) x = s->expectCol;
            int tgt = vk == VK_UP ? line - 1 : line + 1;
            if (tgt < 0 || tgt > (int)lay.size() - 1) break;   /* 已在首/末行 */
            const InpLine& ln = lay[tgt];   /* 目标行内按 x 定位 */
            size_t idx = ln.start;
            bool hit = false;
            for (size_t i = 0; i < ln.atoms.size(); i++) {
                const AiAtom& a = ln.atoms[i];
                if (x < ln.cum[i] + a.w) {
                    if (a.t.size() > 1) idx += (size_t)((x - ln.cum[i]) / a.w * (float)a.t.size() + 0.5f);
                    else if (x >= ln.cum[i] + a.w * 0.5f) idx += a.t.size();
                    hit = true;
                    break;
                }
                idx += a.t.size();
            }
            if (!hit) idx = ln.start + ln.len();
            s->caret = idx;
            s->expectCol = x;
            if (!shift) s->anchor = s->caret;
            UpdateImeAnchor(s);
            break;
        }
        case VK_HOME: case VK_END: {   /* 可视行首/尾 (折行后) */
            needLay();
            float boxInnerW = s->hInput.w - 20.0f * s->scale;
            int line; float x;
            InputPosOf(s, lay, boxInnerW, s->caret, &line, &x);
            s->caret = vk == VK_HOME ? lay[line].start : lay[line].start + lay[line].len();
            if (!shift) s->anchor = s->caret;
            s->expectCol = -1; UpdateImeAnchor(s); break;
        }
        case VK_BACK:
            InputSnapUndo(s);
            if (s->anchor != s->caret) { InputEraseSel(s); }
            else if (s->caret > 0) {
                size_t p = PrevCp(s->input, s->caret);
                s->input.erase(p, s->caret - p);
                s->caret = s->anchor = p;
            }
            UpdateImeAnchor(s); break;
        case VK_DELETE:
            InputSnapUndo(s);
            if (s->anchor != s->caret) { InputEraseSel(s); }
            else if (s->caret < s->input.size()) {
                size_t n2 = NextCp(s->input, s->caret);
                s->input.erase(s->caret, n2 - s->caret);
            }
            UpdateImeAnchor(s); break;
        case VK_ESCAPE:   /* 搜索框口径: 选区→折叠, 再清空, 空则失焦 */
            if (s->anchor != s->caret) s->anchor = s->caret;
            else if (!s->input.empty()) { InputSnapUndo(s); s->input.clear(); s->caret = s->anchor = 0; }
            else s->inputFocus = false;
            UpdateImeAnchor(s); break;
        case VK_RETURN:
            if (shift) { InsertText(s, L"\n"); UpdateImeAnchor(s); }
            else { SendCurrent(s); }
            break;
        case 'A': if (ctrl) InputSelectAll(s); break;
        case 'C': if (ctrl) InputCopySel(s, false); break;
        case 'X': if (ctrl) InputCopySel(s, true); break;
        case 'Z': if (ctrl) { InputUndo(s); UpdateImeAnchor(s); } break;
        case 'V':
            if (ctrl) { InputPaste(s); UpdateImeAnchor(s); }
            break;
        default: break;
    }
    RenderDeliver(s);
}

static void PanelChar(AiSess* s, unsigned int ch) {
    if (s->dlg) {
        if (ch >= 0x20 && s->dFocus >= 1 && s->dFocus <= 3) {
            std::wstring* f = s->dFocus == 1 ? &s->dUrl : s->dFocus == 2 ? &s->dKey : &s->dModel;
            if (s->dCaret > f->size()) s->dCaret = f->size();
            if (s->dAnchor > f->size()) s->dAnchor = f->size();
            if (s->dAnchor != s->dCaret) {   /* 输入替换选区 */
                size_t lo = s->dAnchor < s->dCaret ? s->dAnchor : s->dCaret;
                size_t hi = s->dAnchor < s->dCaret ? s->dCaret : s->dAnchor;
                f->erase(lo, hi - lo);
                s->dCaret = s->dAnchor = lo;
            }
            if (f->size() < 2048) { f->insert(s->dCaret, 1, (wchar_t)ch); s->dCaret++; s->dAnchor = s->dCaret; }
            DlgUpdateIme(s);
        }
        RenderDeliver(s);
        return;
    }
    if (!s->inputFocus) return;
    if (ch == L'\r' || ch == L'\t' || ch == 0x08) return;   /* Enter/Tab/退格 走 KEY_DOWN */
    if (ch >= 0x20 || ch == L'\n') {
        InsertText(s, std::wstring(1, (wchar_t)ch));
        UpdateImeAnchor(s);
        RenderDeliver(s);
    }
}

/* ==================== 发送 + WinHTTP 流式 (工作线程只攒增量, 泵回 UI 渲染) ==================== */

/* 系统提示词: 产品能力内建 (搜索模式/语法/API 摘要取自 搜索模式简介.md 提炼) —
   助手要能直接给出可粘贴的搜索式, 而不是泛泛的通用回答 */
/* 角色/产品说明 (agent 循环里的"我有什么工具、怎么用"部分; Lua 脚本规范由引擎内嵌提示词提供) */
static const wchar_t* AI_INSTRUCTIONS =
    L"你是“蜗牛快搜”内置的 AI 助手(agent 模式)。蜗牛快搜是 Windows 本地文件极速搜索工具：全盘秒级索引，"
    L"支持文件名/大小/时间/类型/别名/内容等搜索。请用简体中文回答，简洁、准确、直接。\n"
    L"\n"
    L"## 工作方式\n"
    L"你有工具可用：涉及找文件/查文件/统计文件的任务，**先调用 run_search 工具实际执行搜索**，"
    L"根据返回的条数与路径样本判断结果是否符合，不符合就换口径再搜（逐步逼近：先粗筛再精筛），"
    L"不要凭空编造文件路径；需要给用户看文件时用 open_file，需要给出路径清单时用 copy_paths。"
    L"得到足够信息后，用最终答复总结：找到什么、在哪、关键数据；并给出可粘贴到搜索框的搜索式（注明模式）。\n"
    L"与搜索无关的问题直接回答，不要调用工具。\n"
    L"\n"
    L"## 搜索模式选择（run_search 的 mode 参数）\n"
    L"- wildcard 通配符（日常默认）：* 任意长度、? 单个字符；不含 * ? 时自动按包含匹配；支持拼音首拼/全拼（wd 命中 文档.docx）；"
    L"空格=且，|=或；搜索词含 \\\\ 或 / 时按完整路径匹配。\n"
    L"- regex 正则（PCRE2）：如 ^[0-9]{4}-报告.*\\.docx$。\n"
    L"- sql（功能最强）：SELECT Path FROM alltable WHERE Size > '100M' AND ModTime > NOW() - INTERVAL '7 days' ORDER BY Size DESC LIMIT 100；"
    L"支持 LIKE/ILIKE/~/GROUP BY/COUNT/CASE WHEN/CTE 等；尺寸简写 '100M'；常用字段：ID、Path、FName、Ext、Size、CreateTime、ModTime、"
    L"AccessTime、FileType、IsDir、Alias、Score、FileContent（不区分大小写）；不支持窗口函数(OVER)/多表 FROM/EXISTS/DDL。\n"
    L"- lua_filter 过滤模式：对每个文件做一次真值判断的 Lua 脚本（多线程快、增量同步）；脚本规范见下方《Lua 过滤模式脚本规范》。\n"
    L"- lua_exec 执行模式：脚本全权遍历数据库/跨文件聚合/自定义排序，return 的 ID 数组即结果；脚本规范见下方《Lua 执行模式脚本规范》。\n"
    L"选择建议：找名字用 wildcard/regex；按字段组合筛选/统计用 sql；逐文件自定义判断用 lua_filter；"
    L"跨文件聚合/自定义排序/树形下钻用 lua_exec。\n"
    L"\n"
    L"以下两份规范来自搜索引擎内置提示词，写 lua_filter / lua_exec 脚本时必须严格遵守其中的 API 全集与硬性规则"
    L"（脚本沙箱删除了 io/os 等库，API 以规范所列为准，绝不虚构函数）：\n"
    L"\n"
    L"════════════ 《Lua 过滤模式脚本规范》(mode=lua_filter) ════════════\n";

static const wchar_t* AI_INSTRUCTIONS_TAIL =
    L"\n════════════ 《Lua 执行模式脚本规范》(mode=lua_exec) ════════════\n";

/* 工具定义 (Responses API tools 数组; 与 AgentToolExec 的名字/参数一一对应) */
static const char* AI_TOOLS_JSON = R"json([
  {"type":"function","name":"run_search","description":"在蜗牛快搜索引中执行一次搜索, 返回命中总数与前 20 条路径样本。可多次调用逐步逼近目标 (先粗筛再精筛)。5 种 mode 的搜索词语法以系统提示词中的说明与两份 Lua 规范为准。","parameters":{"type":"object","properties":{"mode":{"type":"string","enum":["wildcard","regex","sql","lua_filter","lua_exec"],"description":"wildcard=通配符 regex=PCRE2正则 sql=SELECT语句 lua_filter=过滤模式Lua脚本 lua_exec=执行模式Lua脚本"},"query":{"type":"string","description":"搜索词/脚本全文 (lua 两种模式传完整脚本文本)"}},"required":["mode","query"]}},
  {"type":"function","name":"open_file","description":"打开最近一次 run_search 样本列表中的某个文件 (在用户屏幕上打开/定位), 用于让用户直接看到该文件。","parameters":{"type":"object","properties":{"index":{"type":"integer","description":"样本列表序号 (1 起)"},"reveal":{"type":"boolean","description":"true=只在资源管理器中定位, 不打开"}},"required":["index"]}},
  {"type":"function","name":"copy_paths","description":"把最近一次 run_search 的前 100 条完整路径 (每行一条) 复制到剪贴板, 供用户粘贴。","parameters":{"type":"object","properties":{},"required":[]}}
])json";

/* 系统提示词组装: 角色说明 + 引擎内嵌两份 Lua 规范 (进程级构建一次) */
static std::string g_instrA;
static void BuildInstructions() {
    std::wstring s = AI_INSTRUCTIONS;
    const char* p0 = xjs_LUA_GetPprompt(0);   /* 过滤模式 (-3) */
    if (p0 && *p0) s += W8(p0);
    s += AI_INSTRUCTIONS_TAIL;
    const char* p1 = xjs_LUA_GetPprompt(1);   /* 执行模式 (-4) */
    if (p1 && *p1) s += W8(p1);
    g_instrA = U8(s);
}

static void WorkerMain(AiJob* j);   /* 前置 (线程入口, 实现在下方) */

static void SendCurrent(AiSess* s) {
    if (s->sending || !g_host) return;
    std::wstring text = TrimW(s->input);
    if (text.empty()) return;
    if (g_cfg.apiKey.empty()) {
        g_host->Toast(g_ctx, s->tok, "尚未配置接口密钥 — 请点右上角 接口设置 填写", XJS_PLUGIN_TOAST_WARN);
        return;
    }
    AiMsg um;
    um.role = 0;
    um.text = text;
    s->msgs.push_back(um);
    s->input.clear();
    s->caret = 0;
    s->anchor = 0;
    s->inScroll = 0;
    s->layDirty = true;
    s->sticky = true;
    MsgSelClear(s);   /* 新消息入列重排, 旧选区失效 (含键盘归还) */
    /* 请求要素快照 (线程只读这些; 请求体每轮在 worker 构建 — input 随工具往返增长) */
    AiJob* j = new AiJob();
    j->keyA = U8(g_cfg.apiKey);
    /* baseUrl → host/port/path (https 默认) */
    std::wstring base = TrimW(g_cfg.baseUrl);
    while (!base.empty() && base.back() == L'/') base.pop_back();
    bool secure = true;
    if (base.rfind(L"https://", 0) == 0) { secure = true; base = base.substr(8); }
    else if (base.rfind(L"http://", 0) == 0) { secure = false; base = base.substr(7); }
    size_t slash = base.find(L'/');
    std::wstring hostpart = slash == std::wstring::npos ? base : base.substr(0, slash);
    std::wstring path = slash == std::wstring::npos ? L"" : base.substr(slash);
    j->secure = secure;
    j->port = secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    {
        std::wstring host = hostpart;
        size_t colon = host.rfind(L':');
        if (colon != std::wstring::npos) {
            int p = _wtoi(host.substr(colon + 1).c_str());
            if (p > 0) j->port = (INTERNET_PORT)p;
            host = host.substr(0, colon);
        }
        j->hostA = U8(host);
    }
    j->pathA = U8(path + L"/responses");
    j->tok = s->tok;   /* open_file 走宿主 OpenFile 的目标窗口 */
    /* 对话快照 (只含 role 0/1 文本消息; 工具往返由 worker 在循环中累计) */
    for (auto& m : s->msgs)
        if (m.role != 2 && !m.text.empty()) j->hist.push_back(m);
    if (j->hist.size() > 30) j->hist.erase(j->hist.begin(), j->hist.end() - 30);
    while (!j->hist.empty() && j->hist.front().role != 0) j->hist.erase(j->hist.begin());
    s->job = j;
    s->sending = true;
    s->netStatus = g_cfg.apiKey.empty() ? 0 : s->netStatus;
    j->th = new std::thread([j]() { WorkerMain(j); });
    RenderDeliver(s);
}

/* 一轮 SSE 事件里攒下的 function_call (模型要求执行的工具) */
struct AiCall {
    std::string callId, name, args;
};

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
    body += W8(JsonEscapeUtf8(W8(g_instrA.c_str())).c_str());
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
            std::string detail = msg.empty() ? (U8(W8(eb)).size() > 200 ? "" : U8(W8(eb))) : U8(msg);
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
                        AiCall c;
                        c.name = U8(item->S(L"name"));
                        c.callId = U8(item->S(L"call_id"));
                        const Jv* oi = ev.Get(L"output_index");
                        int outIdx = (oi && oi->t == 2) ? (int)oi->num : (int)turnCalls->size();
                        if (outIdx < 0) outIdx = (int)turnCalls->size();
                        while ((int)turnCalls->size() <= outIdx) turnCalls->push_back(AiCall());
                        (*turnCalls)[outIdx] = c;
                    }
                } else if (type == L"response.function_call_arguments.delta" ||
                           type == L"response.function_call_arguments.done") {
                    const Jv* oi = ev.Get(L"output_index");
                    size_t p = (oi && oi->t == 2 && oi->num >= 0 && oi->num < (double)turnCalls->size())
                                   ? (size_t)oi->num : (turnCalls->empty() ? 0 : turnCalls->size() - 1);
                    if (!turnCalls->empty()) {
                        const Jv* d = ev.Get(L"delta");
                        const Jv* a = ev.Get(L"arguments");
                        if (type == L"response.function_call_arguments.done")
                            (*turnCalls)[p].args = a && a->t == 3 ? U8(a->str) : (*turnCalls)[p].args;
                        else if (d)
                            (*turnCalls)[p].args += (d->t == 3 ? U8(d->str) : std::string());
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

static void WorkerMain(AiJob* j) {   /* agent 循环: SSE → 工具执行 → 结果回填 → 下一轮, 直到最终答复 */
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
            bool okTurn = AgentRunTurn(j, hc, accCalls, accOuts, !lastTurn, &turnCalls,
                                       &aborted, &truncated, &failed, &errMsg);
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
                std::wstring err = AgentToolExec(c.name, c.args, j->tok, &local);
                local.state = err.empty() ? 2 : 3;
                local.err = err;
                std::string output = AgentToolOutput(err, local);
                EnterCriticalSection(&j->cs);
                if (sidx >= 0 && sidx < (int)j->steps.size()) {
                    AiToolStep& dst = j->steps[sidx];
                    dst.kind = local.kind;
                    dst.state = local.state;
                    dst.mode = local.mode;
                    dst.query = local.query;
                    dst.count = local.count;
                    dst.elapsedMs = local.elapsedMs;
                    dst.err = local.err;
                    dst.top = local.top;   /* open 展开态归泵/用户, 不覆盖 */
                    j->stepsVersion++;
                }
                LeaveCriticalSection(&j->cs);
                accCalls.push_back(c);
                accOuts.push_back(output);
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

/* UI 泵: 抽增量 / 同步工具卡片 / 收尾 (join) / 孤儿清扫 */
static void PumpStreams() {
    for (auto& s : g_sess) {
        AiJob* j = s.job;
        if (!s.inUse || !j) continue;
        std::wstring out, reason;
        int state = 0, phase = 0, stepsVer = 0;
        std::wstring err;
        bool truncated = false;
        std::vector<AiToolStep> steps;
        EnterCriticalSection(&j->cs);
        out = j->out;
        reason = j->reason;
        state = j->state;
        phase = j->phase;
        err = j->err;
        truncated = j->truncated;
        stepsVer = j->stepsVersion;
        if (stepsVer != s.lastStepsVer) steps = j->steps;   /* 有变化才拷 (少一次全量复制) */
        LeaveCriticalSection(&j->cs);
        if (state == 0) {
            /* 工具卡片同步: steps 镜像 → msgs 的 role==2 消息 (追加只增; 内容按版本对齐; 保留 open) */
            if (!steps.empty()) {
                int have = 0;
                for (auto& m : s.msgs) if (m.role == 2) have++;
                while (have < (int)steps.size()) {
                    AiMsg cm;
                    cm.role = 2;
                    s.msgs.push_back(cm);
                    have++;
                    s.layDirty = true;
                }
                int seen = 0;
                for (auto& m : s.msgs) {
                    if (m.role != 2 || seen >= (int)steps.size()) continue;
                    bool changed = (int)m.steps.size() != 1 ||
                                   m.steps[0].state != steps[seen].state ||
                                   m.steps[0].count != steps[seen].count ||
                                   m.steps[0].err != steps[seen].err ||
                                   m.steps[0].top != steps[seen].top ||
                                   m.steps[0].name != steps[seen].name ||
                                   m.steps[0].mode != steps[seen].mode ||
                                   m.steps[0].query != steps[seen].query;
                    if (changed) {
                        bool wasOpen = !m.steps.empty() && m.steps[0].open;
                        steps[seen].open = wasOpen;
                        m.steps.assign(1, steps[seen]);
                        RelayoutOne(&s, (int)(&m - &s.msgs[0]));
                    }
                    seen++;
                }
                s.lastStepsVer = stepsVer;
            }
            /* 工具执行期: 冻结文本气泡; 若尾部挂着空文本气泡 (本轮无文字输出) 摘掉 */
            if (phase == 1) {
                if (!s.msgs.empty() && s.msgs.back().role == 1 &&
                    s.msgs.back().text.empty() && s.msgs.back().reason.empty()) {
                    s.msgs.pop_back();
                    s.layDirty = true;
                }
            } else if (!out.empty() || !reason.empty() || s.msgs.empty() || s.msgs.back().role != 1) {
                if (s.msgs.empty() || s.msgs.back().role != 1) {
                    AiMsg am;
                    am.role = 1;
                    am.reasonOpen = true;
                    s.msgs.push_back(am);
                    s.layDirty = true;
                }
                AiMsg& back = s.msgs.back();
                if (back.text != out || back.reason != reason) {
                    bool reasonGrew = reason.size() > back.reason.size();
                    back.text = out;
                    back.reason = reason;
                    if (reasonGrew) back.reasonOpen = true;
                    RelayoutOne(&s, (int)s.msgs.size() - 1);
                }
                if (back.reason.empty() && back.reasonOpen) back.reasonOpen = false;
            }
        } else {
            /* 收尾: 状态落消息 + 中止的执行中卡片落败 + 落库 (role==2 不入库) + join + 清作业 */
            for (auto& m : s.msgs)
                for (auto& st : m.steps)
                    if (st.state == 0 || st.state == 1) { st.state = 3; st.err = L"已中止"; }
            if (s.msgs.empty() || s.msgs.back().role != 1) {
                AiMsg am;
                am.role = 1;
                s.msgs.push_back(am);
            }
            AiMsg& back = s.msgs.back();
            back.text = out;
            back.reason = reason;
            back.reasonOpen = !reason.empty();   /* 收尾默认展开推理 (有内容才展开; 用户可点收) */
            if (state == 3) {
                if (back.text.empty() && back.reason.empty()) {
                    bool anyStep = false;
                    for (auto& m : s.msgs) if (m.role == 2) anyStep = true;
                    if (anyStep) s.msgs.pop_back();   /* 只有工具没文字的中止: 不留空气泡 */
                }
                if (!back.text.empty()) back.text += L"\n\n*(已停止生成)*";
                else back.text = L"已停止生成";
                if (!err.empty()) s.netStatus = 2;
            } else if (state == 2) {
                s.netStatus = 2;
                std::wstring et = err;
                back.text = back.text.empty() ? (L"请求失败: " + et) : (back.text + L"\n\n请求失败: " + et);
            } else {
                s.netStatus = 1;
                if (truncated) back.text += L"\n\n*(回答已截断)*";
            }
            s.sending = false;
            s.layDirty = true;
            MsgSelClear(&s);   /* 收尾重排, 选区失效 (含键盘归还) */
            s.curId = HistUpsert(s.curId, s.msgs);
            s.job = NULL;
            s.lastStepsVer = -1;
            if (j->th) {
                j->th->join();   /* state 已置 = 线程将尽, join 只等收尾微秒 */
                delete j->th;
                j->th = NULL;
            }
            delete j;
        }
        RenderDeliver(&s);
    }
    /* 孤儿清扫 (会话已关而流未完) */
    for (size_t i = 0; i < s_orphans.size();) {
        AiJob* j = s_orphans[i];
        bool done = false;
        EnterCriticalSection(&j->cs);
        done = j->state != 0;
        LeaveCriticalSection(&j->cs);
        if (done) {
            if (j->th) { j->th->join(); delete j->th; }
            delete j;
            s_orphans.erase(s_orphans.begin() + i);
        } else {
            i++;
        }
    }
}

/* ==================== 消息窗口 (流式泵 + 光标闪烁节拍) ==================== */

static LRESULT CALLBACK AiMsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case XJS_AI_STREAM:
            PumpStreams();
            return 0;
        case WM_TIMER:   /* 光标闪烁: 输入框聚焦的会话重渲染 (530ms 相位在渲染里取 GetTickCount64) */
            if (wParam == 1)
                for (auto& s : g_sess)
                    if (s.inUse && (s.inputFocus || s.dlg || s.sending)) RenderDeliver(&s);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

static bool AiMsgWndCreate() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = AiMsgWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"XjsAiAssistantMsg";
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    g_msgwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"XjsAiAssistantMsg", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL,
                               wc.hInstance, NULL);
    return g_msgwnd != NULL;
}

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







