/*
 * xjs_md.cpp — 通用 Markdown 引擎: md4c 解析 + D2D 排版绘制 (实例式, 多文档可并存)
 *
 * 使用者: 设置"搜索模式"分类 (内置 RCDATA 文档, xjs_settings.cpp 装载); 未来 .md 文件预览同 API。
 * 用法 (见 xjs_app.h): XjsMdCreate → XjsMdSetText(utf8) → XjsMdLayout(rt,宽,unit) 得总高
 *                      → XjsMdPaint(rt, x, yTop, unit, 裁剪上下界); XjsMdFree 销毁。
 *
 *   解析 → 块模型 (标题/段落/列表项/代码块/表格/分隔线 + 行内 加粗/斜体/行内代码/链接)
 *   排版 (宽/单位尺度/皮肤纪元 任一变即重排) → 逐块 XjsTextLayout, 自动换行交给 DirectWrite,
 *       行内样式走 SetFontWeight/SetFontFamilyName/SetDrawingEffect; 表格先测自然列宽再逐格预排
 *   绘制 → 传入目标窗口自己的 RT: 画刷/文本格式全部建在该 RT 上 (跨 RT 画刷 = 整帧丢弃)
 *
 * 行距口径: 正文/表格/标题一律 DWRITE_LINE_SPACING_METHOD_UNIFORM (行高 = 字号×固定系数) —
 *   默认行距按请求字体 (Segoe UI) 计算, 而 CJK 回退字形 (雅黑) 实际更高, 多行段落相邻行会
 *   视觉叠压 (2026-09-16 实锤); uniform 行高既消除叠画, 又让全文档行节奏一致。
 *   UNIFORM 下 GetMetrics().height = 行数×行高, 块高精确, 相邻块不再叠画。
 */
#include "xjs_app.h"
#include "md4c/md4c.h"

struct XjsMdDoc {
    /* ---- 解析产物 (与排版/绘制解耦) ---- */
    struct MdRun {                 /* 同一样式的连续行内文本 */
        std::wstring text;
        bool strong = false, em = false, code = false, link = false;
        bool SameStyle(const MdRun& o) const {
            return strong == o.strong && em == o.em && code == o.code && link == o.link;
        }
    };
    struct MdCell { std::vector<MdRun> runs; };
    struct MdBlock {
        int type = 0;              /* MD_BLOCKTYPE 原值 */
        int level = 0;             /* 标题级别 (1..3 有专属字号, 更深按 3) */
        int depth = 0;             /* 列表嵌套深度 (0 起) */
        int ord = 0;               /* >0 = 有序列表项的序号 */
        int cols = 0;              /* 表格列数 (MD_BLOCK_TABLE_DETAIL.col_count) */
        std::vector<MdRun> runs;   /* 段落/标题/列表项的行内内容 */
        std::wstring codeRaw;      /* 代码块原文 ('\n' 分行) */
        std::vector<std::vector<MdCell>> rows;   /* 表格行, rows[0] = 表头 */
    };

    /* ---- 排版产物 ---- */
    struct MdTableCell { XjsTextLayout* lay = NULL; float x = 0, y = 0; };
    struct MdDrawItem {
        int kind = 0;              /* 0=文本块 1=分隔线 2=代码块 3=表格 */
        float y = 0, h = 0;
        /* kind 0: 整段一布局 (DirectWrite 自动换行); xOff = 左缩进; bullet: <0=圆点 >0=有序号 0=无
           (哨兵必须是负数: 有序列表首项编号就是 1, 曾与"1=圆点"撞车画出 • , 2026-09-16 实锤) */
        XjsTextLayout* lay = NULL;
        float xOff = 0;
        int bullet = 0;
        std::vector<XjsRect> codeRects;   /* 行内代码背景 (相对布局原点) */
        /* kind 2: 代码块逐行布局 */
        std::vector<XjsTextLayout*> lines;
        /* kind 3: 表格 */
        std::vector<float> colX, colW, rowY, rowH;
        std::vector<std::vector<MdTableCell>> cells;
    };

    /* ---- 状态 ---- */
    std::vector<MdBlock> doc;
    std::vector<MdDrawItem> items;
    float totalH = 0;
    float laidW = -1, laidUnit = 0;     /* laidW < 0 = 尚未排版过 */
    long laidEpoch = -1;
    /* RT 绑定资源, 键 = (RT, 皮肤纪元, 单位尺度) */
    XjsRt* resRt = NULL;
    long resEpoch = -1;
    float resUnit = 0;
    XjsSolidBrush *brText = NULL, *brDim = NULL, *brAccent = NULL,
        *brBorder = NULL, *brBorderStrong = NULL, *brCodeBg = NULL;
    XjsFormat *tfH1 = NULL, *tfH2 = NULL, *tfH3 = NULL, *tfBody = NULL,
        *tfCell = NULL, *tfCellH = NULL, *tfCode = NULL;
};

namespace {

/* ==================== md4c 回调: 文本 → 块模型 ==================== */

using MdRun = XjsMdDoc::MdRun;
using MdCell = XjsMdDoc::MdCell;
using MdBlock = XjsMdDoc::MdBlock;

struct MdParseCtx {
    /* 行内文本落点栈: 存下标而非 &runs — blocks/rows 扩容搬移会让元素指针悬垂,
       之后再写入 = 堆损坏 (tight 列表项内嵌代码块再跟直属文本即可构造, 曾潜伏) */
    struct MdSink { int block = -1; int cellRow = -1, cellCol = -1; };   /* cell<0 = 块顶层 runs */
    std::vector<MdBlock>* blocks = NULL;
    std::vector<MdSink> sinks;
    std::vector<int> listNext;                /* 每层列表: -1 = 无序, 否则 = 下一序号 */
    int strong = 0, em = 0, codeSpan = 0, link = 0;   /* 行内样式嵌套深度 */
    bool inCodeBlock = false;

    std::vector<MdRun>* SinkRuns() {
        const MdSink& sk = sinks.back();
        MdBlock& b = (*blocks)[sk.block];
        return (sk.cellRow < 0) ? &b.runs : &b.rows[sk.cellRow][sk.cellCol].runs;
    }

    void AppendText(const std::wstring& t) {
        if (t.empty()) return;
        if (inCodeBlock) { blocks->back().codeRaw += t; return; }
        if (sinks.empty()) return;
        auto& runs = *SinkRuns();
        MdRun nr;
        nr.strong = strong > 0; nr.em = em > 0; nr.code = codeSpan > 0; nr.link = link > 0;
        if (!runs.empty() && runs.back().SameStyle(nr)) runs.back().text += t;
        else { nr.text = t; runs.push_back(nr); }
    }
};

std::wstring MdUtf8(const char* s, MD_SIZE n) {
    std::string tmp(s, n);
    return Utf8ToUtf16(tmp.c_str());
}

int MdEnterBlock(MD_BLOCKTYPE type, void* detail, void* ud) {
    auto* c = (MdParseCtx*)ud;
    switch (type) {
        case MD_BLOCK_H:
            c->blocks->push_back({});
            c->blocks->back().type = type;
            c->blocks->back().level = (int)((MD_BLOCK_H_DETAIL*)detail)->level;
            c->sinks.push_back({ (int)c->blocks->size() - 1, -1, -1 });
            break;
        case MD_BLOCK_LI: {
            c->blocks->push_back({});
            auto& b = c->blocks->back();
            b.type = type;
            b.depth = c->listNext.empty() ? 0 : (int)c->listNext.size() - 1;
            if (!c->listNext.empty() && c->listNext.back() >= 0) b.ord = c->listNext.back()++;
            c->sinks.push_back({ (int)c->blocks->size() - 1, -1, -1 });
            break;
        }
        case MD_BLOCK_P:
            c->blocks->push_back({});
            c->blocks->back().type = type;
            c->sinks.push_back({ (int)c->blocks->size() - 1, -1, -1 });
            break;
        case MD_BLOCK_UL: c->listNext.push_back(-1); break;
        case MD_BLOCK_OL: c->listNext.push_back((int)((MD_BLOCK_OL_DETAIL*)detail)->start); break;
        case MD_BLOCK_TABLE: {
            c->blocks->push_back({});
            auto& b = c->blocks->back();
            b.type = type;
            b.cols = (int)((MD_BLOCK_TABLE_DETAIL*)detail)->col_count;
            break;
        }
        case MD_BLOCK_TR: c->blocks->back().rows.emplace_back(); break;
        case MD_BLOCK_TH: case MD_BLOCK_TD: {
            MdBlock& b = c->blocks->back();
            b.rows.back().push_back({});
            c->sinks.push_back({ (int)c->blocks->size() - 1,
                                 (int)b.rows.size() - 1, (int)b.rows.back().size() - 1 });
            break;
        }
        case MD_BLOCK_CODE: c->blocks->push_back({}); c->blocks->back().type = type; c->inCodeBlock = true; break;
        case MD_BLOCK_HR: c->blocks->push_back({}); c->blocks->back().type = type; break;
        default: break;   /* DOC/THEAD/TBODY/BLOCK_QUOTE 容器不建模 (引用内段落按普通段落绘制) */
    }
    return 0;
}

int MdLeaveBlock(MD_BLOCKTYPE type, void* /*detail*/, void* ud) {
    auto* c = (MdParseCtx*)ud;
    switch (type) {
        case MD_BLOCK_H: case MD_BLOCK_P: case MD_BLOCK_LI:
        case MD_BLOCK_TH: case MD_BLOCK_TD:
            if (!c->sinks.empty()) c->sinks.pop_back();
            break;
        case MD_BLOCK_UL: case MD_BLOCK_OL:
            if (!c->listNext.empty()) c->listNext.pop_back();
            break;
        case MD_BLOCK_CODE: c->inCodeBlock = false; break;
        default: break;
    }
    return 0;
}

int MdEnterSpan(MD_SPANTYPE type, void* /*detail*/, void* ud) {
    auto* c = (MdParseCtx*)ud;
    if (type == MD_SPAN_STRONG) c->strong++;
    else if (type == MD_SPAN_EM) c->em++;
    else if (type == MD_SPAN_CODE) c->codeSpan++;
    else if (type == MD_SPAN_A) c->link++;
    return 0;
}

int MdLeaveSpan(MD_SPANTYPE type, void* /*detail*/, void* ud) {
    auto* c = (MdParseCtx*)ud;
    if (type == MD_SPAN_STRONG) c->strong--;
    else if (type == MD_SPAN_EM) c->em--;
    else if (type == MD_SPAN_CODE) c->codeSpan--;
    else if (type == MD_SPAN_A) c->link--;
    return 0;
}

int MdText(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* ud) {
    auto* c = (MdParseCtx*)ud;
    if (type == MD_TEXT_BR || type == MD_TEXT_SOFTBR) c->AppendText(L" ");   /* 软/硬换行一律作空格 */
    else if (type == MD_TEXT_NULLCHAR) c->AppendText(L"\uFFFD");
    else c->AppendText(MdUtf8(text, size));   /* 实体原文透传 */
    return 0;
}

/* ==================== 资源 / 排版 ==================== */

void MdFreeLayout(XjsMdDoc* d) {
    for (auto& it : d->items) {
        if (it.lay) it.lay->Release();
        for (auto* l : it.lines) l->Release();
        for (auto& row : it.cells) for (auto& cl : row) if (cl.lay) cl.lay->Release();
    }
    d->items.clear();
    d->totalH = 0;
    d->laidW = -1;
}

void MdFreeResources(XjsMdDoc* d) {
    MdFreeLayout(d);   /* 布局经 SetDrawingEffect 引用画刷, 必须先于画刷释放 */
    XjsSolidBrush* brs[] = { d->brText, d->brDim, d->brAccent,
                                    d->brBorder, d->brBorderStrong, d->brCodeBg };
    for (auto* b : brs) if (b) b->Release();
    d->brText = d->brDim = d->brAccent = d->brBorder = d->brBorderStrong = d->brCodeBg = NULL;
    XjsFormat* tfs[] = { d->tfH1, d->tfH2, d->tfH3, d->tfBody, d->tfCell, d->tfCellH, d->tfCode };
    for (auto* t : tfs) if (t) t->Release();
    d->tfH1 = d->tfH2 = d->tfH3 = d->tfBody = d->tfCell = d->tfCellH = d->tfCode = NULL;
    d->resRt = NULL;
    d->resEpoch = -1;
    d->resUnit = 0;
}

/* uniform 行距格式: 行高 = 字号×spacingEm, 基线 0.68 (CJK 回退字形不再叠压相邻行) */
void MdMakeFormat(XjsDwFactory* dw, XjsFormat** out, float px, float unit,
                  DWRITE_FONT_WEIGHT w, const wchar_t* family, float spacingEm) {
    dw->CreateTextFormat(family, NULL, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                         px * unit, L"zh-cn", out);
    if (*out) {
        float lineH = px * unit * spacingEm;
        (*out)->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lineH, lineH * 0.68f);
    }
}

void MdRelayout(XjsMdDoc* d, float contentW, float unit);     /* 定义在下: 资源重建后由 EnsureResources 调用来恢复排版 */

void MdEnsureResources(XjsMdDoc* d, XjsRt* rt, float unit) {
    if (d->resRt == rt && d->resEpoch == g_skinEpoch && d->resUnit == unit) return;
    /* 资源键 (RT/皮肤纪元/单位) 变化 = 旧画刷与旧文本格式全作废; 布局经 SetDrawingEffect
       引用旧画刷, 必须先于画刷释放。但"作废"不等于"结束": 绘制路径 (XjsMdPaint) 只画 items,
       不会自建排版 —— 释放后无人重排, 整页文档从此静默空白 (2026-09-18 实锤: 双击 Ctrl 唤起的
       异皮肤窗口再开设置, g_skinName 在两窗皮肤间来回镜像, 纪元每翻一次设置窗资源就重建一次,
       md 页首帧即空白, 须切分类重建行模型才恢复)。故按记忆宽度就地重排, 兑现"纪元变更即重排" */
    float wasW = d->laidW;
    MdFreeResources(d);
    d->resRt = rt;
    d->resEpoch = g_skinEpoch;
    d->resUnit = unit;
    rt->CreateSolidColorBrush(g_skin.text, &d->brText);
    rt->CreateSolidColorBrush(g_skin.textDim, &d->brDim);
    rt->CreateSolidColorBrush(g_skin.accent, &d->brAccent);
    rt->CreateSolidColorBrush(g_skin.border, &d->brBorder);
    rt->CreateSolidColorBrush(g_skin.borderStrong, &d->brBorderStrong);
    rt->CreateSolidColorBrush(g_skin.panel2, &d->brCodeBg);
    MdMakeFormat(g_dw, &d->tfH1,   20.0f, unit, DWRITE_FONT_WEIGHT_SEMI_BOLD, L"Segoe UI",  1.45f);
    MdMakeFormat(g_dw, &d->tfH2,   16.0f, unit, DWRITE_FONT_WEIGHT_SEMI_BOLD, L"Segoe UI",  1.50f);
    MdMakeFormat(g_dw, &d->tfH3,   14.0f, unit, DWRITE_FONT_WEIGHT_SEMI_BOLD, L"Segoe UI",  1.50f);
    MdMakeFormat(g_dw, &d->tfBody, 13.0f, unit, DWRITE_FONT_WEIGHT_NORMAL,    L"Segoe UI",  1.60f);
    MdMakeFormat(g_dw, &d->tfCell, 12.5f, unit, DWRITE_FONT_WEIGHT_NORMAL,    L"Segoe UI",  1.55f);
    MdMakeFormat(g_dw, &d->tfCellH,12.5f, unit, DWRITE_FONT_WEIGHT_SEMI_BOLD, L"Segoe UI",  1.55f);
    MdMakeFormat(g_dw, &d->tfCode, 12.0f, unit, DWRITE_FONT_WEIGHT_NORMAL,    L"Consolas",  1.50f);
    if (wasW >= 40.0f && !d->doc.empty()) MdRelayout(d, wasW, unit);   /* 资源重建后立即按原宽重排 (见上注) */
}

/* 行内内容 → 一个自动换行布局; 行内代码背景矩形同步求出 (相对布局原点, codeRects 可为 NULL)。
   代码段与相邻文字之间补一个间隔空格 (归入相邻普通文本区间): 代码底色不能贴着左右文字,
   间隔须是真实空格 (有步宽) 而非底色外扩 — 外扩的底色会垫到相邻字形底下, 视觉上仍是贴着的 */
XjsTextLayout* MdMakeInline(XjsMdDoc* d, const std::vector<MdRun>& runs, XjsFormat* fmt,
                                float maxW, float unit, std::vector<XjsRect>* codeRects) {
    std::vector<std::wstring> parts(runs.size());
    for (size_t i = 0; i < runs.size(); i++) parts[i] = runs[i].text;
    for (size_t i = 0; i < runs.size(); i++) {
        if (!runs[i].code) continue;
        if (i > 0 && !parts[i - 1].empty() && parts[i - 1].back() != L' ') parts[i - 1] += L' ';
        if (i + 1 < runs.size() && !parts[i + 1].empty() && parts[i + 1].front() != L' ')
            parts[i + 1].insert(parts[i + 1].begin(), L' ');
    }
    std::wstring text;
    struct Seg { UINT32 start, len; const MdRun* run; };
    std::vector<Seg> segs;
    for (size_t i = 0; i < runs.size(); i++) {
        segs.push_back({ (UINT32)text.size(), (UINT32)parts[i].size(), &runs[i] });
        text += parts[i];
    }
    XjsTextLayout* lay = NULL;
    if (FAILED(g_dw->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt,
                                      xf_max(maxW, 10.0f), 100000.0f, &lay)) || !lay)
        return NULL;
    FLOAT fmtSize = fmt->GetFontSize();
    for (auto& sg : segs) {
        if (!sg.len) continue;
        XjsTextRange rg = { sg.start, sg.len };
        if (sg.run->strong) lay->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, rg);
        if (sg.run->em) lay->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, rg);
        if (sg.run->code) {
            lay->SetFontFamilyName(L"Consolas", rg);
            lay->SetFontSize(fmtSize * 0.92f, rg);   /* 等宽字号略小, 与中文正文协调 (uniform 行距下行高不变) */
            if (codeRects) {
                std::vector<XjsHitTestMetrics> hm(16);
                UINT32 count = 0;
                HRESULT hr = lay->HitTestTextRange(sg.start, sg.len, 0, 0, hm.data(), (UINT32)hm.size(), &count);
                if (FAILED(hr) && count > hm.size()) {   /* 缓冲不足: 按实际数重取 */
                    hm.resize(count);
                    hr = lay->HitTestTextRange(sg.start, sg.len, 0, 0, hm.data(), (UINT32)hm.size(), &count);
                }
                if (SUCCEEDED(hr))
                    for (UINT32 i = 0; i < count && i < hm.size(); i++)
                        codeRects->push_back(XjsRectF(hm[i].left - 3 * unit, hm[i].top,
                                                         hm[i].left + hm[i].width + 3 * unit,
                                                         hm[i].top + hm[i].height));
            }
        }
        if (sg.run->link) {   /* 链接: accent 色 + 下划线 */
            lay->SetDrawingEffect(d->brAccent, rg);
            lay->SetUnderline(TRUE, rg);
        }
    }
    return lay;
}

/* 单元格自然宽度 (不换行测宽, 用于列宽分配) */
float MdMeasureRuns(const std::vector<MdRun>& runs, XjsFormat* fmt) {
    std::wstring t;
    for (auto& r : runs) t += r.text;
    return XjsMeasureText(t.c_str(), fmt);
}

/* 全量重排: 宽度 / 单位尺度 / 皮肤纪元 任一变化后重建 items */
void MdRelayout(XjsMdDoc* d, float contentW, float unit) {
    MdFreeLayout(d);
    d->laidW = contentW;
    d->laidUnit = unit;
    d->laidEpoch = g_skinEpoch;
    const float u = unit;
    /* 列表标记缩进: 标记 (• / "n.") 右对齐挂在正文左缘外 7u 处, 正文左缘 = 基础缩进 (18+14×深度)u。
       基础缩进给标记留的空档只有 (18-7)u = 11u —— 有序列表进到两位数 ("10." ≈ 17u) 时标记左半
       伸出内容左缘之外, 被 XjsMdPaint 的内容裁剪框切掉首位 (序号 10 画成 "0.", 2026-09-18 实锤)。
       故缩进取"基础缩进"与该深度最宽标记+间隙的较大者; 按深度取整篇最大值而非逐项各算 —
       同一层级的列表项才共用一条正文左缘 (逐项各算 = 列表自身参差)。 */
    int liMaxDepth = 0;
    for (auto& b : d->doc)
        if (b.type == MD_BLOCK_LI && b.depth > liMaxDepth) liMaxDepth = b.depth;
    std::vector<float> liIndent((size_t)liMaxDepth + 1, 0.0f);
    for (auto& b : d->doc) {
        if (b.type != MD_BLOCK_LI) continue;
        float& ind = liIndent[(size_t)b.depth];
        if (ind <= 0) ind = (18 + 14 * b.depth) * u;
        if (b.ord > 0 && d->tfBody) {   /* 圆点标记窄, 基础缩进恒够; 只有序号会长到越界 */
            wchar_t mk[16];
            _snwprintf(mk, 16, L"%d.", b.ord);
            float need = 7 * u + XjsMeasureText(mk, d->tfBody) + 1 * u;   /* 7u 间隙 + 1u 抗取整 */
            if (need > ind) ind = need;
        }
    }
    float y = 0;
    bool first = true;
    for (auto& b : d->doc) {
        switch (b.type) {
            case MD_BLOCK_H: {
                XjsFormat* fmt = d->tfH3;
                float mt = 14, mb = 6;
                if (b.level == 1)      { fmt = d->tfH1; mt = first ? 0.0f : 16.0f; mb = 10.0f; }
                else if (b.level == 2) { fmt = d->tfH2; mt = 18.0f; mb = 9.0f; }
                y += mt * u;
                XjsMdDoc::MdDrawItem it;
                it.kind = 0;
                it.y = y;
                it.lay = MdMakeInline(d, b.runs, fmt, contentW, unit, NULL);
                if (it.lay) { XjsTextMetrics m; it.lay->GetMetrics(&m); it.h = m.height; }
                y += it.h + mb * u;
                d->items.push_back(std::move(it));
                break;
            }
            case MD_BLOCK_P: {
                XjsMdDoc::MdDrawItem it;
                it.kind = 0;
                it.y = y;
                it.lay = MdMakeInline(d, b.runs, d->tfBody, contentW, unit, &it.codeRects);
                if (it.lay) { XjsTextMetrics m; it.lay->GetMetrics(&m); it.h = m.height; }
                y += it.h + 9 * u;
                d->items.push_back(std::move(it));
                break;
            }
            case MD_BLOCK_LI: {
                float indent = liIndent[(size_t)b.depth];   /* 该深度统一值 (见 MdRelayout 顶部注) */
                XjsMdDoc::MdDrawItem it;
                it.kind = 0;
                it.y = y;
                it.xOff = indent;
                it.bullet = b.ord > 0 ? b.ord : -1;
                it.lay = MdMakeInline(d, b.runs, d->tfBody, contentW - indent, unit, &it.codeRects);
                if (it.lay) { XjsTextMetrics m; it.lay->GetMetrics(&m); it.h = m.height; }
                y += it.h + 4 * u;
                d->items.push_back(std::move(it));
                break;
            }
            case MD_BLOCK_HR: {
                XjsMdDoc::MdDrawItem it;
                it.kind = 1;
                it.y = y + 12 * u;
                it.h = 1;
                y += 25 * u;
                d->items.push_back(std::move(it));
                break;
            }
            case MD_BLOCK_CODE: {
                const float pad = 12 * u;
                XjsMdDoc::MdDrawItem it;
                it.kind = 2;
                it.y = y + 6 * u;
                float ly = pad;
                for (auto& line : XjsSplitLines(b.codeRaw)) {
                    XjsTextLayout* lay = NULL;
                    if (SUCCEEDED(g_dw->CreateTextLayout(line.c_str(), (UINT32)line.size(), d->tfCode,
                                                         xf_max(contentW - 2 * pad, 10.0f), 10000.0f, &lay)) && lay) {
                        XjsTextMetrics m;
                        lay->GetMetrics(&m);
                        ly += m.height;
                        it.lines.push_back(lay);
                    }
                }
                if (it.lines.empty()) ly = 2 * pad;   /* 空代码块 */
                it.h = ly;
                y += 6 * u + it.h + 12 * u;
                d->items.push_back(std::move(it));
                break;
            }
            case MD_BLOCK_TABLE: {
                int n = b.cols;
                for (auto& r : b.rows) n = (int)r.size() > n ? (int)r.size() : n;
                if (n <= 0) break;
                const float hpad = 9 * u, vpad = 7 * u;
                /* 列宽: 各列取最大单元格自然宽 (上限 0.55×内容宽, 下限 0.14×内容宽 —
                   防止长文列挤得短列只剩一条缝, 如"附:如何选择"的"用哪个"列), 再等比归一到内容宽 */
                XjsMdDoc::MdDrawItem it;
                it.kind = 3;
                it.y = y + 6 * u;
                std::vector<float> nat((size_t)n, 0.0f);
                for (size_t ri = 0; ri < b.rows.size(); ri++)
                    for (int i = 0; i < (int)b.rows[ri].size() && i < n; i++)
                        nat[(size_t)i] = xf_max(nat[(size_t)i],
                            MdMeasureRuns(b.rows[ri][i].runs, ri == 0 ? d->tfCellH : d->tfCell) + 2 * hpad);
                float sum = 0;
                for (auto& w : nat) { w = xf_min(w, contentW * 0.55f); w = xf_max(w, contentW * 0.14f); sum += w; }
                if (sum <= 0) break;
                float cx = 0;
                for (int i = 0; i < n; i++) {
                    float w = nat[(size_t)i] * (contentW / sum);
                    it.colX.push_back(cx);
                    it.colW.push_back(w);
                    cx += w;
                }
                float ry = 0;
                for (size_t ri = 0; ri < b.rows.size(); ri++) {
                    float rh = 0;
                    std::vector<XjsMdDoc::MdTableCell> row;
                    for (int i = 0; i < (int)b.rows[ri].size() && i < n; i++) {
                        XjsMdDoc::MdTableCell cl;
                        cl.lay = MdMakeInline(d, b.rows[ri][i].runs, ri == 0 ? d->tfCellH : d->tfCell,
                                              it.colW[(size_t)i] - 2 * hpad, unit, NULL);
                        float ch = 0;
                        if (cl.lay) { XjsTextMetrics m; cl.lay->GetMetrics(&m); ch = m.height; }
                        cl.x = it.colX[(size_t)i] + hpad;
                        cl.y = ry + vpad;
                        rh = xf_max(rh, ch);
                        row.push_back(cl);
                    }
                    it.rowY.push_back(ry);
                    it.rowH.push_back(rh + 2 * vpad);
                    ry += rh + 2 * vpad;
                    it.cells.push_back(std::move(row));
                }
                it.h = ry;
                y += 6 * u + it.h + 14 * u;
                d->items.push_back(std::move(it));
                break;
            }
            default: break;
        }
        first = false;
    }
    d->totalH = y;
}

}   /* namespace */

/* ==================== 对外接口 (见 xjs_app.h) ==================== */

XjsMdDoc* XjsMdCreate() {
    return new XjsMdDoc();
}

void XjsMdFree(XjsMdDoc* d) {
    if (!d) return;
    MdFreeResources(d);
    delete d;
}

bool XjsMdSetText(XjsMdDoc* d, const char* utf8, unsigned len) {
    if (!d || !utf8 || len == 0) return false;
    MdFreeLayout(d);   /* 内容换了, 排版产物全作废 (模型重建) */
    MD_PARSER parser = {};
    parser.abi_version = 0;
    parser.flags = MD_FLAG_TABLES | MD_FLAG_COLLAPSEWHITESPACE;
    parser.enter_block = MdEnterBlock;
    parser.leave_block = MdLeaveBlock;
    parser.enter_span = MdEnterSpan;
    parser.leave_span = MdLeaveSpan;
    parser.text = MdText;
    parser.debug_log = NULL;
    parser.syntax = NULL;
    MdParseCtx ctx;
    ctx.blocks = &d->doc;
    d->doc.clear();
    md_parse(utf8, (MD_SIZE)len, &parser, &ctx);
    return !d->doc.empty();
}

float XjsMdLayout(XjsMdDoc* d, XjsRt* rt, float contentW, float unit) {
    if (!d || !rt || contentW < 40.0f) return 0;
    MdEnsureResources(d, rt, unit);
    if (d->doc.empty()) return 0;
    if (!d->items.empty() && d->laidW == contentW && d->laidUnit == unit && d->laidEpoch == g_skinEpoch)
        return d->totalH;   /* 干净: 复用 (行模型重建频繁, 排版只在尺寸/纪元变化时发生) */
    MdRelayout(d, contentW, unit);
    return d->totalH;
}

void XjsMdPaint(XjsMdDoc* d, XjsRt* rt, float x, float yTop, float unit, float clipT, float clipB) {
    if (!d || !rt || clipB <= clipT) return;
    MdEnsureResources(d, rt, unit);   /* 资源键变化会就地重排 (内部兑现); 必须先于空判 — 否则一整帧乃至此后每帧都画空 */
    if (d->items.empty()) return;
    rt->PushAxisAlignedClip(XjsRectF(x, clipT, x + d->laidW, clipB), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    float visT = clipT - yTop, visB = clipB - yTop;   /* 转内容坐标做整项剔除, 视口外的块一个都不画 */
    for (auto& it : d->items) {
        if (it.y + it.h < visT || it.y > visB) continue;
        switch (it.kind) {
            case 0: {   /* 文本块: 行内代码底 → 项目符号 → 文本 (原点取整像素: 亚像素定位伤 ClearType) */
                float iy = floorf(yTop + it.y), ix = floorf(x + it.xOff);
                for (auto& cr : it.codeRects)
                    rt->FillRoundedRectangle(XjsRoundedRectF(
                        XjsRectF(floorf(ix + cr.left), floorf(iy + cr.top),
                                    ceilf(ix + cr.right), ceilf(iy + cr.bottom)),
                        3 * unit, 3 * unit), d->brCodeBg);
                if (it.bullet) {
                    wchar_t bl[16];
                    if (it.bullet > 0) _snwprintf(bl, 16, L"%d.", it.bullet);
                    else wcscpy_s(bl, L"•");
                    float bw = XjsMeasureText(bl, d->tfBody);
                    rt->DrawText(bl, (UINT32)wcslen(bl), d->tfBody,
                        XjsRectF(x + it.xOff - 7 * unit - bw, iy, x + it.xOff - 7 * unit, iy + it.h),
                        d->brAccent);
                }
                /* MdMakeInline 失败时 lay=NULL: 传 NULL 布局进 DWrite = 崩溃 (case 2/3 均有守卫) */
                if (it.lay)
                    rt->DrawTextLayout(XjsPoint2F(ix, iy), it.lay, d->brText, D2D1_DRAW_TEXT_OPTIONS_NONE);
                break;
            }
            case 1: {   /* 分隔线 */
                float ly = floorf(yTop + it.y);
                rt->FillRectangle(XjsRectF(x, ly, x + d->laidW, ly + 1), d->brBorder);
                break;
            }
            case 2: {   /* 代码块: panel2 圆角底 + 描边 + 逐行等宽文本 */
                float iy = floorf(yTop + it.y), pad = 12 * unit;
                XjsRect bg = XjsRectF(x, iy, x + d->laidW, iy + ceilf(it.h));
                rt->FillRoundedRectangle(XjsRoundedRectF(bg, 5 * unit, 5 * unit), d->brCodeBg);
                rt->DrawRoundedRectangle(XjsRoundedRectF(bg, 5 * unit, 5 * unit), d->brBorder, 1.0f);
                float ly = iy + pad;
                for (auto* l : it.lines) {
                    XjsTextMetrics m;
                    l->GetMetrics(&m);
                    rt->DrawTextLayout(XjsPoint2F(floorf(x + pad), floorf(ly)), l, d->brText,
                                       D2D1_DRAW_TEXT_OPTIONS_NONE);
                    ly += m.height;
                }
                break;
            }
            case 3: {   /* 表格: 表头 panel2 底 + 完整网格线 (外框+全部分隔, 表头下缘加重, 同正式版) */
                float ty = floorf(yTop + it.y), tw = d->laidW;
                int nC = (int)it.colW.size(), nR = (int)it.cells.size();
                if (nR > 1)
                    rt->FillRectangle(XjsRectF(x, ty, x + tw, ty + it.rowY[1]), d->brCodeBg);
                for (size_t ri = 0; ri < it.cells.size(); ri++)
                    for (auto& cl : it.cells[ri])
                        if (cl.lay)
                            rt->DrawTextLayout(XjsPoint2F(floorf(x + cl.x), floorf(ty + cl.y)), cl.lay,
                                               d->brText, D2D1_DRAW_TEXT_OPTIONS_NONE);
                float tb = ty + it.h;
                for (int i = 0; i <= nC; i++) {   /* 竖线: 列边界 + 右外框; 右线收进 [tw-1, tw) —
                                                     裁剪框右缘 = x+laidW, 线画到 [tw, tw+1) 会被整体剔除 */
                    float vx = floorf(x + (i == nC ? tw - 1.0f : it.colX[i]));
                    rt->FillRectangle(XjsRectF(vx, ty, vx + 1, tb), d->brBorder);
                }
                for (int ri = 0; ri <= nR; ri++) {   /* 横线: 行分隔 + 底外框; 表头下缘加重 */
                    float hy = floorf(ty + (ri == nR ? it.h : it.rowY[ri]));
                    rt->FillRectangle(XjsRectF(x, hy, x + tw, hy + 1),
                                      ri == 1 ? d->brBorderStrong : d->brBorder);
                }
                break;
            }
        }
    }
    rt->PopAxisAlignedClip();
}
