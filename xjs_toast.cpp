/*
 * xjs_toast.cpp — Toast 通知组件 (源样式 System.css .toast 族 + 20-toast.js 照抄, 一文件一职责)
 * 右下角卡片栈: XjsToastShow(宿主, text, type) 弹出, 定时自动关闭, 非模态不阻塞操作。
 * 组件化: 按宿主 HWND 挂载 (任意窗口可复用 — 搜索窗/设置窗各自独立栈与画刷),
 * 宿主只需三处接线: WM_PAINT 尾调 XjsToastRender / WM_TIMER 透传 XjsToastTimerTick /
 * 鼠标消息先问 XjsToastMouseMove / XjsToastMouseDown。
 */
#include "xjs_app.h"

/* ==================== Toast 通知 (源样式 System.css .toast 族 + 20-toast.js 照抄) ====================
 * 右下角卡片栈: 右 16 / 底 46 (状态栏 36+10) / 卡间距 10, 末尾=最新=栈底, 上限 4 条移除最旧。
 * 卡片: panel 底 + 1px border + 圆角 12 + 左 3px 类型色条 + 底 2.5px 进度条 (线性收缩, .7 不透明)。
 * 进场 .24s cubic-bezier(.21,1.02,.55,1) 右滑 28px+scale(.96); 离场 .18s ease 反放; error 停 6s 其余 4s。
 * 源样式 box-shadow-md 不抄: Dome 其余浮层 (菜单/弹窗) 均为纯边框, 视觉一致且省一层效果图。 */

static double XjsToastNow() { return (double)GetTickCount64() / 1000.0; }

/* CSS cubic-bezier(x1,y1,x2,y2) 求值: 二分解 x→参数 u 再取 y(u) (toast 进出场缓动) */
static float XjsToastBezier(float t, float x1, float y1, float x2, float y2) {
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    auto bez = [&](float u, float a, float b) {
        float v = 1 - u;
        return 3 * v * v * u * a + 3 * v * u * u * b + u * u * u;
    };
    float lo = 0, hi = 1, u = t;
    for (int i = 24; i--;) {
        u = (lo + hi) / 2;
        if (bez(u, x1, x2) < t) lo = u; else hi = u;
    }
    return bez(u, y1, y2);
}

/* ==================== 宿主注册表 (每 HWND 一份栈+资源; 画刷建在宿主 RT 上) ====================
 * 跨 RT 用别窗画刷 = EndDraw WRONG_RESOURCE_DOMAIN 丢整帧 (别名窗口全透明的前车之鉴),
 * 故画刷缓存归宿主条目所有, RT 指针/皮肤纪元变化即整体重建。 */

struct XjsToastHost {
    std::vector<XjsToastItem> items;
    int hoverIdx = -1, hoverBtn = 0;              /* 悬停卡片下标 / 按钮 (1=复制 2=关闭) */
    XjsRt* rt = NULL;                 /* 画刷所属 RT (变化=宿主 RT 重建) */
    UINT epoch = 0;                               /* 建画刷时的皮肤纪元 (变化=换肤) */
    std::unordered_map<unsigned int, XjsSolidBrush*> brCache;   /* 颜色值 → 画刷 */
};
static std::unordered_map<HWND, XjsToastHost> s_hosts;

/* 当前绘制/命中上下文 (公开 API 先 Begin 再走内部逻辑, 内部不摸全局宏) */
static XjsToastHost* s_cur = NULL;
static XjsRt* s_rt = NULL;
static float s_w = 0, s_h = 0, s_scale = 1;

#define TS(v) ((float)(v) * s_scale)   /* Toast 尺度 (宿主 DPI×缩放, 原 XSF 同义) */

static void XjsToastBegin(HWND host, XjsRt* rt, float w, float h, float scale) {
    s_cur = &s_hosts[host];
    s_rt = rt;
    s_w = w;
    s_h = h;
    s_scale = scale > 0 ? scale : 1.0f;
    if (rt && (s_cur->rt != rt || s_cur->epoch != g_skinEpoch)) {
        for (auto& kv : s_cur->brCache) if (kv.second) kv.second->Release();
        s_cur->brCache.clear();
        s_cur->rt = rt;
        s_cur->epoch = g_skinEpoch;
    }
}

/* 临时画刷 (宿主缓存; 主题色/类型色/进度色同一通道) */
static XjsBrush* XjsToastBrush(XjsColor c) {
    unsigned int key = (unsigned int)((int)(c.a * 255.0f + 0.5f) << 24 |
                                      (int)(c.r * 255.0f + 0.5f) << 16 |
                                      (int)(c.g * 255.0f + 0.5f) << 8 |
                                      (int)(c.b * 255.0f + 0.5f));
    auto it = s_cur->brCache.find(key);
    if (it != s_cur->brCache.end()) return it->second;
    XjsSolidBrush* br = NULL;
    if (s_rt && SUCCEEDED(s_rt->CreateSolidColorBrush(c, &br)) && br)
        s_cur->brCache[key] = br;
    return br;
}

/* 类型主题色 (System.css 固定值; info 用皮肤 --accent) */
static XjsColor XjsToastTypeColor(int type) {
    switch (type) {
        case XTOAST_ERROR:   return XjsCol(0xef4444);
        case XTOAST_WARN:    return XjsCol(0xf59e0b);
        case XTOAST_SUCCESS: return XjsCol(0x10b981);
        default:             return g_skin.accent;
    }
}

/* 一张卡的命中/布局矩形 (最终位置, 不含动画偏移; 命中测试用) */
struct XjsToastCardRects {
    XjsRect card{}, copy{}, close{};
};

/* 图标中心定位: SVG viewBox 16/12 单位 → 18px 类型图标 / 11px 按钮小图标 */
static XjsPoint2 XjsToastIcPt(XjsPoint2 center, float vx, float vy, float box) {
    return XjsPoint2F(center.x + TS((vx - 8) * box / 16), center.y + TS((vy - 8) * box / 16));
}

/* 栈布局: 按停靠位分组独立成栈, 末尾(最新)贴锚点, 同位向内叠, 间距 10, 卡宽夹 [220,360]。
 * BOTTOM_RIGHT 底 46 = 源样式口径 (搜索窗状态栏让位), 其余停靠位 16 内边距 */
static void XjsToastLayout(XjsToastCardRects* out) {
    float lineH = TS(19.4f);
    for (int pos = 0; pos < XTOPOS_COUNT; pos++) {
        bool top = (pos == XTOPOS_TOP_RIGHT || pos == XTOPOS_TOP_LEFT || pos == XTOPOS_TOP_CENTER);
        bool center = (pos == XTOPOS_TOP_CENTER || pos == XTOPOS_BOTTOM_CENTER);
        bool right = (pos == XTOPOS_TOP_RIGHT || pos == XTOPOS_BOTTOM_RIGHT);
        float y = top ? TS(16) : s_h - (pos == XTOPOS_BOTTOM_RIGHT ? TS(46) : TS(16));
        for (size_t i = s_cur->items.size(); i-- > 0;) {
            XjsToastItem& t = s_cur->items[i];
            if (t.pos != pos) continue;
            float natural = XjsMeasureText(t.text.c_str(), g_tfToast);
            float cw = TS(112) + natural;                     /* padL14+icon18+gap10+gap10+copy20+gap10+close20+padR10 */
            if (cw < TS(220)) cw = TS(220);
            if (cw > TS(360)) cw = TS(360);
            float textW = cw - TS(112);
            float lines = 1;
            if (natural > textW) {
                XjsTextLayout* lay = NULL;
                if (SUCCEEDED(g_dw->CreateTextLayout(t.text.c_str(), (UINT32)t.text.length(), g_tfToast, textW, 10000.0f, &lay)) && lay) {
                    XjsTextMetrics tm;
                    lay->GetMetrics(&tm);
                    lay->Release();
                    lines = (int)(tm.height / lineH + 0.05f); /* 统一行高: 高度=行数×lineH */
                    if (lines < 1) lines = 1;
                }
            }
            float ch = TS(11) + lines * lineH + TS(14);
            float x1 = center ? (s_w - cw) / 2 : (right ? s_w - TS(16) - cw : TS(16));
            float y1;
            if (top) { y1 = y; y += ch + TS(10); }
            else     { y1 = y - ch; y = y1 - TS(10); }
            out[i].card = XjsRectF(x1, y1, x1 + cw, y1 + ch);
            out[i].close = XjsRectF(x1 + cw - TS(10) - TS(20), y1 + TS(11), x1 + cw - TS(10), y1 + TS(11) + TS(20));
            out[i].copy = XjsRectF(out[i].close.left - TS(10) - TS(20), out[i].close.top, out[i].close.left - TS(10), out[i].close.bottom);
        }
    }
}

/* 卡片并集 (失效区域; pad 30 = 进场右滑 28px 摆动余量) */
static bool XjsToastUnionRect(RECT* r) {
    if (s_cur->items.empty()) return false;
    XjsToastCardRects cards[XJS_TOAST_MAX];
    XjsToastLayout(cards);
    XjsRect u = cards[0].card;
    for (size_t i = 1; i < s_cur->items.size(); i++) {
        u.left = xf_min(u.left, cards[i].card.left);   u.top = xf_min(u.top, cards[i].card.top);
        u.right = xf_max(u.right, cards[i].card.right); u.bottom = xf_max(u.bottom, cards[i].card.bottom);
    }
    u.left -= TS(30); u.top -= TS(4); u.right += TS(4); u.bottom += TS(4);
    *r = RECT{ (LONG)u.left, (LONG)u.top, (LONG)u.right + 1, (LONG)u.bottom + 1 };
    return true;
}

/* 类型图标 18px: 照源样式 TOAST_ICONS SVG 逐路径描边 (16 单位 viewBox) */
static void XjsToastDrawIcon(XjsPoint2 center, int type, XjsBrush* br, XjsStroke* ss) {
    auto P = [&](float x, float y) { return XjsToastIcPt(center, x, y, 18); };
    float w = TS(1.4f * 18 / 16), wChk = TS(1.5f * 18 / 16);
    auto seg = [&](float x1, float y1, float x2, float y2, float sw) {
        s_rt->DrawLine(P(x1, y1), P(x2, y2), br, sw, ss);
    };
    switch (type) {
        case XTOAST_ERROR:   /* 圆+X */
            s_rt->DrawEllipse(XjsEllipseF(P(8, 8), TS(6.5f), TS(6.5f)), br, w, ss);
            seg(5.8f, 5.8f, 10.2f, 10.2f, w); seg(10.2f, 5.8f, 5.8f, 10.2f, w);
            break;
        case XTOAST_WARN: {  /* 三角+! */
            seg(8, 2, 1.8f, 13, w); seg(1.8f, 13, 14.2f, 13, w); seg(14.2f, 13, 8, 2, w);
            seg(8, 6.2f, 8, 9.2f, w); seg(8, 11.6f, 8, 11.9f, w);
            break;
        }
        case XTOAST_SUCCESS: /* 圆+对勾 */
            s_rt->DrawEllipse(XjsEllipseF(P(8, 8), TS(6.5f), TS(6.5f)), br, w, ss);
            seg(5, 8.3f, 7.2f, 10.5f, wChk); seg(7.2f, 10.5f, 11, 6, wChk);
            break;
        default:             /* 圆+i */
            s_rt->DrawEllipse(XjsEllipseF(P(8, 8), TS(6.5f), TS(6.5f)), br, w, ss);
            seg(8, 7.4f, 8, 11, w); seg(8, 5.2f, 8, 5.5f, w);
            break;
    }
}

/* 按钮小图标 11px (复制/对勾/关闭, 12 单位 viewBox, 照源样式按钮 SVG) */
static void XjsToastDrawBtnIcon(XjsPoint2 center, int kind, XjsBrush* br, XjsStroke* ss) {
    auto P = [&](float x, float y) { return XjsPoint2F(center.x + TS((x - 6) * 11 / 12), center.y + TS((y - 6) * 11 / 12)); };
    float w = TS(1.3f * 11 / 12), wX = TS(1.5f * 11 / 12), wD = TS(1.8f * 11 / 12);
    auto seg = [&](float x1, float y1, float x2, float y2, float sw) {
        s_rt->DrawLine(P(x1, y1), P(x2, y2), br, sw, ss);
    };
    if (kind == 1) {           /* 复制: 前页圆角矩形+后页 L 形 */
        XjsRoundedRect rr = XjsRoundedRectF(XjsRectF(P(3.6f, 3.6f).x, P(3.6f, 3.6f).y, P(9.2f, 9.2f).x, P(9.2f, 9.2f).y), TS(1), TS(1));
        s_rt->DrawRoundedRectangle(rr, br, w, ss);
        seg(8.6f, 2, 3.1f, 2, w); seg(3.1f, 2, 2, 3.1f, w); seg(2, 3.1f, 2, 8.6f, w);
    } else if (kind == 2) {    /* 已复制: 绿对勾 */
        seg(2.5f, 6.5f, 4.9f, 8.9f, wD); seg(4.9f, 8.9f, 9.5f, 3.5f, wD);
    } else {                   /* 关闭 X */
        seg(3, 3, 9, 9, wX); seg(9, 3, 3, 9, wX);
    }
}

/* 矩形 ∩ 卡片圆角几何 求交填充: 色条/进度条端头严格跟随圆角曲线 (后端原语, 不依赖透明度组) */
static void XjsToastFillClipped(float l, float t, float r, float b,
                                XjsGeo* cardGeo, XjsBrush* br) {
    if (!cardGeo || !br || r <= l || b <= t) return;
    s_rt->FillGeoIntersectRect(cardGeo, XjsRectF(l, t, r, b), br);
}

void XjsToastRender(HWND host, XjsRt* rt, float w, float h, float scale) {
    XjsToastBegin(host, rt, w, h, scale);
    if (s_cur->items.empty() || !s_rt) return;
    static XjsStroke* ssRound = NULL;   /* 描边圆头 (SVG stroke-linecap:round), 设备无关建一次 */
    if (!ssRound) g_gfx->RoundStroke(&ssRound);
    XjsToastCardRects cards[XJS_TOAST_MAX];
    XjsToastLayout(cards);
    double now = XjsToastNow();
    for (size_t i = 0; i < s_cur->items.size(); i++) {
        XjsToastItem& t = s_cur->items[i];
        XjsRect c = cards[i].card;
        /* 进/离场: 进 .24s bezier(.21,1.02,.55,1) 由 右28+scale.96+透明 → 就位; 离 .18s ease 反向 */
        float e, alpha;
        if (t.leaving) {
            e = XjsToastBezier(xf_min((float)((now - t.leaveStart) / 0.18), 1), 0.25f, 0.1f, 0.25f, 1);
            alpha = 1 - e;
        } else {
            e = XjsToastBezier(xf_min((float)((now - t.start) / 0.24), 1), 0.21f, 1.02f, 0.55f, 1);
            alpha = e;
        }
        float dx = TS(28) * (1 - e), sc = 0.96f + 0.04f * e;
        float cxm = (c.left + c.right) / 2, cym = (c.top + c.bottom) / 2;
        XjsRect a = XjsRectF((c.left - cxm) * sc + cxm + dx, (c.top - cym) * sc + cym,
                                    (c.right - cxm) * sc + cxm + dx, (c.bottom - cym) * sc + cym);
        XjsColor tc = XjsToastTypeColor(t.type);
        /* 卡片圆角几何: 左色条/进度条经求交裁进圆角 (CSS overflow:hidden 同义)。
           不用透明度组做裁剪 — 求交先于整卡透明度, 条带形状不受影响 */
        XjsGeo* cardGeo = NULL;
        s_rt->CreateRoundRectGeo(XjsRoundedRectF(a, TS(12) * sc, TS(12) * sc), &cardGeo);
        /* 整卡透明度动画 (进/离场): 后端透明度组 (透明度不影响条带形状, 条带已在上面求交) */
        if (alpha < 1.0f) s_rt->PushAlpha(a, alpha);
        s_rt->FillRoundedRectangle(XjsRoundedRectF(a, TS(12), TS(12)), XjsToastBrush(g_skin.panel));
        s_rt->DrawRoundedRectangle(XjsRoundedRectF(a, TS(12), TS(12)), XjsToastBrush(g_skin.border), 1.0f);
        /* 左 3px 类型色条: 与卡片圆角求交 (同 mask 曲线, 端头跟随圆角) */
        XjsToastFillClipped(a.left, a.top, a.left + TS(3), a.bottom, cardGeo, XjsToastBrush(tc));
        /* 类型图标 (top 11 + 1) */
        XjsToastDrawIcon(XjsPoint2F(a.left + TS(14 + 9), a.top + TS(12 + 9)), t.type, XjsToastBrush(tc), ssRound);
        /* 正文 (left 14+18+10, right 让开 padR10+close20+gap10+copy20+gap10) */
        s_rt->DrawText(t.text.c_str(), (UINT32)t.text.length(), g_tfToast,
            XjsRectF(a.left + TS(42), a.top + TS(11), a.right - TS(70), a.bottom), XjsToastBrush(g_skin.text));
        /* 复制/关闭按钮 (top 11, 20px, hover=rowHover 底 + 图标提亮); 动画中按钮跟卡走 */
        XjsRect bc = cards[i].copy, xb = cards[i].close;
        bc.left += dx; bc.right += dx; bc.top = (bc.top - cym) * sc + cym; bc.bottom = (bc.bottom - cym) * sc + cym;
        xb.left += dx; xb.right += dx; xb.top = (xb.top - cym) * sc + cym; xb.bottom = (xb.bottom - cym) * sc + cym;
        bool hovCopy = ((int)i == s_cur->hoverIdx && s_cur->hoverBtn == 1);
        bool hovClose = ((int)i == s_cur->hoverIdx && s_cur->hoverBtn == 2);
        if (hovCopy) s_rt->FillRoundedRectangle(XjsRoundedRectF(bc, TS(5), TS(5)), XjsToastBrush(g_skin.rowHover));
        if (hovClose) s_rt->FillRoundedRectangle(XjsRoundedRectF(xb, TS(5), TS(5)), XjsToastBrush(g_skin.rowHover));
        XjsPoint2 bcm = XjsPoint2F((bc.left + bc.right) / 2, (bc.top + bc.bottom) / 2);
        XjsPoint2 xbm = XjsPoint2F((xb.left + xb.right) / 2, (xb.top + xb.bottom) / 2);
        if (t.copied && now - t.copiedAt < 1.2)
            XjsToastDrawBtnIcon(bcm, 2, XjsToastBrush(XjsCol(0x10b981)), ssRound);
        else
            XjsToastDrawBtnIcon(bcm, 1, XjsToastBrush(hovCopy ? g_skin.text : g_skin.textFaint), ssRound);
        XjsToastDrawBtnIcon(xbm, 0, XjsToastBrush(hovClose ? g_skin.text : g_skin.textFaint), ssRound);
        /* 底部进度条 2.5px 线性收缩 (opacity .7), 左端跟随卡片圆角; 离场冻结在离场时刻的剩余量 */
        float used = (float)((t.leaving ? t.leaveStart : now) - t.start);
        float frac = 1 - used / t.duration;
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        XjsColor pc = tc; pc.a *= 0.7f;
        XjsToastFillClipped(a.left, a.bottom - TS(2.5f), a.left + (a.right - a.left) * frac, a.bottom,
                            cardGeo, XjsToastBrush(pc));
        if (alpha < 1.0f) s_rt->PopAlpha();
        if (cardGeo) cardGeo->Release();
    }
}

/* 命中: 返回卡片下标与按钮 (0=卡体 1=复制 2=关闭); 未命中 -1 */
static int XjsToastHit(POINT pt, int* btnOut) {
    *btnOut = 0;
    if (s_cur->items.empty()) return -1;
    XjsToastCardRects cards[XJS_TOAST_MAX];
    XjsToastLayout(cards);
    for (size_t i = 0; i < s_cur->items.size(); i++) {
        if (!XjsPtIn(cards[i].card, pt)) continue;
        *btnOut = XjsPtIn(cards[i].copy, pt) ? 1 : XjsPtIn(cards[i].close, pt) ? 2 : 0;
        return (int)i;
    }
    return -1;
}

bool XjsToastMouseMove(HWND host, float scale, float w, float h, const POINT& pt) {
    XjsToastBegin(host, NULL, w, h, scale);
    int btn = 0;
    int idx = XjsToastHit(pt, &btn);
    if (idx != s_cur->hoverIdx || btn != s_cur->hoverBtn) {
        RECT r;
        if (XjsToastUnionRect(&r)) InvalidateRect(host, &r, FALSE);
        s_cur->hoverIdx = idx;
        s_cur->hoverBtn = btn;
    }
    return idx >= 0;
}

bool XjsToastMouseDown(HWND host, float scale, float w, float h, const POINT& pt) {
    XjsToastBegin(host, NULL, w, h, scale);
    int btn = 0;
    int idx = XjsToastHit(pt, &btn);
    /* 卡片吃点击防穿透 (源样式 pointer-events:auto); 复制/关闭按钮走松开触发 (命令类口径),
       松开时重新命中同一按钮即生效, 由 XjsToastMouseUp 执行 */
    return idx >= 0;
}

void XjsToastMouseUp(HWND host, float scale, float w, float h, const POINT& pt) {
    XjsToastBegin(host, NULL, w, h, scale);
    int btn = 0;
    int idx = XjsToastHit(pt, &btn);
    if (idx < 0 || btn == 0) return;
    XjsToastItem& t = s_cur->items[idx];
    double now = XjsToastNow();
    if (btn == 2 && !t.leaving) {                   /* 关闭: 免等到期直接离场 (源样式 clearTimeout+dismiss) */
        t.leaving = true;
        t.leaveStart = now;
    } else if (btn == 1 && !t.copied) {             /* 复制消息: 拷贝全文, 图标变绿对勾 1.2s */
        XjsCopyClipboard(t.text);
        t.copied = true;
        t.copiedAt = now;
    }
    RECT r;
    if (XjsToastUnionRect(&r)) InvalidateRect(host, &r, FALSE);
}

bool XjsToastTimerTick(HWND host, float scale) {
    auto it = s_hosts.find(host);
    if (it == s_hosts.end()) return false;
    /* 布局需要宿主尺寸: 取客户区 (tick 只做动画推进+失效, 不画; rt=NULL = 不动画刷缓存) */
    RECT crc;
    GetClientRect(host, &crc);
    XjsToastBegin(host, NULL, (float)crc.right, (float)crc.bottom, scale);
    if (s_cur->items.empty()) return false;
    double now = XjsToastNow();
    RECT r;
    bool hasUnion = XjsToastUnionRect(&r);        /* 移除前取并集: 被移除卡的残影也要抹 */
    for (size_t i = 0; i < s_cur->items.size(); i++) {
        XjsToastItem& t = s_cur->items[i];
        if (!t.leaving && now - t.start >= t.duration) {
            t.leaving = true;
            t.leaveStart = now;
        }
    }
    /* 离场完成 (0.18s 动画 + 一帧余量) 才移除 */
    for (size_t i = s_cur->items.size(); i-- > 0;) {
        if (s_cur->items[i].leaving && now - s_cur->items[i].leaveStart >= 0.19) s_cur->items.erase(s_cur->items.begin() + i);
    }
    if (hasUnion) InvalidateRect(host, &r, FALSE);
    return !s_cur->items.empty();
}

/* 宿主窗口销毁: 摘除注册表条目 (释放其画刷; 条目悬挂的 hwnd 不会再被复用) */
void XjsToastDetach(HWND host) {
    auto it = s_hosts.find(host);
    if (it == s_hosts.end()) return;
    for (auto& kv : it->second.brCache) if (kv.second) kv.second->Release();
    s_hosts.erase(it);
}

/* 设备重建 (自愈网/RECREATE_TARGET): 全部宿主的画刷都绑在死 RT 域上 —
   Begin 的"同包装指针/纪元"判定感知不到包装内原生 RT 已换, 必须显式作废 (死域画刷 = 丢帧循环) */
void XjsToastDropBrushCache() {
    for (auto& kv : s_hosts) {
        for (auto& b : kv.second.brCache) if (b.second) b.second->Release();
        kv.second.brCache.clear();
        kv.second.rt = NULL;   /* 强制下次 Begin 整体重建画刷 */
    }
}

/* 鼠标离开宿主: 清悬停态 (WM_MOUSELEAVE) */
void XjsToastHoverReset(HWND host) {
    auto it = s_hosts.find(host);
    if (it == s_hosts.end()) return;
    it->second.hoverIdx = -1;
    it->second.hoverBtn = 0;
}

void XjsToastShow(HWND host, const wchar_t* text, int type, float scale,
                  int pos, float durationSec) {
    if (!host || !text || !*text) return;
    XjsToastHost& hst = s_hosts[host];
    XjsToastItem t;
    t.text = text;
    t.type = type;
    t.pos = (pos >= 0 && pos < XTOPOS_COUNT) ? pos : XTOPOS_BOTTOM_RIGHT;
    t.start = XjsToastNow();
    t.duration = durationSec > 0 ? durationSec : (type == XTOAST_ERROR ? 6.0f : 4.0f);
    hst.items.push_back(t);
    while ((int)hst.items.size() > XJS_TOAST_MAX) hst.items.erase(hst.items.begin());   /* 上限 4 条, 移除最旧 (源样式同款) */
    SetTimer(host, ID_TIMER_TOAST, 30, NULL);     /* 进度条连续收缩 + 进出场动画驱动 */
    RECT crc;
    GetClientRect(host, &crc);
    XjsToastBegin(host, NULL, (float)crc.right, (float)crc.bottom, scale);
    RECT r;
    if (XjsToastUnionRect(&r)) InvalidateRect(host, &r, FALSE);
}
