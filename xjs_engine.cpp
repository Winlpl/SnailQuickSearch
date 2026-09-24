/*
 * xjs_engine.cpp — 迅捷搜引擎封装
 * SDK 回调(UI线程转发)/结果对象/搜索(强制类型)/筛选分类/行数据缓存(含驱动器信息)/图标缓存/配置读写
 */
#include "xjs_app.h"
#include "picojson.h"    /* 全库唯一 include 点: JSON 是配置门面的实现细节, 换模块只改本文件 */

#define MAX_SEARCH_HISTORY 1000

/* ==================== SDK 回调 (SDK 线程 → PostMessage → UI 线程) ==================== */

/* 记账版 post: w = 消息归属窗 (引擎线程已持有指针, 禁止在此查实例表 — 见 OfResult 注释)。
   DestroyWindow 会把队列里本窗的未处理消息整批清除, 处理分支的 XjsPostToUiDone 轮不到它们
   → 全局闸门计数按每窗账在窗口销毁时返还 (XjsPostToUiDropWindow), 否则配额慢性泄漏 */
static bool XjsPostToUiFor(XjsSearchWindow* w, UINT msg, WPARAM wp, LPARAM lp) {
    if (!w || !w->hWnd) return false;
    if (!XjsPostToUi(w->hWnd, msg, wp, lp)) return false;
    w->uiPostPending.fetch_add(1, std::memory_order_relaxed);
    return true;
}

INT XJS_CALLBACK Xjs_EnumProgress(void* userData, xjs_engine* engine, const char* driveLetter, int totalCount, int enumeratedCount) {
    {
        XjsScanProgressData* d = new XjsScanProgressData();
        d->drive = driveLetter ? (wchar_t)driveLetter[0] : L'?';
        d->enumerated = enumeratedCount;
        d->total = totalCount;
        if (!XjsPostToUiFor(XjsSearchWindow::Main(), WM_SCAN_PROGRESS, (WPARAM)d, 0)) delete d;   /* 队满: 静默丢弃 */
    }
    return 0;
}

void XJS_CALLBACK Xjs_LoadComplete(void* userData, xjs_engine* engine, int fileCount) {
    XjsPostToUiFor(XjsSearchWindow::Main(), WM_LOAD_COMPLETE, (WPARAM)fileCount, 0);
}

/* 文件同步变化回调 (同步_文件创建/修改/移动/删除, SDK 线程, 引擎写锁内): 源样式 引擎_文件同步变化
   同构 — 只能快速置标记。SYNC_* 签名无结果对象 (引擎共享, 变化影响所有窗的结果集),
   故落引擎级标记 g_syncFileChanged; 覆盖 修改 这类不发结果变化事件、也不变结果数量的变化。
   由 ID_TIMER_SYNCWATCH 时钟节流消费重绘。返回 0 不拦截本次同步 */
std::atomic<bool> g_syncFileChanged{false};
std::atomic<bool> g_doubleCtrl{false};   /* 双击 Ctrl 当前激活态 (目标有效=真; 钩子线程读; 默认禁用) */
std::wstring g_doubleCtrlTarget;         /* 双击 Ctrl 触发目标窗口名 (空=禁用; "默认窗口"或档案名) */

/* 跨编译单元注册 (main.cpp WM_CREATE), 勿加 static (SDK回调跨TU不能static) */
INT XJS_CALLBACK Xjs_SyncFileChanged(void* userData, xjs_engine* engine, const char* filePath) {
    g_syncFileChanged = true;
    return 0;
}

INT XJS_CALLBACK Xjs_SyncFileMoved(void* userData, xjs_engine* engine, const char* srcPath, const char* destPath) {
    g_syncFileChanged = true;
    return 0;
}

void XJS_CALLBACK Xjs_EnumPartition(void* userData, xjs_engine* engine, const char* driveLetter) {
    {
        wchar_t* d = new wchar_t[2];
        d[0] = driveLetter ? (wchar_t)driveLetter[0] : L'?';
        d[1] = 0;
        if (!XjsPostToUiFor(XjsSearchWindow::Main(), WM_SCAN_DRIVE, (WPARAM)d, 0)) delete[] d;   /* 队满: 静默丢弃 */
    }
}

void XJS_CALLBACK Xjs_EnumComplete(void* userData, xjs_engine* engine, int elapsedMs) {
    g_isScanning = false;
    {
        XjsScanCompleteData* d = new XjsScanCompleteData();
        d->fileCount = xjs_db_GetFileCount(engine);
        d->elapsedMs = elapsedMs;
        if (!XjsPostToUiFor(XjsSearchWindow::Main(), WM_SCAN_COMPLETE, (WPARAM)d, 0)) delete d;   /* 队满: 静默丢弃 */
    }
}

/* 搜索类回调按 结果对象→所属窗 定位 (每窗独立 xjs_result; 引擎线程禁用 Cur()) */
static int XJS_CALLBACK Xjs_SearchComplete(void* userData, xjs_engine* engine, xjs_result* result, int searchFingerprint, const char* keyword, BOOL discarded) {
    XjsSearchWindow* w = XjsSearchWindow::OfResult(result);
    /* 计数按值直传 (曾堆分配载荷: 每次搜索完成一次堆分配, 且子窗销毁时队列残留无人释放) */
    if (w && w->hWnd && !discarded)
        XjsPostToUiFor(w, WM_SEARCH_COMPLETE, (WPARAM)xjs_result_GetCount(result), 0);
    return 0;
}

/* 结果变化 (搜索结果变化事件, DLL 维护线程): 源样式 m_文件变化待刷新 同构 — 绑窗成员只置标记,
   由 ID_TIMER_SYNCWATCH 时钟节流重绘 (宿主改名/别名等非 USN 变化只走这条事件) */
static int XJS_CALLBACK Xjs_SearchChange(void* userData, xjs_engine* engine, xjs_result* result, BOOL resetCount) {
    XjsSearchWindow* w = XjsSearchWindow::OfResult(result);
    if (w) w->fileChangePending = true;
    else g_syncFileChanged = true;   /* 结果对象未绑窗 (不应发生): 退到引擎级标记保底 */
    return 0;
}

static int XJS_CALLBACK Xjs_SearchFailed(void* userData, xjs_engine* engine, xjs_result* result, int searchFingerprint, const char* errorJson) {
    XjsSearchWindow* w = XjsSearchWindow::OfResult(result);
    /* 登记式投递: 子窗销毁时队列残留的载荷由 XjsPostToUiDropWindow 兜底释放 */
    if (w && w->hWnd && errorJson)
        XjsPostUiOwnedString(w, WM_SEARCH_FAILED, searchFingerprint, new std::string(errorJson));
    return 0;
}

/* 图标按需获取闸门 (XJS_RESULT_EVENT_ICON_ASK): 异步图标线程在真实获取前询问, 非0=继续。
   搜索结果读锁内运行 — 只读普通全局, 禁止加锁/等待/调用会拿锁的 xjs API。
   按 结果对象→所属窗 取闸门状态; 三条拒绝线 (源样式同口径): ①旧搜索残留请求
   ②尺寸档≠所属窗当前视图模式的请求档 ③行不在所属窗最近一帧渲染的可见区间 */
static int XJS_CALLBACK Xjs_IconAsk(void* userData, xjs_engine* engine, xjs_result* result, int searchFingerprint, int fileId, int itemIndex, int iconSize, const char* callbackInfo) {
    XjsSearchWindow* w = XjsSearchWindow::OfResult(result);
    if (!w) return 0;
    if (searchFingerprint != w->searchFingerprint) return 0;
    if (callbackInfo && strstr(callbackInfo, "preview"))
        return (w->previewVisible && fileId == w->previewFileId) ? 1 : 0;   /* 预览卡小图标: 只取当前预览选中文件 */
    if (itemIndex < w->visFirst || itemIndex > w->visLast) return 0;        /* 表项未显示 → 不获取 */
    /* 期望档 = 逻辑档 × 每窗尺度 × 每窗页面缩放 (与渲染侧 XjsIconFetchPx 同口径;
       引擎线程禁 Cur() → 用 w->dpiS / w->uiZoom)。
       两张表都放行: 列表/详情按 XJS_ICON_PX 请求, 网格按 XJS_FETCH_ICON 请求 */
    float zs = w->uiZoom / 10.0f;
    int expectFetch = (int)(XJS_FETCH_ICON[w->viewMode] * w->dpiS * zs + 0.5f);
    int expectDisp  = (int)(XJS_ICON_PX[w->viewMode]  * w->dpiS * zs + 0.5f);
    return (iconSize == expectFetch || iconSize == expectDisp) ? 1 : 0;    /* 与所属窗当前请求档不同 → 拒绝 */
}

/* 图标就绪: dome 不做位图缓存 (DLL 内部已有后缀/路径级字节缓存), 仅触发重绘 —
   下一帧渲染经 GetFileIco 同步取得就绪图标。图标缓存进程共享 → 发主窗, 处理时广播各窗 */
static void XJS_CALLBACK Xjs_DrawIcon(void* userData, xjs_engine* engine, xjs_result* result, int searchFingerprint, int id, int itemIndex, const void* iconData, int iconLength, const char* callbackInfo) {
    (void)userData; (void)engine; (void)result; (void)searchFingerprint; (void)id; (void)itemIndex; (void)iconData; (void)iconLength; (void)callbackInfo;
    XjsPostToUiFor(XjsSearchWindow::Main(), WM_ICON_READY, 0, 0);   /* 队满/主窗未建: 静默丢弃 (本回调无堆分配) */
}

/* ==================== 结果对象 / 搜索 ==================== */

std::atomic<bool> g_dbReady{false};   /* 数据库就绪 (加载完成 WM_LOAD_COMPLETE / 首次扫描完成 WM_SCAN_COMPLETE 置位)。
                                         结果对象创建时即按"库内当前已开启字段"定死查询顺序 (DLL 初始化查询顺序:
                                         评分字段在库=评分序, 不在=文件名序) —— 库还没加载就建对象会被永久定成
                                         文件名序 (2026-09-15 实锤), 故未就绪前一切创建请求一律拒绝 */

/* 为指定窗口创建结果对象 (若尚无): 绑定回调/匹配设置/UserValue; 数据库未就绪 = 拒绝创建 (见 g_dbReady) */
bool XjsEngineEnsureResultFor(XjsSearchWindow* w) {
    if (!w) return false;
    if (w->result) return true;
    if (!g_engine || !g_dbReady.load()) return false;
    w->result = xjs_result_Create(g_engine);
    if (!w->result) return false;
    /* 结果对象绑定所属窗口上下文 (SDK 用户标记值): 回调线程据此刻分是哪个窗口的结果 */
    xjs_result_SetUserValue(w->result, w);
    xjs_result_SetCallback(w->result, XJS_RESULT_EVENT_COMPLETE,  (const void*)Xjs_SearchComplete, NULL);
    xjs_result_SetCallback(w->result, XJS_RESULT_EVENT_CHANGE,    (const void*)Xjs_SearchChange, NULL);
    xjs_result_SetCallback(w->result, XJS_RESULT_EVENT_FAILED,    (const void*)Xjs_SearchFailed, NULL);
    xjs_result_SetCallback(w->result, XJS_RESULT_EVENT_DRAW_ICON, (const void*)Xjs_DrawIcon, NULL);
    xjs_result_SetCallback(w->result, XJS_RESULT_EVENT_ICON_ASK,  (const void*)Xjs_IconAsk, NULL);
    XjsApplyMatchSettingsFor(w);   /* 搜索匹配设置按结果对象生效: 本窗新建即按本窗档案下发 */
    return true;
}

bool XjsEngineEnsureResult() { return XjsEngineEnsureResultFor(XjsSearchWindow::Cur()); }

static void XjsEnsureResultOne(XjsSearchWindow* w) { (void)XjsEngineEnsureResultFor(w); }

/* 数据库就绪广播 (WM_LOAD_COMPLETE / WM_SCAN_COMPLETE 入口调): 给启动早于就绪的窗口补建结果对象
 * (主窗 WM_CREATE 建不了; 也不落 Per-窗掉队 —— 含加载完成前开出的第二窗) */
void XjsEngineEnsureResultAll() {
    XjsSearchWindow::ForEach(&XjsEnsureResultOne);
}

/* 渲染缓存作废是引擎级事件 (重建索引/退出) → 广播到所有窗口 */
static void XjsClearWinCaches(XjsSearchWindow* w) {
    w->rowCache.clear();
    w->ClearDebounceSnapshot();   /* 防抖快照随渲染缓存一并作废 (重建索引/退出) */
    w->searching = false;
    for (auto& kv : w->iconCache) { if (kv.second) kv.second->Release(); }   /* 图标位图随渲染缓存作废 (重建索引) */
    w->iconCache.clear();
    if (w->previewImage) { w->previewImage->Release(); w->previewImage = NULL; }
    w->previewImageFileId = -1;
    if (w->plugPanelCache) { w->plugPanelCache->Release(); w->plugPanelCache = NULL; }   /* 面板接管位图绑 RT, 随缓存作废 */
    w->plugPanelCacheRt = NULL;
    w->plugPanelCacheRev = 0;
}
void XjsClearRenderCaches() {
    XjsSearchWindow::ForEach(&XjsClearWinCaches);
    /* 模块级 RT 域缓存 (位图/画刷都绑建它那一刻的原生 RT): 设备重建后旧域全死,
       Begin 处的"同包装指针"判定感知不到包装内原生 RT 已换, 必须显式作废 */
    XjsPreviewPlugCacheInvalidate();
    XjsToastDropBrushCache();
}

/* 重新搜索/结果增量变化时清行缓存 (索引→新文件); 搜索提交后保留 —
   防抖重绘直接复用旧行数据 (索引→快照ID 未变), 完成回调再清。图标无 dome 级缓存
   (DLL 内部有字节缓存), 每帧按需即取即解码即释放。
   库内容变化 (重命名/别名/同步删除…) 对所有窗生效 → 行缓存全部作废 */
static void XjsClearWinRowCache(XjsSearchWindow* w) {
    w->rowCache.clear();
}
void XjsClearRowCache() {
    XjsSearchWindow::ForEach(&XjsClearWinRowCache);
}

/* ==================== 文件同步变化轮询 (源样式 线程时钟 移植) ====================
 * 源样式 w_线程时钟类 起后台线程 100ms 轮询 取结果数量(), 变了发 searchChanged 通知页面刷新;
 * 源样式另有 m_文件变化待刷新 (SDK同步/结果变化回调只置标记) 但时钟只比数量 — 改名/修改这类
 * 数量不变的变化没人消费, 列表不重绘。dome 把两条来源收编进同一个系统定时器 (UI 线程,
 * 每窗一个, 零线程同步问题): ①数量比对 ②g_fileChangePending 标记 (改名/修改/删除/新建全走这),
 * 任一变化用 GetTickCount64 真实时钟节流 — 距上次实际刷新不足 XJS_SYNC_REFRESH_MS 就顺延到
 * 下一拍 (基线/标记都不动, 下拍重检, 变化不丢), 同步风暴时刷新频率恒有上限不吃 CPU */
static ULONGLONG s_syncLastRefreshMs = 0;   /* 全局节流: 多窗合计刷新率也封顶 */

void XjsSyncWatchStart(HWND hwnd) {
    /* 基线取建窗时的结果数量: 启动建库的正常增长不算"变化", 只兜同步引起的变化 */
    g_syncWatchCount = g_result ? xjs_result_GetCount(g_result) : -1;
    SetTimer(hwnd, ID_TIMER_SYNCWATCH, XJS_SYNC_POLL_MS, NULL);
}

void XjsSyncWatchTick() {
    if (g_searching.load()) return;   /* 搜索中数量是瞬态0 (源样式 m_正在搜索 跳过, 防列表闪"未找到") */
    int cnt = 0;
    bool countChanged = false;
    if (g_result) {
        cnt = xjs_result_GetCount(g_result);
        countChanged = (cnt != g_syncWatchCount);
    }
    /* 只窥探不消费: 节流未放行前 标记/基线 都保留, 顺延的下一拍重检 (不丢变化) */
    if (!g_fileChangePending.load() && !g_syncFileChanged.load() && !countChanged) return;
    ULONGLONG now = GetTickCount64();
    if (now - s_syncLastRefreshMs < XJS_SYNC_REFRESH_MS) return;
    s_syncLastRefreshMs = now;
    g_fileChangePending = false;      /* 置假只在本 UI 线程 (置真在 DLL 维护线程); 窄竞态最坏顺延到下次变化 */
    g_syncFileChanged = false;
    if (countChanged) {
        g_syncWatchCount = cnt;
        g_resultCount = cnt;
        XjsClampScroll();
    }
    XjsClearRowCache();                     /* 同步改了库内容 (改名/修改等数量不变的变化也走这), 行缓存全作废 */
    XjsSearchWindow::InvalidateAllLists();  /* 库内容变化对所有窗生效, 各窗列表都重绘 */
    XjsPluginOnSyncAfter();                 /* 插件 events 订阅: 文件同步节流刷新点 (聚合语义, UI 线程) */
}

/* ==================== 搜索历史导航 (Alt+←/→ / 鼠标侧键, 源样式会话导航栈口径) ==================== */

static std::vector<std::wstring> s_navStack;
static int s_navPos = -1;
static bool s_navJumping = false;

void XjsHistoryNav(int dir) {
    int np = s_navPos + dir;
    if (np < 0 || np >= (int)s_navStack.size()) return;
    s_navPos = np;
    s_navJumping = true;
    XjsSearchSetText(s_navStack[s_navPos]);   /* 置入即触发搜索 (false 不再入栈) */
    s_navJumping = false;
    XjsSearchWindow::Cur()->Invalidate();
}

/* 搜索防抖快照: 拷贝"已显示行"的文件ID (仅可见区, 不拷全部)、当前结果数与行选中态。
   只能在结果数组仍是上一场搜索的稳定态时采样 (提交新查询前调用);
   可见区间来自最近一帧渲染回写 (g_visFirst/g_visLast), 无有效可见区 = 空快照 (防抖不生效) */
static void XjsDebounceSnapshot() {
    XjsSearchWindow::Cur()->ClearDebounceSnapshot();
    g_debounceCount = g_resultCount;
    if (!g_result || g_resultCount <= 0 || g_visFirst < 0 || g_visLast < g_visFirst) return;
    int first = g_visFirst;
    int last = g_visLast < g_resultCount ? g_visLast : g_resultCount - 1;
    if (first > last) return;
    g_debounceFirst = first;
    g_debounceIds.reserve((size_t)(last - first + 1));
    g_debounceSel.reserve((size_t)(last - first + 1));
    for (int i = first; i <= last; i++) {
        g_debounceIds.push_back(xjs_result_GetFileId(g_result, i));
        /* 选中态必须此刻快照: 新查询一提交引擎就清空选中, 冻结期选中的行会全丢高亮 */
        g_debounceSel.push_back(xjs_result_IsSelectedByIndex(g_result, i) != FALSE);
    }
}

void XjsSearchNow(bool commitHistory) {
    if (!g_engine || g_isScanning) return;
    if (!XjsEngineEnsureResult()) return;
    std::wstring text = XjsSearchGetText();   /* 自绘搜索框状态 (原 GetWindowText 口径) */
    /* 托管标签链接管 (源样式 doSearch 接入口径): ①输入词命中模式名 → 转标签接管;
       ②已有标签 → 标签链(+输入文字尾阶段) 多重搜索 (词已是链中标签时按纯文本作尾阶段);
       ③普通搜索路径静默清标签 (标签只代表"搜索框处于托管搜索状态") */
    if (XjsHostedTrySearch(text)) return;
    if (!g_hostedTags.empty()) { XjsHostedExecChain(text); return; }
    XjsHostedClearTags();
    std::string kw = Utf16ToUtf8(text.c_str());
    /* 防抖: 提交前快照已显示行ID; 上一场搜索未结束时不重采样 (此刻数组是过渡态, 保留旧快照) */
    if (!g_searching.load()) XjsDebounceSnapshot();
    int fingerprint = -1;
    /* g_modeToKeyword 直接映射: Lua 过滤档=-3 谓词 / Lua 执行档=-4 执行 (引擎已移除专用入口,
       两种 Lua 均经 Query 以类型提交) */
    fingerprint = xjs_result_Query(g_result, kw.c_str(), g_modeToKeyword[g_mode], FALSE);
    if (fingerprint != -1) {
        g_searching.store(true);   /* 重绘转用快照画, 完成/失败回调解除 */
        g_searchFingerprint = fingerprint;
        g_errText.clear();
        /* 引擎在新查询提交时自行清空选中 (搜索结果.清空) — 宿主只清索引类 UI 状态 */
        g_anchorIdx = g_focusIdx = -1;
        g_cutSet.clear();   /* 结果集换血, 剪切灰显失效 */
        g_hoverTraces.clear();   /* 拖尾按行下标记忆, 结果集换血立即作废 */
        g_scrollTop = 0;
        /* 状态栏防闪: 不立即显示"正在搜索…", 200ms 仍未完成才显示 (WM_TIMER 里落地;
           快速完成则计时器被杀, 状态栏直接从旧文字切到结果文字) */
        if (g_hWnd) SetTimer(g_hWnd, ID_TIMER_SEARCHSTATUS, 200, NULL);
        if (commitHistory && !text.empty()) {
            XjsAddHistory(text);
            /* 会话导航栈: 提交型搜索入栈, 截掉前进分支 (源样式 07-search.js 同语义) */
            if (!s_navJumping) {
                if (s_navPos >= 0 && s_navPos + 1 < (int)s_navStack.size()) s_navStack.resize(s_navPos + 1);
                if (s_navStack.empty() || s_navStack.back() != text) s_navStack.push_back(text);
                if (s_navStack.size() > 200) s_navStack.erase(s_navStack.begin());
                s_navPos = (int)s_navStack.size() - 1;
            }
        }
        SetWindowTextW(g_hWnd, text.empty() ? XjsT(L"应用.名称") : (text + L" - " + XjsT(L"应用.名称")).c_str());
        XjsPreviewUpdateSelection();
    }
    else {
        /* 引擎拒绝提交 (fingerprint=-1): 不显示就等于"点了没反应", 用户无从排查 */
        g_errText = XjsT(L"错误.查询提交失败");
    }
    XjsSearchWindow::Cur()->Invalidate();
}

/* ==================== 筛选分类 ==================== */

void XjsLoadFilters() {
    g_filters.clear();
    if (!g_result) return;
    std::string json = xjs_result_GetAllFilter(g_result);
    if (json.length() >= 3) {
        picojson::value v;
        if (picojson::parse(v, json).empty() && v.is<picojson::array>()) {
            std::string keyName = Utf16ToUtf8(L"名称");
            for (auto& e : v.get<picojson::array>()) {
                if (!e.is<picojson::object>()) continue;
                auto& obj = e.get<picojson::object>();
                auto it = obj.find(keyName);
                if (it != obj.end() && it->second.is<std::string>())
                    g_filters.push_back({ Utf8ToUtf16(it->second.get<std::string>().c_str()) });
            }
        }
    }
    if (g_filters.empty() || g_filters[0].name != L"全部")
        g_filters.insert(g_filters.begin(), { L"全部" });
    /* 分类表重建后可变短: 各窗已选下标原样保留会越界 (标题栏布局/下拉菜单按它直接索引) — 全窗夹回 */
    for (int i = 0; i < XjsSearchWindow::Count(); i++)
        if (XjsSearchWindow::At(i)->filterSel >= (int)g_filters.size())
            XjsSearchWindow::At(i)->filterSel = 0;
}

void XjsApplyFilter(int idx) {
    if (idx < 0 || idx >= (int)g_filters.size() || !g_result) return;
    g_filterSel = idx;
    std::string name = Utf16ToUtf8(g_filters[idx].name.c_str());
    xjs_result_SetSelectedFilter(g_result, name.c_str());
    XjsSearchNow(false);
}

/* ==================== 行数据缓存 (含驱动器信息) ==================== */

/* 驱动器行判定: 父目录ID == -1 (引擎分区根约定, 同正式版) */
bool XjsIsDriveRow(int idx) {
    if (!g_engine || !g_result || idx < 0) return false;
    int fileId = xjs_result_GetFileId(g_result, idx);
    if (fileId < 0) return false;
    return xjs_db_GetParentDirectoryId(g_engine, fileId) == -1;
}

/* 驱动器占用色: 蓝 rgb(59,130,246) → 红 rgb(239,68,68) 逐通道线性插值
   (正式版 08-vlist.js/18-preview.js 同款; 中低占用=蓝紫→紫红。
    禁改 HSL 色相旋转 — 色相路过 120° 会让中低占用变绿, 与源样式不符, 2026-09-16 实锤) */
XjsColor XjsDriveColor(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    float t = percent / 100.0f;
    return XjsColorF((59 + 180 * t) / 255.0f, (130 - 62 * t) / 255.0f, (246 - 178 * t) / 255.0f, 1.0f);
}

static void XjsFillDriveInfo(XjsRowData& rd, const std::wstring& driveName) {
    std::wstring root = driveName + L"\\";
    wchar_t label[MAX_PATH + 1] = {0};
    DWORD serial = 0, maxLen = 0, flags = 0;
    wchar_t fs[64] = {0};
    if (GetVolumeInformationW(root.c_str(), label, MAX_PATH, &serial, &maxLen, &flags, fs, 64)) {
        rd.driveLabel = label;
        rd.driveSerial = serial;
        rd.driveFs = fs;
    }
    if (rd.driveLabel.empty()) rd.driveLabel = XjsT(L"预览.本地磁盘");
    ULONGLONG total = 0, freeQ = 0;
    if (GetDiskFreeSpaceExW(root.c_str(), NULL, (PULARGE_INTEGER)&total, (PULARGE_INTEGER)&freeQ)) {
        rd.driveTotal = (long long)total;
        rd.driveFree = (long long)freeQ;
        rd.driveUsed = rd.driveTotal - rd.driveFree;
        rd.drivePercent = rd.driveTotal > 0 ? (int)((double)rd.driveUsed * 100 / rd.driveTotal) : 0;
    }
    /* 文件夹列: "卷标 (总容量)" (正式版驱动器行样式) */
    rd.folder = rd.driveLabel + L" (" + Utf8ToUtf16(xjs_util_FormatFileSize(rd.driveTotal)) + L")";
}

/* GetMatchKeywordsEx JSON: {"文件名":[..],"文件夹":[..],"别名":[..]} */
struct XjsExKeywords { std::vector<std::wstring> name, folder, alias; };
static XjsExKeywords XjsParseKeywordsEx(const char* json) {
    XjsExKeywords ex;
    if (!json || strlen(json) < 3) return ex;
    std::string copy = json;
    picojson::value v;
    if (!picojson::parse(v, copy).empty() || !v.is<picojson::object>()) return ex;
    picojson::object& obj = v.get<picojson::object>();
    auto get = [&](const wchar_t* key) {
        auto it = obj.find(Utf16ToUtf8(key));
        std::vector<std::wstring> out;
        if (it != obj.end() && it->second.is<picojson::array>())
            for (auto& e : it->second.get<picojson::array>())
                if (e.is<std::string>()) out.push_back(Utf8ToUtf16(e.get<std::string>().c_str()));
        return out;
    };
    ex.name = get(L"文件名");
    ex.folder = get(L"文件夹");
    ex.alias = get(L"别名");
    return ex;
}

static std::vector<XjsHlSeg> XjsBuildSegs(const std::wstring& text, const std::vector<std::wstring>& keys) {
    std::vector<XjsHlSeg> segs;
    if (text.empty()) return segs;
    if (keys.empty()) { segs.push_back({text, false}); return segs; }
    std::vector<bool> mask(text.size(), false);
    for (const auto& kw : keys) {
        if (kw.empty()) continue;
        size_t pos = 0;
        while ((pos = text.find(kw, pos)) != std::wstring::npos) {
            for (size_t j = 0; j < kw.size() && pos + j < mask.size(); j++) mask[pos + j] = true;
            pos += kw.size();
        }
    }
    std::wstring cur; bool curKey = mask[0];
    for (size_t i = 0; i < text.size(); i++) {
        if (mask[i] != curKey) {
            if (!cur.empty()) segs.push_back({cur, curKey});
            cur = text[i]; curKey = mask[i];
        } else cur += text[i];
    }
    if (!cur.empty()) segs.push_back({cur, curKey});
    return segs;
}

XjsRowData* XjsEnsureRowData(int idx) {
    if (!g_result) return NULL;
    return XjsEnsureRowData(idx, xjs_result_GetFileId(g_result, idx));
}

/* 指定文件ID版 (防抖绘制: 搜索中 idx→ID 从快照取, 不读过渡态结果数组; fileId<0 = 空行) */
XjsRowData* XjsEnsureRowData(int idx, int fileId) {
    auto it = g_rowCache.find(idx);
    if (it != g_rowCache.end()) return &it->second;
    if (!g_result || !g_engine || fileId < 0) return NULL;
    XjsRowData rd;
    rd.fileId = fileId;
    rd.isDrive = (xjs_db_GetParentDirectoryId(g_engine, fileId) == -1);
    rd.name = Utf8ToUtf16(xjs_db_GetName(g_engine, fileId));
    if (rd.isDrive) {
        XjsFillDriveInfo(rd, rd.name);
        /* 驱动器行: 名称列不分段高亮 */
        rd.nameSegs.push_back({rd.name, false});
        /* 驱动器别名 (引擎按 fileId 同表存储, 右键"设置别名"可设, 2026-09-16 用户口径) */
        const char* daliasA = xjs_db_GetAlias(g_engine, fileId);
        if (daliasA && daliasA[0]) { rd.alias = Utf8ToUtf16(daliasA); rd.hasAlias = true; }
    } else {
        rd.folder = Utf8ToUtf16(xjs_db_GetParentDirectory(g_engine, fileId));
        rd.size = xjs_db_GetFileSize(g_engine, fileId);
        rd.mtime = xjs_db_GetModifyTime(g_engine, fileId);
        rd.ctime = xjs_db_GetCreateTime(g_engine, fileId);
        rd.atime = xjs_db_GetAccessTime(g_engine, fileId);
        rd.attrs = (unsigned)xjs_db_GetFileAttributes(g_engine, fileId);
        rd.rating = xjs_db_GetRating(g_engine, fileId);
        const char* aliasA = xjs_db_GetAlias(g_engine, fileId);
        if (aliasA && aliasA[0]) { rd.alias = Utf8ToUtf16(aliasA); rd.hasAlias = true; }
        std::string json = xjs_result_GetMatchKeywordsEx(g_result, fileId);
        XjsExKeywords ex = XjsParseKeywordsEx(json.c_str());
        rd.nameSegs = XjsBuildSegs(rd.name, ex.name);
        rd.folderSegs = XjsBuildSegs(rd.folder, ex.folder);
        if (rd.hasAlias) rd.aliasSegs = XjsBuildSegs(rd.alias, ex.alias);
    }
    if (g_rowCache.size() > 4096) g_rowCache.clear();
    return &g_rowCache.emplace(idx, std::move(rd)).first->second;
}

/* ==================== 图标缓存 ==================== */

/* 取行图标位图 (返回值归调用方所有, 用完 Release): dome 侧按 fileId+档位缓存解码位图 —
   整窗重绘的性能前提 (此前每帧逐行重解码 WIC, 是被迫走局部脏区重绘的直接原因)。
   DLL 内部另有 后缀/路径+尺寸 级字节缓存; 未缓存则入队 (ICON_ASK 闸门把关),
   排队期返回临时默认图标 — 照画但**不进缓存** (现解码, 真图标就绪后 DRAW_ICON 触发重绘即取而代之)。
   位图缓存上限 512, 超限全清 (与既有 Trim 口径一致); RT 重建/设备丢弃时随 brushCache 全清 */
XjsBitmap* XjsGetRowIcon(int idx, int fileId, int iconPx, const char* askTag) {
    if (!g_result) return NULL;
    unsigned long long key = ((unsigned long long)(unsigned int)fileId << 16) | (unsigned)(iconPx & 0xFFFF);   /* 档位含缩放请求尺寸 (最大 384, 16 位足够) */
    auto it = g_iconCache.find(key);
    if (it != g_iconCache.end()) {
        if (it->second) it->second->AddRef();
        return it->second;
    }
    /* callbackInfo 带请求尺寸与来源 ("row:64"/"preview:32"), 供 ICON_ASK 闸门区分 */
    char tag[24];
    _snprintf(tag, sizeof(tag), "%s:%d", askTag ? askTag : "row", iconPx);
    tag[sizeof(tag) - 1] = 0;
    int len = 0;
    const void* data = xjs_result_GetFileIco(g_result, fileId, idx, iconPx, &len, tag);
    if (!data || len <= 0) return NULL;
    if (xjs_result_IsIconPending(g_result, g_searchFingerprint, fileId)) {
        /* 排队期: GetFileIco 返回的是临时默认图标 — 照画, 但绝不进缓存 (key 只有 fileId+尺寸,
           缓住了真图标就绪后会被临时图挡住); 每次重绘现解码, 就绪后 (DRAW_ICON→重绘) 走缓存 */
        return XjsDecodeImage(data, len);
    }
    XjsBitmap* bmp = XjsDecodeImage(data, len);
    if (!bmp) return NULL;
    if ((int)g_iconCache.size() >= 512) {
        for (auto& kv : g_iconCache) if (kv.second) kv.second->Release();
        g_iconCache.clear();
    }
    bmp->AddRef();            /* 缓存持有一份独立引用 (否则调用方 Release 即销毁, 缓存变悬垂指针) */
    g_iconCache[key] = bmp;
    return bmp;               /* 返回值仍归调用方 Release */
}

/* ==================== 历史 ==================== */

/* ==================== 配置 (xjs_config.json, 源样式 settings.json 同风格 camelCase 键) ====================
 * XjsConfig = 配置存取唯一门面, 类与 g_cfg 都收在本文件 (picojson 是实现细节, 不进公共头;
 * 换 JSON 模块 = 只改本文件)。键分发在 XjsLoadConfig/XjsSaveConfig */

/* 配置文档内存镜像 + 文件读写 + 类型化键存取 */
class XjsConfig {
public:
    void Load();             /* 文件 → 内存 (缺失/坏 JSON = 空档, 由调用方决定旧档导入/走默认) */
    void WriteBack();        /* 内存整档序列化落盘 (serialize(true) 格式化输出) */
    picojson::object& Root() { return m_obj; }
    picojson::object& Obj(const char* key);   /* 取子对象 (无/类型不符则建空对象) */
    void Set(const char* key, bool v) { m_obj[key] = picojson::value(v); }
    void Set(const char* key, int v) { m_obj[key] = picojson::value((double)v); }
    void Set(const char* key, const std::string& v) { m_obj[key] = picojson::value(v); }
    void Set(const char* key, const picojson::value& v) { m_obj[key] = v; }
    /* 类型化读 (缺键/类型不符 = 默认值); 静态 → 子对象 (uiWindows[i] 等) 同样适用 */
    static int Int(const picojson::object& o, const char* k, int def);
    static bool Bool(const picojson::object& o, const char* k, bool def);
    static std::wstring Str(const picojson::object& o, const char* k, const wchar_t* def);
private:
    picojson::object m_obj;  /* 整份配置的内存镜像 (utf8 键值), 保存 = 全量序列化 */
};

static XjsConfig g_cfg;      /* 进程唯一配置档案 (引擎/皮肤/历史/每窗档案共用, 仅 UI 线程; 本文件私有) */

/* ==================== 配置键名 (载入/保存共用常量, 防读写键名漂移) ====================
 * 绑定窗口的私有配置一律挂在顶层 "窗口" 数组各自条目的 "窗口名称" 之下
 * (快捷键/窗口行为/搜索设置/显示字段/私有搜索模式/搜索历史/界面行为……包括但不限于);
 * 进程共享的 (共享搜索模式/皮肤文件/页面缩放/双击Ctrl目标 等) 才放顶层独立键 */
static const char* const K_WINARR = "窗口";          /* 窗口档案数组 */
static const char* const K_WINNAME = "窗口名称";      /* 窗口唯一身份 (持久档案名) */
static const char* const K_SKIN = "皮肤";
static const char* const K_VIEW = "视图";             /* 值: list/details/medium/large */
static const char* const K_PREVIEW = "预览";
static const char* const K_PREVW = "预览宽度";
static const char* const K_OPENELE = "打开提权";
static const char* const K_OPENASY = "异步打开";
static const char* const K_OPENHIDE = "打开后隐藏";
static const char* const K_HOTKEY = "快捷键";          /* {"修饰键":..,"虚拟键":..} (录制才有值, 无内置默认) */
static const char* const K_MATCH = "搜索设置";         /* 内键同 DLL SetSearchSettings (中文) */
static const char* const K_COLS = "显示字段";          /* {列表:{顺序,宽度,弹性,显隐}, 详情:{...}} */
static const char* const K_COL_LISTVIEW = "列表";
static const char* const K_COL_DETVIEW = "详情";
static const char* const K_COL_ORDER = "顺序";
static const char* const K_COL_W = "宽度";
static const char* const K_COL_FLEX = "弹性";
static const char* const K_COL_V = "显隐";
static const char* const K_DRVPROG = "驱动器进度条";
static const char* const K_ROWHOVER = "悬停高亮";
static const char* const K_HOVERFADE = "悬停残影";
static const char* const K_BLUR = "失焦行为";           /* 0=无 1=关闭窗口 */
static const char* const K_APPEAR = "出现位置";         /* 激活/创建时位置档位 (序 = xjs_app.h XJS_APPEAR_*);
                                                          旧键 "激活位置" (四档语义) 已废弃不读, 按本库"不兼容旧格式"口径直接换名 */
static const char* const K_CTRLBTN = "显示控制按钮";    /* 最小化/最大化/关闭 (隐藏后左侧填充) */
static const char* const K_FILTERBOX = "显示筛选框";    /* 标题栏筛选下拉 (隐藏后搜索框填充) */
static const char* const K_STATUSBAR = "显示状态栏";
static const char* const K_TASKBAR = "任务栏图标";
static const char* const K_TOPMOST = "置顶";
static const char* const K_MOUSEOPEN = "鼠标打开";         /* 0=双击打开 1=单击打开 */
static const char* const K_DEFSEL = "默认选中";            /* 0=不选 1=结果刷新后选中第一个 */
static const char* const K_CREATEFILL = "创建填入搜索框";  /* 0=清空 1=用户指定关键词 2=上一次搜索词 */
static const char* const K_CREATEKEYWORD = "创建关键词";   /* createFill=1 的内容 */
static const char* const K_LASTSEARCH = "上次搜索词";      /* 关窗时的搜索框内容 (createFill=2 用) */
static const char* const K_SORTFIELD = "排序字段";         /* 默认排序 (空=引擎默认评分序) */
static const char* const K_SORTWAY = "排序方向";           /* true=升序 */
static const char* const K_MODES_SHARED = "共享搜索模式";   /* 顶层: 全部窗口生效 */
static const char* const K_MODES_PRIVATE = "私有搜索模式";  /* 窗口条目内: 仅本窗生效 (存储位置即作用域) */
static const char* const K_HISTORY = "搜索历史";            /* 窗口条目内: 每窗搜索历史 */
static const char* const K_SEARCHMODE = "搜索模式";          /* 窗口条目内: 关键词模式 (wildcard/regex/sql/lua);
                                                              曾顶层共享, 已每窗化 (顶层旧键迁移后废弃) */
static const char* const K_ZOOM = "页面缩放";               /* 曾顶层共享, 已每窗化 (条目内; 顶层旧键迁移后废弃) */
static const char* const K_DOUBLECTRL = "双击Ctrl目标";      /* 顶层: 档案名, 空=禁用 */
static const char* const K_ENGINE = "绘制引擎";              /* 顶层: "d2d"|"gdiplus", 重启生效 (设置-通用) */
static const char* const K_WINRECT = "窗口矩形";             /* 窗口条目内: {横坐标,纵坐标,宽,高,DPI}; 曾为顶层共享键(已迁入槽0后废弃) */
static const char* const K_REBUILD = "重建记忆";             /* 顶层: 重建对话框记忆 */
static const char* const K_COLSG = "列布局";                 /* 顶层: 全局列默认 (旧扁平键, 兜底用) */
static const char* const K_PLUGINS = "插件";                 /* 顶层: 插件用户状态数组 [{标识,启用,已确认版本}]
                                                                (目录发现/清单/加载状态不落盘 — 运行时扫 plugins\) */
static const char* const K_P_ID = "标识";
static const char* const K_P_ENABLED = "启用";
static const char* const K_P_CONFIRMED = "已确认版本";
/* 搜索模式条目内键 (共享/私有同一结构) */
static const char* const K_M_ID = "标识";                    /* 曾为 "id", 2026-09-19 全配置中文主键 */
static const char* const K_M_NAME = "名称";
static const char* const K_M_DESC = "简介";
static const char* const K_M_TYPE = "类型";
static const char* const K_M_TPL = "模板";

static const char* const K_LANG = "语言";                    /* 窗口条目内: "auto"/"zh"/"zh-TW"/"en"/"ko"/"th"/"ms"
                                                               (每窗, 2026-09-19 每窗化; 曾为顶层键, 读作迁移种子后废弃) */
static const char* const K_FILTERCFG = "文件分类";            /* 顶层: 引擎筛选器配置 (内嵌 JSON 数组字符串, 设置-文件分类 表格保存) */
static const char* const K_ALIASCFG = "路径别名";             /* 顶层: 路径别名配置 (内嵌 JSON 对象字符串, 设置-别名 表格保存) */

/* 设置保存的引擎下发配置 (内嵌 JSON 原文, utf8): Apply 成功即更新 + XjsSaveConfig 落盘;
   启动由 XjsEngineApplySavedConfigs 再下发 (引擎运行期配置不落盘, 见 xunjieso.h 筛选器/别名 API 注) */
static std::string g_savedFilterJson, g_savedAliasJson;

/* 读 exe 目录邻接的 UTF-8 文本文件原文 (剥 BOM): 文件缺席/读失败 = 假且 *out 清空。
   语言包 (XjsI18nLoadFile) / 用户配置 (XjsConfig::Load) / 别名内置词典 (Alias.json) 三处共用 */
static bool XjsReadUtf8File(const std::wstring& path, std::string* out) {
    out->clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[8192]; DWORD rd = 0;
    while (ReadFile(h, buf, sizeof(buf), &rd, NULL) && rd) out->append(buf, rd);
    CloseHandle(h);
    if (out->size() >= 3 && (unsigned char)(*out)[0] == 0xEF && (unsigned char)(*out)[1] == 0xBB && (unsigned char)(*out)[2] == 0xBF)
        out->erase(0, 3);   /* UTF-8 BOM */
    return true;
}

/* ==================== 多国语言 (i18n) ====================
 * 主键 = 点分中文主键 (如 "设置.外观.界面语言", 2026-09-19 起弃用"中文句子当键"):
 * 调用点 XjsT 传主键; 中文文案在 languages\zh.json 的值里, 各语言包同键。
 * 包 = exe 目录 languages\{zh,zh-TW,en,ko,th,ms}.json, 一层扁平 {"主键":"译文"} (不嵌套)。
 * **语言 = 每窗私有** (XjsSearchWindow::lang): XjsT 解析到当前绑定窗 (Cur), 引擎线程不画 UI 文案。
 * 启动一次全载所有表 (窗口间语言不同也不重装);
 * 回落链: 当前窗语言缺键/缺文件 → 英语表 → 简体表 → 键名。 */
#include <unordered_map>
static std::unordered_map<std::wstring, std::wstring> s_i18nTab[XLANG_N];

static const wchar_t* const XJS_LANG_FN[XLANG_N] = { L"zh", L"zh-TW", L"en", L"ko", L"th", L"ms" };

static int XjsLangFromCode(const wchar_t* s) {
    if (!wcscmp(s, L"zh")) return XLANG_ZH;
    if (!wcscmp(s, L"zh-TW")) return XLANG_ZHTW;
    if (!wcscmp(s, L"en")) return XLANG_EN;
    if (!wcscmp(s, L"ko")) return XLANG_KO;
    if (!wcscmp(s, L"th")) return XLANG_TH;
    if (!wcscmp(s, L"ms")) return XLANG_MS;
    return XLANG_AUTO;
}

static const char* XjsLangToCode(int lang) {
    switch (lang) {
        case XLANG_ZH: return "zh"; case XLANG_ZHTW: return "zh-TW"; case XLANG_EN: return "en";
        case XLANG_KO: return "ko"; case XLANG_TH: return "th"; case XLANG_MS: return "ms";
        default: return "auto";
    }
}

static void XjsI18nLoadFile(int lang, std::unordered_map<std::wstring, std::wstring>& out) {
    out.clear();
    if (lang < XLANG_ZH || lang >= XLANG_N) return;   /* 未知=空表走回落 */
    std::wstring p = XjsGetExeDir() + L"\\languages\\" + XJS_LANG_FN[lang] + L".json";
    std::string utf8;
    if (!XjsReadUtf8File(p, &utf8)) return;   /* 缺文件 = 该语言整表缺席 (回落英语/简体) */
    picojson::value v;
    if (picojson::parse(v, utf8).empty() && v.is<picojson::object>()) {
        for (auto& kv : v.get<picojson::object>())
            if (kv.second.is<std::string>() && !kv.first.empty())
                out[Utf8ToUtf16(kv.first.c_str())] = Utf8ToUtf16(kv.second.get<std::string>().c_str());
    }
}

/* 系统 UI 语言 → 程序语言 (中文再按子语言分简/繁: 台/港/澳=繁体, 大陆/新加坡=简体) */
static int XjsI18nSystemLang() {
    LANGID ui = GetUserDefaultUILanguage();
    switch (PRIMARYLANGID(ui)) {
        case LANG_CHINESE: {
            WORD sub = SUBLANGID(ui);
            return (sub == SUBLANG_CHINESE_TRADITIONAL || sub == SUBLANG_CHINESE_HONGKONG || sub == SUBLANG_CHINESE_MACAU)
                       ? XLANG_ZHTW : XLANG_ZH;
        }
        case LANG_KOREAN:  return XLANG_KO;
        case LANG_THAI:    return XLANG_TH;
        case LANG_MALAY:   return XLANG_MS;
        default:           return XLANG_EN;
    }
}

/* 当前绑定窗的生效语言 (auto=按系统; 无绑定窗=系统口径) */
static int XjsI18nWinLang() {
    const XjsSearchWindow* w = XjsSearchWindow::Cur();
    if (!w || w->lang == XLANG_AUTO) return XjsI18nSystemLang();
    return w->lang;
}

void XjsI18nInit() {
    for (int l = XLANG_ZH; l < XLANG_N; l++) XjsI18nLoadFile(l, s_i18nTab[l]);
}

const wchar_t* XjsT(const wchar_t* key) {
    if (!key) return L"";
    int cur = XjsI18nWinLang();
    auto& tab = s_i18nTab[cur];
    auto it = tab.find(key);
    if (it != tab.end() && !it->second.empty()) return it->second.c_str();
    auto& en = s_i18nTab[XLANG_EN];
    auto ie = en.find(key);
    if (ie != en.end() && !ie->second.empty()) return ie->second.c_str();
    auto& zh = s_i18nTab[XLANG_ZH];
    auto iz = zh.find(key);
    if (iz != zh.end() && !iz->second.empty()) return iz->second.c_str();
    return key;   /* 三表皆缺: 键名兜底 (点分主键, 新增文案漏配时可见) */
}

std::string XjsTUtf8(const wchar_t* key) { return Utf16ToUtf8(XjsT(key)); }

const wchar_t* XjsLangLabel(int lang) {
    static const wchar_t* const LB[XLANG_N] = { L"简体中文", L"繁體中文", L"English", L"한국어", L"ไทย", L"Bahasa Melayu" };
    return (lang >= XLANG_ZH && lang < XLANG_N) ? LB[lang] : L"";
}

const wchar_t* XjsLangAutoLabel() { return XjsT(L"通用词.自动跟随系统"); }

int XjsLangIndexFromCode(const wchar_t* code) { return code ? XjsLangFromCode(code) : XLANG_AUTO; }

const char* XjsLangCodeUtf8(int lang) { return XjsLangToCode(lang); }

/* 语言应用唯一入口 (设置页下拉与插件扩展 API 同落点): 写 owner 窗字段 → 落盘;
   各搜索窗标题按**各自语言**刷新 (回调内 XjsWindowScope 切换), 托盘提示随主窗语言。
   设置行模型重建/设置窗重绘是设置窗自身行为, 归调用方 (见 xjs_settings ACT_LANG 结果分支)。 */
void XjsApplyUiLang(XjsSearchWindow* w, int lang) {
    if (!w) return;
    w->lang = lang;
    XjsSaveConfig();
    struct LangRefresh { static void Run(XjsSearchWindow* x) {
        XjsWindowScope ws(x);   /* XjsT 解析到 x 自己的语言 */
        const std::wstring& st = x->searchEd.text;
        SetWindowTextW(x->hWnd, st.empty() ? XjsT(L"应用.名称")
                                           : (st + L" - " + XjsT(L"应用.名称")).c_str());
        x->Invalidate();
    }};
    XjsSearchWindow::ForEach(&LangRefresh::Run);
    if (w->isMain)
        XjsTrayAdd(XjsSearchWindow::MainHwnd());   /* 已入托盘时 = NIM_MODIFY 刷新提示 */
}

static void XjsCmToJson(const XjsCustomMode& cm, picojson::object& o) {
    o[K_M_ID] = picojson::value(Utf16ToUtf8(cm.id.c_str()));
    o[K_M_NAME] = picojson::value(Utf16ToUtf8(cm.name.c_str()));
    o[K_M_DESC] = picojson::value(Utf16ToUtf8(cm.desc.c_str()));
    o[K_M_TYPE] = picojson::value(Utf16ToUtf8(cm.type.c_str()));
    o[K_M_TPL] = picojson::value(Utf16ToUtf8(cm.tpl.c_str()));
}

static XjsCustomMode XjsCmFromJson(const picojson::object& o) {
    XjsCustomMode cm;
    cm.id = XjsConfig::Str(o, K_M_ID, L"");
    if (cm.id.empty()) cm.id = XjsConfig::Str(o, "id", L"");   /* 一次性迁移: 2026-09-19 前的英文键, 写回即消失 */
    cm.name = XjsConfig::Str(o, K_M_NAME, L"");
    cm.desc = XjsConfig::Str(o, K_M_DESC, L"");
    cm.type = XjsConfig::Str(o, K_M_TYPE, L"sql");
    cm.tpl = XjsConfig::Str(o, K_M_TPL, L"");
    return cm;
}

/* ==================== 插件 manifest 解析 ====================
 * 清单键为中文主键 (2026-09-19 用户口径, 与主配置一致); 枚举值 (file/dir/drive、file.read…、
 * library/app) 是数据串, 保持英文。值 (名称/简介) 可为任意语言, 宽字符化后给 UI。
 * 本函数 = manifest JSON 唯一解析点 (picojson 全库唯一 include 红线); 注册表在 xjs_plugin.cpp。
 * 字段缺失/类型不对 = 尽量容忍 (缺关键键才判清单错误), 损坏插件显示"清单错误(原因)"且永不加载 */
static const picojson::value XJS_PM_NULL;   /* 缺键兜底 (null) */

static std::wstring XjsPmStr(const picojson::object& o, const char* k) {
    auto it = o.find(k);
    if (it == o.end() || !it->second.is<std::string>()) return L"";
    return Utf8ToUtf16(it->second.get<std::string>().c_str());
}
static void XjsPmStrArr(const picojson::value& v, std::vector<std::wstring>* out, bool lowerExt) {
    out->clear();
    if (!v.is<picojson::array>()) return;
    for (auto& e : v.get<picojson::array>()) {
        if (!e.is<std::string>()) continue;
        std::wstring s = Utf8ToUtf16(e.get<std::string>().c_str());
        if (lowerExt) {   /* 扩展名表: 小写、去前导点与通配星 ("*.jpg"/".jpg"/"jpg" 三种写法统一收录为 "jpg",
                             匹配侧做严格相等 — 曾只剥点不剥星, 带星写法被原样收录后永不匹配且无诊断) */
            for (auto& c : s) c = towlower(c);
            if (!s.empty() && s[0] == L'*') s.erase(0, 1);
            if (!s.empty() && s[0] == L'.') s.erase(0, 1);
        }
        if (!s.empty()) out->push_back(s);
    }
}
static unsigned XjsPmCaps(const picojson::value& v) {
    unsigned m = 0;
    if (!v.is<picojson::array>()) return 0;
    for (auto& e : v.get<picojson::array>()) {
        if (!e.is<std::string>()) continue;
        const std::string& s = e.get<std::string>();
        if (s == "fileContextMenu") m |= XPC_FILECTX;
        else if (s == "searchBoxMenu") m |= XPC_SEARCHBOXMENU;
        else if (s == "searchModes") m |= XPC_SEARCHMODES;
        else if (s == "hostedSearch") m |= XPC_HOSTED;
        else if (s == "searchInputIntercept") m |= XPC_INPUTINTERCEPT;
        else if (s == "statusBar") m |= XPC_STATUSBAR;
        else if (s == "events") m |= XPC_EVENTS;
        else if (s == "preview") m |= XPC_PREVIEW;
        else if (s == "preview-panel") m |= XPC_PANEL;
        else if (s == "batchRename") m |= XPC_BATCHRENAME;
        /* 未识别能力忽略 (向前兼容) */
    }
    return m;
}
static unsigned XjsPmPerms(const picojson::value& v) {
    unsigned m = 0;
    if (!v.is<picojson::array>()) return 0;
    for (auto& e : v.get<picojson::array>()) {
        if (!e.is<std::string>()) continue;
        const std::string& s = e.get<std::string>();
        if (s == "file.read") m |= XPP_READ;
        else if (s == "file.write") m |= XPP_WRITE;
        else if (s == "exec") m |= XPP_EXEC;
        else if (s == "ui") m |= XPP_UI;
        else if (s == "settings") m |= XPP_SETTINGS;   /* 扩展 API: 设置读写/运行时搜索模式管理 */
    }
    return m;
}
static void XjsPmMenus(const picojson::value& v, std::vector<XjsPluginMenuDef>* out, bool withWhen) {
    out->clear();
    if (!v.is<picojson::array>()) return;
    for (auto& e : v.get<picojson::array>()) {
        if (!e.is<picojson::object>()) continue;
        const picojson::object& o = e.get<picojson::object>();
        XjsPluginMenuDef d;
        d.cmd = XjsPmStr(o, "标识");
        d.text = XjsPmStr(o, "文字");
        if (d.cmd.empty() || d.text.empty()) continue;
        d.order = XjsConfig::Int(o, "顺序", 0);
        if (withWhen) {
            std::wstring w = XjsPmStr(o, "时机");
            d.when = (w == L"file") ? 1 : (w == L"dir") ? 2 : (w == L"drive") ? 3 : 0;
            auto eit = o.find("扩展名");
            if (eit != o.end()) XjsPmStrArr(eit->second, &d.exts, true);
        }
        out->push_back(d);
    }
}

bool XjsPluginManifestParse(const char* utf8Json, XjsPluginManifest* out) {
    *out = XjsPluginManifest{};
    if (!utf8Json || !*utf8Json) { out->err = L"清单为空"; return false; }
    picojson::value v;
    if (!picojson::parse(v, utf8Json).empty()) { out->err = L"JSON 语法错误"; return false; }
    if (!v.is<picojson::object>()) { out->err = L"清单不是 JSON 对象"; return false; }
    const picojson::object& o = v.get<picojson::object>();
    out->id = XjsPmStr(o, "标识");
    out->name = XjsPmStr(o, "名称");
    out->version = XjsPmStr(o, "版本");
    out->author = XjsPmStr(o, "作者");
    out->description = XjsPmStr(o, "简介");
    out->iconFile = XjsPmStr(o, "图标");
    out->dllName = XjsPmStr(o, "动态库");
    out->abi = XjsConfig::Int(o, "接口版本", 0);
    std::wstring type = XjsPmStr(o, "类型");
    if (!type.empty() && type != L"library" && type != L"app") { out->err = L"类型 字段未知"; return false; }
    out->type = (type == L"app") ? 1 : 0;
    if (out->name.empty()) { out->err = L"缺 名称"; return false; }
    { auto it = o.find("能力"); out->caps = XjsPmCaps(it != o.end() ? it->second : XJS_PM_NULL); }
    { auto it = o.find("权限"); out->perms = XjsPmPerms(it != o.end() ? it->second : XJS_PM_NULL); }
    { auto it = o.find("菜单"); XjsPmMenus(it != o.end() ? it->second : XJS_PM_NULL, &out->menus, true); }
    { auto it = o.find("搜索框菜单"); XjsPmMenus(it != o.end() ? it->second : XJS_PM_NULL, &out->searchBoxMenus, false); }
    if (auto it = o.find("状态栏"); it != o.end() && it->second.is<picojson::array>()) {
        for (auto& e : it->second.get<picojson::array>()) {
            if (!e.is<picojson::object>()) continue;
            const picojson::object& so = e.get<picojson::object>();
            XjsPluginStatusBarDef d;
            d.cmd = XjsPmStr(so, "标识");
            d.label = XjsPmStr(so, "文字");
            if (d.cmd.empty() || d.label.empty()) continue;
            d.title = XjsPmStr(so, "提示");
            d.order = XjsConfig::Int(so, "顺序", 0);
            out->statusBar.push_back(d);
        }
    }
    if (auto it = o.find("搜索模式"); it != o.end() && it->second.is<picojson::array>()) {
        for (auto& e : it->second.get<picojson::array>()) {
            if (!e.is<picojson::object>()) continue;
            const picojson::object& mo = e.get<picojson::object>();
            XjsPluginModeDef d;
            d.id = XjsPmStr(mo, "标识");
            d.name = XjsPmStr(mo, "名称");
            if (d.id.empty() || d.name.empty()) continue;
            d.desc = XjsPmStr(mo, "简介");
            d.type = XjsPmStr(mo, "类型");
            d.tplUtf8 = Utf16ToUtf8(XjsPmStr(mo, "模板").c_str());   /* 空 = 接管型 (OnSearchMode) */
            out->modes.push_back(d);
        }
    }
    if (auto it = o.find("托管搜索"); it != o.end() && it->second.is<picojson::array>()) {
        for (auto& e : it->second.get<picojson::array>()) {
            if (!e.is<picojson::object>()) continue;
            const picojson::object& ho = e.get<picojson::object>();
            XjsPluginHostedDef d;
            d.id = XjsPmStr(ho, "标识");
            if (d.id.empty()) continue;
            auto wit = ho.find("词条");
            if (wit != ho.end()) XjsPmStrArr(wit->second, &d.words, false);
            if (d.words.empty()) continue;
            d.mode = XjsPmStr(ho, "模式");
            d.queryUtf8 = Utf16ToUtf8(XjsPmStr(ho, "查询").c_str());
            out->hosted.push_back(d);
        }
    }
    if (auto it = o.find("预览"); it != o.end() && it->second.is<picojson::array>()) {
        for (auto& e : it->second.get<picojson::array>()) {
            if (!e.is<picojson::object>()) continue;
            auto eit = e.get<picojson::object>().find("扩展名");
            if (eit != e.get<picojson::object>().end()) {
                std::vector<std::wstring> tmp;
                XjsPmStrArr(eit->second, &tmp, true);
                out->previewExts.insert(out->previewExts.end(), tmp.begin(), tmp.end());
            }
        }
    }
    out->ok = true;
    return true;
}

/* DialogJson 选项解析 (同样收口本文件): {"title":"..","filter":[["标签","*.png"],…],
   "initialDir":"..","initialName":".."} — 全部字段可选, 容忍缺键/类型不对 */
bool XjsPluginDialogOptsParse(const char* utf8Json, XjsPluginDialogOpts* out) {
    *out = XjsPluginDialogOpts{};
    if (!utf8Json || !*utf8Json) return false;
    picojson::value v;
    if (!picojson::parse(v, utf8Json).empty() || !v.is<picojson::object>()) return false;
    const picojson::object& o = v.get<picojson::object>();
    out->title = XjsPmStr(o, "title");
    out->initialDir = XjsPmStr(o, "initialDir");
    out->initialName = XjsPmStr(o, "initialName");
    if (auto it = o.find("filter"); it != o.end() && it->second.is<picojson::array>()) {
        for (auto& e : it->second.get<picojson::array>()) {
            if (!e.is<picojson::array>()) continue;
            const auto& pair = e.get<picojson::array>();
            if (pair.size() < 2 || !pair[0].is<std::string>() || !pair[1].is<std::string>()) continue;
            out->filter.push_back({ Utf8ToUtf16(pair[0].get<std::string>().c_str()),
                                    Utf8ToUtf16(pair[1].get<std::string>().c_str()) });
        }
    }
    return true;
}

void XjsConfig::Load() {
    m_obj.clear();
    std::wstring dir = XjsGetExeDir() + L"\\Config";
    CreateDirectoryW(dir.c_str(), NULL);   /* 已存在 = ERROR_ALREADY_EXISTS, 忽略 */
    std::wstring p = dir + L"\\xjs_config.json";
    /* 2026-09-22 配置收进 Config\ 子目录: 旧版根目录文件一次性搬入 (新路径缺席才搬, 纯改名不转格式) */
    std::wstring legacy = XjsGetExeDir() + L"\\xjs_config.json";
    if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(legacy.c_str()) != INVALID_FILE_ATTRIBUTES)
        MoveFileExW(legacy.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING);
    std::string utf8;
    if (!XjsReadUtf8File(p, &utf8))
        XjsReadUtf8File(legacy, &utf8);   /* 搬不动 (旧文件被占用) 回落直读旧路径, 首次落盘自然迁入 */
    picojson::value v;
    if (picojson::parse(v, utf8).empty() && v.is<picojson::object>())
        m_obj = v.get<picojson::object>();
}

void XjsConfig::WriteBack() {
    std::string utf8 = picojson::value(m_obj).serialize(true);   /* true = 格式化 (缩进) 输出 */
    std::wstring dir = XjsGetExeDir() + L"\\Config";
    CreateDirectoryW(dir.c_str(), NULL);   /* 目录被人为删除时自愈 (已存在 = 忽略) */
    std::wstring p = dir + L"\\xjs_config.json";
    /* 原子落盘 (同索引库 XjsEngineShutdown 口径): CREATE_ALWAYS 直写会在 CreateFile 成功瞬间
       截断旧文件, 写一半崩溃/断电 = 全部配置丢失且被启动期的默认骨架覆盖 —
       先写 .tmp, 成功后原子改名顶上; 中途崩溃最多残留 .tmp (启动加载前不清理配置 tmp,
       下次 WriteBack 的 CREATE_ALWAYS 会复用它), 旧配置始终完整 */
    std::wstring tmp = p + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    bool ok = (DWORD)utf8.size() == 0 ||
              (WriteFile(h, utf8.data(), (DWORD)utf8.size(), &wr, NULL) && wr == (DWORD)utf8.size());
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp.c_str()); return; }   /* 半成品丢弃, 旧配置保持不动 */
    if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING))
        DeleteFileW(tmp.c_str());   /* 替换失败删临时文件 (旧配置保持上一次的完整版) */
}

picojson::object& XjsConfig::Obj(const char* key) {
    auto it = m_obj.find(key);
    if (it == m_obj.end() || !it->second.is<picojson::object>()) {
        m_obj[key] = picojson::value(picojson::object());
        return m_obj[key].get<picojson::object>();
    }
    return it->second.get<picojson::object>();
}
int XjsConfig::Int(const picojson::object& o, const char* k, int def) {
    auto it = o.find(k);
    if (it == o.end() || !it->second.is<double>()) return def;
    return (int)it->second.get<double>();
}
bool XjsConfig::Bool(const picojson::object& o, const char* k, bool def) {
    auto it = o.find(k);
    if (it == o.end() || !it->second.is<bool>()) return def;
    return it->second.get<bool>();
}
std::wstring XjsConfig::Str(const picojson::object& o, const char* k, const wchar_t* def) {
    auto it = o.find(k);
    if (it == o.end() || !it->second.is<std::string>()) return def;
    return Utf8ToUtf16(it->second.get<std::string>().c_str());
}

/* ============ 内嵌 JSON 字符串解析 (声明在 xjs_app.h; JSON 类型不出本文件, 换 JSON 模块只改这里) ============ */

/* 解析字符串 JSON 数组 (排除目录列表等): 返回成员数 */
int XjsJsonStringArray(const char* json, std::vector<std::wstring>* out) {
    out->clear();
    if (!json || strlen(json) < 2) return 0;
    std::string copy = json;
    picojson::value v;
    if (!picojson::parse(v, copy).empty() || !v.is<picojson::array>()) return 0;
    for (auto& e : v.get<picojson::array>())
        if (e.is<std::string>()) out->push_back(Utf8ToUtf16(e.get<std::string>().c_str()));
    return (int)out->size();
}

bool XjsPluginJsonMembers(const char* utf8Json, std::vector<XjsJsonMember>* out) {
    /* 插件扩展 API 的顶层对象摊平 (settings.set / modes.add 入参): 只收顶层标量成员,
       嵌套数组/对象/null 忽略 (扩展 API 不吃嵌套) — picojson 细节不外泄本文件 */
    out->clear();
    if (!utf8Json || !*utf8Json) return false;
    picojson::value v;
    if (!picojson::parse(v, utf8Json).empty() || !v.is<picojson::object>()) return false;
    for (auto& kv : v.get<picojson::object>()) {
        XjsJsonMember m;
        m.key = Utf8ToUtf16(kv.first.c_str());
        const picojson::value& e = kv.second;
        if (e.is<bool>()) { m.type = 1; m.b = e.get<bool>(); }
        else if (e.is<double>()) { m.type = 2; m.num = e.get<double>(); }
        else if (e.is<std::string>()) { m.type = 3; m.str = Utf8ToUtf16(e.get<std::string>().c_str()); }
        else continue;
        out->push_back(m);
    }
    return true;
}

/* ==================== 文件分类 (引擎筛选器) / 路径别名 配置 ====================
 * 设置窗表格编辑后整体下发引擎 (正式版同款交互); 数据串格式 = 引擎口径
 * (筛选器 [{"名称":"..","类型":99,"后缀":"EXE,BAT"}], 别名 {"完整路径":"别名"})。
 * picojson 序列化负责转义; Apply 成功才更新保存串并落盘 (引擎拒绝 = 原配置不动)。 */

static std::string XjsFilterJsonMake(const std::vector<XjsFilterItem>& rows) {
    picojson::array arr;
    for (auto& r : rows) {
        picojson::object o;
        o["名称"] = picojson::value(Utf16ToUtf8(r.name.c_str()));
        o["类型"] = picojson::value((double)r.type);
        o["后缀"] = picojson::value(Utf16ToUtf8(r.ext.c_str()));
        arr.push_back(picojson::value(o));
    }
    return picojson::value(arr).serialize();
}

static std::string XjsAliasJsonMake(const std::vector<XjsAliasItem>& rows) {
    picojson::object o;   /* picojson::object = map, 键序经排序; 引擎不依赖行序 */
    for (auto& r : rows)
        o[Utf16ToUtf8(r.path.c_str())] = picojson::value(Utf16ToUtf8(r.alias.c_str()));
    return picojson::value(o).serialize();
}

static bool XjsFilterJsonParse(const char* utf8, std::vector<XjsFilterItem>* out) {
    out->clear();
    if (!utf8 || strlen(utf8) < 2) return false;
    std::string copy = utf8;
    picojson::value v;
    if (!picojson::parse(v, copy).empty() || !v.is<picojson::array>()) return false;
    const std::string kN = Utf16ToUtf8(L"名称"), kT = Utf16ToUtf8(L"类型"), kE = Utf16ToUtf8(L"后缀");
    for (auto& e : v.get<picojson::array>()) {
        if (!e.is<picojson::object>()) continue;
        auto& o = e.get<picojson::object>();
        XjsFilterItem it;
        auto n = o.find(kN);
        if (n != o.end() && n->second.is<std::string>()) it.name = Utf8ToUtf16(n->second.get<std::string>().c_str());
        auto t = o.find(kT);
        if (t != o.end() && t->second.is<double>()) it.type = (int)t->second.get<double>();
        auto x = o.find(kE);
        if (x != o.end() && x->second.is<std::string>()) it.ext = Utf8ToUtf16(x->second.get<std::string>().c_str());
        out->push_back(it);
    }
    return true;
}

static bool XjsAliasJsonParse(const char* utf8, std::vector<XjsAliasItem>* out) {
    out->clear();
    if (!utf8 || strlen(utf8) < 2) return false;
    std::string copy = utf8;
    picojson::value v;
    if (!picojson::parse(v, copy).empty() || !v.is<picojson::object>()) return false;
    for (auto& kv : v.get<picojson::object>())
        if (kv.second.is<std::string>())
            out->push_back({ Utf8ToUtf16(kv.first.c_str()), Utf8ToUtf16(kv.second.get<std::string>().c_str()) });
    return true;
}

bool XjsFilterConfigApply(const std::vector<XjsFilterItem>& rows) {
    if (!g_engine) return false;
    std::string json = XjsFilterJsonMake(rows);
    /* sync=FALSE (正式版口径): 仅对之后入库的文件生效, 已入库文件的分类需重建索引才重算 */
    if (xjs_filter_SetFilterJSON(g_engine, json.c_str(), FALSE) == FALSE) return false;
    g_savedFilterJson = json;
    XjsSaveConfig();
    return true;
}

bool XjsAliasConfigApply(const std::vector<XjsAliasItem>& rows) {
    if (!g_engine) return false;
    std::string json = XjsAliasJsonMake(rows);
    /* sync=TRUE (正式版口径 "保存后立即生效"): 命中新配置且当前无别名的已入库行立即写入;
       大库同步遍历可能耗时数百毫秒~数秒, 遍历/保存/加载期间引擎拒绝 (35) */
    if (xjs_alias_SetAliasJSON(g_engine, json.c_str(), TRUE) == FALSE) return false;
    g_savedAliasJson = json;
    XjsSaveConfig();
    return true;
}

void XjsFilterConfigLoad(std::vector<XjsFilterItem>* rows) {
    rows->clear();
    if (!g_engine) return;
    std::string cur = xjs_filter_GetFilterJSON(g_engine);   /* 内部管理指针, 第一时间拷贝 */
    XjsFilterJsonParse(cur.c_str(), rows);
}

void XjsAliasConfigLoad(std::vector<XjsAliasItem>* rows) {
    rows->clear();
    if (!g_engine) return;
    std::string cur = xjs_alias_GetAliasJSON(g_engine);   /* 路径已展开为绝对路径, 回显真实生效值 */
    XjsAliasJsonParse(cur.c_str(), rows);
}

void XjsEngineApplySavedConfigs() {
    if (!g_engine) return;
    /* sync=FALSE: 行数据要么此前已应用过别名/分类 (加载的库), 要么正随扫描入库时套用当前配置 */

    /* 筛选器默认源 (别名同款优先链): 配置 "文件分类" 键 (设置页保存/首次播种后才有) 优先;
       键空回落 exe 目录 Config\ 子目录随包发布的 Filter.json 内置词典 —— 原文透传, 引擎解析器
       自理该宽松 JSONC; 文件缺席 = 跳过 (引擎自动用内置默认表)。程序不写回该文件 */
    std::string filterJson = g_savedFilterJson;
    bool filterFromFile = filterJson.empty();
    if (filterFromFile) XjsReadUtf8File(XjsGetExeDir() + L"\\Config\\Filter.json", &filterJson);
    if (!filterJson.empty() && xjs_filter_SetFilterJSON(g_engine, filterJson.c_str(), FALSE)) {
        /* 首次播种追加 sync=TRUE (别名同款): 一次性回填已入库行的分类; 35=忙 只废播种不废
           配置, 下次启动重试。成功即把词典原文存进 "文件分类" 键 —— 此后键非空不再走文件,
           一次全库回填的成本不逐启动重付 */
        if (filterFromFile && xjs_filter_SetFilterJSON(g_engine, filterJson.c_str(), TRUE)) {
            g_savedFilterJson = filterJson;
            XjsSaveConfig();
        }
    }
    /* 别名默认源 (正式版同款优先链): 配置 "路径别名" 键 (设置页保存后才有) 优先;
       键空回落 exe 目录 Config\ 子目录随包发布的 Alias.json 内置词典 —— 原文透传, 引擎解析器直接
       吃该宽松 JSONC (<系统盘>/<用户名> 占位符、/ 正斜杠); 文件缺席 = 跳过 (正式版同)。程序不写回该文件 */
    std::string alias = g_savedAliasJson;
    bool fromFile = alias.empty();
    if (fromFile) XjsReadUtf8File(XjsGetExeDir() + L"\\Config\\Alias.json", &alias);
    if (alias.empty()) return;
    if (!xjs_alias_SetAliasJSON(g_engine, alias.c_str(), FALSE)) return;   /* 引擎拒绝 = 配置不下发, 下次启动重试 */
    if (!fromFile) return;
    /* 首次播种追加 sync=TRUE: 一次性回填已入库且"无别名"的行 (正式版设置页保存同款; 否则旧库
       必须重建索引才见别名)。35=忙 (扫描/保存并发) 只废回填不废配置, 下次启动重试;
       成功即把词典原文存进 "路径别名" 键 —— 此后键非空不再走文件 (正式版以回写后的
       Alias.json 为源, 等效), 全库遍历的一次性成本也不逐启动重付 */
    if (xjs_alias_SetAliasJSON(g_engine, alias.c_str(), TRUE)) {
        g_savedAliasJson = alias;
        XjsSaveConfig();
    }
}

/* 询问框按钮数组 [{"text":"..","style":"default|primary|danger"},..] → 样式化按钮; 无有效按钮回退单个"确定" */
int XjsParseDialogButtons(const char* buttonsJson, XjsAskBtnDef* out, int cap) {
    int n = 0;
    picojson::value v;
    if (picojson::parse(v, buttonsJson ? buttonsJson : "").empty() && v.is<picojson::array>()) {
        for (auto& e : v.get<picojson::array>()) {
            if (!e.is<picojson::object>() || n >= cap) break;
            auto& o = e.get<picojson::object>();
            XjsAskBtnDef b;
            auto t = o.find("text");
            if (t != o.end() && t->second.is<std::string>()) b.text = Utf8ToUtf16(t->second.get<std::string>().c_str());
            auto st = o.find("style");
            if (st != o.end() && st->second.is<std::string>()) {
                const std::string& sv = st->second.get<std::string>();
                if (sv == "primary") b.style = 1;
                else if (sv == "danger") b.style = 2;
            }
            if (!b.text.empty()) out[n++] = std::move(b);
        }
    }
    if (n == 0 && cap > 0) out[n++] = { XjsT(L"通用词.确定"), 1 };
    return n;
}

/* 引擎搜索失败串 → 人类可读消息: 串是 JSON 对象时取 "错误信息" 键, 否则按原文直转 */
std::wstring XjsEngineErrText(const std::string& errUtf8) {
    picojson::value v;
    if (picojson::parse(v, errUtf8).empty() && v.is<picojson::object>()) {
        auto& obj = v.get<picojson::object>();
        auto it = obj.find(Utf16ToUtf8(L"错误信息"));
        if (it != obj.end() && it->second.is<std::string>())
            return Utf8ToUtf16(it->second.get<std::string>().c_str());
    }
    return Utf8ToUtf16(errUtf8.c_str());
}

/* ==================== 搜索历史 (每窗) ==================== */
void XjsSaveHistory() {
    /* 搜索历史每窗 (2026-09-17): g_history 宏 = 当前窗 history, 已由 XjsSaveConfig 的档案收集
       刷进所属窗口条目的 "搜索历史" — 这里整体落盘一次 (历史增改频繁, 档案收集顺带覆盖) */
    XjsSaveConfig();
}

void XjsAddHistory(const std::wstring& text) {
    if (text.empty()) return;
    auto it = std::find(g_history.begin(), g_history.end(), text);
    if (it != g_history.end()) g_history.erase(it);
    g_history.insert(g_history.begin(), text);
    if (g_history.size() > MAX_SEARCH_HISTORY) g_history.resize(MAX_SEARCH_HISTORY);
    XjsSaveHistory();
}

/* ==================== XjsColumnSet: 列 JSON 存取 (配置持久化, 键名同源样式 columns.*) ====================
 * 本头文件不见 JSON 类型, 存取做成配置层的文件内静态函数 (声明曾挂在 XjsColumnSet 上, 2026-09-17 收口) */
static void XjsColsApplyJson(XjsColumnSet& cs, picojson::object& cols, const char* orderKey,
                             const char* wKey, const char* flexKey, const char* vKey) {
    auto oit = cols.find(orderKey);
    if (oit != cols.end() && oit->second.is<picojson::array>()) {
        const picojson::array& a = oit->second.get<picojson::array>();
        bool used[cs.MAX] = { false };
        XjsColSpec tmp[cs.MAX];
        int m = 0;
        for (auto& e : a) {
            if (!e.is<std::string>() || m >= cs.n) continue;
            for (int i = 0; i < cs.n; i++) {
                if (!used[i] && e.get<std::string>() == cs.arr[i].sortField) {
                    tmp[m++] = cs.arr[i];
                    used[i] = true;
                    break;
                }
            }
        }
        for (int i = 0; i < cs.n && m < cs.n; i++)
            if (!used[i]) tmp[m++] = cs.arr[i];
        for (int i = 0; i < cs.n; i++) cs.arr[i] = tmp[i];
    }
    auto wit = cols.find(wKey);
    if (wit != cols.end() && wit->second.is<picojson::object>()) {
        const picojson::object& wmap = wit->second.get<picojson::object>();
        for (int i = 0; i < cs.n; i++) {
            auto w = wmap.find(cs.arr[i].sortField);
            if (w != wmap.end() && w->second.is<double>()) {
                int v = (int)w->second.get<double>();
                if (v >= 50 && v <= 800) cs.arr[i].width = v;
            }
        }
    }
    auto fitx = cols.find(flexKey);
    if (fitx != cols.end() && fitx->second.is<picojson::object>()) {
        const picojson::object& fmap = fitx->second.get<picojson::object>();
        for (int i = 0; i < cs.n; i++) {
            auto f = fmap.find(cs.arr[i].sortField);
            if (f != fmap.end() && f->second.is<bool>()) cs.arr[i].flex = f->second.get<bool>();
        }
    }
    auto vit = cols.find(vKey);
    if (vit != cols.end() && vit->second.is<picojson::object>()) {
        const picojson::object& vmap = vit->second.get<picojson::object>();
        for (int i = 0; i < cs.n; i++) {
            auto f = vmap.find(cs.arr[i].sortField);
            if (f != vmap.end() && f->second.is<bool>())
                cs.arr[i].visible = f->second.get<bool>();
        }
    }
}

static void XjsColsSaveJson(const XjsColumnSet& cs, picojson::object& out, const char* orderKey,
                            const char* wKey, const char* flexKey, const char* vKey) {
    picojson::array od;
    for (int i = 0; i < cs.n; i++) od.push_back(picojson::value(cs.arr[i].sortField));
    out[orderKey] = picojson::value(od);
    picojson::object wd, fd, vd;
    for (int i = 0; i < cs.n; i++) {
        wd[cs.arr[i].sortField] = picojson::value((double)cs.arr[i].width);
        fd[cs.arr[i].sortField] = picojson::value(cs.arr[i].flex);
        vd[cs.arr[i].sortField] = picojson::value(cs.arr[i].visible);
    }
    out[wKey] = picojson::value(wd);
    out[flexKey] = picojson::value(fd);
    out[vKey] = picojson::value(vd);
}

/* 把一个存活窗口的界面设置刷进它的档案槽 (保存时调用; 关窗的槽不动 → 档案持久保留,
   ☰菜单"创建新窗口"启动器可按未打开的槽重建窗口) */
static void XjsCollectUiProfileInto(XjsSearchWindow* w, XjsUiProfile& p) {
    p.skin = w->skinName;
    p.name = w->name;
    p.viewMode = w->viewMode;
    p.mode = w->mode;         /* 搜索模式 (每窗, 2026-09-18 每窗化) */
    p.previewVisible = w->previewVisible;
    p.previewWidth = w->previewWidth;
    p.openElevated = w->openElevated;
    p.openAsync = w->openAsync;
    p.openHideWindow = w->openHideWindow;
    p.hotkeyMod = w->hotkeyMod;
    p.hotkeyVk = w->hotkeyVk;
    p.match = w->match;
    p.history = w->history;   /* 搜索历史 (每窗) */
    p.driveProgress = w->driveProgress;
    p.rowHover = w->rowHover;
    p.rowHoverFade = w->rowHoverFade;
    /* 窗口行为 (每窗): 失焦动作/激活位置/标题栏与状态栏可见性/任务栏图标/置顶 */
    p.blurAction = w->blurAction;
    p.appearPos = w->appearPos;
    p.showCtrlBtns = w->showCtrlBtns;
    p.showFilterBox = w->showFilterBox;
    p.showStatusbar = w->showStatusbar;
    p.taskbarIcon = w->taskbarIcon;
    p.topmost = w->topmost;
    p.mouseOpen = w->mouseOpen;
    p.uiZoom = w->uiZoom;
    p.lang = w->lang;   /* 界面语言 (每窗) */
    p.defaultSel = w->defaultSel;
    p.createFill = w->createFill;
    p.createKeyword = w->createKeyword;
    p.lastSearch = w->searchEd.text;   /* 上一次输入的搜索词 (createFill=2 的记忆源) */
    if (w->result) {   /* 排序态事实源 = 结果对象 (宿主无影子状态); 档案只存初始默认 */
        const char* sf = xjs_result_GetSortField(w->result);
        p.sortField = sf ? Utf8ToUtf16(sf) : std::wstring();
        p.sortWay = xjs_result_GetSortway(w->result) != FALSE;
    }
    /* 窗口矩形 (每窗私有, 2026-09-17 用户口径: 尺寸不全局统一 — 主窗槽 0 同样入档):
       只在正常态采集 — 最小化矩形是 (-32000) 幻影坐标, 最大化矩形还原成普通窗会占满整屏;
       DPI 取被采集窗口自己的 w->dpiS (g_s 宏解析到"当前窗", 采集别窗时会拿错) */
    if (w->hWnd && !IsIconic(w->hWnd) && !IsZoomed(w->hWnd)) {
        RECT r;
        if (GetWindowRect(w->hWnd, &r)) {
            p.rectValid = true;
            p.rx = r.left; p.ry = r.top;
            p.rw = r.right - r.left; p.rh = r.bottom - r.top;
            p.rdpi = (int)(w->dpiS * 96.0f + 0.5f);   /* 矩形所属 DPI, 恢复时折算 */
        }
    }
    p.colsDetails = w->colsDetails;
    p.colsList = w->colsList;
}

/* ==================== 搜索匹配设置 (源样式 搜索匹配 组) ====================
 * 键名与 xjs_result_SetSearchSettings 中文键一致; 设置仅作用于结果对象 → 每窗一份
 * (窗口 match 字段, 随 uiWindows 档案持久化), 结果对象创建时 + 设置切换时下发本窗。 */
void XjsApplyMatchSettingsFor(XjsSearchWindow* w) {
    if (!w) return;
    picojson::object o;
    o["支持首拼"] = picojson::value(w->match.pinyinInitial);
    o["支持全拼"] = picojson::value(w->match.pinyinFull);
    o["拼音完整匹配"] = picojson::value(w->match.pinyinExact);
    o["支持星号"] = picojson::value(w->match.wildcardStar);
    o["支持问号"] = picojson::value(w->match.wildcardQuestion);
    o["匹配全角"] = picojson::value(w->match.matchFullWidth);
    o["区分大小写"] = picojson::value(w->match.caseSensitive);
    o["搜索词为空时显示所有文件"] = picojson::value(w->match.emptyShowsAll);
    if (w->result) {
        std::string json = picojson::value(o).serialize();
        xjs_result_SetSearchSettings(w->result, json.c_str());
    }
}

/* ==================== 用户自定义搜索模式 (源样式 11-search-modes.js 同构) ====================
 * {id,name,desc,type,template} 存 xjs_config.json customSearchModes, 上限 100, 新模式头部插入。
 * 执行 = 模板提交 (其中 <keyword> 占位符替换为搜索框纯输入文字, 见 XjsCmExpandTpl);
 * 搜索框有未被占位符消费的输入词时 = [模板阶段 + 输入词阶段(当前全局模式)]
 * 引擎多重搜索 (XJS_KEYWORD_MULTI, 链式过滤, 源样式 execHostedChain 同语义)。 */
std::vector<XjsCustomMode> g_customModes;

/* 作用范围 (2026-09-17): 0=全局共享(全部窗口) / 1=仅本窗口(ownerName=窗口档案名)。
   绑定按档案名持久 — 关窗重开仍生效; 窗口改名 XjsCmRenameWindow 随迁, 删档 XjsCmPurgeWindow 连带删 */
bool XjsCmApplies(const XjsCustomMode& cm, const std::wstring& winName) {
    return cm.scope != 1 || cm.ownerName == winName;
}

int XjsCmWindowCount(const std::wstring& winName) {
    int n = 0;
    for (auto& cm : g_customModes)
        if (cm.scope == 1 && cm.ownerName == winName) n++;
    return n;
}

int XjsCmPurgeWindow(const std::wstring& winName) {
    int n = 0;
    for (size_t i = g_customModes.size(); i-- > 0;)
        if (g_customModes[i].scope == 1 && g_customModes[i].ownerName == winName) {
            g_customModes.erase(g_customModes.begin() + i);
            n++;
        }
    return n;
}

void XjsCmRenameWindow(const std::wstring& oldName, const std::wstring& newName) {
    for (auto& cm : g_customModes)
        if (cm.scope == 1 && cm.ownerName == oldName) cm.ownerName = newName;
}

static const wchar_t* XjsCmStageMode(const std::wstring& type) {
    if (type == L"regex") return L"正则";
    if (type == L"sql") return L"SQL";
    if (type == L"lua") return L"Lua";
    return L"通配符";
}

static int XjsCmKeyword(const std::wstring& type) {
    if (type == L"regex") return XJS_KEYWORD_REGEX;
    if (type == L"sql") return XJS_KEYWORD_SQL;
    if (type == L"lua") return XJS_KEYWORD_LUA;
    if (type == L"lua-exec") return XJS_KEYWORD_LUA_EXEC;   /* 单发路径; 不入标签链 (见 XjsHostedWordAsTag) */
    return XJS_KEYWORD_WILDCARD;
}

static const wchar_t* XjsCmStageModeByG(int mode) {   /* 当前全局模式 → 阶段"搜索模式"串
   (Lua 执行是"脚本即整个搜索", 不是谓词, 进不了多重搜索链 — 链尾阶段归一 Lua 过滤) */
    static const wchar_t* const N[5] = { L"通配符", L"正则", L"SQL", L"Lua", L"Lua" };
    return (mode >= 0 && mode < 5) ? N[mode] : L"通配符";
}

/* ==================== 搜索框托管标签链 (源样式 26-hosted-search.js 一比一) ====================
 * 标签来源 = 用户自定义搜索模式 + 插件模板型模式 (统一视图 XjsModeViewById, 见上一节;
 * 源样式插件来源即 collectHostedSources 前段)。
 * 搜索 = 各标签(按加入顺序, 各取当前来源, 模板 <keyword> 已替换为框内纯文字) + 输入文字尾阶段
 * (非空且未被占位符消费时, 按当前全局模式) 组成:
 *   单阶段直接执行 (标签=模板按类型显式指定模式; 尾阶段=普通搜索);
 *   多阶段组引擎多重搜索 JSON ([{"搜索模式":"..","搜索词":".."}..], XJS_KEYWORD_MULTI 链式过滤)。
 * 接管语义: 不记搜索历史/不进导航栈 (不经 XjsSearchNow(true) 路径); 任何未命中托管的普通搜索
 * 自动清空标签 (标签只代表"搜索框处于托管搜索状态")。来源只存模式 id, 执行/显示时活取 ——
 * 编辑模式后标签按新模板重搜生效; 模式被删 → 来源剔除, 全失效标签在链中跳过 (源样式 hostedPruneSources)。 */
static const XjsCustomMode* XjsCmById(const std::wstring& id) {
    for (auto& cm : g_customModes)
        if (cm.id == id) return &cm;
    return NULL;
}

/* ==================== 统一模式来源 (用户自定义 + 插件清单模式, 2026-09-19 插件系统) ====================
 * 托管标签链的"来源"不区分提供方: 用户模式 id 原样; 插件模式 id = "p:<插件id>:<模式序>"
 * (插件 id = 目录名, 与注册表下标无关 — 插件被删/禁用时解析失败, Prune 自然剔除该来源)。
 * 模板型插件模式 (有 template) = 零代码, 与用户模式同一执行路径; 无模板 = 接管型 (OnSearchMode
 * 由插件自行驱动, 不入标签链)。下面的视图解析是托管链/菜单/点击三处唯一的合并遍历点。 */
static bool XjsPluginSrcParse(const std::wstring& srcId, std::wstring* pluginId, int* modeIdx) {
    if (srcId.rfind(L"p:", 0) != 0) return false;
    size_t colon = srcId.rfind(L':');
    if (colon == std::wstring::npos || colon < 2) return false;
    *pluginId = srcId.substr(2, colon - 2);
    *modeIdx = _wtoi(srcId.c_str() + colon + 1);
    return true;
}

std::wstring XjsPluginModeSrcId(const std::wstring& pluginId, int modeIdx) {
    return L"p:" + pluginId + L":" + std::to_wstring(modeIdx);
}

/* srcId → 模式视图 (用户模式查表; 插件模式按"启用且可用"现取值拷贝)。
   false = 来源已失效 (模式被删 / 插件被禁用或移除), Prune 据此剔除 */
bool XjsModeViewById(const std::wstring& srcId, XjsCustomMode* out) {
    if (const XjsCustomMode* cm = XjsCmById(srcId)) { *out = *cm; return true; }
    std::wstring pid; int mi = -1;
    if (!XjsPluginSrcParse(srcId, &pid, &mi)) return false;
    for (int i = 0; i < XjsPluginCount(); i++) {
        XjsPluginBrief b;
        if (!XjsPluginBriefAt(i, &b) || b.id != pid) continue;
        const XjsPluginModeDef* d = XjsPluginModeDefAt({ i, mi });
        if (!d) return false;
        out->id = srcId;
        out->name = d->name;
        out->desc = d->desc;
        out->type = d->type.empty() ? L"wildcard" : d->type;
        out->tpl = Utf8ToUtf16(d->tplUtf8.c_str());
        out->scope = 0;
        return true;
    }
    return false;
}

/* 接管型插件模式触发 (药丸菜单点击): 按 srcId 定位 → 插件 OnSearchMode */
void XjsPluginFireSearchModeBySrc(const std::wstring& srcId, const std::wstring& input) {
    std::wstring pid; int mi = -1;
    if (!XjsPluginSrcParse(srcId, &pid, &mi)) return;
    for (int i = 0; i < XjsPluginCount(); i++) {
        XjsPluginBrief b;
        if (!XjsPluginBriefAt(i, &b) || b.id != pid) continue;
        XjsPluginModeRef r{ i, mi };
        if (!XjsPluginModeDefAt(r)) return;
        XjsPluginFireSearchMode(r, XjsPluginCurWindowToken(), input);
        return;
    }
}

/* 模板 <keyword> 占位符 (大小写不敏感) 全量展开为搜索框纯输入文字 (已 trim, 不含托管标签 ——
   标签词只存 g_hostedTags, 不在搜索框文本里); found 回写"模板是否含占位符" (= 输入文字已由模板消费)。 */
static std::wstring XjsCmExpandTpl(const std::wstring& tpl, const std::wstring& kw, bool* found) {
    static const wchar_t* const TOKEN = L"<keyword>";
    const size_t tn = wcslen(TOKEN);
    if (found) *found = false;
    std::wstring out;
    out.reserve(tpl.size() + kw.size());
    for (size_t i = 0; i < tpl.size();) {
        if (i + tn <= tpl.size() && _wcsnicmp(tpl.c_str() + i, TOKEN, tn) == 0) {
            out += kw;
            i += tn;
            if (found) *found = true;
        } else {
            out += tpl[i++];
        }
    }
    return out;
}

int XjsHostedPrune() {
    int changed = 0;
    /* 作用范围过滤: 窗口专属模式在别的窗口视同已删 (编辑模式改作用域后, 他窗标签由此修剪);
       插件来源 (p:<id>:<序>) 已禁用/已删 → 视图解析失败, 同口径剔除 */
    const std::wstring win = XjsSearchWindow::Cur()->name;
    for (auto& t : g_hostedTags) {
        for (size_t s = t.srcIds.size(); s-- > 0;) {
            XjsCustomMode v;
            if (!XjsModeViewById(t.srcIds[s], &v) || !XjsCmApplies(v, win)) {
                t.srcIds.erase(t.srcIds.begin() + s);
                changed = 1;
            }
        }
        if (t.active >= (int)t.srcIds.size()) t.active = 0;
    }
    return changed;
}

/* 提交一次查询 (快照→Query→防抖状态机, 同 XjsSearchNow 口径); titleWord = 窗口标题词
   (源样式 state.lastKeyword = 尾阶段词 || 首标签词) */
static void XjsHostedCommit(const std::string& kw, int kwType, const std::wstring& titleWord) {
    if (!g_searching.load()) XjsDebounceSnapshot();
    int fingerprint = xjs_result_Query(g_result, kw.c_str(), kwType, FALSE);
    if (fingerprint == -1) return;
    g_searching.store(true);
    g_searchFingerprint = fingerprint;
    g_errText.clear();
    /* 引擎在新查询提交时自行清空选中 (搜索结果.清空) — 宿主只清索引类 UI 状态 */
    g_anchorIdx = g_focusIdx = -1;
    g_cutSet.clear();
    g_hoverTraces.clear();   /* 结果集即将换血: 按行下标记忆的拖尾高亮立即作废 (防重影) */
    g_scrollTop = 0;
    if (g_hWnd) {
        SetTimer(g_hWnd, ID_TIMER_SEARCHSTATUS, 200, NULL);   /* 状态栏防闪: 200ms 未完成才显"正在搜索…" */
        SetWindowTextW(g_hWnd, titleWord.empty() ? XjsT(L"应用.名称") : (titleWord + L" - " + XjsT(L"应用.名称")).c_str());
    }
    XjsPreviewUpdateSelection();
}

void XjsHostedExecChain(const std::wstring& tailKw) {
    if (!g_engine || g_isScanning) return;
    if (!XjsEngineEnsureResult()) return;
    XjsHostedPrune();   /* 执行前懒清理已删除的模式来源 (源样式 execHostedChain) */
    const std::wstring tail = XjsTrimWs(tailKw);   /* 搜索框纯输入文字 (不含托管标签) */
    /* 组阶段: 来源全失效的标签跳过 (不中断链, 源样式同口径); 顺带展开各模板的 <keyword> 占位符 */
    std::vector<int> tagStages;
    std::vector<std::wstring> stageQueries;   /* 与 tagStages 同序: 占位符已替换的搜索词 */
    bool tplConsumesBox = false;              /* 链中任一模板含 <keyword> = 输入文字已由模板消费 */
    for (int i = 0; i < (int)g_hostedTags.size(); i++) {
        const XjsHostedTag& t = g_hostedTags[i];
        if (t.srcIds.empty()) continue;
        XjsCustomMode v;
        if (!XjsModeViewById(t.srcIds[t.active], &v)) continue;   /* 来源失效 (prune 后不会出现, 同原多阶段跳过口径) */
        bool found = false;
        stageQueries.push_back(XjsCmExpandTpl(v.tpl, tail, &found));
        tagStages.push_back(i);
        if (found) tplConsumesBox = true;
    }
    /* 输入文字已被占位符消费时不再追加为链尾阶段 —— 否则内容搜索类模板后面还会叠一层同名文件名过滤,
       结果反而漏文件 (例: "搜索文件内容"标签 + 框内 "报表" 只该查内容, 不该再要求文件名含"报表") */
    const bool hasTail = !tail.empty() && !tplConsumesBox;
    if (tagStages.empty() && !hasTail) {
        /* 标签全部失效: 清标签回普通搜索 (源样式 clearHostedTags + doSearch 分支) */
        XjsHostedClearTags();
        if (g_resultCount > 0) XjsSearchNow(false);
        XjsSearchWindow::Cur()->Invalidate();
        return;
    }
    std::wstring titleWord = !tail.empty() ? tail : g_hostedTags[tagStages[0]].word;   /* 框内文字(含被模板消费的)优先 */
    if (tagStages.size() == 1 && !hasTail) {
        /* 单标签直接执行: 模板(占位符已替换)按类型显式指定 (全局模式可能是通配符, 空参会让 SQL 模板按通配符跑) */
        XjsCustomMode v;
        if (XjsModeViewById(g_hostedTags[tagStages[0]].srcIds[g_hostedTags[tagStages[0]].active], &v)) {
            XjsHostedCommit(Utf16ToUtf8(stageQueries[0].c_str()), XjsCmKeyword(v.type), titleWord);
            XjsSearchWindow::Cur()->Invalidate();
        }
        return;
    }
    if (tagStages.empty() && hasTail) {
        /* 仅尾阶段: 等价普通搜索 (按当前全局模式) */
        XjsHostedCommit(Utf16ToUtf8(tail.c_str()), g_modeToKeyword[g_mode], titleWord);
        XjsSearchWindow::Cur()->Invalidate();
        return;
    }
    /* 多阶段: 组引擎多重搜索 JSON, 链式过滤 (先搜 A 再以 A 结果为基础搜 B) */
    picojson::array arr;
    for (int i = 0; i <= (int)tagStages.size(); i++) {
        picojson::object o;
        if (i == (int)tagStages.size()) {
            if (!hasTail) break;
            o["搜索模式"] = picojson::value(Utf16ToUtf8(XjsCmStageModeByG(g_mode)));
            o["搜索词"] = picojson::value(Utf16ToUtf8(tail.c_str()));
        } else {
            XjsHostedTag& t = g_hostedTags[tagStages[i]];
            XjsCustomMode v;
            if (!XjsModeViewById(t.srcIds[t.active], &v)) continue;
            o["搜索模式"] = picojson::value(Utf16ToUtf8(XjsCmStageMode(v.type)));
            o["搜索词"] = picojson::value(Utf16ToUtf8(stageQueries[i].c_str()));   /* 模板 (占位符已替换) */
        }
        arr.push_back(picojson::value(o));
    }
    XjsHostedCommit(picojson::value(arr).serialize(), XJS_KEYWORD_MULTI, titleWord);
    XjsSearchWindow::Cur()->Invalidate();
}

/* 输入词命中托管显示词 → 转标签接管 (源样式 tryHostedSearch): 词已是链中标签 (忽略大小写) 时放行,
   按纯文本作尾阶段 (再次输入同词 = 标签 AND 该词) */
bool XjsHostedTrySearch(const std::wstring& keyword) {
    std::wstring k = XjsTrimWs(keyword);
    if (k.empty()) return false;
    for (auto& t : g_hostedTags)
        if (_wcsicmp(t.word.c_str(), k.c_str()) == 0) return false;
    return XjsHostedWordAsTag(k, true);   /* 源样式 doSearch 先 trim 再转标签, 标签词不带首尾空格 */
}

/* 词转托管标签 (输入命中 clearInput=真 / 模式菜单点击 clearInput=假 共用; 源样式 hostWordAsTag):
 * 收集同名来源 (模式名 trim 后忽略大小写, 多个合并为一个标签多来源); 已有同名标签刷新来源集合
 * (尽量保留当前选中来源)。clearInput=真: 输入词已被消费成标签, 清空输入框并聚焦, 链尾为空;
 * 假: 模式菜单点击语义, 保留输入框词作为链尾 (标签链 + 输入词 组成多重搜索)。 */
bool XjsHostedWordAsTag(const std::wstring& word, bool clearInput) {
    if (word.empty()) return false;
    std::wstring key = XjsTrimWs(word);
    if (key.empty()) return false;
    std::wstring keyLower = key;
    _wcslwr_s(&keyLower[0], keyLower.size() + 1);
    std::vector<std::wstring> srcIds;
    XjsCustomMode execMode;                       /* Lua 执行类型 (type="lua-exec"): 不入链, 命中即单发 */
    bool hasExec = false;
    const std::wstring win = XjsSearchWindow::Cur()->name;   /* 窗口专属模式仅其所属窗命中 (作用范围) */
    for (auto& cm : g_customModes) {
        if (!XjsCmApplies(cm, win)) continue;
        std::wstring n = XjsTrimWs(cm.name);
        if (n.empty()) continue;
        _wcslwr_s(&n[0], n.size() + 1);
        if (n == keyLower) {
            if (cm.type == L"lua-exec") {
                if (!hasExec) { execMode = cm; hasExec = true; }   /* 同名多条取第一条 */
                continue;   /* 执行型来源不参与标签链; 同名混有普通模式时标签只收普通来源 */
            }
            srcIds.push_back(cm.id);
        }
    }
    /* 插件模板型模式并入来源 (同名合并一个标签多来源, 与用户模式同链; 接管型不入链) */
    for (int pm = 0; pm < XjsPluginModeCount(); pm++) {
        XjsPluginModeRef r;
        if (!XjsPluginModeAt(pm, &r)) break;
        const XjsPluginModeDef* d = XjsPluginModeDefAt(r);
        if (!d || d->tplUtf8.empty()) continue;
        std::wstring n = XjsTrimWs(d->name);
        if (n.empty()) continue;
        _wcslwr_s(&n[0], n.size() + 1);
        if (n != keyLower) continue;
        if (d->type == L"lua-exec") continue;   /* 执行型不入链 (与用户模式同口径; 指南约束外类型的防御) */
        XjsPluginBrief b;
        if (XjsPluginBriefAt(r.plugin, &b))
            srcIds.push_back(XjsPluginModeSrcId(b.id, r.modeIdx));
    }
    if (srcIds.empty() && hasExec) {
        /* Lua 执行类型: "脚本即整个搜索", 引擎一期不进多重搜索链 — 不转标签, 命中即单发。
           输入命中 (clearInput) = 模式名已被消费, 清框后模板按空词展开 (占位符等后续输入);
           菜单点击 = 框内词交给模板 <keyword> 占位符 (模板无占位符则词无处安放, 不组链尾, 忽略)。
           与普通搜索同口径: 单发前静默清掉残留标签链 */
        XjsHostedClearTags();
        std::wstring boxWord;
        if (clearInput) {
            XjsLineEdit& ed = XjsSearchWindow::Cur()->searchEd;
            ed.text.clear();
            ed.caret = 0; ed.anchor = -1; ed.scroll = 0; ed.dragging = false; ed.dirty = false;
            g_errText.clear();
        } else {
            boxWord = XjsTrimWs(XjsSearchGetText());
        }
        bool found = false;
        std::wstring q = XjsCmExpandTpl(execMode.tpl, boxWord, &found);
        std::wstring titleWord = boxWord.empty() ? XjsTrimWs(execMode.name) : boxWord;
        XjsHostedCommit(Utf16ToUtf8(q.c_str()), XjsCmKeyword(execMode.type), titleWord);
        XjsSearchWindow::Cur()->Invalidate();
        return true;
    }
    if (srcIds.empty()) return false;
    int idx = -1;
    for (int i = 0; i < (int)g_hostedTags.size(); i++)
        if (_wcsicmp(g_hostedTags[i].word.c_str(), word.c_str()) == 0) { idx = i; break; }
    bool isNew = idx < 0;
    if (isNew) {
        XjsHostedTag t;
        t.word = word;
        t.srcIds = srcIds;
        t.active = 0;
        t.born = (double)GetTickCount64() / 1000.0;
        g_hostedTags.push_back(t);
    } else {
        /* 同词再次命中 (模式重发/菜单再点): 刷新来源集合, 尽量保留当前选中来源 */
        XjsHostedTag& t = g_hostedTags[idx];
        std::wstring prev = (t.active >= 0 && t.active < (int)t.srcIds.size()) ? t.srcIds[t.active] : L"";
        t.srcIds = srcIds;
        t.active = 0;
        for (int s = 0; s < (int)srcIds.size(); s++)
            if (srcIds[s] == prev) { t.active = s; break; }
    }
    g_hostedCaret = (int)g_hostedTags.size();   /* 转标签: 回正常态 (新标签在链尾, 光标回输入框) */
    if (clearInput) {
        /* 输入命中语义 (源样式 keywordEl.value=''; focus 已按"默认无焦点"口径移除 — 增删标签不夺焦) */
        XjsLineEdit& ed = XjsSearchWindow::Cur()->searchEd;
        ed.text.clear();
        ed.caret = 0; ed.anchor = -1; ed.scroll = 0; ed.dragging = false; ed.dirty = false;
        g_errText.clear();
    }
    XjsSearchWindow::Cur()->Invalidate();
    XjsHostedExecChain(clearInput ? std::wstring() : XjsSearchGetText());
    return true;
}

/* 移除标签: 链式下任何标签都参与结果, 移除后重搜剩余链 (无标签则回普通搜索); 源样式 removeHostedTag */
void XjsHostedRemoveTag(int idx) {
    if (idx < 0 || idx >= (int)g_hostedTags.size()) return;
    g_hostedTags.erase(g_hostedTags.begin() + idx);
    if (idx < g_hostedCaret) g_hostedCaret--;
    XjsSearchWindow::Cur()->Invalidate();
    if (!g_hostedTags.empty()) XjsHostedExecChain(XjsSearchGetText());
    else if (g_resultCount > 0 || !XjsTrimWs(XjsSearchGetText()).empty()) XjsSearchNow(false);
}

/* 退出托管标签态 (普通搜索路径调用): 静默清空标签, 不触发搜索 (源样式 clearHostedTags) */
void XjsHostedClearTags() {
    if (g_hostedTags.empty()) return;
    g_hostedTags.clear();
    g_hostedCaret = 0;
    XjsSearchWindow::Cur()->Invalidate();
}

/* 模式删除: 批量剔除其来源, 来源清空的整标签移除, 重搜一次整链 (源样式 purgeHostedSources 同口径) */
void XjsHostedPurgeMode(const std::wstring& modeId) {
    bool changed = false;
    for (size_t i = g_hostedTags.size(); i-- > 0;) {
        XjsHostedTag& t = g_hostedTags[i];
        for (size_t s = t.srcIds.size(); s-- > 0;)
            if (t.srcIds[s] == modeId) { t.srcIds.erase(t.srcIds.begin() + s); changed = true; }
        if (t.active >= (int)t.srcIds.size()) t.active = 0;
        if (t.srcIds.empty()) {
            g_hostedTags.erase(g_hostedTags.begin() + i);
            if ((int)i < g_hostedCaret) g_hostedCaret--;
        }
    }
    if (!changed) return;
    XjsSearchWindow::Cur()->Invalidate();
    if (!g_hostedTags.empty()) XjsHostedExecChain(XjsSearchGetText());
    else if (g_resultCount > 0 || !XjsTrimWs(XjsSearchGetText()).empty()) XjsSearchNow(false);
}

/* 来源切换菜单命令 (源样式 openHostedSrcMenu item click): 换当前来源并重搜整链 */
void XjsHostedPickSource(int srcIdx) {
    int ti = g_hostMenuTag;
    if (ti < 0 || ti >= (int)g_hostedTags.size()) return;
    XjsHostedTag& t = g_hostedTags[ti];
    if (srcIdx < 0 || srcIdx >= (int)t.srcIds.size()) return;
    if (t.active != srcIdx) {
        t.active = srcIdx;
        XjsSearchWindow::Cur()->Invalidate();
        XjsHostedExecChain(XjsSearchGetText());   /* 来源变化: 链式结果改变, 带当前输入重搜整链 */
    }
}

/* 视图名 ↔ 枚举: 档序/文字单一来源 (配置里 list/details/medium/large 是持久化口径,
   与 XjsViewMode 序一一对应; 曾载入两处各写一套 if 链 + 保存处另有一张表, 三处漂移) */
static const wchar_t* VIEW_NAMES[4] = { L"list", L"details", L"medium", L"large" };
static int XjsViewIndexFromName(const std::wstring& s) {
    for (int i = 0; i < 4; i++)
        if (s == VIEW_NAMES[i]) return i;
    return VM_LIST;   /* 未知名回落列表视图 (原口径) */
}

/* 搜索模式名 ↔ 枚举 (每窗持久化口径 = g_modeIni 的 wildcard/regex/sql/lua/lua-exec, 与 XMODE_* 序一致):
   载入两处 (顶层迁移种子 + 各窗口条目) 共用, 未知名回落通配符 */
static int XjsModeIndexFromName(const std::wstring& s) {
    for (int m = 0; m < 5; m++)
        if (s == g_modeIni[m]) return m;
    return XMODE_WILDCARD;
}

void XjsLoadConfig() {
    g_cfg.Load();

    /* 全局快捷键已整体移除 (2026-09-17 二次口径: 私有快捷键取消注册且不再注册) */

    /* 搜索模式已每窗化 (2026-09-18): 旧顶层共享键作为迁移种子读出后废弃 —
       所有窗口当时显示在同一模式下, 各档案缺省继承它, 之后各自独立 */
    const int legacyMode = XjsModeIndexFromName(XjsConfig::Str(g_cfg.Root(), K_SEARCHMODE, L"wildcard"));
    g_cfg.Root().erase(K_SEARCHMODE);

    /* 页面缩放已每窗化 (2026-09-17): 旧顶层共享键作为迁移种子读出后废弃 —
       所有窗口当时显示在同一缩放下, 各档案缺省继承它, 之后各自独立 */
    int legacyZoom = XjsConfig::Int(g_cfg.Root(), K_ZOOM, 10);
    if (legacyZoom < 5 || legacyZoom > 20) legacyZoom = 10;
    g_cfg.Root().erase(K_ZOOM);

    g_viewMode = (XjsViewMode)XjsViewIndexFromName(XjsConfig::Str(g_cfg.Root(), K_VIEW, L"list"));

    g_previewVisible = XjsConfig::Bool(g_cfg.Root(), K_PREVIEW, true);
    g_previewWidth = XjsConfig::Int(g_cfg.Root(), K_PREVW, 400);
    if (g_previewWidth < 280) g_previewWidth = 280;
    if (g_previewWidth > 800) g_previewWidth = 800;

    /* 列布局 (顶层全局默认兜底; 扁平子键名同源样式 columns.*): 顺序/宽度/弹性/显隐全按字段名记忆 */
    {
        picojson::object& cols = g_cfg.Obj(K_COLSG);
        XjsColsApplyJson(g_colsDetails, cols, "orderDetails", "wDetails", "flexDetails", "vDetails");
        XjsColsApplyJson(g_colsList, cols, "orderList", "wList", "flexList", "vList");
    }

    /* 搜索模式: 共享 = 顶层 "共享搜索模式"; 私有 = 各窗口条目的 "私有搜索模式" (窗口循环里收)。
       运行时统一进 g_customModes, scope/ownerName 记归属 — 存储位置即作用域 */
    {
        g_customModes.clear();
        auto cit = g_cfg.Root().find(K_MODES_SHARED);
        if (cit != g_cfg.Root().end() && cit->second.is<picojson::array>()) {
            for (auto& e : cit->second.get<picojson::array>()) {
                if (!e.is<picojson::object>()) continue;
                XjsCustomMode cm = XjsCmFromJson(e.get<picojson::object>());
                if (!cm.id.empty() && !cm.name.empty()) g_customModes.push_back(cm);
                if ((int)g_customModes.size() >= 100) break;
            }
        }
    }

    /* 双击 Ctrl 唤起目标 (空/名称不存在 = 禁用) */
    g_doubleCtrlTarget = XjsConfig::Str(g_cfg.Root(), K_DOUBLECTRL, L"");
    g_gfxEngine = (XjsConfig::Str(g_cfg.Root(), K_ENGINE, L"d2d") == L"gdiplus") ? 1 : 0;   /* 绘制引擎 (重启生效) */

    /* 插件用户状态 (顶层 "插件" 数组): 标识/启用/已确认版本 — 目录发现与清单在 XjsPluginStartup 扫描 */
    if (auto pit = g_cfg.Root().find(K_PLUGINS); pit != g_cfg.Root().end() && pit->second.is<picojson::array>()) {
        for (auto& e : pit->second.get<picojson::array>()) {
            if (!e.is<picojson::object>()) continue;
            const picojson::object& po = e.get<picojson::object>();
            std::wstring id = XjsConfig::Str(po, K_P_ID, L"");
            if (!id.empty())
                XjsPluginUserStateSet(id.c_str(), XjsConfig::Bool(po, K_P_ENABLED, false),
                                      XjsConfig::Str(po, K_P_CONFIRMED, L"").c_str());
        }
    }

    /* 语言每窗化迁移种子 (2026-09-19): 曾为顶层键, 读出后 erase 废弃 — 各窗口条目缺 "语言" 键时
       以它兜底 (老配置升级 = 全部窗口沿用迁移前的语言, 不集体跳回 auto) */
    const std::wstring legacyLang = XjsConfig::Str(g_cfg.Root(), K_LANG, L"auto");
    g_cfg.Root().erase(K_LANG);

    /* 皮肤 (兜底初值, 随后被槽 0 档案覆盖): skin\skin-<名>.css (文件缺失/解析失败 = 内置 dark 默认) */
    g_skinName = XjsConfig::Str(g_cfg.Root(), K_SKIN, L"dark");
    XjsSearchWindow::Cur()->skinName = g_skinName;
    XjsSkinLoad(g_skinName.c_str());

    /* 窗口档案数组 ("窗口"): 绑定窗口的私有配置全在各自条目的 "窗口名称" 之下;
       下标 = 档案槽 (持久, 关窗保留), 主窗恒槽 0; 空配置兜底补一个默认槽 */
    XjsUiProfilesReset();
    auto uit = g_cfg.Root().find(K_WINARR);
    if (uit != g_cfg.Root().end() && uit->second.is<picojson::array>()) {
        for (auto& e : uit->second.get<picojson::array>()) {
            if (!e.is<picojson::object>()) continue;
            picojson::object& o = e.get<picojson::object>();
            XjsUiProfile p;
            /* 列集合无默认列来源, 裸局部=垃圾 sortField 指针, ApplyJson/ApplyUiProfile 读它=AV;
               先拷当前默认列 (含扁平键回退), JSON 只做覆盖 */
            p.colsDetails = g_colsDetails;
            p.colsList = g_colsList;
            p.skin = XjsConfig::Str(o, K_SKIN, L"dark");
            p.name = XjsConfig::Str(o, K_WINNAME, L"");   /* 窗口名称 (主窗恒固定名, 不以此为准) */
            if (auto hit = o.find(K_HOTKEY); hit != o.end() && hit->second.is<picojson::object>()) {
                picojson::object& ho = hit->second.get<picojson::object>();
                p.hotkeyMod = (UINT)XjsConfig::Int(ho, "修饰键", 0);
                p.hotkeyVk = (UINT)XjsConfig::Int(ho, "虚拟键", 0);
                if (!p.hotkeyMod && !p.hotkeyVk) {   /* 一次性迁移: 2026-09-19 前的英文键, 写回即消失 */
                    p.hotkeyMod = (UINT)XjsConfig::Int(ho, "Mod", 0);
                    p.hotkeyVk = (UINT)XjsConfig::Int(ho, "Vk", 0);
                }
            }
            p.viewMode = (XjsViewMode)XjsViewIndexFromName(XjsConfig::Str(o, K_VIEW, L"list"));
            /* 搜索模式 (每窗; 条目缺键 = 旧配置 → 继承已废弃的顶层共享键作迁移种子) */
            p.mode = XjsModeIndexFromName(XjsConfig::Str(o, K_SEARCHMODE, g_modeIni[legacyMode]));
            p.previewVisible = XjsConfig::Bool(o, K_PREVIEW, true);
            p.previewWidth = XjsConfig::Int(o, K_PREVW, 400);
            if (p.previewWidth < 280) p.previewWidth = 280;
            if (p.previewWidth > 800) p.previewWidth = 800;
            p.openElevated = XjsConfig::Bool(o, K_OPENELE, false);
            p.openAsync = XjsConfig::Bool(o, K_OPENASY, true);
            p.openHideWindow = XjsConfig::Bool(o, K_OPENHIDE, false);
            /* 列表框行为: 驱动器进度条/悬停高亮/残影 */
            p.driveProgress = XjsConfig::Bool(o, K_DRVPROG, true);
            p.rowHover = XjsConfig::Bool(o, K_ROWHOVER, true);
            p.rowHoverFade = XjsConfig::Bool(o, K_HOVERFADE, true);
            /* 窗口行为 (每窗): 失焦动作/激活位置/标题栏与状态栏可见性/置顶 */
            p.blurAction = XjsConfig::Int(o, K_BLUR, 0);
            if (p.blurAction < 0 || p.blurAction > 1) p.blurAction = 0;
            p.appearPos = XjsConfig::Int(o, K_APPEAR, 0);
            if (p.appearPos < 0 || p.appearPos >= XJS_APPEAR_COUNT) p.appearPos = 0;
            p.showCtrlBtns = XjsConfig::Bool(o, K_CTRLBTN, true);
            p.showFilterBox = XjsConfig::Bool(o, K_FILTERBOX, true);
            p.showStatusbar = XjsConfig::Bool(o, K_STATUSBAR, true);
            p.taskbarIcon = XjsConfig::Bool(o, K_TASKBAR, true);
            p.topmost = XjsConfig::Bool(o, K_TOPMOST, false);
            /* 鼠标打开 (每窗) + 页面缩放 (每窗, 缺省继承旧顶层共享值) */
            p.mouseOpen = XjsConfig::Int(o, K_MOUSEOPEN, 0);
            if (p.mouseOpen < 0 || p.mouseOpen > 1) p.mouseOpen = 0;
            p.defaultSel = XjsConfig::Int(o, K_DEFSEL, 0);
            if (p.defaultSel < 0 || p.defaultSel > 1) p.defaultSel = 0;
            p.uiZoom = XjsConfig::Int(o, K_ZOOM, legacyZoom);
            if (p.uiZoom < 5 || p.uiZoom > 20) p.uiZoom = legacyZoom;
            p.lang = XjsLangFromCode(XjsConfig::Str(o, K_LANG, legacyLang.c_str()).c_str());   /* 界面语言 (每窗; 缺键沿用顶层迁移种子) */
            /* 创建套用 (每窗): 填入方式/指定关键词/上次搜索词/默认排序 */
            p.createFill = XjsConfig::Int(o, K_CREATEFILL, 0);
            if (p.createFill < 0 || p.createFill > 2) p.createFill = 0;
            p.createKeyword = XjsConfig::Str(o, K_CREATEKEYWORD, L"");
            p.lastSearch = XjsConfig::Str(o, K_LASTSEARCH, L"");
            p.sortField = XjsConfig::Str(o, K_SORTFIELD, L"");
            p.sortWay = XjsConfig::Bool(o, K_SORTWAY, false);
            /* 窗口矩形 (子窗"之前的位置"记忆; 主窗矩形仍在顶层 "窗口矩形" 键) */
            if (auto rit = o.find(K_WINRECT); rit != o.end() && rit->second.is<picojson::object>()) {
                picojson::object& ro = rit->second.get<picojson::object>();
                p.rectValid = ro.find("横坐标") != ro.end() && ro.find("纵坐标") != ro.end();
                p.rx = XjsConfig::Int(ro, "横坐标", 0);
                p.ry = XjsConfig::Int(ro, "纵坐标", 0);
                p.rw = XjsConfig::Int(ro, "宽", 1250);
                p.rh = XjsConfig::Int(ro, "高", 780);
                p.rdpi = XjsConfig::Int(ro, "DPI", 96);
            }
            /* 搜索设置 (每窗一份), 内键同 DLL SetSearchSettings; 缺省 = XjsMatchSettings 成员默认 */
            if (auto mit = o.find(K_MATCH); mit != o.end() && mit->second.is<picojson::object>()) {
                picojson::object& mo = mit->second.get<picojson::object>();
                p.match.pinyinInitial = XjsConfig::Bool(mo, "支持首拼", true);
                p.match.pinyinFull = XjsConfig::Bool(mo, "支持全拼", true);
                p.match.pinyinExact = XjsConfig::Bool(mo, "拼音完整匹配", false);
                p.match.wildcardStar = XjsConfig::Bool(mo, "支持星号", true);
                p.match.wildcardQuestion = XjsConfig::Bool(mo, "支持问号", true);
                p.match.matchFullWidth = XjsConfig::Bool(mo, "匹配全角", false);
                p.match.caseSensitive = XjsConfig::Bool(mo, "区分大小写", false);
                p.match.emptyShowsAll = XjsConfig::Bool(mo, "搜索词为空时显示所有文件", true);
            }
            /* 显示字段: {列表:{顺序,宽度,弹性,显隐}, 详情:{...}} (缺省沿用当前默认列) */
            if (auto cfit = o.find(K_COLS); cfit != o.end() && cfit->second.is<picojson::object>()) {
                picojson::object& co = cfit->second.get<picojson::object>();
                if (auto lit = co.find(K_COL_LISTVIEW); lit != co.end() && lit->second.is<picojson::object>())
                    XjsColsApplyJson(p.colsList, lit->second.get<picojson::object>(), K_COL_ORDER, K_COL_W, K_COL_FLEX, K_COL_V);
                if (auto dit = co.find(K_COL_DETVIEW); dit != co.end() && dit->second.is<picojson::object>())
                    XjsColsApplyJson(p.colsDetails, dit->second.get<picojson::object>(), K_COL_ORDER, K_COL_W, K_COL_FLEX, K_COL_V);
            }
            /* 搜索历史 (每窗) */
            if (auto hhit = o.find(K_HISTORY); hhit != o.end() && hhit->second.is<picojson::array>()) {
                for (auto& he : hhit->second.get<picojson::array>()) {
                    if (!he.is<std::string>()) continue;
                    std::wstring s = Utf8ToUtf16(he.get<std::string>().c_str());
                    if (!s.empty()) p.history.push_back(s);
                }
            }
            XjsUiProfilesPush(p);
            /* 私有搜索模式: 存储位置即作用域 → scope=1, 归属 = 本条目窗口名称 */
            if (auto pmit = o.find(K_MODES_PRIVATE); pmit != o.end() && pmit->second.is<picojson::array>()) {
                for (auto& me : pmit->second.get<picojson::array>()) {
                    if (!me.is<picojson::object>() || (int)g_customModes.size() >= 100) continue;
                    XjsCustomMode cm = XjsCmFromJson(me.get<picojson::object>());
                    cm.scope = 1;
                    cm.ownerName = p.name;
                    if (!cm.id.empty() && !cm.name.empty()) g_customModes.push_back(cm);
                }
            }
        }
    }
    if (XjsUiProfileCount() == 0)
        XjsUiProfilesAppend(XJS_MAIN_WIN_NAME);   /* 空配置兜底: 主窗槽 0 恒存在 */
    /* 无内置默认热键 (2026-09-17 用户口径: 程序不自动注册, 录制过才有): 未设置的槽保持 0/0 */
    XjsUiProfile* p0 = XjsUiProfileAt(0);
    if (p0) {
        g_viewMode = p0->viewMode;
        g_mode = p0->mode;                     /* 搜索模式 (每窗; 曾顶层共享键, 已迁入档案) */
        g_previewVisible = p0->previewVisible;
        g_previewWidth = p0->previewWidth;
        /* 空列档案视同无档案 (ApplyUiProfile 同口径): 空配置兜底 append 的槽 0 列集为空,
           照拷 = 抹掉主窗构造时的内置默认列 → 0 可见列 (列表只剩左缘碎条、表头消失,
           2026-09-18 配置文件不存在时实锤)。跳过拷贝保留默认, 首次落盘即把存活
           主窗的有效列集写回槽 0 自愈 */
        if (p0->colsDetails.Count() > 0 && p0->colsList.Count() > 0) {
            g_colsDetails = p0->colsDetails;
            g_colsList = p0->colsList;
        }
        g_skinName = p0->skin;
        g_openElevated = p0->openElevated;
        g_openAsync = p0->openAsync;
        g_openHideWindow = p0->openHideWindow;
        g_match = p0->match;                   /* 搜索匹配×8/列表框行为×3 (每窗字段, 曾漏同步 = 重启回落默认) */
        g_driveProgress = p0->driveProgress;
        g_rowHover = p0->rowHover;
        g_rowHoverFade = p0->rowHoverFade;
        g_history = p0->history;               /* 搜索历史 (每窗; 曾为顶层 "history" 进程共享) */
        /* 热键注册事实源是窗口字段 (XjsHotkeyRegisterSlot 窗口字段优先, 档案只是未开窗槽位兜底):
           启动载入把槽 0 档案热键同步进主窗字段 — 只同步显式录制过的值, 不再造默认 */
        XjsSearchWindow::Cur()->hotkeyMod = p0->hotkeyMod;
        XjsSearchWindow::Cur()->hotkeyVk = p0->hotkeyVk;
        XjsSearchWindow::Cur()->lang = p0->lang;   /* 界面语言 (每窗): 槽 0 档案 → 主窗字段 */
        XjsSearchWindow::Cur()->blurAction = p0->blurAction;
        XjsSearchWindow::Cur()->appearPos = p0->appearPos;
        XjsSearchWindow::Cur()->showCtrlBtns = p0->showCtrlBtns;
        XjsSearchWindow::Cur()->showFilterBox = p0->showFilterBox;
        XjsSearchWindow::Cur()->showStatusbar = p0->showStatusbar;
        XjsSearchWindow::Cur()->taskbarIcon = p0->taskbarIcon;
        XjsSearchWindow::Cur()->topmost = p0->topmost;
        XjsSearchWindow::Cur()->mouseOpen = p0->mouseOpen;
        XjsSearchWindow::Cur()->defaultSel = p0->defaultSel;
        XjsSearchWindow::Cur()->uiZoom = p0->uiZoom;
        XjsSearchWindow::Cur()->createFill = p0->createFill;
        XjsSearchWindow::Cur()->createKeyword = p0->createKeyword;
        XjsSearchWindow::Cur()->skinName = p0->skin;
        XjsSkinLoad(g_skinName.c_str());
    }

    /* 窗口矩形已全面每窗化 (含主窗槽 0, 2026-09-17 用户口径: 尺寸不全局统一):
       旧顶层共享键一次性迁入槽 0 档案后废弃 — 原全局键会被"最后一次保存配置时的当前窗"
       覆盖 (子窗改个设置 → 主窗下次启动变成子窗尺寸), 这正是"尺寸全局串味"的根源。
       迁移原样入库不折算, DPI 折算统一在创建时做 (与子窗矩形同口径)。
       子键保持英文: 这里读的是 2026-09-19 前的旧格式遗留数据, 新配置不再产生该顶层键 */
    {
        picojson::object& wobj = g_cfg.Obj(K_WINRECT);
        XjsUiProfile* p0m = XjsUiProfileAt(0);
        if (p0m && !p0m->rectValid && wobj.find("x") != wobj.end() && wobj.find("y") != wobj.end()) {
            p0m->rectValid = true;
            p0m->rx = XjsConfig::Int(wobj, "x", CW_USEDEFAULT);
            p0m->ry = XjsConfig::Int(wobj, "y", CW_USEDEFAULT);
            p0m->rw = XjsConfig::Int(wobj, "width", 1250);
            p0m->rh = XjsConfig::Int(wobj, "height", 780);
            p0m->rdpi = XjsConfig::Int(wobj, "dpi", 96);
        }
        g_cfg.Root().erase(K_WINRECT);   /* 迁移完成即清除, 写回得干净配置 */
    }

    /* 文件分类 / 路径别名 (设置窗表格保存的引擎下发配置; 值 = 内嵌 JSON 原文, utf8) */
    if (auto it = g_cfg.Root().find(K_FILTERCFG); it != g_cfg.Root().end() && it->second.is<std::string>())
        g_savedFilterJson = it->second.get<std::string>();
    if (auto it = g_cfg.Root().find(K_ALIASCFG); it != g_cfg.Root().end() && it->second.is<std::string>())
        g_savedAliasJson = it->second.get<std::string>();

    /* 重建对话框记忆 */
    picojson::object& rb = g_cfg.Obj(K_REBUILD);
    g_rbSaved = XjsConfig::Bool(rb, "已保存", false);
    auto fit = rb.find("字段");
    if (fit != rb.end() && fit->second.is<picojson::array>()) {
        const picojson::array& a = fit->second.get<picojson::array>();
        for (int i = 0; i < 7 && i < (int)a.size(); i++)
            g_rbFields[i] = a[i].is<bool>() && a[i].get<bool>();
    }
    g_rbDrives = XjsConfig::Str(rb, "驱动器", L"");

    /* 旧版英文键清扫 (2026-09-17 全部中文化后不再读取; 写回即得纯中文配置) */
    static const char* const LEGACY[] = {
        "uiWindows", "customSearchModes", "history", "hotkeyMod", "hotkeyVk", "searchMatch",
        "doubleCtrl", "columns", "window", "rebuild", "searchMode", "viewMode", "skin",
        "preview", "previewWidth", "zoomTenths", "doubleCtrlTarget" };
    for (auto* k : LEGACY) g_cfg.Root().erase(k);

    g_cfg.WriteBack();   /* 建档/规范化 (json 缺失时落盘默认骨架) */
}

void XjsSaveConfig() {
    /* 搜索模式/页面缩放已每窗化: 顶层不再写 K_SEARCHMODE / K_ZOOM
       (每窗值在下方档案循环进各自条目) */

    /* 窗口档案数组 ("窗口"): 档案槽持久 (关窗的槽保留 → ☰菜单启动器可按槽重建窗口);
       存活窗口把当前设置刷进自己绑定的槽, 未打开的槽按内存档案原样保留。
       绑定窗口的私有配置全在各自条目的 "窗口名称" 之下; 共享项不在此数组 */
    picojson::array uiarr;
    for (int i = 0; i < XjsUiProfileCount(); i++) {
        XjsUiProfile* p = XjsUiProfileAt(i);
        for (int wi = 0; wi < XjsSearchWindow::Count(); wi++) {
            XjsSearchWindow* w = XjsSearchWindow::At(wi);
            if (w->uiIndex == i && w->hWnd) { XjsCollectUiProfileInto(w, *p); break; }
        }
        picojson::object o;
        o[K_WINNAME] = picojson::value(Utf16ToUtf8(p->name.c_str()));
        o[K_SKIN] = picojson::value(Utf16ToUtf8(p->skin.c_str()));
        o[K_VIEW] = picojson::value(Utf16ToUtf8(VIEW_NAMES[(int)p->viewMode]));
        o[K_SEARCHMODE] = picojson::value(Utf16ToUtf8(g_modeIni[p->mode]));
        o[K_PREVIEW] = picojson::value(p->previewVisible);
        o[K_PREVW] = picojson::value((double)p->previewWidth);
        o[K_OPENELE] = picojson::value(p->openElevated);
        o[K_OPENASY] = picojson::value(p->openAsync);
        o[K_OPENHIDE] = picojson::value(p->openHideWindow);
        {
            picojson::object ho;
            ho["修饰键"] = picojson::value((double)p->hotkeyMod);
            ho["虚拟键"] = picojson::value((double)p->hotkeyVk);
            o[K_HOTKEY] = picojson::value(ho);
        }
        o[K_DRVPROG] = picojson::value(p->driveProgress);
        o[K_ROWHOVER] = picojson::value(p->rowHover);
        o[K_HOVERFADE] = picojson::value(p->rowHoverFade);
        /* 窗口行为 (每窗): 失焦动作/激活位置/标题栏与状态栏可见性/任务栏图标/置顶 */
        o[K_BLUR] = picojson::value((double)p->blurAction);
        o[K_APPEAR] = picojson::value((double)p->appearPos);
        o[K_CTRLBTN] = picojson::value(p->showCtrlBtns);
        o[K_FILTERBOX] = picojson::value(p->showFilterBox);
        o[K_STATUSBAR] = picojson::value(p->showStatusbar);
        o[K_TASKBAR] = picojson::value(p->taskbarIcon);
        o[K_TOPMOST] = picojson::value(p->topmost);
        o[K_MOUSEOPEN] = picojson::value((double)p->mouseOpen);
        o[K_DEFSEL] = picojson::value((double)p->defaultSel);
        o[K_ZOOM] = picojson::value((double)p->uiZoom);
        o[K_LANG] = picojson::value(XjsLangToCode(p->lang));   /* 界面语言 (每窗) */
        o[K_CREATEFILL] = picojson::value((double)p->createFill);
        o[K_CREATEKEYWORD] = picojson::value(Utf16ToUtf8(p->createKeyword.c_str()));
        o[K_LASTSEARCH] = picojson::value(Utf16ToUtf8(p->lastSearch.c_str()));
        o[K_SORTFIELD] = picojson::value(Utf16ToUtf8(p->sortField.c_str()));
        o[K_SORTWAY] = picojson::value(p->sortWay);
        /* 窗口矩形 (每窗私有, 含主窗槽 0; 顶层 "窗口矩形" 共享键已废弃迁移, 见 LoadConfig) */
        if (p->rectValid) {
            picojson::object ro;
            ro["横坐标"] = picojson::value((double)p->rx);
            ro["纵坐标"] = picojson::value((double)p->ry);
            ro["宽"] = picojson::value((double)p->rw);
            ro["高"] = picojson::value((double)p->rh);
            ro["DPI"] = picojson::value((double)p->rdpi);
            o[K_WINRECT] = picojson::value(ro);
        }
        /* 显示字段: {列表:{顺序,宽度,弹性,显隐}, 详情:{...}} */
        {
            picojson::object lv, dv, co;
            XjsColsSaveJson(p->colsList, lv, K_COL_ORDER, K_COL_W, K_COL_FLEX, K_COL_V);
            XjsColsSaveJson(p->colsDetails, dv, K_COL_ORDER, K_COL_W, K_COL_FLEX, K_COL_V);
            co[K_COL_LISTVIEW] = picojson::value(lv);
            co[K_COL_DETVIEW] = picojson::value(dv);
            o[K_COLS] = picojson::value(co);
        }
        /* 搜索设置 (每窗一份), 内键同 DLL SetSearchSettings */
        {
            picojson::object mo;
            mo["支持首拼"] = picojson::value(p->match.pinyinInitial);
            mo["支持全拼"] = picojson::value(p->match.pinyinFull);
            mo["拼音完整匹配"] = picojson::value(p->match.pinyinExact);
            mo["支持星号"] = picojson::value(p->match.wildcardStar);
            mo["支持问号"] = picojson::value(p->match.wildcardQuestion);
            mo["匹配全角"] = picojson::value(p->match.matchFullWidth);
            mo["区分大小写"] = picojson::value(p->match.caseSensitive);
            mo["搜索词为空时显示所有文件"] = picojson::value(p->match.emptyShowsAll);
            o[K_MATCH] = picojson::value(mo);
        }
        /* 私有搜索模式: 存储位置即作用域 (scope=1 且归属 = 本条目窗口名称) */
        {
            picojson::array pm;
            for (auto& cm : g_customModes) {
                if (cm.scope != 1 || cm.ownerName != p->name) continue;
                picojson::object mo;
                XjsCmToJson(cm, mo);
                pm.push_back(picojson::value(mo));
            }
            o[K_MODES_PRIVATE] = picojson::value(pm);
        }
        /* 搜索历史 (每窗) */
        {
            picojson::array harr;
            for (auto& s : p->history) harr.push_back(picojson::value(Utf16ToUtf8(s.c_str())));
            o[K_HISTORY] = picojson::value(harr);
        }
        uiarr.push_back(picojson::value(o));
    }
    g_cfg.Set(K_WINARR, picojson::value(uiarr));

    /* 共享搜索模式 (顶层, 全部窗口生效; 私有的已序列化进各自窗口条目) */
    {
        picojson::array carr;
        for (auto& cm : g_customModes) {
            if (cm.scope == 1) continue;
            picojson::object mo;
            XjsCmToJson(cm, mo);
            carr.push_back(picojson::value(mo));
        }
        g_cfg.Set(K_MODES_SHARED, picojson::value(carr));
    }

    g_cfg.Set(K_DOUBLECTRL, Utf16ToUtf8(g_doubleCtrlTarget.c_str()));
    g_cfg.Set(K_ENGINE, std::string(g_gfxEngine == 1 ? "gdiplus" : "d2d"));   /* std::string 包裹: 裸三元是 const char*, 会被隐式匹配到 Set(bool) 重载落盘成 true (09-24 白屏崩溃元凶之一) */
    /* 语言已每窗化 (条目 "语言"); 顶层键废弃, LoadConfig 读作迁移种子后 erase */

    /* 文件分类 / 路径别名 (顶层; XjsFilterConfigApply/XjsAliasConfigApply 成功时更新, 此处随档落盘) */
    if (!g_savedFilterJson.empty()) g_cfg.Set(K_FILTERCFG, g_savedFilterJson);
    if (!g_savedAliasJson.empty()) g_cfg.Set(K_ALIASCFG, g_savedAliasJson);

    /* 插件用户状态 (顶层 "插件"): 全部已知条目原样写回 (目录已删的也保留, 重装免重新启用) */
    {
        picojson::array parr;
        for (int i = 0; i < XjsPluginUserStateCount(); i++) {
            std::wstring id, ver; bool en = false;
            if (!XjsPluginUserStateAt(i, &id, &en, &ver)) continue;
            picojson::object po;
            po[K_P_ID] = picojson::value(Utf16ToUtf8(id.c_str()));
            po[K_P_ENABLED] = picojson::value(en);
            po[K_P_CONFIRMED] = picojson::value(Utf16ToUtf8(ver.c_str()));
            parr.push_back(picojson::value(po));
        }
        g_cfg.Set(K_PLUGINS, picojson::value(parr));
    }

    /* 窗口矩形不再写顶层共享键: 每窗矩形已在上方档案循环刷进各自条目
       (旧顶层键由 XjsLoadConfig 一次性迁入槽 0 档案后废弃) */

    picojson::object& rb = g_cfg.Obj(K_REBUILD);
    rb["已保存"] = picojson::value(g_rbSaved);
    picojson::array f;
    for (int i = 0; i < 7; i++) f.push_back(picojson::value(g_rbFields[i]));
    rb["字段"] = picojson::value(f);
    rb["驱动器"] = picojson::value(Utf16ToUtf8(g_rbDrives.c_str()));

    /* 搜索历史已每窗化: 各窗口条目的 "搜索历史" 在上方档案循环里写入, 顶层不再有 history 键 */

    g_cfg.WriteBack();
}

void XjsSaveWindowRect() {
    XjsSaveConfig();   /* 每窗矩形已在 SaveConfig 的档案收集里刷进各自槽 (主窗=槽 0) */
}

/* ==================== 重建 / 退出收尾 ==================== */

/* 重建核心: enableFields[7] = 评分/大小/修改/创建/访问/属性/别名 (只 AddField 勾选的, 未勾选=关闭);
   drivesJsonWide = L"[""C:\\"",""D:\\""]" 形式的盘符 JSON, 空 = 全盘扫描 (正式版 rebuildIndex 同口径) */
void XjsEngineRebuildEx(const bool enableFields[7], const std::wstring& drivesJsonWide) {
    if (!g_engine) return;
    if (g_isScanning) {
        g_statusText = XjsT(L"状态栏.扫描中禁止重建");
        XjsSearchWindow::Cur()->Invalidate();
        return;
    }
    xjs_sync_AllStop(g_engine, FALSE);
    std::wstring dbPath = XjsGetExeDir() + L"\\xjs_db.dat";
    DeleteFileW(dbPath.c_str());
    if (g_result) { xjs_result_Destroy(g_result); g_result = NULL; }   /* 选中集合随结果对象一并销毁 */
    XjsClearRenderCaches();
    g_resultCount = 0;
    g_anchorIdx = g_focusIdx = -1;
    xjs_db_Clear(g_engine);
    static const char* const FN[7] = { "文件评分", "文件大小", "修改时间", "创建时间", "访问时间", "文件属性", "别名" };
    for (int i = 0; i < 7; i++)
        if (enableFields[i]) xjs_db_AddField(g_engine, FN[i], NULL);
    /* 正式版口径: 重建 = 清库后、重新扫描前重放 文件分类/路径别名 —— 筛选器只对之后入库的文件
       生效, 必须赶在扫描前就位 (别名同随入库套用) */
    XjsEngineApplySavedConfigs();
    g_statusText = XjsT(L"状态栏.正在重建");   /* 源样式 Search.IndexRebuilding */
    std::string dj = Utf16ToUtf8(drivesJsonWide.c_str());
    xjs_db_ScanPath(g_engine, dj.empty() ? NULL : dj.c_str(), TRUE);
    XjsSearchWindow::Cur()->Invalidate();
}

void XjsEngineShutdown(bool warnOnSaveFail) {
    if (!g_engine) return;
    int state = xjs_db_GetEngineState(g_engine);
    bool scanning = g_isScanning || state == XJS_DB_STATE_SCANNING;
    std::wstring dbPath = XjsGetExeDir() + L"\\xjs_db.dat";
    if (scanning) {
        xjs_db_StopScan(g_engine);
        xjs_sync_AllStop(g_engine, FALSE);
        DeleteFileW(dbPath.c_str());
        return;
    }
    xjs_sync_AllStop(g_engine, TRUE);
    /* 保存蜗牛快搜索引库: 先写临时文件, 成功后删原库、临时文件改名顶上 ——
       保存中途崩溃/断电最多残留一个 .tmp, 原库完整不损坏 (残留 .tmp 由启动加载前清理) */
    std::wstring tmpPath = dbPath + L".tmp";
    std::string utf8 = Utf16ToUtf8(tmpPath.c_str());
    BOOL ok = FALSE;
    for (int i = 0; i < 10; i++) {
        if (xjs_db_Save(g_engine, utf8.c_str())) { ok = TRUE; break; }
        Sleep(100);
    }
    if (ok) {
        DeleteFileW(dbPath.c_str());
        if (!MoveFileW(tmpPath.c_str(), dbPath.c_str())) ok = FALSE;   /* 同目录改名, 失败即按保存失败口径 */
    }
    if (!ok)
        DeleteFileW(tmpPath.c_str());   /* 失败删半成品临时文件, 未替换成功的旧库保持不动 */
    if (!ok && warnOnSaveFail)
        MessageBoxW(g_hWnd, XjsT(L"状态栏.数据库保存失败"), XjsT(L"通用词.警告"), MB_OK | MB_ICONWARNING);
}

/* ==================== 托盘 / 热键 ==================== */

void XjsTrayAdd(HWND hwnd) {
    /* 托盘图标 = exe 内嵌资源 32512 (私有句柄, 重入时 DestroyIcon 只销毁本处加载的) */
    HICON hIcon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(32512), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!hIcon) hIcon = LoadIconW(NULL, IDI_APPLICATION);
    if (g_inTray) {
        if (g_nid.hIcon) DestroyIcon(g_nid.hIcon);
        g_nid.hIcon = hIcon;
        wcscpy_s(g_nid.szTip, XjsT(L"应用.名称"));   /* 重入即刷新提示文字 (语言切换后经 XjsTrayAdd 同步) */
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        return;
    }
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = ID_TRAY_ICON;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY_NOTIFY;
    g_nid.hIcon = hIcon;
    wcscpy_s(g_nid.szTip, XjsT(L"应用.名称"));   /* 源样式 App.TrayTip: 托盘提示就是应用名 */
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_inTray = true;
}

void XjsTrayRemove() {
    if (g_inTray) { Shell_NotifyIconW(NIM_DELETE, &g_nid); g_inTray = false; }
    if (g_nid.hIcon) { DestroyIcon(g_nid.hIcon); g_nid.hIcon = NULL; }
}

/* 注册一个档案槽的全局快捷键 (登记在主窗 hwnd 上, WM_HOTKEY wParam = ID_HOTKEY_SHOW+槽)。
   vk=0 = 该窗未设置热键, 不注册也不算失败 (2026-09-17 用户口径: 程序无内置默认热键,
   只注册设置页录制过的) */
static BOOL XjsHotkeyRegisterSlot(int slot, HWND mainH) {
    UINT mod = 0, vk = 0;
    if (XjsSearchWindow* w = XjsSearchWindow::AtSlot(slot)) { mod = w->hotkeyMod; vk = w->hotkeyVk; }
    else if (XjsUiProfile* p = XjsUiProfileAt(slot)) { mod = p->hotkeyMod; vk = p->hotkeyVk; }
    UnregisterHotKey(mainH, ID_HOTKEY_SHOW + slot);
    if (!vk) return TRUE;   /* 未设置: 卸载即终, 不报失败 */
    return RegisterHotKey(mainH, ID_HOTKEY_SHOW + slot, mod, vk);
}

/* 程序启动/主窗迁移/热键录制与清除/删除档案 后统一重注册全部槽 (重复调用安全)。
   返回 = 主窗槽"未设置或注册成功" (只有明确设置过且注册失败才 FALSE → 启动期警告) */
BOOL XjsHotkeysRegisterAll() {
    HWND mainH = XjsSearchWindow::MainHwnd();
    if (!mainH) return FALSE;
    BOOL mainOk = TRUE;
    for (int i = 0; i < XjsUiProfileCount(); i++) {
        BOOL ok = XjsHotkeyRegisterSlot(i, mainH);
        if (XjsSearchWindow* w = XjsSearchWindow::AtSlot(i)) w->hotkeyActive = (ok != FALSE);
        if (i == 0) mainOk = ok;
    }
    return mainOk;
}

void XjsHotkeysUnregisterAll() {
    HWND mainH = XjsSearchWindow::MainHwnd();
    if (!mainH) return;
    for (int i = 0; i < 64; i++)
        UnregisterHotKey(mainH, ID_HOTKEY_SHOW + i);   /* 未注册的槽位卸载失败无害 */
}

/* 插件动态菜单返回解析 (BuildMenu 的返回 JSON: [{"标识":"..","文字":"..","顺序":0},..] —
   与 manifest 菜单同构, 复用清单菜单项解析 (无 时机/扩展名) */
int XjsPluginDynMenuParse(const char* utf8Json, std::vector<XjsPluginMenuDef>* out) {
    out->clear();
    if (!utf8Json || !*utf8Json) return 0;
    picojson::value v;
    if (!picojson::parse(v, utf8Json).empty() || !v.is<picojson::array>()) return 0;
    XjsPmMenus(v, out, false);
    return (int)out->size();
}
