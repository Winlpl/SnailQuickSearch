/*
 * ai_core.cpp — AI 助手基础设施: 编码转换 / base64 / 简易 JSON / 颜色几何 / 配置 / 多对话历史
 * 结构与接口见 ai_assistant.h。
 */
#include "ai_assistant.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;   /* 链接器提供: 取本 DLL 模块路径用 (临时诊断) */

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

/* base64 字节版 (read_image data URL 与 EncodedCommand 等共用; 文本版 B64Enc 供密钥加密) */
std::string AiB64Enc(const unsigned char* d, size_t n) {
    return B64Enc(std::string((const char*)d, n));
}

/* UTF-16 字节流 → UTF-8 (AiTextToUtf8 的 BOM 分支用)。
 * 不能走 MultiByteToWideChar(1200/1201) — 实测直接拒 (ERROR_INVALID_PARAMETER, CP1200
 * 不作源码页), 显式按字节拼 wide: LE 原生拷, BE 逐单元换序。 */
static std::string U16BytesToU8(const char* d, size_t n, UINT codePage) {
    if (n % 2) n -= 1;   /* 截齐偶数 (截断文件尾部半单元) */
    std::wstring w(n / 2, 0);
    if (n) {
        if (codePage == 1200)
            memcpy(&w[0], d, n);
        else
            for (size_t i = 0; i < n / 2; i++)
                w[i] = (wchar_t)(((unsigned char)d[2 * i + 1] << 8) | (unsigned char)d[2 * i]);
    }
    return U8(w);
}

/* ---- 文本 → UTF-8 (read_file / ai.read 共用): BOM 识别 → UTF-8 严格校验 → ANSI 回落 ----
 * 中文 Windows 的 ANSI 页是 GBK 系, 模型管线全程 UTF-8 — 不转就是乱码。
 * encName 回传识别结果 (工具结果里告知模型); 编码存疑时以输出可读为准, 不做二次猜测。 */
std::string AiTextToUtf8(const std::string& raw, std::wstring* encName) {
    if (encName) *encName = L"UTF-8";
    if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF &&
        (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF)
        return raw.substr(3);   /* UTF-8 BOM: 剥壳 */
    if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) {
        if (encName) *encName = L"UTF-16LE";
        return U16BytesToU8(raw.data() + 2, raw.size() - 2, 1200);
    }
    if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFE && (unsigned char)raw[1] == 0xFF) {
        if (encName) *encName = L"UTF-16BE";
        return U16BytesToU8(raw.data() + 2, raw.size() - 2, 1201);
    }
    int strict = raw.empty() ? 0 : MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                                       raw.data(), (int)raw.size(), NULL, 0);
    if (strict > 0) {   /* 合法 UTF-8: 原样 (recode 等价, 直取原文免一次转换) */
        if (encName) *encName = L"UTF-8";
        return raw;
    }
    if (encName) *encName = L"ANSI (本地编码, 已转 UTF-8)";
    int wl = raw.empty() ? 0 : MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)raw.size(), NULL, 0);
    std::wstring w(wl > 0 ? (size_t)wl : 0, 0);
    if (wl > 0) MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)raw.size(), &w[0], wl);
    return U8(w);
}

/* JSON 收口 (实现见 ai_assistant.h 声明处注释): 解析/序列化全走 picojson */
static void JvFromP(Jv& jv, const picojson::value& v) {   /* picojson 树 → Jv 宽字符视图 (机械转换) */
    if (v.is<bool>()) { jv.t = 1; jv.b = v.get<bool>(); }
    else if (v.is<double>()) { jv.t = 2; jv.num = v.get<double>(); }
    else if (v.is<std::string>()) { jv.t = 3; jv.str = W8(v.get<std::string>().c_str()); }
    else if (v.is<picojson::array>()) {
        jv.t = 4;
        for (auto& e : v.get<picojson::array>()) {
            Jv x;
            JvFromP(x, e);
            jv.arr.push_back(std::move(x));
        }
    } else if (v.is<picojson::object>()) {
        jv.t = 5;
        for (auto& kv : v.get<picojson::object>()) {
            Jv x;
            JvFromP(x, kv.second);
            jv.obj.push_back({ W8(kv.first.c_str()), std::move(x) });
        }
    } else {
        jv.t = 0;   /* null */
    }
}
Jv JsonParseW(const std::wstring& text) {
    Jv jv;
    picojson::value v;
    if (picojson::parse(v, U8(text)).empty()) JvFromP(jv, v);
    return jv;   /* 解析失败 = null 型 Jv (与旧行为一致) */
}
bool JParseU8(picojson::value& out, const std::string& u8) {
    out = picojson::value();
    return picojson::parse(out, u8).empty();
}
picojson::value JvToP(const Jv& v) {   /* Jv 树 → picojson (键/字符串转 UTF-8) */
    switch (v.t) {
        case 1: return picojson::value(v.b);
        case 2: return picojson::value(v.num);
        case 3: return JS(v.str);
        case 4: {
            picojson::array a;
            for (auto& e : v.arr) a.push_back(JvToP(e));
            return picojson::value(a);
        }
        case 5: {
            picojson::object o;
            for (auto& kv : v.obj) o[U8(kv.first)] = JvToP(kv.second);
            return picojson::value(o);
        }
        default: return picojson::value();   /* null */
    }
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
    std::wstring mk = MachineKeyStr();
    auto enc = [&](const std::wstring& k) -> picojson::value {   /* 密钥加密 (空 = 不落该字段) */
        std::string e = B64Enc(XorSecret(U8(k), U8(mk)));
        return picojson::value("enc:1:" + e);
    };
    picojson::object root;
    root["reasoning"] = JB(g_cfg.reasoning);
    root["filePolicy"] = JN(g_cfg.filePolicy);
    root["execPolicy"] = JN(g_cfg.execPolicy);
    root["syncResults"] = JB(g_cfg.syncResults);
    root["activeId"] = JS(g_cfg.activeId);
    picojson::array profs;
    for (const AiProfile& p : g_cfg.profiles) {
        picojson::object o;
        o["id"] = JS(p.id);
        o["name"] = JS(p.name);
        o["baseUrl"] = JS(p.baseUrl);
        o["model"] = JS(p.model);
        if (!p.apiKey.empty()) o["apiKeyEnc"] = enc(p.apiKey);
        o["ctx"] = JN(p.ctx);
        o["maxOut"] = JN(p.maxOut);
        o["img"] = JB(p.img);
        o["video"] = JB(p.video);
        o["audio"] = JB(p.audio);
        profs.push_back(picojson::value(o));
    }
    root["profiles"] = picojson::value(profs);
    std::string u8 = picojson::value(root).serialize();
    g_host->StorageSet(g_ctx, "cfg", u8.c_str(), (int)u8.size());
}
/* 密钥解密 ("enc:1:<b64>"; 换机解出乱码 = 视为未配置 — GUID 密码不出本机) */
static std::wstring DecryptKeyStr(const std::wstring& encv) {
    if (encv.rfind(L"enc:1:", 0) != 0) return L"";
    std::string raw = B64Dec(U8(encv.substr(6)));
    std::string plain = XorSecret(raw, U8(MachineKeyStr()));
    return ValidUtf8(plain) ? W8(plain.c_str()) : L"";
}
/* token 长度夹取 (前端已验格式; 这里只防越界值: 0=未指定, 上限 1e8 与前端同值) */
long long CfgClampTok(double v) {
    if (!(v > 0)) return 0;
    if (v > 100000000.0) v = 100000000.0;
    return (long long)v;
}
std::wstring CfgGenProfileId() {
    static volatile long s_seq = 0;
    wchar_t b[40];
    swprintf(b, 40, L"p%llx%lx", (unsigned long long)(GetTickCount64() & 0xFFFFFFFFFFFFULL),
             (unsigned long)InterlockedIncrement(&s_seq));
    return b;
}
AiProfile* CfgActive() {
    for (auto& p : g_cfg.profiles)
        if (p.id == g_cfg.activeId) return &p;
    return g_cfg.profiles.empty() ? NULL : &g_cfg.profiles[0];
}
std::wstring CfgDisplayName(const AiProfile* p) {
    if (!p) return L"未配置接口";
    if (!p->name.empty()) return p->name;
    if (!p->model.empty()) return p->model;
    return L"未命名模型";
}
/* 活动档案 → 派生镜像。activeId 失效回落第一条 (与前端 activeAiProfile 同规);
   空表 = 镜像全空 (请求侧按"未配置"拒绝)。切档/保存/删除后必须调。 */
void CfgApplyActive() {
    AiProfile* p = CfgActive();
    if (p) {
        g_cfg.activeId = p->id;
        g_cfg.baseUrl = p->baseUrl;
        g_cfg.model = p->model;
        g_cfg.apiKey = p->apiKey;
        g_cfg.ctx = p->ctx;
        g_cfg.maxOut = p->maxOut;
        g_cfg.img = p->img;
        g_cfg.video = p->video;
        g_cfg.audio = p->audio;
    } else {
        g_cfg.activeId.clear();
        g_cfg.baseUrl.clear();
        g_cfg.model.clear();
        g_cfg.apiKey.clear();
        g_cfg.ctx = 0;
        g_cfg.maxOut = 0;
        g_cfg.img = false;
        g_cfg.video = false;
        g_cfg.audio = false;
    }
}
void CfgLoad() {
    if (!g_host) return;
    /* 档案表可超旧的单配置几百字节: 两步读 (先查长度再取) 不设固定上限 */
    int n = g_host->StorageGet(g_ctx, "cfg", NULL, 0);
    bool firstRun = (n <= 0);
    std::wstring text;
    if (!firstRun) {
        std::string u8((size_t)n + 1, '\0');
        n = g_host->StorageGet(g_ctx, "cfg", &u8[0], n);
        if (n > 0) text = W8(u8.c_str());
    }
    if (!text.empty()) {
        Jv v = JsonParseW(text);
        if (v.t == 5) {
            const Jv* r = v.Get(L"reasoning");
            if (r && r->t == 1) g_cfg.reasoning = r->b;
            const Jv* fp = v.Get(L"filePolicy");
            if (fp && fp->t == 2 && fp->num >= 0 && fp->num <= 3) g_cfg.filePolicy = (int)fp->num;
            /* 命令执行权限只有 0/2/3 三档 (没有只读); 存量里出现 1 一律按询问处理 */
            const Jv* ep = v.Get(L"execPolicy");
            if (ep && ep->t == 2 && ep->num >= 0 && ep->num <= 3)
                g_cfg.execPolicy = (ep->num == 1) ? 2 : (int)ep->num;
            const Jv* sy = v.Get(L"syncResults");
            if (sy && sy->t == 1) g_cfg.syncResults = sy->b;
            const Jv* av = v.Get(L"activeId");
            if (av && av->t == 3) g_cfg.activeId = av->str;
            const Jv* ps = v.Get(L"profiles");
            if (ps && ps->t == 4) {
                for (auto& pv : ps->arr) {
                    if (pv.t != 5) continue;
                    AiProfile p;
                    p.id = pv.S(L"id");
                    if (p.id.empty()) continue;   /* 无 id 不可寻址, 丢弃 */
                    p.name = pv.S(L"name");
                    p.baseUrl = pv.S(L"baseUrl");
                    p.model = pv.S(L"model");
                    const Jv* cv = pv.Get(L"ctx");
                    if (cv && cv->t == 2) p.ctx = CfgClampTok(cv->num);
                    const Jv* mv = pv.Get(L"maxOut");
                    if (mv && mv->t == 2) p.maxOut = CfgClampTok(mv->num);
                    const Jv* iv = pv.Get(L"img");
                    if (iv && iv->t == 1) p.img = iv->b;
                    const Jv* vd = pv.Get(L"video");
                    if (vd && vd->t == 1) p.video = vd->b;
                    const Jv* au = pv.Get(L"audio");
                    if (au && au->t == 1) p.audio = au->b;
                    p.apiKey = DecryptKeyStr(pv.S(L"apiKeyEnc"));
                    g_cfg.profiles.push_back(std::move(p));
                }
            }
            /* 旧格式迁移: 单配置字段收编为第一条档案 (一次性, 立即按新格式落盘) */
            if (g_cfg.profiles.empty() && (!v.S(L"baseUrl").empty() || !v.S(L"model").empty())) {
                AiProfile p;
                p.id = CfgGenProfileId();
                p.baseUrl = v.S(L"baseUrl");
                p.model = v.S(L"model");
                p.apiKey = DecryptKeyStr(v.S(L"apiKeyEnc"));
                g_cfg.profiles.push_back(std::move(p));
                g_cfg.activeId = g_cfg.profiles[0].id;
                firstRun = true;   /* 借下面同一出口触发落盘 */
            }
        }
    } else if (firstRun) {
        /* 首次运行: 播一条默认档案 (面板预填地址与模型, 用户只差密钥) */
        AiProfile p;
        p.id = CfgGenProfileId();
        p.baseUrl = g_cfg.baseUrl;   /* 结构体默认值 = DeepSeek 官方 */
        p.model = g_cfg.model;
        g_cfg.profiles.push_back(std::move(p));
        g_cfg.activeId = g_cfg.profiles[0].id;
    }
    if (g_cfg.profiles.empty()) g_cfg.activeId.clear();
    CfgApplyActive();
    if (firstRun) CfgSave();
}

/* ==================== 多对话历史 (按会话拆分: 索引键 "历史索引" + 每会话一个正文文件
 * "会话-<id>.json"; 上限 30 会话/每会话 200 条。索引只存元数据 [{"会话标题","文件名","id","时间"}],
 * 打开面板零消息体载入; 会话正文在 load/落库时按需读写 — 旧版"所有会话堆一个文件"废弃,
 * 旧全量键 "历史" 在索引缺失时一次性搬入分文件+索引后删除) ==================== */

std::vector<AiConvRef> g_hist;
static unsigned long long g_nextConvId = 1;

static std::wstring HistConvFile(unsigned long long id) {   /* 会话正文文件名 (索引."文件名" 同源) */
    return L"会话-" + std::to_wstring(id) + L".json";
}
static std::wstring ConvTitleOf(const std::wstring& firstUser) {
    std::wstring t = TrimW(firstUser);
    if (t.size() > 30) { t = t.substr(0, 30); t += L"…"; }
    return t;
}

/* ---- 会话正文 ↔ JSON (分会话文件与旧全量迁移共用; 写侧 picojson, 读侧 Jv) ---- */
static void HistConvToJson(const AiConv& c, picojson::object& oc) {
    picojson::array msgs;
    for (const AiMsg& m : c.msgs) {
        picojson::object om;
        if (m.role == 2) {
            /* 工具卡片组: query/err 截到 512 字符 (绘制端同款上限, 存整段脚本无展示出口) */
            om["r"] = JN(2);
            picojson::array steps;
            for (const AiToolStep& t : m.steps) {
                picojson::object os;
                os["k"] = JN(t.kind);
                os["st"] = JN(t.state);
                os["n"] = JN(t.count);
                os["ms"] = JN(t.elapsedMs);
                os["name"] = JS(t.name);
                if (!t.argz.empty()) os["argz"] = JS(t.argz);
                if (!t.mode.empty()) os["mode"] = JS(t.mode);
                if (!t.query.empty()) os["query"] = JS(t.query.substr(0, 512));
                if (!t.err.empty()) os["err"] = JS(t.err.substr(0, 512));
                if (!t.adj.items.empty()) {
                    /* 待应用的调整 (含逐项状态): 落库后重开会话卡片仍可应用/忽略 */
                    picojson::object oa;
                    oa["kind"] = JN(t.adj.kind);
                    oa["win"] = JS(t.adj.win);
                    picojson::array items;
                    for (const AiAdjustItem& it : t.adj.items) {
                        picojson::object oi;
                        oi["key"] = JS(it.key);
                        oi["val"] = JS(it.val);
                        oi["json"] = JS(W8(it.json.c_str()));
                        oi["st"] = JN(it.state);
                        if (!it.err.empty()) oi["err"] = JS(it.err.substr(0, 200));
                        items.push_back(picojson::value(oi));
                    }
                    oa["items"] = picojson::value(items);
                    os["adj"] = picojson::value(oa);
                }
                    if (!t.top.empty()) {
                        picojson::array top;
                        for (const std::wstring& p : t.top) top.push_back(JS(p));
                        os["top"] = picojson::value(top);
                    }
                    if (!t.wrote.empty()) {   /* 脚本导出的文件 (会话结束后仍可见/可点) */
                        picojson::array wf;
                        for (const std::wstring& p : t.wrote) wf.push_back(JS(p));
                        os["w"] = picojson::value(wf);
                    }
                    if (!t.chg.empty()) {   /* file_op 逐项更改记录 ([动作,源,目标] 紧凑数组, 失败项
                                             追加 [0,原因]; 会话重开后更改块仍可见, 回答引用改后路径仍可点) */
                        picojson::array ca;
                        for (const AiFileChange& c : t.chg) {
                            picojson::array e;
                            e.push_back(JN(c.act));
                            e.push_back(JS(c.from));
                            e.push_back(JS(c.to));
                            if (!c.ok) {
                                e.push_back(JN(0));
                                e.push_back(JS(c.err));
                            }
                            ca.push_back(picojson::value(e));
                        }
                        os["chg"] = picojson::value(ca);
                    }
                steps.push_back(picojson::value(os));
            }
            om["steps"] = picojson::value(steps);
            msgs.push_back(picojson::value(om));
            continue;
        }
        om["r"] = JN(m.role);
        om["text"] = JS(m.text);
        if (!m.atts.empty()) {
            /* 多模态附件 (dataUrl 可能已被 HistUpsert 的存储预算清空 = 占位, 照存) */
            picojson::array atts;
            for (const AiAttach& a : m.atts) {
                picojson::object oa;
                oa["k"] = JN(a.kind);
                oa["n"] = JS(a.name);
                oa["u"] = JS(a.dataUrl);
                atts.push_back(picojson::value(oa));
            }
            om["att"] = picojson::value(atts);
        }
        if (!m.reason.empty()) om["reason"] = JS(m.reason);
        msgs.push_back(picojson::value(om));
    }
    oc["id"] = JN((long long)c.id);
    oc["t"] = JN(c.t);
    oc["title"] = JS(c.title);
    oc["msgs"] = picojson::value(msgs);
}
static bool HistConvFromJson(const Jv& jc, AiConv& c) {
    if (jc.t != 5) return false;
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
                        const Jv* jw = jst.Get(L"w");
                        if (jw && jw->t == 4)
                            for (auto& jw2 : jw->arr) if (jw2.t == 3) t.wrote.push_back(jw2.str);
                        const Jv* jcg = jst.Get(L"chg");
                        if (jcg && jcg->t == 4)
                            for (auto& je : jcg->arr) {
                                if (je.t != 4 || je.arr.size() < 3) continue;
                                AiFileChange c;
                                c.act = (je.arr[0].t == 2) ? (int)je.arr[0].num : 0;
                                if (je.arr[1].t == 3) c.from = je.arr[1].str;
                                if (je.arr[2].t == 3) c.to = je.arr[2].str;
                                if (je.arr.size() >= 4 && je.arr[3].t == 2 && je.arr[3].num == 0) {
                                    c.ok = false;   /* 失败项 ([动作,源,目标,0,原因]; 旧存档 3 元 = 成功) */
                                    if (je.arr.size() >= 5 && je.arr[4].t == 3) c.err = je.arr[4].str;
                                }
                                t.chg.push_back(std::move(c));
                            }
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
                const Jv* ja2 = jmsg.Get(L"att");
                if (ja2 && ja2->t == 4) {
                    for (auto& jatt : ja2->arr) {
                        if (jatt.t != 5) continue;
                        AiAttach a;
                        const Jv* jk = jatt.Get(L"k");
                        a.kind = (jk && jk->t == 2) ? (int)jk->num : 0;
                        if (a.kind < 0 || a.kind > 2) a.kind = 0;
                        a.name = jatt.S(L"n");
                        a.dataUrl = jatt.S(L"u");
                        m.atts.push_back(std::move(a));
                    }
                }
                if (m.text.empty() && m.reason.empty() && m.atts.empty()) continue;
            }
            if (c.msgs.size() < AI_MSG_MAX) c.msgs.push_back(m);
        }
    }
    return !c.msgs.empty();
}
/* 索引落盘 ([{"会话标题","文件名","id","时间"}]) — 只有元数据, 恒为几 KB */
static void HistIdxSave() {
    if (!g_host) return;
    picojson::array arr;
    for (const AiConvRef& r : g_hist) {
        picojson::object o;
        o["会话标题"] = JS(r.title);
        o["文件名"] = JS(r.file);
        o["id"] = JN((long long)r.id);
        o["时间"] = JN(r.t);
        arr.push_back(picojson::value(o));
    }
    std::string u8 = picojson::value(arr).serialize();
    g_host->StorageSet(g_ctx, "历史索引", u8.c_str(), (int)u8.size());
}

/* 按需读取一个会话正文 (侧栏 load 的唯一入口): 此刻才从该会话自己的文件读盘解析。
 * file 取索引条目, 条目缺席时按 id 推导; 文件缺失/解析失败/无消息 = false。 */
bool HistGet(unsigned long long id, AiConv& out) {
    out = AiConv();
    if (!g_host || !id) return false;
    std::wstring file;
    for (const AiConvRef& r : g_hist)
        if (r.id == id) { file = r.file; break; }
    if (file.empty()) file = HistConvFile(id);
    /* 两步读 (先查长度再取): 附件随会话落库后单会话文件可达几十 MB,
       固定上限缓冲读截断 = JSON 解析失败 = 该会话打不开 (CfgLoad 同口径) */
    int n = g_host->StorageGet(g_ctx, U8(file).c_str(), NULL, 0);
    if (n <= 0) return false;
    std::string buf((size_t)n + 1, '\0');
    n = g_host->StorageGet(g_ctx, U8(file).c_str(), &buf[0], n);
    if (n <= 0) return false;
    buf.resize((size_t)n);
    buf.push_back(0);
    Jv v = JsonParseW(W8(buf.c_str()));
    if (v.t != 5 || !HistConvFromJson(v, out)) return false;
    out.id = id;   /* 身份以索引/请求方为准 (文件自带 id 仅作展示冗余) */
    return true;
}

/* 旧版全量键 "历史" (所有会话堆一个 JSON 数组) → 一次性搬成 每会话文件 + 索引, 搬完删旧键。
 * 只在索引缺失/为空时走一次; 旧键不在 = 首次运行, 什么都不做。 */
static void MigrateHistFromLegacy() {
    int n = g_host->StorageGet(g_ctx, "历史", NULL, 0);
    if (n <= 0) return;
    std::string buf((size_t)n + 1, '\0');
    n = g_host->StorageGet(g_ctx, "历史", &buf[0], n);
    if (n <= 0) return;
    buf.resize((size_t)n);
    buf.push_back(0);
    Jv v = JsonParseW(W8(buf.c_str()));
    if (v.t == 4) {
        for (auto& jc : v.arr) {
            AiConv c;
            if (!HistConvFromJson(jc, c) || !c.id) continue;
            picojson::object oc;
            HistConvToJson(c, oc);
            std::string u8 = picojson::value(oc).serialize();
            g_host->StorageSet(g_ctx, U8(HistConvFile(c.id)).c_str(), u8.c_str(), (int)u8.size());
            AiConvRef r;
            r.id = c.id;
            r.t = c.t;
            r.title = c.title;
            r.file = HistConvFile(c.id);
            g_hist.push_back(std::move(r));
        }
        HistIdxSave();
    }
    g_host->StorageRemove(g_ctx, "历史");   /* 解析失败也删: 半截数据救不回来, 留着只会反复重迁 */
}
void HistLoad() {
    g_hist.clear();
    if (!g_host) return;
    /* 只读索引 (元数据, 恒几 KB): 会话正文留在各自文件里, load 时 HistGet 按需取 */
    int n = g_host->StorageGet(g_ctx, "历史索引", NULL, 0);
    if (n > 0) {
        std::string buf((size_t)n + 1, '\0');
        n = g_host->StorageGet(g_ctx, "历史索引", &buf[0], n);
        if (n > 0) {
            buf.resize((size_t)n);
            buf.push_back(0);
            Jv v = JsonParseW(W8(buf.c_str()));
            if (v.t == 4) {
                for (auto& je : v.arr) {
                    if (je.t != 5) continue;
                    AiConvRef r;
                    const Jv* ji = je.Get(L"id");
                    r.id = (ji && ji->t == 2) ? (unsigned long long)ji->num
                          : (unsigned long long)wcstoull(je.S(L"id").c_str(), NULL, 10);
                    if (!r.id) continue;   /* 无 id 不可寻址, 丢弃 */
                    r.file = je.S(L"文件名");
                    if (r.file.empty()) r.file = HistConvFile(r.id);   /* 旧索引缺文件名 = 推导兜底 */
                    r.title = je.S(L"会话标题");
                    const Jv* jt = je.Get(L"时间");
                    if (jt && jt->t == 2) r.t = (long long)jt->num;
                    bool dup = false;   /* 同 id 重复条目取先出现的 (后写覆盖前写为 upsert 语义) */
                    for (const AiConvRef& x : g_hist)
                        if (x.id == r.id) { dup = true; break; }
                    if (!dup) g_hist.push_back(std::move(r));
                }
            }
        }
    }
    if (g_hist.empty()) MigrateHistFromLegacy();   /* 索引缺席 = 首次运行或旧版数据, 一次性搬入 */
    while (g_hist.size() > AI_CONV_MAX) {          /* 最旧丢弃 (索引层兜底; 正常落盘时已裁) */
        if (!g_hist.front().file.empty())
            g_host->StorageRemove(g_ctx, U8(g_hist.front().file).c_str());
        g_hist.erase(g_hist.begin());
    }
    for (const AiConvRef& r : g_hist)
        if (r.id >= g_nextConvId) g_nextConvId = r.id + 1;
}
/* 当前会话落盘 (curId=0 → 新建; 否则原位更新), 返回会话 id; 工具卡片组 (role==2) 一并落库。
 * 落盘 = 会话正文写自己的文件 (HistConvFile) + 索引条目原位更新/追加 (HistIdxSave)。
 * 存储预算: 会话内全部附件 dataUrl 总量超 AI_ATT_HIST_BUDGET 时, 最旧的先清成占位
 * (kind/name 留着渲染"已清理", dataUrl 清空 = 不再上请求) — 防大视频把存储值撑失控。 */
unsigned long long HistUpsert(unsigned long long curId, const std::vector<AiMsg>& msgs) {
    std::wstring firstUser;
    for (auto& m : msgs)
        if (m.role == 0 && !m.text.empty()) { firstUser = m.text; break; }
    if (firstUser.empty()) {
        /* 纯附件消息 (无文字): 标题按附件类型兜底, 否则 HistUpsert 早退 = 不落库 */
        for (auto& m : msgs) {
            if (m.role != 0 || m.atts.empty()) continue;
            static const wchar_t* KIND_NAME[3] = { L"图片", L"视频", L"音频" };
            firstUser = L"[" + std::wstring(KIND_NAME[m.atts.front().kind % 3]) + L"]";
            if (m.atts.size() > 1) firstUser += L"×" + std::to_wstring(m.atts.size());
            break;
        }
    }
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
    size_t budget = AI_ATT_HIST_BUDGET;
    for (auto& m : c.msgs) {   /* 从最旧往新扫, 超预算的部分清 dataUrl */
        if (m.role != 0 || m.atts.empty()) continue;
        for (auto& a : m.atts) {
            size_t sz = a.dataUrl.size();
            if (budget >= sz) { budget -= sz; continue; }
            a.dataUrl.clear();   /* 占位: 渲染"已清理", 请求侧跳过 */
            budget = 0;
        }
    }
    /* 会话正文写自己的文件 (只动这一个会话, 别的会话文件不重写) */
    if (g_host) {
        picojson::object oc;
        HistConvToJson(c, oc);
        std::string u8 = picojson::value(oc).serialize();
        g_host->StorageSet(g_ctx, U8(HistConvFile(c.id)).c_str(), u8.c_str(), (int)u8.size());
    }
    for (size_t i = 0; i < g_hist.size(); i++) {
        if (g_hist[i].id != c.id) continue;
        g_hist[i].t = c.t;   /* 索引原位更新 (正文已在上面落盘) */
        g_hist[i].title = c.title;
        HistIdxSave();
        return c.id;
    }
    AiConvRef r;
    r.id = c.id;
    r.t = c.t;
    r.title = c.title;
    r.file = HistConvFile(c.id);
    g_hist.push_back(std::move(r));
    while (g_hist.size() > AI_CONV_MAX) {   /* 挤出最旧 = 连它的正文文件一起删 */
        if (g_host && !g_hist.front().file.empty())
            g_host->StorageRemove(g_ctx, U8(g_hist.front().file).c_str());
        g_hist.erase(g_hist.begin());
    }
    HistIdxSave();
    return c.id;
}

/* 删除会话 (侧栏删除 / 手动删光当前问答): 正文文件 + 索引条目一并移除 */
void HistRemove(unsigned long long id) {
    for (size_t i = 0; i < g_hist.size(); i++) {
        if (g_hist[i].id != id) continue;
        if (g_host && !g_hist[i].file.empty())
            g_host->StorageRemove(g_ctx, U8(g_hist[i].file).c_str());
        g_hist.erase(g_hist.begin() + i);
        HistIdxSave();
        return;
    }
}

/* 清空历史: 每个会话的正文文件 + 索引一并清掉 */
void HistClearAll() {
    if (g_host)
        for (const AiConvRef& r : g_hist)
            if (!r.file.empty()) g_host->StorageRemove(g_ctx, U8(r.file).c_str());
    g_hist.clear();
    HistIdxSave();
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
