/*
 * ai_net.cpp — 联网访问层 (web_search / fetch_url 两工具实体)
 * ============================================================================
 * web_search = 搜索引擎结果清单, 三级回退 (全部实测 2026-09-26):
 *   1) bing 结果 RSS (search?q=..&format=rss) — item/title/link/description, 最稳;
 *   2) bing 结果页 HTML 的 b_algo 块 (RSS 失效/改版时);
 *   3) DuckDuckGo Lite 的 result-link 行 (国内网络不可达, 供海外环境兜底)。
 *   跳转包装链接统一还原 (bing/ck 的 u=a1<base64url>、ddg 的 uddg=<百分号编码>)。
 * fetch_url  = 抓一个 http(s) 网页 → AiTextToUtf8 转码 → 二进制占比判定 →
 *   WebHtmlToText 提正文 → 头 80%+尾 20% 封顶 (AiCapUtf8HeadTail, 与 read_file 同上限)。
 * 约束:
 *   - 全部在 agent 工作线程调用; 只带 AiJob* 做「停止」(j->hReq 登记 = UI 并发关句柄
 *     打断阻塞读, 与 SSE 主请求同口径), 不碰引擎/宿主/UI。
 *   - 纯解析函数 (WebParse 系列/WebHtmlToText/WebUnwrapResultUrl) 不带网络,
 *     供 test\test_ai_net.cpp 直测。
 *   - 工具结果遵守"省略数恒给精确值"口径; 查询词会发给第三方搜索引擎,
 *     提示词与工具 description 都已声明 (涉及隐私先问过用户)。
 */
#include "ai_assistant.h"
#include <string.h>
#include <ctype.h>

/* ==================== URL 基元 ==================== */

/* UTF-8 百分号编码 (RFC3986 unreserved 之外全部转义; 搜索词进查询参数用) */
static std::string UrlEncodeU8(const std::string& s8) {
    static const char* HEX = "0123456789ABCDEF";
    std::string o;
    o.reserve(s8.size() * 3);
    for (unsigned char c : s8) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            o += (char)c;
        } else {
            o += '%';
            o += HEX[c >> 4];
            o += HEX[c & 15];
        }
    }
    return o;
}

/* 百分号解码 (ddg uddg= 参数还原; '+' 视作空格的表单口径一并接受) */
static std::wstring UrlDecodeW(const std::wstring& s) {
    auto hexv = [](wchar_t c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string o8;
    o8.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'%' && i + 2 < s.size()) {
            int h = hexv(s[i + 1]), l = hexv(s[i + 2]);
            if (h >= 0 && l >= 0) { o8 += (char)(h * 16 + l); i += 2; continue; }
        }
        if (s[i] == L'+') { o8 += ' '; continue; }
        o8 += (char)(s[i] & 0xFF);
    }
    return W8(o8.c_str());
}

/* base64url 解码 (bing/ck 载荷; 非法字符跳过, 流式凑位不需要 padding) */
static std::string B64UrlDec(const std::string& in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };
    std::string o;
    int buf = 0, bits = 0;
    for (char c : in) {
        int v = val(c);
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) { bits -= 8; o += (char)((buf >> bits) & 0xFF); }
    }
    return o;
}

/* ==================== HTML/文本基元 (纯解析, 可单测) ==================== */

static size_t FindI(const std::string& s, const char* nd, size_t from) {
    size_t n = strlen(nd);
    if (n == 0 || s.size() < n) return std::string::npos;
    for (size_t i = from; i + n <= s.size(); i++) {
        size_t k = 0;
        while (k < n && tolower((unsigned char)s[i + k]) == tolower((unsigned char)nd[k])) k++;
        if (k == n) return i;
    }
    return std::string::npos;
}
static bool StartsWithI(const std::string& s, const char* nd, size_t at) {
    size_t n = strlen(nd);
    if (at + n > s.size()) return false;
    for (size_t k = 0; k < n; k++)
        if (tolower((unsigned char)s[at + k]) != tolower((unsigned char)nd[k])) return false;
    return true;
}

static void Utf8Append(unsigned cp, std::string* out) {
    if (cp < 0x80) { *out += (char)cp; return; }
    if (cp < 0x800) { *out += (char)(0xC0 | (cp >> 6)); *out += (char)(0x80 | (cp & 63)); return; }
    if (cp < 0x10000) {
        *out += (char)(0xE0 | (cp >> 12));
        *out += (char)(0x80 | ((cp >> 6) & 63));
        *out += (char)(0x80 | (cp & 63));
        return;
    }
    *out += (char)(0xF0 | (cp >> 18));
    *out += (char)(0x80 | ((cp >> 12) & 63));
    *out += (char)(0x80 | ((cp >> 6) & 63));
    *out += (char)(0x80 | (cp & 63));
}

/* '&' 起始的字符实体 (命名常用表 + &#NNN; / &#xHH;)。成功 = 追加 UTF-8 并返回 ';'
 * 之后的下一个位置; 不识别 = 返回 0 (调用方原样输出 '&') */
static size_t DecEntity(const std::string& s, size_t at, std::string* out) {
    size_t e = s.find(';', at);
    if (e == std::string::npos || e == at + 1 || e - at > 11) return 0;
    std::string name = s.substr(at + 1, e - at - 1);
    for (auto& c : name) c = (char)tolower((unsigned char)c);
    static const struct { const char* n; const char* u8; } NAMED[] = {
        { "amp", "&" },     { "lt", "<" },      { "gt", ">" },      { "quot", "\"" },
        { "apos", "'" },    { "nbsp", " " },    { "hellip", "…" },
        { "ensp", " " },    { "emsp", " " },    { "thinsp", " " },
        { "mdash", "—" },   { "ndash", "–" },   { "ldquo", "“" },   { "rdquo", "”" },
        { "lsquo", "‘" },   { "rsquo", "’" },   { "copy", "©" },    { "reg", "®" },
        { "trade", "™" },   { "middot", "·" },  { "laquo", "«" },   { "raquo", "»" },
        { "deg", "°" },     { "times", "×" },   { "divide", "÷" },
    };
    for (const auto& en : NAMED)
        if (name == en.n) { *out += en.u8; return e + 1; }
    unsigned cp = 0;
    if (name[0] == '#') {
        const char* d = name.c_str() + 1;
        if (*d == 'x' || *d == 'X') {
            for (d++; *d; d++) {
                int v = (*d >= '0' && *d <= '9') ? *d - '0'
                      : (*d >= 'a' && *d <= 'f') ? *d - 'a' + 10
                      : (*d >= 'A' && *d <= 'F') ? *d - 'A' + 10 : -1;
                if (v < 0) return 0;
                cp = cp * 16 + (unsigned)v;
            }
        } else {
            for (; *d; d++) {
                if (*d < '0' || *d > '9') return 0;
                cp = cp * 10 + (unsigned)(*d - '0');
            }
        }
        if (cp > 0 && cp <= 0x10FFFF) { Utf8Append(cp, out); return e + 1; }
    }
    return 0;
}

/* 块级闭合标签 → 结构字符 (换行/制表), 其余闭合不产生结构 */
static char BlockCloserChar(const std::string& tag) {
    if (tag == "td" || tag == "th") return '\t';
    if (tag == "p" || tag == "div" || tag == "li" || tag == "tr" || tag == "table" ||
        tag == "ul" || tag == "ol" || tag == "dl" || tag == "dt" || tag == "dd" ||
        tag == "pre" || tag == "blockquote" || tag == "form" || tag == "section" ||
        tag == "article" || tag == "header" || tag == "footer" || tag == "nav" ||
        tag == "main" || tag == "aside" || tag == "h1" || tag == "h2" || tag == "h3" ||
        tag == "h4" || tag == "h5" || tag == "h6" || tag == "figure" || tag == "figcaption")
        return '\n';
    return 0;
}

/* 结构换行: 先收掉尾部悬挂的制表/空格 (</td></tr> 连写只留一个换行) */
static void EmitNewline(std::string* out) {
    while (!out->empty() && (out->back() == '\t' || out->back() == ' ')) out->pop_back();
    *out += '\n';
}

/* 标签剥离状态机: [from,to) 的可见文本 → out (UTF-8)。
 * blockMode=0: 标签边界折叠成空格 (片段: 标题/摘要);  =1: 块级边界换行、单元格制表 (整页正文)。
 * 注释/脚本/样式/模板/svg 整块剔除; 原始 UTF-8 的 nbsp (C2 A0) 与普通空白一并折叠。 */
static void StripTags(const std::string& s, size_t from, size_t to, int blockMode, std::string* out) {
    bool ws = false;   /* 折叠中的空白 (输出推迟到下一个非空白字节, 免行首空白) */
    auto pushWs = [&]() {
        if (ws && !out->empty() && out->back() != '\n' && out->back() != '\t') *out += ' ';
        ws = false;
    };
    for (size_t i = from; i < to; ) {
        if (s[i] == '<') {
            if (StartsWithI(s, "<!--", i)) {
                size_t e = s.find("-->", i + 4);
                i = (e == std::string::npos || e > to) ? to : e + 3;
                continue;
            }
            if (StartsWithI(s, "<![CDATA[", i)) {
                /* CDATA 内容 = 字面文本: 原样保留 (不解析实体), 只剥首尾标记 */
                size_t e = s.find("]]>", i + 9);
                size_t bodyEnd = (e == std::string::npos || e > to) ? to : e;
                out->append(s, i + 9, bodyEnd - (i + 9));
                i = (e == std::string::npos || e > to) ? to : e + 3;
                continue;
            }
            if (StartsWithI(s, "<script", i) || StartsWithI(s, "<style", i) ||
                StartsWithI(s, "<noscript", i) || StartsWithI(s, "<template", i) ||
                StartsWithI(s, "<svg", i)) {
                size_t nameEnd = s.find_first_of(" \t\r\n>/", i + 2);
                std::string tag = s.substr(i + 1, (nameEnd == std::string::npos ? to : nameEnd) - (i + 1));
                for (auto& c : tag) c = (char)tolower((unsigned char)c);
                size_t e = FindI(s, ("</" + tag).c_str(), i + 2);
                if (e == std::string::npos) { i = to; continue; }
                size_t eEnd = s.find('>', e);
                i = (eEnd == std::string::npos || eEnd > to) ? to : eEnd + 1;
                continue;
            }
            size_t e = s.find('>', i);
            if (e == std::string::npos || e >= to) break;
            std::string tagn = s.substr(i + 1, e - i - 1);
            for (auto& c : tagn) c = (char)tolower((unsigned char)c);
            if (blockMode) {
                if (tagn == "br" || tagn == "br/") {
                    EmitNewline(out);
                } else if (!tagn.empty() && tagn[0] == '/') {
                    char bc = BlockCloserChar(tagn.substr(1));
                    if (bc == '\n') EmitNewline(out);
                    else if (bc == '\t') { pushWs(); *out += '\t'; }
                }
            } else if (!tagn.empty() && tagn[0] == '/' && BlockCloserChar(tagn.substr(1))) {
                ws = true;   /* 片段模式: 块级闭合当空白折叠 */
            }
            i = e + 1;
            continue;
        }
        if (s[i] == '&') {
            pushWs();   /* 实体前悬挂的空白先落地 ("结果 &amp; 标题" 的空格不能被吃掉) */
            size_t consumed = DecEntity(s, i, out);
            if (consumed) { i = consumed; continue; }
            *out += '&';
            i++;
            continue;
        }
        unsigned char c = (unsigned char)s[i];
        if (c == 0xC2 && i + 1 < to && (unsigned char)s[i + 1] == 0xA0) { ws = true; i += 2; continue; }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f') { ws = true; i++; continue; }
        pushWs();
        *out += (char)c;
        i++;
    }
}

/* [from,to) 字节范围里的可见文本 (去标签+实体解码+空白折叠) → 宽字符 */
static std::wstring FragText(const std::string& s, size_t from, size_t to) {
    std::string o;
    StripTags(s, from, to, 0, &o);
    std::wstring w = W8(o.c_str());
    return TrimW(w);
}

/* 开标签 [tagFrom, tagTo) 里取属性值 (大小写不敏感; 引号/裸值两种写法) */
static bool TagAttr(const std::string& s, size_t tagFrom, size_t tagTo, const char* attr, std::string* val) {
    size_t alen = strlen(attr);
    size_t p = tagFrom;
    for (;;) {
        p = FindI(s, attr, p);
        if (p == std::string::npos || p >= tagTo) return false;
        /* 属性名边界: 前一个字符须是空白或 '<' (防 data-href 之类误配) */
        if (p > tagFrom && s[p - 1] != ' ' && s[p - 1] != '<' && s[p - 1] != '\t' &&
            s[p - 1] != '\r' && s[p - 1] != '\n') {
            p += alen;
            continue;
        }
        size_t q = p + alen;
        while (q < tagTo && (s[q] == ' ' || s[q] == '\t' || s[q] == '\r' || s[q] == '\n')) q++;
        if (q >= tagTo || s[q] != '=') { p = q; continue; }
        q++;
        while (q < tagTo && (s[q] == ' ' || s[q] == '\t' || s[q] == '\r' || s[q] == '\n')) q++;
        if (q < tagTo && (s[q] == '"' || s[q] == '\'')) {
            char cq = s[q];
            size_t e = s.find(cq, q + 1);
            if (e == std::string::npos || e > tagTo) return false;
            *val = s.substr(q + 1, e - q - 1);
            return true;
        }
        size_t e = q;
        while (e < tagTo && s[e] != ' ' && s[e] != '>' && s[e] != '\t' && s[e] != '\r' && s[e] != '\n') e++;
        *val = s.substr(q, e - q);
        return true;
    }
}

/* 属性值里的字符实体解码 (href 源码里的 &amp; 还原成 '&', 否则还原出的链接点不开) */
static std::string DecAttrEntities(const std::string& v) {
    std::string o;
    o.reserve(v.size());
    size_t i = 0;
    while (i < v.size()) {
        if (v[i] == '&') {
            size_t c = DecEntity(v, i, &o);
            if (c) { i = c; continue; }
        }
        o += v[i];
        i++;
    }
    return o;
}

/* 搜索结果跳转链接 → 真实 URL */
std::wstring WebUnwrapResultUrl(const std::wstring& u) {
    std::wstring s = TrimW(u);
    if (s.rfind(L"//", 0) == 0) s = L"https:" + s;   /* 协议相对 (ddg 常见写法) */
    /* bing 点击跳转: /ck/a?...&u=a1<base64url(真实 URL)> — "a1" 之后才是载荷 */
    size_t ck = s.find(L"/ck/a?");
    if (ck != std::wstring::npos) {
        size_t up = s.find(L"u=a1", ck);
        if (up != std::wstring::npos) {
            std::string payload = U8(s.substr(up + 4));
            size_t amp = payload.find('&');
            if (amp != std::string::npos) payload.resize(amp);
            std::wstring real = W8(B64UrlDec(payload).c_str());
            if (real.rfind(L"http://", 0) == 0 || real.rfind(L"https://", 0) == 0) s = real;
        }
    }
    /* duckduckgo 跳转: /l/?uddg=<百分号编码的真实 URL> */
    size_t lp = s.find(L"duckduckgo.com/l/");
    if (lp != std::wstring::npos) {
        size_t up = s.find(L"uddg=", lp);
        if (up != std::wstring::npos) {
            std::wstring enc = s.substr(up + 5);
            size_t amp = enc.find(L'&');
            if (amp != std::wstring::npos) enc.resize(amp);
            std::wstring real = UrlDecodeW(enc);
            if (real.rfind(L"http://", 0) == 0 || real.rfind(L"https://", 0) == 0) s = real;
        }
    }
    return s;
}

static bool UrlOk(const std::wstring& u) {
    return u.size() >= 11 && (u.rfind(L"http://", 0) == 0 || u.rfind(L"https://", 0) == 0) && u.size() <= 2000;
}
static void PushHit(std::vector<AiWebHit>* out, const AiWebHit& h) {
    for (const auto& e : *out)
        if (e.url == h.url) return;   /* 同一 URL 出现多次 (广告位/聚合块) 只留首个 */
    out->push_back(h);
}

/* ---- bing RSS (主路): <item><title>..</title><link>..</link><description>..</description> ---- */
void WebParseBingRss(const std::string& xml8, int maxN, std::vector<AiWebHit>* out) {
    out->clear();
    size_t pos = 0;
    while ((int)out->size() < maxN) {
        size_t it = FindI(xml8, "<item", pos);
        if (it == std::string::npos) break;
        size_t itEnd = FindI(xml8, "</item>", it);
        if (itEnd == std::string::npos) break;
        pos = itEnd + 7;
        AiWebHit h;
        size_t p = FindI(xml8, "<title>", it);
        if (p == std::string::npos || p > itEnd) continue;
        size_t pe = FindI(xml8, "</title>", p);
        if (pe == std::string::npos || pe > itEnd) continue;
        h.title = FragText(xml8, p + 7, pe);
        p = FindI(xml8, "<link>", it);
        if (p == std::string::npos || p > itEnd) continue;
        size_t ple = FindI(xml8, "</link>", p);
        if (ple == std::string::npos || ple > itEnd) continue;
        h.url = WebUnwrapResultUrl(FragText(xml8, p + 6, ple));
        p = FindI(xml8, "<description>", it);
        if (p != std::string::npos && p < itEnd) {
            size_t pde = FindI(xml8, "</description>", p);
            if (pde != std::string::npos && pde < itEnd) h.snippet = FragText(xml8, p + 13, pde);
        }
        if (h.title.empty() || !UrlOk(h.url)) continue;
        if (h.snippet.size() > 800) h.snippet.resize(800);
        PushHit(out, h);
    }
}

/* ---- bing 结果页 HTML (回落): <li class="b_algo"> → <h2><a href>标题</a></h2> + <p>摘要</p> ---- */
void WebParseBingHtml(const std::string& html8, int maxN, std::vector<AiWebHit>* out) {
    out->clear();
    size_t pos = 0;
    while ((int)out->size() < maxN) {
        size_t p = FindI(html8, "b_algo", pos);
        if (p == std::string::npos) break;
        pos = p + 6;
        size_t end = FindI(html8, "b_algo", pos);          /* 结果块 = 相邻两个 b_algo 之间 */
        if (end == std::string::npos) end = html8.size();
        size_t h2 = FindI(html8, "<h2", p);
        if (h2 == std::string::npos || h2 >= end) continue;
        size_t a = FindI(html8, "<a", h2);
        if (a == std::string::npos || a >= end) continue;
        size_t tagClose = html8.find('>', a);
        if (tagClose == std::string::npos || tagClose >= end) continue;
        size_t aEnd = FindI(html8, "</a>", a);
        if (aEnd == std::string::npos || aEnd > end) continue;
        std::string href;
        if (!TagAttr(html8, a, tagClose + 1, "href", &href)) continue;
        AiWebHit h;
        h.url = WebUnwrapResultUrl(W8(DecAttrEntities(href).c_str()));
        h.title = FragText(html8, tagClose + 1, aEnd);
        if (h.title.empty() || !UrlOk(h.url)) continue;
        size_t ps = FindI(html8, "<p", aEnd);              /* 摘要 = 标题之后第一个 <p>…</p> */
        if (ps != std::string::npos && ps < end) {
            size_t pTxt = html8.find('>', ps);
            size_t pe = FindI(html8, "</p>", ps);
            if (pTxt != std::string::npos && pe != std::string::npos && pTxt < pe && pe <= end)
                h.snippet = FragText(html8, pTxt + 1, pe);
        }
        if (h.snippet.size() > 800) h.snippet.resize(800);
        PushHit(out, h);
    }
}

/* ---- DuckDuckGo Lite (再回落): <a class='result-link' href>标题</a> + result-snippet ---- */
void WebParseDdg(const std::string& html8, int maxN, std::vector<AiWebHit>* out) {
    out->clear();
    size_t pos = 0;
    while ((int)out->size() < maxN) {
        size_t p = FindI(html8, "result-link", pos);
        if (p == std::string::npos) break;
        pos = p + 11;
        size_t a = html8.rfind("<a", p);                   /* class 名在 <a 开标签内 */
        if (a == std::string::npos) continue;
        size_t tagClose = html8.find('>', p);
        if (tagClose == std::string::npos || tagClose <= a) continue;
        std::string href;
        if (!TagAttr(html8, a, tagClose + 1, "href", &href)) continue;
        size_t aEnd = FindI(html8, "</a>", tagClose);
        if (aEnd == std::string::npos) continue;
        AiWebHit h;
        h.url = WebUnwrapResultUrl(W8(DecAttrEntities(href).c_str()));
        h.title = FragText(html8, tagClose + 1, aEnd);
        if (h.title.empty() || !UrlOk(h.url)) continue;
        size_t sn = FindI(html8, "result-snippet", pos);
        if (sn != std::string::npos) {
            size_t sTxt = html8.find('>', sn);
            size_t se = FindI(html8, "</td>", sn);
            if (sTxt != std::string::npos && se != std::string::npos && sTxt < se)
                h.snippet = FragText(html8, sTxt + 1, se);
        }
        if (h.snippet.size() > 800) h.snippet.resize(800);
        PushHit(out, h);
    }
}

/* 整页 → 正文 (fetch_url 用; 3+ 连续换行折叠成 1 个空行, 首尾空白剔除) */
void WebHtmlToText(const std::string& html8, std::string* out8) {
    std::string o;
    StripTags(html8, 0, html8.size(), 1, &o);
    std::string r;
    int nl = 0;
    for (char c : o) {
        if (c == '\n') { if (++nl > 2) continue; }
        else nl = 0;
        r += c;
    }
    size_t b = r.find_first_not_of(" \t\r\n\f");
    if (b == std::string::npos) { out8->clear(); return; }
    size_t e = r.find_last_not_of(" \t\r\n\f");
    *out8 = r.substr(b, e - b + 1);
}

/* ==================== HTTP GET (WinHTTP) ==================== */

static const wchar_t* WEB_UA =
    L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    L"Chrome/124.0.0.0 Safari/537.36";   /* 默认 UA 会被搜索引擎降级到无 JS 兜底页 */

/* GET 一个 http(s) 地址: *out8 = 原始字节 (2MB 封顶, 极大页的"尾 20%"按截断点计),
 * *status = HTTP 状态码; false = 传输失败/被停止 (*err, 此时不看 status)。
 * 代理档位链 = 用户静态代理(WinInet) → WPAD → 直连 (2026-09-26 实测: 曾用 AUTOMATIC_PROXY
 * 单档 — 它只走 WPAD 不吃 per-user 静态代理, 用户开了系统代理时 bing 被分流的出口节点
 * 风控: RSS/HTML 返回 200 空壳、ddg 反爬 202; 换 DEFAULT_PROXY (读 WinInet 静态代理)
 * 三端点全绿)。档位只在传输失败时降级 — 200 空壳是服务端应答, 不换代理重试, 由上层换端点。 */
static bool HttpGet(AiJob* j, const std::wstring& url, std::string* out8,
                    unsigned long* status, std::wstring* err) {
    *status = 0;
    out8->clear();
    URL_COMPONENTSW uc = { sizeof(uc) };
    wchar_t host[256] = {}, path[1792] = {}, extra[512] = {};
    uc.lpszHostName = host;      uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;       uc.dwUrlPathLength = 1791;
    uc.lpszExtraInfo = extra;    uc.dwExtraInfoLength = 511;
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) {
        *err = L"URL 无法解析 (只支持标准的 http/https 地址)";
        return false;
    }
    bool secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    std::wstring wpath = std::wstring(path) + extra;
    for (int mode = 0; mode < 3; mode++) {
        const DWORD access = mode == 0 ? WINHTTP_ACCESS_TYPE_DEFAULT_PROXY
                           : mode == 1 ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
                                       : WINHTTP_ACCESS_TYPE_NO_PROXY;
        HINTERNET hs = WinHttpOpen(L"snail-quicksearch-ai-assistant", access, NULL, NULL, 0);
        if (!hs) continue;
        WinHttpSetTimeouts(hs, 10000, 10000, 15000, 20000);   /* 解析/连接/发送/收包 (毫秒) */
        HINTERNET hc = WinHttpConnect(hs, host, uc.nPort ? uc.nPort : (secure ? 443 : 80), 0);
        HINTERNET hr = NULL;
        if (hc)
            hr = WinHttpOpenRequest(hc, L"GET", wpath.c_str(), NULL, WINHTTP_NO_REFERER,
                                    WINHTTP_DEFAULT_ACCEPT_TYPES,
                                    secure ? WINHTTP_FLAG_SECURE : 0);
        bool ok = false;
        bool stopped = false;
        if (hr) {
            if (j) {
                EnterCriticalSection(&j->cs);
                j->hReq = hr;   /* UI「停止」并发关句柄打断阻塞读 (与 SSE 主请求同口径) */
                LeaveCriticalSection(&j->cs);
            }
            std::wstring hdr = std::wstring(L"User-Agent: ") + WEB_UA +
                L"\r\nAccept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n"
                L"Accept-Language: zh-CN,zh;q=0.9,en;q=0.8\r\n";
            BOOL sent = WinHttpAddRequestHeaders(hr, hdr.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD) &&
                        WinHttpSendRequest(hr, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                        WinHttpReceiveResponse(hr, NULL);
            if (sent) {
                DWORD st = 0, sz = sizeof(st);
                WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    NULL, &st, &sz, NULL);
                *status = st;
                const size_t MAXD = 2u * 1024 * 1024;
                for (;;) {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(hr, &avail) || !avail) break;
                    if (out8->size() >= MAXD) break;   /* 极大页: 到封顶为止 (尾部按截断点计) */
                    char buf[16384];
                    DWORD rd = 0, want = avail < sizeof(buf) ? avail : sizeof(buf);
                    if (!WinHttpReadData(hr, buf, want, &rd) || !rd) break;
                    out8->append(buf, rd);
                }
                ok = true;   /* 拿到应答 (任何状态码) = 事实, 判断交调用方 */
            } else {
                stopped = j && InterlockedCompareExchange(&j->abort, 0, 0) != 0;
            }
            if (j) {
                EnterCriticalSection(&j->cs);
                if (j->hReq == hr) j->hReq = NULL;
                LeaveCriticalSection(&j->cs);
            }
            WinHttpCloseHandle(hr);
        }
        if (hc) WinHttpCloseHandle(hc);
        WinHttpCloseHandle(hs);
        if (ok) return true;
        if (stopped) { *err = L"已停止"; return false; }
        /* 传输失败 → 换下一档代理重试 */
    }
    if (j && InterlockedCompareExchange(&j->abort, 0, 0)) *err = L"已停止";
    else *err = L"连接失败 (网络不可达、超时或被安全软件拦截)";
    return false;
}

static bool Aborted(AiJob* j) {
    return j && InterlockedCompareExchange(&j->abort, 0, 0) != 0;
}

/* ==================== web_search ==================== */

std::wstring WebSearchExec(AiJob* j, const Jv& v, AiToolStep* st) {
    st->kind = 15;
    std::wstring query = TrimW(v.S(L"query"));
    if (query.empty()) return L"query 不能为空";
    int maxN = 8;
    const Jv* cv = v.Get(L"count");
    if (cv && cv->t == 2 && cv->num >= 1 && cv->num <= 10) maxN = (int)cv->num;
    std::string enc8 = UrlEncodeU8(U8(query));
    std::wstring q8 = W8(enc8.c_str());
    ULONGLONG t0 = GetTickCount64();
    std::vector<AiWebHit> hits;
    std::wstring source, diag;
    std::string page8;
    unsigned long status = 0;
    std::wstring err;
    /* 1) bing RSS (结构最稳; bing.com 会 302 到区域站, WinHTTP 自动跟随) */
    if (HttpGet(j, L"https://www.bing.com/search?q=" + q8 + L"&format=rss&mkt=zh-CN",
                &page8, &status, &err)) {
        if (status == 200) {
            WebParseBingRss(page8, maxN, &hits);
            if (!hits.empty()) source = L"bing";
        } else {
            diag = L"HTTP " + std::to_wstring(status);
        }
    } else {
        diag = err;
    }
    if (Aborted(j)) return L"已停止";
    /* 2) bing 结果页 HTML (RSS 失效/改版时) */
    if (hits.empty()) {
        page8.clear(); status = 0; err.clear();
        if (HttpGet(j, L"https://www.bing.com/search?q=" + q8 + L"&mkt=zh-CN",
                    &page8, &status, &err)) {
            if (status == 200) {
                WebParseBingHtml(page8, maxN, &hits);
                if (!hits.empty()) source = L"bing";
            } else if (diag.empty()) diag = L"HTTP " + std::to_wstring(status);
        } else if (diag.empty()) diag = err;
    }
    if (Aborted(j)) return L"已停止";
    /* 3) DuckDuckGo Lite (bing 整体不可达时; 国内网络通常到不了这一步) */
    if (hits.empty()) {
        page8.clear(); status = 0; err.clear();
        if (HttpGet(j, L"https://lite.duckduckgo.com/lite/?q=" + q8, &page8, &status, &err)) {
            if (status == 200) {
                WebParseDdg(page8, maxN, &hits);
                if (!hits.empty()) source = L"duckduckgo";
            } else if (diag.empty()) diag = L"HTTP " + std::to_wstring(status);
        } else if (diag.empty()) diag = err;
    }
    if (Aborted(j)) return L"已停止";
    if (hits.empty())
        return L"联网搜索没有拿到结果" +
               (diag.empty() ? std::wstring(L" (搜索引擎返回了无法解析的页面或被风控)") : (L" — " + diag)) +
               L"; 可改用 fetch_url 直接抓取已知网址, 或稍后重试";
    st->count = (int)hits.size();
    st->elapsedMs = (long long)(GetTickCount64() - t0);
    /* 卡片样本行 = "标题 — URL" (末次出现的 " — " 是分隔符; 渲染端按它切成可点链接) */
    for (const auto& h : hits) {
        std::wstring title = h.title.size() > 120 ? h.title.substr(0, 120) + L"…" : h.title;
        st->top.push_back(title + L" — " + h.url);
    }
    picojson::object o;
    o["query"] = JS(query);
    o["source"] = JS(source);
    o["count"] = JN((long long)hits.size());
    picojson::array rs;
    for (const auto& h : hits) {
        picojson::object r;
        r["title"] = JS(h.title);
        r["url"] = JS(h.url);
        if (!h.snippet.empty()) r["snippet"] = JS(h.snippet);
        rs.push_back(picojson::value(r));
    }
    o["results"] = picojson::value(rs);
    o["note"] = JS(L"结果来自搜索引擎, 摘要只是线索: 要引用具体数据/结论前, 先用 fetch_url 打开对应 url 核对正文; "
                   L"回答里引用网页用 [标题](url) 即可点击。本地文件相关的问题仍用 run_search (索引内搜索)");
    st->res8 = picojson::value(o).serialize();
    return L"";
}

/* ==================== fetch_url ==================== */

std::wstring FetchUrlExec(AiJob* j, const Jv& v, AiToolStep* st) {
    st->kind = 16;
    std::wstring url = TrimW(v.S(L"url"));
    if (url.empty()) return L"url 不能为空 (http/https 绝对地址)";
    if (url.rfind(L"http://", 0) != 0 && url.rfind(L"https://", 0) != 0)
        return L"url 必须以 http:// 或 https:// 开头 (其它协议不支持)";
    if (url.size() > 2048) return L"url 过长 (≤2048 字符)";
    std::string raw;
    unsigned long status = 0;
    std::wstring err;
    if (!HttpGet(j, url, &raw, &status, &err)) return L"抓取失败: " + err;
    if (Aborted(j)) return L"已停止";
    if (status != 200)
        return L"HTTP " + std::to_wstring(status) +
               L" — 网页不可达 (404=地址失效, 403=站点拒绝程序访问); 如实告知用户, 不要编造内容";
    std::wstring enc;
    std::string text8 = AiTextToUtf8(raw, &enc);
    /* 二进制判定 (UTF-16 转码后不该再有 NUL): 控制字符 (除 \t\r\n\f) 占比 >5% = 非文本, 不硬猜 */
    size_t ctrl = 0;
    for (unsigned char c : text8)
        if (c < 32 && c != '\t' && c != '\n' && c != '\r' && c != '\f') ctrl++;
    if (!text8.empty() && ctrl * 20 > text8.size())
        return L"该地址返回的不是文本内容 (疑似图片/PDF 等二进制数据), 不硬猜";
    WebHtmlToText(text8, &text8);
    if (text8.empty())
        return L"网页没有可提取的正文 (可能整页由脚本渲染, 本工具拿不到) — 如实告知用户";
    text8 = AiCapUtf8HeadTail(text8);   /* 头 80%+尾 20% 封顶 (与 read_file 同 readCapKB 口径) */
    picojson::object o;
    o["url"] = JS(url);
    o["格式或编码"] = JS(enc + L" 网页正文");
    o["content"] = JS(W8(text8.c_str()));
    o["总字节"] = JN((long long)raw.size());
    st->res8 = picojson::value(o).serialize();
    return L"";
}
