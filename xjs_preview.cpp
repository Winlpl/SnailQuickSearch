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

/* 字节 → 宽文本: BOM(UTF-16/UTF-8) → 严格 UTF-8 → GBK 兜底 (源样式同识别顺序) */
static std::wstring XjsDecodeTextBytes(const std::string& raw) {
    auto utf16le = [&](const char* p, size_t bytes) {
        std::wstring w(bytes / 2, L'\0');
        memcpy(&w[0], p, bytes / 2 * sizeof(wchar_t));
        return w;
    };
    if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE)
        return utf16le(raw.data() + 2, raw.size() - 2);
    if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFE && (unsigned char)raw[1] == 0xFF) {
        std::string be(raw.data() + 2, raw.size() - 2);
        for (size_t i = 0; i + 1 < be.size(); i += 2) std::swap(be[i], be[i + 1]);
        return utf16le(be.data(), be.size());
    }
    const char* p = raw.data();
    size_t n = raw.size();
    if (n >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
        p += 3;
        n -= 3;
    }
    int wl = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, (int)n, NULL, 0);
    if (wl > 0) {
        std::wstring w(wl, L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, (int)n, &w[0], wl);
        return w;
    }
    int gl = MultiByteToWideChar(936, 0, p, (int)n, NULL, 0);
    if (gl > 0) {
        std::wstring w(gl, L'\0');
        MultiByteToWideChar(936, 0, p, (int)n, &w[0], gl);
        return w;
    }
    return L"";
}

static void XjsLoadPreviewText(int fileId, const std::wstring& path, long long size) {
    s_textLines.clear();
    s_textFileId = -1;
    s_textScroll = 0;
    if (size > 5LL * 1024 * 1024) {
        s_textLines.push_back(XjsT(L"预览.文件过大"));
        s_textFileId = fileId;
        return;
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string raw;
    char buf[65536];
    DWORD rd = 0;
    while (raw.size() < 5u * 1024 * 1024 && ReadFile(h, buf, sizeof(buf), &rd, NULL) && rd)
        raw.append(buf, rd);
    CloseHandle(h);
    bool binary = false;
    for (size_t i = 0; i < raw.size() && i < 4096; i++)
        if (raw[i] == '\0') { binary = true; break; }
    if (binary) {   /* 二进制文件不预览 (保留信息卡) */
        return;
    }
    std::wstring text = XjsDecodeTextBytes(raw);
    if (text.empty()) return;
    s_textLines = XjsSplitLines(text);
    s_textFileId = fileId;
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
static int s_pvPlugReq = 0;             /* 世代号 (每次预览目标刷新递增) */
static int s_pvPlugFileId = -1;         /* 接管中的目标文件 (无 = 未接管) */
static std::vector<uint8_t> s_pvPlugBmp;
static int s_pvPlugW = 0, s_pvPlugH = 0, s_pvPlugStride = 0;
static XjsBitmap* s_pvPlugCache = NULL; /* 交付字节 → 本域位图缓存 (懒建) */
static XjsRt* s_pvPlugCacheRt = NULL;   /* 缓存位图所属 RT 包装 (换窗/设备重建都换域, 位图必须随域重建) */

void XjsPreviewPlugCacheInvalidate() {
    if (s_pvPlugCache) { s_pvPlugCache->Release(); s_pvPlugCache = NULL; }
    s_pvPlugCacheRt = NULL;
}

static void XjsPreviewPluginDrop() {
    s_pvPlugFileId = -1;
    s_pvPlugBmp.clear();
    s_pvPlugW = s_pvPlugH = s_pvPlugStride = 0;
    XjsPreviewPlugCacheInvalidate();
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
        XjsPreviewPlugCacheInvalidate();
        s_pvPlugCache = XjsBitmapFromBgra(s_pvPlugBmp.data(), s_pvPlugW, s_pvPlugH, s_pvPlugStride);
        s_pvPlugCacheRt = g_rt;
    }
    return s_pvPlugCache;
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
    s_imgZoom = 1.0f;
    if (g_previewImage) { g_previewImage->Release(); g_previewImage = NULL; }
    g_previewImageFileId = -1;
    XjsPreviewPluginDrop();
    s_pvPlugReq++;   /* 世代推进: 在途的旧交付作废 */
    s_textLines.clear();
    s_textFileId = -1;
    s_textScroll = 0;
    if (fileId >= 0 && g_engine && g_previewVisible) {   /* 面板未开启只记 ID 不读文件 (隐藏期读图/读文本纯属浪费) */
        std::wstring name = Utf8ToUtf16(xjs_db_GetName(g_engine, fileId));
        std::wstring path = Utf8ToUtf16(xjs_db_GetPath(g_engine, fileId));
        if (XjsPreviewPluginTryTake(fileId, path, name)) {   /* 插件接管: 异步交付, 不走内置分类 */
            XjsSearchWindow::Cur()->Invalidate();
            return;
        }
        if (XjsIsImageExt(name)) {
            /* 图片文件: 预载内容位图 */
            g_previewImage = XjsDecodeFileImage(path);
            if (g_previewImage) g_previewImageFileId = fileId;
        } else if (XjsIsTextExt(name)) {
            /* 文本文件: 预载行集 (≤5MB) */
            XjsLoadPreviewText(fileId, path, xjs_db_GetFileSize(g_engine, fileId));
        }
    }
    XjsSearchWindow::Cur()->Invalidate();
}

bool XjsPreviewIsText() {
    return g_previewFileId >= 0 && s_textFileId == g_previewFileId && !s_textLines.empty();
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

/* 查找大目录: 按父路径聚合子项数 (SQL GROUP BY; 空间地图插件另提供矩形树图视图) */
void XjsPreviewQueryBigDirs() {
    int idx = XjsSelPrimaryIdx();
    XjsRowData* rd = idx >= 0 ? XjsEnsureRowData(idx) : NULL;
    if (!rd || !rd->isDrive) return;
    std::wstring sql = L"SELECT ParentPath, COUNT(*) AS cnt FROM alltable WHERE ParentPath LIKE '" +
        rd->name + L"\\%' GROUP BY ParentPath ORDER BY cnt DESC;";
    g_mode = XMODE_SQL;
    XjsSearchSetText(sql);   /* 自绘搜索框: 置入即触发搜索 */
}

void XjsPreviewQueryBigFiles() {
    int idx = XjsSelPrimaryIdx();
    XjsRowData* rd = idx >= 0 ? XjsEnsureRowData(idx) : NULL;
    if (!rd || !rd->isDrive) return;
    std::wstring sql = L"SELECT * FROM alltable WHERE IsDir=0 AND ParentPath LIKE '" +
        rd->name + L"\\%' AND Size > '100M' ORDER BY Size DESC;";
    g_mode = XMODE_SQL;
    XjsSearchSetText(sql);   /* 自绘搜索框: 置入即触发搜索 */
}

/* ==================== 渲染 ==================== */

/* 信息行: label 左 / value 右; 返回下一行 y */
static float XjsInfoRow(float yTop, const wchar_t* label, const std::wstring& value, float px) {
    float rowH = XSF(24);
    float right = g_layout.preview.right - XSF(16);
    XjsDrawEllText(label, XjsRectF(px, yTop, px + XSF(70), yTop + rowH), g_tfTiny, g_br[XTH_TEXT_FAINT]);
    XjsDrawEllText(value, XjsRectF(px + XSF(76), yTop, right, yTop + rowH), g_tfDim, g_br[XTH_TEXT_DIM]);
    return yTop + rowH;
}

/* 描边按钮 */
static void XjsPanelButton(const XjsRect& r, const wchar_t* text, int iconKind) {
    g_rt->FillRoundedRectangle(XjsRoundedRectF(r, XSF(8), XSF(8)), g_br[XTH_PANEL2]);
    g_rt->DrawRoundedRectangle(XjsRoundedRectF(r, XSF(8), XSF(8)), g_br[XTH_BORDER], 1.0f);
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

/* 圆形小按钮 (面板头: 锁/最大/关闭) */
static void XjsPanelHeaderBtn(const XjsRect& r, int kind, bool active) {
    if (active) g_rt->FillRoundedRectangle(XjsRoundedRectF(r, XSF(6), XSF(6)), g_br[XTH_ROW_HOVER]);
    float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    XjsBrush* bc = active ? (XjsBrush*)g_br[XTH_ACCENT] : (XjsBrush*)g_br[XTH_TEXT_DIM];
    if (kind == 0) {
        /* 锁 */
        g_rt->DrawRectangle(XjsRectF(cx - XSF(4), cy - XSF(1), cx + XSF(4), cy + XSF(5)), bc, 1.2f);
        g_rt->DrawEllipse(XjsEllipseF(XjsPoint2F(cx, cy - XSF(2.5f)), XSF(2.8f), XSF(2.8f)), bc, 1.2f);
    } else if (kind == 1) {
        g_rt->DrawRectangle(XjsRectF(cx - XSF(4.5f), cy - XSF(4.5f), cx + XSF(4.5f), cy + XSF(4.5f)), bc, 1.2f);
    } else {
        g_rt->DrawLine(XjsPoint2F(cx - XSF(4), cy - XSF(4)), XjsPoint2F(cx + XSF(4), cy + XSF(4)), bc, 1.3f);
        g_rt->DrawLine(XjsPoint2F(cx + XSF(4), cy - XSF(4)), XjsPoint2F(cx - XSF(4), cy + XSF(4)), bc, 1.3f);
    }
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
    XjsPanelHeaderBtn(s_hits.lockBtn, 0, g_previewLocked);
    XjsPanelHeaderBtn(s_hits.maxBtn, 1, false);
    XjsPanelHeaderBtn(s_hits.closeBtn, 2, false);
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
            g_rt->FillRoundedRectangle(XjsRoundedRectF(s_hits.copySerial, XSF(5), XSF(5)), g_br[XTH_PANEL2]);
            std::wstring cp = XjsT(L"通用词.复制");
            g_rt->DrawText(cp.c_str(), (UINT32)cp.length(), g_tfTiny, s_hits.copySerial, g_br[XTH_TEXT_DIM]);
        }
        cy = XjsInfoRow(cy, XjsT(L"预览.可用空间"), Utf8ToUtf16(xjs_util_FormatFileSize(rd->driveFree)), px);
        cy = XjsInfoRow(cy, XjsT(L"预览.盘符"), rd->name + L"\\", px);
        cy += XSF(8);
        /* 查找大目录 / 查找大文件 */
        float bw2 = (pw - XSF(8)) / 2;
        s_hits.bigDirs = XjsRectF(px, cy, px + bw2, cy + XSF(32));
        s_hits.bigFiles = XjsRectF(px + bw2 + XSF(8), cy, px + pw, cy + XSF(32));
        XjsPanelButton(s_hits.bigDirs, XjsT(L"预览.查找大目录"), -1);
        XjsPanelButton(s_hits.bigFiles, XjsT(L"预览.查找大文件"), -1);
    } else {
        /* ===== 文件 / 目录 ===== */
        /* 内容位图: 内置解码图优先, 插件交付位图兜底 (世代/目标同源才取) */
        XjsBitmap* contentImg = (g_previewImage && g_previewImageFileId == rd->fileId)
                                    ? g_previewImage : XjsPreviewPluginBitmap(rd->fileId);
        bool isImage = (contentImg != NULL);
        bool isText = XjsPreviewIsText();
        float drawH = XSF(160);
        if (isImage) {
            /* 图片内容预览 (等比缩放; Ctrl+滚轮可再缩放, 超出面板部分裁剪) */
            XjsSizeU sz = contentImg->GetPixelSize();
            float iw = (float)sz.width, ih = (float)sz.height;
            float maxW = pw * s_imgZoom, maxH = drawH * s_imgZoom;
            float scale = xf_min(maxW / iw, maxH / ih);
            if (scale > 1) scale = 1;
            float dw = iw * scale, dh = ih * scale;
            XjsRect dst = XjsRectF(px + (maxW - dw) / 2, cy, px + (maxW - dw) / 2 + dw, cy + dh);
            g_rt->PushAxisAlignedClip(body, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            g_rt->FillRectangle(dst, XjsTempBrush(XjsCol(0x000000, 0.35f)));
            g_rt->DrawBitmap(contentImg, dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            g_rt->PopAxisAlignedClip();
            cy += maxH + XSF(12);
        } else if (!isText) {
            XjsBitmap* ic = XjsPreviewIcon(rd->fileId);
            float isz = XSF(64);
            if (ic) {
                g_rt->DrawBitmap(ic, XjsRectF(px, cy, px + isz, cy + isz));
                ic->Release();
            }
            cy += isz + XSF(10);
        }
        /* 名称 + 路径 */
        {
            std::wstring path = Utf8ToUtf16(xjs_db_GetPath(g_engine, rd->fileId));
            XjsDrawEllText(rd->name, XjsRectF(px, cy, body.right - XSF(16), cy + XSF(22)), g_tfCardVal, g_br[XTH_TEXT]);
            cy += XSF(24);
            XjsDrawEllText(path, XjsRectF(px, cy, body.right - XSF(16), cy + XSF(18)), g_tfTiny, g_br[XTH_TEXT_FAINT]);
            cy += XSF(24);
        }
        if (isText) {
            /* ===== 文本内容预览 (普通滚轮滚动, 底部按钮让位) ===== */
            float lineH = XSF(17);
            int visLines = (int)((contentBottom - cy) / lineH);
            int maxScroll = (int)s_textLines.size() - ximax(visLines, 1);
            if (maxScroll < 0) maxScroll = 0;
            if (s_textScroll > maxScroll) s_textScroll = maxScroll;
            int first = (int)s_textScroll;
            float y = cy;
            g_rt->PushAxisAlignedClip(XjsRectF(body.left, cy - XSF(2), body.right, contentBottom + XSF(4)),
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            for (int i = first; i < (int)s_textLines.size() && y < contentBottom; i++, y += lineH) {
                if (!s_textLines[i].empty())
                    XjsDrawEllText(s_textLines[i], XjsRectF(px, y, body.right - XSF(10), y + lineH),
                        g_tfTiny, g_br[XTH_TEXT_DIM]);
            }
            g_rt->PopAxisAlignedClip();
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
    XjsPanelButton(s_hits.locate, XjsT(L"预览.定位文件"), 0);
    XjsPanelButton(s_hits.open, XjsT(L"通用词.打开"), 1);
}

/* ==================== 鼠标 ==================== */

/* Ctrl+滚轮缩放预览图片 (同正式版: 预览面板上滚轮缩放预览内容) */
void XjsPreviewWheel(int dir) {
    s_imgZoom += dir * 0.2f;
    if (s_imgZoom < 0.5f) s_imgZoom = 0.5f;
    if (s_imgZoom > 4.0f) s_imgZoom = 4.0f;
    XjsSearchWindow::Cur()->Invalidate();
}

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
   5=大目录 6=大文件 7=定位 8=打开 */
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
    else return false;
    return true;
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
        /* 序列号从行数据再取一次 (仅驱动器行有意义; 双保险 — 命中表已按帧清零) */
        int idx = XjsSelPrimaryIdx();
        XjsRowData* rd = (idx >= 0 && idx < g_resultCount) ? XjsEnsureRowData(idx) : NULL;
        if (rd && rd->isDrive) {
            _snwprintf(sb, 32, L"%04X-%04X", HIWORD(rd->driveSerial), LOWORD(rd->driveSerial));
            XjsCopyClipboard(sb);
        }
    } else if (cmd == 5) {
        XjsPreviewQueryBigDirs();
    } else if (cmd == 6) {
        XjsPreviewQueryBigFiles();
    } else if (cmd == 7 || cmd == 8) {
        int idx = XjsSelPrimaryIdx();
        XjsRowData* rd = idx >= 0 ? XjsEnsureRowData(idx) : NULL;
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

/* ==================== 插件预览交付 (preview 能力, P2) ====================
 * 插件经宿主表 PreviewDeliverBitmap/Text 异步交付; requestId 必须等于当前世代,
 * 过期请求静默丢弃 (同搜索指纹丢过期查询口径), 命中才落地面并只失效预览区 */
bool XjsPreviewPluginDeliverBitmap(int requestId, int w, int h, const void* bgra, int stride) {
    if (requestId != s_pvPlugReq || s_pvPlugFileId < 0) return false;   /* 过期世代 / 未接管 */
    if (!bgra || w <= 0 || h <= 0 || w > 32768 || h > 32768 || stride < w * 4) return false;
    /* 上限同加 (同 FnPrevBitmap 口径): 异常交付不得让 assign 抛 bad_alloc 终止进程,
       也不得按虚高 stride 越界读插件来源缓冲 (stride 只验过下限曾是大洞) */
    if (stride > w * 4 + 4096 || (long long)stride * h > (256LL << 20)) return false;
    s_pvPlugBmp.assign((const uint8_t*)bgra, (const uint8_t*)bgra + (size_t)stride * h);
    s_pvPlugW = w; s_pvPlugH = h; s_pvPlugStride = stride;
    if (s_pvPlugCache) { s_pvPlugCache->Release(); s_pvPlugCache = NULL; }   /* 旧缓存作废, 渲染时懒重建 */
    if (g_hWnd) {   /* 只失效预览区 */
        XjsRect b = g_layout.preview;
        RECT r = { (int)b.left, (int)b.top, (int)b.right, (int)b.bottom };
        InvalidateRect(g_hWnd, &r, FALSE);
    }
    return true;
}
bool XjsPreviewPluginDeliverText(int requestId, const char* utf8) {
    if (requestId != s_pvPlugReq || s_pvPlugFileId < 0 || !utf8) return false;
    /* 文本走既有文本管线 (s_textLines 渲染/滚动全复用); 文件过大口径同内置 (5MB) */
    std::wstring text = Utf8ToUtf16(utf8);
    if (text.size() > 5u * 1024 * 1024) return false;
    s_textLines = XjsSplitLines(text);
    s_textFileId = s_pvPlugFileId;
    s_textScroll = 0;
    if (g_hWnd) {
        XjsRect b = g_layout.preview;
        RECT r = { (int)b.left, (int)b.top, (int)b.right, (int)b.bottom };
        InvalidateRect(g_hWnd, &r, FALSE);
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
    w->plugPanelOn = false;
    w->plugPanelPluginId.clear();
    w->plugPanelKey = false;
    w->plugPanelCapture = false;
    w->plugPanelResyncPosted = false;
    {
        XjsPanelLock lk;
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

/* 插件工作线程交付落点 (xjs_plugin.cpp FnPanelDeliverBitmap 转): 校验世代后暂存,
   命中只失效预览区 (同 XjsPreviewPluginDeliverBitmap 口径) */
bool XjsPreviewPanelDeliver(XjsSearchWindow* w, long long serial, int w2, int h, const void* bgra, int stride) {
    if (!w || !w->plugPanelOn) return false;
    if (!bgra || w2 <= 0 || h <= 0 || w2 > 16384 || h > 16384 || stride < w2 * 4) return false;
    if (stride > w2 * 4 + 4096 || (long long)stride * h > (256LL << 20)) return false;   /* 上限同预览交付 */
    {
        XjsPanelLock lk;
        if (serial != w->plugPanelSerial || w2 != w->plugPanelW || h != w->plugPanelH) return false;   /* 过期世代/尺寸 */
        w->plugPanelBmp.assign((const uint8_t*)bgra, (const uint8_t*)bgra + (size_t)stride * h);
        w->plugPanelBmpW = w2;
        w->plugPanelBmpH = h;
        w->plugPanelBmpStride = stride;
        w->plugPanelRev++;
    }
    if (w->hWnd) {   /* 只失效预览区 (插件整块交付含头部带) */
        XjsRect b = w == XjsSearchWindow::Cur() ? g_layout.preview : XjsRectF(0, 0, 0, 0);
        if (b.right > b.left) {
            RECT r = { (int)b.left, (int)b.top, (int)b.right, (int)b.bottom };
            InvalidateRect(w->hWnd, &r, FALSE);
        } else {
            w->Invalidate();
        }
    }
    return true;
}

/* 当前世代读取 (xjs_plugin.cpp FnPanelGetInfo 转; 任意线程) */
void XjsPreviewPanelInfo(XjsSearchWindow* w, long long* serial, int* w2, int* h, float* scale) {
    if (!w) { if (serial) *serial = 0; if (w2) *w2 = 0; if (h) *h = 0; if (scale) *scale = 1.0f; return; }
    XjsPanelLock lk;
    if (serial) *serial = w->plugPanelSerial;
    if (w2) *w2 = w->plugPanelW;
    if (h) *h = w->plugPanelH;
    if (scale) *scale = w->plugPanelScale;
}

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
    if (!w->sysCaretMade && CreateCaret(hwnd, (HBITMAP)NULL, 1, 1)) w->sysCaretMade = true;
    if (w->sysCaretMade) SetCaretPos(p.x, p.y);
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
