/*
 * ai_web.cpp — Web 前端宿主: 系统 WebView2 生命周期 / C++↔JS JSON 桥 / 皮肤调色派生 /
 * markdown→HTML (md4c) / 消息 HTML 生成。
 * 形态: 插件在宿主搜索窗口内建一个真子窗口 (宿主表 PanelGetRect 定位面板内容区),
 * WebView2 控制器以其为父 — 鼠标/键盘/IME/选区/滚动全部归浏览器原生体系, 宿主面板
 * 事件转发与位图交付不再使用 (OPEN/RESIZE/CLOSE 之外的面板事件一律忽略)。
 * 线程: 本文件全部函数仅 UI 线程 (agent 工作线程只经 XJS_AI_STREAM 泵间接进来)。
 * 安全: JS 前端带 CSP (禁 fetch/XHR/表单/外域), 模型输出经 md4c 转义裁剪 (裸 HTML/
 * 实体按旧口径处理) 永不产生活脚本; 外链经 openurl 命令放行 http(s) 后 ShellExecute。
 */
#include "ai_assistant.h"
#include <shellapi.h>
#include "../../md4c/md4c.h"
#include "../../webview2/include/WebView2.h"

/* ==================== 调色 (参考 AI 对话框的皮肤派生) ==================== */

AiPal PalOf(AiSess* s) {
    AiPal p;
    p.bg = s->cBg;
    p.panel = s->cPanel;
    p.text = s->cText;
    p.dim = s->cDim;
    p.accent = s->cAccent;
    p.t3 = MixCol(s->cDim, s->cBg, 0.42f);            /* text-tertiary */
    p.hover = WithA(s->cText, 31);                    /* btn-secondary-hover (12%) */
    p.divider = WithA(s->cText, 15);                  /* divider (6%) */
    p.border = WithA(s->cText, 18);                   /* glass-border (7%) */
    p.borderStrong = WithA(s->cText, 46);             /* overlay-border (18%) */
    p.cyan = Gdiplus::Color(255, 14, 165, 233);       /* #0ea5e9 */
    p.emerald = Gdiplus::Color(255, 16, 185, 129);    /* #10b981 */
    p.amber = Gdiplus::Color(255, 245, 158, 11);      /* #f59e0b */
    p.red = Gdiplus::Color(255, 239, 68, 68);         /* #ef4444 */
    p.ok = Gdiplus::Color(255, 34, 197, 94);          /* #22c55e (在线绿) */
    p.userAcc = Gdiplus::Color(255, 249, 115, 22);    /* #f97316 (用户气泡暖橙) */
    return p;
}

std::wstring ColHex(const Gdiplus::Color& c) {
    wchar_t b[8];
    swprintf(b, 8, L"#%02x%02x%02x", c.GetR(), c.GetG(), c.GetB());
    return b;
}
std::wstring ColHexA(const Gdiplus::Color& c) {
    wchar_t b[12];
    swprintf(b, 12, L"#%02x%02x%02x%02x", c.GetR(), c.GetG(), c.GetB(), c.GetA());
    return b;
}

static picojson::value WebPalValue(AiSess* s) {
    AiPal p = PalOf(s);
    /* 键 = ai_web_ui.cpp :root 的 CSS 变量名; 8 位 #rrggbbaa 浏览器原样可解析 */
    picojson::object o;
    auto add = [&](const char* k, const std::wstring& v) { o[k] = JS(v); };
    add("bg", ColHex(p.bg));
    add("panel", ColHex(p.panel));
    add("text", ColHex(p.text));
    add("dim", ColHex(p.dim));
    add("accent", ColHex(p.accent));
    add("t3", ColHexA(p.t3));
    add("hover", ColHexA(p.hover));
    add("divider", ColHexA(p.divider));
    add("border", ColHexA(p.border));
    add("borderStrong", ColHexA(p.borderStrong));
    add("cyan", ColHex(p.cyan));
    add("emerald", ColHex(p.emerald));
    add("amber", ColHex(p.amber));
    add("red", ColHex(p.red));
    add("ok", ColHex(p.ok));
    add("userAcc", ColHex(p.userAcc));
    return picojson::value(o);
}

static picojson::value WebCfgValue() {
    AiProfile* act = CfgActive();
    picojson::object o;
    o["url"] = JS(g_cfg.baseUrl);
    o["model"] = JS(g_cfg.model);
    o["hasKey"] = JB(!g_cfg.apiKey.empty());
    o["reasoning"] = JB(g_cfg.reasoning);
    o["policy"] = JN(g_cfg.filePolicy);
    o["epolicy"] = JN(g_cfg.execPolicy);
    /* 活动档案显示名: 状态点与工具栏模型按钮用它 (与前端兜底同款: name||model||未命名模型) */
    o["name"] = JS(CfgDisplayName(act));
    o["ctx"] = JN(g_cfg.ctx);
    o["maxOut"] = JN(g_cfg.maxOut);
    o["active"] = JS(g_cfg.activeId);
    /* 档案表整包下发 (密钥只出 hasKey, 明文不出宿主) */
    picojson::array profs;
    for (AiProfile& p : g_cfg.profiles) {
        picojson::object po;
        po["id"] = JS(p.id);
        po["name"] = JS(p.name);
        po["url"] = JS(p.baseUrl);
        po["model"] = JS(p.model);
        po["hasKey"] = JB(!p.apiKey.empty());
        po["ctx"] = JN(p.ctx);
        po["maxOut"] = JN(p.maxOut);
        profs.push_back(picojson::value(po));
    }
    o["profs"] = picojson::value(profs);
    return picojson::value(o);
}

std::wstring WebPaletteJson(AiSess* s) { return JDumpW(WebPalValue(s)); }
std::wstring WebCfgJson() { return JDumpW(WebCfgValue()); }

/* ==================== markdown → HTML (md4c; 表格/任务列表/删除线) ====================
 * 模型裸 HTML 一律丢弃 (MD_TEXT_HTML/IMG), 实体原文透传, 其余文本转义 — 输出永不携带
 * 可执行标记; 浏览器端 CSP 再关一道脚本/fetch 面。代码块原文嵌 data-code 属性 (复制)。 */

struct HCtx {
    std::wstring out;
    std::vector<int> blocks;   /* 块栈 (MD_BLOCKTYPE; LI 直下首段不再包 <p>, 段距由 li margin 表达) */
    std::vector<int> heads;    /* 打开的标题层级/任务完成标记 (闭标签用; md4c 不嵌套标题) */
    bool inCode = false;
    std::wstring codeRaw;      /* 代码块原文 (data-code 属性, 复制按钮用) */
    int codeIdx = 0;           /* 代码块占位序号 (leave 时回填原文) */
    size_t pStart = 0;         /* 当前 <p> 的写出位置 (小标题改写用) */
    int imgSkip = 0;           /* >0 = 正在渲染捐赠二维码 img, alt 文本不重复输出 */
};

static std::wstring MdUtf8(const char* s, MD_SIZE n) {
    if (!s || n == 0) return L"";
    int wl = MultiByteToWideChar(CP_UTF8, 0, s, (int)n, NULL, 0);
    std::wstring w(wl > 0 ? (size_t)wl : 0, L'\0');
    if (wl > 0) MultiByteToWideChar(CP_UTF8, 0, s, (int)n, &w[0], wl);
    return w;
}

/* 文本 HTML 转义 (正文/属性值共用; md4c 的实体串原文透传不经此) */
static void HtmlEscape(std::wstring* out, const std::wstring& t) {
    for (wchar_t c : t) {
        switch (c) {
            case L'&': *out += L"&amp;"; break;
            case L'<': *out += L"&lt;"; break;
            case L'>': *out += L"&gt;"; break;
            case L'"': *out += L"&quot;"; break;
            case L'\'': *out += L"&#39;"; break;
            default: *out += c; break;
        }
    }
}

static int HEnterBlock(MD_BLOCKTYPE type, void* detail, void* ud) {
    HCtx* c = (HCtx*)ud;
    switch (type) {
        case MD_BLOCK_P:
            if (!c->blocks.empty() && c->blocks.back() == MD_BLOCK_LI) { c->blocks.push_back(type); break; }
            c->pStart = c->out.size();
            c->out += L"<p>";
            c->blocks.push_back(type);
            break;
        case MD_BLOCK_H: {
            int lvl = (int)((MD_BLOCK_H_DETAIL*)detail)->level;
            if (lvl < 1) lvl = 1;
            if (lvl > 6) lvl = 6;
            wchar_t b[16];
            swprintf(b, 16, L"<h%d>", lvl);
            c->out += b;
            c->heads.push_back(lvl);
            c->blocks.push_back(type);
            break;
        }
        case MD_BLOCK_UL: c->out += L"<ul>"; c->blocks.push_back(type); break;
        case MD_BLOCK_OL: {
            int start = (int)((MD_BLOCK_OL_DETAIL*)detail)->start;
            wchar_t b[32];
            swprintf(b, 32, L"<ol start=\"%d\">", start > 0 ? start : 1);
            c->out += b;
            c->blocks.push_back(type);
            break;
        }
        case MD_BLOCK_LI: {
            /* 任务列表 (- [ ] / - [x]): 复选框 + 完成态划线 (参考实现 .task)。
               heads 栈顶复用为完成标记 (1=完成) */
            MD_BLOCK_LI_DETAIL* d = (MD_BLOCK_LI_DETAIL*)detail;
            if (d->is_task) {
                c->out += L"<li class=\"ai-task\"><span class=\"ai-task-box";
                if (d->task_mark != L' ') c->out += L" done";
                c->out += L"\"></span>";
                if (d->task_mark != L' ') {
                    c->out += L"<span class=\"ai-task-text\">";
                    c->heads.push_back(1);
                }
                c->blocks.push_back(type);
                break;
            }
            c->out += L"<li>";
            c->blocks.push_back(type);
            break;
        }
        case MD_BLOCK_QUOTE: c->out += L"<blockquote>"; c->blocks.push_back(type); break;
        case MD_BLOCK_CODE: {
            MD_BLOCK_CODE_DETAIL* d = (MD_BLOCK_CODE_DETAIL*)detail;
            c->inCode = true;
            c->codeRaw.clear();
            c->out += L"<div class=\"ai-code\" data-code=\"\x1CODE";
            wchar_t nb[16];
            swprintf(nb, 16, L"%d\x1\">", c->codeIdx++);   /* 占位 (leave 时回填转义原文) */
            c->out += nb;
            c->out += L"<div class=\"ai-code-head\"><span class=\"ai-code-lang\">";
            std::wstring lang = d->info.text ? MdUtf8(d->info.text, (MD_SIZE)d->info.size) : L"";
            HtmlEscape(&c->out, lang.empty() ? L"text" : lang);
            c->out += L"</span><span class=\"ai-code-copy\">复制</span></div><pre><code>";
            c->blocks.push_back(type);
            break;
        }
        case MD_BLOCK_TABLE: c->out += L"<div class=\"ai-table-wrap\"><table>"; c->blocks.push_back(type); break;
        case MD_BLOCK_THEAD: c->out += L"<thead>"; c->blocks.push_back(type); break;
        case MD_BLOCK_TBODY: c->out += L"<tbody>"; c->blocks.push_back(type); break;
        case MD_BLOCK_TR: c->out += L"<tr>"; c->blocks.push_back(type); break;
        case MD_BLOCK_TH: case MD_BLOCK_TD: {
            int align = (int)((MD_BLOCK_TD_DETAIL*)detail)->align;   /* 0=默认 1=左 2=中 3=右 */
            c->out += (type == MD_BLOCK_TH) ? L"<th" : L"<td";
            if (align == 1) c->out += L" style=\"text-align:left\"";
            else if (align == 2) c->out += L" style=\"text-align:center\"";
            else if (align == 3) c->out += L" style=\"text-align:right\"";
            c->out += L">";
            c->blocks.push_back(type);
            break;
        }
        default: c->blocks.push_back(type); break;   /* DOC 等容器 */
    }
    return 0;
}

static int HLeaveBlock(MD_BLOCKTYPE type, void* /*detail*/, void* ud) {
    HCtx* c = (HCtx*)ud;
    switch (type) {
        case MD_BLOCK_P: {
            if (!c->blocks.empty() && c->blocks.back() == MD_BLOCK_P) {
                c->out += L"</p>";
                /* 加粗短句 (≤60 内部字符, 尾部最多一个冒号) 视为小标题 (参考实现 sub) */
                if (c->out.size() > c->pStart + 12 &&
                    c->out.compare(c->pStart, 3, L"<p>") == 0 &&
                    c->out.compare(c->pStart + 3, 8, L"<strong>") == 0) {
                    size_t close = c->out.find(L"</strong>", c->pStart + 3);
                    size_t bodyEnd = c->out.size() - 4;   /* "</p>" 前 */
                    if (close != std::wstring::npos && close < bodyEnd) {
                        size_t strongInner = close - (c->pStart + 11);
                        size_t tailLen = bodyEnd - (close + 9);
                        if (strongInner >= 1 && strongInner <= 60 && tailLen == 0)
                            c->out.replace(c->pStart, 3, L"<p class=\"ai-md-sub\">");
                        else if (strongInner >= 1 && strongInner <= 60 && tailLen == 1 &&
                                 (c->out[bodyEnd - 1] == L':' || c->out[bodyEnd - 1] == L'\xFF1A'))
                            c->out.replace(c->pStart, 3, L"<p class=\"ai-md-sub\">");
                    }
                }
            }
            break;
        }
        case MD_BLOCK_H: {
            int lvl = c->heads.empty() ? 1 : c->heads.back();
            if (!c->heads.empty()) c->heads.pop_back();
            wchar_t b[16];
            swprintf(b, 16, L"</h%d>", lvl);
            c->out += b;
            break;
        }
        case MD_BLOCK_UL: c->out += L"</ul>"; break;
        case MD_BLOCK_OL: c->out += L"</ol>"; break;
        case MD_BLOCK_LI:
            if (!c->heads.empty() && c->heads.back() == 1) {   /* 完成态任务项收口 (tdone span) */
                c->heads.pop_back();
                c->out += L"</span>";
            }
            c->out += L"</li>";
            break;
        case MD_BLOCK_QUOTE: c->out += L"</blockquote>"; break;
        case MD_BLOCK_CODE: {
            c->inCode = false;
            /* 回填代码块原文进 data-code 属性 (转义含引号) */
            wchar_t key[24];
            swprintf(key, 24, L"\x1CODE%d\x1", c->codeIdx - 1);
            std::wstring fill;
            HtmlEscape(&fill, c->codeRaw);
            size_t pos = c->out.find(key);
            if (pos != std::wstring::npos) c->out.replace(pos, wcslen(key), fill);
            c->out += L"</code></pre></div>";
            break;
        }
        case MD_BLOCK_TABLE: c->out += L"</table></div>"; break;
        case MD_BLOCK_THEAD: c->out += L"</thead>"; break;
        case MD_BLOCK_TBODY: c->out += L"</tbody>"; break;
        case MD_BLOCK_TR: c->out += L"</tr>"; break;
        case MD_BLOCK_TH: c->out += L"</th>"; break;
        case MD_BLOCK_TD: c->out += L"</td>"; break;
        default: break;
    }
    if (!c->blocks.empty() && c->blocks.back() == type) c->blocks.pop_back();
    return 0;
}

static int HEnterSpan(MD_SPANTYPE type, void* detail, void* ud) {
    HCtx* c = (HCtx*)ud;
    switch (type) {
        case MD_SPAN_STRONG: c->out += L"<strong>"; break;
        case MD_SPAN_EM: c->out += L"<em>"; break;
        case MD_SPAN_CODE: c->out += L"<code>"; break;
        case MD_SPAN_DEL: c->out += L"<del>"; break;
        case MD_SPAN_A: {
            MD_SPAN_A_DETAIL* d = (MD_SPAN_A_DETAIL*)detail;
            c->out += L"<a href=\"";
            HtmlEscape(&c->out, MdUtf8((const char*)d->href.text, (MD_SIZE)d->href.size));
            c->out += L"\">";
            break;
        }
        case MD_SPAN_IMG: {
            /* 模型输出里的 ![捐赠码](xjs://donate?kind=..) = get_donate_qr 工具教它的引用语法:
               渲染层在此把引用换成缓存的 data URL 真图 (图片本体从不进对话通道)。
               其它图片维持降级为 alt 文本 */
            MD_SPAN_A_DETAIL* d = (MD_SPAN_A_DETAIL*)detail;   /* IMG detail 前两个字段与 A 同构 (href/title) */
            std::wstring href = MdUtf8((const char*)d->href.text, (MD_SIZE)d->href.size);
            if (href.rfind(L"xjs://donate?kind=", 0) == 0) {
                std::wstring kind = href.substr(18);
                bool wechat = kind == L"wechat";
                if (wechat || kind == L"alipay") {
                    std::wstring url = DonateQrDataUrl(wechat ? 0 : 1);
                    if (!url.empty()) {
                        c->out += L"<img class=\"ai-donate-qr\" src=\"";
                        HtmlEscape(&c->out, url);
                        c->out += wechat ? L"\" alt=\"微信捐赠码\">" : L"\" alt=\"支付宝捐赠码\">";
                        c->imgSkip++;
                        break;
                    }
                }
            }
            break;
        }
        default: break;
    }
    return 0;
}
static int HLeaveSpan(MD_SPANTYPE type, void* /*detail*/, void* ud) {
    HCtx* c = (HCtx*)ud;
    switch (type) {
        case MD_SPAN_STRONG: c->out += L"</strong>"; break;
        case MD_SPAN_EM: c->out += L"</em>"; break;
        case MD_SPAN_CODE: c->out += L"</code>"; break;
        case MD_SPAN_DEL: c->out += L"</del>"; break;
        case MD_SPAN_A: c->out += L"</a>"; break;
        case MD_SPAN_IMG: if (c->imgSkip) c->imgSkip--; break;   /* 捐赠二维码 img 已写完, 恢复 alt 文本输出 */
        default: break;
    }
    return 0;
}

static int HText(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* ud) {
    HCtx* c = (HCtx*)ud;
    std::wstring w = MdUtf8(text, size);
    switch (type) {
        /* 行内/块级代码内容 (反引号、``` 围栏) 走独立 CODE 类型 — 曾落进 default 被整段
           丢弃 = 气泡里所有代码/路径内容凭空消失、复制按钮取不到原文 */
        case MD_TEXT_CODE:
            if (c->inCode) c->codeRaw += w;   /* 块级代码原文 (复制按钮用) */
            HtmlEscape(&c->out, w);
            break;
        case MD_TEXT_NORMAL:
            if (c->imgSkip) break;   /* 捐赠二维码 img 的 alt 内文不重复渲染 (图已带 alt 属性) */
            if (c->inCode) c->codeRaw += w;
            HtmlEscape(&c->out, w);
            break;
        case MD_TEXT_ENTITY: c->out += w; break;   /* 实体原文透传 (HTML 输出直接可用) */
        case MD_TEXT_SOFTBR: if (!c->inCode) c->out += L"<br/>"; break;   /* 段内换行保留为 <br> */
        case MD_TEXT_BR: c->out += L"<br/>"; break;
        default: break;   /* HTML/NULLCHAR 丢弃 (不渲染模型输出里的裸 HTML) */
    }
    return 0;
}

/* ---- 解析前归一化: 表头行前补空行 ----
 * md4c 的表格判定要求表头行是"恰好 1 行的段落"(md4c.c: 分隔行只在当前块仅 1 行时生效)。
 * 模型高频把加粗引导行直接贴着表头写 — 表头成了该段落的懒续行, 轮到 |---| 分隔行时
 * 段落已有 2 行, 表格判定短路, 整表退化成带竖线的纯文本。只在
 * "非空非表格行 / | 表头 | / |---|" 三行相邻时插入一个空行, 代码围栏内不动。 */
static bool MdLineIsTableRow(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t");
    return b != std::wstring::npos && b <= 3 && s[b] == L'|';
}
static bool MdLineIsTableDelimiter(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t"), e = s.find_last_not_of(L" \t");
    if (b == std::wstring::npos) return false;
    bool bar = false, dash = false;
    for (size_t i = b; i <= e; i++) {
        wchar_t c = s[i];
        if (c == L'|') bar = true;
        else if (c == L'-') dash = true;
        else if (c != L':' && c != L' ' && c != L'\t') return false;   /* 分隔行只许 | - : 与空白 */
    }
    return bar && dash;
}
static std::wstring MdNormalizeTables(const std::wstring& text) {
    if (text.find(L'|') == std::wstring::npos) return text;
    std::vector<std::wstring> lines;   /* 切行 (吃 \r; 重建时统一 \n, 只喂解析器无妨) */
    size_t pos = 0;
    for (;;) {
        size_t nl = text.find(L'\n', pos);
        std::wstring ln = (nl == std::wstring::npos) ? text.substr(pos)
                                                     : text.substr(pos, nl - pos);
        if (!ln.empty() && ln.back() == L'\r') ln.pop_back();
        lines.push_back(ln);
        if (nl == std::wstring::npos) break;
        pos = nl + 1;
    }
    std::vector<char> fenced(lines.size(), 0);   /* 该行之前是否处于 ``` / ~~~ 围栏内 */
    wchar_t fence = 0;
    for (size_t i = 0; i < lines.size(); i++) {
        fenced[i] = fence != 0;
        size_t b = lines[i].find_first_not_of(L" \t");
        if (b == std::wstring::npos || lines[i].size() - b < 3) continue;
        wchar_t c = lines[i][b];
        if ((c == L'`' || c == L'~') && lines[i][b + 1] == c && lines[i][b + 2] == c)
            fence = (fence == 0) ? c : (c == fence ? (wchar_t)0 : fence);
    }
    std::wstring out;
    out.reserve(text.size() + 16);
    for (size_t i = 0; i < lines.size(); i++) {
        if (i >= 1 && i + 1 < lines.size() &&
            !fenced[i] && !fenced[i - 1] && !fenced[i + 1] &&
            MdLineIsTableRow(lines[i]) && MdLineIsTableDelimiter(lines[i + 1]) &&
            !MdLineIsTableRow(lines[i - 1]) &&
            !lines[i - 1].empty() &&
            lines[i - 1].find_first_not_of(L" \t") != std::wstring::npos)
        {
            out += L'\n';
        }
        out += lines[i];
        out += L'\n';
    }
    return out;
}

/* md4c 解析失败返回 false (调用方兜底: 原文按代码块呈现, 内容不丢) */
bool MdToHtml(const std::wstring& text, std::wstring* out) {
    HCtx ctx;
    MD_PARSER p = {};
    p.abi_version = 0;
    p.flags = MD_FLAG_TABLES | MD_FLAG_COLLAPSEWHITESPACE | MD_FLAG_PERMISSIVEURLAUTOLINKS |
              MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS;
    p.enter_block = HEnterBlock;
    p.leave_block = HLeaveBlock;
    p.enter_span = HEnterSpan;
    p.leave_span = HLeaveSpan;
    p.text = HText;
    std::string u8 = U8(MdNormalizeTables(text));
    if (md_parse(u8.c_str(), (MD_SIZE)u8.size(), &p, &ctx) != 0) return false;
    *out = ctx.out;
    return true;
}

/* ==================== 消息 HTML 生成 ==================== */

static std::wstring StepStatText(const AiToolStep& st) {
    if (st.state <= 1) return st.state == 0 ? L"排队中…" : L"执行中…";
    if (st.state == 2) {
        if (st.kind == 0 && st.count >= 0) {
            wchar_t nb[64];
            swprintf(nb, 64, L"✓ %d 条 · %lld ms", st.count, st.elapsedMs);
            return nb;
        }
        return L"✓ 完成";
    }
    if (st.state == 4) return L"等待确认";
    return L"✕ 失败";
}

/* 工具卡片徽标 (kind 与 ai_assistant.h AiToolStep 注释一致) */
static const wchar_t* StepBadge(int kind) {
    switch (kind) {
        case 0: return L"搜索";
        case 1: return L"打开";
        case 2: return L"复制";
        case 3: return L"设置";
        case 4: return L"窗口";
        case 5: return L"搜索框";
        case 6: return L"模式";
        case 7: return L"插件";
        case 8: return L"皮肤";
        case 9: return L"规范";
        case 10: return L"关于";
        case 11: return L"命令";
    }
    return L"工具";
}

/* role==2 工具卡片组 (旧 .ai-cmd-entry 口径; 查询展示截 200 字符 — 浏览器端折行,
   卡头不因超长脚本无限增高); state==4 带确认按钮; 样本列表默认收起 (JS 按展开态回放) */
static void StepsHtml(const AiMsg& m, int mi, std::wstring* out) {
    wchar_t b[64];
    for (size_t si = 0; si < m.steps.size(); si++) {
        const AiToolStep& st = m.steps[si];
        /* 卡片命令串 = kind 0/11 用查询原文 (11 = 待确认的命令, 用户要审的就是它;
         * 前缀 shell 名), 其余 工具名+参数 (显示截断 200, data-q 存全文) */
        std::wstring cmd;
        if (st.kind == 11 && !st.query.empty()) {
            cmd = L"[" + (st.mode.empty() ? L"cmd" : st.mode) + L"] " + st.query;
        } else if (st.kind == 0 && !st.query.empty()) {
            cmd = st.query;
        } else {
            cmd = !st.argz.empty() ? (st.name.empty() ? st.argz : st.name + L" " + st.argz)
                                   : (st.name.empty() ? L"工具" : st.name);
        }
        swprintf(b, 64, L"<div class=\"step%s\" data-gi=\"%d\"", st.state == 3 ? L" failed" : L"", mi);
        *out += b;
        /* data-q = 完整查询原文 (头部 .scmd 截 200 只供显示; 右键"复制查询语句"要全文) */
        *out += L" data-q=\"";
        HtmlEscape(out, cmd);
        *out += L"\">";
        *out += L"<div class=\"shead\"><span class=\"sbadge\">";
        HtmlEscape(out, StepBadge(st.kind));
        *out += L"</span><span class=\"scmd\">";
        if (cmd.size() > 200) { cmd.resize(200); cmd += L"…"; }
        HtmlEscape(out, cmd);
        *out += L"</span><span class=\"sst";
        if (st.state == 3 || st.state == 4) *out += L" bad";
        *out += L"\">";
        HtmlEscape(out, StepStatText(st));
        /* 折叠/展开箭头 (点击头部切换; 样本列表默认收起) */
        *out += L"</span><span class=\"sarr glyph\">&#xE70D;</span></div>";
        if (st.state == 4) {
            if (st.kind == 11) {
                /* 命令执行确认卡: 允许一次 = 只放行这条命令的完全相同重试 (不持久放权);
                   data-mi/si 供 eallow 回传定位 (凭据按 shell|command 键比对) */
                wchar_t ab[48];
                swprintf(ab, 48, L"<div class=\"sask\" data-mi=\"%d\" data-si=\"%d\">", mi, (int)si);
                *out += ab;
                HtmlEscape(out, st.err.empty() ? L"等待用户确认命令执行" : st.err);
                *out += L"<br/><span class=\"abtn primary\" data-act=\"execallow\">允许一次</span>";
                *out += L"<span class=\"abtn\" data-act=\"execdeny\">拒绝</span></div>";
            } else {
                /* 策略询问: 允许 (转允许并放行后续) / 保持拒绝 */
                *out += L"<div class=\"sask\">";
                HtmlEscape(out, st.err.empty() ? L"等待用户确认文件操作" : st.err);
                *out += L"<br/><span class=\"abtn primary\" data-act=\"authallow\">允许并继续</span>";
                *out += L"<span class=\"abtn\" data-act=\"authdeny\">保持拒绝</span></div>";
            }
        } else if (st.state == 3 && !st.err.empty()) {
            *out += L"<div class=\"sout\">";
            HtmlEscape(out, st.err);
            *out += L"</div>";
        }
        if (!st.adj.items.empty()) {
            /* 待应用的调整: AI 提案逐项列出, 用户点 应用/忽略 才执行 (WebCommand "adj")。
               只渲染会话份步骤的状态 — 泵不比对 adj 字段, 用户裁决不会被 worker 镜像回写 */
            int pend = 0, okn = 0, ign = 0, bad = 0;
            for (auto& it : st.adj.items) {
                if (it.state == 0) pend++;
                else if (it.state == 1) okn++;
                else if (it.state == 2) ign++;
                else bad++;
            }
            swprintf(b, 64, L"<div class=\"sadj\" data-mi=\"%d\" data-si=\"%d\">", mi, (int)si);
            *out += b;
            *out += L"<div class=\"sadj-head\">";
            *out += pend ? L"⏳ 待应用的调整 — 点「应用」才会生效" : L"待应用的调整 — 已处理完";
            *out += L"</div>";
            for (size_t ai = 0; ai < st.adj.items.size(); ai++) {
                const AiAdjustItem& it = st.adj.items[ai];
                *out += L"<div class=\"sadj-it";
                if (it.state == 1) *out += L" done";
                else if (it.state == 3) *out += L" bad";
                *out += L"\" data-ii=\"";
                swprintf(b, 32, L"%d", (int)ai);
                *out += b;
                *out += L"\"><span class=\"sadj-k\">";
                HtmlEscape(out, it.key);
                *out += L" → </span><span class=\"sadj-v\">";
                HtmlEscape(out, it.val);
                *out += L"</span><span class=\"sadj-st\">";
                if (it.state == 1) *out += L"✓ 已应用";
                else if (it.state == 2) *out += L"已忽略";
                else if (it.state == 3) { *out += L"✕ "; HtmlEscape(out, it.err); }
                *out += L"</span>";
                if (it.state == 0) {
                    *out += L"<span class=\"sadj-btns\"><span class=\"abtn\" data-act=\"adjIgnore\">忽略</span>"
                            L"<span class=\"abtn primary\" data-act=\"adjApply\">应用</span></span>";
                }
                *out += L"</div>";
            }
            *out += L"<div class=\"sadj-foot\">";
            wchar_t sum[80];
            swprintf(sum, 80, L"<span class=\"sadj-sum\">共 %d 项 · 待应用 %d · 已应用 %d · 已忽略 %d</span>",
                     (int)st.adj.items.size(), pend, okn, ign);
            *out += sum;
            if (pend) {
                *out += L"<span class=\"sadj-btns\"><span class=\"abtn\" data-act=\"adjIgnoreAll\">全部忽略</span>"
                        L"<span class=\"abtn primary\" data-act=\"adjApplyAll\">全部应用</span></span>";
            }
            *out += L"</div></div>";
        }
        if (!st.top.empty()) {
            *out += L"<div class=\"ssamples\" style=\"display:none\">";
            for (size_t ti = 0; ti < st.top.size(); ti++) {
                wchar_t no[16];
                swprintf(no, 16, L"%d. ", (int)(ti + 1));
                *out += no;
                /* 样本路径 = 可点击链接 (单击打开/右键定位复制; 前端 .ai-path 统一处理) */
                *out += L"<span class=\"ai-path\" data-path=\"";
                HtmlEscape(out, st.top[ti]);
                *out += L"\">";
                HtmlEscape(out, st.top[ti]);
                *out += L"</span>";
                if (ti + 1 < st.top.size()) *out += L"\n";
            }
            *out += L"</div>";
        }
        *out += L"</div>";
    }
}

/* 消息气泡 (类名与参考实现同源: ai-bubble / ai-bubble-user / ai-bubble-error /
 * ai-steps — 前端 CSS 按这套名字取值) */
void MsgHtmlOf(AiSess* s, const AiMsg& m, int mi, bool thinking, std::wstring* out) {
    (void)s;
    (void)thinking;
    if (m.role == 2) {
        *out += L"<div class=\"ai-steps\">";
        StepsHtml(m, mi, out);
        *out += L"</div>";
        return;
    }
    std::wstring body;
    if (!m.text.empty()) {
        if (!MdToHtml(m.text, &body)) {   /* 解析失败兜底: 原文按代码块呈现 */
            body = L"<div class=\"ai-code\"><div class=\"ai-code-head\"><span class=\"ai-code-lang\">text</span></div>"
                   L"<pre><code>";
            HtmlEscape(&body, m.text);
            body += L"</code></pre></div>";
        }
    }
    if (body.empty()) body = L"&nbsp;";   /* 空气泡占位 (打字三点由前端覆绘, 这里只兜瞬态) */
    *out += L"<div class=\"ai-bubble";
    if (m.role == 0) *out += L" ai-bubble-user";
    if (m.err) *out += L" ai-bubble-error";
    *out += L"\">";
    *out += body;
    *out += L"</div>";
}

/* ==================== WebView2 宿主 ==================== */

struct AiWebCtx {
    HWND hwnd = NULL;                       /* 插件自建子窗口 (面板内容区大小, 宿主窗的 child) */
    ICoreWebView2Controller* ctrl = NULL;
    ICoreWebView2* web = NULL;
    EventRegistrationToken msgTok{}, focusTok{}, focusTok2{}, navTok{}, newTok{};
    bool msgHooked = false, focusHooked = false, navHooked = false, newHooked = false;
    bool focusBorrowed = false;             /* 浏览器持有真实焦点 (已向宿主借键盘) */
    /* 推送缓存 (status/usage 变化判定; boot 后有效) */
    bool stSending = false;
    int stNet = -1, stPhase = -1;
    std::wstring stNote;                    /* 过程状态条 (重试/自愈中; 变化才推) */
    bool uHas = false;
    long long uUp = -1, uUo = -1, uUt = -1, uUch = -1, uLp = -1, uLc = -1;
    double uTps = -1;
};

static ICoreWebView2Environment* g_webEnv = NULL;   /* 进程一份 (UDF 绑插件目录) */
static bool g_envPending = false;                   /* 环境创建在途 */
static bool g_noRuntime = false;                    /* 系统无 WebView2 运行时 (每会话 Toast 一次) */
static std::wstring g_udfDir;

/* ---- 加载器 = 官方 WebView2Loader.dll (动态加载, 与插件同目录随包部署) ----
 * 不静态链接 WebView2LoaderStatic.lib: 10MB 静态库换来的只有体积, 动态 DLL 只依赖
 * 系统库, 边界干净。注意真正的面板崩溃根因是回调交付指针的引用语义 (见 EnvHandler /
 * WebCtrlHandler::Invoke 的 AddRef 注释), 与加载器形态无关 — 动态化只是顺手收敛。 */
typedef HRESULT (__stdcall* PFN_GetVerW)(PCWSTR browserExecutableFolder, LPWSTR* versionInfo);
typedef HRESULT (__stdcall* PFN_CreateEnvW)(PCWSTR browserExecutableFolder, PCWSTR userDataFolder,
                                            ICoreWebView2EnvironmentOptions* options,
                                            ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* handler);
static PFN_GetVerW  pfnGetVer = NULL;
static PFN_CreateEnvW pfnCreateEnv = NULL;

static bool WebEnsureLoader() {
    if (pfnCreateEnv && pfnGetVer) return true;
    HMODULE mod = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&WebEnsureLoader, &mod);   /* 任意插件模块地址 → 同目录 */
    wchar_t path[MAX_PATH] = {};
    if (!mod || !GetModuleFileNameW(mod, path, MAX_PATH)) return false;
    std::wstring dir(path);
    size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    dir.resize(slash + 1);
    mod = LoadLibraryW((dir + L"WebView2Loader.dll").c_str());
    if (!mod) return false;
    pfnGetVer = (PFN_GetVerW)(void*)GetProcAddress(mod, "GetAvailableCoreWebView2BrowserVersionString");
    pfnCreateEnv = (PFN_CreateEnvW)(void*)GetProcAddress(mod, "CreateCoreWebView2EnvironmentWithOptions");
    return pfnGetVer && pfnCreateEnv;
}

/* ---- 环境延迟建 (首次 PanelOpen 才起浏览器进程, 不空耗内存) ---- */

static bool WebEnsureUdfDir() {
    if (!g_udfDir.empty()) return true;
    HMODULE mod = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&WebEnsureUdfDir, &mod))
        return false;
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(mod, path, MAX_PATH)) return false;
    std::wstring dir(path);
    size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    g_udfDir = dir.substr(0, slash) + L"\\webview2-data";
    return true;
}

static void WebCreateControllersPending();   /* 前置: 环境就绪后补建全部在途会话控制器 */

static bool WebEnsureEnv() {
    if (g_webEnv) return true;
    if (g_envPending) return true;
    if (g_noRuntime) return false;
    if (!WebEnsureUdfDir()) return false;
    if (!WebEnsureLoader()) {   /* 加载器 DLL 缺失 (部署不完整) 按运行时缺失口径收敛 */
        g_noRuntime = true;
        return false;
    }
    wchar_t* ver = NULL;
    HRESULT hr = pfnGetVer(NULL, &ver);
    bool has = SUCCEEDED(hr) && ver && *ver;
    if (ver) CoTaskMemFree(ver);
    if (!has) {   /* 未装 WebView2 运行时 (系统组件, Win11 自带) */
        g_noRuntime = true;
        return false;
    }
    g_envPending = true;
    struct EnvHandler : ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
        ULONG ref = 1;
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
            if (!pp) return E_POINTER;
            if (riid == __uuidof(IUnknown) || riid == __uuidof(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)) {
                *pp = (ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*)this;
                AddRef();
                return S_OK;
            }
            *pp = NULL;
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
        ULONG STDMETHODCALLTYPE Release() override {
            ULONG r = InterlockedDecrement(&ref);
            if (!r) delete this;
            return r;
        }
        HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Environment* env) override {
            g_envPending = false;
            if (SUCCEEDED(result) && env) {
                env->AddRef();   /* 回调交付 = 借用引用, Invoke 返回即释放: 自留一份 (官方样例 QI 同义) */
                g_webEnv = env;  /* 终身持有 (插件不卸载) */
                WebCreateControllersPending();
            } else {
                g_noRuntime = true;
            }
            return S_OK;
        }
    };
    hr = pfnCreateEnv(NULL, g_udfDir.c_str(), NULL, new EnvHandler());
    if (FAILED(hr)) {
        g_envPending = false;
        g_noRuntime = true;
        return false;
    }
    return true;   /* 在途 = 异步回调里补建 */
}

static void WebCreateControllerFor(AiSess* s);   /* 前置 */

static void WebCreateControllersPending() {
    for (auto& ss : g_sess)
        if (ss.inUse && ss.web) WebCreateControllerFor(&ss);
}

/* ---- 子窗口 (面板内容区大小; WebView2 控制器的父) ---- */

/* 焦点对账心跳 (250ms, WM_TIMER 驱动): 控制器 GotFocus/LostFocus 之外的自愈网 —
   plugPanelKey (双光标防线/键盘路由闸) 必须与浏览器真实焦点强同步, 事件竞态丢失时
   按 GetFocus 实测归位 (浏览器持焦 = 搜索框光标让位; 焦点在宿主 = 归还键盘) */
static void WebPost(AiSess* s, const std::string& jsonUtf8);   /* 前置 (实现在"推送"节) */

/* 焦点借出/归还的统一落点 (LostFocus 事件与心跳两路都汇到这里):
   同步宿主键盘路由之外, 归还时给页面推一条 blur — 面板外的宿主点击 (文件列表/
   搜索框/状态栏) WebView2 根本收不到, JS 的 document mousedown 点外收起够不着,
   不推这条, 权限下拉/用量浮层/右键菜单在点击面板外后就一直挂着 */
static void WebFocusApply(AiSess* s, bool has) {
    AiWebCtx* w = (AiWebCtx*)s->web;
    if (!w || !HOST_PANEL_OK || !g_host) return;
    if (has == w->focusBorrowed) return;   /* 值不变不重发 */
    w->focusBorrowed = has;
    g_host->PanelSetFocus(g_ctx, s->tok, has ? 1 : 0);
    if (!has) {
        picojson::object b;
        b["t"] = picojson::value("blur");
        WebPost(s, picojson::value(b).serialize());
    }
}

static void WebFocusTick(AiSess* s) {
    if (!s || !s->inUse || !s->web || !HOST_PANEL_OK || !g_host) return;
    AiWebCtx* w = (AiWebCtx*)s->web;
    if (!w || !w->hwnd) return;
    HWND f = GetFocus();
    WebFocusApply(s, f && IsChild(w->hwnd, f));
}

static LRESULT CALLBACK AiWebChildProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;   /* 底色由 WebView2 DefaultBackgroundColor 出, 免白闪 */
        case WM_TIMER:
            if (wParam == 1) { WebFocusTick((AiSess*)GetWindowLongPtrW(hwnd, GWLP_USERDATA)); return 0; }
            break;
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            break;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void WebInit() {
    HMODULE mod = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&WebInit, &mod))
        return;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);   /* 红线: 缺它注册静默失败 */
    wc.lpfnWndProc = AiWebChildProc;
    wc.hInstance = mod;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"XjsAiWebChild";
    RegisterClassExW(&wc);   /* 已存在 = 忽略 */
}

/* 面板矩形 (宿主 PanelGetRect; 失败 = 会话已失效) */
static bool WebRectOf(AiSess* s, int* x, int* y, int* w, int* h) {
    if (!HOST_RECT_OK || !g_host) return false;
    void* hwnd = NULL;
    return g_host->PanelGetRect(g_ctx, s->tok, &hwnd, x, y, w, h) == XJS_PLUGIN_OK;
}

/* 页面缩放 = scale ÷ DPI 缩放 (WebView2 已按父窗 DPI 自动缩放, 只补页面缩放档) */
static void WebApplyZoom(AiSess* s) {
    AiWebCtx* w = (AiWebCtx*)s->web;
    if (!w || !w->ctrl || !w->hwnd) return;
    UINT dpi = GetDpiForWindow(w->hwnd);
    if (!dpi) dpi = 96;
    float zoom = s->scale > 0 ? s->scale * 96.0f / (float)dpi : 1.0f;
    if (zoom < 0.25f) zoom = 0.25f;
    if (zoom > 4.0f) zoom = 4.0f;
    w->ctrl->put_ZoomFactor(zoom);   /* 控制器基接口成员 (ICoreWebView2Controller) */
}

void WebSessionRect(AiSess* s) {
    AiWebCtx* w = (AiWebCtx*)s->web;
    if (!w || !w->hwnd || !IsWindow(w->hwnd)) return;
    int x, y, cw, ch;
    if (!WebRectOf(s, &x, &y, &cw, &ch)) return;
    SetWindowPos(w->hwnd, NULL, x, y, cw, ch, SWP_NOZORDER | SWP_NOACTIVATE);
    if (w->ctrl) {
        RECT rc = { 0, 0, cw, ch };
        w->ctrl->put_Bounds(rc);
    }
    WebApplyZoom(s);
}

/* ---- JS 命令 (WebMessageReceived; 实现 WebCommand 在本文件尾部) ---- */
void WebCommand(AiSess* s, const Jv& msg);

/* ---- 事件回调 (持有 AiSess* — g_sess 是静态数组, 地址进程级稳定) ---- */

static void WebPushBoot(AiSess* s);   /* 前置 */

/* COM 回调小件: 真引用计数 (登记给 WebView2, 注销/释放时归还) */
#define WEB_HANDLER_BEGIN(Ifc) \
    ULONG ref = 1; \
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override { \
        if (!pp) return E_POINTER; \
        if (riid == __uuidof(IUnknown) || riid == __uuidof(Ifc)) { \
            *pp = (Ifc*)this; \
            AddRef(); \
            return S_OK; \
        } \
        *pp = NULL; \
        return E_NOINTERFACE; \
    } \
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); } \
    ULONG STDMETHODCALLTYPE Release() override { \
        ULONG r = InterlockedDecrement(&ref); \
        if (!r) delete this; \
        return r; \
    }

struct WebMsgHandler : ICoreWebView2WebMessageReceivedEventHandler {
    AiSess* s;
    explicit WebMsgHandler(AiSess* sess) : s(sess) {}
    WEB_HANDLER_BEGIN(ICoreWebView2WebMessageReceivedEventHandler)
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) override {
        (void)sender;
        if (!args || !s->web || ((AiWebCtx*)s->web)->web == NULL) return S_OK;
        LPWSTR json = NULL;
        if (SUCCEEDED(args->get_WebMessageAsJson(&json)) && json) {
            Jv msg = JsonParseW(json);
            if (msg.t == 5) WebCommand(s, msg);
            CoTaskMemFree(json);
        }
        return S_OK;
    }
};

struct WebFocusHandler : ICoreWebView2FocusChangedEventHandler {
    AiSess* s;
    explicit WebFocusHandler(AiSess* sess) : s(sess) {}
    WEB_HANDLER_BEGIN(ICoreWebView2FocusChangedEventHandler)
    /* 挂在控制器的 GotFocus/LostFocus 上 (sender = 控制器) */
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2Controller* sender, IUnknown* args) override {
        (void)sender; (void)args;
        /* 浏览器真实焦点 ↔ 宿主键盘路由/双光标防线 (plugPanelKey) 同步:
           聚焦 = 借键盘 (搜索框让路), 失焦 = 归还 + 通知 JS 收瞬态弹层 */
        AiWebCtx* w = (AiWebCtx*)s->web;
        if (!w || !w->hwnd) return S_OK;
        bool has = w->hwnd && GetFocus() && IsChild(w->hwnd, GetFocus());
        WebFocusApply(s, has);
        return S_OK;
    }
};

/* 外域导航/新窗口一律掐掉 (CSP 之外的第二道; 外链走 openurl 命令; data: = NavigateToString 自身) */
struct WebNavHandler : ICoreWebView2NavigationStartingEventHandler {
    WEB_HANDLER_BEGIN(ICoreWebView2NavigationStartingEventHandler)
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender, ICoreWebView2NavigationStartingEventArgs* args) override {
        if (!args) return S_OK;
        LPWSTR uri = NULL;
        bool allow = false;
        if (SUCCEEDED(args->get_Uri(&uri)) && uri) {
            allow = wcsncmp(uri, L"data:", 5) == 0 || wcscmp(uri, L"about:blank") == 0;
            CoTaskMemFree(uri);
        }
        if (!allow) args->put_Cancel(TRUE);
        return S_OK;
    }
};

struct WebNewWinHandler : ICoreWebView2NewWindowRequestedEventHandler {
    WEB_HANDLER_BEGIN(ICoreWebView2NewWindowRequestedEventHandler)
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender, ICoreWebView2NewWindowRequestedEventArgs* args) override {
        (void)sender;
        if (args) args->put_Handled(TRUE);
        return S_OK;
    }
};

struct WebCtrlHandler : ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
    AiSess* s;
    AiWebCtx* ctx;
    explicit WebCtrlHandler(AiSess* sess) : s(sess), ctx((AiWebCtx*)sess->web) {}
    WEB_HANDLER_BEGIN(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Controller* ctrl) override {
        /* 会话可能在异步创建期间关闭 (s->web 换成/清成 NULL = 放弃)。
         * 回调交付 = 借用引用 (Invoke 返回 loader 即释放): 接受时 AddRef 自留,
         * 放弃时只 Close 不 Release (没有自留就没有可放) — 曾裸存借用的控制器/
         * 环境, Invoke 一返回对象即析构, 下一次 put_Bounds/虚表调用即 UAF 崩溃。 */
        if (!s->inUse || (AiWebCtx*)s->web != ctx) {
            if (ctrl) ctrl->Close();
            return S_OK;
        }
        if (FAILED(result) || !ctrl || !ctx || ctx->hwnd == NULL) {
            g_noRuntime = true;   /* 创建失败按运行时缺失口径收敛 (不再重试) */
            return S_OK;
        }
        ctrl->AddRef();
        ctx->ctrl = ctrl;
        ctrl->get_CoreWebView2(&ctx->web);   /* getter 自带引用 */
        if (!ctx->web) return S_OK;
        /* 设置: 关 devtools/缩放快捷键/状态条 (右键编辑菜单保留 = 选区复制的入口) */
        ICoreWebView2Settings* st = NULL;
        if (SUCCEEDED(ctx->web->get_Settings(&st)) && st) {
            st->put_AreDevToolsEnabled(FALSE);
            st->put_IsZoomControlEnabled(FALSE);
            st->put_IsStatusBarEnabled(FALSE);
            st->put_IsBuiltInErrorPageEnabled(FALSE);
            st->Release();
        }
        /* 底色 = 皮肤背景 (免白闪) */
        ICoreWebView2Controller2* c2 = NULL;
        if (SUCCEEDED(ctrl->QueryInterface(__uuidof(ICoreWebView2Controller2), (void**)&c2)) && c2) {
            COREWEBVIEW2_COLOR col;
            col.A = 255;
            col.R = s->cBg.GetR();
            col.G = s->cBg.GetG();
            col.B = s->cBg.GetB();
            c2->put_DefaultBackgroundColor(col);
            c2->Release();
        }
        /* 桥: JS→C++ 命令 + 焦点同步 (GotFocus/LostFocus 各挂一份) + 导航/新窗拦截 */
        ctx->web->add_WebMessageReceived(new WebMsgHandler(s), &ctx->msgTok);
        ctx->msgHooked = true;
        ctrl->add_GotFocus(new WebFocusHandler(s), &ctx->focusTok);
        ctrl->add_LostFocus(new WebFocusHandler(s), &ctx->focusTok2);
        ctx->focusHooked = true;
        ctx->web->add_NavigationStarting(new WebNavHandler(), &ctx->navTok);
        ctx->navHooked = true;
        ctx->web->add_NewWindowRequested(new WebNewWinHandler(), &ctx->newTok);
        ctx->newHooked = true;
        /* 定位 + 首页 (控制器就绪才亮子窗口 — 免白底闪帧) */
        WebSessionRect(s);
        ShowWindow(ctx->hwnd, SW_SHOWNOACTIVATE);
        ctrl->put_IsVisible(TRUE);
        ctx->web->NavigateToString(AiWebUiHtml());
        return S_OK;
    }
};

static void WebCreateControllerFor(AiSess* s) {
    AiWebCtx* w = (AiWebCtx*)s->web;
    if (!w || !w->hwnd || w->ctrl || !g_webEnv) return;
    g_webEnv->CreateCoreWebView2Controller(w->hwnd, new WebCtrlHandler(s));
}

void WebSessionCreate(AiSess* s) {
    if (s->web) return;
    if (!HOST_RECT_OK || !g_host) return;
    void* parent = NULL;
    int x, y, cw, ch;
    if (g_host->PanelGetRect(g_ctx, s->tok, &parent, &x, &y, &cw, &ch) != XJS_PLUGIN_OK || !parent) return;
    if (!g_noRuntime && !WebEnsureEnv() && !g_envPending) {
        /* 宿主 Toast 唯一保留点: 面板没起来 = 没有页面, 页内 toast 无处可画 */
        g_host->Toast(g_ctx, s->tok, "本机缺少 WebView2 运行时 (系统组件), AI 助手无法打开", XJS_PLUGIN_TOAST_ERROR);
        return;
    }
    HMODULE mod = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&WebInit, &mod);
    AiWebCtx* w = new AiWebCtx();
    w->hwnd = CreateWindowExW(0, L"XjsAiWebChild", L"", WS_CHILD | WS_CLIPCHILDREN,
                              x, y, cw, ch, (HWND)parent, NULL, mod, NULL);
    if (!w->hwnd) {
        delete w;
        return;
    }
    s->web = w;   /* 建好即挂 (环境在途时由 WebCreateControllersPending 补建控制器) */
    SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, (LONG_PTR)s);
    SetTimer(w->hwnd, 1, 250, NULL);   /* 焦点对账心跳 (WebFocusTick) */
    if (g_webEnv) WebCreateControllerFor(s);
}

void WebSessionDestroy(AiSess* s) {
    AiWebCtx* w = (AiWebCtx*)s->web;
    s->web = NULL;
    s->bootDone = false;
    s->syncN = -1;
    s->syncStamp = -1;
    if (!w) return;
    if (w->web) {
        if (w->msgHooked) w->web->remove_WebMessageReceived(w->msgTok);
        if (w->navHooked) w->web->remove_NavigationStarting(w->navTok);
        if (w->newHooked) w->web->remove_NewWindowRequested(w->newTok);
        w->web->Release();
    }
    if (w->ctrl) {
        if (w->focusHooked) {
            w->ctrl->remove_GotFocus(w->focusTok);
            w->ctrl->remove_LostFocus(w->focusTok2);
        }
        w->ctrl->Close();   /* 摘掉浏览器子 HWND (同步), 再收控制器 */
        w->ctrl->Release();
    }
    if (w->hwnd && IsWindow(w->hwnd)) DestroyWindow(w->hwnd);
    delete w;
}

void WebShutdown() {
    for (auto& s : g_sess)
        if (s.web) WebSessionDestroy(&s);
    if (g_webEnv) { g_webEnv->Release(); g_webEnv = NULL; }
    g_envPending = false;
}

/* ==================== 推送 (C++ → JS) ==================== */

static void WebPost(AiSess* s, const std::string& jsonUtf8) {
    AiWebCtx* w = (AiWebCtx*)s->web;
    if (!w || !w->web) return;
    std::wstring j = W8(jsonUtf8.c_str());
    w->web->PostWebMessageAsJson(j.c_str());
}

/* 页面内 Toast: 面板区域被浏览器真子窗盖住, 宿主 Toast 画不进来 — 面板打开期间的
 * 提示一律推给页面画 (kind = XJS_PLUGIN_TOAST_*, 前端按 0info/1ok/2warn/3err 着色) */
void WebToast(AiSess* s, const char* utf8, int kind) {
    if (!s->web || !s->bootDone) return;
    picojson::object o;
    o["t"] = picojson::value("toast");
    o["msg"] = JS(W8(utf8));
    o["k"] = JN(kind >= XJS_PLUGIN_TOAST_INFO && kind <= XJS_PLUGIN_TOAST_ERROR
                    ? kind : XJS_PLUGIN_TOAST_WARN);
    WebPost(s, picojson::value(o).serialize());
}

/* 作业过程状态条 (重试/自愈中; worker 写, 变化才推) */
static std::wstring JobNoteOf(AiSess* s) {
    if (!s->job) return std::wstring();
    EnterCriticalSection(&s->job->cs);
    std::wstring n = s->job->note;
    LeaveCriticalSection(&s->job->cs);
    return n;
}

static picojson::value WebStatusValue(AiSess* s) {
    int phase = 0;
    if (s->job) {
        EnterCriticalSection(&s->job->cs);
        phase = s->job->phase;
        LeaveCriticalSection(&s->job->cs);
    }
    picojson::object o;
    o["sending"] = JB(s->sending);
    o["net"] = JN(s->netStatus);
    o["phase"] = JN(phase);
    std::wstring note = JobNoteOf(s);
    if (!note.empty()) o["note"] = JS(note);
    return picojson::value(o);
}

/* 单条消息 → JSON 值 (msgs/last 共用; html=气泡, t=原始 Markdown 供"复制",
   reason=原始推理文本由前端渲染折叠块) */
static picojson::value WebMsgValue(AiSess* s, const AiMsg& m, int mi, bool thinking, bool withHtml) {
    (void)s;
    (void)mi;
    (void)thinking;
    picojson::object o;
    o["r"] = JN(m.role);
    if (withHtml) {
        std::wstring html;
        MsgHtmlOf(s, m, mi, false, &html);
        o["html"] = JS(html);
    }
    if (m.role == 1 && !m.text.empty())   /* 原始 Markdown: 复制按钮的事实源 (渲染 HTML 抽文本会丢块级换行) */
        o["t"] = JS(m.text);
    if (!m.reason.empty()) o["reason"] = JS(m.reason);
    if (m.err) o["err"] = JB(true);
    /* empty = 没有正文 (推理不算正文): 过程区只画推理行 — 若按旧口径 (正文+推理都空才算
       empty), 推理轮的 &nbsp; 占位气泡会照渲染, 过程面板里每轮跟一条空行 (2026-09-25 实锤) */
    if (m.role == 1 && m.text.empty()) o["empty"] = JB(true);
    if (m.role == 0)
        o["q"] = JS(m.text.size() > 200 ? m.text.substr(0, 200) : m.text);
    if (m.role == 2 && !m.steps.empty()) {   /* 步骤状态: 前端聚合头部计数用 (卡面视觉由 MsgHtmlOf 直出) */
        picojson::array steps;
        for (const AiToolStep& st : m.steps) {
            picojson::object so;
            so["state"] = JN(st.state);
            steps.push_back(picojson::value(so));
        }
        o["steps"] = picojson::value(steps);
    }
    return picojson::value(o);
}

void WebMsgObj(AiSess* s, const AiMsg& m, int mi, bool thinking, bool withHtml, std::string* out) {
    *out += WebMsgValue(s, m, mi, thinking, withHtml).serialize();
}

static void WebMsgsPush(AiSess* s) {
    picojson::object o;
    o["t"] = picojson::value("msgs");
    o["cur"] = JN((long long)s->curId);
    picojson::array msgs;
    for (size_t i = 0; i < s->msgs.size(); i++)
        msgs.push_back(WebMsgValue(s, s->msgs[i], (int)i, false, true));
    o["msgs"] = picojson::value(msgs);
    o["st"] = WebStatusValue(s);
    WebPost(s, picojson::value(o).serialize());
    s->syncN = (int)s->msgs.size();
    s->syncStamp = s->pushStamp;
    s->syncText = s->msgs.empty() ? 0 : s->msgs.back().text.size();
    s->syncReason = s->msgs.empty() ? 0 : s->msgs.back().reason.size();
}

static void WebLastPush(AiSess* s) {
    if (s->msgs.empty()) return;
    picojson::object o;
    o["t"] = picojson::value("last");
    o["m"] = WebMsgValue(s, s->msgs.back(), (int)s->msgs.size() - 1, true, true);
    o["st"] = WebStatusValue(s);
    WebPost(s, picojson::value(o).serialize());
    s->syncText = s->msgs.back().text.size();
    s->syncReason = s->msgs.back().reason.size();
}

static void WebStatusPush(AiSess* s) {
    AiWebCtx* w = (AiWebCtx*)s->web;
    picojson::value sv = WebStatusValue(s);
    sv.get<picojson::object>()["t"] = picojson::value("status");
    WebPost(s, sv.serialize());
    if (w) {
        w->stSending = s->sending;
        w->stNet = s->netStatus;
        w->stNote = JobNoteOf(s);
        int phase = 0;
        if (s->job) {
            EnterCriticalSection(&s->job->cs);
            phase = s->job->phase;
            LeaveCriticalSection(&s->job->cs);
        }
        w->stPhase = phase;
    }
}

static void WebUsagePush(AiSess* s) {
    AiWebCtx* w = (AiWebCtx*)s->web;
    picojson::object u;
    u["has"] = JB(s->usageHas);
    u["up"] = JN(s->uPrompt);
    u["uo"] = JN(s->uCompletion);
    u["ut"] = JN(s->uTotal);
    u["uch"] = JN(s->uCacheHit);
    u["lp"] = JN(s->uLastPrompt);
    u["lc"] = JN(s->uLastCompletion);
    u["tps"] = picojson::value(std::round(s->uTokPerSec * 10.0) / 10.0);   /* 0.1 精度 (前端原样显示) */
    picojson::object o;
    o["t"] = picojson::value("usage");
    o["u"] = picojson::value(u);
    WebPost(s, picojson::value(o).serialize());
    if (w) {
        w->uHas = s->usageHas;
        w->uUp = s->uPrompt;
        w->uUo = s->uCompletion;
        w->uUt = s->uTotal;
        w->uUch = s->uCacheHit;
        w->uLp = s->uLastPrompt;
        w->uLc = s->uLastCompletion;
        w->uTps = s->uTokPerSec;
    }
}

static void WebConvsPushOne(AiSess* s) {
    picojson::array convs;
    for (size_t i = g_hist.size(); i-- > 0;) {   /* 最新在前 (显示序) */
        const AiConv& c = g_hist[i];
        picojson::object co;
        co["id"] = JN((long long)c.id);
        co["t"] = JN(c.t);
        co["title"] = JS(c.title);
        convs.push_back(picojson::value(co));
    }
    picojson::object o;
    o["t"] = picojson::value("convs");
    o["convs"] = picojson::value(convs);
    WebPost(s, picojson::value(o).serialize());
    s->histSynced = true;
}

void WebSyncHist() {
    for (auto& s : g_sess)
        if (s.inUse && s.web && s.bootDone) WebConvsPushOne(&s);
}

void WebTouch(AiSess* s) {
    s->pushStamp++;
}

/* JS ready → 会话全量快照 (boot) */
static void WebPushBoot(AiSess* s) {
    picojson::array convs;
    for (size_t i = g_hist.size(); i-- > 0;) {
        const AiConv& c = g_hist[i];
        picojson::object co;
        co["id"] = JN((long long)c.id);
        co["t"] = JN(c.t);
        co["title"] = JS(c.title);
        convs.push_back(picojson::value(co));
    }
    picojson::array msgs;
    for (size_t i = 0; i < s->msgs.size(); i++)
        msgs.push_back(WebMsgValue(s, s->msgs[i], (int)i, false, true));
    picojson::object u;
    u["has"] = JB(s->usageHas);
    u["up"] = JN(s->uPrompt);
    u["uo"] = JN(s->uCompletion);
    u["ut"] = JN(s->uTotal);
    u["uch"] = JN(s->uCacheHit);
    u["lp"] = JN(s->uLastPrompt);
    u["lc"] = JN(s->uLastCompletion);
    u["tps"] = picojson::value(std::round(s->uTokPerSec * 10.0) / 10.0);   /* 0.1 精度 (前端原样显示) */
    picojson::object o;
    o["t"] = picojson::value("boot");
    o["cfg"] = WebCfgValue();
    o["pal"] = WebPalValue(s);
    o["cur"] = JN((long long)s->curId);
    o["convs"] = picojson::value(convs);
    o["msgs"] = picojson::value(msgs);
    o["usage"] = picojson::value(u);
    o["st"] = WebStatusValue(s);
    WebPost(s, picojson::value(o).serialize());
    s->syncN = (int)s->msgs.size();
    s->syncStamp = s->pushStamp;
    s->syncText = s->msgs.empty() ? 0 : s->msgs.back().text.size();
    s->syncReason = s->msgs.empty() ? 0 : s->msgs.back().reason.size();
    s->histSynced = true;
    s->bootDone = true;
    AiWebCtx* w = (AiWebCtx*)s->web;
    if (w) {
        w->stSending = s->sending;
        w->stNet = s->netStatus;
        w->stPhase = -2;   /* 强制下次 status 变化即推 */
        w->stNote = JobNoteOf(s);
        w->uHas = s->usageHas;
        w->uUp = s->uPrompt;
        w->uUo = s->uCompletion;
        w->uUt = s->uTotal;
        w->uUch = s->uCacheHit;
        w->uLp = s->uLastPrompt;
        w->uLc = s->uLastCompletion;
        w->uTps = s->uTokPerSec;
    }
}

/* 泵/命令后同步: 结构性变化 → msgs 全量; 末条流式增长 → last 重推; 状态/用量按变化推 */
void WebSyncSession(AiSess* s) {
    if (!s->web || !s->bootDone) return;
    AiWebCtx* w = (AiWebCtx*)s->web;
    int n = (int)s->msgs.size();
    bool structural = (n != s->syncN) || (s->pushStamp != s->syncStamp);
    if (structural) {
        WebMsgsPush(s);
    } else if (n > 0 && s->msgs[n - 1].role == 1 &&
               (s->msgs[n - 1].text.size() != s->syncText ||
                s->msgs[n - 1].reason.size() != s->syncReason)) {
        WebLastPush(s);   /* 流式中的末条: 部分文本没法增量转 md, 整条重推 (泵 40ms 节流) */
    }
    int phase = 0;
    if (s->job) {
        EnterCriticalSection(&s->job->cs);
        phase = s->job->phase;
        LeaveCriticalSection(&s->job->cs);
    }
    if (!w || w->stSending != s->sending || w->stNet != s->netStatus || w->stPhase != phase ||
        w->stNote != JobNoteOf(s)) {
        WebStatusPush(s);
    }
    if (!w || !w->uHas != !s->usageHas || w->uUp != s->uPrompt || w->uUo != s->uCompletion ||
        w->uUt != s->uTotal || w->uUch != s->uCacheHit || w->uLp != s->uLastPrompt ||
        w->uLc != s->uLastCompletion || w->uTps != s->uTokPerSec) {
        WebUsagePush(s);
    }
}

void WebPushSkin(AiSess* s) {
    if (!s->web) return;
    picojson::object o;
    o["t"] = picojson::value("pal");
    o["pal"] = WebPalValue(s);
    WebPost(s, picojson::value(o).serialize());
}

/* ==================== JS 命令 (JS → C++) ==================== */

static void CfgBroadcast() {   /* 配置全进程生效: 推给全部活跃会话 (状态点/分段控件跟手) */
    for (auto& ss : g_sess) {
        if (!ss.inUse || !ss.web) continue;
        picojson::object o;
        o["t"] = picojson::value("cfg");
        o["cfg"] = WebCfgValue();
        WebPost(&ss, picojson::value(o).serialize());
        WebSyncSession(&ss);   /* 完备态变化影响状态点 */
    }
}

/* ==================== 捐赠二维码 (get_donate_qr 工具 + 对话页渲染) ====================
 * 二维码图片随包发布在 exe 根目录 (donate-alipay.jpg / donate-wechat.png); 插件 DLL 在
 * plugins\ai-assistant\ 下, 从自身模块路径回推 exe 根 (大小写不敏感找 "\plugins\" 段)。
 * 读取走宿主表 ReadFile (file.read 权限, 任意线程)。图片本体从不进对话通道: 工具只把
 * 引用语法教给模型, md 渲染层 (MD_SPAN_IMG 拦截 xjs://donate?kind=..) 在此取缓存 data URL
 * 换成真图 — base64 进程内缓存一次, 工作者(工具)与 UI(渲染)两线程都会摸 → SRWLOCK 护。 */

static std::wstring DonateB64(const std::string& raw) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::wstring out;
    out.reserve((raw.size() + 2) / 3 * 4 + 4);
    size_t i = 0;
    for (; i + 2 < raw.size(); i += 3) {
        unsigned v = ((unsigned char)raw[i] << 16) | ((unsigned char)raw[i + 1] << 8) | (unsigned char)raw[i + 2];
        out += T[(v >> 18) & 63];
        out += T[(v >> 12) & 63];
        out += T[(v >> 6) & 63];
        out += T[v & 63];
    }
    if (i + 1 == raw.size()) {   /* 剩 1 字节: 2 码 + "==" */
        unsigned v = (unsigned char)raw[i] << 16;
        out += T[(v >> 18) & 63];
        out += T[(v >> 12) & 63];
        out += L"==";
    } else if (i + 2 == raw.size()) {   /* 剩 2 字节: 3 码 + "=" */
        unsigned v = ((unsigned char)raw[i] << 16) | ((unsigned char)raw[i + 1] << 8);
        out += T[(v >> 18) & 63];
        out += T[(v >> 12) & 63];
        out += T[(v >> 6) & 63];
        out += L'=';
    }
    return out;
}

/* 整文件读入 → data URL; 失败 = false (文件缺失/超限) */
static bool DonateReadDataUrl(const std::wstring& path, const wchar_t* mime, std::wstring* out) {
    if (!g_host) return false;
    std::string raw;
    char chunk[65536];
    long long off = 0;
    for (;;) {
        int eof = 0;
        int n = g_host->ReadFile(g_ctx, U8(path).c_str(), off, (int)sizeof(chunk),
                                 chunk, (int)sizeof(chunk), &eof);
        if (n <= 0) break;
        raw.append(chunk, (size_t)n);
        off += n;
        if (eof) break;
        if (raw.size() > 8u * 1024 * 1024) return false;   /* 8MB 兜底防失控 */
    }
    if (raw.empty()) return false;
    *out = L"data:";
    *out += mime;
    *out += L";base64,";
    *out += DonateB64(raw);
    return true;
}

std::wstring DonateQrDataUrl(int kind) {   /* 0=微信 1=支付宝; 空串 = 不可用 (含文件缺失, 负缓存) */
    static SRWLOCK lock = SRWLOCK_INIT;
    static std::wstring urls[2];
    static bool tried[2] = { false, false };
    AcquireSRWLockExclusive(&lock);
    if (!tried[kind]) {
        tried[kind] = true;
        HMODULE hm = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&DonateQrDataUrl, &hm);
        wchar_t dll[MAX_PATH] = {};
        if (hm && GetModuleFileNameW(hm, dll, MAX_PATH)) {
            std::wstring p = dll;
            std::wstring low = p;
            for (auto& ch : low) ch = (wchar_t)towlower(ch);
            size_t plug = low.rfind(L"\\plugins\\");
            if (plug != std::wstring::npos) {
                DonateReadDataUrl(p.substr(0, plug) + (kind == 0 ? L"\\donate-wechat.png" : L"\\donate-alipay.jpg"),
                                  kind == 0 ? L"image/png" : L"image/jpeg", &urls[kind]);
            }
        }
    }
    std::wstring r = urls[kind];
    ReleaseSRWLockExclusive(&lock);
    return r;
}

/* ---- 点击链接的路径解析 (前端 linkifyPaths 的兜底, 2026-09-25) ----
 * 前端把路径后的词贪婪并进链接 (空格粘连, "决战! 碧游村4K")、句尾闭合标点剥出链接外
 * ("美人鱼 (2016)" 的 ")"), 链接串因此可能比真路径长或短一截。这里按
 * "整串 → 补一个被剥的闭合标点 → 按空格从尾部逐段回退(每层再试补标点)" 找最长真实存在者;
 * 存在性 = 文件系统为准, 命中后能进索引 (GetFileIdByPath) 就给 fid 走宿主 OpenFile (打开行为生效)。
 * 返回 ≥0 = 索引 FileId; -1 = 文件系统存在但不在索引 (outPath=可用路径); -2 = 全不中。 */
static int ResolveClickablePath(xjs_engine* eng, const std::wstring& raw, std::wstring* outPath)
{
    static const wchar_t* const closers[] = {
        L")", L"）", L"]", L"】", L"」", L"』", L"》", L"'", L"\"", L"!", L"。"
    };
    auto hit = [&](const std::wstring& p, int* fid)->bool {
        bool drive = p.size() >= 4 && p[1] == L':';           /* "C:\a" 起 */
        bool unc   = p.size() >= 5 && p[0] == L'\\' && p[1] == L'\\';
        if (!drive && !unc) return false;                     /* 拒绝 "C:" 一类退化候选 */
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
        *fid = -1;
        if (eng) {
            int f = xjs_db_GetFileIdByPath(eng, U8(p).c_str());
            if (f >= 0) *fid = f;
        }
        *outPath = p;                                         /* 命中即落账可用路径 */
        return true;
    };
    auto trailPunct = [](wchar_t ch)->bool {
        return ch == L'.' || ch == L',' || ch == L';' || ch == L':' || ch == L'!' || ch == L'?' ||
               ch == L'\'' || ch == L'"' || ch == 0x2026 /* … */ ||
               ch == L'，' || ch == L'。' || ch == L'；' || ch == L'！' || ch == L'？';
    };
    std::wstring cur = raw;
    for (;;) {
        /* 每层两轮: 0=原样 1=剥尾部普通标点 (空格断词把 "F:\a\b," 连逗号吞进链接的余量);
         * 每轮先试整串再试补一个闭合标点 (前端剥离的 ")" 真是名字一部分时还原)。 */
        int f = -999;
        bool done = false;
        for (int round = 0; round < 2 && !done; round++) {
            std::wstring p = cur;
            if (round == 1)
                while (!p.empty() && trailPunct(p.back())) p.pop_back();
            if (p.empty()) continue;
            if (hit(p, &f)) { done = true; break; }
            for (const wchar_t* cl : closers) {
                if (hit(p + cl, &f)) { done = true; break; }
            }
        }
        if (done) return f;
        size_t sp = cur.find_last_of(L' ');
        if (sp == std::wstring::npos) break;
        std::wstring head = cur.substr(0, sp);
        while (!head.empty() && (head.back() == L' ' || head.back() == L'\t')) head.pop_back();
        if (head.empty()) break;
        cur = head;
    }
    return -2;
}

void WebCommand(AiSess* s, const Jv& msg) {
    std::wstring c = msg.S(L"c");
    if (c == L"ready") {
        WebPushBoot(s);
        return;
    }
    if (!s->bootDone) return;   /* ready 之前的命令一律忽略 */
    if (c == L"send") {
        SendCurrent(s, msg.S(L"text"));
        return;
    }
    if (c == L"stop") {
        AbortSend(s);
        return;
    }
    if (c == L"close") {
        if (HOST_PANEL_OK && g_host) g_host->PanelClose(g_ctx, s->tok);
        return;
    }
    if (c == L"profSave") {
        /* 保存表单到活动档案 (一条都没有时 = 建第一条再用)。
         * 密钥留空 = 保留已存密钥 — 前端永不持有明文 (面板密钥框恒空),
         * 若照"表单为权威"处理, 切到别的档案随手一存就会把密钥抹掉。 */
        AiProfile* p = CfgActive();
        if (!p) {
            AiProfile np;
            np.id = CfgGenProfileId();
            g_cfg.profiles.push_back(std::move(np));
            p = CfgActive();
            if (!p) return;
        }
        const Jv* v;
        if ((v = msg.Get(L"name")) != NULL && v->t == 3) p->name = TrimW(v->str);
        if ((v = msg.Get(L"url")) != NULL && v->t == 3) p->baseUrl = TrimW(v->str);
        if ((v = msg.Get(L"model")) != NULL && v->t == 3) p->model = TrimW(v->str);
        if ((v = msg.Get(L"key")) != NULL && v->t == 3 && !TrimW(v->str).empty()) p->apiKey = TrimW(v->str);
        if ((v = msg.Get(L"ctx")) != NULL && v->t == 2) p->ctx = CfgClampTok(v->num);
        if ((v = msg.Get(L"max")) != NULL && v->t == 2) p->maxOut = CfgClampTok(v->num);
        if ((v = msg.Get(L"reasoning")) != NULL && v->t == 1) g_cfg.reasoning = v->b;
        CfgApplyActive();
        CfgSave();
        CfgBroadcast();
        return;
    }
    if (c == L"profNew") {   /* 新建 / 复制档案并切过去 (新建的目的就是要用它) */
        if ((int)g_cfg.profiles.size() >= AiProfileMax) {
            WebToast(s, "最多只能保存 50 个模型", XJS_PLUGIN_TOAST_WARN);
            return;
        }
        AiProfile np;
        np.id = CfgGenProfileId();
        const Jv* d = msg.Get(L"dup");
        if (d && d->t == 1 && d->b) {   /* 复制: 密钥不出宿主, 在 C++ 侧随档案一起搬 */
            const AiProfile* src = CfgActive();
            if (src) {
                np.name = CfgDisplayName(src) + L" 副本";
                np.baseUrl = src->baseUrl;
                np.apiKey = src->apiKey;
                np.model = src->model;
                np.ctx = src->ctx;
                np.maxOut = src->maxOut;
            }
        }
        g_cfg.profiles.push_back(std::move(np));
        g_cfg.activeId = g_cfg.profiles.back().id;
        CfgApplyActive();
        CfgSave();
        CfgBroadcast();
        return;
    }
    if (c == L"profDel") {   /* 删除 (两步确认在前端); activeId 失效自动回落第一条 */
        std::wstring id = msg.S(L"id");
        if (id.empty()) {
            AiProfile* p = CfgActive();
            if (p) id = p->id;
        }
        for (size_t i = 0; i < g_cfg.profiles.size(); i++) {
            if (g_cfg.profiles[i].id != id) continue;
            g_cfg.profiles.erase(g_cfg.profiles.begin() + i);
            break;
        }
        CfgApplyActive();
        CfgSave();
        CfgBroadcast();
        return;
    }
    if (c == L"profActive") {   /* 切换当前档案 (切换是离散动作, 立即生效并持久化) */
        std::wstring id = msg.S(L"id");
        for (auto& p : g_cfg.profiles) {
            if (p.id != id) continue;
            g_cfg.activeId = id;
            break;
        }
        CfgApplyActive();
        CfgSave();
        CfgBroadcast();
        return;
    }
    if (c == L"policy") {
        const Jv* v = msg.Get(L"v");
        if (v && v->t == 2 && v->num >= 0 && v->num <= 3) {
            g_cfg.filePolicy = (int)v->num;
            CfgSave();
            if (s->job) InterlockedExchange(&s->job->policy, g_cfg.filePolicy);
            CfgBroadcast();
        }
        return;
    }
    if (c == L"epolicy") {   /* 命令执行权限档 (0 禁用 2 询问 3 允许; 1 不是合法档) */
        const Jv* v = msg.Get(L"v");
        if (v && v->t == 2 && v->num >= 0 && v->num <= 3 && v->num != 1) {
            g_cfg.execPolicy = (int)v->num;
            CfgSave();
            if (s->job) InterlockedExchange(&s->job->execPolicy, g_cfg.execPolicy);
            CfgBroadcast();
        }
        return;
    }
    if (c == L"pallow" || c == L"pdeny") {   /* 文件策略询问卡: 允许并继续 / 保持拒绝 (只碰文件卡, 不碰命令卡) */
        bool allow = c == L"pallow";
        if (allow) {
            g_cfg.filePolicy = 3;
            CfgSave();
            if (s->job) InterlockedExchange(&s->job->policy, 3);
            CfgBroadcast();
        }
        for (auto& m : s->msgs) {
            if (m.role != 2) continue;
            for (auto& st : m.steps) {
                if (st.state == 4 && st.kind != 11) {
                    st.state = 3;
                    st.err = allow ? L"已允许文件操作, 本次回答后生效" : L"用户保持拒绝";
                }
            }
        }
        WebTouch(s);
        WebSyncSession(s);
        return;
    }
    if (c == L"eallow" || c == L"edeny") {
        /* 命令执行确认卡: worker 正**挂起**在该调用上等裁决 (dsh approval 口径) —
           这里只落标志, 卡片状态 (执行中/失败) 由 worker 推进并经泵同步。
           一次只有一张询问卡 (挂起期间模型无法再发调用), 裸标志即可;
           作业已结束的旧卡点按钮 = 无收件人, 自然无效。execPolicy 档位不变
           (要"不再问"请用户自己切「允许」)。 */
        AiJob* aj = s->job;
        if (aj) {
            if (c == L"eallow") {
                EnterCriticalSection(&aj->cs);
                InterlockedExchange(&aj->execGrant, 1);
                LeaveCriticalSection(&aj->cs);
            } else {
                InterlockedExchange(&aj->execDeny, 1);
            }
        }
        return;
    }
    if (c == L"adj") {
        /* 待应用的调整: 用户逐项/全部 应用|忽略 (卡片按钮 → 这里 UI 线程执行宿主调用)。
         * 状态改在会话份步骤上 — 泵的步骤比对不含 adj 字段, 不会被 worker 镜像回写。
         * 窗口令牌应用时才解析 (提案只存名): 目标窗已关 = 该项 ✕ 失败, 不悬垂。 */
        const Jv* mv = msg.Get(L"mi");
        const Jv* sv = msg.Get(L"si");
        const Jv* iv = msg.Get(L"ii");
        std::wstring act = msg.S(L"act");
        if (!mv || mv->t != 2 || !sv || sv->t != 2 || !iv || iv->t != 2) return;
        int mi = (int)mv->num, si = (int)sv->num, ii = (int)iv->num;
        bool all = (act == L"applyAll" || act == L"ignoreAll");
        bool doApply = (act == L"apply" || act == L"applyAll");
        if (mi < 0 || mi >= (int)s->msgs.size()) return;
        AiMsg& cm = s->msgs[mi];
        if (cm.role != 2 || si < 0 || si >= (int)cm.steps.size()) return;
        AiToolStep& st = cm.steps[si];
        if (st.adj.items.empty()) return;
        for (int i = 0; i < (int)st.adj.items.size(); i++) {
            if (!all && i != ii) continue;
            AiAdjustItem& it = st.adj.items[i];
            if (it.state != 0) continue;   /* 已处理的项点旧按钮 = 无操作 (幂等防线) */
            if (!doApply) { it.state = 2; it.err.clear(); continue; }
            if (!g_host) { it.state = 3; it.err = L"宿主不可用"; continue; }
            std::wstring errw;
            int rc = XJS_PLUGIN_OK;
            if (st.adj.kind == 0) {
                if (!g_api.settingsSet) { it.state = 3; it.err = L"宿主不支持该操作"; continue; }
                long long tok = AgentUiWindowToken(st.adj.win, (long long)s->tok, &errw);
                if (!errw.empty()) { it.state = 3; it.err = errw; continue; }
                rc = g_api.settingsSet(g_ctx, (XjsWindowToken)tok, it.json.c_str());
            } else if (st.adj.kind == 1) {
                if (!g_api.globalSet) { it.state = 3; it.err = L"宿主不支持该操作"; continue; }
                rc = g_api.globalSet(g_ctx, it.json.c_str());
            } else if (st.adj.kind == 2) {
                if (!g_api.windowCmd) { it.state = 3; it.err = L"宿主不支持该操作"; continue; }
                long long tok = AgentUiWindowToken(st.adj.win, (long long)s->tok, &errw);
                if (!errw.empty()) { it.state = 3; it.err = errw; continue; }
                rc = g_api.windowCmd(g_ctx, (XjsWindowToken)tok, it.json.c_str());
            } else {   /* kind 3: 新建窗口 (json = "档案名|继承窗名") */
                if (!g_api.windowCreate) { it.state = 3; it.err = L"宿主不支持该操作"; continue; }
                std::wstring pj = W8(it.json.c_str());
                std::wstring profile = pj, inherit;
                size_t bar = pj.find(L'|');
                if (bar != std::wstring::npos) { profile = pj.substr(0, bar); inherit = pj.substr(bar + 1); }
                long long inh = AgentUiWindowToken(inherit, 0, &errw);
                if (!errw.empty()) { it.state = 3; it.err = errw; continue; }
                XjsWindowToken tokOut = 0;
                rc = g_api.windowCreate(g_ctx, (XjsWindowToken)inh,
                                        profile.empty() ? NULL : U8(profile).c_str(), &tokOut);
            }
            if (rc == XJS_PLUGIN_OK) { it.state = 1; it.err.clear(); }
            else { it.state = 3; it.err = AgentApiErrText(rc); }
        }
        WebTouch(s);
        WebSyncSession(s);   /* 即时重推 msgs (卡片按钮态刷新); 同 pallow 口径 */
        SessSaveConv(s);     /* 当前会话先 upsert 进 g_hist (否则 HistSave 落的是旧副本) */
        HistSave();
        return;
    }
    if (c == L"retry") {   /* 重试本轮: 截断到该轮提问之前再重发 (提问由 SendCurrent 重挂;
                              提问之后的工具卡片与回答一并丢弃 — 与参考实现 retryAiTurn 同语义) */
        if (s->sending) return;
        int lastA = -1, lastU = -1;
        for (int i = (int)s->msgs.size() - 1; i >= 0; i--)
            if (s->msgs[i].role == 1) { lastA = i; break; }
        if (lastA <= 0) return;
        for (int i = lastA - 1; i >= 0; i--)
            if (s->msgs[i].role == 0) { lastU = i; break; }
        if (lastU < 0) return;
        std::wstring prompt = s->msgs[lastU].text;
        s->msgs.resize(lastU);
        SendCurrent(s, prompt);
        return;
    }
    if (c == L"new") {
        AbortSend(s);
        SessSaveConv(s);
        s->msgs.clear();
        s->usageHas = false;
        s->uPrompt = s->uCompletion = s->uTotal = s->uCacheHit = s->uCacheWrite = 0;
        s->uLastPrompt = s->uLastCompletion = s->uLastCacheHit = 0;
        s->uTokPerSec = 0;
        s->stepBase = 0;
        s->curId = 0;
        WebTouch(s);
        WebSyncSession(s);
        WebSyncHist();   /* 保存后的当前对话可能新进历史 */
        return;
    }
    if (c == L"load") {
        unsigned long long id = (unsigned long long)(msg.Get(L"id") ? msg.Get(L"id")->num : 0);
        int idx = -1;
        for (int i = 0; i < (int)g_hist.size(); i++)
            if (g_hist[i].id == id) { idx = i; break; }
        if (idx < 0) return;
        AbortSend(s);
        SessSaveConv(s);
        s->msgs = g_hist[idx].msgs;
        s->stepBase = (int)s->msgs.size();   /* 恢复的历史卡片不参与任何在途作业的步骤同步 */
        s->usageHas = false;
        s->uPrompt = s->uCompletion = s->uTotal = s->uCacheHit = s->uCacheWrite = 0;
        s->uLastPrompt = s->uLastCompletion = s->uLastCacheHit = 0;
        s->uTokPerSec = 0;
        s->curId = g_hist[idx].id;
        WebTouch(s);
        WebSyncSession(s);
        WebSyncHist();
        return;
    }
    if (c == L"del") {
        unsigned long long id = (unsigned long long)(msg.Get(L"id") ? msg.Get(L"id")->num : 0);
        for (int i = 0; i < (int)g_hist.size(); i++) {
            if (g_hist[i].id != id) continue;
            g_hist.erase(g_hist.begin() + i);
            HistSave();
            if (s->curId == id) { s->curId = 0; s->msgs.clear(); WebTouch(s); WebSyncSession(s); }
            WebSyncHist();
            break;
        }
        return;
    }
    if (c == L"clearHist") {
        g_hist.clear();
        HistSave();
        s->curId = 0;
        s->msgs.clear();
        s->usageHas = false;
        s->uPrompt = s->uCompletion = s->uTotal = s->uCacheHit = s->uCacheWrite = 0;
        s->uLastPrompt = s->uLastCompletion = s->uLastCacheHit = 0;
        s->uTokPerSec = 0;
        WebTouch(s);
        WebSyncSession(s);
        WebSyncHist();
        return;
    }
    if (c == L"copy") {
        const Jv* t = msg.Get(L"text");
        if (t && t->t == 3 && g_host && !t->str.empty())
            g_host->ClipboardSetText(g_ctx, U8(t->str).c_str());
        return;
    }
    if (c == L"openurl") {
        const Jv* u = msg.Get(L"href");
        if (u && u->t == 3 &&
            (u->str.rfind(L"http://", 0) == 0 || u->str.rfind(L"https://", 0) == 0))
            ShellExecuteW(NULL, L"open", u->str.c_str(), NULL, NULL, SW_SHOWNORMAL);
        return;
    }
    /* ---- 可点击交互 (用户点回答里的搜索卡片/文件路径) ----
       search/searchfill = 搜索卡片: 置入搜索词 (+可选切换搜索模式), execute 区分是否立即执行;
       open/reveal = 文件路径: 索引内走宿主 OpenFile (打开行为/资源管理器定位), 索引外插件自开;
       copypath = 复制路径清单里的一条。全部用户主动点击, 不受 filePolicy 门 (那是 AI 自主工具的闸)。 */
    if (c == L"search" || c == L"searchfill") {
        std::wstring text = TrimW(msg.S(L"text"));
        std::wstring mode = TrimW(msg.S(L"mode"));
        if (text.empty() && mode.empty()) return;
        if (!g_host) return;
        int rc = g_host->SearchSetText(g_ctx, s->tok, U8(text).c_str(),
                                       mode.empty() ? NULL : U8(mode).c_str(),
                                       c == L"search" ? 1 : 0);
        if (rc == XJS_PLUGIN_ERR_ARG) {
            WebToast(s, U8(L"未知搜索模式 (" + mode + L"), 已忽略").c_str(), XJS_PLUGIN_TOAST_WARN);
        }
        else if (rc != XJS_PLUGIN_OK)
            WebToast(s, "搜索窗口不可用 (已关闭?)", XJS_PLUGIN_TOAST_WARN);
        return;
    }
    /* 文件动作: 新协议 = 引擎 FileId (模型只输出 ID, 程序按 ID 取路径 — 2026-09-25 用户口径);
     * path 参数 = 旧历史消息里的路径版链接, 继续受理 (载入的历史不重排)。 */
    if (c == L"open" || c == L"reveal") {
        bool isReveal = c == L"reveal";
        xjs_engine* eng = xjs_GetDefaultEngine();
        std::wstring path = TrimW(msg.S(L"path"));
        const Jv* idv = msg.Get(L"id");
        bool hasIdParam = idv != NULL;
        int fid = -1;
        if (idv && idv->t == 2) fid = (int)idv->num;
        else if (!path.empty()) {
            /* 链接带路径: 过解析器 (空格回退取最长存在前缀 / 补被剥的闭合标点);
             * ≥0=索引命中给 fid 走宿主 OpenFile; -1=索引外但文件系统存在, path 已定为可用路径;
             * -2=全不中, path 保持原文走下方 ShellExecute 失败提示 */
            int rf = ResolveClickablePath(eng, path, &path);
            if (rf >= 0) fid = rf;
        }
        int rc = (fid >= 0 && g_host) ? g_host->OpenFile(g_ctx, s->tok, fid, isReveal ? 1 : 0)
                                      : XJS_PLUGIN_ERR_NOTFOUND;
        if (rc != XJS_PLUGIN_OK) {
            if (fid >= 0) {   /* ID 命中但打开失败: 按 ID 反查真路径自理 */
                const char* p = eng ? xjs_db_GetPath(eng, fid) : NULL;
                if (p) path = W8(p);
            }
            if (path.empty()) {
                /* 两类失败分开说: 参数里根本没有有效数字 = 链接本身无效;
                 * 数字有效但库里查无此 ID = 索引重建后 ID 已变 (ID 引用的固有限制) */
                if (hasIdParam && fid < 0)
                    WebToast(s, "链接无效: 没有有效的文件 ID", XJS_PLUGIN_TOAST_WARN);
                else
                    WebToast(s, "文件 ID 已失效 (索引重建后 ID 会变化), 请让 AI 重新搜索这个文件",
                             XJS_PLUGIN_TOAST_WARN);
                return;
            }
            /* 未索引/窗口失效: 插件自开 (SDK 契约: 非索引路径自理) */
            HINSTANCE r = isReveal
                ? ShellExecuteW(NULL, L"open", L"explorer.exe",
                                (L"/select,\"" + path + L"\"").c_str(), NULL, SW_SHOWNORMAL)
                : ShellExecuteW(NULL, L"open", path.c_str(), NULL, NULL, SW_SHOWNORMAL);
            if ((INT_PTR)r <= 32)
                WebToast(s, "打开失败: 路径不存在或无法访问", XJS_PLUGIN_TOAST_WARN);
        }
        return;
    }
    if (c == L"copypath") {
        const Jv* t = msg.Get(L"path");
        std::wstring path = (t && t->t == 3) ? TrimW(t->str) : L"";
        const Jv* idv = msg.Get(L"id");
        if (idv && idv->t == 2) {   /* ID 版: 程序自取真实路径 */
            xjs_engine* eng = xjs_GetDefaultEngine();
            const char* p = eng ? xjs_db_GetPath(eng, (int)idv->num) : NULL;
            if (p) path = W8(p);
        }
        else if (!path.empty())
            ResolveClickablePath(xjs_GetDefaultEngine(), path, &path);   /* 全不中(-2) = 照抄原文复制 */
        if (!path.empty() && g_host)
            g_host->ClipboardSetText(g_ctx, U8(path).c_str());
        return;
    }
}
