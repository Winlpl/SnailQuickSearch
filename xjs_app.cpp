/*
 * xjs_app.cpp — 全局状态定义 (唯一实例) + 每窗上下文注册表
 */
#include "xjs_app.h"

/* 列布局内置默认表 (每个 XjsSearchWindow 构造时拷贝; details 8 列 / list 9 列, 同源样式 COLS_ALL)。
   draw = 列内容绘制器 (数据驱动行渲染, 见 xjs_list.cpp)。
   创建/访问时间/文件属性 依赖数据库字段 (重建索引时勾选), 默认隐藏 —
   表头右键菜单里可开启 (字段未开启时置灰, 同 评分/大小/修改时间 的既有口径) */
static const XjsColSpec s_colsDetailsDef[8] = {
    { L"列.名称", 100, false, "文件名", true, 1.0f, true, XjsColDrawName },
    { L"列.别名", 140, false, "别名", false, 0, true, XjsColDrawAlias },
    { L"列.评分", 60, false, "文件评分", false, 0, true, XjsColDrawRating },
    { L"列.大小", 110, false, "文件大小", false, 0, true, XjsColDrawSize },
    { L"列.修改时间", 150, false, "修改时间", false, 0, true, XjsColDrawTime },
    { L"列.创建时间", 150, false, "创建时间", false, 0, false, XjsColDrawCtime },
    { L"列.访问时间", 150, false, "访问时间", false, 0, false, XjsColDrawAtime },
    { L"列.文件属性", 110, false, "文件属性", false, 0, false, XjsColDrawAttrs } };
static const XjsColSpec s_colsListDef[9] = {
    { L"名称", 120, false, "文件名", true, 1.2f, true, XjsColDrawName },
    { L"列.别名", 140, false, "别名", false, 0, true, XjsColDrawAlias },
    { L"列.文件夹", 130, false, "文件夹", true, 1.3f, true, XjsColDrawFolder },
    { L"列.评分", 60, false, "文件评分", false, 0, true, XjsColDrawRating },
    { L"列.大小", 100, false, "文件大小", false, 0, true, XjsColDrawSize },
    { L"列.修改时间", 140, false, "修改时间", false, 0, true, XjsColDrawTime },
    { L"列.创建时间", 140, false, "创建时间", false, 0, false, XjsColDrawCtime },
    { L"列.访问时间", 140, false, "访问时间", false, 0, false, XjsColDrawAtime },
    { L"列.文件属性", 100, false, "文件属性", false, 0, false, XjsColDrawAttrs } };

/* ==================== 每窗界面设置档案 (uiWindows 数组) ==================== */
static std::vector<XjsUiProfile> s_uiProfiles;
int XjsUiProfileCount() { return (int)s_uiProfiles.size(); }
XjsUiProfile* XjsUiProfileAt(int i) {
    if (i < 0 || i >= (int)s_uiProfiles.size()) return NULL;
    return &s_uiProfiles[i];
}
int XjsUiProfilesAppend(const wchar_t* name) {
    XjsUiProfile p;
    p.name = name ? name : L"";
    s_uiProfiles.push_back(p);
    return (int)s_uiProfiles.size() - 1;
}
void XjsUiProfilesReset() { s_uiProfiles.clear(); }
void XjsUiProfilesPush(const XjsUiProfile& p) { s_uiProfiles.push_back(p); }

/* ==================== 每窗上下文注册表 (多搜索窗口) ====================
 * 固定容量指针表 (引擎回调线程会遍历, 避免 vector 扩容竞态);
 * "当前窗"由主窗 WndProc 入口 XjsWinEnter 绑定, 引擎线程禁用 XjsWinCur()。 */
static XjsSearchWindow* s_wins[16] = {};
static int s_winCount = 0;
static XjsSearchWindow* s_curWin = NULL;
static XjsSearchWindow* s_pendingWin = NULL;   /* CreateWindowExW 期间待绑定的上下文 */
static HWND s_hWndMain = NULL;

XjsSearchWindow::XjsSearchWindow() {
    colsDetails.Init(s_colsDetailsDef, 8);
    colsList.Init(s_colsListDef, 9);
}

XjsSearchWindow* XjsSearchWindow::Cur() { return s_curWin; }

/* 新窗口默认名称 = GUID (CoCreateGuid, 标准连字符格式 36 字符): 用户可随时重命名 */
std::wstring XjsGenerateWindowName() {
    GUID g;
    CoCreateGuid(&g);
    wchar_t buf[40];
    swprintf(buf, 40, L"%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             (unsigned)g.Data1, g.Data2, g.Data3,
             g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
             g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}
void XjsSearchWindow::SetCur(XjsSearchWindow* w) { s_curWin = w; }
HWND XjsSearchWindow::MainHwnd() { return s_hWndMain; }

XjsSearchWindow* XjsSearchWindow::Main() {
    for (int i = 0; i < s_winCount; i++)
        if (s_wins[i]->isMain) return s_wins[i];
    return NULL;
}

/* 窗口名称唯一性校验 (重命名用): 除 except 外不得有同名窗口; 主窗保留名也视为占用 */
bool XjsSearchWindow::NameInUse(const wchar_t* name, const XjsSearchWindow* except) {
    if (!name || !name[0]) return false;
    if (wcscmp(name, XJS_MAIN_WIN_NAME) == 0) return true;
    for (int i = 0; i < s_winCount; i++)
        if (s_wins[i] != except && s_wins[i]->name == name) return true;
    return false;
}

XjsSearchWindow* XjsSearchWindow::At(int index) {
    if (index < 0 || index >= s_winCount) return NULL;
    return s_wins[index];
}

int XjsSearchWindow::Count() { return s_winCount; }

bool XjsSearchWindow::Alive(const XjsSearchWindow* w) {
    for (int i = 0; i < s_winCount; i++)
        if (s_wins[i] == w) return true;
    return false;
}

XjsSearchWindow* XjsSearchWindow::OfResult(xjs_result* result) {
    if (!result) return NULL;
    /* 唯一权威 = 结果对象创建时绑定的 UserValue (XjsEngineEnsureResultFor 是唯一创建点)。
       不再回落遍历窗口表: 本函数在引擎回调线程调用, 扫 s_wins 会与 UI 线程竞态。 */
    return (XjsSearchWindow*)xjs_result_GetUserValue(result);
}

void XjsSearchWindow::RegisterPending(bool main, int profileSlot) {
    if (s_winCount >= (int)(sizeof(s_wins) / sizeof(s_wins[0]))) return;   // 表满: 拒绝再开 (调用方 g_hWnd 为空即静默放弃)
    XjsSearchWindow* w = new XjsSearchWindow();
    w->isMain = main;
    w->uiIndex = main ? 0 : profileSlot;   /* 档案槽持久绑定: 关窗档案保留, ☰菜单可按槽位重建窗口。
                                              (曾用只增计数器/存活序两版, 都有编号漂移问题, 2026-09-17 定案) */
    w->name = main ? XJS_MAIN_WIN_NAME : XjsGenerateWindowName();   /* 主窗固定名, 其余默认 GUID */
    while (XjsUiProfileCount() <= w->uiIndex)
        XjsUiProfilesAppend(L"");   /* 槽不足补默认档案 (主窗槽 0 随后由 LoadConfig 重建) */
    s_wins[s_winCount++] = w;
    s_pendingWin = w;
    if (main) {
        s_hWndMain = NULL;   /* WM_CREATE 绑定 hwnd 后回填 */
        s_curWin = w;        /* 主窗上下文先行: 配置加载等"窗口创建前"代码经宏写每窗状态 (load-config 曾空指针崩溃) */
    }
}

XjsSearchWindow* XjsSearchWindow::OfHwnd(HWND hwnd) {
    for (int i = 0; i < s_winCount; i++)
        if (s_wins[i]->hWnd == hwnd) return s_wins[i];
    return NULL;
}

void XjsSearchWindow::Enter(HWND hwnd) {
    for (int i = 0; i < s_winCount; i++) {
        if (s_wins[i]->hWnd == hwnd) {
            SetCur(s_wins[i]);
            s_wins[i]->SyncSkin();   /* 窗口间皮肤不同: 全局配色镜像跟随当前窗 (弹窗/菜单取色一致) */
            XjsSyncTextFormats();    /* 窗口间 缩放/DPI 不同: 共享文本格式按当前窗倍率重建 (失配才动) */
            return;
        }
        /* WM_CREATE: 窗口尚未入表 → 绑定待建上下文 */
        if (s_pendingWin && s_pendingWin->hWnd == NULL && s_wins[i] == s_pendingWin) {
            s_pendingWin->hWnd = hwnd;
            if (s_pendingWin->isMain) s_hWndMain = hwnd;
            SetCur(s_pendingWin);
            s_pendingWin->SyncSkin();
            XjsSyncTextFormats();   /* 新窗缩放/DPI 与现有共享格式失配时先对齐 (首建由 DeviceCreate 负责) */
            s_pendingWin = NULL;
            return;
        }
    }
    SetCur(NULL);   /* 窗口创建早期消息 (WM_GETMINMAXINFO 等): 无上下文, WndProc 须回 DefWindowProc */
}

void XjsSearchWindow::SyncSkin() {
    if (skinName != g_skinName) {
        g_skinName = skinName;
        XjsSkinLoad(g_skinName.c_str());
        g_skinEpoch++;
    }
}

void XjsSearchWindow::Invalidate() {
    if (hWnd) InvalidateRect(hWnd, NULL, FALSE);
}

/* 绑定到指定档案槽的存活窗口 (无=NULL): ☰菜单"已打开→激活/未打开→创建"判定用。
   只认已绑定 hwnd 的上下文 —— 创建中途失败的裸上下文 (hWnd=NULL) 不算"窗口已打开" */
XjsSearchWindow* XjsSearchWindow::AtSlot(int slot) {
    for (int i = 0; i < s_winCount; i++)
        if (s_wins[i]->uiIndex == slot && s_wins[i]->hWnd) return s_wins[i];
    return NULL;
}

/* 删除一个档案槽 (窗口管理): 其后槽位前移, 存活窗口的槽位绑定随之修正 (热键按新槽位重注册) */
void XjsUiProfilesRemove(int slot) {
    if (slot < 0 || slot >= (int)s_uiProfiles.size()) return;
    s_uiProfiles.erase(s_uiProfiles.begin() + slot);
    for (int i = 0; i < s_winCount; i++)
        if (s_wins[i]->uiIndex > slot) s_wins[i]->uiIndex--;
}

void XjsSearchWindow::InvalidateList() {
    if (!hWnd) return;
    RECT lr = { (LONG)layout.list.left, (LONG)layout.list.top,
                (LONG)layout.list.right, (LONG)layout.list.bottom + 1 };
    if (lr.bottom > lr.top + 1) InvalidateRect(hWnd, &lr, FALSE);
    else InvalidateRect(hWnd, NULL, FALSE);
}

void XjsSearchWindow::ApplyUiProfile() {
    XjsUiProfile* p = XjsUiProfileAt(uiIndex);
    XjsSearchWindow* m = Main();
    /* 档案必须带有效列集才可用: RegisterPending 给新窗补槽的空白档案列集合为空 (n=0, 其余字段是
       结构体默认值), 照拷进去 = 0 可见列 (行内容压成左缘细条、表头消失, 2026-09-17 实锤)。
       空列档案视同无档案 → 跟随主窗 (OpenNew"新建空白档案跟随主窗设置"的本意);
       存活窗口落盘时会把跟随来的列写回自己的槽, 坏档案随之自愈 */
    if (p && p->colsDetails.Count() > 0 && p->colsList.Count() > 0) {
        viewMode = p->viewMode;
        mode = p->mode;   /* 搜索模式 (每窗, 2026-09-18 每窗化: 曾顶层共享 + 新窗强制跟随主窗) */
        previewVisible = p->previewVisible;
        previewWidth = p->previewWidth;
        colsDetails = p->colsDetails;
        colsList = p->colsList;
        skinName = p->skin;
        openElevated = p->openElevated;
        openAsync = p->openAsync;
        openHideWindow = p->openHideWindow;
        /* 热键: 按档案重建的窗口必须带走档案热键, 否则窗口字段 0/0 会在后续 RegisterAll
           时把已注册的档案热键卸掉 (注册事实源 = 窗口字段) */
        hotkeyMod = p->hotkeyMod;
        hotkeyVk = p->hotkeyVk;
        match = p->match;
        history = p->history;   /* 搜索历史 (每窗) */
        driveProgress = p->driveProgress;
        rowHover = p->rowHover;
        rowHoverFade = p->rowHoverFade;
        /* 窗口行为 (每窗): 失焦动作/激活位置/标题栏与状态栏可见性/任务栏图标/鼠标打开/页面缩放/置顶 */
        blurAction = p->blurAction;
        appearPos = p->appearPos;
        showCtrlBtns = p->showCtrlBtns;
        showFilterBox = p->showFilterBox;
        showStatusbar = p->showStatusbar;
        taskbarIcon = p->taskbarIcon;
        mouseOpen = p->mouseOpen;
        uiZoom = p->uiZoom;
        lang = p->lang;   /* 界面语言 (每窗) */
        defaultSel = p->defaultSel;
        createFill = p->createFill;
        createKeyword = p->createKeyword;
        topmost = p->topmost;
        /* 窗口名称: 主窗恒固定名 (不可重命名); 其余取档案名, 档案无名则保留创建时的 GUID */
        if (isMain) name = XJS_MAIN_WIN_NAME;
        else if (!p->name.empty()) name = p->name;
    } else if (m && m != this) {
        /* 无本窗档案 → 跟随主窗当前界面设置 */
        viewMode = m->viewMode;
        previewVisible = m->previewVisible;
        previewWidth = m->previewWidth;
        colsDetails = m->colsDetails;
        colsList = m->colsList;
        skinName = m->skinName;
        openElevated = m->openElevated;
        openAsync = m->openAsync;
        openHideWindow = m->openHideWindow;
        match = m->match;
        mode = m->mode;    /* 搜索模式 (每窗): 无档案新窗跟随主窗, 之后各自独立 */
        history.clear();   /* 跟随主窗不含历史: 新窗搜索历史从空开始 */
        driveProgress = m->driveProgress;
        rowHover = m->rowHover;
        rowHoverFade = m->rowHoverFade;
        /* 跟随主窗不含热键: 新窗默认未设置 (拷主窗热键 = 同组合双注册必然一败), 字段保持 0/0 */
        hotkeyMod = 0;
        hotkeyVk = 0;
        /* 窗口行为跟随主窗; 置顶不跟随 (新窗不因主窗图钉而压顶) */
        blurAction = m->blurAction;
        appearPos = m->appearPos;
        showCtrlBtns = m->showCtrlBtns;
        showFilterBox = m->showFilterBox;
        showStatusbar = m->showStatusbar;
        taskbarIcon = m->taskbarIcon;
        mouseOpen = m->mouseOpen;
        uiZoom = m->uiZoom;
        lang = m->lang;   /* 界面语言: 无档案新窗跟随主窗, 之后各自独立 */
        defaultSel = m->defaultSel;
        createFill = m->createFill;
        createKeyword = m->createKeyword;
        topmost = false;
        /* 名称不跟随主窗: 每窗身份唯一 (主窗固定名不外借, 新窗保留创建时 GUID) */
    }
    SyncSkin();
    /* 热键生效态回填: hotkeyActive 只在录制/清除时和启动期 RegisterAll 更新, 关窗期间经档案
       兜底注册的热键在开窗后镜像仍为 false —— 设置页会误显"（未生效）"。RegisterAll 逐槽
       Unregister+Register 幂等, 顺带按真实注册结果刷新全部存活窗口的 hotkeyActive */
    XjsHotkeysRegisterAll();
}

/* 主窗"真关闭"(非最后一个窗口)时: 托盘/热键/isMain 迁往最早创建的存留窗口 —
 * 窗口彼此对等, 应用角色跟着最老窗口走; 关闭窗自身降级为普通窗口走普通清理 */
void XjsSearchWindow::MigrateMainRole() {
    if (!isMain) return;
    XjsSearchWindow* succ = NULL;
    for (int i = 0; i < s_winCount; i++) {
        if (s_wins[i] != this && s_wins[i]->hWnd) { succ = s_wins[i]; break; }
    }
    if (!succ) return;   // 没有别的窗口 → 保持主窗身份 (走藏托盘)
    isMain = false;
    succ->isMain = true;
    s_hWndMain = succ->hWnd;
    XjsTrayRemove();
    XjsTrayAdd(succ->hWnd);
    XjsHotkeysRegisterAll();   /* 快捷键每窗: 全部槽重登记到新主窗 (旧主窗销毁时随窗卸载) */
}

/* 关闭请求 (WM_CLOSE): 最后一个窗口 = 藏托盘 (返回 true, 源样式 closeAction=hide);
   非最后窗口 = 真关闭 (返回 false, 调用方 DestroyWindow; 主窗先迁移托盘角色) */
bool XjsSearchWindow::CloseRequest() {
    if (s_winCount <= 1) {
        if (hWnd) ShowWindow(hWnd, SW_HIDE);
        return true;
    }
    MigrateMainRole();
    return false;
}

/* RT 绑定资源统一释放 (设备丢弃 / 窗口销毁共用) */
XjsSearchWindow::~XjsSearchWindow() {
    if (hFontEdit) { DeleteObject(hFontEdit); hFontEdit = NULL; }
    for (auto& kv : brushCache) { if (kv.second) kv.second->Release(); }
    brushCache.clear();
    if (previewImage) { previewImage->Release(); previewImage = NULL; }
    if (plugPanelCache) { plugPanelCache->Release(); plugPanelCache = NULL; }   /* 面板接管位图持 RT 引用, 随窗释放 */
    plugPanelCacheRt = NULL;
    if (appIcon) { appIcon->Release(); appIcon = NULL; }
    for (auto& b : br) { if (b) { b->Release(); b = NULL; } }
    if (brWhite) { brWhite->Release(); brWhite = NULL; }
    if (brCloseHover) { brCloseHover->Release(); brCloseHover = NULL; }
    if (brErr) { brErr->Release(); brErr = NULL; }
    if (brSelGrad) { brSelGrad->Release(); brSelGrad = NULL; }
    if (brSelBar) { brSelBar->Release(); brSelBar = NULL; }
    if (brProgress) { brProgress->Release(); brProgress = NULL; }
    for (auto& kv : iconCache) { if (kv.second) kv.second->Release(); }   /* 位图持 RT 引用, 先于 RT 释放 */
    iconCache.clear();
    /* rt 与 hwndRt 指向同一 COM 包装 (XjsDeviceCreate 里 g_rt = g_hwndRt, 见 xjs_d2d.cpp 生命周期注释):
       只能经 hwndRt 释放一次, 先 Release rt 会把对象打回 0、再 Release hwndRt = 释放已释放内存。
       正常销毁路径 XjsDeviceDiscardCtx 已把两者置空, 此处只兜 hwndRt 一处 */
    if (hwndRt) { hwndRt->Release(); hwndRt = NULL; }
    rt = NULL;
    if (result) {
        /* 先摘回调: 阻断"新的"引擎回调再进入本上下文 (COMPLETE/CHANGE/FAILED/DRAW_ICON/ICON_ASK
           与注册处 xjs_engine.cpp 一一对应; 已在途回调的窄竞态为 DLL 分发粒度所限, 此步收窄窗口) */
        xjs_result_SetCallback(result, XJS_RESULT_EVENT_COMPLETE,  NULL, NULL);
        xjs_result_SetCallback(result, XJS_RESULT_EVENT_CHANGE,    NULL, NULL);
        xjs_result_SetCallback(result, XJS_RESULT_EVENT_FAILED,    NULL, NULL);
        xjs_result_SetCallback(result, XJS_RESULT_EVENT_DRAW_ICON, NULL, NULL);
        xjs_result_SetCallback(result, XJS_RESULT_EVENT_ICON_ASK,  NULL, NULL);
        /* 先摘 UserValue: 回调线程经 OfResult 取窗, 销毁竞态窗口内不再拿到本上下文 */
        xjs_result_SetUserValue(result, NULL);
        xjs_result_Destroy(result); result = NULL;
    }
}

/* 窗口销毁后从注册表摘除并释放上下文 */
void XjsSearchWindow::DestroyAndFree() {
    for (int i = 0; i < s_winCount; i++) {
        if (s_wins[i] == this) {
            for (int j = i; j < s_winCount - 1; j++) s_wins[j] = s_wins[j + 1];
            s_wins[--s_winCount] = NULL;
            /* 档案槽 (uiIndex) 不回收不前移: 槽持久绑定命名窗口档案, ☰菜单启动器
               可随时按未打开的槽重建窗口 (曾按存活序前移, 与持久槽语义冲突) */
            /* 整体前移 = 后窗换槽: 插件窗口令牌按 (代<<32)|槽号 寻址, 从摘除槽起递增代号
               作废旧令牌 (否则旧令牌命中压缩后占住该槽的别的窗口 = 串窗) */
            XjsPluginOnWindowsCompacted(i);
            if (s_curWin == this) SetCur(NULL);
            delete this;
            return;
        }
    }
}

/* 图标就绪广播: 图标缓存进程共享。只失效各窗列表区 (整窗失效会与状态栏旋灯等
   定时失效叠成整窗重绘风暴); 非当前窗的 layout 是其上一帧的值, 尺寸未变时精确,
   变了则随该窗下次整窗重画自愈 */
void XjsSearchWindow::InvalidateAllLists() {
    for (int i = 0; i < s_winCount; i++) s_wins[i]->InvalidateList();
}

/* 遍历全部窗口上下文 (引擎级广播: 渲染缓存作废等) */
void XjsSearchWindow::ForEach(void (*fn)(XjsSearchWindow*)) {
    for (int i = 0; i < s_winCount; i++) fn(s_wins[i]);
}

/* 退出收尾: 先销毁其余窗口 (各自 WM_DESTROY 清理), 主窗自己随后走完整关闭 */
void XjsSearchWindow::DestroyOthers(XjsSearchWindow* keep) {
    for (int i = s_winCount - 1; i >= 0; i--) {
        HWND h = s_wins[i]->hWnd;
        if (h && (!keep || h != keep->hWnd)) DestroyWindow(h);   // 同步 WM_DESTROY → 上下文即时摘除
    }
    /* 嵌套 WM_DESTROY 经 WndProc 入口 Enter 重绑到被销毁子窗, 其 DestroyAndFree 把 Cur 置 NULL —
       返回后调用方的 g_* 宏仍必须解析到 keep: 重绑回 keep (keep 尚未撕毁, 仍在窗口表中)。
       缺此步 = 多窗退出时 Cur()==NULL, 下一个 g_* 读/写直接解引用空指针 */
    if (keep && Alive(keep) && keep->hWnd) Enter(keep->hWnd);
}

/* ==================== 进程共享状态 ==================== */
DWORD g_clipSeqOurs = 0;       /* 自己最后一次写剪贴板后的序列号 */

/* 渲染设备资源 (句柄包装见 xjs_app.h "渲染句柄与后端分发"; g_d2d 是 D2D 后端私有, 在 xjs_d2d.cpp) */
XjsGfxApi* g_gfx = NULL;      /* 当前绘图后端函数表 */
int g_gfxEngine = 0;          /* 绘制引擎 (设置-通用, 重启生效): 0=D2D 1=GDI+ */
XjsDwFactory* g_dw = NULL;
IWICImagingFactory* g_wic = NULL;
XjsFormat* g_tfTitle = NULL;
XjsFormat* g_tfMenu = NULL;
XjsFormat* g_tfHead = NULL;
XjsFormat* g_tfHeadR = NULL;
XjsFormat* g_tfRow = NULL;
XjsFormat* g_tfRowBold = NULL;
XjsFormat* g_tfDim = NULL;
XjsFormat* g_tfTiny = NULL;
XjsFormat* g_tfTinyR = NULL;
XjsFormat* g_tfStatus = NULL;
XjsFormat* g_tfTip = NULL;
XjsFormat* g_tfChip = NULL;
XjsFormat* g_tfRowR = NULL;
XjsFormat* g_tfBig = NULL;
XjsFormat* g_tfCardVal = NULL;
XjsFormat* g_tfSearch = NULL;
XjsFormat* g_tfToast = NULL;
XjsFormat* g_tfTag = NULL;
/* g_uiZoomTenths 已每窗化: 宏 → XjsSearchWindow::Cur()->uiZoom (见 xjs_app.h 宏区) */

/* 引擎状态 */
xjs_engine* g_engine = NULL;
std::atomic<bool> g_isScanning{false};
std::atomic<bool> g_busy{false};
int g_fileCount = 0;
std::wstring g_scanDrive = L"C";   /* 盘符无冒号 (源样式 StatusScanning {0} 同格式) */
int g_scanEnumerated = 0, g_scanTotal = 0;

/* 跨线程 PostMessage 闸门 (见 xjs_app.h 常量节): 引擎线程 post 前 +1, 主窗处理分支末尾 -1。
   计数用 CAS 循环无锁递增; PostMessage 本身失败 (队列句柄异常) 时回退计数, 不留虚账 */
static std::atomic<long> s_postPending{0};

bool XjsPostToUi(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    long cur = s_postPending.load(std::memory_order_relaxed);
    for (;;) {
        if (cur >= XJS_POST_QUEUE_LIMIT) return false;   /* 队满: 静默失败, 调用方就地释放本次分配 */
        if (s_postPending.compare_exchange_weak(cur, cur + 1, std::memory_order_relaxed)) break;
    }
    if (PostMessageW(hwnd, msg, wp, lp)) return true;
    s_postPending.fetch_sub(1, std::memory_order_relaxed);
    return false;
}

void XjsPostToUiDone() {
    s_postPending.fetch_sub(1, std::memory_order_relaxed);
    /* 每窗账同步对冲: 处理分支都在目标窗 WndProc 内, Cur 即消息所属窗。
       CAS 下限 0: 约定被破坏时宁可少冲账也不把窗账减负 (销毁返还按窗账, 负账会多减全局) */
    XjsSearchWindow* w = XjsSearchWindow::Cur();
    if (w) {
        int p = w->uiPostPending.load(std::memory_order_relaxed);
        while (p > 0 && !w->uiPostPending.compare_exchange_weak(p, p - 1, std::memory_order_relaxed)) {}
    }
}

/* 窗口销毁: 队列里本窗的未处理消息将被 DestroyWindow 整批清除, 全局闸门计数按窗账返还 */
void XjsPostToUiDropWindow(XjsSearchWindow* w) {
    if (!w) return;
    int p = w->uiPostPending.exchange(0, std::memory_order_relaxed);
    if (p > 0) s_postPending.fetch_sub((long)p, std::memory_order_relaxed);
    XjsUiOwnedStringsDropHwnd(w->hWnd);   /* 堆载荷 (登记表内) 随窗口一并释放 */
}

/* ---- 堆载荷投递登记 (WM_SEARCH_FAILED 的 std::string*; 值类型载荷一律按值直传不经此) ----
   子窗销毁时队列里未处理的消息被 DestroyWindow 整批清除, 载荷无人 delete = 泄漏。
   投递时登记、消费点 XjsUiOwnedStringTake 摘除后照常 delete、销毁时 DropHwnd 兜底释放。
   登记表只存存活指针 (摘除先于 delete; Take 按指针摘、找不到也原样移交), 即使某天消费点
   忘了 Take, 兜底释放的也仍是存活指针, 不产生二次释放面 */
static std::mutex s_ownedMu;
static std::vector<std::pair<HWND, std::string*>> s_ownedStrings;

bool XjsPostUiOwnedString(XjsSearchWindow* w, UINT msg, LPARAM lp, std::string* payload) {
    if (!payload) return false;
    if (!w || !w->hWnd) { delete payload; return false; }
    {   /* 先登记后投递: 消息一经入队即可被派发, 登记必须发生在派发可能发生之前 */
        std::lock_guard<std::mutex> lk(s_ownedMu);
        s_ownedStrings.push_back({ w->hWnd, payload });
    }
    if (!XjsPostToUi(w->hWnd, msg, (WPARAM)payload, lp)) {
        XjsUiOwnedStringTake(payload);   /* post 失败 = 消息未进队列, 永无消费点, 就地回收 */
        delete payload;
        return false;
    }
    w->uiPostPending.fetch_add(1, std::memory_order_relaxed);   /* 每窗账与 XjsPostToUiFor 同口径 */
    return true;
}

std::string* XjsUiOwnedStringTake(std::string* payload) {
    std::lock_guard<std::mutex> lk(s_ownedMu);
    for (size_t i = 0; i < s_ownedStrings.size(); i++)
        if (s_ownedStrings[i].second == payload) { s_ownedStrings.erase(s_ownedStrings.begin() + i); break; }
    return payload;   /* 所有权移交调用方 (调用方 delete) */
}

void XjsUiOwnedStringsDropHwnd(HWND hwnd) {
    std::lock_guard<std::mutex> lk(s_ownedMu);
    for (size_t i = s_ownedStrings.size(); i-- > 0;)
        if (s_ownedStrings[i].first == hwnd) { delete s_ownedStrings[i].second; s_ownedStrings.erase(s_ownedStrings.begin() + i); }
}

const int g_modeToKeyword[5] = { XJS_KEYWORD_WILDCARD, XJS_KEYWORD_REGEX, XJS_KEYWORD_SQL, XJS_KEYWORD_LUA, XJS_KEYWORD_LUA_EXEC };
const wchar_t* g_modeName[5] = { L"通配符", L"正则表达式", L"SQL", L"Lua 过滤", L"Lua 执行" };
const wchar_t* g_modeDesc[5] = {
    L"支持 * ? 通配与拼音/首拼搜索",
    L"按正则语法匹配文件名",
    L"执行 SQL 语句查询 (SELECT ...)",
    L"用 Lua 表达式过滤每个文件 (f 为文件信息)",
    L"脚本即程序: 自己遍历数据库、自己排序 (return ID 数组)" };
const wchar_t* g_modeHint[5] = {
    L"输入文件名关键词，支持 * 和 ? 通配符，输入即搜…",
    L"输入正则表达式，输入即搜…",
    L"输入 SQL 语句 (SELECT ... FROM alltable WHERE ...)，输入即搜…",
    L"输入 Lua 过滤表达式 (f 为文件信息)，输入即搜…",   /* 源样式各模式 SearchPlaceholder 同文案 */
    L"输入完整 Lua 程序 (可遍历 db 表)，return ID 数组即结果，输入即搜…" };
const wchar_t* g_modeIni[5] = { L"wildcard", L"regex", L"sql", L"lua", L"lua-exec" };

/* 列表数据 */
std::vector<XjsFilterCat> g_filters;   /* 搜索历史已每窗化 (XjsSearchWindow::history) */

/* 皮肤 */
XjsSkin g_skin;
std::wstring g_skinName = L"dark";
std::vector<std::wstring> g_skinMenuNames;
int g_skinEpoch = 0;

/* 托盘 (全局快捷键每窗: hotkeyMod/hotkeyVk 为窗口字段, 见 XjsSearchWindow) */
/* 配置 (xjs_config.json) 派生状态 — 窗口矩形已每窗化入档案, 无全局矩形状态 */
bool g_rbSaved = false;                                          /* 重建对话框"记住上次" */
bool g_rbFields[7] = { true, true, true, false, false, false, true };
std::wstring g_rbDrives;                                         /* 勾选盘符 "C,D", 空=全勾语义按 saved 判 */
NOTIFYICONDATAW g_nid = {0};
bool g_inTray = false;
