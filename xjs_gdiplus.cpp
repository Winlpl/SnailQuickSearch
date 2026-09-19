/*
 * xjs_gdiplus.cpp — GDI+ 绘图后端 (XjsGfxApi 的第二实现, 2026-09-17)
 * 画布 = 32 位 DIB 内存位图: 几何/位图走 GDI+, 文本走 GDI DrawTextW (ClearType),
 * EndDraw 一次 BitBlt 到窗口 (整帧重绘口径与 D2D 后端一致)。
 * 已知能力差异 (选择引擎时告知): 无彩色 emoji (退单色), md 范围内字重/斜体/等宽近似忽略
 * (链接色/下划线保留), 渐变取首尾停靠点 (现有调用都是 2 停靠点, 无损)。
 * 字号口径: GDI 负 lfHeight("字符高") 对 TrueType 即 em 高, 与 DWrite fontSize 同义,
 * 直接 -round(px) 建 HFONT; Semibold(600) 需发独立家族 "Segoe UI Semibold" (见 GpMakeFormat);
 * UNIFORM 行距按 baseline 参数对齐 D2D 基线; 框内垂直对齐按 XjsParaAlign 偏移 (GdiParaOffset,
 * 单行/多行/效果叠画三处同式 — 数值口径与 D2D 一致, 见 xjs_app.h 枚举注释)。
 */
#include "xjs_app.h"
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

/* 工程全局 NODRAWTEXT 剥掉了 GDI 文本绘制 (防 DrawText 宏与 D2D 冲突, 见 xjs_app.h);
   本后端的文本走 GDI DrawTextW — 按 winuser 原样补回所需最小面 */
extern "C" int WINAPI DrawTextW(HDC hdc, LPCWSTR lpchText, int cchText, LPRECT lprc, UINT format);
#ifndef DT_SINGLELINE
#define DT_SINGLELINE   0x00000020
#define DT_NOPREFIX     0x00000800
#define DT_END_ELLIPSIS 0x00008000
#define DT_RIGHT        0x00000002
#define DT_CENTER       0x00000001
#define DT_VCENTER      0x00000004
#define DT_CALCRECT     0x00000400
#define DT_WORDBREAK    0x00000010
#endif

/* ==================== 后端原生态类型 ==================== */

/* 文本格式: HFONT + 对齐/换行/省略/行距 配置 (FormatSet* 原语原地修改) */
struct GdiFormat {
    HFONT font = NULL;
    HFONT cjk = NULL;           /* CJK 段伴随字体 (同 em 雅黑): GDI font linking 会把回退字体放大 ~25%,
                                   显式选字与 DWrite 同 em fallback 对齐 (GpMakeFormat 建, FreeFormat 删) */
    float px = 12;
    int align = 0;              /* DWRITE_TEXT_ALIGNMENT: 0=leading 1=trailing 2=center */
    int paraAlign = 0;          /* XjsParaAlign (xjs_app.h): 数值 = DWRITE_PARAGRAPH_ALIGNMENT (NEAR=0/FAR=1/CENTER=2,
                                   非直觉顺序, static_assert 锁死); 框内正文块偏移 0/½/1 = GdiParaOffset */
    int wrap = XJS_WRAP_WORD;   /* XjsWrapMode (xjs_app.h): 0=词换行(默认, 同 D2D CreateTextFormat) 1=单行 2=字符级; 两后端同值同义 */
    bool ellipsis = false;
    int spacingMethod = 0;      /* DWRITE_LINE_SPACING_METHOD: 1=UNIFORM */
    float spacing = 0, baseline = 0;
};

/* 文本布局: 文本 + 格式 + 效果范围 (md 链接色/下划线); 度量按需现算 */
struct GdiLayout {
    std::wstring text;
    GdiFormat* fmt = NULL;
    float maxW = 0, maxH = 0;
    int wrap = -1;              /* 覆盖格式的换行 (-1 = 随格式) */
    bool ellipsis = false;
    std::vector<std::pair<XjsTextRange, void*>> effects;      /* 画刷包装 (XjsSolidBrush*) → 颜色 */
    std::vector<std::pair<XjsTextRange, bool>> underlines;
};

struct GdiSolidBrush {
    int kind = 0;                     /* 0=纯色 (FreeBrush 按 kind 分派; 禁止裸转型混读) */
    XjsColor c;
    Gdiplus::SolidBrush* gp = NULL;   /* 惰性建 (填充用); 文本走 c→COLORREF */
};
struct GdiGradBrush {
    int kind = 1;                     /* 1=渐变 */
    Gdiplus::LinearGradientBrush* gp = NULL;   /* 线性 */
    Gdiplus::PathGradientBrush* pg = NULL;     /* 径向 */
    XjsColor c0, c1;                           /* 端点色缓存 (SetBrushColor 不触及渐变, 安全) */
    Gdiplus::PointF p0, p1;
};
struct GdiBitmap {
    Gdiplus::Bitmap* bmp = NULL;
    std::vector<BYTE> own;      /* Bitmap(scan0) 不拷贝内存 → 缓冲随包装共存亡 */
    UINT w = 0, h = 0;
};
struct GdiStroke { bool round = true; };
struct GdiGeo { Gdiplus::GraphicsPath* path = NULL; };

/* 画布: DIB 内存位图 + memDC (GDI 文本) + Graphics (GDI+ 几何), EndDraw 整帧 BitBlt */
struct GpSurface {
    HWND hwnd = NULL;
    int w = 0, h = 0;
    bool inFrame = false;       /* BeginDraw..EndDraw 期间: 禁止重建表面 (会作废帧内已保存的 DC 状态/裁剪) */
    HBITMAP hbmp = NULL;
    BYTE* bits = NULL;          /* DIB 内存 (透明度组 CPU 混合用) */
    HDC memDC = NULL;
    void* oldBmp = NULL;
    Gdiplus::Graphics* gfx = NULL;
    /* 透明度组栈 (toast 卡片进离场): 画到离屏 DIB, Pop 时 CPU 按 alpha 混合回母画布
       (GDI 文本不写 alpha 通道, GDI+ ImageAttributes 途径不可靠 → 逐像素混合最稳) */
    struct Alpha {
        XjsRect r; float a = 1;
        int w = 0, h = 0;
        HDC dc = NULL; HBITMAP hbmp = NULL; void* oldBmp = NULL; BYTE* bits = NULL;
        Gdiplus::Graphics* gfx = NULL;
    };
    std::vector<Alpha> alphas;
    ~GpSurface() { Destroy(); }
    void EnsureSize(int nw, int nh);
    void Destroy();
};

static ULONG_PTR s_gdipToken = 0;
static GpSurface* s_curSurf = NULL;   /* 当前帧表面 (BeginDraw 置位; 度量/命中借它的 DC) */
static HDC s_measureDC = NULL;
static const XjsGfxApi& GpApi();

/* ==================== 小工具 ==================== */

static COLORREF ToCOLORREF(const XjsColor& c) {
    return RGB((int)(c.r * 255 + 0.5f), (int)(c.g * 255 + 0.5f), (int)(c.b * 255 + 0.5f));
}
static Gdiplus::Color ToGpColor(const XjsColor& c) {
    return Gdiplus::Color((BYTE)(c.a * 255 + 0.5f), (BYTE)(c.r * 255 + 0.5f),
                          (BYTE)(c.g * 255 + 0.5f), (BYTE)(c.b * 255 + 0.5f));
}
static Gdiplus::RectF ToGpRect(const XjsRect& r) {
    return Gdiplus::RectF(r.left, r.top, r.right - r.left, r.bottom - r.top);
}
static GpSurface* AsSurf(XjsRt* rt)       { return static_cast<GpSurface*>(rt->h); }
static GdiFormat* AsFmt(XjsFormat* f)     { return static_cast<GdiFormat*>(f->h); }
static GdiLayout* AsLay(XjsTextLayout* l) { return static_cast<GdiLayout*>(l->h); }
static GdiBitmap* AsBmp(XjsBitmap* b)     { return static_cast<GdiBitmap*>(b->h); }

static HDC DrawDC(GpSurface* s)     { return s->alphas.empty() ? s->memDC : s->alphas.back().dc; }
static Gdiplus::Graphics* DrawGfx(GpSurface* s) {
    return s->alphas.empty() ? s->gfx : s->alphas.back().gfx;
}
/* 透明度组期: 坐标平移到组矩形左上 */
static XjsRect InCur(GpSurface* s, const XjsRect& r) {
    if (s->alphas.empty()) return r;
    const XjsRect& o = s->alphas.back().r;
    return XjsRectF(r.left - o.left, r.top - o.top, r.right - o.left, r.bottom - o.top);
}
static XjsEllipse InCur(GpSurface* s, const XjsEllipse& e) {
    if (s->alphas.empty()) return e;
    const XjsRect& o = s->alphas.back().r;
    return XjsEllipseF(XjsPoint2F(e.point.x - o.left, e.point.y - o.top), e.radiusX, e.radiusY);
}

static XjsRoundedRect InCur(GpSurface* s, const XjsRoundedRect& rr) {
    if (s->alphas.empty()) return rr;
    const XjsRect& o = s->alphas.back().r;
    return XjsRoundedRectF(XjsRectF(rr.rect.left - o.left, rr.rect.top - o.top,
                                     rr.rect.right - o.left, rr.rect.bottom - o.top), rr.radiusX, rr.radiusY);
}

/* 画刷分派: 包装类型经 RTTI 区分 (纯色→GpSolidFill; 渐变→GP 画刷本体) */
static Gdiplus::Brush* FillBrushOf(XjsBrush* b) {
    (void)0;
    if (!b) return NULL;
    if (dynamic_cast<XjsSolidBrush*>(b)) {
        GdiSolidBrush* sb = static_cast<GdiSolidBrush*>(b->h);
        if (!sb) return NULL;
        if (!sb->gp) sb->gp = new Gdiplus::SolidBrush(ToGpColor(sb->c));
        return sb->gp;
    }
    if (dynamic_cast<XjsGradBrush*>(b)) {
        GdiGradBrush* gb = static_cast<GdiGradBrush*>(b->h);
        if (!gb) return NULL;
        if (!gb->gp && !gb->pg)
            gb->gp = new Gdiplus::LinearGradientBrush(gb->p0, gb->p1, ToGpColor(gb->c0), ToGpColor(gb->c1));
        return (Gdiplus::Brush*)(gb->gp ? (Gdiplus::Brush*)gb->gp : (Gdiplus::Brush*)gb->pg);
    }
    return NULL;
}
static GdiSolidBrush* SolidOf(XjsBrush* b) {
    return (b && dynamic_cast<XjsSolidBrush*>(b)) ? static_cast<GdiSolidBrush*>(b->h) : NULL;
}

/* 圆角路径 = 手写 kappa 贝塞尔 (k=0.5523), 不走 GdipAddArc。
   实测 (2026-09-17, frame dump + 压平路径点取证): 本进程加载的 WinSxS 侧 gdiplus
   (1.1.26100, 见下方 EnsureSize 日志) 对同一组 AddArc(90/180/270/0) 存入的贝塞尔
   与 system32/.NET 结果不一致 — 部分弧被"弦线+镜像弧"替代, 填充碎裂成块;
   手写贝塞尔在同进程同 Graphics 上逐像素正确, 故一律用显式控制点构造 */
static void GpRoundRectPath(Gdiplus::GraphicsPath* p, const XjsRoundedRect& rr) {
    float l = rr.rect.left, t = rr.rect.top, r = rr.rect.right, b = rr.rect.bottom;
    float rx = xf_min(rr.radiusX, (r - l) / 2), ry = xf_min(rr.radiusY, (b - t) / 2);
    p->Reset();
    if (rx <= 0 || ry <= 0 || r <= l || b <= t) {
        p->AddRectangle(Gdiplus::RectF(l, t, r - l, b - t));
        return;
    }
    float kx = 0.5522847498f * rx, ky = 0.5522847498f * ry;
    Gdiplus::PointF pb(l + rx, b), pl(l, b - ry), pt(l + rx, t), pr(r, t + ry);
    /* 左下角 → 左边 → 左上角 → 顶边 → 右上角 → 右边 → 右下角 → 闭合 */
    p->AddBezier(pb, Gdiplus::PointF(l + rx - kx, b), Gdiplus::PointF(l, b - ry + ky), pl);
    p->AddLine(pl, Gdiplus::PointF(l, t + ry));
    p->AddBezier(Gdiplus::PointF(l, t + ry), Gdiplus::PointF(l, t + ry - ky),
                 Gdiplus::PointF(l + rx - kx, t), pt);
    p->AddLine(pt, Gdiplus::PointF(r - rx, t));
    p->AddBezier(Gdiplus::PointF(r - rx, t), Gdiplus::PointF(r - rx + kx, t),
                 Gdiplus::PointF(r, t + ry - ky), pr);
    p->AddLine(pr, Gdiplus::PointF(r, b - ry));
    p->AddBezier(Gdiplus::PointF(r, b - ry), Gdiplus::PointF(r, b - ry + ky),
                 Gdiplus::PointF(r - rx + kx, b), Gdiplus::PointF(r - rx, b));
    p->CloseFigure();
}

/* ==================== 文本: 度量与绘制 (自研逐段管线) ====================
 * 单一实现覆盖 单行/多行/省略号/混排: 逐段选字体 (CJK 段雅黑, 其余主字体) →
 * 贪心换行 (GdiWrapLines, 度量与绘制/度量共用) → 逐字符省略号截断。
 * 禁止回退 DrawTextW 的 DT_* 旗标: font linking 会放大 CJK ~25%, 且 DT_WORDBREAK
 * 与自研换行是两套口径 (行数对不上, 2026-09-17 实锤)。 */

static float GdiLineHeight(GdiFormat* f, HDC dc) {
    if (f->spacingMethod == 1 /*UNIFORM*/ && f->spacing > 0) return f->spacing;
    TEXTMETRICW tm = {};
    GetTextMetricsW(dc, &tm);
    return (float)(tm.tmHeight + tm.tmExternalLeading);
}

/* 框内垂直对齐偏移 (D2D 语义: 正文块相对框顶偏移 "框高−块高" 的 0/½/1 倍)。
   与 DWrite 同式, 不夹负值: 文本块比框高时 D2D 也照此居中 (文字溢出属调用方矩形口径问题)。
   判据用 XjsParaAlign 常量 (数值由 xjs_app.h 的 static_assert 锁死 = DWRITE_PARAGRAPH_ALIGNMENT)。 */
static float GdiParaOffset(int paraAlign, float boxH, float textH) {
    if (paraAlign == XJS_PARA_CENTER) return (boxH - textH) * 0.5f;
    if (paraAlign == XJS_PARA_FAR)    return boxH - textH;
    return 0;   /* NEAR = 顶对齐 (DWrite 默认) */
}

/* 中日韩字符判定 (表意文字/部首/标点/全角): CJK 段用伴随雅黑绘制, 其余用请求字体 —
   GDI font linking 会把缺字回退放大 ~25%, 禁止依赖它 (0x2E80 以下含注音/拼音用主字体) */
static bool GdiIsCjkW(wchar_t c) { return c >= 0x2E80; }

static HFONT GdiRunFont(GdiFormat* f, wchar_t c) { return (GdiIsCjkW(c) && f->cjk) ? f->cjk : f->font; }

/* 单字符宽 (按段选字体) */
static float GdiCharW(HDC dc, GdiFormat* f, wchar_t ch) {
    HFONT of = (HFONT)SelectObject(dc, GdiRunFont(f, ch));
    SIZE sz = {};
    GetTextExtentPoint32W(dc, &ch, 1, &sz);
    SelectObject(dc, of);
    return (float)sz.cx;
}

/* 混排宽度: 逐段选字体实测累计 (CJK 段雅黑 / 其余主字体) */
static float GdiMixedExtent(HDC dc, GdiFormat* f, const wchar_t* s, int len) {
    float w = 0;
    SIZE sz = {};
    int i = 0;
    while (i < len) {
        bool cjk = GdiIsCjkW(s[i]);
        int j = i + 1;
        while (j < len && GdiIsCjkW(s[j]) == cjk) j++;
        HFONT of = (HFONT)SelectObject(dc, GdiRunFont(f, s[i]));
        GetTextExtentPoint32W(dc, s + i, j - i, &sz);
        SelectObject(dc, of);
        w += (float)sz.cx;
        i = j;
    }
    return w;
}

/* 贪心换行: CJK 逐字成 token, ASCII 连续段成词 token; 超宽单 token 逐字硬拆。
   token 宽度走混排测量 (与绘制同一字体选择, 否则行数/行宽和绘制对不上) */
static std::vector<std::wstring> GdiWrapLines(HDC dc, GdiFormat* f, const std::wstring& s, float maxW) {
    std::vector<std::wstring> lines;
    if (s.empty()) { lines.push_back(L""); return lines; }
    std::vector<std::wstring> toks;
    std::wstring cur;
    for (size_t i = 0; i < s.size(); i++) {
        bool cjk = GdiIsCjkW(s[i]);
        if (!cur.empty() && cjk == GdiIsCjkW(cur[0])) cur += s[i];
        else { if (!cur.empty()) toks.push_back(cur); cur = s[i]; }
    }
    if (!cur.empty()) toks.push_back(cur);
    /* 拉丁段按空格细分成词 (空格独立 token): 整段英文原是一个 token, 超宽只能逐字硬拆 =
       单词被劈断 (英文界面说明文字 "syste/ms" 之坑); 细分后贪心断行优先落在空格,
       与 DWrite WORD 换行同观感; 仍超宽的单词 (长路径/URL) 走下面逐字硬拆兜底 */
    std::vector<std::wstring> words;
    for (auto& t : toks) {
        if (GdiIsCjkW(t[0])) { words.push_back(t); continue; }
        std::wstring w;
        for (size_t i = 0; i <= t.size(); i++) {
            if (i == t.size() || t[i] == L' ') {
                if (!w.empty()) words.push_back(w);
                if (i < t.size()) words.push_back(L" ");   /* 空格独立: 断行落行首时丢弃 */
                w.clear();
            } else w += t[i];
        }
    }
    std::wstring line;
    float lineW = 0;
    for (auto& t : words) {
        float tw = GdiMixedExtent(dc, f, t.c_str(), (int)t.size());
        if (lineW + tw > maxW && !line.empty()) {
            lines.push_back(line);
            line.clear();
            lineW = 0;
            if (t == L" ") continue;   /* 断行后的行首空格丢弃 (行尾空格宽已计入, 视觉无害) */
        }
        if (tw > maxW) {   /* 单 token 起步即超宽: 逐字硬拆 */
            for (wchar_t ch : t) {
                float cw = GdiCharW(dc, f, ch);
                if (lineW + cw > maxW && !line.empty()) { lines.push_back(line); line = ch; lineW = cw; }
                else { line += ch; lineW += cw; }
            }
            continue;
        }
        line += t;
        lineW += tw;
    }
    lines.push_back(line);
    return lines;
}

/* 单行混排绘制: 逐段选字体 TextOut (x 浮点累计, 段间亚像素衔接)。
   ellipsis 且超宽 → 字符级截断补 "..." (同 D2D trimming 语义, 省略号用主字体) */
static void GdiDrawRuns(HDC dc, GdiFormat* f, const wchar_t* s, int len, float x, float y, float maxW, bool ellipsis) {
    if (len <= 0) return;
    HFONT of = (HFONT)SelectObject(dc, f->font);
    int cut = len;
    if (ellipsis && GdiMixedExtent(dc, f, s, len) > maxW && maxW > 0) {
        float ew = GdiMixedExtent(dc, f, L"...", 3);
        float acc = 0;
        cut = 0;
        for (int i = 0; i < len; i++) {
            float cw = GdiCharW(dc, f, s[i]);
            if (acc + cw + ew > maxW) break;
            acc += cw;
            cut = i + 1;
        }
        len = cut;   /* 前缀之后补 "..." */
    }
    int i = 0;
    while (i < len) {
        bool cjk = GdiIsCjkW(s[i]);
        int j = i + 1;
        while (j < len && GdiIsCjkW(s[j]) == cjk) j++;
        SelectObject(dc, GdiRunFont(f, s[i]));
        TextOutW(dc, (int)floorf(x), (int)floorf(y), s + i, j - i);
        SIZE sz = {};
        GetTextExtentPoint32W(dc, s + i, j - i, &sz);
        x += (float)sz.cx;
        i = j;
    }
    if (cut < (int)wcslen(s)) {
        SelectObject(dc, f->font);
        TextOutW(dc, (int)floorf(x), (int)floorf(y), L"...", 3);
    }
    SelectObject(dc, of);
}

/* 多行逐行绘制 (自研贪心换行): lineH 由调用方给定 —
   UNIFORM 行距传 f->spacing (行高精确=行数×行距), 否则传字体默认行高 */
static void GdiDrawMultiLine(HDC dc, const std::wstring& s, GdiFormat* f, const XjsRect& r, COLORREF clr, float lineH) {
    auto lines = GdiWrapLines(dc, f, s, r.right - r.left);
    int oldAlign = SetTextAlign(dc, TA_TOP | TA_LEFT);
    int oldBk = SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, clr);
    /* D2D UNIFORM 行距: 第 N 行基线 = top + N×lineH + baseline; GDI TextOut 基线 = y + tmAscent —
       每行平移 (baseline - tmAscent) 对齐 D2D 基线 */
    float yOff = 0;
    if (f->spacingMethod == 1) {
        TEXTMETRICW tm = {};
        GetTextMetricsW(dc, &tm);
        yOff = f->baseline - (float)tm.tmAscent;
    }
    /* 整块垂直对齐 (与 D2D 同式): 块高 = 行数×行高, 多行说明/标题在框内居中或贴底 */
    float y = r.top + GdiParaOffset(f->paraAlign, r.bottom - r.top, lineH * (float)lines.size());
    for (auto& ln : lines) {
        float lw = GdiMixedExtent(dc, f, ln.c_str(), (int)ln.size());
        float x = r.left;
        if (f->align == 1) x = r.right - lw;
        else if (f->align == 2) x = r.left + ((r.right - r.left) - lw) / 2;
        GdiDrawRuns(dc, f, ln.c_str(), (int)ln.size(), x, y + yOff, r.right - r.left, false);
        y += lineH;
    }
    SetTextAlign(dc, oldAlign);
    SetBkMode(dc, oldBk);
}

/* 主文本绘制: 按 XjsWrapMode 分派 (与 D2D 同值同义, 换行行为唯一事实源见 xjs_app.h 枚举注释)。
   全部走自研逐段管线 (混排选字/贪心换行/字符级省略号), 不再依赖 DrawTextW 的 DT_* —
   linking 放大 CJK 与 DT_WORDBREAK、GdiWrapLines 两套换行口径不一致都根除于此 */
static void GdiDrawString(HDC dc, const std::wstring& s, GdiFormat* f, const XjsRect& r, COLORREF clr) {
    if (!f || !f->font || s.empty() || r.right <= r.left) return;
    int oldBk = SetBkMode(dc, TRANSPARENT);
    COLORREF oc = SetTextColor(dc, clr);
    /* 本格式的字体全程选中: 行盒/基线度量 (GetTextMetricsW/GdiLineHeight) 必须来自它 ——
       DC 字体是粘的, 拿上一位调用方留下的字体度量会按别人的行高算垂直对齐 */
    HFONT of = (HFONT)SelectObject(dc, f->font);
    bool uniform = (f->spacingMethod == 1 && f->spacing > 0);
    if (f->wrap == XJS_WRAP_NONE) {
        /* 单行: 水平对齐 / 垂直对齐 (行盒=asc+desc, XjsParaAlign 偏移, 同 D2D) / 省略号截断 */
        float maxW = r.right - r.left;
        float w = GdiMixedExtent(dc, f, s.c_str(), (int)s.size());
        float drawW = (f->ellipsis && w > maxW) ? maxW : w;
        float x = r.left;
        if (f->align == 1) x = r.right - drawW;
        else if (f->align == 2) x = r.left + (maxW - drawW) / 2;
        TEXTMETRICW tm = {};
        GetTextMetricsW(dc, &tm);
        float y = r.top + GdiParaOffset(f->paraAlign, r.bottom - r.top, (float)(tm.tmAscent + tm.tmDescent));
        bool clipped = !f->ellipsis && w > maxW;   /* 旧 DrawTextW 语义: 无省略号也裁在矩形内 */
        if (clipped) {
            SaveDC(dc);
            IntersectClipRect(dc, (int)r.left - 1, (int)r.top - 1, (int)r.right + 1, (int)r.bottom + 1);
        }
        GdiDrawRuns(dc, f, s.c_str(), (int)s.size(), x, y, maxW, f->ellipsis);
        if (clipped) RestoreDC(dc, -1);
    } else {
        GdiDrawMultiLine(dc, s, f, r, clr, uniform ? f->spacing : GdiLineHeight(f, dc));
    }
    SelectObject(dc, of);
    SetTextColor(dc, oc);
    SetBkMode(dc, oldBk);
}

/* 效果范围 (链接色/下划线) 叠画: 按 GdiWrapLines 的行分布把范围内的子串重画 (混排逐段) */
static void GdiDrawEffects(HDC dc, GdiLayout* L, const XjsRect& r) {
    if (L->effects.empty() && L->underlines.empty()) return;
    GdiFormat* f = L->fmt;
    float lineH = GdiLineHeight(f, dc);
    auto lines = GdiWrapLines(dc, f, L->text, r.right - r.left);
    /* 与 GdiDrawMultiLine 同一 UNIFORM 基线平移 (效果范围叠画必须落回基线, 否则相对正文漂移) */
    float yOff = 0;
    if (f->spacingMethod == 1) {
        TEXTMETRICW tm = {};
        GetTextMetricsW(dc, &tm);
        yOff = f->baseline - (float)tm.tmAscent;
    }
    std::vector<UINT32> starts;
    UINT32 off = 0;
    for (auto& ln : lines) { starts.push_back(off); off += (UINT32)ln.size(); }
    /* 与 GdiDrawMultiLine 同一起点: 效果范围叠画必须落在正文同一位置 (含垂直对齐偏移) */
    const float y0 = r.top + GdiParaOffset(f->paraAlign, r.bottom - r.top, lineH * (float)lines.size());
    auto drawRange = [&](UINT32 rs, UINT32 rl, COLORREF clr, bool under) {
        float y = y0;
        for (size_t i = 0; i < lines.size(); i++) {
            UINT32 ls = starts[i], le = ls + (UINT32)lines[i].size();
            UINT32 a = xf_max(rs, ls), b = xf_min(rs + rl, le);
            if (b > a && rl > 0) {
                std::wstring pre = lines[i].substr(0, a - ls);
                std::wstring seg = lines[i].substr(a - ls, b - a);
                float p1 = GdiMixedExtent(dc, f, pre.c_str(), (int)pre.size());
                float p2 = GdiMixedExtent(dc, f, seg.c_str(), (int)seg.size());
                float pw = GdiMixedExtent(dc, f, lines[i].c_str(), (int)lines[i].size());
                float x0 = r.left;
                if (f->align == 1) x0 = r.right - pw;
                else if (f->align == 2) x0 = r.left + ((r.right - r.left) - pw) / 2;
                x0 += p1;
                int oldBk = SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, clr);
                GdiDrawRuns(dc, f, seg.c_str(), (int)seg.size(), x0, y + yOff, 1e9f, false);
                if (under) {
                    HPEN pen = CreatePen(PS_SOLID, 1, clr);
                    HPEN op = (HPEN)SelectObject(dc, pen);
                    MoveToEx(dc, (int)floorf(x0), (int)floorf(y + yOff + p2), NULL);
                    LineTo(dc, (int)floorf(x0 + p2), (int)floorf(y + yOff + p2));
                    SelectObject(dc, op);
                    DeleteObject(pen);
                }
                SetBkMode(dc, oldBk);
            }
            y += lineH;
        }
    };
    for (auto& e : L->effects) {
        auto* sb = e.second ? static_cast<GdiSolidBrush*>(static_cast<XjsSolidBrush*>(e.second)->h) : NULL;
        if (!sb) continue;
        drawRange(e.first.startPosition, e.first.length, ToCOLORREF(sb->c), false);
    }
    for (auto& u : L->underlines) {
        if (!u.second) continue;
        COLORREF clr = RGB(255, 255, 255);
        for (auto& e : L->effects)
            if (e.first.startPosition <= u.first.startPosition &&
                u.first.startPosition < e.first.startPosition + e.first.length) {
                auto* sb = static_cast<GdiSolidBrush*>(static_cast<XjsSolidBrush*>(e.second)->h);
                if (sb) clr = ToCOLORREF(sb->c);
            }
        drawRange(u.first.startPosition, u.first.length, clr, true);
    }
}

/* ==================== 表面生命周期 ==================== */

void GpSurface::EnsureSize(int nw, int nh) {
    nw = xf_max(nw, 1); nh = xf_max(nh, 1);
    if (memDC && w == nw && h == nh) return;
    Destroy();
    w = nw; h = nh;
    HDC sdc = GetDC(hwnd);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;          /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    hbmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void**)&bits, NULL, 0);
    ReleaseDC(hwnd, sdc);
    memDC = CreateCompatibleDC(NULL);
    oldBmp = SelectObject(memDC, hbmp);
    gfx = Gdiplus::Graphics::FromHDC(memDC);
    gfx->SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    gfx->SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
}

void GpSurface::Destroy() {
    if (gfx) { delete gfx; gfx = NULL; }
    for (auto& a : alphas) {
        if (a.gfx) delete a.gfx;
        if (a.dc) { SelectObject(a.dc, a.oldBmp); DeleteDC(a.dc); }
        if (a.hbmp) DeleteObject(a.hbmp);
    }
    alphas.clear();
    if (memDC) { SelectObject(memDC, oldBmp); DeleteDC(memDC); memDC = NULL; }
    if (hbmp) { DeleteObject(hbmp); hbmp = NULL; }
    bits = NULL;
    w = h = 0;
}

/* ==================== XjsGfxApi 实现 ==================== */

static void GpCreateWindowRt(HWND hwnd, int w, int h, XjsRt** out) {
    GpSurface* s = new GpSurface();
    s->hwnd = hwnd;
    RECT rc; GetClientRect(hwnd, &rc);
    s->EnsureSize(ximax(rc.right, w), ximax(rc.bottom, h));
    *out = new XjsRt(s);
}
static void GpFreeRt(void* n) { delete (GpSurface*)n; }

static void GpBeginDraw(XjsRt* rt) {
    GpSurface* s = AsSurf(rt);
    RECT rc; GetClientRect(s->hwnd, &rc);
    s->EnsureSize(ximax(rc.right, 1), ximax(rc.bottom, 1));
    s_curSurf = s;
    s->inFrame = true;
    if (s->gfx) s->gfx->ResetTransform();
    SelectClipRgn(s->memDC, NULL);   /* 帧起点干净状态 (PushClip/PopClip 严格配对, 此为兜底) */
}

static HRESULT GpEndDraw(XjsRt* rt) {
    GpSurface* s = AsSurf(rt);
    /* gfx 随表面存活, 不逐帧删 (删了下一帧 EnsureSize 因尺寸未变不重建,
       gfx 残 NULL → 全部 GDI+ 填充对空指针调虚表) */
    HDC wdc = GetDC(s->hwnd);
    if (wdc) BitBlt(wdc, 0, 0, s->w, s->h, s->memDC, 0, 0, SRCCOPY);
    ReleaseDC(s->hwnd, wdc);
    if (s_curSurf == s) s_curSurf = NULL;
    s->inFrame = false;
    return S_OK;
}
static void GpClear(XjsRt* rt, const XjsColor& c) {
    GpSurface* s = AsSurf(rt);
    RECT rc = { 0, 0, s->w, s->h };
    SetBkColor(s->memDC, ToCOLORREF(c));
    ExtTextOutW(s->memDC, 0, 0, ETO_OPAQUE, &rc, NULL, 0, NULL);   /* 不透明硬清屏 (残影口径同 D2D 后端) */
}
/* 表面尺寸事实源 = 窗口客户区 (与 D2D 的 HwndRT Resize 同语义)。消费方允许在 BeginDraw
   之前读 GetPixelSize 排版 (设置窗 XjsSetPaint 即是), 表面不在此刻跟上 = 首帧仍按旧尺寸
   画、新暴露区域留在未绘的 DIB 上 (最大化后右/下整片黑, 鼠标滑过才重绘, 2026-09-18 实锤)。 */
static void GpResize(XjsRt* rt, UINT w, UINT h) {
    GpSurface* s = AsSurf(rt);
    if (s && !s->inFrame) s->EnsureSize(ximax((int)w, 1), ximax((int)h, 1));
}

static XjsSizeU GpPixelSize(XjsRt* rt) {
    GpSurface* s = AsSurf(rt);
    if (!s) return { 0, 0 };
    /* 帧外: 即时对齐客户区后返回; 帧内: BeginDraw 已对齐, 不得重建 (会作废 DC 状态栈/裁剪) */
    if (!s->inFrame && s->hwnd) {
        RECT rc; GetClientRect(s->hwnd, &rc);
        s->EnsureSize(ximax(rc.right, 1), ximax(rc.bottom, 1));
    }
    return { (UINT32)xf_max(s->w, 0), (UINT32)xf_max(s->h, 0) };
}

static void GpFillRect(XjsRt* rt, const XjsRect& r, XjsBrush* b) {
    GpSurface* s = AsSurf(rt);
    Gdiplus::Brush* br = FillBrushOf(b);
    if (br) DrawGfx(s)->FillRectangle(br, ToGpRect(InCur(s, r)));
}
static void GpFillRoundRect(XjsRt* rt, const XjsRoundedRect& rr, XjsBrush* b) {
    GpSurface* s = AsSurf(rt);
    Gdiplus::Brush* br = FillBrushOf(b);
    if (!br) return;
    Gdiplus::GraphicsPath p;
    GpRoundRectPath(&p, InCur(s, rr));
    DrawGfx(s)->FillPath(br, &p);
}
static Gdiplus::Pen* GpPenFor(XjsBrush* b, float w, XjsStroke* st) {
    GdiSolidBrush* sb = SolidOf(b);
    if (!sb) return NULL;   /* 描边全部为纯色画刷 (现有调用无渐变描边) */
    Gdiplus::Pen* pen = new Gdiplus::Pen(ToGpColor(sb->c), w);
    if (st) pen->SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
    return pen;
}
static void GpFrameRect(XjsRt* rt, const XjsRect& r, XjsBrush* b, float w, XjsStroke* st) {
    GpSurface* s = AsSurf(rt);
    Gdiplus::Pen* pen = GpPenFor(b, w, st);
    if (!pen) return;
    DrawGfx(s)->DrawRectangle(pen, ToGpRect(InCur(s, r)));
    delete pen;
}
static void GpFrameRoundRect(XjsRt* rt, const XjsRoundedRect& rr, XjsBrush* b, float w, XjsStroke* st) {
    GpSurface* s = AsSurf(rt);
    Gdiplus::Pen* pen = GpPenFor(b, w, st);
    if (!pen) return;
    Gdiplus::GraphicsPath p;
    GpRoundRectPath(&p, InCur(s, rr));
    DrawGfx(s)->DrawPath(pen, &p);
    delete pen;
}
static void GpLine(XjsRt* rt, XjsPoint2 a, XjsPoint2 b2, XjsBrush* br, float w, XjsStroke* st) {
    GpSurface* s = AsSurf(rt);
    XjsPoint2 pa = { a.x, a.y }, pb = { b2.x, b2.y };
    if (!s->alphas.empty()) {
        const XjsRect& o = s->alphas.back().r;
        pa = { a.x - o.left, a.y - o.top };
        pb = { b2.x - o.left, b2.y - o.top };
    }
    Gdiplus::Pen* pen = GpPenFor(br, w, st);
    if (pen) { DrawGfx(s)->DrawLine(pen, pa.x, pa.y, pb.x, pb.y); delete pen; }
}
static void GpFrameEllipse(XjsRt* rt, const XjsEllipse& e, XjsBrush* b, float w, XjsStroke* st) {
    GpSurface* s = AsSurf(rt);
    XjsEllipse ce = InCur(s, e);
    Gdiplus::Pen* pen = GpPenFor(b, w, st);
    if (!pen) return;
    DrawGfx(s)->DrawEllipse(pen, ce.point.x - ce.radiusX, ce.point.y - ce.radiusY,
                            ce.radiusX * 2, ce.radiusY * 2);
    delete pen;
}
static void GpFillEllipse(XjsRt* rt, const XjsEllipse& e, XjsBrush* b) {
    GpSurface* s = AsSurf(rt);
    XjsEllipse ce = InCur(s, e);
    Gdiplus::Brush* br = FillBrushOf(b);
    if (br) DrawGfx(s)->FillEllipse(br, ce.point.x - ce.radiusX, ce.point.y - ce.radiusY,
                                    ce.radiusX * 2, ce.radiusY * 2);
}
static void GpDrawTextStr(XjsRt* rt, const wchar_t* s2, UINT32 len, XjsFormat* f, const XjsRect& r,
                          XjsBrush* b, XjsDrawTextOpts o, int measuring) {
    GpSurface* s = AsSurf(rt);
    GdiSolidBrush* sb = SolidOf(b);
    if (!sb) return;   /* 文本画刷全部纯色 (渐变文字现有调用无此形态) */
    GdiDrawString(DrawDC(s), std::wstring(s2, len), AsFmt(f), InCur(s, r), ToCOLORREF(sb->c));
}
static void GpDrawTextLay(XjsRt* rt, XjsPoint2 org, XjsTextLayout* l, XjsBrush* b, XjsDrawTextOpts o) {
    GpSurface* s = AsSurf(rt);
    GdiLayout* L = AsLay(l);
    GdiSolidBrush* sb = SolidOf(b);
    if (!L || !sb || !L->fmt) return;
    HDC dc = DrawDC(s);
    XjsRect r = InCur(s, XjsRectF(org.x, org.y, org.x + L->maxW, org.y + L->maxH));
    GdiFormat tmp = *L->fmt;
    if (L->wrap >= 0) tmp.wrap = L->wrap;
    tmp.ellipsis = L->ellipsis || tmp.ellipsis;
    GdiDrawString(dc, L->text, &tmp, r, ToCOLORREF(sb->c));
    /* 效果范围按布局自己的格式画 (叠画子串/下划线) */
    if (!L->effects.empty() || !L->underlines.empty()) {
        HFONT of = (HFONT)SelectObject(dc, L->fmt->font);
        GdiDrawEffects(dc, L, r);
        SelectObject(dc, of);
    }
}
static void GpPushClip(XjsRt* rt, const XjsRect& r, int aa) {
    GpSurface* s = AsSurf(rt);
    XjsRect cr = InCur(s, r);
    SaveDC(DrawDC(s));
    IntersectClipRect(DrawDC(s), (int)cr.left, (int)cr.top, (int)cr.right, (int)cr.bottom);
    Gdiplus::Graphics* g = DrawGfx(s);
    if (g) g->SetClip(ToGpRect(cr), Gdiplus::CombineModeIntersect);
}
static void GpPopClip(XjsRt* rt) {
    GpSurface* s = AsSurf(rt);
    RestoreDC(DrawDC(s), -1);
    Gdiplus::Graphics* g = DrawGfx(s);
    if (g) g->ResetClip();
}
static void GpSetRotate(XjsRt* rt, float deg, XjsPoint2 c) {
    /* 图钉图标只经 GDI 线段绘制: DC 世界变换即可 (GDI 文本/画线都吃世界变换) */
    HDC dc = DrawDC(AsSurf(rt));
    if (SetGraphicsMode(dc, GM_ADVANCED)) {
        float rad = deg * 3.14159265f / 180;
        float cs = cosf(rad), sn = sinf(rad);
        XFORM xf = { cs, sn, -sn, cs, c.x - cs * c.x + sn * c.y, c.y - sn * c.x - cs * c.y };
        SetWorldTransform(dc, &xf);
    }
}
static void GpResetTransform(XjsRt* rt) {
    HDC dc = DrawDC(AsSurf(rt));
    XFORM id = { 1, 0, 0, 1, 0, 0 };
    SetWorldTransform(dc, &id);
    SetGraphicsMode(dc, GM_COMPATIBLE);
}
static void GpPushAlpha(XjsRt* rt, const XjsRect& r, float opacity) {
    GpSurface* s = AsSurf(rt);
    GpSurface::Alpha a;
    a.r = r; a.a = opacity;
    a.w = (int)(r.right - r.left); a.h = (int)(r.bottom - r.top);
    if (a.w <= 0 || a.h <= 0) return;
    HDC sdc = GetDC(s->hwnd);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi);
    bi.bmiHeader.biWidth = a.w;
    bi.bmiHeader.biHeight = -a.h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    a.hbmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void**)&a.bits, NULL, 0);
    ReleaseDC(s->hwnd, sdc);
    if (!a.hbmp) return;
    memset(a.bits, 0, (size_t)a.w * 4 * a.h);   /* 未画区域 = 透明黑 (混合时不动母画布) */
    a.dc = CreateCompatibleDC(NULL);
    a.oldBmp = SelectObject(a.dc, a.hbmp);
    a.gfx = Gdiplus::Graphics::FromHDC(a.dc);
    a.gfx->SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    s->alphas.push_back(a);
}
static void GpPopAlpha(XjsRt* rt) {
    GpSurface* s = AsSurf(rt);
    if (s->alphas.empty()) return;
    GpSurface::Alpha a = s->alphas.back();
    s->alphas.pop_back();
    if (a.gfx) { delete a.gfx; a.gfx = NULL; }
    if (a.dc) { SelectObject(a.dc, a.oldBmp); DeleteDC(a.dc); a.dc = NULL; }
    /* CPU 逐像素混合回母画布 (卡片小, 一次矩形循环) */
    BYTE* dst = s->bits;
    int x0 = (int)a.r.left, y0 = (int)a.r.top;
    for (int y = 0; y < a.h; y++) {
        int dy = y0 + y;
        if (dy < 0 || dy >= s->h) continue;
        const BYTE* srcRow = a.bits + (size_t)y * a.w * 4;
        BYTE* dstRow = dst + ((size_t)dy * s->w + x0) * 4;
        for (int x = 0; x < a.w; x++) {
            int dx = x0 + x;
            if (dx < 0 || dx >= s->w) continue;
            const BYTE* sp = srcRow + x * 4;
            BYTE* dp = dstRow + x * 4;
            float sa = (sp[3] / 255.0f) * a.a;
            dp[0] = (BYTE)(sp[0] * sa + dp[0] * (1 - sa));
            dp[1] = (BYTE)(sp[1] * sa + dp[1] * (1 - sa));
            dp[2] = (BYTE)(sp[2] * sa + dp[2] * (1 - sa));
        }
    }
    DeleteObject(a.hbmp);
}
static void GpBitmapFromWic(XjsRt* rt, IWICBitmapSource* src, XjsBitmap** out) {
    if (!src) return;
    UINT w = 0, h = 0;
    if (FAILED(src->GetSize(&w, &h)) || !w || !h) return;
    std::vector<BYTE> px((size_t)w * 4 * h);
    if (FAILED(src->CopyPixels(NULL, w * 4, (UINT)px.size(), px.data()))) return;
    /* PBGRA (预乘) = GDI+ PARGB 同布局 */
    GdiBitmap* gb = new GdiBitmap();
    gb->bmp = new Gdiplus::Bitmap(w, h, w * 4, PixelFormat32bppPARGB, px.data());
    gb->own = std::move(px);   /* Bitmap(scan0) 不拷贝内存: 缓冲随包装共存亡 */
    gb->w = w; gb->h = h;
    if (!gb->bmp) { delete gb; return; }
    *out = new XjsBitmap(gb);
}
static void GpBitmapFromMemory(XjsRt* rt, UINT w, UINT h, UINT stride, const BYTE* px, XjsBitmap** out) {
    /* 字节序 = B8G8R8A8, alpha 忽略 (PrintWindow 背板) → GDI+ 32bppRGB 同布局 */
    GdiBitmap* gb = new GdiBitmap();
    gb->own.assign(px, px + (size_t)stride * h);
    gb->bmp = new Gdiplus::Bitmap(w, h, stride, PixelFormat32bppRGB, gb->own.data());
    gb->w = w; gb->h = h;
    if (!gb->bmp) { delete gb; return; }
    *out = new XjsBitmap(gb);
}
static void GpDrawBmp(XjsRt* rt, XjsBitmap* bm, const XjsRect& r, float opacity, int interp, const XjsRect* src) {
    GpSurface* s = AsSurf(rt);
    GdiBitmap* gb = AsBmp(bm);
    if (!gb || !gb->bmp) return;
    Gdiplus::Graphics* g = DrawGfx(s);
    if (!g) return;
    g->SetInterpolationMode(interp == 1 ? Gdiplus::InterpolationModeNearestNeighbor
                                        : Gdiplus::InterpolationModeHighQualityBicubic);
    XjsRect dr = InCur(s, r);
    if (src) {
        g->DrawImage(gb->bmp, ToGpRect(dr), src->left, src->top,
                     src->right - src->left, src->bottom - src->top, Gdiplus::UnitPixel, NULL);
    } else {
        g->DrawImage(gb->bmp, ToGpRect(dr));
    }
}
static XjsSizeU GpBmpPixelSize(XjsBitmap* bm) {
    GdiBitmap* gb = AsBmp(bm);
    return gb ? XjsSizeU{ gb->w, gb->h } : XjsSizeU{ 0, 0 };
}
static void GpFreeBitmap(void* n) { delete (GdiBitmap*)n; }
static HRESULT GpSolidBrush(XjsRt* rt, const XjsColor& c, XjsSolidBrush** out) {
    GdiSolidBrush* sb = new GdiSolidBrush();
    sb->c = c;
    *out = new XjsSolidBrush(sb);
    return S_OK;
}
static void GpGradBrush(XjsRt* rt, XjsPoint2 s0, XjsPoint2 e0, const XjsGradientStop* st, int n, XjsGradBrush** out) {
    GdiGradBrush* gb = new GdiGradBrush();
    gb->c0 = st[0].color;
    gb->c1 = st[(size_t)xf_max(n - 1, 0)].color;
    gb->p0 = Gdiplus::PointF(s0.x, s0.y);
    gb->p1 = Gdiplus::PointF(e0.x, e0.y);
    gb->gp = new Gdiplus::LinearGradientBrush(gb->p0, gb->p1, ToGpColor(gb->c0), ToGpColor(gb->c1));
    *out = new XjsGradBrush(gb);
}
static void GpRadialBrush(XjsRt* rt, XjsPoint2 c, XjsPoint2 off, float rx, float ry, const XjsGradientStop* st, int n, XjsGradBrush** out) {
    GdiGradBrush* gb = new GdiGradBrush();
    gb->c0 = st[0].color;
    gb->c1 = st[(size_t)xf_max(n - 1, 0)].color;
    Gdiplus::GraphicsPath p;
    p.AddEllipse(c.x - rx, c.y - ry, rx * 2, ry * 2);
    Gdiplus::PathGradientBrush* pg = new Gdiplus::PathGradientBrush(&p);
    pg->SetCenterPoint(Gdiplus::PointF(c.x + off.x, c.y + off.y));
    pg->SetCenterColor(ToGpColor(gb->c0));
    INT sc = 1;
    Gdiplus::Color surr = ToGpColor(gb->c1);
    pg->SetSurroundColors(&surr, &sc);
    gb->pg = pg;
    *out = new XjsGradBrush(gb);
}
static void GpSetBrushColor(XjsSolidBrush* b, const XjsColor& c) {
    GdiSolidBrush* sb = b ? static_cast<GdiSolidBrush*>(b->h) : NULL;
    if (!sb) return;
    sb->c = c;
    if (sb->gp) { delete sb->gp; sb->gp = NULL; }   /* 下次填充惰性重建 */
}
static XjsColor GpGetBrushColor(XjsSolidBrush* b) {
    GdiSolidBrush* sb = b ? static_cast<GdiSolidBrush*>(b->h) : NULL;
    return sb ? sb->c : XjsColor();
}
static void GpFreeBrush(void* n) {
    /* 按 kind 分派: 曾把渐变原生当 GdiSolidBrush 读 gp 成员 (落在颜色数据上)
       delete = 堆损坏 → 启动数十秒后任意路径 AV/白窗 (09-24 用户崩溃根因) */
    int kind = n ? *(int*)n : -1;
    if (kind == 1) {
        GdiGradBrush* gb = (GdiGradBrush*)n;
        if (gb->gp) delete gb->gp;
        if (gb->pg) delete gb->pg;
        delete gb;
        return;
    }
    GdiSolidBrush* sb = (GdiSolidBrush*)n;
    if (sb && sb->gp) delete sb->gp;
    delete sb;
}
static void GpRoundStroke(XjsStroke** out) { *out = new XjsStroke(new GdiStroke()); }
static void GpFreeStroke(void* n) { delete (GdiStroke*)n; }
static void GpRoundRectGeo(const XjsRoundedRect& r, XjsGeo** out) {
    GdiGeo* g = new GdiGeo();
    g->path = new Gdiplus::GraphicsPath();
    GpRoundRectPath(g->path, r);
    *out = new XjsGeo(g);
}
static void GpFillGeoIntersectRect(XjsRt* rt, XjsGeo* geo, const XjsRect& r, XjsBrush* b) {
    /* 矩形 ∩ 圆角求交填充 (toast 色条/进度条端头跟随圆角): 裁剪到路径再填充 */
    GpSurface* s = AsSurf(rt);
    GdiGeo* gg = geo ? static_cast<GdiGeo*>(geo->h) : NULL;
    Gdiplus::Brush* br = FillBrushOf(b);
    Gdiplus::Graphics* g = DrawGfx(s);
    if (!gg || !br || !g) return;
    XjsRect cr = InCur(s, r);
    Gdiplus::GraphicsPath band;
    band.AddRectangle(ToGpRect(cr));
    Gdiplus::GraphicsState stt = g->Save();
    g->SetClip(&band, Gdiplus::CombineModeReplace);
    g->SetClip(gg->path, Gdiplus::CombineModeIntersect);
    g->FillRectangle(br, ToGpRect(cr));
    g->Restore(stt);
}
static void GpFreeGeo(void* n) {
    GdiGeo* g = (GdiGeo*)n;
    if (g && g->path) delete g->path;
    delete g;
}
static void GpMakeFormat(const wchar_t* family, void* coll, float weight, int style, int stretch,
                         float px, const wchar_t* locale, XjsFormat** out) {
    GdiFormat* f = new GdiFormat();
    f->px = px;
    LOGFONTW lf = {};
    lf.lfWeight = (LONG)xf_max(xf_min(weight, 1000.f), 1.f);
    lf.lfItalic = (style == 1 /*OBLIQUE*/ || style == 2 /*ITALIC*/) ? TRUE : FALSE;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, family ? family : L"Segoe UI");
    /* DirectWrite 的 "Segoe UI" 家族含 Semibold(600) 面; GDI 把它当独立家族 —
       600 直发 "Segoe UI" 会被就近映射到 Bold(700), 粗一档 (驱动器行名/md标题/弹窗标题) */
    if (lf.lfWeight >= 550 && lf.lfWeight <= 650 && !_wcsicmp(lf.lfFaceName, L"Segoe UI")) {
        wcscpy_s(lf.lfFaceName, L"Segoe UI Semibold");
        lf.lfWeight = 600;
    }
    /* 负 lfHeight = "字符高"(cell−InternalLeading), 对 TrueType 就是 em 高 — 实测
       (GetTextMetrics: -16 → asc17/desc4/cell21/IL5, asc·desc 反推 em≈15.8≈16) 与
       DWrite fontSize 同义, 直接 -round(px)。勿乘 winAsc+winDesc 的 cell/em 比 (1.33),
       2026-09-17 实测那样会整版大 33% */
    lf.lfHeight = -(LONG)xf_max(px + 0.5f, 1.0f);
    f->font = CreateFontIndirectW(&lf);
    /* CJK 段伴随字体 (同 em 雅黑): GDI 对 Segoe UI 缺字走 font linking, 会把回退字体放大 ~25%
       (实测 em13 一行 "QQ飞车": DWrite fallback 墨迹 12px / GDI linked 15px)。显式选字与
       DWrite 的同 em fallback 对齐; 雅黑无 Semibold 面, 600 就近用 Bold (与 linking 行为一致) */
    if (f->font) {
        LOGFONTW cf = lf;
        wcscpy_s(cf.lfFaceName, L"Microsoft YaHei UI");
        if (cf.lfWeight > 500) cf.lfWeight = FW_BOLD;
        f->cjk = CreateFontIndirectW(&cf);
    }
    *out = new XjsFormat(f);
}
static void GpFormatSetAlign(XjsFormat* f2, int a)     { ((GdiFormat*)f2->h)->align = a; }
static void GpFormatSetParaAlign(XjsFormat* f2, int a) { ((GdiFormat*)f2->h)->paraAlign = a; }   /* a = XjsParaAlign */
static void GpFormatSetWrap(XjsFormat* f2, int w)      { ((GdiFormat*)f2->h)->wrap = w; }
static void GpFormatSetCharEllipsis(XjsFormat* f2)     { ((GdiFormat*)f2->h)->ellipsis = true; }
static void GpFormatSetLineSpacing(XjsFormat* f2, int m, float s, float b2) {
    GdiFormat* f = (GdiFormat*)f2->h;
    f->spacingMethod = m;
    f->spacing = s;
    f->baseline = b2;
}
static float GpFormatFontSize(XjsFormat* f2)           { return ((GdiFormat*)f2->h)->px; }
static void GpFreeFormat(void* n) {
    GdiFormat* f = (GdiFormat*)n;
    if (f && f->font) DeleteObject(f->font);
    if (f && f->cjk) DeleteObject(f->cjk);
    delete f;
}
static HRESULT GpMakeLayout(const wchar_t* text, UINT32 len, XjsFormat* f2, float maxW, float maxH, XjsTextLayout** out) {
    GdiLayout* L = new GdiLayout();
    L->text.assign(text, len);
    L->fmt = (GdiFormat*)f2->h;
    L->maxW = maxW;
    L->maxH = maxH;
    *out = new XjsTextLayout(L);
    return S_OK;
}
static HRESULT GpLayoutMetrics(XjsTextLayout* l, XjsTextMetrics* m) {
    GdiLayout* L = AsLay(l);
    if (!L || !L->fmt || !L->fmt->font || !m) return E_FAIL;
    HDC dc = s_measureDC;
    if (!dc) dc = DrawDC(s_curSurf);
    if (!dc) return E_FAIL;
    HFONT of = (HFONT)SelectObject(dc, L->fmt->font);
    int wrap = L->wrap >= 0 ? L->wrap : L->fmt->wrap;
    float wdt = 0, hgt = 0;
    if (wrap == XJS_WRAP_NONE) {
        SIZE sz = {};
        GetTextExtentPoint32W(dc, L->text.c_str(), (int)L->text.size(), &sz);
        wdt = GdiMixedExtent(dc, L->fmt, L->text.c_str(), (int)L->text.size());
        hgt = (float)sz.cy;
    } else {
        /* 多行度量与 GdiDrawMultiLine 严格同口径 (同一 GdiWrapLines + 混排测宽):
           UNIFORM 行距下高度=行数×行距 (与 D2D UNIFORM 一致, toast 卡高/md 块高/设置行高都靠它);
           词换行/字符级换行共用同一拆行实现, 绘制与度量不再两套口径 */
        auto lines = GdiWrapLines(dc, L->fmt, L->text, L->maxW);
        for (auto& ln : lines)
            wdt = xf_max(wdt, GdiMixedExtent(dc, L->fmt, ln.c_str(), (int)ln.size()));
        hgt = GdiLineHeight(L->fmt, dc) * (float)lines.size();
    }
    SelectObject(dc, of);
    *m = { 0, 0, wdt, wdt, hgt };
    return S_OK;
}
static void GpLayoutSetWrap(XjsTextLayout* l, int w)          { AsLay(l)->wrap = w; }
static void GpLayoutSetCharEllipsis(XjsTextLayout* l)         { AsLay(l)->ellipsis = true; }
static void GpLayoutSetLineSpacing(XjsTextLayout* l, int m2, float s, float b2) {
    GdiFormat* f = AsLay(l)->fmt;
    f->spacingMethod = m2; f->spacing = s; f->baseline = b2;
}
static bool GpLayoutHitTest(XjsTextLayout* l, UINT32 idx, BOOL trailing, float* x, float* y, XjsHitTestMetrics* m) {
    /* 行内编辑 (NO_WRAP): 光标 x = 前缀宽度 */
    GdiLayout* L = AsLay(l);
    if (!L || !L->fmt || !L->fmt->font) return false;
    HDC dc = s_measureDC ? s_measureDC : DrawDC(s_curSurf);
    if (!dc) return false;
    HFONT of = (HFONT)SelectObject(dc, L->fmt->font);
    idx = (UINT32)xf_min((int)idx, (int)L->text.size());
    if (x) *x = GdiMixedExtent(dc, L->fmt, L->text.c_str(), (int)idx);
    if (y) *y = 0;
    SelectObject(dc, of);
    if (m) *m = { 0, 0, 0, (float)GdiLineHeight(L->fmt, dc), idx, 0 };
    return true;
}
static HRESULT GpLayoutHitRange(XjsTextLayout* l, UINT32 pos, UINT32 len, float ox, float oy, XjsHitTestMetrics* out, UINT32 max, UINT32* returned) {
    /* md 行内代码底色: 单矩形近似 (前缀宽 → 段宽) */
    GdiLayout* L = AsLay(l);
    if (!L || !L->fmt || !L->fmt->font || !max || !out) return E_FAIL;
    HDC dc = s_measureDC ? s_measureDC : DrawDC(s_curSurf);
    if (!dc) return E_FAIL;
    HFONT of = (HFONT)SelectObject(dc, L->fmt->font);
    pos = (UINT32)xf_min((int)pos, (int)L->text.size());
    len = (UINT32)xf_min((int)len, (int)L->text.size() - (int)pos);
    float p1 = GdiMixedExtent(dc, L->fmt, L->text.c_str(), (int)pos);
    float p2 = GdiMixedExtent(dc, L->fmt, L->text.c_str() + pos, (int)len);
    SelectObject(dc, of);
    out[0] = { ox + p1, oy, p2, (float)GdiLineHeight(L->fmt, dc), pos, len };
    if (returned) *returned = 1;
    return S_OK;
}
static bool GpLayoutHitPoint(XjsTextLayout* l, float x, float y, BOOL* trail, BOOL* inside, XjsHitTestMetrics* m) {
    /* 行内编辑: 逐字累计宽度找落点 (近似 DWrite HitTestPoint) */
    GdiLayout* L = AsLay(l);
    if (!L || !L->fmt || !L->fmt->font) return false;
    HDC dc = s_measureDC ? s_measureDC : DrawDC(s_curSurf);
    if (!dc) return false;
    HFONT of = (HFONT)SelectObject(dc, L->fmt->font);
    UINT32 i = 0;
    float pre = 0;
    for (; i < L->text.size(); i++) {
        float one = GdiCharW(dc, L->fmt, L->text[i]);
        if (x < one) {
            if (trail) *trail = (x > pre + one / 2) ? TRUE : FALSE;
            if (inside) *inside = TRUE;
            if (m) *m = { pre, 0, one, (float)GdiLineHeight(L->fmt, dc), i, 1 };
            SelectObject(dc, of);
            return true;
        }
        x -= one;
        pre += one;
    }
    if (trail) *trail = FALSE;
    if (inside) *inside = TRUE;
    if (m) *m = { pre, 0, 0, (float)GdiLineHeight(L->fmt, dc), (UINT32)L->text.size(), 0 };
    SelectObject(dc, of);
    return true;
}
static void GpFreeLayout(void* n) { delete (GdiLayout*)n; }

/* md 范围样式: GDI 布局记录效果/覆盖, DrawTextLay 时叠画; 字重/斜体/等宽/字号范围近似忽略
   (md 的粗体/斜体段独立成布局, 行内 code 底色走 HitRange, 链接色/下划线按范围叠画) */
static void GpLayoutFontWeight(XjsTextLayout* l, float w, XjsTextRange rg) {}
static void GpLayoutFontStyle(XjsTextLayout* l, int s, XjsTextRange rg) {}
static void GpLayoutFontFamily(XjsTextLayout* l, const wchar_t* f, XjsTextRange rg) {}
static void GpLayoutFontSize(XjsTextLayout* l, float px, XjsTextRange rg) {}
static void GpLayoutDrawingEffect(XjsTextLayout* l, void* effect, XjsTextRange rg) {
    AsLay(l)->effects.push_back({ rg, effect });
}
static void GpLayoutUnderline(XjsTextLayout* l, BOOL on, XjsTextRange rg) {
    AsLay(l)->underlines.push_back({ rg, on != FALSE });
}

/* 渐变端点跟随: GP 画刷原地改 (选中条逐行横渐/进度条) */
static void GpGradSetStart(XjsGradBrush* b, XjsPoint2 p) {
    GdiGradBrush* gb = b ? static_cast<GdiGradBrush*>(b->h) : NULL;
    if (!gb) return;
    gb->p0 = Gdiplus::PointF(p.x, p.y);
    if (gb->gp) { delete gb->gp; gb->gp = NULL; }   /* GDI+ 无原地改端点: 惰性重建 (下次填充) */
}
static void GpGradSetEnd(XjsGradBrush* b, XjsPoint2 p) {
    GdiGradBrush* gb = b ? static_cast<GdiGradBrush*>(b->h) : NULL;
    if (!gb) return;
    gb->p1 = Gdiplus::PointF(p.x, p.y);
    if (gb->gp) { delete gb->gp; gb->gp = NULL; }   /* 惰性重建 */
}

/* ==================== 后端函数表 + 启停 ==================== */

static const XjsGfxApi& GpApi() {
    static XjsGfxApi api = {
        GpCreateWindowRt, GpFreeRt,
        GpBeginDraw, GpEndDraw, GpClear, GpResize, GpPixelSize,
        GpFillRect, GpFillRoundRect, GpFrameRect, GpFrameRoundRect, GpLine,
        GpFrameEllipse, GpFillEllipse, GpDrawTextStr, GpDrawTextLay,
        GpPushClip, GpPopClip, GpSetRotate, GpResetTransform, GpPushAlpha, GpPopAlpha,
        GpBitmapFromWic, GpBitmapFromMemory, GpDrawBmp, GpBmpPixelSize, GpFreeBitmap,
        GpSolidBrush, GpGradBrush, GpRadialBrush, GpSetBrushColor, GpGetBrushColor, GpFreeBrush,
        GpGradSetStart, GpGradSetEnd,
        GpLayoutFontWeight, GpLayoutFontStyle, GpLayoutFontFamily, GpLayoutFontSize,
        GpLayoutDrawingEffect, GpLayoutUnderline, GpLayoutHitRange, GpLayoutHitPoint,
        GpFormatSetCharEllipsis, GpLayoutSetCharEllipsis,
        GpRoundStroke, GpFreeStroke, GpRoundRectGeo, GpFillGeoIntersectRect, GpFreeGeo,
        GpMakeFormat, GpFormatSetAlign, GpFormatSetParaAlign, GpFormatSetWrap,
        GpFormatSetLineSpacing, GpFormatFontSize, GpFreeFormat,
        GpMakeLayout, GpLayoutMetrics, GpLayoutSetWrap,
        GpLayoutSetLineSpacing, GpLayoutHitTest, GpFreeLayout,
    };
    return api;
}


bool XjsGdiplusInit() {
    Gdiplus::GdiplusStartupInput si;
    if (Gdiplus::GdiplusStartup(&s_gdipToken, &si, NULL) != Gdiplus::Ok) return false;
    HRESULT r3 = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                  __uuidof(IWICImagingFactory), (void**)&g_wic);
    if (FAILED(r3) || !g_wic) { Gdiplus::GdiplusShutdown(s_gdipToken); s_gdipToken = 0; return false; }
    s_measureDC = CreateCompatibleDC(NULL);
    g_dw = new XjsDwFactory(NULL);   /* 文本工厂无原生态 (MakeFormat/MakeLayout 走函数表) */
    g_gfx = (XjsGfxApi*)&GpApi();    return true;
}

void XjsGdiplusShutdown() {
    g_gfx = NULL;
    if (g_dw) { delete g_dw; g_dw = NULL; }
    if (g_wic) { g_wic->Release(); g_wic = NULL; }
    if (s_measureDC) { DeleteDC(s_measureDC); s_measureDC = NULL; }
    if (s_gdipToken) { Gdiplus::GdiplusShutdown(s_gdipToken); s_gdipToken = 0; }
}
