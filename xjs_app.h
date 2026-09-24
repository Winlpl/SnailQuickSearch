/*
 * xjs_app.h — 蜗牛快搜 (D2D 演示宿主, 引擎为 xunjieso.dll) 全局状态与共享定义 (所有模块的唯一公共头)
 *
 * 模块划分 (一文件一职责, 提高复用):
 *   xjs_app     全局状态定义 + 每窗上下文注册表 (XjsSearchWindow 实例表)
 *   xjs_util    工具: 编码/时间/数字/剪贴板/文件操作(重命名/别名/OLE 拖出)/提权/自启动
 *   xjs_d2d     D2D 后端 (XjsGfxApi 主实现): 设备/画刷/文本格式/图片解码/通用绘制
 *   xjs_gdiplus GDI+ 后端 (XjsGfxApi 第二实现): DIB 画布, 设置-通用"绘制引擎"切换
 *   xjs_engine  引擎封装: 回调/搜索/筛选/行数据缓存(含驱动器信息)/图标缓存/历史导航/配置
 *   xjs_popup   自绘弹出菜单 (LL 钩子点外关闭) + 共享输入框组件 XjsLineEdit/输入框/询问框
 *   xjs_chrome  标题栏: 图标/☰菜单/搜索框(模式按钮+清空+历史)/筛选下拉/窗口按钮/状态栏
 *   xjs_list    列表: 表头/行(驱动器行+时间徽章+容量条)/滚动条/选择/框选/行内重命名
 *   xjs_preview 预览面板: 驱动器信息卡/文件信息/图片预览/查找大目录大文件
 *   xjs_md      Markdown 引擎 (md4c 解析 + 后端排版绘制, 设置内置文档页与 .md 预览共用)
 *   xjs_toast   Toast 通知组件 (按宿主 HWND 挂载, 搜索窗/设置窗各自独立栈)
 *   xjs_settings 设置窗口 (独立顶层窗, 左侧两层分类树 + 右侧行模型)
 *   main        窗口过程编排 + 入口
 */
#pragma once

#define NODRAWTEXT      /* 禁用 user32 DrawText 宏, 避免与 D2D DrawText 冲突 */
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#undef CreateWindowEx
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wincodec.h>
#include <dwmapi.h>
#include <imm.h>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>

#include "xunjieso.h"   /* 引擎 SDK 头, 必须在本目录 (仓库根), 禁止 ..\ 引用仓库外文件 (2026-09-19 用户口径) */
/* picojson 只允许 xjs_engine.cpp include (XjsConfig 门面的实现细节):
   换 JSON 模块 = 只改 xjs_engine.cpp, 本头文件与全部 UI 模块零感知 (2026-09-17) */

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "imm32.lib")

/* ==================== 常量 ==================== */
#define ID_TIMER_STATUS     1
#define ID_TIMER_ANIM       3
#define ID_TIMER_CARET      4
#define ID_TIMER_SEARCHSTATUS 5   /* "正在搜索…"延时显示 (200ms 未完成才显示, 防状态栏闪烁) */
#define ID_TIMER_TOAST      6   /* Toast 卡片进度条/进出场动画驱动 (30ms, 有活动 toast 才挂) */
#define ID_TIMER_SYNCWATCH  7   /* 文件同步变化轮询 (源样式 线程时钟 移植): 100ms 比对结果数量, 变了才节流刷新 */
#define ID_TIMER_HOVERFADE  8   /* 列表悬停高亮渐隐拖尾驱动 (30ms, 有衰减中的行才挂) */
#define ID_TIMER_HOSTEDSRC  9   /* 多来源标签悬停 180ms 后弹"切换搜索来源"菜单 (一次性) */
#define ID_TIMER_SETLIVE    10  /* 设置窗 内存/性能分析页 实时数据 1s 刷新 (仅这两分类重建行模型, 其余分类空转) */
#define ID_TIMER_SBTRACK    11  /* 滚动条轨道按住连发翻页 (首延 400ms, 之后 150ms/步; thumb 到指针即停) */
#define XJS_MARQUEE_PV_MS   60  /* 框选拖动中预览重载最小间隔 (时间戳节流, 同单击打开的防重口径; 完全实时=逐行读盘/解码会拖垮帧率) */
#define XJS_SYNC_POLL_MS    100   /* 同步轮询周期 (源样式 m_线程时钟.时钟周期=100) */
#define XJS_SYNC_REFRESH_MS 200   /* 真实时钟最小刷新间隔: 距上次实际刷新不足则顺延一拍 (同步风暴时刷新率恒有上限) */
#define ID_HOTKEY_SHOW      1     /* 全局快捷键注册基址: id = 本值+档案槽 (登记在主窗 hwnd) */
#define ID_TRAY_ICON        2

#define WM_SCAN_PROGRESS    (WM_USER + 100)
#define WM_SCAN_COMPLETE    (WM_USER + 101)
#define WM_SEARCH_COMPLETE  (WM_USER + 102)
#define WM_SCAN_DRIVE       (WM_USER + 103)
#define WM_ICON_READY       (WM_USER + 104)
#define WM_LOAD_COMPLETE    (WM_USER + 105)
#define WM_TRAY_NOTIFY      (WM_USER + 106)
#define WM_SEARCH_FAILED    (WM_USER + 107)
#define WM_INPUT_DONE       (WM_USER + 108)   /* 输入对话框收尾: wParam=结果id, lParam=new std::wstring*(取消=NULL) */
#define WM_DOUBLE_CTRL      (WM_APP + 2)      /* 双击 Ctrl 全局唤起 (钩子线程 → 主窗) */
#define WM_POPUP_RESULT     (WM_APP + 1)
#define WM_HOTKEY_WARN      (WM_APP + 3)      /* 全局热键注册失败提醒 (WM_CREATE 时窗未显示, 延后弹 toast) */
#define WM_WAKEUP           (WM_APP + 4)      /* 第二实例唤起本窗恢复显示 (跨完整性级别唯一放行通道, 见 wWinMain 单实例守卫) */
#define WM_PANEL_RESYNC     (WM_APP + 5)      /* 插件面板接管: 绘制帧发现尺寸/位置失配, 投到消息循环做世代同步 (回调禁在 WM_PAINT 内) */

/* 面板接管事件类型 (镜像 xjs_plugin_sdk.h 的 XJS_PANEL_*; SDK 头只有 xjs_plugin.cpp include,
   宿主路由走这套同名值 — xjs_plugin.cpp 内 static_assert 逐项对值, 禁止单边改号) */
enum { XJS_HPANEL_OPEN = 1, XJS_HPANEL_CLOSE = 2, XJS_HPANEL_RESIZE = 3, XJS_HPANEL_MOUSE_MOVE = 4,
       XJS_HPANEL_LDOWN = 5, XJS_HPANEL_LUP = 6, XJS_HPANEL_RDOWN = 7, XJS_HPANEL_RUP = 8,
       XJS_HPANEL_DBLCLK = 9, XJS_HPANEL_WHEEL = 10, XJS_HPANEL_KEY_DOWN = 11, XJS_HPANEL_KEY_CHAR = 12,
       XJS_HPANEL_FOCUS = 13, XJS_HPANEL_CAPTURE_LOST = 14, XJS_HPANEL_KEY_BLUR = 15 };

/* ===== 跨线程 PostMessage 闸门 =====
 * 主线程被长操作卡住时 (D2D 慢帧/模态同步泵/磁盘迟滞), 引擎回调线程的 UI 通知会无限堆积,
 * 解卡后洪泛重绘。post 前查未处理计数: 达上限 = 静默丢弃 (回调内堆分配就地释放),
 * 消息处理分支末尾递减。上限只此一处定义, 调用点禁止写死数字。 */
constexpr long XJS_POST_QUEUE_LIMIT = 20000;
class XjsSearchWindow;   /* 前置声明: 本节声明在类定义之前 */
bool XjsPostToUi(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);   /* 计数入队 post; 队满=返回 false (未投递) */
void XjsPostToUiDone();                                        /* 引擎消息处理完毕递减 (每条恰好一次) */
void XjsPostToUiDropWindow(XjsSearchWindow* w);                /* 窗口销毁: 返还该窗已投未处理消息的闸门计数 */
bool XjsPostUiOwnedString(XjsSearchWindow* w, UINT msg, LPARAM lp, std::string* payload);  /* 堆载荷投递 (销毁兜底释放) */
std::string* XjsUiOwnedStringTake(std::string* payload);       /* 消费点摘登记并接管所有权 (调用方 delete) */
void XjsUiOwnedStringsDropHwnd(HWND hwnd);                     /* 窗口销毁: 释放该窗名下未消费的登记载荷 */

/* 崩溃取证阶段标记 (定义在 main.cpp; VEH 落盘 startup_stack.txt 首行 phase=)。
   插件扫描/加载/回调入口也标阶段 (plugin-scan / plugin-load:<id> / plugin-call:<id>) */
void XjsSetPhase(const wchar_t* p);

/* 命令 id 段 (自绘菜单/按钮回传, 各段宽见括注; 分发唯一入口 = XjsOnPopupResult,
   判定顺序有讲究 — 带高位标志的先剥标志, 无上界的 IDM_MENU_BASE 段必须排在最后) */
#define IDM_CTX_BASE        1000   /* 文件右键: +1..8 单选组 / +9 重命名 +10 别名 +11 多选打开 +12 多选定位;
                                      +20..24 搜索框右键编辑; +30 别名对话框; +40..44 重命名框右键编辑 */
#define IDM_MODE_BASE       2000   /* +0..4 关键词模式 (通配符/正则/SQL/Lua 过滤/Lua 执行) */
#define IDM_CMODE_BASE      2500   /* 用户自定义搜索模式菜单项 (+下标, 上限100) */
#define IDM_HISTORY_BASE    3000   /* +0..998 历史项; +999 清空历史 */
#define IDM_FILTER_BASE     4000   /* +筛选分类下标 (XjsApplyFilter) */
#define IDM_MENU_BASE       5000   /* ☰菜单: +46 新窗口 +50 设置 +51 捐赠(开设置直达捐赠页) +80..143 窗口启动器 (档案槽) */
#define IDM_COL_BASE        6000   /* 表头列显隐菜单: +fullIdx */
#define IDM_HOSTED_SRC_BASE 7000   /* 托管标签"切换搜索来源"菜单: +来源下标 */
#define IDM_TRAY_BASE       8000   /* 托盘右键菜单: +1恢复窗口 +2设置 +4捐赠(开设置直达捐赠页) +3退出程序 */
#define IDM_FCTX_BASE       9000   /* 输入字段右键编辑菜单 (路由层通用): +0剪切 +1复制 +2粘贴 +3全选 +4删除 */
#define IDM_PLUGIN_BASE     10000  /* 插件菜单/状态栏项 (票号表下标, 上限200; 回传 → XjsPluginOnMenuTicket) */

/* 悬停位 (标题栏搜索框内) */
enum HoverBits { HB_CLEAR = 1, HB_HISTORY = 2, HB_FILTER = 4, HB_PILL = 8, HB_MENU = 16 };

/* 标题栏窗口按钮 (2026-09-17 移除右侧 ≡ 菜单按钮: 与左侧 ☰"菜单"重复, 双入口不合适;
   应用菜单唯一入口 = 左侧 ☰菜单按钮) */
enum XjsWndBtn { WBTN_NONE = 0, WBTN_PIN, WBTN_MIN, WBTN_MAX, WBTN_CLOSE };

/* 视图模式四档 (正式版 VIEW_MODES 按图标从小到大排列: list 16 → details 32 → medium 48 → large 72;
   Ctrl+滚轮切换: 向上滚=放大, 向下滚=缩小, 两端停止不循环; 紧凑为默认, 同正式版) */
enum XjsViewMode { VM_LIST = 0, VM_DETAILS = 1, VM_MEDIUM = 2, VM_LARGE = 3 };
static const int XJS_ROW_H[4] = { 34, 54, 116, 142 };      /* medium/large=网格排高 (VIEW_CFG.rowH) */
static const int XJS_ICON_PX[4] = { 16, 32, 48, 72 };      /* 图标显示尺寸 */
static const int XJS_FETCH_ICON[4] = { 16, 32, 64, 128 };  /* 向引擎请求的图标尺寸 (网格取大图保清晰) */
static const int XJS_GRID_ITEM_W[4] = { 0, 0, 100, 140 };  /* 网格格子宽 (VIEW_CFG.minItemW, 0=列表型) */

/* ==================== 绘图后端中立值类型 (换绘图引擎的接缝, 2026-09-17) ====================
 * 布局/皮肤/几何/颜色这类"值"不再绑定 D2D: 字段名与 D2D 同名 (left/top/right/bottom, r/g/b/a),
 * 存量取值代码零改动; 隐式转换运算符让值可以直接传进 rt->FillRectangle 等 D2D 调用。
 * 换绘图后端 (GDI+/Skia/…) 时: ① 本块换成新后端的桥接(include+转换运算符); ② 句柄别名指向
 * 新后端; ③ 重写 xjs_d2d.cpp —— UI 模块的布局/命中/渲染编排零改动 (绘制方法调用的收口属
 * "基元层", 审计报告 R3, 暂缓; 届时调用点 rt->Xxx(...) 改走 Xjs 绘制函数即可)。 */
struct XjsRect {
    float left = 0, top = 0, right = 0, bottom = 0;
    XjsRect() {}
    XjsRect(float l, float t, float r, float b) : left(l), top(t), right(r), bottom(b) {}
    operator D2D1_RECT_F() const { D2D1_RECT_F r = { left, top, right, bottom }; return r; }
};
struct XjsColor {
    float r = 0, g = 0, b = 0, a = 1.f;
    XjsColor() {}
    XjsColor(float rr, float gg, float bb, float aa = 1.f) : r(rr), g(gg), b(bb), a(aa) {}
    /* 反向桥接: 画刷取色 GetColor() 返回 D2D 颜色 (取色后改 alpha 再经 XjsTempBrush 用回) */
    XjsColor(const D2D1_COLOR_F& c) : r(c.r), g(c.g), b(c.b), a(c.a) {}
    operator D2D1_COLOR_F() const { D2D1_COLOR_F c = { r, g, b, a }; return c; }
};
struct XjsPoint2 {
    float x = 0, y = 0;
    XjsPoint2() {}
    XjsPoint2(float xx, float yy) : x(xx), y(yy) {}
    operator D2D1_POINT_2F() const { D2D1_POINT_2F p = { x, y }; return p; }
};
struct XjsRoundedRect {
    XjsRect rect;
    float radiusX = 0, radiusY = 0;
    XjsRoundedRect() {}
    XjsRoundedRect(const XjsRect& r, float rx, float ry) : rect(r), radiusX(rx), radiusY(ry) {}
    operator D2D1_ROUNDED_RECT() const { D2D1_ROUNDED_RECT rr = { rect, radiusX, radiusY }; return rr; }
};
inline XjsRect XjsRectF(float l, float t, float r, float b) { return XjsRect(l, t, r, b); }
inline XjsColor XjsColorF(float r, float g, float b, float a = 1.f) { return XjsColor(r, g, b, a); }
inline XjsPoint2 XjsPoint2F(float x, float y) { return XjsPoint2(x, y); }
inline XjsRoundedRect XjsRoundedRectF(const XjsRect& r, float rx, float ry) { return XjsRoundedRect(r, rx, ry); }
using XjsDrawTextOpts = D2D1_DRAW_TEXT_OPTIONS;
/* 换行语义中性常量 (SetWordWrapping 唯一合法实参): 两后端同值同义, 数值对齐
   DWRITE_WORD_WRAPPING 以便 D2D 表直转。调用点禁止裸写 DWRITE_* 拼写, 换行行为
   以这里的注释为唯一事实源 (GDI+ 曾自造 "1=换行" 映射致列表折行/toast 不折行)。
   WORD=词边界换行 (D2D 默认) / NONE=单行不换 (配 SetCharEllipsis 做省略号) / CHAR=字符级换行 (break-all) */
enum XjsWrapMode { XJS_WRAP_WORD = 0, XJS_WRAP_NONE = 1, XJS_WRAP_CHAR = 2 };
/* 垂直(段落)对齐中性常量 (SetParagraphAlignment 唯一合法实参): 两后端同值同义, 数值**必须**
   恒等 DWRITE_PARAGRAPH_ALIGNMENT (D2D 表直转; 下方 static_assert 锁死 —— 该枚举的真实
   顺序是 NEAR/FAR/CENTER 而非直觉的 NEAR/CENTER/FAR, CENTER=2 / FAR=1, 2026-09-18 踩坑:
   凭直觉写成 CENTER=1 = DWRITE 的 FAR, 值一路直通 D2D → 全库单行文本底部对齐 (列表文字
   压在行底/容量条上), 只查 GDI+ 一侧永远看不出根因)。GDI+ 按"框高 − 文本块高"的 0/½/1
   偏移实现 (GdiParaOffset)。调用点禁止裸写 DWRITE_PARAGRAPH_ALIGNMENT_*;
   水平对齐是另一套枚举 (DWRITE_TEXT_ALIGNMENT: LEADING=0/TRAILING=1/CENTER=2), 勿混。 */
enum XjsParaAlign { XJS_PARA_NEAR = 0, XJS_PARA_FAR = 1, XJS_PARA_CENTER = 2 };
static_assert(XJS_PARA_NEAR   == (int)DWRITE_PARAGRAPH_ALIGNMENT_NEAR,   "XjsParaAlign 必须与 DWrite 同值");
static_assert(XJS_PARA_FAR    == (int)DWRITE_PARAGRAPH_ALIGNMENT_FAR,    "XjsParaAlign 必须与 DWrite 同值");
static_assert(XJS_PARA_CENTER == (int)DWRITE_PARAGRAPH_ALIGNMENT_CENTER, "XjsParaAlign 必须与 DWrite 同值");
/* 值类型补充 (几何/渐变/度量 — 同样字段名对齐 D2D) */
struct XjsGradientStop { float position = 0; XjsColor color; };   /* 聚合: 站点 {pos, color} 初始化可用 */
struct XjsEllipse   { XjsPoint2 point; float radiusX = 0, radiusY = 0; };
inline XjsEllipse XjsEllipseF(XjsPoint2 c, float rx, float ry) { XjsEllipse e; e.point = c; e.radiusX = rx; e.radiusY = ry; return e; }
struct XjsSizeU          { UINT32 width = 0, height = 0; };
struct XjsTextMetrics    { float left = 0, top = 0, width = 0, widthIncludingTrailingWhitespace = 0, height = 0; };
struct XjsHitTestMetrics { float left = 0, top = 0, width = 0, height = 0; UINT32 textPosition = 0, length = 0; };
struct XjsTextRange      { UINT32 startPosition = 0, length = 0; };   /* 富文本范围 (字段名同 DWRITE_TEXT_RANGE) */

/* ==================== 渲染句柄与后端分发 (换绘图引擎的接缝, 2026-09-17) ====================
 * 句柄 = 薄包装类 (成员 void* h 装后端原生态), 方法名与 D2D 同形 → 全库调用点 (g_rt->FillRectangle
 * 等) 后端无关; 方法体内经 g_gfx (XjsGfxApi 函数表) 分发到当前后端。
 * 后端实现 = 一张 XjsGfxApi 静态表: D2D 在 xjs_d2d.cpp (转发 COM), GDI+ 在 xjs_gdiplus.cpp (整体)。
 * 增换后端 = 新写一个文件实现 XjsGfxApi + 在 XjsGfxStartup 选边, UI 模块零改动。
 * 已知后端差异 (可接受): GDI+ 无彩色 emoji (经 g_dc==NULL 自动退单色路径)、md UNIFORM 行距近似。 */
struct XjsRt; struct XjsBrush; struct XjsSolidBrush; struct XjsGradBrush; struct XjsBitmap;
struct XjsFormat; struct XjsTextLayout; struct XjsStroke; struct XjsGeo; struct XjsDwFactory; struct XjsDc;

struct XjsGfxApi {
    /* 表面生命周期 */
    void  (*CreateWindowRt)(HWND hwnd, int w, int h, XjsRt** out);
    void  (*FreeRt)(void* native);
    /* 帧 */
    void  (*BeginDraw)(XjsRt*);
    HRESULT (*EndDraw)(XjsRt*);
    void  (*Clear)(XjsRt*, const XjsColor&);
    void  (*Resize)(XjsRt*, UINT w, UINT h);
    XjsSizeU (*PixelSize)(XjsRt*);
    /* 几何 */
    void  (*FillRect)(XjsRt*, const XjsRect&, XjsBrush*);
    void  (*FillRoundRect)(XjsRt*, const XjsRoundedRect&, XjsBrush*);
    void  (*FrameRect)(XjsRt*, const XjsRect&, XjsBrush*, float w, XjsStroke*);
    void  (*FrameRoundRect)(XjsRt*, const XjsRoundedRect&, XjsBrush*, float w, XjsStroke*);
    void  (*Line)(XjsRt*, XjsPoint2, XjsPoint2, XjsBrush*, float w, XjsStroke*);
    void  (*FrameEllipse)(XjsRt*, const XjsEllipse&, XjsBrush*, float w, XjsStroke*);
    void  (*FillEllipse)(XjsRt*, const XjsEllipse&, XjsBrush*);
    void  (*DrawTextStr)(XjsRt*, const wchar_t*, UINT32, XjsFormat*, const XjsRect&, XjsBrush*, XjsDrawTextOpts, int measuring);
    void  (*DrawTextLay)(XjsRt*, XjsPoint2, XjsTextLayout*, XjsBrush*, XjsDrawTextOpts);
    /* 裁剪 / 变换 / 透明度组 (toast 卡片进离场) */
    void  (*PushClip)(XjsRt*, const XjsRect&, int aaMode);
    void  (*PopClip)(XjsRt*);
    void  (*SetRotate)(XjsRt*, float deg, XjsPoint2 center);
    void  (*ResetTransform)(XjsRt*);
    void  (*PushAlpha)(XjsRt*, const XjsRect&, float opacity);
    void  (*PopAlpha)(XjsRt*);
    /* 位图 (WIC 解码为两后端共用, 这里只负责成图) */
    void  (*BitmapFromWic)(XjsRt*, IWICBitmapSource*, XjsBitmap**);
    void  (*BitmapFromMemory)(XjsRt*, UINT w, UINT h, UINT stride, const BYTE* px, XjsBitmap**);
    void  (*DrawBmp)(XjsRt*, XjsBitmap*, const XjsRect&, float opacity, int interp, const XjsRect* src);
    XjsSizeU (*BmpPixelSize)(XjsBitmap*);
    void  (*FreeBitmap)(void* native);
    /* 画刷 */
    HRESULT (*SolidBrush)(XjsRt*, const XjsColor&, XjsSolidBrush**);
    void  (*GradBrush)(XjsRt*, XjsPoint2 start, XjsPoint2 end, const XjsGradientStop*, int n, XjsGradBrush**);
    void  (*RadialBrush)(XjsRt*, XjsPoint2 center, XjsPoint2 offset, float rx, float ry, const XjsGradientStop*, int n, XjsGradBrush**);
    void  (*SetBrushColor)(XjsSolidBrush*, const XjsColor&);
    XjsColor (*GetBrushColor)(XjsSolidBrush*);
    void  (*FreeBrush)(void* native);

    /* 渐变端点跟随 (选中条逐行横渐/进度条) */
    void  (*GradSetStart)(XjsGradBrush*, XjsPoint2);
    void  (*GradSetEnd)(XjsGradBrush*, XjsPoint2);
    /* md 富文本范围样式 (effect = 画刷包装 XjsSolidBrush*, 两端后端都必须自行解包装;
       D2D 侧须 D2() 取原生再交 DWrite — 直传包装会被 DWrite AddRef 虚表越界, 2026-09-18 实锤) */
    void  (*LayoutFontWeight)(XjsTextLayout*, float weight, XjsTextRange);
    void  (*LayoutFontStyle)(XjsTextLayout*, int style, XjsTextRange);
    void  (*LayoutFontFamily)(XjsTextLayout*, const wchar_t*, XjsTextRange);
    void  (*LayoutFontSize)(XjsTextLayout*, float px, XjsTextRange);
    void  (*LayoutDrawingEffect)(XjsTextLayout*, void* effect, XjsTextRange);
    void  (*LayoutUnderline)(XjsTextLayout*, BOOL on, XjsTextRange);
    HRESULT (*LayoutHitRange)(XjsTextLayout*, UINT32 pos, UINT32 len, float ox, float oy, XjsHitTestMetrics*, UINT32 max, UINT32* returned);
    /* 行内编辑像素→下标 (HitTestPoint) */
    bool  (*LayoutHitPoint)(XjsTextLayout*, float x, float y, BOOL* trail, BOOL* inside, XjsHitTestMetrics*);
    /* 字符级省略号 (D2D=签名对象, GDI+=DT_END_ELLIPSIS; 签名缓存在后端内部) */
    void  (*FormatSetCharEllipsis)(XjsFormat*);
    void  (*LayoutSetCharEllipsis)(XjsTextLayout*);
    /* 圆头描边 (SVG stroke-linecap:round) 与 圆角几何求交填充 (toast 色条/进度条端头) */
    void  (*RoundStroke)(XjsStroke**);
    void  (*FreeStroke)(void* native);
    void  (*RoundRectGeo)(const XjsRoundedRect&, XjsGeo**);
    void  (*FillGeoIntersectRect)(XjsRt*, XjsGeo*, const XjsRect&, XjsBrush*);
    void  (*FreeGeo)(void* native);
    /* 文本 */
    void  (*MakeFormat)(const wchar_t* family, void* collection, float weight, int style, int stretch,
                        float px, const wchar_t* locale, XjsFormat**);
    void  (*FormatSetAlign)(XjsFormat*, int align);
    void  (*FormatSetParaAlign)(XjsFormat*, int align);
    void  (*FormatSetWrap)(XjsFormat*, int wrap);
    void  (*FormatSetLineSpacing)(XjsFormat*, int method, float spacing, float baseline);
    float (*FormatFontSize)(XjsFormat*);
    void  (*FreeFormat)(void* native);
    HRESULT (*MakeLayout)(const wchar_t* text, UINT32 len, XjsFormat*, float maxW, float maxH, XjsTextLayout**);
    HRESULT (*LayoutMetrics)(XjsTextLayout*, XjsTextMetrics*);
    void  (*LayoutSetWrap)(XjsTextLayout*, int wrap);
    void  (*LayoutSetLineSpacing)(XjsTextLayout*, int method, float spacing, float baseline);
    bool  (*LayoutHitTest)(XjsTextLayout*, UINT32 idx, BOOL trailing, float* x, float* y, XjsHitTestMetrics*);
    void  (*FreeLayout)(void* native);
};
extern XjsGfxApi* g_gfx;     /* 当前绘图后端函数表 (XjsGfxStartup 选边) */
extern int g_gfxEngine;      /* 绘制引擎选择 (设置-通用, 重启生效): 0=D2D 1=GDI+ */

/* 句柄薄类: 成员只有一个后端原生态指针; 方法内联转发 g_gfx (名与 D2D 同形, 调用点零改动) */
struct XjsRt {
    void* h;
    XjsRt() : h(NULL) {}
    XjsRt(void* n) : h(n) {}
    void BeginDraw()                       { g_gfx->BeginDraw(this); }
    HRESULT EndDraw()                      { return g_gfx->EndDraw(this); }
    void Clear(const XjsColor& c)          { g_gfx->Clear(this, c); }
    void Resize(UINT w, UINT hh)           { g_gfx->Resize(this, w, hh); }
    XjsSizeU GetPixelSize()                { return g_gfx->PixelSize(this); }
    void FillRectangle(const XjsRect& r, XjsBrush* b)                    { g_gfx->FillRect(this, r, b); }
    void FillRoundedRectangle(const XjsRoundedRect& r, XjsBrush* b)      { g_gfx->FillRoundRect(this, r, b); }
    void DrawRectangle(const XjsRect& r, XjsBrush* b, float w = 1.0f, XjsStroke* st = NULL)   { g_gfx->FrameRect(this, r, b, w, st); }
    void DrawRoundedRectangle(const XjsRoundedRect& r, XjsBrush* b, float w = 1.0f, XjsStroke* st = NULL) { g_gfx->FrameRoundRect(this, r, b, w, st); }
    void DrawLine(XjsPoint2 a, XjsPoint2 b, XjsBrush* br, float w = 1.0f, XjsStroke* st = NULL) { g_gfx->Line(this, a, b, br, w, st); }
    void DrawEllipse(const XjsEllipse& e, XjsBrush* b, float w = 1.0f, XjsStroke* st = NULL)  { g_gfx->FrameEllipse(this, e, b, w, st); }
    void FillEllipse(const XjsEllipse& e, XjsBrush* b)                   { g_gfx->FillEllipse(this, e, b); }
    void DrawText(const wchar_t* s, UINT32 len, XjsFormat* f, const XjsRect& r, XjsBrush* b,
                  XjsDrawTextOpts o = D2D1_DRAW_TEXT_OPTIONS_NONE, int measuring = 0)   { g_gfx->DrawTextStr(this, s, len, f, r, b, o, measuring); }
    void DrawTextLayout(XjsPoint2 org, XjsTextLayout* l, XjsBrush* b, XjsDrawTextOpts o = D2D1_DRAW_TEXT_OPTIONS_NONE) { g_gfx->DrawTextLay(this, org, l, b, o); }
    void PushAxisAlignedClip(const XjsRect& r, int aa)   { g_gfx->PushClip(this, r, aa); }
    void PopAxisAlignedClip()                { g_gfx->PopClip(this); }
    void SetRotate(float deg, XjsPoint2 c)   { g_gfx->SetRotate(this, deg, c); }
    void ResetTransform()                    { g_gfx->ResetTransform(this); }
    void PushAlpha(const XjsRect& r, float opacity) { g_gfx->PushAlpha(this, r, opacity); }
    void PopAlpha()                          { g_gfx->PopAlpha(this); }
    void CreateBitmapFromWicBitmap(IWICBitmapSource* src, void* /*props*/, XjsBitmap** out)   { g_gfx->BitmapFromWic(this, src, out); }
    void CreateBitmapFromMemory(UINT w, UINT hh, UINT stride, const BYTE* px, XjsBitmap** out) { g_gfx->BitmapFromMemory(this, w, hh, stride, px, out); }
    void DrawBitmap(XjsBitmap* bm, const XjsRect& r, float opacity = 1.0f, int interp = 0, const XjsRect* src = NULL) { g_gfx->DrawBmp(this, bm, r, opacity, interp, src); }
    HRESULT CreateSolidColorBrush(const XjsColor& c, XjsSolidBrush** out)   { return g_gfx->SolidBrush(this, c, out); }
    void CreateLinearGradientBrush(XjsPoint2 s, XjsPoint2 e, const XjsGradientStop* st, int n, XjsGradBrush** out) { g_gfx->GradBrush(this, s, e, st, n, out); }
    void CreateRadialGradientBrush(XjsPoint2 c, XjsPoint2 off, float rx, float ry, const XjsGradientStop* st, int n, XjsGradBrush** out) { g_gfx->RadialBrush(this, c, off, rx, ry, st, n, out); }
    void CreateRoundRectGeo(const XjsRoundedRect& r, XjsGeo** out)       { g_gfx->RoundRectGeo(r, out); }
    void FillGeoIntersectRect(XjsGeo* g, const XjsRect& r, XjsBrush* b)  { g_gfx->FillGeoIntersectRect(this, g, r, b); }
    void Release()                           { if (h) { g_gfx->FreeRt(h); h = NULL; } delete this; }
    virtual ~XjsRt() {}
};
struct XjsBitmap {
    void* h;
    int refs = 1;   /* 包装级引用计数 (图标缓存: 缓存持一份 + 调用方一份) */
    XjsBitmap(void* n) : h(n) {}
    void AddRef()              { refs++; }
    XjsSizeU GetPixelSize()    { return g_gfx->BmpPixelSize(this); }
    void Release()             { if (--refs > 0) return; if (h) { g_gfx->FreeBitmap(h); h = NULL; } delete this; }
    virtual ~XjsBitmap() {}
};
struct XjsBrush {
    void* h;
    XjsBrush(void* n) : h(n) {}
    void Release()            { if (h) { g_gfx->FreeBrush(h); h = NULL; } delete this; }
    virtual ~XjsBrush() {}
};
struct XjsSolidBrush : XjsBrush {
    XjsSolidBrush(void* n) : XjsBrush(n) {};
    void SetColor(const XjsColor& c)   { g_gfx->SetBrushColor(this, c); }
    XjsColor GetColor()                { return g_gfx->GetBrushColor(this); }
};
struct XjsGradBrush : XjsBrush {
    XjsGradBrush(void* n) : XjsBrush(n) {}
    void SetStartPoint(XjsPoint2 p) { g_gfx->GradSetStart(this, p); }
    void SetEndPoint(XjsPoint2 p)   { g_gfx->GradSetEnd(this, p); }
};
struct XjsStroke {
    void* h;
    XjsStroke(void* n) : h(n) {}
    void Release()            { if (h) { g_gfx->FreeStroke(h); h = NULL; } delete this; }
    virtual ~XjsStroke() {}
};
struct XjsGeo {
    void* h;
    XjsGeo(void* n) : h(n) {}
    void Release()            { if (h) { g_gfx->FreeGeo(h); h = NULL; } delete this; }
    virtual ~XjsGeo() {}
};
struct XjsFormat {
    void* h;
    XjsFormat(void* n) : h(n) {}
    void SetTextAlignment(int a)               { g_gfx->FormatSetAlign(this, a); }
    void SetParagraphAlignment(int a)          { g_gfx->FormatSetParaAlign(this, a); }
    void SetWordWrapping(int w)                { g_gfx->FormatSetWrap(this, w); }
    void SetCharEllipsis()                     { g_gfx->FormatSetCharEllipsis(this); }
    void SetLineSpacing(int m, float s, float b) { g_gfx->FormatSetLineSpacing(this, m, s, b); }
    float GetFontSize()            { return g_gfx->FormatFontSize(this); }
    void Release()                 { if (h) { g_gfx->FreeFormat(h); h = NULL; } delete this; }
    virtual ~XjsFormat() {}
};
struct XjsTextLayout {
    void* h;
    XjsTextLayout(void* n) : h(n) {}
    bool HitTestPoint(float x, float y, BOOL* trail, BOOL* inside, XjsHitTestMetrics* m) { return g_gfx->LayoutHitPoint(this, x, y, trail, inside, m); }
    void SetFontWeight(float w, XjsTextRange rg)      { g_gfx->LayoutFontWeight(this, w, rg); }
    void SetFontStyle(int s, XjsTextRange rg)         { g_gfx->LayoutFontStyle(this, s, rg); }
    void SetFontFamilyName(const wchar_t* f, XjsTextRange rg) { g_gfx->LayoutFontFamily(this, f, rg); }
    void SetFontSize(float px, XjsTextRange rg)       { g_gfx->LayoutFontSize(this, px, rg); }
    void SetDrawingEffect(void* effect, XjsTextRange rg) { g_gfx->LayoutDrawingEffect(this, effect, rg); }   /* effect 传包装 (XjsSolidBrush*), 解包装是后端职责 */
    void SetUnderline(BOOL on, XjsTextRange rg)       { g_gfx->LayoutUnderline(this, on, rg); }
    HRESULT HitTestTextRange(UINT32 pos, UINT32 len, float ox, float oy, XjsHitTestMetrics* out, UINT32 max, UINT32* returned) { return g_gfx->LayoutHitRange(this, pos, len, ox, oy, out, max, returned); }
    HRESULT GetMetrics(XjsTextMetrics* m)          { return g_gfx->LayoutMetrics(this, m); }
    void SetWordWrapping(int w)                 { g_gfx->LayoutSetWrap(this, w); }
    void SetCharEllipsis()                      { g_gfx->LayoutSetCharEllipsis(this); }
    void SetLineSpacing(int m, float s, float b) { g_gfx->LayoutSetLineSpacing(this, m, s, b); }
    bool HitTestTextPosition(UINT32 idx, BOOL trailing, float* x, float* y, XjsHitTestMetrics* m) { return g_gfx->LayoutHitTest(this, idx, trailing, x, y, m); }
    void Release()                 { if (h) { g_gfx->FreeLayout(h); h = NULL; } delete this; }
    virtual ~XjsTextLayout() {}
};
/* 文本工厂 (方法转发 g_gfx->MakeFormat/MakeLayout; 生命周期随后端 Startup/Shutdown) */
struct XjsDwFactory {
    void* h;
    XjsDwFactory(void* n) : h(n) {}
    void CreateTextFormat(const wchar_t* family, void* collection, float weight, int style, int stretch,
                          float px, const wchar_t* locale, XjsFormat** out)   { g_gfx->MakeFormat(family, collection, weight, style, stretch, px, locale, out); }
    HRESULT CreateTextLayout(const wchar_t* text, UINT32 len, XjsFormat* f, float maxW, float maxH, XjsTextLayout** out) { return g_gfx->MakeLayout(text, len, f, maxW, maxH, out); }
    void Release()            { delete this; }   /* 无后端原生态, 只拆包装 */
    virtual ~XjsDwFactory() {}
};
/* D2D 专属: 设备上下文视角 (彩色 emoji 走 COLR 字形); GDI+ 后端不创建它, 调用点自动退单色 */
struct XjsDc {
    void* h;
    XjsDc(void* n) : h(n) {}
    void DrawText(const wchar_t* s, UINT32 len, XjsFormat* f, const XjsRect& r, XjsBrush* b, XjsDrawTextOpts o);
    void DrawTextLayout(XjsPoint2 org, XjsTextLayout* l, XjsBrush* b, XjsDrawTextOpts o);
    void Release()            { h = NULL; delete this; }
    virtual ~XjsDc() {}
};
using XjsHwndRt = XjsRt;     /* 主窗 hwnd 绑定句柄 (与 rt 指向同一包装, 释放口径见 XjsDeviceDiscardCtx) */
using XjsWicFactory = IWICImagingFactory;   /* WIC = 图片解码管道, 两后端共用, 不做句柄化 (内存解码走 CreateDecoderFromStream + IStream, 不引 Factory2 的 WINVER 门控) */

/* ==================== 主题 (皮肤文件驱动, 正式版 skin-*.css 同款变量) ==================== */
inline XjsColor XjsCol(UINT32 rgb, float a = 1.0f) {
    return XjsColorF((FLOAT)((rgb >> 16) & 0xFF) / 255.0f, (FLOAT)((rgb >> 8) & 0xFF) / 255.0f,
                        (FLOAT)(rgb & 0xFF) / 255.0f, a);
}

/* 皮肤颜色集 (变量名与正式版 skin-*.css 的 :root --xxx 一一对应; 内置默认 = skin-dark 值) */
struct XjsSkin {
    XjsColor bg1, bg2;             /* --bg1 --bg2 窗口背景渐变 */
    XjsColor panel, panel2;        /* --panel --panel-2 一二级表面 */
    XjsColor border, borderStrong; /* --border --border-strong */
    XjsColor text, textDim, textFaint; /* --text --text-dim --text-faint */
    XjsColor accent, accent2, accentSoft; /* --accent --accent2 --accent-soft */
    XjsColor rowHover;             /* --row-hover */
    XjsColor hl, hlBg;             /* --hl --hl-bg 匹配词高亮 */
    XjsColor timeBadge;            /* --time-badge 时间徽章满底色 */
    XjsColor menuBg;               /* --menu-bg 菜单底 (弹窗窗口不支持逐像素alpha, 取色相不透明) */
    XjsColor driveTrack;           /* --drive-track 容量条轨道 */
    XjsColor ok, err, warn;        /* --ok --err --warn 状态色 */
};
extern XjsSkin g_skin;
extern std::wstring g_skinName;   /* 当前窗皮肤名镜像 (每窗真值 = XjsSearchWindow::skinName; 对应 skin\skin-<名>.css) */

void XjsSkinReset();                            /* 恢复内置默认 (skin-dark 同值) */
bool XjsSkinLoad(const wchar_t* name);          /* 复位默认后解析皮肤文件覆盖; 找不到/解析失败=假(即内置默认) */
void XjsSkinApply();                            /* g_skin → 主题画刷/渐变 (需已建 RT), 重建弹窗画刷 */
std::vector<std::wstring> XjsSkinEnumerate();   /* 扫描可用皮肤名 (skin目录/exe目录 skin-*.css) */
extern std::vector<std::wstring> g_skinMenuNames;  /* ☰菜单"皮肤"分组当前条目 (点击 id 换算索引) */

void XjsPopupReleaseResources();                /* 释放弹窗RT/画刷 (换肤后下次打开按新皮肤重建) */

/* ==================== 设置窗口 (独立顶层窗口, 皮肤列表等迁入) ==================== */
extern int g_skinEpoch;                         /* 每次换肤 +1 (子窗口据此重建自己的画刷) */
void XjsRegisterSettingsClass(HINSTANCE hInst);
void XjsSettingsShow(int cat = -1);             /* 打开/前置设置窗口 (单例); cat>=0 = 打开后直接切到该分类页 (SC_*, xjs_settings.cpp) */
void XjsSettingsShowDonate();                   /* ☰菜单"捐赠": 打开设置并直达捐赠页 (分类 id 收在设置模块内, 外部经此入口防下标漂移) */
bool XjsSettingsOpenFor(HWND ownerSearchHwnd);  /* 设置窗打开中且 owner=该搜索窗 (失焦关闭豁免判定用) */

/* ---- xjs_md (通用 Markdown 引擎: md4c 解析 + D2D 排版绘制; 设置"搜索模式"页与未来 .md 预览共用) ----
   用法: Create → SetText(utf8) → Layout(rt, 内容宽, unit) 得总高 → Paint(rt, x, yTop, unit, 裁剪上下界)。
   画刷/文本格式由引擎建在传入的 RT 上 (跨 RT 画刷 = 整帧丢弃), 键 = (RT, 皮肤纪元, 单位尺度);
   unit = DPI 尺度 × 页面缩放, 排版随 (contentW, unit, g_skinEpoch) 失效自动重排 */
struct XjsMdDoc;
XjsMdDoc* XjsMdCreate();                                              /* 创建文档实例 (空内容) */
void      XjsMdFree(XjsMdDoc* d);                                     /* 销毁实例 (含 RT 绑定资源) */
bool      XjsMdSetText(XjsMdDoc* d, const char* utf8, unsigned len);  /* 换内容 (UTF-8; 返回=非空文档) */
float     XjsMdLayout(XjsMdDoc* d, XjsRt* rt, float contentW, float unit);   /* 返回总高 */
void      XjsMdPaint(XjsMdDoc* d, XjsRt* rt, float x, float yTop, float unit, float clipT, float clipB);

enum XjsThemeIdx {
    XTH_BG1 = 0, XTH_BG2, XTH_PANEL, XTH_PANEL2, XTH_BORDER, XTH_BORDER_STRONG,
    XTH_TEXT, XTH_TEXT_DIM, XTH_TEXT_FAINT, XTH_ACCENT, XTH_ACCENT2, XTH_ACCENT_SOFT,
    XTH_ROW_HOVER, XTH_HL, XTH_HL_BG, XTH_COUNT
};

/* ==================== 布局区域 ==================== */
struct XjsLayout {
    float w = 0, h = 0;
    XjsRect titlebar{};     // 40px 标题栏
    XjsRect menuBtn{};      // ☰ 菜单按钮
    XjsRect searchBox{};    // 标题栏内搜索框
    XjsRect modeBtn{};      // 搜索模式按钮 (放大镜)
    XjsRect editRect{};     // 输入区 (自绘文本/光标/选区的矩形)
    XjsRect clearBtn{};     // 清空
    XjsRect historyBtn{};   // 历史
    XjsRect filterBtn{};    // 筛选下拉 (☰ 全部 ˅)
    XjsRect listHead{};     // 表头 28px
    XjsRect list{};         // 列表区 (预览面板打开时右缘收窄)
    XjsRect vtrack{};       // 滚动条轨道
    XjsRect statusbar{};    // 状态栏 36px
    XjsRect preview{};      // 预览面板 (含 3px resizer 左缘)
    XjsRect sbToolbox{};    // 工具箱
    XjsRect sbPlugin[8]{};  // 插件状态栏项 (statusBar 能力; 内置组左侧向左排, 未用槽=零矩形)
    int sbPluginN = 0;      // 本帧插件项数 (布局/渲染/悬停/点击四处同源; 渲染帧写回, 惯用 1 帧内几何)
    XjsRect btnRects[5]{};  // 标题栏窗口按钮 [WBTN_PIN..WBTN_CLOSE] (被隐藏的按钮 = 零矩形)
};

/* ==================== 数据结构 ==================== */
struct XjsHlSeg { std::wstring text; bool isKey; };

struct XjsRowData {
    int fileId = -1;
    bool isDrive = false;
    std::wstring name, folder, alias;      // folder: 驱动器行 = "卷标 (总容量)"
    long long size = 0, mtime = 0;
    long long ctime = 0, atime = 0;   /* 创建/访问时间 ms (字段未开启=0, 列显"-"同源样式) */
    unsigned attrs = 0;               /* Windows 属性位 (字段未开启=0, 列显"-"同源样式) */
    int rating = 0;
    bool hasAlias = false;
    std::vector<XjsHlSeg> nameSegs, folderSegs, aliasSegs;
    /* 驱动器信息 (isDrive 时有效) */
    std::wstring driveLabel, driveFs;
    long long driveTotal = 0, driveUsed = 0, driveFree = 0;
    int drivePercent = 0;
    unsigned long driveSerial = 0;
};

struct XjsFilterCat { std::wstring name; };

/* 列内容绘制器 (数据驱动行渲染: 行循环只按可见列调 draw, 列藏/换/增不改渲染代码)。
   cell=该列原始矩形 (行高全高, 各绘制器自行内缩), listView=false = 详情视图 (名称带副行) */
typedef void (*XjsColDrawFn)(const XjsRect& cell, XjsRowData* rd, int idx, bool listView);

/* 列绘制器 (xjs_list.cpp 实现, 默认列表按字段挂接) */
void XjsColDrawName(const XjsRect& cell, XjsRowData* rd, int idx, bool listView);
void XjsColDrawAlias(const XjsRect& cell, XjsRowData* rd, int idx, bool listView);
void XjsColDrawFolder(const XjsRect& cell, XjsRowData* rd, int idx, bool listView);
void XjsColDrawRating(const XjsRect& cell, XjsRowData* rd, int idx, bool listView);
void XjsColDrawSize(const XjsRect& cell, XjsRowData* rd, int idx, bool listView);
void XjsColDrawTime(const XjsRect& cell, XjsRowData* rd, int idx, bool listView);
void XjsColDrawCtime(const XjsRect& cell, XjsRowData* rd, int idx, bool listView);
void XjsColDrawAtime(const XjsRect& cell, XjsRowData* rd, int idx, bool listView);
void XjsColDrawAttrs(const XjsRect& cell, XjsRowData* rd, int idx, bool listView);

struct XjsColSpec {
    const wchar_t* label;
    int width;             /* 列宽 (逻辑px; 弹性列=未溢出下限/用户拖宽后的固定宽) */
    bool right;
    const char* sortField;
    bool flex;             /* 弹性列: 未溢出时按 fr 伸展, 溢出时钉在 width; 拖宽后转固定列 */
    float fr;              /* 弹性比例 (名称1 / 名称1.2 / 文件夹1.3, 同源样式 COLS_ALL) */
    bool visible = true;   /* 列显隐 (表头右键菜单切换; 至少保留一列) */
    XjsColDrawFn draw = nullptr;  /* 行内容绘制器 (运行时装配, 不持久化) */
};

/* 一套列布局 (详情/列表视图各一套): 规格数组 + 实际列数 + 存取/显隐映射。
   以前裸数组+魔法数 5/6 散布 4 个文件 (窗口上下文/每窗档案/存取/列几何), 改列数要逐处对齐 */
struct XjsColumnSet {
    static const int MAX = 9;            /* 缓冲上限 = 列表视图 9 列 (详情 8), 定长避免堆分配 */
    XjsColSpec arr[MAX] = {};
    int n = 0;                            /* 实际列数 (详情 8 / 列表 9) */
    int Count() const { return n; }
    void Init(const XjsColSpec* defs, int count) { n = count; memcpy(arr, defs, sizeof(XjsColSpec) * count); }
    /* 可见序 → 全数组下标 (命中/拖宽等按全数组下标取列规格) */
    int VisibleAt(int visIdx) const {
        int vi = 0;
        for (int i = 0; i < n; i++)
            if (arr[i].visible && vi++ == visIdx) return i;
        return -1;
    }
    /* 列 JSON ←→ 本集合 的序列化 (XjsColsApplyJson/XjsColsSaveJson) 是配置层实现细节,
       与 XjsConfig 门面同住 xjs_engine.cpp (picojson 不进本头文件, 换 JSON 模块只改那一处) */
};

/* 菜单项线性图标 (16px 描边小图标, 照源样式 ctx-ic SVG; 0=无) */
enum XjsMenuIcon {
    XMI_NONE = 0,
    XMI_OPEN, XMI_FOLDER, XMI_RENAME, XMI_ALIAS,
    XMI_CUT, XMI_COPY, XMI_PASTE, XMI_SELECTALL, XMI_DELETE,
    XMI_COPYPATH, XMI_GEAR,
    XMI_RESTORE, XMI_EXIT,   /* 托盘菜单: 恢复窗口 / 退出程序 (照源样式 Tray.Restore/Tray.Exit SVG) */
    XMI_HEART,               /* 托盘菜单: 捐赠 (心形; 源样式无此项, 按同族线段风格自绘) */
};

struct XjsPopupItem {
    int id = 0;
    std::wstring title, sub;
    bool checked = false;
    bool sep = false;
    bool header = false;   /* 分组标签: 暗色小字, 无悬停高亮不可点 (同源样式"搜索模式") */
    bool accent = false;   /* 强调色文字 (同源样式"+ 添加搜索模式") */
    bool disabled = false; /* 置灰不可点 (源样式列显隐菜单未开启字段/搜索框菜单无选区项) */
    int icon = XMI_NONE;   /* 左侧 16px 图标槽 (文件右键/编辑菜单同源样式带图标) */
    bool editBtn = false;  /* 尾部编辑按钮 (自定义模式项, 源样式菜单项内联 ✎; 点击回传 id|XJS_POPUP_EDIT) */
    bool delBtn = false;   /* 尾部删除按钮 (与 editBtn 同现: ✎ 左 ✕ 右; 首击原地变红"确定删除"且菜单保持, 再击回传 id|XJS_POPUP_DEL) */
    std::vector<XjsPopupItem> children;   /* 子菜单 (通用级联, 2026-09-17): 非空 = 父项, 右缘画 ▸, 悬停/→ 展开;
                                             父项本体不可执行 (点击只展开); 子项 id 由调用方自行分配 */
};
#define XJS_POPUP_EDIT 0x40000000   /* 弹窗项尾部编辑按钮点击标志 (回传 id 时置位) */
#define XJS_POPUP_DEL  0x20000000   /* 弹窗项尾部删除按钮确认后点击标志 (回传 id 时置位) */

/* 单行自绘编辑器组件 (搜索框 / 行内重命名 / 别名对话框共用同一实现):
 *   - 文本/光标/选区/横向滚动 全 D2D 绘制, 无原生 EDIT
 *   - 鼠标: 点选/拖拽选字/双击选词; 键盘: 方向/Home/End/Back/Del/Ctrl+A,C,X,V,Z(单档撤销)
 *   - IME 上屏走 WM_IME_COMPOSITION GCS_RESULTSTR (组字锚点 UpdateImeAnchor)
 *   - 宿主只需给出区域/RenderTarget/文本格式/画刷; 文本被改时 dirty=1, 宿主读取后自行处理
 *     (搜索框据此"输入即搜", 重命名/别名对话框忽略) */
struct XjsLineEdit {
    std::wstring text;
    int caret = 0;          /* 光标位置 (UTF-16 码元下标, 0..len) */
    int anchor = -1;        /* 选区锚点 (<0 = 无选区) */
    float scroll = 0;       /* 文本宽于区域时的横向跟随 (多行模式全行共享) */
    float scrollY = 0;      /* 多行模式: 内容高于区域时的纵向跟随 */
    float vx = -1;          /* 多行模式: ↑↓ 连续移动的期望列 x (<0 无; 其它移动复位) */
    bool dragV = false;     /* 多行模式: 正在拖纵向滚动条 */
    bool dragH = false;     /* 多行模式: 正在拖横向滚动条 */
    int sbHover = 0;        /* 多行模式: 悬停的滚动条 (0=无 1=纵向 2=横向; thumb 提亮一档) */
    float dragGrab = 0;     /* 拖拽抓点相对 thumb 端的偏移 */
    bool dragging = false;  /* 鼠标拖拽选字中 (宿主据此把 WM_MOUSEMOVE 转交 MouseMove) */
    bool dirty = false;     /* 文本被修改 (宿主读后清零; 纯光标移动不置位) */
    bool multiline = false; /* 多行模式: 按 '\n' 切逻辑行 (不软换行), Enter=换行, Paste 保留换行 */

    void SetText(const std::wstring& s, bool selectAll);
    void SelRange(int* a, int* b) const;
    bool HasSel() const { return anchor >= 0; }
    void Clear();                                    /* 清空 + 记撤销档 */
    void SelectAll();
    void CopySel();
    void CutSel();                                   /* 复制选区后删除 */
    void Paste();                                    /* CF_UNICODETEXT, 换行折空格 */
    void DeleteSel();
    void SnapshotUndo();

    bool Key(WPARAM vk, XjsFormat* fmt = NULL);  /* 编辑键 (Esc 与单行的回车等宿主语义键由宿主先处理; 多行回车=换行; fmt 供多行 ↑↓ 列测量) */
    bool Wheel(int delta, bool horizontal, const XjsRect& area, XjsFormat* fmt);  /* 多行滚轮 (纵向 3 行/格; 单行=假交宿主) */
    /* 滚动条几何 (渲染/命中/拖拽同源; 样式随列表滚动条: 宽8 圆角4 最短24, 覆盖式贴缘)。maxScroll*=0 表示无溢出不画 */
    void Scrollbars(const XjsRect& area, XjsFormat* fmt,
                    XjsRect* vt, XjsRect* ht, float* maxScrollX, float* maxScrollY) const;
    bool Char(wchar_t ch);                           /* WM_CHAR / WM_IME_CHAR */
    bool ImeResult(HWND hwnd, LPARAM lParam);        /* GCS_RESULTSTR 整串上屏 */

    int  IndexAtPoint(POINT pt, const XjsRect& area, XjsFormat* fmt);
    void MouseDown(POINT pt, const XjsRect& area, XjsFormat* fmt);   /* 点定位+起拖 */
    void MouseMove(POINT pt, const XjsRect& area, XjsFormat* fmt);   /* 拖拽扩展选区 */
    bool UpdateSbHover(POINT pt, const XjsRect& area, XjsFormat* fmt);   /* 多行滚动条悬停命中 (更新 sbHover, 变化返回真) */
    void MouseUp();                                  /* 结束拖拽 (单击零长选区收拢) */
    bool MouseDoubleClick(POINT pt, const XjsRect& area, XjsFormat* fmt);  /* 双击选词 */

    void EnsureCaretVisible(const XjsRect& area, XjsFormat* fmt);
    void UpdateImeAnchor(HWND hwnd, const XjsRect& area, XjsFormat* fmt);
    /* 绘制: 选区底 → 文本(空文本时画 placeholder) → 光标。空文本也画光标(与有文本一致);
       placeholder 可传 NULL(不画占位符)。画刷须与 target 同源。
       caretOn 由 XjsCaretBlink::On() 给出 (是否画光标: 聚焦 && 窗口激活 && 闪烁相位)。 */
    void Render(XjsRt* target, const XjsRect& area, XjsFormat* fmt,
                XjsBrush* textBr, XjsBrush* selectionBr, XjsBrush* caretBr,
                const wchar_t* placeholder, XjsBrush* placeholderBr, bool caretOn,
                XjsBrush* scrollbarBr = NULL,   /* 多行滚动条画刷 (NULL=不画) */
                XjsBrush* scrollbarHoverBr = NULL);   /* 悬停/拖拽中提亮画刷 (NULL=不提亮) */

private:
    std::wstring m_undoText;                         /* 单档撤销 */
    int m_undoCaret = 0;
    bool m_undoValid = false;
};

/* 右键编辑菜单的通用构造/执行 (搜索框/行内重命名/路由层字段/别名框共用同一份五项):
   宿主 WM_RBUTTONDOWN 内对命中字段调 XjsEditMenuAppendItems + XjsShowPopupMenu(本窗),
   WM_POPUP_RESULT 里 XjsEditMenuApplyCmd — 禁止再各写一份菜单表。
   base = 该宿主菜单的 id 基址 (项 = base+0..+4: 剪切/复制/粘贴/分隔/全选/删除), 各宿主 ID 段不同 */
void XjsEditMenuAppendItems(const XjsLineEdit& ed, std::vector<XjsPopupItem>& items, int base = IDM_FCTX_BASE);
void XjsEditMenuApplyCmd(XjsLineEdit& ed, int cmd);   /* 0剪切 1复制 2粘贴 3全选 4删除 */

/* 输入光标闪烁驱动器 (搜索框/行内重命名/独立输入窗共用同一套焦点口径):
 * 集中管理 ID_TIMER_CARET 计时器 + 闪烁相位 + "此刻该不该显示光标", 宿主不再各写一份 ——
 * 此前"窗口失去焦点(Alt+Tab/点其它程序)光标仍在闪烁"只在搜索框之外反复复发, 每加一个
 * 输入框都要重踩; 收进本类后所有输入框自动获得同一份失焦熄光标/复焦恢复行为。
 *   显示条件 = 输入框聚焦(m_focus) && 承载窗口激活(m_active); 二者任一为假即熄光标并停表。
 * 宿主用法: Attach(hwnd, 局部重绘回调) → Focus(true/false) → 渲染读 On();
 *   WM_TIMER(ID_TIMER_CARET) → TickWindow(hwnd); WM_ACTIVATE → SetWindowActive(hwnd, 激活?). */
class XjsCaretBlink {
public:
    void Attach(HWND hwnd, std::function<void()> repaint);   /* 绑定承载窗口 + 局部重绘回调 (登记到该窗口) */
    void Detach();                                           /* 解绑 (窗口销毁前调用) */
    bool Attached() const { return m_hwnd != NULL; }
    void Focus(bool on);                                     /* 输入框聚焦/失焦: 聚焦=重启闪烁; 失焦=熄光标停表 */
    void Reset();                                            /* 光标重新可见 (按键/点选后, 原生 EDIT 口径) */
    bool On() const { return m_focus && m_active && m_blink; }   /* 渲染: 本帧是否画光标 */
    void Tick();                                             /* 翻转相位 + 重绘 (仅"该显示"时; 失焦/失活期停摆) */
    static void TickWindow(HWND hwnd);                       /* WM_TIMER(ID_TIMER_CARET) 分发入口 */
    static void SetWindowActive(HWND hwnd, bool active);     /* WM_ACTIVATE 入口: 广播同窗全部实例 */
    static void DetachWindow(HWND hwnd);                     /* WM_DESTROY 入口: 摘除同窗全部实例并停表 */
private:
    void RestartTimer();                                     /* 按可见性重算该窗计时器 (窗口级裁决) */
    static bool AnyWants(HWND hwnd);                         /* 该窗上是否还有实例想显光标 (聚焦&&激活) */
    static void ApplyTimer(HWND hwnd);                       /* 有则 SetTimer, 无则 KillTimer (同窗单计时器) */
    HWND m_hwnd = NULL;
    std::function<void()> m_repaint;
    bool m_focus = false;     /* 输入框聚焦 */
    bool m_active = true;     /* 承载窗口激活态 (初始为真; 首个 WM_ACTIVATE 校正, 避免漏消息=光标永不闪) */
    bool m_blink = true;      /* 闪烁相位 (true=本帧显光标) */
};

/* ==================== 输入字段组件 (XjsLineEdit+XjsCaretBlink 组合, 任意宿主窗口可嵌) ====================
 * XjsLineEdit 本就只依赖宿主给的 矩形/RT/格式/画刷, 但 键盘/IME/I-beam/闪烁 的"路由"此前
 * 散在搜索窗 WndProc 手写 — 每个新宿主都得重抄一遍。本组件把 路由+注册表 收口, 宿主接线:
 *   1. 成员 XjsEditField f; 首用 f.Attach(hwnd, 重绘回调);
 *   2. WM_PAINT 布局得矩形 → f.Render(rt, rect, fmt, 画刷×3, placeholder, 占位刷) (每帧回写矩形/格式);
 *   3. WM_LBUTTONDOWN → XjsEditFieldMouseDown(hwnd, pt) (点进=聚焦+点定位+取捕获, 点外=失焦, 真=吃掉);
 *      WM_MOUSEMOVE/WM_LBUTTONUP/WM_LBUTTONDBLCLK → XjsEditFieldMouseMove/Up/DoubleClick (拖选/收尾/选整词, 真=已消费);
 *      WM_RBUTTONDOWN → XjsEditFieldContextMenu(hwnd, pt) (字段内=编辑菜单, 真=吃掉);
 *      WM_POPUP_RESULT (IDM_FCTX_BASE..+4) → XjsEditFieldMenuCmd(hwnd, id-基址);
 *      WM_SETCURSOR → XjsEditFieldHit(hwnd, pt) 真=I-beam;
 *   4. WM_KEYDOWN/WM_CHAR/WM_IME_CHAR → XjsEditRouteMsg(hwnd,msg,wp,lp) (真=已消费;
 *      宿主语义键 Enter/Esc 先于路由自行处理), WM_IME_COMPOSITION → XjsEditRouteImeResult;
 *   5. WM_TIMER(ID_TIMER_CARET) → XjsCaretBlink::TickWindow(hwnd); WM_ACTIVATE → SetWindowActive;
 *   6. WM_DESTROY → XjsEditFieldCleanupWindow(hwnd) (字段析构亦自动摘除)。 */
struct XjsEditField {
    XjsLineEdit ed;
    XjsCaretBlink blink;
    bool focused = false;
    XjsRect area{};                  /* 最近一帧渲染矩形 (命中/IME 定位; Render 每帧回写) */
    XjsFormat* fmt = NULL;

    void Attach(HWND hwnd, std::function<void()> repaint);
    void Detach();                       /* 解绑+摘注册表 (析构自动调) */
    void SetFocused(HWND hwnd, bool on); /* 聚焦/失焦 (同窗其余字段自动失焦) */
    void Render(XjsRt* rt, const XjsRect& r, XjsFormat* f,
                XjsBrush* textBr, XjsBrush* selBr, XjsBrush* caretBr,
                const wchar_t* placeholder, XjsBrush* phBr, XjsBrush* sbBr = NULL,
                XjsBrush* sbHoverBr = NULL) {
        area = r; fmt = f;
        ed.Render(rt, r, f, textBr, selBr, caretBr, placeholder, phBr, focused && blink.On(), sbBr, sbHoverBr);
    }
    bool UpdateSbHover(POINT pt) { return ed.UpdateSbHover(pt, area, fmt); }   /* 多行滚动条悬停命中 (矩形/格式取最近帧渲染回写, 变化返回真) */
    ~XjsEditField() { Detach(); }
};
XjsEditField* XjsEditFocused(HWND hwnd);                  /* 该窗当前聚焦字段 (无=NULL) */
bool XjsEditFieldHit(HWND hwnd, POINT pt);                /* 任一字段矩形命中 (I-beam 判定) */
bool XjsEditFieldMouseDown(HWND hwnd, POINT pt);          /* 点进=聚焦+定位+SetCapture, 点外=失焦 (真=吃掉点击) */
bool XjsEditFieldMouseMove(HWND hwnd, POINT pt);          /* 拖选转发 (真=有字段拖选中, 已消费) */
bool XjsEditFieldMouseUp(HWND hwnd);                      /* 拖选收尾+释放捕获 (真=已消费) */
bool XjsEditFieldDoubleClick(HWND hwnd, POINT pt);        /* 双击选整词 (点在字段上真=吃掉) */
bool XjsEditFieldWheel(HWND hwnd, int delta, bool horizontal, const POINT& pt);  /* 多行字段滚轮滚动 (优先聚焦字段, 否则光标悬停字段; 真=已消费) */
bool XjsEditFieldContextMenu(HWND hwnd, POINT pt);        /* 字段内右键=编辑菜单 (真=字段命中, 已吃掉) */
void XjsEditFieldMenuCmd(HWND hwnd, int cmd);             /* 菜单结果落地到该窗聚焦字段 (WM_POPUP_RESULT 分发) */
bool XjsEditRouteMsg(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);  /* WM_KEYDOWN/WM_CHAR/WM_IME_CHAR/WM_IME_STARTCOMPOSITION (真=已消费) */
bool XjsEditRouteImeResult(HWND hwnd, LPARAM lParam);     /* WM_IME_COMPOSITION: GCS_RESULTSTR 整串上屏 */
bool XjsEditFieldAnchorUpdate(HWND hwnd);                 /* 把 IME 组字/候选窗钉到该窗聚焦字段 (真=有聚焦字段) */
void XjsEditFieldCleanupWindow(HWND hwnd);                /* 宿主销毁: 摘该窗全部字段登记+闪烁驱动 */
void XjsEditFieldsDropFormats();                          /* 文本格式全量重建时作废字段缓存 fmt (悬垂防线) */

/* 宿主窗口销毁时的组件统一退登记 (搜索窗/设置窗共用同一入口, 别在各自的 WM_DESTROY 里各写一串):
   输入字段登记 (路由层命中/聚焦/拖选表) + 光标闪烁驱动器 + Toast 条目/画刷。
   漏一处 = 登记表留该窗字段指针 (HWND 一旦被系统复用即 UAF) 与画刷泄漏 */
void XjsWindowComponentsDetach(HWND hwnd);

/* 添加/编辑搜索模式 对话框状态 (主窗模态遮罩; 归所属窗口类 — 用户红线: 状态绑窗不落全局) */
struct XjsModeDlg {
    bool open = false;
    bool edit = false;               /* 编辑态: 标题"编辑搜索模式"/按钮"保存"/含删除 */
    std::wstring editId;
    int type = 0;                    /* 0通配符 1正则 2SQL 3Lua (默认通配符, 2026-09-16 用户口径) */
    int scope = 0;                   /* 作用范围段: 0=全局共享 1=仅本窗口 (新增默认全局, 2026-09-17) */
    std::wstring ownerName;          /* 打开对话框的窗口档案名 (scope=1 绑定/提示用; 打开时捕获, 不依赖"当前窗") */
    bool delConfirm = false;
    int pressCmd = 0;               /* 按下待定 (松开触发口径): 0=无; 1..5=类型段 6+i=作用范围段 8=取消 9=保存 10=删除 */
    XjsEditField nameEd, descEd, tplEd;
};

/* 预览面板内命中区域 (渲染时填写, 命中测试读取; 每窗一份) */
struct XjsPreviewHits {
    XjsRect lockBtn{}, maxBtn{}, closeBtn{};
    XjsRect copySerial{};
    XjsRect bigDirs{}, bigFiles{};
    XjsRect locate{}, open{};
    bool valid = false;
};

/* 搜索模式 (keywordType 强制指定; AUTO 已移除) — 每窗可各自选择。
   LUA=脚本过滤 (每文件求布尔谓词, XJS_KEYWORD_LUA); LUA_EXEC=执行 (脚本即程序,
   自主遍历/排序, return ID 数组=结果, XJS_KEYWORD_LUA_EXEC; 不进多重搜索链) */
enum XjsSearchMode { XMODE_WILDCARD = 0, XMODE_REGEX = 1, XMODE_SQL = 2, XMODE_LUA = 3, XMODE_LUA_EXEC = 4 };

/* 用户自定义搜索模式 (源样式 11-search-modes.js: "添加搜索模式"弹窗创建, 上限 100)
 * 存储: 共享 = 顶层 "共享搜索模式"; 私有 = 各窗口条目的 "私有搜索模式" (存储位置即作用域)。
 * 模板里的 <keyword> (大小写不敏感) 提交前替换为搜索框纯输入文字 (不含标签); 占位符消费掉框内文字时
 * 不再追加链尾阶段, 否则 = [模板阶段 + 输入词阶段(本窗搜索模式)] 多重搜索 */
struct XjsCustomMode {
    std::wstring id;        /* "m"+时间戳36进制 (源样式同款) */
    std::wstring name;      /* 模式名称 (必填) */
    std::wstring desc;      /* 简介 */
    std::wstring type;      /* wildcard / regex / sql / lua */
    std::wstring tpl;       /* 模板内容 (必填; <keyword> = 搜索框纯输入文字的占位符) */
    int scope = 0;          /* 作用范围 (2026-09-17): 0=全局共享(默认,全部窗口生效) 1=仅本窗口(ownerName 窗口生效) */
    std::wstring ownerName; /* scope=1: 所属窗口档案名 (按名持久绑定: 关窗重开仍生效; 改名随迁/删档连带删) */
};
extern std::vector<XjsCustomMode> g_customModes;
/* 作用范围判定与窗口档案联动 (xjs_engine.cpp; winName = 窗口档案名) */
bool XjsCmApplies(const XjsCustomMode& cm, const std::wstring& winName);   /* 模式对该窗口是否生效 */
int  XjsCmWindowCount(const std::wstring& winName);   /* 该档案名下专属模式数 (删除确认提示用) */
int  XjsCmPurgeWindow(const std::wstring& winName);   /* 删除档案: 连带删除其专属模式 (返回剔除数) */
void XjsCmRenameWindow(const std::wstring& oldName, const std::wstring& newName);  /* 窗口改名: 专属模式归属随迁 */
/* 药丸模式菜单打开时的自定义模式 id 快照 (xjs_chrome.cpp): 菜单按作用范围过滤后下标≠全局下标,
   且菜单开着时容器可能被其它窗口改动 — 回传统一按 id 定位 (菜单鼠标互斥, 全进程单份快照) */
std::wstring XjsCmodeMenuIdAt(int idx);   /* 越界=空串 */

/* 搜索框托管标签 (源样式 26-hosted-search.js 一比一: 标签链多重搜索, dome 标签来源=自定义搜索模式):
 * 输入词命中模式名 → 自动转成搜索框标签; 搜索 = 各标签(按加入序, 各取当前来源) + 输入文字尾阶段
 * 组成引擎多重搜索 (XJS_KEYWORD_MULTI 链式过滤; 模板含 <keyword> 时替换为框内纯文字且不再加尾阶段)。
 * 来源只存模式 id, 执行/显示时活取 (编辑模式后按新模板重搜; 模式被删 → 来源剔除, 剔空整标签移除)。 */
struct XjsHostedTag {
    std::wstring word;              /* 显示词 (= 命中的模式名, 链内忽略大小写唯一) */
    std::vector<std::wstring> srcIds;   /* 同名来源 (customSearchModes 的 id, ≥1) */
    int active = 0;                 /* 当前生效来源下标 */
    double born = 0;                /* 入场动画起点 (秒, GetTickCount64 口径; 源样式 hostedTagIn .22s) */
};
void XjsHostedExecChain(const std::wstring& tailKw);   /* 标签链(+尾阶段)执行; 无标签时清标签回普通搜索 */
bool XjsHostedTrySearch(const std::wstring& keyword);  /* 输入词命中模式名 → 转标签接管 (真=已接管) */
bool XjsHostedWordAsTag(const std::wstring& word, bool clearInput); /* 词转标签 (输入命中/模式菜单点击共用) */
void XjsHostedRemoveTag(int idx);                      /* 移除标签并重搜剩余链 (无标签回普通搜索) */
void XjsHostedClearTags();                             /* 静默清空标签 (不触发搜索) */
void XjsHostedPurgeMode(const std::wstring& modeId);   /* 模式删除: 剔除其来源并重搜 (源样式 purgeHostedSources) */
void XjsHostedPickSource(int srcIdx);                  /* 来源切换菜单命令: 换当前来源并重搜整链 */
int  XjsHostedPrune();                                 /* 剔除已删模式来源 (真=有变化); 布局/执行前调 */
/* 搜索框标签链 UI (xjs_chrome.cpp): 布局回写矩形 / 绘制 / 命中 / 键盘穿行 / 悬停菜单计时 */
float XjsHostedLayout();                 /* 计算标签矩形并右移 g_layout.editRect 左缘, 返回占用宽度 */
void XjsHostedRender();                  /* 绘制标签链 + 标签光标条 (XjsChromeRenderTitlebar 内) */
bool XjsHostedMouseDown(POINT pt, int* pressTagOut = NULL);   /* 标签区命中: ×记待定(pressTagOut), 卡体吃点击 (真=已消费) */
bool XjsHostedMouseUp(POINT pt, int pressTag);                /* × 松开触发: 按下/松开同一 × 才移除 */
bool XjsHostedKey(WPARAM vk);            /* ←→ 穿行 / Backspace·Delete 删标签 / 其它键退出模式 */
bool XjsHostedInCaretMode();             /* 标签光标模式 (输入框光标隐藏, 光标条显示) */
void XjsHostedExitCaret();               /* 退出标签光标模式 (光标回输入框) */
void XjsHostedMouseMove(POINT pt);       /* 多来源标签悬停计时 (180ms 弹来源菜单) */
void XjsHostedHoverTimer(HWND hwnd);     /* WM_TIMER(ID_TIMER_HOSTEDSRC): 弹"切换搜索来源"菜单 */
void XjsHostedHoverReset(HWND hwnd);     /* 鼠标离开宿主: 清悬停态并停菜单计时 (WM_MOUSELEAVE) */

/* 添加/编辑搜索模式 对话框 (主窗模态遮罩, xjs_chrome.cpp) */
void XjsModeDlgOpen(bool edit, const std::wstring& editId);   /* edit=编辑态 (预填 editId 条目) */
bool XjsModeDlgActive();
bool XjsModeDlgMouseDown(POINT pt);                           /* 真=吃掉点击 (命令记待定) */
bool XjsModeDlgMouseUp(POINT pt);                             /* 命令控件松开触发 (按下待定校验) */
bool XjsModeDlgKey(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);  /* 键盘/IME 直通, 真=已消费 (模态) */
void XjsModeDlgRender(XjsRt* rt, float w, float h);

/* Toast 类型 (源样式 20-toast.js showToast: error 失败红 / warn 警告橙 / success 成功绿 / info 信息蓝) */
enum XjsToastType { XTOAST_INFO = 0, XTOAST_SUCCESS, XTOAST_WARN, XTOAST_ERROR };

/* Toast 弹出位置 (组件参数; BOTTOM_RIGHT 底 46 = 源样式口径, 为搜索窗状态栏让位, 其余 16 内边距) */
enum XjsToastPos {
    XTOPOS_BOTTOM_RIGHT = 0, XTOPOS_TOP_RIGHT, XTOPOS_TOP_LEFT, XTOPOS_BOTTOM_LEFT,
    XTOPOS_TOP_CENTER, XTOPOS_BOTTOM_CENTER, XTOPOS_COUNT
};

/* 一条 Toast 卡片 (窗口右下角内嵌, 定时自动关闭, 非模态不阻塞; 源样式同款) */
static const int XJS_TOAST_MAX = 4;   /* 同屏上限, 超出移除最旧 (源样式同款) */
struct XjsToastItem {
    std::wstring text;
    int type = XTOAST_INFO;
    int pos = XTOPOS_BOTTOM_RIGHT;  /* 弹出停靠位 (同位聚拢成栈, 最新贴锚点) */
    double start = 0;        /* 入场时刻 (秒, GetTickCount64 口径) */
    float duration = 4;      /* 展示时长: error 默认 6s 其余 4s (源样式同款), 可显式覆盖 */
    bool leaving = false;    /* 离场动画中 (到期或点关闭, 0.18s 后移除) */
    double leaveStart = 0;
    bool copied = false;     /* 复制按钮已点 (图标变绿对勾 1.2s) */
    double copiedAt = 0;
};

/* 列表悬停高亮渐隐拖尾: 鼠标移走的行高亮不立即消失, 按 XJS_HOVER_FADE_MS 衰减 (停留行满亮) */
struct XjsHoverTrace {
    int row = -1;        /* 衰减中的行下标 */
    double start = 0;    /* 开始衰减时刻 (GetTickCount64 毫秒) */
};

/* 搜索匹配设置 (源样式 搜索匹配 组, 键名与 DLL SetSearchSettings 中文键一致):
   每窗一份 (2026-09-17 用户口径: 不同的窗口允许不同的匹配设置), 随 uiWindows 档案持久化 */
struct XjsMatchSettings {
    bool pinyinFull = true;        /* 支持全拼 */
    bool pinyinInitial = true;     /* 支持首拼 */
    bool pinyinExact = false;      /* 拼音完整匹配 */
    bool wildcardStar = true;      /* 通配符支持 * */
    bool wildcardQuestion = true;  /* 通配符支持 ? */
    bool matchFullWidth = false;   /* 匹配全角 */
    bool caseSensitive = false;    /* 区分大小写 */
    bool emptyShowsAll = true;     /* 搜索词为空时显示所有文件 */
};

/* 窗口激活或创建时位置 (每窗设置 appearPos): 档位序 = 设置菜单顺序, 下拉文字/落点计算/配置读写
 * 三处同源 (XjsAppearPosLabel 给文字, ApplyAppearPos 给落点, K_APPEAR 给持久化)。
 * 一切落点夹在显示器工作区 (rcWork) 内 —— 不覆盖任务栏 (贴角/贴光标越界同样收回)。
 * 档位 = 屏组 (主屏 / 鼠标所在屏幕) × 五个落点 (居中/左上/右上/左下/右下), 见 XJS_SPOT_*。 */
enum XjsAppearScreen { XJS_SCR_PRIMARY = 0, XJS_SCR_CURSOR = 1, XJS_SCR_COUNT };
enum XjsAppearSpot { XJS_SPOT_CENTER = 0, XJS_SPOT_LT = 1, XJS_SPOT_RT = 2, XJS_SPOT_LB = 3, XJS_SPOT_RB = 4,
                     XJS_SPOT_COUNT };
enum {
    XJS_APPEAR_NONE = 0,        /* 之前的位置: 不动 (子窗按档案记忆的矩形恢复) */
    XJS_APPEAR_FOLLOW = 1,      /* 跟随鼠标: 左上角贴光标右下 +12px */
    XJS_APPEAR_SCREEN_BASE = 2, /* 屏幕落点档起点 = BASE + 屏组×XJS_SPOT_COUNT + 落点序 */
    XJS_APPEAR_COUNT = XJS_APPEAR_SCREEN_BASE + XJS_SCR_COUNT * XJS_SPOT_COUNT   /* 菜单 id 段宽, 见 xjs_settings.cpp ACT_APPEAR */
};

/* 每窗界面设置档案 (xjs_config.json 顶层 "窗口" 数组, 下标=档案槽):
 * 绑定窗口的私有配置 (皮肤/视图/预览/列布局/搜索设置/打开行为/列表框行为/窗口行为) 全在各自
 * 条目的 "窗口名称" 之下; 开机启动/页面缩放/双击Ctrl目标 等共享项在顶层独立键 */
/* 语言枚举 (i18n, 实现在 xjs_engine.cpp; 声明在此因 窗口/档案 的 lang 字段默认值要用) */
enum XjsLang { XLANG_AUTO = -1, XLANG_ZH = 0, XLANG_ZHTW, XLANG_EN, XLANG_KO, XLANG_TH, XLANG_MS, XLANG_N };

struct XjsUiProfile {
    std::wstring skin = L"dark";    std::wstring name;                    /* 窗口名称 (主窗恒为 XJS_MAIN_WIN_NAME, 不以此为准) */
    UINT hotkeyMod = 0;                   /* 全局快捷键修饰键 (0=未设置; 无内置默认, 设置页录制才有) */
    UINT hotkeyVk = 0;                    /* 全局快捷键主键 */
    XjsViewMode viewMode = VM_LIST;
    int mode = XMODE_WILDCARD;            /* 搜索模式 (每窗, 条目"搜索模式"; 曾为顶层共享键, 2026-09-18 每窗化) */
    bool previewVisible = true;
    int previewWidth = 400;
    bool openElevated = false;            /* 打开文件: 继承管理员权限 (runas), 默认关 */
    bool openAsync = true;                /* 打开文件: 异步线程执行 (防卡主线程), 默认开 */
    bool openHideWindow = false;          /* 打开文件后隐藏窗口到托盘, 默认关 */
    XjsMatchSettings match;               /* 搜索匹配 ×8 (搜索设置) */
    std::vector<std::wstring> history;    /* 搜索历史 (每窗, 条目"搜索历史"; 曾为进程共享) */
    bool driveProgress = true;            /* 绘制驱动器占用进度条 (列表框) */
    bool rowHover = true;                 /* 高亮鼠标经过行 (列表框) */
    bool rowHoverFade = true;             /* 鼠标经过残影 (列表框; 依赖 rowHover) */
    /* 窗口行为 (每窗, 2026-09-17): 失焦动作 / 激活与创建时的位置 / 标题栏·状态栏可见性 / 置顶 */
    int blurAction = 0;                   /* 窗口失去焦点: 0=无 1=关闭窗口 (统一策略: 主窗藏托盘/子窗销毁) */
    int appearPos = 0;                    /* 窗口激活或创建时位置 (档位序见 XJS_APPEAR_*) */
    bool showCtrlBtns = true;             /* 显示控制按钮 (最小化/最大化/关闭; 隐藏后左侧元素填充) */
    bool showFilterBox = true;            /* 显示筛选框 (标题栏筛选下拉; 隐藏后搜索框填充) */
    bool showStatusbar = true;            /* 显示状态栏 */
    bool taskbarIcon = true;              /* 任务栏图标 (关 = WS_EX_TOOLWINDOW: 不在任务栏/Alt+Tab 显示) */
    int mouseOpen = 0;                    /* 鼠标打开文件: 0=双击 1=单击 */
    int defaultSel = 0;                   /* 默认选中表项: 0=不选 1=结果刷新后自动选中第一个 */
    int uiZoom = 10;                      /* 页面缩放 (十分位: 5..20 = 50%..200%) */
    int lang = XLANG_AUTO;                /* 界面语言 (每窗, 2026-09-19 每窗化; 条目键 "语言"; auto=按系统) */
    int createFill = 0;                   /* 创建窗口填入搜索框: 0=清空 1=用户指定关键词 2=上一次输入的搜索词 */
    std::wstring createKeyword;           /* createFill=1 的用户指定关键词 */
    std::wstring lastSearch;              /* 上一次输入的搜索词 (关窗时随档案记忆, createFill=2 用) */
    std::wstring sortField;               /* 默认排序字段 (空=引擎默认评分序; 排序态事实源仍是结果对象) */
    bool sortWay = false;                 /* 默认排序方向: true=升序 */
    bool topmost = false;                 /* 置顶 (标题栏图钉态持久化) */
    bool rectValid = false;               /* 窗口矩形 (子窗按档案槽记忆"之前的位置"; 主窗仍走顶层 窗口矩形 键) */
    int rx = 0, ry = 0, rw = 0, rh = 0, rdpi = 96;
    XjsColumnSet colsDetails;             /* 详情视图列 (8 列) */
    XjsColumnSet colsList;                /* 列表视图列 (9 列) */
};

#define XJS_MAIN_WIN_NAME  L"默认窗口"   /* 第一个窗口 (主窗) 的固定名称, 不可重命名 */

/* ==================== 每窗上下文 (多搜索窗口) ====================
 * 一个进程多个搜索窗口: 共享一个引擎/索引/皮肤/文本格式/列配置, 每窗独立持有
 * 搜索结果对象 (xjs_result) 与全部界面状态 (D2D 设备/选择/滚动/预览/重命名…)。
 * 原 g_* 每窗全局改为 XjsSearchWindow 字段, 经宏 (XjsSearchWindow::Cur()->…) 重定向, 既有代码零改动。
 * "当前窗"在主窗 WndProc 入口按 hwnd 绑定; 引擎线程回调禁用 Cur(),
 * 一律经 OfResult(结果对象→所属窗) / MainHwnd(引擎级事件) 显式定位。 */
class XjsSearchWindow {
public:
    bool isMain = false;         /* 主窗: 托盘/热键/引擎生命周期/配置归属 */
    int uiIndex = 0;             /* 绑定的 uiWindows 档案槽 (持久: 关窗档案保留, 菜单可按槽位重建窗口;
                                    主窗恒槽 0; 不再是存活序号, 窗口无编号只有名称) */
    std::wstring name;           /* 窗口名称 (显示用身份; 主窗固定 XJS_MAIN_WIN_NAME, 其余默认 GUID, 可重命名) */
    /* 全局快捷键 (每窗, 随 uiWindows 档案持久化; vk=0 = 未设置, 不注册。
       2026-09-17 用户口径: 无内置默认热键, 程序不自动注册 — 只有设置页录制过的才注册,
       录制时立即注册, 录制中按 Delete 清除并取消注册): 全部登记在主窗 hwnd 上,
       id = ID_HOTKEY_SHOW+槽位, WM_HOTKEY 按槽分发 */
    UINT hotkeyMod = 0;
    UINT hotkeyVk = 0;
    bool hotkeyActive = false;   /* 当前热键注册成功态 (设置行警告态显示用) */
    /* 窗口行为 (每窗, 随 uiWindows 档案持久化; 默认值同 XjsUiProfile 同名字段) */
    int blurAction = 0;          /* 窗口失去焦点: 0=无 1=关闭窗口 */
    int appearPos = 0;           /* 窗口激活或创建时位置 (档位序见 XJS_APPEAR_*) */
    bool showCtrlBtns = true;    /* 显示控制按钮 (最小化/最大化/关闭) */
    bool showFilterBox = true;   /* 显示筛选框 (标题栏筛选下拉) */
    bool showStatusbar = true;   /* 显示状态栏 */
    bool taskbarIcon = true;     /* 任务栏图标 (关 = WS_EX_TOOLWINDOW: 不在任务栏/Alt+Tab 显示) */
    int mouseOpen = 0;           /* 鼠标打开文件: 0=双击 1=单击 (单击模式 Ctrl/Shift+点击仍为多选) */
    int defaultSel = 0;          /* 默认选中表项: 0=不选 1=结果刷新后自动选中第一个 */
    bool selFirstPending = false; /* 选中首项待落地 (运行期瞬态, 不入档案): 搜索框回车/↓提交了新查询, 新结果集就绪时选中首项 */
    int rowNullLast = -1;        /* 行数据空行数收敛重试的记忆值 (运行期瞬态): 空行数变化才择机重绘, 每窗私有防互抑 */
    int uiZoom = 10;             /* 页面缩放 (十分位: 5..20 = 50%..200%; 每窗私有, 文本格式随窗重建) */
    int lang = XLANG_AUTO;       /* 界面语言 (每窗私有; auto=按系统 UI 语言) */
    int createFill = 0;          /* 创建窗口填入搜索框: 0=清空 1=用户指定关键词 2=上一次输入的搜索词 */
    std::wstring createKeyword;  /* createFill=1 的用户指定关键词 */
    std::wstring skinName = L"dark";   /* 本窗皮肤 (每窗可不同; g_skin 为当前窗镜像) */
    /* 打开文件行为 (每窗配置, 随 uiWindows 档案持久化; 默认值见 XjsUiProfile 同名字段) */
    bool openElevated = false;
    bool openAsync = true;
    bool openHideWindow = false;
    XjsMatchSettings match;      /* 搜索匹配 ×8 (每窗, 随 uiWindows 档案持久化) */
    std::vector<std::wstring> history;   /* 搜索历史 (每窗; 曾为进程共享 g_history) */
    /* 列表框行为 (每窗, 随 uiWindows 档案持久化) */
    bool driveProgress = true;   /* 绘制驱动器占用进度条 */
    bool rowHover = true;        /* 高亮鼠标经过行 */
    bool rowHoverFade = true;    /* 鼠标经过残影 (渐隐拖尾; 依赖 rowHover) */
    HWND hWnd = NULL;
    HFONT hFontEdit = NULL;      /* 仅 IME 组字窗口字体 (每窗各自 DPI) */
    bool sysCaretMade = false;   /* IME 锚点隐藏系统光标 (挂在本窗上) */
    XjsColumnSet colsDetails;   /* 列布局每窗独立 (字段名持久化) */
    XjsColumnSet colsList;

    /* D2D 设备: RT/画刷/位图绑定渲染目标, 必须每窗一份 (跨 RT 用 = WRONG_RESOURCE_DOMAIN 丢帧) */
    bool needFullPaint = true;
    XjsRt* rt = NULL;
    XjsHwndRt* hwndRt = NULL;
    XjsDc* dc = NULL;
    XjsSolidBrush* br[XTH_COUNT] = {};
    XjsSolidBrush *brWhite = NULL, *brCloseHover = NULL, *brErr = NULL;
    XjsGradBrush *brSelGrad = NULL, *brSelBar = NULL, *brProgress = NULL;
    XjsBitmap* appIcon = NULL;                       /* 标题栏图标 (按窗懒解码, 绑本窗 RT) */
    std::unordered_map<unsigned int, XjsSolidBrush*> brushCache;   /* XjsTempBrush 按色缓存 */

    /* 布局/DPI (窗口可在不同显示器) */
    XjsLayout layout;
    float dpiS = 1.0f;

    /* 搜索结果对象 (每窗独立; 引擎共享) */
    xjs_result* result = NULL;
    int resultCount = 0;
    int syncWatchCount = -1;                /* 同步轮询基线: 上次已刷新的结果数量 (UI线程专用) */
    std::atomic<int> searchFingerprint{-1};
    std::atomic<bool> searching{false};     /* 搜索防抖: 提交→完成间冻结列表快照 */
    std::atomic<bool> fileChangePending{false};  /* 文件变化待刷新 (源样式 m_文件变化待刷新, 绑窗成员): 结果变化回调置位, ID_TIMER_SYNCWATCH 时钟节流消费重绘 */
    std::atomic<int> uiPostPending{0};      /* 已投给本窗、未处理的引擎消息数 (闸门每窗账): DestroyWindow 会整批清除队列里
                                               本窗的未处理消息, 全局闸门计数按此账返还 (XjsPostToUiFor/DropWindow) */
    std::vector<int> debounceIds;
    std::vector<unsigned char> debounceSel;  /* 防抖快照选中位 (与 debounceIds 一一对应): 提交查询时引擎清空选中, 冻结期选中态随快照画 */
    int debounceFirst = 0;
    int debounceCount = 0;
    int visFirst = 0;      /* 最近一帧可见行区间 (图标按需闸门) */
    int visLast = -1;
    std::wstring statusText = L"正在初始化蜗牛快搜引擎…";
    std::wstring errText;
    int mode = XMODE_WILDCARD;              /* 搜索模式 (每窗可不同) */
    XjsViewMode viewMode = VM_LIST;

    /* 搜索框 */
    bool searchFocused = false;
    XjsLineEdit searchEd;
    XjsCaretBlink searchBlink;
    int imeUpdBusy = 0;                     /* IME 重入守卫 */
    XjsModeDlg modeDlg;                     /* 添加/编辑搜索模式 对话框 (模态; 状态归窗口) */

    /* 托管标签链 (源样式 26-hosted-search.js; 每窗一份, 不持久化 — 源样式页面重载即清零) */
    std::vector<XjsHostedTag> hostedTags;   /* 标签 (按加入顺序; word 忽略大小写唯一) */
    int hostedCaret = 0;                    /* 标签光标插入位 (0..count; <count=光标条模式) */
    std::vector<XjsRect> hostedRects;   /* 标签矩形 (每帧布局回写, 下标对齐 hostedTags) */
    XjsRect hostedCaretRect {};         /* 标签光标条矩形 (布局回写) */
    XjsRect hostedRegion {};            /* 标签链绘制区域 (溢出截断裁剪; 布局回写) */
    int hostHoverTag = -1;                  /* 悬停的标签下标 (多来源标签悬停 180ms 弹来源菜单) */
    int hostHoverBtn = 0;                   /* 悬停的标签子区 (0=卡体 1=×钮) */
    int hostMenuTag = -1;                   /* 来源菜单打开时的标签下标 (弹出后悬停已被 MOUSELEAVE 清掉) */

    /* 行内重命名 (F2) */
    bool renActive = false;
    int renIdx = -1;
    XjsLineEdit renEd;
    XjsCaretBlink renBlink;
    std::wstring renOldPath, renOldName;
    bool renIsDir = false;

    /* 列表状态
       (选中集合无宿主副本: 权威在引擎 xjs_result_Select*, 渲染/命令按需回读 —
        见 xjs_list.cpp "选中" 节 XjsSel*) */
    std::unordered_set<int> cutSet;         /* 剪切灰显行集 */
    std::vector<XjsHoverTrace> hoverTraces; /* 悬停高亮渐隐拖尾 (移走的行逐帧衰减, 上限16) */
    int anchorIdx = -1;
    int focusIdx = -1;
    double scrollTop = 0;
    bool topmost = false;                   /* 置顶 (每窗; 标题栏图钉) */
    std::unordered_map<int, XjsRowData> rowCache;
    std::unordered_map<unsigned long long, XjsBitmap*> iconCache;   /* 图标位图缓存 (fileId<<8|像素档): 整窗重绘的性能前提, RT重建/设备丢弃/重建索引时全清 */
    /* 排序字段/方向不落宿主状态 — 以各窗结果对象为准 (xjs_result_GetSortField/GetSortway), 宿主只下发 SetSortField */
    int filterSel = 0;                      /* 筛选分类选中下标 (每窗; 对齐 DLL 结果对象各自的 SetSelectedFilter) */
    double hScroll = 0;
    bool dragHScroll = false;
    double hScrollGrab = 0;

    /* 鼠标/拖拽/悬停 */
    int hoverBtn = 0;
    int hoverWndBtn = 0;
    int hoverStatus = -1;
    bool listHover = false;
    int hoverRow = -1;
    int hoverHandle = -1;                   /* 悬停的列宽调整边界下标 (-1=无) */
    int sbHover = 0;                        /* 悬停的滚动条 (0=无 1=纵向 2=横向; thumb 提亮一档; 纯悬停无捕获, 必须每窗) */
    bool modeMenuOpen = false;
    bool appMenuOpen = false;
    bool dragScroll = false;
    float scrollGrab = 0;
    bool dragCol = false;
    int dragColIdx = -1;
    float dragColX = 0;
    bool marquee = false;
    bool marqueeMoved = false;
    float marqueeStartX = 0;
    double marqueeStartY = 0;
    XjsRect marqueeRect = {};
    bool mouseTracking = false;

    /* Toast 通知: 已组件化到 xjs_toast.cpp (按宿主 HWND 挂载, 栈/悬停/画刷随组件走) */

    /* 预览面板 */
    bool previewVisible = true;             /* 正式版 settings.preview=true */
    int previewWidth = 400;
    bool previewLocked = false;
    int previewFileId = -1;
    XjsBitmap* previewImage = NULL;       /* 图片预览位图 (绑本窗 RT) */
    int previewImageFileId = -1;
    bool previewDrag = false;
    XjsPreviewHits previewHits;             /* 面板命中区域 (渲染填写) */
    float previewImgZoom = 1.0f;            /* Ctrl+滚轮图片缩放 (切换文件复位) */
    std::vector<std::wstring> previewTextLines;   /* 文本预览行缓存 */
    int previewTextFileId = -1;
    double previewTextScroll = 0;

    /* 插件面板接管 (preview-panel 能力, AI 助手等; 实现收口 xjs_preview.cpp "面板接管"节。
       会话状态必须住窗口类 (可维护性红线: 禁按 hwnd 平行散表); 位图暂存经 s_panelCs 保护 —
       插件工作线程可随时交付, 渲染帧快照拷贝) */
    bool plugPanelOn = false;               /* 接管会话激活 (预览面板内容区由插件交付) */
    std::wstring plugPanelPluginId;         /* 接管插件 id (派发按 id 找插件, 重扫换槽不串窗) */
    bool plugPanelWasVisible = false;       /* 打开时预览面板原状态 (关闭恢复: 原本关着就连预览一起关) */
    long long plugPanelSerial = 0;          /* 面板世代 (尺寸/位置/缩放变化递增; 插件交付按它对齐, 过期静默丢弃) */
    int plugPanelX = 0, plugPanelY = 0;     /* 当前世代期望像素原点 (内容区, 客户区坐标; 只比宽高会漏"宽度不变位置平移"的失配) */
    int plugPanelW = 0, plugPanelH = 0;     /* 当前世代期望像素尺寸 (内容区, 物理像素) */
    float plugPanelScale = 1.0f;            /* 当前世代 dpi×页面缩放 (96dpi=1.0) */
    bool plugPanelKey = false;              /* 插件持有键盘 (面板输入框聚焦; 键盘/IME 路由让给面板) */
    bool plugPanelCapture = false;          /* 面板鼠标捕获中 (拖拽出面板也持续转发 move/up) */
    bool plugPanelResyncPosted = false;     /* 尺寸失配重同步已投递 (WM_PANEL_RESYNC 防重复投) */
    int plugPanelCaretX = 0, plugPanelCaretY = 0;  /* IME 锚点 (面板内坐标, PanelSetCaret 写) */
    std::vector<uint8_t> plugPanelBmp;      /* 交付字节暂存 (BGRA, s_panelCs 内换血) */
    int plugPanelBmpW = 0, plugPanelBmpH = 0, plugPanelBmpStride = 0;
    unsigned long long plugPanelRev = 0;    /* 交付序号 (每成功交付 ++, 渲染缓存重建判定) */
    XjsBitmap* plugPanelCache = NULL;       /* 交付字节 → 本域位图缓存 (懒建; 绑建它那一刻的 RT) */
    XjsRt* plugPanelCacheRt = NULL;
    unsigned long long plugPanelCacheRev = 0;

    ~XjsSearchWindow();
    XjsSearchWindow();           /* 列布局等数组字段从内置默认表拷贝 */

    /* ---- 静态入口: 窗口数组与全局操作 (唯一合法的窗口访问方式) ---- */
    static XjsSearchWindow* At(int index);            /* 窗口数组: 越界 = NULL */
    static int Count();                               /* 活动窗口数 */
    static XjsSearchWindow* Cur();                    /* 当前窗 (UI 线程; 引擎线程禁用) */
    static XjsSearchWindow* Main();                   /* 主窗 (托盘/热键/引擎归属) */
    static HWND MainHwnd();                           /* 主窗 hwnd (引擎级事件投递目标) */
    static XjsSearchWindow* OfResult(xjs_result* r);  /* 结果对象→所属窗 (回调线程安全) */
    static XjsSearchWindow* OfHwnd(HWND hwnd);        /* hwnd→所属窗 (浮层/弹窗认亲; 无归属 = NULL) */
    static void Enter(HWND hwnd);                     /* WndProc 入口: 按 hwnd 绑定当前窗 */
    static void SetCur(XjsSearchWindow* w);           /* 供 XjsWindowScope 作用域切换 */
    static void InvalidateAllLists();                 /* 图标就绪广播: 各窗失效列表区 */
    static void ForEach(void (*fn)(XjsSearchWindow*));/* 遍历全部窗口 (引擎级广播) */
    static void DestroyOthers(XjsSearchWindow* keep); /* 销毁 keep 以外全部窗口 (退出收尾) */
    static void OpenNew(int profileSlot = -1);        /* 创建新搜索窗口 (遍历中拒绝; main.cpp 实现);
                                                         profileSlot=-1=新建空白档案(GUID 名); >=0=绑定既有档案槽 */
    static XjsSearchWindow* AtSlot(int slot);         /* 绑定到指定档案槽的存活窗口 (无=NULL) */
    static bool NameInUse(const wchar_t* name, const XjsSearchWindow* except = NULL);   /* 名称唯一性 (重命名校验) */
    static void RegisterPending(bool main, int profileSlot);   /* CreateWindowExW 前登记新实例 (WM_CREATE 绑定
                                                                  hwnd) 并绑定档案槽 (槽不足时补默认档案) */
    static bool Alive(const XjsSearchWindow* w);      /* 实例是否仍在窗口表内 (owner 悬垂校验) */

    /* ---- 实例操作 ---- */
    void Invalidate();                                /* 整窗重绘 (消息/交互路径统一走它, 别直接 InvalidateRect) */
    void InvalidateList();                            /* 只失效列表区 */
    void SyncSkin();                                  /* 本窗皮肤 → 全局镜像 (g_skin/纪元) */
    void ApplyUiProfile();                            /* 应用 uiWindows[uiIndex] 档案, 无则跟随主窗 */
    void ApplyAppearPos();                            /* 激活/创建位置策略 (appearPos): main.cpp 实现 */
    void ApplyTaskbarIcon();                          /* 任务栏图标开关 (taskbarIcon): 装卸 WS_EX_TOOLWINDOW */
    void ApplyCreateSetup();                          /* 创建套用 (WM_CREATE 内, 显示前): 默认排序+填入搜索词+首搜 */
    void MigrateMainRole();                           /* 主窗真关闭前: 托盘/热键/isMain 迁往最老存留窗 */
    bool CloseRequest();                              /* 关闭请求: 最后窗口=藏托盘(返回true); 否则真关闭(返回false) */
    void DestroyAndFree();                            /* 窗口销毁后: 注册表摘除并释放 */
    void ClearDebounceSnapshot() {                    /* 防抖快照清理唯一入口 (完成/失败回调/渲染缓存作废/重采样前) */
        debounceIds.clear(); debounceSel.clear(); debounceFirst = 0; debounceCount = 0;
    }
};

/* 作用域内把"当前窗"临时切到指定窗口 (设置窗等 owner 绑定场景); 析构自动恢复 */
class XjsWindowScope {
    XjsSearchWindow* m_prev;
public:
    explicit XjsWindowScope(XjsSearchWindow* w) : m_prev(XjsSearchWindow::Cur()) { XjsSearchWindow::SetCur(w); }
    ~XjsWindowScope() {
        /* 作用域内可能已 DestroyAndFree(m_prev 被删): 恢复前校验存活, 悬垂指针绝不能写回 Cur() */
        XjsSearchWindow::SetCur(XjsSearchWindow::Alive(m_prev) ? m_prev : NULL);
    }
};

/* 每窗界面设置档案 (uiWindows 数组) */
int XjsUiProfileCount();
XjsUiProfile* XjsUiProfileAt(int i);              /* 越界 = NULL */
int XjsUiProfilesAppend(const wchar_t* name);     /* 追加一个默认档案 (name 可空串), 返回槽位 */
void XjsUiProfilesRemove(int slot);               /* 删除档案槽 (窗口管理), 其后槽位前移并修正窗口绑定 */
void XjsUiProfilesReset();                        /* 保存前清空 (随后每窗 push) */
void XjsUiProfilesPush(const XjsUiProfile& p);

/* ==================== 全局状态 (定义在 xjs_app.cpp) ==================== */

/* ---- 每窗状态 → XjsSearchWindow 字段 (宏重定向; 字段名 = 原名去 g_) ---- */
#define g_hWnd            (XjsSearchWindow::Cur()->hWnd)
#define g_hFontEdit       (XjsSearchWindow::Cur()->hFontEdit)
#define g_sysCaretMade    (XjsSearchWindow::Cur()->sysCaretMade)
#define g_blurAction      (XjsSearchWindow::Cur()->blurAction)       /* 窗口失去焦点动作 (每窗) */
#define g_appearPos       (XjsSearchWindow::Cur()->appearPos)        /* 激活/创建时位置 (每窗) */
#define g_showCtrlBtns    (XjsSearchWindow::Cur()->showCtrlBtns)     /* 显示控制按钮 (每窗) */
#define g_showFilterBox   (XjsSearchWindow::Cur()->showFilterBox)    /* 显示筛选框 (每窗) */
#define g_showStatusbar   (XjsSearchWindow::Cur()->showStatusbar)    /* 显示状态栏 (每窗) */
#define g_taskbarIcon     (XjsSearchWindow::Cur()->taskbarIcon)      /* 任务栏图标 (每窗) */
#define g_mouseOpen       (XjsSearchWindow::Cur()->mouseOpen)        /* 鼠标打开: 0=双击 1=单击 (每窗) */
#define g_defaultSel      (XjsSearchWindow::Cur()->defaultSel)       /* 默认选中表项 (每窗) */
#define g_createFill      (XjsSearchWindow::Cur()->createFill)       /* 创建填入搜索框方式 (每窗) */
#define g_createKeyword   (XjsSearchWindow::Cur()->createKeyword)    /* 创建指定关键词 (每窗) */
#define g_uiZoomTenths    (XjsSearchWindow::Cur()->uiZoom)           /* 页面缩放 (每窗, 十分位 5..20; 曾为进程共享) */
#define g_lang            (XjsSearchWindow::Cur()->lang)             /* 界面语言 (每窗, XjsLang; auto=按系统; 曾为进程共享) */
#define g_needFullPaint   (XjsSearchWindow::Cur()->needFullPaint)
#define g_rt              (XjsSearchWindow::Cur()->rt)
#define g_hwndRt          (XjsSearchWindow::Cur()->hwndRt)
#define g_dc              (XjsSearchWindow::Cur()->dc)
#define g_br              (XjsSearchWindow::Cur()->br)
#define g_brWhite         (XjsSearchWindow::Cur()->brWhite)
#define g_brCloseHover    (XjsSearchWindow::Cur()->brCloseHover)
#define g_brErr           (XjsSearchWindow::Cur()->brErr)
#define g_brSelGrad       (XjsSearchWindow::Cur()->brSelGrad)
#define g_brSelBar        (XjsSearchWindow::Cur()->brSelBar)
#define g_brProgress      (XjsSearchWindow::Cur()->brProgress)
#define g_appIcon         (XjsSearchWindow::Cur()->appIcon)
#define g_layout          (XjsSearchWindow::Cur()->layout)
#define g_s               (XjsSearchWindow::Cur()->dpiS)
#define g_result          (XjsSearchWindow::Cur()->result)
#define g_resultCount     (XjsSearchWindow::Cur()->resultCount)
#define g_syncWatchCount  (XjsSearchWindow::Cur()->syncWatchCount)
#define g_searchFingerprint (XjsSearchWindow::Cur()->searchFingerprint)
#define g_searching       (XjsSearchWindow::Cur()->searching)
#define g_modeDlg         (XjsSearchWindow::Cur()->modeDlg)
#define g_hostedTags      (XjsSearchWindow::Cur()->hostedTags)
#define g_hostedCaret     (XjsSearchWindow::Cur()->hostedCaret)
#define g_hostedRects     (XjsSearchWindow::Cur()->hostedRects)
#define g_hostedCaretRect (XjsSearchWindow::Cur()->hostedCaretRect)
#define g_hostedRegion    (XjsSearchWindow::Cur()->hostedRegion)
#define g_hostHoverTag    (XjsSearchWindow::Cur()->hostHoverTag)
#define g_hostHoverBtn    (XjsSearchWindow::Cur()->hostHoverBtn)
#define g_hostMenuTag     (XjsSearchWindow::Cur()->hostMenuTag)
#define g_fileChangePending (XjsSearchWindow::Cur()->fileChangePending)
#define g_debounceIds     (XjsSearchWindow::Cur()->debounceIds)
#define g_debounceSel     (XjsSearchWindow::Cur()->debounceSel)
#define g_debounceFirst   (XjsSearchWindow::Cur()->debounceFirst)
#define g_debounceCount   (XjsSearchWindow::Cur()->debounceCount)
#define g_visFirst        (XjsSearchWindow::Cur()->visFirst)
#define g_visLast         (XjsSearchWindow::Cur()->visLast)
#define g_statusText      (XjsSearchWindow::Cur()->statusText)



#define g_errText         (XjsSearchWindow::Cur()->errText)
#define g_mode            (XjsSearchWindow::Cur()->mode)
#define g_viewMode        (XjsSearchWindow::Cur()->viewMode)
#define g_searchFocused   (XjsSearchWindow::Cur()->searchFocused)
#define g_cutSet          (XjsSearchWindow::Cur()->cutSet)
#define g_hoverTraces     (XjsSearchWindow::Cur()->hoverTraces)
#define g_iconCache       (XjsSearchWindow::Cur()->iconCache)
#define g_anchorIdx       (XjsSearchWindow::Cur()->anchorIdx)
#define g_focusIdx        (XjsSearchWindow::Cur()->focusIdx)
#define g_scrollTop       (XjsSearchWindow::Cur()->scrollTop)
#define g_topmost         (XjsSearchWindow::Cur()->topmost)
#define g_rowCache        (XjsSearchWindow::Cur()->rowCache)
#define g_hScroll         (XjsSearchWindow::Cur()->hScroll)
#define g_dragHScroll     (XjsSearchWindow::Cur()->dragHScroll)
#define g_hScrollGrab     (XjsSearchWindow::Cur()->hScrollGrab)
#define g_hoverBtn        (XjsSearchWindow::Cur()->hoverBtn)
#define g_hoverWndBtn     (XjsSearchWindow::Cur()->hoverWndBtn)
#define g_hoverStatus     (XjsSearchWindow::Cur()->hoverStatus)
#define g_listHover       (XjsSearchWindow::Cur()->listHover)
#define g_hoverRow        (XjsSearchWindow::Cur()->hoverRow)
#define g_modeMenuOpen    (XjsSearchWindow::Cur()->modeMenuOpen)
#define g_appMenuOpen     (XjsSearchWindow::Cur()->appMenuOpen)
#define g_dragScroll      (XjsSearchWindow::Cur()->dragScroll)
#define g_scrollGrab      (XjsSearchWindow::Cur()->scrollGrab)
#define g_dragCol         (XjsSearchWindow::Cur()->dragCol)
#define g_dragColIdx      (XjsSearchWindow::Cur()->dragColIdx)
#define g_dragColX        (XjsSearchWindow::Cur()->dragColX)
#define g_hoverHandle     (XjsSearchWindow::Cur()->hoverHandle)   /* 悬停的列宽调整边界 (纯悬停无捕获, 必须每窗) */
#define g_sbHover         (XjsSearchWindow::Cur()->sbHover)       /* 悬停的滚动条 0无/1纵/2横 (纯悬停无捕获, 必须每窗) */
#define g_filterSel       (XjsSearchWindow::Cur()->filterSel)     /* 筛选分类选中项: DLL SetSelectedFilter 按结果对象, 必须每窗 */
#define g_marquee         (XjsSearchWindow::Cur()->marquee)
#define g_marqueeMoved    (XjsSearchWindow::Cur()->marqueeMoved)
#define g_marqueeStartX   (XjsSearchWindow::Cur()->marqueeStartX)
#define g_marqueeStartY   (XjsSearchWindow::Cur()->marqueeStartY)
#define g_marqueeRect     (XjsSearchWindow::Cur()->marqueeRect)
#define g_mouseTracking   (XjsSearchWindow::Cur()->mouseTracking)
#define g_previewVisible  (XjsSearchWindow::Cur()->previewVisible)
#define g_openElevated    (XjsSearchWindow::Cur()->openElevated)     /* 打开文件: 继承管理员权限 (每窗) */
#define g_openAsync       (XjsSearchWindow::Cur()->openAsync)        /* 打开文件: 异步线程执行 (每窗) */
#define g_openHideWindow  (XjsSearchWindow::Cur()->openHideWindow)   /* 打开文件后隐藏窗口 (每窗) */
#define g_match           (XjsSearchWindow::Cur()->match)            /* 搜索匹配 ×8 (每窗) */
#define g_history         (XjsSearchWindow::Cur()->history)          /* 搜索历史 (每窗; 曾为进程共享) */
#define g_name            (XjsSearchWindow::Cur()->name)             /* 窗口名称 (每窗, 主窗固定) */
#define g_driveProgress   (XjsSearchWindow::Cur()->driveProgress)    /* 绘制驱动器占用进度条 (每窗) */
#define g_rowHover        (XjsSearchWindow::Cur()->rowHover)         /* 高亮鼠标经过行 (每窗) */
#define g_rowHoverFade    (XjsSearchWindow::Cur()->rowHoverFade)     /* 鼠标经过残影 (每窗) */
#define g_previewWidth    (XjsSearchWindow::Cur()->previewWidth)
#define g_previewLocked   (XjsSearchWindow::Cur()->previewLocked)
#define g_previewFileId   (XjsSearchWindow::Cur()->previewFileId)
#define g_previewImage    (XjsSearchWindow::Cur()->previewImage)
#define g_previewImageFileId (XjsSearchWindow::Cur()->previewImageFileId)
#define g_previewDrag     (XjsSearchWindow::Cur()->previewDrag)
/* 插件面板接管会话 (xjs_preview.cpp "面板接管"节) */
#define g_plugPanelOn       (XjsSearchWindow::Cur()->plugPanelOn)
#define g_plugPanelPluginId (XjsSearchWindow::Cur()->plugPanelPluginId)
#define g_plugPanelKey      (XjsSearchWindow::Cur()->plugPanelKey)
#define g_plugPanelCapture  (XjsSearchWindow::Cur()->plugPanelCapture)
#define g_plugPanelResyncPosted (XjsSearchWindow::Cur()->plugPanelResyncPosted)
#define g_plugPanelCaretX   (XjsSearchWindow::Cur()->plugPanelCaretX)
#define g_plugPanelCaretY   (XjsSearchWindow::Cur()->plugPanelCaretY)
#define g_colsDetails     (XjsSearchWindow::Cur()->colsDetails)
#define g_colsList        (XjsSearchWindow::Cur()->colsList)

/* ---- 进程共享状态 (引擎/皮肤/配置/文本格式/列配置/历史) ---- */
extern DWORD g_clipSeqOurs;     /* 自己最后一次写剪贴板后的序列号 (WM_CLIPBOARDUPDATE 忽略自己) */

/* D2D 工厂与文本格式 (设备无关, 进程一份) */
extern XjsDwFactory* g_dw;
extern XjsWicFactory* g_wic;
extern XjsFormat* g_tfTitle;
extern XjsFormat* g_tfMenu;
extern XjsFormat* g_tfHead;
extern XjsFormat* g_tfHeadR;
extern XjsFormat* g_tfRow;
extern XjsFormat* g_tfRowBold;
extern XjsFormat* g_tfDim;
extern XjsFormat* g_tfTiny;
extern XjsFormat* g_tfTinyR;
extern XjsFormat* g_tfStatus;
extern XjsFormat* g_tfTip;
extern XjsFormat* g_tfChip;
extern XjsFormat* g_tfRowR;
extern XjsFormat* g_tfBig;      // 预览面板大字 (84%)
extern XjsFormat* g_tfCardVal;  // 卡片值
extern XjsFormat* g_tfSearch;   // 搜索框输入文本 (无省略号: 溢出走横向滚动)
extern XjsFormat* g_tfToast;    // Toast 正文 12.5px (顶对齐+字符级换行, 源样式 word-break:break-all)
extern XjsFormat* g_tfTag;      // 搜索框托管标签 11.5px 中字重 (源样式 .hosted-tag 12px/500 标题栏紧凑档 11.5)

/* 引擎级状态 (一个引擎, 全窗共享) */
extern xjs_engine* g_engine;
extern std::atomic<bool> g_isScanning;
extern std::atomic<bool> g_busy;
extern std::atomic<bool> g_syncFileChanged;     /* 引擎级同步变化标记: SYNC_* 回调签名无结果对象 (源样式单窗都落窗口成员, dome 多窗按信号归属分两级) */
extern std::atomic<bool> g_doubleCtrl;          /* 双击 Ctrl 当前激活态 (目标有效=真; 钩子线程读, 默认关: 杀软误报) */
extern std::wstring g_doubleCtrlTarget;         /* 双击 Ctrl 触发目标窗口名 ("默认窗口"/档案名; 空=禁用; UI 线程) */
extern std::atomic<bool> g_dbReady;             /* 数据库就绪 (加载/首扫完成): 结果对象只允许就绪后创建 (早建=被定死文件名序) */
extern int g_fileCount;
extern std::wstring g_scanDrive;
extern int g_scanEnumerated, g_scanTotal;

/* 搜索模式枚举已上移至 XjsSearchWindow 之前 (每窗字段初始化需要) */
extern const int g_modeToKeyword[5];
extern const wchar_t* g_modeName[5];
extern const wchar_t* g_modeDesc[5];
extern const wchar_t* g_modeHint[5];
extern const wchar_t* g_modeIni[5];

/* 列表/筛选 (筛选进程共享: DB 级分类, 各窗内容一致, 库加载/扫描完成时刷新);
   搜索历史已每窗化 (g_history 宏 → Cur()->history, 见宏区) */
extern std::vector<XjsFilterCat> g_filters;

/* 热键/托盘 */
/* 全局快捷键每窗: hotkeyMod/hotkeyVk 为窗口字段 (XjsSearchWindow), 注册管理见 XjsHotkeysRegisterAll;
   2026-09-17 用户口径: 无内置默认热键, 程序不自动注册 — 设置页录制过才注册, Delete 清除取消注册 */
extern NOTIFYICONDATAW g_nid;
extern bool g_inTray;

/* 配置派生状态 (xjs_config.json) — 窗口矩形已每窗化入档案, 无全局矩形状态 */
extern bool g_rbSaved;                        /* 重建对话框"记住上次" */
extern bool g_rbFields[7];                    /* 评分/大小/修改/创建/访问/属性/别名 */
extern std::wstring g_rbDrives;               /* 勾选盘符 "C,D" */

/* ==================== 小工具 (inline 共享) ==================== */
inline float XjsUiZoom() { return g_uiZoomTenths / 10.0f; }
/* XSF = 逻辑 px → 物理 px: DPI × 页面缩放 (正式版 = WebView2 ZoomFactor 缩放整页) */
inline float XSF(float v) { return v * g_s * XjsUiZoom(); }

/* 图标请求像素尺寸 = 显示逻辑档 × 当前缩放 (每窗 DPI × 页面缩放), 渲染 1:1 像素不模糊
   (旧口径取固定逻辑档, 高缩放下小图被拉伸发糊)。图标缓存键含该尺寸, 不同缩放各自成档。
   仅渲染线程可用 (读 g_s = Cur()->dpiS); 引擎 ICON_ASK 闸门用 w->dpiS 同口径现算 */
inline int XjsIconFetchPx(int logicalPx) { return (int)(logicalPx * g_s * XjsUiZoom() + 0.5f); }

/* 每窗有效 DPI (系统随 WM_DPICHANGED 同步维护): 工程钉 WINVER=0x0601, 声明被宏挡 → GetProcAddress。
   返回 0 = 系统 <Win10 1607 或调用失败 — 业务代码一律改用 XjsWindowDpi (内含回落, 唯一入口);
   仅设置窗 WM_DISPLAYCHANGE/WM_SETTINGCHANGE 自检直接用它, 就是靠这个 0 判断"不可用" */
inline UINT XjsGetDpiForWindow(HWND hwnd) {
    typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
    static PFN_GetDpiForWindow fn = []() -> PFN_GetDpiForWindow {
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        return u32 ? (PFN_GetDpiForWindow)(void*)GetProcAddress(u32, "GetDpiForWindow") : NULL;
    }();
    return fn ? fn(hwnd) : 0;
}
inline float xf_min(float a, float b) { return a < b ? a : b; }
inline float xf_max(float a, float b) { return a > b ? a : b; }
inline int ximax(int a, int b) { return a > b ? a : b; }
inline int ximin(int a, int b) { return a < b ? a : b; }
inline bool XjsPtIn(const XjsRect& r, const POINT& pt) {
    return pt.x >= r.left && pt.x <= r.right && pt.y >= r.top && pt.y <= r.bottom;
}

/* ==================== SDK 回调约定与消息结构体 ==================== */
#if defined(_WIN64) || defined(__x86_64__)
    #define XJS_CALLBACK
#else
    #define XJS_CALLBACK __stdcall
#endif

INT XJS_CALLBACK Xjs_EnumProgress(void* userData, xjs_engine* engine, const char* driveLetter, int totalCount, int enumeratedCount);
void XJS_CALLBACK Xjs_LoadComplete(void* userData, xjs_engine* engine, int fileCount);
void XJS_CALLBACK Xjs_EnumPartition(void* userData, xjs_engine* engine, const char* driveLetter);
void XJS_CALLBACK Xjs_EnumComplete(void* userData, xjs_engine* engine, int elapsedMs);
INT XJS_CALLBACK Xjs_SyncFileChanged(void* userData, xjs_engine* engine, const char* filePath);   /* 同步_创建/修改/删除 共用: 只置 g_fileChangePending */
INT XJS_CALLBACK Xjs_SyncFileMoved(void* userData, xjs_engine* engine, const char* srcPath, const char* destPath); /* 同步_移动: 同上 */

struct XjsScanProgressData { wchar_t drive; int enumerated; int total; };
struct XjsScanCompleteData { int fileCount; int elapsedMs; };

/* ==================== 模块接口 ==================== */

/* ---- 多国语言 (i18n, xjs_engine.cpp 实现; 键 = 中文原文, 包 = exe 目录 languages\*.json) ----
   **语言 = 每窗私有** (XjsSearchWindow::lang, 随档案持久化): XjsT 解析到当前绑定窗 (Cur)。
   简体中文零表直出 (XjsT 原样返回键); 缺键/缺文件回落英语表 → 键原文。
   窗口字段 auto = 按系统 UI 语言 (大陆/新加坡=简体, 台/港/澳=繁体, 其余英语)。
   仅 UI 线程调用 (引擎线程禁 Cur(), 也不画 UI 文案)。 */
const wchar_t* XjsT(const wchar_t* key);        // 取当前窗语言文案 (每帧绘制可调, 查表开销可忽略)
std::string XjsTUtf8(const wchar_t* key);       // UTF-8 场景 (询问框 buttonsJson 等)
void XjsI18nInit();                             // wWinMain 启动时装载全部语言表 (一次全载, 切换零重装)
const wchar_t* XjsLangLabel(int lang);          // 语言显示名 (设置下拉/菜单用, 恒母语不翻译)
const wchar_t* XjsLangAutoLabel();              // "自动 (跟随系统)"
int XjsLangIndexFromCode(const wchar_t* code);  // 语言代码 → XjsLang ("zh"/"zh-TW"/…; 未知/NULL = XLANG_AUTO)
const char* XjsLangCodeUtf8(int lang);          // XjsLang → 语言代码串 ("zh"/…/"auto"; 配置与扩展 API 同串)
void XjsApplyUiLang(XjsSearchWindow* w, int lang);   // 语言应用唯一入口 (写窗字段+落盘+逐窗标题按各自语言刷新+托盘随主窗; 设置页下拉与插件扩展 API 同落点)

/* ---- xjs_util ---- */
std::wstring XjsGetExeDir();
UINT XjsWindowDpi(HWND hwnd);                       // 每窗有效 DPI: GetDpiForWindow 取不到才回落 GetDeviceCaps (hwnd=NULL=桌面口径)
std::wstring Utf8ToUtf16(const char* str);
std::string Utf16ToUtf8(const wchar_t* str);
std::wstring XjsTimeBadge(long long ms);            // 相对时间徽章: 刚刚/N分钟/N天/…
std::wstring XjsAttrsText(unsigned long a);         // 属性位→中文标签: 目录/只读/隐藏/系统/归档, 0='-'
std::wstring XjsFmt(const wchar_t* tpl, const std::wstring& a0 = L"",
                    const std::wstring& a1 = L"", const std::wstring& a2 = L"");  // {0}{1}{2} 占位替换 (XjsT 模板键搭配用)
std::wstring XjsTrimWs(const std::wstring& s, const wchar_t* set = L" \t\r\n");  // 去首尾空白 (默认 \r\n\t 空格; set 可换字符集)
std::wstring XjsTimeText(long long ms);             // 完整时间文本
std::wstring XjsNumText(long long n);               // 千分位
std::wstring XjsWanText(long long n);               // 4,483,452 (448.3万)
void XjsCopyClipboard(const std::wstring& text);
std::wstring XjsItemPath(int idx);
void XjsOpenFile(const std::wstring& path);
void XjsOpenFolderAndSelect(const std::wstring& path);
void XjsShowProperties(int idx);                    // 系统"属性"对话框 (ShellExecuteEx verb=properties)
void XjsDeleteSelected();
void XjsCopySelected(bool namesOnly);
void XjsCopyFilesToClipboard(const std::vector<int>& idxs, bool cut);  /* CF_HDROP+DropEffect (源样式剪切/复制文件) */
void XjsCutSelected();                                                 /* Ctrl+X: 写剪贴板+灰显 */
void XjsDragOutSelected();                                             /* 按住已选行拖动: OLE 拖出选中集合 */
bool XjsRenameStart(int idx);                                          /* F2/右键重命名: 进入行内编辑 */
void XjsRenameFinish(bool commit);                                     /* 提交(MoveFile)/取消 */
bool XjsRenameKey(WPARAM vk);
bool XjsRenameChar(wchar_t ch);
bool XjsRenameImeResult(HWND hwnd, LPARAM lParam);
bool XjsRenameActive();
int XjsRenameTargetIdx();                                              /* 重命名目标行 (无=-1) */
void XjsRenameMouseDown(POINT pt);                                     /* 编辑框内点击定位光标 */
bool XjsRenameDoubleClick(POINT pt);                                   /* 编辑框内双击=选整词 (返回 true 已消费) */
void XjsRenameMouseMove(POINT pt);                                     /* 编辑框内拖拽选字 */
void XjsRenameMouseUp();                                               /* 结束拖拽 */
bool XjsRenameDragging();                                              /* 重命名编辑框拖拽中 (WM_MOUSEMOVE 分流) */
void XjsRenameMenuCmd(int cmd);                                        /* 编辑框右键菜单命令: 0剪切 1复制 2粘贴 3全选 4删除 */
bool XjsShowRenameBoxMenu(POINT screenPt);                             /* 编辑框右键菜单 (返回 true 已弹) */
void XjsRenameRender();                                                /* 行内重命名编辑框绘制 (列表末尾) */
XjsRect XjsRenameEditRect();                                       /* 重命名编辑框几何 (列表/网格现算) */
void XjsApplyAlias(const std::vector<int>& idxs, const std::wstring& alias);  /* 空=删除别名 */
void XjsShowAliasDialog();                                             /* 右键"设置别名" */
BOOL XjsIsAutoStartEnabled();    /* 查询计划任务开机自启动是否已注册 (schtasks /query) */
BOOL XjsSetAutoStart(BOOL enable); /* 注册/注销计划任务 (schtasks /xml, 附加 --autostart); 假=失败由调用方提示 */
BOOL XjsIsRunningAsAdmin();
HICON XjsAppIconBig();       /* 应用图标 (exe 资源 32512): 任务栏按钮/Alt+Tab 尺寸; 进程内缓存, 勿 DestroyIcon */
HICON XjsAppIconSmall();     /* 同上小尺寸: 窗口类 hIconSm 与 WM_SETICON(ICON_SMALL), 任务栏悬停预览用它 */
std::wstring XjsBuildTimeText();                               /* 本 exe 的编译时刻 "2026-09-18 02:00:54" (设置-关于) */
std::wstring XjsOsVersionText();                               /* 系统版本 "Windows 11 (Build 26100)" (设置-关于) */
bool XjsPickFolder(HWND owner, std::wstring& folder);          /* IFileDialog 选文件夹 */

/* 双击 Ctrl 全局唤起 (默认关: 全局钩子易被杀毒软件误报, 设置-通用 显式开启才注册 键盘+鼠标双 LL 钩子, 关闭态零钩子)。
 * 判定 = 两次"干净"Ctrl 按下间隔<1000ms: 按住期间/两次之间混入其它键或鼠标点击不触发; 触发键自身的抬起被消耗,
 * 不武装新一轮 (双击后补按一下 Ctrl 不会再触发) — 详见 xjs_util.cpp 实现 */
void XjsDoubleCtrlStart();   /* 开启态才创建钩子线程 (未开启为空操作) */
void XjsDoubleCtrlStop();    /* 摘钩子并停线程 (未启动过为空操作; 设置开关关闭时即时调用) */
int XjsDoubleCtrlResolveSlot();   /* 双击 Ctrl 目标档案槽 (空/名称不存在或被删 = -1 = 禁用; "默认窗口"=槽0) */
void XjsDoubleCtrlApply();        /* 按目标有效性实时启停钩子 (下拉切换/改名失效/启动载入 后调用) */

/* 搜索匹配设置: 结构体 XjsMatchSettings 见上 (每窗字段), 应用见 XjsApplyMatchSettingsFor */
void XjsApplyMatchSettingsFor(XjsSearchWindow* w);   /* w->match → w->result (xjs_result_SetSearchSettings) */
void XjsDismissWindow(HWND hwnd);   /* 窗口消失统一策略入口 (✕/热键/失焦关闭/双击Ctrl/Esc末级): 主窗藏托盘, 子窗真销毁 */
void XjsElevateAndRestart();
std::vector<std::wstring> XjsSplitLines(const std::wstring& text);     /* 文本预览分行 */

/* ---- 绘图后端 (xjs_d2d = D2D 实现 / xjs_gdiplus = GDI+ 实现, XjsGfxApi 两张表) ---- */
bool XjsD2DInit();
void XjsD2DShutdown();
bool XjsGdiplusInit();        /* GDI+ 后端 (设置-通用 "绘制引擎" 选择, 重启生效) */
void XjsGdiplusShutdown();
void XjsDeviceCreate();
void XjsDeviceResize(int w, int h);              /* 窗口尺寸变化: HwndRT Resize */
void XjsDeviceDiscardCtx(XjsSearchWindow& w);   /* 显式上下文: 撕毁期 Cur() 可能被嵌套消息重绑 */
void XjsDeviceDiscard();                        /* Cur() 便捷版 (撕毁路径一律用 Ctx 显式传参) */
void XjsReleaseTextFormats();
void XjsEllSignCacheDropFormat(XjsFormat* fmt);   /* 格式被单独释放 (弹窗旁路) 前从省略号签名缓存摘除 */
void XjsRecreateTextFormats();   /* 页面缩放变化后按新倍率重建文本格式 */
void XjsSyncTextFormats();       /* 窗口上下文切换后调用: 当前窗 DPI/缩放与格式构建时失配 → 重建 */
XjsBitmap* XjsDecodeImage(const void* data, int len);
XjsBitmap* XjsDecodeFileImage(const std::wstring& path);
XjsBitmap* XjsBitmapFromBgra(const void* bgra, int w, int h, int stride);   /* 插件预览交付: 裸 BGRA → 本域位图 */
XjsSolidBrush* XjsTempBrush(XjsColor c);          // 临时色刷缓存 (设备重建时清)
float XjsMeasureText(const wchar_t* s, XjsFormat* fmt);
/* 彩色字体绘制 (emoji): g_dc 可用时带 ENABLE_COLOR_FONT, 否则退 g_rt 单色轮廓 */
void XjsDrawTextC(const wchar_t* s, UINT32 len, XjsFormat* fmt,
                  const XjsRect& r, XjsBrush* brush, XjsDrawTextOpts opts);
void XjsDrawTextLayoutC(const XjsPoint2& origin, XjsTextLayout* lay,
                        XjsBrush* brush, XjsDrawTextOpts opts);
void XjsDrawEllText(const std::wstring& s, const XjsRect& r, XjsFormat* fmt, XjsBrush* brush);
void XjsDrawHlSegs(const std::vector<XjsHlSeg>& segs, float x, const XjsRect& clip, XjsFormat* fmt);
void XjsDrawMagnifier(XjsPoint2 c, float r, XjsBrush* brush, float stroke);
void XjsDrawSpinner(XjsPoint2 c, float r, float phase);

/* ---- xjs_engine ---- */
bool XjsEngineEnsureResult();                       /* 当前窗 (Cur) 版: 无则创建 (数据库未就绪 = 拒绝, 见 g_dbReady) */
bool XjsEngineEnsureResultFor(XjsSearchWindow* w);  /* 指定窗版本 (ForEach/广播用) */
void XjsEngineEnsureResultAll();                    /* 数据库就绪广播: 给所有尚无结果对象的窗口补建 */
void XjsClearRenderCaches();
void XjsClearRowCache();
void XjsSyncWatchStart(HWND hwnd);   /* 文件同步变化轮询: 挂 ID_TIMER_SYNCWATCH + 初始化数量基线 */
void XjsSyncWatchTick();             /* WM_TIMER(ID_TIMER_SYNCWATCH): 数量变了 → 真实时钟节流刷新 */
void XjsSearchNow(bool commitHistory);
void XjsLoadFilters();
void XjsApplyFilter(int idx);
XjsRowData* XjsEnsureRowData(int idx);
XjsRowData* XjsEnsureRowData(int idx, int fileId);   /* 指定文件ID版 (防抖绘制: 搜索中ID取自快照) */
XjsBitmap* XjsGetRowIcon(int idx, int fileId, int iconPx, const char* askTag = "row");  // 返回位图归调用方所有, 用完 Release; askTag 透传 GetFileIco callbackInfo (ICON_ASK 闸门区分 列表行/预览卡)。排队期画临时默认图标但**不进缓存** (真图标就绪后须能取而代之)
void XjsSaveHistory();
void XjsAddHistory(const std::wstring& text);
/* ==================== 配置文件 (xjs_config.json) ====================
 * 配置存取的门面 = XjsConfig 类 + XjsLoadConfig/XjsSaveConfig, 全部收在 xjs_engine.cpp
 * (picojson 是它的实现细节, 不进本头文件; 换 JSON 模块 = 只改 xjs_engine.cpp)。
 * 禁止绕过门面直接摸 JSON 文档; 字符串里内嵌 JSON 的解析也走下面三个辅助函数。
 * 键 → 全局状态的分发在 XjsLoadConfig/XjsSaveConfig; 新增键 = 两处各一行, 不碰文件层。 */
int XjsJsonStringArray(const char* json, std::vector<std::wstring>* out);  /* 解析字符串JSON数组 */

/* 顶层 JSON 对象摊平 (插件扩展 API 的 settings.set / modes.add 入参; 实现在 xjs_engine.cpp,
   picojson 唯一 include 红线不破)。只收顶层标量成员: type 1=bool 2=number 3=string
   (嵌套数组/对象/null 忽略)。返回假 = 非法 JSON 或顶层不是对象。 */
struct XjsJsonMember { std::wstring key; int type = 0; bool b = false; double num = 0; std::wstring str; };
bool XjsPluginJsonMembers(const char* utf8Json, std::vector<XjsJsonMember>* out);

void XjsLoadConfig();
void XjsSaveConfig();
void XjsSaveWindowRect();

/* ---- 文件分类 (引擎筛选器) 与 路径别名 配置 (设置窗表格编辑, 整体下发引擎) ----
 * 数据串 = 引擎口径: 筛选器 JSON 数组 [{"名称":"..","类型":99,"后缀":"EXE,BAT"}];
 * 别名 JSON 对象 {"完整路径":"别名"}。解析/序列化/落盘全收在 xjs_engine.cpp (JSON 细节不过本头)。
 * Apply = 下发引擎 + 写配置键 + 即时落盘; Load = 取引擎当前生效配置 (保存后回显经引擎规范化/占位符
 * 展开的真实值)。Apply 返回假时用 xjs_GetLastError 取 30(参数无效)/35(数据库正忙)。 */
struct XjsFilterItem { std::wstring name, ext; int type = 0; };
struct XjsAliasItem  { std::wstring path, alias; };
bool XjsFilterConfigApply(const std::vector<XjsFilterItem>& rows);   /* sync=FALSE: 已入库文件类型需重建才重算 */
bool XjsAliasConfigApply(const std::vector<XjsAliasItem>& rows);     /* sync=TRUE: 立即应用到现有库 */
void XjsFilterConfigLoad(std::vector<XjsFilterItem>* rows);          /* 引擎当前配置 → 行 (引擎无效=空表) */
void XjsAliasConfigLoad(std::vector<XjsAliasItem>* rows);
void XjsEngineApplySavedConfigs();   /* 启动/重建索引前: 下发保存的 筛选器/别名; 两者键空时各自回落 exe 目录 Config\ 下 Filter.json / Alias.json 内置词典 (原文透传; 首次播种含 sync=TRUE 一次性回填旧行并入键, 文件缺席跳过) */

void XjsEngineRebuildEx(const bool enableFields[7], const std::wstring& drivesJsonWide);  /* 字段开关+盘符JSON(空=全盘), 设置对话框用 (确认在设置窗自绘对话框完成; 曾有无调用方的 XjsEngineRebuild 带系统 MessageBox, 已删) */
void XjsEngineShutdown(bool warnOnSaveFail);
XjsColor XjsDriveColor(int percent);
bool XjsIsDriveRow(int idx);                      // 父目录ID==-1 → 驱动器

/* ---- xjs_popup ---- */
void XjsRegisterPopupClass(HINSTANCE hInst);
void XjsShowPopupMenu(HWND owner, POINT anchorScreen, const std::vector<XjsPopupItem>& items, float minWidthPx);
bool XjsPopupMenuOpen();   /* 自绘弹窗菜单当前是否打开 (菜单会抢前台, "失活即取消"的宿主须豁免) */
void XjsOnPopupResult(int id);

/* 通用模态询问框 (删除确认/多选打开/定位 等一切"询问"点共用; 禁止再用系统 MessageBoxW):
 * buttonsJson = 按钮数组 [{"text":"确定","style":"primary|danger|default"},...]; owner 背景钟罩虚化
 * (源样式 .exit-mask 口径)。同步阻塞到关闭, 返回被点按钮下标; 点蒙层空白/Esc/失活 = -1。 */
int  XjsShowAskDialog(HWND owner, const wchar_t* title, const wchar_t* desc, const char* buttonsJson);
/* buttonsJson 的解析 (JSON 细节在 xjs_engine.cpp, 本头文件不见 JSON 类型):
   style: 0=default 1=primary(强调实心) 2=danger(红); 无有效按钮时实现回退返回单个"确定" */
struct XjsAskBtnDef { std::wstring text; int style = 0; };
int  XjsParseDialogButtons(const char* buttonsJson, XjsAskBtnDef* out, int cap);
/* 引擎搜索失败回调的错误串 → 人类可读消息 (串是 JSON 时取 "错误信息" 键, 否则原文直转) */
std::wstring XjsEngineErrText(const std::string& errUtf8);
bool XjsAskDialogOpen();              /* 询问框打开期: 宿主窗口须吞键吞鼠 (点击蒙层=取消) */
bool XjsModalOverlayFor(HWND hwnd);   /* 独立模态弹窗(别名框/询问框)挂在 hwnd 上 — 该窗背景钟罩虚化判定 */

/* ---- xjs_chrome (标题栏: 菜单/搜索框/筛选/窗口按钮/背景) ---- */
void XjsChromeLayout();                           // 全窗口布局计算 (含预览面板收窄)
void XjsChromeRenderBackground();
void XjsChromeRenderTitlebar();
void XjsRenderStatusbar();
/* Toast 通知 (源样式 showToast 照抄: 右下角卡片栈, 类型色左条+进度条, 进出场动画) */
/* Toast 通知组件 (按宿主 HWND 挂载, 任意窗口复用; scale = 该宿主的 DPI×缩放系数):
   宿主接线: WM_PAINT 尾调 Render / WM_TIMER 透传 Tick / 鼠标先问 MouseMove+MouseDown */
void XjsToastShow(HWND host, const wchar_t* text, int type, float scale,
                  int pos = XTOPOS_BOTTOM_RIGHT, float durationSec = 0);
void XjsToastRender(HWND host, XjsRt* rt, float w, float h, float scale);  /* 自带圆角遮罩层, 在 PopAxisAlignedClip 之后调 */
bool XjsToastTimerTick(HWND host, float scale);                        /* 假=无活动 toast 应 KillTimer */
bool XjsToastMouseMove(HWND host, float scale, float w, float h, const POINT& pt);  /* 命中卡片/按钮悬停 (真=吃掉后续悬停处理) */
bool XjsToastMouseDown(HWND host, float scale, float w, float h, const POINT& pt);  /* 卡片吃点击 (命令记待定在组件内, 真=已消费) */
void XjsToastMouseUp(HWND host, float scale, float w, float h, const POINT& pt);   /* 复制/关闭按钮松开触发 */
void XjsToastDetach(HWND host);                        /* 宿主销毁: 摘除组件条目释放画刷 */
void XjsToastDropBrushCache();                         /* 设备重建: 全宿主画刷作废 (旧画刷绑死 RT 域) */
void XjsToastHoverReset(HWND host);                    /* 鼠标离开宿主: 清悬停态 */
void XjsUpdateHoverState(POINT pt);
bool XjsChromeMouseDown(POINT pt);                // 命中返回 true 已处理
bool XjsAnyEditBoxHit(POINT pt);                  // 任一输入框命中 (搜索框/行内重命名): I-beam 光标判定
bool XjsChromeMouseUp(POINT pt);
void XjsShowAppMenu();                            // ☰ 菜单
void XjsShowHistoryPanel();
void XjsShowFilterMenu(POINT screenPt);
void XjsShowContextMenu(POINT screenPt, int targetIdx);   /* 文件右键菜单 (targetIdx=作用行, 调用方已保证其选中) */
void XjsShowSearchBoxMenu(POINT screenPt);        /* 搜索框右键: 剪切/复制/粘贴/全选/删除 (禁用态随选区) */
void XjsSearchMenuCmd(int cmd);                   /* 0剪切 1复制 2粘贴 3全选 4删除选中 */
void XjsSearchJumpToList(bool commit);            /* 回车/↓: 选中列表首项并聚焦列表 (源样式口径) */
void XjsShowToolboxMenu();
void XjsTrayAdd(HWND hwnd);
void XjsTrayRemove();
BOOL XjsHotkeysRegisterAll();     /* 注册全部档案槽的全局快捷键 (返回=主窗槽"未设置或注册成功", 供启动警告判定) */
void XjsHotkeysUnregisterAll();   /* 卸载全部档案槽的全局快捷键 */
std::wstring XjsHotkeyText(UINT mod, UINT vk);    /* 热键组合 → "Ctrl + Alt + A" 文本 (设置胶囊/主窗提醒共用) */

/* ---- 自绘搜索输入框 (实现在 xjs_chrome; 替代原生 EDIT: 文本/选区/光标全 D2D 绘制) ---- */
void XjsSearchCaretAttach();                      // 首次窗口创建后绑定光标闪烁驱动器 (WM_CREATE 内, 先于聚焦)
bool XjsSearchDragging();                         // 搜索框正拖拽选字 (WM_MOUSEMOVE 分流依据)
bool XjsSearchDragPending();                      // 未聚焦文本区按下待定: 动=拖窗, 原地松开=聚焦
void XjsSearchFocus(bool on);                     // 聚焦/失焦 (光标闪烁计时器+IME 组字窗定位); false=真失焦, 仅"其它输入框接管"用
void XjsSearchYieldKeys();                        // 键盘路由交列表但搜索框光标不灭 (点列表/↓/Tab/下拉菜单; 编辑键仍进搜索框)
const std::wstring& XjsSearchGetText();           // 当前输入内容
void XjsSearchSetText(const std::wstring& s);     // 置入文本 (光标到末尾, 触发输入即搜)
void XjsSearchSetTextQuiet(const std::wstring& s); // 置入文本不触发搜索 (插件 SearchSetText execute=0)
void XjsSearchCaretToEnd();                       // 光标移到末尾 (工具函数; 打字聚焦入口已删, 现无调用方)
void XjsSearchClear();                            // 清空 (留一键撤销)
bool XjsSearchKey(WPARAM vk);                     // WM_KEYDOWN (未聚焦仅 VK_BACK/←/→ 放行; 退格实删=聚焦, ←→ 不夺焦, 其余返回 false 归列表)
bool XjsSearchChar(wchar_t ch);                   // WM_CHAR / WM_IME_CHAR (未聚焦也接字, 实际写入即聚焦 — 文字输入/删除口径; 返回 true 已消费)
bool XjsSearchImeResult(HWND hwnd, LPARAM lParam);// WM_IME_COMPOSITION: GCS_RESULTSTR 整串上屏 (上屏实入文字=聚焦)
bool XjsSearchMouseDown(POINT pt);                // 按下: 聚焦+点定位光标 (Shift=扩展选区)
void XjsSearchMouseMove(POINT pt);                // 拖拽选字
void XjsSearchMouseUp(POINT pt);                  // 结束拖拽
bool XjsSearchDoubleClick(POINT pt);              // 双击选词
void XjsSearchUpdateImeWindow();                  // 组字窗贴到光标处 (重命名激活期让位给重命名框)

/* ---- xjs_list ---- */
double XjsListContentHeight();                    /* 449万×行高≈1.5亿px, double 防 float 量化 */
float XjsListViewHeight();
double XjsMaxScroll();
void XjsClampScroll();
void XjsListHoverChanged(int oldRow, bool wasInList);  /* 悬停行变化: 旧行高亮进渐隐拖尾并启动时钟 */
float XjsListHoverAlpha(int idx);                      /* 该行当前悬停亮度 0..1 (量化; 0=无) */
bool XjsListHoverTick();                               /* WM_TIMER(ID_TIMER_HOVERFADE): 修剪过期, 假=应 KillTimer */
void XjsScrollTo(double top);
void XjsEnsureVisible(int idx);
int XjsRowAtY(double yInList);
bool XjsIsGridView();                             // 网格模式 (中图标/大图标)
int XjsGridCols();                                // 网格每排格数 (按列表宽度算)
int XjsItemAtPoint(POINT pt);                     // 命中项目索引 (列表按行, 网格按格子)
void XjsSetViewMode(int m);                       // 切视图模式 (锚点保持滚动位置)
void XjsCycleViewMode(int dir);                   // Ctrl+滚轮: dir>0 放大(更大图标), dir<0 缩小
void XjsGetColumnRects(float listWidth, bool listView, float* xs, float* ws);
double XjsColumnsContentWidth(bool listView);     /* 列总宽 (含左右 padding), 超视口 → 横向滚动条 */
void XjsClampHScroll(bool listView);              /* g_hScroll 收敛到 [0, 内容宽-视口宽] */
int XjsHitTestColHandle(POINT pt);
void XjsShowColumnMenu(POINT screenPt);           /* 表头右键: 列显隐 (未开启字段置灰, 名称列恒显) */
void XjsColumnToggle(int fullIdx);                /* 菜单命令: 切换某列显隐 */
void XjsAutoFitColumn(int handleIdx);             /* 双击列宽手柄: 按可视内容自适应 (50~800) */
/* 选中 (权威在引擎结果对象, 宿主不存副本; 实现见 xjs_list.cpp) */
bool XjsSelIsSelected(int idx);                   /* 该行是否选中 (渲染/命中) */
int  XjsSelCount();                               /* 选中项数 (状态栏/确认文案) */
int  XjsSelPrimaryIdx();                          /* 主选中项 (焦点项优先, 回落最小选中索引) */
std::vector<int> XjsSelIndices();                 /* 全部选中行索引 (升序; 引擎按ID复制后反查) */
void XjsSelClear();                               /* 清空引擎选中 */
void XjsSelToggle(int idx);                       /* Ctrl+点击: 增删该行选中 */
void XjsSelectOnly(int idx);
void XjsSelectRange(int from, int to);
void XjsSelectAllRows();
bool XjsMarqueeMoved();                           /* 框选拖动中 (预览跟随冻结闸; 解除时调用方补刷新) */
void XjsListRender();
bool XjsListMouseDown(POINT pt, WPARAM flags);
bool XjsListScrollMouseDown(POINT pt);            /* 滚动条按下: thumb 抓取 / 轨道翻页+按住连发 (双击第二下同入口, 防落"打开文件") */
bool XjsListScrollRegionHit(POINT pt);            /* 点在滚动条区域 (纵+横): 单击打开待定武装的排除判定 */
bool XjsListTrackTick();                          /* WM_TIMER(ID_TIMER_SBTRACK): 轨道按住连发; 假=请 KillTimer */
bool XjsListMouseMove(POINT pt);
bool XjsListMouseUp(POINT pt);
void XjsListWheel(float delta);
bool XjsListKey(WPARAM vk);

/* ---- xjs_preview ---- */
void XjsPreviewToggle();
void XjsPreviewUpdateSelection();                 // 选中变化后刷新面板目标
void XjsPreviewPlugCacheInvalidate();             // 设备重建: 插件交付位图缓存作废 (位图绑死 RT 域)
void XjsPreviewRender();
bool XjsPreviewMouseDown(POINT pt);
bool XjsPreviewResizerHit(POINT pt);              // 分隔线命中带 (按下+光标共用, 宽于视觉线)
bool XjsPreviewMouseMove(POINT pt);
bool XjsPreviewMouseUp(POINT pt);
void XjsPreviewQueryBigDirs();                    // 查找大目录 (SQL)
void XjsPreviewQueryBigFiles();                   // 查找大文件 (SQL)
void XjsPreviewWheel(int dir);                    // Ctrl+滚轮缩放预览图片 (dir=±1)
void XjsPreviewScrollLines(int dir);              // 文本预览普通滚轮滚动 (dir=±1)
bool XjsPreviewIsText();                          // 当前预览目标是文本内容

/* ---- xjs_popup (自绘输入对话框; 自绘菜单接口见上方模块接口区) ---- */
std::wstring XjsGenerateWindowName();   /* 新窗口默认名称 = GUID (CoCreateGuid, 36 连字符格式) */
void XjsShowInputDialog(HWND owner, const wchar_t* title, const wchar_t* desc, const std::wstring& initial,
                        int resultId);            /* 自绘输入对话框 (WM_INPUT_DONE 回传给 owner) */

/* ---- main (窗口/控件) ---- */
void XjsApplyZoom(int tenths);                    // 应用页面缩放 (5..20; 重建字体/文本格式)
void XjsUpdateEditFont();                         // 按当前缩放重建 IME 组字窗口字体
void XjsHistoryNav(int dir);                      // Alt+←/→ 搜索历史后退/前进 (会话导航栈)
void XjsSummonActivate(HWND hwnd);                // 唤起到前台 (借前台线程; 插件 Summon API 复用同一实现)

/* ---- xjs_plugin (原生插件系统; 插件作者 SDK = xjs_plugin_sdk.h, 只有 xjs_plugin.cpp include 它。
   注册表/扫描/加载/闸门/票号收口 xjs_plugin.cpp; manifest 的 picojson 解析收口 xjs_engine.cpp ---- */

/* manifest capabilities / permissions → 位掩码 (解析在 xjs_engine.cpp, 消费在各挂接点) */
enum { XPC_FILECTX = 1 << 0, XPC_SEARCHBOXMENU = 1 << 1, XPC_SEARCHMODES = 1 << 2,
       XPC_HOSTED = 1 << 3, XPC_INPUTINTERCEPT = 1 << 4, XPC_STATUSBAR = 1 << 5,
       XPC_EVENTS = 1 << 6, XPC_PREVIEW = 1 << 7, XPC_BATCHRENAME = 1 << 8,
       XPC_PANEL = 1 << 9 };
enum { XPP_READ = 1 << 0, XPP_WRITE = 1 << 1, XPP_EXEC = 1 << 2, XPP_UI = 1 << 3,
       XPP_SETTINGS = 1 << 4 };   /* settings: 读写程序设置/增删运行时搜索模式 (扩展 API, 2026-09-24) */

/* 清单解析产物 (宿主内部用, 不跨界; 插件菜单 when: 0=any 1=file 2=dir 3=drive) */
struct XjsPluginMenuDef { std::wstring cmd, text; int order = 0; int when = 0; std::vector<std::wstring> exts; };
struct XjsPluginStatusBarDef { std::wstring cmd, label, title; int order = 0; };
struct XjsPluginModeDef { std::wstring id, name, desc, type; std::string tplUtf8; };   /* tpl 空 = OnSearchMode 接管 */
struct XjsPluginHostedDef { std::wstring id; std::vector<std::wstring> words; std::wstring mode; std::string queryUtf8; };
struct XjsPluginManifest {
    bool ok = false;
    std::wstring err;             /* 清单错误原因 (ok=false 时给管理页) */
    std::wstring id, name, version, author, description, iconFile;
    int type = 0;                 /* 0=library 1=app(自建窗口) */
    std::wstring dllName;         /* 相对插件目录; 空 = 纯声明式插件 */
    int abi = 0;
    unsigned caps = 0, perms = 0;
    std::vector<XjsPluginMenuDef> menus, searchBoxMenus;
    std::vector<XjsPluginStatusBarDef> statusBar;
    std::vector<XjsPluginModeDef> modes;
    std::vector<XjsPluginHostedDef> hosted;
    std::vector<std::wstring> previewExts;   /* 小写、不含点 */
};
bool XjsPluginManifestParse(const char* utf8Json, XjsPluginManifest* out);   /* xjs_engine.cpp (picojson 唯一点) */

/* 插件文件/目录选择对话框选项 (DialogJson 的 optsJson 解析产物; filter = [标签, 通配符] 对) */
struct XjsPluginDialogOpts {
    std::wstring title, initialDir, initialName;
    std::vector<std::pair<std::wstring, std::wstring>> filter;
};
bool XjsPluginDialogOptsParse(const char* utf8Json, XjsPluginDialogOpts* out);   /* xjs_engine.cpp */
int  XjsPluginDynMenuParse(const char* utf8Json, std::vector<XjsPluginMenuDef>* out);   /* BuildMenu 返回解析 (xjs_engine.cpp) */

/* 生命周期 (全部 UI 线程) */
void XjsPluginStartup();                      /* wWinMain: 扫描 + 按配置加载已启用 (配置已载后调一次) */
void XjsPluginShutdown();                     /* 主窗 WM_DESTROY 收尾: 逆序通知 (不 FreeLibrary, 照源样式口径) */
void XjsPluginRescan();                       /* 设置页"重新扫描": 收新目录/刷新清单; 已加载 DLL 不重载 (需重启) */
void XjsPluginOnWindowDestroyed(HWND hwnd);   /* 搜索窗 WM_DESTROY: 递增该槽窗口令牌代 (防槽位复用串窗) */
void XjsPluginOnWindowsCompacted(int fromSlot); /* 窗口注册表压缩 (DestroyAndFree): fromSlot 起令牌代全递增 */
std::wstring XjsPluginRootDir();              /* exe\plugins 运行时目录 (不自动创建) */

/* 设置-插件 页数据源 */
struct XjsPluginBrief {
    std::wstring id, name, version, author, description;
    bool declared = false;        /* manifest 解析成功 */
    std::wstring manifestErr;
    bool enabled = false;         /* 用户开关 (宿主闸门依据) */
    bool loaded = false;          /* DLL 已加载 Init 成功 */
    bool staleDll = false;        /* 重扫发现 DLL 比加载时新 → "需重启生效" */
    std::wstring loadErr;
    std::wstring dir;             /* "打开目录"用 */
    unsigned caps = 0, perms = 0;
    int type = 0;
    bool hasDll = false;
    std::wstring dllFile;         /* DLL 文件名 (确认框展示完整路径用; 纯声明式 = 空) */
};
int  XjsPluginCount();
bool XjsPluginBriefAt(int i, XjsPluginBrief* out);
bool XjsPluginNeedsConfirm(int i);                 /* 首次启用或版本变化 → 启用前弹风险确认框 */
void XjsPluginMarkConfirmed(int i);                /* 记当前版本为已确认 (调用方随后 XjsSaveConfig) */
bool XjsPluginEnable(int i, std::wstring* err);    /* 闸门开 + 有 DLL 时立即加载; 失败=false+原因, 不写回启用态 */
void XjsPluginDisable(int i);                      /* 闸门关 (不卸载); 其搜索模式/托管来源由调用方剔除并重搜 */
void XjsPluginOpenDir(int i);

/* 用户状态 (启用/已确认版本) — XjsLoadConfig/XjsSaveConfig 经此读写顶层 "插件" 键 (picojson 留 engine 文件) */
void XjsPluginUserStateSet(const wchar_t* id, bool enabled, const wchar_t* confirmedVer);
int  XjsPluginUserStateCount();
bool XjsPluginUserStateAt(int i, std::wstring* id, bool* enabled, std::wstring* confirmedVer);

/* 文件右键 / 搜索框右键菜单追加 (无活跃插件 = 零痕迹; 项 id = IDM_PLUGIN_BASE+票号, 回传进 OnMenuTicket)。
   ids = 文件上下文 (引擎 FileId, v3 口径: 宿主与插件的文件引用一律 ID, 路径各自直连引擎自取) */
void XjsPluginAppendFileMenuItems(std::vector<XjsPopupItem>& items,
                                  const std::vector<int>& ids, bool isDir, bool isDrive);
void XjsPluginAppendSearchBoxMenuItems(std::vector<XjsPopupItem>& items, const std::wstring& inputText);
void XjsPluginOnMenuTicket(int slot);              /* IDM_PLUGIN_BASE 段回传: 查票号表 → 插件 OnCommand */
bool XjsPluginActiveCap(unsigned capMask);         /* 有无"启用且可用"插件声明该能力 (零插件零开销短路) */
unsigned long long XjsPluginCurWindowToken();      /* 当前窗 (Cur) 的窗口令牌 (UI 线程; 搜索路径用) */

/* 统一模式来源 (xjs_engine.cpp): 用户自定义 + 插件清单模式合并视图。
   插件模式 srcId = "p:<插件id>:<模式序>"; 模板型 = 零代码入托管标签链, 无模板 = 接管型 (OnSearchMode) */
bool XjsModeViewById(const std::wstring& srcId, XjsCustomMode* out);   /* false = 来源已失效 */
void XjsPluginFireSearchModeBySrc(const std::wstring& srcId, const std::wstring& input);
std::wstring XjsPluginModeSrcId(const std::wstring& pluginId, int modeIdx);

/* ===== P1 搜索集成 ===== */
struct XjsPluginModeRef { int plugin = -1; int modeIdx = -1; };   /* 只读视图: 指向启用插件清单里的模式 */
int  XjsPluginModeCount();                         /* 启用插件的 searchModes 总数 */
bool XjsPluginModeAt(int i, XjsPluginModeRef* out);
const XjsPluginModeDef* XjsPluginModeDefAt(const XjsPluginModeRef& r);   /* 越界/已禁用 = NULL */
void XjsPluginFireSearchMode(const XjsPluginModeRef& r, unsigned long long window,
                             const std::wstring& input);   /* 接管型: 通知插件 OnSearchMode */
bool XjsPluginInputIntercept(const std::wstring& text);   /* true = 被某插件拦截本次搜索 (同步, UI 线程) */
int  XjsPluginStatusBarCount();
bool XjsPluginStatusBarAt(int i, XjsPluginStatusBarDef* out);
void XjsPluginStatusBarCommand(int i, unsigned long long window);   /* 状态栏项松开触发 → OnCommand */
void XjsPluginOnSearchComplete();                  /* WM_SEARCH_COMPLETE 尾部分发 */
void XjsPluginOnSelectionChanged();                /* 选中变化汇点分发 (预览刷新统一入口) */
void XjsPluginOnSyncAfter();                       /* 文件同步节流刷新点分发 (聚合语义, 无单文件载荷) */
void XjsPluginOnSkinChanged(unsigned long long windowToken);   /* 皮肤变化分发 (设置窗换肤后; window=换肤窗口令牌) */

/* ===== P2 内容接管 ===== */
bool XjsPluginPreviewTake(int fileId, int requestId, unsigned long long window);   /* true = 插件已接管 (将异步交付) */
/* 插件异步交付落点 (xjs_preview.cpp 实现: 校验 requestId 是否当前世代, 过期静默丢弃; 命中只失效预览区) */
bool XjsPreviewPluginDeliverBitmap(int requestId, int w, int h, const void* bgra, int stride);
bool XjsPreviewPluginDeliverText(int requestId, const char* utf8);
bool XjsPluginBatchRenameAvailable();
void XjsPluginBatchRename(const std::vector<int>& ids, unsigned long long window);

/* ===== P3 面板接管 (preview-panel 能力; 实现收口 xjs_preview.cpp "面板接管"节) =====
   插件整块接管预览面板体 (含原头部带, 宿主头部不画; 关闭 = 插件自绘 ✕ → SDK PanelClose,
   结束接管并按打开前状态恢复)。
   交互 = 宿主转发鼠标/滚轮/键盘/IME + 插件交付整块位图 (世代对齐, 过期静默丢弃)。
   会话状态住 XjsSearchWindow (plugPanel* 字段); 这组入口即窗口级操作 (一律作用 Cur 或显式窗) */
bool XjsPreviewPanelOpen(XjsSearchWindow* w, unsigned long long window, const wchar_t* pluginId);   /* 开/激活 (幂等; 预览没开先展开) */
void XjsPreviewPanelClose(XjsSearchWindow* w, unsigned long long window, bool restore);   /* restore=false=窗口销毁路径不回写配置 */
XjsRect XjsPreviewPanelContentRect();             /* 面板体矩形 (整块含头部带; 渲染/命中/坐标换算同源) */
void XjsPreviewPanelSyncSize(bool notify);        /* 尺寸世代同步 (变化则 serial++ 并派发 RESIZE; notify=false 只记账) */
bool XjsPreviewPanelRectOf(XjsSearchWindow* w, HWND* hwnd, int* x, int* y, int* w2, int* h2);
                                                  /* 面板内容区矩形 → 真子窗口型面板 (WebView2) 定位:
                                                     hwnd = 所属窗, x/y/w/h = 客户区物理像素 (未接管 = false) */
void XjsPreviewPanelRender();                     /* 绘制接管位图 (XjsPreviewRender 接管分支; 绘制帧兼探测尺寸失配投 WM_PANEL_RESYNC) */
bool XjsPreviewPanelMouseDown(POINT pt);          /* 内容区命中 → 转发 LDOWN (带捕获; 假=未接管或不在内容区) */
bool XjsPreviewPanelMouseMove(POINT pt);          /* 捕获中/悬停内容区 → 转发 MOVE */
bool XjsPreviewPanelMouseUp(POINT pt);            /* 转发 LUP (收捕获) */
bool XjsPreviewPanelWheel(POINT pt, int delta, unsigned flags);   /* 转发滚轮 (Ctrl/普通都转, 插件自决) */
bool XjsPreviewPanelWantsPt(POINT pt);            /* 接管中且 pt 落内容区 (dblclk/rb 分支前置判定) */
void XjsPreviewPanelMouse(int type, POINT pt);    /* DBLCLK/RDOWN/RUP 转发 (type = XJS_PANEL_*, 主窗内联小转发用) */
void XjsPreviewPanelMouseLeave();                 /* WM_MOUSELEAVE: 转发 x=y=-1 (指针离面板, 悬停态复位) */
void XjsPreviewPanelKeyBlur(POINT pt);            /* 面板外宿主点击: 收回键盘让渡 + KEY_BLUR 事件 (插件字段失焦) */
/* 宿主表落点包装 (xjs_plugin.cpp FnPanel* 调; 交付任意线程/读取任意线程, 状态由 s_panelCs 保护) */
bool XjsPreviewPanelDeliver(XjsSearchWindow* w, long long serial, int w2, int h, const void* bgra, int stride);
void XjsPreviewPanelInfo(XjsSearchWindow* w, long long* serial, int* w2, int* h, float* scale);
void XjsPreviewPanelKey(unsigned vk);             /* KEY_DOWN (插件持键盘期间主窗路由) */
void XjsPreviewPanelChar(unsigned int ch);        /* KEY_CHAR (键盘字符/IME 上屏统一码点入口) */
bool XjsPreviewPanelImeResult(HWND hwnd, LPARAM lParam);   /* GCS_RESULTSTR 整串取回 → 拆码点转发 (消费=真) */
void XjsPreviewPanelUpdateIme(HWND hwnd);         /* 组字/候选窗锚定到面板光标 (PanelSetCaret 记的点位) */
void XjsPreviewPanelFocus(HWND hwnd, bool active);/* 宿主 WM_ACTIVATE → FOCUS 事件 (熄插件自绘光标) */
void XjsPreviewPanelOnWindowClosing(HWND hwnd);   /* WM_DESTROY: 令牌代递增前派发 CLOSE (窗口令牌仍有效) */
void XjsPreviewPanelCloseForToggle();             /* 设置/预览开关把面板藏起来时先结束会话 (restore=false) */
/* 宿主表落点与派发 (xjs_plugin.cpp 实现; 注册表/能力位是插件模块实现细节) */
void XjsPluginPanelDispatch(unsigned long long window, int type, long long serial,
                            int w, int h, float scale, int x, int y, int delta,
                            unsigned flags, unsigned ch);   /* 按会话 owner 插件派发 OnPanelEvent (纯 C 形参打包 SDK 事件) */
void XjsPluginPanelValidateOwners();              /* 插件禁用/重扫后校验: owner 失效的会话一律结束 (restore=true) */
