/*
 * xjs_market.cpp — 插件商城窗口: 独立顶层窗口 (单例, 构型同 xjs_settings:
 * owner 绑定 + 文件级状态 + 自绘 D2D + 每显示器 DPI 尺度 + 换肤 g_skinEpoch 重建画刷)。
 * 布局 = 头部 (标题/副标题/搜索框) + 左侧分类栏 (全部/已安装/可安装) + 右侧插件卡片栅格。
 * 数据 = 本机插件注册表 (与设置-插件页同源) + 样式演示占位卡 (市场下载源尚未接入,
 * 示例卡按钮点击仅提示; 接入真实市场源后删 MK_DEMO 并在此接拉取管线)。
 * 搜索框走共享输入字段组件 XjsEditField (路由/IME/闪烁全在组件, 宿主只接线)。
 */
#include "xjs_app.h"
#include <cwctype>
#include <algorithm>

/* ==================== 状态 (单例; 窗口级状态住结构体, 不落游离全局) ==================== */

struct XjsMkEntry {
    std::wstring id, name, version, author, desc;
    bool installed = false;   /* 本机注册表条目 (已安装) */
    bool enabled = false;     /* 用户启用开关 (徽章展示, 设置页才是操作入口) */
    bool demo = false;        /* 样式演示占位卡 (市场源未接入) */
};

enum { MK_CAT_ALL = 0, MK_CAT_INSTALLED, MK_CAT_AVAILABLE, MK_CAT_N };

/* 样式演示占位卡 (文案存点分 i18n 主键; 文件级 static 表初始化器禁调 XjsT) */
static const struct { const wchar_t* nameKey; const wchar_t* descKey; } MK_DEMO[3] = {
    { L"商城.示例.Markdown阅读器",  L"商城.示例.Markdown阅读器.描述"  },
    { L"商城.示例.彩色文件夹图标",  L"商城.示例.彩色文件夹图标.描述"  },
    { L"商城.示例.重复文件查找",    L"商城.示例.重复文件查找.描述"    },
};

struct XjsMarketState {
    HWND hwnd = NULL;
    XjsHwndRt* rt = NULL;
    XjsSolidBrush *brBg = NULL, *brBorder = NULL, *brBorderStrong = NULL, *brText = NULL,
        *brDim = NULL, *brFaint = NULL, *brAccent = NULL, *brAccent2 = NULL, *brAccentSoft = NULL,
        *brPanel = NULL, *brPanel2 = NULL, *brHover = NULL, *brOk = NULL;
    XjsGradBrush* brBgGrad = NULL;
    XjsFormat *tfTitle = NULL, *tfSub = NULL, *tfName = NULL, *tfMeta = NULL, *tfDesc = NULL,
        *tfBtn = NULL, *tfCat = NULL, *tfCount = NULL, *tfGlyph = NULL, *tfEmpty = NULL, *tfSearch = NULL;
    int brushEpoch = -1;
    float unit = 0;              /* 资源创建时的 SS 单位 (缩放/DPI 变了须重建) */
    float scale = 1.0f;          /* 本窗口显示器 DPI 尺度 (与主窗 g_s 独立) */

    int cat = MK_CAT_ALL;
    float scroll = 0, contentH = 0;
    std::vector<XjsMkEntry> entries;
    std::vector<int> view;       /* 过滤后可见的 entries 下标 (分类 + 搜索词) */
    bool modelDirty = true;      /* 打开/激活/切分类/搜索词变化后重建 (注册表可在设置页被改动) */

    int hoverSide = -1;          /* 悬停分类条目 (MK_CAT_*) */
    int hoverCard = -1;          /* 悬停卡片 (view 下标) */
    bool trackingLeave = false;

    /* 头部搜索框 (共享单行输入字段组件: 键盘/IME/I-beam/闪烁 路由在组件层) */
    XjsEditField searchEd;
    /* 命令类松开触发 (口径同设置窗): 按下只记待定, 松开仍命中同一目标才执行 */
    int pressSide = -1;
    int pressCard = -1;          /* 按下待定的卡片按钮 (view 下标) */
};
static XjsMarketState s_mk;

static XjsSearchWindow* s_mkOwner = NULL;
static XjsSearchWindow* XjsMarketOwner() {
    if (!XjsSearchWindow::Alive(s_mkOwner)) s_mkOwner = XjsSearchWindow::Main();
    return s_mkOwner;
}

static float SS(float v) { return v * s_mk.scale * XjsUiZoom(); }

/* ==================== 数据模型 ==================== */

static void XjsMkRebuildEntries() {
    s_mk.entries.clear();
    for (int i = 0; i < XjsPluginCount(); i++) {
        XjsPluginBrief b;
        if (!XjsPluginBriefAt(i, &b)) continue;
        XjsMkEntry e;
        e.id = b.id;
        e.name = b.name.empty() ? b.id : b.name;
        e.version = b.version;
        e.author = b.author;
        e.desc = b.description;
        e.installed = true;
        e.enabled = b.enabled;
        s_mk.entries.push_back(std::move(e));
    }
    for (const auto& d : MK_DEMO) {
        XjsMkEntry e;
        e.name = XjsT(d.nameKey);
        e.desc = XjsT(d.descKey);
        e.author = XjsT(L"商城.示例作者");
        e.demo = true;
        s_mk.entries.push_back(std::move(e));
    }
}

static std::wstring XjsMkLower(const std::wstring& s) {
    std::wstring r;
    r.reserve(s.size());
    for (wchar_t c : s) r.push_back((wchar_t)towlower(c));
    return r;
}

static bool XjsMkMatch(const std::wstring& hay, const std::wstring& qLower) {
    if (qLower.empty()) return true;
    return XjsMkLower(hay).find(qLower) != std::wstring::npos;
}

static void XjsMkRebuildView() {
    std::wstring q = XjsMkLower(s_mk.searchEd.ed.text);
    s_mk.view.clear();
    for (int i = 0; i < (int)s_mk.entries.size(); i++) {
        const XjsMkEntry& e = s_mk.entries[i];
        if (s_mk.cat == MK_CAT_INSTALLED && !e.installed) continue;
        if (s_mk.cat == MK_CAT_AVAILABLE && !e.demo) continue;
        if (!XjsMkMatch(e.name, q) && !XjsMkMatch(e.desc, q) &&
            !XjsMkMatch(e.author, q) && !XjsMkMatch(e.id, q)) continue;
        s_mk.view.push_back(i);
    }
}

/* 分类条目计数 (侧栏右侧小字): 全部/已安装/可安装, 不受搜索词影响 */
static int XjsMkCatCount(int cat) {
    int n = 0;
    for (const auto& e : s_mk.entries) {
        if (cat == MK_CAT_ALL) n++;
        else if (cat == MK_CAT_INSTALLED && e.installed) n++;
        else if (cat == MK_CAT_AVAILABLE && e.demo) n++;
    }
    return n;
}

/* ==================== 布局 (渲染/命中同源, 每帧按现值算) ==================== */

static float XjsMkHeaderH() { return SS(88); }
static float XjsMkSideW()   { return SS(176); }
static float XjsMkCardH()   { return SS(124); }

struct XjsMkGrid { float x0 = 0, x1 = 0, y0 = 0, colW = 0; int cols = 1; };

static XjsMkGrid XjsMkGridGeom(float w, float vh) {
    XjsMkGrid g;
    g.x0 = XjsMkSideW() + SS(22);
    g.x1 = w - SS(26);   /* 右缘留滚动条带 */
    g.y0 = XjsMkHeaderH() + SS(14);
    float cw = g.x1 - g.x0;
    if (cw < SS(60)) cw = SS(60);
    g.cols = (cw >= SS(620)) ? 2 : 1;
    float gap = SS(14);
    g.colW = (cw - (g.cols - 1) * gap) / g.cols;
    int rows = ((int)s_mk.view.size() + g.cols - 1) / g.cols;
    if (rows < 1) rows = 1;
    s_mk.contentH = g.y0 + rows * (XjsMkCardH() + gap);
    (void)vh;
    return g;
}

static XjsRect XjsMkCardRect(const XjsMkGrid& g, int vi, float scroll) {
    int col = vi % g.cols, row = vi / g.cols;
    float x = g.x0 + col * (g.colW + SS(14));
    float y = g.y0 + row * (XjsMkCardH() + SS(14)) - scroll;
    return XjsRectF(x, y, x + g.colW, y + XjsMkCardH());
}

/* 卡片内按钮/徽章矩形 (绘制/命中同源): 卡片右下角。
   上缘 = bottom-36, 与简介区 (top+56..top+86) 不相交 — 简介两行曾压到徽章上 (2026-10-06) */
static XjsRect XjsMkBtnRect(const XjsRect& card) {
    return XjsRectF(card.right - SS(16) - SS(92), card.bottom - SS(36),
                    card.right - SS(16), card.bottom - SS(10));
}

/* ==================== 资源 (换肤纪元/DPI 变化重建) ==================== */

static void XjsMkFreeResources() {
    XjsLayersDropRt(s_mk.rt);   /* 先摘 s_layers 条目再放 RT (本窗也弹 Toast), 漏摘 = 悬垂堆地址 */
    if (s_mk.rt) { s_mk.rt->Release(); s_mk.rt = NULL; }
    /* 表驱动必须取成员地址遍历并就地置空: 拷贝到栈数组只清副本, s_mk.brXxx/tfXxx 残留
       悬垂包装, 下次 Free (关闭后重开, EnsureResources 先行清理) 二次 Release = UAF 崩
       (2026-10-06 实锤: 关商城→重开商城, WM_PAINT 重建路径崩) */
    XjsSolidBrush** brs[] = { &s_mk.brBg, &s_mk.brBorder, &s_mk.brBorderStrong, &s_mk.brText,
                              &s_mk.brDim, &s_mk.brFaint, &s_mk.brAccent, &s_mk.brAccent2,
                              &s_mk.brAccentSoft, &s_mk.brPanel, &s_mk.brPanel2, &s_mk.brHover, &s_mk.brOk };
    for (auto* pb : brs) if (*pb) { (*pb)->Release(); *pb = NULL; }
    if (s_mk.brBgGrad) { s_mk.brBgGrad->Release(); s_mk.brBgGrad = NULL; }
    XjsFormat** fmts[] = { &s_mk.tfTitle, &s_mk.tfSub, &s_mk.tfName, &s_mk.tfMeta, &s_mk.tfDesc,
                           &s_mk.tfBtn, &s_mk.tfCat, &s_mk.tfCount, &s_mk.tfGlyph, &s_mk.tfEmpty, &s_mk.tfSearch };
    for (auto* pf : fmts) if (*pf) { (*pf)->Release(); *pf = NULL; }
    s_mk.brushEpoch = -1;
}

static void XjsMkEnsureResources(HWND hwnd) {
    float unit = SS(1);
    if (s_mk.rt && s_mk.brushEpoch == g_skinEpoch && s_mk.unit == unit) return;
    XjsMkFreeResources();
    RECT rc;
    GetClientRect(hwnd, &rc);
    g_gfx->CreateWindowRt(hwnd, ximax(rc.right, 1), ximax(rc.bottom, 1), &s_mk.rt);
    if (!s_mk.rt) return;
    XjsColor bg1 = g_skin.bg1; bg1.a = 1.0f;   /* HwndRT 无逐像素 alpha */
    XjsColor bg2 = g_skin.bg2; bg2.a = 1.0f;
    s_mk.rt->CreateSolidColorBrush(bg1, &s_mk.brBg);
    s_mk.rt->CreateSolidColorBrush(g_skin.border, &s_mk.brBorder);
    s_mk.rt->CreateSolidColorBrush(g_skin.borderStrong, &s_mk.brBorderStrong);
    s_mk.rt->CreateSolidColorBrush(g_skin.text, &s_mk.brText);
    s_mk.rt->CreateSolidColorBrush(g_skin.textDim, &s_mk.brDim);
    s_mk.rt->CreateSolidColorBrush(g_skin.textFaint, &s_mk.brFaint);
    s_mk.rt->CreateSolidColorBrush(g_skin.accent, &s_mk.brAccent);
    s_mk.rt->CreateSolidColorBrush(g_skin.accent2, &s_mk.brAccent2);
    s_mk.rt->CreateSolidColorBrush(g_skin.accentSoft, &s_mk.brAccentSoft);
    s_mk.rt->CreateSolidColorBrush(g_skin.panel, &s_mk.brPanel);
    s_mk.rt->CreateSolidColorBrush(g_skin.panel2, &s_mk.brPanel2);
    s_mk.rt->CreateSolidColorBrush(g_skin.rowHover, &s_mk.brHover);
    s_mk.rt->CreateSolidColorBrush(g_skin.ok, &s_mk.brOk);
    XjsGradientStop gs[2] = { { 0.0f, bg1 }, { 1.0f, bg2 } };
    s_mk.rt->CreateLinearGradientBrush(XjsPoint2F(0, 0), XjsPoint2F(0, (FLOAT)rc.bottom + 1), gs, 2, &s_mk.brBgGrad);
    float px = SS(1);
    struct FDef { XjsFormat** out; float size; DWRITE_FONT_WEIGHT weight; };
    const FDef defs[] = {
        { &s_mk.tfTitle, 15, DWRITE_FONT_WEIGHT_SEMI_BOLD },
        { &s_mk.tfSub,   11, DWRITE_FONT_WEIGHT_NORMAL },
        { &s_mk.tfName,  13, DWRITE_FONT_WEIGHT_SEMI_BOLD },
        { &s_mk.tfMeta,  10.5f, DWRITE_FONT_WEIGHT_NORMAL },
        { &s_mk.tfDesc,  11, DWRITE_FONT_WEIGHT_NORMAL },
        { &s_mk.tfBtn,   11.5f, DWRITE_FONT_WEIGHT_NORMAL },
        { &s_mk.tfCat,   12, DWRITE_FONT_WEIGHT_NORMAL },
        { &s_mk.tfCount, 10, DWRITE_FONT_WEIGHT_NORMAL },
        { &s_mk.tfGlyph, 18, DWRITE_FONT_WEIGHT_SEMI_BOLD },
        { &s_mk.tfEmpty, 13, DWRITE_FONT_WEIGHT_NORMAL },
        { &s_mk.tfSearch, 12, DWRITE_FONT_WEIGHT_NORMAL },
    };
    for (const auto& d : defs) {
        g_dw->CreateTextFormat(L"Segoe UI", NULL, d.weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, d.size * px, L"zh-cn", d.out);
        if (*d.out) (*d.out)->SetParagraphAlignment(XJS_PARA_CENTER);   /* 全部垂直居中 (DWrite 默认顶对齐) */
    }
    /* 单行文本关自动换行; 溢出处字符省略号 */
    XjsFormat* nowrap[] = { s_mk.tfTitle, s_mk.tfSub, s_mk.tfName, s_mk.tfMeta,
                            s_mk.tfBtn, s_mk.tfCat, s_mk.tfCount, s_mk.tfGlyph, s_mk.tfEmpty, s_mk.tfSearch };
    for (auto* f : nowrap)
        if (f) { f->SetWordWrapping(XJS_WRAP_NONE); f->SetCharEllipsis(); }
    /* 简介两行词换行 + 末行省略号 (英文按空格断行不劈词, 中文仍逐字) */
    if (s_mk.tfDesc) { s_mk.tfDesc->SetWordWrapping(XJS_WRAP_WORD); s_mk.tfDesc->SetCharEllipsis(); }
    s_mk.brushEpoch = g_skinEpoch;
    s_mk.unit = unit;
}

/* ==================== 绘制 ==================== */

/* 搜索框放大镜 (16u 线性小图标: 圆环 + 柄, 颜色随画刷) */
static void XjsMkSearchGlyph(XjsRt* rt, float cx, float cy, XjsBrush* br) {
    float r = SS(4.2f);
    rt->DrawEllipse(XjsEllipseF(XjsPoint2F(cx - SS(1), cy - SS(1)), r, r), br, SS(1.3f));
    rt->DrawLine(XjsPoint2F(cx + SS(2), cy + SS(2)), XjsPoint2F(cx + SS(5.4f), cy + SS(5.4f)), br, SS(1.3f));
}

static void XjsMkPaint(HWND hwnd) {
    XjsWindowScope scope(XjsMarketOwner());   /* 绘制期作用域: XjsT/宏按 owner 解析 (语言每窗化) */
    XjsMkEnsureResources(hwnd);
    if (!s_mk.rt) return;
    if (s_mk.modelDirty) {
        XjsMkRebuildEntries();
        XjsMkRebuildView();
        s_mk.modelDirty = false;
    }
    if (!s_mk.searchEd.blink.Attached())   /* 输入字段组件: 绑定承载窗口 (局部重绘回调) */
        s_mk.searchEd.Attach(hwnd, [hwnd] { InvalidateRect(hwnd, NULL, FALSE); });
    XjsSizeU sz = s_mk.rt->GetPixelSize();
    float w = (float)sz.width, vh = (float)sz.height;
    float headH = XjsMkHeaderH();
    XjsMkGrid g = XjsMkGridGeom(w, vh);
    float maxScroll = s_mk.contentH - vh;
    if (maxScroll < 0) maxScroll = 0;
    if (s_mk.scroll > maxScroll) s_mk.scroll = maxScroll;
    if (s_mk.scroll < 0) s_mk.scroll = 0;

    s_mk.rt->BeginDraw();
    if (s_mk.brBgGrad) s_mk.rt->FillRectangle(XjsRectF(0, 0, w, vh), s_mk.brBgGrad);
    else s_mk.rt->Clear(s_mk.brBg->GetColor());

    /* ---- 头部: 标题 + 副标题 + 搜索框 ---- */
    {
        const wchar_t* title = XjsT(L"商城.标题");
        s_mk.rt->DrawText(title, (UINT32)wcslen(title), s_mk.tfTitle,
            XjsRectF(SS(24), SS(14), w - SS(24), SS(14) + SS(30)), s_mk.brText);
        const wchar_t* sub = XjsT(L"商城.副标题");
        s_mk.rt->DrawText(sub, (UINT32)wcslen(sub), s_mk.tfSub,
            XjsRectF(SS(24), SS(46), w - SS(320), SS(46) + SS(22)), s_mk.brDim);
        XjsRect sr = XjsRectF(w - SS(24) - SS(280), SS(16), w - SS(24), SS(16) + SS(36));
        s_mk.rt->FillRoundedRectangle(XjsRoundedRectF(sr, SS(8), SS(8)), s_mk.brPanel2);
        s_mk.rt->DrawRoundedRectangle(XjsRoundedRectF(sr, SS(8), SS(8)),
            s_mk.searchEd.focused ? s_mk.brAccent : s_mk.brBorder, SS(1.2f));
        XjsMkSearchGlyph(s_mk.rt, sr.left + SS(16), sr.top + SS(18), s_mk.brFaint);
        /* 帧边界即清字段几何: 本帧真画到才由 Render 回写 (滚出/隐藏后命中不落隐形框) */
        s_mk.searchEd.area = {};
        s_mk.searchEd.Render(s_mk.rt,
            XjsRectF(sr.left + SS(28), sr.top + SS(3), sr.right - SS(10), sr.bottom - SS(3)),
            s_mk.tfSearch, s_mk.brText, s_mk.brAccentSoft, s_mk.brText,
            XjsT(L"商城.搜索占位"), s_mk.brFaint);
        s_mk.rt->FillRectangle(XjsRectF(0, headH, w, headH + 1), s_mk.brBorder);
    }

    /* ---- 左侧分类栏 ---- */
    {
        static const struct { int cat; const wchar_t* nameKey; } SIDES[MK_CAT_N] = {
            { MK_CAT_ALL,        L"商城.分类.全部"   },
            { MK_CAT_INSTALLED,  L"商城.分类.已安装" },
            { MK_CAT_AVAILABLE,  L"商城.分类.可安装" },
        };
        float chh = SS(34);
        float y = headH + SS(14);
        for (int si = 0; si < MK_CAT_N; si++) {
            XjsRect br = XjsRectF(SS(10), y, XjsMkSideW() - SS(10), y + chh);
            bool sel = SIDES[si].cat == s_mk.cat;
            if (sel) {
                s_mk.rt->FillRoundedRectangle(XjsRoundedRectF(br, SS(8), SS(8)), s_mk.brAccentSoft);
            } else if (si == s_mk.hoverSide) {
                s_mk.rt->FillRoundedRectangle(XjsRoundedRectF(br, SS(8), SS(8)), s_mk.brHover);
            }
            const wchar_t* nm = XjsT(SIDES[si].nameKey);
            s_mk.rt->DrawText(nm, (UINT32)wcslen(nm), s_mk.tfCat,
                XjsRectF(br.left + SS(14), br.top, br.right - SS(40), br.bottom),
                sel ? s_mk.brAccent : s_mk.brText);
            wchar_t cnt[16];
            swprintf(cnt, 16, L"%d", XjsMkCatCount(SIDES[si].cat));
            s_mk.rt->DrawText(cnt, (UINT32)wcslen(cnt), s_mk.tfCount,
                XjsRectF(br.right - SS(38), br.top, br.right - SS(12), br.bottom),
                sel ? s_mk.brAccent : s_mk.brFaint);
            y += chh;
        }
        s_mk.rt->FillRectangle(XjsRectF(XjsMkSideW(), headH + 1, XjsMkSideW() + 1, vh), s_mk.brBorder);
    }

    /* ---- 插件卡片栅格 ---- */
    for (int vi = 0; vi < (int)s_mk.view.size(); vi++) {
        XjsRect cr = XjsMkCardRect(g, vi, s_mk.scroll);
        if (cr.bottom < headH || cr.top > vh) continue;   /* 滚出视口的卡不画 */
        const XjsMkEntry& e = s_mk.entries[s_mk.view[vi]];
        bool hovered = vi == s_mk.hoverCard;
        s_mk.rt->FillRoundedRectangle(XjsRoundedRectF(cr, SS(10), SS(10)),
            hovered ? s_mk.brPanel2 : s_mk.brPanel);
        s_mk.rt->DrawRoundedRectangle(XjsRoundedRectF(cr, SS(10), SS(10)),
            hovered ? s_mk.brBorderStrong : s_mk.brBorder, SS(1.2f));
        /* 图标位: 圆角色板 + 名称首字 (接入真实图标后替换) */
        XjsRect ir = XjsRectF(cr.left + SS(16), cr.top + SS(14), cr.left + SS(60), cr.top + SS(58));
        s_mk.rt->FillRoundedRectangle(XjsRoundedRectF(ir, SS(10), SS(10)), s_mk.brAccentSoft);
        if (!e.name.empty()) {
            wchar_t gl[2] = { e.name[0], 0 };
            s_mk.rt->DrawText(gl, 1, s_mk.tfGlyph, ir, s_mk.brAccent);
        }
        /* 名称 + 版本·作者 (元信息行) */
        float tx = cr.left + SS(72);
        s_mk.rt->DrawText(e.name.c_str(), (UINT32)e.name.length(), s_mk.tfName,
            XjsRectF(tx, cr.top + SS(12), cr.right - SS(16), cr.top + SS(34)), s_mk.brText);
        std::wstring meta;
        if (!e.version.empty()) meta = L"v" + e.version;
        if (!e.author.empty()) {
            if (!meta.empty()) meta += L" · ";
            meta += e.author;
        }
        if (!meta.empty())
            s_mk.rt->DrawText(meta.c_str(), (UINT32)meta.length(), s_mk.tfMeta,
                XjsRectF(tx, cr.top + SS(34), cr.right - SS(16), cr.top + SS(52)), s_mk.brDim);
        /* 简介 (两行, 词换行 + 末行省略号); 下缘 86 = 按钮上缘 88 之前, 不与右下角状态重叠 */
        s_mk.rt->DrawText(e.desc.c_str(), (UINT32)e.desc.length(), s_mk.tfDesc,
            XjsRectF(cr.left + SS(16), cr.top + SS(56), cr.right - SS(16), cr.top + SS(56) + SS(30)),
            s_mk.brDim);
        /* 右下角状态: 已安装 = 徽章 (启用/禁用); 演示卡 = "敬请期待" 置灰钮 (松开触发)。
           按钮款 = 设置窗同款描边钮 (文字水平居中, 悬停垫 accentSoft) */
        XjsRect br = XjsMkBtnRect(cr);
        if (e.installed) {
            s_mk.rt->DrawRoundedRectangle(XjsRoundedRectF(br, SS(6), SS(6)),
                e.enabled ? s_mk.brOk : s_mk.brBorder, SS(1.2f));
            const wchar_t* st = XjsT(e.enabled ? L"商城.已启用" : L"商城.已禁用");
            float tw = XjsMeasureText(st, s_mk.tfBtn);
            float cx = (br.left + br.right - tw) / 2;
            s_mk.rt->DrawText(st, (UINT32)wcslen(st), s_mk.tfBtn,
                XjsRectF(cx, br.top, cx + tw, br.bottom), e.enabled ? s_mk.brOk : s_mk.brDim);
        } else {
            if (hovered) {
                /* 悬停垫 accentSoft; 按住 (松开触发待定) = accent2 实底反白 (bg1 字) */
                if (vi == s_mk.pressCard)
                    s_mk.rt->FillRoundedRectangle(XjsRoundedRectF(br, SS(6), SS(6)), s_mk.brAccent2);
                else
                    s_mk.rt->FillRoundedRectangle(XjsRoundedRectF(br, SS(6), SS(6)), s_mk.brAccentSoft);
            }
            s_mk.rt->DrawRoundedRectangle(XjsRoundedRectF(br, SS(6), SS(6)), s_mk.brAccent, SS(1.2f));
            const wchar_t* bt = XjsT(L"商城.敬请期待");
            float tw = XjsMeasureText(bt, s_mk.tfBtn);
            float cx = (br.left + br.right - tw) / 2;
            s_mk.rt->DrawText(bt, (UINT32)wcslen(bt), s_mk.tfBtn,
                XjsRectF(cx, br.top, cx + tw, br.bottom),
                (hovered && vi == s_mk.pressCard) ? s_mk.brBg : s_mk.brAccent);
        }
    }

    /* ---- 空态 (分类无内容 / 搜索无匹配) ---- */
    if (s_mk.view.empty()) {
        const wchar_t* msg;
        if (!s_mk.searchEd.ed.text.empty()) msg = XjsT(L"商城.搜索无结果");
        else if (s_mk.cat == MK_CAT_INSTALLED) msg = XjsT(L"商城.尚未安装");
        else msg = XjsT(L"商城.暂无插件");
        s_mk.rt->DrawText(msg, (UINT32)wcslen(msg), s_mk.tfEmpty,
            XjsRectF(g.x0, headH, g.x1, headH + SS(120)), s_mk.brFaint);
    }

    /* ---- 右缘细滚动条 (内容不超高 = 不显示) ---- */
    if (maxScroll > 0 && vh > SS(8)) {
        float track = vh - SS(8);
        float tH = ximax(track * vh / s_mk.contentH, SS(24));
        float tY = SS(4) + (track - tH) * (s_mk.scroll / maxScroll);
        s_mk.rt->FillRoundedRectangle(
            XjsRoundedRectF(XjsRectF(w - SS(7), tY, w - SS(3), tY + tH), SS(2), SS(2)),
            s_mk.brBorderStrong);
    }

    XjsToastRender(hwnd, s_mk.rt, w, vh, SS(1));   /* Toast 组件 (宿主=商城窗, 浮于一切) */
    s_mk.rt->EndDraw();
}

/* ==================== 交互 ==================== */

static void XjsMkClampScroll(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    float maxScroll = s_mk.contentH - (float)(rc.bottom - rc.top);
    if (maxScroll < 0) maxScroll = 0;
    if (s_mk.scroll > maxScroll) s_mk.scroll = maxScroll;
    if (s_mk.scroll < 0) s_mk.scroll = 0;
}

static void XjsMkInvalidate(HWND hwnd) { InvalidateRect(hwnd, NULL, FALSE); }

/* 搜索词变化 → 重过滤 + 重绘 (路由层在编辑后置 ed.dirty) */
static void XjsMkOnSearchEdited(HWND hwnd) {
    if (!s_mk.searchEd.ed.dirty) return;
    s_mk.searchEd.ed.dirty = false;
    XjsMkRebuildView();
    XjsMkClampScroll(hwnd);
    XjsMkInvalidate(hwnd);
}

/* 悬停侧分类条目命中 (屏内 y; 未滚动坐标比对与绘制同源) */
static int XjsMkSideHit(float y) {
    float chh = SS(34);
    float y0 = XjsMkHeaderH() + SS(14);
    for (int si = 0; si < MK_CAT_N; si++)
        if (y >= y0 + si * chh && y < y0 + (si + 1) * chh) return si;
    return -1;
}

/* 悬停卡片命中 → view 下标 (点在卡片矩形上) */
static int XjsMkCardHit(const XjsMkGrid& g, float x, float y, float scroll) {
    if (x < g.x0 || x > g.x1) return -1;
    for (int vi = 0; vi < (int)s_mk.view.size(); vi++) {
        XjsRect cr = XjsMkCardRect(g, vi, scroll);
        if (x >= cr.left && x <= cr.right && y >= cr.top && y <= cr.bottom) return vi;
    }
    return -1;
}

static void XjsMkActivateDemoCard(HWND hwnd, int vi) {
    XjsWindowScope scope(XjsMarketOwner());   /* Toast 文案按 owner 语言解析 */
    (void)vi;
    XjsToastShow(hwnd, XjsT(L"商城.提示.市场未接入"), XTOAST_INFO, SS(1));
}

static void XjsMkSwitchCat(HWND hwnd, int cat) {
    if (s_mk.cat == cat) return;
    s_mk.cat = cat;
    XjsMkRebuildView();
    s_mk.scroll = 0;
    XjsMkInvalidate(hwnd);
}

static LRESULT CALLBACK Xjs_MarketWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    XjsMarketState* p = &s_mk;
    /* 整个 WndProc 统一钉在 owner 上下文 (口径同设置窗): 语言/皮肤/SS() 按 owner 解析,
       owner 悬垂回落主窗 (XjsMarketOwner), 各搜索窗的定时器重绑不影响本窗作用域 */
    XjsWindowScope scope(XjsMarketOwner());
    switch (msg) {
        case WM_PAINT: {
            if (s_mkOwner) s_mkOwner->SyncSkin();   /* 画刷取色跟随 owner 皮肤 (g_skin 是竞态镜像) */
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            XjsMkPaint(hwnd);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (p->rt) { p->rt->Resize(ximax(LOWORD(lParam), 1), ximax(HIWORD(lParam), 1)); XjsMkInvalidate(hwnd); }
            XjsMkClampScroll(hwnd);
            return 0;
        case WM_GETMINMAXINFO: {   /* 拖边自由调整的下限 (侧栏 + 最窄一列卡片放得下) */
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            mmi->ptMinTrackSize.x = (LONG)SS(600);
            mmi->ptMinTrackSize.y = (LONG)SS(420);
            return 0;
        }
        case WM_DPICHANGED: {   /* 拖到不同缩放的显示器: 尺度更新 + 资源重建 + 系统建议矩形 */
            p->scale = HIWORD(wParam) / 96.0f;
            XjsMkFreeResources();
            if (lParam) {
                RECT* r = (RECT*)lParam;
                SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                    SWP_NOZORDER | SWP_NOACTIVATE);
            }
            XjsMkInvalidate(hwnd);
            return 0;
        }
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE: {   /* 分辨率广播不带 WM_DPICHANGED: 按每窗有效 DPI 自检对齐 */
            UINT dpi = XjsGetDpiForWindow(hwnd);
            if (dpi && (int)(p->scale * 96.0f + 0.5f) != (int)dpi) {
                p->scale = dpi / 96.0f;
                XjsMkFreeResources();
                XjsMkInvalidate(hwnd);
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (XjsEditFieldMouseMove(hwnd, pt)) return 0;   /* 输入字段拖选跟手 */
            { RECT crc; GetClientRect(hwnd, &crc);
              if (XjsToastMouseMove(hwnd, SS(1), (float)crc.right, (float)crc.bottom, pt)) return 0; }
            RECT crc;
            GetClientRect(hwnd, &crc);
            XjsMkGrid g = XjsMkGridGeom((float)crc.right, (float)crc.bottom);
            int hs = -1, hc = -1;
            if (pt.x < XjsMkSideW()) hs = XjsMkSideHit((float)pt.y);
            else hc = XjsMkCardHit(g, (float)pt.x, (float)pt.y, p->scroll);
            if (hs != p->hoverSide || hc != p->hoverCard) {
                p->hoverSide = hs;
                p->hoverCard = hc;
                XjsMkInvalidate(hwnd);
            }
            if (!p->trackingLeave) {
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                if (TrackMouseEvent(&tme)) p->trackingLeave = true;
            }
            /* 本处理直接 return 0 不过 DefWindowProc → WM_SETCURSOR 不会到来, 显式设光标 */
            SetCursor(LoadCursorW(NULL,
                (hc >= 0 && !s_mk.entries[p->view[hc]].installed) ? IDC_HAND : IDC_ARROW));
            return 0;
        }
        case WM_MOUSELEAVE:
            p->trackingLeave = false;
            XjsToastHoverReset(hwnd);
            if (p->hoverSide != -1 || p->hoverCard != -1) {
                p->hoverSide = -1;
                p->hoverCard = -1;
                XjsMkInvalidate(hwnd);
            }
            return 0;
        case WM_MOUSEWHEEL: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            float delta = -((float)GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA) * SS(34) * 3.0f;
            float maxScroll = p->contentH - (float)(rc.bottom - rc.top);
            if (maxScroll < 0) maxScroll = 0;
            float ns = p->scroll + delta;
            if (ns < 0) ns = 0;
            if (ns > maxScroll) ns = maxScroll;
            if (ns != p->scroll) { p->scroll = ns; XjsMkInvalidate(hwnd); }
            return 0;
        }
        case WM_RBUTTONDOWN: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            XjsEditFieldContextMenu(hwnd, pt);   /* 输入字段右键=编辑菜单 (未中字段=无操作) */
            return 0;
        }
        case WM_POPUP_RESULT: {
            int id = (int)wParam;
            if (id >= IDM_FCTX_BASE && id < IDM_FCTX_BASE + 5) {   /* 字段编辑菜单回传必须最先分流 */
                XjsEditFieldMenuCmd(hwnd, id - IDM_FCTX_BASE);
                return 0;
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (XjsEditFieldDoubleClick(hwnd, pt)) return 0;   /* 字段内双击=选整词 */
        }
            [[fallthrough]];
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            { RECT crc; GetClientRect(hwnd, &crc);
              if (XjsToastMouseDown(hwnd, SS(1), (float)crc.right, (float)crc.bottom, pt)) return 0; }
            /* 输入字段: 点进=聚焦+点定位; 点字段外=失焦后继续常规处理 */
            if (XjsEditFieldMouseDown(hwnd, pt)) return 0;
            RECT crc;
            GetClientRect(hwnd, &crc);
            XjsMkGrid g = XjsMkGridGeom((float)crc.right, (float)crc.bottom);
            if (pt.x < XjsMkSideW()) {
                p->pressSide = XjsMkSideHit((float)pt.y);   /* 分类: 记待定, 松开触发 */
                return 0;
            }
            int hc = XjsMkCardHit(g, (float)pt.x, (float)pt.y, p->scroll);
            if (hc >= 0) {
                XjsRect br = XjsMkBtnRect(XjsMkCardRect(g, hc, p->scroll));
                if (!s_mk.entries[p->view[hc]].installed &&
                    pt.x >= br.left && pt.x <= br.right && pt.y >= br.top && pt.y <= br.bottom)
                    p->pressCard = hc;   /* 演示卡按钮: 记待定, 松开触发 */
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            XjsEditFieldMouseUp(hwnd);   /* 字段拖选收尾+释放捕获 */
            { RECT crc; GetClientRect(hwnd, &crc);
              XjsToastMouseUp(hwnd, SS(1), (float)crc.right, (float)crc.bottom, pt); }
            /* 命令落地 (松开触发口径): 按下待定 + 松开仍命中同一目标才执行, 拖离=取消 */
            int pendingSide = p->pressSide;
            int pendingCard = p->pressCard;
            p->pressSide = -1;
            p->pressCard = -1;
            RECT crc;
            GetClientRect(hwnd, &crc);
            XjsMkGrid g = XjsMkGridGeom((float)crc.right, (float)crc.bottom);
            if (pendingSide >= 0 && pt.x < XjsMkSideW() && pendingSide == XjsMkSideHit((float)pt.y)) {
                static const int SIDE_CAT[MK_CAT_N] = { MK_CAT_ALL, MK_CAT_INSTALLED, MK_CAT_AVAILABLE };
                XjsMkSwitchCat(hwnd, SIDE_CAT[pendingSide]);
                return 0;
            }
            if (pendingCard >= 0) {
                XjsRect br = XjsMkBtnRect(XjsMkCardRect(g, pendingCard, p->scroll));
                if (pt.x >= br.left && pt.x <= br.right && pt.y >= br.top && pt.y <= br.bottom)
                    XjsMkActivateDemoCard(hwnd, pendingCard);
            }
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            /* 输入字段聚焦: Enter/Esc=收束聚焦, 其余编辑键交组件路由 */
            if (XjsEditFocused(hwnd)) {
                if (wParam == VK_RETURN || wParam == VK_ESCAPE) {
                    XjsEditFocused(hwnd)->SetFocused(hwnd, false);
                    XjsMkInvalidate(hwnd);
                    return 0;
                }
                if (XjsEditRouteMsg(hwnd, msg, wParam, lParam)) { XjsMkOnSearchEdited(hwnd); return 0; }
            }
            if (wParam == VK_ESCAPE) {   /* 无聚焦字段: Esc 关窗 (设置窗同口径) */
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_CHAR:
            if (XjsEditRouteMsg(hwnd, msg, wParam, lParam)) { XjsMkOnSearchEdited(hwnd); return 0; }
            return 0;   /* 无其它字符行为, 吞掉防误触 */
        case WM_IME_CHAR:
            XjsEditRouteMsg(hwnd, msg, wParam, lParam);   /* 聚焦期吞掉防双份插入 */
            XjsMkOnSearchEdited(hwnd);
            return 0;
        case WM_IME_STARTCOMPOSITION:
            XjsEditRouteMsg(hwnd, msg, wParam, lParam);   /* 组字窗钉到字段 */
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case WM_IME_COMPOSITION: {
            if (XjsEditRouteImeResult(hwnd, lParam)) { XjsMkOnSearchEdited(hwnd); return 0; }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_SETCURSOR: {
            if (LOWORD(lParam) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                if (XjsEditFieldHit(hwnd, pt)) { SetCursor(LoadCursorW(NULL, IDC_IBEAM)); return TRUE; }
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_ACTIVATE:
            XjsCaretBlink::SetWindowActive(hwnd, LOWORD(wParam) != WA_INACTIVE);   /* 失活熄光标 */
            if (LOWORD(wParam) != WA_INACTIVE) p->modelDirty = true;   /* 激活即重拉: 插件可在设置页被启停 */
            return 0;
        case WM_TIMER:
            if (wParam == ID_TIMER_CARET) {
                XjsCaretBlink::TickWindow(hwnd);   /* 输入字段光标闪烁 */
                return 0;
            }
            if (wParam == ID_TIMER_TOAST) {
                if (!XjsToastTimerTick(hwnd, SS(1))) KillTimer(hwnd, ID_TIMER_TOAST);
                return 0;
            }
            break;
        case WM_DESTROY:
            XjsMkFreeResources();
            KillTimer(hwnd, ID_TIMER_TOAST);           /* Toast 组件定时器摘除 */
            XjsWindowComponentsDetach(hwnd);           /* 组件统一退登记 (Toast 条目/画刷 + 字段登记 + 闪烁驱动) */
            p->searchEd.area = {};
            p->hwnd = NULL;
            p->hoverSide = p->hoverCard = -1;
            p->pressSide = p->pressCard = -1;
            p->scroll = 0;
            p->modelDirty = true;   /* 单例状态复位 (窗销毁后残留会在重开时"复活", 口径同设置窗) */
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void XjsRegisterMarketClass(HINSTANCE hInst) {
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Xjs_MarketWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.style = CS_DBLCLKS;   /* 缺它系统不发 WM_LBUTTONDBLCLK, 输入字段双击选词失效 */
    wc.hIcon = XjsAppIconBig();
    wc.hIconSm = XjsAppIconSmall();
    wc.lpszClassName = L"XJS_MarketWnd";
    RegisterClassExW(&wc);
}

void XjsMarketShow() {
    s_mkOwner = XjsSearchWindow::Cur();   /* 绑定发起窗口 (语言/皮肤按它解析) */
    if (s_mkOwner) s_mkOwner->SyncSkin(); /* g_skin 认回 owner: 首帧画刷即 owner 皮肤 */
    s_mk.modelDirty = true;               /* 打开即重拉注册表 (插件启停状态可能已变) */
    if (s_mk.hwnd) {
        SetWindowTextW(s_mk.hwnd, XjsT(L"应用.商城窗口标题"));
        InvalidateRect(s_mk.hwnd, NULL, FALSE);
        ShowWindow(s_mk.hwnd, SW_RESTORE);
        SetForegroundWindow(s_mk.hwnd);
        return;
    }
    s_mk.scale = g_s;   /* 初值取主窗尺度 (通常同屏); 跨屏由 WM_DPICHANGED 纠正 */
    s_mk.cat = MK_CAT_ALL;
    s_mk.scroll = 0;
    s_mk.searchEd.ed.SetText(L"", false);
    int w = (int)SS(880), h = (int)SS(640);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    if (w > sw - 40) w = sw - 40;
    if (h > sh - 100) h = sh - 100;
    RECT orr;
    GetWindowRect(g_hWnd, &orr);
    int x = orr.left + ((orr.right - orr.left) - w) / 2;
    int y = orr.top + ((orr.bottom - orr.top) - h) / 2;
    if (x < 8) x = 8;
    if (y < 8) y = 8;
    HWND hwnd = CreateWindowExW(0, L"XJS_MarketWnd", XjsT(L"应用.商城窗口标题"),
        WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME | WS_MAXIMIZEBOX, x, y, w, h,
        g_hWnd, NULL, GetModuleHandleW(NULL), NULL);
    if (!hwnd) return;
    s_mk.hwnd = hwnd;
    SendMessageW(hwnd, WM_SETICON, ICON_BIG,   (LPARAM)XjsAppIconBig());     /* 窗口级图标 (商城窗有任务栏按钮) */
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)XjsAppIconSmall());
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));   /* 深色标题栏 (20H1+) */
    DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));                                     /* 更早 build 兜底, 失败无碍 */
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    InvalidateRect(hwnd, NULL, FALSE);
}
