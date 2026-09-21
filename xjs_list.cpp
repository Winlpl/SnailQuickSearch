/*
 * xjs_list.cpp — 结果列表: 表头/行(驱动器行+时间徽章+容量条)/滚动条/选择/框选/键盘
 */
#include "xjs_app.h"

/* ==================== 滚动 / 选择 ==================== */

/* ==================== 搜索防抖 (重绘取数) ====================
 * 提交搜索后引擎异步重建结果数组, 过渡期内 idx→ID 漂移 + 行数跳动 = 列表闪烁。
   提交前已快照"已显示行"ID、结果数与行选中态 (XjsDebounceSnapshot); 重绘时判断 g_searching,
   正在搜索一律用快照画 (布局与内容冻结在提交瞬间); 完成/失败回调清快照并解除标志 */
static bool XjsDebounceOn() {
    return g_searching.load() && !g_debounceIds.empty();
}
static int XjsPaintCount() {   /* 搜索中 = 快照行数 (冻结), 其余 = 实时结果数 */
    return XjsDebounceOn() ? g_debounceCount : g_resultCount;
}
static int XjsDebounceFileId(int idx) {   /* 快照区间外的行 (滚动越界) = -1 画空行 */
    int i = idx - g_debounceFirst;
    if (i < 0 || i >= (int)g_debounceIds.size()) return -1;
    return g_debounceIds[i];
}
static bool XjsDebounceSelected(int idx) {   /* 快照选中位 (与快照ID一一对应); 区间外 = 未选中 */
    int i = idx - g_debounceFirst;
    if (i < 0 || i >= (int)g_debounceSel.size()) return false;
    return g_debounceSel[i] != 0;
}
/* 渲染取选中唯一口径: 防抖冻结期引擎选中已被新查询清空, 随快照画; 其余走引擎事实源。
   仅绘制用 — 交互路径 (点击/键盘) 仍走 XjsSelIsSelected, 语义是操作真实结果对象 */
static bool XjsSelForPaint(int idx) {
    return XjsDebounceOn() ? XjsDebounceSelected(idx) : XjsSelIsSelected(idx);
}

bool XjsIsGridView() { return g_viewMode == VM_MEDIUM || g_viewMode == VM_LARGE; }

/* 网格每排格数: 列表可用宽 / 格子宽 (正式版: availW = clientWidth-36, 向下取整, ≥1) */
int XjsGridCols() {
    int itemW = XJS_GRID_ITEM_W[g_viewMode];
    if (itemW <= 0) return 1;
    float availW = g_layout.vtrack.left - XSF(12) - XSF(6);
    int cols = (int)(availW / XSF((float)itemW));
    return cols < 1 ? 1 : cols;
}

/* 行高 double 版: 449万行×行高≈1.5亿px, float 24位尾数在 >1677万 后量化 (ulp 2→8px),
   行 y 被吸附到 4/8px 网格 → 每隔若干行挤出一格周期性空槽 (滚动越深周期越短) */
static double XjsRowHd() { return (double)XJS_ROW_H[g_viewMode] * g_s * XjsUiZoom(); }

double XjsListContentHeight() {
    int rows = XjsPaintCount();
    if (XjsIsGridView()) {
        int cols = XjsGridCols();
        rows = cols > 0 ? (XjsPaintCount() + cols - 1) / cols : 0;
    }
    return (double)rows * XjsRowHd();
}
float XjsListViewHeight() { return g_layout.list.bottom - g_layout.list.top; }
double XjsMaxScroll() { double m = XjsListContentHeight() - (double)XjsListViewHeight(); return m > 0 ? m : 0; }

void XjsClampScroll() {
    double m = XjsMaxScroll();
    if (g_scrollTop > m) g_scrollTop = m;
    if (g_scrollTop < 0) g_scrollTop = 0;
}

void XjsScrollTo(double top) {
    /* 整行对齐: 滚轮/拖滚动条/翻页一律落到行高整数倍 (与滚轮同口径), 顶部不出现半截表项 */
    double rowH = XjsRowHd();
    if (rowH > 0) top = floor(top / rowH + 0.5) * rowH;
    g_scrollTop = top;
    XjsClampScroll();
    /* 滚动后同一行下标对应的文件已变: 拖尾高亮按行下标记忆, 不清会叠在不相关行上成重影 */
    if (!g_hoverTraces.empty()) g_hoverTraces.clear();
    XjsSearchWindow::Cur()->Invalidate();
}

void XjsEnsureVisible(int idx) {
    if (idx < 0 || g_resultCount <= 0) return;
    double rowHd = XjsRowHd();
    double y0 = (XjsIsGridView() ? (double)(idx / XjsGridCols()) : (double)idx) * rowHd;
    double y1 = y0 + rowHd;
    if (y0 < g_scrollTop) XjsScrollTo(y0);
    else if (y1 > g_scrollTop + XjsListViewHeight()) {
        /* 底边对齐时向上取整到整行: 目标行完整可见, 顶部也不出半截行 */
        XjsScrollTo(ceil((y1 - (double)XjsListViewHeight()) / rowHd) * rowHd);
    }
}

int XjsRowAtY(double yInList) {
    int count = XjsPaintCount();
    if (yInList < 0 || count <= 0) return -1;
    long long idx = (long long)(yInList / XjsRowHd());
    if (idx >= count) return -1;
    return (int)idx;
}

/* 命中项目索引: 列表按行 (整行命中), 网格按格子 (格间空隙不命中, 同正式版 grid-item 定宽) */
int XjsItemAtPoint(POINT pt) {
    double yInList = (double)pt.y - g_layout.list.top + g_scrollTop;
    if (!XjsIsGridView()) return XjsRowAtY(yInList);
    if (XjsPaintCount() <= 0 || yInList < 0) return -1;
    int cols = XjsGridCols();
    int itemW = XJS_GRID_ITEM_W[g_viewMode];
    long long row = (long long)(yInList / XjsRowHd());
    /* 负商必须 floor (截断会把列表左 12px 空白带命中成第 0 列表项) */
    int col = (int)floor((pt.x - g_layout.list.left - XSF(12)) / XSF((float)itemW));
    if (col < 0 || col >= cols) return -1;
    long long idx = row * cols + col;
    if (idx >= XjsPaintCount()) return -1;
    return (int)idx;
}

/* 当前视图的列集合 (详情 8 列 / 列表 9 列): 数量与显隐映射收在 XjsColumnSet 里 */
static XjsColumnSet& XjsColSet(bool listView) {
    return listView ? g_colsList : g_colsDetails;
}

/* 切换视图模式: 锚点 = 视口首项, 换算到新视图排索引保持内容位置不跳变 (同正式版 setViewMode) */
void XjsSetViewMode(int m) {
    if (m < 0 || m > VM_LARGE || m == (int)g_viewMode) return;
    int oldCols = XjsIsGridView() ? XjsGridCols() : 1;
    double oldRowH = XjsRowHd();
    int anchorItem = (int)(g_scrollTop / oldRowH) * oldCols;
    /* 排序态随结果对象, 跨视图无需重映射 (箭头按字段名对号, 字段跨视图同名) */
    g_viewMode = (XjsViewMode)m;
    int newCols = XjsIsGridView() ? XjsGridCols() : 1;
    double newRowH = XjsRowHd();
    int rows = XjsIsGridView() ? (newCols > 0 ? (g_resultCount + newCols - 1) / newCols : 0) : g_resultCount;
    int newOff = anchorItem / newCols;
    int maxOff = rows > 0 ? rows - 1 : 0;
    if (newOff > maxOff) newOff = maxOff;
    if (newOff < 0) newOff = 0;
    g_scrollTop = (double)newOff * newRowH;
    XjsClampScroll();
    XjsSaveConfig();
    XjsSearchWindow::Cur()->Invalidate();
}

/* Ctrl+滚轮: dir>0 向上滚=放大 (更大图标), dir<0 缩小, 两端停止不循环 */
void XjsCycleViewMode(int dir) {
    int m = (int)g_viewMode + dir;
    if (m < VM_LIST || m > VM_LARGE) return;
    XjsSetViewMode(m);
}

/* ==================== 可见列视图 ====================
 * 列显隐 (表头右键菜单) 后, 几何/渲染/命中一律走"可见列序列";
 * 排序态以结果对象为准 (GetSortField 按字段名), 显隐/换位/切视图都不影响它 */
struct XjsVisCols {
    XjsColSpec* c[XjsColumnSet::MAX];
    int n = 0;
    XjsColSpec* operator[](int i) { return c[i]; }
};
static XjsVisCols XjsVis(bool listView) {
    XjsColumnSet& S = XjsColSet(listView);
    XjsVisCols v;
    for (int i = 0; i < S.n; i++)
        if (S.arr[i].visible) v.c[v.n++] = &S.arr[i];
    return v;
}
/* 可见序 → 全数组下标 */
static int XjsVisFull(bool listView, int visIdx) {
    return XjsColSet(listView).VisibleAt(visIdx);
}

/* 列总宽 (含左右 padding): 超出视口 → 横向溢出 (同源样式 hOverflow/scrollMax) */
double XjsColumnsContentWidth(bool listView) {
    XjsVisCols V = XjsVis(listView);
    double sum = XSF(12) * 2;
    for (int i = 0; i < V.n; i++) sum += (double)XSF((float)V.c[i]->width);
    return sum;
}

/* g_hScroll 收敛: 溢出时 [0, 内容宽-视口宽], 未溢出归零 */
void XjsClampHScroll(bool listView) {
    double smax = XjsColumnsContentWidth(listView) - (double)g_layout.list.right;
    if (!listView || smax <= 0) { g_hScroll = 0; return; }
    if (g_hScroll > smax) g_hScroll = smax;
    if (g_hScroll < 0) g_hScroll = 0;
}

/* 列几何 (横向滚动语义, 同源样式 minmax/fr): 只排可见列;
   固定列恒按存储宽; 弹性列在 剩余空间≥其下限和 时按 fr 比例伸展,
   否则钉在下限 → 内容宽超出视口, 调用方 XjsClampHScroll 后经 g_hScroll 平移 */
void XjsGetColumnRects(float listWidth, bool listView, float* xs, float* ws) {
    XjsVisCols V = XjsVis(listView);
    XjsClampHScroll(listView);
    double pad = XSF(12);
    /* 垂直滚动条显示时右侧让位 (源样式 has-vscroll: 右 padding 20→34)。
       不让位则末列右缘伸进 vtrack 命中区 ~5px, 有纵向溢出时点行尾会误触发翻页 */
    double rightPad = pad;
    if (XjsListContentHeight() > (double)XjsListViewHeight()) rightPad += XSF(14);
    double avail = (double)listWidth - pad - rightPad;
    double fixedSum = 0, frSum = 0, flexNeed = 0;
    for (int i = 0; i < V.n; i++) {
        double w = (double)XSF((float)V.c[i]->width);
        if (V.c[i]->flex) { frSum += V.c[i]->fr; flexNeed += w; }
        else fixedSum += w;
    }
    double w[XjsColumnSet::MAX] = {0};
    double free = avail - fixedSum;
    for (int i = 0; i < V.n; i++) {
        if (V.c[i]->flex)
            w[i] = (free >= flexNeed && frSum > 0) ? free * V.c[i]->fr / frSum : (double)XSF((float)V.c[i]->width);
        else
            w[i] = (double)XSF((float)V.c[i]->width);
    }
    /* 垂直滚动条显示时右侧让位已在 avail 里扣减 (见函数头 rightPad) */
    double x = pad - g_hScroll;
    for (int i = 0; i < V.n; i++) {
        xs[i] = (float)x;
        ws[i] = (float)w[i];
        x += w[i];
    }
}

int XjsHitTestColHandle(POINT pt) {
    /* bottom 用 >=: 网格模式 listHead 是 top==bottom 的零高矩形, == 时会漏进来按详情列集调宽 */
    if (pt.y < g_layout.listHead.top || pt.y >= g_layout.listHead.bottom) return -1;
    bool listView = (g_viewMode == VM_LIST);
    XjsVisCols V = XjsVis(listView);
    float xs[XjsColumnSet::MAX] = {0}, ws[XjsColumnSet::MAX] = {0};
    XjsGetColumnRects(g_layout.list.right, listView, xs, ws);
    /* 全部边界都算 (含最后一列右缘): 手柄恒属边界左侧列, 拖拽/双击自适应按同一下标语义 */
    for (int i = 0; i < V.n; i++) {
        float bx = xs[i] + ws[i];
        if (pt.x >= bx - XSF(4) && pt.x <= bx + XSF(4)) return i;
    }
    return -1;
}

/* ==================== 选中 (权威在引擎 xjs_result_Select*, 宿主不存选中副本) ====================
 * 引擎选中按"文件ID"存, 与列表索引无关: 重排序/文件同步后仍精确定位, 磁盘文件被删由引擎
 * 自动清除; 新查询提交时引擎自行清空选中。宿主渲染/命令一律按需回读, 增删直接下发引擎 —
 * 宿主不维护任何选中集合 (旧 g_selSet 与框选预览区间均已删: 双份状态必然与引擎漂移)。 */

bool XjsSelIsSelected(int idx) {
    if (idx < 0 || !g_result) return false;
    /* 极值态短路: 引擎"是否选中"是对选中数组线性扫描 (SDK 文档: 绘制用), 全选 450 万后
       逐行查询 = 每行扫约 idx 个成员, 深滚动一帧几百 ms。0 = 全未选, 计数=结果数 = 全选态,
       两端直接定值, 只有部分选中才逐行查询 */
    int cnt = xjs_result_GetSelectedCount(g_result);
    if (cnt <= 0) return false;
    if (cnt >= g_resultCount) return true;
    return xjs_result_IsSelectedByIndex(g_result, idx) != FALSE;
}

int XjsSelCount() {
    return g_result ? xjs_result_GetSelectedCount(g_result) : 0;
}

/* 主选中项: 焦点项已选中即用它 (源样式 VL.selected 口径 — 点击/方向键/Shift 移动的都是它);
   焦点项未选中时回落"最小选中索引" (源样式 Ctrl+点选取消最后一项时焦点回退集合内最小项);
   全无选中回落焦点项 (预览仍跟随键盘移动)。
   取最小项走引擎复制首个选中ID 反查 (结果顺序=索引顺序), 不整集遍历 */
int XjsSelPrimaryIdx() {
    bool focusValid = (g_focusIdx >= 0 && g_focusIdx < g_resultCount);
    if (g_result) {
        if (focusValid && xjs_result_IsSelectedByIndex(g_result, g_focusIdx)) return g_focusIdx;
        /* 短路: 无选中直接回落焦点 (否则复制选中ID 会完整扫一遍结果数组) */
        if (xjs_result_GetSelectedCount(g_result) > 0) {
            int fid = -1;
            if (xjs_result_CopySelectedFileId(g_result, &fid, 1) == 1) {
                int idx = xjs_result_GetFileIdIndex(g_result, fid);
                if (idx >= 0) return idx;
            }
        }
    }
    return focusValid ? g_focusIdx : -1;
}

/* 全部选中行索引 (升序): 引擎复制选中ID → 反查索引 (同源样式宿主 选中_复制选中ID 口径, 命令用) */
std::vector<int> XjsSelIndices() {
    std::vector<int> out;
    if (!g_result) return out;
    int n = xjs_result_GetSelectedCount(g_result);
    if (n <= 0) return out;
    std::vector<int> ids((size_t)n);
    n = xjs_result_CopySelectedFileId(g_result, ids.data(), n);
    out.reserve((size_t)n);
    for (int i = 0; i < n; i++) {
        int idx = xjs_result_GetFileIdIndex(g_result, ids[i]);
        if (idx >= 0) out.push_back(idx);
    }
    return out;   /* 引擎按结果顺序复制 = 索引升序 */
}

void XjsSelClear() {
    if (g_result) xjs_result_SelectClear(g_result);
}

static void XjsSelAddIdx(int idx) {
    if (!g_result || idx < 0) return;
    int fid = xjs_result_GetFileId(g_result, idx);
    if (fid >= 0) xjs_result_SelectAdd(g_result, fid);
}

void XjsSelectOnly(int idx) {
    if (g_result) {
        xjs_result_SelectClear(g_result);
        XjsSelAddIdx(idx);
    }
    g_anchorIdx = idx;
    g_focusIdx = idx;
    XjsPreviewUpdateSelection();
    XjsSearchWindow::Cur()->Invalidate();
}

void XjsSelToggle(int idx) {   /* Ctrl+点击: 选中集合增删该行 */
    if (g_result && idx >= 0) {
        int fid = xjs_result_GetFileId(g_result, idx);
        if (fid >= 0) {
            if (xjs_result_IsSelected(g_result, fid)) xjs_result_SelectRemove(g_result, fid);
            else xjs_result_SelectAdd(g_result, fid);
        }
    }
    g_anchorIdx = idx;
    g_focusIdx = idx;
    XjsPreviewUpdateSelection();
    XjsSearchWindow::Cur()->Invalidate();
}

void XjsSelectRange(int from, int to) {
    if (from > to) { int t = from; from = to; to = t; }
    if (from < 0) from = 0;
    if (g_result) {
        xjs_result_SelectClear(g_result);
        if (to >= g_resultCount) to = g_resultCount - 1;
        /* 全区间 = 引擎整集标记; 其余逐行加 (大范围跨选与正式版同走逐ID通道) */
        if (from == 0 && to >= g_resultCount - 1) xjs_result_SelectAll(g_result);
        else for (int i = from; i <= to; i++) XjsSelAddIdx(i);
    }
    XjsPreviewUpdateSelection();
    XjsSearchWindow::Cur()->Invalidate();
}

void XjsSelectAllRows() {   /* 全选: 引擎整集标记 (一次写锁批量完成) */
    if (g_result) xjs_result_SelectAll(g_result);
    /* 焦点/锚点口径照源样式 selectAll: 未设置时落首项 */
    if (g_focusIdx < 0 && g_resultCount > 0) g_focusIdx = 0;
    if (g_anchorIdx < 0 && g_resultCount > 0) g_anchorIdx = g_focusIdx;
    XjsPreviewUpdateSelection();
    XjsSearchWindow::Cur()->Invalidate();
}

/* 矩形相交选中 (正式版 09-row-selection.js marqueeApply):
   列表/详情按 y 范围选整行 (数学求交, 滚出窗口的行不丢); 网格按格子二维相交;
   y0/y1=全局内容坐标, rx1/rx2=客户区 x (网格无横向滚动, 直接换算列) */
static long long XjsFloorIdx(double v, double d) { return (long long)floor(v / d); }   /* 负坐标须向下取整 */

static void XjsApplyMarquee(double y0, double y1, float rx1, float rx2) {
    int paintCount = XjsPaintCount();   /* 搜索中按冻结视图计 (与重绘一致) */
    if (paintCount <= 0) return;
    double rowHd = XjsRowHd();
    bool empty = false;
    std::vector<std::pair<int,int>> spans;   /* <首,尾> 闭区间 (网格按排拆多段) */
    if (XjsIsGridView()) {
        int cols = XjsGridCols();
        float itemW = XSF((float)XJS_GRID_ITEM_W[g_viewMode]);
        if (cols <= 0 || itemW <= 0) return;
        long long r1 = XjsFloorIdx(y0, rowHd), r2 = XjsFloorIdx(y1 - 0.001, rowHd);
        double gx0 = (double)rx1 - (double)g_layout.list.left - XSF(12);
        double gx1 = (double)rx2 - (double)g_layout.list.left - XSF(12);
        int c1 = (int)floor(gx0 / itemW), c2 = (int)floor((gx1 - 0.001) / itemW);
        if (r1 < 0) r1 = 0;
        if (c1 < 0) c1 = 0;
        if (c2 >= cols) c2 = cols - 1;
        if (r2 < 0 || c2 < 0 || r1 > r2 || c1 > c2) empty = true;
        else {
            for (long long r = r1; r <= r2; r++) {
                long long lo = r * cols + c1, hi = r * cols + c2;
                if (lo >= paintCount) break;
                if (hi >= paintCount) hi = paintCount - 1;
                spans.push_back({ (int)lo, (int)hi });
            }
            if (spans.empty()) empty = true;
        }
    } else {
        long long r1 = XjsFloorIdx(y0, rowHd);
        long long r2 = XjsFloorIdx(y1 - 0.001, rowHd);   /* 尾边贴行界时不含下一行 */
        if (r2 < 0 || r1 >= paintCount || r1 > r2) empty = true;
        else {
            if (r1 < 0) r1 = 0;
            if (r2 >= paintCount) r2 = paintCount - 1;
            spans.push_back({ (int)r1, (int)r2 });
        }
    }
    /* 框选意图直接下发引擎 (清空+逐行添加, 同源样式宿主 同步选中到引擎 的 选中_清空+选中_添加 口径)。
       曾试过差集增量下发: XjsSpanDiff 把"被新框完全覆盖的旧区间"整段误判为要移除, 每帧先删掉
       已选中行再只加新增段 = 引擎里只剩矩形尾部几行, 选中可视化整段失效 — 增量回退, 每帧重放
       (视口内行数量级, 配合下方局部失效与插件事件零成本化, 帧开销可忽略) */
    XjsSelClear();
    if (empty) {
        g_anchorIdx = g_focusIdx = -1;
    } else {
        /* 全覆盖 = 引擎整集标记 (避免逐行 SelectAdd); 其余区间逐行加 */
        if (spans.size() == 1 && spans[0].first == 0 && spans[0].second >= g_resultCount - 1) {
            if (g_result) xjs_result_SelectAll(g_result);
        } else {
            for (auto& sp : spans)
                for (int i = sp.first; i <= sp.second; i++) XjsSelAddIdx(i);
        }
    }
    XjsPreviewUpdateSelection();   /* 框选拖动中被时间戳节流 (XJS_MARQUEE_PV_MS), 松开补刷终态 */
    /* 整窗失效移除: 框选=高频路径, 失效区域由 mousemove 框选分支按旧∪新矩形局部计算 */
}

/* 框选拖动中 (武装且已过误触阈值): 预览跟随冻结闸 — 拖动每跨一行主选中项就变,
   预览同步重载 (图片解码/文本读盘) 会把框选拖成逐行卡顿, 解除时 (mouseup/兜底) 补刷新 */
bool XjsMarqueeMoved() {
    return g_marquee && g_marqueeMoved;
}

/* ==================== 渲染 ==================== */

/* 列拖动换位状态 (源样式 03-columns.js: 按下不动作, 位移超 6px 才算拖动;
   松开时拖动了=重排列, 没拖动=单击排序; ghost 跟随 + 目标列前 accent 插入条) */
static bool s_colMovePending = false;   // 按下待定 (拖动/单击二选一)
static bool s_colDragging = false;      // 已过阈值, 拖动中
static int  s_colMoveFrom = -1;         // 源列下标
static int  s_colDropTo = -1;           // 插入目标 (getDropIndex)
static float s_colMoveStartX = 0, s_colMoveStartY = 0, s_colMoveCurX = 0;
/* s_hoverHandle 已上收为每窗成员 hoverHandle (宏 g_hoverHandle): 纯悬停无鼠标捕获,
   静态全局会在多窗间残留上一窗的高亮边界 */

/* 源样式 getDropIndex: 首个中点在鼠标右侧的列 → 插到它前面; 全过 = n (插到末尾)。可见列序 */
static int XjsColDropIndex(float x) {
    bool listView = (g_viewMode == VM_LIST);
    XjsVisCols V = XjsVis(listView);
    float xs[XjsColumnSet::MAX] = {0}, ws[XjsColumnSet::MAX] = {0};
    XjsGetColumnRects(g_layout.list.right, listView, xs, ws);
    for (int i = 0; i < V.n; i++)
        if (x < xs[i] + ws[i] / 2) return i;
    return V.n;
}

/* 可见列重排: 可见序列内 from→to, 隐藏列保持原全数组槽位不动 (回填进可见槽位) */
static void XjsMoveColVis(bool listView, int fromV, int toV) {
    XjsColumnSet& S = XjsColSet(listView);
    int n = S.n;
    XjsColSpec vis[XjsColumnSet::MAX];
    int vn = 0;
    for (int i = 0; i < n; i++)
        if (S.arr[i].visible) vis[vn++] = S.arr[i];
    if (fromV == toV || fromV < 0 || toV < 0 || fromV >= vn || toV > vn) return;
    XjsColSpec tmp = vis[fromV];
    if (fromV < toV) {
        for (int j = fromV; j < toV - 1; j++) vis[j] = vis[j + 1];
        vis[toV - 1] = tmp;
    } else {
        for (int j = fromV; j > toV; j--) vis[j] = vis[j - 1];
        vis[toV] = tmp;
    }
    int vi = 0;
    for (int i = 0; i < n; i++)
        if (S.arr[i].visible) S.arr[i] = vis[vi++];
}

static void XjsRenderEmptyState();

/* 相对时间徽章 (正式版 time-ago: 底色随"新旧程度"淡化; 随列靠左, 不再居中) */
static void XjsDrawTimeBadge(const std::wstring& text, const XjsRect& cell, long long mtime) {
    float tw = XjsMeasureText(text.c_str(), g_tfTiny);
    float bw = tw + XSF(12), bh = XSF(15);
    float cellW = cell.right - cell.left;
    if (bw > cellW) bw = cellW;
    /* 行槽实际高度比 row.bottom 多 2px (调用方修剪), 中心按真实槽高取; CJK 字形在行框内
       天然偏上 ~1.5px, 文本绘制整体下移补正 */
    float x = cell.left;
    float y = (cell.top + cell.bottom + XSF(2)) / 2 - bh / 2;
    /* 新→旧: 底色 #664242 由较满到接近透明 */
    long long ageDays = 0;
    if (mtime > 0) {
        long long ftVal = mtime * 10000LL + 116444736000000000LL;
        FILETIME nowFt;
        GetSystemTimeAsFileTime(&nowFt);
        long long now = ((long long)nowFt.dwHighDateTime << 32) | nowFt.dwLowDateTime;
        ageDays = (now - ftVal) / 10000000LL / 86400;
        if (ageDays < 0) ageDays = 0;
    }
    float alpha = 1.0f - (float)ageDays / 90.0f;
    if (alpha < 0.12f) alpha = 0.12f;
    if (alpha > 1) alpha = 1;
    XjsColor badge = g_skin.timeBadge;
    badge.a *= alpha;
    g_rt->FillRoundedRectangle(XjsRoundedRectF(XjsRectF(x, y, x + bw, y + bh), XSF(8), XSF(8)), XjsTempBrush(badge));
    /* 文字在徽章内水平居中 (g_tfTiny 为左对齐, 按实测文本宽定位);
       徽章被窄列压缩时改从左缘起画, 超宽部分字符级截断补 "...";
       文字矩形=胶囊矩形 (实测 offset 0 时墨迹上下间隙 3/3 完美居中, 旧 +1.5 下移=偏低下沉 2px) */
    float tx = x + (bw - tw) / 2;
    if (tx < x + XSF(3)) tx = x + XSF(3);
    XjsDrawEllText(text, XjsRectF(tx, y, x + bw - XSF(4), y + bh), g_tfTiny, g_br[XTH_TEXT_DIM]);
}

/* 驱动器行容量条 (正式版 .row[data-drive]::after 胶囊: 占用色光晕 + 全程轨道 + 蓝→占用色渐变 + 圆头;
   磁盘信息没取到 (driveTotal=0) 时源样式删 data-drive, 整条不画)。
   胶囊高随视图档: 紧凑视图行仅 34px 高, 条高 2px (圆头/最小填充随之), 详情视图 3px */
static void XjsDrawDriveBar(const XjsRect& row, const XjsRowData& rd, bool listView) {
    if (rd.driveTotal <= 0) return;
    float barL = row.left + XSF(12), barR = row.right - XSF(12);
    float barH = XSF(listView ? 2.0f : 3.0f);   /* 胶囊高 (紧凑视图更细) */
    float cap = barH / 2;                       /* 圆头半径 = 半高 */
    float barB = row.bottom - XSF(4), barT = barB - barH;
    XjsColor driveColor = XjsDriveColor(rd.drivePercent);
    /* 源样式整条胶囊带 box-shadow 0 0 6px rgba(占用色,.25) — 未占用段因此有虚化光晕;
       本宿主 RT 的效果管线帧必废 (跨渲染域铁律), 用逐层外扩低透明胶囊近似
       (条仅 2~3px 高而光晕半径 6px, 透入轨道的内圈正是源样式光晕罩住轨道的观感) */
    for (int i = 3; i >= 1; i--) {
        XjsColor gc = driveColor; gc.a *= 0.09f - 0.03f * (i - 1);
        g_rt->FillRoundedRectangle(XjsRoundedRectF(XjsRectF(
            barL - XSF((float)i), barT - XSF((float)i), barR + XSF((float)i), barB + XSF((float)i)),
            cap + XSF((float)i), cap + XSF((float)i)), XjsTempBrush(gc));
    }
    XjsRect track = XjsRectF(barL, barT, barR, barB);
    g_rt->FillRoundedRectangle(XjsRoundedRectF(track, cap, cap),
        XjsTempBrush(g_skin.driveTrack));
    float fillW = xf_max((barR - barL) * rd.drivePercent / 100, barH * 2);
    XjsGradientStop gs[2] = { { 0.0f, XjsCol(0x60a5fa) }, { 1.0f, driveColor } };
    XjsGradBrush* br = NULL;
    g_rt->CreateLinearGradientBrush(XjsPoint2F(barL, 0), XjsPoint2F(barL + fillW, 0), gs, 2, &br);
    if (br) {
        /* 主体 + 圆头尾端 */
        XjsRect body = XjsRectF(barL, barT, barL + fillW, barB);
        g_rt->FillRoundedRectangle(XjsRoundedRectF(body, cap, cap), br);
        br->Release();
    }
}

/* 横向滚动条几何 (同源样式 htrack: 左起列表左缘, 右缩 3px(有预览)/5px(无), 底部 3px, 高 8px) */
static void XjsHTrackGeom(float* trackL, float* trackW, float* trackY, float* trackH) {
    XjsLayout& L = g_layout;
    *trackL = L.list.left;
    *trackW = (L.list.right - XSF(g_previewVisible ? 3.0f : 5.0f)) - *trackL;
    *trackH = XSF(8);
    *trackY = L.list.bottom - XSF(3) - *trackH;
}

/* 纵向滚动条 thumb 几何 (渲染/命中/拖拽三处同源; 最小长 24px)。
   返回 false = 内容不高于视口, 无 thumb。maxScroll 回带滚动范围给拖拽换算用 */
static bool XjsVThumbGeom(float* thumbY, float* thumbH, double* maxScroll) {
    double content = XjsListContentHeight(), view = (double)XjsListViewHeight();
    if (!(content > view && view > 0)) return false;
    float trackH = g_layout.vtrack.bottom - g_layout.vtrack.top;
    *thumbH = xf_max(XSF(24), (float)(view / content * trackH));
    *maxScroll = content - view;
    *thumbY = g_layout.vtrack.top + (float)((g_scrollTop / *maxScroll) * (double)(trackH - *thumbH));
    return true;
}

/* 横向滚动条 thumb 几何 (同上三处同源)。返回 false = 无横向溢出。
   smax 回带滚动范围 (拖拽换算用); 轨道矩形经 trackOut 回带给渲染 */
static bool XjsHThumbGeom(float* thumbX, double* thumbW, double* smax, XjsRect* trackOut) {
    XjsClampHScroll(g_viewMode == VM_LIST);
    *smax = XjsColumnsContentWidth(g_viewMode == VM_LIST) - (double)g_layout.list.right;
    if (*smax <= 0) return false;
    float trackL, trackW, trackY, trackH;
    XjsHTrackGeom(&trackL, &trackW, &trackY, &trackH);
    *trackOut = XjsRectF(trackL, trackY, trackL + trackW, trackY + trackH);
    /* 比例 = 视口宽/(视口宽+溢出量): 横向滚动的视口是"宽", 纵向版 (VThumbGeom) 才用高 */
    double v = (double)g_layout.list.right;
    *thumbW = xf_max(XSF(24), v / (v + *smax) * (double)trackW);
    *thumbX = trackL + (g_hScroll / *smax) * (double)(trackW - *thumbW);
    return true;
}

/* 渲染尾部: 滚动条(纵向+横向) + 框选矩形 (列表/网格共用) */
static void XjsRenderListTail() {
    XjsLayout& L = g_layout;
    float thumbY = 0, thumbH = 0;
    double maxScroll = 0;
    if (XjsVThumbGeom(&thumbY, &thumbH, &maxScroll)) {
        /* thumb 右缘贴 track 右缘 (距列表右缘 3px): 与横向滚动条右端对齐, 原先右缩 6 间距过大 */
        XjsRect th = XjsRectF(L.vtrack.right - XSF(8), thumbY, L.vtrack.right, thumbY + thumbH);
        g_rt->FillRoundedRectangle(XjsRoundedRectF(th, XSF(4), XSF(4)),
            g_dragScroll ? (XjsBrush*)g_br[XTH_TEXT_FAINT] : (XjsBrush*)g_br[XTH_BORDER_STRONG]);
    }
    /* 横向滚动条 (列总宽超出视口时, 同源样式 htrack; 详情视图 XjsClampHScroll 恒归零不滚动, 不渲染死条) */
    if (g_viewMode == VM_LIST) {
        float thumbX = 0;
        double thumbW = 0, smax = 0;
        XjsRect track;
        if (XjsHThumbGeom(&thumbX, &thumbW, &smax, &track)) {
            XjsRect th = XjsRectF(thumbX, track.top, (float)(thumbX + thumbW), track.bottom);
            g_rt->FillRoundedRectangle(XjsRoundedRectF(th, XSF(4), XSF(4)),
                g_dragHScroll ? (XjsBrush*)g_br[XTH_TEXT_FAINT] : (XjsBrush*)g_br[XTH_BORDER_STRONG]);
        }
    }
    if (g_marquee && g_marqueeMoved) {
        /* 正式版 .marquee: 1px accent 描边 + 淡底 + 2px 圆角; 填充比 accent-soft 加深一档.
           显示矩形夹回列表区 (内容坐标未裁剪, 向上拖出会侵入表头) */
        XjsColor fill = g_skin.accentSoft;
        fill.a *= 2.0f;
        if (fill.a < 0.30f) fill.a = 0.30f;
        if (fill.a > 0.55f) fill.a = 0.55f;
        XjsRect mr = g_marqueeRect;
        if (mr.top < L.list.top) mr.top = L.list.top;
        if (mr.bottom > L.list.bottom) mr.bottom = L.list.bottom;
        if (mr.bottom > mr.top) {
            XjsRoundedRect rr = XjsRoundedRectF(mr, XSF(2), XSF(2));
            g_rt->FillRoundedRectangle(rr, XjsTempBrush(fill));
            g_rt->DrawRoundedRectangle(rr, g_br[XTH_ACCENT], 1.0f);
        }
    }
}

/* 网格项名称: 居中最多两行, 超宽字符级折行 (正式版 grid-name: line-clamp 2 + word-break) */
static void XjsDrawGridName(const std::wstring& name, const XjsRect& r) {
    if (name.empty()) return;
    float maxW = r.right - r.left;
    if (XjsMeasureText(name.c_str(), g_tfChip) <= maxW) {
        XjsDrawTextC(name.c_str(), (UINT32)name.length(), g_tfChip, r, g_br[XTH_TEXT_DIM], D2D1_DRAW_TEXT_OPTIONS_CLIP);
        return;
    }
    int n = (int)name.size(), cut = n;
    float acc = 0;
    for (int i = 0; i < n; i++) {
        acc += XjsMeasureText(std::wstring(1, name[i]).c_str(), g_tfChip);
        if (acc > maxW) { cut = i; break; }
    }
    if (cut <= 0) cut = n;
    /* 截断点落在代理对中间 (emoji 等增补平面字符) 时回退一位整对归下行: 两行各持半对会被画成替换符 */
    if (cut > 1 && cut < n && name[cut] >= 0xDC00 && name[cut] <= 0xDFFF
        && name[cut - 1] >= 0xD800 && name[cut - 1] <= 0xDBFF)
        cut--;
    float half = r.top + (r.bottom - r.top) / 2;
    std::wstring l1 = name.substr(0, cut), l2 = name.substr(cut);
    XjsDrawTextC(l1.c_str(), (UINT32)l1.length(), g_tfChip,
        XjsRectF(r.left, r.top, r.right, half), g_br[XTH_TEXT_DIM], D2D1_DRAW_TEXT_OPTIONS_CLIP);
    if (!l2.empty())
        XjsDrawTextC(l2.c_str(), (UINT32)l2.length(), g_tfChip,
            XjsRectF(r.left, half, r.right, r.bottom), g_br[XTH_TEXT_DIM], D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

/* 网格模式 (中图标/大图标): 无表头, 定宽格子 图标居上+名称两行居中 (正式版 grid-item) */
static void XjsRenderGrid() {
    XjsLayout& L = g_layout;
    bool deb = XjsDebounceOn();
    int paintCount = XjsPaintCount();
    if (paintCount <= 0) { XjsRenderEmptyState(); return; }
    double rowHd = XjsRowHd();
    int cols = XjsGridCols();
    float itemW = XSF((float)XJS_GRID_ITEM_W[g_viewMode]);
    float rowH = (float)rowHd;
    int totalRows = cols > 0 ? (paintCount + cols - 1) / cols : 0;
    long long firstRow = (long long)(g_scrollTop / rowHd);
    long long lastRow = (long long)((g_scrollTop + XjsListViewHeight()) / rowHd);
    if (lastRow >= totalRows) lastRow = totalRows - 1;
    /* 可见区间回写: ICON_ASK 闸门据此放行"正在显示的表项" (图标线程只读) */
    g_visFirst = (int)(firstRow * cols);
    long long vLast = (lastRow + 1) * cols - 1;
    if (vLast > paintCount - 1) vLast = paintCount - 1;
    g_visLast = (int)vLast;
    float boxH = XSF(g_viewMode == VM_LARGE ? 80.0f : 56.0f);   /* 图标盒 (medium 56 / large 80) */
    float iconSize = XSF((float)XJS_ICON_PX[g_viewMode]);
    g_rt->PushAxisAlignedClip(L.list, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (long long row = firstRow; row <= lastRow; row++) {
        for (int col = 0; col < cols; col++) {
            int idx = (int)(row * cols + col);
            if (idx >= paintCount) break;
            float x = L.list.left + XSF(12) + col * itemW;
            float y = (float)((double)L.list.top + (double)row * rowHd - g_scrollTop);
            XjsRect item = XjsRectF(x, y, x + itemW, y + rowH);
            bool sel = XjsSelForPaint(idx);
            if (sel) {
                g_rt->FillRoundedRectangle(XjsRoundedRectF(item, XSF(10), XSF(10)), g_br[XTH_ACCENT_SOFT]);
                g_rt->DrawRoundedRectangle(XjsRoundedRectF(item, XSF(10), XSF(10)), g_br[XTH_ACCENT], 1.0f);
            } else {
                float ha = XjsListHoverAlpha(idx);   /* 悬停渐隐拖尾: 停留行满亮, 移走行按剩余时长衰减 */
                if (ha > 0) {
                    XjsColor hc = g_skin.rowHover; hc.a *= ha;
                    g_rt->FillRoundedRectangle(XjsRoundedRectF(item, XSF(10), XSF(10)), XjsTempBrush(hc));
                }
            }
            /* 防抖中 idx→ID 走快照 (不读过渡态结果数组) */
            XjsRowData* rd = deb ? XjsEnsureRowData(idx, XjsDebounceFileId(idx)) : XjsEnsureRowData(idx);
            if (!rd) continue;
            /* 图标盒居中 (盒顶 = item 顶 + 10) */
            XjsBitmap* icon = XjsGetRowIcon(idx, rd->fileId, XjsIconFetchPx(XJS_FETCH_ICON[g_viewMode]));
            if (icon) {
                float bx = x + (itemW - boxH) / 2, by = y + XSF(10);
                g_rt->DrawBitmap(icon, XjsRectF(bx + (boxH - iconSize) / 2, by + (boxH - iconSize) / 2,
                    bx + (boxH + iconSize) / 2, by + (boxH + iconSize) / 2));
                icon->Release();
            }
            /* 名称 (图标盒 + 间距 6, 到 item 底缘留 4); 重命名中不绘旧名 (输入框替换语义) */
            if (XjsRenameTargetIdx() != idx)
                XjsDrawGridName(rd->name, XjsRectF(x + XSF(6), y + XSF(10) + boxH + XSF(6),
                    x + itemW - XSF(6), y + rowH - XSF(4)));
            /* 剪切灰显 (源样式 data-cut 半透明) */
            if (g_cutSet.count(idx)) {
                XjsColor dim = g_skin.bg1;
                dim.a *= 0.55f;
                g_rt->FillRoundedRectangle(XjsRoundedRectF(item, XSF(10), XSF(10)), XjsTempBrush(dim));
            }
        }
    }
    g_rt->PopAxisAlignedClip();
}

/* ==================== 列绘制器 (XjsColSpec::draw) ====================
 * 行渲染数据驱动: 行循环只按可见列调 draw — 名称列的图标+文件名块就是它自己的绘制器,
 * 列隐藏/换位/增列都不再需要渲染侧特判 (藏"名称"后图标+文件名仍显示的根因即旧特判) */
void XjsColDrawName(const XjsRect& cell, XjsRowData* rd, int idx, bool listView) {
    /* 图标贴列左缘 (间距/宽度同源样式 .row padding 8 + .cell-main gap)。
       请求尺寸 = 显示逻辑档 × 缩放 (1:1 像素), 显示尺寸仍按逻辑档 × XSF */
    float iconSize = XSF((float)XJS_ICON_PX[g_viewMode]);
    XjsBitmap* icon = XjsGetRowIcon(idx, rd->fileId, XjsIconFetchPx(XJS_ICON_PX[g_viewMode]));
    float iconY = (cell.top + cell.bottom) / 2 - iconSize / 2;
    if (icon) {
        g_rt->DrawBitmap(icon, XjsRectF(cell.left, iconY, cell.left + iconSize, iconY + iconSize));
        icon->Release();
    }
    float nameX = cell.left + iconSize + (listView ? XSF(8) : XSF(11));
    float nameW = cell.right - XSF(8) - nameX;
    bool renHide = (XjsRenameTargetIdx() == idx);   /* 重命名中: 输入框替换名称文本, 行内旧名不再绘制 (否则旧名露出框外成重影) */
    if (listView) {
        if (renHide) return;
        XjsRect clip = XjsRectF(nameX, cell.top, nameX + nameW, cell.bottom);
        XjsDrawHlSegs(rd->nameSegs, nameX, clip, rd->isDrive ? g_tfRowBold : g_tfRow);
    } else {
        float mid = (cell.top + cell.bottom) / 2;
        /* 名称下移 3 / 路径上移 4: 收紧两行间距 (文本都顶对齐绘制, 各自让出半格空白) */
        XjsRect clip = XjsRectF(nameX, cell.top + XSF(3), nameX + nameW, mid + XSF(4));
        if (!renHide) XjsDrawHlSegs(rd->nameSegs, nameX, clip, rd->isDrive ? g_tfRowBold : g_tfRow);
        /* 驱动器行底部有容量条 (条顶≈行底-7), 副行抬高 8px 让位免得贴字 */
        float folderB = rd->isDrive ? cell.bottom - XSF(8) : cell.bottom;
        XjsDrawEllText(rd->folder, XjsRectF(nameX, mid - XSF(4), nameX + nameW, folderB), g_tfTiny, g_br[XTH_TEXT_FAINT]);
    }
}

void XjsColDrawAlias(const XjsRect& cell, XjsRowData* rd, int idx, bool listView) {
    XjsRect cr = XjsRectF(cell.left + XSF(8), cell.top, cell.right - XSF(8), cell.bottom);
    const std::wstring& atext = rd->hasAlias ? rd->alias : std::wstring(L"-");
    if (rd->hasAlias && !rd->aliasSegs.empty())
        XjsDrawHlSegs(rd->aliasSegs, cr.left, cr, g_tfDim);
    else
        XjsDrawEllText(atext, cr, g_tfDim,
            rd->hasAlias ? (XjsBrush*)g_br[XTH_TEXT_DIM] : (XjsBrush*)g_br[XTH_TEXT_FAINT]);
}

void XjsColDrawFolder(const XjsRect& cell, XjsRowData* rd, int idx, bool listView) {
    XjsRect cr = XjsRectF(cell.left + XSF(8), cell.top, cell.right - XSF(8), cell.bottom);
    XjsDrawHlSegs(rd->folderSegs, cr.left, cr, g_tfTiny);
    if (rd->folderSegs.empty()) XjsDrawEllText(rd->folder, cr, g_tfTiny, g_br[XTH_TEXT_FAINT]);
}

void XjsColDrawRating(const XjsRect& cell, XjsRowData* rd, int idx, bool listView) {
    XjsRect cr = XjsRectF(cell.left + XSF(8), cell.top, cell.right - XSF(8), cell.bottom);
    if (rd->isDrive) {
        /* 驱动器行: 评分列=已用容量 */
        std::wstring used = Utf8ToUtf16(xjs_util_FormatFileSize(rd->driveUsed));
        XjsDrawEllText(used, cr, g_tfRow, g_br[XTH_TEXT]);
    } else {
        wchar_t buf[16];
        _snwprintf(buf, 16, L"%d", rd->rating);
        XjsDrawEllText(buf, cr, g_tfRow, g_br[XTH_TEXT_DIM]);
    }
}

void XjsColDrawSize(const XjsRect& cell, XjsRowData* rd, int idx, bool listView) {
    XjsRect cr = XjsRectF(cell.left + XSF(8), cell.top, cell.right - XSF(8), cell.bottom);
    if (rd->isDrive) {
        /* 驱动器行: 大小列=已用 NN% (占用色) */
        std::wstring pct = XjsFmt(XjsT(L"列表.已用百分比"), XjsNumText(rd->drivePercent));
        XjsDrawEllText(pct, cr, g_tfRow, XjsTempBrush(XjsDriveColor(rd->drivePercent)));
    } else {
        std::wstring sizeW = Utf8ToUtf16(xjs_util_FormatFileSize(rd->size));
        XjsDrawEllText(sizeW, cr, g_tfRow, g_br[XTH_TEXT_DIM]);
    }
}

/* 时间列共通 (修改/创建/访问): 驱动器行与字段未开启 (ms=0) 显 '-', 文件行画相对时间徽章 */
static void XjsColDrawTimeCell(const XjsRect& cell, long long ms) {
    XjsRect cr = XjsRectF(cell.left + XSF(8), cell.top, cell.right - XSF(8), cell.bottom);
    if (ms <= 0) {
        g_rt->DrawText(L"-", 1, g_tfTiny,
            XjsRectF(cr.left, cell.top, cr.right + XSF(8), cell.bottom), g_br[XTH_TEXT_FAINT]);
    } else {
        XjsDrawTimeBadge(XjsTimeBadge(ms), cr, ms);
    }
}

void XjsColDrawTime(const XjsRect& cell, XjsRowData* rd, int idx, bool listView) {
    XjsColDrawTimeCell(cell, rd->mtime);
}

void XjsColDrawCtime(const XjsRect& cell, XjsRowData* rd, int idx, bool listView) {
    XjsColDrawTimeCell(cell, rd->ctime);
}

void XjsColDrawAtime(const XjsRect& cell, XjsRowData* rd, int idx, bool listView) {
    XjsColDrawTimeCell(cell, rd->atime);
}

void XjsColDrawAttrs(const XjsRect& cell, XjsRowData* rd, int idx, bool listView) {
    XjsRect cr = XjsRectF(cell.left + XSF(8), cell.top, cell.right - XSF(8), cell.bottom);
    std::wstring t = XjsAttrsText(rd->attrs);
    XjsDrawEllText(t, cr, g_tfDim, g_br[XTH_TEXT_DIM]);
}

void XjsListRender() {
    XjsLayout& L = g_layout;
    if (XjsIsGridView()) { XjsRenderGrid(); XjsRenderListTail(); XjsRenameRender(); return; }
    bool listView = (g_viewMode == VM_LIST);
    XjsClampHScroll(listView);
    XjsVisCols V = XjsVis(listView);
    float xs[XjsColumnSet::MAX] = {0}, ws[XjsColumnSet::MAX] = {0};
    XjsGetColumnRects(L.list.right, listView, xs, ws);
    /* ---- 表头 (裁剪: 窄窗下列宽溢出不侵入预览面板) ---- */
    g_rt->FillRectangle(XjsRectF(0, L.listHead.bottom - 1, L.list.right, L.listHead.bottom), g_br[XTH_BORDER]);
    g_rt->PushAxisAlignedClip(L.listHead, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    /* 排序态以结果对象为事实源 (GetSortField/GetSortway): 宿主不持影子状态,
       未就绪/无结果对象时无高亮无箭头 (此刻也无结果可排) */
    const char* sortFieldNow = g_result ? xjs_result_GetSortField(g_result) : NULL;
    BOOL sortAscNow = g_result ? xjs_result_GetSortway(g_result) : FALSE;
    for (int i = 0; i < V.n; i++) {
        XjsColSpec* col = V.c[i];
        XjsRect cr = XjsRectF(xs[i], L.listHead.top, xs[i] + ws[i], L.listHead.bottom);
        /* 列分隔细线: 通栏全高 (源样式 .col border-right rgba(127,127,127,.14)) —
           旧实现画成零宽退化矩形 D2D 不上屏, 等于没有线, 用户看不到从哪调宽。
           每列都画 (含最后一列右缘 —— 那里正是它的调宽手柄), 旧版 i<n-1 漏画最后一列 */
        {
            XjsSolidBrush* sb = XjsTempBrush(XjsCol(0x7f7f7f, 0.14f));
            if (sb) g_rt->FillRectangle(XjsRectF(cr.right, cr.top, cr.right + 1.0f, cr.bottom), sb);
            /* 悬停/拖宽中的调整手柄高亮条 (源样式 col-resize:hover 7px 背景) */
            if ((g_hoverHandle == i || (g_dragCol && g_dragColIdx == i)) && !s_colDragging) {
                XjsSolidBrush* hb = XjsTempBrush(XjsCol(0x7f7f7f, 0.25f));
                if (hb) g_rt->FillRoundedRectangle(XjsRoundedRectF(
                    XjsRectF(cr.right - XSF(3.5f), cr.top, cr.right + XSF(3.5f), cr.bottom), XSF(3), XSF(3)), hb);
            }
        }
        bool sorted = (sortFieldNow && !strcmp(col->sortField, sortFieldNow));
        XjsBrush* bc = sorted ? (XjsBrush*)g_br[XTH_ACCENT] : (XjsBrush*)g_br[XTH_TEXT_FAINT];
        if (s_colMovePending && s_colDragging && i == s_colMoveFrom) {
            XjsColor c0 = ((XjsSolidBrush*)bc)->GetColor();
            c0.a *= 0.45f;   /* 拖动中的源列淡化 (源样式 .col.dragging) */
            bc = XjsTempBrush(c0);
        }
        float tx1 = cr.right - XSF(8) - (sorted ? XSF(14) : 0);
        XjsRect tr = XjsRectF(cr.left + XSF(8), cr.top, xf_max(tx1, cr.left + XSF(8)), cr.bottom);
        XjsDrawEllText(XjsT(col->label), tr, col->right ? g_tfHeadR : g_tfHead, bc);
        if (sorted) {
            float ax = cr.right - XSF(14), ay = (cr.top + cr.bottom) / 2;
            if (sortAscNow) {
                g_rt->DrawLine(XjsPoint2F(ax, ay + XSF(2.5f)), XjsPoint2F(ax + XSF(4), ay - XSF(2.5f)), bc, 1.6f);
                g_rt->DrawLine(XjsPoint2F(ax + XSF(4), ay - XSF(2.5f)), XjsPoint2F(ax + XSF(8), ay + XSF(2.5f)), bc, 1.6f);
            } else {
                g_rt->DrawLine(XjsPoint2F(ax, ay - XSF(2.5f)), XjsPoint2F(ax + XSF(4), ay + XSF(2.5f)), bc, 1.6f);
                g_rt->DrawLine(XjsPoint2F(ax + XSF(4), ay + XSF(2.5f)), XjsPoint2F(ax + XSF(8), ay - XSF(2.5f)), bc, 1.6f);
            }
        }
    }
    /* 列拖动中: 目标列前 accent 插入条 (源样式 .drop-before::before) +
       ghost 列跟随鼠标 (源样式 .col-ghost: bg1 底/边框加亮/70% 不透明) */
    if (s_colMovePending && s_colDragging && s_colMoveFrom < V.n) {
        if (s_colDropTo != s_colMoveFrom && s_colDropTo < V.n) {
            float bx = xs[s_colDropTo] - XSF(4);
            g_rt->FillRoundedRectangle(XjsRoundedRectF(
                XjsRectF(bx, L.listHead.top + XSF(4), bx + XSF(3), L.listHead.bottom - XSF(4)),
                XSF(1.5f), XSF(1.5f)), g_br[XTH_ACCENT]);
        }
        float gx = xs[s_colMoveFrom] + (s_colMoveCurX - s_colMoveStartX);
        XjsRect gr = XjsRectF(gx, L.listHead.top, gx + ws[s_colMoveFrom], L.listHead.bottom);
        XjsColor bgc = g_skin.bg1;  bgc.a *= 0.7f;
        XjsColor bdc = g_skin.borderStrong; bdc.a *= 0.7f;
        XjsSolidBrush* bgb = XjsTempBrush(bgc);
        XjsSolidBrush* bdb = XjsTempBrush(bdc);
        if (bgb && bdb) {
            g_rt->FillRoundedRectangle(XjsRoundedRectF(gr, XSF(6), XSF(6)), bgb);
            g_rt->DrawRoundedRectangle(XjsRoundedRectF(gr, XSF(6), XSF(6)), bdb, 1.0f);
            XjsColor txc = g_skin.textFaint; txc.a *= 0.7f;
            XjsColSpec* mc = V.c[s_colMoveFrom];
            XjsRect tr2 = XjsRectF(gr.left + XSF(8), gr.top, xf_max(gr.right - XSF(8), gr.left + XSF(8)), gr.bottom);
            XjsDrawEllText(XjsT(mc->label), tr2, mc->right ? g_tfHeadR : g_tfHead, XjsTempBrush(txc));
        }
    }
    g_rt->PopAxisAlignedClip();
    bool deb = XjsDebounceOn();
    if (XjsPaintCount() <= 0) { XjsRenderEmptyState(); return; }
    double rowHd = XjsRowHd();
    float rowH = (float)rowHd;
    int first = XjsRowAtY(g_scrollTop);
    int last = XjsRowAtY(g_scrollTop + XjsListViewHeight());
    if (last < 0) last = XjsPaintCount() - 1;
    /* scrollTop 越界 (缩放下调/跨屏 DPI 降低改行高后未钳制的偏移) 时 XjsRowAtY 返回 -1:
       不钳则循环从 -1 跑到 count-1, 负下标先传进引擎, 且视口外每行照常取数 = 大结果集整帧卡死 */
    if (first < 0) first = 0;
    /* 可见区间回写: ICON_ASK 闸门据此放行"正在显示的表项" (图标线程只读) */
    g_visFirst = first;
    g_visLast = last;
    /* 行矩形随横向滚动整体平移 (同源样式整行随 scrollX 走):
       左缘=首列左-8, 右缘=末列右+8 — 未溢出时恰好 12..宽-12, 溢出时行=内容宽随滚动平移,
       选中/悬停底、选中条、驱动器容量条与列内容保持一体 */
    float rowL = xs[0] - XSF(8);
    float rowR = (V.n > 0 ? xs[V.n - 1] + ws[V.n - 1] : xs[0]) + XSF(8);
    g_rt->PushAxisAlignedClip(L.list, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    int nullRows = 0;
    for (int idx = first; idx <= last; idx++) {
        /* 防抖中 idx→ID 走快照 (不读过渡态结果数组); 快照区间外 = 空行 */
        XjsRowData* rd = deb ? XjsEnsureRowData(idx, XjsDebounceFileId(idx)) : XjsEnsureRowData(idx);
        /* 行 y 用 double 差值: (idx×行高 − 滚动位置) 在 1.5亿px 量级下 float 会量化出周期空槽 */
        float y = (float)((double)L.list.top + (double)idx * rowHd - g_scrollTop);
        XjsRect row = XjsRectF(rowL, y, rowR, y + rowH - XSF(2));
        bool sel = XjsSelForPaint(idx);
        if (sel) {
            g_brSelGrad->SetStartPoint(XjsPoint2F(row.left, y));
            g_brSelGrad->SetEndPoint(XjsPoint2F(row.right, y));
            g_rt->FillRoundedRectangle(XjsRoundedRectF(row, XSF(9), XSF(9)), g_brSelGrad);
            g_rt->DrawRoundedRectangle(XjsRoundedRectF(row, XSF(9), XSF(9)), g_br[XTH_ACCENT], 1.0f);
            XjsRect bar = XjsRectF(row.left, y + XSF(7), row.left + XSF(3), y + rowH - XSF(2) - XSF(7));
            g_brSelBar->SetStartPoint(XjsPoint2F(0, bar.top));
            g_brSelBar->SetEndPoint(XjsPoint2F(0, bar.bottom));
            g_rt->FillRoundedRectangle(XjsRoundedRectF(bar, XSF(1.5f), XSF(1.5f)), g_brSelBar);
        } else {
            float ha = XjsListHoverAlpha(idx);
            if (ha > 0) {
                XjsColor hc = g_skin.rowHover; hc.a *= ha;
                g_rt->FillRoundedRectangle(XjsRoundedRectF(row, XSF(9), XSF(9)), XjsTempBrush(hc));
            }
        }
        if (!rd) { nullRows++; continue; }
        /* 可见列数据驱动绘制 (名称列的图标+文件名块也是普通列绘制器; 列隐藏 = 整列不画) */
        for (int vi = 0; vi < V.n; vi++) {
            XjsColSpec* col = V.c[vi];
            if (col->draw)
                col->draw(XjsRectF(xs[vi], row.top, xs[vi] + ws[vi], row.bottom), rd, idx, listView);
        }
        if (rd->isDrive && g_driveProgress) XjsDrawDriveBar(row, *rd, listView);
        /* 剪切灰显 (源样式 data-cut 半透明): 内容之上罩一层底色 */
        if (g_cutSet.count(idx)) {
            XjsColor dim = g_skin.bg1;
            dim.a *= 0.55f;
            g_rt->FillRoundedRectangle(XjsRoundedRectF(row, XSF(9), XSF(9)), XjsTempBrush(dim));
        }
    }
    g_rt->PopAxisAlignedClip();
    /* 行数据瞬时取不到 (引擎同步/排序中): 择机整帧重试; 空行数不变则停, 收敛不刷屏。
       记忆值每窗私有 (曾进程级 static: 双窗同处空行态互相抑制对方的重绘) */
    {
        int& lastNull = XjsSearchWindow::Cur()->rowNullLast;
        if (nullRows > 0) {
            if (nullRows != lastNull) {
                lastNull = nullRows;
                XjsSearchWindow::Cur()->Invalidate();
            }
        } else {
            lastNull = -1;
        }
    }
    XjsRenderListTail();
    XjsRenameRender();
}

/* ==================== 列显隐菜单 / 自适应列宽 ==================== */

void XjsShowColumnMenu(POINT screenPt) {
    bool listView = (g_viewMode == VM_LIST);
    XjsColumnSet& S = XjsColSet(listView);
    int n = S.n;
    int visCount = 0;
    for (int i = 0; i < n; i++) if (S.arr[i].visible) visCount++;
    std::vector<XjsPopupItem> items;
    items.push_back({ 0, XjsT(L"列.列显示菜单标题"), L"", false, false, true, false });
    for (int i = 0; i < n; i++) {
        /* 数据库未开启的字段不可显示 (置灰, 同源样式"未开启字段置灰"); 名称列结构性恒显 */
        bool structural = (!strcmp(S.arr[i].sortField, "文件名") || !strcmp(S.arr[i].sortField, "文件夹"));
        bool fieldOn = structural || (g_engine && xjs_db_IsFieldEnabled(g_engine, S.arr[i].sortField));
        bool disable = !fieldOn || (S.arr[i].visible && visCount <= 1);
        items.push_back({ IDM_COL_BASE + i, XjsT(S.arr[i].label), L"", S.arr[i].visible, false, false, false, disable });
    }
    XjsShowPopupMenu(g_hWnd, screenPt, items, XSF(170));
}

void XjsColumnToggle(int fullIdx) {
    bool listView = (g_viewMode == VM_LIST);
    XjsColumnSet& S = XjsColSet(listView);
    int n = S.n;
    if (fullIdx < 0 || fullIdx >= n) return;
    int visCount = 0;
    for (int i = 0; i < n; i++) if (S.arr[i].visible) visCount++;
    if (S.arr[fullIdx].visible && visCount <= 1) return;   /* 源样式"至少保留一列" */
    S.arr[fullIdx].visible = !S.arr[fullIdx].visible;
    XjsClampHScroll(listView);
    XjsSaveConfig();
    XjsSearchWindow::Cur()->Invalidate();
}

/* 列内容文本 (自适应列宽测量用; 驱动器行同渲染口径) */
static std::wstring XjsColTextOf(const char* field, XjsRowData* rd) {
    if (!strcmp(field, "别名")) return rd->hasAlias ? rd->alias : std::wstring(L"-");
    if (!strcmp(field, "文件夹")) return rd->folder;
    if (!strcmp(field, "文件评分")) {
        if (rd->isDrive) return Utf8ToUtf16(xjs_util_FormatFileSize(rd->driveUsed));
        wchar_t buf[16];
        _snwprintf(buf, 16, L"%d", rd->rating);
        return buf;
    }
    if (!strcmp(field, "文件大小")) {
        if (rd->isDrive) return XjsFmt(XjsT(L"列表.已用百分比"), XjsNumText(rd->drivePercent));
        return Utf8ToUtf16(xjs_util_FormatFileSize(rd->size));
    }
    if (!strcmp(field, "修改时间")) {
        if (rd->isDrive) return L"-";
        return XjsTimeBadge(rd->mtime);
    }
    if (!strcmp(field, "创建时间")) {
        if (rd->isDrive) return L"-";
        return XjsTimeBadge(rd->ctime);
    }
    if (!strcmp(field, "访问时间")) {
        if (rd->isDrive) return L"-";
        return XjsTimeBadge(rd->atime);
    }
    if (!strcmp(field, "文件属性")) {
        if (rd->isDrive) return L"-";
        return XjsAttrsText(rd->attrs);
    }
    if (!strcmp(field, "文件名")) return rd->name;
    return L"";
}

/* 双击列宽手柄: 按可视区内容最大宽自适应 (50~800, 源样式 dblclick 同款), 该列转固定列 */
void XjsAutoFitColumn(int handleIdx) {
    bool listView = (g_viewMode == VM_LIST);
    XjsVisCols V = XjsVis(listView);
    if (handleIdx < 0 || handleIdx >= V.n || g_resultCount <= 0) return;
    XjsColSpec* col = V.c[handleIdx];
    XjsFormat* fmt = (!strcmp(col->sortField, "文件夹") || !strcmp(col->sortField, "修改时间")
        || !strcmp(col->sortField, "创建时间") || !strcmp(col->sortField, "访问时间"))
        ? g_tfTiny : (!strcmp(col->sortField, "别名") || !strcmp(col->sortField, "文件属性") ? g_tfDim : g_tfRow);
    float maxPx = XjsMeasureText(XjsT(col->label), g_tfHead) + XSF(24);
    int first = XjsRowAtY(g_scrollTop);
    int last = XjsRowAtY(g_scrollTop + XjsListViewHeight());
    if (last < 0) last = g_resultCount - 1;
    if (first < 0) first = 0;
    for (int idx = first; idx <= last && idx < g_resultCount; idx++) {
        XjsRowData* rd = XjsEnsureRowData(idx);
        if (!rd) continue;
        std::wstring t = XjsColTextOf(col->sortField, rd);
        if (t.empty()) continue;
        float w = XjsMeasureText(t.c_str(), fmt) + XSF(24);
        if (w > maxPx) maxPx = w;
    }
    /* 测量值=物理px(96钉死RT), 列宽存逻辑px → 除回缩放 */
    float k = g_s * XjsUiZoom();
    int logical = (int)(maxPx / (k > 0 ? k : 1) + 0.5f);
    col->width = ximax(50, ximin(800, logical));
    col->flex = false;
    XjsSaveConfig();
    XjsSearchWindow::Cur()->Invalidate();
}

/* ==================== 行内重命名几何 / 绘制转发 ==================== */

int XjsRenameTargetIdx();

XjsRect XjsRenameEditRect() {
    int idx = XjsRenameTargetIdx();
    if (idx < 0 || g_resultCount <= 0) return XjsRectF(0, 0, 0, 0);
    double rowHd = XjsRowHd();
    if (XjsIsGridView()) {
        int cols = XjsGridCols();
        float itemW = XSF((float)XJS_GRID_ITEM_W[g_viewMode]);
        float boxH = XSF(g_viewMode == VM_LARGE ? 80.0f : 56.0f);
        float x = g_layout.list.left + XSF(12) + (idx % cols) * itemW;
        float y = (float)((double)g_layout.list.top + (double)(idx / cols) * rowHd - g_scrollTop);
        return XjsRectF(x + XSF(6), y + XSF(10) + boxH + XSF(6), x + itemW - XSF(6), y + rowHd - XSF(4));
    }
    float xs[XjsColumnSet::MAX] = {0}, ws[XjsColumnSet::MAX] = {0};
    bool listView = (g_viewMode == VM_LIST);
    XjsGetColumnRects(g_layout.list.right, listView, xs, ws);
    /* 编辑框锚到名称列矩形; 名称列隐藏时锚到第一可见列 (改的仍是文件名) */
    XjsVisCols V = XjsVis(listView);
    if (V.n <= 0) return XjsRectF(0, 0, 0, 0);
    int nameVi = 0;
    for (int vi = 0; vi < V.n; vi++)
        if (!strcmp(V.c[vi]->sortField, "文件名")) { nameVi = vi; break; }
    float y = (float)((double)g_layout.list.top + (double)idx * rowHd - g_scrollTop);
    return XjsRectF(xs[nameVi] - XSF(8), y + XSF(2), xs[nameVi] + ws[nameVi] - XSF(4), y + rowHd - XSF(4));
}

/* 空状态 (文件夹+放大镜线稿 + 提示 + 索引进度条) */
static void XjsRenderEmptyState() {
    XjsLayout& L = g_layout;
    float cy = L.list.top + (L.list.bottom - L.list.top) * 0.38f;
    float cx = (L.list.right - L.list.left) / 2;
    XjsGradientStop rgs[2] = { { 0.0f, g_br[XTH_ACCENT_SOFT]->GetColor() }, { 1.0f, XjsCol(0, 0.f) } };
    XjsGradBrush* rb = NULL;
    g_rt->CreateRadialGradientBrush(XjsPoint2F(cx, cy), XjsPoint2F(0, 0), XSF(80), XSF(80), rgs, 2, &rb);
    if (rb) { g_rt->FillEllipse(XjsEllipseF(XjsPoint2F(cx, cy), XSF(80), XSF(80)), rb); rb->Release(); }
    XjsColor art = g_br[XTH_ACCENT]->GetColor();
    art.a *= 0.55f;
    XjsSolidBrush* ab = XjsTempBrush(art);
    if (ab) {
        float s9 = XSF(1.2f);
        float w9 = XSF(48), h9 = XSF(40);
        float fx = cx - w9 / 2, fy = cy - h9 / 2;
        g_rt->DrawRoundedRectangle(XjsRoundedRectF(XjsRectF(fx, fy + XSF(8), fx + w9, fy + h9), XSF(4), XSF(4)), ab, s9);
        g_rt->DrawLine(XjsPoint2F(fx + XSF(2), fy + XSF(8)), XjsPoint2F(fx + XSF(12), fy + XSF(8)), ab, s9);
        g_rt->DrawLine(XjsPoint2F(fx + XSF(12), fy + XSF(8)), XjsPoint2F(fx + XSF(16), fy + XSF(2)), ab, s9);
        g_rt->DrawLine(XjsPoint2F(fx + XSF(16), fy + XSF(2)), XjsPoint2F(fx + XSF(28), fy + XSF(2)), ab, s9);
        float mcx = fx + w9 * 0.62f, mcy = fy + h9 * 0.56f, mr = XSF(11);
        g_rt->DrawEllipse(XjsEllipseF(XjsPoint2F(mcx, mcy), mr, mr), ab, s9);
        float k = 0.7071f;
        g_rt->DrawLine(XjsPoint2F(mcx + mr * k, mcy + mr * k),
            XjsPoint2F(mcx + mr * k + XSF(9), mcy + mr * k + XSF(9)), ab, s9);
    }
    /* 空态文案 (源样式 search.html 空态框同款 zh.json Search 命名空间):
       扫描中优先接管 (源样式 state3: tip=正在扫描磁盘{盘符}… / sub=正在建立全盘索引，完成后即可搜索);
       有词无结果 = "未找到匹配的文件 / 换个关键词试试", 筛选分类非"全部"时提示分类
       (源样式 NoMatchSubFiltered 的分类名是高亮 span, 此处「」内联); 空词 = 欢迎语 */
    std::wstring tip, sub;
    if (g_isScanning) {
        tip = XjsFmt(XjsT(L"列表.空态正在扫描"), g_scanDrive);
        sub = XjsT(L"状态栏.正在建立索引");
    } else if (!XjsSearchGetText().empty()) {
        tip = XjsT(L"列表.空态未找到");
        if (g_filterSel > 0 && g_filterSel < (int)g_filters.size())
            sub = XjsFmt(XjsT(L"列表.空态分类无匹配"), g_filters[g_filterSel].name);
        else
            sub = XjsT(L"列表.空态换个关键词");
    } else {
        tip = (g_mode == XMODE_SQL || g_mode == XMODE_LUA) ? XjsT(L"列表.空态输入语句") : XjsT(L"列表.空态输入关键词");
        sub = XjsT(L"列表.空态欢迎语");
    }
    g_rt->DrawText(tip.c_str(), (UINT32)tip.length(), g_tfTip,
        XjsRectF(cx - XSF(200), cy + XSF(52), cx + XSF(200), cy + XSF(76)), g_br[XTH_TEXT_DIM]);
    g_rt->DrawText(sub.c_str(), (UINT32)sub.length(), g_tfChip,
        XjsRectF(cx - XSF(240), cy + XSF(78), cx + XSF(240), cy + XSF(98)), g_br[XTH_TEXT_FAINT]);
    if (g_isScanning && g_scanTotal > 0) {
        int percent = (int)((double)g_scanEnumerated * 100 / g_scanTotal);
        if (percent > 100) percent = 100;
        /* 进度文本 = 源样式 indexProgressText "{盘符} · {p}%" */
        std::wstring info = g_scanDrive + L" · " + XjsNumText(percent) + L"%";
        g_rt->DrawText(info.c_str(), (UINT32)info.length(), g_tfChip,
            XjsRectF(cx - XSF(130), cy + XSF(110), cx + XSF(130), cy + XSF(128)), g_br[XTH_TEXT_FAINT]);
        XjsRect track = XjsRectF(cx - XSF(130), cy + XSF(132), cx + XSF(130), cy + XSF(136));
        g_rt->FillRoundedRectangle(XjsRoundedRectF(track, XSF(2), XSF(2)), g_br[XTH_BORDER]);
        if (percent > 0) {
            XjsRect bar = XjsRectF(track.left, track.top, track.left + (track.right - track.left) * percent / 100, track.bottom);
            g_brProgress->SetStartPoint(XjsPoint2F(track.left, 0));
            g_brProgress->SetEndPoint(XjsPoint2F(track.right, 0));
            g_rt->FillRoundedRectangle(XjsRoundedRectF(bar, XSF(2), XSF(2)), g_brProgress);
        }
    }
}

/* ==================== 鼠标 / 键盘 ==================== */

/* 按住已选行拖动 = OLE 拖出文件 (源样式 09-row-selection: >5px 触发);
   已选行单击不立即收拢单选 — 松开没拖动才收拢 (资源管理器同款, 否则多选拖不出) */
static bool s_dragFiles = false;
static int s_clickPendIdx = -1;
static float s_dragStartX = 0, s_dragStartY = 0;
static float s_dragColRem = 0;   /* 列宽拖动的逻辑px小数余量 (物理位移除回缩放后逐move并入, 鼠标瞬态) */

/* 滚动条轨道手势状态 (鼠标瞬态): 按住轨道空白 = 按步连翻, 期间移动鼠标不拖 thumb —
   拖动只认直接抓 thumb, 否则抓取点漂移 = 长按中稍动鼠标就跳位 (2026-09-22 实锤) */
static int s_trackDir = 0;           /* 0=非轨道手势; ±1 = 连翻方向 (纵: 下/上, 横: 右/左) */
static POINT s_trackPt = { 0, 0 };   /* 按下点位: thumb 行进到达即停的判定基准 */

/* 轨道手势收尾 (连翻到位/异常): 清方向与拖拽标志, 后续移动/松开不再进拖拽分支 */
static void XjsTrackGestureEnd() {
    s_trackDir = 0;
    g_dragScroll = false;
    g_dragHScroll = false;
}

/* 滚动条按下 (纵+横): thumb 抓取=拖拽; 轨道空白=向点击侧翻一页 + 按住连发 (ID_TIMER_SBTRACK,
   400ms 首延后 150ms/步, thumb 行进到指针下方即停)。轨道手势期内移动鼠标不拖 thumb。
   双击的第二下按下以 WM_LBUTTONDBLCLK 到达 (不走 WM_LBUTTONDOWN), 主窗双击分支也调这里,
   否则滚动条上双击落进"双击打开文件" (2026-09-22 实锤) */
bool XjsListScrollMouseDown(POINT pt) {
    /* 纵向 */
    if (XjsPtIn(g_layout.vtrack, pt)) {
        float thumbY = 0, thumbH = 0;
        double maxScroll = 0;
        if (XjsVThumbGeom(&thumbY, &thumbH, &maxScroll)) {
            if (pt.y >= thumbY && pt.y <= thumbY + thumbH) {
                g_scrollGrab = (float)pt.y - thumbY;
            } else {
                /* 轨道空白: 翻一页起步, 按住连发 (步长 = 一屏, 不是滚动范围); 不设抓取点 = 移动不拖拽 */
                s_trackDir = pt.y < thumbY ? -1 : 1;
                XjsScrollTo(g_scrollTop + s_trackDir * (double)XjsListViewHeight());
                s_trackPt = pt;
                SetTimer(g_hWnd, ID_TIMER_SBTRACK, 400, NULL);
            }
            g_dragScroll = true;
            SetCapture(g_hWnd);
            return true;
        }
    }
    /* 横向滚动条 (同渲染口径: 仅列表视图; 详情视图 ClampHScroll 恒归零, 命中了也是死拖) */
    if (g_viewMode == VM_LIST) {
        float thumbX = 0;
        double thumbW = 0, smax = 0;
        XjsRect track;
        if (XjsHThumbGeom(&thumbX, &thumbW, &smax, &track) &&
            pt.x >= track.left && pt.x <= track.right &&
            pt.y >= track.top - XSF(2) && pt.y <= track.bottom + XSF(2)) {
            if ((double)pt.x >= thumbX && (double)pt.x <= thumbX + thumbW) {
                g_hScrollGrab = (double)pt.x - thumbX;
            } else {
                s_trackDir = (double)pt.x < thumbX ? -1 : 1;
                g_hScroll += s_trackDir * (double)g_layout.list.right;   /* 一屏宽 (与 smax 口径一致) */
                XjsClampHScroll(g_viewMode == VM_LIST);
                s_trackPt = pt;
                SetTimer(g_hWnd, ID_TIMER_SBTRACK, 400, NULL);
                XjsSearchWindow::Cur()->Invalidate();
            }
            g_dragHScroll = true;
            SetCapture(g_hWnd);
            return true;
        }
    }
    return false;
}

/* 滚动条区域命中 (纵+横; 供外部判定"这点属于滚动条不是列表行"): 单击打开待定的武装要排除它 —
   XjsItemAtPoint 详情视图只看 y, 滚动条上点按松开 ≤5px 会误开该行文件 (2026-09-22 实锤) */
bool XjsListScrollRegionHit(POINT pt) {
    if (XjsPtIn(g_layout.vtrack, pt)) {
        float thumbY = 0, thumbH = 0;
        double maxScroll = 0;
        if (XjsVThumbGeom(&thumbY, &thumbH, &maxScroll)) return true;
    }
    if (g_viewMode == VM_LIST) {
        float thumbX = 0;
        double thumbW = 0, smax = 0;
        XjsRect track;
        if (XjsHThumbGeom(&thumbX, &thumbW, &smax, &track) &&
            pt.x >= track.left && pt.x <= track.right &&
            pt.y >= track.top - XSF(2) && pt.y <= track.bottom + XSF(2))
            return true;
    }
    return false;
}

/* WM_TIMER(ID_TIMER_SBTRACK): 轨道按住连发翻页 (按下首发, 首延 400ms 后转 150ms/步)。
   thumb 行进到指针下方 = 到位即停 (标准滚动条口径); 返回 假 = 请 KillTimer */
bool XjsListTrackTick() {
    if (!s_trackDir) return false;
    /* 兜底: 左键已不在按下状态 (捕获被夺/消息丢失) = 停连发 (与 MouseMove 手势兜底同口径) */
    if (!(GetKeyState(VK_LBUTTON) & 0x8000)) { XjsTrackGestureEnd(); return false; }
    SetTimer(g_hWnd, ID_TIMER_SBTRACK, 150, NULL);
    if (g_dragScroll) {
        float thumbY = 0, thumbH = 0;
        double maxScroll = 0;
        if (!XjsVThumbGeom(&thumbY, &thumbH, &maxScroll) ||
            (s_trackPt.y >= thumbY && s_trackPt.y <= thumbY + thumbH)) {
            XjsTrackGestureEnd();   /* 到位即停, 手势一并收 (后续移动不得拿旧抓取点拖拽) */
            return false;
        }
        XjsScrollTo(g_scrollTop + s_trackDir * (double)XjsListViewHeight());   /* 自带失效 */
    } else if (g_dragHScroll) {
        float thumbX = 0;
        double thumbW = 0, smax = 0;
        XjsRect track;
        if (!XjsHThumbGeom(&thumbX, &thumbW, &smax, &track) ||
            (s_trackPt.x >= thumbX && s_trackPt.x <= thumbX + thumbW)) {
            XjsTrackGestureEnd();
            return false;
        }
        g_hScroll += s_trackDir * (double)g_layout.list.right;
        XjsClampHScroll(g_viewMode == VM_LIST);
        XjsSearchWindow::Cur()->Invalidate();
    } else {
        XjsTrackGestureEnd();
        return false;
    }
    return true;
}

bool XjsListMouseDown(POINT pt, WPARAM flags) {
    if (pt.y < g_layout.listHead.top || pt.y >= g_layout.list.bottom) return false;
    SetFocus(g_hWnd);
    /* 列宽拖动 (手柄下标=可见列序) */
    int colHandle = XjsHitTestColHandle(pt);
    if (colHandle >= 0) {
        g_dragCol = true;
        g_dragColIdx = colHandle;
        g_dragColX = (float)pt.x;
        s_dragColRem = 0;
        SetCapture(g_hWnd);
        return true;
    }
    /* 表头: 记录待定列拖动 (源样式: 拖过 6px 阈值=换位, 松开没拖=单击排序) */
    if (!XjsIsGridView() && pt.y < g_layout.listHead.bottom) {
        bool listView = (g_viewMode == VM_LIST);
        XjsVisCols V = XjsVis(listView);
        float xs[XjsColumnSet::MAX] = {0}, ws[XjsColumnSet::MAX] = {0};
        XjsGetColumnRects(g_layout.list.right, listView, xs, ws);
        for (int i = 0; i < V.n; i++) {
            if (pt.x >= xs[i] && pt.x <= xs[i] + ws[i]) {
                s_colMovePending = true;
                s_colDragging = false;
                s_colMoveFrom = i;
                s_colDropTo = i;
                s_colMoveStartX = s_colMoveCurX = (float)pt.x;
                s_colMoveStartY = (float)pt.y;
                SetCapture(g_hWnd);
                return true;
            }
        }
        return true;
    }
    /* 滚动条: thumb 抓取 / 轨道翻页+按住连发 (双击的第二下也经此入口, 见函数注释) */
    if (XjsListScrollMouseDown(pt)) return true;
    /* 表头 (按下已在上面的待定块处理; 单击排序在松开时判定) */
    if (pt.y < g_layout.listHead.bottom) return true;
    /* 行内重命名编辑框: 点击定位光标/起拖选字, 不透传给行选择 */
    if (XjsRenameActive()) {
        if (XjsPtIn(XjsRenameEditRect(), pt)) { XjsRenameMouseDown(pt); SetCapture(g_hWnd); return true; }
        XjsRenameFinish(false);   /* 点编辑框外=失焦提交 (源样式口径) */
    }
    /* 行选择 / 框选 / 拖出 */
    double yInList = (double)pt.y - g_layout.list.top + g_scrollTop;
    int idx = XjsItemAtPoint(pt);
    s_dragFiles = false;
    s_clickPendIdx = -1;
    if (idx >= 0) {
        if (flags & MK_CONTROL) {
            XjsSelToggle(idx);
        } else if (flags & MK_SHIFT) {
            if (g_anchorIdx < 0) g_anchorIdx = idx;
            XjsSelectRange(g_anchorIdx, idx);
            g_focusIdx = idx;
            XjsPreviewUpdateSelection();
        } else if (XjsSelIsSelected(idx)) {
            /* 已选行: 不立即收拢 — 松开没拖动才收拢单选; 拖动=拖出文件 (源样式 dragOutCheck) */
            s_dragFiles = true;
            s_clickPendIdx = idx;
            s_dragStartX = (float)pt.x;
            s_dragStartY = (float)pt.y;
        } else {
            /* 未选中行: 不立即选中 (源样式 09-row-selection: 未选中项按住拖动=框选,
               单击不拖才在松开时选中该行 — s_clickPendIdx 复用作"松开选中"挂起) */
            s_clickPendIdx = idx;
        }
    } else {
        XjsSelClear();
        g_anchorIdx = g_focusIdx = -1;
        XjsPreviewUpdateSelection();
        XjsSearchWindow::Cur()->Invalidate();
    }
    /* 框选预备: 空白+未选中行按下即武装 (源样式口径, 资源管理器同款 —
       铺满视口的列表也能从任意未选中行起框选); 已选行按下留给拖出手势,
       Ctrl/Shift 各自接管不框选 */
    g_marquee = !s_dragFiles && !(flags & (MK_CONTROL | MK_SHIFT));
    g_marqueeMoved = false;
    g_marqueeStartX = (float)pt.x;
    g_marqueeStartY = yInList;
    g_marqueeRect = XjsRectF((float)pt.x, (float)pt.y, (float)pt.x, (float)pt.y);   /* 从鼠标处起, 拖动才展开 */
    SetCapture(g_hWnd);
    return true;
}

/* ==================== 悬停高亮渐隐拖尾 (源样式视觉: 移走的行逐帧衰减, 停留行满亮) ==================== */

static double XjsHoverNowMs() { return (double)GetTickCount64(); }

/* 悬停行变化 (chrome MouseMove / WM_MOUSELEAVE 调): 旧行高亮进拖尾并启动动画时钟 */
void XjsListHoverChanged(int oldRow, bool wasInList) {
    if (oldRow < 0 || !wasInList) return;
    if (!g_rowHover || !g_rowHoverFade) return;   /* 残影不可用: 不录拖尾不起动画表 (停留高亮由渲染闸管) */
    g_hoverTraces.push_back({ oldRow, XjsHoverNowMs() });
    if (g_hoverTraces.size() > 16) g_hoverTraces.erase(g_hoverTraces.begin());   /* 快速扫过时拖尾上限 */
    if (g_hWnd) SetTimer(g_hWnd, ID_TIMER_HOVERFADE, 30, NULL);
}

/* 该行当前悬停亮度 0..1: 停留行=满亮; 拖尾行按剩余时长线性衰减。
   透明度量化到 1/12 步进 — XjsTempBrush 按颜色值缓存, 连续 alpha 会撑爆缓存键 */
float XjsListHoverAlpha(int idx) {
    if (!g_rowHover) return 0;   /* 悬停高亮关 = 悬停系全灭 (残影依赖它, 一并失效, 设置里置灰) */
    if (idx == g_hoverRow && g_listHover) return 1.0f;
    if (!g_rowHoverFade) return 0;   /* 残影关 = 只保留停留行满亮, 无渐隐拖尾 */
    double now = XjsHoverNowMs();
    float best = 0;
    for (auto& t : g_hoverTraces) {
        if (t.row != idx) continue;
        float a = 1.0f - (float)((now - t.start) / 280.0);
        if (a > best) best = a;
    }
    if (best <= 0.03f) return 0;
    return ceilf(best * 12.0f) / 12.0f;
}

bool XjsListHoverTick() {
    double now = XjsHoverNowMs();
    for (size_t i = g_hoverTraces.size(); i-- > 0;) {
        if (now - g_hoverTraces[i].start >= 280.0) g_hoverTraces.erase(g_hoverTraces.begin() + i);
    }
    return !g_hoverTraces.empty();
}

bool XjsListMouseMove(POINT pt) {    /* 表头调整边界悬停高亮 (源样式 col-resize:hover 背景条); 变化才失效重画 */
    /* 兜底: 拖拽手势激活但左键已不在按下状态 (捕获被夺/消息丢失) = 解除手势 —
       与 s_dragFiles/g_marquee 分支内的同类兜底同口径, 否则松开后仍持续滚动/改列宽 */
    if (!(GetKeyState(VK_LBUTTON) & 0x8000) &&
        (g_dragScroll || g_dragHScroll || g_dragCol || s_colMovePending)) {
        g_dragScroll = false;
        g_dragHScroll = false;
        g_dragCol = false;
        s_colMovePending = false;
        s_colDragging = false;
        s_colMoveFrom = -1;
        s_trackDir = 0;                                   /* 轨道连发随手势一并解除 (残留=无按键仍持续翻页) */
        KillTimer(g_hWnd, ID_TIMER_SBTRACK);
        XjsSearchWindow::Cur()->Invalidate();
    }
    {
        int h = (pt.y >= g_layout.listHead.top && pt.y <= g_layout.listHead.bottom) ? XjsHitTestColHandle(pt) : -1;
        if (h != g_hoverHandle) { g_hoverHandle = h; XjsSearchWindow::Cur()->Invalidate(); }
    }
    /* 横向滚动条拖动 (轨道手势期不拖拽: 移动鼠标不换算, 只等连发/松开) */
    if (g_dragHScroll) {
        if (s_trackDir) return true;
        float thumbX = 0;
        double thumbW = 0, smax = 0;
        XjsRect track;
        if (XjsHThumbGeom(&thumbX, &thumbW, &smax, &track)) {
            double trackW = track.right - track.left;
            if (trackW - thumbW > 0) {
                g_hScroll = ((double)pt.x - g_hScrollGrab - track.left) / (trackW - thumbW) * smax;
                XjsClampHScroll(g_viewMode == VM_LIST);
                XjsSearchWindow::Cur()->Invalidate();
            }
        }
        return true;
    }
    /* 滚动条拖动 (轨道手势期不拖拽: 长按中稍动鼠标不得把 thumb 拽到指针处 = 跳位, 2026-09-22 实锤) */
    if (g_dragScroll) {
        if (s_trackDir) return true;
        float thumbY = 0, thumbH = 0;
        double maxScroll = 0;
        if (XjsVThumbGeom(&thumbY, &thumbH, &maxScroll)) {
            double denom = (double)(g_layout.vtrack.bottom - g_layout.vtrack.top) - thumbH;
            if (denom > 0) {
                /* 整行对齐 (与滚轮/翻页同口径, 走 XjsScrollTo): 首行永远完整显示, 不出半截行 */
                XjsScrollTo(((double)pt.y - g_scrollGrab - g_layout.vtrack.top) / denom * maxScroll);
            }
        }
        return true;
    }
    /* 列宽拖动 (边界改边界左侧那一列, 下标=可见列序; min 50, 拖过即接管为固定列) */
    if (g_dragCol) {
        bool listView = (g_viewMode == VM_LIST);
        XjsVisCols V = XjsVis(listView);
        /* 拖动边界改边界左侧那一列 (同源样式 resize 手柄属于左列, min 50, 拖过即接管为固定列)。
           列宽存逻辑px (XjsAutoFitColumn 同口径): 物理位移除回缩放, 否则高 DPI/缩放下手柄漂离光标;
           小数余量并入下一 move (逐 move 截断会丢慢拖步进) */
        int target = g_dragColIdx;
        if (target >= 0 && target < V.n) {
            float k = g_s * XjsUiZoom();
            float dLogical = (pt.x - g_dragColX) / (k > 0 ? k : 1) + s_dragColRem;
            int add = (int)floorf(dLogical);
            s_dragColRem = dLogical - add;
            V.c[target]->width = ximax(50, V.c[target]->width + add);
            V.c[target]->flex = false;   /* 用户拖宽即接管为固定列 (同源样式 flex 清空, 永久像素宽) */
        }
        g_dragColX = (float)pt.x;
        XjsSearchWindow::Cur()->Invalidate();
        return true;
    }
    /* 列拖动换位 (源样式 03-columns.js: 位移超 6px 启动, ghost 跟随鼠标, 松开重排) */
    if (s_colMovePending) {
        if (!s_colDragging &&
            (fabs((double)pt.x - (double)s_colMoveStartX) > (double)XSF(6) ||
             fabs((double)pt.y - (double)s_colMoveStartY) > (double)XSF(6)))
            s_colDragging = true;
        if (s_colDragging) {
            s_colMoveCurX = (float)pt.x;
            s_colDropTo = XjsColDropIndex((float)pt.x);
            XjsSearchWindow::Cur()->Invalidate();   /* ghost 跟手, 每移动都重画 */
        }
        return true;   /* 待定中也不放行下方交互 (按住不动时选区/框选不误触) */
    }
    /* 按住已选行拖动 >5px = OLE 拖出选中文件 (源样式 dragOut; 框选/滚动不被误触) */
    if (s_dragFiles) {
        /* 兜底: 左键已不在按下状态 (任何路径漏清) = 解除手势, 不吃这条移动 */
        if (!(GetKeyState(VK_LBUTTON) & 0x8000)) { s_dragFiles = false; s_clickPendIdx = -1; return false; }
        if (fabs((double)pt.x - (double)s_dragStartX) > (double)XSF(5) ||
            fabs((double)pt.y - (double)s_dragStartY) > (double)XSF(5)) {
            s_dragFiles = false;
            s_clickPendIdx = -1;
            ReleaseCapture();
            XjsDragOutSelected();   /* 模态 DoDragDrop, 返回即拖放结束 */
        }
        return true;
    }
    /* 框选 (正式版 marqueeApply: 二维橡皮筋, 矩形从鼠标按下处展开; <4px 视为误触不启动) */
    if (g_marquee) {
        /* 兜底: 左键已不在按下状态 = 手势已被外部打断, 解除武装 (点击收尾漏解除时,
           松开后一动就误框选; 选中已逐帧下发引擎, 无需在此补提交) */
        if (!(GetKeyState(VK_LBUTTON) & 0x8000)) {
            g_marquee = false;
            XjsPreviewUpdateSelection();   /* 拖动期预览被冻结, 解除即补刷新 */
            XjsSearchWindow::Cur()->Invalidate();
            return false;
        }
        double yInList = (double)pt.y - g_layout.list.top + g_scrollTop;
        if (fabs(yInList - g_marqueeStartY) > XSF(4) || fabs((double)pt.x - (double)g_marqueeStartX) > XSF(4))
            g_marqueeMoved = true;
        if (g_marqueeMoved) {
            XjsRect prevR = g_marqueeRect;   /* 更新前留旧矩形 (失效并集用) */
            double y0 = g_marqueeStartY < yInList ? g_marqueeStartY : yInList;
            double y1 = g_marqueeStartY > yInList ? g_marqueeStartY : yInList;
            float rx1 = pt.x < g_marqueeStartX ? (float)pt.x : g_marqueeStartX;
            float rx2 = pt.x > g_marqueeStartX ? (float)pt.x : g_marqueeStartX;
            /* 绘制矩形: x=客户区, y=内容坐标投回视口 (不随滚动漂移); 选择计算走内容坐标 */
            g_marqueeRect = XjsRectF(rx1,
                (float)((double)g_layout.list.top + (y0 - g_scrollTop)), rx2,
                (float)((double)g_layout.list.top + (y1 - g_scrollTop)));
            XjsApplyMarquee(y0, y1, rx1, rx2);   /* 选中增量下发引擎 (差集 Add/Remove) */
            /* 局部失效 (高频路径红线, 替代整窗 Invalidate): 失效矩形 = 列表全宽 × 旧∪新
               y 包围 — 框线/淡底在 marquee 矩形 y 内, 行选中变化 (新增/移除) 的高亮是整行,
               y 包围恰是新旧选中区间的投影, x 全宽保证行高亮两侧都重画; y 夹回列表区与
               渲染显示矩形同口径 (内容坐标未裁剪, 向上拖出会侵入表头) */
            float yTop = prevR.top < g_marqueeRect.top ? prevR.top : g_marqueeRect.top;
            float yBot = prevR.bottom > g_marqueeRect.bottom ? prevR.bottom : g_marqueeRect.bottom;
            if (yTop < g_layout.list.top) yTop = g_layout.list.top;
            if (yBot > g_layout.list.bottom) yBot = g_layout.list.bottom;
            if (yBot > yTop && g_hWnd) {
                RECT lr = { (LONG)g_layout.list.left, (LONG)yTop - 1,
                            (LONG)g_layout.list.right, (LONG)yBot + 2 };
                InvalidateRect(g_hWnd, &lr, FALSE);
            }
        }
        return true;
    }
    return false;
}

bool XjsListMouseUp(POINT pt) {
    (void)pt;
    if (g_dragHScroll) { g_dragHScroll = false; s_trackDir = 0; KillTimer(g_hWnd, ID_TIMER_SBTRACK); return true; }
    if (g_dragScroll) { g_dragScroll = false; s_trackDir = 0; KillTimer(g_hWnd, ID_TIMER_SBTRACK); return true; }
    if (g_dragCol) { g_dragCol = false; XjsSaveConfig(); return true; }
    /* 已选行单击收尾: 没拖动 = 收拢单选 (拖出路径已在 Move 里清标志) */
    if (s_dragFiles || s_clickPendIdx >= 0) {
        if (s_clickPendIdx >= 0 && !g_marqueeMoved) XjsSelectOnly(s_clickPendIdx);
        s_dragFiles = false;
        s_clickPendIdx = -1;
        g_marquee = false;   /* 未选中行按下也武装了框选, 单击收尾必须一并解除 (残留=松开后一动就误框选) */
        XjsPreviewUpdateSelection();   /* 实际拖成了框选时 (marqueeMoved) 拖动期预览被冻结, 补刷新;
                                          纯单击时 SelectOnly 已联动, 此处幂等直回 */
        return true;
    }
    /* 列拖动收尾: 拖动了=重排列(列宽/顺序/排序列随行), 没拖动=单击排序 (源样式同款) */
    if (s_colMovePending) {
        bool listView = (g_viewMode == VM_LIST);
        XjsColumnSet& S = XjsColSet(listView);
        if (s_colDragging) {
            if (s_colMoveFrom != s_colDropTo && s_colDropTo >= 0 && s_colDropTo <= XjsVis(listView).n) {
                XjsMoveColVis(listView, s_colMoveFrom, s_colDropTo);
                XjsSaveConfig();   /* 列顺序随宽度一并记忆 (源样式 saveColWidths); 排序态随结果对象, 无需重定位 */
            }
        } else if (s_colMoveFrom >= 0) {
            int fi = XjsVisFull(listView, s_colMoveFrom);
            if (fi >= 0 && g_result) {
                /* 排序态以结果对象为准: 同列再点=翻转方向, 换列=默认方向 (名称列升序, 其余降序, 原口径)。
                   按字段名判定名称列 — 列可拖动换位, 全数组槽 0 不保证仍是"文件名" */
                const char* field = S.arr[fi].sortField;
                const char* cur = xjs_result_GetSortField(g_result);
                BOOL asc = (cur && !strcmp(field, cur)) ? !xjs_result_GetSortway(g_result)
                                                        : (BOOL)(!strcmp(field, "文件名"));
                xjs_result_SetSortField(g_result, field, asc);
                XjsSearchNow(false);
                XjsSaveConfig();   /* 排序随窗口档案持久化 (每窗"默认排序", 重开/按档案重建恢复) */
            }
        }
        s_colMovePending = false;
        s_colDragging = false;
        s_colMoveFrom = -1;
        XjsSearchWindow::Cur()->Invalidate();
        return true;
    }
    if (g_marquee) {
        g_marquee = false;
        XjsPreviewUpdateSelection();   /* 拖动期预览被冻结, 松开即补刷新 (主项落在最终框选结果) */
        XjsSearchWindow::Cur()->Invalidate();
        return true;
    }
    return false;
}

void XjsListWheel(float delta) {
    XjsScrollTo(g_scrollTop - delta);
}

/* 系统属性对话框 (Alt+双击/Alt+Enter/右键"属性") */
void XjsShowProperties(int idx) {
    std::wstring p = XjsItemPath(idx);
    if (p.empty()) return;
    SHELLEXECUTEINFOW sei = {0};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_INVOKEIDLIST;
    sei.lpVerb = L"properties";
    sei.lpFile = p.c_str();
    sei.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&sei);
}

bool XjsListKey(WPARAM vk) {
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    int step = 0;
    switch (vk) {
        case VK_DOWN: step = 1; break;
        case VK_UP: step = -1; break;
        case VK_NEXT: step = 10; break;
        case VK_PRIOR: step = -10; break;
        case VK_HOME: step = -g_resultCount; break;
        case VK_END: step = g_resultCount; break;
        case 'A':
            if (ctrl) { XjsSelectAllRows(); return true; }
            return false;
        case 'C':
            if (ctrl) { XjsCopyFilesToClipboard({}, false); return true; }   /* 源样式: Ctrl+C=复制文件 (CF_HDROP) */
            return false;
        case 'X':
            if (ctrl) { XjsCutSelected(); return true; }                     /* 源样式: Ctrl+X=剪切 (粘贴进资源管理器=移动) */
            return false;
        case 'F':
            if (ctrl) { XjsSearchFocus(true); XjsSearchMenuCmd(3); return true; }   /* Ctrl+F 聚焦搜索框并全选 */
            return false;
        case VK_F2: {
            /* 重命名 (单选; 驱动器禁用) */
            int selCount = XjsSelCount();
            if (selCount == 1) {
                int idx = XjsSelPrimaryIdx();
                if (idx >= 0 && !XjsRenameStart(idx)) g_statusText = XjsT(L"状态栏.驱动器不可重命名");
                XjsSearchWindow::Cur()->Invalidate();
            } else if (selCount > 1) {
                /* 批量重命名 = 插件能力 (batchRename; 正式版即插件提供): 有活跃接管插件 → OnCommand,
                   插件自建窗口处理; 无插件保持现状提示 */
                if (XjsPluginBatchRenameAvailable()) {
                    std::vector<int> ids;   /* 文件上下文 = 选中集引擎 FileId (v3 口径, 不传路径) */
                    for (int i : XjsSelIndices()) {
                        if (g_result) { int fid = xjs_result_GetFileId(g_result, i); if (fid >= 0) ids.push_back(fid); }
                    }
                    XjsPluginBatchRename(ids, XjsPluginCurWindowToken());
                } else {
                    g_statusText = XjsT(L"状态栏.批量重命名未实现");
                }
                XjsSearchWindow::Cur()->Invalidate();
            }
            return true;
        }
        case VK_DELETE: XjsDeleteSelected(); return true;
        case VK_RETURN: {
            std::vector<int> sel = XjsSelIndices();
            if (sel.empty()) {
                /* 无选中 = 两段式第一段: 选中首项, 再按才打开 (与搜索框回车"选中首项并聚焦列表"同口径;
                   搜索在途时立即选会被结果集换血冲掉, 记 selFirstPending 待 WM_SEARCH_COMPLETE 落地) */
                if (g_searching.load()) XjsSearchWindow::Cur()->selFirstPending = true;
                else if (g_resultCount > 0) { XjsSelectOnly(0); XjsEnsureVisible(0); }
                return true;
            }
            if (ctrl) { XjsOpenFolderAndSelect(XjsItemPath(sel[0])); return true; }      /* Ctrl+Enter 定位 */
            if (GetKeyState(VK_MENU) & 0x8000) { XjsShowProperties(sel[0]); return true; } /* Alt+Enter 属性 */
            XjsOpenFile(XjsItemPath(sel[0]));
            return true;
        }
        case VK_ESCAPE: {
            /* Esc 多级链 (源样式): 取消剪切灰显 → 清除选择 → 清空搜索词 → 窗口消失 (统一策略) */
            if (!g_cutSet.empty()) { g_cutSet.clear(); XjsSearchWindow::Cur()->Invalidate(); return true; }
            if (XjsSelCount() > 0) {
                XjsSelClear(); g_anchorIdx = g_focusIdx = -1;
                XjsPreviewUpdateSelection();
                XjsSearchWindow::Cur()->Invalidate();
                return true;
            }
            if (!XjsSearchGetText().empty()) { XjsSearchClear(); return true; }
            XjsDismissWindow(g_hWnd);
            return true;
        }
        case VK_APPS: {
            /* 菜单键: 在焦点行弹出右键菜单 (源样式同款) */
            int idx = g_focusIdx;
            if (idx < 0) idx = XjsSelPrimaryIdx();
            if (idx >= 0) {
                if (!XjsSelIsSelected(idx)) XjsSelectOnly(idx);
                XjsEnsureVisible(idx);
                double rowHd = XjsRowHd();
                float y = (float)((double)g_layout.list.top + (double)idx * rowHd - g_scrollTop);
                POINT sp = { (LONG)(g_layout.list.left + XSF(120)), (LONG)y };
                ClientToScreen(g_hWnd, &sp);
                XjsShowContextMenu(sp, idx);
            }
            return true;
        }
        default:
            if (vk == VK_F10 && shift) { /* Shift+F10 同菜单键 */
                int idx = g_focusIdx >= 0 ? g_focusIdx : XjsSelPrimaryIdx();
                if (idx >= 0) {
                    if (!XjsSelIsSelected(idx)) XjsSelectOnly(idx);
                    double rowHd = XjsRowHd();
                    float y = (float)((double)g_layout.list.top + (double)idx * rowHd - g_scrollTop);
                    POINT sp = { (LONG)(g_layout.list.left + XSF(120)), (LONG)y };
                    ClientToScreen(g_hWnd, &sp);
                    XjsShowContextMenu(sp, idx);
                }
                return true;
            }
            return false;
    }
    if (step != 0 && g_resultCount > 0) {
        if (step == -g_resultCount) g_focusIdx = 0;
        else if (step == g_resultCount) g_focusIdx = g_resultCount - 1;
        else if (g_focusIdx < 0) g_focusIdx = (step > 0 ? 0 : g_resultCount - 1);
        else g_focusIdx = ximax(0, ximin(g_resultCount - 1, g_focusIdx + step));
        if (GetKeyState(VK_SHIFT) & 0x8000) {
            if (g_anchorIdx < 0) g_anchorIdx = g_focusIdx;
            XjsSelectRange(g_anchorIdx, g_focusIdx);
        } else {
            XjsSelectOnly(g_focusIdx);
        }
        XjsEnsureVisible(g_focusIdx);
    }
    return true;
}
