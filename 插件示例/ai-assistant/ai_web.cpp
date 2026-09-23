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

std::wstring WebPaletteJson(AiSess* s) {
    AiPal p = PalOf(s);
    /* 键 = ai_web_ui.cpp :root 的 CSS 变量名; 8 位 #rrggbbaa 浏览器原样可解析 */
    std::wstring j = L"{";
    auto add = [&](const wchar_t* k, const std::wstring& v) {
        if (j.size() > 1) j += L",";
        j += std::wstring(L"\"") + k + L"\":" + W8(JsonEscapeUtf8(v).c_str());
    };
    add(L"bg", ColHex(p.bg));
    add(L"panel", ColHex(p.panel));
    add(L"text", ColHex(p.text));
    add(L"dim", ColHex(p.dim));
    add(L"accent", ColHex(p.accent));
    add(L"t3", ColHexA(p.t3));
    add(L"hover", ColHexA(p.hover));
    add(L"divider", ColHexA(p.divider));
    add(L"border", ColHexA(p.border));
    add(L"borderStrong", ColHexA(p.borderStrong));
    add(L"cyan", ColHex(p.cyan));
    add(L"emerald", ColHex(p.emerald));
    add(L"amber", ColHex(p.amber));
    add(L"red", ColHex(p.red));
    add(L"ok", ColHex(p.ok));
    add(L"userAcc", ColHex(p.userAcc));
    j += L"}";
    return j;
}

std::wstring WebCfgJson() {
    std::wstring j = L"{\"url\":";
    j += W8(JsonEscapeUtf8(g_cfg.baseUrl).c_str());
    j += L",\"model\":";
    j += W8(JsonEscapeUtf8(g_cfg.model).c_str());
    j += L",\"hasKey\":";
    j += g_cfg.apiKey.empty() ? L"false" : L"true";
    j += L",\"reasoning\":";
    j += g_cfg.reasoning ? L"true" : L"false";
    j += L",\"policy\":";
    j += std::to_wstring(g_cfg.filePolicy);
    j += L"}";
    return j;
}

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
        case MD_SPAN_IMG: break;   /* 图片降级为 alt 文本 (内层文本照常进正文) */
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
    std::string u8 = U8(text);
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

/* role==2 工具卡片组 (旧 .ai-cmd-entry 口径; 查询展示截 200 字符 — 浏览器端折行,
   卡头不因超长脚本无限增高); state==4 带确认按钮; 样本列表默认收起 (JS 按展开态回放) */
static void StepsHtml(const AiMsg& m, int mi, std::wstring* out) {
    wchar_t b[64];
    for (size_t si = 0; si < m.steps.size(); si++) {
        const AiToolStep& st = m.steps[si];
        swprintf(b, 64, L"<div class=\"step%s\" data-gi=\"%d\">", st.state == 3 ? L" failed" : L"", mi);
        *out += b;
        *out += L"<div class=\"shead\"><span class=\"sbadge\">";
        std::wstring badge = st.kind == 0 ? L"搜索" : st.kind == 1 ? L"打开" : st.kind == 2 ? L"复制" : L"工具";
        HtmlEscape(out, badge);
        *out += L"</span><span class=\"scmd\">";
        std::wstring cmd = (st.kind == 0 && !st.query.empty()) ? st.query
                         : (st.name.empty() ? L"工具" : st.name);
        if (cmd.size() > 200) { cmd.resize(200); cmd += L"…"; }
        HtmlEscape(out, cmd);
        *out += L"</span><span class=\"sst";
        if (st.state == 3 || st.state == 4) *out += L" bad";
        *out += L"\">";
        HtmlEscape(out, StepStatText(st));
        /* 折叠/展开箭头 (点击头部切换; 样本列表默认收起) */
        *out += L"</span><span class=\"sarr glyph\">&#xE70D;</span></div>";
        if (st.state == 4) {
            /* 策略询问: 允许 (转允许并放行后续) / 保持拒绝 */
            *out += L"<div class=\"sask\">";
            HtmlEscape(out, st.err.empty() ? L"等待用户确认文件操作" : st.err);
            *out += L"<br/><span class=\"abtn primary\" data-act=\"authallow\">允许并继续</span>";
            *out += L"<span class=\"abtn\" data-act=\"authdeny\">保持拒绝</span></div>";
        } else if (st.state == 3 && !st.err.empty()) {
            *out += L"<div class=\"sout\">";
            HtmlEscape(out, st.err);
            *out += L"</div>";
        }
        if (!st.top.empty()) {
            *out += L"<div class=\"ssamples\" style=\"display:none\">";
            for (size_t ti = 0; ti < st.top.size(); ti++) {
                wchar_t no[16];
                swprintf(no, 16, L"%d. ", (int)(ti + 1));
                *out += no;
                HtmlEscape(out, st.top[ti]);
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

static LRESULT CALLBACK AiWebChildProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;   /* 底色由 WebView2 DefaultBackgroundColor 出, 免白闪 */
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
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
           聚焦 = 借键盘 (搜索框让路), 失焦 = 归还 — 值不变不重发 */
        AiWebCtx* w = (AiWebCtx*)s->web;
        if (!w || !HOST_PANEL_OK || !g_host) return S_OK;
        bool has = w->hwnd && GetFocus() && IsChild(w->hwnd, GetFocus());
        if (has != w->focusBorrowed) {
            w->focusBorrowed = has;
            g_host->PanelSetFocus(g_ctx, s->tok, has ? 1 : 0);
        }
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

static void WebStatusObj(AiSess* s, std::string* out) {
    int phase = 0;
    if (s->job) {
        EnterCriticalSection(&s->job->cs);
        phase = s->job->phase;
        LeaveCriticalSection(&s->job->cs);
    }
    char b[128];
    snprintf(b, 128, "{\"sending\":%s,\"net\":%d,\"phase\":%d}",
             s->sending ? "true" : "false", s->netStatus, phase);
    *out += b;
}

/* 单条消息 → JSON 对象 (msgs/last 共用; html=气泡, reason=原始推理文本由前端渲染折叠块) */
void WebMsgObj(AiSess* s, const AiMsg& m, int mi, bool thinking, bool withHtml, std::string* out) {
    (void)s;
    (void)mi;
    (void)thinking;
    char head[64];
    snprintf(head, 64, "{\"r\":%d", m.role);
    *out += head;
    if (withHtml) {
        std::wstring html;
        MsgHtmlOf(s, m, mi, false, &html);
        *out += ",\"html\":";
        *out += JsonEscapeUtf8(html);
    }
    if (!m.reason.empty()) {
        *out += ",\"reason\":";
        *out += JsonEscapeUtf8(m.reason);
    }
    if (m.err) *out += ",\"err\":true";
    if (m.role == 1 && m.text.empty() && m.reason.empty()) *out += ",\"empty\":true";
    if (m.role == 0) {
        *out += ",\"q\":";
        std::wstring q = m.text.size() > 200 ? m.text.substr(0, 200) : m.text;
        *out += JsonEscapeUtf8(q);
    }
    *out += "}";
}

static void WebMsgsPush(AiSess* s) {
    std::string j = "{\"t\":\"msgs\",\"cur\":";
    {
        char b[32];
        snprintf(b, 32, "%llu", s->curId);
        j += b;
    }
    j += ",\"msgs\":[";
    for (size_t i = 0; i < s->msgs.size(); i++) {
        if (i) j += ",";
        WebMsgObj(s, s->msgs[i], (int)i, false, true, &j);
    }
    j += "],\"st\":";
    WebStatusObj(s, &j);
    j += "}";
    WebPost(s, j);
    s->syncN = (int)s->msgs.size();
    s->syncStamp = s->pushStamp;
    s->syncText = s->msgs.empty() ? 0 : s->msgs.back().text.size();
    s->syncReason = s->msgs.empty() ? 0 : s->msgs.back().reason.size();
}

static void WebLastPush(AiSess* s) {
    if (s->msgs.empty()) return;
    std::string j = "{\"t\":\"last\",\"m\":";
    WebMsgObj(s, s->msgs.back(), (int)s->msgs.size() - 1, true, true, &j);
    j += ",\"st\":";
    WebStatusObj(s, &j);
    j += "}";
    WebPost(s, j);
    s->syncText = s->msgs.back().text.size();
    s->syncReason = s->msgs.back().reason.size();
}

static void WebStatusPush(AiSess* s) {
    AiWebCtx* w = (AiWebCtx*)s->web;
    std::string j = "{\"t\":\"status\",";
    std::string st;
    WebStatusObj(s, &st);
    j += st.substr(1);   /* 去掉 '{' 并入 */
    j += "}";
    WebPost(s, j);
    if (w) {
        w->stSending = s->sending;
        w->stNet = s->netStatus;
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
    char b[320];
    snprintf(b, 320,
             "{\"t\":\"usage\",\"u\":{\"has\":%s,\"up\":%lld,\"uo\":%lld,\"ut\":%lld,\"uch\":%lld,"
             "\"lp\":%lld,\"lc\":%lld,\"tps\":%.1f}}",
             s->usageHas ? "true" : "false", s->uPrompt, s->uCompletion, s->uTotal, s->uCacheHit,
             s->uLastPrompt, s->uLastCompletion, s->uTokPerSec);
    WebPost(s, b);
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
    std::string j = "{\"t\":\"convs\",\"convs\":[";
    for (size_t i = g_hist.size(); i-- > 0;) {   /* 最新在前 (显示序) */
        const AiConv& c = g_hist[i];
        if (i != g_hist.size() - 1) j += ",";
        char head[96];
        snprintf(head, 96, "{\"id\":%llu,\"t\":%lld,\"title\":", c.id, c.t);
        j += head;
        j += JsonEscapeUtf8(c.title);
        j += "}";
    }
    j += "]}";
    WebPost(s, j);
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
    std::string j = "{\"t\":\"boot\",\"cfg\":";
    j += U8(WebCfgJson());
    j += ",\"pal\":";
    j += U8(WebPaletteJson(s));
    j += ",\"cur\":";
    {
        char b[32];
        snprintf(b, 32, "%llu", s->curId);
        j += b;
    }
    j += ",\"convs\":[";
    for (size_t i = g_hist.size(); i-- > 0;) {
        const AiConv& c = g_hist[i];
        if (i != g_hist.size() - 1) j += ",";
        char head[96];
        snprintf(head, 96, "{\"id\":%llu,\"t\":%lld,\"title\":", c.id, c.t);
        j += head;
        j += JsonEscapeUtf8(c.title);
        j += "}";
    }
    j += "],\"msgs\":[";
    for (size_t i = 0; i < s->msgs.size(); i++) {
        if (i) j += ",";
        WebMsgObj(s, s->msgs[i], (int)i, false, true, &j);
    }
    j += "],\"usage\":{\"has\":";
    j += s->usageHas ? "true" : "false";
    {
        char b[160];
        snprintf(b, 160, ",\"up\":%lld,\"uo\":%lld,\"ut\":%lld,\"uch\":%lld,\"lp\":%lld,\"lc\":%lld,\"tps\":%.1f}",
                 s->uPrompt, s->uCompletion, s->uTotal, s->uCacheHit,
                 s->uLastPrompt, s->uLastCompletion, s->uTokPerSec);
        j += b;
    }
    j += ",\"st\":";
    WebStatusObj(s, &j);
    j += "}";
    WebPost(s, j);
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
    if (!w || w->stSending != s->sending || w->stNet != s->netStatus || w->stPhase != phase) {
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
    std::string j = "{\"t\":\"pal\",\"pal\":";
    j += U8(WebPaletteJson(s));
    j += "}";
    WebPost(s, j);
}

/* ==================== JS 命令 (JS → C++) ==================== */

static void CfgBroadcast() {   /* 配置全进程生效: 推给全部活跃会话 (状态点/分段控件跟手) */
    for (auto& ss : g_sess) {
        if (!ss.inUse || !ss.web) continue;
        std::string j = "{\"t\":\"cfg\",\"cfg\":";
        j += U8(WebCfgJson());
        j += "}";
        WebPost(&ss, j);
        WebSyncSession(&ss);   /* 完备态变化影响状态点 */
    }
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
    if (c == L"settings") {
        std::wstring url = TrimW(msg.S(L"url"));
        std::wstring model = TrimW(msg.S(L"model"));
        if (!url.empty()) g_cfg.baseUrl = url;
        if (!model.empty()) g_cfg.model = model;
        const Jv* key = msg.Get(L"key");
        if (key && key->t == 3) g_cfg.apiKey = TrimW(key->str);
        const Jv* rs = msg.Get(L"reasoning");
        if (rs && rs->t == 1) g_cfg.reasoning = rs->b;
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
    if (c == L"pallow" || c == L"pdeny") {   /* 策略询问卡: 允许并继续 / 保持拒绝 */
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
                if (st.state == 4) {
                    st.state = 3;
                    st.err = allow ? L"已允许文件操作, 本次回答后生效" : L"用户保持拒绝";
                }
            }
        }
        WebTouch(s);
        WebSyncSession(s);
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
    if (c == L"notify") {
        const Jv* t = msg.Get(L"msg");
        if (t && t->t == 3 && g_host)
            g_host->Toast(g_ctx, s->tok, U8(t->str).c_str(), XJS_PLUGIN_TOAST_WARN);
        return;
    }
}
