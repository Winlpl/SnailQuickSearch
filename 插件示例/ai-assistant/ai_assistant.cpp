/*
 * ai_assistant.cpp — 蜗牛快搜(D2D) 插件「AI 助手」(preview-panel 面板接管, 一比一对齐正式版 ai-assistant)
 * 形态 = manifest.json + 本 DLL: 接管预览面板内容区, GDI+ 软渲染整块位图交付, 宿主转发鼠标/键盘/IME。
 * 正式版口径: 状态栏"AI 助手"打开; 预览面板没开自动展开 (关闭按打开前状态恢复); 聊天期间选中文件
 * 不覆盖聊天 (宿主守卫); 协议 = OpenAI Responses API (POST {baseUrl}/responses, SSE 流式)。
 * 演示点: ①面板接管全套 (交付位图世代对齐 / 鼠标滚轮键盘 IME / 尺寸自适应)
 *         ②WinHTTP SSE 流式 (工作线程只攒增量, 经消息窗口回 UI 线程渲染 — 渲染不进工作线程)
 *         ③存储 cfg (API Key 按机器码 XOR 混淆, 明文不落盘) + 多对话历史
 *         ④深度思考 (reasoning.effort=high) 可折叠推理过程块
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
static std::string JsonEscapeUtf8(const std::wstring& s) {
    std::string u = U8(s);
    std::string out;
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
    return out;
}
static Jv JsonParseW(const std::wstring& text) {
    JParser jp(text);
    return jp.Val();
}

/* ==================== 配置 (存储键 "cfg"; API Key 按机器码 XOR 混淆, 明文不落盘) ==================== */

struct AiCfg {
    std::wstring baseUrl = L"https://api.deepseek.com";
    std::wstring model = L"deepseek-chat";
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

struct AiMsg {
    int role = 0;               /* 0=user 1=assistant */
    std::wstring text;
    std::wstring reason;        /* 推理过程 (只在折叠块显示, 从不发送/入库发送体) */
    bool reasonOpen = false;    /* 折叠块展开态 (流式中自动展开, 完成后收起) */
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
/* 当前会话落库 (curId=0 → 新建; 否则原位更新), 返回会话 id */
static unsigned long long HistUpsert(unsigned long long curId, const std::vector<AiMsg>& msgs) {
    std::wstring firstUser;
    for (auto& m : msgs)
        if (m.role == 0 && !m.text.empty()) { firstUser = m.text; break; }
    if (firstUser.empty()) return curId;
    AiConv c;
    c.id = curId ? curId : g_nextConvId++;
    c.t = (long long)time(NULL);
    c.title = ConvTitleOf(firstUser);
    c.msgs = msgs;
    while (c.msgs.size() > AI_MSG_MAX) c.msgs.erase(c.msgs.begin());
    for (size_t i = 0; i < g_hist.size(); i++) {
        if (g_hist[i].id == c.id) { g_hist[i] = c; HistSave(); return c.id; }
    }
    g_hist.push_back(c);
    while (g_hist.size() > AI_CONV_MAX) g_hist.erase(g_hist.begin());
    HistSave();
    return c.id;
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
    HIT_CLEAR = 5, HIT_INPUT = 6, HIT_SEND = 7,
    HIT_SBROW = 20, HIT_SBDEL = 60,          /* +i: 侧栏行 / 行删除 */
    HIT_DURL = 100, HIT_DKEY = 101, HIT_DMODEL = 102, HIT_DCHK = 103, HIT_DCANCEL = 104, HIT_DSAVE = 105,
    HIT_THUMB = 120, HIT_TRACK = 121, HIT_REASON = 130   /* +i: 推理块头 */
};

struct AiLine {           /* 排版产物: 一行 = 若干 run (自带文本与样式, 渲染期不依赖临时块) */
    float h = 0;
    float prefixW = 0;    /* 列表前缀宽 (首行 runs 整体右移) */
    std::wstring prefix;  /* 列表前缀文本 ("• " / "1. ") */
    struct Run { float x = 0, w = 0; std::wstring text; bool bold = false, code = false, accent = false; };
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

struct AiJob {            /* 一次流式请求 (堆分配; 工作线程只摸它, UI 经消息泵抽取) */
    CRITICAL_SECTION cs;
    std::wstring out, reason;      /* 已收增量 (worker 写, UI 抽) */
    int state = 0;                 /* 0=进行中 1=完成 2=失败 3=已中止 */
    std::wstring err;
    bool truncated = false;
    std::string hostA, pathA, bodyA, keyA;   /* 请求要素 (UTF-8; worker 自取) */
    INTERNET_PORT port = 443;
    bool secure = true;
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
    Gdiplus::Font* fBody = NULL;      /* 12.5px 正文 */
    Gdiplus::Font* fBodyB = NULL;
    Gdiplus::Font* fMono = NULL;      /* 11.5px 代码 */
    Gdiplus::Font* fTiny = NULL;      /* 10.5px 提示 */
    Gdiplus::Font* fTitle = NULL;     /* 15px 标题 (加粗) */
    float madeScale = 0.0f;
    Gdiplus::FontFamily* famUI = NULL;
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
    int netStatus = 0;                /* 0=未配置/未知(灰) 1=正常(绿) 2=失败(红) */
    /* 输入框 */
    std::wstring input;
    size_t caret = 0;
    bool inputFocus = false;
    /* 滚动 */
    double scrollY = 0;
    bool sticky = true;               /* 吸底 (流式跟随); 向上滚即解除 */
    /* 历史侧栏 */
    bool sideOpen = false;
    double sideScroll = 0;
    bool clearArm = false;
    /* 接口设置对话框 */
    bool dlg = false;
    std::wstring dUrl, dKey, dModel;
    int dFocus = 0;                   /* 0无 1=URL 2=Key 3=Model */
    bool dReason = false;
    /* 命中矩形 (渲染回填; 面板内坐标) */
    FRect hSet, hHist, hNew, hInput, hSend, hClear;
    FRect hThumb; float thumbTrackY = 0, thumbTrackH = 0;
    std::vector<FRect> hRows, hRowDel;
    FRect dUrlR, dKeyR, dModelR, dChkR, dCancelR, dSaveR;
    int hover = HIT_NONE;             /* 高亮 hover (低频: 值变化才重渲染) */
    int press = HIT_NONE;
    int pressGrab = 0;                /* 滚动条拖拽: 抓点偏移 */
    /* 排版缓存 */
    struct MsgLayout {
        std::vector<AiLine> lines;
        std::vector<AiLine> reasonLines;   /* 推理块 (reason 非空才有) */
        float bodyH = 0, reasonH = 0, totalH = 0, bubbleW = 0;
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
    delete s->fBody; delete s->fBodyB; delete s->fMono; delete s->fTiny; delete s->fTitle;
    s->fBody = s->fBodyB = s->fMono = s->fTiny = s->fTitle = NULL;
    delete s->famUI; delete s->famMono;
    s->famUI = s->famMono = NULL;
    s->madeScale = 0.0f;
}
static void SessEnsureFonts(AiSess* s) {
    if (s->madeScale == s->scale && s->fBody) return;
    SessFreeFonts(s);
    s->famUI = new Gdiplus::FontFamily(L"Microsoft YaHei UI");
    if (!s->famUI->IsAvailable()) { delete s->famUI; s->famUI = new Gdiplus::FontFamily(L"Microsoft YaHei"); }
    if (!s->famUI->IsAvailable()) { delete s->famUI; s->famUI = new Gdiplus::FontFamily(L"Segoe UI"); }
    s->famMono = new Gdiplus::FontFamily(L"Consolas");
    if (!s->famMono->IsAvailable()) { delete s->famMono; s->famMono = new Gdiplus::FontFamily(L"Courier New"); }
    float k = s->scale;
    s->fBody = new Gdiplus::Font(s->famUI, 12.5f * k, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    s->fBodyB = new Gdiplus::Font(s->famUI, 12.5f * k, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    s->fMono = new Gdiplus::Font(s->famMono, 11.0f * k, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    s->fTiny = new Gdiplus::Font(s->famUI, 10.5f * k, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
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
    bool code = false, bold = false, accent = false;
};

static float AiMeasure(Gdiplus::Graphics& g, Gdiplus::Font* f, const std::wstring& t) {
    if (t.empty()) return 0;
    Gdiplus::StringFormat fmt;
    fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
    Gdiplus::RectF r;
    g.MeasureString(t.c_str(), (INT)t.size(), f, Gdiplus::PointF(0, 0), &fmt, &r);
    return r.Width;
}

static Gdiplus::Font* AiFontOf(AiSess* s, bool code, bool bold) {
    if (code) return s->fMono;
    return bold ? s->fBodyB : s->fBody;
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
            a.w = AiMeasure(g, AiFontOf(s, a.code, a.bold), piece);
            out->push_back(a);
        };
        while (i < t.size()) {
            wchar_t c = t[i];
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
            for (auto& cl : b.codeLines) {
                AiLine ln; ln.h = AI_CODE_H * k;
                AiBlock::Seg seg; seg.text = cl; seg.code = true;
                std::vector<AiBlock::Seg> one{ seg };
                std::vector<AiAtom> atoms;
                AiSegsToAtoms(g, s, one, &atoms);
                float x = 0;
                for (auto& a : atoms) {
                    AiLine::Run r; r.x = x; r.w = a.w; r.text = a.t; r.code = true;
                    ln.runs.push_back(r);
                    x += a.w;
                }
                lines->push_back(ln);
                *contentH += ln.h;
            }
            lines->push_back(AiLine()); lines->back().h = codePad;
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
            lines->push_back(cur);
            *contentH += cur.h;
            cur = AiLine();
            x = indent;
            firstLine = false;
        };
        for (auto& a : atoms) {
            if (a.t == L" " && cur.runs.empty() && !firstLine) continue;   /* 行首空格丢弃 */
            if (x + a.w > maxW && !cur.runs.empty()) { emit(); }
            AiLine::Run r; r.x = x; r.w = a.w; r.text = a.t;
            r.bold = a.bold; r.code = a.code; r.accent = a.accent;
            cur.runs.push_back(r);
            x += a.w;
            if (x > maxW) { emit(); }   /* 超宽单词兜底逐段硬拆观感 (下一 atom 起新行) */
        }
        emit();
        lines->push_back(AiLine()); lines->back().h = 5.0f * k;   /* 块间距 */
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
            AiLine::Run r; r.x = x; r.w = at.w; r.text = at.t;
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

    L.lines.clear();
    L.reasonLines.clear();
    L.bodyH = 0;
    L.reasonH = 0;
    std::vector<AiBlock> blocks;
    MdParse(m.text, &blocks);
    AiPackLines(s, mg, blocks, maxTextW, k, &L.lines, &L.bodyH);
    if (!m.reason.empty() && m.reasonOpen) {
        WrapPlain(s, mg, m.reason, maxTextW - 8.0f * k, k, &L.reasonH);
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
    L.totalH = 10.0f * k * 2 + L.bodyH + L.reasonH + (m.reason.empty() ? 0 : (m.reasonOpen ? 4.0f * k : 24.0f * k));
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
    Gdiplus::StringFormat fmt;
    fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
    g.DrawString(t.c_str(), (INT)t.size(), f, Gdiplus::PointF(x, y), &fmt, br);
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

/* 输入框换行数 (1..4; 排版口径同消息行) */
static int InputLineCount(AiSess* s, float boxInnerW) {
    if (s->input.empty()) return 1;
    Gdiplus::Bitmap tmp(1, 1, PixelFormat32bppPARGB);
    Gdiplus::Graphics mg(&tmp);
    std::vector<AiBlock::Seg> segs{ AiBlock::Seg{} };
    segs[0].text = s->input;
    std::vector<AiAtom> atoms;
    AiSegsToAtoms(mg, s, segs, &atoms);
    int lines = 1;
    float x = 0;
    for (auto& a : atoms) {
        if (x + a.w > boxInnerW && x > 0) { lines++; x = 0; }
        x += a.w;
        if (lines >= 4) return 4;
    }
    return lines;
}

/* 输入框内光标 (列, 行, 行内 x) — caret 折算到换行后的可视位置 */
static void InputCaretPos(AiSess* s, float boxInnerW, int* outLine, float* outX) {
    Gdiplus::Bitmap tmp(1, 1, PixelFormat32bppPARGB);
    Gdiplus::Graphics mg(&tmp);
    std::vector<AiBlock::Seg> segs{ AiBlock::Seg{} };
    segs[0].text = s->input.substr(0, s->caret);
    std::vector<AiAtom> atoms;
    AiSegsToAtoms(mg, s, segs, &atoms);
    int line = 0;
    float x = 0;
    for (auto& a : atoms) {
        if (x + a.w > boxInnerW && x > 0) { line++; x = 0; }
        x += a.w;
    }
    *outLine = line;
    *outX = x;
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
                    if (!r.text.empty()) AiText(g, r.text, s->fTiny, &db, rx + 6.0f * k + r.x, ty);
                ty += ln.h;
            }
        }
        yCur += (m.reasonOpen ? L.reasonH + 4.0f * k : 24.0f * k);
    }
    /* 正文行 */
    for (auto& ln : L.lines) {
        if (!ln.prefix.empty()) AiText(g, ln.prefix, s->fBody, Gdiplus::SolidBrush(s->cDim), bubbleX + bp, yCur);
        for (auto& r : ln.runs) {
            if (r.text.empty()) continue;
            Gdiplus::Font* f = AiFontOf(s, r.code, r.bold);
            Gdiplus::Color c = r.code ? MixCol(s->cAccent, s->cText, 0.45f)
                              : r.accent ? s->cAccent : s->cText;
            Gdiplus::SolidBrush br(c);
            AiText(g, r.text, f, &br, bubbleX + bp + r.x, yCur);
        }
        yCur += ln.h;
    }
    /* 流式光标 (最后一条 AI 消息发送中) */
    if (s->sending && !user && mi == (int)s->msgs.size() - 1 && ((GetTickCount64() / 500) & 1)) {
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
            float y = msgsTop + s->layOffsets[i] - (float)s->scrollY;
            if (y > msgsTop + msgsH || y + s->lay[i].totalH < msgsTop) continue;
            DrawMsg(s, g, i, y);
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
        /* 文本 / 占位 / 光标 */
        g.SetClip(Gdiplus::RectF(pad + 2.0f * k, boxY, boxW - 4.0f * k, boxH));
        if (s->input.empty()) {
            Gdiplus::SolidBrush db(WithA(s->cDim, 150));
            AiText(g, L"输入消息，Enter 发送…", s->fBody, &db, pad + 10.0f * k, boxY + 10.0f * k);
        } else {
            std::vector<AiBlock::Seg> segs{ AiBlock::Seg{} };
            segs[0].text = s->input;
            std::vector<AiAtom> atoms;
            Gdiplus::Bitmap tmp(1, 1, PixelFormat32bppPARGB);
            Gdiplus::Graphics mg(&tmp);
            AiSegsToAtoms(mg, s, segs, &atoms);
            Gdiplus::SolidBrush tb(s->cText);
            float x = 10.0f * k, y = 10.0f * k;
            float caretX = x, caretY = y;
            size_t acc = 0;
            for (auto& a : atoms) {
                if (s->caret >= acc + a.t.size()) {
                    if (x + a.w > boxInnerW && x > 10.0f * k) { y += AI_LINE_H * k; x = 10.0f * k; }
                    AiText(g, a.t, s->fBody, &tb, pad + x, boxY + y);
                    x += a.w;
                    acc += a.t.size();
                    caretX = x;
                    caretY = y;
                } else {
                    size_t off = s->caret - acc;
                    std::wstring before = a.t.substr(0, off);
                    if (!before.empty()) {
                        if (x + a.w > boxInnerW && x > 10.0f * k) { y += AI_LINE_H * k; x = 10.0f * k; }
                        AiText(g, before, s->fBody, &tb, pad + x, boxY + y);
                        caretX = x + AiMeasure(mg, s->fBody, before);
                        caretY = y;
                        x += caretX - (x == 10.0f * k ? 0 : x);
                    }
                    break;
                }
            }
            if (s->inputFocus && s->winActive && ((GetTickCount64() / 530) & 1))
                AiFillRect(g, Gdiplus::SolidBrush(s->cText),
                                Gdiplus::RectF(pad + caretX, boxY + caretY + 2.0f * k, 1.4f * k, AI_LINE_H * k - 4.0f * k));
        }
        g.ResetClip();
        /* 发送 / 停止 */
        float sendX = s->w - pad - sendW;
        float sendY = boxY + boxH - sendH;
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
        std::wstring bl = stop ? L"■ 停止" : L"发送";
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
            if (s->hover == HIT_SBROW + i)
                AiFillRect(g, Gdiplus::SolidBrush(WithA(s->cText, 14)), Gdiplus::RectF(sx, y, sideW, rh));
            Gdiplus::SolidBrush tb2(active ? s->cText : MixCol(s->cText, s->cDim, 0.35f));
            AiTextTrunc(g, c.title, s->fBody, &tb2, sx + 14.0f * k, y + 8.0f * k, sideW - 60.0f * k);
            Gdiplus::SolidBrush db2(s->cDim);
            AiText(g, TimeTextOf(c.t), s->fTiny, &db2, sx + 14.0f * k, y + 28.0f * k);
            /* 删除 ✕ */
            float dx = s->w - 26.0f * k, dy = y + 8.0f * k;
            bool hotDel = s->hover == HIT_SBDEL + i;
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
        (void)vi;
    } else {
        s->hClear = FRect{};
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
                AiText(g, f.ph, s->fTiny, &pb, bx + 8.0f * k, fy + (bh2 - AI_TINY_H * k) / 2);
            } else {
                std::wstring vis = show;
                while (vis.size() > 2 && AiMeasure(g, s->fBody, vis) > bw - 16.0f * k) vis.erase(0, 1);   /* 超长左裁 (密钥习惯看尾) */
                AiText(g, vis, s->fBody, &tb, bx + 8.0f * k, fy + (bh2 - AI_LINE_H * k) / 2);
                if (foc && s->winActive && ((GetTickCount64() / 530) & 1)) {
                    float tw = AiMeasure(g, s->fBody, vis);
                    AiFillRect(g, Gdiplus::SolidBrush(s->cText),
                               Gdiplus::RectF(bx + 8.0f * k + tw, fy + 5.0f * k, 1.4f * k, bh2 - 10.0f * k));
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
    if (s->hSet.Hit(x, y)) return HIT_HSET;
    if (s->hHist.Hit(x, y)) return HIT_HHIST;
    if (s->hNew.Hit(x, y)) return HIT_HNEW;
    if (s->sideOpen) {
        if (s->hClear.Hit(x, y)) return HIT_CLEAR;
        for (int i = 0; i < (int)s->hRows.size(); i++) {
            if (s->hRows[i].Hit(x, y)) {
                if (i < (int)s->hRowDel.size() && s->hRowDel[i].Hit(x, y)) { *idxOut = i; return HIT_SBDEL; }
                *idxOut = i;
                return HIT_SBROW;
            }
        }
    }
    if (s->hSend.Hit(x, y)) return HIT_SEND;
    if (s->hThumb.w > 0 && s->hThumb.Hit(x, y)) return HIT_THUMB;
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
static void LoadConv(AiSess* s, int i) {
    if (i < 0 || i >= (int)g_hist.size()) return;
    AbortSend(s);
    SessSaveConv(s);   /* 当前未存对话先存 */
    s->msgs = g_hist[i].msgs;
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

/* ---- 输入编辑 ---- */
static void InsertText(AiSess* s, const std::wstring& t) {
    if (s->input.size() + t.size() > 8000) return;
    s->input.insert(s->caret, t);
    s->caret += t.size();
}

/* IME 锚点 → 宿主 (输入框内光标点位; 渲染几何同源换算) */
static void UpdateImeAnchor(AiSess* s) {
    if (!HOST_PANEL_OK || !g_host || !s->inUse) return;
    float bx, by, bw, bh;
    InputGeom(s, &bx, &by, &bw, &bh, NULL);
    int line = 0;
    float cx = 0;
    InputCaretPos(s, bw - 10.0f * s->scale * 2, &line, &cx);
    g_host->PanelSetCaret(g_ctx, s->tok, (int)(bx + 10.0f * s->scale + cx), (int)(by + 10.0f * s->scale + line * AI_LINE_H * s->scale));
}

/* 鼠标/滚轮/键盘事件 (OnPanelEvent 转发; 全 UI 线程) */
static void PanelMouse(AiSess* s, int type, float x, float y, unsigned flags) {
    (void)flags;
    int idx = -1;
    int hit = x < 0 ? HIT_NONE : HitTest(s, x, y, &idx);
    switch (type) {
        case XJS_PANEL_MOUSE_MOVE: {
            if (s->pressGrab && s->hThumb.w > 0) {   /* 滚动条拖拽 */
                float trackH = s->thumbTrackH - s->hThumb.h;
                float maxScroll = s->contentH - s->thumbTrackH;
                if (trackH > 0 && maxScroll > 0)
                    s->scrollY = maxScroll * (double)((y - s->thumbTrackY - s->pressGrab) / trackH);
                if (s->scrollY < 0) s->scrollY = 0;
                if (s->scrollY > maxScroll) s->scrollY = maxScroll;
                RenderDeliver(s);
                return;
            }
            if (hit != s->hover) { s->hover = hit; RenderDeliver(s); }
            return;
        }
        case XJS_PANEL_LDOWN: {
            s->press = hit;
            if (s->dlg) {
                /* 对话框开着: 点字段 = 聚焦 (文本框按下聚焦是标准语义, 不走松开触发) */
                if (hit == HIT_DURL) s->dFocus = 1;
                else if (hit == HIT_DKEY) s->dFocus = 2;
                else if (hit == HIT_DMODEL) s->dFocus = 3;
            } else if (hit == HIT_INPUT) {
                s->inputFocus = true;
                if (HOST_PANEL_OK && g_host) g_host->PanelSetFocus(g_ctx, s->tok, 1);
                UpdateImeAnchor(s);
            } else if (hit == HIT_THUMB) {
                s->pressGrab = y - s->hThumb.y;
            } else if (hit == HIT_TRACK && s->hThumb.w > 0) {
                float maxScroll = s->contentH - s->thumbTrackH;
                if (maxScroll > 0) s->scrollY = maxScroll * (double)((y - s->thumbTrackY) / s->thumbTrackH);
                s->pressGrab = s->hThumb.h / 2;
            } else if (hit == HIT_NONE) {
                /* 点输入框以外: 归还键盘 (点消息流/空白不再打字) */
                if (s->inputFocus) {
                    s->inputFocus = false;
                    if (HOST_PANEL_OK && g_host) g_host->PanelSetFocus(g_ctx, s->tok, 0);
                }
            }
            RenderDeliver(s);
            return;
        }
        case XJS_PANEL_LUP: {
            int press = s->press;
            s->press = HIT_NONE;
            s->pressGrab = 0;
            /* 对话框开着点蒙层空白 (按下与松开都在卡外) = 取消 (原版 ai-chat 点遮罩取消口径) */
            if (s->dlg && press == HIT_NONE && hit != HIT_DSAVE && hit != HIT_DCANCEL &&
                hit != HIT_DCHK && hit != HIT_DURL && hit != HIT_DKEY && hit != HIT_DMODEL) {
                DlgClose(s);
                RenderDeliver(s);
                return;
            }
            bool same = (hit == press) || (press == HIT_TRACK);
            if (!same) { RenderDeliver(s); return; }
            switch (press) {
                case HIT_HSET: DlgOpen(s); break;
                case HIT_HHIST: s->sideOpen = !s->sideOpen; s->clearArm = false; s->sideScroll = 0; break;
                case HIT_HNEW: NewConv(s); break;
                case HIT_CLEAR:
                    if (s->clearArm) ClearHist(s);
                    else s->clearArm = true;
                    break;
                case HIT_SBROW: LoadConv(s, idx); break;
                case HIT_SBDEL: DeleteConv(s, idx); break;
                case HIT_REASON:
                    if (idx >= 0 && idx < (int)s->msgs.size()) {
                        s->msgs[idx].reasonOpen = !s->msgs[idx].reasonOpen;
                        RelayoutOne(s, idx);
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
        default: return;   /* RDOWN/RUP/DBLCLK 预留 */
    }
}

static void PanelWheel(AiSess* s, float x, float y, int delta) {
    float k = s->scale;
    if (s->sideOpen && x >= s->w - minf(320.0f * k, s->w * 0.62f)) {
        s->sideScroll -= delta / 120.0 * 3 * 15.0f * k;
        float rows = (float)g_hist.size() * 50.0f * k - (s->bh - AI_HEAD_H * k - 40.0f * k);
        if (s->sideScroll < 0) s->sideScroll = 0;
        if (s->sideScroll > rows) s->sideScroll = rows;
    } else {
        s->scrollY -= delta / 120.0 * 3 * (double)AI_LINE_H * k;
        if (s->scrollY < 0) s->scrollY = 0;
        s->sticky = (delta > 0);   /* 向上滚解除吸底, 滚回底部恢复 */
    }
    RenderDeliver(s);
}

static void PanelKey(AiSess* s, unsigned vk, unsigned flags) {
    bool ctrl = (flags & 1) != 0, shift = (flags & 2) != 0;
    if (vk == VK_PROCESSKEY) return;   /* IME 组合中的键 (宿主原样转发) */
    if (s->dlg) {
        /* 对话框字段编辑 */
        std::wstring* f = s->dFocus == 1 ? &s->dUrl : s->dFocus == 2 ? &s->dKey : s->dFocus == 3 ? &s->dModel : NULL;
        if (vk == VK_TAB) {
            s->dFocus = s->dFocus == 3 ? 1 : s->dFocus + 1;
        } else if (vk == VK_ESCAPE) {
            DlgClose(s);
            RenderDeliver(s);
            return;
        } else if (vk == VK_RETURN) {
            DlgSave(s);
        } else if (f) {
            if (vk == VK_BACK && !f->empty()) { *f = f->substr(0, PrevCp(*f, f->size())); }
            else if (vk == VK_DELETE) { /* 对话框字段无 caret, 退格即可 */ }
            else if (vk == 'V' && ctrl) {
                char buf[8192];
                int n = g_host->ClipboardGetText(g_ctx, buf, (int)sizeof(buf) - 1);
                if (n > 0) { buf[n] = 0; *f += W8(buf); }
            }
        }
        RenderDeliver(s);
        return;
    }
    if (!s->inputFocus) {
        if (vk == VK_ESCAPE && s->sideOpen) { s->sideOpen = false; s->clearArm = false; RenderDeliver(s); }
        return;
    }
    switch (vk) {
        case VK_LEFT: s->caret = PrevCp(s->input, s->caret); UpdateImeAnchor(s); break;
        case VK_RIGHT: s->caret = NextCp(s->input, s->caret); UpdateImeAnchor(s); break;
        case VK_HOME: s->caret = 0; UpdateImeAnchor(s); break;
        case VK_END: s->caret = s->input.size(); UpdateImeAnchor(s); break;
        case VK_UP: case VK_DOWN: break;   /* 单行可视高度内不移动 (预留多行行导航) */
        case VK_BACK:
            if (s->caret > 0) {
                size_t p = PrevCp(s->input, s->caret);
                s->input.erase(p, s->caret - p);
                s->caret = p;
                UpdateImeAnchor(s);
            }
            break;
        case VK_DELETE:
            if (s->caret < s->input.size()) {
                size_t n2 = NextCp(s->input, s->caret);
                s->input.erase(s->caret, n2 - s->caret);
            }
            break;
        case VK_RETURN:
            if (shift) InsertText(s, L"\n");
            else { SendCurrent(s); }
            break;
        case 'V':
            if (ctrl) {
                char buf[65536];
                int n = g_host->ClipboardGetText(g_ctx, buf, (int)sizeof(buf) - 1);
                if (n > 0) {
                    buf[n] = 0;
                    std::wstring t = W8(buf);
                    for (auto& c : t) if (c == L'\r') c = L'\n';
                    InsertText(s, t);
                    UpdateImeAnchor(s);
                }
            }
            break;
        default: break;
    }
    RenderDeliver(s);
}

static void PanelChar(AiSess* s, unsigned int ch) {
    if (s->dlg) {
        if (ch >= 0x20 && s->dFocus >= 1 && s->dFocus <= 3) {
            std::wstring* f = s->dFocus == 1 ? &s->dUrl : s->dFocus == 2 ? &s->dKey : &s->dModel;
            if (f->size() < 2048) *f += (wchar_t)ch;
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

static const wchar_t* AI_INSTRUCTIONS =
    L"你是蜗牛快搜内置的 AI 助手。请用简体中文回答，尽量简洁、准确、直接。";

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
    s->layDirty = true;
    s->sticky = true;
    /* 请求要素 (快照; 线程只读这些) */
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
    if (path.empty()) path = L"";
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
    std::wstring fullPath = path + L"/responses";
    j->pathA = U8(fullPath);
    /* body: 历史 (≤30 条) + 本条, assistant→output_text / user→input_text; 剔除开头非 user */
    std::wstring body = L"{\"model\":";
    body += W8(JsonEscapeUtf8(g_cfg.model).c_str());
    body += L",\"input\":[";
    {
        std::vector<const AiMsg*> hs;
        for (auto& m : s->msgs)
            if (!m.text.empty()) hs.push_back(&m);
        if ((int)hs.size() > 30) hs.erase(hs.begin(), hs.end() - 30);
        while (!hs.empty() && hs.front()->role != 0) hs.erase(hs.begin());
        bool first = true;
        for (auto* m : hs) {
            if (!first) body += L",";
            first = false;
            body += m->role ? L"{\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":" 
                            : L"{\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":";
            body += W8(JsonEscapeUtf8(m->text).c_str());
            body += L"}]}";
        }
    }
    body += L"],\"stream\":true,\"instructions\":";
    body += W8(JsonEscapeUtf8(AI_INSTRUCTIONS).c_str());
    body += L",\"reasoning\":{\"effort\":\"";
    body += g_cfg.reasoning ? L"high" : L"none";
    body += L"\"}}";
    j->bodyA = U8(body);
    s->job = j;
    s->sending = true;
    s->netStatus = g_cfg.apiKey.empty() ? 0 : s->netStatus;
    j->th = new std::thread([j]() { WorkerMain(j); });
    RenderDeliver(s);
}

static void WorkerMain(AiJob* j) {   /* 工作线程: 只摸 j, 渲染交回 UI 泵 */
    wchar_t whost[512] = {};
    wchar_t wpath[1024] = {};
    MultiByteToWideChar(CP_UTF8, 0, j->hostA.c_str(), -1, whost, 512);
    MultiByteToWideChar(CP_UTF8, 0, j->pathA.c_str(), -1, wpath, 1024);
    HINTERNET hs = WinHttpOpen(L"snail-quicksearch-ai-assistant", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!hs) hs = WinHttpOpen(L"snail-quicksearch-ai-assistant", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    HINTERNET hc = NULL, hr = NULL;
    bool gotErr = false, aborted = false, truncated = false;
    std::string errMsg;
    if (hs) WinHttpSetTimeouts(hs, 15000, 30000, 30000, 120000);
    if (!hs) { gotErr = true; errMsg = "network init failed"; goto done_nolock; }
    hc = WinHttpConnect(hs, whost, j->port, 0);
    if (!hc) { gotErr = true; errMsg = "connect failed"; goto done_nolock; }
    hr = WinHttpOpenRequest(hc, L"POST", wpath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                            j->secure ? WINHTTP_FLAG_SECURE : 0);
    if (!hr) { gotErr = true; errMsg = "open request failed"; goto done_nolock; }
    {
        EnterCriticalSection(&j->cs);
        j->hReq = hr;   /* UI"停止"并发关句柄打断阻塞读 */
        LeaveCriticalSection(&j->cs);
        std::wstring hdr = L"Content-Type: application/json\r\nAuthorization: Bearer ";
        {
            std::wstring wk = W8(j->keyA.c_str());
            hdr += wk;
        }
        WinHttpAddRequestHeaders(hr, hdr.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
        if (!WinHttpSendRequest(hr, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                (LPVOID)j->bodyA.data(), (DWORD)j->bodyA.size(), (DWORD)j->bodyA.size(), 0) ||
            !WinHttpReceiveResponse(hr, NULL)) {
            gotErr = true;
            errMsg = "send/receive failed";
            goto done_nolock;
        }
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &sz, NULL);
        if (status != 200) {
            char eb[8192] = {};
            DWORD erd = 0, eofc = 0;
            while (erd < sizeof(eb) - 1 && WinHttpReadData(hr, eb + erd, sizeof(eb) - 1 - erd, &eofc) && eofc)
                erd += eofc;
            eb[erd] = 0;
            std::wstring wide = W8(eb);
            Jv v = JsonParseW(wide);
            std::wstring msg;
            const Jv* em = v.Get(L"message");
            if (em && em->t == 3) msg = em->str;
            if (msg.empty()) {
                const Jv* eo = v.Get(L"error");
                if (eo && eo->t == 5) msg = eo->S(L"message");
            }
            std::string detail = msg.empty() ? (wide.size() > 200 ? U8(wide.substr(0, 200)) : U8(wide))
                                             : U8(msg);
            char mb[512];
            _snprintf_s(mb, sizeof(mb), _TRUNCATE, "HTTP %lu: %s", (unsigned long)status, detail.c_str());
            gotErr = true;
            errMsg = mb;
            goto done_nolock;
        }
        /* SSE 流式 */
        {
            std::string buf;
            ULONGLONG lastPost = 0;
            bool failed = false;
            for (;;) {
                if (InterlockedCompareExchange(&j->abort, 0, 0)) { aborted = true; break; }
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(hr, &avail)) break;
                if (!avail) break;
                std::string chunk((size_t)avail, 0);
                DWORD rd = 0;
                if (!WinHttpReadData(hr, &chunk[0], avail, &rd)) break;
                if (InterlockedCompareExchange(&j->abort, 0, 0)) { aborted = true; break; }
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
                    } else if (type == L"response.failed") {
                        const Jv* rsp = ev.Get(L"response");
                        if (rsp && rsp->t == 5) {
                            const Jv* er = rsp->Get(L"error");
                            if (er && er->t == 5) errMsg = U8(er->S(L"message").c_str());
                        }
                        failed = true;
                    } else if (type == L"response.incomplete") {
                        truncated = true;
                    } else if (type == L"error") {
                        const Jv* er = ev.Get(L"error");
                        errMsg = U8((er && er->t == 5 ? er->S(L"message") : ev.S(L"message")).c_str());
                        failed = true;
                    }
                    LeaveCriticalSection(&j->cs);
                }
                ULONGLONG now = GetTickCount64();
                if (now - lastPost > 40 && g_msgwnd) {   /* 节流回泵 (UI 抽增量渲染) */
                    lastPost = now;
                    PostMessageW(g_msgwnd, XJS_AI_STREAM, 0, (LPARAM)j);
                }
            }
            if (failed) gotErr = true;
        }
    }
done_nolock:
    if (hr) WinHttpCloseHandle(hr);
    if (hc) WinHttpCloseHandle(hc);
    if (hs) WinHttpCloseHandle(hs);
    EnterCriticalSection(&j->cs);
    if (j->hReq) { WinHttpCloseHandle(j->hReq); j->hReq = NULL; }
    j->truncated = truncated;
    j->err = W8(errMsg.c_str());
    j->state = aborted ? 3 : (gotErr ? 2 : 1);
    LeaveCriticalSection(&j->cs);
    if (g_msgwnd) PostMessageW(g_msgwnd, XJS_AI_STREAM, 0, (LPARAM)j);
}

/* UI 泵: 抽增量 / 收尾 (join) / 孤儿清扫 */
static void PumpStreams() {
    for (auto& s : g_sess) {
        AiJob* j = s.job;
        if (!s.inUse || !j) continue;
        std::wstring out, reason;
        int state = 0;
        std::wstring err;
        bool truncated = false;
        EnterCriticalSection(&j->cs);
        out = j->out;
        reason = j->reason;
        state = j->state;
        err = j->err;
        truncated = j->truncated;
        LeaveCriticalSection(&j->cs);
        if (state == 0) {
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
        } else {
            /* 收尾: 状态落消息 + 落库 + join + 清作业 */
            if (s.msgs.empty() || s.msgs.back().role != 1) {
                AiMsg am;
                am.role = 1;
                s.msgs.push_back(am);
            }
            AiMsg& back = s.msgs.back();
            back.text = out;
            back.reason = reason;
            back.reasonOpen = false;
            if (state == 3) {
                if (back.text.empty()) back.text = L"已停止生成";
                else back.text += L"\n\n*(已停止生成)*";
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
            s.curId = HistUpsert(s.curId, s.msgs);
            s.job = NULL;
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
    static const XjsPluginInfo info = { XJS_PLUGIN_ABI_VERSION, sizeof(XjsPluginInfo), "ai-assistant", "1.0.0" };
    return &info;
}

static int XJS_PLUGIN_CALL XjsPlugin_Init(XjsPluginCtx* ctx, const XjsPluginHost* host) {
    g_ctx = ctx;
    g_host = host;
    g_uiThread = GetCurrentThreadId();
    if (!HOST_PANEL_OK) return XJS_PLUGIN_ERR_FAIL;   /* 旧宿主 (无 v4 Panel* 表) = 干净失败 */
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
        default:
            return;
    }
}

/* 固定导出: OnHostGone (引擎尚未销毁的最后通知; 只收线程, GDI+/窗口留给随后的 Shutdown) */
extern "C" __declspec(dllexport) void XJS_PLUGIN_CALL XjsPlugin_OnHostGone(XjsPluginCtx* ctx) {
    (void)ctx;
    AbortAndJoinAll(2000);
}







