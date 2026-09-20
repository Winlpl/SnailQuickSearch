/*
 * xjs_d2d.cpp — D2D 设备资源与通用绘制
 * 工厂/渲染目标生命周期、主题画刷、文本格式、WIC 图片解码、测量/省略/高亮/图标绘制
 */
#include "xjs_app.h"

static std::unordered_map<XjsFormat*, IDWriteInlineObject*> g_ellSignCache;   // 省略号签名按格式缓存
static ID2D1Factory* g_d2d = NULL;   /* D2D 后端私有工厂 (建 HwndRT/描边样式/几何; 不进公共头) */
static ID2D1StrokeStyle* s_roundStroke = NULL;   /* 圆头描边单例原生 (引用计数随包装平衡) */
static const XjsGfxApi& D2dApi();  /* D2D 后端函数表 (实现在文件尾) */

/* ============ D2D 后端内部: 包装 ↔ 原生 互转 ============ */
static ID2D1RenderTarget* D2(XjsRt* p)            { return (ID2D1RenderTarget*)p->h; }
static ID2D1Brush*        D2(XjsBrush* p)         { return (ID2D1Brush*)p->h; }
static ID2D1SolidColorBrush* D2(XjsSolidBrush* p) { return (ID2D1SolidColorBrush*)p->h; }
static ID2D1LinearGradientBrush* D2(XjsGradBrush* p) { return (ID2D1LinearGradientBrush*)p->h; }
static ID2D1Bitmap*       D2(XjsBitmap* p)        { return (ID2D1Bitmap*)p->h; }
static IDWriteTextFormat* D2(XjsFormat* p)        { return (IDWriteTextFormat*)p->h; }
static IDWriteTextLayout* D2(XjsTextLayout* p)    { return (IDWriteTextLayout*)p->h; }
static ID2D1StrokeStyle*  D2(XjsStroke* p)        { return (ID2D1StrokeStyle*)p->h; }
static IDWriteFactory*    D2(XjsDwFactory* p)     { return (IDWriteFactory*)p->h; }

void XjsReleaseTextFormats() {
    /* 必须经二级指针清全局: 曾用值数组 for(auto& f) 置空 — 只清了局部副本, g_tf* 全部悬空 */
    XjsFormat** const fmts[] = { &g_tfTitle, &g_tfMenu, &g_tfHead, &g_tfHeadR, &g_tfRow, &g_tfRowBold,
        &g_tfDim, &g_tfTiny, &g_tfTinyR, &g_tfStatus, &g_tfTip, &g_tfChip, &g_tfRowR, &g_tfBig, &g_tfCardVal,
        &g_tfSearch, &g_tfToast, &g_tfTag };
    for (auto* pf : fmts) { if (*pf) { (*pf)->Release(); *pf = NULL; } }
    for (auto& kv : g_ellSignCache) { if (kv.second) kv.second->Release(); }
    g_ellSignCache.clear();   // 签名由旧格式生成, 格式重建后须作废
}

static void XjsMakeFormat(XjsFormat** out, float px, DWRITE_FONT_WEIGHT w,
                          DWRITE_TEXT_ALIGNMENT align, bool ellipsis) {
    *out = NULL;   /* 先清: 创建失败时保持 NULL (悬挂的旧包装已被 Release, 若残留会被 if(*out) 当有效用) */
    g_dw->CreateTextFormat(L"Segoe UI", NULL, w, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, px * g_s * XjsUiZoom(), L"zh-cn", out);
    if (*out) {
        (*out)->SetTextAlignment(align);
        (*out)->SetParagraphAlignment(XJS_PARA_CENTER);
        (*out)->SetWordWrapping(XJS_WRAP_NONE);
        if (ellipsis) (*out)->SetCharEllipsis();
    }
}

/* 文本格式构建时的 每窗尺度×页面缩放 镜像 (XjsRecreateTextFormats 写, XjsSyncTextFormats 判) */
static float s_tfDpiS = -1.0f;
static int s_tfZoom = -1;

/* 窗口上下文切换后调用 (Enter): 页面缩放已每窗化, 共享文本格式跟随当前窗倍率 —
   当前窗的 尺度/缩放 与上次构建不一致才重建 (同值零开销; 必须在消息处理早期, 不得落在绘制中途) */
void XjsSyncTextFormats() {
    if (g_tfTitle && (s_tfDpiS != g_s || s_tfZoom != g_uiZoomTenths))
        XjsRecreateTextFormats();
}

/* 显式上下文版本: 撕毁期嵌套消息会经 Enter 重绑 Cur(), 宏若解析到别的窗口 = double-free 崩溃 */
/* Layer 是 RT 绑定资源: 跨 RT 复用 = EndDraw WRONG_RESOURCE_DOMAIN 丢整帧 (同画刷跨域口径),
   设备重建后旧 layer 也随旧 RT 失效 → 按 RT 包装指针各存一份, XjsDeviceDiscardCtx 统一作废 */
static std::unordered_map<XjsRt*, ID2D1Layer*> s_layers;

void XjsDeviceDiscardCtx(XjsSearchWindow& w) {
    w.needFullPaint = true;   /* 设备资源作废: 下一帧必须整窗重画 */
    if (w.dc) {
        /* QI 出的原生 DC 引用归本窗: 包装 Release 只拆壳, 原生必须先 Release (漏了=每次重建泄一个) */
        if (w.dc->h) ((ID2D1DeviceContext*)w.dc->h)->Release();
        w.dc->Release();
        w.dc = NULL;
    }
    { auto it = s_layers.find(w.rt); if (it != s_layers.end()) { if (it->second) it->second->Release(); s_layers.erase(it); } }
    /* rt 是 hwndRt 同一 COM 对象的别名 (XjsDeviceCreate 里直接赋值, 无独立引用):
       只能经 hwndRt 释放一次 — 先 Release rt 会把对象提前打回 0, hwndRt 再 Release = UAF 崩溃 */
    if (w.hwndRt) { w.hwndRt->Release(); w.hwndRt = NULL; }
    w.rt = NULL;
    for (auto& b : w.br) { if (b) { b->Release(); b = NULL; } }
    if (w.brWhite) { w.brWhite->Release(); w.brWhite = NULL; }
    if (w.brCloseHover) { w.brCloseHover->Release(); w.brCloseHover = NULL; }
    if (w.brErr) { w.brErr->Release(); w.brErr = NULL; }
    if (w.brSelGrad) { w.brSelGrad->Release(); w.brSelGrad = NULL; }
    if (w.brSelBar) { w.brSelBar->Release(); w.brSelBar = NULL; }
    if (w.brProgress) { w.brProgress->Release(); w.brProgress = NULL; }
    if (w.appIcon) { w.appIcon->Release(); w.appIcon = NULL; }
    for (auto& kv : w.brushCache) { if (kv.second) kv.second->Release(); }
    w.brushCache.clear();
    for (auto& kv : w.iconCache) { if (kv.second) kv.second->Release(); }   /* 图标位图绑定旧 RT, 同步全清 */
    w.iconCache.clear();
    XjsClearRenderCaches();   // 位图绑定渲染目标, 设备重建后需重新解码
}

void XjsDeviceDiscard() {
    XjsSearchWindow* cur = XjsSearchWindow::Cur();
    if (cur) XjsDeviceDiscardCtx(*cur);
}

/* ==================== 皮肤 (正式版 skin-*.css :root 变量, 同名同义) ==================== */

/* 内置默认 = skin-dark.css 值 (皮肤文件缺失或单变量缺省时的兜底) */
void XjsSkinReset() {
    g_skin.bg1 = XjsCol(0x15171e);    g_skin.bg2 = XjsCol(0x0e1015);
    g_skin.panel = XjsCol(0x1b1e27);  g_skin.panel2 = XjsCol(0x20242f);
    g_skin.border = XjsCol(0x272c38); g_skin.borderStrong = XjsCol(0x3a4152);
    g_skin.text = XjsCol(0xe9ecf3);   g_skin.textDim = XjsCol(0x9aa2b3);
    g_skin.textFaint = XjsCol(0x697082);
    g_skin.accent = XjsCol(0x6e8bff); g_skin.accent2 = XjsCol(0x9d7bff);
    g_skin.accentSoft = XjsCol(0x6e8bff, 0.17f);
    g_skin.rowHover = XjsCol(0x262c3b);
    g_skin.hl = XjsCol(0xff8a7a);     g_skin.hlBg = XjsCol(0xff8a7a, 0.13f);
    g_skin.timeBadge = XjsCol(0x664242);
    g_skin.menuBg = XjsCol(0x1b1e27, 0.88f);
    g_skin.driveTrack = XjsCol(0xffffff, 0.12f);
    g_skin.ok = XjsCol(0x3ddc97);     g_skin.err = XjsCol(0xff6b5e);
    g_skin.warn = XjsCol(0xffc53d);
}

static std::wstring XjsStripCssComments(const std::wstring& s) {
    std::wstring r; r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        if (i + 1 < s.size() && s[i] == L'/' && s[i + 1] == L'*') {
            size_t e = s.find(L"*/", i + 2);
            i = (e == std::wstring::npos) ? s.size() : e + 2;
            r.push_back(L' ');
        } else {
            r.push_back(s[i++]);
        }
    }
    return r;
}

static int XjsHexVal(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

/* #RGB #RGBA #RRGGBB #RRGGBBAA / rgb(r,g,b) / rgba(r,g,b,a) (百分比数值同样接受) */
static bool XjsParseCssColor(const std::wstring& v, XjsColor& out) {
    if (!v.empty() && v[0] == L'#') {
        int n = (int)v.size() - 1;
        if (n == 3 || n == 4) {
            int r = XjsHexVal(v[1]), g = XjsHexVal(v[2]), b = XjsHexVal(v[3]), a = n == 4 ? XjsHexVal(v[4]) : 15;
            if (r < 0 || g < 0 || b < 0 || a < 0) return false;
            out = XjsColorF(r / 15.0f, g / 15.0f, b / 15.0f, a / 15.0f);
            return true;
        }
        if (n == 6 || n == 8) {
            int c[4] = { 0, 0, 0, 255 };
            for (int i = 0; i < n / 2; i++) {
                int hi = XjsHexVal(v[1 + i * 2]), lo = XjsHexVal(v[2 + i * 2]);
                if (hi < 0 || lo < 0) return false;
                c[i] = hi * 16 + lo;
            }
            out = XjsColorF(c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, c[3] / 255.0f);
            return true;
        }
        return false;
    }
    if (v.rfind(L"rgb", 0) == 0) {
        size_t p1 = v.find(L'('), p2 = v.rfind(L')');
        if (p1 == std::wstring::npos || p2 == std::wstring::npos || p2 < p1) return false;
        std::wstring body = v.substr(p1 + 1, p2 - p1 - 1);
        float c[4] = { 0, 0, 0, 1 };
        int idx = 0;
        size_t s = 0;
        for (size_t i = 0; i <= body.size() && idx < 4; i++) {
            if (i == body.size() || body[i] == L',') {
                std::wstring t = XjsTrimWs(body.substr(s, i - s));
                if (!t.empty()) {
                    float f = (float)_wtof(t.c_str());
                    if (!t.empty() && t[t.size() - 1] == L'%') f = f * 255.0f / 100.0f;
                    c[idx++] = f;
                }
                s = i + 1;
            }
        }
        if (idx < 3) return false;
        for (int i = 0; i < 3; i++) { if (c[i] < 0) c[i] = 0; if (c[i] > 255) c[i] = 255; }
        out = XjsColorF(c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, c[3] > 1 ? 1 : (c[3] < 0 ? 0 : c[3]));
        return true;
    }
    return false;
}

static void XjsSkinSetVar(const std::wstring& n, const XjsColor& c) {
    struct { const wchar_t* n; XjsColor* p; } tbl[] = {
        { L"bg1", &g_skin.bg1 },           { L"bg2", &g_skin.bg2 },
        { L"panel", &g_skin.panel },       { L"panel-2", &g_skin.panel2 },
        { L"border", &g_skin.border },     { L"border-strong", &g_skin.borderStrong },
        { L"text", &g_skin.text },         { L"text-dim", &g_skin.textDim },
        { L"text-faint", &g_skin.textFaint },
        { L"accent", &g_skin.accent },     { L"accent2", &g_skin.accent2 },
        { L"accent-soft", &g_skin.accentSoft },
        { L"row-hover", &g_skin.rowHover },
        { L"hl", &g_skin.hl },             { L"hl-bg", &g_skin.hlBg },
        { L"time-badge", &g_skin.timeBadge },
        { L"menu-bg", &g_skin.menuBg },
        { L"drive-track", &g_skin.driveTrack },
        { L"ok", &g_skin.ok },             { L"err", &g_skin.err },
        { L"warn", &g_skin.warn },
    };
    for (auto& e : tbl)
        if (_wcsicmp(n.c_str(), e.n) == 0) { *e.p = c; return; }
}

/* 查找皮肤文件: exe目录\skin\skin-<名>.css → exe目录\skin-<名>.css */
static std::wstring XjsSkinPath(const std::wstring& name) {
    std::wstring dir = XjsGetExeDir();
    std::wstring p1 = dir + L"\\skin\\skin-" + name + L".css";
    std::wstring p2 = dir + L"\\skin-" + name + L".css";
    const wchar_t* cand[2] = { p1.c_str(), p2.c_str() };   /* 字符串须存活到函数尾, 不可存临时物 c_str */
    for (int i = 0; i < 2; i++) {
        DWORD a = GetFileAttributesW(cand[i]);
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) return i == 0 ? p1 : p2;
    }
    return L"";
}

/* 加载皮肤: 先复位内置默认再覆盖文件里出现的变量 (缺文件/缺变量都有确定状态) */
bool XjsSkinLoad(const wchar_t* name) {
    XjsSkinReset();
    std::wstring path = XjsSkinPath(name ? name : L"");
    if (path.empty()) return false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string utf8;
    char buf[8192];
    DWORD rd = 0;
    while (ReadFile(h, buf, sizeof(buf), &rd, NULL) && rd) utf8.append(buf, rd);
    CloseHandle(h);
    if (utf8.size() >= 3 && (unsigned char)utf8[0] == 0xEF && (unsigned char)utf8[1] == 0xBB && (unsigned char)utf8[2] == 0xBF)
        utf8.erase(0, 3);
    std::wstring css = XjsStripCssComments(Utf8ToUtf16(utf8.c_str()));
    size_t r = css.find(L":root");
    if (r == std::wstring::npos) return false;
    size_t b = css.find(L'{', r), e = css.find(L'}', b);
    if (b == std::wstring::npos || e == std::wstring::npos) return false;
    for (size_t i = b + 1; i < e; ) {
        size_t d = css.find(L"--", i);
        if (d == std::wstring::npos || d >= e) break;
        size_t c1 = css.find(L':', d), c2 = css.find(L';', d);
        if (c1 == std::wstring::npos || c2 == std::wstring::npos || c1 > c2 || c2 > e) break;
        std::wstring n = XjsTrimWs(css.substr(d + 2, c1 - d - 2));
        std::wstring v = XjsTrimWs(css.substr(c1 + 1, c2 - c1 - 1));
        XjsColor col;
        if (!n.empty() && XjsParseCssColor(v, col)) XjsSkinSetVar(n, col);
        i = c2 + 1;
    }
    return true;
}

std::vector<std::wstring> XjsSkinEnumerate() {
    std::vector<std::wstring> names;
    std::wstring dir = XjsGetExeDir();
    const std::wstring dirs[2] = { dir + L"\\skin", dir };
    for (int i = 0; i < 2; i++) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dirs[i] + L"\\skin-*.css").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::wstring f = fd.cFileName;
            if (f.length() <= 9) continue;
            if (_wcsnicmp(f.c_str(), L"skin-", 5) != 0 || _wcsicmp(f.c_str() + f.length() - 4, L".css") != 0) continue;
            std::wstring n = f.substr(5, f.length() - 9);
            bool dup = false;
            for (auto& x : names) if (x == n) { dup = true; break; }
            if (!dup) names.push_back(n);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return names;
}

/* 主题渐变: 选中行横渐(accent-soft→透明88%) / 选中竖条与进度条(accent→accent2); 换肤时重建 */
static void XjsCreateThemeGradients() {
    if (g_brSelGrad) { g_brSelGrad->Release(); g_brSelGrad = NULL; }
    if (g_brSelBar) { g_brSelBar->Release(); g_brSelBar = NULL; }
    if (g_brProgress) { g_brProgress->Release(); g_brProgress = NULL; }
    XjsGradientStop gs1[2] = { { 0.0f, g_skin.accentSoft }, { 0.88f, XjsCol(0, 0.f) } };
    g_rt->CreateLinearGradientBrush(XjsPoint2F(0, 0), XjsPoint2F(100, 0), gs1, 2, &g_brSelGrad);
    XjsGradientStop gs2[2] = { { 0.0f, g_skin.accent }, { 1.0f, g_skin.accent2 } };
    g_rt->CreateLinearGradientBrush(XjsPoint2F(0, 0), XjsPoint2F(0, 100), gs2, 2, &g_brSelBar);
    g_rt->CreateLinearGradientBrush(XjsPoint2F(0, 0), XjsPoint2F(260, 0), gs2, 2, &g_brProgress);
}

/* g_skin → 主题色数组 (XjsThemeIdx 序) */
static void XjsThemeArray(XjsColor* t) {
    t[XTH_BG1] = g_skin.bg1;             t[XTH_BG2] = g_skin.bg2;
    t[XTH_PANEL] = g_skin.panel;         t[XTH_PANEL2] = g_skin.panel2;
    t[XTH_BORDER] = g_skin.border;       t[XTH_BORDER_STRONG] = g_skin.borderStrong;
    t[XTH_TEXT] = g_skin.text;           t[XTH_TEXT_DIM] = g_skin.textDim;
    t[XTH_TEXT_FAINT] = g_skin.textFaint;
    t[XTH_ACCENT] = g_skin.accent;       t[XTH_ACCENT2] = g_skin.accent2;
    t[XTH_ACCENT_SOFT] = g_skin.accentSoft;
    t[XTH_ROW_HOVER] = g_skin.rowHover;
    t[XTH_HL] = g_skin.hl;               t[XTH_HL_BG] = g_skin.hlBg;
}

/* 换肤热应用: 刷子 SetColor + 重建渐变 + GDI 侧 EDIT 背景刷 + 弹窗资源重建 (下次打开按新皮肤) */
void XjsSkinApply() {
    g_skinEpoch++;
    if (g_rt) {
        XjsColor theme[XTH_COUNT];
        XjsThemeArray(theme);
        for (int i = 0; i < XTH_COUNT; i++)
            if (g_br[i]) g_br[i]->SetColor(theme[i]);
        if (g_brErr) g_brErr->SetColor(g_skin.err);
        XjsCreateThemeGradients();
    }
    XjsPopupReleaseResources();
    XjsSearchWindow::Cur()->Invalidate();
}

/* D2D1 HwndRenderTarget: 纯 D2D 呈现 (EndDraw 即 BitBlt 到窗口), 无显式 D3D11/DXGI 层 */
void XjsDeviceCreate() {
    if (!g_hWnd || !g_gfx) return;   /* 表面创建走当前后端函数表 (GDI+ 模式 g_d2d 恒空, 曾致白窗) */
    RECT rc; GetClientRect(g_hWnd, &rc);
    int w = ximax(rc.right, 1), h = ximax(rc.bottom, 1);
    /* RT DPI 钉死 96 (DIP==物理像素): 默认参数会取系统/显示器 DPI 当换算基数,
       与 XSF 的 DPI 缩放叠加成双重放大 (125% 屏上 UI 1.56 倍且右缘被裁)。
       所有尺寸缩放统一由 XSF 负责, RT 在此只做 1:1 位图 (口径在下方 ApiCreateWindowRt) */
    g_gfx->CreateWindowRt(g_hWnd, w, h, &g_hwndRt);
    if (g_hwndRt) g_rt = g_hwndRt;
    if (!g_rt) return;
    /* 同一 RT 取设备上下文视角 (Win8.1+ QI 必成): DrawText 系列才能带
       ENABLE_COLOR_FONT 画 emoji 的 COLR 彩色字形; 失败(老系统)退单色轮廓。
       仅 D2D 后端执行 (GDI+ 模式的 hwndRt 原生是 GpSurface, 瞎 QI = AV, 白窗元凶之一) */
    if (g_gfxEngine == 0) {
        ID2D1DeviceContext* dcp = NULL;
        if (SUCCEEDED(D2(g_hwndRt)->QueryInterface(__uuidof(ID2D1DeviceContext), (void**)&dcp)) && dcp)
            g_dc = new XjsDc(dcp);
    }
    XjsColor theme[XTH_COUNT];
    XjsThemeArray(theme);
    for (int i = 0; i < XTH_COUNT; i++) g_rt->CreateSolidColorBrush(theme[i], &g_br[i]);
    g_rt->CreateSolidColorBrush(XjsColorF(1.0f, 1.0f, 1.0f, 1.0f), &g_brWhite);
    g_rt->CreateSolidColorBrush(XjsCol(0xe0442e), &g_brCloseHover);   /* 官方 System.css 固定值, 非皮肤变量 */
    g_rt->CreateSolidColorBrush(g_skin.err, &g_brErr);
    XjsCreateThemeGradients();
    /* 文本格式 (px 随 DPI × 页面缩放) */
    XjsRecreateTextFormats();
}

void XjsRecreateTextFormats() {
    XjsReleaseTextFormats();
    XjsMakeFormat(&g_tfTitle, 12, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_LEADING, false);    XjsMakeFormat(&g_tfMenu, 12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    XjsMakeFormat(&g_tfHead, 12, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    XjsMakeFormat(&g_tfHeadR, 12, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_TRAILING, true);
    XjsMakeFormat(&g_tfRow, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    XjsMakeFormat(&g_tfRowBold, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    XjsMakeFormat(&g_tfDim, 12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    XjsMakeFormat(&g_tfTiny, 11, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    XjsMakeFormat(&g_tfTinyR, 11, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING, true);
    XjsMakeFormat(&g_tfStatus, 12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    XjsMakeFormat(&g_tfTip, 14, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    XjsMakeFormat(&g_tfChip, 12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, true);
    XjsMakeFormat(&g_tfRowR, 12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING, true);
    XjsMakeFormat(&g_tfBig, 22, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING, false);
    XjsMakeFormat(&g_tfCardVal, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    XjsMakeFormat(&g_tfSearch, 12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    /* Toast 正文 12.5px (System.css .toast-text): line-height 1.55 统一行高 + word-break:break-all;
       顶对齐 (XjsMakeFormat 固定居中, 不适用) — 行高/基线自管, 换行交给布局宽 */
    g_dw->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 12.5f * g_s * XjsUiZoom(), L"zh-cn", &g_tfToast);
    if (g_tfToast) {
        g_tfToast->SetWordWrapping(XJS_WRAP_CHAR);
        g_tfToast->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 19.4f * g_s * XjsUiZoom(), 14.6f * g_s * XjsUiZoom());
    }
    /* 搜索框托管标签 11.5px 中字重 (源样式 .hosted-tag font 12/500, 标题栏紧凑档 11.5) */
    XjsMakeFormat(&g_tfTag, 11.5f, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    /* 记录构建时的 每窗尺度×页面缩放 镜像: 页面缩放每窗化后, 共享格式跟随"当前窗"倍率,
       窗口切换时经 XjsSyncTextFormats 按镜像判失配重建 (XjsMakeFormat/toast 行高都吃这两个值) */
    s_tfDpiS = g_s;
    s_tfZoom = g_uiZoomTenths;
}

bool XjsD2DInit() {
    D2D1_FACTORY_OPTIONS fo = {};
    IDWriteFactory* dwRaw = NULL;
    HRESULT r1 = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &fo, (void**)&g_d2d);
    HRESULT r2 = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&dwRaw);
    HRESULT r3 = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), (void**)&g_wic);
    if (FAILED(r1) || FAILED(r2) || FAILED(r3) || !g_d2d || !dwRaw || !g_wic) {
        if (dwRaw) dwRaw->Release();
        return false;
    }
    g_dw = new XjsDwFactory(dwRaw);
    g_gfx = (XjsGfxApi*)&D2dApi();    return true;
}

void XjsD2DShutdown() {
    if (g_dw) { D2(g_dw)->Release(); delete g_dw; g_dw = NULL; }
    if (g_d2d) { g_d2d->Release(); g_d2d = NULL; }
    if (g_wic) { g_wic->Release(); g_wic = NULL; }
    g_gfx = NULL;
}

/* 窗口尺寸变化: HwndRT 走 RT Resize (Resize 后缓冲内容未定义 → 下一帧必须整窗重画) */
void XjsDeviceResize(int w, int h) {
    if (g_hwndRt) {
        g_hwndRt->Resize((UINT)ximax(w, 1), (UINT)ximax(h, 1));
        g_needFullPaint = true;
    }
}

/* 内存图片 (PNG 等) → D2D 位图 */
XjsBitmap* XjsDecodeImage(const void* data, int len) {
    if (!g_wic || !g_rt || !data || len <= 0) return NULL;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
    if (!hMem) return NULL;
    void* p = GlobalLock(hMem);
    if (!p) { GlobalFree(hMem); return NULL; }
    memcpy(p, data, len);
    GlobalUnlock(hMem);
    IStream* stream = NULL;
    if (CreateStreamOnHGlobal(hMem, TRUE, &stream) != S_OK) { GlobalFree(hMem); return NULL; }
    XjsBitmap* bmp = NULL;
    IWICBitmapDecoder* dec = NULL;
    IWICBitmapFrameDecode* frame = NULL;
    IWICFormatConverter* conv = NULL;
    if (SUCCEEDED(g_wic->CreateDecoderFromStream(stream, NULL, WICDecodeMetadataCacheOnDemand, &dec)) && dec &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && frame &&
        SUCCEEDED(g_wic->CreateFormatConverter(&conv)) && conv &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom))) {
        g_rt->CreateBitmapFromWicBitmap(conv, NULL, &bmp);
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    stream->Release();
    return bmp;
}

/* 裸 32bpp BGRA (预乘 alpha) 像素 → 本 RT 域位图 (插件预览交付用):
   WIC CreateBitmapFromMemory 内部拷贝 — 外部像素一律 CPU 拷贝进本域, 不持外部句柄 (跨渲染域铁律) */
XjsBitmap* XjsBitmapFromBgra(const void* bgra, int w, int h, int stride) {
    if (!g_wic || !g_rt || !bgra || w <= 0 || h <= 0 || stride < w * 4) return NULL;
    IWICBitmap* src = NULL;
    IWICFormatConverter* conv = NULL;
    XjsBitmap* out = NULL;
    if (SUCCEEDED(g_wic->CreateBitmapFromMemory((UINT)w, (UINT)h, GUID_WICPixelFormat32bppPBGRA,
                                                (UINT)stride, (UINT)stride * h, (BYTE*)(void*)bgra, &src)) && src &&
        SUCCEEDED(g_wic->CreateFormatConverter(&conv)) && conv &&
        SUCCEEDED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom))) {
        g_rt->CreateBitmapFromWicBitmap(conv, NULL, &out);
    }
    if (conv) conv->Release();
    if (src) src->Release();
    return out;
}

/* 磁盘图片文件 → D2D 位图 (预览面板) */
XjsBitmap* XjsDecodeFileImage(const std::wstring& path) {
    if (!g_wic || !g_rt || path.empty()) return NULL;
    XjsBitmap* bmp = NULL;
    IWICBitmapDecoder* dec = NULL;
    IWICBitmapFrameDecode* frame = NULL;
    IWICFormatConverter* conv = NULL;
    if (SUCCEEDED(g_wic->CreateDecoderFromFilename(path.c_str(), NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) && dec &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && frame &&
        SUCCEEDED(g_wic->CreateFormatConverter(&conv)) && conv &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom))) {
        g_rt->CreateBitmapFromWicBitmap(conv, NULL, &bmp);
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    return bmp;
}

/* 临时色刷缓存 (同一颜色复用; 设备重建时随 XjsDeviceDiscard 清空) */
XjsSolidBrush* XjsTempBrush(XjsColor c) {
    UINT32 key = ((UINT32)(c.a * 255) << 24) | ((UINT32)(c.r * 255) << 16) | ((UINT32)(c.g * 255) << 8) | (UINT32)(c.b * 255);
    auto& cache = XjsSearchWindow::Cur()->brushCache;   // 画刷绑定 RT: 缓存必须每窗一份
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    XjsSolidBrush* br = NULL;
    if (FAILED(g_rt->CreateSolidColorBrush(c, &br)) || !br) return NULL;
    cache[key] = br;
    return br;
}

float XjsMeasureText(const wchar_t* s, XjsFormat* fmt) {
    if (!s || !fmt || !*s) return 0;
    XjsTextLayout* layout = NULL;
    float w = 0;
    if (SUCCEEDED(g_dw->CreateTextLayout(s, (UINT32)wcslen(s), fmt, 10000.0f, 100.0f, &layout)) && layout) {
        XjsTextMetrics tm;
        layout->GetMetrics(&tm);
        w = tm.width;
        layout->Release();
    }
    return w;
}

/* 实测签名必须逐布局经 SetTrimming 传入才会画出 "...": 传 NULL 只截断无省略号,
   格式级 SetTrimming 对 D2D 的 DrawText/DrawTextLayout 均不生效 */
static IDWriteInlineObject* XjsEllipsisSign(XjsFormat* fmt) {
    auto it = g_ellSignCache.find(fmt);
    if (it != g_ellSignCache.end()) return it->second;
    IDWriteInlineObject* sign = NULL;
    if (FAILED(D2(g_dw)->CreateEllipsisTrimmingSign(D2(fmt), &sign)) || !sign) return NULL;
    g_ellSignCache[fmt] = sign;
    return sign;
}

/* 从签名缓存摘除指定格式的条目: 格式不经 XjsReleaseTextFormats 而被单独释放前必须调用 —
   弹窗 tfTitle 旁路统一释放, 不摘则键悬垂 (地址被新格式复用会命中旧签名画错省略号) */
void XjsEllSignCacheDropFormat(XjsFormat* fmt) {
    auto it = g_ellSignCache.find(fmt);
    if (it != g_ellSignCache.end()) {
        if (it->second) it->second->Release();
        g_ellSignCache.erase(it);
    }
}

/* 彩色字体绘制 (emoji 走 Segoe UI Emoji 的 COLR 字形): g_dc 可用时带 ENABLE_COLOR_FONT,
   否则退 g_rt 单色轮廓。文件名/搜索框等含 emoji 的内容一律走这两个入口 */
void XjsDrawTextC(const wchar_t* s, UINT32 len, XjsFormat* fmt,
                  const XjsRect& r, XjsBrush* brush, D2D1_DRAW_TEXT_OPTIONS opts) {
    if (!g_rt || !s) return;
    if (g_dc) g_dc->DrawText(s, len, fmt, r, brush, opts | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    else g_rt->DrawText(s, len, fmt, r, brush, opts);
}

void XjsDrawTextLayoutC(const XjsPoint2& origin, XjsTextLayout* lay,
                        XjsBrush* brush, D2D1_DRAW_TEXT_OPTIONS opts) {
    if (!g_rt || !lay) return;
    if (g_dc) g_dc->DrawTextLayout(origin, lay, brush, opts | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    else g_rt->DrawTextLayout(origin, lay, brush, opts);
}

void XjsDrawEllText(const std::wstring& s, const XjsRect& r, XjsFormat* fmt, XjsBrush* brush) {
    if (s.empty() || !fmt || !g_rt) return;
    float maxW = r.right - r.left;
    if (maxW <= 0) return;
    XjsTextLayout* layout = NULL;
    if (FAILED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.length(), fmt, maxW, r.bottom - r.top, &layout)) || !layout)
        return;
    layout->SetWordWrapping(XJS_WRAP_NONE);
    layout->SetCharEllipsis();
    XjsDrawTextLayoutC(XjsPoint2F(r.left, r.top), layout, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    layout->Release();
}

/* 逐段绘制高亮文本; 段尾超出 clip 右界时对该段做字符级截断补 "..." 后结束。
   旧写法超宽段只画可见前缀无省略号, 更早的版本整段消失 */
void XjsDrawHlSegs(const std::vector<XjsHlSeg>& segs, float x, const XjsRect& clip, XjsFormat* fmt) {
    float cx = x;
    for (auto& seg : segs) {
        if (cx >= clip.right) break;
        float w = XjsMeasureText(seg.text.c_str(), fmt);
        XjsBrush* br = seg.isKey ? (XjsBrush*)g_br[XTH_HL] : (XjsBrush*)g_br[XTH_TEXT];
        if (cx + w > clip.right) {
            XjsDrawEllText(seg.text, XjsRectF(cx, clip.top, clip.right, clip.bottom), fmt, br);
            break;
        }
        XjsRect r = XjsRectF(cx, clip.top, cx + w + 1, clip.bottom);
        XjsDrawTextC(seg.text.c_str(), (UINT32)seg.text.length(), fmt, r, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        cx += w;
    }
}

void XjsDrawMagnifier(XjsPoint2 c, float r, XjsBrush* brush, float stroke) {
    g_rt->DrawEllipse(XjsEllipseF(XjsPoint2F(c.x - r * 0.2f, c.y - r * 0.2f), r * 0.8f, r * 0.8f), brush, stroke);
    g_rt->DrawLine(XjsPoint2F(c.x + r * 0.35f, c.y + r * 0.35f), XjsPoint2F(c.x + r, c.y + r), brush, stroke);
}

void XjsDrawSpinner(XjsPoint2 c, float r, float phase) {
    XjsColor dim = g_br[XTH_ACCENT]->GetColor();
    dim.a = 0.25f;
    XjsSolidBrush* db = XjsTempBrush(dim);
    if (db) g_rt->DrawEllipse(XjsEllipseF(c, r, r), db, 1.6f);
    /* 弧 = 圆弧折线近似 (24 段, 视觉与 PathGeometry 圆弧一致, 且后端无关) */
    float a0 = phase, a1 = phase + 1.6f;
    const int SEG = 24;
    XjsPoint2 prev = { c.x + r * cosf(a0), c.y + r * sinf(a0) };
    for (int i = 1; i <= SEG; i++) {
        float a = a0 + (a1 - a0) * i / SEG;
        XjsPoint2 p = { c.x + r * cosf(a), c.y + r * sinf(a) };
        g_rt->DrawLine(prev, p, g_br[XTH_ACCENT], 1.6f);
        prev = p;
    }
}

/* ==================== D2D 后端函数表 (XjsGfxApi 实现; 增删绘制原语只动这里与表) ==================== */

void XjsDc::DrawText(const wchar_t* s, UINT32 len, XjsFormat* f, const XjsRect& r, XjsBrush* b, XjsDrawTextOpts o) {
    ID2D1DeviceContext* c = (ID2D1DeviceContext*)h;
    if (!c || !s) return;
    c->DrawText(s, len, (IDWriteTextFormat*)f->h, *(const D2D1_RECT_F*)&r, (ID2D1Brush*)b->h,
                o | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}
void XjsDc::DrawTextLayout(XjsPoint2 org, XjsTextLayout* l, XjsBrush* b, XjsDrawTextOpts o) {
    ID2D1DeviceContext* c = (ID2D1DeviceContext*)h;
    if (!c || !l) return;
    c->DrawTextLayout(*(const D2D1_POINT_2F*)&org, (IDWriteTextLayout*)l->h, (ID2D1Brush*)b->h,
                      o | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

static void ApiCreateWindowRt(HWND hwnd, int w, int h, XjsRt** out) {
    /* RT DPI 钉死 96: DIP==物理像素, 缩放统一由 XSF 负责 (双重放大红线, 见 AGENTS.md) */
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), 96.0f, 96.0f);
    ID2D1HwndRenderTarget* rt = NULL;
    if (FAILED(g_d2d->CreateHwndRenderTarget(props,
            D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(ximax(w, 1), ximax(h, 1))), &rt)) || !rt) return;
    *out = new XjsRt(rt);
}
static void ApiBeginDraw(XjsRt* rt)                 { D2(rt)->BeginDraw(); }
static HRESULT ApiEndDraw(XjsRt* rt)                { return D2(rt)->EndDraw(); }
static void ApiClear(XjsRt* rt, const XjsColor& c)  { D2(rt)->Clear(*(const D2D1_COLOR_F*)&c); }
static void ApiResize(XjsRt* rt, UINT w, UINT hh)   { ((ID2D1HwndRenderTarget*)rt->h)->Resize(D2D1::SizeU(w, ximax(hh, 1))); }
static XjsSizeU ApiPixelSize(XjsRt* rt) {
    D2D1_SIZE_U s = D2(rt)->GetPixelSize();
    return { s.width, s.height };
}
static void ApiFillRect(XjsRt* rt, const XjsRect& r, XjsBrush* b)              { D2(rt)->FillRectangle(*(const D2D1_RECT_F*)&r, D2(b)); }
static void ApiFillRoundRect(XjsRt* rt, const XjsRoundedRect& r, XjsBrush* b)  { D2(rt)->FillRoundedRectangle((D2D1_ROUNDED_RECT)r, D2(b)); }
static void ApiFrameRect(XjsRt* rt, const XjsRect& r, XjsBrush* b, float w, XjsStroke* st)      { D2(rt)->DrawRectangle(*(const D2D1_RECT_F*)&r, D2(b), w, st ? D2(st) : NULL); }
static void ApiFrameRoundRect(XjsRt* rt, const XjsRoundedRect& r, XjsBrush* b, float w, XjsStroke* st) { D2(rt)->DrawRoundedRectangle((D2D1_ROUNDED_RECT)r, D2(b), w, st ? D2(st) : NULL); }
static void ApiLine(XjsRt* rt, XjsPoint2 a, XjsPoint2 b2, XjsBrush* br, float w, XjsStroke* st) { D2(rt)->DrawLine(*(const D2D1_POINT_2F*)&a, *(const D2D1_POINT_2F*)&b2, D2(br), w, st ? D2(st) : NULL); }
static void ApiFrameEllipse(XjsRt* rt, const XjsEllipse& e, XjsBrush* b, float w, XjsStroke* st) { D2(rt)->DrawEllipse(*(const D2D1_ELLIPSE*)&e, D2(b), w, st ? D2(st) : NULL); }
static void ApiFillEllipse(XjsRt* rt, const XjsEllipse& e, XjsBrush* b)        { D2(rt)->FillEllipse(*(const D2D1_ELLIPSE*)&e, D2(b)); }
static void ApiDrawTextStr(XjsRt* rt, const wchar_t* s, UINT32 len, XjsFormat* f, const XjsRect& r,
                           XjsBrush* b, XjsDrawTextOpts o, int measuring) {
    D2(rt)->DrawText(s, len, D2(f), *(const D2D1_RECT_F*)&r, D2(b), o, (DWRITE_MEASURING_MODE)measuring);
}
static void ApiDrawTextLay(XjsRt* rt, XjsPoint2 org, XjsTextLayout* l, XjsBrush* b, XjsDrawTextOpts o) {
    D2(rt)->DrawTextLayout(*(const D2D1_POINT_2F*)&org, D2(l), D2(b), o);
}
static void ApiPushClip(XjsRt* rt, const XjsRect& r, int aa)  { D2(rt)->PushAxisAlignedClip(*(const D2D1_RECT_F*)&r, (D2D1_ANTIALIAS_MODE)aa); }
static void ApiPopClip(XjsRt* rt)                             { D2(rt)->PopAxisAlignedClip(); }
static void ApiSetRotate(XjsRt* rt, float deg, XjsPoint2 c)   { D2(rt)->SetTransform(D2D1::Matrix3x2F::Rotation(deg, *(const D2D1_POINT_2F*)&c)); }
static void ApiResetTransform(XjsRt* rt)                      { D2(rt)->SetTransform(D2D1::Matrix3x2F::Identity()); }
static void ApiPushAlpha(XjsRt* rt, const XjsRect& r, float opacity) {
    ID2D1Layer*& layer = s_layers[rt];
    if (!layer && FAILED(D2(rt)->CreateLayer(&layer))) { s_layers.erase(rt); return; }
    if (!layer) return;
    D2D1_LAYER_PARAMETERS lp = D2D1::LayerParameters(*(const D2D1_RECT_F*)&r, NULL,
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::Matrix3x2F::Identity(), opacity, NULL, D2D1_LAYER_OPTIONS_NONE);
    D2(rt)->PushLayer(lp, layer);
}
static void ApiPopAlpha(XjsRt* rt) {
    auto it = s_layers.find(rt);
    if (it != s_layers.end() && it->second) D2(rt)->PopLayer();
}
static void ApiBitmapFromWic(XjsRt* rt, IWICBitmapSource* src, XjsBitmap** out) {
    ID2D1Bitmap* nb = NULL;
    if (FAILED(D2(rt)->CreateBitmapFromWicBitmap(src, NULL, &nb)) || !nb) return;
    *out = new XjsBitmap(nb);
}
static void ApiBitmapFromMemory(XjsRt* rt, UINT w, UINT h, UINT stride, const BYTE* px, XjsBitmap** out) {
    /* 字节序 = B8G8R8A8 (PrintWindow 背板产物), alpha 忽略 */
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    ID2D1Bitmap* nb = NULL;
    if (FAILED(D2(rt)->CreateBitmap(D2D1::SizeU(w, ximax(h, 1)), px, stride, bp, &nb)) || !nb) return;
    *out = new XjsBitmap(nb);
}
static void ApiDrawBmp(XjsRt* rt, XjsBitmap* bm, const XjsRect& r, float opacity, int interp, const XjsRect* src) {
    D2(rt)->DrawBitmap(D2(bm), *(const D2D1_RECT_F*)&r, opacity,
                       (D2D1_BITMAP_INTERPOLATION_MODE)interp, (const D2D1_RECT_F*)src);
}
static XjsSizeU ApiBmpPixelSize(XjsBitmap* bm) {
    D2D1_SIZE_U s = D2(bm)->GetPixelSize();
    return { s.width, s.height };
}
static void ApiFreeRt(void* n)     { if (n) ((ID2D1RenderTarget*)n)->Release(); }
static void ApiFreeBitmap(void* n) { if (n) ((ID2D1Bitmap*)n)->Release(); }
static void ApiFreeBrush(void* n)  { if (n) ((ID2D1Brush*)n)->Release(); }
static void ApiFreeStroke(void* n) {
    if (!n) return;
    if (n == (void*)s_roundStroke) s_roundStroke = NULL;   /* 末份释放后单例指针失效 */
    ((ID2D1StrokeStyle*)n)->Release();
}
static void ApiFreeGeo(void* n)    { if (n) ((ID2D1Geometry*)n)->Release(); }
static void ApiFreeFormat(void* n) { if (n) ((IDWriteTextFormat*)n)->Release(); }
static void ApiFreeLayout(void* n) { if (n) ((IDWriteTextLayout*)n)->Release(); }
static HRESULT ApiSolidBrush(XjsRt* rt, const XjsColor& c, XjsSolidBrush** out) {
    ID2D1SolidColorBrush* nb = NULL;
    HRESULT hr = D2(rt)->CreateSolidColorBrush(*(const D2D1_COLOR_F*)&c, &nb);
    if (FAILED(hr) || !nb) return hr;
    *out = new XjsSolidBrush(nb);
    return S_OK;
}
static void ApiGradBrush(XjsRt* rt, XjsPoint2 s, XjsPoint2 e, const XjsGradientStop* st, int n, XjsGradBrush** out) {
    ID2D1GradientStopCollection* sc = NULL;
    if (FAILED(D2(rt)->CreateGradientStopCollection((const D2D1_GRADIENT_STOP*)st, n, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &sc)) || !sc) return;
    ID2D1LinearGradientBrush* nb = NULL;
    if (SUCCEEDED(D2(rt)->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
            *(const D2D1_POINT_2F*)&s, *(const D2D1_POINT_2F*)&e), sc, &nb)) && nb)
        *out = new XjsGradBrush(nb);
    sc->Release();
}
static void ApiRadialBrush(XjsRt* rt, XjsPoint2 c, XjsPoint2 off, float rx, float ry, const XjsGradientStop* st, int n, XjsGradBrush** out) {
    ID2D1GradientStopCollection* sc = NULL;
    if (FAILED(D2(rt)->CreateGradientStopCollection((const D2D1_GRADIENT_STOP*)st, n, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &sc)) || !sc) return;
    ID2D1RadialGradientBrush* nb = NULL;
    if (SUCCEEDED(D2(rt)->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(
            *(const D2D1_POINT_2F*)&c, *(const D2D1_POINT_2F*)&off, rx, ry), sc, &nb)) && nb)
        *out = new XjsGradBrush(nb);
    sc->Release();
}
static void ApiSetBrushColor(XjsSolidBrush* b, const XjsColor& c)     { D2(b)->SetColor(*(const D2D1_COLOR_F*)&c); }
static XjsColor ApiGetBrushColor(XjsSolidBrush* b)                    { return D2(b)->GetColor(); }
static void ApiRoundStroke(XjsStroke** out) {
    /* 圆头描边 (SVG stroke-linecap/linejoin=round), 设备无关建一次。
       原生样式进程一份, 但每次调用返回【新包装】并对原生 AddRef —— 消费者 (菜单/设置窗/toast)
       各自按"自己拥有"Release, 单例包装曾被 WM_DESTROY 释放后二次分发 = UAF/双重删除 (堆损坏崩溃) */
    ID2D1StrokeStyle*& st = s_roundStroke;
    if (!st) {
        D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
            D2D1_LINE_JOIN_ROUND, 0.0f, D2D1_DASH_STYLE_SOLID);
        if (FAILED(g_d2d->CreateStrokeStyle(sp, NULL, 0, &st)) || !st) return;
    }
    st->AddRef();
    *out = new XjsStroke(st);
}
static void ApiRoundRectGeo(const XjsRoundedRect& r, XjsGeo** out) {
    ID2D1RoundedRectangleGeometry* g = NULL;
    if (FAILED(g_d2d->CreateRoundedRectangleGeometry((D2D1_ROUNDED_RECT)r, &g)) || !g) return;
    *out = new XjsGeo(g);
}
static void ApiFillGeoIntersectRect(XjsRt* rt, XjsGeo* geo, const XjsRect& r, XjsBrush* b) {
    /* 矩形 ∩ 圆角几何求交填充 (toast 色条/进度条端头跟随圆角; 原 toast 内联实现收编) */
    if (!geo || !b) return;
    ID2D1RectangleGeometry* rectGeo = NULL;
    if (FAILED(g_d2d->CreateRectangleGeometry(*(const D2D1_RECT_F*)&r, &rectGeo)) || !rectGeo) return;
    ID2D1PathGeometry* cut = NULL;
    ID2D1GeometrySink* sink = NULL;
    if (SUCCEEDED(g_d2d->CreatePathGeometry(&cut)) && cut &&
        SUCCEEDED(cut->Open(&sink)) && sink) {
        if (SUCCEEDED(((ID2D1RoundedRectangleGeometry*)geo->h)->CombineWithGeometry(
                rectGeo, D2D1_COMBINE_MODE_INTERSECT, NULL, sink)))
            sink->Close();
        D2(rt)->FillGeometry(cut, D2(b));
    }
    if (sink) sink->Release();
    if (cut) cut->Release();
    rectGeo->Release();
}
static void ApiMakeFormat(const wchar_t* family, void* coll, float weight, int style, int stretch,
                          float px, const wchar_t* locale, XjsFormat** out) {
    IDWriteTextFormat* f = NULL;
    if (FAILED(D2(g_dw)->CreateTextFormat(family, (IDWriteFontCollection*)coll, (DWRITE_FONT_WEIGHT)(UINT32)weight,
            (DWRITE_FONT_STYLE)style, (DWRITE_FONT_STRETCH)stretch, px, locale, &f)) || !f) return;
    *out = new XjsFormat(f);
}
static void ApiFormatSetAlign(XjsFormat* f, int a)        { D2(f)->SetTextAlignment((DWRITE_TEXT_ALIGNMENT)a); }
static void ApiFormatSetParaAlign(XjsFormat* f, int a)    { D2(f)->SetParagraphAlignment((DWRITE_PARAGRAPH_ALIGNMENT)a); }
static void ApiFormatSetWrap(XjsFormat* f, int w)         { D2(f)->SetWordWrapping((DWRITE_WORD_WRAPPING)w); }
static void ApiFormatSetCharEllipsis(XjsFormat* f) {
    DWRITE_TRIMMING t = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0x2026, 1 };
    D2(f)->SetTrimming(&t, XjsEllipsisSign(f));
}
static void ApiFormatSetLineSpacing(XjsFormat* f, int m, float s, float b2) {
    D2(f)->SetLineSpacing((DWRITE_LINE_SPACING_METHOD)m, s, b2);
}
static float ApiFormatFontSize(XjsFormat* f)              { return D2(f)->GetFontSize(); }
static HRESULT ApiMakeLayout(const wchar_t* text, UINT32 len, XjsFormat* f, float maxW, float maxH, XjsTextLayout** out) {
    IDWriteTextLayout* l = NULL;
    HRESULT hr = D2(g_dw)->CreateTextLayout(text, len, D2(f), maxW, maxH, &l);
    if (FAILED(hr) || !l) return hr;
    *out = new XjsTextLayout(l);
    return S_OK;
}
static HRESULT ApiLayoutMetrics(XjsTextLayout* l, XjsTextMetrics* m) {
    DWRITE_TEXT_METRICS tm = {};
    D2(l)->GetMetrics(&tm);
    *m = { tm.left, tm.top, tm.width, tm.widthIncludingTrailingWhitespace, tm.height };
    return S_OK;
}
static void ApiLayoutSetWrap(XjsTextLayout* l, int w)          { D2(l)->SetWordWrapping((DWRITE_WORD_WRAPPING)w); }
static void ApiLayoutSetCharEllipsis(XjsTextLayout* l) {
    /* 签名必须从布局自身创建 (布局 IS-A 格式): NULL 签名 = 纯截断无 "..." (红线配方) */
    IDWriteInlineObject* sign = NULL;
    if (SUCCEEDED(D2(g_dw)->CreateEllipsisTrimmingSign(D2(l), &sign))) {
        DWRITE_TRIMMING t = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0x2026, 1 };
        D2(l)->SetTrimming(&t, sign);
        sign->Release();
    }
}
static void ApiLayoutSetLineSpacing(XjsTextLayout* l, int m, float s, float b2) {
    D2(l)->SetLineSpacing((DWRITE_LINE_SPACING_METHOD)m, s, b2);
}
static bool ApiLayoutHitTest(XjsTextLayout* l, UINT32 idx, BOOL trailing, float* x, float* y, XjsHitTestMetrics* m) {
    DWRITE_HIT_TEST_METRICS hm = {};
    float hx = 0, hy = 0;
    if (FAILED(D2(l)->HitTestTextPosition(idx, trailing, &hx, &hy, &hm))) return false;
    if (x) *x = hx;
    if (y) *y = hy;
    *m = { hm.left, hm.top, hm.width, hm.height, hm.textPosition, hm.length };
    return true;
}

static void ApiGradSetStart(XjsGradBrush* b, XjsPoint2 p)  { D2(b)->SetStartPoint(*(const D2D1_POINT_2F*)&p); }
static void ApiGradSetEnd(XjsGradBrush* b, XjsPoint2 p)    { D2(b)->SetEndPoint(*(const D2D1_POINT_2F*)&p); }
static void ApiLayoutFontWeight(XjsTextLayout* l, float w, XjsTextRange rg)  { D2(l)->SetFontWeight((DWRITE_FONT_WEIGHT)(UINT32)w, *(const DWRITE_TEXT_RANGE*)&rg); }
static void ApiLayoutFontStyle(XjsTextLayout* l, int s, XjsTextRange rg)     { D2(l)->SetFontStyle((DWRITE_FONT_STYLE)s, *(const DWRITE_TEXT_RANGE*)&rg); }
static void ApiLayoutFontFamily(XjsTextLayout* l, const wchar_t* f, XjsTextRange rg) { D2(l)->SetFontFamilyName(f, *(const DWRITE_TEXT_RANGE*)&rg); }
static void ApiLayoutFontSize(XjsTextLayout* l, float px, XjsTextRange rg)   { D2(l)->SetFontSize(px, *(const DWRITE_TEXT_RANGE*)&rg); }
static void ApiLayoutDrawingEffect(XjsTextLayout* l, void* effect, XjsTextRange rg) {
    /* effect = 接缝画刷包装 (XjsSolidBrush*), 必须解包装再交给 DWrite (接缝约定: UI 侧只递包装)。
       直传包装 = DWrite 存属性时对该指针走 IUnknown::AddRef (虚表第 1 槽), 而包装类仅有虚析构,
       槽位越界命中相邻 RTTI 数据地址 → 跳只读数据执行 0xC0000005 —— md 链接渲染崩溃根因
       (2026-09-18 实锤: 许可证页的邮箱自动链接一渲染就崩; GDI+ 侧 AsLay 存包装后自解, 本处漏解) */
    D2(l)->SetDrawingEffect(effect ? D2((XjsBrush*)effect) : NULL, *(const DWRITE_TEXT_RANGE*)&rg);
}
static void ApiLayoutUnderline(XjsTextLayout* l, BOOL on, XjsTextRange rg)   { D2(l)->SetUnderline(on, *(const DWRITE_TEXT_RANGE*)&rg); }
static HRESULT ApiLayoutHitRange(XjsTextLayout* l, UINT32 pos, UINT32 len, float ox, float oy, XjsHitTestMetrics* out, UINT32 max, UINT32* returned) {
    return D2(l)->HitTestTextRange(pos, len, ox, oy, (DWRITE_HIT_TEST_METRICS*)out, max, returned);
}
static bool ApiLayoutHitPoint(XjsTextLayout* l, float x, float y, BOOL* trail, BOOL* inside, XjsHitTestMetrics* m) {
    DWRITE_HIT_TEST_METRICS hm = {};
    BOOL t = FALSE, i2 = FALSE;
    if (FAILED(D2(l)->HitTestPoint(x, y, &t, &i2, &hm))) return false;
    if (trail) *trail = t;
    if (inside) *inside = i2;
    *m = { hm.left, hm.top, hm.width, hm.height, hm.textPosition, hm.length };
    return true;

}

static const XjsGfxApi& D2dApi() {
    static XjsGfxApi api = {
        ApiCreateWindowRt, ApiFreeRt,
        ApiBeginDraw, ApiEndDraw, ApiClear, ApiResize, ApiPixelSize,
        ApiFillRect, ApiFillRoundRect, ApiFrameRect, ApiFrameRoundRect, ApiLine,
        ApiFrameEllipse, ApiFillEllipse, ApiDrawTextStr, ApiDrawTextLay,
        ApiPushClip, ApiPopClip, ApiSetRotate, ApiResetTransform, ApiPushAlpha, ApiPopAlpha,
        ApiBitmapFromWic, ApiBitmapFromMemory, ApiDrawBmp, ApiBmpPixelSize, ApiFreeBitmap,
        ApiSolidBrush, ApiGradBrush, ApiRadialBrush, ApiSetBrushColor, ApiGetBrushColor, ApiFreeBrush,
        ApiGradSetStart, ApiGradSetEnd,
        ApiLayoutFontWeight, ApiLayoutFontStyle, ApiLayoutFontFamily, ApiLayoutFontSize,
        ApiLayoutDrawingEffect, ApiLayoutUnderline, ApiLayoutHitRange, ApiLayoutHitPoint,
        ApiFormatSetCharEllipsis, ApiLayoutSetCharEllipsis,
        ApiRoundStroke, ApiFreeStroke, ApiRoundRectGeo, ApiFillGeoIntersectRect, ApiFreeGeo,
        ApiMakeFormat, ApiFormatSetAlign, ApiFormatSetParaAlign, ApiFormatSetWrap,
        ApiFormatSetLineSpacing, ApiFormatFontSize, ApiFreeFormat,
        ApiMakeLayout, ApiLayoutMetrics, ApiLayoutSetWrap,
        ApiLayoutSetLineSpacing, ApiLayoutHitTest, ApiFreeLayout,
    };
    return api;
}
