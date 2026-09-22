/*
 * ai_render.cpp — 渲染层: markdown-lite / 排版断行 / 绘制小件 / 消息与工具卡片
 * 渲染 / 输入框布局 / 整帧渲染 / 交付。布局排版随内容与宽度现算。
 */
#include "ai_assistant.h"

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

/* ==================== 排版 (块 → 行; 量宽贪心换行, CJK 逐字/拉丁按词) ==================== */


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

float AiMeasure(Gdiplus::Graphics& g, Gdiplus::Font* f, const std::wstring& t) {
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

void RelayoutOne(AiSess* s, int mi) {
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
/* 输入框布局: 显式 '\n' (Shift+Enter) = 硬换行切逻辑行, 行内再按宽软换行。
   '\n' 不产 atom (AiSegsToAtoms 是消息排版口径, 遇 \n 丢弃) 但记 1 个下标归上一逻辑行尾 —
   整串直喂会让显示下标比编辑下标少 (差 \n 个数), 光标显示位置与删除位置错位 */
void InputLayout(AiSess* s, float boxInnerW, std::vector<InpLine>* out) {
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
void InputPosOf(AiSess* s, const std::vector<InpLine>& lay, float boxInnerW, size_t idx,
                       int* outLine, float* outX) {
    if (idx > s->input.size()) idx = s->input.size();
    int li = 0;
    while (li < (int)lay.size() - 1 && idx > lay[li].start + lay[li].len()) li++;
    *outLine = li;
    *outX = InputXInLine(lay[li], idx);
}

/* 事件点 → 字符偏移 (点定位/拖选; 行按 y, 行内按 x 就近, 词内比例)
   px/py = 面板坐标, boxX/boxY = 输入框左上角 — 文本原点 = 框内缩 10k (渲染同源) */
size_t InputIndexFromPoint(AiSess* s, const std::vector<InpLine>& lay, float boxInnerW,
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
int InputLineCount(AiSess* s, float boxInnerW) {
    std::vector<InpLine> lay;
    InputLayout(s, boxInnerW, &lay);
    int n = (int)lay.size();
    return n > 4 ? 4 : n;
}

/* 输入框内光标 (列, 行, 行内 x) — caret 折算到换行后的可视位置 */
void InputCaretPos(AiSess* s, float boxInnerW, int* outLine, float* outX) {
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
void RenderDeliver(AiSess* s) {
    if (!s || !s->inUse || !s->w) return;
    RenderSession(s);
    if (!HOST_PANEL_OK || !g_host) return;
    g_host->PanelDeliverBitmap(g_ctx, s->tok, s->serial, s->bw, s->bh, s->px.data(), s->stride);
}
