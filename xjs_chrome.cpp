/*
 * xjs_chrome.cpp — 标题栏集成区 (一比一正式版标题栏搜索框模式)
 * 背景渐变 / 图标 / ☰菜单 / 搜索框(模式按钮+清空+历史) / 筛选下拉 / 窗口按钮 / 状态栏
 */
#include "xjs_app.h"

/* 搜索框编辑内核 + 光标闪烁驱动器: 每窗一份 (XjsSearchWindow 字段, 宏重定向) */
#define s_searchEd     (XjsSearchWindow::Cur()->searchEd)
#define s_searchBlink  (XjsSearchWindow::Cur()->searchBlink)
#define s_imeUpdBusy   (XjsSearchWindow::Cur()->imeUpdBusy)

/* ==================== 布局 ==================== */

void XjsChromeLayout() {
    RECT rc; GetClientRect(g_hWnd, &rc);
    XjsLayout& L = g_layout;
    L.w = (float)(rc.right - rc.left);
    L.h = (float)(rc.bottom - rc.top);
    L.titlebar = XjsRectF(0, 0, L.w, XSF(40));

    /* 预览面板 (右缘): 3px resizer + 面板 */
    float sbH = g_showStatusbar ? XSF(36) : 0;   /* 状态栏可按每窗设置隐藏 (列表/预览底缘贴窗口底) */
    if (g_previewVisible) {
        L.preview = XjsRectF(L.w - XSF((float)g_previewWidth) - XSF(3), XSF(40), L.w, L.h - sbH);
    } else {
        L.preview = XjsRectF(L.w, XSF(40), L.w, L.h - sbH);
    }
    float listRight = g_previewVisible ? L.preview.left - XSF(3) : L.w;

    /* 表头 / 列表 / 状态栏 (网格模式无表头, 列表区顶到标题栏, 同正式版 list-head display:none) */
    float headH = XjsIsGridView() ? 0 : 28;
    L.listHead = XjsRectF(0, XSF(40), listRight, XSF(40) + XSF(headH));
    L.statusbar = XjsRectF(0, L.h - sbH, L.w, L.h);
    L.list = XjsRectF(0, L.listHead.bottom, listRight, L.statusbar.top);
    L.vtrack = XjsRectF(listRight - XSF(17), L.list.top + XSF(2), listRight - XSF(3), L.list.bottom - XSF(6));

    /* 标题栏: 图标 + ☰菜单 + 搜索框 + 筛选下拉 + 窗口按钮 */
    float iconSize = XSF(22);
    XjsRect iconR = XjsRectF(XSF(12), (XSF(40) - iconSize) / 2, XSF(12) + iconSize, (XSF(40) + iconSize) / 2);
    (void)iconR;
    /* ☰菜单按钮 = 图标区(24) + 实测"菜单"文本宽 + 右留白(10): 右缘与左侧内容边距(☰左缘≈9.5)对称,
       旧固定宽 74 右侧空白过大 (2026-09-15) */
    L.menuBtn = XjsRectF(XSF(48), XSF(6),
                            XSF(48) + XSF(24) + XjsMeasureText(XjsT(L"菜单.菜单按钮"), g_tfMenu) + XSF(10), XSF(34));
    /* 窗口按钮 (右起): 置顶 ─ □ ✕。≡菜单按钮已移除 (与左侧 ☰菜单双入口重复, 2026-09-17);
       控制按钮(─□✕)可按每窗设置隐藏, 隐藏后只留置顶图钉且靠右缘 (左侧元素随之填充) */
    float bw = XSF(32), bh = XSF(28);
    float bx = L.w - XSF(8);
    for (int b = WBTN_CLOSE; b >= WBTN_PIN; b--) {
        if (b != WBTN_PIN && !g_showCtrlBtns) { L.btnRects[b] = XjsRectF(0, 0, 0, 0); continue; }
        bx -= bw;
        L.btnRects[b] = XjsRectF(bx, (XSF(40) - bh) / 2, bx + bw, (XSF(40) - bh) / 2 + bh);
        bx -= XSF(2);
    }
    float btnLeft = L.btnRects[WBTN_PIN].left;   /* 图钉恒可见且必是最左按钮 (控制按钮显示时它排在最左, 隐藏时它是唯一) */
    /* 筛选下拉: ☐ 图标 + 名称 + ˅ (可按每窗设置隐藏, 搜索框向右填满空位) */
    float searchRight;
    if (g_showFilterBox) {
        std::wstring filterName = g_filters.empty() ? L"全部" : g_filters[g_filterSel].name;
        float filterW = XjsMeasureText(filterName.c_str(), g_tfMenu) + XSF(58);
        L.filterBtn = XjsRectF(btnLeft - XSF(12) - filterW, XSF(5), btnLeft - XSF(12), XSF(35));
        searchRight = L.filterBtn.left - XSF(12);
    } else {
        L.filterBtn = XjsRectF(0, 0, 0, 0);
        searchRight = btnLeft - XSF(12);
    }
    /* 搜索框 */
    float searchLeft = L.menuBtn.right + XSF(16);
    L.searchBox = XjsRectF(searchLeft, XSF(5), xf_max(searchRight, searchLeft + XSF(200)), XSF(35));
    L.modeBtn = XjsRectF(L.searchBox.left + XSF(3), XSF(7), L.searchBox.left + XSF(31), XSF(33));
    L.historyBtn = XjsRectF(L.searchBox.right - XSF(29), XSF(7), L.searchBox.right - XSF(3), XSF(33));
    L.clearBtn = XjsRectF(L.historyBtn.left - XSF(28), XSF(7), L.historyBtn.left, XSF(33));
    bool hasText = !s_searchEd.text.empty() || !g_hostedTags.empty();   /* 有标签也显清空钮 (源样式 updateSearchBtns) */
    if (!hasText) L.clearBtn = L.historyBtn;
    /* 输入区 (自绘): 文本/选区/光标全在 D2D 里垂直居中绘制, 无原生 EDIT */
    L.editRect = XjsRectF(L.modeBtn.right + XSF(4), L.searchBox.top + XSF(2),
                             L.clearBtn.left - XSF(2), L.searchBox.bottom - XSF(2));
    XjsHostedLayout();   /* 托管标签链占输入区左段 (源样式 #searchTags), 输入区随之右移 */
}

/* ==================== 自绘搜索输入框 ====================
 * 编辑内核复用共享组件 XjsLineEdit (与行内重命名/别名对话框同一份实现): 文本/选区/光标/
 * 鼠标选字/双击选词/剪贴板/撤销/IME 全部走组件; 搜索框只额外负责"输入即搜"、模式占位符
 * 与模式/历史/筛选按钮的命中。组件在文本被改时置 dirty, 这里读取后触发搜索。 */

/* s_imeUpdBusy = IME 重入守卫, 每窗一份 (上方宏重定向到 XjsSearchWindow) */

static void XjsSearchInvalidate() {
    if (!g_hWnd) return;
    XjsRect b = g_layout.searchBox;
    RECT r = { (int)(b.left - XSF(4)), (int)(b.top - XSF(4)), (int)(b.right + XSF(4)), (int)(b.bottom + XSF(4)) };
    InvalidateRect(g_hWnd, &r, FALSE);
}
/* 编辑后统一收尾: 横向跟随光标 + IME 锚点 + 重置闪烁 + 局部重绘; 文本确实变了才重搜 */
static void XjsSearchAfterEdit() {
    if (s_searchEd.dirty && XjsHostedInCaretMode())
        XjsHostedExitCaret();   /* 文本被改即退出标签光标模式 (源样式 input 事件) */
    s_searchEd.EnsureCaretVisible(g_layout.editRect, g_tfSearch);
    XjsSearchUpdateImeWindow();
    s_searchBlink.Reset();   /* 按键/编辑后光标重新可见 */
    XjsSearchInvalidate();
    if (s_searchEd.dirty) {
        s_searchEd.dirty = false;
        /* 插件输入拦截 (searchInputIntercept 能力, 照正式版 §5.13): 同步询问,
           首个返回 1 的插件接管本次搜索 — 不走原生搜索也不记历史/导航;
           插件拦截器内部发起的搜索不再二次询问 (防自递归, 重入闸在插件侧) */
        if (XjsPluginInputIntercept(s_searchEd.text)) {
            XjsSearchWindow::Cur()->Invalidate();
            return;
        }
        XjsSearchNow(false);
    }
}

bool XjsSearchDragging() { return s_searchEd.dragging; }
/* 未聚焦文本区按下待定 (鼠标交互瞬态, 文件级 static): 移动>阈值=拖窗口, 原地松开=聚焦。
   搜索框占了标题栏大半, 不留这个出口用户很难拖动窗口 (2026-09-16 用户口径) */
static bool s_searchDragPending = false;
static POINT s_searchDragPt = {};
bool XjsSearchDragPending() { return s_searchDragPending; }
const std::wstring& XjsSearchGetText() { return s_searchEd.text; }

void XjsSearchFocus(bool on) {
    if (on && g_searchFocused) return;
    if (!on) {   /* 真失焦 = 其它输入框接管键盘路由: 清选区/拖拽/系统光标。**不再熄闪烁光标** —
                    编辑与焦点分离后光标跟随"编辑权", 接管期的隐藏由渲染处判定 (重命名/模式对话框);
                    熄灯曾在此处理, 会把"接管结束后光标不回来"埋成暗坑 (2026-09-16 口径修订) */
        g_searchFocused = false;
        s_searchEd.anchor = -1;
        s_searchEd.dragging = false;
        s_searchDragPending = false;   /* 焦点变化作废"未聚焦按下待定" (拖窗/聚焦二选一已失时机) */
        if (g_sysCaretMade) { DestroyCaret(); g_sysCaretMade = false; }
        XjsSearchInvalidate();
        return;
    }
    g_searchFocused = true;
    s_searchEd.anchor = -1;
    s_searchEd.dragging = false;
    s_searchDragPending = false;   /* 焦点变化作废"未聚焦按下待定" (聚焦后移动=拖选字, 不再转拖窗) */
    s_searchBlink.Focus(true);
    /* 窗口创建中途(未显示)严禁调 IMM32: TSF 会同步 SendMessage 重入窗口过程造成递归
       (启动白屏闪退根因); 窗口显示后 WM_IME_SETCONTEXT 会自然触发一次更新 */
    if (IsWindowVisible(g_hWnd)) XjsSearchUpdateImeWindow();
    XjsSearchInvalidate();
}

/* 键盘路由交给列表, 搜索框光标照常 (光标跟随"编辑权"常亮, 见 XjsSearchCaretAttach):
 * 点列表/↓/回车跳列表/Tab/弹下拉菜单 都只是"键给列表", 打字/退格/←→ 仍无缝编辑搜索框 */
void XjsSearchYieldKeys() {
    if (!g_searchFocused) return;
    g_searchFocused = false;
    s_searchEd.anchor = -1;
    s_searchEd.dragging = false;
    s_searchDragPending = false;
    XjsSearchInvalidate();
}

/* 首次窗口创建后绑定 (搜索框光标闪烁走共享驱动器; 主窗 WM_ACTIVATE 广播驱动其 激活/失活) */
void XjsSearchCaretAttach() {
    if (s_searchBlink.Attached()) return;
    s_searchBlink.Attach(g_hWnd, []() { XjsSearchInvalidate(); });
    /* 默认置亮: 编辑与焦点分离后, 键盘编辑 (打字/退格/←→/IME) 恒落搜索框, 光标跟随"编辑权"
       而非路由焦点 —— 窗口激活即闪 (启动/唤起可见), 隐藏只由 ①其它编辑接管 (渲染处判定
       重命名/模式对话框) ②窗口失活 (驱动器自管) 负责, 不再随 XjsSearchFocus(false) 熄灭 */
    s_searchBlink.Focus(true);
}

void XjsSearchSetText(const std::wstring& s) {   /* 历史/SQL 入口: 整体置入并触发搜索 */
    s_searchEd.SetText(s, false);
    s_searchEd.dirty = true;
    XjsSearchAfterEdit();
}

void XjsSearchSetTextQuiet(const std::wstring& s) {   /* 插件 SearchSetText execute=0: 只填不搜 (SDK 契约) */
    s_searchEd.SetText(s, false);   /* SetText 不置 dirty → 不触发输入即搜 */
    s_searchEd.EnsureCaretVisible(g_layout.editRect, g_tfSearch);
    XjsSearchUpdateImeWindow();
    XjsSearchInvalidate();
}

void XjsSearchCaretToEnd() {   /* 光标归末尾 (工具函数; 打字聚焦入口已删, 现无调用方) */
    s_searchEd.caret = (int)s_searchEd.text.length();
    s_searchEd.anchor = -1;
    XjsSearchAfterEdit();
}

/* 回车/↓: 选中列表首项并聚焦列表 (源样式口径; 预览开启时经 XjsSelectOnly 联动) */
void XjsSearchJumpToList(bool commit) {
    if (commit) XjsSearchNow(true);
    XjsSearchYieldKeys();   /* 键给列表, 光标不灭 (后续打字/退格/←→ 仍直接编辑搜索框) */
    if (g_resultCount > 0) {
        XjsSelectOnly(0);
        XjsEnsureVisible(0);
    }
    XjsSearchWindow::Cur()->Invalidate();
}

void XjsSearchClear() {
    if (s_searchEd.text.empty()) {
        s_searchEd.caret = 0; s_searchEd.anchor = -1; s_searchEd.scroll = 0;
        XjsSearchInvalidate();
        return;
    }
    s_searchEd.Clear();      /* 记撤销档 (留一键 Ctrl+Z) + 置 dirty */
    XjsSearchAfterEdit();
}

bool XjsSearchChar(wchar_t ch) {
    XjsHostedExitCaret();   /* 字符输入退出标签光标模式 (源样式 input 事件), 字符照常写入 */
    if (!s_searchEd.Char(ch)) return false;
    XjsSearchAfterEdit();
    return true;
}   /* 未聚焦也接字 (编辑不夺焦): 光标不亮, 文本/输入即搜照常 — 聚焦仍只来自点击 */

bool XjsSearchKey(WPARAM vk) {
    /* 未聚焦放行 = 编辑键 (退格删字 / ←→ 移光标, 含 Shift/Ctrl 变体): 编辑不夺焦, 其余键归列表 */
    if (!g_searchFocused && vk != VK_BACK && vk != VK_LEFT && vk != VK_RIGHT) return false;
    if (XjsHostedKey(vk)) return true;   /* 标签链键盘 (←→ 穿行/删除标签) 先于搜索框自身按键 */
    /* 搜索语义键先行 (组件不处理回车/Esc/Tab/下箭头) */
    switch (vk) {
        case VK_RETURN:
            /* 默认选中表项=第一个表项: 结果刷新后首项已常驻选中, 再"选中首项"是无操作 ——
               回车改为直接打开选中项 (列表回车同口径: 回车打开 / Ctrl+回车定位 / Alt+回车属性);
               搜索未结束 (选中项还属上一结果集) 时不打开, 回落下方"选中首项并聚焦列表"口径 */
            if (g_defaultSel == 1 && !g_searching.load() && XjsSelCount() > 0) {
                XjsListKey(VK_RETURN);
                XjsSearchNow(true);   /* 回车照常提交搜索历史 (排在打开之后: 提交会换血结果集/清选中) */
                return true;
            }
            XjsSearchJumpToList(true);   /* 回车: 提交历史 + 选中列表首项并聚焦列表 (源样式口径) */
            return true;
        case VK_DOWN:
            XjsSearchJumpToList(false);  /* ↓: 选中列表首项并聚焦列表 */
            return true;
        case VK_ESCAPE:
            /* Esc 链: 清空搜索词 → 窗口消失 (统一策略: 主窗藏托盘 / 子窗销毁) */
            if (!s_searchEd.text.empty()) XjsSearchClear();
            else XjsDismissWindow(g_hWnd);
            return true;
        case VK_TAB:
            XjsSearchYieldKeys();
            XjsSearchWindow::Cur()->Invalidate();
            return true;
    }
    if (!s_searchEd.Key(vk)) return false;
    XjsSearchAfterEdit();
    return true;
}

/* 搜索框光标本帧是否可显: 驱动器 (闪烁相位×窗口激活) 之外, 其它编辑接管期必须让位
   (行内重命名/模式对话框字段 — 否则接管期双光标齐闪, 同 2026-09-15 实锤的口径, 判定移到这里) */
static bool XjsSearchCaretAllowed() {
    return !XjsRenameActive() && !XjsModeDlgActive();
}

/* 搜索框右键菜单命令 (0剪切 1复制 2粘贴 3全选 4删除选中): 编辑操作走共享 XjsEditMenuApplyCmd */
void XjsSearchMenuCmd(int cmd) {
    if (!g_searchFocused) XjsSearchFocus(true);
    XjsEditMenuApplyCmd(s_searchEd, cmd);
    XjsSearchAfterEdit();
}

/* 搜索框右键菜单 (源样式 searchBoxMenu: SB 图标+快捷键; 禁用态随选区/剪贴板)。
   菜单表走共享 XjsEditMenuAppendItems (与重命名框/路由层字段同一份), ID 段 = IDM_CTX_BASE+20..24;
   尾部追加插件 searchBoxMenus 项区 (无活跃插件 = 零痕迹) */
void XjsShowSearchBoxMenu(POINT screenPt) {
    std::vector<XjsPopupItem> items;
    XjsEditMenuAppendItems(s_searchEd, items, IDM_CTX_BASE + 20);
    if (XjsPluginActiveCap(XPC_SEARCHBOXMENU))
        XjsPluginAppendSearchBoxMenuItems(items, s_searchEd.text);
    XjsShowPopupMenu(g_hWnd, screenPt, items, XSF(150));
}

bool XjsSearchMouseDown(POINT pt) {
    XjsHostedExitCaret();   /* 点击输入框=回到文字输入 (源样式 mousedown 退出标签光标模式) */
    if (!g_searchFocused) {
        /* 未聚焦: 按下只记待定不聚焦 — 原地松开=聚焦+光标定位, 拖动=拖窗口 (见 s_searchDragPending 注) */
        s_searchDragPending = true;
        s_searchDragPt = pt;
        SetCapture(g_hWnd);
        return true;
    }
    s_searchEd.MouseDown(pt, g_layout.editRect, g_tfSearch);   /* 点定位 + 起拖 (锚点=落点) */
    SetCapture(g_hWnd);
    XjsSearchAfterEdit();
    return true;
}

void XjsSearchMouseMove(POINT pt) {
    if (s_searchDragPending) {
        if (!(GetKeyState(VK_LBUTTON) & 0x8000)) { s_searchDragPending = false; return; }   /* 捕获中途丢失: 待定作废 */
        if (abs(pt.x - s_searchDragPt.x) + abs(pt.y - s_searchDragPt.y) <= 6) return;   /* 拖动阈值 6px (同列拖动) */
        s_searchDragPending = false;
        ReleaseCapture();
        SendMessage(g_hWnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);   /* 系统移动循环, 松开即返回 */
        return;
    }
    if (!s_searchEd.dragging) return;
    int before = s_searchEd.caret;
    s_searchEd.MouseMove(pt, g_layout.editRect, g_tfSearch);
    if (s_searchEd.caret != before) XjsSearchAfterEdit();
}

void XjsSearchMouseUp(POINT pt) {
    if (s_searchDragPending) {
        /* 未聚焦单击: 聚焦 + 光标定位到落点 (原生输入框点进口径; 不起选区) */
        s_searchDragPending = false;
        XjsSearchFocus(true);
        s_searchEd.MouseDown(pt, g_layout.editRect, g_tfSearch);
        s_searchEd.MouseUp();   /* 立即收拢单击零长选区 */
        XjsSearchAfterEdit();
        return;
    }
    s_searchEd.MouseUp();   /* 结束拖拽 + 单击零长选区收拢 */
}

bool XjsSearchDoubleClick(POINT pt) {
    XjsSearchFocus(true);
    if (s_searchEd.MouseDoubleClick(pt, g_layout.editRect, g_tfSearch))
        XjsSearchAfterEdit();
    return true;
}

/* 组字窗/候选窗/隐藏系统光标三路都钉到自绘光标处 (实现在组件 UpdateImeAnchor)。
   **重入守卫必须**: Imm 调用会让 IMM32/TSF 同步 SendMessage 重入本窗口过程,
   不加守卫 = 互调递归 → 栈溢出闪退 (启动白屏崩根因)。
   未聚焦也锚定 (打字不夺焦, 组字窗跟随搜索框光标); 两条让位线: 重命名激活期锚点归重命名框,
   窗口未显示期严禁碰 IMM32 (WM_CREATE 中途 TSF 重入 = 启动闪退根因) */
void XjsSearchUpdateImeWindow() {
    if (!g_hWnd || !IsWindowVisible(g_hWnd) || s_imeUpdBusy || XjsRenameActive()) return;
    s_imeUpdBusy = 1;
    s_searchEd.UpdateImeAnchor(g_hWnd, g_layout.editRect, g_tfSearch);
    s_imeUpdBusy = 0;
}

/* IME 上屏结果: GCS_RESULTSTR 整串插入 (组件内完成); 未聚焦也接字 (编辑不夺焦, 聚焦只来自点击) */
bool XjsSearchImeResult(HWND hwnd, LPARAM lParam) {
    if (!(lParam & GCS_RESULTSTR)) return false;
    XjsHostedExitCaret();   /* 输入即退出标签光标模式 (源样式 input 事件) */
    if (s_searchEd.ImeResult(hwnd, lParam)) {
        XjsSearchAfterEdit();
        return true;
    }
    return false;
}

/* ==================== 搜索框托管标签链 UI (源样式 26-hosted-search.js renderHostedTags 等) ====================
 * 标签画在搜索框输入区左段 (源样式 #searchTags flex 在 input 前), 输入区随之右移; 标题栏紧凑档规格:
 * 高 20 胶囊 (accent-soft 底 + accent 字 11.5px 中字重, 悬停描边 accent), × 钮 14 圆, 多来源带 ˅;
 * 标签光标条 2×18 accent (← 进入标签区, 输入框光标同时隐藏, 源样式 .hosted-caret 1.1s 闪烁) */
static const XjsCustomMode* XjsHostedCmById(const std::wstring& id) {
    for (auto& cm : g_customModes)
        if (cm.id == id) return &cm;
    return NULL;
}
static const wchar_t* XjsHostedTypeName(const std::wstring& type) {
    if (type == L"regex") return XjsT(L"搜索模式.正则表达式");
    if (type == L"sql") return L"SQL";
    if (type == L"lua") return XjsT(L"搜索模式.Lua脚本");
    return XjsT(L"搜索模式.通配符");
}
/* × 圆钮矩形 (右缘内缩 padR: 多来源 4 / 单来源 3, 源样式 .multi-src padding-right) */
static XjsRect XjsHostedCloseRect(const XjsRect& r, bool multi) {
    float padR = multi ? XSF(4) : XSF(3);
    float cy = (r.top + r.bottom) / 2;
    return XjsRectF(r.right - padR - XSF(14), cy - XSF(7), r.right - padR, cy + XSF(7));
}

bool XjsHostedInCaretMode() { return g_hostedCaret < (int)g_hostedTags.size(); }

void XjsHostedExitCaret() {
    if (!XjsHostedInCaretMode()) return;
    g_hostedCaret = (int)g_hostedTags.size();
    XjsSearchWindow::Cur()->Invalidate();
}

/* 布局: 计算各标签矩形 (g_hostedRects 与 hostedTags 对齐) 与光标条槽位, 右移输入区左缘。
   源样式 #searchTags 在输入框前 flex 排布把输入框挤窄; dome 给输入区保留最小宽, 溢出标签渲染裁掉 */
float XjsHostedLayout() {
    XjsLayout& L = g_layout;
    g_hostedRects.clear();
    g_hostedCaretRect = XjsRectF(0, 0, 0, 0);
    g_hostedRegion = XjsRectF(0, 0, 0, 0);
    XjsHostedPrune();   /* 渲染时顺带清理已删模式来源 (源样式 renderHostedTags 内 hostedPruneSources) */
    if (g_hostedTags.empty()) return 0;
    bool inMode = XjsHostedInCaretMode();
    float gap = XSF(4), h = XSF(20);
    float cy = (L.editRect.top + L.editRect.bottom) / 2;
    float top = cy - h / 2;
    float x = L.editRect.left;
    for (int i = 0; i < (int)g_hostedTags.size(); i++) {
        XjsHostedTag& t = g_hostedTags[i];
        bool multi = t.srcIds.size() > 1;
        float wTag = XSF(8) + XjsMeasureText(t.word.c_str(), g_tfTag)
                   + (multi ? XSF(2) + XSF(11) : 0) + XSF(2) + XSF(14) + (multi ? XSF(4) : XSF(3));
        if (inMode && i == g_hostedCaret) {
            g_hostedCaretRect = XjsRectF(x, cy - XSF(9), x + XSF(2), cy + XSF(9));
            x += XSF(2) + gap;
        }
        g_hostedRects.push_back(XjsRectF(x, top, x + wTag, top + h));
        x += wTag + gap;
    }
    float total = (x - gap) - L.editRect.left;
    float avail = (L.editRect.right - L.editRect.left) - XSF(36);   /* 输入区最小宽 */
    if (avail < XSF(24)) avail = XSF(24);
    float used = total > avail ? avail : total;
    g_hostedRegion = XjsRectF(L.editRect.left, top - XSF(2), L.editRect.left + used, top + h + XSF(2));
    L.editRect.left += used;
    return used;
}

void XjsHostedRender() {
    bool inMode = XjsHostedInCaretMode();
    if ((g_hostedTags.empty() && !inMode) || !g_rt) return;
    if (g_hostedRegion.right <= g_hostedRegion.left) return;
    g_rt->PushAxisAlignedClip(g_hostedRegion, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    double now = (double)GetTickCount64() / 1000.0;
    for (int i = 0; i < (int)g_hostedRects.size() && i < (int)g_hostedTags.size(); i++) {
        XjsHostedTag& t = g_hostedTags[i];
        XjsRect r = g_hostedRects[i];
        /* 入场动画 (源样式 hostedTagIn .22s: scale .7→1 + 透明 .3→1) */
        float e = xf_min((float)((now - t.born) / 0.22), 1.0f);
        if (e < 0) e = 0;
        float sc = 0.7f + 0.3f * e, al = 0.3f + 0.7f * e;
        float cxm = (r.left + r.right) / 2, cym = (r.top + r.bottom) / 2;
        XjsRect a = XjsRectF((r.left - cxm) * sc + cxm, (r.top - cym) * sc + cym,
                                    (r.right - cxm) * sc + cxm, (r.bottom - cym) * sc + cym);
        bool multi = t.srcIds.size() > 1;
        bool hov = (g_hostHoverTag == i);
        XjsColor accent = g_skin.accent; accent.a *= al;
        XjsColor soft = g_skin.accentSoft; soft.a *= al;   /* 底 = accent-soft (源样式 background: var(--accent-soft)) */
        /* 底 + 悬停描边 (源样式 .hosted-tag:hover border accent); 字/图标用 accent —
           底若是全饱和 accent 就是蓝底蓝字 (首版翻车点) */
        g_rt->FillRoundedRectangle(XjsRoundedRectF(a, XSF(10), XSF(10)), XjsTempBrush(soft));
        if (hov) g_rt->DrawRoundedRectangle(XjsRoundedRectF(a, XSF(10), XSF(10)), XjsTempBrush(accent), 1.0f);
        /* 词 */
        float tx = a.left + XSF(8);
        XjsDrawTextC(t.word.c_str(), (UINT32)t.word.length(), g_tfTag,
            XjsRectF(tx, a.top, a.right - XSF(3), a.bottom), XjsTempBrush(accent), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        /* 多来源 ˅ 下拉指示 (svg 16: m4 6 4 4 4-4 → 11px 盒) */
        if (multi) {
            float ccx = tx + XjsMeasureText(t.word.c_str(), g_tfTag) + XSF(2) + XSF(5.5f);
            float ccy = (a.top + a.bottom) / 2;
            g_rt->DrawLine(XjsPoint2F(ccx - XSF(2.75f), ccy - XSF(1.4f)), XjsPoint2F(ccx, ccy + XSF(1.4f)), XjsTempBrush(accent), XSF(1.24f));
            g_rt->DrawLine(XjsPoint2F(ccx, ccy + XSF(1.4f)), XjsPoint2F(ccx + XSF(2.75f), ccy - XSF(1.4f)), XjsTempBrush(accent), XSF(1.24f));
        }
        /* × 钮 (14 圆; svg 24: M6 6l12 12 → 10px; 悬停圆底 rgba(0,0,0,.14)) */
        XjsRect cb = XjsHostedCloseRect(a, multi);
        float ccx2 = (cb.left + cb.right) / 2, ccy2 = (cb.top + cb.bottom) / 2;
        if (hov && g_hostHoverBtn == 1)
            g_rt->FillEllipse(XjsEllipseF(XjsPoint2F(ccx2, ccy2), XSF(7), XSF(7)), XjsTempBrush(XjsCol(0x000000, 0.14f * al)));
        XjsColor xc = accent; xc.a *= 0.65f;
        float u = XSF(2.5f);
        g_rt->DrawLine(XjsPoint2F(ccx2 - u, ccy2 - u), XjsPoint2F(ccx2 + u, ccy2 + u), XjsTempBrush(xc), XSF(1.1f));
        g_rt->DrawLine(XjsPoint2F(ccx2 + u, ccy2 - u), XjsPoint2F(ccx2 - u, ccy2 + u), XjsTempBrush(xc), XSF(1.1f));
    }
    /* 标签光标条 (闪烁相位随搜索框光标驱动器; 此模式下输入框光标由 XjsSearchRender 隐藏) */
    if (inMode && g_hostedCaretRect.right > g_hostedCaretRect.left && s_searchBlink.On() && XjsSearchCaretAllowed())
        g_rt->FillRectangle(g_hostedCaretRect, XjsTempBrush(g_skin.accent));
    g_rt->PopAxisAlignedClip();
}

bool XjsHostedMouseDown(POINT pt, int* pressTagOut) {
    if (pressTagOut) *pressTagOut = -1;
    for (int i = 0; i < (int)g_hostedRects.size() && i < (int)g_hostedTags.size(); i++) {
        if (!XjsPtIn(g_hostedRects[i], pt)) continue;
        if (pressTagOut && XjsPtIn(XjsHostedCloseRect(g_hostedRects[i], g_hostedTags[i].srcIds.size() > 1), pt))
            *pressTagOut = i;        /* ×: 记待定, 松开才移除 (源样式 tag-close click) */
        return true;                 /* 卡体吃点击不做事 (源样式 tag 本体无 click 处理) */
    }
    return false;
}

bool XjsHostedMouseUp(POINT pt, int pressTag) {
    if (pressTag < 0 || pressTag >= (int)g_hostedRects.size() || pressTag >= (int)g_hostedTags.size())
        return false;
    if (XjsPtIn(g_hostedRects[pressTag], pt) &&
        XjsPtIn(XjsHostedCloseRect(g_hostedRects[pressTag], g_hostedTags[pressTag].srcIds.size() > 1), pt)) {
        XjsHostedRemoveTag(pressTag);   /* 按下与松开都在同一 × 上才移除 */
        return true;
    }
    return false;
}

/* 标签当文字处理 (源样式 keywordEl keydown): ← (输入框光标在开头时) 进入标签区, ←→ 穿行;
 * Backspace 删光标左侧标签, Delete 删光标右侧标签; 正常态 Backspace 在开头/Delete 空框删末尾标签;
 * 其它按键退出模式恢复正常输入 (修饰键除外)。返回 true=已消费 */
bool XjsHostedKey(WPARAM vk) {
    int n = (int)g_hostedTags.size();
    bool inMode = XjsHostedInCaretMode();
    if (vk == VK_LEFT) {
        bool atStart = (s_searchEd.caret == 0 && !s_searchEd.HasSel());
        if (n > 0 && (inMode || atStart)) {
            if (!inMode) g_hostedCaret = n;
            if (g_hostedCaret > 0) g_hostedCaret--;
            XjsSearchWindow::Cur()->Invalidate();
            return true;
        }
        return false;
    }
    if (vk == VK_RIGHT && inMode) {
        g_hostedCaret++;
        if (g_hostedCaret >= n) XjsHostedExitCaret();   /* 穿过全部标签回输入框 (光标落开头) */
        else XjsSearchWindow::Cur()->Invalidate();
        s_searchEd.caret = 0;
        s_searchEd.anchor = -1;
        return true;
    }
    if (vk == VK_BACK) {
        if (inMode) {
            if (g_hostedCaret > 0) { g_hostedCaret--; XjsHostedRemoveTag(g_hostedCaret); }
            return true;
        }
        if (s_searchEd.caret == 0 && !s_searchEd.HasSel() && n > 0) { XjsHostedRemoveTag(n - 1); return true; }
        return false;
    }
    if (vk == VK_DELETE) {
        if (inMode) {
            if (g_hostedCaret < n) XjsHostedRemoveTag(g_hostedCaret);
            return true;
        }
        if (s_searchEd.text.empty() && n > 0) { XjsHostedRemoveTag(n - 1); return true; }
        return false;
    }
    /* 其它按键退出标签光标模式 (修饰键除外, 字符照常写入) */
    if (inMode && vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU && vk != VK_LWIN && vk != VK_RWIN)
        XjsHostedExitCaret();
    return false;
}

/* 多来源标签悬停计时 (源样式 mouseenter 180ms 开 / mouseleave 260ms 关; dome 菜单点外关闭) */
void XjsHostedMouseMove(POINT pt) {
    int hit = -1, btn = 0;
    for (int i = 0; i < (int)g_hostedRects.size() && i < (int)g_hostedTags.size(); i++) {
        if (XjsPtIn(g_hostedRects[i], pt)) {
            hit = i;
            btn = XjsPtIn(XjsHostedCloseRect(g_hostedRects[i], g_hostedTags[i].srcIds.size() > 1), pt) ? 1 : 0;
            break;
        }
    }
    if (hit == g_hostHoverTag && btn == g_hostHoverBtn) return;
    g_hostHoverTag = hit;
    g_hostHoverBtn = btn;
    if (hit >= 0 && btn == 0 && g_hostedTags[hit].srcIds.size() > 1)
        SetTimer(g_hWnd, ID_TIMER_HOSTEDSRC, 180, NULL);
    else
        KillTimer(g_hWnd, ID_TIMER_HOSTEDSRC);
    XjsSearchWindow::Cur()->Invalidate();
}

/* WM_TIMER(ID_TIMER_HOSTEDSRC) 一次性: 仍在多来源标签上 → 弹"切换搜索来源"菜单 */
void XjsHostedHoverTimer(HWND hwnd) {
    KillTimer(hwnd, ID_TIMER_HOSTEDSRC);
    int i = g_hostHoverTag;
    if (i < 0 || i >= (int)g_hostedTags.size() || g_hostedTags[i].srcIds.size() < 2) return;
    g_hostMenuTag = i;
    XjsHostedTag& t = g_hostedTags[i];
    std::vector<XjsPopupItem> items;
    items.push_back({ 0, XjsT(L"菜单.切换搜索来源"), L"", false, false, true, false });   /* 源样式 .src-menu-title */
    for (int s = 0; s < (int)t.srcIds.size(); s++) {
        const XjsCustomMode* cm = XjsHostedCmById(t.srcIds[s]);
        std::wstring label = cm ? cm->name : XjsT(L"菜单.已删除自定义模式");
        label += L" (";
        label += XjsHostedTypeName(cm ? cm->type : L"");
        std::wstring desc = cm ? XjsTrimWs(cm->desc) : L"";
        label += desc.empty() ? L")" : (L": " + desc + L")");
        items.push_back({ IDM_HOSTED_SRC_BASE + s, label, L"", s == t.active, false, false, false });
    }
    XjsRect r = g_hostedRects[i];
    POINT anchor = { (LONG)r.left, (LONG)r.bottom };
    ClientToScreen(g_hWnd, &anchor);
    XjsShowPopupMenu(g_hWnd, anchor, items, XSF(220));
}

void XjsHostedHoverReset(HWND hwnd) {
    KillTimer(hwnd, ID_TIMER_HOSTEDSRC);
    if (g_hostHoverTag == -1 && g_hostHoverBtn == 0) return;
    g_hostHoverTag = -1;
    g_hostHoverBtn = 0;
    XjsSearchWindow::Cur()->Invalidate();
}

/* 占位符 + 文本/选区/光标 (XjsChromeRenderTitlebar 内调用, 裁剪在输入区内)。
   占位符与光标都交给组件统一处理: 空文本同样有光标 (三个输入框行为一致) */
static void XjsSearchRender() {
    const XjsRect a = g_layout.editRect;
    if (a.right - a.left <= 1) return;
    XjsBrush* selBr = XjsTempBrush(XjsColorF(0.22f, 0.48f, 0.95f, 0.80f));   /* 高对比选区底 */
    /* 光标用文字色: 选中底色是蓝的, 光标用强调色/白色都会与底色同色看不见;
       标签光标模式下输入框光标隐藏 (源样式 hosted-caret-hide, 光标条在标签区) */
    bool caretOn = s_searchBlink.On() && XjsSearchCaretAllowed() && !XjsHostedInCaretMode();
    s_searchEd.Render(g_rt, a, g_tfSearch, g_br[XTH_TEXT], selBr, g_br[XTH_TEXT],
        g_modeHint[g_mode], g_br[XTH_TEXT_FAINT], caretOn);
}

/* ==================== 背景渐变 ==================== */

void XjsChromeRenderBackground() {
    XjsSizeU szU = g_rt->GetPixelSize();
    float w = (float)szU.width, h = (float)szU.height;
    /* 硬清屏: 无条件用不透明底色刷掉整屏旧内容 (Clear 不会失败) —
       此前渐变创建失败会静默跳过填充, 内容叠在保留的旧帧上 = 残影 */
    g_rt->Clear(g_skin.bg1);
    XjsGradientStop gs[2] = { { 0.0f, g_br[XTH_BG1]->GetColor() }, { 1.0f, g_br[XTH_BG2]->GetColor() } };
    XjsGradBrush* br = NULL;
    g_rt->CreateLinearGradientBrush(XjsPoint2F(0, 0), XjsPoint2F(0, h), gs, 2, &br);
    if (br) { g_rt->FillRectangle(XjsRectF(0, 0, w, h), br); br->Release(); }
    XjsGradientStop rgs[2] = { { 0.0f, g_br[XTH_ACCENT_SOFT]->GetColor() }, { 1.0f, XjsCol(0, 0.f) } };
    XjsGradBrush* rb = NULL;
    g_rt->CreateRadialGradientBrush(XjsPoint2F(w * 0.88f, -XSF(80)), XjsPoint2F(0, 0), XSF(450), XSF(170), rgs, 2, &rb);
    if (rb) { g_rt->FillRectangle(XjsRectF(w * 0.4f, 0, w, XSF(180)), rb); rb->Release(); }
}

/* ==================== 标题栏 ==================== */

static void XjsRenderTitleIcon() {
    if (!g_appIcon) {
        /* 图标 = exe 内嵌 RCDATA 205 的完整 ico 字节 (main.rc, 与资源 32512 同源文件), WIC 内存解码取最大帧 —
           不依赖 exe 目录磁盘 ico 文件 (单文件分发图标不丢; 磁盘方案曾因图标改名断链, 2026-09-20 实锤) */
        HMODULE mod = GetModuleHandleW(NULL);
        HRSRC rc = FindResourceW(mod, MAKEINTRESOURCEW(205), RT_RCDATA);
        HGLOBAL hg = rc ? LoadResource(mod, rc) : NULL;
        const void* icoData = hg ? LockResource(hg) : NULL;
        DWORD icoSize = rc ? SizeofResource(mod, rc) : 0;
        IWICBitmapDecoder* dec = NULL;
        IStream* stream = NULL;
        if (icoData && icoSize && g_wic) {
            /* 资源字节包成内存 IStream 走老接口 CreateDecoderFromStream (Factory2 的
               CreateDecoderFromMemory 受工程 WINVER=0x0601 门控不可见, 不为它提门控) */
            HGLOBAL hgMem = GlobalAlloc(GMEM_MOVEABLE, icoSize);
            void* pMem = hgMem ? GlobalLock(hgMem) : NULL;
            if (pMem) {
                CopyMemory(pMem, icoData, icoSize);
                GlobalUnlock(hgMem);
                if (SUCCEEDED(CreateStreamOnHGlobal(hgMem, TRUE, &stream)) && stream)
                    g_wic->CreateDecoderFromStream(stream, NULL, WICDecodeMetadataCacheOnDemand, &dec);
            }
            if (!stream && hgMem) GlobalFree(hgMem);   /* 流未接管内存 (创建失败/流创建失败) : 手动放 */
        }
        if (dec) {
            UINT n = 0;
            dec->GetFrameCount(&n);
            IWICBitmapFrameDecode* frame = NULL;
            UINT bestW = 0;
            for (UINT i = 0; i < n; i++) {
                IWICBitmapFrameDecode* f = NULL;
                if (SUCCEEDED(dec->GetFrame(i, &f)) && f) {
                    UINT w = 0, h = 0;
                    f->GetSize(&w, &h);
                    if (w >= bestW) { bestW = w; if (frame) frame->Release(); frame = f; }
                    else f->Release();
                }
            }
            if (frame) {
                IWICFormatConverter* conv = NULL;
                if (SUCCEEDED(g_wic->CreateFormatConverter(&conv)) && conv &&
                    SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom)))
                    g_rt->CreateBitmapFromWicBitmap(conv, NULL, &g_appIcon);
                if (conv) conv->Release();
                frame->Release();
            }
            dec->Release();
        }
        if (stream) stream->Release();   /* dec 已放完再放流 (fDeleteOnRelease=TRUE 连带内存) */
    }
    float isz = XSF(18);
    if (g_appIcon)
        g_rt->DrawBitmap(g_appIcon, XjsRectF(XSF(12), (XSF(40) - isz) / 2, XSF(12) + isz, (XSF(40) + isz) / 2));
}

void XjsChromeRenderTitlebar() {
    XjsLayout& L = g_layout;
    /* 底部分隔线 */
    g_rt->FillRectangle(XjsRectF(0, L.titlebar.bottom - 1, L.w, L.titlebar.bottom), g_br[XTH_BORDER]);
    XjsRenderTitleIcon();
    /* ☰ 菜单按钮 */
    {
        XjsRect r = L.menuBtn;
        bool hov = (g_hoverBtn & HB_MENU) || g_appMenuOpen;
        if (hov) g_rt->FillRoundedRectangle(XjsRoundedRectF(r, XSF(6), XSF(6)), g_br[XTH_ROW_HOVER]);
        float cx = r.left + XSF(14), cy = (r.top + r.bottom) / 2;
        for (int k = -1; k <= 1; k++)
            g_rt->DrawLine(XjsPoint2F(cx - XSF(4.5f), cy + k * XSF(3)), XjsPoint2F(cx + XSF(4.5f), cy + k * XSF(3)),
                g_appMenuOpen ? (XjsBrush*)g_br[XTH_ACCENT] : (XjsBrush*)g_br[XTH_TEXT_DIM], 1.4f);
        std::wstring t = XjsT(L"菜单.菜单按钮");
        g_rt->DrawText(t.c_str(), (UINT32)t.length(), g_tfMenu,
            XjsRectF(r.left + XSF(24), r.top, r.right, r.bottom),
            g_appMenuOpen ? (XjsBrush*)g_br[XTH_ACCENT] : (XjsBrush*)g_br[XTH_TEXT_DIM]);
    }
    /* 搜索框: 始终持焦点 (失焦只熄光标, 见 XjsSearchFocus), 故不再画焦点外圈/高亮边
       —— 恒为普通边框, 焦点态不做视觉区分 (用户口径 2026-09-16) */
    {
        XjsRoundedRect sb = XjsRoundedRectF(L.searchBox, XSF(8), XSF(8));
        g_rt->FillRoundedRectangle(sb, g_br[XTH_PANEL]);
        g_rt->DrawRoundedRectangle(sb, g_br[XTH_BORDER], 1.0f);
        /* 模式按钮 (放大镜, 点击切换搜索模式) */
        {
            XjsRect mb = L.modeBtn;
            if (g_modeMenuOpen)
                g_rt->FillRoundedRectangle(XjsRoundedRectF(mb, XSF(6), XSF(6)), g_br[XTH_ACCENT_SOFT]);
            else if (g_hoverBtn & HB_PILL)
                g_rt->FillRoundedRectangle(XjsRoundedRectF(mb, XSF(6), XSF(6)), g_br[XTH_ROW_HOVER]);
            XjsBrush* bc = g_modeMenuOpen ? (XjsBrush*)g_br[XTH_ACCENT] : (XjsBrush*)g_br[XTH_TEXT_FAINT];
            XjsDrawMagnifier(XjsPoint2F((mb.left + mb.right) / 2, (mb.top + mb.bottom) / 2), XSF(6), bc, 1.6f);
        }
        /* 清空 × */
        if (!s_searchEd.text.empty()) {
            XjsRect cb = L.clearBtn;
            float ccx = (cb.left + cb.right) / 2, ccy = (cb.top + cb.bottom) / 2, u = XSF(5);
            XjsBrush* cbr = (g_hoverBtn & HB_CLEAR) ? (XjsBrush*)g_br[XTH_TEXT] : (XjsBrush*)g_br[XTH_TEXT_FAINT];
            g_rt->DrawLine(XjsPoint2F(ccx - u, ccy - u), XjsPoint2F(ccx + u, ccy + u), cbr, 1.8f);
            g_rt->DrawLine(XjsPoint2F(ccx + u, ccy - u), XjsPoint2F(ccx - u, ccy + u), cbr, 1.8f);
        }
        /* 历史 (时钟) */
        {
            XjsRect hb = L.historyBtn;
            float cx2 = (hb.left + hb.right) / 2, cy2 = (hb.top + hb.bottom) / 2;
            XjsBrush* hbr = (g_hoverBtn & HB_HISTORY) ? (XjsBrush*)g_br[XTH_TEXT] : (XjsBrush*)g_br[XTH_TEXT_FAINT];
            g_rt->DrawEllipse(XjsEllipseF(XjsPoint2F(cx2, cy2), XSF(6.5f), XSF(6.5f)), hbr, 1.4f);
            g_rt->DrawLine(XjsPoint2F(cx2, cy2 - XSF(3.6f)), XjsPoint2F(cx2, cy2), hbr, 1.4f);
            g_rt->DrawLine(XjsPoint2F(cx2, cy2), XjsPoint2F(cx2 + XSF(2.8f), cy2 + XSF(1.8f)), hbr, 1.4f);
        }
        /* 输入文本/选区/光标 (自绘, 最后画在图标之上层); 托管标签链画输入区左段 */
        XjsHostedRender();
        XjsSearchRender();
    }
    /* 筛选下拉: ☰ 全部 ˅ */
    {
        XjsRect fb = L.filterBtn;
        XjsRoundedRect fr = XjsRoundedRectF(fb, XSF(8), XSF(8));
        g_rt->FillRoundedRectangle(fr, g_br[XTH_PANEL]);
        g_rt->DrawRoundedRectangle(fr, (g_hoverBtn & HB_FILTER) ? (XjsBrush*)g_br[XTH_BORDER_STRONG] : (XjsBrush*)g_br[XTH_BORDER], 1.0f);
        /* 栅格小图标 */
        float ix = fb.left + XSF(12), iy = (fb.top + fb.bottom) / 2;
        XjsBrush* ic = (XjsBrush*)g_br[XTH_TEXT_DIM];
        g_rt->DrawLine(XjsPoint2F(ix - XSF(4), iy - XSF(4)), XjsPoint2F(ix + XSF(4), iy - XSF(4)), ic, 1.3f);
        g_rt->DrawLine(XjsPoint2F(ix - XSF(4), iy), XjsPoint2F(ix + XSF(4), iy), ic, 1.3f);
        g_rt->DrawLine(XjsPoint2F(ix - XSF(4), iy + XSF(4)), XjsPoint2F(ix + XSF(4), iy + XSF(4)), ic, 1.3f);
        std::wstring name = g_filters.empty() ? L"全部" : g_filters[g_filterSel].name;
        g_rt->DrawText(name.c_str(), (UINT32)name.length(), g_tfMenu,
            XjsRectF(fb.left + XSF(22), fb.top, fb.right - XSF(18), fb.bottom), g_br[XTH_TEXT]);
        /* 箭头 */
        float cx = fb.right - XSF(12), cy = (fb.top + fb.bottom) / 2;
        g_rt->DrawLine(XjsPoint2F(cx - XSF(3), cy - XSF(1.5f)), XjsPoint2F(cx, cy + XSF(1.5f)), ic, 1.3f);
        g_rt->DrawLine(XjsPoint2F(cx, cy + XSF(1.5f)), XjsPoint2F(cx + XSF(3), cy - XSF(1.5f)), ic, 1.3f);
    }
    /* 窗口按钮 (图标按源样式 search.html #tb-btn-group 的 12×12 viewBox SVG 逐点还原;
       置顶=图钉形状, 激活时 accent 色 + 旋转 45°; 最大化/还原=单框 / 双框标准图标;
       零矩形 = 被每窗设置隐藏的控制按钮, 跳过不画) */
    for (int b = WBTN_PIN; b <= WBTN_CLOSE; b++) {
        XjsRect r = L.btnRects[b];
        if (r.right - r.left < 1) continue;
        bool hov = (g_hoverWndBtn == b);
        if (hov) {
            if (b == WBTN_CLOSE) g_rt->FillRoundedRectangle(XjsRoundedRectF(r, XSF(6), XSF(6)), g_brCloseHover);
            else g_rt->FillRoundedRectangle(XjsRoundedRectF(r, XSF(6), XSF(6)), g_br[XTH_ROW_HOVER]);
        }
        bool pinActive = (b == WBTN_PIN && g_topmost);
        XjsBrush* bc = (b == WBTN_CLOSE && hov) ? (XjsBrush*)g_brWhite
                       : pinActive ? (XjsBrush*)g_br[XTH_ACCENT]
                       : (XjsBrush*)g_br[XTH_TEXT_DIM];
        float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
        const float U = XSF(1.0f);        /* viewBox 单位 → px (图标盒 12×12 单位) */
        const float SW = XSF(1.5f);       /* 源样式 stroke-width: 1.5 单位 */
        auto PX = [&](float v) { return cx + (v - 6.0f) * U; };
        auto PY = [&](float v) { return cy + (v - 6.0f) * U; };
        auto Line = [&](float x1, float y1, float x2, float y2) {
            g_rt->DrawLine(XjsPoint2F(PX(x1), PY(y1)), XjsPoint2F(PX(x2), PY(y2)), bc, SW);
        };
        if (b == WBTN_MIN) {
            Line(2.5f, 6.0f, 9.5f, 6.0f);                       /* M2.5 6h7 */
        } else if (b == WBTN_MAX) {
            if (IsZoomed(g_hWnd)) {
                /* 还原: 前框完整 + 后框只留上/右两条边 (源样式 maximized 态 SVG) */
                g_rt->DrawRoundedRectangle(XjsRoundedRectF(
                    XjsRectF(PX(2.5f), PY(4.0f), PX(8.0f), PY(9.5f)), U, U), bc, SW);
                Line(4.5f, 4.0f, 4.5f, 2.5f);
                Line(4.5f, 2.5f, 10.0f, 2.5f);
                Line(10.0f, 2.5f, 10.0f, 8.0f);
                Line(10.0f, 8.0f, 8.0f, 8.0f);
            } else {
                g_rt->DrawRoundedRectangle(XjsRoundedRectF(
                    XjsRectF(PX(2.5f), PY(2.5f), PX(9.5f), PY(9.5f)), U, U), bc, SW);
            }
        } else if (b == WBTN_CLOSE) {
            Line(3.0f, 3.0f, 9.0f, 9.0f);                       /* M3 3l6 6 */
            Line(9.0f, 3.0f, 3.0f, 9.0f);                       /* M9 3 3 9 */
        } else if (b == WBTN_PIN) {
            /* 置顶: 图钉 (针 + 帽); 激活时源样式旋转 45° (=pin-active svg rotate) */
            if (pinActive)
                g_rt->SetRotate(45.0f, XjsPoint2F(cx, cy));
            Line(6.0f, 10.5f, 6.0f, 5.0f);                      /* M6 10.5V5 针 */
            Line(3.2f, 1.5f, 8.8f, 1.5f);                       /* M3.2 1.5h5.6 */
            Line(8.8f, 1.5f, 8.3f, 4.2f);                       /* l-.5 2.7 */
            Line(8.3f, 4.2f, 10.2f, 6.0f);                      /* l1.9 1.8 */
            Line(10.2f, 6.0f, 10.2f, 7.2f);                     /* v1.2 */
            Line(10.2f, 7.2f, 1.8f, 7.2f);                      /* H1.8 */
            Line(1.8f, 7.2f, 1.8f, 6.0f);                       /* V6 */
            Line(1.8f, 6.0f, 3.7f, 4.2f);                       /* l1.9-1.8 */
            Line(3.7f, 4.2f, 3.2f, 1.5f);                       /* L3.2 1.5 z */
            if (pinActive) g_rt->ResetTransform();
        }
    }
}

/* ==================== 状态栏 ==================== */

/* 右侧功能按钮: 工具箱 + 插件状态栏项 (空间地图等入口由插件 statusBar 能力提供) */
static void XjsStatusButton(const XjsRect& r, int hoverFlag, const wchar_t* text, int iconKind) {
    bool hov = (g_hoverStatus == hoverFlag);
    if (hov) g_rt->FillRoundedRectangle(XjsRoundedRectF(r, XSF(6), XSF(6)), g_br[XTH_PANEL2]);
    float cy = (r.top + r.bottom) / 2;
    float ix = r.left + XSF(12);
    XjsBrush* bc = hov ? (XjsBrush*)g_br[XTH_TEXT] : (XjsBrush*)g_br[XTH_TEXT_DIM];
    if (iconKind >= 0) {
        /* 工具箱: 铃铛 */
        g_rt->DrawEllipse(XjsEllipseF(XjsPoint2F(ix, cy - XSF(0.5f)), XSF(4), XSF(4)), bc, 1.2f);
        g_rt->DrawLine(XjsPoint2F(ix - XSF(5.5f), cy + XSF(3.5f)), XjsPoint2F(ix + XSF(5.5f), cy + XSF(3.5f)), bc, 1.2f);
        g_rt->DrawLine(XjsPoint2F(ix, cy - XSF(5.5f)), XjsPoint2F(ix, cy - XSF(4)), bc, 1.2f);
    }
    /* iconKind < 0 = 无图标 (插件状态栏项): 只画文字, 文字贴左缘 */
    float tx = r.left + (iconKind < 0 ? XSF(10) : XSF(24));
    g_rt->DrawText(text, (UINT32)wcslen(text), g_tfMenu,
        XjsRectF(tx, r.top, r.right, r.bottom), bc);
}

void XjsRenderStatusbar() {
    XjsLayout& L = g_layout;
    g_rt->FillRectangle(XjsRectF(0, L.statusbar.top, L.w, L.statusbar.top + 1), g_br[XTH_BORDER]);
    g_rt->FillRectangle(XjsRectF(0, L.statusbar.top + 1, L.w, L.statusbar.bottom), g_br[XTH_PANEL]);
    float x = XSF(20);
    float mid = (L.statusbar.top + L.statusbar.bottom) / 2;
    if (g_busy) {
        static float phase = 0;
        phase += 0.22f;
        XjsDrawSpinner(XjsPoint2F(x + XSF(7), mid), XSF(6), phase);
        x += XSF(24);
    }
    if (!g_errText.empty()) {
        g_rt->DrawText(g_errText.c_str(), (UINT32)g_errText.length(), g_tfStatus,
            XjsRectF(x, L.statusbar.top, L.w - XSF(320), L.statusbar.bottom), g_br[XTH_HL]);
    } else {
        g_rt->DrawText(g_statusText.c_str(), (UINT32)g_statusText.length(), g_tfStatus,
            XjsRectF(x, L.statusbar.top, L.w - XSF(320), L.statusbar.bottom), g_br[XTH_TEXT_FAINT]);
    }
    /* · 已选中 N 项 (accent; 源样式 Search.SelectedCount, 仅多选时显示; 计数回读引擎选中_取数量) */
    int selCount = XjsSelCount();
    if (selCount > 1) {
        std::wstring sel = XjsFmt(XjsT(L"状态栏.已选中N项"), XjsNumText((long long)selCount));
        float sw = XjsMeasureText(sel.c_str(), g_tfStatus);
        float baseX = x;
        if (g_errText.empty()) baseX += XjsMeasureText(g_statusText.c_str(), g_tfStatus);
        g_rt->DrawText(sel.c_str(), (UINT32)sel.length(), g_tfStatus,
            XjsRectF(baseX + XSF(16), L.statusbar.top, baseX + XSF(16) + sw + XSF(4), L.statusbar.bottom), g_br[XTH_ACCENT]);
    }
    /* 右侧按钮 */
    float by = L.statusbar.top + XSF(5), bh = XSF(26);
    float bx = L.w - XSF(16);
    L.sbToolbox = XjsRectF(bx - XSF(78), by, bx, by + bh); bx -= XSF(84);
    XjsStatusButton(L.sbToolbox, 3, XjsT(L"状态栏.工具箱"), 2);
    /* 插件状态栏项 (statusBar 能力, 2026-09-19): 内置组左侧向左排, 右对齐依次向左;
       宽 = 文字实测 + 内边距, 总宽上限 = 状态栏宽 40% 超出按序截断。
       几何写回 g_layout.sbPlugin[] — 悬停/命中/落地三处同读 (列几何同源纪律) */
    L.sbPluginN = 0;
    if (XjsPluginActiveCap(XPC_STATUSBAR)) {
        float budget = (L.w - XSF(32)) * 0.40f;
        int n = XjsPluginStatusBarCount();
        for (int i = 0; i < n && L.sbPluginN < 8; i++) {
            XjsPluginStatusBarDef d;
            if (!XjsPluginStatusBarAt(i, &d) || d.label.empty()) continue;
            float tw = XjsMeasureText(d.label.c_str(), g_tfMenu) + XSF(18);
            if (tw > budget) break;
            budget -= tw;
            bx -= XSF(8);   /* 与左侧邻项间距 */
            L.sbPlugin[L.sbPluginN] = XjsRectF(bx - tw, by, bx, by + bh);
            bx -= tw;
            XjsStatusButton(L.sbPlugin[L.sbPluginN], 100 + L.sbPluginN, d.label.c_str(), -1);
            L.sbPluginN++;
        }
    }
}

/* ==================== 悬停状态 ==================== */

void XjsUpdateHoverState(POINT pt) {
    if (!g_mouseTracking) {
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, g_hWnd, 0 };
        TrackMouseEvent(&tme);
        g_mouseTracking = true;
    }
    int newBtn = 0, newWndBtn = WBTN_NONE, newStatus = -1, newHoverRow = -1;
    bool inList = false;
    if (pt.y < g_layout.titlebar.bottom) {
        for (int b = WBTN_PIN; b <= WBTN_CLOSE; b++)
            if (g_layout.btnRects[b].right - g_layout.btnRects[b].left > 1 &&
                XjsPtIn(g_layout.btnRects[b], pt)) { newWndBtn = b; break; }
        if (newWndBtn == WBTN_NONE) {
            if (XjsPtIn(g_layout.menuBtn, pt)) newBtn |= HB_MENU;
            if (XjsPtIn(g_layout.modeBtn, pt)) newBtn |= HB_PILL;
            if (!s_searchEd.text.empty() && XjsPtIn(g_layout.clearBtn, pt)) newBtn |= HB_CLEAR;
            if (XjsPtIn(g_layout.historyBtn, pt)) newBtn |= HB_HISTORY;
            if (g_layout.filterBtn.right > g_layout.filterBtn.left &&   /* 隐藏态 = 零矩形 (每窗设置) */
                XjsPtIn(g_layout.filterBtn, pt)) newBtn |= HB_FILTER;
        }
    } else if (pt.y >= g_layout.statusbar.top) {
        int plugHover = -1;   /* 插件状态栏项 (hoverFlag = 100+下标, 渲染同源) */
        for (int i = 0; i < g_layout.sbPluginN; i++)
            if (XjsPtIn(g_layout.sbPlugin[i], pt)) { plugHover = i; break; }
        if (plugHover >= 0) newStatus = 100 + plugHover;
        else if (XjsPtIn(g_layout.sbToolbox, pt)) newStatus = 3;
    } else if (pt.y >= g_layout.list.top && pt.y < g_layout.list.bottom && pt.x < g_layout.list.right) {
        inList = true;
        newHoverRow = XjsItemAtPoint(pt);
    }
    if (newWndBtn != g_hoverWndBtn || newBtn != g_hoverBtn || newStatus != g_hoverStatus ||
        newHoverRow != g_hoverRow || inList != g_listHover) {
        if (newHoverRow != g_hoverRow || inList != g_listHover)
            XjsListHoverChanged(g_hoverRow, g_listHover);   /* 旧行高亮进渐隐拖尾 (源样式悬停余晖) */
        g_hoverWndBtn = newWndBtn;
        g_hoverBtn = newBtn;
        g_hoverStatus = newStatus;
        g_hoverRow = newHoverRow;
        g_listHover = inList;
        XjsSearchWindow::Cur()->Invalidate();
    }
}

/* ==================== 鼠标 ==================== */

/* 任一活动编辑框命中 (客户区坐标): 搜索框只认文本输入区 editRect — 框内的放大镜/
   清空/历史是按钮, 悬停须保持箭头, 整框 I-beam 是错的; 且搜索框**聚焦后**才给 I-beam —
   未聚焦时按下拖动是拖窗口, 悬停箭头提示"这里能拖" (2026-09-16 用户口径);
   行内重命名/别名对话框区域由各自模块提供。命中即应显示 I-beam 文本光标 —
   所有输入框统一走这一处判定, 避免只修搜索框 */
bool XjsAnyEditBoxHit(POINT pt) {
    if (g_searchFocused && XjsPtIn(g_layout.editRect, pt)) return true;
    if (XjsRenameActive() && XjsPtIn(XjsRenameEditRect(), pt)) return true;
    return false;
}

/* 药丸模式菜单打开时的自定义模式 id 快照 (下标→id): 菜单按作用范围过滤后下标≠全局下标,
   且菜单开着时其它窗口仍可能增删模式使下标漂移 — 回传统一按 id 定位 (菜单鼠标互斥, 全进程单份) */
static std::vector<std::wstring> s_pillModeIds;
std::wstring XjsCmodeMenuIdAt(int idx) {
    if (idx >= 0 && idx < (int)s_pillModeIds.size()) return s_pillModeIds[idx];
    return std::wstring();
}

/* 标题栏命令按钮按下待定 (2026-09-17 用户口径: 命令类控件一律松开才触发, 按下只悬停):
   0=无; 1..4=WBTN_PIN..WBTN_CLOSE; 5=☰菜单 6=筛选下拉 7=模式药丸 8=清空 9=历史;
   s_hostPressTag = 托管标签 × 待定下标 (-1 无)。鼠标天然互斥, 文件级 static 即可 */
static int s_chromePress = 0;
static int s_hostPressTag = -1;

/* 标题栏命令按钮命中 (按下/松开两处同源, 不依赖悬停状态): 与 XjsUpdateHoverState 同几何 */
static int XjsChromeHitCmd(POINT pt) {
    for (int b = WBTN_PIN; b <= WBTN_CLOSE; b++)
        if (g_layout.btnRects[b].right - g_layout.btnRects[b].left > 1 &&
            XjsPtIn(g_layout.btnRects[b], pt)) return b;
    if (XjsPtIn(g_layout.menuBtn, pt)) return 5;
    if (g_layout.filterBtn.right > g_layout.filterBtn.left &&   /* 隐藏态 = 零矩形 (每窗设置) */
        XjsPtIn(g_layout.filterBtn, pt)) return 6;
    if (XjsPtIn(g_layout.modeBtn, pt)) return 7;
    if (!s_searchEd.text.empty() && XjsPtIn(g_layout.clearBtn, pt)) return 8;
    if (XjsPtIn(g_layout.historyBtn, pt)) return 9;
    return 0;
}

bool XjsChromeMouseDown(POINT pt) {
    if (pt.y >= g_layout.titlebar.bottom) return false;
    /* 命令按钮: 只记待定并吃掉 (不给 HTCAPTION 拖窗), 松开仍命中同一按钮才执行。
       SetCapture 保证"拖出窗外松开"也能收到 mouseup 消费待定, 否则残留待定会被
       之后其它带捕获手势的松开 (落点恰在原按钮上) 误触发 */
    int cmd = XjsChromeHitCmd(pt);
    if (cmd) { s_chromePress = cmd; SetCapture(g_hWnd); return true; }
    /* 搜索框区域: 标签链 ×(记待定)/卡体吃点击 → 文本区点定位光标, 其余按钮各司其职 */
    if (XjsPtIn(g_layout.searchBox, pt)) {
        if (XjsHostedMouseDown(pt, &s_hostPressTag)) { SetCapture(g_hWnd); return true; }
        XjsSearchMouseDown(pt);   /* 点定位光标/拖拽选字 (编辑类交互, 保持按下语义) */
        return true;
    }
    return false;   /* 其余标题栏区域 → HTCAPTION 拖动 */
}

bool XjsChromeMouseUp(POINT pt) {
    if (pt.y >= g_layout.titlebar.bottom) { s_chromePress = 0; s_hostPressTag = -1; return false; }
    int cmd = s_chromePress; s_chromePress = 0;
    int tag = s_hostPressTag; s_hostPressTag = -1;
    if (tag >= 0) XjsHostedMouseUp(pt, tag);
    if (!cmd || XjsChromeHitCmd(pt) != cmd) return cmd != 0 || tag >= 0;   /* 拖离原按钮 = 取消 */
    switch (cmd) {
        case WBTN_PIN:
            g_topmost = !g_topmost;
            SetWindowPos(g_hWnd, g_topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            XjsSaveConfig();   /* 置顶随窗口档案持久化 (开窗恢复) — 审计口径: 状态改动即时落盘 */
            XjsSearchWindow::Cur()->Invalidate();
            break;
        case WBTN_MIN: ShowWindow(g_hWnd, SW_MINIMIZE); break;
        case WBTN_MAX: ShowWindow(g_hWnd, IsZoomed(g_hWnd) ? SW_RESTORE : SW_MAXIMIZE); break;
        case WBTN_CLOSE:
            /* 标题栏 ✕ → 窗口消失统一策略入口 (子窗=真销毁, 主窗=藏托盘; 见 XjsDismissWindow) */
            XjsDismissWindow(g_hWnd);
            break;
        case 5: XjsShowAppMenu(); break;
        case 6: {
            POINT anchor = { (LONG)g_layout.filterBtn.left, (LONG)g_layout.filterBtn.bottom };
            ClientToScreen(g_hWnd, &anchor);
            XjsShowFilterMenu(anchor);
            break;
        }
        case 7: {
            /* 模式药丸下拉 */
            XjsSearchYieldKeys();   /* 弹下拉菜单: 键交列表, 光标不灭 (菜单非输入框) */
            std::vector<XjsPopupItem> items;
            items.push_back({ 0, XjsT(L"菜单.搜索模式"), L"", false, false, true, false });
            for (int m = 0; m < 4; m++)
                items.push_back({ IDM_MODE_BASE + m, g_modeName[m], g_modeDesc[m], m == g_mode, false, false, false });
            /* 用户自定义模式 (点击=模板提交, <keyword> 换搜索框输入; 有未被占位符消费的输入词=模板+输入词
               多重搜索; 尾部✎编辑 ✕删除, 源样式同款)
               作用范围过滤: 只列 全局共享 + 本窗专属; 窗口专属 sub 显示"仅本窗口"标识 */
            {
                std::vector<XjsPopupItem> cmItems;
                const std::wstring winName = XjsSearchWindow::Cur()->name;
                s_pillModeIds.clear();
                for (int i = 0; i < (int)g_customModes.size(); i++) {
                    if (!XjsCmApplies(g_customModes[i], winName)) continue;
                    s_pillModeIds.push_back(g_customModes[i].id);
                    cmItems.push_back({ IDM_CMODE_BASE + (int)s_pillModeIds.size() - 1,
                                        g_customModes[i].name,
                                        g_customModes[i].scope == 1 ? XjsT(L"菜单.仅本窗口") : g_customModes[i].desc,
                                        false, false, false, false, false, XMI_NONE, true, true });   /* 尾两参 editBtn/delBtn */
                }
                if (!cmItems.empty()) {
                    items.push_back({ 0, L"", L"", false, true, false, false });
                    items.insert(items.end(), cmItems.begin(), cmItems.end());
                }
            }
            /* 插件模式 (searchModes 能力, 2026-09-19): 模板型 = 同用户模式语义 (点击转托管标签,
               同名合并来源); 接管型 (无 template) = 点击通知插件 OnSearchMode (插件自行驱动)。
               id 快照表 s_pillModeIds 混存 用户id 与 "p:<插件id>:<序>", 回传主窗按前缀分流;
               插件项无 ✎/✕ (宿主不代编辑插件清单) */
            if (XjsPluginModeCount() > 0) {
                std::vector<XjsPopupItem> pmItems;
                for (int pm = 0; pm < XjsPluginModeCount(); pm++) {
                    XjsPluginModeRef r;
                    if (!XjsPluginModeAt(pm, &r)) break;
                    const XjsPluginModeDef* d = XjsPluginModeDefAt(r);
                    XjsPluginBrief b;
                    if (!d || !XjsPluginBriefAt(r.plugin, &b)) continue;
                    s_pillModeIds.push_back(XjsPluginModeSrcId(b.id, r.modeIdx));
                    pmItems.push_back({ IDM_CMODE_BASE + (int)s_pillModeIds.size() - 1, d->name,
                                        d->desc.empty() ? b.name : d->desc,
                                        false, false, false, false, false, XMI_NONE });
                }
                if (!pmItems.empty()) {
                    items.push_back({ 0, L"", L"", false, true, false, false });
                    items.insert(items.end(), pmItems.begin(), pmItems.end());
                }
            }
            items.push_back({ 0, L"", L"", false, true, false, false });
            items.push_back({ IDM_MENU_BASE + 45, XjsT(L"菜单.添加搜索模式"), L"", false, false, false, true });
            POINT anchor = { (LONG)g_layout.modeBtn.left, (LONG)g_layout.modeBtn.bottom };
            ClientToScreen(g_hWnd, &anchor);
            g_modeMenuOpen = true;
            XjsSearchWindow::Cur()->Invalidate();
            XjsShowPopupMenu(g_hWnd, anchor, items, XSF(300));
            break;
        }
        case 8: {
            /* 清空按钮 = 全部清空 (输入 + 托管标签, 源样式 searchClear click 同语义 —
               只清输入会被"纯标签链重搜"接管, 标签永远清不掉); 空词走完整搜索链路.
               默认无焦点口径: 清空不回焦搜索框, 要输入先点文本区 */
            s_searchEd.SetText(L"", false);
            s_searchEd.dirty = false;
            XjsHostedClearTags();
            XjsSearchNow(false);
            XjsSearchInvalidate();
            break;
        }
        case 9:
            XjsSearchYieldKeys();   /* 历史下拉面板: 键交列表, 光标不灭 */
            XjsShowHistoryPanel();
            break;
        default: break;
    }
    return true;
}

/* ==================== 菜单 ==================== */

void XjsShowAppMenu() {
    std::vector<XjsPopupItem> items;
    /* 搜索模式/添加搜索模式 只放搜索框放大镜菜单 (HB_PILL), ☰ 菜单不再重复 (用户要求) */
    {   /* 创建新窗口 = 窗口启动器子菜单 (通用级联): "新建空白窗口" + 全部命名档案。
           已打开的档案带 ✓, 点击 = 激活该窗口; 未打开的点击 = 按档案重建窗口 (2026-09-17 口径)。
           子项 id = IDM_MENU_BASE+80+档案槽 (分发见 XjsOnPopupResult; 槽 0 = 主窗不列出) */
        XjsPopupItem nw;
        nw.id = IDM_MENU_BASE + 46;
        nw.title = XjsT(L"菜单.创建新窗口");
        nw.sub = L"Ctrl+N";
        nw.children.push_back({ IDM_MENU_BASE + 46, XjsT(L"菜单.新建空白窗口"), L"Ctrl+N", false, false, false, false, false, XMI_NONE });
        for (int k = 1; k < XjsUiProfileCount(); k++) {
            XjsUiProfile* prof = XjsUiProfileAt(k);
            if (!prof) continue;
            XjsSearchWindow* w = XjsSearchWindow::AtSlot(k);
            XjsPopupItem c;
            c.id = IDM_MENU_BASE + 80 + k;
            c.title = w ? w->name : (prof->name.empty() ? XjsT(L"菜单.未命名窗口") : prof->name);
            c.checked = (w != NULL);   /* ✓ = 已打开 (点击激活, 不再新建) */
            /* sub = 该窗口私有的全局快捷键 (存活窗读窗口字段, 未打开读档案 — 与注册事实源同源);
               兼为双击 Ctrl 目标时追加 " / 双击Ctrl" (按档案名匹配, 口径同 XjsDoubleCtrlTargetSlot) */
            UINT hkMod = 0, hkVk = 0;
            if (w) { hkMod = w->hotkeyMod; hkVk = w->hotkeyVk; }
            else   { hkMod = prof->hotkeyMod; hkVk = prof->hotkeyVk; }
            if (hkVk) c.sub = XjsHotkeyText(hkMod, hkVk);
            if (!g_doubleCtrlTarget.empty() && prof->name == g_doubleCtrlTarget) {
                if (!c.sub.empty()) c.sub += L" / ";
                c.sub += XjsT(L"菜单.双击Ctrl");
            }
            nw.children.push_back(c);
        }
        items.push_back(nw);
    }
    items.push_back({ IDM_MENU_BASE + 41, XjsT(L"菜单.紧凑视图"), L"", g_viewMode == VM_LIST, false, false, false });
    items.push_back({ IDM_MENU_BASE + 42, XjsT(L"菜单.详情视图"), L"", g_viewMode == VM_DETAILS, false, false, false });
    items.push_back({ IDM_MENU_BASE + 43, XjsT(L"菜单.中等图标"), L"", g_viewMode == VM_MEDIUM, false, false, false });
    items.push_back({ IDM_MENU_BASE + 44, XjsT(L"菜单.大图标"), L"", g_viewMode == VM_LARGE, false, false, false });
    items.push_back({ IDM_MENU_BASE + 14, XjsT(L"菜单.预览面板"), L"", g_previewVisible, false, false, false });
    items.push_back({ 0, L"", L"", false, true, false, false });
    /* 皮肤/自启动/关于 → 独立设置窗口 (皮肤列表 18 项曾把菜单撑出屏幕); 重建索引也已移入设置 */
    items.push_back({ IDM_MENU_BASE + 50, XjsT(L"菜单.设置"), L"", false, false, false, false });
    items.push_back({ 0, L"", L"", false, true, false, false });
    items.push_back({ IDM_MENU_BASE + 2, XjsT(L"通用词.退出"), L"", false, false, false, false });
    XjsRect r = g_layout.menuBtn;
    POINT anchor = { (LONG)r.left, (LONG)r.bottom };
    ClientToScreen(g_hWnd, &anchor);
    g_appMenuOpen = true;
    XjsShowPopupMenu(g_hWnd, anchor, items, XSF(260));
}

void XjsShowHistoryPanel() {
    std::vector<XjsPopupItem> items;
    int n = (int)g_history.size() < 14 ? (int)g_history.size() : 14;
    if (n == 0) {
        items.push_back({ 0, XjsT(L"菜单.暂无搜索历史"), L"", false, false, true, false });
    } else {
        for (int i = 0; i < n; i++)
            items.push_back({ IDM_HISTORY_BASE + i, g_history[i], L"", false, false });
    }
    items.push_back({ 0, L"", L"", false, true });
    items.push_back({ IDM_HISTORY_BASE + 999, XjsT(L"菜单.清空搜索历史"), L"", false, false });
    POINT anchor = { (LONG)g_layout.historyBtn.left, (LONG)g_layout.historyBtn.bottom };
    ClientToScreen(g_hWnd, &anchor);
    XjsShowPopupMenu(g_hWnd, anchor, items, XSF(340));
}

void XjsShowFilterMenu(POINT anchorScreen) {
    std::vector<XjsPopupItem> items;
    for (int i = 0; i < (int)g_filters.size(); i++)
        items.push_back({ IDM_FILTER_BASE + i, g_filters[i].name, L"", i == g_filterSel, false });
    XjsShowPopupMenu(g_hWnd, anchorScreen, items, XSF(180));
}

/* 文件右键菜单 (targetIdx = 菜单作用行, 调用方已保证它是选中项; 多选态菜单项走批量语义) */
void XjsShowContextMenu(POINT screenPt, int targetIdx) {
    std::vector<XjsPopupItem> items;
    int selCount = XjsSelCount();
    bool one = (selCount <= 1);
    int idx = (targetIdx >= 0) ? targetIdx : XjsSelPrimaryIdx();
    bool isDrive = (idx >= 0 && XjsIsDriveRow(idx));
    bool aliasOn = (g_engine && xjs_db_IsFieldEnabled(g_engine, "别名") != 0);
    if (one) {
        /* 项序/文案/图标 = 源样式 buildFileCtxMenu: 打开/打开路径/重命名/设置别名/剪切/复制/复制路径/复制名称/删除/属性 */
        items.push_back({ IDM_CTX_BASE + 1, XjsT(L"通用词.打开"), L"", false, false, false, false, false, XMI_OPEN });
        items.push_back({ IDM_CTX_BASE + 2, XjsT(L"右键菜单.打开路径"), L"", false, false, false, false, false, XMI_FOLDER });
        items.push_back({ IDM_CTX_BASE + 9, XjsT(L"右键菜单.重命名"), L"F2", false, false, false, false, isDrive, XMI_RENAME });
        items.push_back({ IDM_CTX_BASE + 10, XjsT(L"右键菜单.设置别名"), L"", false, false, false, false, !aliasOn, XMI_ALIAS });
        items.push_back({ 0, L"", L"", false, true });
        items.push_back({ IDM_CTX_BASE + 7, XjsT(L"编辑菜单.剪切"), L"Ctrl+X", false, false, false, false, isDrive, XMI_CUT });
        items.push_back({ IDM_CTX_BASE + 8, XjsT(L"通用词.复制"), L"Ctrl+C", false, false, false, false, isDrive, XMI_COPY });
        items.push_back({ IDM_CTX_BASE + 3, XjsT(L"右键菜单.复制路径"), L"", false, false, false, false, false, XMI_COPYPATH });
        items.push_back({ IDM_CTX_BASE + 4, XjsT(L"右键菜单.复制名称"), L"", false, false, false, false, false, XMI_RENAME });
        items.push_back({ 0, L"", L"", false, true });
        items.push_back({ IDM_CTX_BASE + 6, XjsT(L"通用词.删除"), L"Del", false, false, false, false, isDrive, XMI_DELETE });
        items.push_back({ 0, L"", L"", false, true });
        items.push_back({ IDM_CTX_BASE + 5, XjsT(L"右键菜单.属性"), L"", false, false, false, false, false, XMI_GEAR });
    } else {
        items.push_back({ IDM_CTX_BASE + 11, XjsT(L"通用词.打开"), L"", false, false, false, false, false, XMI_OPEN });
        items.push_back({ IDM_CTX_BASE + 12, XjsT(L"右键菜单.打开路径"), L"", false, false, false, false, false, XMI_FOLDER });
        items.push_back({ 0, L"", L"", false, true });
        items.push_back({ IDM_CTX_BASE + 7, XjsT(L"编辑菜单.剪切"), L"Ctrl+X", false, false, false, false, false, XMI_CUT });
        items.push_back({ IDM_CTX_BASE + 8, XjsT(L"通用词.复制"), L"Ctrl+C", false, false, false, false, false, XMI_COPY });
        items.push_back({ IDM_CTX_BASE + 3, XjsT(L"右键菜单.复制路径"), L"", false, false, false, false, false, XMI_COPYPATH });
        items.push_back({ IDM_CTX_BASE + 4, XjsT(L"右键菜单.复制名称"), L"", false, false, false, false, false, XMI_RENAME });
        items.push_back({ 0, L"", L"", false, true });
        items.push_back({ IDM_CTX_BASE + 10, XjsT(L"右键菜单.设置别名"), L"", false, false, false, false, !aliasOn, XMI_ALIAS });
        items.push_back({ 0, L"", L"", false, true });
        items.push_back({ IDM_CTX_BASE + 6, XjsT(L"通用词.删除"), L"Del", false, false, false, false, false, XMI_DELETE });
    }
    /* 插件项区 (fileContextMenu 能力): 内置项之后追加; 无活跃插件 = 零痕迹。
       路径取选中集 (空回落目标行); kind (文件/目录/驱动器) 取菜单作用行 */
    if (XjsPluginActiveCap(XPC_FILECTX)) {
        std::vector<int> pids;   /* 文件上下文 = 选中集引擎 FileId (v3: 宿主与插件间引用一律 ID, 空回落目标行) */
        for (int si : XjsSelIndices()) {
            if (g_result) { int fid = xjs_result_GetFileId(g_result, si); if (fid >= 0) pids.push_back(fid); }
        }
        if (pids.empty() && idx >= 0 && g_result) {
            int fid = xjs_result_GetFileId(g_result, idx);
            if (fid >= 0) pids.push_back(fid);
        }
        bool isDirRow = isDrive || (idx >= 0 && g_engine && g_result
            && (xjs_db_GetFileAttributes(g_engine, xjs_result_GetFileId(g_result, idx)) & FILE_ATTRIBUTE_DIRECTORY));
        XjsPluginAppendFileMenuItems(items, pids, isDirRow, isDrive);
    }
    XjsShowPopupMenu(g_hWnd, screenPt, items, XSF(230));
}

void XjsShowToolboxMenu() {
    XjsShowAppMenu();
}

/* ==================== 添加/编辑搜索模式 对话框 (主窗模态遮罩, 源样式 11-search-modes.js smMask 同构) ====================
 * 名称/模板必填 (空则 toast 提示); 类型段选择;
 * 模板提交执行, 其中 <keyword> 替换为搜索框纯输入文字 (不含标签, 见 XjsCmExpandTpl)。
 * 编辑态保留 id 原位更新 + 删除二次确认 (源样式 toggleModeDel: 首击变红"确定删除", 再击真删)。
 * 状态 g_modeDlg 归所属窗口类 (XjsSearchWindow::modeDlg) — 状态绑窗不落全局 (用户红线)。 */

struct XjsModeDlgRects {
    XjsRect card{}, name{}, desc{}, tpl{}, type[4]{}, scope[2]{}, ok{}, cancel{}, del{};
};

static void XjsMdlgLayout(float W, float H, XjsModeDlgRects* R) {
    float s = XSF(1);
    float cw = 470 * s, chh = 548 * s;   /* 466→532: 新增作用范围段 (label+段28) 与提示两行; 532→548: <keyword> 提示加一行 */
    float x0 = (W - cw) / 2, y0 = (H - chh) / 2;
    if (x0 < 8 * s) x0 = 8 * s;
    if (y0 < 8 * s) y0 = 8 * s;
    R->card = XjsRectF(x0, y0, x0 + cw, y0 + chh);
    float pad = 20 * s, lx = x0 + pad, rx = x0 + cw - pad;
    float y = y0 + 14 * s + 30 * s + 10 * s;          /* 标题行高 */
    float labelH = 18 * s, fieldH = 30 * s, gap = 12 * s;
    auto fieldRow = [&](XjsRect& f) {
        y += labelH + 4 * s;
        f = XjsRectF(lx, y, rx, y + fieldH);
        y = f.bottom + gap;
    };
    fieldRow(R->name);
    fieldRow(R->desc);
    y += labelH + 4 * s;                              /* 类型段 */
    {
        float segW = (rx - lx - 3 * 6 * s) / 4;
        for (int i = 0; i < 4; i++)
            R->type[i] = XjsRectF(lx + i * (segW + 6 * s), y, lx + i * (segW + 6 * s) + segW, y + 28 * s);
    }
    y = R->type[0].bottom + gap;
    y += labelH + 4 * s;                              /* 作用范围段 (全局共享 / 仅本窗口) */
    {
        float segW = (rx - lx - 6 * s) / 2;
        for (int i = 0; i < 2; i++)
            R->scope[i] = XjsRectF(lx + i * (segW + 6 * s), y, lx + i * (segW + 6 * s) + segW, y + 28 * s);
    }
    y = R->scope[0].bottom + gap;
    y += labelH + 4 * s;
    R->tpl = XjsRectF(lx, y, rx, y + 96 * s);      /* 模板多行 (XjsLineEdit::multiline): 行高=字号×1.5 */
    y = R->tpl.bottom + gap;
    y += 48 * s;                                      /* 提示三行 (类型提示 + 作用范围提示 + 占位符提示) */
    float btnH = 30 * s, by = y0 + chh - pad - btnH;
    R->ok = XjsRectF(rx - 76 * s, by, rx, by + btnH);
    R->cancel = XjsRectF(R->ok.left - 10 * s - 64 * s, by, R->ok.left - 10 * s, by + btnH);
    R->del = XjsRectF(lx, by, lx + 96 * s, by + btnH);
}

static const wchar_t* XjsMdlgTip(int type) {
    switch (type) {
        case 0: return XjsT(L"搜索框.提示通配符");
        case 1: return XjsT(L"搜索框.提示正则");
        case 3: return XjsT(L"搜索框.提示Lua");
        default: return XjsT(L"搜索框.提示SQL");
    }
}

static const wchar_t* XjsMdlgTypeName(int type) {
    /* 译文指针每次现取: static 数组会冻结首调时的表内指针, 换语言重装表后即悬垂 */
    const wchar_t* N[4] = { XjsT(L"搜索模式.通配符"), XjsT(L"搜索模式.正则"), L"SQL", L"Lua" };
    return N[type];
}

static int XjsMdlgTypeIndex(const std::wstring& t) {   /* 类型串 → 段下标 (编辑态回填) */
    if (t == L"regex") return 1;
    if (t == L"sql") return 2;
    if (t == L"lua") return 3;
    return 0;
}

static void XjsMdlgClose() {
    XjsModeDlg& d = g_modeDlg;
    d.open = false;
    d.delConfirm = false;
    d.nameEd.SetFocused(g_hWnd, false);
    d.descEd.SetFocused(g_hWnd, false);
    d.tplEd.SetFocused(g_hWnd, false);
    /* 状态切换帧硬保证: 下一帧整窗重绘, 遮罩/对话框不留任何残迹; 顺带清拖尾 (对话框期间的滚动已使行下标失效) */
    g_needFullPaint = true;
    g_hoverTraces.clear();
    /* 默认无焦点口径: 对话框关闭不回焦搜索框, 要输入先点文本区 */
}

static void XjsMdlgSave(HWND hwnd) {
    XjsModeDlg& d = g_modeDlg;
    std::wstring name = XjsTrimWs(d.nameEd.ed.text), tpl = XjsTrimWs(d.tplEd.ed.text), desc = XjsTrimWs(d.descEd.ed.text);
    if (name.empty()) {
        XjsToastShow(g_hWnd, XjsT(L"模式对话框.请输入名称"), XTOAST_WARN, XSF(1));
        d.nameEd.SetFocused(hwnd, true);
        return;
    }
    if (tpl.empty()) {
        XjsToastShow(g_hWnd, XjsT(L"模式对话框.请输入模板"), XTOAST_WARN, XSF(1));
        d.tplEd.SetFocused(hwnd, true);
        return;
    }
    static const wchar_t* const TYPES[4] = { L"wildcard", L"regex", L"sql", L"lua" };
    if (d.edit) {
        for (auto& cm : g_customModes) {
            if (cm.id == d.editId) {
                cm.name = name; cm.desc = desc; cm.type = TYPES[d.type]; cm.tpl = tpl;
                cm.scope = d.scope;   /* 作用范围: 仅本窗口绑定发起窗档案名 (对话框打开时捕获) */
                cm.ownerName = d.scope == 1 ? d.ownerName : std::wstring();
                break;
            }
        }
        XjsMdlgClose();
        XjsSaveConfig();   /* 编辑: 原位更新保持 id 不变, 不切换/不触发搜索 (源样式同款) */
        /* 标签链中有同名标签: 按新模板重搜整链 (源样式 saveAddMode inChain 分支) */
        for (auto& t : g_hostedTags)
            if (_wcsicmp(t.word.c_str(), name.c_str()) == 0) { XjsHostedExecChain(XjsSearchGetText()); break; }
        return;
    }
    /* 新增: 头部插入 (上限 100, 淘汰最旧并提示, 源样式同款); id = "m"+时间戳36进制 (源样式同款) */
    XjsCustomMode cm;
    wchar_t buf[24];
    unsigned long long t = GetTickCount64();
    const wchar_t* digits = L"0123456789abcdefghijklmnopqrstuvwxyz";
    int n = 0;
    do { buf[n++] = digits[t % 36]; t /= 36; } while (t);
    buf[n] = 0;
    cm.id = std::wstring(L"m") + std::wstring(buf, n);
    cm.name = name; cm.desc = desc; cm.type = TYPES[d.type]; cm.tpl = tpl;
    cm.scope = d.scope;
    cm.ownerName = d.scope == 1 ? d.ownerName : std::wstring();
    g_customModes.insert(g_customModes.begin(), cm);
    if ((int)g_customModes.size() > 100) {
        int removed = (int)g_customModes.size() - 100;
        g_customModes.resize(100);
        XjsToastShow(g_hWnd, XjsFmt(XjsT(L"模式对话框.超上限提示"), std::to_wstring(removed)).c_str(),
                     XTOAST_WARN, XSF(1));
    }
    XjsSaveConfig();
    XjsMdlgClose();
    /* 添加后把模式名转为搜索框托管标签 (不切换全局模式), 输入词保留为链尾 (源样式 saveAddMode 尾段) */
    XjsHostedWordAsTag(name, false);
}

void XjsModeDlgOpen(bool edit, const std::wstring& editId) {
    XjsModeDlg& d = g_modeDlg;
    XjsSearchFocus(false);   /* 模态接管输入: 搜索框失焦 (光标停闪, 键盘不再进搜索框) */
    d.open = true;
    d.edit = edit;
    d.editId = editId;
    d.type = 0;   /* 默认通配符 (2026-09-16 用户口径, 原默认 SQL) */
    d.scope = 0;  /* 默认全局共享 (与既有行为一致) */
    d.ownerName = XjsSearchWindow::Cur()->name;   /* 捕获发起窗口档案名 (scope=1 绑定/提示用, 不依赖"当前窗") */
    d.delConfirm = false;
    g_needFullPaint = true;   /* 首帧整窗重绘: 遮罩一次性盖满全窗 */
    if (!d.nameEd.blink.Attached()) {
        auto repaint = [] { XjsSearchWindow::Cur()->Invalidate(); };
        d.nameEd.Attach(g_hWnd, repaint);
        d.descEd.Attach(g_hWnd, repaint);
        d.tplEd.Attach(g_hWnd, repaint);
    }
    std::wstring n, ds, t;
    if (edit) {
        for (auto& cm : g_customModes) {
            if (cm.id == editId) {
                n = cm.name; ds = cm.desc; t = cm.tpl;
                d.type = XjsMdlgTypeIndex(cm.type);   /* 编辑态回填类型 (曾漏回填: 保存把正则/SQL 静默改成通配符) */
                d.scope = cm.scope;
                break;
            }
        }
    }
    d.tplEd.ed.multiline = true;   /* 模板 = 多行输入 (XjsLineEdit 通用能力, 组件默认单行零影响) */
    d.nameEd.ed.SetText(n, edit);
    d.descEd.ed.SetText(ds, false);
    d.tplEd.ed.SetText(t, false);
    d.nameEd.SetFocused(g_hWnd, true);
    XjsSearchWindow::Cur()->Invalidate();
}

bool XjsModeDlgActive() { return g_modeDlg.open; }

bool XjsModeDlgMouseDown(POINT pt) {
    XjsModeDlg& d = g_modeDlg;
    if (!d.open) return false;
    RECT crc;
    GetClientRect(g_hWnd, &crc);
    XjsModeDlgRects R;
    XjsMdlgLayout((float)crc.right, (float)crc.bottom, &R);
    if (XjsEditFieldMouseDown(g_hWnd, pt)) return true;   /* 三个字段 (点外自动失焦) */
    /* 命令控件: 按下只记待定 (松开触发口径), 松开仍命中同一控件才执行 */
    for (int i = 0; i < 4; i++)
        if (XjsPtIn(R.type[i], pt)) { d.pressCmd = 1 + i; return true; }
    for (int i = 0; i < 2; i++)
        if (XjsPtIn(R.scope[i], pt)) { d.pressCmd = 5 + i; return true; }
    if (XjsPtIn(R.cancel, pt)) { d.pressCmd = 7; return true; }
    if (XjsPtIn(R.ok, pt)) { d.pressCmd = 8; return true; }
    if (d.edit && XjsPtIn(R.del, pt)) { d.pressCmd = 9; return true; }
    if (!XjsPtIn(R.card, pt)) { XjsMdlgClose(); XjsSearchWindow::Cur()->Invalidate(); return true; }   /* 点遮罩空白关闭 */
    d.delConfirm = false;   /* 点卡片其它区域: 重置删除确认态 (源样式 resetModeDel) */
    XjsSearchWindow::Cur()->Invalidate();
    return true;             /* 卡体吃点击 (模态) */
}

bool XjsModeDlgMouseUp(POINT pt) {
    XjsModeDlg& d = g_modeDlg;
    if (!d.open) return false;
    int cmd = d.pressCmd;
    d.pressCmd = 0;
    if (!cmd) return false;
    RECT crc;
    GetClientRect(g_hWnd, &crc);
    XjsModeDlgRects R;
    XjsMdlgLayout((float)crc.right, (float)crc.bottom, &R);
    bool hit = false;
    if (cmd >= 1 && cmd < 5) hit = XjsPtIn(R.type[cmd - 1], pt);
    else if (cmd >= 5 && cmd < 7) hit = XjsPtIn(R.scope[cmd - 5], pt);
    else if (cmd == 7) hit = XjsPtIn(R.cancel, pt);
    else if (cmd == 8) hit = XjsPtIn(R.ok, pt);
    else if (cmd == 9 && d.edit) hit = XjsPtIn(R.del, pt);
    if (!hit) return true;   /* 拖离原控件 = 取消 */
    if (cmd >= 1 && cmd < 5) {
        d.type = cmd - 1;
        d.delConfirm = false;
    } else if (cmd >= 5 && cmd < 7) {
        d.scope = cmd - 5;
        d.delConfirm = false;
    } else if (cmd == 7) {
        XjsMdlgClose();
    } else if (cmd == 8) {
        XjsMdlgSave(g_hWnd);
    } else if (cmd == 9) {
        if (!d.delConfirm) {
            d.delConfirm = true;   /* 首击: 变红"确定删除" (源样式 toggleModeDel) */
        } else {
            for (size_t i = 0; i < g_customModes.size(); i++) {
                if (g_customModes[i].id == d.editId) { g_customModes.erase(g_customModes.begin() + i); break; }
            }
            XjsSaveConfig();
            XjsHostedPurgeMode(d.editId);   /* 标签链剔除该模式来源并重搜 (源样式 removeCustomMode → purgeHostedSources) */
            XjsMdlgClose();
            XjsToastShow(g_hWnd, XjsT(L"模式对话框.已删除"), XTOAST_SUCCESS, XSF(1));
        }
    }
    XjsSearchWindow::Cur()->Invalidate();
    return true;
}

bool XjsModeDlgKey(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    XjsModeDlg& d = g_modeDlg;
    if (!d.open) return false;
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_ESCAPE) { XjsMdlgClose(); XjsSearchWindow::Cur()->Invalidate(); return true; }
        if (wParam == VK_RETURN) {
            /* 多行模板字段聚焦时 Enter=换行 (落 XjsEditRouteMsg 插入), Ctrl+Enter / 其余字段=保存 */
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            XjsEditField* fe = XjsEditFocused(hwnd);
            if (!ctrl && fe && fe->ed.multiline) {
                /* 落到下方 XjsEditRouteMsg 走组件换行 */
            } else {
                XjsMdlgSave(hwnd);
                XjsSearchWindow::Cur()->Invalidate();
                return true;
            }
        }
    }
    if (msg == WM_IME_COMPOSITION) {
        /* IME 整串上屏 → 插入聚焦字段 (此前吞掉 = 中文打不进输入框, 组字窗还钉在搜索框) */
        XjsEditRouteImeResult(hwnd, lParam);
        XjsSearchWindow::Cur()->Invalidate();
        return true;
    }
    if (msg == WM_MOUSEMOVE) {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        XjsEditFieldMouseMove(hwnd, pt);   /* 字段拖选跟手 (MouseDown 已 SetCapture, 拖出字段消息照来) */
        return true;
    }
    if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) {
        /* 多行模板字段滚轮: 优先聚焦字段, 否则悬停字段 (纵向滚内容, 横向滚轮=横滚); 其余照吞不动 */
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &pt);   /* WM_MOUSEWHEEL 的 lParam 是屏幕坐标 */
        XjsEditFieldWheel(hwnd, GET_WHEEL_DELTA_WPARAM(wParam), msg == WM_MOUSEHWHEEL, pt);
        XjsSearchWindow::Cur()->Invalidate();
        return true;
    }
    if (msg == WM_LBUTTONUP) {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        XjsEditFieldMouseUp(hwnd);          /* 字段拖选收尾+释放捕获 (无拖选=无操作) */
        XjsModeDlgMouseUp(pt);              /* 命令控件松开触发 (按下待定校验) */
        return true;
    }
    if (msg == WM_LBUTTONDBLCLK) {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        XjsEditFieldDoubleClick(hwnd, pt);   /* 字段内双击=选整词 */
        return true;
    }
    if (msg == WM_RBUTTONDOWN) {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        XjsEditFieldContextMenu(hwnd, pt);   /* 字段内右键=编辑菜单 (未中字段模态照吞) */
        return true;
    }
    if (XjsEditRouteMsg(hwnd, msg, wParam, lParam)) return true;
    return true;   /* 模态: 未聚焦字段按键也吞掉 (防触发搜索框/列表) */
}

void XjsModeDlgRender(XjsRt* rt, float w, float h) {
    XjsModeDlg& d = g_modeDlg;
    if (!d.open || !rt) return;
    XjsModeDlgRects R;
    XjsMdlgLayout(w, h, &R);
    float pad = XSF(20);
    XjsColor mask = g_skin.bg1; mask.a *= 0.6f;
    rt->FillRectangle(XjsRectF(0, 0, w, h), XjsTempBrush(mask));
    rt->FillRoundedRectangle(XjsRoundedRectF(R.card, XSF(12), XSF(12)), g_br[XTH_PANEL]);
    rt->DrawRoundedRectangle(XjsRoundedRectF(R.card, XSF(12), XSF(12)), g_br[XTH_BORDER], 1.0f);
    /* 标题 */
    {
        const wchar_t* t = d.edit ? XjsT(L"模式对话框.编辑标题") : XjsT(L"模式对话框.添加标题");
        rt->DrawText(t, (UINT32)wcslen(t), g_tfTitle,
            XjsRectF(R.card.left + pad, R.card.top + XSF(14), R.card.right - pad, R.card.top + XSF(44)),
            g_br[XTH_TEXT_DIM]);
    }
    /* 字段行 (label + 底框 + 字段)。label/提示 用左对齐小字号格式 —
       g_tfTip 是居中+不换行, 长文案会两侧溢出、label 会居中 (首版翻车点) */
    auto label = [&](const wchar_t* s, float x0, float y0) {
        rt->DrawText(s, (UINT32)wcslen(s), g_tfTiny, XjsRectF(x0, y0, R.card.right - pad, y0 + XSF(16)), g_br[XTH_TEXT_DIM]);
    };
    auto fieldRow = [&](const wchar_t* lab, const XjsRect& f, XjsEditField& ed) {
        label(lab, R.card.left + pad, f.top - XSF(22));
        rt->FillRoundedRectangle(XjsRoundedRectF(f, XSF(8), XSF(8)), g_br[XTH_PANEL2]);
        rt->DrawRoundedRectangle(XjsRoundedRectF(f, XSF(8), XSF(8)),
            ed.focused ? (XjsBrush*)g_br[XTH_ACCENT] : g_br[XTH_BORDER], 1.0f);
        ed.Render(rt, XjsRectF(f.left + XSF(10), f.top, f.right - XSF(10), f.bottom),
            g_tfRow, g_br[XTH_TEXT], g_br[XTH_ACCENT_SOFT], g_br[XTH_ACCENT], NULL, NULL);
    };
    fieldRow(XjsT(L"模式对话框.模式名称"), R.name, d.nameEd);
    fieldRow(XjsT(L"模式对话框.简介"), R.desc, d.descEd);
    /* 类型段 */
    label(XjsT(L"模式对话框.类型"), R.card.left + pad, R.type[0].top - XSF(22));
    for (int i = 0; i < 4; i++) {
        bool act = d.type == i;
        rt->FillRoundedRectangle(XjsRoundedRectF(R.type[i], XSF(8), XSF(8)), act ? g_br[XTH_ACCENT_SOFT] : g_br[XTH_PANEL2]);
        rt->DrawRoundedRectangle(XjsRoundedRectF(R.type[i], XSF(8), XSF(8)), act ? g_br[XTH_ACCENT] : g_br[XTH_BORDER], 1.0f);
        const wchar_t* tn = XjsMdlgTypeName(i);
        float tw = XjsMeasureText(tn, g_tfTip);
        rt->DrawText(tn, (UINT32)wcslen(tn), g_tfTip,
            XjsRectF(R.type[i].left + ((R.type[i].right - R.type[i].left) - tw) / 2, R.type[i].top,
                        R.type[i].left + ((R.type[i].right - R.type[i].left) + tw) / 2, R.type[i].bottom),
            act ? g_br[XTH_ACCENT] : g_br[XTH_TEXT_DIM]);
    }
    /* 作用范围段 (样式同类型段) */
    label(XjsT(L"模式对话框.作用范围"), R.card.left + pad, R.scope[0].top - XSF(22));
    {
        const wchar_t* const SC[2] = { XjsT(L"模式对话框.全局共享"), XjsT(L"菜单.仅本窗口") };
        for (int i = 0; i < 2; i++) {
            bool act = d.scope == i;
            rt->FillRoundedRectangle(XjsRoundedRectF(R.scope[i], XSF(8), XSF(8)), act ? g_br[XTH_ACCENT_SOFT] : g_br[XTH_PANEL2]);
            rt->DrawRoundedRectangle(XjsRoundedRectF(R.scope[i], XSF(8), XSF(8)), act ? g_br[XTH_ACCENT] : g_br[XTH_BORDER], 1.0f);
            float tw = XjsMeasureText(SC[i], g_tfTip);
            rt->DrawText(SC[i], (UINT32)wcslen(SC[i]), g_tfTip,
                XjsRectF(R.scope[i].left + ((R.scope[i].right - R.scope[i].left) - tw) / 2, R.scope[i].top,
                            R.scope[i].left + ((R.scope[i].right - R.scope[i].left) + tw) / 2, R.scope[i].bottom),
                act ? g_br[XTH_ACCENT] : g_br[XTH_TEXT_DIM]);
        }
    }
    {
        /* 模板多行字段: 传滚动条画刷 (溢出才画, 样式同列表滚动条色) */
        const XjsRect f = R.tpl;
        label(XjsT(L"模式对话框.模板内容"), R.card.left + pad, f.top - XSF(22));
        rt->FillRoundedRectangle(XjsRoundedRectF(f, XSF(8), XSF(8)), g_br[XTH_PANEL2]);
        rt->DrawRoundedRectangle(XjsRoundedRectF(f, XSF(8), XSF(8)),
            d.tplEd.focused ? (XjsBrush*)g_br[XTH_ACCENT] : g_br[XTH_BORDER], 1.0f);
        d.tplEd.Render(rt, XjsRectF(f.left + XSF(10), f.top, f.right - XSF(10), f.bottom),
            g_tfRow, g_br[XTH_TEXT], g_br[XTH_ACCENT_SOFT], g_br[XTH_ACCENT], NULL, NULL, g_br[XTH_BORDER_STRONG]);
    }
    /* 提示三行 (左对齐小字, 收在卡片内): 类型语法提示 + 作用范围提示 + <keyword> 占位符提示 */
    {
        const wchar_t* tip = XjsMdlgTip(d.type);
        rt->DrawText(tip, (UINT32)wcslen(tip), g_tfTiny,
            XjsRectF(R.card.left + pad, R.tpl.bottom + XSF(6), R.card.right - pad, R.tpl.bottom + XSF(22)),
            g_br[XTH_TEXT_FAINT]);
        std::wstring scopeTip = (d.scope == 1)
            ? XjsFmt(XjsT(L"模式对话框.仅本窗口说明"), d.ownerName)
            : XjsT(L"模式对话框.全局共享说明");
        rt->DrawText(scopeTip.c_str(), (UINT32)scopeTip.length(), g_tfTiny,
            XjsRectF(R.card.left + pad, R.tpl.bottom + XSF(22), R.card.right - pad, R.tpl.bottom + XSF(38)),
            g_br[XTH_TEXT_FAINT]);
        const wchar_t* kwTip = XjsT(L"模式对话框.占位符说明");
        rt->DrawText(kwTip, (UINT32)wcslen(kwTip), g_tfTiny,
            XjsRectF(R.card.left + pad, R.tpl.bottom + XSF(38), R.card.right - pad, R.tpl.bottom + XSF(54)),
            g_br[XTH_TEXT_FAINT]);
    }
    /* 按钮行: 删除(编辑态) 左; 取消/确定 右 */
    auto btnText = [&](const XjsRect& b, const wchar_t* s, XjsBrush* br) {
        float tw = XjsMeasureText(s, g_tfTip);
        rt->DrawText(s, (UINT32)wcslen(s), g_tfTip,
            XjsRectF(b.left + ((b.right - b.left) - tw) / 2, b.top, b.left + ((b.right - b.left) + tw) / 2, b.bottom), br);
    };
    if (d.edit) {
        rt->DrawRoundedRectangle(XjsRoundedRectF(R.del, XSF(8), XSF(8)), XjsTempBrush(XjsCol(0xef4444)), 1.0f);
        btnText(R.del, d.delConfirm ? XjsT(L"模式对话框.确定删除") : XjsT(L"模式对话框.删除此模式"), XjsTempBrush(XjsCol(0xef4444)));
    }
    rt->FillRoundedRectangle(XjsRoundedRectF(R.ok, XSF(8), XSF(8)), g_br[XTH_ACCENT]);
    btnText(R.ok, d.edit ? XjsT(L"通用词.保存") : XjsT(L"通用词.添加"), XjsTempBrush(XjsCol(0xFFFFFF)));
    rt->DrawRoundedRectangle(XjsRoundedRectF(R.cancel, XSF(8), XSF(8)), g_br[XTH_BORDER], 1.0f);
    btnText(R.cancel, XjsT(L"通用词.取消"), g_br[XTH_TEXT_DIM]);
}
