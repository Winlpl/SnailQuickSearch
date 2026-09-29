/*
 * xjs_preview.cpp — 预览面板 (正式版 preview-panel)
 * 驱动器信息卡(容量/文件系统/序列号/查找大目录大文件) / 文件信息 / 图片预览 / 定位·打开
 */
#include "xjs_app.h"

/* 预览面板渲染/命中状态: 每窗一份 (XjsSearchWindow 字段, 宏重定向) */
#define s_hits          (XjsSearchWindow::Cur()->previewHits)
#define s_imgZoom       (XjsSearchWindow::Cur()->previewImgZoom)
#define s_textLines     (XjsSearchWindow::Cur()->previewTextLines)
#define s_textFileId    (XjsSearchWindow::Cur()->previewTextFileId)
#define s_textScroll    (XjsSearchWindow::Cur()->previewTextScroll)
#define s_textPending   (XjsSearchWindow::Cur()->previewTextPending)
#define s_textHasMore   (XjsSearchWindow::Cur()->previewTextHasMore)
#define s_lightbox      (XjsSearchWindow::Cur()->previewLightbox)
#define s_lbBmp         (XjsSearchWindow::Cur()->previewLbBmp)
#define s_lbKeyFile     (XjsSearchWindow::Cur()->previewLbKeyFile)
#define s_lbKeyW        (XjsSearchWindow::Cur()->previewLbKeyW)
#define s_lbKeyH        (XjsSearchWindow::Cur()->previewLbKeyH)
#define s_lbKeyRot      (XjsSearchWindow::Cur()->previewLbKeyRot)
#define s_lbPending     (XjsSearchWindow::Cur()->previewLbPending)
#define s_lbFailed      (XjsSearchWindow::Cur()->previewLbFailed)
#define s_lbRotL        (XjsSearchWindow::Cur()->previewLbRotL)
#define s_lbRotR        (XjsSearchWindow::Cur()->previewLbRotR)
#define s_lbOne         (XjsSearchWindow::Cur()->previewLbOne)
#define s_lbFit         (XjsSearchWindow::Cur()->previewLbFit)
#define s_lbPress       (XjsSearchWindow::Cur()->previewLbPress)
#define s_lbBase        (XjsSearchWindow::Cur()->previewLbBase)
#define s_lbZoom        (XjsSearchWindow::Cur()->previewLbZoom)
#define s_lbPanX        (XjsSearchWindow::Cur()->previewLbPanX)
#define s_lbPanY        (XjsSearchWindow::Cur()->previewLbPanY)
#define s_lbDrag        (XjsSearchWindow::Cur()->previewLbDrag)
#define s_lbDragPt      (XjsSearchWindow::Cur()->previewLbDragPt)
#define s_lbDragPanX    (XjsSearchWindow::Cur()->previewLbDragPanX)
#define s_lbDragPanY    (XjsSearchWindow::Cur()->previewLbDragPanY)
#define s_lbImg         (XjsSearchWindow::Cur()->previewLbImg)
#define s_lbPannable    (XjsSearchWindow::Cur()->previewLbPannable)
#define s_pvScaled      (XjsSearchWindow::Cur()->previewPvScaled)
#define s_pvKeyFile     (XjsSearchWindow::Cur()->previewPvKeyFile)
#define s_pvKeyW        (XjsSearchWindow::Cur()->previewPvKeyW)
#define s_pvKeyH        (XjsSearchWindow::Cur()->previewPvKeyH)
#define s_pvKeyRot      (XjsSearchWindow::Cur()->previewPvKeyRot)
#define s_pvPending     (XjsSearchWindow::Cur()->previewPvPending)
#define s_pvFailed      (XjsSearchWindow::Cur()->previewPvFailed)
#define s_rot           (XjsSearchWindow::Cur()->previewImgRot)
#define s_imgBase       (XjsSearchWindow::Cur()->previewImgBase)
/* 插件预览接管会话状态: 同为每窗 (2026-09-28 迁入 — 曾为文件级 static, 双窗口互相
   踢掉对方的接管世代, 违反"窗口级状态一律是类字段"红线) */
#define s_pvPlugReq     (XjsSearchWindow::Cur()->previewPlugReq)
#define s_pvPlugFileId  (XjsSearchWindow::Cur()->previewPlugFileId)
#define s_pvPlugBmp     (XjsSearchWindow::Cur()->previewPlugBmp)
#define s_pvPlugW       (XjsSearchWindow::Cur()->previewPlugW)
#define s_pvPlugH       (XjsSearchWindow::Cur()->previewPlugH)
#define s_pvPlugStride  (XjsSearchWindow::Cur()->previewPlugStride)
#define s_pvPlugCache   (XjsSearchWindow::Cur()->previewPlugCache)
#define s_pvPlugCacheRt (XjsSearchWindow::Cur()->previewPlugCacheRt)

static bool XjsIsImageExt(const std::wstring& name) {
    size_t dot = name.rfind(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = name.substr(dot + 1);
    for (auto& c : ext) c = (wchar_t)towlower(c);
    static const wchar_t* exts[] = { L"jpg", L"jpeg", L"png", L"bmp", L"gif", L"tif", L"tiff" };
    for (auto* e : exts) if (ext == e) return true;
    return false;
}

static bool XjsIsTextExt(const std::wstring& name) {
    size_t dot = name.rfind(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = name.substr(dot + 1);
    for (auto& c : ext) c = (wchar_t)towlower(c);
    static const wchar_t* exts[] = {
        L"txt", L"log", L"md", L"markdown", L"json", L"ini", L"cfg", L"conf", L"xml", L"html", L"htm",
        L"h", L"c", L"cpp", L"hpp", L"cs", L"js", L"ts", L"py", L"java", L"bat", L"ps1", L"css",
        L"sql", L"lua", L"yaml", L"yml", L"rst", L"srt", L"csv" };
    for (auto* e : exts) if (ext == e) return true;
    return false;
}

/* 同步取大图标 (itemIndex=-1 不触发异步回调); dome 不做位图缓存, 返回值归调用方所有用完 Release */
static XjsBitmap* XjsPreviewIcon(int fileId) {
    if (!g_result) return NULL;
    int len = 0;
    const void* data = xjs_result_GetFileIco(g_result, fileId, -1, 128, &len, NULL);
    if (!data || len <= 0) return NULL;
    return XjsDecodeImage(data, len);
}

/* ==================== 插件预览接管 (preview 能力, P2) ====================
 * 选中目标按扩展名询问插件 (内置分类之前); 返回 1 = 已接管, 插件异步交付位图/文本。
 * requestId = 世代号: 选中/预览刷新即 ++, 过期交付静默丢弃 (同搜索指纹丢过期查询口径)。
 * 位图 = 32bpp BGRA (预乘 alpha) 原始字节暂存, 渲染时懒转本 RT 域位图 (跨渲染域铁律:
 * 外部像素一律 CPU 拷贝进本域, 绝不直接持外部句柄); 文本交付直接喂 s_textLines 走既有文本管线。
 * 面板隐藏时不发起询问 ("不可见就不干活" — UpdateSelection 的 g_previewVisible 闸已保证)。 */
static void XjsPreviewPlugDropCacheOf(XjsSearchWindow* w) {
    if (w->previewPlugCache) { w->previewPlugCache->Release(); w->previewPlugCache = NULL; }
    w->previewPlugCacheRt = NULL;
}

/* 灯箱图异步装载缓存清理 (RT 域换/内容换/收层; 键一并归零 = 下次重新投作业) */
static void XjsLightboxDropCacheOf(XjsSearchWindow* w) {
    if (w->previewLbBmp) { w->previewLbBmp->Release(); w->previewLbBmp = NULL; }
    w->previewLbKeyFile = -1;
    w->previewLbKeyW = w->previewLbKeyH = w->previewLbKeyRot = 0;
    w->previewLbPending = false;
    w->previewLbFailed = false;
}

/* 预览面板图异步装载缓存清理 (同灯箱口径) */
static void XjsPreviewScaledDropCacheOf(XjsSearchWindow* w) {
    if (w->previewPvScaled) { w->previewPvScaled->Release(); w->previewPvScaled = NULL; }
    w->previewPvKeyFile = -1;
    w->previewPvKeyW = w->previewPvKeyH = w->previewPvKeyRot = 0;
    w->previewPvPending = false;
    w->previewPvFailed = false;
}

/* 两处预采样缓存一并清 (内容换/收层/设备重建共用) */
static void XjsPreviewImageCacheDropAll(XjsSearchWindow* w) {
    XjsLightboxDropCacheOf(w);
    XjsPreviewScaledDropCacheOf(w);
}

void XjsPreviewPlugCacheInvalidate() {
    XjsSearchWindow::ForEach(&XjsPreviewPlugDropCacheOf);   /* 设备重建 = 全部窗的 RT 域都换 */
    XjsSearchWindow::ForEach(&XjsPreviewImageCacheDropAll); /* 预采样位图同样绑 RT 域 */
}

static void XjsPreviewPluginDrop() {
    s_pvPlugFileId = -1;
    s_pvPlugBmp.clear();
    s_pvPlugW = s_pvPlugH = s_pvPlugStride = 0;
    if (s_pvPlugCache) { s_pvPlugCache->Release(); s_pvPlugCache = NULL; }   /* 只丢本窗缓存 */
    s_pvPlugCacheRt = NULL;
}

/* 选中变化时询问接管 (内置图片/文本分类之前调); 真 = 已接管, 本次预览由插件交付。
   只传 fileId (v3 口径): 插件自取路径/名称, 宿主不打包 */
static bool XjsPreviewPluginTryTake(int fileId, const std::wstring& path, const std::wstring& name) {
    if (!XjsPluginActiveCap(XPC_PREVIEW)) return false;
    s_pvPlugReq++;
    s_pvPlugFileId = -1;
    if (!XjsPluginPreviewTake(fileId, s_pvPlugReq, XjsPluginCurWindowToken()))
        return false;
    s_pvPlugFileId = fileId;
    return true;
}

/* 渲染取用: 交付字节懒转本域位图 (世代/目标不匹配 = NULL 回落内置)。
   域校验: 缓存位图绑定建它那一刻的 RT — 换窗口/设备重建都换域, 旧位图必须作废重建 */
static XjsBitmap* XjsPreviewPluginBitmap(int fileId) {
    if (s_pvPlugFileId != fileId || s_pvPlugBmp.empty()) return NULL;
    if (!s_pvPlugCache || s_pvPlugCacheRt != g_rt) {
        XjsPreviewPlugDropCacheOf(XjsSearchWindow::Cur());   /* 只丢本窗缓存 (曾作废全部窗) */
        s_pvPlugCache = XjsBitmapFromBgra(s_pvPlugBmp.data(), s_pvPlugW, s_pvPlugH, s_pvPlugStride);
        s_pvPlugCacheRt = g_rt;
    }
    return s_pvPlugCache;
}

/* ==================== 图片异步装载 (2026-09-29) ====================
 * 几十 M 的图在 UI 线程同步解码 = 每次选中/滚轮冻结数百毫秒 (用户对比 Everything 实锤)。
 * 统一改异步: 渲染帧发现缓存缺口 → 落键置 pending → 投作业; 单工作线程 解码+Fant缩放
 * +旋转 → PBGRA 字节 → PostMessage 回 UI 线程转本域位图入缓存。UI 线程从此零解码,
 * 装载期间垫显旧缓存 (同源) 或暗底占位, 到货即换。双槽 [0]=面板 [1]=灯箱, 新请求覆盖
 * 同槽 = 取代旧作业; 结果单槽被新结果覆盖 = 代号校验丢弃, 下一帧重投自愈。 */
static struct XjsImgJobCs { CRITICAL_SECTION cs; XjsImgJobCs() { InitializeCriticalSectionAndSpinCount(&cs, 100); } } s_imgJobCs;
static HANDLE s_imgThread = NULL, s_imgWake = NULL;

struct XjsImgJob {
    bool pending = false;
    long long gen = 0;
    int ctx = 0;            /* 0=面板预采样 1=灯箱 */
    HWND hwnd = NULL;
    int fileId = -1;
    std::wstring path;
    int tw = 0, th = 0, rot = 0;
};
static XjsImgJob s_imgSlot[2];
static long long s_imgGen = 0;   /* 作业代号 (请求递增; 结果按代号对账) */

static DWORD WINAPI XjsImgThreadProc(LPVOID);

struct XjsImgDone {
    long long gen = 0;
    int ctx = 0;
    HWND hwnd = NULL;
    int fileId = -1;
    int w = 0, h = 0;
    bool failed = false;
    std::vector<uint8_t> bgra;
};
static XjsImgDone s_imgDone;   /* 单结果槽 (UI 消费前被覆盖 = 代号校验丢弃, 重投自愈) */

/* 渲染帧缓存缺口 → 记键置 pending 投异步作业 (UI 线程零解码; 仅查引擎取路径)。
   键在请求时落: 命中判定 (sameSrc/band) 与在途判定全靠它; 新请求覆盖同槽 = 取代旧作业 */
static void XjsPreviewImgAsk(int ctx, int fileId, int tw, int th, int rot) {
    std::wstring path = (g_engine && fileId >= 0) ? Utf8ToUtf16(xjs_db_GetPath(g_engine, fileId)) : std::wstring();
    if (path.empty()) return;
    XjsImgJobCs lk;
    XjsImgJob& j = s_imgSlot[ctx];
    j.pending = true;
    j.gen = ++s_imgGen;
    j.ctx = ctx;
    j.hwnd = g_hWnd;
    j.fileId = fileId;
    j.path = path;
    j.tw = tw;
    j.th = th;
    j.rot = rot;
    if (ctx == 1) {
        s_lbKeyFile = fileId;
        s_lbKeyW = tw;
        s_lbKeyH = th;
        s_lbKeyRot = rot;
        s_lbPending = true;
        s_lbFailed = false;
    } else {
        s_pvKeyFile = fileId;
        s_pvKeyW = tw;
        s_pvKeyH = th;
        s_pvKeyRot = rot;
        s_pvPending = true;
        s_pvFailed = false;
    }
    if (!s_imgWake) {
        s_imgWake = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (s_imgWake) s_imgThread = CreateThread(NULL, 0, XjsImgThreadProc, NULL, 0, NULL);
    }
    if (s_imgWake) SetEvent(s_imgWake);
}

static DWORD WINAPI XjsImgThreadProc(LPVOID) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);   /* WIC 工厂自由线程, MTA 直用 */
    for (;;) {
        WaitForSingleObject(s_imgWake, INFINITE);
        for (;;) {   /* 排空两槽: 自动复位事件不计数, 面板+灯箱先后投递只醒一次 */
            XjsImgJob job;
            int ctx = -1;
            {
                XjsImgJobCs lk;
                ctx = s_imgSlot[0].pending ? 0 : (s_imgSlot[1].pending ? 1 : -1);
                if (ctx >= 0) {
                    job = s_imgSlot[ctx];
                    s_imgSlot[ctx].pending = false;
                }
            }
            if (ctx < 0) break;
            XjsImgDone done;
            done.gen = job.gen;
            done.ctx = ctx;
            done.hwnd = job.hwnd;
            done.fileId = job.fileId;
            done.w = job.tw;
            done.h = job.th;
            if (IsWindow(job.hwnd)) {
                int stride = 0;
                std::vector<uint8_t> bgra;
                if (XjsDecodeFileImagePixels(job.path, job.tw, job.th, job.rot, &bgra, &stride)) {
                    done.bgra = std::move(bgra);
                } else {
                    done.failed = true;
                }
            } else {
                done.failed = true;   /* 归属窗已没: 结果照回, UI 侧选中校验兜底丢弃 */
            }
            {
                XjsImgJobCs lk;
                s_imgDone = std::move(done);
            }
            PostMessage(job.hwnd, WM_PV_IMG_READY, 0, (LPARAM)job.gen);
        }
    }
    return 0;
}

/* WM_PV_IMG_READY 落点 (窗口过程调, Cur 已绑本窗): 代号/归属/选中校验后转本域位图入缓存 */
void XjsPreviewImgAdopt(HWND hwnd, long long gen) {
    XjsBitmap* nb = NULL;
    int ctx = -1, fileId = -1;
    {
        XjsImgJobCs lk;
        if (s_imgDone.gen != gen || s_imgDone.hwnd != hwnd) return;   /* 槽已被更新结果覆盖 */
        ctx = s_imgDone.ctx;
        fileId = s_imgDone.fileId;
        if (!s_imgDone.failed && s_imgDone.w > 0)
            nb = XjsBitmapFromBgra(s_imgDone.bgra.data(), s_imgDone.w, s_imgDone.h, s_imgDone.w * 4);
        s_imgDone.bgra.clear();
        s_imgDone.bgra.shrink_to_fit();
    }
    XjsSearchWindow* w = XjsSearchWindow::Cur();
    if (!w || w->hWnd != hwnd || w->previewFileId != fileId) {   /* 期间已换窗/换选中 */
        if (nb) nb->Release();
        return;
    }
    if (ctx == 1) {
        if (s_lbBmp) s_lbBmp->Release();
        s_lbBmp = nb;
        s_lbPending = false;
        s_lbFailed = (nb == NULL);
        w->Invalidate();
    } else {
        if (s_pvScaled) s_pvScaled->Release();
        s_pvScaled = nb;
        s_pvPending = false;
        s_pvFailed = (nb == NULL);
        if (w->hWnd) {   /* 只失效预览区 (同插件交付口径) */
            XjsRect b = w->layout.preview;
            RECT r = { (int)b.left, (int)b.top, (int)b.right, (int)b.bottom };
            InvalidateRect(w->hWnd, &r, FALSE);
        } else {
            w->Invalidate();
        }
    }
}

/* ==================== 文本异步装载 (流式分块, 2026-09-29) ====================
 * 旧实现 = UI 线程同步读 ≤5MB + 解码 + 全量分行 (大文本选中卡顿; >5MB 直接拒览)。
 * 现全异步流式: UI 只投作业; 单 worker 256KB 一块 读→BOM/严格UTF-8/GBK 定编码→解码→
 * 分行 (跨块残字节/残行留 session, 编码只探一次), 行集回 UI 追加进 s_textLines;
 * 渲染帧快到底 (300 行内) 自动续投下一块 — 任意大小文本可滚动预览 (单文件加载上限
 * 64MB 原始字节, 到顶不再续载)。超长残行 (>512K 字符) 强制出行封内存。 */
static const int XJS_TEXT_CHUNK = 256 * 1024;
static const long long XJS_TEXT_LOAD_MAX = 64LL * 1024 * 1024;
static const size_t XJS_TEXT_WLINE_CAP = 512 * 1024;

static struct XjsTxtJobCs { CRITICAL_SECTION cs; XjsTxtJobCs() { InitializeCriticalSectionAndSpinCount(&cs, 100); } } s_txtJobCs;
static HANDLE s_txtThread = NULL, s_txtWake = NULL;
static long long s_txtGen = 0;

struct XjsTxtReq {
    bool pending = false;
    long long gen = 0;
    HWND hwnd = NULL;
    int fileId = -1;
    std::wstring path;      /* first 才带 */
    long long size = 0;     /* first 才带 */
    bool first = false;
};
static XjsTxtReq s_txtReq;

struct XjsTxtDone {
    long long gen = 0;
    HWND hwnd = NULL;
    int fileId = -1;
    bool first = false;
    std::vector<std::wstring> lines;   /* 本块完整行 (跨块残行留 worker session) */
    bool hasMore = false;
    long long total = 0, loaded = 0;
};
static XjsTxtDone s_txtDone;

/* worker 专属串行 session (仅 worker 线程触碰): 编码/残字节/残行/进度 */
struct XjsTxtSess {
    int fileId = -1;
    std::wstring path;
    long long size = 0, nextOff = 0, consumed = 0;
    int enc = 0;                     /* 0=未定 1=UTF-8 2=GBK 3=UTF-16LE 4=UTF-16BE */
    std::vector<uint8_t> rawCarry;   /* 不完整多字节序列 (≤4 字节, 块尾) */
    std::wstring wcarry;             /* 跨块残行 */
};
static XjsTxtSess s_txtSess;

static DWORD WINAPI XjsTxtThreadProc(LPVOID);

/* UI 线程投作业 (first=从文件头开会话; 续块只带 fileId, 路径/大小 session 自持) */
static void XjsPreviewTextAsk(int fileId, const std::wstring& path, long long size, bool first) {
    s_textPending = true;   /* 在途闸: 渲染帧/选中变化不重复投 (adopt 复位) */
    XjsTxtJobCs lk;
    s_txtReq.pending = true;
    s_txtReq.gen = ++s_txtGen;
    s_txtReq.hwnd = g_hWnd;
    s_txtReq.fileId = fileId;
    if (first) {
        s_txtReq.path = path;
        s_txtReq.size = size;
    }
    s_txtReq.first = first;
    if (!s_txtWake) {
        s_txtWake = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (s_txtWake) s_txtThread = CreateThread(NULL, 0, XjsTxtThreadProc, NULL, 0, NULL);
    }
    if (s_txtWake) SetEvent(s_txtWake);
}

/* 块尾不完整序列 → carry (UTF-8 按前导字节应有续字节计; GBK 尾字节是双字节前导;
   UTF-16 奇数字节)。enc==0 先按 UTF-8 形状试剥 (探测在其后, 误剥的原样随 carry 回来) */
static void TxtStripTail(std::vector<uint8_t>& buf, int enc, std::vector<uint8_t>& carry) {
    size_t n = buf.size();
    if (!n) return;
    size_t keep = 0;
    if (enc == 3 || enc == 4) {
        if (n & 1) keep = 1;
    } else if (enc == 2) {
        uint8_t b = buf[n - 1];
        if (b >= 0x81 && b <= 0xFE) keep = 1;
    } else {
        size_t i = n, conts = 0;
        while (i > 0 && (buf[i - 1] & 0xC0) == 0x80) { i--; conts++; if (conts >= 4) break; }
        if (i > 0 && conts < 4) {
            uint8_t lead = buf[i - 1];
            int need = (lead & 0x80) == 0 ? 0
                     : (lead & 0xE0) == 0xC0 ? 1
                     : (lead & 0xF0) == 0xE0 ? 2
                     : (lead & 0xF8) == 0xF0 ? 3 : -1;
            if (need > (int)conts) keep = conts + 1;
        }
    }
    if (keep) {
        carry.assign(buf.end() - keep, buf.end());
        buf.resize(n - keep);
    }
}

/* 只分行 (调用方保证 text 以 \n 结尾或为空; 无 20000 行截断 — 流式追加无上限) */
static void TxtSplitComplete(const std::wstring& text, std::vector<std::wstring>& lines) {
    size_t start = 0;
    for (;;) {
        size_t nl = text.find(L'\n', start);
        if (nl == std::wstring::npos) break;
        std::wstring line = text.substr(start, nl - start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        lines.push_back(std::move(line));
        start = nl + 1;
    }
}

static DWORD WINAPI XjsTxtThreadProc(LPVOID) {
    for (;;) {
        WaitForSingleObject(s_txtWake, INFINITE);
        for (;;) {
            XjsTxtReq req;
            {
                XjsTxtJobCs lk;
                if (!s_txtReq.pending) break;
                req = s_txtReq;
                s_txtReq.pending = false;
            }
            if (!IsWindow(req.hwnd)) continue;   /* 归属窗已没: 弃 (窗口重建自有新会话) */
            if (req.first || s_txtSess.fileId != req.fileId) {
                s_txtSess = XjsTxtSess{};
                s_txtSess.fileId = req.fileId;
                s_txtSess.path = req.path;
                s_txtSess.size = req.size;
            } else if (s_txtSess.path.empty()) {
                continue;   /* 续块但会话没头 (不该发生): 弃 */
            }
            XjsTxtDone done;
            done.gen = req.gen;
            done.hwnd = req.hwnd;
            done.fileId = req.fileId;
            done.first = req.first;
            done.total = s_txtSess.size;
            bool stop = false;
            HANDLE h = CreateFileW(s_txtSess.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h == INVALID_HANDLE_VALUE) {
                stop = true;   /* 文件没了/被占: 结束会话 */
            } else {
                LARGE_INTEGER li;
                li.QuadPart = s_txtSess.nextOff;
                SetFilePointerEx(h, li, NULL, FILE_BEGIN);
                std::vector<uint8_t> chunk((size_t)XJS_TEXT_CHUNK);
                DWORD rd = 0;
                BOOL ok = ReadFile(h, chunk.data(), XJS_TEXT_CHUNK, &rd, NULL);
                CloseHandle(h);
                if (!ok || rd == 0) {
                    stop = true;
                } else {
                    chunk.resize(rd);
                    std::vector<uint8_t> buf = std::move(s_txtSess.rawCarry);
                    s_txtSess.rawCarry.clear();
                    buf.insert(buf.end(), chunk.begin(), chunk.end());
                    /* 首块: BOM 定编码 + 二进制判 (BOM 剥后查 NUL — UTF-16 文本不再误判二进制) */
                    bool binary = false;
                    if (s_txtSess.enc == 0 && buf.size() >= 2) {
                        if ((unsigned char)buf[0] == 0xFF && (unsigned char)buf[1] == 0xFE) { s_txtSess.enc = 3; buf.erase(buf.begin(), buf.begin() + 2); }
                        else if ((unsigned char)buf[0] == 0xFE && (unsigned char)buf[1] == 0xFF) { s_txtSess.enc = 4; buf.erase(buf.begin(), buf.begin() + 2); }
                        else if (buf.size() >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) { s_txtSess.enc = 1; buf.erase(buf.begin(), buf.begin() + 3); }
                        size_t scan = buf.size() < 4096 ? buf.size() : 4096;
                        for (size_t i = 0; i < scan; i++)
                            if (buf[i] == 0) { binary = true; break; }
                    }
                    if (binary) {
                        stop = true;   /* 二进制不预览 (信息卡兜底), 会话终止 */
                    } else {
                        std::vector<uint8_t> tail;
                        TxtStripTail(buf, s_txtSess.enc, tail);
                        s_txtSess.rawCarry = std::move(tail);
                        /* 编码探测: 严格 UTF-8 过 = UTF-8, 否则 GBK (BOM 已剥, 误剥的原样随 carry 回流) */
                        if (s_txtSess.enc == 0) {
                            int wl = buf.empty() ? 0 : MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (LPCCH)buf.data(), (int)buf.size(), NULL, 0);
                            s_txtSess.enc = (wl > 0 || buf.empty()) ? 1 : 2;
                        }
                        std::wstring text;
                        if (s_txtSess.enc == 3 || s_txtSess.enc == 4) {
                            if (s_txtSess.enc == 4)
                                for (size_t k = 0; k + 1 < buf.size(); k += 2) std::swap(buf[k], buf[k + 1]);
                            text.resize(buf.size() / 2);
                            if (!text.empty()) memcpy(&text[0], buf.data(), text.size() * 2);
                        } else if (s_txtSess.enc == 2 || buf.empty()) {
                            int gl = buf.empty() ? 0 : MultiByteToWideChar(936, 0, (LPCCH)buf.data(), (int)buf.size(), NULL, 0);
                            if (gl > 0) { text.resize(gl); MultiByteToWideChar(936, 0, (LPCCH)buf.data(), (int)buf.size(), &text[0], gl); }
                        } else {
                            int wl = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (LPCCH)buf.data(), (int)buf.size(), NULL, 0);
                            if (wl <= 0) {   /* 中段损坏: 本块按 GBK 尽力解 */
                                int gl = MultiByteToWideChar(936, 0, (LPCCH)buf.data(), (int)buf.size(), NULL, 0);
                                if (gl > 0) { text.resize(gl); MultiByteToWideChar(936, 0, (LPCCH)buf.data(), (int)buf.size(), &text[0], gl); }
                            } else {
                                text.resize(wl);
                                MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (LPCCH)buf.data(), (int)buf.size(), &text[0], wl);
                            }
                        }
                        /* 跨块残行拼合: 最后一个 \n 之前出行, 之后留 wcarry */
                        std::wstring combined;
                        combined.swap(s_txtSess.wcarry);
                        combined.append(text);
                        size_t lastNl = combined.find_last_of(L'\n');
                        if (lastNl == std::wstring::npos) {
                            s_txtSess.wcarry = std::move(combined);
                            if (s_txtSess.wcarry.size() > XJS_TEXT_WLINE_CAP) {   /* 超长残行强制出行 (显示端本就省略) */
                                done.lines.push_back(std::move(s_txtSess.wcarry));
                                s_txtSess.wcarry.clear();
                            }
                        } else {
                            s_txtSess.wcarry = combined.substr(lastNl + 1);
                            combined.resize(lastNl + 1);
                            TxtSplitComplete(combined, done.lines);
                        }
                        s_txtSess.nextOff += rd;
                        s_txtSess.consumed += rd;
                    }
                }
            }
            done.loaded = s_txtSess.consumed;
            done.hasMore = !stop && s_txtSess.nextOff < s_txtSess.size && s_txtSess.consumed < XJS_TEXT_LOAD_MAX;
            {
                XjsTxtJobCs lk;
                s_txtDone = std::move(done);
            }
            PostMessage(req.hwnd, WM_PV_TXT_READY, 0, (LPARAM)req.gen);
        }
    }
    return 0;
}

/* WM_PV_TXT_READY 落点 (窗口过程调): 代号/归属/选中校验后追加行集 */
void XjsPreviewTextAdopt(HWND hwnd, long long gen) {
    std::vector<std::wstring> lines;
    int fileId = -1;
    bool first = false, hasMore = false;
    long long total = 0, loaded = 0;
    {
        XjsTxtJobCs lk;
        if (s_txtDone.gen != gen || s_txtDone.hwnd != hwnd) return;   /* 槽已被更新结果覆盖 */
        fileId = s_txtDone.fileId;
        first = s_txtDone.first;
        hasMore = s_txtDone.hasMore;
        total = s_txtDone.total;
        loaded = s_txtDone.loaded;
        lines = std::move(s_txtDone.lines);
        s_txtDone.lines.clear();
    }
    XjsSearchWindow* w = XjsSearchWindow::Cur();
    if (!w || w->hWnd != hwnd || w->previewFileId != fileId) return;   /* 期间已换窗/换选中 */
    if (first) {
        w->previewTextLines.clear();
        w->previewTextScroll = 0;
        w->previewTextFileId = fileId;
    }
    if (!lines.empty())
        w->previewTextLines.insert(w->previewTextLines.end(), std::make_move_iterator(lines.begin()),
                                   std::make_move_iterator(lines.end()));
    w->previewTextHasMore = hasMore;
    w->previewTextTotal = total;
    w->previewTextLoaded = loaded;
    w->previewTextPending = false;
    if (w->hWnd) {
        XjsRect b = w->layout.preview;
        RECT r = { (int)b.left, (int)b.top, (int)b.right, (int)b.bottom };
        InvalidateRect(w->hWnd, &r, FALSE);
    } else {
        w->Invalidate();
    }
}

/* 框选拖动中预览节流 (鼠标交互瞬态): 上次放行时刻 (时间戳比对, 同单击打开防重的无时钟口径;
   拖动中鼠标持续产生 move, 间隔一够下一帧就刷新, 松开经解除点补刷终态) */
static ULONGLONG s_pvMarqueeLastLoad = 0;

/* 选中/聚焦目标变化: 刷新面板内容 (锁定时不跟随) */
void XjsPreviewUpdateSelection() {
    if (XjsMarqueeMoved()) {
        /* 框选拖动中: 节流实时跟随 (用户口径"框选改成实时", 取代早期完全冻结) —
           限频重载挡住逐行读盘/解码 */
        ULONGLONG now = GetTickCount64();
        if (now - s_pvMarqueeLastLoad < XJS_MARQUEE_PV_MS) return;
        s_pvMarqueeLastLoad = now;
    }
    XjsPluginOnSelectionChanged();   /* 插件 events 订阅: 预览刷新 = 选中变化的统一汇点 */
    if (g_plugPanelOn) return;       /* 面板接管中: 选中变化不覆盖聊天 (原版 ensurePreviewGuard 口径; 只保留上方事件派发) */
    /* 锁定 (面板头部图钉): 面板钉住当前文件, 选中变化不再跟随 — 只保留上方插件事件派发。
       无内容时 (fileId<0) 不算"钉住", 放行走正常装载, 装到内容后锁定才生效 */
    if (g_previewLocked && g_previewFileId >= 0) return;
    int idx = XjsSelPrimaryIdx();
    int fileId = -1;
    if (idx >= 0 && g_result) fileId = xjs_result_GetFileId(g_result, idx);
    if (fileId == g_previewFileId) return;
    g_previewFileId = fileId;
    g_previewImgW = 0;
    g_previewImgH = 0;    /* 原始尺寸随目标复位 (图片在下方只读头重取) */
    s_imgZoom = 0;        /* 复位 = 适应窗口 (哨兵值, 渲染帧现算适配比例) */
    s_rot = 0;            /* 旋转随文件复位 (灯箱共用同字段) */
    s_lightbox = false;   /* 内容已换: 灯箱随手收 (防程序性选中刷新后残留看不见的模态) */
    XjsPreviewImageCacheDropAll(XjsSearchWindow::Cur());
    XjsPreviewPluginDrop();
    s_pvPlugReq++;   /* 世代推进: 在途的旧交付作废 */
    s_textLines.clear();
    s_textFileId = -1;
    s_textScroll = 0;
    s_textPending = false;
    s_textHasMore = false;   /* 文本流式状态随目标复位 (下方 Ask 重新置 pending) */
    if (fileId >= 0 && g_engine && g_previewVisible) {   /* 面板未开启只记 ID 不读文件 (隐藏期读图/读文本纯属浪费) */
        std::wstring name = Utf8ToUtf16(xjs_db_GetName(g_engine, fileId));
        std::wstring path = Utf8ToUtf16(xjs_db_GetPath(g_engine, fileId));
        if (XjsPreviewPluginTryTake(fileId, path, name)) {   /* 插件接管: 异步交付, 不走内置分类 */
            XjsSearchWindow::Cur()->Invalidate();
            return;
        }
        if (XjsIsImageExt(name)) {
            /* 图片文件: 只读头取原始尺寸 (毫秒级, 几十 M 也不冻结); 像素走异步装载
               (渲染帧投作业, 工作线程解码缩放后回主线程 — UI 线程零解码, 2026-09-29) */
            int iw2 = 0, ih2 = 0;
            if (XjsImageFileDims(path, &iw2, &ih2)) {
                g_previewImgW = iw2;
                g_previewImgH = ih2;
            }
        } else if (XjsIsTextExt(name)) {
            /* 文本文件: 异步流式装载 (首块 256KB 到货即显, 快到底自动续载 — 大文件可
               滚动预览, UI 线程零读取零解码, 2026-09-29) */
            XjsPreviewTextAsk(fileId, path, xjs_db_GetFileSize(g_engine, fileId), true);
        }
    }
    XjsSearchWindow::Cur()->Invalidate();
}

/* 滚轮缩放预览图片 (普通滚轮/Ctrl+滚轮同义, dir=±1): 适应态 (0 哨兵) 从当前适配比例
   起步, 每格 ×1.25; 范围 5%~400% (400 上限同灯箱)。绝对比例不随布局漂移, 换文件复位适应 */
void XjsPreviewWheel(int dir) {
    float z = (s_imgZoom > 0) ? s_imgZoom : s_imgBase;
    z = (dir > 0) ? z * 1.25f : z / 1.25f;
    if (z > 4) z = 4;
    if (z < 0.05f) z = 0.05f;
    s_imgZoom = z;
    XjsSearchWindow::Cur()->Invalidate();
}

bool XjsPreviewIsText() {
    return g_previewFileId >= 0 && s_textFileId == g_previewFileId && !s_textLines.empty();
}

bool XjsPreviewIsImage() {
    if (!g_previewVisible || g_previewFileId < 0) return false;
    if (XjsPreviewPluginBitmap(g_previewFileId)) return true;
    return g_previewImgW > 0;   /* 内置图: 选目标时已只读头取到尺寸 */
}

/* 文本预览普通滚轮滚动 (每格 3 行) */
void XjsPreviewScrollLines(int dir) {
    s_textScroll += dir * 3;
    if (s_textScroll < 0) s_textScroll = 0;
    XjsSearchWindow::Cur()->Invalidate();
}

void XjsPreviewToggle() {
    if (g_previewVisible && g_plugPanelOn) XjsPreviewPanelCloseForToggle();   /* 藏面板先结束接管 (会话状态不跨隐藏) */
    g_previewVisible = !g_previewVisible;
    if (!g_previewVisible) {
        s_lightbox = false;   /* 藏面板随手收灯箱 (防下次开启吃到残留态) */
        XjsPreviewImageCacheDropAll(XjsSearchWindow::Cur());
    }
    if (g_previewVisible) {
        /* 开启即回填当前选中: 隐藏期间 UpdateSelection 只记 ID 未读文件,
           置 -1 破去重, 让下方加载真正执行 (否则同一选中会吃到旧缓存/空内容) */
        g_previewFileId = -1;
        XjsPreviewUpdateSelection();
    }
    XjsSaveConfig();
    XjsClampScroll();
    XjsSearchWindow::Cur()->Invalidate();
}

/* 预览卡片行 = 卡片当前显示的项 (g_previewFileId): 卡上按钮 (复制序列号/查找大目录·大文件/
   定位/打开) 作用于用户看到的这张卡 — 用列表选中项会在 图钉锁定预览 时与卡片分叉, 按钮静默
   落空 (表象 = "复制点不了") */
static XjsRowData* XjsPreviewCardRow() {
    if (g_previewFileId < 0 || !g_result) return NULL;
    int idx = xjs_result_GetFileIdIndex(g_result, g_previewFileId);
    return idx >= 0 ? XjsEnsureRowData(idx) : NULL;
}

/* 查找大目录: 按父路径聚合子项数 (SQL GROUP BY; 空间地图插件另提供矩形树图视图) */
void XjsPreviewQueryBigDirs() {
    XjsRowData* rd = XjsPreviewCardRow();
    if (!rd || !rd->isDrive) return;
    /* 引擎 SQL 的 LIKE 里 \ 是转义字符, 路径分隔符必须写 \\ (单写一个 \ 匹配 0 条) */
    std::wstring sql = L"SELECT ParentPath, COUNT(*) AS cnt FROM alltable WHERE ParentPath LIKE '" +
        rd->name + L"\\\\%' GROUP BY ParentPath ORDER BY cnt DESC;";
    g_mode = XMODE_SQL;
    XjsSearchSetText(sql);   /* 自绘搜索框: 置入即触发搜索 */
}

void XjsPreviewQueryBigFiles() {
    XjsRowData* rd = XjsPreviewCardRow();
    if (!rd || !rd->isDrive) return;
    std::wstring sql = L"SELECT * FROM alltable WHERE IsDir=0 AND ParentPath LIKE '" +
        rd->name + L"\\\\%' AND Size > '100M' ORDER BY Size DESC;";
    g_mode = XMODE_SQL;
    XjsSearchSetText(sql);   /* 自绘搜索框: 置入即触发搜索 */
}

/* ==================== 渲染 ==================== */

/* 面板按钮悬停高亮 (鼠标交互瞬态, 同列拖动文件级 static 口径): 存命中命令号, 0=无。
   渲染按命令号点亮对应按钮; 变化才整帧失效 — 面板内空处移动不重绘 */
static int s_pvHover = 0;
static bool XjsPvHover(int cmd) { return s_pvHover == cmd; }

/* 信息行: label 左 / value 右; 返回下一行 y */
static float XjsInfoRow(float yTop, const wchar_t* label, const std::wstring& value, float px) {
    float rowH = XSF(24);
    float right = g_layout.preview.right - XSF(16);
    XjsDrawEllText(label, XjsRectF(px, yTop, px + XSF(70), yTop + rowH), g_tfTiny, g_br[XTH_TEXT_FAINT]);
    XjsDrawEllText(value, XjsRectF(px + XSF(76), yTop, right, yTop + rowH), g_tfDim, g_br[XTH_TEXT_DIM]);
    return yTop + rowH;
}

/* 描边按钮 (hover=悬停高亮) */
static void XjsPanelButton(const XjsRect& r, const wchar_t* text, int iconKind, bool hover) {
    g_rt->FillRoundedRectangle(XjsRoundedRectF(r, XSF(8), XSF(8)), g_br[hover ? XTH_ROW_HOVER : XTH_PANEL2]);
    g_rt->DrawRoundedRectangle(XjsRoundedRectF(r, XSF(8), XSF(8)), g_br[hover ? XTH_ACCENT : XTH_BORDER], 1.0f);
    float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    float tw = XjsMeasureText(text, g_tfMenu);
    float tx = cx - tw / 2 - XSF(10);
    XjsBrush* bc = (XjsBrush*)g_br[XTH_TEXT_DIM];
    /* 小图标 */
    if (iconKind == 0) {
        /* 定位: 十字准星 */
        float ix = tx + XSF(6);
        g_rt->DrawEllipse(XjsEllipseF(XjsPoint2F(ix, cy), XSF(4.5f), XSF(4.5f)), bc, 1.2f);
        g_rt->DrawLine(XjsPoint2F(ix, cy - XSF(6.5f)), XjsPoint2F(ix, cy - XSF(3.5f)), bc, 1.2f);
        g_rt->DrawLine(XjsPoint2F(ix, cy + XSF(3.5f)), XjsPoint2F(ix, cy + XSF(6.5f)), bc, 1.2f);
        g_rt->DrawLine(XjsPoint2F(ix - XSF(6.5f), cy), XjsPoint2F(ix - XSF(3.5f), cy), bc, 1.2f);
        g_rt->DrawLine(XjsPoint2F(ix + XSF(3.5f), cy), XjsPoint2F(ix + XSF(6.5f), cy), bc, 1.2f);
    } else if (iconKind == 1) {
        /* 打开: → */
        float ix = tx + XSF(6);
        g_rt->DrawLine(XjsPoint2F(ix - XSF(4.5f), cy), XjsPoint2F(ix + XSF(4), cy), bc, 1.3f);
        g_rt->DrawLine(XjsPoint2F(ix + XSF(1), cy - XSF(3)), XjsPoint2F(ix + XSF(4.5f), cy), bc, 1.3f);
        g_rt->DrawLine(XjsPoint2F(ix + XSF(1), cy + XSF(3)), XjsPoint2F(ix + XSF(4.5f), cy), bc, 1.3f);
    }
    XjsRect tr = XjsRectF(tx + XSF(14), r.top, r.right, r.bottom);
    g_rt->DrawText(text, (UINT32)wcslen(text), g_tfMenu, tr, g_br[XTH_TEXT]);
}

/* 圆形小按钮 (面板头: 锁/最大/关闭; 图片工具条: 左旋/右旋; hover=悬停高亮) */
static void XjsPanelHeaderBtn(const XjsRect& r, int kind, bool active, bool hover) {
    if (active || hover) g_rt->FillRoundedRectangle(XjsRoundedRectF(r, XSF(6), XSF(6)), g_br[XTH_ROW_HOVER]);
    float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    XjsBrush* bc = active ? (XjsBrush*)g_br[XTH_ACCENT] : (XjsBrush*)g_br[XTH_TEXT_DIM];
    if (kind == 0) {
        /* 锁 */
        g_rt->DrawRectangle(XjsRectF(cx - XSF(4), cy - XSF(1), cx + XSF(4), cy + XSF(5)), bc, 1.2f);
        g_rt->DrawEllipse(XjsEllipseF(XjsPoint2F(cx, cy - XSF(2.5f)), XSF(2.8f), XSF(2.8f)), bc, 1.2f);
    } else if (kind == 1) {
        g_rt->DrawRectangle(XjsRectF(cx - XSF(4.5f), cy - XSF(4.5f), cx + XSF(4.5f), cy + XSF(4.5f)), bc, 1.2f);
    } else if (kind == 2) {
        g_rt->DrawLine(XjsPoint2F(cx - XSF(4), cy - XSF(4)), XjsPoint2F(cx + XSF(4), cy + XSF(4)), bc, 1.3f);
        g_rt->DrawLine(XjsPoint2F(cx + XSF(4), cy - XSF(4)), XjsPoint2F(cx - XSF(4), cy + XSF(4)), bc, 1.3f);
    } else {
        /* 旋转 (3=左旋/逆时针 4=右旋/顺时针): 圆环 + 顶部切向箭头, 镜像两方向 */
        float rr = XSF(4.2f);
        g_rt->DrawEllipse(XjsEllipseF(XjsPoint2F(cx, cy), rr, rr), bc, 1.3f);
        float dir = (kind == 4) ? 1.0f : -1.0f;   /* 箭头朝向: 右旋朝右, 左旋朝左 */
        float ax = cx + dir * XSF(2.5f), ay = cy - rr;
        g_rt->DrawLine(XjsPoint2F(ax - dir * XSF(2.2f), ay - XSF(1.8f)), XjsPoint2F(ax, ay), bc, 1.3f);
        g_rt->DrawLine(XjsPoint2F(ax, ay), XjsPoint2F(ax - dir * XSF(2.2f), ay + XSF(1.8f)), bc, 1.3f);
    }
}

/* 图片工具条文本小按钮 (1:1/适应): 圆角描边底 hover 高亮, 文字水平居中 (纵对齐恒中) */
static void XjsPvTextBtn(const XjsRect& r, const wchar_t* text, bool hover) {
    g_rt->FillRoundedRectangle(XjsRoundedRectF(r, XSF(5), XSF(5)), g_br[hover ? XTH_ROW_HOVER : XTH_PANEL2]);
    g_rt->DrawRoundedRectangle(XjsRoundedRectF(r, XSF(5), XSF(5)), g_br[hover ? XTH_ACCENT : XTH_BORDER], 1.0f);
    float tw = XjsMeasureText(text, g_tfTiny);
    float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    g_rt->DrawText(text, (UINT32)wcslen(text), g_tfTiny,
        XjsRectF(cx - tw / 2, cy - XSF(9), cx + tw / 2, cy + XSF(9)),
        g_br[hover ? XTH_TEXT : XTH_TEXT_DIM]);
}

void XjsPreviewRender() {
    if (!g_previewVisible) return;
    XjsLayout& L = g_layout;
    XjsRect p = L.preview;
    /* resizer */
    XjsRect resizer = XjsRectF(p.left, p.top, p.left + XSF(3), p.bottom);
    g_rt->FillRectangle(resizer, g_previewDrag ? (XjsBrush*)g_br[XTH_ACCENT] : (XjsBrush*)g_br[XTH_BORDER]);
    /* 面板体 */
    XjsRect body = XjsRectF(p.left + XSF(3), p.top, p.right, p.bottom);
    g_rt->FillRectangle(body, g_br[XTH_PANEL]);
    g_rt->FillRectangle(XjsRectF(body.left, body.top, body.left + 1, body.bottom), g_br[XTH_BORDER]);
    s_hits.valid = true;
    /* 命中表 = 本帧真实画出的按钮: 分支按钮先全部清零, 由下面各分支画到才回写 —
       否则上一帧 (驱动器卡/文件卡) 的按钮矩形残留仍可命中, 点出错误命令 */
    s_hits.copySerial = s_hits.bigDirs = s_hits.bigFiles = {};
    s_hits.locate = s_hits.open = {};
    s_hits.image = {};
    /* 面板接管中: 整块面板体 (含原头部带) 交给插件位图, 宿主头部 (标题/锁/宽窄/✕) 不画 —
       关闭入口 = 插件头部自绘 ✕ (SDK PanelClose, 按打开前状态恢复预览); 头部按钮命中矩形按帧清零 */
    if (g_plugPanelOn) {
        s_hits.lockBtn = s_hits.maxBtn = s_hits.closeBtn = {};
        XjsPreviewPanelRender();
        return;
    }
    float px = body.left + XSF(14);
    float pw = body.right - px;

    /* 头部: 小图标 + 标题 + 锁/最大/关闭 */
    float headH = XSF(40);
    XjsRowData* rd = NULL;
    int idx = -1;
    if (g_previewFileId >= 0 && g_result) {
        int fileIdx = xjs_result_GetFileIdIndex(g_result, g_previewFileId);
        if (fileIdx >= 0) { rd = XjsEnsureRowData(fileIdx); idx = fileIdx; }
    }
    std::wstring title = rd ? rd->name : XjsT(L"通用词.预览");
    {
        float ty = p.top + XSF(8);
        if (rd) {
            XjsBitmap* ic = XjsGetRowIcon(idx, rd->fileId, XjsIconFetchPx(32), "preview");
            if (ic) {
                float isz = XSF(22);
                g_rt->DrawBitmap(ic, XjsRectF(px, ty, px + isz, ty + isz));
                ic->Release();
            }
        }
        XjsRect tr = XjsRectF(px + (rd ? XSF(28) : 0), ty, body.right - XSF(100), ty + XSF(24));
        XjsDrawEllText(title, tr, g_tfCardVal, g_br[XTH_TEXT]);
    }
    s_hits.lockBtn = XjsRectF(body.right - XSF(96), p.top + XSF(8), body.right - XSF(72), p.top + XSF(32));
    s_hits.maxBtn = XjsRectF(body.right - XSF(68), p.top + XSF(8), body.right - XSF(44), p.top + XSF(32));
    s_hits.closeBtn = XjsRectF(body.right - XSF(40), p.top + XSF(8), body.right - XSF(16), p.top + XSF(32));
    XjsPanelHeaderBtn(s_hits.lockBtn, 0, g_previewLocked, XjsPvHover(3));
    XjsPanelHeaderBtn(s_hits.maxBtn, 1, false, XjsPvHover(2));
    XjsPanelHeaderBtn(s_hits.closeBtn, 2, false, XjsPvHover(1));
    g_rt->FillRectangle(XjsRectF(body.left, p.top + headH, body.right, p.top + headH + 1), g_br[XTH_BORDER]);

    if (!rd || g_previewFileId < 0) {
        std::wstring tip = XjsT(L"预览.单击预览");
        g_rt->DrawText(tip.c_str(), (UINT32)tip.length(), g_tfChip,
            XjsRectF(body.left, p.top + headH, body.right, p.top + headH + XSF(120)), g_br[XTH_TEXT_FAINT]);
        return;
    }

    float cy = p.top + headH + XSF(14);
    float contentBottom = L.statusbar.top - XSF(52);   /* 底部留给 定位/打开 按钮 */

    if (rd->isDrive) {
        /* ===== 驱动器信息卡 ===== */
        /* 大图标 + 名称/副标题 + 大百分比 */
        XjsBitmap* ic = XjsPreviewIcon(rd->fileId);
        float isz = XSF(56);
        if (ic) {
            g_rt->DrawBitmap(ic, XjsRectF(px, cy, px + isz, cy + isz));
            ic->Release();
        }
        std::wstring pctText = XjsNumText(rd->drivePercent) + L"%";
        float pctW = XjsMeasureText(pctText.c_str(), g_tfBig);
        g_rt->DrawText(pctText.c_str(), (UINT32)pctText.length(), g_tfBig,
            XjsRectF(body.right - XSF(16) - pctW, cy, body.right - XSF(16), cy + XSF(34)), XjsTempBrush(XjsDriveColor(rd->drivePercent)));
        XjsRect tr = XjsRectF(px + isz + XSF(12), cy + XSF(4), body.right - XSF(16) - pctW - XSF(8), cy + XSF(28));
        XjsDrawEllText(rd->driveLabel, tr, g_tfCardVal, g_br[XTH_TEXT]);
        std::wstring sub = XjsT(L"预览.本地磁盘") + (rd->driveFs.empty() ? L"" : (L" · " + rd->driveFs));
        XjsDrawEllText(sub, XjsRectF(tr.left, cy + XSF(28), tr.right, cy + XSF(46)), g_tfTiny, g_br[XTH_TEXT_FAINT]);
        cy += xf_max(isz, XSF(48)) + XSF(10);
        /* 容量条 (正式版 .drive-bar: --drive-track 轨道 + .drive-bar-fill 蓝→占用色渐变+光晕;
           渐变随填充宽铺, 末端恒为占用色) */
        {
            XjsRect track = XjsRectF(px, cy, body.right - XSF(16), cy + XSF(6));
            g_rt->FillRoundedRectangle(XjsRoundedRectF(track, XSF(3), XSF(3)), XjsTempBrush(g_skin.driveTrack));
            float fillW = (track.right - track.left) * rd->drivePercent / 100;
            if (fillW > XSF(3)) {
                XjsColor endC = XjsDriveColor(rd->drivePercent);
                /* 源样式 .drive-bar-fill 带 box-shadow 0 0 8px rgba(占用色,.35), 效果管线帧必废
                   (跨渲染域铁律), 逐层外扩低透明胶囊近似 */
                for (int i = 3; i >= 1; i--) {
                    XjsColor gc = endC; gc.a *= 0.12f - 0.04f * (i - 1);
                    g_rt->FillRoundedRectangle(XjsRoundedRectF(XjsRectF(
                        track.left - XSF((float)i), track.top - XSF((float)i),
                        track.left + fillW + XSF((float)i), track.bottom + XSF((float)i)),
                        XSF(3 + i), XSF(3 + i)), XjsTempBrush(gc));
                }
                XjsGradientStop gs[2] = { { 0.0f, XjsCol(0x60a5fa) }, { 1.0f, endC } };
                XjsGradBrush* br = NULL;
                g_rt->CreateLinearGradientBrush(XjsPoint2F(track.left, 0), XjsPoint2F(track.left + fillW, 0), gs, 2, &br);
                if (br) {
                    g_rt->FillRoundedRectangle(XjsRoundedRectF(
                        XjsRectF(track.left, track.top, track.left + fillW, track.bottom), XSF(3), XSF(3)), br);
                    br->Release();
                }
            }
            cy += XSF(16);
        }
        /* 三卡片: 已用 / 剩余 / 总容量 */
        {
            float gap = XSF(8);
            float cw = (pw - gap * 2) / 3;
            const wchar_t* labels[3] = { XjsT(L"预览.已用"), XjsT(L"预览.剩余"), XjsT(L"预览.总容量") };
            std::wstring vals[3] = {
                Utf8ToUtf16(xjs_util_FormatFileSize(rd->driveUsed)),
                Utf8ToUtf16(xjs_util_FormatFileSize(rd->driveFree)),
                Utf8ToUtf16(xjs_util_FormatFileSize(rd->driveTotal)) };
            for (int i = 0; i < 3; i++) {
                XjsRect cr = XjsRectF(px + i * (cw + gap), cy, px + i * (cw + gap) + cw, cy + XSF(52));
                g_rt->FillRoundedRectangle(XjsRoundedRectF(cr, XSF(8), XSF(8)), g_br[XTH_PANEL2]);
                std::wstring lab = labels[i];
                g_rt->DrawText(lab.c_str(), (UINT32)lab.length(), g_tfTiny,
                    XjsRectF(cr.left + XSF(10), cr.top + XSF(6), cr.right, cr.top + XSF(22)), g_br[XTH_TEXT_FAINT]);
                g_rt->DrawText(vals[i].c_str(), (UINT32)vals[i].length(), g_tfCardVal,
                    XjsRectF(cr.left + XSF(10), cr.top + XSF(24), cr.right - XSF(4), cr.bottom - XSF(4)), g_br[XTH_TEXT]);
            }
            cy += XSF(64);
        }
        /* 信息行 */
        cy = XjsInfoRow(cy, XjsT(L"预览.文件系统"), rd->driveFs.empty() ? L"-" : rd->driveFs, px);
        std::wstring serial;
        {
            wchar_t sb[32];
            _snwprintf(sb, 32, L"%04X-%04X", HIWORD(rd->driveSerial), LOWORD(rd->driveSerial));
            serial = sb;
        }
        float rowH = XSF(24);
        s_hits.copySerial = XjsRectF(body.right - XSF(60), cy + XSF(2), body.right - XSF(16), cy + rowH - XSF(2));
        {
            cy = XjsInfoRow(cy, XjsT(L"预览.序列号"), serial, px);
            g_rt->FillRoundedRectangle(XjsRoundedRectF(s_hits.copySerial, XSF(5), XSF(5)),
                g_br[XjsPvHover(4) ? XTH_ROW_HOVER : XTH_PANEL2]);
            std::wstring cp = XjsT(L"通用词.复制");
            /* 水平居中 (文本格式纵对齐恒中, 横对齐是 leading — 量宽后画在中间) */
            float tw = XjsMeasureText(cp.c_str(), g_tfTiny);
            float cx = (s_hits.copySerial.left + s_hits.copySerial.right) / 2;
            g_rt->DrawText(cp.c_str(), (UINT32)cp.length(), g_tfTiny,
                XjsRectF(cx - tw / 2, s_hits.copySerial.top, cx + tw / 2, s_hits.copySerial.bottom),
                g_br[XjsPvHover(4) ? XTH_TEXT : XTH_TEXT_DIM]);
        }
        cy = XjsInfoRow(cy, XjsT(L"预览.可用空间"), Utf8ToUtf16(xjs_util_FormatFileSize(rd->driveFree)), px);
        cy = XjsInfoRow(cy, XjsT(L"预览.盘符"), rd->name + L"\\", px);
        cy += XSF(8);
        /* 查找大目录 / 查找大文件 */
        float bw2 = (pw - XSF(8)) / 2;
        s_hits.bigDirs = XjsRectF(px, cy, px + bw2, cy + XSF(32));
        s_hits.bigFiles = XjsRectF(px + bw2 + XSF(8), cy, px + pw, cy + XSF(32));
        XjsPanelButton(s_hits.bigDirs, XjsT(L"预览.查找大目录"), -1, XjsPvHover(5));
        XjsPanelButton(s_hits.bigFiles, XjsT(L"预览.查找大文件"), -1, XjsPvHover(6));
    } else {
        /* ===== 文件 / 目录 ===== */
        /* 内容位图: 插件交付位图优先 (已在内存); 内置图 = 异步装载缓存 (UI 线程零解码,
           几十 M 的图同步解码曾冻结 UI 数百毫秒, 2026-09-29 用户对比 Everything 实锤) */
        XjsBitmap* contentImg = XjsPreviewPluginBitmap(rd->fileId);
        bool builtin = (contentImg == NULL) && g_previewImgW > 0;
        bool isImage = (contentImg != NULL) || builtin;
        bool isText = XjsPreviewIsText();
        /* 图片可用区 = 头部以下到预留行之间整块 (面板多高图就多大 — 用户口径 "面板这么
           宽高图片还这么小不合理", 2026-09-29); 预留 = 信息五行120 + 图下间距12 (名称/路径
           行已删 — 与头部标题重复, 空间让给内容, 同日用户口径)。下限 120u: 矮窗口不把图挤没 */
        float imgAvailH = xf_max(contentBottom - cy - XSF(24 * 5 + 12), XSF(120));
        if (isImage) {
            /* 图片内容预览 (等比适配可用区, 上限 4 倍; 滚轮/Ctrl+滚轮缩放, 超出面板裁剪)。
               s_imgZoom: 0=适应窗口哨兵, >0=相对原图的绝对比例 (0.05~4)。内置图旋转走
               WIC FlipRotator (异步装载链内, 无损); 装载在途垫显旧缓存, 到货即换。
               插件交付图已在内存直接绘 (无旋转)。
               工具条 (旋转/1:1/适应/比例) 在灯箱 (用户口径 "点击图片放大后"), 不在面板。 */
            float iw, ih;
            bool rotated = builtin && (s_rot % 2) == 1;
            if (builtin) {
                iw = rotated ? (float)g_previewImgH : (float)g_previewImgW;   /* 转后内容宽高 */
                ih = rotated ? (float)g_previewImgW : (float)g_previewImgH;
            } else {
                XjsSizeU sz = contentImg->GetPixelSize();
                iw = (float)sz.width;
                ih = (float)sz.height;
            }
            /* 左右内边距镜像 (各 14u): pw 含到面板体右缘的 0 边距, 直接用会"左有空位右没有";
               下方文本行右缘是 16u, 差 2u 不可感, 图片自身对称优先 */
            float areaW = pw - XSF(14);
            float base = xf_min(areaW / iw, imgAvailH / ih);
            if (base > 4) base = 4;
            s_imgBase = base;   /* 滚轮起步比例 (渲染帧现算) */
            float scale = (s_imgZoom > 0) ? s_imgZoom : base;
            float dw = iw * scale, dh = ih * scale;
            float dx = px + (areaW - dw) / 2;
            XjsBitmap* draw = contentImg;
            if (builtin) {
                int tw2 = ximax(1, (int)(dw + 0.5f)), th2 = ximax(1, (int)(dh + 0.5f));
                bool sameSrc = s_pvScaled && s_pvKeyFile == rd->fileId && s_pvKeyRot == s_rot;
                bool band = sameSrc && tw2 * 20 <= s_pvKeyW * 21 && tw2 * 20 >= s_pvKeyW * 19 &&
                            th2 * 20 <= s_pvKeyH * 21 && th2 * 20 >= s_pvKeyH * 19;
                if (!band && !s_pvPending && !s_pvFailed && tw2 <= 4096 && th2 <= 4096)
                    XjsPreviewImgAsk(0, rd->fileId, tw2, th2, s_rot);
                if (band) {
                    draw = s_pvScaled;   /* 键尺寸命中: 1:1 贴图 (零插值) */
                    dw = (float)s_pvKeyW;
                    dh = (float)s_pvKeyH;
                    dx = floorf(px + (areaW - dw) / 2 + 0.5f);
                } else if (sameSrc) {
                    draw = s_pvScaled;   /* 重采样在途: 旧图垫显 (略软, 到货即换) */
                }
                /* 都没有 = 暗底占位 (装载完成自动浮现) */
            }
            XjsRect dst = XjsRectF(dx, floorf(cy + 0.5f), dx + dw, floorf(cy + 0.5f) + dh);
            s_hits.image = dst;   /* 点击图片 = 打开放大层 (命中表随帧, 命中序在底部按钮之后) */
            g_rt->PushAxisAlignedClip(body, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            g_rt->FillRectangle(dst, XjsTempBrush(XjsCol(0x000000, 0.35f)));
            if (draw) g_rt->DrawBitmap(draw, dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            g_rt->PopAxisAlignedClip();
            cy += xf_max(dh, imgAvailH) + XSF(12);   /* 适应态行贴底稳定; 放大溢出随图推移 (同旧缩放行为) */
        } else if (!isText) {
            XjsBitmap* ic = XjsPreviewIcon(rd->fileId);
            float isz = XSF(64);
            if (ic) {
                g_rt->DrawBitmap(ic, XjsRectF(px, cy, px + isz, cy + isz));
                ic->Release();
            }
            if (s_textPending && XjsIsTextExt(rd->name)) {   /* 文本首块在途: 加载中占位 (异步, 面板不冻结) */
                const wchar_t* lh = XjsT(L"预览.文本加载中");
                g_rt->DrawText(lh, (UINT32)wcslen(lh), g_tfTiny,
                    XjsRectF(px, cy + isz + XSF(8), body.right - XSF(10), cy + isz + XSF(26)), g_br[XTH_TEXT_FAINT]);
            }
            cy += isz + XSF(10);
        }
        /* 名称+路径行已删 (2026-09-29 用户口径: 与头部标题重复, 空间让给内容):
           文本内容直接从头部下方起画, 普通文件信息行上移 */
        if (isText) {
            /* ===== 文本内容预览 (普通滚轮滚动, 底部按钮让位) ===== */
            float lineH = XSF(17);
            /* 底部状态行 (加载中/还有更多) 常驻一条带: 行区让出, 不与末行重叠 (2026-09-29 实锤) */
            float listBottom = (s_textPending || s_textHasMore) ? contentBottom - XSF(18) : contentBottom;
            int visLines = (int)((listBottom - cy) / lineH);
            int maxScroll = (int)s_textLines.size() - ximax(visLines, 1);
            if (maxScroll < 0) maxScroll = 0;
            if (s_textScroll > maxScroll) s_textScroll = maxScroll;
            int first = (int)s_textScroll;
            float y = cy;
            g_rt->PushAxisAlignedClip(XjsRectF(body.left, cy - XSF(2), body.right, listBottom + XSF(4)),
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            for (int i = first; i < (int)s_textLines.size() && y < listBottom; i++, y += lineH) {
                if (!s_textLines[i].empty())
                    XjsDrawEllText(s_textLines[i], XjsRectF(px, y, body.right - XSF(10), y + lineH),
                        g_tfTiny, g_br[XTH_TEXT_DIM]);
            }
            g_rt->PopAxisAlignedClip();
            /* 流式大文件: 快到底 (300 行内) 预取下一块 (pending 闸防重复投); 底部状态行 */
            if (s_textHasMore && !s_textPending && g_previewFileId >= 0 && maxScroll - (int)s_textScroll < 300)
                XjsPreviewTextAsk(g_previewFileId, std::wstring(), 0, false);
            {
                const wchar_t* hint = s_textPending ? XjsT(L"预览.文本加载中")
                                     : s_textHasMore ? XjsT(L"预览.继续滚动") : NULL;
                if (hint)
                    g_rt->DrawText(hint, (UINT32)wcslen(hint), g_tfTiny,
                        XjsRectF(px, listBottom + XSF(2), body.right - XSF(10), listBottom + XSF(18)),
                        g_br[XTH_TEXT_FAINT]);
            }
        } else {
            /* 信息行 */
            if (!rd->isDrive) {
                cy = XjsInfoRow(cy, XjsT(L"列.大小"), Utf8ToUtf16(xjs_util_FormatFileSize(rd->size)), px);
                cy = XjsInfoRow(cy, XjsT(L"列.修改时间"), XjsTimeText(rd->mtime), px);
                wchar_t rb[16];
                _snwprintf(rb, 16, L"%d", rd->rating);
                cy = XjsInfoRow(cy, XjsT(L"列.评分"), rb, px);
                cy = XjsInfoRow(cy, XjsT(L"列.别名"), rd->hasAlias ? rd->alias : L"-", px);
                if (!rd->isDrive) {
                    /* 目录: 子项数量 */
                    int cnt = xjs_db_GetChildrenCount(g_engine, rd->fileId, 0);
                    cy = XjsInfoRow(cy, XjsT(L"预览.子项数量"), XjsNumText(cnt), px);
                }
            }
        }
    }

    /* 底部: 定位文件 / 打开 */
    float fbH = XSF(34);
    float fbY = L.statusbar.top - XSF(44);
    float bw3 = (pw - XSF(8)) / 2;
    s_hits.locate = XjsRectF(px, fbY, px + bw3, fbY + fbH);
    s_hits.open = XjsRectF(px + bw3 + XSF(8), fbY, px + pw, fbY + fbH);
    XjsPanelButton(s_hits.locate, XjsT(L"预览.定位文件"), 0, XjsPvHover(7));
    XjsPanelButton(s_hits.open, XjsT(L"通用词.打开"), 1, XjsPvHover(8));
}

/* ==================== 鼠标 ==================== */

/* 预览面板分隔线命中带 — 用户口径: 命中必须与视觉线条完全一致 (3px), 不做任何
   外扩。按下 (XjsPreviewMouseDown) 与光标 (main.cpp resizerHover) 都走这里,
   两处永不漂移 */
bool XjsPreviewResizerHit(POINT pt) {
    if (!g_previewVisible) return false;
    XjsLayout& L = g_layout;
    return pt.x >= L.preview.left && pt.x <= L.preview.left + XSF(3) &&
           pt.y >= L.preview.top && pt.y <= L.preview.bottom;
}

/* 面板按钮命令编码 (按下待定/松开触发两处同源): 1=关闭 2=宽窄切换 3=锁定 4=复制序列号
   5=大目录 6=大文件 7=定位 8=打开 9=图片放大层 (灯箱工具条命令在 XjsLightboxHitCmd) */
static int s_pvPress = 0;

static bool XjsPreviewHitCmd(POINT pt, int* cmdOut) {
    if (XjsPtIn(s_hits.closeBtn, pt)) *cmdOut = 1;
    else if (XjsPtIn(s_hits.maxBtn, pt)) *cmdOut = 2;
    else if (XjsPtIn(s_hits.lockBtn, pt)) *cmdOut = 3;
    else if (XjsPtIn(s_hits.copySerial, pt)) *cmdOut = 4;
    else if (XjsPtIn(s_hits.bigDirs, pt)) *cmdOut = 5;
    else if (XjsPtIn(s_hits.bigFiles, pt)) *cmdOut = 6;
    else if (XjsPtIn(s_hits.locate, pt)) *cmdOut = 7;
    else if (XjsPtIn(s_hits.open, pt)) *cmdOut = 8;
    else if (XjsPtIn(s_hits.image, pt)) *cmdOut = 9;   /* 最后判: 图片放大超出面板时按钮优先 (视觉层序同) */
    else return false;
    return true;
}

void XjsPreviewHoverUpdate(POINT pt) {
    int cmd = 0;
    bool in = g_previewVisible && !g_previewDrag && !g_plugPanelOn && s_hits.valid &&
              XjsPtIn(g_layout.preview, pt) && XjsPreviewHitCmd(pt, &cmd);
    int hov = in ? cmd : 0;
    if (hov != s_pvHover) {
        s_pvHover = hov;
        XjsSearchWindow::Cur()->Invalidate();
    }
}

void XjsPreviewHoverReset() {
    s_pvHover = 0;   /* 失效由调用方负责 (WM_MOUSELEAVE 分支本就整帧重绘) */
}

bool XjsPreviewMouseDown(POINT pt) {
    if (!g_previewVisible || !s_hits.valid) return false;
    XjsLayout& L = g_layout;
    /* resizer 拖动 */
    if (XjsPreviewResizerHit(pt)) {
        g_previewDrag = true;
        SetCapture(g_hWnd);
        return true;
    }
    if (!XjsPtIn(L.preview, pt)) return false;
    /* 命令按钮: 按下只记待定 (松开触发口径), 松开仍命中同一按钮才执行 */
    int cmd = 0;
    if (XjsPreviewHitCmd(pt, &cmd)) s_pvPress = cmd;
    /* 面板接管: 内容区点击转发插件 (带捕获; 头部 ✕/宽窄/锁 已被上面 cmd 消费) */
    XjsPreviewPanelMouseDown(pt);
    return true;   /* 面板内其余点击不透传给列表 */
}

/* 按钮命令执行 (cmd 编码见 XjsPreviewHitCmd) */
static void XjsPreviewRunCmd(int cmd) {
    if (cmd == 1) {
        if (g_plugPanelOn) {   /* 面板接管中: ✕ 只结束聊天, 按打开前状态恢复预览 (原版口径) */
            XjsPreviewPanelClose(XjsSearchWindow::Cur(), XjsPluginCurWindowToken(), true);
            return;
        }
        g_previewVisible = false;
        s_lightbox = false;
        XjsPreviewImageCacheDropAll(XjsSearchWindow::Cur());
        XjsSaveConfig();
        XjsClampScroll();
    } else if (cmd == 2) {
        g_previewWidth = (g_previewWidth >= 640) ? 400 : 640;
        XjsSaveConfig();
        XjsPreviewPanelSyncSize(true);   /* 面板接管中: 宽窄切换即世代同步 (会话若未开是空操作) */
    } else if (cmd == 3) {
        g_previewLocked = !g_previewLocked;
    } else if (cmd == 4) {
        wchar_t sb[32];
        /* 序列号取卡片正在显示的驱动器 (命中表已按帧清零) */
        XjsRowData* rd = XjsPreviewCardRow();
        if (rd && rd->isDrive) {
            _snwprintf(sb, 32, L"%04X-%04X", HIWORD(rd->driveSerial), LOWORD(rd->driveSerial));
            XjsCopyClipboard(sb);
            XjsToastShow(g_hWnd, XjsT(L"状态栏.已复制"), XTOAST_SUCCESS, XSF(1));   /* 无反馈会被当"点不了" */
        }
    } else if (cmd == 5) {
        XjsPreviewQueryBigDirs();
    } else if (cmd == 6) {
        XjsPreviewQueryBigFiles();
    } else if (cmd == 7 || cmd == 8) {
        XjsRowData* rd = XjsPreviewCardRow();
        if (rd) {
            bool open = (cmd == 8);
            if (rd->isDrive) {
                std::wstring root = rd->name + L"\\";
                if (open) XjsOpenFile(root);
                else ShellExecuteW(NULL, L"explore", root.c_str(), NULL, NULL, SW_SHOWNORMAL);
            } else {
                std::wstring path = Utf8ToUtf16(xjs_db_GetPath(g_engine, rd->fileId));
                if (open) XjsOpenFile(path);
                else XjsOpenFolderAndSelect(path);
            }
        }
    } else if (cmd == 9) {
        /* 图片放大层 (灯箱): 整窗模态放大 + 工具条 (旋转/1:1/适应/滚轮缩放/拖动平移),
           关闭入口见 XjsLightboxMsg (点工具条外任意处/Esc)。每次开层从适应窗口居中起步 */
        s_lightbox = true;
        s_lbZoom = 0;
        s_lbPress = 0;
        s_lbPanX = s_lbPanY = 0;
    }
    XjsSearchWindow::Cur()->Invalidate();
}

bool XjsPreviewMouseMove(POINT pt) {
    (void)pt;
    if (!g_previewDrag) return false;
    float newW = (float)g_layout.w - pt.x - XSF(6);
    g_previewWidth = ximax(280, ximin(800, (int)newW));
    XjsSearchWindow::Cur()->Invalidate();
    return true;
}

bool XjsPreviewMouseUp(POINT pt) {
    if (g_previewDrag) { g_previewDrag = false; XjsSaveConfig(); XjsPreviewPanelSyncSize(true); return true; }
    if (XjsPreviewPanelMouseUp(pt)) return true;   /* 面板捕获中: 转发 LUP (拖离面板也算) */
    int cmd = s_pvPress;
    s_pvPress = 0;
    if (!cmd || !g_previewVisible || !s_hits.valid) return false;
    int again = 0;
    if (XjsPtIn(g_layout.preview, pt) && XjsPreviewHitCmd(pt, &again) && again == cmd)
        XjsPreviewRunCmd(cmd);   /* 松开仍命中同一按钮才执行 (拖离=取消) */
    return true;
}

/* ==================== 图片放大层 (灯箱) ====================
 * 预览面板图片被点击后整窗模态放大: 等比缩放至窗口内居中, 非图片区域盖同款模态钟罩
 * (XjsModeDlgRender 口径 g_skin.bg1×0.6)。模态 = main.cpp WndProc 总闸统一拦截输入。
 * 底部工具条 (用户口径 "点击图片放大后"): 左旋/右旋/1:1/适应 + 原始尺寸·当前比例,
 * 滚轮 = 缩放 (适应态起步, 每格 ×1.25, 5%~400%); 点工具条外任意处/Esc 关闭。
 * 关闭发生在"按下" (LDOWN), 其后同一连击的第二击以 WM_LBUTTONDBLCLK 到达时本层已收 —
 * XjsLightboxJustClosedAt 按系统双击判定 (时限+双击矩形) 吞掉, 否则第二击穿透到底下
 * 列表/预览误开文件。 */
static ULONGLONG s_lbCloseTick = 0;   /* 鼠标点击关闭的时刻/落点 (鼠标交互瞬态, 文件级 static 口径) */
static POINT s_lbClosePt = {};
static int s_lbHover = 0;             /* 灯箱按钮悬停命令号 (0=无; 悬停态随本层收起熄灭) */

bool XjsLightboxActive() {
    return g_previewVisible && s_lightbox;
}

void XjsLightboxClose() {
    s_lightbox = false;
    s_lbPress = 0;
    s_lbHover = 0;
    if (s_lbDrag) {   /* Esc 等路径可在拖动中收层: 捕获一并归还 */
        s_lbDrag = false;
        ReleaseCapture();
    }
    XjsPreviewImageCacheDropAll(XjsSearchWindow::Cur());
    XjsSearchWindow::Cur()->Invalidate();
}

bool XjsLightboxJustClosedAt(POINT pt) {
    if (GetTickCount64() - s_lbCloseTick > (unsigned long long)GetDoubleClickTime()) return false;
    return abs(pt.x - s_lbClosePt.x) <= GetSystemMetrics(SM_CXDOUBLECLK) &&
           abs(pt.y - s_lbClosePt.y) <= GetSystemMetrics(SM_CYDOUBLECLK);
}

/* 灯箱工具条命中 (渲染帧填写的按钮矩形): 10=左旋 11=右旋 12=适应 13=1:1, 0=无 */
static int XjsLightboxHitCmd(POINT pt) {
    if (XjsPtIn(s_lbRotL, pt)) return 10;
    if (XjsPtIn(s_lbRotR, pt)) return 11;
    if (XjsPtIn(s_lbFit, pt)) return 12;
    if (XjsPtIn(s_lbOne, pt)) return 13;
    return 0;
}

static void XjsLightboxRunCmd(int cmd) {
    if (cmd == 10) {
        s_rot = (s_rot + 3) % 4;   /* 左旋 90° (预览面板同步跟随, 缓存按旋转档自动重采样) */
        s_lbPanX = s_lbPanY = 0;   /* 转后尺寸变, 平移回中 */
    } else if (cmd == 11) {
        s_rot = (s_rot + 1) % 4;   /* 右旋 90° */
        s_lbPanX = s_lbPanY = 0;
    } else if (cmd == 12) {
        s_lbZoom = 0;              /* 适应窗口 (哨兵) */
        s_lbPanX = s_lbPanY = 0;
    } else if (cmd == 13) {
        s_lbZoom = 1.0f;           /* 1:1 原始像素 */
        s_lbPanX = s_lbPanY = 0;
    }
    XjsSearchWindow::Cur()->Invalidate();
}

/* 滚轮缩放 (dir=±1): 适应态从本帧适配比例起步, 每格 ×1.25, 范围 5%~400% (同面板口径) */
static void XjsLightboxWheel(int dir) {
    float z = (s_lbZoom > 0) ? s_lbZoom : s_lbBase;
    z = (dir > 0) ? z * 1.25f : z / 1.25f;
    if (z > 4) z = 4;
    if (z < 0.05f) z = 0.05f;
    s_lbZoom = z;
    XjsSearchWindow::Cur()->Invalidate();
}

/* 模态总闸输入消费: LDOWN=按钮待定 / 图面按下拖动平移 / 其余关闭并记连击落点,
   LUP=按钮落地 (松开仍命中才执行) 或 拖动收尾 (未动视作点击关闭), 滚轮=缩放,
   MOVE=拖动跟手/按钮悬停/手型光标, Esc=关闭, 其余一概吞 */
void XjsLightboxMsg(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)hwnd;
    if (!XjsLightboxActive()) return;
    POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };   /* 滚轮的 lParam 是屏幕坐标, 本函数不取用 */
    if (msg == WM_LBUTTONDOWN) {
        int cmd = XjsLightboxHitCmd(pt);
        if (cmd) {
            s_lbPress = cmd;
        } else if (s_lbPannable && XjsPtIn(s_lbImg, pt)) {
            /* 图面按下 = 拖动平移 (溢出才可拖); 捕获保证拖出窗外松开也收到 LUP */
            s_lbDrag = true;
            s_lbDragPt = pt;
            s_lbDragPanX = s_lbPanX;
            s_lbDragPanY = s_lbPanY;
            SetCapture(g_hWnd);
        } else {
            s_lbCloseTick = GetTickCount64();
            s_lbClosePt = pt;
            XjsLightboxClose();
        }
    } else if (msg == WM_LBUTTONUP) {
        if (s_lbPress) {
            if (XjsLightboxHitCmd(pt) == s_lbPress) XjsLightboxRunCmd(s_lbPress);
            s_lbPress = 0;
        } else if (s_lbDrag) {
            s_lbDrag = false;
            ReleaseCapture();
            if (abs(pt.x - s_lbDragPt.x) + abs(pt.y - s_lbDragPt.y) <= 5) {
                /* 原地点击 (未拖动) = 关闭, 同非拖路径; 记落点供连击第二击吞掉 */
                s_lbCloseTick = GetTickCount64();
                s_lbClosePt = pt;
                XjsLightboxClose();
            }
        }
    } else if (msg == WM_MOUSEWHEEL) {
        XjsLightboxWheel(((short)HIWORD(wParam)) > 0 ? 1 : -1);
    } else if (msg == WM_MOUSEMOVE) {
        if (s_lbDrag) {
            s_lbPanX = s_lbDragPanX + (pt.x - s_lbDragPt.x);
            s_lbPanY = s_lbDragPanY + (pt.y - s_lbDragPt.y);
            XjsSearchWindow::Cur()->Invalidate();   /* 越界由渲染帧按溢出量钳制 */
        } else {
            int hov = XjsLightboxHitCmd(pt);
            if (hov != s_lbHover) {
                s_lbHover = hov;
                XjsSearchWindow::Cur()->Invalidate();
            }
        }
        bool onImg = !XjsLightboxHitCmd(pt) && XjsPtIn(s_lbImg, pt);
        SetCursor(LoadCursorW(NULL, (onImg && s_lbPannable) ? IDC_HAND : IDC_ARROW));
    } else if (msg == WM_KEYDOWN && wParam == VK_ESCAPE) {
        XjsLightboxClose();
    }
}

/* 钟罩+大图 (WM_PAINT 状态栏之后调)。
 * 取图口径: 插件交付图 (已在内存) / 内置图 = 异步装载缓存 (UI 线程零解码)。尺寸 =
 * 等比适配窗口 (底部让出工具条带), 上限 4 倍 (同预览滚轮上限)。质量路线: 内置图按
 * 目标尺寸走工作线程 Fant 重采样结果 1:1 贴图 (照片查看器级; DrawBitmap 只有双线性,
 * 直接拉大必糊)。缓存键 = 文件ID+目标尺寸+旋转档 (请求时落键), 目标尺寸漂移 ≤5% 内
 * 复用旧图 (拖窗缩放不逐帧重投, 5% 内差值插值不可感); 在途垫显旧缓存/面板缓存,
 * 都没有画暗底占位 — 装载到货即换。装载失败不再重投 (换文件/旋转复位)。 */
void XjsLightboxRender(XjsRt* rt, float w, float h) {
    if (!rt || !XjsLightboxActive()) return;
    XjsBitmap* plugImg = XjsPreviewPluginBitmap(g_previewFileId);
    bool builtin = (plugImg == NULL) && g_previewImgW > 0;
    if (!plugImg && !builtin) {
        s_lightbox = false;   /* 非图片目标 (选中被程序性换走等): 就地收层防"看不见的模态" */
        return;
    }
    float iw, ih;
    if (plugImg) {
        XjsSizeU sz = plugImg->GetPixelSize();
        iw = (float)sz.width;
        ih = (float)sz.height;
    } else {
        iw = (float)g_previewImgW;   /* 原始尺寸 (选目标时只读头取得) */
        ih = (float)g_previewImgH;
    }
    bool rotated = builtin && (s_rot % 2) == 1;
    float effW = rotated ? ih : iw, effH = rotated ? iw : ih;   /* 转后内容宽高 (预览旋转灯箱跟随) */
    XjsColor mask = g_skin.bg1;
    mask.a *= 0.6f;   /* 同款模态钟罩 */
    rt->FillRectangle(XjsRectF(0, 0, w, h), XjsTempBrush(mask));
    float margin = XSF(24);
    float tbH = XSF(30);   /* 底部工具条带高 */
    float availH = xf_max(h - margin * 2 - tbH, XSF(60));
    float base = xf_min((w - margin * 2) / effW, availH / effH);
    if (base > 4) base = 4;
    s_lbBase = base;   /* 滚轮起步比例 (渲染帧现算) */
    float scale = (s_lbZoom > 0) ? s_lbZoom : base;
    float dw = effW * scale, dh = effH * scale;
    XjsBitmap* draw = plugImg;
    if (builtin) {   /* 内置图: 异步装载缓存 (请求时落键; 在途垫显, 失败不重投) */
        int tw = ximax(1, (int)(dw + 0.5f)), th = ximax(1, (int)(dh + 0.5f));
        bool sameSrc = s_lbBmp && s_lbKeyFile == g_previewFileId && s_lbKeyRot == s_rot;
        bool band = sameSrc && tw * 20 <= s_lbKeyW * 21 && tw * 20 >= s_lbKeyW * 19 &&
                    th * 20 <= s_lbKeyH * 21 && th * 20 >= s_lbKeyH * 19;
        if (!band && !s_lbPending && !s_lbFailed && tw <= 4096 && th <= 4096)
            XjsPreviewImgAsk(1, g_previewFileId, tw, th, s_rot);
        if (band) {
            draw = s_lbBmp;   /* 键尺寸命中: 1:1 贴图 (零插值) */
            dw = (float)s_lbKeyW;
            dh = (float)s_lbKeyH;
        } else if (sameSrc) {
            draw = s_lbBmp;   /* 重采样在途: 旧图垫显 */
        } else if (s_pvScaled && s_pvKeyFile == g_previewFileId && s_pvKeyRot == s_rot) {
            draw = s_pvScaled;   /* 面板缓存先垫 (小图放大, 点开即刻有内容) */
        }
    }
    /* 拖动平移: 溢出才可拖 (钳制使图缘不进视口, 不溢出轴恒居中); 渲染帧自愈式钳制 —
       缩放/旋转/开层后的旧偏移自动归位, 拖动中越界同帧收回 */
    float viewW = w, viewH = h - tbH;
    float maxPanX = xf_max((dw - viewW) / 2, 0), maxPanY = xf_max((dh - viewH) / 2, 0);
    if (s_lbPanX > maxPanX) s_lbPanX = maxPanX;
    if (s_lbPanX < -maxPanX) s_lbPanX = -maxPanX;
    if (s_lbPanY > maxPanY) s_lbPanY = maxPanY;
    if (s_lbPanY < -maxPanY) s_lbPanY = -maxPanY;
    s_lbPannable = maxPanX > 0 || maxPanY > 0;
    float bx = floorf((viewW - dw) / 2 + s_lbPanX + 0.5f), by = floorf((viewH - dh) / 2 + s_lbPanY + 0.5f);
    XjsRect dst = XjsRectF(bx, by, bx + dw, by + dh);   /* 整数对齐: 半像素落点徒增发虚, 拖动也整像素步进 */
    s_lbImg = dst;   /* 拖动/手型光标命中 (XjsLightboxMsg) */
    /* 缩放溢出裁到工具条带上缘: 放大图不盖工具条 (按钮/文字画在其上仍清晰) */
    rt->PushAxisAlignedClip(XjsRectF(0, 0, w, h - tbH), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    rt->FillRectangle(dst, XjsTempBrush(XjsCol(0x000000, 0.35f)));   /* 占位暗底 (装载中即见图框) */
    if (draw) rt->DrawBitmap(draw, dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    rt->DrawRectangle(dst, g_br[XTH_BORDER], 1.0f);   /* 细边: 深色图片在深色钟罩上有轮廓 */
    rt->PopAxisAlignedClip();
    /* ===== 底部工具条: 左旋/右旋 (仅内置图) + 1:1 + 适应 + 原始尺寸·当前比例, 居中一组 ===== */
    float ty2 = h - tbH + XSF(3);
    g_rt->FillRectangle(XjsRectF(0, h - tbH, w, h - tbH + 1), g_br[XTH_BORDER]);
    const wchar_t* fitLabel = XjsT(L"预览.适应");
    float fitW = XjsMeasureText(fitLabel, g_tfTiny) + XSF(16);
    wchar_t infoBuf[48];
    _snwprintf(infoBuf, 48, L"%d×%d · %d%%", (int)iw, (int)ih, (int)(scale * 100.0f + 0.5f));
    float infoW = XjsMeasureText(infoBuf, g_tfTiny);
    float total = (builtin ? XSF(24 + 6 + 24 + 8) : 0) + XSF(34 + 6) + fitW + XSF(16) + infoW;
    if (total > w - XSF(16)) {   /* 窄窗放不下: 去尺寸信息只留按钮与比例 */
        _snwprintf(infoBuf, 48, L"%d%%", (int)(scale * 100.0f + 0.5f));
        infoW = XjsMeasureText(infoBuf, g_tfTiny);
        total = (builtin ? XSF(24 + 6 + 24 + 8) : 0) + XSF(34 + 6) + fitW + XSF(16) + infoW;
    }
    float x0 = floorf((w - total) / 2 + 0.5f);
    if (builtin) {
        s_lbRotL = XjsRectF(x0, ty2, x0 + XSF(24), ty2 + XSF(24));
        XjsPanelHeaderBtn(s_lbRotL, 3, false, s_lbHover == 10);
        x0 += XSF(30);
        s_lbRotR = XjsRectF(x0, ty2, x0 + XSF(24), ty2 + XSF(24));
        XjsPanelHeaderBtn(s_lbRotR, 4, false, s_lbHover == 11);
        x0 += XSF(32);
    } else {
        s_lbRotL = s_lbRotR = {};   /* 插件交付图无文件可重解码, 旋转不可用 */
    }
    s_lbOne = XjsRectF(x0, ty2, x0 + XSF(34), ty2 + XSF(24));
    XjsPvTextBtn(s_lbOne, L"1:1", s_lbHover == 13);
    x0 += XSF(40);
    s_lbFit = XjsRectF(x0, ty2, x0 + fitW, ty2 + XSF(24));
    XjsPvTextBtn(s_lbFit, fitLabel, s_lbHover == 12);
    x0 += fitW + XSF(16);
    g_rt->DrawText(infoBuf, (UINT32)wcslen(infoBuf), g_tfTiny,
        XjsRectF(x0, ty2 + XSF(3), x0 + infoW + XSF(4), ty2 + XSF(21)), g_br[XTH_TEXT_FAINT]);
}

/* ==================== 插件预览交付 (preview 能力, P2) ====================
 * 插件经宿主表 PreviewDeliverBitmap/Text 异步交付; requestId 必须等于当前世代,
 * 过期请求静默丢弃 (同搜索指纹丢过期查询口径), 命中才落地面并只失效预览区 */
/* 交付寻窗: SDK 的 PreviewDeliver 不带窗口令牌, 世代比对只能按"当前窗"字段 — 多窗下
 * 交付晚于窗口切换会失配丢帧 (接管会话还开着, 预览停在旧图)。当前窗不匹配时扫描全部
 * 存活实例找世代匹配的接管会话, 找到 = 切到那扇窗作用域完成落地面 (UI 线程)。 */
static XjsSearchWindow* XjsPreviewPlugTarget(int requestId) {
    XjsSearchWindow* cur = XjsSearchWindow::Cur();
    if (cur && cur->previewPlugReq == requestId && cur->previewPlugFileId >= 0) return cur;
    for (int i = 0; i < XjsSearchWindow::Count(); i++) {
        XjsSearchWindow* w = XjsSearchWindow::At(i);
        if (w && w != cur && w->previewPlugReq == requestId && w->previewPlugFileId >= 0) return w;
    }
    return NULL;
}

bool XjsPreviewPluginDeliverBitmap(int requestId, int w, int h, const void* bgra, int stride) {
    XjsSearchWindow* target = XjsPreviewPlugTarget(requestId);
    if (!target) return false;   /* 过期世代 / 未接管 */
    XjsWindowScope scope(target);   /* s_pvPlug* 宏按 target 解析 */
    if (s_pvPlugFileId < 0) return false;
    if (!bgra || w <= 0 || h <= 0 || w > 32768 || h > 32768 || stride < w * 4) return false;
    /* 上限同加 (同 FnPrevBitmap 口径): 异常交付不得让 assign 抛 bad_alloc 终止进程,
       也不得按虚高 stride 越界读插件来源缓冲 (stride 只验过下限曾是大洞) */
    if (stride > w * 4 + 4096 || (long long)stride * h > (256LL << 20)) return false;
    s_pvPlugBmp.assign((const uint8_t*)bgra, (const uint8_t*)bgra + (size_t)stride * h);
    s_pvPlugW = w; s_pvPlugH = h; s_pvPlugStride = stride;
    if (s_pvPlugCache) { s_pvPlugCache->Release(); s_pvPlugCache = NULL; }   /* 旧缓存作废, 渲染时懒重建 */
    if (target->hWnd) {   /* 只失效预览区 (target 自己的布局, 不读 Cur 的 g_layout) */
        XjsRect b = target->layout.preview;
        RECT r = { (int)b.left, (int)b.top, (int)b.right, (int)b.bottom };
        InvalidateRect(target->hWnd, &r, FALSE);
    }
    return true;
}
bool XjsPreviewPluginDeliverText(int requestId, const char* utf8) {
    XjsSearchWindow* target = XjsPreviewPlugTarget(requestId);
    if (!target || !utf8) return false;   /* 过期世代 / 未接管 */
    XjsWindowScope scope(target);   /* s_pvPlug* 宏按 target 解析 */
    /* 文本走既有文本管线 (s_textLines 渲染/滚动全复用); 文件过大口径同内置 (5MB) */
    std::wstring text = Utf8ToUtf16(utf8);
    if (text.size() > 5u * 1024 * 1024) return false;
    s_textLines = XjsSplitLines(text);
    s_textFileId = s_pvPlugFileId;
    s_textScroll = 0;
    if (target->hWnd) {
        XjsRect b = target->layout.preview;
        RECT r = { (int)b.left, (int)b.top, (int)b.right, (int)b.bottom };
        InvalidateRect(target->hWnd, &r, FALSE);
    }
    return true;
}

/* ==================== 插件面板接管 (preview-panel 能力, P3) ====================
 * 插件整块接管预览面板体 (含原头部带, 宿主头部不画; 关闭 = 插件自绘 ✕ → SDK PanelClose,
 * 结束会话并按打开前状态恢复预览 — 正式版 ai-assistant 口径: 预览没开先展开,
 * 聊天期间选中变化不覆盖聊天)。
 * 交互 = 宿主转发 鼠标/滚轮/键盘/IME (XjsPluginOnPanelEvent), 插件交付整块位图 (世代对齐)。
 * 会话状态住 XjsSearchWindow::plugPanel* (可维护性红线: 禁按 hwnd 平行散表); 世代同步点:
 *   - 本节 SyncSize: PanelOpen / 宽拖松开 / 头部宽窄切换 / WM_PANEL_RESYNC (绘制帧探测到
 *     失配后投递 — 插件回调禁在 WM_PAINT 内, 消息循环里做) — 覆盖 窗口缩放/页面缩放/DPI/
 *     状态栏显隐 等一切几何来源;
 *   - 交付 serial/w/h 与当前世代不符 = 静默丢弃 (同预览接管世代号口径)。
 * 线程: 交付可来自插件工作线程 (流式回复) — 位图暂存经 s_panelCs 保护, 渲染帧在锁内快照;
 * 位图懒转本 RT 域缓存 (跨渲染域铁律, 同 XjsPreviewPluginBitmap 口径)。 */

static struct XjsPanelCs { CRITICAL_SECTION cs; XjsPanelCs() { InitializeCriticalSectionAndSpinCount(&cs, 100); } } s_panelCs;

/* 世代/暂存访问锁 (交付线程 vs 渲染帧) */
struct XjsPanelLock {
    XjsPanelLock() { EnterCriticalSection(&s_panelCs.cs); }
    ~XjsPanelLock() { LeaveCriticalSection(&s_panelCs.cs); }
};

/* 派发小包装: Cur 即会话所属窗 (全部调用点都在该窗 WndProc / XjsWindowScope 内), 令牌现取 */
static void XjsPanelSend(XjsSearchWindow* w, int type, int x, int y, int delta, unsigned flags, unsigned ch) {
    XjsPluginPanelDispatch(XjsPluginCurWindowToken(), type, w->plugPanelSerial,
                           w->plugPanelW, w->plugPanelH, w->plugPanelScale, x, y, delta, flags, ch);
}

static void XjsPanelDropCache(XjsSearchWindow* w) {
    if (w->plugPanelCache) { w->plugPanelCache->Release(); w->plugPanelCache = NULL; }
    w->plugPanelCacheRt = NULL;
    w->plugPanelCacheRev = 0;
}

XjsRect XjsPreviewPanelContentRect() {
    XjsLayout& L = g_layout;
    /* 面板体 (resizer 右缘起) 整块交给插件 — 含原 40px 头部带, 宿主头部不画 (2026-09-23 口径) */
    return XjsRectF(L.preview.left + XSF(3), L.preview.top, L.preview.right, L.preview.bottom);
}

/* XjsPreviewPanelInfo 的定位版 (宿主表 PanelGetRect 落点, xjs_plugin.cpp 转): 面板内容区在
 * 所属窗口客户区内的物理像素矩形 + 所属窗口 HWND — 真子窗口型面板 (WebView2 自带输入体系)
 * 用它定位/缩放自建子窗口。布局现算 (XjsChromeLayout 每帧现算口径, 无缓存可失效)。 */
bool XjsPreviewPanelRectOf(XjsSearchWindow* w, HWND* hwnd, int* x, int* y, int* w2, int* h2) {
    if (!w || !w->plugPanelOn) return false;
    XjsRect r = XjsPreviewPanelContentRect();
    if (hwnd) *hwnd = w->hWnd;
    if (x) *x = (int)(r.left + 0.5f);
    if (y) *y = (int)(r.top + 0.5f);
    if (w2) *w2 = ximax(1, (int)(r.right - r.left + 0.5f));
    if (h2) *h2 = ximax(1, (int)(r.bottom - r.top + 0.5f));
    return true;
}

bool XjsPreviewPanelWantsPt(POINT pt) {
    if (!g_plugPanelOn || !g_previewVisible) return false;
    return XjsPtIn(XjsPreviewPanelContentRect(), pt);
}

void XjsPreviewPanelSyncSize(bool notify) {
    XjsSearchWindow* w = XjsSearchWindow::Cur();
    if (!w || !w->plugPanelOn) return;
    XjsRect r = XjsPreviewPanelContentRect();
    int cx = (int)(r.left + 0.5f);
    int cy = (int)(r.top + 0.5f);
    int cw = ximax(1, (int)(r.right - r.left + 0.5f));
    int chh = ximax(1, (int)(r.bottom - r.top + 0.5f));
    float sc = XSF(1.0f);   /* dpi×页面缩放 (XSF 基准), 插件字号/几何按它缩放 */
    bool changed = false;
    long long serial;
    {
        XjsPanelLock lk;
        /* 原点必须一并比对: 加宽窗口时预览面板宽高都不变、只有左缘随列表平移,
           漏比 x/y 插件就永远收不到 RESIZE, 子窗停在旧位置 (宽度失配之坑) */
        changed = (cx != w->plugPanelX || cy != w->plugPanelY ||
                   cw != w->plugPanelW || chh != w->plugPanelH || sc != w->plugPanelScale);
        if (changed) {
            w->plugPanelX = cx;
            w->plugPanelY = cy;
            w->plugPanelW = cw;
            w->plugPanelH = chh;
            w->plugPanelScale = sc;
            w->plugPanelSerial++;   /* 世代推进: 在途旧交付作废 */
        }
        serial = w->plugPanelSerial;
    }
    if (changed && notify)
        XjsPluginPanelDispatch(XjsPluginCurWindowToken(), XJS_HPANEL_RESIZE, serial, cw, chh, sc, 0, 0, 0, 0, 0);
}

bool XjsPreviewPanelOpen(XjsSearchWindow* w, unsigned long long window, const wchar_t* pluginId) {
    if (!w || !pluginId || !*pluginId) return false;
    bool fresh = !w->plugPanelOn || w->plugPanelPluginId != pluginId;
    if (fresh) {
        if (w->plugPanelOn)   /* 换插件接管: 旧会话先收尾 (不回写预览状态) */
            XjsPreviewPanelClose(w, window, false);
        w->plugPanelWasVisible = w->previewVisible;
        w->plugPanelOn = true;
        w->plugPanelPluginId = pluginId;
        w->plugPanelKey = false;
        w->plugPanelCapture = false;
        w->plugPanelResyncPosted = false;
        s_pvHover = 0;   /* 宿主头部/正文按钮不再画: 悬停态一并熄灭 (关闭恢复时不残留高亮) */
        w->plugPanelCaretX = w->plugPanelCaretY = 0;
        {
            XjsPanelLock lk;
            w->plugPanelBmp.clear();
            w->plugPanelBmpW = w->plugPanelBmpH = w->plugPanelBmpStride = 0;
            w->plugPanelRev++;
        }
        XjsPanelDropCache(w);
        w->previewVisible = true;      /* 原版口径: 预览没开先展开 (关闭按 wasVisible 恢复) */
        w->previewFileId = -1;         /* 内容作废标记: 关闭恢复时强制重载当前选中 */
        XjsChromeLayout();             /* 预览刚展开: g_layout 还是上一帧的, 先对齐再记尺寸 */
        XjsClampScroll();
        XjsPreviewPanelSyncSize(false);
        XjsPanelSend(w, XJS_HPANEL_OPEN, 0, 0, 0, 0, 0);
    } else {
        XjsPreviewPanelSyncSize(false);   /* 幂等: 已是本插件的会话, 只对齐尺寸 */
    }
    w->Invalidate();
    return true;
}

void XjsPreviewPanelClose(XjsSearchWindow* w, unsigned long long window, bool restore) {
    if (!w || !w->plugPanelOn) return;
    XjsPanelSend(w, XJS_HPANEL_CLOSE, 0, 0, 0, 0, 0);   /* 先通知收尾 (派发按仍在的 pluginId 找插件) */
    {
        /* 会话态翻转必须整体在面板锁内: 插件工作线程的 交付/取尺寸 在锁内校验归属,
           锁外翻转 = 校验读到半新半旧 (任意线程 API 与 UI 关闭并发的悬垂解引用防线) */
        XjsPanelLock lk;
        w->plugPanelOn = false;
        w->plugPanelPluginId.clear();
        w->plugPanelKey = false;
        w->plugPanelCapture = false;
        w->plugPanelResyncPosted = false;
        w->plugPanelBmp.clear();
        w->plugPanelBmpW = w->plugPanelBmpH = w->plugPanelBmpStride = 0;
        w->plugPanelRev++;
    }
    XjsPanelDropCache(w);
    if (restore) {
        w->previewVisible = w->plugPanelWasVisible;   /* 原本开着 = 只关聊天; 原本关着 = 连预览一起关 */
        XjsSaveConfig();
        if (w->previewVisible) {
            w->previewFileId = -1;                    /* 会话期选中变化被冻结, 破缓存重载当前选中 */
            XjsPreviewUpdateSelection();
        }
    }
    XjsClampScroll();
    w->Invalidate();
}

/* 设置/预览开关把面板藏起来时先结束会话 (会话状态不跨隐藏; 不回写预览状态) */
void XjsPreviewPanelCloseForToggle() {
    if (g_plugPanelOn)
        XjsPreviewPanelClose(XjsSearchWindow::Cur(), XjsPluginCurWindowToken(), false);
}

/* 插件工作线程交付落点 (xjs_plugin.cpp FnPanelDeliverBitmap 转): 归属+世代校验后暂存,
   命中只失效预览区 (同 XjsPreviewPluginDeliverBitmap 口径)。
   任意线程: 归属校验必须与拷贝同锁 (UI 线程关会话/拆窗的对应段也持锁, 校验通过后
   本调用期间窗对象不可能被拆; 锁外校验 = 读半新半旧/已 delete 的对象) */
bool XjsPreviewPanelDeliver(XjsSearchWindow* w, const wchar_t* pluginId, long long serial, int w2, int h, const void* bgra, int stride) {
    if (!w || !pluginId) return false;
    if (!bgra || w2 <= 0 || h <= 0 || w2 > 16384 || h > 16384 || stride < w2 * 4) return false;
    if (stride > w2 * 4 + 4096 || (long long)stride * h > (256LL << 20)) return false;   /* 上限同预览交付 */
    {
        XjsPanelLock lk;
        if (!w->plugPanelOn || w->plugPanelPluginId != pluginId) return false;   /* 不是本插件的会话 */
        if (serial != w->plugPanelSerial || w2 != w->plugPanelW || h != w->plugPanelH) return false;   /* 过期世代/尺寸 */
        w->plugPanelBmp.assign((const uint8_t*)bgra, (const uint8_t*)bgra + (size_t)stride * h);
        w->plugPanelBmpW = w2;
        w->plugPanelBmpH = h;
        w->plugPanelBmpStride = stride;
        w->plugPanelRev++;
        if (w->hWnd) {   /* 只失效预览区 (插件整块交付含头部带); 失效也在锁内: 出锁后不再碰本对象。
                            用 w 自己的布局 (worker 线程禁读 Cur()/g_layout 宏): 当前窗时二者同值,
                            非当前窗取其上一帧布局 = 同样只失效它自己的面板区 */
            XjsRect b = w->layout.preview;
            if (b.right > b.left) {
                RECT r = { (int)b.left, (int)b.top, (int)b.right, (int)b.bottom };
                InvalidateRect(w->hWnd, &r, FALSE);
            } else {
                w->Invalidate();
            }
        }
    }
    return true;
}

/* 当前世代读取 (xjs_plugin.cpp FnPanelGetInfo 转; 任意线程) — 归属校验同锁 (见 Deliver),
   返回 false = 不是本插件的在会话 (输出已清零) */
bool XjsPreviewPanelInfo(XjsSearchWindow* w, const wchar_t* pluginId, long long* serial, int* w2, int* h, float* scale) {
    if (serial) *serial = 0;
    if (w2) *w2 = 0;
    if (h) *h = 0;
    if (scale) *scale = 1.0f;
    if (!w || !pluginId) return false;
    XjsPanelLock lk;
    if (!w->plugPanelOn || w->plugPanelPluginId != pluginId) return false;
    if (serial) *serial = w->plugPanelSerial;
    if (w2) *w2 = w->plugPanelW;
    if (h) *h = w->plugPanelH;
    if (scale) *scale = w->plugPanelScale;
    return true;
}

/* 窗口拆毁 exclusivity (xjs_app.cpp DestroyAndFree): 持面板锁跨越 delete — 插件工作线程
   可能正在锁内做归属校验/字节拷贝, 锁内看到的必是活对象 (CRITICAL_SECTION 同线程可重入,
   析构链里的 XjsPanelDropCache 不会自锁死) */
void XjsPreviewPanelLockEnter() { EnterCriticalSection(&s_panelCs.cs); }
void XjsPreviewPanelLockLeave() { LeaveCriticalSection(&s_panelCs.cs); }

/* 渲染接管位图 (XjsPreviewRender 会话分支; 绘制帧兼探测尺寸/位置失配 → 投 WM_PANEL_RESYNC,
   下一拍消息循环里做世代同步 — 插件回调禁在 WM_PAINT 内) */
void XjsPreviewPanelRender() {
    XjsSearchWindow* w = XjsSearchWindow::Cur();
    XjsRect r = XjsPreviewPanelContentRect();
    XjsBitmap* bmp = NULL;
    {
        XjsPanelLock lk;
        if (!w->plugPanelBmp.empty() && w->plugPanelBmpW > 0) {
            /* 域/世代校验: 缓存位图绑定建它那一刻的 RT; 新交付必须重转 (同预览插件位图口径) */
            if (!w->plugPanelCache || w->plugPanelCacheRt != g_rt || w->plugPanelCacheRev != w->plugPanelRev) {
                XjsPanelDropCache(w);
                w->plugPanelCache = XjsBitmapFromBgra(w->plugPanelBmp.data(), w->plugPanelBmpW,
                                                      w->plugPanelBmpH, w->plugPanelBmpStride);
                w->plugPanelCacheRt = g_rt;
                w->plugPanelCacheRev = w->plugPanelRev;
            }
            bmp = w->plugPanelCache;
        }
    }
    int ex = (int)(r.left + 0.5f);
    int ey = (int)(r.top + 0.5f);
    int ew = ximax(1, (int)(r.right - r.left + 0.5f));
    int eh = ximax(1, (int)(r.bottom - r.top + 0.5f));
    /* 原点一并探测 (同 SyncSize 口径): 宽度不变位置平移也是失配 */
    if (!w->plugPanelResyncPosted &&
        (ex != w->plugPanelX || ey != w->plugPanelY || ew != w->plugPanelW || eh != w->plugPanelH) && g_hWnd) {
        w->plugPanelResyncPosted = true;
        PostMessageW(g_hWnd, WM_PANEL_RESYNC, 0, 0);
    }
    if (bmp)   /* 尺寸失配窗口期拉伸旧图兜底 (插件按 RESIZE 重交付后恢复 1:1), 不留空白闪帧 */
        g_rt->DrawBitmap(bmp, r, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

/* ==================== 面板接管: 鼠标 / 键盘 / IME 转发 ==================== */

static unsigned XjsPanelModFlags() {
    return (unsigned)((GetKeyState(VK_CONTROL) & 0x8000 ? 1 : 0) |
                      (GetKeyState(VK_SHIFT) & 0x8000 ? 2 : 0) |
                      (GetKeyState(VK_MENU) & 0x8000 ? 4 : 0));
}

bool XjsPreviewPanelMouseDown(POINT pt) {
    if (!XjsPreviewPanelWantsPt(pt)) return false;
    XjsSearchWindow* w = XjsSearchWindow::Cur();
    XjsRect cr = XjsPreviewPanelContentRect();
    g_plugPanelCapture = true;   /* 捕获后拖出面板也持续转发 move/up (滚动条/自绘拖拽用) */
    SetCapture(g_hWnd);
    XjsPanelSend(w, XJS_HPANEL_LDOWN, (int)(pt.x - cr.left), (int)(pt.y - cr.top), 0, XjsPanelModFlags(), 0);
    return true;
}

bool XjsPreviewPanelMouseMove(POINT pt) {
    XjsSearchWindow* w = XjsSearchWindow::Cur();
    if (!w || !w->plugPanelOn) return false;
    if (g_previewDrag) return false;   /* 分隔条拖动中让路 — 拖窄方向指针一进内容区就,
                                          会被此函数拦截短路 XjsPreviewMouseMove = 宽度卡死 (只能拖宽不能拖小) */
    XjsRect cr = XjsPreviewPanelContentRect();
    bool inContent = XjsPtIn(cr, pt);
    if (!g_plugPanelCapture && !inContent) return false;
    int x = inContent ? (int)(pt.x - cr.left) : -1;   /* x=y=-1 = 指针已离开面板 (SDK 约定) */
    int y = inContent ? (int)(pt.y - cr.top) : -1;
    XjsPanelSend(w, XJS_HPANEL_MOUSE_MOVE, x, y, 0, XjsPanelModFlags(), 0);
    return true;
}

bool XjsPreviewPanelMouseUp(POINT pt) {
    XjsSearchWindow* w = XjsSearchWindow::Cur();
    if (!w || !w->plugPanelCapture) return false;
    g_plugPanelCapture = false;
    if (w->plugPanelOn) {
        XjsRect cr = XjsPreviewPanelContentRect();
        bool inContent = XjsPtIn(cr, pt);
        XjsPanelSend(w, XJS_HPANEL_LUP, inContent ? (int)(pt.x - cr.left) : -1,
                     inContent ? (int)(pt.y - cr.top) : -1, 0, XjsPanelModFlags(), 0);
    }
    return true;
}

bool XjsPreviewPanelWheel(POINT pt, int delta, unsigned flags) {
    if (!g_plugPanelOn || !g_previewVisible) return false;
    XjsRect cr = XjsPreviewPanelContentRect();
    if (!XjsPtIn(cr, pt)) return false;
    XjsPanelSend(XjsSearchWindow::Cur(), XJS_HPANEL_WHEEL, (int)(pt.x - cr.left), (int)(pt.y - cr.top),
                 delta, flags, 0);
    return true;
}

/* DBLCLK / RDOWN / RUP 小转发 (主窗分支调; type = XJS_HPANEL_*) */
void XjsPreviewPanelMouse(int type, POINT pt) {
    XjsSearchWindow* w = XjsSearchWindow::Cur();
    if (!w || !w->plugPanelOn) return;
    XjsRect cr = XjsPreviewPanelContentRect();
    bool inContent = XjsPtIn(cr, pt);
    if (!inContent && type != XJS_HPANEL_RUP) return;   /* RUP 收尾也转发 (拖离取消口径) */
    XjsPanelSend(w, type, inContent ? (int)(pt.x - cr.left) : -1,
                 inContent ? (int)(pt.y - cr.top) : -1, 0, XjsPanelModFlags(), 0);
}

void XjsPreviewPanelMouseLeave() {
    XjsSearchWindow* w = XjsSearchWindow::Cur();
    if (!w || !w->plugPanelOn || w->plugPanelCapture) return;
    XjsPanelSend(w, XJS_HPANEL_MOUSE_MOVE, -1, -1, 0, 0, 0);
}

/* 面板外宿主点击 (搜索框/列表/标题栏/预览头/右键): 键盘让渡即时收回并通知插件失焦 —
   否则 plugPanelKey 闸仍开着, 按键继续吞给面板 (表象: 焦点已在搜索框, 打字却进 AI 输入框)。
   面板内容区内的点击不收 (插件自管聚焦); 插件自愿归还仍走 PanelSetFocus(0), 不经此处。
   SetFocus 顶层 = 真实焦点一并收回 (2026-09-24 实锤): WebView2 等真子窗渲染层拿走 Win32
   焦点后, 自绘搜索框没有 HWND 抢不回来 — 只清标志则聊天框光标不灭 (双光标齐亮)、
   按键继续进浏览器 (打字窜道); SetFocus 后浏览器 LostFocus 自然到达, 插件侧光标自灭 */
void XjsPreviewPanelKeyBlur(POINT pt) {
    XjsSearchWindow* w = XjsSearchWindow::Cur();
    if (!w || !w->plugPanelOn || !w->plugPanelKey) return;
    if (XjsPreviewPanelWantsPt(pt)) return;
    w->plugPanelKey = false;
    XjsPanelSend(w, XJS_HPANEL_KEY_BLUR, 0, 0, 0, 0, 0);
    if (w->hWnd && GetFocus() && IsChild(w->hWnd, GetFocus())) SetFocus(w->hWnd);
}

void XjsPreviewPanelKey(unsigned vk) {
    XjsSearchWindow* w = XjsSearchWindow::Cur();
    if (!w || !w->plugPanelOn) return;
    XjsPanelSend(w, XJS_HPANEL_KEY_DOWN, 0, 0, (int)vk, XjsPanelModFlags(), 0);
}

void XjsPreviewPanelChar(unsigned int ch) {
    XjsSearchWindow* w = XjsSearchWindow::Cur();
    if (!w || !w->plugPanelOn) return;
    XjsPanelSend(w, XJS_HPANEL_KEY_CHAR, 0, 0, 0, 0, ch);
}

/* IME 上屏: GCS_RESULTSTR 整串取回 → 逐 UTF-16 单元转发 (代理对拆两发, 插件侧拼回码点) */
bool XjsPreviewPanelImeResult(HWND hwnd, LPARAM lParam) {
    XjsSearchWindow* w = XjsSearchWindow::Cur();
    if (!w || !w->plugPanelOn || !(lParam & GCS_RESULTSTR)) return false;
    HIMC himc = ImmGetContext(hwnd);
    if (!himc) return false;
    bool consumed = false;
    LONG bytes = ImmGetCompositionStringW(himc, GCS_RESULTSTR, NULL, 0);
    if (bytes > 0) {
        std::wstring res((size_t)bytes / sizeof(wchar_t), L'\0');
        if (ImmGetCompositionStringW(himc, GCS_RESULTSTR, &res[0], bytes) >= 0) {
            for (wchar_t c : res) XjsPreviewPanelChar((unsigned int)c);
            consumed = true;
        }
    }
    ImmReleaseContext(hwnd, himc);
    return consumed;
}

/* 组字/候选窗锚定: PanelSetCaret 记的面板内点位 → 窗口客户区坐标 (同 XjsLineEdit::UpdateImeAnchor 三路) */
void XjsPreviewPanelUpdateIme(HWND hwnd) {
    XjsSearchWindow* w = XjsSearchWindow::Cur();
    if (!w || !w->plugPanelOn || !hwnd || !IsWindowVisible(hwnd)) return;
    /* 重入守卫 (同搜索框 s_imeUpdBusy 口径, 违者必炸): IMM/TSF 调用会同步 SendMessage 重入
       窗口过程 — SetCaretPos/ImmSet* → TSF 回发 WM_IME_NOTIFY/SETCONTEXT → main 分支再次进入
       本函数 → 无限递归, 实测 MSCTF/msvcrt 栈溢出 0xC00000FD 崩溃 (2026-09-22 实锤) */
    static bool s_busy = false;
    if (s_busy) return;
    s_busy = true;
    XjsRect cr = XjsPreviewPanelContentRect();
    POINT p = { (LONG)(cr.left + w->plugPanelCaretX), (LONG)(cr.top + w->plugPanelCaretY) };
    if (XjsSysCaretEnsure(hwnd)) SetCaretPos(p.x, p.y);   /* 隐藏系统光标作位置源 (归属按 hwnd 记账) */
    HIMC himc = ImmGetContext(hwnd);
    if (himc) {
        COMPOSITIONFORM cf = {};
        cf.dwStyle = CFS_POINT;
        cf.ptCurrentPos = p;
        ImmSetCompositionWindow(himc, &cf);
        CANDIDATEFORM cdf = {};
        cdf.dwIndex = 0;
        cdf.dwStyle = CFS_CANDIDATEPOS;
        cdf.ptCurrentPos = p;
        ImmSetCandidateWindow(himc, &cdf);
        if (w->hFontEdit) {
            LOGFONTW lf = {};
            if (GetObjectW(w->hFontEdit, sizeof(lf), &lf)) ImmSetCompositionFontW(himc, &lf);
        }
        ImmReleaseContext(hwnd, himc);
    }
    s_busy = false;
}

void XjsPreviewPanelFocus(HWND hwnd, bool active) {
    XjsSearchWindow* w = XjsSearchWindow::Cur();
    if (!w || !w->plugPanelOn || w->hWnd != hwnd) return;
    XjsPanelSend(w, XJS_HPANEL_FOCUS, 0, 0, active ? 1 : 0, 0, 0);
}

/* WM_DESTROY: 令牌代递增前派发 CLOSE (此刻窗口令牌仍有效, 插件可安全收尾) */
void XjsPreviewPanelOnWindowClosing(HWND hwnd) {
    XjsSearchWindow* w = XjsSearchWindow::OfHwnd(hwnd);
    if (!w || !w->plugPanelOn) return;
    XjsPreviewPanelClose(w, XjsPluginCurWindowToken(), false);
}
