/*
 * xjs_popup.cpp — 自绘弹出菜单组件
 * 菜单栏/模式/历史/筛选/右键共用; LL 鼠标钩子实现"点外关闭并吞掉点击"; DWM 圆角
 */
#include "xjs_app.h"

struct XjsPopupState {
    HWND hwnd = NULL;
    HWND owner = NULL;
    XjsSearchWindow* ownerCtx = NULL;   /* owner 窗口上下文 (多窗皮肤各异: 取色认亲不竞态) */
    XjsSkin skin{};                     /* 打开时刻 owner 皮肤快照 (g_skin 是全局镜像, 绘制期会被别的窗口改写) */
    std::vector<XjsPopupItem> items;
    std::vector<float> y;
    float height = 0, width = 0;
    int hover = -1;
    /* 子菜单 (通用级联, 2026-09-17): 同窗右邻面板, 不另开窗口 —— LL 点外钩子/前台管理/结果
       回传全部天然复用。openSub = 展开子菜单的父项下标; subItems 指向 items[openSub].children
       (菜单打开期 items 不增删, 指针稳定); 根面板右缘 = width, 子面板 [width, width+subW) */
    int openSub = -1;
    const std::vector<XjsPopupItem>* subItems = NULL;
    std::vector<float> ySub;
    float subW = 0, subH = 0;
    int hoverSub = -1;
    XjsHwndRt* rt = NULL;
    XjsSolidBrush *brBg = NULL, *brBorder = NULL, *brHover = NULL, *brText = NULL;
    XjsSolidBrush *brDim = NULL, *brAccent = NULL;
    XjsSolidBrush* brIcon = NULL;      /* 菜单图标 (文字色×0.7, 同源样式 .ctx-ic opacity) */
    XjsSolidBrush* brDanger = NULL;    /* 删除确认药丸红 (同源样式 .mode-del.confirm #e0442e) */
    XjsSolidBrush* brWhite = NULL;     /* 确认药丸文字白 */
    int delConfirm = -1;                      /* 删除确认态项下标 (首击 ✕ 原地变红"确定删除"菜单保持, 再击才真删) */
    XjsFormat *tfTitle = NULL, *tfSub = NULL;
    XjsFormat* tfKb = NULL;           /* 快捷键: 同行右对齐 (同源样式 .ctx-kb margin-left:auto) */
    XjsStroke* ssRound = NULL;         /* 线性图标圆头描边 (同源样式 SVG stroke-linecap=round) */
};
static XjsPopupState s_popup;
static HHOOK s_popupHook = NULL;

static LRESULT CALLBACK Xjs_PopupMouseHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && s_popup.hwnd) {
        MSLLHOOKSTRUCT* ms = (MSLLHOOKSTRUCT*)lp;
        if (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN || wp == WM_MOUSEWHEEL) {
            RECT rc;
            GetWindowRect(s_popup.hwnd, &rc);
            if (!PtInRect(&rc, ms->pt)) {
                HWND h = s_popup.hwnd;
                DestroyWindow(h);
                return 1;
            }
        }
    }
    return CallNextHookEx(s_popupHook, code, wp, lp);
}

/* 释放弹窗 D2D 资源 (文本格式设备无关但一并重建简单).
   换肤热应用时调用, 下次 WM_PAINT 按新皮肤重建; WM_DESTROY 复用 */
static void XjsPopupFreeResources(XjsPopupState* p) {
    if (p->rt) { p->rt->Release(); p->rt = NULL; }
    /* 纯色画刷是独立 COM 对象 (不随 RT 释放), 只置空 = 每次开菜单/换肤泄漏一轮 (同设置窗画刷之坑) */
    if (p->brBg) { p->brBg->Release(); }
    if (p->brBorder) { p->brBorder->Release(); }
    if (p->brHover) { p->brHover->Release(); }
    if (p->brText) { p->brText->Release(); }
    if (p->brDim) { p->brDim->Release(); }
    if (p->brAccent) { p->brAccent->Release(); }
    if (p->brIcon) { p->brIcon->Release(); }
    if (p->brDanger) { p->brDanger->Release(); }
    if (p->brWhite) { p->brWhite->Release(); }
    p->brBg = NULL; p->brBorder = NULL; p->brHover = NULL;
    p->brText = NULL; p->brDim = NULL; p->brAccent = NULL;
    p->brIcon = NULL; p->brDanger = NULL; p->brWhite = NULL;
    /* tfTitle 带字符级省略号 (SetCharEllipsis): 释放前先从签名缓存摘除本格式条目,
       否则缓存键悬垂 (地址复用画错省略号) */
    XjsEllSignCacheDropFormat(p->tfTitle);
    if (p->tfTitle) { p->tfTitle->Release(); p->tfTitle = NULL; }
    if (p->tfSub) { p->tfSub->Release(); p->tfSub = NULL; }
    if (p->tfKb) { p->tfKb->Release(); p->tfKb = NULL; }
    if (p->ssRound) { p->ssRound->Release(); p->ssRound = NULL; }
}

/* ==================== 子菜单 (通用级联, 2026-09-17) ====================
 * 同一弹窗窗口内右邻面板 (不另开窗口): LL 点外钩子/前台管理/WM_POPUP_RESULT 回传全部复用。
 * 面板几何: 根面板 [0, width), 子面板 [width, width+subW); 行高/间距与根面板同款 */

/* 行文字区左缩进 (渲染/宽度测量同源): 带图标 = 图标槽+间距 (同源样式 padding 10 + gap 10); 无图标 = 勾选槽位 */
static float XjsPopupRowIndent(const XjsPopupItem& it) {
    return it.icon ? XSF(36) : XSF(26);
}

/* 行文字区右缘让位 (渲染/宽度测量同源): ✎✕ 双按钮 62 / 仅 ✕ 34 / 子菜单父项 ▸ 22 / 无尾部 = 右缘留白 10 */
static float XjsPopupRowTail(const XjsPopupItem& it) {
    return it.editBtn ? XSF(62) : (it.delBtn ? XSF(34) : (it.children.empty() ? XSF(10) : XSF(22)));
}

/* 子面板行布局 + 宽度 (按内容实测, 夹在 140~320 逻辑 px; tfTitle 未建时取下限) */
static void XjsPopupLayoutSub(XjsPopupState* p) {
    const std::vector<XjsPopupItem>& ch = *p->subItems;
    float pad = XSF(5);
    p->ySub.clear();
    p->ySub.push_back(pad);
    for (auto& it : ch)
        p->ySub.push_back(p->ySub.back() + (it.sep ? XSF(9) : XSF(28)));
    p->subH = p->ySub.back() + pad;
    float tw = XSF(140);
    if (p->tfTitle) {
        for (auto& it : ch) {
            if (it.title.empty()) continue;
            /* 行内容宽 = 左缩进 + 标题 + 快捷键段 + 右让位 (与 drawRow 几何同源);
               漏算快捷键 = 标题省略号右缘正好压在快捷键上 (窗口启动器子菜单"新建空白窗口 Ctrl+N"实锤) */
            float need = pad * 2 + XjsPopupRowIndent(it)
                       + XjsMeasureText(it.title.c_str(), p->tfTitle) + XjsPopupRowTail(it);
            if (!it.sub.empty() && it.children.empty() && p->tfKb)
                need += XjsMeasureText(it.sub.c_str(), p->tfKb) + XSF(12);
            if (need > tw) tw = need;
        }
    }
    p->subW = xf_min(tw, XSF(320));
}

/* 子菜单开合后的窗口尺寸/位置: 总宽=根+子, 总高=两者大者; 越出工作区则整窗收敛 */
static void XjsPopupApplySize(XjsPopupState* p) {
    if (!p->hwnd) return;
    float totalW = p->width + (p->openSub >= 0 ? p->subW : 0);
    float totalH = p->height;
    if (p->openSub >= 0 && p->subH > totalH) totalH = p->subH;
    RECT rc;
    GetWindowRect(p->hwnd, &rc);
    int x = rc.left, y = rc.top;
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfoW(MonitorFromWindow(p->hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
        if (y + (int)totalH > mi.rcWork.bottom - 8) y = mi.rcWork.bottom - 8 - (int)totalH;
        if (y < mi.rcWork.top + 8) y = mi.rcWork.top + 8;
        if (x + (int)totalW > mi.rcWork.right - 8) x = mi.rcWork.right - 8 - (int)totalW;
        if (x < mi.rcWork.left + 8) x = mi.rcWork.left + 8;
    }
    SetWindowPos(p->hwnd, NULL, x, y, (int)totalW, (int)totalH, SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(p->hwnd, NULL, FALSE);
}

static void XjsPopupOpenSub(XjsPopupState* p, int i) {
    if (i < 0 || i >= (int)p->items.size() || p->items[i].children.empty()) return;
    if (i == p->openSub) return;
    p->openSub = i;
    p->subItems = &p->items[i].children;   /* 菜单打开期 items 不增删, 指针稳定 */
    p->hoverSub = -1;
    XjsPopupLayoutSub(p);
    XjsPopupApplySize(p);
}

static void XjsPopupCloseSub(XjsPopupState* p) {
    if (p->openSub < 0) return;
    p->openSub = -1;
    p->subItems = NULL;
    p->hoverSub = -1;
    XjsPopupApplySize(p);
}

/* 自定义模式项尾部按钮几何 (渲染/命中共用, 同源样式 .mode-edit 左 .mode-del 右):
   常态 ✎ 中心 -44 槽 / ✕ 中心 -17 槽; 确认态 ✕ 原地变红色"确定删除"药丸并向左展宽,
   ✎ 左移让位 (源样式 flex: del 变宽把 edit 推向左)。pill 的 y 由调用方按行中心填。 */
static void XjsPopupTailGeom(float w, bool confirm, float* editCx, float* delCx, XjsRect* pill) {
    float pad = XSF(5);
    *editCx = w - pad - XSF(44);
    *delCx = w - pad - XSF(17);
    *pill = XjsRectF(0, 0, 0, 0);
    if (confirm) {
        *editCx = w - pad - XSF(96);
        *pill = XjsRectF(w - pad - XSF(84), 0, w - pad - XSF(4), 0);
    }
}

/* ==================== 菜单线性图标 (照源样式 ctx-ic 16×16 SVG 逐路径描边) ====================
 * 坐标系 = SVG viewBox 16 单位, 以 (cx,cy) 为中心; 描边宽: 文件右键图标 1.2 / SB 族 1.3 (同源样式) */

/* 捐赠图标 (心+托手) 折点表: 用户提供的 1024 viewBox 实心 SVG 路径按 1/64 扁平化到 16 单位坐标
 * (圆弧 24 段/贝塞尔 12 段采样 + RDP 0.015 简化, 首尾同点闭合; 圆头描边下视觉与原图标一致)。
 * 子路径 0 = 心形轮廓, 子路径 1 = 托手轮廓 */
static const float XjsMenuDonateHeartPts[] = {
    8.709f, 9.763f, 7.941f, 9.227f, 7.189f, 8.638f,
    6.356f, 7.899f, 5.948f, 7.488f, 5.568f, 7.058f,
    5.229f, 6.614f, 4.809f, 5.921f, 4.635f, 5.451f,
    4.585f, 5.177f, 4.568f, 4.899f, 4.590f, 4.582f,
    4.653f, 4.278f, 4.756f, 3.989f, 4.894f, 3.719f,
    5.066f, 3.470f, 5.268f, 3.246f, 5.498f, 3.048f,
    5.752f, 2.880f, 6.027f, 2.744f, 6.322f, 2.644f,
    6.633f, 2.581f, 6.956f, 2.560f, 7.406f, 2.617f,
    7.615f, 2.683f, 8.000f, 2.870f, 8.486f, 3.229f,
    8.944f, 3.691f, 9.273f, 3.343f, 9.557f, 3.093f,
    9.890f, 2.862f, 10.272f, 2.680f, 10.699f, 2.574f,
    10.930f, 2.560f, 11.254f, 2.581f, 11.564f, 2.644f,
    11.859f, 2.744f, 12.135f, 2.880f, 12.389f, 3.048f,
    12.618f, 3.246f, 12.820f, 3.470f, 12.992f, 3.719f,
    13.130f, 3.989f, 13.233f, 4.278f, 13.296f, 4.582f,
    13.318f, 4.899f, 13.293f, 5.228f, 13.219f, 5.551f,
    13.097f, 5.864f, 12.991f, 6.065f, 12.673f, 6.572f,
    12.366f, 6.977f, 12.021f, 7.373f, 11.267f, 8.118f,
    10.147f, 9.051f, 9.111f, 9.788f, 8.910f, 9.831f,
    8.709f, 9.763f, 8.709f, 9.763f,
};
static const float XjsMenuDonateHandPts[] = {
    10.349f, 10.886f, 10.405f, 11.001f, 10.378f, 11.182f,
    10.273f, 11.344f, 10.178f, 11.425f, 9.870f, 11.560f,
    9.370f, 11.624f, 8.675f, 11.585f, 7.753f, 11.433f,
    7.045f, 11.223f, 6.685f, 11.075f, 6.201f, 10.829f,
    6.095f, 10.819f, 5.890f, 10.881f, 5.729f, 11.025f,
    5.652f, 11.218f, 5.657f, 11.323f, 5.699f, 11.429f,
    5.782f, 11.531f, 5.912f, 11.625f, 6.719f, 12.004f,
    7.529f, 12.296f, 8.435f, 12.526f, 8.892f, 12.592f,
    9.332f, 12.613f, 9.744f, 12.577f, 10.115f, 12.476f,
    10.431f, 12.300f, 10.894f, 11.892f, 11.200f, 11.513f,
    11.367f, 11.165f, 11.414f, 10.850f, 11.359f, 10.573f,
    11.299f, 10.449f, 11.181f, 10.306f, 11.652f, 9.852f,
    12.003f, 9.586f, 12.286f, 9.472f, 12.611f, 9.453f,
    12.800f, 9.501f, 12.950f, 9.569f, 13.230f, 9.804f,
    13.396f, 10.103f, 13.438f, 10.431f, 13.391f, 10.649f,
    13.310f, 10.816f, 12.000f, 12.488f, 11.179f, 13.627f,
    10.971f, 13.821f, 10.801f, 13.935f, 10.518f, 14.054f,
    10.324f, 14.080f, 6.802f, 14.067f, 6.506f, 13.969f,
    5.464f, 13.372f, 4.709f, 13.023f, 3.932f, 12.949f,
    2.608f, 12.913f, 2.407f, 12.858f, 2.230f, 12.757f,
    2.086f, 12.616f, 1.983f, 12.443f, 1.927f, 12.246f,
    1.920f, 10.326f, 1.949f, 10.120f, 2.029f, 9.934f,
    2.230f, 9.710f, 2.407f, 9.608f, 2.609f, 9.554f,
    2.961f, 9.531f, 3.194f, 9.491f, 4.270f, 9.201f,
    4.613f, 9.162f, 5.005f, 9.167f, 5.280f, 9.189f,
    5.756f, 9.272f, 6.335f, 9.479f, 6.826f, 9.763f,
    7.789f, 10.420f, 7.955f, 10.520f, 8.161f, 10.599f,
    8.876f, 10.735f, 10.349f, 10.886f, 10.349f, 10.886f,
};

static void XjsMenuIconDraw(XjsRt* rt, int icon, float cx, float cy,
                            XjsBrush* br, XjsStroke* ss) {
    if (!icon || !rt || !br) return;
    auto P = [&](float x, float y) { return XjsPoint2F(cx - XSF(8) + XSF(x), cy - XSF(8) + XSF(y)); };
    float wN = XSF(1.2f), wSB = XSF(1.3f), wTray = XSF(1.5f);   /* 托盘项 1.5 (同源样式 Tray.* SVG stroke-width) */
    auto seg = [&](float x1, float y1, float x2, float y2, float w) {
        rt->DrawLine(P(x1, y1), P(x2, y2), br, w, ss);
    };
    switch (icon) {
        case XMI_OPEN:   /* 打开: 盒+右向箭头 */
            seg(10, 3.5f, 3.5f, 3.5f, wN);
            seg(3.5f, 3.5f, 3.5f, 12.5f, wN);
            seg(3.5f, 12.5f, 10, 12.5f, wN);
            seg(7, 8, 13, 8, wN);
            seg(10.5f, 5.5f, 13, 8, wN);
            seg(13, 8, 10.5f, 10.5f, wN);
            break;
        case XMI_FOLDER: {   /* 打开路径: 文件夹 */
            seg(1.5f, 5.5f, 14.5f, 5.5f, wN);
            seg(5.5f, 3.5f, 8, 3.5f, wN);
            seg(8, 3.5f, 9.5f, 5.5f, wN);
            XjsRoundedRect rr = XjsRoundedRectF(
                XjsRectF(cx - XSF(8) + XSF(3), cy - XSF(8) + XSF(5.5f), cx - XSF(8) + XSF(13), cy - XSF(8) + XSF(13)),
                XSF(1.5f), XSF(1.5f));
            rt->DrawRoundedRectangle(rr, br, wN, ss);
            break;
        }
        case XMI_RENAME: {   /* 重命名/复制名称: 铅笔+两条底线 */
            seg(8.5f, 4.5f, 11, 2, wN);
            seg(11, 2, 14, 5, wN);
            seg(14, 5, 8, 11, wN);
            seg(8, 11, 5, 11, wN);
            seg(5, 11, 5, 8, wN);
            seg(5, 8, 8.5f, 4.5f, wN);
            seg(6, 9.5f, 10.5f, 9.5f, wN);
            seg(6, 12, 9, 12, wN);
            break;
        }
        case XMI_ALIAS:   /* 设置别名: 文字+译文行 */
            seg(7, 3.5f, 6, 8, wN);
            seg(3.5f, 7, 8.5f, 7, wN);
            seg(5, 8, 6, 10.5f, wN);
            seg(6, 10.5f, 7.5f, 9, wN);
            seg(7, 3.5f, 9, 2, wN);
            seg(9, 2, 10.5f, 8, wN);
            seg(10.5f, 8, 8, 9.5f, wN);
            seg(12.5f, 5, 15, 5, wN);
            seg(13, 9, 15, 9, wN);
            break;
        case XMI_CUT:   /* 剪刀 */
            rt->DrawEllipse(XjsEllipseF(P(4, 4.5f), XSF(1.7f), XSF(1.7f)), br, wSB, ss);
            rt->DrawEllipse(XjsEllipseF(P(4, 11.5f), XSF(1.7f), XSF(1.7f)), br, wSB, ss);
            seg(5.3f, 5.8f, 12, 13, wSB);
            seg(5.3f, 10.2f, 12, 3, wSB);
            break;
        case XMI_COPY: {   /* 复制: 前后两页 */
            XjsRoundedRect rr = XjsRoundedRectF(
                XjsRectF(cx - XSF(8) + XSF(5.5f), cy - XSF(8) + XSF(5.5f), cx - XSF(8) + XSF(13.5f), cy - XSF(8) + XSF(13.5f)),
                XSF(1.5f), XSF(1.5f));
            rt->DrawRoundedRectangle(rr, br, wSB, ss);
            seg(10.5f, 5.5f, 10.5f, 3.5f, wSB);
            seg(10.5f, 3.5f, 9.5f, 2.5f, wSB);
            seg(9.5f, 2.5f, 4.5f, 2.5f, wSB);
            seg(4.5f, 2.5f, 3.5f, 3.5f, wSB);
            seg(3.5f, 3.5f, 3.5f, 9.5f, wSB);
            seg(3.5f, 9.5f, 4.5f, 10.5f, wSB);
            seg(4.5f, 10.5f, 6.5f, 10.5f, wSB);
            break;
        }
        case XMI_PASTE: {   /* 粘贴: 带夹子的剪贴板 */
            XjsRoundedRect rr = XjsRoundedRectF(
                XjsRectF(cx - XSF(8) + XSF(3.5f), cy - XSF(8) + XSF(2.5f), cx - XSF(8) + XSF(12.5f), cy - XSF(8) + XSF(13.5f)),
                XSF(1.5f), XSF(1.5f));
            rt->DrawRoundedRectangle(rr, br, wSB, ss);
            seg(6.2f, 2.5f, 6.2f, 1.8f, wSB);
            seg(6.2f, 1.8f, 9.8f, 1.8f, wSB);
            seg(9.8f, 1.8f, 9.8f, 2.5f, wSB);
            seg(5.5f, 8, 10.5f, 8, wSB);
            seg(5.5f, 10.5f, 8.5f, 10.5f, wSB);
            break;
        }
        case XMI_SELECTALL: {   /* 全选: 虚框+勾 */
            XjsRoundedRect rr = XjsRoundedRectF(
                XjsRectF(cx - XSF(8) + XSF(3), cy - XSF(8) + XSF(3), cx - XSF(8) + XSF(13), cy - XSF(8) + XSF(13)),
                XSF(1.5f), XSF(1.5f));
            rt->DrawRoundedRectangle(rr, br, wSB, ss);
            seg(6, 8, 7.8f, 9.8f, wSB);
            seg(7.8f, 9.8f, 10.5f, 6.5f, wSB);
            break;
        }
        case XMI_DELETE:   /* 删除: 垃圾桶 */
            seg(3, 4.5f, 13, 4.5f, wSB);
            seg(6.5f, 4.5f, 6.5f, 3, wSB);
            seg(6.5f, 3, 9.5f, 3, wSB);
            seg(9.5f, 3, 9.5f, 4.5f, wSB);
            seg(5, 4.5f, 5.6f, 12.7f, wSB);
            seg(5.6f, 12.7f, 6.6f, 13.6f, wSB);
            seg(6.6f, 13.6f, 9.4f, 13.6f, wSB);
            seg(9.4f, 13.6f, 10.4f, 12.7f, wSB);
            seg(10.4f, 12.7f, 11, 4.5f, wSB);
            break;
        case XMI_COPYPATH: {   /* 复制路径: 页+左侧三条短横 */
            XjsRoundedRect rr = XjsRoundedRectF(
                XjsRectF(cx - XSF(8) + XSF(5), cy - XSF(8) + XSF(2.5f), cx - XSF(8) + XSF(13), cy - XSF(8) + XSF(12.5f)),
                XSF(1), XSF(1));
            rt->DrawRoundedRectangle(rr, br, wN, ss);
            seg(2.5f, 6, 4, 6, wN);
            seg(2.5f, 9.5f, 4, 9.5f, wN);
            seg(2.5f, 13, 4, 13, wN);
            break;
        }
        case XMI_GEAR: {   /* 属性/设置: 齿轮 (中圆+8齿) */
            rt->DrawEllipse(XjsEllipseF(P(8, 8), XSF(1.6f), XSF(1.6f)), br, wN, ss);
            for (int a = 0; a < 8; a++) {
                float ang = a * 3.14159265f / 4;
                float ca = cosf(ang), sa = sinf(ang);
                seg(8 + 4.2f * ca, 8 + 4.2f * sa, 8 + 5.7f * ca, 8 + 5.7f * sa, wN);
            }
            break;
        }
        case XMI_RESTORE: {   /* 托盘-恢复窗口: 窗口框+顶栏 (源样式 Tray.Restore) */
            XjsRoundedRect rr = XjsRoundedRectF(
                XjsRectF(cx - XSF(8) + XSF(2.5f), cy - XSF(8) + XSF(2.5f), cx - XSF(8) + XSF(13.5f), cy - XSF(8) + XSF(13.5f)),
                XSF(1.5f), XSF(1.5f));
            rt->DrawRoundedRectangle(rr, br, wTray, ss);
            seg(2.5f, 5.5f, 13.5f, 5.5f, wTray);
            break;
        }
        case XMI_EXIT:   /* 托盘-退出程序: 左侧门+右向箭头 (源样式 Tray.Exit; 门圆角以斜线段近似, 同 XMI_COPY 拐角口径) */
            seg(10, 3.5f, 4, 3.5f, wTray);
            seg(4, 3.5f, 3, 4.5f, wTray);
            seg(3, 4.5f, 3, 11.5f, wTray);
            seg(3, 11.5f, 4, 12.5f, wTray);
            seg(4, 12.5f, 10, 12.5f, wTray);
            seg(7, 8, 13, 8, wTray);
            seg(10.5f, 5.5f, 13, 8, wTray);
            seg(13, 8, 10.5f, 10.5f, wTray);
            break;
        case XMI_HEART: {   /* 托盘-捐赠: 心+托手 (用户提供 SVG 逐路径描边, 密折点表见函数上方; 圆头描边平滑轮廓) */
            auto poly = [&](const float* xy, int n) {
                for (int i = 0; i < n; i++) {
                    int j = (i + 1) % n;
                    seg(xy[i * 2], xy[i * 2 + 1], xy[j * 2], xy[j * 2 + 1], wTray);
                }
            };
            poly(XjsMenuDonateHeartPts, (int)(sizeof(XjsMenuDonateHeartPts) / sizeof(float) / 2));
            poly(XjsMenuDonateHandPts, (int)(sizeof(XjsMenuDonateHandPts) / sizeof(float) / 2));
            break;
        }
    }
}

/* 撤钟罩/关闭浮层后重绘被盖住的 owner 窗: 目标 = owner (浮层可由任意搜索窗发起, 关闭时
   Cur 往往是别的窗 — 用 Cur 曾把重绘刷错窗, owner 停留在虚化+暗罩残影)。
   owner = 搜索窗 → 实例 Invalidate (统一入口); 其它 (设置窗等) → 直接失效 */
static void XjsInvalidateOverlayOwner(HWND owner) {
    XjsSearchWindow* w = XjsSearchWindow::OfHwnd(owner);
    if (w) w->Invalidate();
    else if (owner && IsWindow(owner)) InvalidateRect(owner, NULL, FALSE);
}

static LRESULT CALLBACK Xjs_PopupWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    XjsPopupState* p = &s_popup;
    /* 整个 WndProc 统一钉在 owner 上下文: 弹窗是独立 hwnd 不过 Enter 绑定, Cur() = 最后处理消息的
       窗口 (其它搜索窗的定时器消息随时重绑) — 只绑 WM_PAINT 时, 鼠标/键盘分支的 XSF 几何
       (✎/✕ 命中框、子面板行高) 与绘制侧两套尺度, 多窗不同 DPI/缩放时命中错位 */
    XjsWindowScope scope(XjsSearchWindow::Alive(p->ownerCtx) ? p->ownerCtx : XjsSearchWindow::Cur());
    switch (msg) {
        case WM_PAINT: {
            /* 弹窗是独立 hwnd: 作用域已在入口钉住 owner (XSF 尺度); 颜色一律读打开时刻的 skin 快照 */
            if (!p->rt) {
                RECT rc; GetClientRect(hwnd, &rc);
                g_gfx->CreateWindowRt(hwnd, ximax(rc.right, 1), ximax(rc.bottom, 1), &p->rt);
                if (p->rt) {
                    /* 画刷取皮肤快照 (owner 的); HwndRT 窗口不支持逐像素 alpha, menu-bg 取色相强制不透明 */
                    XjsColor mb = p->skin.menuBg;
                    mb.a = 1.0f;
                    p->rt->CreateSolidColorBrush(mb, &p->brBg);
                    p->rt->CreateSolidColorBrush(p->skin.borderStrong, &p->brBorder);
                    p->rt->CreateSolidColorBrush(p->skin.rowHover, &p->brHover);
                    p->rt->CreateSolidColorBrush(p->skin.text, &p->brText);
                    p->rt->CreateSolidColorBrush(p->skin.textFaint, &p->brDim);
                    p->rt->CreateSolidColorBrush(p->skin.accent, &p->brAccent);
                    g_dw->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL,
                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f * XjsUiZoom() * g_s, L"zh-cn", &p->tfTitle);
                    g_dw->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL,
                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f * XjsUiZoom() * g_s, L"zh-cn", &p->tfSub);
                    g_dw->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL,
                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f * XjsUiZoom() * g_s, L"zh-cn", &p->tfKb);
                    if (p->tfTitle) {
                        p->tfTitle->SetParagraphAlignment(XJS_PARA_CENTER);
                        /* 单行不换行 + 字符级省略号 (长窗口名/GUID 超宽时截断, 不再叠压下一行) */
                        p->tfTitle->SetWordWrapping(XJS_WRAP_NONE);
                        p->tfTitle->SetCharEllipsis();
                    }
                    if (p->tfSub) p->tfSub->SetParagraphAlignment(XJS_PARA_CENTER);
                    if (p->tfKb) {
                        p->tfKb->SetParagraphAlignment(XJS_PARA_CENTER);
                        p->tfKb->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);   /* 快捷键贴右 */
                        p->tfKb->SetWordWrapping(XJS_WRAP_NONE);
                    }
                    /* 图标画刷 = 文字色×0.7 (同源样式 .ctx-ic opacity:.7); 圆头圆角描边 (同 SVG linecap/linejoin=round) */
                    XjsColor ic = p->skin.text;
                    ic.a *= 0.7f;
                    p->rt->CreateSolidColorBrush(ic, &p->brIcon);
                    p->rt->CreateSolidColorBrush(XjsCol(0xe0442e), &p->brDanger);   /* 删除确认药丸 (同源样式) */
                    p->rt->CreateSolidColorBrush(XjsColorF(1, 1, 1, 1), &p->brWhite);
                    g_gfx->RoundStroke(&p->ssRound);
                }
            }
            if (!p->rt) break;
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            p->rt->BeginDraw();
            {
                XjsColor mb = p->skin.menuBg;
                mb.a = 1.0f;
                p->rt->Clear(mb);
            }
            float pad = XSF(5);
            float rootW = p->width;   /* 根面板右缘: 子面板展开时窗口总宽 > rootW, 不能用像素宽 w */
            /* 单行绘制 (根面板/子面板共用): x0..x1=面板行区, pw=面板几何宽 (尾部按钮用), hovered=悬停高亮 */
            auto drawRow = [&](const XjsPopupItem& it, float x0, float x1, float pw, float y0, float y1,
                               bool hovered, bool confirming) {
                if (it.sep) {
                    p->rt->FillRectangle(XjsRectF(x0 + XSF(6), (y0 + y1) / 2, x1 - XSF(6), (y0 + y1) / 2 + 1), p->brBorder);
                    return;
                }
                if (it.header) {
                    /* 分组标签: 暗色、无悬停、不可点 (同源样式"搜索模式") */
                    XjsRect trr = XjsRectF(x0 + XSF(12), y0, x1 - XSF(10), y1);
                    p->rt->DrawText(it.title.c_str(), (UINT32)it.title.length(), p->tfSub, trr, p->brDim);
                    return;
                }
                if (hovered && !it.disabled)
                    p->rt->FillRoundedRectangle(XjsRoundedRectF(XjsRectF(x0, y0 + XSF(2), x1, y1 - XSF(2)), XSF(6), XSF(6)), p->brHover);
                /* 文字缩进: 带图标 = 图标槽+间距 (同源样式 padding 10 + gap 10); 无图标 = 勾选槽位 */
                float tx = x0 + XjsPopupRowIndent(it);
                if (it.checked) {
                    XjsPoint2 c = { x0 + XSF(14), (y0 + y1) / 2 };
                    float r = XSF(3.4f);
                    p->rt->DrawLine(XjsPoint2F(c.x - r, c.y), XjsPoint2F(c.x - r * 0.2f, c.y + r * 0.8f), p->brAccent, 1.8f);
                    p->rt->DrawLine(XjsPoint2F(c.x - r * 0.2f, c.y + r * 0.8f), XjsPoint2F(c.x + r * 0.9f, c.y - r * 0.7f), p->brAccent, 1.8f);
                }
                if (it.icon)
                    XjsMenuIconDraw(p->rt, it.icon, x0 + XSF(18), (y0 + y1) / 2,
                        it.disabled ? (XjsBrush*)p->brDim : (XjsBrush*)p->brIcon, p->ssRound);
                bool hasChildren = !it.children.empty();
                bool hasKb = !it.sub.empty() && !hasChildren;   /* 子菜单父项右缘画 ▸, 不画快捷键 */
                XjsBrush* tb = it.disabled ? (XjsBrush*)p->brDim : (it.accent ? (XjsBrush*)p->brAccent : (XjsBrush*)p->brText);
                /* 单行: 标题 + 快捷键同行右对齐 (同源样式 .ctx-kb margin-left:auto; 原先画成第二行=换行);
                   尾部让位: ✎✕ 双按钮 (右缩 62) / 仅 ✎ (34) / 子菜单父项 (▸ 箭头 22) */
                float trR = x1 - XjsPopupRowTail(it);
                /* 标题矩形右缘给快捷键让位: 两者同矩形时省略号打满的行会让标题末字压在快捷键下 */
                float kbW = hasKb ? XjsMeasureText(it.sub.c_str(), p->tfKb) : 0;
                XjsRect tr = XjsRectF(tx, y0, trR - (hasKb ? kbW + XSF(12) : 0), y1);
                p->rt->DrawText(it.title.c_str(), (UINT32)it.title.length(), p->tfTitle, tr, tb);
                if (hasKb)
                    p->rt->DrawText(it.sub.c_str(), (UINT32)it.sub.length(), p->tfKb,
                                    XjsRectF(tx, y0, trR, y1), p->brDim);
                if (hasChildren) {
                    /* 右缘 ▸ 子菜单指示 (两段折线, 悬停行随文字同色系) */
                    float cx = x1 - XSF(12), cy = (y0 + y1) / 2;
                    XjsBrush* ab = (hovered && !it.disabled) ? (XjsBrush*)p->brText : (XjsBrush*)p->brDim;
                    p->rt->DrawLine(XjsPoint2F(cx - XSF(2.5f), cy - XSF(4)), XjsPoint2F(cx + XSF(2), cy), ab, XSF(1.4f));
                    p->rt->DrawLine(XjsPoint2F(cx + XSF(2), cy), XjsPoint2F(cx - XSF(2.5f), cy + XSF(4)), ab, XSF(1.4f));
                }
                if (it.editBtn || it.delBtn) {
                    /* 尾部 ✎(编辑) / ✕(删除) (自定义模式项, 源样式菜单项内联按钮; 行悬停染色) */
                    float eCx, dCx;
                    XjsRect pill;
                    XjsPopupTailGeom(pw, confirming, &eCx, &dCx, &pill);
                    float cy = (y0 + y1) / 2;
                    if (it.editBtn) {
                        /* ✎ = 照源样式 .mode-edit 的羽量 edit-2 铅笔轮廓 (SVG 24 viewBox 显示 12×12,
                           即 ×0.5; 描边宽 2→1px)。原先是两根线段拼的斜杠, 视觉不成铅笔 (用户指出)。
                           顶端的圆弧用两段弦线近似 (半径 1.4px, 弦差 <0.5px 不可见) */
                        XjsBrush* eb = hovered ? (XjsBrush*)p->brAccent : (XjsBrush*)p->brDim;
                        auto P24 = [&](float x, float y) {
                            return XjsPoint2F(eCx + XSF((x - 12) * 0.5f), cy + XSF((y - 12) * 0.5f));
                        };
                        /* 源样式 path: M17 3 a2.828 2.828 0 1 1 4 4 L7.5 20.5 L2 22 l1.5 -5.5 z
                           弧段 (17,3)→(21,7) 凸向右上角, 以顶点 (21,3) 两弦近似 */
                        const XjsPoint2 pp[] = {
                            P24(17, 3), P24(21, 3), P24(21, 7), P24(7.5, 20.5),
                            P24(2, 22), P24(3.5, 16.5), P24(17, 3)
                        };
                        for (int k = 0; k < 6; k++)
                            p->rt->DrawLine(pp[k], pp[k + 1], eb, XSF(1.0f), p->ssRound);
                    }
                    if (it.delBtn && confirming && p->brDanger && p->brWhite) {
                        /* 确认态: 红色"确定删除"药丸 (同源样式 .mode-del.confirm; tfKb 尾对齐→居中子矩形) */
                        XjsRect pr = XjsRectF(pill.left, cy - XSF(11), pill.right, cy + XSF(11));
                        p->rt->FillRoundedRectangle(XjsRoundedRectF(pr, XSF(6), XSF(6)), p->brDanger);
                        const wchar_t* ct = XjsT(L"模式对话框.确定删除");
                        float tw = XjsMeasureText(ct, p->tfKb);
                        float pcx = (pill.left + pill.right) / 2;
                        p->rt->DrawText(ct, (UINT32)wcslen(ct), p->tfKb, XjsRectF(pcx - tw / 2, pr.top, pcx + tw / 2, pr.bottom), p->brWhite);
                    } else if (it.delBtn) {   /* ✕ 常态: 行悬停染红提示可删 (源样式 .mode-del: 对角 12 单位×0.5) */
                        float u = XSF(3.0f);
                        XjsBrush* db = hovered ? (XjsBrush*)p->brDanger : (XjsBrush*)p->brDim;
                        p->rt->DrawLine(XjsPoint2F(dCx - u, cy - u), XjsPoint2F(dCx + u, cy + u), db, XSF(1.25f), p->ssRound);
                        p->rt->DrawLine(XjsPoint2F(dCx + u, cy - u), XjsPoint2F(dCx - u, cy + u), db, XSF(1.25f), p->ssRound);
                    }
                }
            };
            for (int i = 0; i < (int)p->items.size(); i++) {
                bool confirming = (p->delConfirm == i);
                drawRow(p->items[i], pad, rootW - pad, rootW, p->y[i], p->y[i + 1], i == p->hover, confirming);
            }
            if (p->openSub >= 0 && p->subItems) {
                /* 子面板: 左缘分隔线 + 行绘制 (子面板行无删除确认态) */
                XjsSizeU wsz = p->rt->GetPixelSize();
                p->rt->FillRectangle(XjsRectF(rootW, pad, rootW + 1, (float)wsz.height - pad), p->brBorder);
                const std::vector<XjsPopupItem>& ch = *p->subItems;
                for (int j = 0; j < (int)ch.size(); j++)
                    drawRow(ch[j], rootW + pad, rootW + p->subW - pad, rootW + p->subW,
                            p->ySub[j], p->ySub[j + 1], j == p->hoverSub, false);
            }
            p->rt->EndDraw();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            /* 双面板命中: 根面板 [0,width) → 根行悬停 + 父项展开/无子项收起;
               子面板 [width, width+subW) → 子行悬停 (openSub 展开期) */
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (p->openSub >= 0 && pt.x >= (int)p->width && pt.x < (int)(p->width + p->subW)) {
                int hs = -1;
                const std::vector<XjsPopupItem>& ch = *p->subItems;
                for (int j = 0; j < (int)ch.size(); j++)
                    if (!ch[j].sep && !ch[j].header && !ch[j].disabled && pt.y >= p->ySub[j] && pt.y < p->ySub[j + 1]) hs = j;
                if (hs != p->hoverSub) { p->hoverSub = hs; InvalidateRect(hwnd, NULL, FALSE); }
                return 0;
            }
            int h = -1;
            for (int i = 0; i < (int)p->items.size(); i++)
                if (!p->items[i].sep && !p->items[i].header && !p->items[i].disabled && pt.y >= p->y[i] && pt.y < p->y[i + 1]) h = i;
            if (h != p->hover) { p->hover = h; InvalidateRect(hwnd, NULL, FALSE); }
            /* 子菜单跟根悬停联动: 移到带 children 的父项=展开; 移到别的项=收起
               (移到行间空白 h=-1 不动 —— 斜向移入子面板的必经路径, 收了会闪) */
            if (h >= 0 && !p->items[h].children.empty()) XjsPopupOpenSub(p, h);
            else if (h >= 0 && h != p->openSub) XjsPopupCloseSub(p);
            return 0;
        }
        case WM_KEYDOWN: {
            /* 键盘导航 (源样式菜单键口径): ↑↓ 选择(跳过禁用项), Enter/空格 执行, Esc 关闭;
               子菜单展开期: ↑↓ 在子面板内选择, → /点击父项 展开, ← /Esc 先收子菜单再关菜单 */
            auto subEnabled = [](const XjsPopupItem& it) { return !it.sep && !it.header && !it.disabled; };
            if (wParam == VK_ESCAPE) {
                if (p->openSub >= 0) XjsPopupCloseSub(p);
                else DestroyWindow(hwnd);
                return 0;
            }
            if (wParam == VK_RIGHT) {
                if (p->hover >= 0 && !p->items[p->hover].children.empty()) {
                    XjsPopupOpenSub(p, p->hover);
                    const std::vector<XjsPopupItem>& ch = *p->subItems;
                    for (int j = 0; j < (int)ch.size(); j++)
                        if (subEnabled(ch[j])) { p->hoverSub = j; break; }
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                return 0;
            }
            if (wParam == VK_LEFT) {
                if (p->openSub >= 0) XjsPopupCloseSub(p);
                return 0;
            }
            if (wParam == VK_UP || wParam == VK_DOWN) {
                int d = (wParam == VK_DOWN) ? 1 : -1;
                if (p->openSub >= 0) {
                    const std::vector<XjsPopupItem>& ch = *p->subItems;
                    int j = p->hoverSub;
                    for (;;) {
                        j += d;
                        if (j < 0 || j >= (int)ch.size()) { j = -1; break; }
                        if (subEnabled(ch[j])) break;
                    }
                    p->hoverSub = j;
                } else {
                    int i = p->hover;
                    for (;;) {
                        i += d;
                        if (i < 0 || i >= (int)p->items.size()) { i = -1; break; }
                        if (!p->items[i].sep && !p->items[i].header && !p->items[i].disabled) break;
                    }
                    p->hover = i;
                }
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (wParam == VK_RETURN || wParam == VK_SPACE) {
                if (p->openSub >= 0) {
                    const std::vector<XjsPopupItem>& ch = *p->subItems;
                    if (p->hoverSub >= 0 && p->hoverSub < (int)ch.size()) {
                        int id = ch[p->hoverSub].id;
                        HWND owner = p->owner;
                        DestroyWindow(hwnd);
                        PostMessageW(owner, WM_POPUP_RESULT, id, 0);
                    }
                    return 0;
                }
                if (p->hover >= 0 && p->hover < (int)p->items.size()) {
                    if (!p->items[p->hover].children.empty()) {   /* 父项 Enter = 展开而非执行 */
                        XjsPopupOpenSub(p, p->hover);
                        InvalidateRect(hwnd, NULL, FALSE);
                        return 0;
                    }
                    int id = p->items[p->hover].id;
                    HWND owner = p->owner;
                    DestroyWindow(hwnd);
                    PostMessageW(owner, WM_POPUP_RESULT, id, 0);
                }
                return 0;
            }
            break;
        }
        case WM_LBUTTONUP: {
            /* 松开才触发 (2026-09-16 用户口径, 全部菜单统一): 按下只悬停高亮, 松开落在哪项
               哪项生效 (防按下误触)。点外关闭仍由低级鼠标钩子负责 (点外"按下"即关)。
               尾部按钮 (几何同渲染 XjsPopupTailGeom): ✕ 首击=原地变红"确定删除"且菜单保持
               (源样式 toggleModeDel, 换行点 ✕ 自动换确认目标), 再击=回传 id|XJS_POPUP_DEL;
               ✎=回传 id|XJS_POPUP_EDIT。子面板行同样松开触发; 父项点击=展开 (菜单保持) */
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            float rootW = p->width;
            if (p->openSub >= 0 && pt.x >= (int)rootW && pt.x < (int)(rootW + p->subW)) {
                const std::vector<XjsPopupItem>& ch = *p->subItems;
                for (int j = 0; j < (int)ch.size(); j++) {
                    if (ch[j].sep || ch[j].header || ch[j].disabled) continue;
                    if (pt.y < p->ySub[j] || pt.y >= p->ySub[j + 1]) continue;
                    int id = ch[j].id;
                    HWND owner = p->owner;
                    DestroyWindow(hwnd);
                    PostMessageW(owner, WM_POPUP_RESULT, id, 0);
                    return 0;
                }
                return 0;
            }
            for (int i = 0; i < (int)p->items.size(); i++) {
                if (p->items[i].sep || p->items[i].header || p->items[i].disabled) continue;
                if (pt.y < p->y[i] || pt.y >= p->y[i + 1]) continue;
                if (!p->items[i].children.empty()) {   /* 父项: 点击只展开子菜单, 不执行不关闭 */
                    XjsPopupOpenSub(p, i);
                    return 0;
                }
                int id = p->items[i].id;
                bool confirming = (p->delConfirm == i);
                HWND owner = p->owner;
                /* 尾部按钮命中 = 与渲染同源的 XjsPopupTailGeom 派生 22px 方框。
                   原先 ✎ 命中框固定 [-58,-30): 确认态 ✎ 视觉已左移让位, 点 ✎ 落空掉进条目=误切模式 */
                float eCx, dCx;
                XjsRect pill;
                XjsPopupTailGeom(rootW, confirming, &eCx, &dCx, &pill);
                if (p->items[i].delBtn &&
                    ((confirming && pt.x >= pill.left && pt.x < pill.right) ||
                     (!confirming && pt.x >= dCx - XSF(11) && pt.x < dCx + XSF(11)))) {
                    if (confirming) {
                        DestroyWindow(hwnd);
                        PostMessageW(owner, WM_POPUP_RESULT, id | XJS_POPUP_DEL, 0);
                    } else {
                        p->delConfirm = i;   /* 首击: 进确认态, 菜单不关 */
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                    return 0;
                }
                if (p->items[i].editBtn && pt.x >= eCx - XSF(11) && pt.x < eCx + XSF(11)) {
                    DestroyWindow(hwnd);
                    PostMessageW(owner, WM_POPUP_RESULT, id | XJS_POPUP_EDIT, 0);
                    return 0;
                }
                DestroyWindow(hwnd);
                PostMessageW(owner, WM_POPUP_RESULT, id, 0);
                return 0;
            }
            return 0;
        }
        case WM_SIZE: {
            /* 子菜单开合经 XjsPopupApplySize 改窗口尺寸: RT 必须随动, 否则绘制落在旧尺寸上 */
            if (p->rt)
                p->rt->Resize(ximax(LOWORD(lParam), 1), ximax(HIWORD(lParam), 1));
            return 0;
        }
        case WM_DESTROY: {
            if (s_popupHook) { UnhookWindowsHookEx(s_popupHook); s_popupHook = NULL; }
            XjsPopupFreeResources(p);
            p->hwnd = NULL;
            g_modeMenuOpen = false;
            g_appMenuOpen = false;
            /* 菜单销毁把激活态还给 owner (原生菜单口径): 菜单弹出曾抢走前台, owner 若靠
               WM_ACTIVATE(WA_INACTIVE) 做"失活即取消"(别名框)会跟着关掉/收不回焦点 */
            if (p->owner && IsWindow(p->owner)) SetForegroundWindow(p->owner);
            XjsInvalidateOverlayOwner(p->owner);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void XjsPopupReleaseResources() {
    XjsPopupFreeResources(&s_popup);
}

void XjsRegisterPopupClass(HINSTANCE hInst) {
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Xjs_PopupWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"XJS_PopupMenu";
    RegisterClassExW(&wc);
}

bool XjsPopupMenuOpen() {
    return s_popup.hwnd != NULL;
}

void XjsShowPopupMenu(HWND owner, POINT anchorScreen, const std::vector<XjsPopupItem>& items, float minWidthPx) {
    if (s_popup.hwnd) DestroyWindow(s_popup.hwnd);
    XjsPopupState* p = &s_popup;
    p->owner = owner;
    p->ownerCtx = XjsSearchWindow::OfHwnd(owner);
    if (p->ownerCtx) p->ownerCtx->SyncSkin();   /* owner 皮肤 → 全局镜像 (此刻 Cur()=owner, 快照即得) */
    p->skin = g_skin;
    p->items = items;
    p->delConfirm = -1;   /* 重开菜单不残留上次的删除确认态 */
    p->openSub = -1; p->subItems = NULL; p->hoverSub = -1;   /* 重开不残留子菜单状态 */
    p->ySub.clear(); p->subW = 0; p->subH = 0;
    /* 行高/边距经 XSF: DPI 感知后窗口像素尺寸须随缩放, 否则高分屏上字大窗小溢出 */
    float pad = XSF(5);
    p->y.clear();
    p->y.push_back(pad);
    for (auto& it : items) {
        if (it.sep) { p->y.push_back(p->y.back() + XSF(9)); continue; }
        p->y.push_back(p->y.back() + XSF(28));   /* 单行高 (快捷键已并入同行右对齐, 不再有双行项) */
    }
    p->height = p->y.back() + pad;
    p->width = xf_max(minWidthPx, XSF(120));
    /* 贴边收敛按锚点所在显示器的工作区 (多屏/任务栏在侧时不串屏) */
    MONITORINFO mi = { sizeof(mi) };
    HMONITOR mon = MonitorFromPoint(anchorScreen, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(mon, &mi);
    int x = (int)anchorScreen.x, y = (int)anchorScreen.y;
    if (x + p->width > mi.rcWork.right - 8) x = (int)(mi.rcWork.right - 8 - p->width);
    if (x < mi.rcWork.left + 8) x = (int)(mi.rcWork.left + 8);
    if (y + p->height > mi.rcWork.bottom - 8) y = (int)(mi.rcWork.bottom - 8 - p->height);
    if (y < mi.rcWork.top + 8) y = (int)(mi.rcWork.top + 8);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"XJS_PopupMenu", L"",
        WS_POPUP, x, y, (int)p->width, (int)p->height, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!hwnd) return;
    p->hwnd = hwnd;
    DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
    ShowWindow(hwnd, SW_SHOWNA);
    SetForegroundWindow(hwnd);
    s_popupHook = SetWindowsHookExW(WH_MOUSE_LL, Xjs_PopupMouseHook, NULL, 0);
    p->hover = -1;
    InvalidateRect(hwnd, NULL, FALSE);
}

/* ==================== 单行自绘编辑器组件 (XjsLineEdit) ====================
 * 搜索框 / 行内重命名 / 别名对话框共用同一实现 (宿主不再各写一份):
 * 光标 x 与文本同源测量 (同一 XjsTextLayout); 鼠标点选/拖拽选字/双击选词;
 * 剪贴板与单档撤销内建; IME 上屏走 GCS_RESULTSTR 整串插入。
 * 文本被改动时置 dirty=1, 由宿主读取后自行处理 (搜索框=输入即搜), 纯光标移动不置位。
 * 光标是否显示由 XjsCaretBlink 决定 (聚焦 && 窗口激活 && 闪烁相位), Render 收 caretOn。 */

/* ==================== 输入光标闪烁驱动器 (XjsCaretBlink) ====================
 * 集中管理 ID_TIMER_CARET + 闪烁相位 + "此刻该不该画光标", 所有输入框共用一份口径:
 * 失焦(Alt+Tab/点其它程序) 一律熄光标并停表, 复焦恢复 —— 不再要求每个宿主各写一遍。
 * 同窗寄存器: WM_TIMER/WM_ACTIVATE 只需知道 HWND, 即可分发到该窗上全部输入框实例。 */
static std::vector<XjsCaretBlink*> s_caretRegs;
static void XjsCaretRegRemove(XjsCaretBlink* p) {
    s_caretRegs.erase(std::remove(s_caretRegs.begin(), s_caretRegs.end(), p), s_caretRegs.end());
}
static void XjsCaretRegAdd(XjsCaretBlink* p) {
    XjsCaretRegRemove(p);
    s_caretRegs.push_back(p);
}
static UINT XjsCaretBlinkMs() { UINT ms = GetCaretBlinkTime(); return ms ? ms : 530; }

void XjsCaretBlink::Attach(HWND hwnd, std::function<void()> repaint) {
    Detach();
    m_hwnd = hwnd;
    m_repaint = std::move(repaint);
    if (m_hwnd) XjsCaretRegAdd(this);
}
void XjsCaretBlink::Detach() {
    HWND hw = m_hwnd;
    m_hwnd = NULL;
    m_repaint = nullptr;
    /* 复位到初始态: 下次 Attach 后 Focus(true) 才能重新起表 (否则 m_focus 残留=第二次进编辑框不闪) */
    m_focus = false;
    m_active = true;
    m_blink = true;
    if (!hw) return;
    XjsCaretRegRemove(this);   /* 先摘登记 (此时 m_hwnd 已清, 不能再按 Hwnd 比对) */
    ApplyTimer(hw);            /* 该窗可能还有别的输入框在闪: 统一重算计时器 */
}
bool XjsCaretBlink::AnyWants(HWND hwnd) {
    for (auto* p : s_caretRegs)
        if (p->m_hwnd == hwnd && p->m_focus && p->m_active) return true;
    return false;
}
void XjsCaretBlink::ApplyTimer(HWND hwnd) {
    if (!hwnd) return;
    /* 同一窗只有一个 ID_TIMER_CARET: 只要该窗上还有任一实例"想显示"(聚焦&&激活)就保持计时器,
       全部不想要才杀 —— 逐实例 kill/set 会互相打架 (后处理的把先处理的计时器关掉) */
    if (AnyWants(hwnd)) SetTimer(hwnd, ID_TIMER_CARET, XjsCaretBlinkMs(), NULL);
    else KillTimer(hwnd, ID_TIMER_CARET);
}
void XjsCaretBlink::RestartTimer() { ApplyTimer(m_hwnd); }
void XjsCaretBlink::Focus(bool on) {
    if (m_focus == on) return;
    m_focus = on;
    m_blink = true;          /* 进入即显 (与系统光标一致: 聚焦/按键后立刻可见) */
    RestartTimer();
}
void XjsCaretBlink::Reset() {
    m_blink = true;
    RestartTimer();          /* 统一走窗口级裁决 (同窗多实例时不误杀别人的计时器) */
}
void XjsCaretBlink::Tick() {
    /* 可见闸门只看 聚焦&&激活 (不看相位): 相位为"熄"时也要继续翻转, 否则永不再亮 */
    if (!m_focus || !m_active) return;
    m_blink = !m_blink;
    if (m_repaint) m_repaint();
}
void XjsCaretBlink::TickWindow(HWND hwnd) {
    /* 遍历中可能有脱绑 (副本防迭代器失效) */
    std::vector<XjsCaretBlink*> copy(s_caretRegs);
    for (auto* p : copy) if (p->m_hwnd == hwnd) p->Tick();
}
void XjsCaretBlink::SetWindowActive(HWND hwnd, bool active) {
    std::vector<XjsCaretBlink*> copy(s_caretRegs);
    for (auto* p : copy) {
        if (p->m_hwnd != hwnd) continue;
        if (p->m_active == active) continue;
        p->m_active = active;
        p->m_blink = active;   /* 失活: 相位归"熄"并重绘一次抹掉静止光标; 复焦: 立刻可见 */
        ApplyTimer(hwnd);
        if (p->m_repaint) p->m_repaint();   /* 失活时抢在停表前擦掉仍画着的光标 (否则冻结残留) */
    }
}
void XjsCaretBlink::DetachWindow(HWND hwnd) {
    std::vector<XjsCaretBlink*> copy(s_caretRegs);
    for (auto* p : copy)
        if (p->m_hwnd == hwnd) p->Detach();
}

/* 每次测量现建一份布局 (文本/格式/高度确定), 光标定位/命中/绘制共用同源坐标 */
static XjsTextLayout* XjsEdMakeLayout(const std::wstring& text, XjsFormat* fmt, float height) {
    XjsTextLayout* lay = NULL;
    if (text.empty() || !fmt || !g_dw) return NULL;
    g_dw->CreateTextLayout(text.c_str(), (UINT32)text.length(), fmt, 100000.0f, height, &lay);
    return lay;
}
static float XjsEdXOf(XjsTextLayout* lay, int idx) {
    FLOAT x = 0, y = 0;
    XjsHitTestMetrics m = {};
    if (lay && SUCCEEDED(lay->HitTestTextPosition((UINT32)idx, FALSE, &x, &y, &m))) return x;
    return 0;
}
static int XjsEdHitIdx(XjsTextLayout* lay, float xLocal, int len, int def) {
    BOOL trail = FALSE, inside = FALSE;
    XjsHitTestMetrics m = {};
    if (lay && SUCCEEDED(lay->HitTestPoint(xLocal, 0.0f, &trail, &inside, &m)))
        return ximin((int)(m.textPosition + (trail ? m.length : 0)), len);
    return def;
}
static bool XjsEdWordChar(wchar_t c) { return iswalnum(c) || c == L'_' || c >= 0x80; }
static int XjsEdWordLeft(const std::wstring& s, int i) {
    while (i > 0 && !XjsEdWordChar(s[i - 1])) i--;
    while (i > 0 && XjsEdWordChar(s[i - 1])) i--;
    return i;
}
static int XjsEdWordRight(const std::wstring& s, int i) {
    int n = (int)s.size();
    while (i < n && !XjsEdWordChar(s[i])) i++;
    while (i < n && XjsEdWordChar(s[i])) i++;
    return i;
}
static int XjsEdAdjIdx(const std::wstring& s, int i, int dir) {
    int n = (int)s.size();
    if (i > 0 && i < n && IS_HIGH_SURROGATE(s[i - 1]) && IS_LOW_SURROGATE(s[i]))
        return dir > 0 ? i + 1 : i - 1;
    return i;
}

/* ---- 多行模式 (multiline) 行模型: 按 '\n' 切逻辑行, 不软换行; 行高 = 字号×1.5, UNIFORM
   行距基线 0.68 (同 xjs_md 配方 — CJK 回退字形不叠压相邻行, 逐行排版行盒精确)。 ---- */
struct XjsEdLine { int start; int len; };   /* 行内容 = text[start, start+len); '\n' 不属于任何行 */
static std::vector<XjsEdLine> XjsEdSplitLines(const std::wstring& s) {
    std::vector<XjsEdLine> v;
    int start = 0, n = (int)s.size();
    for (int i = 0; i <= n; i++)
        if (i == n || s[i] == L'\n') { v.push_back({ start, i - start }); start = i + 1; }
    if (v.empty()) v.push_back({ 0, 0 });
    return v;
}
static float XjsEdLineH(XjsFormat* fmt) { return (fmt ? fmt->GetFontSize() : 12.5f) * 1.5f; }
static XjsTextLayout* XjsEdMakeLineLayout(const std::wstring& line, XjsFormat* fmt) {
    /* 布局高度必须 = 行高: g_tfRow 系格式带 PARAGRAPH_ALIGNMENT_CENTER (单行框靠"布局高=框高"
       垂直居中), 传大高度会把整行垂直居中到几百 px 外 (文字画进裁剪框外 = 看不见, 实锤过) */
    float h = XjsEdLineH(fmt);
    XjsTextLayout* lay = XjsEdMakeLayout(line, fmt, h);
    if (lay) lay->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, h, h * 0.68f);
    return lay;
}
/* 光标绝对下标 → (行, 列): 行 i 覆盖 [start, start+len], 恰在 '\n' 上 = 前一行行尾 */
static void XjsEdCaretLineCol(const std::vector<XjsEdLine>& lines, int caret, int* liOut, int* colOut) {
    int li = (int)lines.size() - 1;
    for (int i = 0; i < (int)lines.size(); i++)
        if (caret <= lines[i].start + lines[i].len) { li = i; break; }
    *liOut = li;
    *colOut = caret - lines[li].start;
}

/* ==================== 输入字段组件: 路由 + 每窗注册表 ==================== */

struct XjsEditReg { HWND hwnd; XjsEditField* f; };
static std::vector<XjsEditReg> s_editRegs;

void XjsEditField::Attach(HWND hwnd, std::function<void()> repaint) {
    blink.Attach(hwnd, repaint);
    for (auto& r : s_editRegs) if (r.f == this) { r.hwnd = hwnd; return; }
    s_editRegs.push_back({ hwnd, this });
}

void XjsEditField::Detach() {
    for (size_t i = 0; i < s_editRegs.size(); i++)
        if (s_editRegs[i].f == this) { s_editRegs.erase(s_editRegs.begin() + i); break; }
    blink.Detach();
}

void XjsEditField::SetFocused(HWND hwnd, bool on) {
    if (focused == on) return;
    focused = on;
    if (on) {
        for (auto& r : s_editRegs)          /* 同窗其余字段自动失焦 (原生 EDIT 单焦点口径) */
            if (r.hwnd == hwnd && r.f != this && r.f->focused) r.f->SetFocused(hwnd, false);
        blink.Focus(true);
        blink.Reset();
        ed.UpdateImeAnchor(hwnd, area, fmt);   /* 聚焦即钉组字窗位置 */
    } else {
        blink.Focus(false);
        ed.dragging = false;
    }
    InvalidateRect(hwnd, NULL, FALSE);   /* 焦点态变化即绘 (光标/聚焦描边), 组件自负责 */
}

XjsEditField* XjsEditFocused(HWND hwnd) {
    for (auto& r : s_editRegs)
        if (r.hwnd == hwnd && r.f->focused) return r.f;
    return NULL;
}

bool XjsEditFieldHit(HWND hwnd, POINT pt) {
    for (auto& r : s_editRegs)
        if (r.hwnd == hwnd && r.f->fmt && XjsPtIn(r.f->area, pt)) return true;
    return false;
}

bool XjsEditFieldMouseDown(HWND hwnd, POINT pt) {
    XjsEditField* hit = NULL;
    for (auto& r : s_editRegs)
        if (r.hwnd == hwnd && r.f->fmt && XjsPtIn(r.f->area, pt)) { hit = r.f; break; }
    if (!hit) {
        if (XjsEditFocused(hwnd)) {   /* 点字段外 = 失焦 (原生 EDIT 口径) */
            XjsEditField* f = XjsEditFocused(hwnd);
            f->SetFocused(hwnd, false);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return false;
    }
    hit->SetFocused(hwnd, true);
    hit->ed.MouseDown(pt, hit->area, hit->fmt);
    SetCapture(hwnd);   /* 拖选出字段仍收 move/up (别名框同款); up 侧由 XjsEditFieldMouseUp 释放 */
    hit->blink.Reset();
    InvalidateRect(hwnd, NULL, FALSE);   /* 点定位/选区起点即绘 */
    return true;
}

/* 拖选/选词的通用转发 (收口处: 宿主只转消息, 不许再手写 SetCapture+MouseMove 私线 —
   搜索框/重命名/别名框三处是先于本层的裸 XjsLineEdit 历史遗留, 新输入框一律走这里) */
bool XjsEditFieldMouseMove(HWND hwnd, POINT pt) {
    bool had = false;
    for (auto& r : s_editRegs)
        if (r.hwnd == hwnd && r.f->fmt && (r.f->ed.dragging || r.f->ed.dragV || r.f->ed.dragH)) {
            r.f->ed.MouseMove(pt, r.f->area, r.f->fmt); had = true;
        }
    if (had) InvalidateRect(hwnd, NULL, FALSE);
    return had;
}

bool XjsEditFieldMouseUp(HWND hwnd) {
    bool had = false;
    for (auto& r : s_editRegs)
        if (r.hwnd == hwnd && (r.f->ed.dragging || r.f->ed.dragV || r.f->ed.dragH)) { r.f->ed.MouseUp(); had = true; }
    if (GetCapture() == hwnd) ReleaseCapture();   /* 模态层吞 up 等异常序列也不漏释放 */
    if (had) InvalidateRect(hwnd, NULL, FALSE);
    return had;
}

/* 多行字段滚轮: 优先滚聚焦字段, 未聚焦滚光标悬停的多行字段 (现代悬停滚动手感);
   纵向 3 行/格, 横向滚轮/Shift+滚轮=横滚; 单行=假 (宿主语义) */
bool XjsEditFieldWheel(HWND hwnd, int delta, bool horizontal, const POINT& pt) {
    XjsEditField* f = XjsEditFocused(hwnd);
    if (!f || !f->ed.multiline || !f->fmt) {
        f = NULL;
        for (auto& r : s_editRegs)
            if (r.hwnd == hwnd && r.f->fmt && r.f->ed.multiline && XjsPtIn(r.f->area, pt)) { f = r.f; break; }
    }
    if (!f) return false;
    return f->ed.Wheel(delta, horizontal, f->area, f->fmt);
}

bool XjsEditFieldDoubleClick(HWND hwnd, POINT pt) {
    for (auto& r : s_editRegs)
        if (r.hwnd == hwnd && r.f->fmt && XjsPtIn(r.f->area, pt)) {
            r.f->ed.MouseDoubleClick(pt, r.f->area, r.f->fmt);
            r.f->blink.Reset();
            InvalidateRect(hwnd, NULL, FALSE);
            return true;   /* 点在字段上: 双击必吃掉 (不落宿主"双击打开"类语义) */
        }
    return false;
}

/* 右键编辑菜单: 菜单表/命令执行收口于此 (搜索框/重命名框/路由层字段/别名框全走这一份) */
void XjsEditMenuAppendItems(const XjsLineEdit& ed, std::vector<XjsPopupItem>& items, int base) {
    bool hasSel = ed.HasSel();
    bool canPaste = IsClipboardFormatAvailable(CF_UNICODETEXT) != 0;
    items.push_back({ base + 0, XjsT(L"编辑菜单.剪切"), L"Ctrl+X", false, false, false, false, !hasSel, XMI_CUT });
    items.push_back({ base + 1, XjsT(L"通用词.复制"), L"Ctrl+C", false, false, false, false, !hasSel, XMI_COPY });
    items.push_back({ base + 2, XjsT(L"编辑菜单.粘贴"), L"Ctrl+V", false, false, false, false, !canPaste, XMI_PASTE });
    items.push_back({ 0, L"", L"", false, true });
    items.push_back({ base + 3, XjsT(L"编辑菜单.全选"), L"Ctrl+A", false, false, false, false, ed.text.empty(), XMI_SELECTALL });
    items.push_back({ base + 4, XjsT(L"通用词.删除"), L"Del", false, false, false, false, !hasSel, XMI_DELETE });
}

void XjsEditMenuApplyCmd(XjsLineEdit& ed, int cmd) {
    switch (cmd) {
        case 0: ed.CutSel(); break;
        case 1: ed.CopySel(); break;
        case 2: ed.Paste(); break;
        case 3: ed.SelectAll(); break;
        case 4: ed.DeleteSel(); break;
    }
}

bool XjsEditFieldContextMenu(HWND hwnd, POINT pt) {
    XjsEditField* hit = NULL;
    for (auto& r : s_editRegs)
        if (r.hwnd == hwnd && r.f->fmt && XjsPtIn(r.f->area, pt)) { hit = r.f; break; }
    if (!hit) return false;
    hit->SetFocused(hwnd, true);   /* 右键也聚焦该字段 (原生 EDIT 口径), 菜单命令按聚焦字段落地 */
    std::vector<XjsPopupItem> items;
    XjsEditMenuAppendItems(hit->ed, items);
    POINT sp = pt;
    ClientToScreen(hwnd, &sp);
    XjsShowPopupMenu(hwnd, sp, items, XSF(150));   /* 结果经 WM_POPUP_RESULT 回宿主, 宿主转 XjsEditFieldMenuCmd */
    return true;
}

void XjsEditFieldMenuCmd(HWND hwnd, int cmd) {
    XjsEditField* f = XjsEditFocused(hwnd);
    if (!f) return;
    XjsEditMenuApplyCmd(f->ed, cmd);
    InvalidateRect(hwnd, NULL, FALSE);
}

bool XjsEditRouteMsg(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    XjsEditField* f = XjsEditFocused(hwnd);
    if (!f) return false;
    switch (msg) {
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) return false;                        /* 宿主语义键先处理 */
            if (wParam == VK_RETURN && !f->ed.multiline) return false;   /* 单行=宿主确认; 多行=字段内换行 */
            if (f->ed.Key(wParam, f->fmt)) {
                f->ed.EnsureCaretVisible(f->area, f->fmt);   /* 多行纵向跟随光标行 (聚焦字段必有本帧矩形) */
                f->blink.Reset();
                InvalidateRect(hwnd, NULL, FALSE);
                return true;
            }
            return false;
        case WM_CHAR:
            if ((wchar_t)wParam == 27) return false;       /* ESC 交宿主 */
            f->ed.Char((wchar_t)wParam);                   /* 聚焦期字符全归字段 (含未消费者, 防漏进宿主全局行为) */
            f->ed.EnsureCaretVisible(f->area, f->fmt);
            f->blink.Reset();
            InvalidateRect(hwnd, NULL, FALSE);             /* 字段即改即绘 (宿主无感, 组件自负责重绘) */
            return true;
        case WM_IME_CHAR:
            return true;                                    /* IME 上屏走 WM_IME_COMPOSITION, 吞掉防双份插入 */
        case WM_IME_STARTCOMPOSITION:
            f->ed.UpdateImeAnchor(hwnd, f->area, f->fmt);
            return false;
        default:
            return false;
    }
}

bool XjsEditRouteImeResult(HWND hwnd, LPARAM lParam) {
    XjsEditField* f = XjsEditFocused(hwnd);
    if (!f) return false;
    if (f->ed.ImeResult(hwnd, lParam)) {
        f->ed.EnsureCaretVisible(f->area, f->fmt);
        f->blink.Reset();
        InvalidateRect(hwnd, NULL, FALSE);   /* 上屏整串即绘 */
        return true;
    }
    return false;
}

bool XjsEditFieldAnchorUpdate(HWND hwnd) {
    XjsEditField* f = XjsEditFocused(hwnd);
    if (!f) return false;
    f->ed.UpdateImeAnchor(hwnd, f->area, f->fmt);
    return true;
}

void XjsEditFieldCleanupWindow(HWND hwnd) {
    for (size_t i = s_editRegs.size(); i-- > 0;)
        if (s_editRegs[i].hwnd == hwnd) s_editRegs.erase(s_editRegs.begin() + i);
    XjsCaretBlink::DetachWindow(hwnd);
}

/* 宿主窗口销毁时的组件统一退登记 (搜索窗/设置窗共用): 见 xjs_app.h 声明处注。
   顺序: 先摘 Toast 条目 (画刷所属 RT 仍有效), 再摘输入字段登记+闪烁驱动 */
void XjsWindowComponentsDetach(HWND hwnd) {
    XjsToastDetach(hwnd);
    XjsEditFieldCleanupWindow(hwnd);
}

void XjsLineEdit::SetText(const std::wstring& s, bool selectAll) {
    text = s;
    caret = (int)s.length();
    anchor = selectAll ? 0 : -1;
    scroll = 0;
    scrollY = 0;
    dragging = dragV = dragH = false;
    m_undoValid = false;
}

void XjsLineEdit::SelRange(int* a, int* b) const {
    if (anchor < 0) { *a = *b = caret; return; }
    *a = ximin(anchor, caret);
    *b = ximax(anchor, caret);
}

void XjsLineEdit::SnapshotUndo() {
    m_undoText = text;
    m_undoCaret = caret;
    m_undoValid = true;
}

void XjsLineEdit::Clear() {
    SnapshotUndo();
    text.clear();
    caret = 0; anchor = -1; scroll = 0; scrollY = 0;
    dragging = dragV = dragH = false;
    dirty = true;
}

void XjsLineEdit::SelectAll() { anchor = 0; caret = (int)text.length(); }

void XjsLineEdit::CopySel() {
    int a, b; SelRange(&a, &b);
    if (b > a) XjsCopyClipboard(text.substr(a, b - a));
}

void XjsLineEdit::CutSel() {
    int a, b; SelRange(&a, &b);
    if (b <= a) return;
    XjsCopyClipboard(text.substr(a, b - a));
    SnapshotUndo();
    text.erase(a, b - a);
    caret = a; anchor = -1;
    dirty = true;
}

void XjsLineEdit::Paste() {
    if (!OpenClipboard(g_hWnd)) return;
    std::wstring s;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t* p = (const wchar_t*)GlobalLock(h);
        if (p) { s = p; GlobalUnlock(h); }
    }
    CloseClipboard();
    if (s.empty()) return;
    if (multiline) {
        for (auto& c : s) if (c == L'\r') c = L'\n';   /* 多行框: CRLF 折 LF, 保留换行 */
    } else {
        for (auto& c : s) if (c == L'\r' || c == L'\n') c = L' ';   /* 单行框: 换行折成空格 */
    }
    SnapshotUndo();
    int a, b; SelRange(&a, &b);
    if (a != b) text.erase(a, b - a);
    caret = ximin(a, (int)text.length());
    text.insert((size_t)caret, s);
    caret += (int)s.length();
    anchor = -1;
    dirty = true;
}

void XjsLineEdit::DeleteSel() {
    int a, b; SelRange(&a, &b);
    if (a == b) return;
    SnapshotUndo();
    text.erase(a, b - a);
    caret = a; anchor = -1;
    dirty = true;
}

bool XjsLineEdit::Key(WPARAM vk, XjsFormat* fmt) {
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    int n = (int)text.length();
    if (vk != VK_UP && vk != VK_DOWN) vx = -1;   /* 纵向移动外的任何键都复位期望列 */
    switch (vk) {
        case VK_LEFT: case VK_RIGHT: {
            int d = (vk == VK_RIGHT) ? 1 : -1;
            if (ctrl) caret = d > 0 ? XjsEdWordRight(text, caret) : XjsEdWordLeft(text, caret);
            else if (shift) {
                if (anchor < 0) anchor = caret;
                int i = caret + d;
                if (i >= 0 && i <= n) caret = XjsEdAdjIdx(text, i, d);
            } else if (anchor >= 0) {
                int a, b; SelRange(&a, &b);
                caret = d > 0 ? b : a;   /* 有选区先收拢到端点 (原生 EDIT 语义) */
            } else {
                int i = caret + d;
                if (i >= 0 && i <= n) caret = XjsEdAdjIdx(text, i, d);
            }
            if (!shift) anchor = -1;
            return true;
        }
        case VK_HOME: case VK_END:
            if (shift && anchor < 0) anchor = caret;
            if (multiline) {   /* 多行 = 当前行的行首/行尾 */
                std::vector<XjsEdLine> lines = XjsEdSplitLines(text);
                int li, col; XjsEdCaretLineCol(lines, caret, &li, &col);
                caret = (vk == VK_HOME) ? lines[li].start : lines[li].start + lines[li].len;
            } else {
                caret = (vk == VK_HOME) ? 0 : n;
            }
            if (!shift) anchor = -1;
            return true;
        case VK_UP: case VK_DOWN: {
            if (!multiline) return false;   /* 单行无纵向移动 (宿主语义) */
            std::vector<XjsEdLine> lines = XjsEdSplitLines(text);
            int li, col;
            XjsEdCaretLineCol(lines, caret, &li, &col);
            if (vx < 0) {   /* 连续 ↑↓ 记住期望列 (斜行不漂移), 其它键已复位 */
                XjsTextLayout* lay = XjsEdMakeLineLayout(text.substr(lines[li].start, lines[li].len), fmt);
                vx = XjsEdXOf(lay, col);
                if (lay) lay->Release();
            }
            if (shift && anchor < 0) anchor = caret;
            int tj = (vk == VK_DOWN) ? li + 1 : li - 1;
            if (tj < 0) caret = lines[0].start;                            /* 首行再上 = 顶部 */
            else if (tj >= (int)lines.size()) caret = (int)text.length();  /* 末行再下 = 底部 */
            else {
                XjsTextLayout* lay = XjsEdMakeLineLayout(text.substr(lines[tj].start, lines[tj].len), fmt);
                int tcol = XjsEdHitIdx(lay, vx, lines[tj].len, lines[tj].len);
                if (lay) lay->Release();
                caret = lines[tj].start + tcol;
            }
            if (!shift) anchor = -1;
            return true;
        }
        case VK_RETURN:
            if (!multiline) return false;   /* 单行 = 宿主语义键 (搜索/确认), 组件不碰 */
            SnapshotUndo();
            {
                int a, b; SelRange(&a, &b);
                if (a != b) text.erase(a, b - a);
                caret = ximin(a, (int)text.length());
                text.insert((size_t)caret, 1, L'\n');
                caret++;
            }
            anchor = -1;
            dirty = true;
            return true;
        case VK_BACK: {
            int before = n;
            SnapshotUndo();
            int a, b; SelRange(&a, &b);
            if (a != b) { text.erase(a, b - a); caret = a; }
            else if (caret > 0) {
                int i = caret - 1;
                if (i > 0 && IS_LOW_SURROGATE(text[i]) && IS_HIGH_SURROGATE(text[i - 1])) i--;
                text.erase(i, caret - i);
                caret = i;
            }
            anchor = -1;
            if ((int)text.length() != before) dirty = true;   /* 空文本退格=纯光标刷新, 不触发重搜 */
            return true;
        }
        case VK_DELETE: {
            int before = n;
            SnapshotUndo();
            int a, b; SelRange(&a, &b);
            if (a != b) { text.erase(a, b - a); caret = a; }
            else if (caret < n) {
                int i = XjsEdAdjIdx(text, caret + 1, 1);
                text.erase(caret, i - caret);
            }
            anchor = -1;
            if ((int)text.length() != before) dirty = true;
            return true;
        }
        case 'A':
            if (!ctrl) return false;
            SelectAll();
            return true;
        case 'C':
            if (!ctrl) return false;
            CopySel();
            return true;
        case 'X':
            if (!ctrl) return false;
            CutSel();
            return true;
        case 'V':
            if (!ctrl) return false;
            Paste();
            return true;
        case 'Z':
            if (!ctrl || !m_undoValid) return ctrl;
            text = m_undoText;
            caret = ximin(m_undoCaret, (int)text.length());
            anchor = -1;
            m_undoValid = false;
            dirty = true;
            return true;
        case VK_INSERT:
            if (ctrl) { CopySel(); return true; }
            if (shift) { Paste(); return true; }
            return false;
    }
    return false;
}

bool XjsLineEdit::Char(wchar_t ch) {
    if (ch < 0x20 || ch == 0x7F) return false;   /* 控制字符走 Key */
    vx = -1;
    SnapshotUndo();
    int a, b; SelRange(&a, &b);
    if (a != b) { text.erase(a, b - a); caret = a; }
    text.insert((size_t)caret, 1, ch);
    caret++;
    anchor = -1;
    dirty = true;
    return true;
}

bool XjsLineEdit::ImeResult(HWND hwnd, LPARAM lParam) {
    if (!(lParam & GCS_RESULTSTR)) return false;
    HIMC himc = ImmGetContext(hwnd);
    if (himc) {
        LONG size = ImmGetCompositionStringW(himc, GCS_RESULTSTR, NULL, 0);
        if (size > 0) {
            std::wstring result((size_t)size / sizeof(wchar_t), L'\0');
            ImmGetCompositionStringW(himc, GCS_RESULTSTR, &result[0], size);
            vx = -1;
            SnapshotUndo();
            int a, b; SelRange(&a, &b);
            if (a != b) { text.erase(a, b - a); caret = a; }
            text.insert((size_t)caret, result);
            caret += (int)result.length();
            anchor = -1;
            dirty = true;
        }
        ImmReleaseContext(hwnd, himc);
    }
    return true;
}

int XjsLineEdit::IndexAtPoint(POINT pt, const XjsRect& area, XjsFormat* fmt) {
    if (!multiline) {
        if (text.empty()) return 0;
        XjsTextLayout* lay = XjsEdMakeLayout(text, fmt, area.bottom - area.top);
        int idx = XjsEdHitIdx(lay, (float)pt.x - area.left + scroll, (int)text.length(), caret);
        if (lay) lay->Release();
        return idx;
    }
    /* 多行: y 定行 (行高精确 = XjsEdLineH), x 行内命中 */
    std::vector<XjsEdLine> lines = XjsEdSplitLines(text);
    float lh = XjsEdLineH(fmt);
    int li = (int)(((float)pt.y - area.top + scrollY) / lh);
    li = ximax(0, ximin(li, (int)lines.size() - 1));
    XjsTextLayout* lay = XjsEdMakeLineLayout(text.substr(lines[li].start, lines[li].len), fmt);
    int col = XjsEdHitIdx(lay, (float)pt.x - area.left + scroll, lines[li].len, lines[li].len);
    if (lay) lay->Release();
    return lines[li].start + col;
}

/* 按下: 无 Shift 时锚点=落点 (随后拖拽即从此展开选区); Shift=从现有锚点扩展。
   多行模式优先命中滚动条 (命中即进入条拖拽, 不动光标) */
void XjsLineEdit::MouseDown(POINT pt, const XjsRect& area, XjsFormat* fmt) {
    vx = -1;
    if (multiline) {
        XjsRect vt, ht; float mx, my;
        Scrollbars(area, fmt, &vt, &ht, &mx, &my);
        if (my > 0 && XjsPtIn(vt, pt)) { dragV = true; dragGrab = (float)pt.y - vt.top; return; }
        if (mx > 0 && XjsPtIn(ht, pt)) { dragH = true; dragGrab = (float)pt.x - ht.left; return; }
    }
    int idx = IndexAtPoint(pt, area, fmt);
    if (GetKeyState(VK_SHIFT) & 0x8000) {
        if (anchor < 0) anchor = caret;
    } else {
        anchor = idx;
    }
    caret = idx;
    dragging = true;
}

void XjsLineEdit::MouseMove(POINT pt, const XjsRect& area, XjsFormat* fmt) {
    if (dragV || dragH) {   /* 滚动条拖拽: 抓点跟随, thumb 位移比例映射到滚动量 */
        XjsRect vt, ht; float mx, my;
        Scrollbars(area, fmt, &vt, &ht, &mx, &my);
        if (dragV && my > 0) {
            float trackT = area.top + XSF(1), trackH = (area.bottom - area.top) - 2 * XSF(1);
            float thumbH = vt.bottom - vt.top;
            float r = (trackH - thumbH) > 0 ? ((float)pt.y - dragGrab - trackT) / (trackH - thumbH) : 0.0f;
            scrollY = xf_max(0.0f, xf_min(r * my, my));
            return;
        }
        if (dragH && mx > 0) {
            float trackL = area.left + XSF(1), trackW = (area.right - area.left) - 2 * XSF(1);
            float thumbW = ht.right - ht.left;
            float r = (trackW - thumbW) > 0 ? ((float)pt.x - dragGrab - trackL) / (trackW - thumbW) : 0.0f;
            scroll = xf_max(0.0f, xf_min(r * mx, mx));
            return;
        }
    }
    if (!dragging) return;
    caret = IndexAtPoint(pt, area, fmt);
}

void XjsLineEdit::MouseUp() {
    dragging = false;
    dragV = dragH = false;
    if (anchor == caret) anchor = -1;   /* 单击零长选区收拢 */
}

bool XjsLineEdit::MouseDoubleClick(POINT pt, const XjsRect& area, XjsFormat* fmt) {
    if (text.empty()) return false;
    int i = IndexAtPoint(pt, area, fmt);
    int a = XjsEdWordLeft(text, i), b = XjsEdWordRight(text, i);
    if (b > a) { anchor = a; caret = b; return true; }
    return false;
}

void XjsLineEdit::EnsureCaretVisible(const XjsRect& area, XjsFormat* fmt) {
    float areaW = area.right - area.left;
    if (areaW <= XSF(16)) return;
    float areaH = area.bottom - area.top;
    if (!multiline) {
        XjsTextLayout* lay = XjsEdMakeLayout(text, fmt, area.bottom - area.top);
        float textW = 0;
        if (lay) { XjsTextMetrics tm = {}; lay->GetMetrics(&tm); textW = tm.width; }
        float cx = XjsEdXOf(lay, caret);
        if (lay) lay->Release();
        if (cx - scroll > areaW - XSF(6)) scroll = cx - areaW + XSF(6);
        if (cx - scroll < XSF(2)) scroll = cx - XSF(2);
        scroll = xf_max(0.0f, xf_min(scroll, xf_max(0.0f, textW - areaW + XSF(4))));
        return;
    }
    /* 多行: 横向跟光标列 (滚动范围按最长行), 纵向跟光标行 (总高 = 行数×行高) */
    std::vector<XjsEdLine> lines = XjsEdSplitLines(text);
    float lh = XjsEdLineH(fmt);
    int li, col;
    XjsEdCaretLineCol(lines, caret, &li, &col);
    XjsTextLayout* lay = XjsEdMakeLineLayout(text.substr(lines[li].start, lines[li].len), fmt);
    float cx = XjsEdXOf(lay, col);
    if (lay) lay->Release();
    if (cx - scroll > areaW - XSF(6)) scroll = cx - areaW + XSF(6);
    if (cx - scroll < XSF(2)) scroll = cx - XSF(2);
    float maxW = 0;
    for (auto& L : lines) {
        XjsTextLayout* l2 = XjsEdMakeLineLayout(text.substr(L.start, L.len), fmt);
        if (l2) { XjsTextMetrics tm = {}; l2->GetMetrics(&tm); maxW = xf_max(maxW, tm.width); l2->Release(); }
    }
    scroll = xf_max(0.0f, xf_min(scroll, xf_max(0.0f, maxW - areaW + XSF(4))));
    float caretTop = li * lh - scrollY;
    if (caretTop < 0) scrollY = (float)li * lh;
    if (caretTop + lh > areaH) scrollY = (float)(li + 1) * lh - areaH;
    scrollY = xf_max(0.0f, xf_min(scrollY, xf_max(0.0f, (float)lines.size() * lh - areaH)));
}

void XjsLineEdit::Scrollbars(const XjsRect& area, XjsFormat* fmt,
                             XjsRect* vt, XjsRect* ht, float* maxScrollX, float* maxScrollY) const {
    float areaW = area.right - area.left, areaH = area.bottom - area.top;
    float lh = XjsEdLineH(fmt);
    std::vector<XjsEdLine> lines = XjsEdSplitLines(text);
    float contentH = (float)lines.size() * lh, contentW = 0;
    for (auto& L : lines) {
        XjsTextLayout* lay = XjsEdMakeLineLayout(text.substr(L.start, L.len), fmt);
        if (lay) { XjsTextMetrics tm = {}; lay->GetMetrics(&tm); contentW = xf_max(contentW, tm.width); lay->Release(); }
    }
    float maxSX = xf_max(0.0f, contentW - areaW + XSF(4));
    float maxSY = xf_max(0.0f, contentH - areaH);
    *maxScrollX = maxSX; *maxScrollY = maxSY;
    *vt = XjsRect{}; *ht = XjsRect{};
    if (maxSY > 0) {
        float trackT = area.top + XSF(1), trackH = areaH - 2 * XSF(1);
        float thumbH = xf_max(XSF(24), areaH / contentH * trackH);
        float ty = trackT + (trackH - thumbH) * (scrollY / maxSY);
        *vt = XjsRectF(area.right - XSF(8) - XSF(1), ty, area.right - XSF(1), ty + thumbH);
    }
    if (maxSX > 0) {
        float trackL = area.left + XSF(1), trackW = areaW - 2 * XSF(1);
        float thumbW = xf_max(XSF(24), areaW / (contentW + XSF(4)) * trackW);
        float tx = trackL + (trackW - thumbW) * (scroll / maxSX);
        *ht = XjsRectF(tx, area.bottom - XSF(8) - XSF(1), tx + thumbW, area.bottom - XSF(1));
    }
}

bool XjsLineEdit::Wheel(int delta, bool horizontal, const XjsRect& area, XjsFormat* fmt) {
    if (!multiline) return false;   /* 单行滚轮 = 宿主语义 (视图切换等) */
    if (!horizontal && (GetKeyState(VK_SHIFT) & 0x8000)) horizontal = true;
    XjsRect vt, ht; float maxSX, maxSY;
    Scrollbars(area, fmt, &vt, &ht, &maxSX, &maxSY);
    float step = (float)delta / 120.0f * XjsEdLineH(fmt) * 3.0f;   /* 一格滚 3 行 */
    if (horizontal) scroll = xf_max(0.0f, xf_min(scroll + step, maxSX));
    else scrollY = xf_max(0.0f, xf_min(scrollY - step, maxSY));
    return true;
}

void XjsLineEdit::UpdateImeAnchor(HWND hwnd, const XjsRect& area, XjsFormat* fmt) {
    if (!hwnd) return;
    POINT p;
    if (!multiline) {
        XjsTextLayout* lay = XjsEdMakeLayout(text, fmt, area.bottom - area.top);
        float cx = XjsEdXOf(lay, caret);
        if (lay) lay->Release();
        p = { (LONG)(area.left + cx - scroll), (LONG)area.top };
    } else {
        std::vector<XjsEdLine> lines = XjsEdSplitLines(text);
        int li, col;
        XjsEdCaretLineCol(lines, caret, &li, &col);
        XjsTextLayout* lay = XjsEdMakeLineLayout(text.substr(lines[li].start, lines[li].len), fmt);
        float cx = XjsEdXOf(lay, col);
        if (lay) lay->Release();
        p = { (LONG)(area.left + cx - scroll), (LONG)(area.top + li * XjsEdLineH(fmt) - scrollY) };
    }
    /* 隐藏系统光标作位置源 (1px 永不 Show; TSF 兼容层据此定位组字/候选窗) */
    if (!g_sysCaretMade && CreateCaret(hwnd, (HBITMAP)NULL, 1, 1)) g_sysCaretMade = true;
    if (g_sysCaretMade) SetCaretPos(p.x, p.y);
    HIMC himc = ImmGetContext(hwnd);
    if (!himc) return;
    COMPOSITIONFORM cf = {};
    cf.dwStyle = CFS_POINT;
    cf.ptCurrentPos = p;
    ImmSetCompositionWindow(himc, &cf);
    CANDIDATEFORM cdf = {};
    cdf.dwIndex = 0;
    cdf.dwStyle = CFS_CANDIDATEPOS;
    cdf.ptCurrentPos = p;
    ImmSetCandidateWindow(himc, &cdf);
    if (g_hFontEdit) {
        LOGFONTW lf = {};
        if (GetObjectW(g_hFontEdit, sizeof(lf), &lf)) ImmSetCompositionFontW(himc, &lf);
    }
    ImmReleaseContext(hwnd, himc);
}

/* 绘制: 选区底 → 文本(空则占位符) → 光标。target 可为主窗 RT 或独立窗口 RT (画刷须同源)。
   占位符也由本组件画, 宿主不再各自判断空文本 —— 空文本时**光标照常显示**(与有文本一致)。
   光标 x 在 layout 释放前取出, 避免悬空 layout (这是重命名一画就崩的根因)。 */
void XjsLineEdit::Render(XjsRt* target, const XjsRect& area, XjsFormat* fmt,
                         XjsBrush* textBr, XjsBrush* selectionBr, XjsBrush* caretBr,
                         const wchar_t* placeholder, XjsBrush* placeholderBr, bool caretOn,
                         XjsBrush* scrollbarBr) {
    if (!target || area.right - area.left <= 1) return;
    if (multiline) {
        /* 多行绘制: 逐逻辑行排版 (行高精确), 原点取整像素 (亚像素定位伤 ClearType);
           光标 x 在 layout 释放前取出 (悬空 layout = DWrite 内崩溃的根因, 与单行同红线) */
        std::vector<XjsEdLine> lines = XjsEdSplitLines(text);
        float lh = XjsEdLineH(fmt);
        target->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if (anchor >= 0 && selectionBr) {
            int a, b; SelRange(&a, &b);
            for (int i = 0; i < (int)lines.size(); i++) {
                int s1 = ximax(a, lines[i].start), s2 = ximin(b, lines[i].start + lines[i].len);
                if (s2 <= s1) continue;
                XjsTextLayout* lay = XjsEdMakeLineLayout(text.substr(lines[i].start, lines[i].len), fmt);
                float x1 = XjsEdXOf(lay, s1 - lines[i].start) - scroll;
                float x2 = XjsEdXOf(lay, s2 - lines[i].start) - scroll;
                if (lay) lay->Release();
                if (x2 > x1)
                    target->FillRectangle(XjsRectF(area.left + x1, area.top + floorf(i * lh - scrollY) + XSF(1),
                                                      area.left + x2, area.top + floorf((i + 1) * lh - scrollY) - XSF(1)),
                                          selectionBr);
            }
        }
        for (int i = 0; i < (int)lines.size(); i++) {
            std::wstring lineText = text.substr(lines[i].start, lines[i].len);
            XjsTextLayout* lay = XjsEdMakeLineLayout(lineText, fmt);
            if (lay) {
                XjsPoint2 org = XjsPoint2F(floorf(area.left - scroll), area.top + floorf(i * lh - scrollY));
                if (target == g_rt)
                    XjsDrawTextLayoutC(org, lay, textBr, D2D1_DRAW_TEXT_OPTIONS_NONE);
                else
                    target->DrawTextLayout(org, lay, textBr, D2D1_DRAW_TEXT_OPTIONS_NONE);
                lay->Release();
            }
        }
        if (text.empty() && placeholder && placeholderBr) {
            if (target == g_rt)
                XjsDrawTextC(placeholder, (UINT32)wcslen(placeholder), fmt,
                             XjsRectF(area.left, area.top, area.right, area.top + lh), placeholderBr,
                             D2D1_DRAW_TEXT_OPTIONS_NONE);
            else {
                XjsTextLayout* ph = XjsEdMakeLineLayout(placeholder, fmt);
                if (ph) {
                    target->DrawTextLayout(XjsPoint2F(area.left, area.top), ph, placeholderBr, D2D1_DRAW_TEXT_OPTIONS_NONE);
                    ph->Release();
                }
            }
        }
        if (caretOn && caretBr) {
            int li, col;
            XjsEdCaretLineCol(lines, caret, &li, &col);
            XjsTextLayout* lay = XjsEdMakeLineLayout(text.substr(lines[li].start, lines[li].len), fmt);
            float cx = XjsEdXOf(lay, col);
            if (lay) lay->Release();
            float top = area.top + floorf(li * lh - scrollY);
            target->FillRectangle(XjsRectF(area.left + cx - scroll, top + XSF(2),
                                              area.left + cx - scroll + 2.0f, top + lh - XSF(2)), caretBr);
        }
        if (scrollbarBr) {   /* 溢出才画 (几何与命中/拖拽同源 Scrollbars) */
            XjsRect vt, ht; float mx, my;
            Scrollbars(area, fmt, &vt, &ht, &mx, &my);
            if (my > 0) target->FillRoundedRectangle(XjsRoundedRectF(vt, XSF(4), XSF(4)), scrollbarBr);
            if (mx > 0) target->FillRoundedRectangle(XjsRoundedRectF(ht, XSF(4), XSF(4)), scrollbarBr);
        }
        target->PopAxisAlignedClip();
        return;
    }
    float cy = (area.top + area.bottom) / 2;
    XjsTextLayout* lay = XjsEdMakeLayout(text, fmt, area.bottom - area.top);
    auto xOf = [&](int idx) {
        FLOAT x = 0, y = 0;
        XjsHitTestMetrics m = {};
        if (lay && SUCCEEDED(lay->HitTestTextPosition((UINT32)idx, FALSE, &x, &y, &m))) return x;
        return 0.0f;
    };
    float caretX = xOf(caret);   /* 空文本时 layout=NULL → 0, 光标落到区域左缘 */
    target->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    /* 选区底必须在裁剪内: 全选超长文本时 x1..x2 跨全文本宽, 画在裁剪外会溢出编辑区
       盖住搜索框右侧 ×/历史按钮 (选区起点还是 0-scroll 的负坐标, 左侧同样会漏) */
    if (anchor >= 0 && selectionBr) {
        int a, b; SelRange(&a, &b);
        float x1 = xOf(a) - scroll, x2 = xOf(b) - scroll;
        if (x2 > x1)
            target->FillRectangle(XjsRectF(area.left + x1, area.top + XSF(2), area.left + x2, area.bottom - XSF(2)),
                selectionBr);
    }
    if (lay) {
        if (target == g_rt)
            XjsDrawTextLayoutC(XjsPoint2F(area.left - scroll, area.top), lay, textBr, D2D1_DRAW_TEXT_OPTIONS_NONE);
        else
            target->DrawTextLayout(XjsPoint2F(area.left - scroll, area.top), lay, textBr, D2D1_DRAW_TEXT_OPTIONS_NONE);
        lay->Release();
        lay = NULL;
    } else if (placeholder && placeholderBr) {
        /* 空文本占位符 (按 fmt 的居中/对齐绘制, 与有文本时同一矩形) */
        if (target == g_rt)
            XjsDrawTextC(placeholder, (UINT32)wcslen(placeholder), fmt, area, placeholderBr, D2D1_DRAW_TEXT_OPTIONS_NONE);
        else {
            XjsTextLayout* ph = XjsEdMakeLayout(placeholder, fmt, area.bottom - area.top);
            if (ph) {
                target->DrawTextLayout(XjsPoint2F(area.left, area.top), ph, placeholderBr, D2D1_DRAW_TEXT_OPTIONS_NONE);
                ph->Release();
            }
        }
    }
    if (caretOn && caretBr) {
        float cx = area.left + caretX - scroll;
        target->FillRectangle(XjsRectF(cx, cy - XSF(8), cx + 2.0f, cy + XSF(8)), caretBr);
    }
    target->PopAxisAlignedClip();
}

/* ==================== 自绘输入对话框 (设置别名等单行输入) ==================== */

struct XjsInputState {
    HWND hwnd = NULL;
    HWND owner = NULL;
    XjsSearchWindow* ownerCtx = NULL;   /* owner 窗口上下文 (多窗皮肤各异: 取色认亲不竞态) */
    XjsSkin skin{};                     /* 打开时刻 owner 皮肤快照 */
    int resultId = 0;
    XjsLineEdit ed;
    XjsRect editBox{}, editText{}, okBtn{}, cancelBtn{};
    XjsHwndRt* rt = NULL;
    XjsSolidBrush *brPanel = NULL;   /* 面板底 */
    XjsSolidBrush *brSelection = NULL;
    XjsSolidBrush *brBorder = NULL;
    XjsSolidBrush *brPanel2 = NULL;
    XjsSolidBrush *brText = NULL;
    XjsSolidBrush *brTextDim = NULL;
    XjsSolidBrush *brTextFaint = NULL;
    XjsSolidBrush *brAccent = NULL;
    XjsFormat* tfDesc = NULL;   /* 说明文字 (字符级换行+UNIFORM 行距: 窗高随内容自适应) */
    float descH = 0;            /* 说明文字实测高 (含多行) */
    std::wstring title, desc;
    int hoverBtn = 0;   /* 1=确定 2=取消 */
    int pressBtn = 0;   /* 按钮按下待定 (松开触发): 1=确定 2=取消 */
    XjsCaretBlink blink;   /* 输入光标闪烁 (窗口激活=WA_ACTIVE 时才显; 独立窗, 不随主窗 WM_ACTIVATE 变) */
};
static XjsInputState s_input;

/* 纯色画刷是独立 COM 对象 (不随 RT 释放), 只置空 = 每次打开对话框泄漏一轮 (同设置窗画刷之坑);
   WM_DESTROY 与设备丢失重建两处同用 */
static void XjsInputFreeBrushes() {
    if (s_input.brPanel) { s_input.brPanel->Release(); }
    if (s_input.brSelection) { s_input.brSelection->Release(); }
    if (s_input.brBorder) { s_input.brBorder->Release(); }
    if (s_input.brPanel2) { s_input.brPanel2->Release(); }
    if (s_input.brText) { s_input.brText->Release(); }
    if (s_input.brTextDim) { s_input.brTextDim->Release(); }
    if (s_input.brTextFaint) { s_input.brTextFaint->Release(); }
    if (s_input.brAccent) { s_input.brAccent->Release(); }
    s_input.brPanel = s_input.brSelection = s_input.brBorder = s_input.brPanel2 = NULL;
    s_input.brText = s_input.brTextDim = s_input.brTextFaint = s_input.brAccent = NULL;
}

static void XjsInputFinish(bool ok) {
    XjsInputState& s = s_input;
    if (!s.hwnd) return;
    HWND owner = s.owner;
    int rid = s.resultId;
    /* 确定=回传文本 (空文本=清除别名的合法语义); 取消=NULL */
    std::wstring* out = ok ? new std::wstring(s.ed.text) : NULL;
    HWND h = s.hwnd;
    s.hwnd = NULL;   /* 同询问框: 销毁期 WM_ACTIVATE(WA_INACTIVE) 会重入本函数 (见 XjsAskFinish 注) */
    DestroyWindow(h);   /* WM_DESTROY 清 rt */
    PostMessageW(owner, WM_INPUT_DONE, (WPARAM)rid, (LPARAM)out);
}

static LRESULT CALLBACK Xjs_InputWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    XjsInputState& s = s_input;
    switch (msg) {
        case WM_PAINT: {
            /* 独立 hwnd 不过 Enter 绑定: 绑 owner 修 XSF 尺度; 颜色读打开时刻快照 */
            XjsWindowScope scope(XjsSearchWindow::Alive(s.ownerCtx) ? s.ownerCtx : XjsSearchWindow::Cur());
            if (!s.rt) {
                RECT rc; GetClientRect(hwnd, &rc);
                g_gfx->CreateWindowRt(hwnd, ximax(rc.right, 1), ximax(rc.bottom, 1), &s.rt);
                if (s.rt) {
                    s.rt->CreateSolidColorBrush(s.skin.panel, &s.brPanel);
                    s.rt->CreateSolidColorBrush(s.skin.borderStrong, &s.brBorder);
                    s.rt->CreateSolidColorBrush(s.skin.panel2, &s.brPanel2);
                    s.rt->CreateSolidColorBrush(s.skin.text, &s.brText);
                    s.rt->CreateSolidColorBrush(s.skin.textDim, &s.brTextDim);
                    s.rt->CreateSolidColorBrush(s.skin.textFaint, &s.brTextFaint);
                    s.rt->CreateSolidColorBrush(s.skin.accent, &s.brAccent);
                    s.rt->CreateSolidColorBrush(s.skin.accentSoft, &s.brSelection);
                }
            }
            if (!s.rt) break;
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            XjsSizeU sz = s.rt->GetPixelSize();
            float w = (float)sz.width, h = (float)sz.height;
            s.rt->BeginDraw();
            s.rt->FillRectangle(XjsRectF(0, 0, w, h), s.brPanel);
            s.rt->DrawRectangle(XjsRectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), s.brBorder, 1.0f);
            s.rt->DrawText(s.title.c_str(), (UINT32)s.title.length(), g_tfCardVal,
                XjsRectF(XSF(16), XSF(12), w - XSF(16), XSF(34)), s.brText);
            s.rt->DrawText(s.desc.c_str(), (UINT32)s.desc.length(), s.tfDesc ? s.tfDesc : g_tfTiny,
                XjsRectF(XSF(16), XSF(36), w - XSF(16), XSF(36) + s.descH), s.brTextDim);
            /* 输入框 (画刷必须来自对话框自己的 RT: 跨 RT 用主窗 g_br[] 会让 EndDraw
               返回 WRONG_RESOURCE_DOMAIN, 整帧丢弃 = 窗口全透明的根因) */
            XjsRoundedRect eb = XjsRoundedRectF(s.editBox, XSF(6), XSF(6));
            s.rt->FillRoundedRectangle(eb, s.brPanel2);
            s.rt->DrawRoundedRectangle(eb, s.brAccent, 1.2f);
            /* 文本/占位符/光标统一交给组件: 空文本同样有光标 (与搜索框/重命名框一致)。
               光标用文字色 (与各自底色对比; 白色在浅色输入框上看不见) */
            s.ed.Render(s.rt, s.editText, g_tfRow, s.brText, s.brSelection, s.brText,
                XjsT(L"对话框.别名留空提示"), s.brTextFaint, s.blink.On());
            /* 按钮 */
            auto btn = [&](const XjsRect& r, const wchar_t* t, int kind) {
                XjsRoundedRect rr = XjsRoundedRectF(r, XSF(6), XSF(6));
                s.rt->FillRoundedRectangle(rr, (s.hoverBtn == kind) ? s.brBorder : s.brPanel2);
                s.rt->DrawRoundedRectangle(rr, kind == 1 ? s.brAccent : s.brBorder, 1.0f);
                float tw = XjsMeasureText(t, g_tfMenu);
                s.rt->DrawText(t, (UINT32)wcslen(t), g_tfMenu,
                    XjsRectF((r.left + r.right) / 2 - tw / 2, r.top, (r.left + r.right) / 2 + tw / 2 + XSF(2), r.bottom),
                    kind == 1 ? (XjsBrush*)s.brAccent : (XjsBrush*)s.brTextDim);
            };
            btn(s.okBtn, XjsT(L"通用词.确定"), 1);
            btn(s.cancelBtn, XjsT(L"通用词.取消"), 2);
            HRESULT hr = s.rt->EndDraw();
            if (hr == (HRESULT)D2DERR_RECREATE_TARGET) {
                /* 设备丢失: RT+画刷全部释放下帧重建, 避免留下坏目标画不出内容 */
                s.rt->Release(); s.rt = NULL;
                XjsInputFreeBrushes();
                InvalidateRect(hwnd, NULL, FALSE);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (s.ed.dragging) { s.ed.MouseMove(pt, s.editText, g_tfRow); InvalidateRect(hwnd, NULL, FALSE); return 0; }
            int hb = XjsPtIn(s.okBtn, pt) ? 1 : (XjsPtIn(s.cancelBtn, pt) ? 2 : 0);
            if (hb != s.hoverBtn) { s.hoverBtn = hb; InvalidateRect(hwnd, NULL, FALSE); }
            return 0;   /* 光标统一走 WM_SETCURSOR */
        }
        case WM_LBUTTONDOWN: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (XjsPtIn(s.okBtn, pt)) { s.pressBtn = 1; return 0; }   /* 按钮: 记待定, 松开触发 */
            if (XjsPtIn(s.cancelBtn, pt)) { s.pressBtn = 2; return 0; }
            if (XjsPtIn(s.editBox, pt)) {
                s.ed.MouseDown(pt, s.editText, g_tfRow);
                SetCapture(hwnd);
                s.ed.UpdateImeAnchor(hwnd, s.editText, g_tfRow);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (s.ed.dragging) { s.ed.MouseUp(); ReleaseCapture(); }
            int press = s.pressBtn;
            s.pressBtn = 0;
            if (press == 1 && XjsPtIn(s.okBtn, pt)) XjsInputFinish(true);
            else if (press == 2 && XjsPtIn(s.cancelBtn, pt)) XjsInputFinish(false);
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (XjsPtIn(s.editBox, pt) && s.ed.MouseDoubleClick(pt, s.editText, g_tfRow))
                InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case WM_RBUTTONDOWN: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (XjsPtIn(s.editBox, pt)) {   /* 编辑框右键=编辑菜单 (共享菜单表, 同路由层字段) */
                std::vector<XjsPopupItem> items;
                XjsEditMenuAppendItems(s.ed, items);
                POINT sp = pt;
                ClientToScreen(hwnd, &sp);
                XjsShowPopupMenu(hwnd, sp, items, XSF(150));
            }
            return 0;
        }
        case WM_POPUP_RESULT:
            if ((int)wParam >= IDM_FCTX_BASE && (int)wParam < IDM_FCTX_BASE + 5) {
                XjsEditMenuApplyCmd(s.ed, (int)wParam - IDM_FCTX_BASE);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        case WM_KEYDOWN: {
            if (wParam == VK_RETURN) { XjsInputFinish(true); return 0; }
            if (wParam == VK_ESCAPE) { XjsInputFinish(false); return 0; }
            if (s.ed.Key(wParam)) {
                s.ed.EnsureCaretVisible(s.editText, g_tfRow);
                s.ed.UpdateImeAnchor(hwnd, s.editText, g_tfRow);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        case WM_CHAR: {
            if (s.ed.Char((wchar_t)wParam)) {
                s.ed.EnsureCaretVisible(s.editText, g_tfRow);
                s.ed.UpdateImeAnchor(hwnd, s.editText, g_tfRow);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        case WM_IME_CHAR:
            return 0;   /* 上屏结果走 WM_IME_COMPOSITION, 这条吞掉防双份 (搜索框同配方) */
        case WM_IME_COMPOSITION: {
            if (s.ed.ImeResult(hwnd, lParam)) {
                s.ed.EnsureCaretVisible(s.editText, g_tfRow);
                s.ed.UpdateImeAnchor(hwnd, s.editText, g_tfRow);
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            s.ed.UpdateImeAnchor(hwnd, s.editText, g_tfRow);
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_IME_STARTCOMPOSITION:
        case WM_IME_SETCONTEXT: {
            LRESULT r = DefWindowProcW(hwnd, msg, wParam, lParam);
            s.ed.UpdateImeAnchor(hwnd, s.editText, g_tfRow);
            return r;
        }
        case WM_SETCURSOR: {
            if (LOWORD(lParam) == HTCLIENT) {
                POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
                bool hand = XjsPtIn(s.okBtn, pt) || XjsPtIn(s.cancelBtn, pt);
                SetCursor(LoadCursorW(NULL, XjsPtIn(s.editBox, pt) ? IDC_IBEAM : (hand ? IDC_HAND : IDC_ARROW)));
                return TRUE;
            }
            break;
        }
        case WM_ACTIVATE:
            /* 点对话框以外=失焦取消 (源样式"遮罩点击取消"的弹窗等价口径);
               本方弹窗菜单抢前台不算失活 (XjsShowPopupMenu 会 SetForegroundWindow) —
               否则右键编辑菜单一弹出对话框就被取消, 菜单根本没法用 */
            if (LOWORD(wParam) == WA_INACTIVE && !XjsPopupMenuOpen()) XjsInputFinish(false);
            else if (LOWORD(wParam) != WA_INACTIVE) { SetFocus(hwnd); s.blink.SetWindowActive(hwnd, true); }   /* 弹窗自身激活 (独立窗不随主窗口径) */
            return 0;
        case WM_TIMER:
            if (wParam == ID_TIMER_CARET) XjsCaretBlink::TickWindow(hwnd);
            return 0;
        case WM_DESTROY: {
            XjsCaretBlink::DetachWindow(hwnd);   /* 退登记 + 停表, 之后窗口不再收 WM_TIMER */
            if (g_sysCaretMade) { DestroyCaret(); g_sysCaretMade = false; }   /* 光标挂在本窗口上, 随窗清理 */
            if (s.rt) { s.rt->Release(); s.rt = NULL; }
            XjsInputFreeBrushes();
            if (s.tfDesc) { s.tfDesc->Release(); s.tfDesc = NULL; }
            s.pressBtn = 0;
            s.hwnd = NULL;
            XjsInvalidateOverlayOwner(s.owner);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void XjsShowInputDialog(HWND owner, const wchar_t* title, const wchar_t* desc, const std::wstring& initial, int resultId) {
    if (!owner) owner = g_hWnd;   /* 无 owner 兜底主窗 (宏 Cur() 为空时 g_hWnd=NULL) */
    if (s_input.hwnd) { DestroyWindow(s_input.hwnd); s_input.hwnd = NULL; }
    /* 弹输入框前先让搜索框失焦: 否则搜索框仍持焦点, 会继续画自己的光标并跑闪烁计时器,
       表现为"焦点/输入在弹窗, 可见光标却留在搜索框"(关闭后主窗重新激活会自行聚焦搜索框) */
    XjsSearchFocus(false);
    static bool s_clsRegistered = false;
    if (!s_clsRegistered) {
        WNDCLASSEXW wc = {0};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS;   /* 缺它系统不发 WM_LBUTTONDBLCLK, 编辑框双击选词失效 */
        wc.lpfnWndProc = Xjs_InputWndProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.lpszClassName = L"XJS_InputDialog";
        RegisterClassExW(&wc);
        s_clsRegistered = true;
    }
    XjsInputState& s = s_input;
    s.owner = owner;
    s.ownerCtx = XjsSearchWindow::OfHwnd(owner);
    if (s.ownerCtx) s.ownerCtx->SyncSkin();
    s.skin = g_skin;
    s.resultId = resultId;
    s.title = title;
    s.desc = desc;
    s.pressBtn = 0;
    s.ed.SetText(initial, false);
    s.ed.caret = (int)initial.length();
    /* 说明文字格式: 字符级换行 + UNIFORM 行距 (CJK 行高红线) — 窗高随 desc 实测行数自适应,
       长说明不再被固定 18px 高的绘制带裁掉 (2026-09-17) */
    if (s.tfDesc) { s.tfDesc->Release(); s.tfDesc = NULL; }
    g_dw->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 11 * g_s * XjsUiZoom(), L"zh-cn", &s.tfDesc);
    if (s.tfDesc) {
        s.tfDesc->SetWordWrapping(XJS_WRAP_CHAR);
        s.tfDesc->SetParagraphAlignment(XJS_PARA_NEAR);
        s.tfDesc->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 16.5f * g_s * XjsUiZoom(), 11.2f * g_s * XjsUiZoom());
    }
    /* 尺寸/布局 (逻辑 px → XSF): 宽 380, 高按 desc 实测行数伸展; 居中于 owner 窗口 */
    float w = XSF(380);
    s.descH = XSF(18);   /* 空/短说明保底一行带 */
    if (s.tfDesc && !s.desc.empty()) {
        XjsTextLayout* lay = NULL;
        if (SUCCEEDED(g_dw->CreateTextLayout(s.desc.c_str(), (UINT32)s.desc.length(), s.tfDesc,
                                             w - XSF(32), 10000.0f, &lay)) && lay) {
            XjsTextMetrics tm = {};
            lay->GetMetrics(&tm);
            if (tm.height > s.descH) s.descH = tm.height;   /* UNIFORM 行距 ⇒ 高=行数×行距 */
            lay->Release();
        }
    }
    float editTop = XSF(36) + s.descH + XSF(6);
    float h = editTop + XSF(30) + XSF(26) + XSF(30) + XSF(12);
    RECT mr; GetWindowRect(owner, &mr);
    int x = mr.left + ((mr.right - mr.left) - (int)w) / 2;
    int y = mr.top + ((mr.bottom - mr.top) - (int)h) / 2;
    /* 内部几何按窗口原点 (像素) */
    float fw = w, fh = h;
    s.editBox = XjsRectF(XSF(16), editTop, fw - XSF(16), editTop + XSF(30));
    /* 内容区 (左内缩 9px 右 6px 上下 2px): 文本/光标/点选命中统一用这个矩形, 否则文字贴左边框 */
    s.editText = XjsRectF(s.editBox.left + XSF(9), s.editBox.top + XSF(2),
                             s.editBox.right - XSF(6), s.editBox.bottom - XSF(2));
    s.okBtn = XjsRectF(fw - XSF(96) - XSF(16), fh - XSF(42), fw - XSF(96) - XSF(16) + XSF(80), fh - XSF(12));
    s.cancelBtn = XjsRectF(fw - XSF(96) - XSF(16) - XSF(8) - XSF(80), fh - XSF(42), fw - XSF(96) - XSF(16) - XSF(8), fh - XSF(12));
    s.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"XJS_InputDialog", L"",
        WS_POPUP, x, y, (int)w, (int)h, owner, NULL, GetModuleHandleW(NULL), NULL);
    if (!s.hwnd) return;
    DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_ROUND;
    DwmSetWindowAttribute(s.hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
    /* 光标闪烁驱动器: 弹窗是独立顶层窗, 自己收 WM_ACTIVATE; 失活(点别处)时弹窗已取消销毁 */
    HWND hw = s.hwnd;
    s.blink.Attach(hw, [hw]() { InvalidateRect(hw, NULL, FALSE); });
    s.blink.Focus(true);
    ShowWindow(s.hwnd, SW_SHOW);
    SetForegroundWindow(s.hwnd);
    SetFocus(s.hwnd);
    s.ed.UpdateImeAnchor(s.hwnd, s.editText, g_tfRow);
    InvalidateRect(s.hwnd, NULL, FALSE);
    InvalidateRect(owner, NULL, FALSE);   /* owner 背景切钟罩虚化 (XjsModalBackdrop 下帧生效) */
}

/* ==================== 通用模态询问框 (JSON 按钮数组; 删除确认/多选打开/定位 等一切"询问"点共用) ====================
 * 独立顶层窗 (卡片居中浮于 owner), owner 背景钟罩虚化由 XjsModalBackdrop (main.cpp WM_PAINT) 承担,
 * 口径照源样式 .exit-mask: rgba(15,18,26,.45) + blur(6px)+saturate(1.1)。
 * XjsShowAskDialog 同步阻塞到关闭: 返回被点按钮下标; 点蒙层空白/Esc/失活 = -1。 */

struct XjsAskState {
    bool open = false;
    HWND hwnd = NULL, owner = NULL;
    XjsSearchWindow* ownerCtx = NULL;   /* owner 窗口上下文 (取色认亲不竞态, 同别名框) */
    XjsSkin skin{};                     /* 打开时刻 owner 皮肤快照 */
    std::wstring title, desc;
    struct Btn { std::wstring text; int style; };   /* 0=default 1=primary(accent实心) 2=danger(红描边,悬停红底) */
    std::vector<Btn> btns;
    std::vector<XjsRect> btnRects;
    float cardW = 0, cardH = 0, descH = 0, titleH = 0;   /* titleH: 标题实测高 (超宽换行, 窗高随动) */
    int hoverBtn = -1;
    int pressBtn = -1;   /* 按钮按下待定 (松开触发) */
    int result = -1;
    bool trackingLeave = false;
    XjsHwndRt* rt = NULL;
    XjsFormat *tfTitle = NULL, *tfDesc = NULL, *tfBtn = NULL;
    XjsSolidBrush *brPanel = NULL, *brBorder = NULL, *brPanel2 = NULL, *brText = NULL,
                         *brTextDim = NULL, *brAccent = NULL, *brWhite = NULL, *brErr = NULL, *brErrSoft = NULL;
};
static XjsAskState s_ask;

/* 纯色画刷是独立 COM 对象 (不随 RT 释放), 只置空 = 每次打开对话框泄漏一轮 (同设置窗画刷之坑);
   WM_DESTROY 与设备丢失重建两处同用 */
static void XjsAskFreeBrushes() {
    if (s_ask.brPanel) { s_ask.brPanel->Release(); }
    if (s_ask.brBorder) { s_ask.brBorder->Release(); }
    if (s_ask.brPanel2) { s_ask.brPanel2->Release(); }
    if (s_ask.brText) { s_ask.brText->Release(); }
    if (s_ask.brTextDim) { s_ask.brTextDim->Release(); }
    if (s_ask.brAccent) { s_ask.brAccent->Release(); }
    if (s_ask.brWhite) { s_ask.brWhite->Release(); }
    if (s_ask.brErr) { s_ask.brErr->Release(); }
    if (s_ask.brErrSoft) { s_ask.brErrSoft->Release(); }
    s_ask.brPanel = s_ask.brBorder = s_ask.brPanel2 = s_ask.brText = s_ask.brTextDim = NULL;
    s_ask.brAccent = s_ask.brWhite = s_ask.brErr = s_ask.brErrSoft = NULL;
}

bool XjsAskDialogOpen() { return s_ask.hwnd != NULL; }
bool XjsModalOverlayFor(HWND hwnd) {
    return (s_ask.hwnd && s_ask.owner == hwnd) || (s_input.hwnd && s_input.owner == hwnd);
}

static void XjsAskFinish(int idx) {
    if (!s_ask.hwnd) return;
    s_ask.result = idx;
    /* 先摘句柄再销毁: DestroyWindow 会同步回调本窗 WM_ACTIVATE(WA_INACTIVE) (前台让回 owner),
       不防则重入 XjsAskFinish(-1) 把刚收到的按钮下标覆盖成"取消" — 删除确认框点"删除"被吞
       (2026-09-17 实测: Finish idx=0 → WM_ACTIVATE low=0 → Finish idx=-1) */
    HWND h = s_ask.hwnd;
    s_ask.hwnd = NULL;
    DestroyWindow(h);   /* WM_DESTROY: open=false + 释放 RT/画刷 + owner 重绘 (钟罩撤) */
}

static LRESULT CALLBACK Xjs_AskWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    XjsAskState& s = s_ask;
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            XjsWindowScope scope(XjsSearchWindow::Alive(s.ownerCtx) ? s.ownerCtx : XjsSearchWindow::Cur());
            if (!s.rt) {
                RECT rc; GetClientRect(hwnd, &rc);
                g_gfx->CreateWindowRt(hwnd, ximax(rc.right, 1), ximax(rc.bottom, 1), &s.rt);
                if (s.rt) {
                    s.rt->CreateSolidColorBrush(s.skin.panel, &s.brPanel);
                    s.rt->CreateSolidColorBrush(s.skin.border, &s.brBorder);
                    s.rt->CreateSolidColorBrush(s.skin.panel2, &s.brPanel2);
                    s.rt->CreateSolidColorBrush(s.skin.text, &s.brText);
                    s.rt->CreateSolidColorBrush(s.skin.textDim, &s.brTextDim);
                    s.rt->CreateSolidColorBrush(s.skin.accent, &s.brAccent);
                    s.rt->CreateSolidColorBrush(XjsColorF(1, 1, 1), &s.brWhite);
                    s.rt->CreateSolidColorBrush(s.skin.err, &s.brErr);
                    XjsColor errSoft = s.skin.err; errSoft.a *= 0.08f;
                    s.rt->CreateSolidColorBrush(errSoft, &s.brErrSoft);
                }
            }
            if (s.rt) {
                RECT rc; GetClientRect(hwnd, &rc);
                float w = (float)rc.right, h = (float)rc.bottom;
                s.rt->BeginDraw();
                s.rt->Clear(s.skin.panel);   /* 窗口即卡片 (DWM 圆角), 边框 1px 内缩防剔除 */
                s.rt->DrawRectangle(XjsRectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), s.brBorder);
                s.rt->DrawText(s.title.c_str(), (UINT32)s.title.length(), s.tfTitle,
                    XjsRectF(XSF(24), XSF(20), w - XSF(24), XSF(20) + s.titleH), s.brText);
                s.rt->DrawText(s.desc.c_str(), (UINT32)s.desc.length(), s.tfDesc,
                    XjsRectF(XSF(24), XSF(20) + s.titleH + XSF(8), w - XSF(24),
                                XSF(20) + s.titleH + XSF(8) + s.descH), s.brTextDim);
                for (size_t i = 0; i < s.btns.size(); i++) {
                    const XjsRect& r = s.btnRects[i];
                    bool hov = (s.hoverBtn == (int)i);
                    if (s.btns[i].style == 1) {          /* primary: accent 实心 + 白字 */
                        s.rt->FillRoundedRectangle(XjsRoundedRectF(r, XSF(8), XSF(8)), s.brAccent);
                        s.rt->DrawText(s.btns[i].text.c_str(), (UINT32)s.btns[i].text.length(), s.tfBtn, r, s.brWhite);
                    } else if (s.btns[i].style == 2) {   /* danger: 红描边红字, 悬停红底白字 (源样式 .del-btn.danger) */
                        s.rt->FillRoundedRectangle(XjsRoundedRectF(r, XSF(8), XSF(8)), hov ? s.brErr : s.brErrSoft);
                        s.rt->DrawRoundedRectangle(XjsRoundedRectF(r, XSF(8), XSF(8)), s.brErr);
                        s.rt->DrawText(s.btns[i].text.c_str(), (UINT32)s.btns[i].text.length(), s.tfBtn, r, hov ? s.brWhite : s.brErr);
                    } else {                             /* default: panel-2 + 描边, 悬停描边加重 */
                        s.rt->FillRoundedRectangle(XjsRoundedRectF(r, XSF(8), XSF(8)), s.brPanel2);
                        s.rt->DrawRoundedRectangle(XjsRoundedRectF(r, XSF(8), XSF(8)), hov ? s.brTextDim : s.brBorder);
                        s.rt->DrawText(s.btns[i].text.c_str(), (UINT32)s.btns[i].text.length(), s.tfBtn, r, s.brText);
                    }
                }
                HRESULT hr = s.rt->EndDraw();
                if (hr == (HRESULT)D2DERR_RECREATE_TARGET) {
                    s.rt->Release(); s.rt = NULL;
                    XjsAskFreeBrushes();
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            int hb = -1;
            for (size_t i = 0; i < s.btnRects.size(); i++)
                if (XjsPtIn(s.btnRects[i], pt)) { hb = (int)i; break; }
            if (hb != s.hoverBtn) { s.hoverBtn = hb; InvalidateRect(hwnd, NULL, FALSE); }
            if (!s.trackingLeave) {
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                if (TrackMouseEvent(&tme)) s.trackingLeave = true;
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            s.trackingLeave = false;
            if (s.hoverBtn != -1) { s.hoverBtn = -1; InvalidateRect(hwnd, NULL, FALSE); }
            return 0;
        case WM_LBUTTONDOWN: {
            /* 按钮记待定 (松开触发口径): 松开仍命中同一按钮才生效, 拖离=取消 */
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            s.pressBtn = -1;
            for (size_t i = 0; i < s.btnRects.size(); i++)
                if (XjsPtIn(s.btnRects[i], pt)) { s.pressBtn = (int)i; }
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            int press = s.pressBtn;
            s.pressBtn = -1;
            if (press >= 0 && press < (int)s.btnRects.size() && XjsPtIn(s.btnRects[press], pt))
                XjsAskFinish(press);
            return 0;
        }
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) { XjsAskFinish(-1); return 0; }
            return 0;
        case WM_ACTIVATE: {
            /* 点蒙层空白(=owner 窗)/切别的程序 = 取消 (源样式"点遮罩空白处取消")。
               本方弹窗菜单若悬于其上须豁免 (同别名框口径, 虽然询问框目前不开菜单) */
            if (LOWORD(wParam) == WA_INACTIVE && !XjsPopupMenuOpen()) XjsAskFinish(-1);
            return 0;
        }
        case WM_DESTROY: {
            s.open = false;
            s.trackingLeave = false;
            s.pressBtn = -1;
            if (s.rt) { s.rt->Release(); s.rt = NULL; }
            XjsAskFreeBrushes();
            if (s.tfTitle) { s.tfTitle->Release(); s.tfTitle = NULL; }
            if (s.tfDesc) { s.tfDesc->Release(); s.tfDesc = NULL; }
            if (s.tfBtn) { s.tfBtn->Release(); s.tfBtn = NULL; }
            s.hwnd = NULL;
            XjsInvalidateOverlayOwner(s.owner);   /* owner 撤钟罩重绘 (曾用 Cur, 多窗下刷错窗) */
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int XjsShowAskDialog(HWND owner, const wchar_t* title, const wchar_t* desc, const char* buttonsJson) {
    if (s_ask.hwnd) XjsAskFinish(-1);
    XjsAskState& s = s_ask;
    /* JSON 按钮数组解析收口在 XjsParseDialogButtons (xjs_engine.cpp); 失败回退单"确定"已含在实现内 */
    s.btns.clear();
    XjsAskBtnDef btnDefs[4];
    int nb = XjsParseDialogButtons(buttonsJson, btnDefs, 4);
    for (int i = 0; i < nb; i++) s.btns.push_back({ btnDefs[i].text, btnDefs[i].style });
    s.owner = owner;
    s.ownerCtx = XjsSearchWindow::OfHwnd(owner);
    if (s.ownerCtx) s.ownerCtx->SyncSkin();
    s.skin = g_skin;
    s.title = title ? title : L"";
    s.desc = desc ? desc : L"";
    s.result = -1;
    s.hoverBtn = -1;
    s.pressBtn = -1;
    /* 文本格式 (owner 尺度: 调用点在 owner 窗口过程流程内, g_s 即 owner 尺度; 随 WM_DESTROY 释放) */
    g_dw->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 16 * g_s * XjsUiZoom(), L"zh-cn", &s.tfTitle);
    if (s.tfTitle) {
        s.tfTitle->SetWordWrapping(XJS_WRAP_CHAR);   /* 标题超宽换行 (窗高随实测行数自适应) */
        s.tfTitle->SetParagraphAlignment(XJS_PARA_CENTER);
        s.tfTitle->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 22 * g_s * XjsUiZoom(), 15 * g_s * XjsUiZoom());
    }
    g_dw->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 12.5f * g_s * XjsUiZoom(), L"zh-cn", &s.tfDesc);
    if (s.tfDesc) {
        /* 多行正文: 换行交给布局宽 + UNIFORM 行距 (CJK 行高红线), 源样式 .exit-sub line-height 1.5 */
        s.tfDesc->SetWordWrapping(XJS_WRAP_CHAR);
        s.tfDesc->SetParagraphAlignment(XJS_PARA_NEAR);
        s.tfDesc->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 18.8f * g_s * XjsUiZoom(), 14.1f * g_s * XjsUiZoom());
    }
    g_dw->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 12 * g_s * XjsUiZoom(), L"zh-cn", &s.tfBtn);
    if (s.tfBtn) {
        s.tfBtn->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        s.tfBtn->SetParagraphAlignment(XJS_PARA_CENTER);   /* 按钮文字垂直居中 (缺它=顶在按钮上缘) */
        s.tfBtn->SetWordWrapping(XJS_WRAP_NONE);
    }
    /* 卡片几何: 380 宽 (源样式 .exit-dialog), 标题/说明高按实测行数 + 按钮行; 居中于 owner */
    s.cardW = XSF(380);
    s.titleH = XSF(22);   /* 标题默认单行带; 超宽实测多行 */
    if (s.tfTitle && !s.title.empty()) {
        XjsTextLayout* lay = NULL;
        if (SUCCEEDED(g_dw->CreateTextLayout(s.title.c_str(), (UINT32)s.title.length(), s.tfTitle,
                                             s.cardW - XSF(48), 10000.0f, &lay)) && lay) {
            XjsTextMetrics tm = {};
            lay->GetMetrics(&tm);
            if (tm.height > s.titleH) s.titleH = tm.height;
            lay->Release();
        }
    }
    s.descH = 0;
    if (s.tfDesc && !s.desc.empty()) {
        XjsTextLayout* lay = NULL;
        if (SUCCEEDED(g_dw->CreateTextLayout(s.desc.c_str(), (UINT32)s.desc.length(), s.tfDesc,
                                             s.cardW - XSF(48), 10000.0f, &lay)) && lay) {
            XjsTextMetrics tm = {};
            lay->GetMetrics(&tm);
            s.descH = tm.height;   /* UNIFORM 行距 ⇒ 高=行数×行距, 多行不叠画 */
            lay->Release();
        }
    }
    /* 按钮行: 右对齐, 宽=文本+36 (源样式 .del-btn padding 6 18), 高 30, 间距 8 */
    float by = XSF(24) + s.titleH + XSF(8) + s.descH + XSF(18);
    float bx = s.cardW - XSF(24);
    s.btnRects.assign(s.btns.size(), XjsRectF(0, 0, 0, 0));
    for (int i = (int)s.btns.size() - 1; i >= 0; i--) {
        float bw = XjsMeasureText(s.btns[i].text.c_str(), s.tfBtn) + XSF(36);
        bx -= bw;
        s.btnRects[i] = XjsRectF(bx, by, bx + bw, by + XSF(30));
        bx -= XSF(8);
    }
    s.cardH = by + XSF(30) + XSF(24);
    RECT mr; GetWindowRect(owner, &mr);
    int x = mr.left + ((mr.right - mr.left) - (int)s.cardW) / 2;
    int y = mr.top + ((mr.bottom - mr.top) - (int)s.cardH) / 2;
    static bool s_clsRegistered = false;
    if (!s_clsRegistered) {
        WNDCLASSEXW wc = {0};
        wc.cbSize = sizeof(wc);   /* 缺它 RegisterClassExW 必败 → CreateWindow 失败 → 询问框秒返回 -1 */
        wc.lpfnWndProc = Xjs_AskWndProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.lpszClassName = L"XJS_AskDialog";
        RegisterClassExW(&wc);
        s_clsRegistered = true;
    }
    s.open = true;
    s.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"XJS_AskDialog", L"",
        WS_POPUP, x, y, (int)s.cardW, (int)s.cardH, owner, NULL, GetModuleHandleW(NULL), NULL);
    if (!s.hwnd) { s.open = false; return -1; }
    DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_ROUND;
    DwmSetWindowAttribute(s.hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
    ShowWindow(s.hwnd, SW_SHOW);
    SetForegroundWindow(s.hwnd);
    SetFocus(s.hwnd);
    InvalidateRect(owner, NULL, FALSE);   /* owner 背景切钟罩虚化 */
    InvalidateRect(s.hwnd, NULL, FALSE);
    /* 同步泵: 阻塞到关闭, 返回被点按钮下标 (点蒙层/Esc 由 WM_ACTIVATE/KEYDOWN 收尾)。
       泵派发的是全线程消息 — 其它搜索窗的定时器/重绘会经 WndProc 入口 Enter 把 Cur
       重绑到那些窗且不恢复, 泵返回后调用方的 g_* 宏就会解析到别的窗 (拿别窗结果集
       同下标路径打开文件)。作用域钉住入口窗, 泵毕恢复 */
    XjsWindowScope scope(XjsSearchWindow::Cur());
    MSG m;
    while (s.open && GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return s.result;
}
