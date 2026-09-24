/*
 * ai_core.cpp — AI 助手基础设施: 编码转换 / base64 / 简易 JSON / 颜色几何 / 配置 / 多对话历史
 * 结构与接口见 ai_assistant.h。
 */
#include "ai_assistant.h"

/* ==================== 基础工具 ==================== */

std::wstring W8(const char* s) {
    if (!s || !*s) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);   /* n 含终止 NUL */
    std::wstring w(n > 0 ? n : 1, 0);
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    while (!w.empty() && w.back() == 0) w.pop_back();
    return w;
}
std::string U8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s(n > 0 ? n : 0, 0);
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}
std::wstring TrimW(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}
/* 代理对安全步进 (caret 按 UTF-16 单元计) */
size_t PrevCp(const std::wstring& s, size_t i) {
    while (i > 0 && (s[i - 1] & 0xFC00) == 0xDC00 && i > 1 && (s[i - 2] & 0xFC00) == 0xD800) i -= 2;
    if (i > 0) i -= 1;
    return i;
}
size_t NextCp(const std::wstring& s, size_t i) {
    if (i < s.size()) {
        i += 1;
        if (i < s.size() && (s[i - 1] & 0xFC00) == 0xD800 && (s[i] & 0xFC00) == 0xDC00) i += 1;
    }
    return i;
}

/* ---- base64 (API Key 加密存储用) ---- */
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

std::string JsonEscapeUtf8(const std::wstring& s) {   /* → 完整 JSON 字符串字面量 (含首尾引号;
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
Jv JsonParseW(const std::wstring& text) {
    JParser jp(text);
    return jp.Val();
}

/* ==================== 配置 (存储键 "cfg"; API Key 以本机 GUID 为密码加密, 明文不落盘) ==================== */

AiCfg g_cfg;

/* 加密密码 = 本机 GUID: ① 注册表 MachineGuid (系统自带, 首选); ② 读不到 (受限环境)
   → C:\ProgramData\SnailQuickSearch\ai-key-guid.txt 生成一份专属 GUID 落盘, 长期复用 —
   密码不出本机且在程序目录之外, 配置目录被整包分享到别的机器也解不开 (防分享泄漏);
   ③ GUID 生成也失败 (几乎不可能) → 常量兜底, 当次可加解自洽, 换机=视为未配置重输 */
static std::wstring MachineKeyStr() {
    const wchar_t* SALT = L"|snail-ai-key";   /* 派生盐: 与注册表路径口径一致, 防密码串被原样照搬 */
    wchar_t buf[128] = {};
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid",
                     RRF_RT_REG_SZ, NULL, buf, &sz) == ERROR_SUCCESS && buf[0])
        return std::wstring(buf) + SALT;
    /* 回退: C 盘 GUID 文件 (UTF-16 文本一行), 有则读用, 无则生成并写回 */
    const wchar_t* file = L"C:\\ProgramData\\SnailQuickSearch\\ai-key-guid.txt";
    wchar_t line[128] = {};
    HANDLE h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD rd = 0;
        ReadFile(h, line, sizeof(line) - sizeof(line[0]), &rd, NULL);
        CloseHandle(h);
        std::wstring guid(line, rd / sizeof(line[0]));
        while (!guid.empty() && (guid.back() == 0 || guid.back() == L'\r' || guid.back() == L'\n'
                              || guid.back() == L' ' || guid.back() == L'\t')) guid.pop_back();
        if (guid.size() >= 36) return guid + SALT;
    }
    GUID g;
    wchar_t wgs[64] = {};
    if (CoCreateGuid(&g) == S_OK && StringFromGUID2(g, wgs, 64) > 0) {
        CreateDirectoryW(L"C:\\ProgramData\\SnailQuickSearch", NULL);   /* 已存在则无害失败 */
        h = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD wr = 0;
            WriteFile(h, wgs, (DWORD)(wcslen(wgs) * sizeof(wchar_t)), &wr, NULL);
            CloseHandle(h);
        }
        return std::wstring(wgs) + SALT;   /* 文件写失败 (只读盘等) = 当次内存 GUID, 下次解不开按未配置 */
    }
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
void CfgSave() {
    if (!g_host) return;
    /* JSON 字面量拼接: 先 UTF-8 转义再转回宽字符 (纯 ASCII 字面量), 整体一次落盘 */
    auto lit = [](const std::wstring& s) { return W8(JsonEscapeUtf8(s).c_str()); };
    std::wstring j = L"{\"baseUrl\":" + lit(g_cfg.baseUrl) + L",\"model\":" + lit(g_cfg.model);
    if (!g_cfg.apiKey.empty()) {
        std::string enc = B64Enc(XorSecret(U8(g_cfg.apiKey), U8(MachineKeyStr())));
        j += L",\"apiKeyEnc\":\"enc:1:" + W8(enc.c_str()) + L"\"";
    }
    j += g_cfg.reasoning ? L",\"reasoning\":true" : L",\"reasoning\":false";
    j += L",\"filePolicy\":" + std::to_wstring(g_cfg.filePolicy) + L"}";
    std::string u8 = U8(j);
    g_host->StorageSet(g_ctx, "cfg", u8.c_str(), (int)u8.size());
}
void CfgLoad() {
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
    const Jv* fp = v.Get(L"filePolicy");
    if (fp && fp->t == 2 && fp->num >= 0 && fp->num <= 3) g_cfg.filePolicy = (int)fp->num;
    std::wstring enc = v.S(L"apiKeyEnc");
    if (enc.rfind(L"enc:1:", 0) == 0) {
        std::string raw = B64Dec(U8(enc.substr(6)));
        std::string plain = XorSecret(raw, U8(MachineKeyStr()));
        g_cfg.apiKey = ValidUtf8(plain) ? W8(plain.c_str()) : L"";   /* 换机解出乱码 = 视为未配置 */
    }
}

/* ==================== 多对话历史 (存储键 "历史"; 上限 30 会话/每会话 200 条) ==================== */

std::vector<AiConv> g_hist;
static unsigned long long g_nextConvId = 1;

static std::wstring ConvTitleOf(const std::wstring& firstUser) {
    std::wstring t = TrimW(firstUser);
    if (t.size() > 30) { t = t.substr(0, 30); t += L"…"; }
    return t;
}
void HistSave() {
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
            if (m.role == 2) {
                /* 工具卡片组: query/err 截到 512 字符 (绘制端同款上限, 存整段脚本无展示出口) */
                j += L"{\"r\":2,\"steps\":[";
                for (size_t si = 0; si < m.steps.size(); si++) {
                    const AiToolStep& t = m.steps[si];
                    if (si) j += L",";
                    wchar_t sh[80];
                    swprintf(sh, 80, L"{\"k\":%d,\"st\":%d,\"n\":%d,\"ms\":%lld",
                             t.kind, t.state, t.count, t.elapsedMs);
                    j += sh;
                    j += L",\"name\":";
                    j += W8(JsonEscapeUtf8(t.name).c_str());
                    if (!t.argz.empty()) {
                        j += L",\"argz\":";
                        j += W8(JsonEscapeUtf8(t.argz).c_str());
                    }
                    if (!t.mode.empty()) {
                        j += L",\"mode\":";
                        j += W8(JsonEscapeUtf8(t.mode).c_str());
                    }
                    if (!t.query.empty()) {
                        j += L",\"query\":";
                        j += W8(JsonEscapeUtf8(t.query.substr(0, 512)).c_str());
                    }
                    if (!t.err.empty()) {
                        j += L",\"err\":";
                        j += W8(JsonEscapeUtf8(t.err.substr(0, 512)).c_str());
                    }
                    if (!t.adj.items.empty()) {
                        /* 待应用的调整 (含逐项状态): 落库后重开会话卡片仍可应用/忽略 */
                        j += L",\"adj\":{\"kind\":";
                        j += std::to_wstring(t.adj.kind);
                        j += L",\"win\":";
                        j += W8(JsonEscapeUtf8(t.adj.win).c_str());
                        j += L",\"items\":[";
                        for (size_t aj = 0; aj < t.adj.items.size(); aj++) {
                            const AiAdjustItem& it = t.adj.items[aj];
                            if (aj) j += L",";
                            j += L"{\"key\":";
                            j += W8(JsonEscapeUtf8(it.key).c_str());
                            j += L",\"val\":";
                            j += W8(JsonEscapeUtf8(it.val).c_str());
                            j += L",\"json\":";
                            j += W8(JsonEscapeUtf8(W8(it.json.c_str())).c_str());
                            wchar_t sb[48];
                            swprintf(sb, 48, L",\"st\":%d", it.state);
                            j += sb;
                            if (!it.err.empty()) {
                                j += L",\"err\":";
                                j += W8(JsonEscapeUtf8(it.err.substr(0, 200)).c_str());
                            }
                            j += L"}";
                        }
                        j += L"]}";
                    }
                    if (!t.top.empty()) {
                        j += L",\"top\":[";
                        for (size_t pi = 0; pi < t.top.size(); pi++) {
                            if (pi) j += L",";
                            j += W8(JsonEscapeUtf8(t.top[pi]).c_str());
                        }
                        j += L"]";
                    }
                    j += L"}";
                }
                j += L"]}";
                continue;
            }
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
void HistLoad() {
    g_hist.clear();
    if (!g_host) return;
    /* 上限裁剪在装载侧做: 存储值无硬上限, 读入 16MB 足够 30 会话满载 (含工具卡片样本路径) */
    std::string buf;
    buf.resize(16 * 1024 * 1024);
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
                if (jr && jr->num == 2) {
                    m.role = 2;
                    const Jv* js = jmsg.Get(L"steps");
                    if (js && js->t == 4) {
                        for (auto& jst : js->arr) {
                            if (jst.t != 5) continue;
                            AiToolStep t;
                            auto num = [&jst](const wchar_t* k, int def) {
                                const Jv* v2 = jst.Get(k);
                                return (v2 && v2->t == 2) ? (int)v2->num : def;
                            };
                            t.kind = num(L"k", 0);
                            t.state = num(L"st", 2);
                            t.count = num(L"n", -1);
                            const Jv* vm = jst.Get(L"ms");
                            t.elapsedMs = (vm && vm->t == 2) ? (long long)vm->num : -1;
                            t.name = jst.S(L"name");
                            t.argz = jst.S(L"argz");
                            t.mode = jst.S(L"mode");
                            t.query = jst.S(L"query");
                            t.err = jst.S(L"err");
                            const Jv* jtp = jst.Get(L"top");
                            if (jtp && jtp->t == 4)
                                for (auto& jp : jtp->arr) if (jp.t == 3) t.top.push_back(jp.str);
                            const Jv* ja = jst.Get(L"adj");
                            if (ja && ja->t == 5) {
                                const Jv* jk = ja->Get(L"kind");
                                t.adj.kind = (jk && jk->t == 2) ? (int)jk->num : 0;
                                t.adj.win = ja->S(L"win");
                                const Jv* ji = ja->Get(L"items");
                                if (ji && ji->t == 4)
                                    for (auto& jai : ji->arr) {
                                        if (jai.t != 5) continue;
                                        AiAdjustItem it;
                                        it.key = jai.S(L"key");
                                        it.val = jai.S(L"val");
                                        it.json = U8(jai.S(L"json"));
                                        const Jv* js2 = jai.Get(L"st");
                                        it.state = (js2 && js2->t == 2) ? (int)js2->num : 0;
                                        it.err = jai.S(L"err");
                                        t.adj.items.push_back(std::move(it));
                                    }
                            }
                            if (t.state < 2) {   /* 存档时的在途步骤 = 进程已结束, 折算为已中止 */
                                t.state = 3;
                                if (t.err.empty()) t.err = L"已中止";
                            }
                            m.steps.push_back(t);
                        }
                    }
                    if (m.steps.empty()) continue;
                } else {
                    m.role = (jr && jr->num == 1) ? 1 : 0;
                    m.text = jmsg.S(L"text");
                    m.reason = jmsg.S(L"reason");
                    if (m.text.empty() && m.reason.empty()) continue;
                }
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
/* 当前会话落库 (curId=0 → 新建; 否则原位更新), 返回会话 id; 工具卡片组 (role==2) 一并落库 */
unsigned long long HistUpsert(unsigned long long curId, const std::vector<AiMsg>& msgs) {
    std::wstring firstUser;
    for (auto& m : msgs)
        if (m.role == 0 && !m.text.empty()) { firstUser = m.text; break; }
    if (firstUser.empty()) return curId;
    AiConv c;
    c.id = curId ? curId : g_nextConvId++;
    c.t = (long long)time(NULL);
    c.title = ConvTitleOf(firstUser);
    for (auto& m : msgs) {
        if (m.role == 2 && m.steps.empty()) continue;   /* 空工具组不落库 */
        c.msgs.push_back(m);
    }
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
Gdiplus::Color HexCol(const std::wstring& hex, int alpha) {   /* "#RRGGBB" */
    unsigned v = 0;
    for (size_t i = (hex.size() > 6 ? hex.size() - 6 : 0); i < hex.size(); i++) {
        wchar_t c = hex[i];
        v = v * 16 + (unsigned)((c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 0);
    }
    return Gdiplus::Color((BYTE)alpha, (BYTE)((v >> 16) & 255), (BYTE)((v >> 8) & 255), (BYTE)(v & 255));
}
Gdiplus::Color MixCol(Gdiplus::Color a, Gdiplus::Color b, float t) {   /* t=0 → a */
    auto ch = [&](BYTE x, BYTE y) { return (BYTE)(x * (1 - t) + y * t + 0.5f); };
    return Gdiplus::Color((BYTE)(a.GetA() * (1 - t) + b.GetA() * t + 0.5f),
                          ch(a.GetR(), b.GetR()), ch(a.GetG(), b.GetG()), ch(a.GetB(), b.GetB()));
}
Gdiplus::Color WithA(Gdiplus::Color c, BYTE a) { return Gdiplus::Color(a, c.GetR(), c.GetG(), c.GetB()); }
