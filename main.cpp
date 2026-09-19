/*
 * main.cpp — 窗口过程编排 + 程序入口
 * 界面渲染/命中/交互分派到各模块 (xjs_chrome / xjs_list / xjs_preview / xjs_popup)
 */
#include "xjs_app.h"

/* ---- 崩溃取证 (常驻设施, 非临时诊断 — 排查完后不要连根删): 阶段标记 + 致命异常自动落盘 ----
   XjsVecStackLogger = 排最前的 VEH, 在任何处理器之前取崩溃栈, 写 exe 目录 startup_stack.txt
   (阶段 + 逐帧 模块!偏移, 供 dumpbin /disasm + llvm-symbolizer 反查符号) */
static const wchar_t* g_phase = L"init";
void XjsSetPhase(const wchar_t* p) { g_phase = p; }   /* 公共声明进 xjs_app.h (插件系统也标阶段) */

static int XjsAppMain(HINSTANCE hInstance, HINSTANCE hPrev, LPWSTR lpCmdLine, int nCmdShow);

/* 诊断日志统一写 exe 目录 (UAC 提权后 CWD 会被重置, 相对路径会把文件写到别处) */
static void XjsLogPath(const wchar_t* name, wchar_t* out, DWORD cap) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t* cut = exe + wcslen(exe);
    while (cut > exe && cut[-1] != L'\\') cut--;
    *cut = 0;
    _snwprintf(out, cap, L"%s%s", exe, name);
}

/* 排最前的 VEH: 只记录致命类异常, 静默写 startup_stack.txt (阶段+逐帧 模块!偏移)。
   良性探测(线程命名/调试输出/断点/单步)不记录不拦截, 直接放行给后续处理器 */
static LONG CALLBACK XjsVecStackLogger(PEXCEPTION_POINTERS ei) {
    static int s_busy = 0;
    if (s_busy) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = ei->ExceptionRecord ? ei->ExceptionRecord->ExceptionCode : 0;
    if (!(code == 0xC0000005 || code == 0xC0000002 || code == 0xC0000409 ||
          code == 0xC00000FD || code == 0xC000001D || code == 0xC0000094))
        return EXCEPTION_CONTINUE_SEARCH;
    s_busy = 1;
    void* frames[32] = {};
    USHORT n = CaptureStackBackTrace(0, 32, frames, NULL);
    wchar_t sp[MAX_PATH];
    XjsLogPath(L"startup_stack.txt", sp, MAX_PATH);
    HANDLE f = CreateFileW(sp, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        wchar_t line[MAX_PATH + 64];
        DWORD written = 0;
        swprintf(line, MAX_PATH + 64, L"phase=%s code=0x%08X addr=%p\r\n",
                 g_phase, code, ei->ExceptionRecord ? ei->ExceptionRecord->ExceptionAddress : NULL);
        WriteFile(f, line, (DWORD)wcslen(line) * sizeof(wchar_t), &written, NULL);
        for (USHORT i = 0; i < n; i++) {
            HMODULE mod = NULL;
            wchar_t mn[MAX_PATH] = L"?";
            unsigned long long off = (unsigned long long)(uintptr_t)frames[i];
            if (frames[i] && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                (LPCWSTR)frames[i], &mod) && mod) {
                GetModuleFileNameW(mod, mn, MAX_PATH);
                const wchar_t* base = mn;
                for (const wchar_t* q = mn; *q; q++) if (*q == L'\\') base = q + 1;
                off = (unsigned long long)((const BYTE*)frames[i] - (const BYTE*)mod);
                swprintf(line, MAX_PATH + 64, L"  #%02u %s+0x%llx\r\n", i, base, off);
            } else {
                swprintf(line, MAX_PATH + 64, L"  #%02u %p\r\n", i, frames[i]);
            }
            WriteFile(f, line, (DWORD)wcslen(line) * sizeof(wchar_t), &written, NULL);
        }
        CloseHandle(f);
    }
    s_busy = 0;
    return EXCEPTION_CONTINUE_SEARCH;   /* 只取证不吞异常, 后续处理器照常工作 */
}

/* ==================== 搜索输入 EDIT 子类化 ==================== */

/* ==================== 搜索输入 (自绘框的 IME 字体) ==================== */

/* 页面缩放应用: 重建文本格式与 IME 组字字体 (同正式版 WebView2 ZoomFactor 缩放整页) */
void XjsApplyZoom(int tenths) {
    if (tenths < 5) tenths = 5;
    if (tenths > 20) tenths = 20;
    if (tenths == g_uiZoomTenths) return;
    g_uiZoomTenths = tenths;
    XjsRecreateTextFormats();
    XjsUpdateEditFont();
    XjsSaveConfig();
    XjsSearchWindow::Cur()->Invalidate();
}

void XjsUpdateEditFont() {
    if (!g_hWnd) return;
    UINT dpi = XjsWindowDpi(g_hWnd);
    HFONT nf = CreateFontW(-MulDiv((int)(12 * XjsUiZoom() + 0.5f), (int)dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (nf) {
        if (g_hFontEdit) DeleteObject(g_hFontEdit);
        g_hFontEdit = nf;
    }
    if (g_searchFocused) XjsSearchUpdateImeWindow();   /* 组字窗字体跟随 */
}

/* Ctrl 组合缩放键 (Ctrl+= 放大 / Ctrl+- 缩小 / Ctrl+0 重置; 正式版步长 0.1, 范围 0.5~2.0) */
static bool XjsHandleZoomKey(WPARAM wParam) {
    if (!(GetKeyState(VK_CONTROL) & 0x8000)) return false;
    if (wParam == VK_OEM_PLUS || wParam == VK_ADD) { XjsApplyZoom(g_uiZoomTenths + 1); return true; }
    if (wParam == VK_OEM_MINUS || wParam == VK_SUBTRACT) { XjsApplyZoom(g_uiZoomTenths - 1); return true; }
    if (wParam == '0') { XjsApplyZoom(10); return true; }
    return false;
}

/* ==================== 统一关闭入口 ====================
 * 任何来源的窗口关闭 (系统 WM_CLOSE/外部进程消息/标题栏 ✕(子窗)/托盘菜单/退出菜单) 一律走这里。
 * 撕毁期嵌套消息会经 Enter 重绑当前窗上下文, 宏若解析到别的窗口 = double-free 崩溃,
 * 故全程 XjsWindowScope 钉住目标窗; 后续退出选择提示 (源样式 closeAction: ask/exit/hide)
 * 只需在 XCLOSE_DEFAULT 分支加一档询问, 各来源不再散改 */
enum XjsCloseHow { XCLOSE_DEFAULT = 0, XCLOSE_EXIT = 1 };   /* EXIT=强制真退出 (托盘菜单), 无视藏托盘口径 */
static void XjsAppCloseWindow(HWND hwnd, XjsCloseHow how) {
    XjsSearchWindow* w = XjsSearchWindow::OfHwnd(hwnd);
    if (!w) { DestroyWindow(hwnd); return; }
    XjsWindowScope scope(w);
    if (how == XCLOSE_EXIT || !w->CloseRequest()) DestroyWindow(hwnd);
    /* CloseRequest 返回真 = 末窗藏托盘 (源样式 closeAction=hide), 窗口保留 */
}

/* ==================== 窗口消失统一策略入口 ====================
 * 用户请求"让窗口消失" (标题栏 ✕ / 全局热键 / 失焦关闭 / 双击 Ctrl / Esc 末级) 只准走这里:
 *   子窗 → 统一关闭入口真销毁 (窗口+结果对象; 档案留启动器可重建);
 *   主窗 → 藏托盘 (默认窗口除外, 永不销毁; 结果保留, 托盘/双击 Ctrl 再按唤回)。
 * 禁止在任何调用点直接对搜索窗 ShowWindow(SW_HIDE) 绕过本函数
 * (2026-09-17 实锤: 标题栏 ✕ 与热键各自 SW_HIDE, 子窗"关闭"只留下隐藏窗口, 启动器 ✓ 不灭) */
void XjsDismissWindow(HWND hwnd) {
    XjsSearchWindow* w = XjsSearchWindow::OfHwnd(hwnd);
    if (!w) return;
    if (w->isMain) { if (w->hWnd) ShowWindow(w->hWnd, SW_HIDE); }
    else XjsAppCloseWindow(hwnd, XCLOSE_DEFAULT);
}

/* ==================== 前台线程借用 (AttachThreadInput) ====================
 * 双击 Ctrl / 全局热键唤起或创建窗口时, 本进程通常不在前台 — 系统前台锁 (硬编码在
 * win32k 内核: 只有前台线程能无条件把窗口提到前台) 会让后台进程的 SetForegroundWindow
 * 落空 (窗口不上来/只闪任务栏)。口径照用户提供的参考实现 (w_线程_借用前台权限):
 * 借当前前台窗口线程的输入队列完成激活, 作用域结束立即归还; 前台本就是本进程时零开销
 * 不借用 (同进程窗口间切换/菜单点击后的创建 都不会触发 ATTACH)。
 * 已有前台授权的通道 (WM_WAKEUP 前第二实例已 AllowSetForegroundWindow) 借用同样无害 */
struct XjsForegroundBorrow {
    DWORD fgThread = 0, myThread = 0;
    bool attached = false;
    XjsForegroundBorrow() {
        myThread = GetCurrentThreadId();
        fgThread = GetWindowThreadProcessId(GetForegroundWindow(), NULL);
        if (fgThread && fgThread != myThread)
            attached = AttachThreadInput(myThread, fgThread, TRUE) != FALSE;
    }
    ~XjsForegroundBorrow() {
        if (attached) AttachThreadInput(myThread, fgThread, FALSE);   /* 还回去 */
    }
};

/* 唤起既有窗口统一入口 (借前台 + 恢复显示 + 提前台): 双击Ctrl/全局热键/托盘/启动器/WAKEUP 共用;
   "默认无焦点"口径由窗口自己的 WM_ACTIVATE 侧保证, 这里不管搜索框。
   公共声明进 xjs_app.h (插件系统的 Summon 宿主 API 复用同一实现) */
void XjsSummonActivate(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    XjsForegroundBorrow borrow;
    ShowWindow(hwnd, SW_RESTORE);   /* 兼顾藏托盘与最小化两种隐藏态 */
    SetForegroundWindow(hwnd);
}

/* 按档案槽唤起窗口 —— 三个触发源 (双击 Ctrl / 全局热键 / ☰启动器子菜单) 共用:
   toggle=true : 已打开且前台中 → 消失, 否则激活 (双击 Ctrl 与热键的"再按一次收起"语义)
   toggle=false: 已打开 → 激活 (启动器子菜单只做唤起, 菜单点不关窗)
   未打开 → 按该槽档案重建 (OpenNew 内部自带前台借用) */
static void XjsSummonSlot(int slot, bool toggle) {
    if (slot < 0) return;
    XjsSearchWindow* t = XjsSearchWindow::AtSlot(slot);
    if (t && t->hWnd) {
        if (toggle && GetForegroundWindow() == t->hWnd) XjsDismissWindow(t->hWnd);
        else XjsSummonActivate(t->hWnd);   /* 借前台线程激活 (触发时本进程多半在后台) */
    } else {
        XjsSearchWindow::OpenNew(slot);
    }
}

/* 激活/创建位置策略 (每窗设置 appearPos, WM_ACTIVATE 激活侧调用; 创建期同经首次激活生效)。
 * 档位序与设置菜单/配置项同源 (xjs_app.h XJS_APPEAR_* 是唯一事实源):
 *   1=跟随鼠标 (左上角贴光标右下 +12px)
 *   2..6 = (主屏)       居中/左上角/右上角/左下角/右下角
 *   7..11= (鼠标所在屏幕) 同五落点
 *   0=之前的位置: 不动 (子窗按档案槽记忆的矩形恢复, 见 OpenNew)
 * 落点一律夹进显示器工作区 rcWork = 不覆盖任务栏 (贴角/贴光标越界同样收回) */
void XjsSearchWindow::ApplyAppearPos() {
    if (!hWnd || appearPos <= XJS_APPEAR_NONE || appearPos >= XJS_APPEAR_COUNT) return;
    RECT wr;
    if (!GetWindowRect(hWnd, &wr)) return;
    const int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    /* 档位解读 (全库唯一处, 序见 xjs_app.h XJS_APPEAR_*):
       跟随鼠标 = 屏组取鼠标所在屏 + 落点用光标位; 其余 = (档位 − BASE) 的商为屏组、余数为落点 */
    const bool follow = (appearPos == XJS_APPEAR_FOLLOW);
    const int scr = follow ? XJS_SCR_CURSOR : (appearPos - XJS_APPEAR_SCREEN_BASE) / XJS_SPOT_COUNT;
    const int spot = (appearPos - XJS_APPEAR_SCREEN_BASE) % XJS_SPOT_COUNT;   /* follow 时不参与 */
    POINT cpt = { 0, 0 };
    if (scr == XJS_SCR_CURSOR) GetCursorPos(&cpt);   /* 主屏档不读光标位置 */
    HMONITOR mon = (scr == XJS_SCR_CURSOR) ? MonitorFromPoint(cpt, MONITOR_DEFAULTTONEAREST)
                                           : MonitorFromWindow(NULL, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(mon, &mi)) return;
    const RECT wk = mi.rcWork;
    int x, y;
    if (follow) {
        x = cpt.x + 12;   /* 贴光标右下角, 越右/下缘交给下方统一收口 */
        y = cpt.y + 12;
    } else {
        switch (spot) {
            case XJS_SPOT_LT: x = wk.left;       y = wk.top; break;
            case XJS_SPOT_RT: x = wk.right - ww; y = wk.top; break;
            case XJS_SPOT_LB: x = wk.left;       y = wk.bottom - wh; break;
            case XJS_SPOT_RB: x = wk.right - ww; y = wk.bottom - wh; break;
            default:          /* XJS_SPOT_CENTER */
                x = wk.left + ((wk.right - wk.left) - ww) / 2;
                y = wk.top + ((wk.bottom - wk.top) - wh) / 2;
                break;
        }
    }
    /* 统一收口工作区: 常规尺寸必落在工作区内 (不覆盖任务栏);
       窗口比工作区还大时保左上缘可见, 溢出朝右下 (两难时优先露出标题栏) */
    x = ximax((int)wk.left, ximin(x, (int)wk.right - ww));
    y = ximax((int)wk.top, ximin(y, (int)wk.bottom - wh));
    SetWindowPos(hWnd, NULL, x, y, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
}

/* 任务栏图标 (每窗设置 taskbarIcon): 关 = WS_EX_TOOLWINDOW — 任务栏不出现本窗按钮,
   同时不进 Alt+Tab 列表 (tool window 语义, 描述文案里已写明)。任务栏按钮在窗口
   "变为可见"时按样式重建, 可见期改样式须 hide/show 一轮才生效; 最小化态跳过循环,
   还原时的可见性转换自然按新样式重建 */
void XjsSearchWindow::ApplyTaskbarIcon() {
    if (!hWnd) return;
    LONG_PTR ex = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);
    bool hasTool = (ex & WS_EX_TOOLWINDOW) != 0;
    if (hasTool == !taskbarIcon) return;   /* 已是目标态 */
    if (taskbarIcon) ex &= ~WS_EX_TOOLWINDOW;
    else ex |= WS_EX_TOOLWINDOW;
    SetWindowLongPtrW(hWnd, GWL_EXSTYLE, ex);
    if (IsWindowVisible(hWnd) && !IsIconic(hWnd)) {
        ShowWindow(hWnd, SW_HIDE);
        ShowWindow(hWnd, SW_SHOWNOACTIVATE);   /* 不抢前台 (设置窗操作中切换不拽焦点) */
    }
}

/* 创建套用 (WM_CREATE 内调用, 窗口显示之前):
 * ① 档案默认排序下发结果对象 (排序态事实源仍是结果对象, 此处只是初始值);
 * ② 按"创建窗口填入搜索框"填入初始词: 用户指定关键词 / 上一次输入的搜索词
 *    (按窗口档案记忆; 新空白窗口无记忆时回落发起窗当前词);
 * ③ 执行一次搜索 (用户口径: 窗口创建完成还没显示就要先搜, 亮出来即有结果)。
 * 数据库未就绪时结果对象缺席 → 只填词不搜 (主窗随 加载完成广播 的首搜会带上填入词) */
void XjsSearchWindow::ApplyCreateSetup() {
    XjsUiProfile* p = XjsUiProfileAt(uiIndex);
    if (result && p && !p->sortField.empty())
        xjs_result_SetSortField(result, Utf16ToUtf8(p->sortField.c_str()).c_str(), p->sortWay ? TRUE : FALSE);
    switch (createFill) {
        case 1:
            if (!createKeyword.empty()) {
                searchEd.SetText(createKeyword, false);
                XjsSearchNow(false);
                return;
            }
            break;
        case 2: {
            std::wstring fill;
            if (p) fill = p->lastSearch;
            if (fill.empty()) { XjsSearchWindow* m = Main(); if (m && m != this) fill = m->searchEd.text; }
            if (!fill.empty()) {
                searchEd.SetText(fill, false);
                XjsSearchNow(false);
                return;
            }
            break;
        }
    }
    XjsSearchNow(false);   /* 清空/无词: 空词首搜 (允许空词搜索 开启时亮出来即全量列表) */
}

/* ==================== 弹出菜单命令分发 ==================== */

/* ====== 模态钟罩 (源样式 .exit-mask 口径: rgba(15,18,26,.45) + blur(6px)+saturate(1.1)) ======
   别名框/通用询问框是独立顶层窗, 背景由 owner 自绘: 弹窗打开期抓一帧弹窗前画面 (PrintWindow,
   每会话只抓一次缓存复用, 无逐帧反馈叠加) → CPU 1/2 降采样 + 单趟盒模糊 r=2 (≈CSS blur(6px)
   的 σ3 量级, 文字残留可见) → RT 域位图放大铺满 + 暗罩。
   实测铁律 (2026-09-16, 探针 d2dprobe.cpp 定案): 工厂直建 HwndRT QI 出的 ID2D1DeviceContext
   是旧式 DC — ① 普通 CreateBitmap 位图作 SetTarget 目标必回 D2DERR_INVALID_TARGET (0x88990024,
   注意不是 WRONG_RESOURCE_DOMAIN 0x88990015); ② 即便 OPTIONS_TARGET 位图可建可画, 也不能作为
   绘制源 (DrawBitmap 回 0x88990001, TARGET|CANNOT_DRAW 组合同样); ③ Effect/CommandList 回放
   一律失败。⇒ GPU 全离屏管线在本架构不可行 (除非引入 DXGI 交换链, 已被口径否决), 钟罩模糊
   只能走 CPU; 但 SetTarget 到 TARGET 位图上"画"是可用的, EndDraw 失败自愈网仍保留在 WM_PAINT。 */
static XjsBitmap* s_bdSmall = NULL;   /* 1/2 模糊小图 (hwndRT 域) */
static int s_endDrawFailStreak = 0;     /* EndDraw 连续失败计数 (自愈网, 见 WM_PAINT) */

static void XjsBackdropDiscard() {
    if (s_bdSmall) { s_bdSmall->Release(); s_bdSmall = NULL; }
}

/* 盒模糊一趟 (水平+垂直, RGB); 单趟 r=2 在 1/2 尺度上 ≈ σ2.3px 全尺度 */
static void XjsBoxBlur(uint8_t* px, int w, int h, int r) {
    std::vector<uint8_t> tmp((size_t)w * h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 3; c++) {
                int sum = 0, n = 0;
                for (int i = x - r; i <= x + r; i++) {
                    if (i < 0 || i >= w) continue;
                    sum += px[((size_t)y * w + i) * 4 + c]; n++;
                }
                tmp[((size_t)y * w + x) * 4 + c] = (uint8_t)(sum / n);
            }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 3; c++) {
                int sum = 0, n = 0;
                for (int j = y - r; j <= y + r; j++) {
                    if (j < 0 || j >= h) continue;
                    sum += tmp[((size_t)j * w + x) * 4 + c]; n++;
                }
                px[((size_t)y * w + x) * 4 + c] = (uint8_t)(sum / n);
            }
}

/* 抓本窗合成像素 (弹窗尚未画暗罩的干净帧, 每会话只抓一次) → 1/2 均值降采样 →
   单趟盒模糊 → RT 域位图 (调用方在 BeginDraw 之前调) */
static bool XjsBackdropBuild(HWND hwnd, const RECT& cr) {
    int w = cr.right - cr.left, h = cr.bottom - cr.top;
    int sw = w / 2, sh = h / 2;
    if (sw < 8 || sh < 8) return false;
    HDC wdc = GetWindowDC(hwnd);
    if (!wdc) return false;
    HDC mem = CreateCompatibleDC(wdc);
    HBITMAP hb = CreateCompatibleBitmap(wdc, w, h);
    HGDIOBJ old = SelectObject(mem, hb);
    BOOL got = PrintWindow(hwnd, mem, 2);   /* 2=PW_RENDERFULLCONTENT: 抓 DX/DWM 合成内容, 不触发 WM_PRINT */
    std::vector<uint8_t> src((size_t)w * h * 4);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;   /* top-down */
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    if (got) got = GetDIBits(mem, hb, 0, (UINT)h, src.data(), &bi, DIB_RGB_COLORS) != 0;
    SelectObject(mem, old);
    DeleteObject(hb);
    DeleteDC(mem);
    ReleaseDC(hwnd, wdc);
    if (!got) return false;
    /* 1/2 均值降采样 (2x2 块), A=255 */
    std::vector<uint8_t> smlBuf((size_t)sw * sh * 4);
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++) {
            int sb = 0, sg = 0, sr = 0;
            for (int j = 0; j < 2; j++)
                for (int i = 0; i < 2; i++) {
                    const uint8_t* p = &src[((size_t)(y * 2 + j) * w + (x * 2 + i)) * 4];
                    sb += p[0]; sg += p[1]; sr += p[2];
                }
            uint8_t* o = &smlBuf[((size_t)y * sw + x) * 4];
            o[0] = (uint8_t)(sb >> 2); o[1] = (uint8_t)(sg >> 2); o[2] = (uint8_t)(sr >> 2); o[3] = 255;
        }
    XjsBoxBlur(smlBuf.data(), sw, sh, 2);
    if (s_bdSmall) { s_bdSmall->Release(); s_bdSmall = NULL; }
    /* 内存字节 = B8G8R8A8 (alpha 忽略; PrintWindow 抓屏产物), 后端各自成图 */
    g_rt->CreateBitmapFromMemory((UINT32)sw, (UINT32)sh, (UINT32)sw * 4, smlBuf.data(), &s_bdSmall);
    if (!s_bdSmall || !s_bdSmall->h) {
        return false;
    }
    return true;
}

static bool XjsBackdropBegin(HWND hwnd, const RECT& cr) {
    if (!XjsModalOverlayFor(g_hWnd)) {
        XjsBackdropDiscard();
        return false;
    }
    if (s_bdSmall) return true;   /* 已有: 内容取自弹窗前画面, 主窗在弹窗期几乎不再重绘 */
    if (XjsBackdropBuild(hwnd, cr)) {
        return true;
    }
    return false;
}
static void XjsBackdropEndBlur(XjsRt* rt, const RECT& cr) {
    if (s_bdSmall) {
        rt->DrawBitmap(s_bdSmall, XjsRectF(0, 0, (float)cr.right, (float)cr.bottom),
                       1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    rt->FillRectangle(XjsRectF(0, 0, (float)cr.right, (float)cr.bottom),
                      XjsTempBrush(XjsColorF(15.0f / 255, 18.0f / 255, 26.0f / 255, 0.45f)));
}

void XjsOnPopupResult(int id) {
    if (id >= IDM_MODE_BASE && id < IDM_MODE_BASE + 4) {
        g_mode = id - IDM_MODE_BASE;
        XjsSaveConfig();
        XjsSearchNow(false);
    } else if ((id & ~(XJS_POPUP_EDIT | XJS_POPUP_DEL)) >= IDM_CMODE_BASE
            && (id & ~(XJS_POPUP_EDIT | XJS_POPUP_DEL)) < IDM_CMODE_BASE + 100) {
        /* 尾部按钮回传的 id 带高位标志, 范围判定必须先剥掉 —
           曾直接用带标志 id 判范围, 永远落不进本分支: ✎ 编辑点击=无操作 (2026-09-16 实锤)
           菜单下标 → 模式按打开时的 id 快照定位 (作用范围过滤后下标≠全局下标,
           菜单开着时容器也可能被其它窗口改动 — 禁止拿下标直索引 g_customModes) */
        std::wstring mid = XjsCmodeMenuIdAt((id & ~(XJS_POPUP_EDIT | XJS_POPUP_DEL)) - IDM_CMODE_BASE);
        if (mid.rfind(L"p:", 0) == 0) {
            /* 插件模式 (id 快照混存 "p:<插件id>:<序>"): 模板型 = 转托管标签 (同名合并来源,
               与用户模式同链); 接管型 (无模板) = 通知插件 OnSearchMode, 插件自行驱动搜索 */
            XjsCustomMode v;
            if (XjsModeViewById(mid, &v)) {
                if (v.tpl.empty()) XjsPluginFireSearchModeBySrc(mid, XjsSearchGetText());
                else XjsHostedWordAsTag(v.name, false);
            }
        } else if (id & XJS_POPUP_DEL) {   /* 行内 ✕ 二次确认后删除 (源样式菜单 ✕ → removeCustomMode) */
            if (!mid.empty()) {
                for (size_t i = 0; i < g_customModes.size(); i++)
                    if (g_customModes[i].id == mid) { g_customModes.erase(g_customModes.begin() + i); break; }
                XjsSaveConfig();
                XjsHostedPurgeMode(mid);   /* 标签链剔除该模式来源并重搜 (源样式 purgeHostedSources) */
                XjsToastShow(g_hWnd, XjsT(L"模式对话框.已删除"), XTOAST_SUCCESS, XSF(1));
            }
        } else if (id & XJS_POPUP_EDIT) {   /* 尾部 ✎: 编辑该模式 (源样式菜单项内联编辑按钮) */
            if (!mid.empty()) XjsModeDlgOpen(true, mid);
        } else {
            /* 点击=模式名转托管标签 (不切换全局模式; 输入词保留为链尾) — 源样式 hostWordAsTag */
            if (!mid.empty())
                for (auto& m : g_customModes)
                    if (m.id == mid) { XjsHostedWordAsTag(m.name, false); break; }
        }
    } else if (id >= IDM_HOSTED_SRC_BASE && id < IDM_HOSTED_SRC_BASE + 100) {
        XjsHostedPickSource(id - IDM_HOSTED_SRC_BASE);   /* 来源切换菜单: 换当前来源并重搜整链 */
    } else if (id >= IDM_HISTORY_BASE && id < IDM_HISTORY_BASE + 999) {
        int i = id - IDM_HISTORY_BASE;
        if (i < (int)g_history.size()) {
            XjsSearchSetText(g_history[i]);   /* 置入即触发搜索 */
        }
    } else if (id == IDM_HISTORY_BASE + 999) {
        g_history.clear();
        XjsSaveHistory();
    } else if (id >= IDM_FILTER_BASE && id < IDM_FILTER_BASE + 1000) {
        XjsApplyFilter(id - IDM_FILTER_BASE);
    } else if (id >= IDM_COL_BASE && id < IDM_COL_BASE + 12) {
        XjsColumnToggle(id - IDM_COL_BASE);
    } else if (id >= IDM_CTX_BASE + 20 && id < IDM_CTX_BASE + 25) {
        XjsSearchMenuCmd(id - (IDM_CTX_BASE + 20));   /* 搜索框右键: 剪切/复制/粘贴/全选/删除 */
    } else if (id >= IDM_CTX_BASE + 40 && id < IDM_CTX_BASE + 45) {
        XjsRenameMenuCmd(id - (IDM_CTX_BASE + 40));   /* 重命名编辑框右键: 同上 */
    } else if (id >= IDM_FCTX_BASE && id < IDM_FCTX_BASE + 5) {
        XjsEditFieldMenuCmd(g_hWnd, id - IDM_FCTX_BASE);   /* 路由层字段右键菜单 (搜索模式对话框等) */
    } else if (id >= IDM_CTX_BASE && id < IDM_CTX_BASE + 40) {
        int cmd = id - IDM_CTX_BASE;
        std::vector<int> sel = XjsSelIndices();            /* 选中集回读引擎 (升序) */
        int first = sel.empty() ? -1 : sel[0];
        switch (cmd) {
            case 1: if (first >= 0) XjsOpenFile(XjsItemPath(first)); break;
            case 2: if (first >= 0) XjsOpenFolderAndSelect(XjsItemPath(first)); break;
            case 3: XjsCopySelected(false); break;
            case 4: XjsCopySelected(true); break;
            case 5: if (first >= 0) XjsShowProperties(first); break;
            case 6: XjsDeleteSelected(); break;
            case 7: XjsCutSelected(); break;                                              /* 剪切 (CF_HDROP+MOVE) */
            case 8:                                                                       /* 复制 (CF_HDROP) */
                XjsCopyFilesToClipboard({}, false);
                g_statusText = XjsT(L"状态栏.已复制");
                break;
            case 9:
                if (sel.size() == 1 && first >= 0) XjsRenameStart(first);                /* 重命名 */
                break;
            case 10: XjsShowAliasDialog(); break;                                         /* 设置别名 */
            case 11: {                                                                    /* 多选打开 (>10 确认, 源样式 bulkConfirm) */
                if (sel.size() > 10 &&
                    XjsShowAskDialog(g_hWnd, XjsT(L"通用词.打开"),
                                     XjsFmt(XjsT(L"对话框.打开确认"), XjsNumText((long long)sel.size())).c_str(),
                                     ("[{\"text\":\"" + XjsTUtf8(L"打开") + "\",\"style\":\"primary\"},{\"text\":\"" +
                                      XjsTUtf8(L"取消") + "\"}]").c_str()) != 0) break;
                for (int idx : sel) XjsOpenFile(XjsItemPath(idx));
                break;
            }
            case 12: {                                                                    /* 多选定位 (>20 确认) */
                if (sel.size() > 20 &&
                    XjsShowAskDialog(g_hWnd, XjsT(L"通用词.打开所在文件夹"),
                                     XjsFmt(XjsT(L"对话框.定位确认"), XjsNumText((long long)sel.size())).c_str(),
                                     ("[{\"text\":\"" + XjsTUtf8(L"定位") + "\",\"style\":\"primary\"},{\"text\":\"" +
                                      XjsTUtf8(L"取消") + "\"}]").c_str()) != 0) break;
                for (int idx : sel) XjsOpenFolderAndSelect(XjsItemPath(idx));
                break;
            }
        }
    } else if (id >= IDM_TRAY_BASE && id < IDM_TRAY_BASE + 8) {
        /* 托盘右键菜单 (自绘弹窗, 结果经 WM_POPUP_RESULT 回托盘归属窗; 隐藏窗消息照常投递) */
        switch (id - IDM_TRAY_BASE) {
            case 1:   /* 恢复窗口 (同托盘左键口径; 借前台线程, 默认无焦点不回焦搜索框) */
                XjsSummonActivate(g_hWnd);
                break;
            case 2: XjsSettingsShow(); break;                          /* 设置 (owner=托盘归属窗) */
            case 3: XjsAppCloseWindow(g_hWnd, XCLOSE_EXIT); break;     /* 退出 = 强制真退出 */
        }
    } else if (id >= IDM_PLUGIN_BASE && id < IDM_PLUGIN_BASE + 200) {
        /* 插件菜单项 (票号表): 定位登记内容 → 插件 OnCommand。
           本分支必须排在无上界的 IDM_MENU_BASE 段之前; 票号上限见 xjs_plugin.cpp XJS_TICKET_MAX */
        XjsPluginOnMenuTicket(id - IDM_PLUGIN_BASE);
    } else if (id >= IDM_MENU_BASE) {
        /* 重建索引(原 case 1)已移入设置窗口 (ACT_REBUILD) */
        if (id >= IDM_MENU_BASE + 80 && id < IDM_MENU_BASE + 144) {
            /* 创建新窗口·子菜单 = 窗口启动器: 该档案槽已有存活窗口 → 激活 (借前台线程,
               默认无焦点口径同托盘唤起); 未打开 → 按槽位档案重建窗口 (菜单点击不收起窗口, toggle=false) */
            XjsSummonSlot(id - (IDM_MENU_BASE + 80), false);
        } else switch (id - IDM_MENU_BASE) {
            case 2: PostMessageW(g_hWnd, WM_CLOSE, 0, 0); break;
            case 46: XjsSearchWindow::OpenNew(); break;   /* 创建新窗口 (独立搜索结果对象) */
            case 41: XjsSetViewMode(VM_LIST); break;
            case 42: XjsSetViewMode(VM_DETAILS); break;
            case 43: XjsSetViewMode(VM_MEDIUM); break;
            case 44: XjsSetViewMode(VM_LARGE); break;
            case 45:
                XjsModeDlgOpen(false, L"");   /* 添加搜索模式 (源样式 smMask 弹窗) */
                break;
            case 14: XjsPreviewToggle(); break;
            case 50: XjsSettingsShow(); break;   /* 皮肤/自启动/关于在设置窗口 */
        }
    }
    XjsSearchWindow::Cur()->Invalidate();
}

/* ==================== 状态文本 ==================== */

static void XjsUpdateStatusTextOnComplete(int resultCount) {
    g_resultCount = resultCount;
    /* 源样式 Search.ResultSummary: 有无关键词一律 "共 N 项" (万位格式化), 无"找到 N 个结果"文案 */
    g_statusText = XjsFmt(XjsT(L"状态栏.共N项"), XjsWanText(g_resultCount));
    /* 选中集合无宿主副本: 引擎在结果刷新/重排序时自行维护 (被删文件自动清除), 不在此清理 */
}

/* ==================== 主窗口过程 ==================== */

/* 视口自愈: DPI 与 RT 尺寸每帧对齐窗口实际值。
   WM_DPICHANGED/WM_SIZE 通知链丢失或竞态时 (改显示缩放率/跨屏拖动), RT 与窗口脱节的
   表象 = 右缘窗口按钮被裁掉、可见画面与命中几何不一致 (点A选B)。
   DPI 事实源 = GetDpiForWindow (系统随 WM_DPICHANGED 同步维护的每窗有效 DPI)。
   曾用 GetDeviceCaps(窗口 DC): RDP 会话里改缩放它不实时更新, 每帧自愈会把
   WM_DPICHANGED 刚设置的新尺度改回去 (窗口缩了、文字停留旧尺度, 须重启进程才恢复) */
static void XjsSyncViewport(HWND hwnd) {
    UINT dpi = XjsWindowDpi(hwnd);
    if ((int)(g_s * 96.0f + 0.5f) != (int)dpi) {
        float oldDpi = g_s * 96.0f;
        g_s = dpi / 96.0f;
        /* 窗口框同步缩放 (尺寸值 × 新DPI/旧DPI): 保持逻辑尺寸不变 —
           WM_DPICHANGED 建议矩形丢失时的兜底, 否则 UI 变大挤在旧窗口里 */
        if (!IsIconic(hwnd) && !IsZoomed(hwnd) && oldDpi > 1.0f) {
            RECT wr;
            GetWindowRect(hwnd, &wr);
            int nw = (int)((wr.right - wr.left) * dpi / oldDpi + 0.5);
            int nh = (int)((wr.bottom - wr.top) * dpi / oldDpi + 0.5);
            SetWindowPos(hwnd, NULL, wr.left, wr.top, ximax(nw, 200), ximax(nh, 200),
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        XjsRecreateTextFormats();
        XjsUpdateEditFont();
        XjsPopupReleaseResources();
        XjsChromeLayout();
        XjsSearchWindow::Cur()->Invalidate();   /* 调用方都在本窗 WndProc 路径内, Cur()=本窗 */
    }
    if (g_rt) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        XjsSizeU px = g_rt->GetPixelSize();
        if ((int)px.width != ximax(rc.right, 1) || (int)px.height != ximax(rc.bottom, 1)) {
            XjsDeviceResize(ximax(rc.right, 1), ximax(rc.bottom, 1));
        }
    }
}

/* 单击打开待定 (每窗设置"鼠标打开=单击"): 按下时记录候选项目, 抬起时未拖动 (≤5px) 且
   无修饰键才真正打开 — OLE 拖出/框选/Ctrl·Shift 多选 都不成待定。双击时间窗内同一项目
   不重复打开 (单击模式的双击 = 一次打开) */
static int s_openPendIdx = -1;
static POINT s_openPendPt = {};
static unsigned long long s_lastClickOpenTick = 0;
static int s_lastClickOpenIdx = -1;

/* 状态栏按钮按下待定 (松开触发口径): 工具箱菜单 */
static bool s_sbToolboxPress = false;
static int s_sbPressPlug = -1;   /* 插件状态栏项待定下标 (松开仍命中同一项才触发, 同 s_sbToolboxPress 口径) */

/* ==================== 主窗口过程 ==================== */

LRESULT CALLBACK Xjs_WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    /* 多窗口: 入口先按 hwnd 绑定"当前窗上下文" — 之后的 g_* 宏全部解析到本窗。
       窗口创建最早期的消息 (表未入/无 pending) 无上下文 → 直接走默认处理 */
    XjsSearchWindow::Enter(hwnd);
    if (!XjsSearchWindow::Cur()) return DefWindowProcW(hwnd, msg, wParam, lParam);
        XjsSearchWindow* w = XjsSearchWindow::Cur();
        /* ===== 模态层总闸 (根治): 模态层打开时, 一切输入类消息在入口统一拦截 =====
         * 只放行 绘制/定时器/激活; IME 上下文走 DefWindowProc 但锚点钉到模态层字段。
         * 右键菜单/悬停/滚轮/打字泄漏的根因 = 零散拦截漏消息; 此后新增模态层不再逐消息打补丁 */
        /* 通用询问框打开期: 主窗只当"蒙层" — 输入全吞, 首次点击经激活路径让询问框失活收 -1
           (点蒙层空白=取消, 与源样式遮罩口径一致; 不吞则蒙层下的列表/按钮会被穿透误触) */
        if (XjsAskDialogOpen()) {
            switch (msg) {
                case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
                case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
                case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: case WM_XBUTTONUP: case WM_MOUSELEAVE:
                case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_CHAR: case WM_UNICHAR:
                case WM_IME_CHAR: case WM_IME_COMPOSITION: case WM_CONTEXTMENU:
                    return 0;
                default: break;
            }
        }
        if (XjsModeDlgActive()) {
            if (msg == WM_SETCURSOR) {
                if (LOWORD(lParam) == HTCLIENT) {
                    POINT cpt;
                    GetCursorPos(&cpt);
                    ScreenToClient(hwnd, &cpt);
                    SetCursor(LoadCursorW(NULL, XjsEditFieldHit(hwnd, cpt) ? IDC_IBEAM : IDC_ARROW));
                }
                return TRUE;   /* 模态期光标全由对话框接管 (列表 I-beam/调整手柄不再泄漏) */
            }
            if (msg == WM_IME_SETCONTEXT) {
                LRESULT r = DefWindowProcW(hwnd, msg, wParam, lParam);
                if (wParam) XjsEditFieldAnchorUpdate(hwnd);   /* 组字/候选窗钉到模态层聚焦字段 */
                return r;
            }
            switch (msg) {
                case WM_LBUTTONDOWN: {
                    POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                    XjsModeDlgMouseDown(pt);
                    return 0;
                }
                case WM_MOUSEMOVE: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
                case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
                case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
                case WM_MOUSELEAVE: case WM_CONTEXTMENU: case WM_SYSCOMMAND:
                case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
                case WM_CHAR: case WM_UNICHAR: case WM_IME_CHAR: case WM_IME_STARTCOMPOSITION:
                case WM_IME_COMPOSITION: case WM_IME_NOTIFY:
                    XjsModeDlgKey(hwnd, msg, wParam, lParam);   /* 全部进模态层按需消费 (未消费也吞) */
                    return 0;
                default: break;
            }
        }
        switch (msg) {
        case WM_CREATE: {
            XjsSetPhase(L"wm-create:init");
            UINT dpi = XjsWindowDpi(hwnd);
            g_s = dpi / 96.0f;
            g_hFontEdit = CreateFontW(-MulDiv((int)(12 * XjsUiZoom() + 0.5f), (int)dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            XjsSetPhase(L"wm-create:layout");
            XjsChromeLayout();
            if (w->isMain) {
                /* 引擎 (新头文件口径: xjs_Create() 零参 / EnableException 两参 / Load 状态码) — 仅主窗创建 */
                XjsSetPhase(L"wm-create:engine");
                g_engine = xjs_Create();
                if (!g_engine) {
                    MessageBoxW(hwnd, XjsT(L"错误.创建引擎失败"), XjsT(L"通用词.错误"), MB_OK | MB_ICONERROR);
                    PostQuitMessage(0);
                    return 0;
                }
                xjs_SetDefaultEngine(g_engine);
                xjs_SetCallback(g_engine, XJS_EVENT_LOAD_COMPLETE, (const void*)Xjs_LoadComplete, NULL);
                xjs_SetCallback(g_engine, XJS_EVENT_ENUM_PARTITION, (const void*)Xjs_EnumPartition, NULL);
                xjs_SetCallback(g_engine, XJS_EVENT_ENUM_PROGRESS, (const void*)Xjs_EnumProgress, NULL);
                xjs_SetCallback(g_engine, XJS_EVENT_ENUM_COMPLETE, (const void*)Xjs_EnumComplete, NULL);
                /* 文件同步变化 (创建/修改/移动/删除, SDK线程/引擎写锁内): 只置 g_fileChangePending,
                   由 ID_TIMER_SYNCWATCH 时钟节流重绘 (覆盖改名/修改这类不变结果数量的变化) */
                xjs_SetCallback(g_engine, XJS_EVENT_SYNC_CREATE, (const void*)Xjs_SyncFileChanged, NULL);
                xjs_SetCallback(g_engine, XJS_EVENT_SYNC_MODIFY, (const void*)Xjs_SyncFileChanged, NULL);
                xjs_SetCallback(g_engine, XJS_EVENT_SYNC_DELETE, (const void*)Xjs_SyncFileChanged, NULL);
                xjs_SetCallback(g_engine, XJS_EVENT_SYNC_MOVE,   (const void*)Xjs_SyncFileMoved, NULL);
                XjsLoadFilters();
            } else {
                /* 新搜索窗口: 引擎共享, 只建自己的结果对象。
                   界面设置应用本窗档案 (皮肤/视图/搜索模式/预览/列布局可不同于主窗); 无档案跟随主窗 */
                w->ApplyUiProfile();
                g_statusText = XjsT(L"状态栏.就绪");
            }
            XjsSetPhase(L"wm-create:device");
            XjsDeviceCreate();
            /* 每窗独立搜索结果对象 (共享引擎); UserValue 绑定窗口供回调区分。
               数据库未就绪时此处拒绝创建 (g_dbReady), 就绪后由 WM_LOAD_COMPLETE / WM_SCAN_COMPLETE 广播补建
               —— 创建时查询顺序即被 DLL 按库内字段定死, 早建 = 启动被强制文件名排序 (2026-09-15 实锤) */
            XjsEngineEnsureResult();
            w->ApplyCreateSetup();   /* 创建套用 (显示前): 默认排序 + 填入搜索词 + 执行一次首搜 */
            XjsSyncWatchStart(hwnd);   /* 文件同步变化轮询 (源样式 线程时钟 → 系统定时器) */
            /* 数据库加载不在此处提交: 主窗首次创建完成后才异步加载 (见 XjsAppMain) */
            XjsSetPhase(L"wm-create:timer");
            SetTimer(hwnd, ID_TIMER_STATUS, 500, NULL);
            if (w->isMain) {
                XjsSetPhase(L"wm-create:hotkey");
                if (!XjsHotkeysRegisterAll())
                    PostMessage(hwnd, WM_HOTKEY_WARN, 0, 0);   /* 明确设置过的热键注册失败: 创建完成后再提醒 */
                XjsSetPhase(L"wm-create:tray");
                XjsTrayAdd(hwnd);
            }
            /* 置顶档案恢复 (图钉态随窗口档案持久化; 启动/按档案重建都生效) */
            if (g_topmost)
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            /* 窗口图标 (任务栏按钮 + 悬停预览标题行): 类图标只是兜底, 窗口自身显式设一份 ——
               悬停预览/缩略图列表读的是窗口 ICON_SMALL, 只设类图标时预览标题行仍空白
               (2026-09-18 用户反馈: 任务栏按钮有图标, 悬停弹窗没有)。两尺寸都设, 见 xjs_util.cpp */
            SendMessageW(hwnd, WM_SETICON, ICON_BIG,   (LPARAM)XjsAppIconBig());
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)XjsAppIconSmall());
            /* 任务栏图标 (每窗档案): 关 = WS_EX_TOOLWINDOW, 此刻未显示, 首次显示即按新样式 */
            w->ApplyTaskbarIcon();
            /* 第二实例唤起通道 (见 wWinMain 单实例守卫): 管理员进程的窗口默认拒收低权限进程
               的一切消息, 对 WM_WAKEUP 放行后未提权的重复启动才能唤醒本窗 (UIPI 白名单) */
            ChangeWindowMessageFilterEx(hwnd, WM_WAKEUP, MSGFLT_ALLOW, NULL);
            AddClipboardFormatListener(hwnd);   /* 剪切灰显: 剪贴板被外部替换即恢复 */
            XjsSetPhase(L"wm-create:focus");
            XjsSearchCaretAttach();   /* 搜索框光标闪烁驱动器绑定本窗 (失活由 WM_ACTIVATE 广播) */
            /* 默认无焦点 (2026-09-16 用户口径): 不再启动即聚焦, 聚焦唯一途径=鼠标点击文本区 */
            XjsSetPhase(L"wm-create:dwm");
            BOOL dark = TRUE;
            DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
            /* Win11 圆角+阴影由 DWMWCP_ROUND 提供 (需配合 NCCALCSIZE 保留 1px NC 边距才有抗锯齿);
               Win10 无此属性, 调用失败即默认直角 */
            DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_ROUND;
            DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            XjsSetPhase(L"paint:device");
            if (!g_rt) XjsDeviceCreate();
            XjsSyncViewport(hwnd);
            XjsSetPhase(L"paint:layout");
            XjsChromeLayout();
            if (g_rt) {
                XjsSetPhase(L"paint:draw");
                RECT cr; GetClientRect(hwnd, &cr);
                /* ===== 根治残影: 每帧完整重绘全窗 =====
                   旧方案 = 脏区局部重绘 + HwndRenderTarget 帧间保留, 任何一处失效区域算小,
                   旧像素即残留成重影 (拖列/重命名/悬停拖尾/搜索防抖轮番触发, 修不胜修)。
                   性能由 图标位图缓存 + 行数据缓存 兜底 (逐行重解码 WIC 才是当年被迫局部重绘的原因) */
                RECT d = cr;
                XjsLayout& L = g_layout;
                auto bandHit = [&](float top, float bottom) {
                    return (LONG)top < d.bottom && (LONG)bottom > d.top && d.left >= 0;
                };
                bool backdrop = XjsBackdropBegin(hwnd, cr);   /* 模态钟罩期: 抓弹窗前画面备虚化底 (EndDraw 前盖回) */
                g_rt->BeginDraw();
                g_rt->PushAxisAlignedClip(
                    XjsRectF((float)d.left, (float)d.top, (float)d.right, (float)d.bottom),
                    D2D1_ANTIALIAS_MODE_ALIASED);
                XjsChromeRenderBackground();
                if (bandHit(0, XSF(40))) XjsChromeRenderTitlebar();
                if (bandHit(L.listHead.top, L.list.bottom)) XjsListRender();
                if (L.preview.bottom > L.preview.top && bandHit(L.preview.top, L.preview.bottom)) XjsPreviewRender();
                if (bandHit(L.statusbar.top, L.statusbar.bottom)) XjsRenderStatusbar();
                g_rt->PopAxisAlignedClip();
                if (backdrop) XjsBackdropEndBlur(g_rt, cr);   /* 虚化底整面盖住 + 暗罩 */
                XjsModeDlgRender(g_rt, (float)cr.right, (float)cr.bottom);   /* 模态对话框 (遮罩置顶) */
                XjsToastRender(hwnd, g_rt, g_layout.w, g_layout.h, XSF(1));   /* Toast 浮层最顶 (自带圆角遮罩层, 不参与脏区 band 裁剪) */
                HRESULT hr = g_rt->EndDraw();
                if (hr != S_OK) {
                    XjsBackdropDiscard();   /* 失败帧的离屏内容与目标态不可信, 下帧重建 */
                    s_endDrawFailStreak++;
                } else {
                    s_endDrawFailStreak = 0;
                }
                /* 连续失败 = 设备/目标态已被污染 (曾致弹窗期每帧全丢 = 画面永久冻结,
                   表象"列表点了没反应"), 重建设备自愈; 一次性失败只丢帧 */
                if (hr == (HRESULT)D2DERR_RECREATE_TARGET || s_endDrawFailStreak >= 2) {
                    s_endDrawFailStreak = 0;
                    XjsBackdropDiscard();
                    XjsDeviceDiscard();
                    XjsDeviceCreate();
                    g_needFullPaint = true;
                    w->Invalidate();
                }
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE: {
            if (wParam == SIZE_MINIMIZED) break;
            RECT rc; GetClientRect(hwnd, &rc);
            XjsDeviceResize(ximax(rc.right, 1), ximax(rc.bottom, 1));
            XjsClampScroll();
            w->Invalidate();
            break;
        }
        case WM_DPICHANGED: {
            /* 跨显示器拖动/系统缩放率变化: 本消息是唯一权威 — wParam 的 DPI 直接定尺度,
               系统建议矩形贴过去, 全套 UI 资源按新值重建 (96 = 100%)。
               之后的每帧自愈读 GetDpiForWindow (系统随本消息同步维护), 不会把新尺度改回去 */
            g_s = HIWORD(wParam) / 96.0f;
            XjsRecreateTextFormats();
            XjsUpdateEditFont();
            XjsPopupReleaseResources();
            if (lParam) {
                RECT* r = (RECT*)lParam;
                SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                    SWP_NOZORDER | SWP_NOACTIVATE);
            }
            w->Invalidate();
            return 0;
        }
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
            /* 分辨率/系统级显示广播: 本身不带尺度, 立即重绘一帧 — 若系统已同步更新
               GetDpiForWindow, paint 的视口自愈随即按新值对齐 (静止窗口由此获得适应能力) */
            w->Invalidate();
            return 0;
        case WM_GETMINMAXINFO: {
            DefWindowProcW(hwnd, msg, wParam, lParam);
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            mmi->ptMinTrackSize.x = (LONG)XSF(720);   /* 最小尺寸也是尺寸值, 跟 DPI 缩放 */
            mmi->ptMinTrackSize.y = (LONG)XSF(420);
            /* WS_POPUP 窗口最大化默认铺满整块显示器(盖住任务栏): 显式钳到最近显示器的工作区 */
            MONITORINFO mi = { sizeof(mi) };
            if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
                mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
                mmi->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
                mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
                mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
            }
            return 0;
        }
        /* 自绘边框: 保留 1px 非客户区边距 (完全 return 0 会让 DWM 的圆角遮罩失去抗锯齿=锯齿),
           最大化时内缩系统边框防溢出 */
        case WM_NCCALCSIZE: {
            if (wParam) {
                if (IsZoomed(hwnd)) {
                    /* 最大化: WM_GETMINMAXINFO 已把窗口矩形钳到工作区 (WS_POPUP 无屏外不可见边框),
                       客户区=整窗。若再按"窗口矩形超出屏幕一圈"内缩, 就是双重补偿 —
                       表现为最大化后四周仍保留一圈边框/阴影边距 (用户可见空边) */
                    return 0;
                }
                /* 非最大化保留 1px NC 边距: DWM 圆角遮罩需要它才有抗锯齿 */
                ((RECT*)lParam)->left += 1;
                ((RECT*)lParam)->top += 1;
                ((RECT*)lParam)->right -= 1;
                ((RECT*)lParam)->bottom -= 1;
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        /* NCCALCSIZE 去框后 DefWindowProc 仍会在激活切换时把标准标题栏盖画进客户区顶部
           (此前被高频全窗重绘掩盖, 表现为闪烁)。lParam=-1: 跳过非客户区重绘 */
        case WM_NCACTIVATE:
            return DefWindowProcW(hwnd, msg, wParam, -1);
        case WM_NCPAINT:
            return 0;   /* 去框窗口不重画标准框架 (DWM 圆角/阴影由系统独立绘制) */
        case WM_NCHITTEST: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &pt);
            RECT rc; GetClientRect(hwnd, &rc);
            if (!IsZoomed(hwnd)) {
                const int b = (int)XSF(6);
                bool l = pt.x < b, r = pt.x > rc.right - b, t = pt.y < b, bt = pt.y > rc.bottom - b;
                if (l && t) return HTTOPLEFT;
                if (r && t) return HTTOPRIGHT;
                if (l && bt) return HTBOTTOMLEFT;
                if (r && bt) return HTBOTTOMRIGHT;
                if (l) return HTLEFT;
                if (r) return HTRIGHT;
                if (t) return HTTOP;
                if (bt) return HTBOTTOM;
            }
            if (pt.y < XSF(40)) {
                XjsUpdateHoverState(pt);
                /* 交互元素 → HTCLIENT; 其余标题栏 → 拖动 */
                if (g_hoverWndBtn != WBTN_NONE) return HTCLIENT;
                if (g_hoverBtn & (HB_MENU | HB_PILL | HB_CLEAR | HB_HISTORY | HB_FILTER)) return HTCLIENT;
                /* 搜索框/筛选药丸本体 → HTCLIENT (输入交互); 药丸上下的标题栏空带 (各 5px)
                   → HTCAPTION: 最大化时按住顶带拖动=还原窗口 (此前该列整体 HTCLIENT 拖不动) */
                if (XjsPtIn(g_layout.searchBox, pt) || XjsPtIn(g_layout.filterBtn, pt)) return HTCLIENT;
                return HTCAPTION;
            }
            return HTCLIENT;
        }
        case WM_MOUSEMOVE: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            XjsSyncViewport(hwnd);   /* 高分屏 DPI/RT 失同步兜底 (点A选B的根因之一) */
            if (XjsSearchDragging()) { XjsSearchMouseMove(pt); return 0; }   /* 搜索框拖拽选字 */
            if (XjsSearchDragPending()) { XjsSearchMouseMove(pt); SetCursor(LoadCursorW(NULL, IDC_ARROW)); return 0; }   /* 未聚焦按下待定: 动=拖窗, 光标保持箭头 */
            if (XjsRenameDragging()) { XjsRenameMouseMove(pt); return 0; }   /* 重命名编辑框拖拽选字 */
            if (XjsToastMouseMove(hwnd, XSF(1), g_layout.w, g_layout.h, pt)) { SetCursor(LoadCursorW(NULL, IDC_ARROW)); return 0; }   /* Toast 卡片浮于一切之上 */
            XjsHostedMouseMove(pt);   /* 托管标签悬停计时 (多来源标签 180ms 弹来源切换菜单) */
            if (XjsListMouseMove(pt) || XjsPreviewMouseMove(pt)) return 0;
            XjsUpdateHoverState(pt);
            bool colHover = (pt.y >= g_layout.listHead.top && pt.y < g_layout.listHead.bottom && XjsHitTestColHandle(pt) >= 0);
            bool resizerHover = XjsPreviewResizerHit(pt);
            bool editHit = XjsAnyEditBoxHit(pt);
            SetCursor(LoadCursorW(NULL, editHit ? IDC_IBEAM : (colHover || resizerHover) ? IDC_SIZEWE : IDC_ARROW));
            return 0;
        }
        /* 输入框(搜索框/行内重命名)统一文本光标: WM_SETCURSOR 在鼠标移动时都会到达,
           比逐处 SetCursor 更稳 — 类光标被系统重置也照常覆盖 */
        case WM_SETCURSOR: {
            /* 6px 调整大小边带: NCCALCSIZE 把客户区扩到整窗后, 边带命中码虽是 HT*,
               但 DefWindowProc 对非客户命中只设类光标(箭头) → 大小光标必须自己设
               (调整大小功能走 WM_NCHITTEST 本就正常, 只有光标样式缺失) */
            switch (LOWORD(lParam)) {
                case HTLEFT: case HTRIGHT: SetCursor(LoadCursorW(NULL, IDC_SIZEWE)); return TRUE;
                case HTTOP: case HTBOTTOM: SetCursor(LoadCursorW(NULL, IDC_SIZENS)); return TRUE;
                case HTTOPLEFT: case HTBOTTOMRIGHT: SetCursor(LoadCursorW(NULL, IDC_SIZENWSE)); return TRUE;
                case HTTOPRIGHT: case HTBOTTOMLEFT: SetCursor(LoadCursorW(NULL, IDC_SIZENESW)); return TRUE;
            }
            if (LOWORD(lParam) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                if (XjsAnyEditBoxHit(pt)) { SetCursor(LoadCursorW(NULL, IDC_IBEAM)); return TRUE; }
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_MOUSELEAVE: {
            g_mouseTracking = false;
            XjsListHoverChanged(g_hoverRow, g_listHover);   /* 移出列表: 末次悬停行也走渐隐拖尾 */
            g_listHover = false;
            g_hoverRow = -1;
            g_hoverBtn = 0;
            g_hoverWndBtn = WBTN_NONE;
            g_hoverStatus = -1;
            XjsToastHoverReset(hwnd);   /* Toast 组件悬停态随组件走 */
            XjsHostedHoverReset(hwnd);  /* 托管标签悬停态+来源菜单计时随组件走 */
            w->Invalidate();
            return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            XjsSyncViewport(hwnd);   /* 命中判定前先对齐视口几何 */
            bool renameWas = XjsRenameActive();
            /* 行内重命名编辑态: 点编辑框以外任意处 = 取消重命名 (含标题栏/搜索框,
               原先只有点列表区才收尾, 点搜索框时编辑框残留且继续吃键盘) */
            if (renameWas && !XjsPtIn(XjsRenameEditRect(), pt)) XjsRenameFinish(false);
            /* 点搜索框以外 = 键盘路由交列表, 但搜索框光标不灭 (用户口径: 光标只随
               "其它输入框接管/窗口失活"熄灭); 未聚焦期键盘归列表, 要输入再点搜索框 */
            if (g_searchFocused && !(pt.y < XSF(40) && XjsPtIn(g_layout.searchBox, pt)))
                XjsSearchYieldKeys();
            if (XjsToastMouseDown(hwnd, XSF(1), g_layout.w, g_layout.h, pt)) return 0;   /* Toast 卡片: 复制/关闭 (浮层吃点击) */
            if (pt.y < XSF(40)) {
                if (!XjsChromeMouseDown(pt))
                    return DefWindowProcW(hwnd, msg, wParam, lParam);   /* 标题栏空白 → 系统拖动 */
                return 0;
            }
            if (pt.y >= g_layout.statusbar.top) {
                /* 状态栏右侧按钮: 按下只记待定, 松开仍命中才触发 (命令类松开触发口径)。
                   SetCapture 保证"拖出窗外松开"也收到 mouseup (待定只在此处清零) */
                s_sbToolboxPress = XjsPtIn(g_layout.sbToolbox, pt);
                if (!s_sbToolboxPress) {
                    for (int i = 0; i < g_layout.sbPluginN; i++)
                        if (XjsPtIn(g_layout.sbPlugin[i], pt)) { s_sbPressPlug = i; break; }
                }
                if (s_sbToolboxPress || s_sbPressPlug >= 0) SetCapture(hwnd);
                return 0;
            }
            /* 单击打开待定记录 (仅"鼠标打开=单击"的窗口): 须在 预览/列表 消费前判定,
               命中的是列表内容区里的项目才算候选 */
            s_openPendIdx = -1;
            if (g_mouseOpen == 1 && !renameWas &&
                !(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_SHIFT) & 0x8000) &&
                pt.y >= g_layout.list.top && pt.y < g_layout.list.bottom && pt.x < g_layout.list.right) {
                s_openPendIdx = XjsItemAtPoint(pt);
                if (s_openPendIdx >= 0) s_openPendPt = pt;
            }
            if (XjsPreviewMouseDown(pt)) return 0;
            if (XjsListMouseDown(pt, wParam)) return 0;
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (pt.y < XSF(40) && XjsPtIn(g_layout.searchBox, pt)) { XjsSearchDoubleClick(pt); return 0; }
            /* 重命名编辑框内双击 = 选整词 (不能落到"双击打开文件") */
            if (XjsRenameDoubleClick(pt)) return 0;
            /* 双击列宽手柄 = 自适应列宽 (源样式 03-columns dblclick, 50~800) */
            if (!XjsIsGridView() && pt.y >= g_layout.listHead.top && pt.y < g_layout.listHead.bottom) {
                int h = XjsHitTestColHandle(pt);
                if (h >= 0) {
                    XjsAutoFitColumn(h);
                    g_dragColX = (float)pt.x;   /* 按下已起的调宽拖动归零基点 */
                    return 0;
                }
            }
            if (pt.y >= g_layout.list.top && pt.y < g_layout.list.bottom && pt.x < g_layout.list.right) {
                int idx = XjsItemAtPoint(pt);
                if (idx >= 0) {
                    std::wstring p = XjsItemPath(idx);
                    if (GetKeyState(VK_CONTROL) & 0x8000) XjsOpenFolderAndSelect(p);        /* Ctrl+双击 定位 */
                    else if (GetKeyState(VK_MENU) & 0x8000) XjsShowProperties(idx);         /* Alt+双击 属性 */
                    /* 双击打开: 仅"鼠标打开=双击"的窗口 (单击模式首击已打开, 双击不重复) */
                    else if (g_mouseOpen == 0) XjsOpenFile(p);
                }
            }
            return 0;
        }
        case WM_RBUTTONUP: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            /* 重命名编辑框右键: 编辑菜单 (不能落到列表的"文件"右键菜单) */
            if (XjsRenameActive() && XjsPtIn(XjsRenameEditRect(), pt)) {
                POINT sp = pt;
                ClientToScreen(hwnd, &sp);
                XjsShowRenameBoxMenu(sp);
                return 0;
            }
            /* 搜索框右键: 编辑菜单 (剪切/复制/粘贴/全选/删除) */
            if (pt.y < XSF(40) && XjsPtIn(g_layout.searchBox, pt)) {
                POINT sp = pt;
                ClientToScreen(hwnd, &sp);
                XjsShowSearchBoxMenu(sp);
                return 0;
            }
            /* 表头右键: 列显隐菜单 (源样式 03-columns header contextmenu) */
            if (!XjsIsGridView() && pt.y >= g_layout.listHead.top && pt.y < g_layout.listHead.bottom) {
                POINT sp = pt;
                ClientToScreen(hwnd, &sp);
                XjsShowColumnMenu(sp);
                return 0;
            }
            if (pt.y >= g_layout.list.top && pt.y < g_layout.list.bottom && pt.x < g_layout.list.right) {
                int idx = XjsItemAtPoint(pt);
                if (idx >= 0) {
                    if (!XjsSelIsSelected(idx)) XjsSelectOnly(idx);
                    POINT sp = pt;
                    ClientToScreen(hwnd, &sp);
                    XjsShowContextMenu(sp, idx);
                }
            }
            return 0;
        }
        /* 鼠标侧键 = 搜索历史后退/前进 (源样式 Alt+←/→ 同源) */
        case WM_XBUTTONUP: {
            int btn = HIWORD(wParam);
            XjsHistoryNav(btn == XBUTTON1 ? -1 : 1);
            return TRUE;
        }
        case WM_CLIPBOARDUPDATE:
            /* 剪切后剪贴板被外部替换 → 恢复灰显 (源样式 data-cut 口径); 自己写的忽略 */
            if (!g_cutSet.empty() && (DWORD)GetClipboardSequenceNumber() != g_clipSeqOurs) {
                g_cutSet.clear();
                w->Invalidate();
            }
            return 0;
        case WM_INPUT_DONE: {
            /* 输入对话框收尾 (设置别名): lParam=new std::wstring*(lParam=NULL=取消)
               取消 = 什么都不做 (原先按空串处理, 会把已有别名清空); 确定但内容为空才是"清除别名" */
            std::wstring* s = (std::wstring*)lParam;
            if (s && (int)wParam == IDM_CTX_BASE + 30) {
                XjsApplyAlias(XjsSelIndices(), *s);
            }
            delete s;
            return 0;
        }
        case WM_ACTIVATE: {
            /* 默认无焦点口径: 激活不再自动聚焦搜索框 (聚焦是窗内持久状态, 靠鼠标点击取得);
               广播给所有输入光标驱动器: 窗口失活(切到别的程序) 一律熄光标并停表 —
               否则搜索框/行内重命名的光标会在后台继续闪 */
            bool active = LOWORD(wParam) != WA_INACTIVE;
            XjsCaretBlink::SetWindowActive(hwnd, active);
            /* 每窗"窗口失去焦点: 关闭窗口" (launcher 口径): 经统一消失策略 (主窗藏托盘/子窗销毁)。
               豁免 = 本窗自有模态层/菜单开着 (它们抢前台会让本窗收到 WA_INACTIVE, 不豁免则
               弹个菜单就把窗口关了) + 设置窗正作用在本窗 */
            if (!active && w->blurAction == 1 && !XjsPopupMenuOpen() && !XjsAskDialogOpen() &&
                !XjsModeDlgActive() && !XjsModalOverlayFor(hwnd) && !XjsSettingsOpenFor(hwnd)) {
                static bool s_blurClosing = false;   /* 藏托盘的 SW_HIDE 会同步再发一次 WA_INACTIVE, 防重入 */
                if (!s_blurClosing) {
                    s_blurClosing = true;
                    XjsDismissWindow(hwnd);
                    s_blurClosing = false;
                }
            }
            /* 每窗"激活或创建时位置": 贴光标 或 主屏/鼠标所在屏 五落点 (之前的位置=不动),
               落点算式见 ApplyAppearPos。
               最小化/最大化态不动 (还原与最大化各按系统布局, 不会被本策略拽走) */
            if (active && w->appearPos != 0 && !IsIconic(hwnd) && !IsZoomed(hwnd))
                w->ApplyAppearPos();
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            XjsChromeMouseUp(pt);   /* 标题栏/搜索框命令按钮: 松开触发 (按下待定校验) */
            if (!IsWindow(hwnd)) return 0;   /* ✕ 关闭子窗 = 窗口已销毁, 后续处理不再安全 */
            XjsSearchMouseUp(pt);
            XjsRenameMouseUp();
            XjsListMouseUp(pt);
            XjsPreviewMouseUp(pt);
            XjsToastMouseUp(hwnd, XSF(1), g_layout.w, g_layout.h, pt);   /* Toast 复制/关闭: 松开触发 */
            /* 状态栏按钮落地: 待定 + 松开仍命中同一按钮 */
            if (s_sbToolboxPress && pt.y >= g_layout.statusbar.top) {
                if (XjsPtIn(g_layout.sbToolbox, pt)) XjsShowToolboxMenu();
            }
            if (s_sbPressPlug >= 0 && pt.y >= g_layout.statusbar.top &&
                s_sbPressPlug < g_layout.sbPluginN && XjsPtIn(g_layout.sbPlugin[s_sbPressPlug], pt))
                XjsPluginStatusBarCommand(s_sbPressPlug, XjsPluginCurWindowToken());   /* 松开仍命中 → 插件 OnCommand */
            s_sbPressPlug = -1;
            s_sbToolboxPress = false;
            ReleaseCapture();
            /* 单击打开落地: 待定项目 + 抬起未拖出 (≤5px) + 无修饰键 → 打开 (在捕获释放后,
               XjsOpenFile 可能弹"文件不存在"询问框, 不能带着鼠标捕获进同步泵) */
            int pend = s_openPendIdx;
            s_openPendIdx = -1;
            if (pend >= 0 && !(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_SHIFT) & 0x8000) &&
                abs(pt.x - s_openPendPt.x) + abs(pt.y - s_openPendPt.y) <= 5) {
                int idx = XjsItemAtPoint(pt);
                unsigned long long now = GetTickCount64();
                if (idx >= 0 && (idx != s_lastClickOpenIdx ||
                                 now - s_lastClickOpenTick > (unsigned long long)GetDoubleClickTime())) {
                    s_lastClickOpenIdx = idx;
                    s_lastClickOpenTick = now;
                    XjsOpenFile(XjsItemPath(idx));
                }
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &pt);
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                int dir = delta > 0 ? 1 : -1;
                /* 正式版口径: 预览面板上缩放预览内容, 列表上切换视图模式 (向上滚=放大) */
                if (g_previewVisible && XjsPtIn(g_layout.preview, pt)) XjsPreviewWheel(dir);
                else XjsCycleViewMode(dir);
                return 0;
            }
            if ((GetKeyState(VK_SHIFT) & 0x8000) && pt.y >= g_layout.list.top && pt.y < g_layout.list.bottom) {
                /* Shift+滚轮 = 横向滚动 (列溢出时) */
                g_hScroll -= (double)delta * XSF(3);
                XjsClampHScroll(g_viewMode == VM_LIST);
                w->Invalidate();
                return 0;
            }
            /* 文本预览: 预览面板上普通滚轮滚动内容 */
            if (g_previewVisible && XjsPtIn(g_layout.preview, pt) && XjsPreviewIsText()) {
                XjsPreviewScrollLines(delta > 0 ? -1 : 1);
                return 0;
            }
            float wheelDelta = (float)delta / WHEEL_DELTA * XSF((float)XJS_ROW_H[g_viewMode]);   /* 每格滚 1 行 */
            XjsListWheel(wheelDelta);
            return 0;
        }
        case WM_MOUSEHWHEEL: {
            /* 横向滚轮: 列总宽超出视口时横向滚动 */
            double delta = (double)GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA * XSF(60);
            g_hScroll -= delta;
            XjsClampHScroll(g_viewMode == VM_LIST);
            w->Invalidate();
            return 0;
        }
        case WM_KEYDOWN: {
            if (XjsHandleZoomKey(wParam)) return 0;
            /* Ctrl+N = 创建新搜索窗口 (与 ☰菜单 同入口) */
            if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'N') { XjsSearchWindow::OpenNew(); return 0; }
            /* Alt+←/→ = 搜索历史后退/前进 (源样式全局口径, 鼠标侧键同源) */
            if ((GetKeyState(VK_MENU) & 0x8000) && (wParam == VK_LEFT || wParam == VK_RIGHT)) {
                XjsHistoryNav(wParam == VK_LEFT ? -1 : 1);
                return 0;
            }
            /* 行内重命名编辑态: 全量吞键 (Esc/Enter 在 XjsRenameKey 内收尾) */
            if (XjsRenameActive()) { XjsRenameKey(wParam); return 0; }
            if (g_searchFocused) {
                XjsSearchKey(wParam);
                return 0;   /* 聚焦搜索框时全量吞键 (原 EDIT 子窗口同款), 不漏给列表 */
            }
            if (wParam == VK_F5) { XjsSearchNow(false); return 0; }
            /* 默认无焦点口径: 聚焦只来自点击, 打字/退格不夺焦 —— 但编辑仍进搜索框
               (未聚焦 可打印字符/退格/←→移光标/IME 上屏 照常生效, 光标不亮);
               其余键 (↑↓/回车/Tab/Esc/Ctrl 组合/Home·End·Delete) 未聚焦期全归列表 */
            if (wParam == VK_BACK || wParam == VK_LEFT || wParam == VK_RIGHT) { XjsSearchKey(wParam); return 0; }
            XjsListKey(wParam);
            return 0;
        }
        case WM_CHAR: {
            /* 行内重命名编辑态优先 */
            if (XjsRenameActive()) { XjsRenameChar((wchar_t)wParam); return 0; }
            /* 自绘输入框字符入口 (IME 结果走 WM_IME_CHAR, 不经此防重复); 控制字符 (含 \r \t
               与 Ctrl 组合码) 组件内拒收。未聚焦也接字: 编辑不夺焦, 光标不亮、文本/搜索照常更新 */
            XjsSearchChar((wchar_t)wParam);
            return 0;
        }
        /* IME: 组字过程由系统组字窗显示 (定位见 XjsSearchUpdateImeWindow);
           上屏结果在 WM_IME_COMPOSITION 的 GCS_RESULTSTR 整串取回插入 (自绘框唯一可靠通道),
           WM_IME_CHAR 那条路吞掉只为防双份插入 */
        case WM_IME_CHAR: {
            if (XjsRenameActive()) { XjsRenameChar((wchar_t)wParam); return 0; }
            if (XjsSearchChar((wchar_t)wParam)) return 0;   /* 未聚焦也接字 (编辑不夺焦) */
            return 0;
        }
        case WM_IME_STARTCOMPOSITION: {
            XjsSearchUpdateImeWindow();
            return 0;
        }
        case WM_IME_COMPOSITION: {
            if (XjsRenameActive() && XjsRenameImeResult(hwnd, lParam)) return 0;
            if (XjsSearchImeResult(hwnd, lParam)) return 0;
            XjsSearchUpdateImeWindow();
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_IME_NOTIFY: {
            /* 候选窗打开/换页时再钉一次位置 (部分输入法只认候选窗点位);
               对话框字段聚焦时钉到字段, 不再钉搜索框 (否则候选词列表出现在搜索框位置) */
            if (wParam == IMN_OPENCANDIDATE || wParam == IMN_SETCANDIDATEPOS) {
                if (XjsModeDlgActive()) XjsEditFieldAnchorUpdate(hwnd);
                else XjsSearchUpdateImeWindow();
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_IME_SETCONTEXT: {
            LRESULT r = DefWindowProcW(hwnd, msg, wParam, lParam);
            if (wParam) {
                if (XjsModeDlgActive()) XjsEditFieldAnchorUpdate(hwnd);
                else XjsSearchUpdateImeWindow();
            }
            return r;
        }
        case WM_TIMER: {
            if (wParam == ID_TIMER_STATUS) {
                if (g_engine) {
                    int state = xjs_db_GetEngineState(g_engine);
                    if (state == XJS_DB_STATE_SCANNING) g_isScanning = true;
                    else if (state == XJS_DB_STATE_IDLE) g_isScanning = false;
                    g_fileCount = xjs_db_GetFileCount(g_engine);
                    bool busy = (state != XJS_DB_STATE_IDLE);
                    g_busy = busy;
                    if (busy) SetTimer(hwnd, ID_TIMER_ANIM, 30, NULL);
                    else KillTimer(hwnd, ID_TIMER_ANIM);
                }
            } else if (wParam == ID_TIMER_CARET) {
                XjsCaretBlink::TickWindow(hwnd);   /* 输入光标闪烁 (搜索框/行内重命名 各自的局部失效) */
                return 0;
            } else if (wParam == ID_TIMER_HOVERFADE) {
                if (!XjsListHoverTick()) KillTimer(hwnd, ID_TIMER_HOVERFADE);
                /* 整窗失效: 局部带状重绘会让 防抖前后两种行内容 在保留帧上混排出重影 */
                w->Invalidate();
                return 0;
            } else if (wParam == ID_TIMER_SEARCHSTATUS) {
                /* 一次性: 200ms 仍未完成才显示"正在搜索…" (快速完成不闪状态栏) */
                KillTimer(hwnd, ID_TIMER_SEARCHSTATUS);
                if (g_searching.load()) {
                    g_statusText = XjsT(L"状态栏.正在搜索");
                    RECT sr = { (LONG)g_layout.statusbar.left, (LONG)g_layout.statusbar.top,
                                (LONG)g_layout.statusbar.right, (LONG)g_layout.statusbar.bottom };
                    if (sr.bottom > sr.top) InvalidateRect(hwnd, &sr, FALSE);
                }
                return 0;
            } else if (wParam == ID_TIMER_TOAST) {
                if (!XjsToastTimerTick(hwnd, XSF(1))) KillTimer(hwnd, ID_TIMER_TOAST);
                return 0;
            } else if (wParam == ID_TIMER_SYNCWATCH) {
                XjsSyncWatchTick();   /* 文件同步变化轮询: 数量变了 → 真实时钟节流刷新 */
                return 0;
            } else if (wParam == ID_TIMER_HOSTEDSRC) {
                XjsHostedHoverTimer(hwnd);   /* 多来源标签悬停 180ms 到点: 弹"切换搜索来源"菜单 */
                return 0;
            } else if (wParam == ID_TIMER_ANIM) {
                /* 忙旋灯只转在状态栏: 仅失效状态栏 (初始扫描的进度条在空状态区, 才全窗)。
                   30ms 全窗失效 = 每秒 33 次全窗口重写 → DWM 撕裂 (列表行错位/搜索框闪烁) */
                if (g_isScanning && g_resultCount <= 0) {
                    w->Invalidate();
                } else {
                    RECT sr = { (LONG)g_layout.statusbar.left, (LONG)g_layout.statusbar.top,
                                (LONG)g_layout.statusbar.right, (LONG)g_layout.statusbar.bottom };
                    if (sr.bottom > sr.top) InvalidateRect(hwnd, &sr, FALSE);
                }
            }
            return 0;
        }
        /* ===== 引擎回调消息 =====
           各分支经闸门 XjsPostToUi 入队, 处理完毕 XjsPostToUiDone() 递减 (勿漏, 见 xjs_app.h 闸门节) */
        case WM_SCAN_DRIVE: {
            wchar_t* drive = (wchar_t*)wParam;
            g_scanDrive = drive;
            delete[] drive;
            w->Invalidate();
            XjsPostToUiDone();
            return 0;
        }
        case WM_SCAN_PROGRESS: {
            XjsScanProgressData* d = (XjsScanProgressData*)wParam;
            g_scanDrive = std::wstring(1, d->drive);   /* 盘符无冒号 (源样式同格式) */
            g_scanEnumerated = d->enumerated;
            g_scanTotal = d->total;
            int percent = d->total > 0 ? (int)((double)d->enumerated * 100 / d->total) : 0;
            if (percent > 100) percent = 100;
            /* 源样式 Search.StatusScanning */
            g_statusText = XjsFmt(XjsT(L"状态栏.正在扫描"), g_scanDrive, XjsNumText(percent));
            delete d;
            w->Invalidate();
            XjsPostToUiDone();
            return 0;
        }
        case WM_SCAN_COMPLETE: {
            XjsScanCompleteData* d = (XjsScanCompleteData*)wParam;
            g_fileCount = d->fileCount;
            /* 数据库首次就绪 (首扫路径): 结果对象自此才允许创建 (早建会被 DLL 按"库未加载"定死文件名序) */
            g_dbReady = true;
            XjsEngineEnsureResultAll();
            /* 源样式 Search.StatusIndexDone */
            g_statusText = XjsFmt(XjsT(L"状态栏.索引完成"), XjsWanText(d->fileCount),
                XjsNumText(d->elapsedMs) + L" ms");
            XjsLoadFilters();
            XjsSearchNow(false);
            delete d;
            w->Invalidate();
            XjsPostToUiDone();
            return 0;
        }
        case WM_LOAD_COMPLETE: {
            /* 数据库首次就绪 (加载路径): 结果对象自此才允许创建 (早建会被 DLL 按"库未加载"定死文件名序) */
            g_dbReady = true;
            XjsEngineEnsureResultAll();
            /* 源样式 Search.StatusReady */
            g_statusText = XjsFmt(XjsT(L"状态栏.就绪索引"),
                XjsWanText(g_engine ? xjs_db_GetFileCount(g_engine) : 0));
            XjsLoadFilters();
            XjsSearchNow(false);
            w->Invalidate();
            XjsPostToUiDone();
            return 0;
        }
        case WM_SEARCH_COMPLETE: {
            /* 结果增量变化改由 g_fileChangePending 标记 + ID_TIMER_SYNCWATCH 时钟节流刷新
               (原 wParam==0&&lParam==1 直发路径已收编, 见 Xjs_SearchChange/Xjs_SyncFileChanged) */
            XjsSearchCompleteData* d = (XjsSearchCompleteData*)wParam;
            KillTimer(hwnd, ID_TIMER_SEARCHSTATUS);   /* 快速完成: "正在搜索…"从未显示, 直接切结果文字 */
            XjsUpdateStatusTextOnComplete(d->resultCount);
            g_syncWatchCount = d->resultCount;   /* 轮询基线对齐新结果集 (必须在 delete 前读, 曾是释放后使用) */
            delete d;
            /* 搜索完成回调: 清防抖快照 + 解除正在搜索; 结果集换血后行缓存作废, 下一帧按新结果画 */
            g_searching.store(false);
            g_debounceIds.clear();
            XjsClearRowCache();
            XjsClampScroll();
            /* 默认选中第一个表项 (每窗设置): 新结果集就绪即选中首项, 预览面板随之联动 */
            if (g_defaultSel == 1 && g_resultCount > 0) {
                XjsSelectOnly(0);
                XjsEnsureVisible(0);
            }
            XjsPluginOnSearchComplete();   /* 插件 events 订阅派发 (UI 线程, 订阅掩码过滤) */
            w->Invalidate();
            XjsPostToUiDone();
            return 0;
        }
        case WM_SEARCH_FAILED: {
            std::string* err = (std::string*)wParam;
            KillTimer(hwnd, ID_TIMER_SEARCHSTATUS);
            if ((int)lParam == g_searchFingerprint) {
                /* 错误串解析 (JSON "错误信息" 键 / 原文) 收口在 XjsEngineErrText */
                std::wstring errMsg = XjsEngineErrText(*err);
                /* 源样式错误状态文案按模式区分 (LuaErrorStatus/SqlErrorStatus/RegexErrorStatus) */
                if (g_mode == XMODE_LUA) g_errText = XjsFmt(XjsT(L"错误.Lua前缀"), errMsg);
                else if (g_mode == XMODE_SQL) g_errText = XjsFmt(XjsT(L"错误.SQL前缀"), errMsg);
                else if (g_mode == XMODE_REGEX) g_errText = XjsFmt(XjsT(L"错误.正则前缀"), errMsg);
                else g_errText = XjsFmt(XjsT(L"错误.搜索失败前缀"), errMsg);
                /* 错误搜索回调: 同样解除防抖 (迟到的旧搜索失败不动当前防抖窗口) */
                g_searching.store(false);
                g_debounceIds.clear();
                XjsClearRowCache();
            }
            delete err;
            w->Invalidate();
            XjsPostToUiDone();
            return 0;
        }
        case WM_ICON_READY:
            /* 图标就绪: 图标缓存进程共享, 各窗列表都可能等到这个图标 → 广播失效。
               dome 不做图标位图缓存, 下一帧经 GetFileIco 同步取得 */
            XjsSearchWindow::InvalidateAllLists();
            XjsPostToUiDone();
            return 0;
        case WM_POPUP_RESULT: {
            XjsOnPopupResult((int)wParam);
            return 0;
        }
        case WM_HOTKEY_WARN: {
            /* 启动时主窗全局热键注册失败 (被其它程序占用): 延后到窗已显示才提醒;
               自启动窗藏在托盘不弹 (无人看见), 状态常驻在设置-窗口设置-窗口 行 */
            XjsSearchWindow* m = XjsSearchWindow::Main();
            if (IsWindowVisible(hwnd) && m && m->hotkeyVk) {
                std::wstring tip = XjsFmt(XjsT(L"错误.快捷键注册失败"), XjsHotkeyText(m->hotkeyMod, m->hotkeyVk));
                XjsToastShow(hwnd, tip.c_str(), XTOAST_WARN, XSF(1));
            }
            return 0;
        }
        case WM_DOUBLE_CTRL: {
            /* 双击 Ctrl 唤起 = 唤起下拉选择的目标窗口 (档案名绑定; 名称失效=禁用, 钩子已实时卸载) */
            XjsSummonSlot(XjsDoubleCtrlResolveSlot(), true);
            return 0;
        }
        case WM_HOTKEY: {
            /* 全局快捷键 (每窗): wParam = ID_HOTKEY_SHOW+档案槽 (见 XjsSummonSlot) */
            XjsSummonSlot((int)wParam - ID_HOTKEY_SHOW, true);
            return 0;
        }
        case WM_WAKEUP: {
            /* 第二实例唤起 (wWinMain 单实例守卫投递, UIPI 放行通道见 WM_CREATE):
               第二实例已 AllowSetForegroundWindow 让权, 借前台口径兼容两种来源 —
               SW_RESTORE 兼顾藏托盘与最小化两种隐藏态, 默认无焦点 */
            XjsSummonActivate(hwnd);
            return 0;
        }
        case WM_TRAY_NOTIFY: {
            if (wParam == ID_TRAY_ICON) {
                if (lParam == WM_LBUTTONUP) {
                    XjsSummonActivate(hwnd);   /* 借前台线程恢复 (默认无焦点: 不聚焦, 要输入先点搜索框) */
                } else if (lParam == WM_RBUTTONUP) {
                    /* 托盘菜单 = 同一套自绘弹窗 (XjsShowPopupMenu): 皮肤快照随 owner,
                       项序/图标照源样式 search_tray.html (Tray.Restore/Settings/Exit);
                       结果经 WM_POPUP_RESULT 回本窗, 见 XjsOnPopupResult IDM_TRAY_BASE 分支 */
                    POINT pt; GetCursorPos(&pt);
                    std::vector<XjsPopupItem> items;
                    items.push_back({ IDM_TRAY_BASE + 1, XjsT(L"托盘.恢复窗口"), L"", false, false, false, false, false, XMI_RESTORE });
                    items.push_back({ 0, L"", L"", false, true, false, false });
                    items.push_back({ IDM_TRAY_BASE + 2, XjsT(L"通用词.设置"), L"", false, false, false, false, false, XMI_GEAR });
                    items.push_back({ 0, L"", L"", false, true, false, false });
                    items.push_back({ IDM_TRAY_BASE + 3, XjsT(L"托盘.退出程序"), L"", false, false, false, false, false, XMI_EXIT });
                    XjsShowPopupMenu(hwnd, pt, items, XSF(180));
                }
            }
            return 0;
        }
        case WM_CLOSE:
            /* 统一关闭入口: 末窗=藏托盘, 其余=真关 (主窗先迁移托盘角色);
               外部进程 PostMessage/SendMessage 0x0010 与点关闭按钮同路, 不再有第二套撕毁逻辑 */
            XjsAppCloseWindow(hwnd, XCLOSE_DEFAULT);
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wParam) XjsEngineShutdown(false);
            return 0;
        case WM_DESTROY: {
            /* 钉住本窗上下文: 撕毁过程会同步触发其他窗口的激活/绘制消息 (嵌套 Enter 重绑 Cur),
               之后所有 g_* 宏必须仍解析到本窗 — 否则 double-free (外部 WM_CLOSE 多窗崩溃根因) */
            XjsWindowScope scope(w);
            XjsPluginOnWindowDestroyed(hwnd);   /* 窗口令牌代递增 (插件持旧令牌失效, 防槽位复用串窗) */
            KillTimer(hwnd, ID_TIMER_STATUS);
            KillTimer(hwnd, ID_TIMER_ANIM);
            KillTimer(hwnd, ID_TIMER_CARET);
            KillTimer(hwnd, ID_TIMER_SEARCHSTATUS);
            KillTimer(hwnd, ID_TIMER_TOAST);
            KillTimer(hwnd, ID_TIMER_SYNCWATCH);
            KillTimer(hwnd, ID_TIMER_HOVERFADE);
            /* 组件统一退登记: 输入字段登记 (路由层命中/聚焦表) + 光标闪烁驱动器 + Toast 条目/画刷。
               必须在 XjsDeviceDiscardCtx 之前 (Toast 画刷需随其所属 RT 一起释放) */
            XjsWindowComponentsDetach(hwnd);
            RemoveClipboardFormatListener(hwnd);
            if (w->isMain) {
                /* 主窗销毁 = 退出: 先关其余搜索窗口 (各自同步清理), 再走完整收尾 */
                XjsSearchWindow::DestroyOthers(w);
                XjsHotkeysUnregisterAll();   /* 主窗销毁随窗卸载全部档案槽快捷键 */
                XjsTrayRemove();
                XjsDoubleCtrlStop();   /* 双击 Ctrl 钩子线程退出 (先于引擎/窗口收尾, 停止投递) */
                XjsSaveWindowRect();
                XjsSaveConfig();
                XjsPluginShutdown();   /* 插件逆序通知 (引擎销毁前的最后回调; DLL 不卸载) */
                XjsEngineShutdown(true);
                if (g_result) { xjs_result_Destroy(g_result); g_result = NULL; }
                XjsClearRenderCaches();
                XjsDeviceDiscardCtx(*w);
                XjsReleaseTextFormats();
                if (g_hFontEdit) { DeleteObject(g_hFontEdit); g_hFontEdit = NULL; }
                if (g_engine) { xjs_Destroy(g_engine); g_engine = NULL; }
                PostQuitMessage(0);
            } else {
                /* 新搜索窗口: 只清理自己的每窗资源 (引擎/托盘归主窗, 不动)。
                   关窗即落盘: 最终窗口矩形 ("之前的位置"记忆) 与全部每窗档案随本次保存刷进自己的槽 */
                XjsSaveConfig();
                XjsDeviceDiscardCtx(*w);
                w->DestroyAndFree();
            }
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

/* ==================== 程序入口 ==================== */

/* 每显示器 V2 DPI 感知: 不声明=进程 DPI 虚拟化, 缩放屏上整窗位图拉伸=模糊,
   且 GetDeviceCaps 恒 96 → g_s 恒 1 自绘全部按 100% 布局。
   工程钉 0x0601 (声明被宏挡), 用 GetProcAddress 走: V2(Win10 1703) → PerMonitor(Win8.1) → 系统级 */
static void XjsEnableDpiAwareness() {
    typedef BOOL (WINAPI *PFN_SetCtx)(HANDLE);
    typedef HRESULT (WINAPI *PFN_SetAware)(int);
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        PFN_SetCtx fn = (PFN_SetCtx)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (fn && fn((HANDLE)-4)) return;   /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
    }
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        PFN_SetAware fn = (PFN_SetAware)GetProcAddress(shcore, "SetProcessDpiAwareness");
        if (fn) { fn(2 /*PROCESS_PER_MONITOR_DPI_AWARE*/); FreeLibrary(shcore); return; }
        FreeLibrary(shcore);
    }
    SetProcessDPIAware();
}

/* 档案矩形 → 创建参数: 按存储 DPI 折算到当前 DPI; 无效 (未记忆/尺寸过小) = 假, 调用方走默认 */
static bool XjsProfileRectToCreate(XjsUiProfile* p, UINT dpi, int& x, int& y, int& w, int& h) {
    if (!p || !p->rectValid || p->rw < 400 || p->rh < 300) return false;
    x = p->rx; y = p->ry; w = p->rw; h = p->rh;
    if (p->rdpi > 0 && dpi > 0 && p->rdpi != (int)dpi) {
        x = (int)((double)x * dpi / p->rdpi);
        y = (int)((double)y * dpi / p->rdpi);
        w = (int)((double)w * dpi / p->rdpi);
        h = (int)((double)h * dpi / p->rdpi);
    }
    return true;
}

static void XjsCreateMainWindow(HINSTANCE hInst, int nCmdShow) {
    /* 默认窗口尺寸也按 DPI 缩放 (尺寸值 × DPI/96), 高分屏首启不再偏小 */
    UINT dpi = XjsWindowDpi(NULL);
    float s = dpi / 96.0f;
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    int w = (int)(1250 * s), h = (int)(780 * s);
    /* 主窗矩形 = 槽 0 档案私有 (每窗尺寸不全局统一; 旧顶层共享键已由 LoadConfig 迁入槽 0) */
    XjsProfileRectToCreate(XjsUiProfileAt(0), dpi, x, y, w, h);
    if (x != CW_USEDEFAULT || y != CW_USEDEFAULT) {
        /* 合法性只看 尺寸下限/垃圾坐标下限/MonitorFromPoint (中心点落在任一显示器) —
           SM_CXSCREEN/CYSCREEN 只是主屏尺寸, 副屏上的合法矩形会被误杀 (连尺寸一起重置) */
        if (w < 400 || h < 300 || x < -10000 || y < -10000) {
            x = CW_USEDEFAULT; y = CW_USEDEFAULT; w = (int)(1250 * s); h = (int)(780 * s);
        } else {
            HMONITOR mon = MonitorFromPoint(POINT{ x + w / 2, y + h / 2 }, MONITOR_DEFAULTTONULL);
            if (!mon) { x = CW_USEDEFAULT; y = CW_USEDEFAULT; }
        }
    }
    /* 无边框(有阴影)窗口: 全部自绘。无 WS_CAPTION → 系统在激活切换/托盘交互时无标准标题栏可盖画
       (此前失焦会闪回浅色系统标题栏); WS_THICKFRAME 保留 DWM 阴影+拖边调整+Win+方向贴靠,
       WS_MIN/MAXIMIZEBOX 保留 Snap 与最小化动画, WM_NCCALCSIZE 返回 0 = 客户区=整窗 */
    CreateWindowExW(0, L"SnailQuickSearchWnd", XjsT(L"应用.名称"),
        WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
        x, y, w, h, NULL, NULL, hInst, NULL);
    if (g_hWnd) { ShowWindow(g_hWnd, nCmdShow); XjsSetPhase(L"show-window"); }
}

/* ☰菜单/Ctrl+N: 新建搜索窗口 — 同进程共享引擎/索引, 独立 xjs_result (各窗搜索互不干扰) */
void XjsSearchWindow::OpenNew(int profileSlot) {
    if (g_isScanning) {   /* 遍历中不允许创建新窗口 */
        XjsToastShow(g_hWnd, XjsT(L"提示.扫描中禁止新建窗口"), XTOAST_WARN, XSF(1));
        return;
    }
    if (XjsSearchWindow::Count() >= 16) { XjsToastShow(g_hWnd, XjsT(L"提示.窗口数上限"), XTOAST_WARN, XSF(1)); return; }
    HINSTANCE hInst = GetModuleHandleW(NULL);
    /* 级联: 依主窗位置向右下错开 (每窗 +28), 出工作区则折回主窗位 */
    RECT mr = { 0, 0, 0, 0 };
    if (XjsSearchWindow::MainHwnd()) GetWindowRect(XjsSearchWindow::MainHwnd(), &mr);
    int off = XjsSearchWindow::Count() * 28;
    UINT dpi = XjsWindowDpi(NULL);
    float s = dpi / 96.0f;
    int w = (int)(1250 * s), h = (int)(780 * s);
    int x = (mr.right > mr.left) ? mr.left + off : CW_USEDEFAULT;
    int y = (mr.right > mr.left) ? mr.top + off : CW_USEDEFAULT;
    /* 尺寸每窗私有 (2026-09-17 用户口径: 不全局统一): ① 按档案槽重建优先用该档案记忆的
       矩形 ("之前的位置", 按存储 DPI 折算); ② 无记忆的新窗继承发起窗 (Cur) 当前尺寸;
       发起窗最大化/最小化取不到正常尺寸时回全局默认 1250×780 */
    bool rectFromProfile = (profileSlot >= 0) &&
                           XjsProfileRectToCreate(XjsUiProfileAt(profileSlot), dpi, x, y, w, h);
    if (!rectFromProfile) {
        XjsSearchWindow* src = XjsSearchWindow::Cur();
        RECT sr;
        if (src && src->hWnd && !IsZoomed(src->hWnd) && !IsIconic(src->hWnd) && GetWindowRect(src->hWnd, &sr)) {
            w = ximax(sr.right - sr.left, (int)(400 * s));
            h = ximax(sr.bottom - sr.top, (int)(300 * s));
        }
    }
    HMONITOR mon = MonitorFromPoint(POINT{ x + w / 2, y + h / 2 }, MONITOR_DEFAULTTONULL);
    if (mon) {
        MONITORINFO minfo = { sizeof(minfo) };
        GetMonitorInfoW(mon, &minfo);
        if (x + w > minfo.rcWork.right - 8 || y + h > minfo.rcWork.bottom - 8) {
            if (profileSlot < 0) { x = mr.left; y = mr.top; }   // 折回主窗位 (级联新建); 档案矩形保留原位
        }
    } else if (profileSlot >= 0) {
        /* 档案记忆的矩形不在任何显示器上 (外接屏已断开等): 回落级联位 */
        x = (mr.right > mr.left) ? mr.left + off : CW_USEDEFAULT;
        y = (mr.right > mr.left) ? mr.top + off : CW_USEDEFAULT;
    }
    /* profileSlot=-1 = 新建空白档案 (GUID 名, 跟随主窗设置); >=0 = 绑定既有档案槽
       (☰菜单窗口启动器: 未打开的命名窗口按其档案重建) */
    /* 先清创建中途失败的裸上下文 (hWnd=NULL): 不清会占着档案槽, 菜单勾选态/激活判定全被带歪 */
    for (int i = XjsSearchWindow::Count() - 1; i >= 1; i--) {
        XjsSearchWindow* stale = XjsSearchWindow::At(i);
        if (stale && !stale->hWnd) stale->DestroyAndFree();
    }
    XjsSearchWindow::RegisterPending(false, profileSlot >= 0 ? profileSlot : XjsUiProfileCount());
    CreateWindowExW(0, L"SnailQuickSearchWnd", XjsT(L"应用.名称"),
        WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
        x, y, w, h, NULL, NULL, hInst, NULL);
    if (g_hWnd) {
        XjsForegroundBorrow borrow;   /* 双击 Ctrl/热键触发创建时本进程不在前台: 借前台线程完成激活
                                         (菜单/Ctrl+N 创建时前台本就是本进程, 借用自动零开销) */
        ShowWindow(g_hWnd, SW_SHOW);
        SetForegroundWindow(g_hWnd);
    }
}

/* 找已有实例的搜索窗: 中完整性进程 FindWindowW 按类名找不到提权实例的窗口 (UIPI,
   2026-09-16 实测; 重编译前优雅落盘同款坑, 同改 EnumWindows), 窗口枚举不受限 —
   逐窗比对类名取第一个命中 (同完整性下行为与 FindWindowW 一致) */
static HWND XjsFindInstanceWindow(const wchar_t* cls) {
    struct Ctx { const wchar_t* cls; HWND found; } ctx = { cls, NULL };
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        Ctx* c = (Ctx*)l;
        wchar_t hcls[64];
        if (GetClassNameW(h, hcls, 64) && wcscmp(hcls, c->cls) == 0) { c->found = h; return FALSE; }
        return TRUE;
    }, (LPARAM)&ctx);
    return ctx.found;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPWSTR lpCmdLine, int nCmdShow) {
    AddVectoredExceptionHandler(1, XjsVecStackLogger);   /* 排最前: 抢在任何处理器之前拿到崩溃栈 */
    /* 暂禁用 xjs_EnableException (2026-09-15 白屏闪退根因: DLL 的 VEH 处理器在异常分发时
       做哈希表/写文件/弹框, 与驱动线程并发破坏堆)。DLL 侧 87cf4a4+2f884be 已重构异常流程,
       待验证稳定后恢复: xjs_EnableException(TRUE, NULL); */

    /* 单实例守卫: 已有实例(含托盘隐藏中)时唤起它并退出 —— 双进程并发初始化引擎、
       抢 xjs_db.dat、重复托盘图标与双击 Ctrl 钩子 = 启动期堆破坏, 白屏闪退 (初始化冲突)
       宽限: 自提权重启时新进程可能在旧进程 ExitProcess 完成前抢跑 (UAC"从不通知"
       时静默提权毫秒级), 此刻互斥量尚被旧进程持有而主窗无影。先找窗口, 找不到时
       每隔 100ms 重试至多 3 秒; 期间拿到互斥量 = 旧实例已死 (WAIT_OBJECT_0 正常
       释放 / WAIT_ABANDONED 异常遗弃, 两种等待成功都取得所有权), 接管继续启动;
       超时仍无窗口且拿不到才放弃退出。互斥量在但窗口找不到的正常场景不存在:
       实例存活必然有窗口 (最后窗口关闭=藏托盘不销毁), 故重试只会遇到"退出中" */
    HANDLE mu = CreateMutexW(NULL, TRUE, L"SnailQuickSearch_D2D_SingleInstance");
    if (!mu || GetLastError() == ERROR_ALREADY_EXISTS) {
        bool takenOver = false;
        HWND prev = XjsFindInstanceWindow(L"SnailQuickSearchWnd");
        for (int i = 0; mu && !prev && !takenOver && i < 30; i++) {
            takenOver = WaitForSingleObject(mu, 100) != WAIT_TIMEOUT;
            if (!takenOver) prev = XjsFindInstanceWindow(L"SnailQuickSearchWnd");
        }
        if (prev) {
            /* 唤起已有实例: 本实例尚未提权 (守卫先于 XjsAppMain 的提权检查), 权限隔离 (UIPI)
               下跨完整性级别调 ShowWindow/SetForegroundWindow 对管理员进程的窗口无效 —
               走消息通道: 先把前台权让出 (用户刚启动的新进程持有前台权), 再投递 WM_WAKEUP
               (首实例 WM_CREATE 已对该消息 MSGFLT_ALLOW 放行, 跨 IL 也能送达),
               恢复动作在首实例自己的特权上下文里执行 (与托盘唤起同口径) */
            AllowSetForegroundWindow(ASFW_ANY);
            PostMessageW(prev, WM_WAKEUP, 0, 0);
            if (mu) CloseHandle(mu);
            return 0;
        }
        if (!mu || !takenOver) {
            if (mu) CloseHandle(mu);
            return 0;   /* CreateMutex 失败或 3 秒宽限仍冲突: 放弃, 宁可不跑不双开 */
        }
        /* takenOver: 互斥量已到手, 旧实例已退出, 接管继续正常启动 */
    }
    return XjsAppMain(hInstance, hPrev, lpCmdLine, nCmdShow);
}

static int XjsAppMain(HINSTANCE hInstance, HINSTANCE hPrev, LPWSTR lpCmdLine, int nCmdShow) {
    XjsSetPhase(L"elevate-check");
    /* XJS_NOELEVATE=1 旁路自提权 (调试: 非管理员调试器可直接附加) */
    if (!XjsIsRunningAsAdmin() && !GetEnvironmentVariableW(L"XJS_NOELEVATE", NULL, 0)) { XjsElevateAndRestart(); return 0; }
    XjsSetPhase(L"dpi-awareness");
    XjsEnableDpiAwareness();   /* 必须先于任何窗口创建 */
    XjsSetPhase(L"load-config");
    XjsSearchWindow::RegisterPending(true, 0);   /* 主窗恒档案槽 0; 配置加载随后重建档案数组 (否则空指针) */
    XjsLoadConfig();
    XjsSetPhase(L"i18n-init");
    XjsI18nInit();   /* 语言表装载 (配置 "语言" 键已读; 之后所有 UI 文案经 XjsT) */
    XjsSetPhase(L"com-init");
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    /* OLE 初始化 (非裸 CoInitializeEx): DoDragDrop 要求线程经 OleInitialize 注册 OLE 拖放机制,
       否则返回 CO_E_NOTINITIALIZED (0x800401F0) — "按住选中行拖不动文件"的根因;
       OleInitialize 内含 COM STA 初始化, D2D/WIC 照常可用 */
    OleInitialize(NULL);
    /* 绘图后端选边 (设置-通用 "绘制引擎", 配置已加载, 重启生效): D2D / GDI+ */
    XjsSetPhase(L"gfx-init");
    bool gfxOk = (g_gfxEngine == 1) ? XjsGdiplusInit() : XjsD2DInit();
    if (g_gfxEngine == 1 && !gfxOk) {   /* GDI+ 失败回落 D2D */
        g_gfxEngine = 0;
        gfxOk = XjsD2DInit();
    }
    if (!gfxOk) {
        MessageBoxW(NULL, XjsT(L"错误.绘图引擎初始化失败"), XjsT(L"通用词.错误"), MB_OK | MB_ICONERROR);
        return 0;
    }
    XjsSetPhase(L"register-class");
    XjsRegisterPopupClass(hInstance);
    XjsRegisterSettingsClass(hInstance);
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;   /* 无 CS_DBLCLKS 系统不发 WM_LBUTTONDBLCLK, 双击打开失效 */
    wc.lpfnWndProc = Xjs_WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = XjsAppIconBig();        /* 窗口类图标: "SnailQuickSearchWnd" 全部搜索窗共用 (任务栏按钮/Alt+Tab) */
    wc.hIconSm = XjsAppIconSmall();    /* 小图标: 任务栏悬停预览标题行 / 缩略图列表取它, 缺 = 空白 (见 xjs_util.cpp) */
    wc.lpszClassName = L"SnailQuickSearchWnd";
    if (!RegisterClassExW(&wc)) return 0;
    XjsSetPhase(L"create-window");
    /* 计划任务自启动 (--autostart): 以隐藏方式创建 (仅托盘, 经托盘/双击 Ctrl 唤起) */
    bool autoStart = lpCmdLine && wcsstr(lpCmdLine, L"--autostart") != NULL;
    XjsCreateMainWindow(hInstance, autoStart ? SW_HIDE : nCmdShow);
    XjsDoubleCtrlApply();   /* 双击 Ctrl 目标载入后按有效性实时装卸钩子 (禁用/名称失效 = 零钩子) */
    /* 插件系统: 扫描 plugins\ + 按配置加载已启用 (主窗已建 — 插件回调/Toast 有宿主窗可用;
       引擎此刻尚未提交加载, 插件侧查询由引擎状态门槛自理, 见 SDK 头"直连搜索引擎") */
    XjsPluginStartup();
    /* 数据库异步加载: 窗口首次创建并显示完成之后才提交 (不与窗口创建过程交叠) */
    XjsSetPhase(L"post-create:load-submit");
    std::wstring dbPath = XjsGetExeDir() + L"\\xjs_db.dat";
    DeleteFileW((dbPath + L".tmp").c_str());   /* 清理上次保存中途被杀残留的半成品临时文件 (保存=先写 .tmp 再改名, 见 XjsEngineShutdown) */
    std::string dbUtf8 = Utf16ToUtf8(dbPath.c_str());
    int loadState = xjs_db_Load(g_engine, dbUtf8.c_str(), TRUE);
    { static wchar_t pb[64]; swprintf(pb, 64, L"post-create:load-ret=%d", loadState); XjsSetPhase(pb); }   /* static: g_phase 要活到崩溃时刻, 局部数组出块即悬垂 */
    if (loadState != XJS_LOAD_OK) {
        /* 默认字段与正式版一致: 文件大小/修改时间/文件评分/别名 */
        XjsSetPhase(L"post-create:add-fields");
        xjs_db_AddField(g_engine, "文件大小", NULL);
        xjs_db_AddField(g_engine, "修改时间", NULL);
        xjs_db_AddField(g_engine, "文件评分", NULL);
        xjs_db_AddField(g_engine, "别名", NULL);
        XjsSetPhase(L"post-create:scan-start");
        xjs_db_ScanPath(g_engine, NULL, TRUE);
        g_statusText = XjsT(L"状态栏.正在建立索引");   /* 源样式 Search.ScanningDesc */
    } else {
        g_statusText = XjsT(L"状态栏.正在加载数据库");   /* 源样式 Search.StatusLoadingDb (省略号字符) */
    }
    XjsSearchWindow::Cur()->Invalidate();
    XjsSetPhase(L"message-loop");
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    XjsSetPhase(L"shutdown");
    if (g_gfxEngine == 1) XjsGdiplusShutdown();
    else XjsD2DShutdown();
    OleUninitialize();   /* 配对启动时的 OleInitialize */
    return (int)msg.wParam;
}
