/*
 * xjs_guide.cpp — 新手引导动画 (一文件一职责; 源样式 25-guide.js + System.css .guide-* 族同构)
 *
 * 结构 = 遮罩聚光灯 + accent 呼吸高亮环 + 说明卡 (标题/正文/步数/上一步/下一步·完成/跳过引导/不再提示)
 * + 演示鼠标点击动画 (光标 PNG 内嵌 + accent 涟漪; 400ms 后按压 650ms, 每步一次)。
 * 启动 = 主窗创建 800ms 后未"不再提示"自动播放 (--autostart 托盘隐藏不播); 走完最后一步或点
 * "不再提示" = 记忆并落盘, 跳过/Esc 不记; ☰菜单"引导动画"随时重开 (手动打开不改记忆)。
 * 引导期全部真实交互被吞 (main.cpp 输入总闸路由 XjsGuideMsg), 仅卡片按钮与 Esc 可用;
 * 步骤目标不可见即剔除 (预览面板关/状态栏关/筛选框关 = 布局零矩形, 启动时过滤)。
 * 目标矩形每帧从 g_layout 现取, 不缓存指针 (可维护性红线: 禁把窗口状态地址存 static)。
 * 洞为直角矩形 (抽象渲染表无"挖洞几何"原语, 两后端同代码): accent 圆角环盖住洞缘, 观感同原版。
 */
#include "xjs_app.h"
#include <cmath>
#include <vector>

/* 演示鼠标光标 (源样式 .guide-cursor svg: 26x32, fill rgba(0,0,0,.75) + 白描边 1.2px round-join;
   8x 超采样预栅格化 PNG, 经 XjsDecodeImage 解码 — 生成器 test\gen_guide_cursor.js) */
static const unsigned char kGuideCursorPng[] = {
    0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,
    0x00,0x00,0x00,0x1a,0x00,0x00,0x00,0x20,0x08,0x06,0x00,0x00,0x00,0x0c,0xab,0x68,
    0x05,0x00,0x00,0x01,0x93,0x49,0x44,0x41,0x54,0x78,0xda,0xed,0x95,0x2f,0xaf,0x82,
    0x60,0x14,0xc6,0xdf,0xb3,0x11,0x0c,0x04,0x83,0xc1,0x60,0x20,0x18,0x08,0x44,0x23,
    0x1f,0xc0,0x40,0x20,0x18,0x88,0x46,0x83,0x81,0x68,0xb0,0x19,0x09,0x04,0x82,0x91,
    0x40,0x30,0x18,0x0c,0x7e,0x00,0x83,0xc1,0x60,0x20,0x18,0x88,0x06,0x02,0xc1,0x68,
    0x34,0x78,0xf7,0x9c,0x5d,0x18,0xb3,0x38,0xd4,0x73,0xb7,0x7b,0x77,0xcf,0xf6,0x14,
    0xc6,0xde,0xdf,0x7b,0x9e,0xf7,0xfc,0x51,0xea,0xa7,0x23,0x8a,0x22,0x8a,0xe3,0x98,
    0x74,0x5d,0x97,0x05,0x9d,0xcf,0x67,0xba,0xdf,0xef,0xb4,0xdf,0xef,0xa9,0xdd,0x6e,
    0xcb,0x83,0xa0,0x2c,0xcb,0xa8,0xd7,0xeb,0xc9,0x81,0xa0,0xd9,0x6c,0xc6,0xb0,0xa2,
    0x28,0xc8,0xb2,0x2c,0x39,0x90,0x52,0x8a,0xc6,0xe3,0x31,0xdd,0x6e,0x37,0xba,0x5e,
    0xaf,0x64,0xdb,0xb6,0x1c,0x08,0x1a,0x0e,0x87,0x0c,0x02,0x70,0x34,0x1a,0xc9,0x81,
    0xa0,0xc1,0x60,0xc0,0x16,0xc2,0x4a,0xdf,0xf7,0xe5,0x40,0x90,0x69,0x9a,0x5c,0x1c,
    0x80,0x85,0x61,0x48,0x62,0x20,0xa8,0xdb,0xed,0xd2,0xe1,0x70,0x60,0xd8,0x6a,0xb5,
    0x22,0x4d,0xd3,0x64,0x40,0x90,0xae,0xeb,0xb4,0xdd,0x6e,0x19,0xb6,0xdb,0xed,0x5e,
    0x6f,0xec,0x67,0x20,0x48,0xd3,0x34,0x9e,0x1e,0x80,0x9d,0x4e,0x27,0x64,0x2a,0x03,
    0x2a,0xb5,0x58,0x2c,0x18,0x86,0xff,0x4d,0xd3,0x94,0x03,0x41,0xbe,0xef,0x33,0xec,
    0x72,0xb9,0x34,0xeb,0xb5,0x67,0x20,0xd7,0x75,0xd9,0x36,0xcc,0xc2,0x3c,0xcf,0xab,
    0x71,0x05,0xa5,0x69,0x4a,0x2f,0x81,0x2c,0xcb,0x62,0xd5,0x41,0xc7,0xe3,0x91,0x0f,
    0x45,0x13,0xe3,0x7d,0x36,0x9b,0x0d,0x97,0xfb,0x74,0x3a,0x55,0x8d,0x46,0x55,0x09,
    0x02,0x00,0x87,0xe1,0xd6,0x75,0xd0,0x7c,0x3e,0x67,0x90,0xeb,0xba,0xef,0xf7,0x11,
    0xfc,0xae,0xdb,0x62,0xdb,0x76,0x05,0xea,0xf7,0xfb,0xfc,0x2d,0x49,0x12,0xfa,0xd8,
    0x9a,0x28,0x4b,0xf8,0x7b,0x12,0x54,0xc2,0x5b,0x20,0xdb,0x56,0xab,0xf5,0x3e,0x28,
    0x08,0x02,0xbe,0x31,0x32,0x13,0xb1,0xaf,0x5c,0xe5,0xe5,0x78,0x41,0x36,0x62,0xf6,
    0xd5,0x03,0xbd,0x21,0x66,0xdf,0x63,0x88,0xd9,0xf7,0x18,0x7f,0xdb,0x3e,0xc3,0x30,
    0xc8,0xf3,0x3c,0x5a,0x2e,0x97,0xd5,0xd6,0x75,0x1c,0xe7,0xf3,0xf6,0xd5,0x05,0xd0,
    0x7a,0xbd,0xa6,0x4e,0xa7,0xf3,0x39,0x10,0xe6,0x18,0xac,0x42,0xe9,0x4f,0x26,0x13,
    0xd5,0x78,0x2d,0xfc,0xc7,0xaf,0x89,0x2f,0xe1,0x95,0x74,0x0d,0xd2,0xfb,0x68,0x23,
    0x00,0x00,0x00,0x00,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82,
};

namespace {

/* 步骤表 (源样式 guideSteps 同序; target = 布局矩形, text/title = i18n 点分主键 — 读取点现取 XjsT) */
enum { GT_SEARCHBOX = 0, GT_MODEBTN, GT_HISTBTN, GT_LIST, GT_PREVIEW, GT_MENUBTN, GT_FILTERBTN, GT_STATUSBAR };
enum { GC_NONE = 0, GC_INPUT, GC_CLICK };   /* cursor: 演示形态 (源样式 'input' = 框内落点, 'click' = 居中) */

struct GuideStep { int target; const wchar_t* title; const wchar_t* text; int cursor; };

const GuideStep kGuideSteps[] = {
    { GT_SEARCHBOX, L"引导.搜索框标题",   L"引导.搜索框文本",   GC_INPUT },
    { GT_MODEBTN,   L"引导.搜索模式标题", L"引导.搜索模式文本", GC_CLICK },
    { GT_HISTBTN,   L"引导.搜索历史标题", L"引导.搜索历史文本", GC_CLICK },
    { GT_SEARCHBOX, L"引导.右键菜单标题", L"引导.右键菜单文本", GC_CLICK },
    { GT_LIST,      L"引导.文件列表标题", L"引导.文件列表文本", GC_CLICK },
    { GT_PREVIEW,   L"引导.预览面板标题", L"引导.预览面板文本", GC_NONE },
    { GT_SEARCHBOX, L"引导.历史导航标题", L"引导.历史导航文本", GC_NONE },
    { GT_MENUBTN,   L"引导.主菜单标题",   L"引导.主菜单文本",   GC_NONE },
    { GT_FILTERBTN, L"引导.分类筛选标题", L"引导.分类筛选文本", GC_NONE },
    { GT_STATUSBAR, L"引导.状态栏标题",   L"引导.状态栏文本",   GC_NONE },
};
const int kGuideStepN = (int)(sizeof(kGuideSteps) / sizeof(kGuideSteps[0]));

/* 卡内命令控件编码 (按下待定/悬停共用): 松开仍命中同一控件才触发 (ModeDlg pressCmd 同口径) */
enum { GBTN_NONE = 0, GBTN_NOMORE = 1, GBTN_SKIP = 2, GBTN_PREV = 3, GBTN_NEXT = 4 };

/* "不再提示" 记忆 (进程共享; K_GUIDE 经 XjsLoadConfig 读入 / XjsSaveConfig 随档落盘) */
bool s_guideDismissed = false;

/* 点击演示时序 (源样式 renderGuideStep: 400ms 后加 .clicking, 动画 0.6s; 按压 0.65s 内 60% 处回弹) */
const double GUIDE_CLICK_AT = 400.0, GUIDE_CLICK_MS = 650.0, GUIDE_RIPPLE_MS = 600.0;

XjsRect GuideTargetRect(int t) {
    XjsLayout& L = g_layout;
    switch (t) {
        case GT_SEARCHBOX: return L.searchBox;
        case GT_MODEBTN:   return L.modeBtn;
        case GT_HISTBTN:   return L.historyBtn;
        case GT_LIST:      return L.list;
        case GT_PREVIEW:   return L.preview;
        case GT_MENUBTN:   return L.menuBtn;
        case GT_FILTERBTN: return L.filterBtn;
        case GT_STATUSBAR: return L.statusbar;
    }
    return XjsRectF(0, 0, 0, 0);
}

bool GuideRectVisible(const XjsRect& r) { return r.right - r.left >= 1 && r.bottom - r.top >= 1; }

inline float GuideClamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* 卡片几何 (渲染/命中两处同源): 洞 = 目标矩形; 卡默认洞下 14px, 放不下翻上方, 左右夹取 (源样式同式)。
   正文按 \n 分段 (语言包静态受控换行), 段内由格式自动换行, 高 = DirectWrite 实测 (设置页说明行同法) */
struct GuideRects {
    XjsRect spot{}, card{};
    XjsRect noMore{}, skip{}, prev{}, next{};
    float textTop = 0, actY = 0, navY = 0, btnH = 0;
};

float GuideTextParas(const wchar_t* key, float maxW, std::vector<std::wstring>* paras) {
    const wchar_t* s = XjsT(key);
    float total = 0;
    const wchar_t* p = s;
    while (p) {
        const wchar_t* q = wcschr(p, L'\n');
        paras->push_back(q ? std::wstring(p, q) : std::wstring(p));
        const std::wstring& seg = paras->back();
        float h = XSF(20.4f);   /* 资源未就绪/空段: 单行估计 (行距 = 12px × 1.7, 源样式 line-height 同) */
        if (!seg.empty() && g_dw && g_tfGuideText) {
            XjsTextLayout* lay = NULL;
            if (SUCCEEDED(g_dw->CreateTextLayout(seg.c_str(), (UINT32)seg.length(), g_tfGuideText, maxW, 10000.0f, &lay)) && lay) {
                XjsTextMetrics m = {};
                if (SUCCEEDED(lay->GetMetrics(&m)) && m.height > 0) h = m.height;
                lay->Release();
            }
        }
        total += h;
        p = q ? q + 1 : NULL;
    }
    return total;
}

GuideRects GuideLayout(float W, float H, std::vector<std::wstring>* paras) {
    XjsGuide& gd = g_guide;
    const GuideStep& st = kGuideSteps[gd.map[gd.idx]];
    GuideRects R;
    R.spot = GuideTargetRect(st.target);
    const float pad = XSF(16), padV = XSF(14), cardW = XSF(320), titleH = XSF(20), btnH = XSF(26);
    R.btnH = btnH;
    float textH = GuideTextParas(st.text, cardW - pad * 2, paras);
    float cardH = padV + titleH + XSF(8) + textH + XSF(12) + btnH + XSF(10) + btnH + padV;
    float cx = GuideClamp(R.spot.left, XSF(8), xf_max(XSF(8), W - cardW - XSF(8)));
    float cy = R.spot.bottom + XSF(14);
    if (cy + cardH > H - XSF(8)) cy = xf_max(XSF(8), R.spot.top - cardH - XSF(14));
    cy = GuideClamp(cy, XSF(8), xf_max(XSF(8), H - cardH - XSF(8)));
    R.card = XjsRectF(cx, cy, cx + cardW, cy + cardH);
    R.textTop = cy + padV + titleH + XSF(8);
    R.actY = cy + cardH - padV - btnH;                 /* actions 行 (不再提示/跳过 + 步数) */
    R.navY = R.actY - XSF(10) - btnH;                  /* nav 行 (上一步/下一步·完成, 右对齐) */
    float x = cx + pad;
    float nw = XjsMeasureText(XjsT(L"引导.不再提示"), g_tfChip) + XSF(12);
    R.noMore = XjsRectF(x, R.actY, x + nw, R.actY + btnH);
    x = R.noMore.right + XSF(8);
    float sw = XjsMeasureText(XjsT(L"引导.跳过引导"), g_tfChip) + XSF(24);
    R.skip = XjsRectF(x, R.actY, x + sw, R.actY + btnH);
    float pw = XjsMeasureText(XjsT(L"引导.上一步"), g_tfChip) + XSF(24);
    float tx = XjsMeasureText(gd.idx < gd.count - 1 ? XjsT(L"引导.下一步") : XjsT(L"引导.完成"), g_tfChip) + XSF(24);
    R.next = XjsRectF(cx + cardW - pad - tx, R.navY, cx + cardW - pad, R.navY + btnH);
    R.prev = XjsRectF(R.next.left - XSF(8) - pw, R.navY, R.next.left - XSF(8), R.navY + btnH);
    return R;
}

int GuideHit(const GuideRects& R, const POINT& pt) {
    XjsGuide& gd = g_guide;
    auto in = [&](const XjsRect& r) { return pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom; };
    if (in(R.noMore)) return GBTN_NOMORE;
    if (in(R.skip)) return GBTN_SKIP;
    if (gd.idx > 0 && in(R.prev)) return GBTN_PREV;    /* 首步上一步隐藏 (CSS visibility:hidden 保位不接点) */
    if (in(R.next)) return GBTN_NEXT;
    return GBTN_NONE;
}

/* 演示光标位图 (懒解码, 绑本窗 RT — appIcon 同口径; 随窗在设备释放处回收) */
XjsBitmap* GuideCursorBitmap() {
    XjsSearchWindow* w = XjsSearchWindow::Cur();
    if (!w->guideCursorBmp)
        w->guideCursorBmp = XjsDecodeImage(kGuideCursorPng, (int)sizeof(kGuideCursorPng));
    return w->guideCursorBmp;
}

void GuideResetStep() { g_guide.stepStart = (double)GetTickCount64(); }

void GuideStop() {
    XjsSearchWindow* w = XjsSearchWindow::Cur();
    XjsGuide& gd = g_guide;
    if (!gd.open) return;
    gd.open = false;
    gd.pressCmd = GBTN_NONE;
    gd.hoverCmd = GBTN_NONE;
    if (w && w->hWnd) KillTimer(w->hWnd, ID_TIMER_GUIDE);
    g_needFullPaint = true;   /* 遮罩一次性揭走 */
    if (w) w->Invalidate();
}

void GuideFinish() {   /* 走完最后一步 / 点"不再提示": 与"不再提示"同等记入本地 (源样式 guideSetDismissed) */
    s_guideDismissed = true;
    XjsSaveConfig();
    GuideStop();
}

} // namespace

/* ==================== 对外接口 (xjs_app.h 声明) ==================== */

bool XjsGuideDismissed() { return s_guideDismissed; }
void XjsGuideSetDismissed(bool v) { s_guideDismissed = v; }
bool XjsGuideActive() { return g_guide.open; }

void XjsGuideStart() {
    if (XjsPopupMenuOpen()) return;   /* 弹窗菜单是独立顶层窗会浮在遮罩上 — 开着先不开 (源样式 hideAllMenus) */
    XjsSearchWindow* w = XjsSearchWindow::Cur();
    if (!w || !w->hWnd) return;
    XjsGuide& gd = g_guide;
    gd.count = 0;
    for (int i = 0; i < kGuideStepN && gd.count < kGuideStepN; i++)
        if (GuideRectVisible(GuideTargetRect(kGuideSteps[i].target))) gd.map[gd.count++] = i;
    if (!gd.count) return;
    XjsSearchFocus(false);            /* 模态接管输入: 搜索框失焦 (ModeDlg 同口径) */
    gd.open = true;
    gd.idx = 0;
    gd.pressCmd = GBTN_NONE;
    gd.hoverCmd = GBTN_NONE;
    GuideResetStep();
    g_needFullPaint = true;
    SetTimer(w->hWnd, ID_TIMER_GUIDE, 30, NULL);   /* 呼吸环/点击演示驱动 (关引导自摘表) */
    w->Invalidate();
}

void XjsGuideAutoStartTick(HWND hwnd) {   /* ID_TIMER_GUIDE_START 到点 (主窗创建 +800ms, 源样式同拍) */
    if (s_guideDismissed) return;
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return;   /* --autostart 托盘隐藏启动: 不播 */
    XjsGuideStart();
}

bool XjsGuideTick(HWND hwnd) {   /* WM_TIMER(ID_TIMER_GUIDE): 引导开着才整窗失效, 关了自摘表 */
    if (!g_guide.open) return false;
    XjsSearchWindow::Cur()->Invalidate();   /* 遮罩盖满全窗, 局部带重绘会漏呼吸环 */
    return true;
}

void XjsGuideMsg(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)hwnd;
    XjsGuide& gd = g_guide;
    switch (msg) {
        case WM_KEYDOWN: case WM_SYSKEYDOWN:
            if (wParam == VK_ESCAPE) { GuideStop(); return; }   /* Esc = 跳过 (不记"不再提示", 源样式同) */
            if (wParam == VK_LEFT && gd.idx > 0) {              /* ← = 上一步 (与点按钮同径) */
                gd.idx--;
                GuideResetStep();
                return;
            }
            if (wParam == VK_RIGHT || wParam == VK_RETURN || wParam == VK_SPACE) {   /* →/Enter/Space = 下一步·完成 */
                if (gd.idx < gd.count - 1) { gd.idx++; GuideResetStep(); }
                else GuideFinish();   /* 最后一步 = 完成, 记忆 (源样式同) */
                return;
            }
            return;                                 /* 其余按键一律吞掉 */
        case WM_LBUTTONDOWN: {
            POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };
            std::vector<std::wstring> paras;
            gd.pressCmd = GuideHit(GuideLayout(g_layout.w, g_layout.h, &paras), pt);
            XjsSearchWindow::Cur()->Invalidate();
            return;
        }
        case WM_MOUSEMOVE: {
            POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };
            std::vector<std::wstring> paras;
            int hov = GuideHit(GuideLayout(g_layout.w, g_layout.h, &paras), pt);
            if (hov != gd.hoverCmd) { gd.hoverCmd = hov; XjsSearchWindow::Cur()->Invalidate(); }
            return;
        }
        case WM_LBUTTONUP: {
            POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };
            int cmd = gd.pressCmd;
            gd.pressCmd = GBTN_NONE;
            std::vector<std::wstring> paras;
            bool still = cmd != GBTN_NONE && GuideHit(GuideLayout(g_layout.w, g_layout.h, &paras), pt) == cmd;
            switch (cmd) {
                case GBTN_NOMORE: if (still) GuideFinish(); break;
                case GBTN_SKIP:   if (still) GuideStop(); break;
                case GBTN_PREV:
                    if (still && gd.idx > 0) { gd.idx--; GuideResetStep(); }
                    break;
                case GBTN_NEXT:
                    if (!still) break;
                    if (gd.idx < gd.count - 1) { gd.idx++; GuideResetStep(); }
                    else GuideFinish();   /* 最后一步 = 完成, 记忆 (源样式同) */
                    break;
            }
            XjsSearchWindow::Cur()->Invalidate();
            return;
        }
        default:
            return;   /* 滚轮/右键/双击/IME… 一律吞掉 (源样式遮罩 mousedown preventDefault 同) */
    }
}

void XjsGuideRender(XjsRt* rt, float w, float h) {
    XjsGuide& gd = g_guide;
    if (!gd.open || !rt) return;
    std::vector<std::wstring> paras;
    GuideRects R = GuideLayout(w, h, &paras);
    const GuideStep& st = kGuideSteps[gd.map[gd.idx]];
    const XjsRect& s = R.spot;
    double el = (double)GetTickCount64() - gd.stepStart;

    /* 遮罩: 全窗 0.45, 洞外再叠 0.55 (源样式 .guide-mask 0.45 + .guide-spot 巨影 0.55 合成同观感) */
    rt->FillRectangle(XjsRectF(0, 0, w, h), XjsTempBrush(XjsCol(0x000000, 0.45f)));
    XjsBrush* outer = XjsTempBrush(XjsCol(0x000000, 0.55f));
    if (s.top > 0)      rt->FillRectangle(XjsRectF(0, 0, w, s.top), outer);
    if (s.bottom < h)   rt->FillRectangle(XjsRectF(0, s.bottom, w, h), outer);
    if (s.left > 0)     rt->FillRectangle(XjsRectF(0, s.top, s.left, s.bottom), outer);
    if (s.right < w)    rt->FillRectangle(XjsRectF(s.right, s.top, w, s.bottom), outer);

    /* 呼吸高亮环 (accent, 2↔6px 1.8s ease-in-out = 余弦半周期; 源样式 guidePulse 同拍) */
    float ph = 0.5f - 0.5f * cosf((float)(el * 3.14159265358979323 / 900.0));
    float ring = XSF(2 + 4 * ph);
    float ringR = XSF(8) + ring * 0.5f;
    rt->DrawRoundedRectangle(XjsRoundedRectF(
        XjsRectF(s.left - ring * 0.5f, s.top - ring * 0.5f, s.right + ring * 0.5f, s.bottom + ring * 0.5f),
        ringR, ringR), g_br[XTH_ACCENT], ring);

    /* 说明卡 (源样式 .guide-card: panel 底 + border + 10px 圆角) */
    rt->FillRoundedRectangle(XjsRoundedRectF(R.card, XSF(10), XSF(10)), g_br[XTH_PANEL]);
    rt->DrawRoundedRectangle(XjsRoundedRectF(R.card, XSF(10), XSF(10)), g_br[XTH_BORDER], 1.0f);
    const float pad = XSF(16);
    {
        /* 标题: accent 竖条 3x12 + 文本 (源样式 .guide-card-title::before 同构) */
        const wchar_t* title = XjsT(st.title);
        rt->FillRectangle(XjsRectF(R.card.left + pad, R.card.top + XSF(16), R.card.left + pad + XSF(3), R.card.top + XSF(28)),
                          g_br[XTH_ACCENT]);
        rt->DrawText(title, (UINT32)wcslen(title), g_tfGuideTitle,
            XjsRectF(R.card.left + pad + XSF(11), R.card.top + XSF(14), R.card.right - pad, R.card.top + XSF(34)),
            g_br[XTH_TEXT]);
    }
    {
        float ty = R.textTop;
        float maxW = R.card.right - pad - (R.card.left + pad);
        for (const auto& seg : paras) {
            bool drawn = false;
            if (!seg.empty() && g_dw && g_tfGuideText) {
                XjsTextLayout* lay = NULL;
                if (SUCCEEDED(g_dw->CreateTextLayout(seg.c_str(), (UINT32)seg.length(), g_tfGuideText, maxW, 10000.0f, &lay)) && lay) {
                    XjsTextMetrics m = {};
                    if (SUCCEEDED(lay->GetMetrics(&m)) && m.height > 0) {
                        rt->DrawTextLayout(XjsPoint2F(R.card.left + pad, ty), lay, g_br[XTH_TEXT_DIM], D2D1_DRAW_TEXT_OPTIONS_NONE);
                        ty += m.height;
                        drawn = true;
                    }
                    lay->Release();
                }
            }
            if (!drawn) {   /* 空段 (段间空行) 或资源未就绪: 按行距推进 */
                rt->DrawText(seg.c_str(), (UINT32)seg.length(), g_tfGuideText,
                    XjsRectF(R.card.left + pad, ty, R.card.left + pad + maxW, ty + XSF(20.4f)), g_br[XTH_TEXT_DIM]);
                ty += XSF(20.4f);
            }
        }
    }
    {
        /* actions 行: 不再提示 (透明文字钮, 悬停红) + 跳过引导 (描边钮) + 步数 (右缘 11px faint) */
        auto btnText = [&](const XjsRect& b, const wchar_t* t, XjsBrush* br) {
            float tw = XjsMeasureText(t, g_tfChip);
            rt->DrawText(t, (UINT32)wcslen(t), g_tfChip,
                XjsRectF(b.left + ((b.right - b.left) - tw) / 2, b.top, b.left + ((b.right - b.left) + tw) / 2, b.bottom), br);
        };
        bool h1 = gd.hoverCmd == GBTN_NOMORE || gd.pressCmd == GBTN_NOMORE;
        btnText(R.noMore, XjsT(L"引导.不再提示"), h1 ? (XjsBrush*)XjsTempBrush(XjsCol(0xe0442e)) : g_br[XTH_TEXT_FAINT]);
        bool h2 = gd.hoverCmd == GBTN_SKIP || gd.pressCmd == GBTN_SKIP;
        rt->FillRoundedRectangle(XjsRoundedRectF(R.skip, XSF(7), XSF(7)), g_br[XTH_PANEL2]);
        rt->DrawRoundedRectangle(XjsRoundedRectF(R.skip, XSF(7), XSF(7)), h2 ? g_br[XTH_BORDER_STRONG] : g_br[XTH_BORDER], 1.0f);
        btnText(R.skip, XjsT(L"引导.跳过引导"), h2 ? g_br[XTH_TEXT] : g_br[XTH_TEXT_DIM]);
        std::wstring step = XjsFmt(XjsT(L"引导.第N步"), XjsNumText((long long)(gd.idx + 1)), XjsNumText((long long)gd.count));
        float stw = XjsMeasureText(step.c_str(), g_tfTiny);
        rt->DrawText(step.c_str(), (UINT32)step.length(), g_tfTiny,
            XjsRectF(R.card.right - pad - stw - XSF(4), R.actY, R.card.right - pad + XSF(4), R.actY + R.btnH),
            g_br[XTH_TEXT_FAINT]);
        /* nav 行: 上一步 (首步隐藏, CSS visibility:hidden 保位) + 下一步·完成 (accent 填充) */
        bool h4 = gd.hoverCmd == GBTN_NEXT || gd.pressCmd == GBTN_NEXT;
        rt->FillRoundedRectangle(XjsRoundedRectF(R.next, XSF(7), XSF(7)),
                                 h4 ? (XjsBrush*)XjsTempBrush(XjsColorF(g_skin.accent.r, g_skin.accent.g, g_skin.accent.b, 0.9f))
                                    : g_br[XTH_ACCENT]);
        btnText(R.next, gd.idx < gd.count - 1 ? XjsT(L"引导.下一步") : XjsT(L"引导.完成"), XjsTempBrush(XjsCol(0xFFFFFF)));
        if (gd.idx > 0) {
            bool h3 = gd.hoverCmd == GBTN_PREV || gd.pressCmd == GBTN_PREV;
            rt->FillRoundedRectangle(XjsRoundedRectF(R.prev, XSF(7), XSF(7)), g_br[XTH_PANEL2]);
            rt->DrawRoundedRectangle(XjsRoundedRectF(R.prev, XSF(7), XSF(7)), h3 ? g_br[XTH_BORDER_STRONG] : g_br[XTH_BORDER], 1.0f);
            btnText(R.prev, XjsT(L"引导.上一步"), h3 ? g_br[XTH_TEXT] : g_br[XTH_TEXT_DIM]);
        }
    }

    /* 演示鼠标点击 (光标 PNG + accent 涟漪; 光标盒 translate(-2,-2), 涟漪圆心 = 盒内 (3+11,3+11)) */
    if (st.cursor != GC_NONE) {
        float tipX, tipY;
        if (st.cursor == GC_INPUT) {
            tipX = s.left + xf_min((s.right - s.left) * 0.6f, XSF(90));
            tipY = (s.top + s.bottom) * 0.5f;
        } else {
            tipX = (s.left + s.right) * 0.5f;
            tipY = (s.top + s.bottom) * 0.5f;
        }
        float scale = 1.0f;
        if (el >= GUIDE_CLICK_AT) {
            double e = xf_min(el - GUIDE_CLICK_AT, GUIDE_CLICK_MS) / GUIDE_CLICK_MS;
            scale = (e < 0.6) ? 0.85f : 0.85f + 0.15f * (float)((e - 0.6) / 0.4);
        }
        XjsBitmap* bmp = GuideCursorBitmap();
        if (bmp) {
            float cw = XSF(26) * scale, ch = XSF(32) * scale;
            rt->DrawBitmap(bmp, XjsRectF(tipX - XSF(2), tipY - XSF(2), tipX - XSF(2) + cw, tipY - XSF(2) + ch), 1.0f, 1, NULL);
        }
        if (el >= GUIDE_CLICK_AT && el < GUIDE_CLICK_AT + GUIDE_RIPPLE_MS) {
            double u = (el - GUIDE_CLICK_AT) / GUIDE_RIPPLE_MS;
            XjsColor rc = g_skin.accent;
            rc.a = 0.9f * (float)(1.0 - u);
            float rr = XSF(11) * (float)(0.4 + 1.8 * u);
            rt->DrawEllipse(XjsEllipse(XjsPoint2F(tipX + XSF(12), tipY + XSF(12)), rr, rr), XjsTempBrush(rc), XSF(2), NULL);
        }
    }
}
