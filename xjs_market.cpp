/*
 * xjs_market.cpp — 插件商城窗口: 独立顶层窗口 (单例, 构型同 xjs_settings:
 * owner 绑定 + 文件级状态 + 自绘 D2D + 每显示器 DPI 尺度 + 换肤 g_skinEpoch 重建画刷)。
 * 布局 = 头部 (标题/副标题/搜索框) + 左侧分类栏 (全部/已安装/可安装) + 右侧插件卡片栅格。
 * 数据 = 本机插件注册表 (本机唯一管理入口: 原设置-插件页已撤销并入, 2026-10-06 —
 * 启停开关在卡片右上, 侧栏底部 重扫描/打开插件目录, 卡片右键打开该插件目录) + 样式演示
 * 占位卡 (市场下载源尚未接入, 示例卡按钮点击仅提示; 接入真实市场源后删 MK_DEMO 并在此接拉取管线)。
 * 卡片图标 = 清单 "图标" 声明的插件目录图片文件 (装载一次缓存绑本窗 RT); 演示卡与
 * 未声明/装载失败的回退 = 矢量线性字形 (画法同菜单/设置分类图标)。
 * 搜索框走共享输入字段组件 XjsEditField (路由/IME/闪烁全在组件, 宿主只接线)。
 */
#include "xjs_app.h"
#include <cwctype>
#include <algorithm>

/* ==================== 状态 (单例; 窗口级状态住结构体, 不落游离全局) ==================== */

/* 矢量回退字形 (XjsMkGlyphDraw 的 switch 分支) */
enum { MKG_BOX = 0, MKG_MD, MKG_FOLDCOLOR, MKG_DUP };

struct XjsMkEntry {
    std::wstring id, name, version, author, desc;
    std::wstring iconPath;    /* 清单 "图标" → 插件目录内文件绝对路径 (空 = 矢量字形回退) */
    int glyph = MKG_BOX;      /* 无位图可画时的矢量字形 (演示卡专属 / 已安装回退 = 包装盒) */
    std::wstring statusErr;   /* 异常状态 (清单错误/加载失败/需重启生效): 非空时警色替代简介显示 */
    bool installed = false;   /* 本机注册表条目 (已安装) */
    bool enabled = false;     /* 用户启用开关 (卡片右上开关可点, 即时落盘) */
    bool demo = false;        /* 样式演示占位卡 (市场源未接入) */
};

enum { MK_CAT_ALL = 0, MK_CAT_INSTALLED, MK_CAT_AVAILABLE, MK_CAT_N };

/* 卡片右键菜单项 id (商城窗自有段: 与字段编辑菜单 IDM_FCTX_BASE 9000 段错开) */
enum { IDM_MKCTX_OPENDIR = 4600, IDM_MKCTX_UNINSTALL = 4601 };

/* 样式演示占位卡 (文案存点分 i18n 主键; 文件级 static 表初始化器禁调 XjsT) */
static const struct { const wchar_t* nameKey; const wchar_t* descKey; int glyph; } MK_DEMO[3] = {
    { L"商城.示例.Markdown阅读器",  L"商城.示例.Markdown阅读器.描述",  MKG_MD        },
    { L"商城.示例.彩色文件夹图标",  L"商城.示例.彩色文件夹图标.描述",  MKG_FOLDCOLOR },
    { L"商城.示例.重复文件查找",    L"商城.示例.重复文件查找.描述",    MKG_DUP       },
};

/* 卡片图标缓存条目: 位图绑本窗 RT (随 XjsMkFreeResources 一并释放), 键 = 路径+修改时间 */
struct XjsMkIcon { std::wstring path; unsigned long long ft = 0; XjsBitmap* bmp = NULL; };

struct XjsMarketState {
    HWND hwnd = NULL;
    XjsHwndRt* rt = NULL;
    XjsSolidBrush *brBg = NULL, *brBorder = NULL, *brBorderStrong = NULL, *brText = NULL,
        *brDim = NULL, *brFaint = NULL, *brAccent = NULL, *brAccent2 = NULL, *brAccentSoft = NULL,
        *brPanel = NULL, *brPanel2 = NULL, *brHover = NULL, *brOk = NULL, *brWhite = NULL;
    XjsGradBrush* brBgGrad = NULL;
    XjsColor bg2;                /* 背景纵渐变末端色 (bg1 在 brBg 里), 轴重建时取用 */
    float bgGradH = -1;          /* 背景纵渐变轴的窗口高 (轴必须随窗口高重建, 钉死 = 最大化下半截钳到末端色) */
    XjsFormat *tfTitle = NULL, *tfSub = NULL, *tfName = NULL, *tfMeta = NULL, *tfDesc = NULL,
        *tfBtn = NULL, *tfCat = NULL, *tfCount = NULL, *tfEmpty = NULL, *tfSearch = NULL;
    int brushEpoch = -1;
    float unit = 0;              /* 资源创建时的 SS 单位 (缩放/DPI 变了须重建) */
    float scale = 1.0f;          /* 本窗口显示器 DPI 尺度 (与主窗 g_s 独立) */

    std::vector<XjsMkIcon> icons;   /* 卡片图标位图缓存 (XjsMkIconGet) */
    XjsStroke* ssRound = NULL;      /* 矢量字形圆头笔画 (画法同菜单/设置分类图标) */
    XjsSolidBrush *brDotR = NULL, *brDotY = NULL, *brDotB = NULL;   /* 彩色文件夹字形的固定彩点 */
    XjsSolidBrush* brWarn = NULL;   /* 异常状态行 (g_skin.warn, 设置页同源) */

    int cat = MK_CAT_ALL;
    float scroll = 0, contentH = 0;
    std::vector<XjsMkEntry> entries;
    std::vector<int> view;       /* 过滤后可见的 entries 下标 (分类 + 搜索词) */
    bool modelDirty = true;      /* 打开/激活/切分类/搜索词变化后重建 (注册表可在设置页被改动) */

    int hoverSide = -1;          /* 悬停分类条目 (MK_CAT_*) */
    int hoverCard = -1;          /* 悬停卡片 (view 下标) */
    bool hoverCtl = false;       /* 指针在悬停卡片的控件 (开关/安装钮) 上 — 控件高亮只认它, 不认整卡悬停 */
    int hoverAct = -1;           /* 悬停侧栏底部动作钮 (0=重扫描 1=打开插件目录) */
    bool trackingLeave = false;

    /* 头部搜索框 (共享单行输入字段组件: 键盘/IME/I-beam/闪烁 路由在组件层) */
    XjsEditField searchEd;
    /* 命令类松开触发 (口径同设置窗): 按下只记待定, 松开仍命中同一目标才执行 */
    int pressSide = -1;
    int pressCard = -1;          /* 按下待定的卡片 (view 下标) */
    bool pressSwitch = false;    /* 待定目标 = 启用开关 (false = 安装钮) */
    int pressAct = -1;           /* 按下待定的侧栏动作钮 */
    std::wstring ctxId;          /* 卡片右键菜单所属插件 id (WM_POPUP_RESULT 回传时按它现查注册表) */
};
static XjsMarketState s_mk;

static XjsSearchWindow* s_mkOwner = NULL;
static XjsSearchWindow* XjsMarketOwner() {
    if (!XjsSearchWindow::Alive(s_mkOwner)) s_mkOwner = XjsSearchWindow::Main();
    return s_mkOwner;
}

static float SS(float v) { return v * s_mk.scale * XjsUiZoom(); }

/* ==================== 数据模型 ==================== */

static void XjsMkRebuildEntries() {
    s_mk.entries.clear();
    for (int i = 0; i < XjsPluginCount(); i++) {
        XjsPluginBrief b;
        if (!XjsPluginBriefAt(i, &b)) continue;
        XjsMkEntry e;
        e.id = b.id;
        e.name = b.name.empty() ? b.id : b.name;
        e.version = b.version;
        e.author = b.author;
        e.desc = b.description;
        e.installed = true;
        e.enabled = b.enabled;
        if (!b.iconFile.empty()) e.iconPath = b.dir + L"\\" + b.iconFile;
        /* 异常状态行 (口径 = 原设置-插件页 行3): 错误信息比简介更重要, 非空时替代简介警色显示 */
        if (!b.declared)
            e.statusErr = XjsT(L"设置.插件.清单错误前缀") + b.manifestErr;
        else if (b.enabled && b.hasDll && !b.loaded && !b.loadErr.empty())
            e.statusErr = XjsT(L"设置.插件.加载失败前缀") + b.loadErr;
        else if (b.enabled && b.staleDll)
            e.statusErr = XjsT(L"设置.插件.需重启说明");
        s_mk.entries.push_back(std::move(e));
    }
    for (const auto& d : MK_DEMO) {
        XjsMkEntry e;
        e.name = XjsT(d.nameKey);
        e.desc = XjsT(d.descKey);
        e.author = XjsT(L"商城.示例作者");
        e.glyph = d.glyph;
        e.demo = true;
        s_mk.entries.push_back(std::move(e));
    }
}

static std::wstring XjsMkLower(const std::wstring& s) {
    std::wstring r;
    r.reserve(s.size());
    for (wchar_t c : s) r.push_back((wchar_t)towlower(c));
    return r;
}

static bool XjsMkMatch(const std::wstring& hay, const std::wstring& qLower) {
    if (qLower.empty()) return true;
    return XjsMkLower(hay).find(qLower) != std::wstring::npos;
}

static void XjsMkRebuildView() {
    std::wstring q = XjsMkLower(s_mk.searchEd.ed.text);
    s_mk.view.clear();
    for (int i = 0; i < (int)s_mk.entries.size(); i++) {
        const XjsMkEntry& e = s_mk.entries[i];
        if (s_mk.cat == MK_CAT_INSTALLED && !e.installed) continue;
        if (s_mk.cat == MK_CAT_AVAILABLE && !e.demo) continue;
        if (!XjsMkMatch(e.name, q) && !XjsMkMatch(e.desc, q) &&
            !XjsMkMatch(e.author, q) && !XjsMkMatch(e.id, q)) continue;
        s_mk.view.push_back(i);
    }
}

/* 分类条目计数 (侧栏右侧小字): 全部/已安装/可安装, 不受搜索词影响 */
static int XjsMkCatCount(int cat) {
    int n = 0;
    for (const auto& e : s_mk.entries) {
        if (cat == MK_CAT_ALL) n++;
        else if (cat == MK_CAT_INSTALLED && e.installed) n++;
        else if (cat == MK_CAT_AVAILABLE && e.demo) n++;
    }
    return n;
}

/* ==================== 布局 (渲染/命中同源, 每帧按现值算) ==================== */

static float XjsMkHeaderH() { return SS(88); }
static float XjsMkSideW()   { return SS(176); }
static float XjsMkCardH()   { return SS(100); }   /* 控件在右上带, 简介独占下部 (2026-10-06 收窄) */

struct XjsMkGrid { float x0 = 0, x1 = 0, y0 = 0, colW = 0; int cols = 1; };

static XjsMkGrid XjsMkGridGeom(float w, float vh) {
    XjsMkGrid g;
    g.x0 = XjsMkSideW() + SS(22);
    g.x1 = w - SS(26);   /* 右缘留滚动条带 */
    g.y0 = XjsMkHeaderH() + SS(14);
    float cw = g.x1 - g.x0;
    if (cw < SS(60)) cw = SS(60);
    g.cols = (cw >= SS(620)) ? 2 : 1;
    float gap = SS(14);
    g.colW = (cw - (g.cols - 1) * gap) / g.cols;
    int rows = ((int)s_mk.view.size() + g.cols - 1) / g.cols;
    if (rows < 1) rows = 1;
    s_mk.contentH = g.y0 + rows * (XjsMkCardH() + gap);
    (void)vh;
    return g;
}

static XjsRect XjsMkCardRect(const XjsMkGrid& g, int vi, float scroll) {
    int col = vi % g.cols, row = vi / g.cols;
    float x = g.x0 + col * (g.colW + SS(14));
    float y = g.y0 + row * (XjsMkCardH() + SS(14)) - scroll;
    return XjsRectF(x, y, x + g.colW, y + XjsMkCardH());
}

/* 卡片内控件矩形 (绘制/命中同源): 右上角, 与 标题/版本·作者 两行同带垂直居中 (带中心 top+32)。
   用户口径 (2026-10-06): 控件挪顶部更省空间 — 卡片高度随之收窄, 简介独占下部整行 */
/* 安装钮宽 = 文本实测 + 24u 内边距 (设置页按钮同式): 短钮给标题让位 (2026-10-06 用户口径) */
static float XjsMkBtnW() {
    if (!s_mk.tfBtn) return SS(72);
    return XjsMeasureText(XjsT(L"商城.安装"), s_mk.tfBtn) + SS(24);
}

static XjsRect XjsMkBtnRect(const XjsRect& card) {
    float w = XjsMkBtnW();
    return XjsRectF(card.right - SS(16) - w, card.top + SS(19),
                    card.right - SS(16), card.top + SS(45));
}

/* 已安装卡片右上角 = 启用开关 (设置页同款 42×24 胶囊, 垂直居中于标题带) */
static XjsRect XjsMkSwitchRect(const XjsRect& card) {
    XjsRect br = XjsMkBtnRect(card);
    float cy = (br.top + br.bottom) / 2, th = SS(24), tw = SS(42);
    return XjsRectF(card.right - SS(16) - tw, cy - th / 2, card.right - SS(16), cy + th / 2);
}

/* 侧栏底部动作钮 (插件页撤销后并入: 0=重新扫描 1=打开插件目录), 底部锚定随窗高。
   重扫描/打开目录 文案复用原设置页键 (设置.插件.*) */
static XjsRect XjsMkSideActRect(float vh, int i) {
    float h = SS(30);
    float y = vh - SS(12) - (2 - i) * (h + SS(8));
    return XjsRectF(SS(10), y, XjsMkSideW() - SS(10), y + h);
}

static int XjsMkSideActHit(float vh, float y) {
    for (int i = 0; i < 2; i++) {
        XjsRect r = XjsMkSideActRect(vh, i);
        if (y >= r.top && y < r.bottom) return i;
    }
    return -1;
}

/* ==================== 资源 (换肤纪元/DPI 变化重建) ==================== */

static void XjsMkFreeResources() {
    XjsLayersDropRt(s_mk.rt);   /* 先摘 s_layers 条目再放 RT (本窗也弹 Toast), 漏摘 = 悬垂堆地址 */
    if (s_mk.rt) { s_mk.rt->Release(); s_mk.rt = NULL; }
    /* 表驱动必须取成员地址遍历并就地置空: 拷贝到栈数组只清副本, s_mk.brXxx/tfXxx 残留
       悬垂包装, 下次 Free (关闭后重开, EnsureResources 先行清理) 二次 Release = UAF 崩
       (2026-10-06 实锤: 关商城→重开商城, WM_PAINT 重建路径崩) */
    XjsSolidBrush** brs[] = { &s_mk.brBg, &s_mk.brBorder, &s_mk.brBorderStrong, &s_mk.brText,
                              &s_mk.brDim, &s_mk.brFaint, &s_mk.brAccent, &s_mk.brAccent2,
                              &s_mk.brAccentSoft, &s_mk.brPanel, &s_mk.brPanel2, &s_mk.brHover, &s_mk.brOk,
                              &s_mk.brDotR, &s_mk.brDotY, &s_mk.brDotB, &s_mk.brWhite, &s_mk.brWarn };
    for (auto* pb : brs) if (*pb) { (*pb)->Release(); *pb = NULL; }
    if (s_mk.brBgGrad) { s_mk.brBgGrad->Release(); s_mk.brBgGrad = NULL; }
    s_mk.bgGradH = -1;
    if (s_mk.ssRound) { s_mk.ssRound->Release(); s_mk.ssRound = NULL; }
    for (auto& ic : s_mk.icons) if (ic.bmp) ic.bmp->Release();   /* 图标位图绑本窗 RT, 随资源重建作废 */
    s_mk.icons.clear();
    XjsFormat** fmts[] = { &s_mk.tfTitle, &s_mk.tfSub, &s_mk.tfName, &s_mk.tfMeta, &s_mk.tfDesc,
                           &s_mk.tfBtn, &s_mk.tfCat, &s_mk.tfCount, &s_mk.tfEmpty, &s_mk.tfSearch };
    for (auto* pf : fmts) if (*pf) { (*pf)->Release(); *pf = NULL; }
    s_mk.brushEpoch = -1;
}

static void XjsMkEnsureResources(HWND hwnd) {
    float unit = SS(1);
    if (s_mk.rt && s_mk.brushEpoch == g_skinEpoch && s_mk.unit == unit) return;
    XjsMkFreeResources();
    RECT rc;
    GetClientRect(hwnd, &rc);
    g_gfx->CreateWindowRt(hwnd, ximax(rc.right, 1), ximax(rc.bottom, 1), &s_mk.rt);
    if (!s_mk.rt) return;
    XjsColor bg1 = g_skin.bg1; bg1.a = 1.0f;   /* HwndRT 无逐像素 alpha */
    XjsColor bg2 = g_skin.bg2; bg2.a = 1.0f;
    s_mk.bg2 = bg2;   /* 背景纵渐变轴在绘制期按窗口高懒重建 (见 XjsMkPaint), 这里只存末端色 */
    s_mk.rt->CreateSolidColorBrush(bg1, &s_mk.brBg);
    s_mk.rt->CreateSolidColorBrush(g_skin.border, &s_mk.brBorder);
    s_mk.rt->CreateSolidColorBrush(g_skin.borderStrong, &s_mk.brBorderStrong);
    s_mk.rt->CreateSolidColorBrush(g_skin.text, &s_mk.brText);
    s_mk.rt->CreateSolidColorBrush(g_skin.textDim, &s_mk.brDim);
    s_mk.rt->CreateSolidColorBrush(g_skin.textFaint, &s_mk.brFaint);
    s_mk.rt->CreateSolidColorBrush(g_skin.accent, &s_mk.brAccent);
    s_mk.rt->CreateSolidColorBrush(g_skin.accent2, &s_mk.brAccent2);
    s_mk.rt->CreateSolidColorBrush(g_skin.accentSoft, &s_mk.brAccentSoft);
    s_mk.rt->CreateSolidColorBrush(g_skin.panel, &s_mk.brPanel);
    s_mk.rt->CreateSolidColorBrush(g_skin.panel2, &s_mk.brPanel2);
    s_mk.rt->CreateSolidColorBrush(g_skin.rowHover, &s_mk.brHover);
    s_mk.rt->CreateSolidColorBrush(g_skin.ok, &s_mk.brOk);
    s_mk.rt->CreateSolidColorBrush(g_skin.warn, &s_mk.brWarn);
    s_mk.rt->CreateSolidColorBrush(XjsColor(1, 1, 1, 1), &s_mk.brWhite);   /* 开关圆钮 (恒白, 设置页同款) */
    float px = SS(1);
    g_gfx->RoundStroke(&s_mk.ssRound);   /* 矢量字形圆头笔画 (两端圆头, 同菜单/设置分类图标) */
    s_mk.rt->CreateSolidColorBrush(XjsColor(0.90f, 0.44f, 0.36f), &s_mk.brDotR);   /* 彩点固定色 (不随皮肤) */
    s_mk.rt->CreateSolidColorBrush(XjsColor(0.91f, 0.72f, 0.29f), &s_mk.brDotY);
    s_mk.rt->CreateSolidColorBrush(XjsColor(0.36f, 0.66f, 0.91f), &s_mk.brDotB);
    struct FDef { XjsFormat** out; float size; DWRITE_FONT_WEIGHT weight; };
    const FDef defs[] = {
        { &s_mk.tfTitle, 15, DWRITE_FONT_WEIGHT_SEMI_BOLD },
        { &s_mk.tfSub,   11, DWRITE_FONT_WEIGHT_NORMAL },
        { &s_mk.tfName,  13, DWRITE_FONT_WEIGHT_SEMI_BOLD },
        { &s_mk.tfMeta,  10.5f, DWRITE_FONT_WEIGHT_NORMAL },
        { &s_mk.tfDesc,  11, DWRITE_FONT_WEIGHT_NORMAL },
        { &s_mk.tfBtn,   11.5f, DWRITE_FONT_WEIGHT_NORMAL },
        { &s_mk.tfCat,   12, DWRITE_FONT_WEIGHT_NORMAL },
        { &s_mk.tfCount, 10, DWRITE_FONT_WEIGHT_NORMAL },
        { &s_mk.tfEmpty, 13, DWRITE_FONT_WEIGHT_NORMAL },
        { &s_mk.tfSearch, 12, DWRITE_FONT_WEIGHT_NORMAL },
    };
    for (const auto& d : defs) {
        g_dw->CreateTextFormat(L"Segoe UI", NULL, d.weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, d.size * px, L"zh-cn", d.out);
        if (*d.out) (*d.out)->SetParagraphAlignment(XJS_PARA_CENTER);   /* 全部垂直居中 (DWrite 默认顶对齐) */
    }
    /* 单行文本关自动换行; 溢出处字符省略号 */
    XjsFormat* nowrap[] = { s_mk.tfTitle, s_mk.tfSub, s_mk.tfName, s_mk.tfMeta,
                            s_mk.tfBtn, s_mk.tfCat, s_mk.tfCount, s_mk.tfEmpty, s_mk.tfSearch };
    for (auto* f : nowrap)
        if (f) { f->SetWordWrapping(XJS_WRAP_NONE); f->SetCharEllipsis(); }
    /* 简介两行词换行 + 末行省略号 (英文按空格断行不劈词, 中文仍逐字); 顶对齐 —
       布局盒封顶绘制时超长内容必须从盒顶起排 (居中会让超长块向上下双向溢出, CLIP 切掉首行) */
    if (s_mk.tfDesc) {
        s_mk.tfDesc->SetWordWrapping(XJS_WRAP_WORD);
        s_mk.tfDesc->SetCharEllipsis();
        s_mk.tfDesc->SetParagraphAlignment(XJS_PARA_NEAR);
    }
    s_mk.brushEpoch = g_skinEpoch;
    s_mk.unit = unit;
}

/* ==================== 卡片图标 (清单 "图标" 位图 + 矢量字形回退) ==================== */

/* 整读小文件 (SHARE_READ 不锁插件目录) */
static bool XjsMkReadIconFile(const std::wstring& path, std::string* out) {
    out->clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[8192]; DWORD rd = 0;
    while (ReadFile(h, buf, sizeof(buf), &rd, NULL) && rd) out->append(buf, rd);
    CloseHandle(h);
    return true;
}

/* 取卡片图标位图 (缓存键 = 路径+修改时间, 插件换图标自动重载; 失败同样入缓存不逐帧重读)。
   同步一次装载 + 缓存口径同引导光标 PNG (小资产懒解码绑本窗 RT); 预览面板的大图异步
   管线不适用此量级 (≤256KB 上限防超大图卡帧), 首帧后零磁盘读。 */
static XjsBitmap* XjsMkIconGet(const std::wstring& path) {
    unsigned long long ft = 0;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa))
        ft = ((unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
    XjsMkIcon* ic = NULL;
    for (auto& e : s_mk.icons)
        if (e.path == path) { ic = &e; break; }
    if (!ic) {
        s_mk.icons.push_back(XjsMkIcon());
        ic = &s_mk.icons.back();
        ic->path = path;
    }
    if (ic->ft == ft) return ic->bmp;   /* 命中 (含失败档 bmp=NULL) */
    if (ic->bmp) ic->bmp->Release();    /* 旧位图只被已完成的帧引用, 换新前释放安全 */
    ic->bmp = NULL;
    ic->ft = ft;
    std::string bytes;
    if (ft && XjsMkReadIconFile(path, &bytes) && bytes.size() <= 256 * 1024)
        ic->bmp = XjsDecodeImageToRt(s_mk.rt, bytes.data(), (int)bytes.size());
    return ic->bmp;
}

/* 矢量字形 (16 格网格线性笔画, 画法同 XjsSetCatIconDraw/XjsMenuIconDraw, 圆头):
   演示占位卡专属字形; 已安装插件未声明 "图标"/装载失败的回退 = 包装盒 (扩展/插件隐喻) */
static void XjsMkGlyphDraw(int glyph, float cx, float cy) {
    XjsRt* rt = s_mk.rt;
    if (!rt || !s_mk.brAccent || !s_mk.ssRound) return;
    float u = SS(2.1f);   /* 16 格 → ~34u, 无底色后随图标区放大 (原 1.7f 是 44u 色板内留边档) */
    auto P = [&](float x, float y) { return XjsPoint2F(cx - 8 * u + x * u, cy - 8 * u + y * u); };
    float w = SS(2.0f);
    XjsBrush* br = s_mk.brAccent;
    auto seg = [&](float x1, float y1, float x2, float y2) { rt->DrawLine(P(x1, y1), P(x2, y2), br, w, s_mk.ssRound); };
    auto rrect = [&](float x0, float y0, float x1, float y1, float r) {
        rt->DrawRoundedRectangle(XjsRoundedRectF(
            XjsRectF(cx - 8 * u + x0 * u, cy - 8 * u + y0 * u, cx - 8 * u + x1 * u, cy - 8 * u + y1 * u),
            r * u, r * u), br, w, s_mk.ssRound);
    };
    auto dot = [&](float x, float y, XjsBrush* c) {
        rt->FillEllipse(XjsEllipseF(P(x, y), 1.25f * u, 1.25f * u), c);
    };
    switch (glyph) {
        case MKG_MD: {   /* Markdown 标记: 圆角框内 M + 下箭头 (官方 mark 同构) */
            rrect(1.6f, 3.4f, 14.4f, 12.6f, 1.8f);
            seg(4.3f, 10.3f, 4.3f, 5.7f);   seg(4.3f, 5.7f, 6.7f, 8.1f);
            seg(6.7f, 8.1f, 9.1f, 5.7f);    seg(9.1f, 5.7f, 9.1f, 10.3f);
            seg(11.9f, 5.7f, 11.9f, 10.5f);
            seg(10.2f, 8.8f, 11.9f, 10.5f); seg(11.9f, 10.5f, 13.6f, 8.8f);
            break;
        }
        case MKG_FOLDCOLOR: {   /* 彩色文件夹: 文件夹 + 三彩点 (固定色, 不随皮肤) */
            seg(5.4f, 3.6f, 7.6f, 3.6f);   seg(7.6f, 3.6f, 9.2f, 5.5f);   seg(9.2f, 5.5f, 14, 5.5f);
            rrect(2, 5.5f, 14, 12.6f, 1.6f);
            if (s_mk.brDotR) dot(5.4f, 9.1f, s_mk.brDotR);
            if (s_mk.brDotY) dot(8, 9.1f, s_mk.brDotY);
            if (s_mk.brDotB) dot(10.6f, 9.1f, s_mk.brDotB);
            break;
        }
        case MKG_DUP: {   /* 重复文件查找: 文档 + 放大镜 */
            rrect(2.4f, 2.6f, 10.6f, 12.2f, 1.6f);
            seg(4.8f, 5.8f, 8.2f, 5.8f);   seg(4.8f, 8.2f, 7.2f, 8.2f);
            rt->DrawEllipse(XjsEllipseF(P(10.7f, 10.7f), 2.7f * u, 2.7f * u), br, w, s_mk.ssRound);
            seg(12.8f, 12.8f, 14.4f, 14.4f);
            break;
        }
        default: {   /* MKG_BOX 插件/扩展回退: 包装盒 (立体六边形 + 内棱) */
            seg(8, 1.8f, 13.6f, 4.9f);    seg(13.6f, 4.9f, 13.6f, 11.1f);
            seg(13.6f, 11.1f, 8, 14.2f);  seg(8, 14.2f, 2.4f, 11.1f);
            seg(2.4f, 11.1f, 2.4f, 4.9f); seg(2.4f, 4.9f, 8, 1.8f);
            seg(2.4f, 4.9f, 8, 8);        seg(8, 8, 13.6f, 4.9f);
            seg(8, 8, 8, 14.2f);
            break;
        }
    }
}

/* ==================== 绘制 ==================== */

/* 搜索框放大镜 (16u 线性小图标: 圆环 + 柄, 颜色随画刷) */
static void XjsMkSearchGlyph(XjsRt* rt, float cx, float cy, XjsBrush* br) {
    float r = SS(4.2f);
    rt->DrawEllipse(XjsEllipseF(XjsPoint2F(cx - SS(1), cy - SS(1)), r, r), br, SS(1.3f));
    rt->DrawLine(XjsPoint2F(cx + SS(2), cy + SS(2)), XjsPoint2F(cx + SS(5.4f), cy + SS(5.4f)), br, SS(1.3f));
}

/* 卡片文本省略绘制 — 必须经 s_mk.rt (本窗自建 RT): 全局 XjsDrawEllText 绑 g_rt=主窗 RT,
   第二窗口跨 RT 域用其画刷 = 绘制被静默丢弃 (同 XjsLineEdit 的 target==g_rt 双路口径)。
   两个 D2D 硬约束决定了这里不能只靠布局选项 (2026-10-07 实测):
   ① DrawTextLayout 的 CLIP 选项裁的是文本度量包围盒而非布局盒 — 垂直溢出形同虚设;
   ② DWrite 省略号签名只在宽度溢出 (单行) 触发, 换行后的高度溢出不裁也不出 "…"。
   故: 单行走布局级 SetCharEllipsis (宽度触发, 可靠); 多行自己二分 "前缀+…" 压进槽位;
   最终一律 PushAxisAlignedClip 硬裁 (XjsLineEdit 同款原语) — 清单文本再长也不越出卡片。 */
static void XjsMkDrawEllLayout(XjsTextLayout* lay, const XjsRect& r, XjsBrush* br) {
    s_mk.rt->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    s_mk.rt->DrawTextLayout(XjsPoint2F(r.left, r.top), lay, br, D2D1_DRAW_TEXT_OPTIONS_NONE);
    s_mk.rt->PopAxisAlignedClip();
}
static void XjsMkDrawEllText(const std::wstring& s, const XjsRect& r, XjsFormat* fmt, XjsBrush* br) {
    if (s.empty() || !s_mk.rt || !fmt) return;
    float maxW = r.right - r.left, maxH = r.bottom - r.top;
    if (maxW <= 0 || maxH <= 0) return;
    XjsTextLayout* lay = NULL;
    if (FAILED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.length(), fmt, maxW, maxH, &lay)) || !lay) return;
    lay->SetWordWrapping(XJS_WRAP_NONE);
    lay->SetCharEllipsis();
    XjsMkDrawEllLayout(lay, r, br);
    lay->Release();
}
static void XjsMkDrawEllLines(const std::wstring& s, const XjsRect& r, XjsFormat* fmt, XjsBrush* br) {
    if (s.empty() || !s_mk.rt || !fmt) return;
    float maxW = r.right - r.left, maxH = r.bottom - r.top;
    if (maxW <= 0 || maxH <= 0) return;
    XjsTextLayout* lay = NULL;
    if (FAILED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.length(), fmt, maxW, 10000, &lay)) || !lay) return;
    XjsTextMetrics m = {};
    bool fits = SUCCEEDED(lay->GetMetrics(&m)) && m.height <= maxH;
    if (fits) {   /* 两行内装得下: 整段直画 */
        XjsMkDrawEllLayout(lay, r, br);
        lay->Release();
        return;
    }
    lay->Release();
    /* 超高: 二分最长前缀使 "前缀+…" 仍装得下 (词换行的实际断点以最终整串实测为准) */
    size_t lo = 0, hi = s.size();
    std::wstring best = L"…";
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        std::wstring t = s.substr(0, mid);
        if (!t.empty() && (t.back() == L' ' || t.back() == L'\t')) t.pop_back();   /* 截点悬空空白 */
        t += L"…";
        XjsTextLayout* tl = NULL;
        if (FAILED(g_dw->CreateTextLayout(t.c_str(), (UINT32)t.length(), fmt, maxW, 10000, &tl))) break;
        XjsTextMetrics tm = {};
        if (SUCCEEDED(tl->GetMetrics(&tm)) && tm.height <= maxH) { best = t; lo = mid; }
        else hi = mid - 1;
        tl->Release();
    }
    if (SUCCEEDED(g_dw->CreateTextLayout(best.c_str(), (UINT32)best.length(), fmt, maxW, 10000, &lay))) {
        XjsMkDrawEllLayout(lay, r, br);
        lay->Release();
    }
}

/* 启用开关 (设置页 XjsSetDrawSwitch 同款: 42×24 胶囊轨道 + 18 白圆钮, 开=accent 轨道+钮右移) */
static void XjsMkDrawSwitch(const XjsRect& tr, bool checked) {
    float r = SS(12);
    s_mk.rt->FillRoundedRectangle(XjsRoundedRectF(tr, r, r),
        checked ? (XjsBrush*)s_mk.brAccent : (XjsBrush*)s_mk.brBorderStrong);
    float d = SS(18);
    float cx = checked ? tr.right - SS(3) - d / 2 : tr.left + SS(3) + d / 2;
    s_mk.rt->FillEllipse(XjsEllipseF(XjsPoint2F(cx, (tr.top + tr.bottom) / 2), d / 2, d / 2), s_mk.brWhite);
}

static void XjsMkPaint(HWND hwnd) {
    XjsWindowScope scope(XjsMarketOwner());   /* 绘制期作用域: XjsT/宏按 owner 解析 (语言每窗化) */
    XjsMkEnsureResources(hwnd);
    if (!s_mk.rt) return;
    if (s_mk.modelDirty) {
        XjsMkRebuildEntries();
        XjsMkRebuildView();
        s_mk.modelDirty = false;
    }
    if (!s_mk.searchEd.blink.Attached())   /* 输入字段组件: 绑定承载窗口 (局部重绘回调) */
        s_mk.searchEd.Attach(hwnd, [hwnd] { InvalidateRect(hwnd, NULL, FALSE); });
    XjsSizeU sz = s_mk.rt->GetPixelSize();
    float w = (float)sz.width, vh = (float)sz.height;
    if (s_mk.bgGradH != vh) {   /* 背景纵渐变轴跟随窗口高: 钉死创建时高度, 拉伸/最大化后下半截会被钳到末端色 */
        XjsGradientStop gs[2] = { { 0.0f, s_mk.brBg->GetColor() }, { 1.0f, s_mk.bg2 } };
        if (s_mk.brBgGrad) { s_mk.brBgGrad->Release(); s_mk.brBgGrad = NULL; }
        s_mk.rt->CreateLinearGradientBrush(XjsPoint2F(0, 0), XjsPoint2F(0, vh + 1), gs, 2, &s_mk.brBgGrad);
        s_mk.bgGradH = vh;
    }
    float headH = XjsMkHeaderH();
    XjsMkGrid g = XjsMkGridGeom(w, vh);
    float maxScroll = s_mk.contentH - vh;
    if (maxScroll < 0) maxScroll = 0;
    if (s_mk.scroll > maxScroll) s_mk.scroll = maxScroll;
    if (s_mk.scroll < 0) s_mk.scroll = 0;

    s_mk.rt->BeginDraw();
    if (s_mk.brBgGrad) s_mk.rt->FillRectangle(XjsRectF(0, 0, w, vh), s_mk.brBgGrad);
    else s_mk.rt->Clear(s_mk.brBg->GetColor());

    /* ---- 头部: 标题 + 副标题 + 搜索框 ---- */
    {
        const wchar_t* title = XjsT(L"商城.标题");
        s_mk.rt->DrawText(title, (UINT32)wcslen(title), s_mk.tfTitle,
            XjsRectF(SS(24), SS(14), w - SS(24), SS(14) + SS(30)), s_mk.brText);
        const wchar_t* sub = XjsT(L"商城.副标题");
        s_mk.rt->DrawText(sub, (UINT32)wcslen(sub), s_mk.tfSub,
            XjsRectF(SS(24), SS(46), w - SS(320), SS(46) + SS(22)), s_mk.brDim);
        XjsRect sr = XjsRectF(w - SS(24) - SS(280), SS(16), w - SS(24), SS(16) + SS(36));
        s_mk.rt->FillRoundedRectangle(XjsRoundedRectF(sr, SS(8), SS(8)), s_mk.brPanel2);
        s_mk.rt->DrawRoundedRectangle(XjsRoundedRectF(sr, SS(8), SS(8)),
            s_mk.searchEd.focused ? s_mk.brAccent : s_mk.brBorder, SS(1.2f));
        XjsMkSearchGlyph(s_mk.rt, sr.left + SS(16), sr.top + SS(18), s_mk.brFaint);
        /* 帧边界即清字段几何: 本帧真画到才由 Render 回写 (滚出/隐藏后命中不落隐形框) */
        s_mk.searchEd.area = {};
        s_mk.searchEd.Render(s_mk.rt,
            XjsRectF(sr.left + SS(28), sr.top + SS(3), sr.right - SS(10), sr.bottom - SS(3)),
            s_mk.tfSearch, s_mk.brText, s_mk.brAccentSoft, s_mk.brText,
            XjsT(L"商城.搜索占位"), s_mk.brFaint);
        s_mk.rt->FillRectangle(XjsRectF(0, headH, w, headH + 1), s_mk.brBorder);
    }

    /* ---- 左侧分类栏 ---- */
    {
        static const struct { int cat; const wchar_t* nameKey; } SIDES[MK_CAT_N] = {
            { MK_CAT_ALL,        L"商城.分类.全部"   },
            { MK_CAT_INSTALLED,  L"商城.分类.已安装" },
            { MK_CAT_AVAILABLE,  L"商城.分类.可安装" },
        };
        float chh = SS(34);
        float y = headH + SS(14);
        for (int si = 0; si < MK_CAT_N; si++) {
            XjsRect br = XjsRectF(SS(10), y, XjsMkSideW() - SS(10), y + chh);
            bool sel = SIDES[si].cat == s_mk.cat;
            if (sel) {
                s_mk.rt->FillRoundedRectangle(XjsRoundedRectF(br, SS(8), SS(8)), s_mk.brAccentSoft);
            } else if (si == s_mk.hoverSide) {
                s_mk.rt->FillRoundedRectangle(XjsRoundedRectF(br, SS(8), SS(8)), s_mk.brHover);
            }
            const wchar_t* nm = XjsT(SIDES[si].nameKey);
            s_mk.rt->DrawText(nm, (UINT32)wcslen(nm), s_mk.tfCat,
                XjsRectF(br.left + SS(14), br.top, br.right - SS(40), br.bottom),
                sel ? s_mk.brAccent : s_mk.brText);
            wchar_t cnt[16];
            swprintf(cnt, 16, L"%d", XjsMkCatCount(SIDES[si].cat));
            s_mk.rt->DrawText(cnt, (UINT32)wcslen(cnt), s_mk.tfCount,
                XjsRectF(br.right - SS(38), br.top, br.right - SS(12), br.bottom),
                sel ? s_mk.brAccent : s_mk.brFaint);
            y += chh;
        }
        /* 侧栏底部动作 (插件页撤销后并入商城): 重扫描 / 打开插件目录 (松开触发) */
        static const wchar_t* const ACT_KEYS[2] = { L"设置.插件.重新扫描", L"设置.插件.插件目录" };
        for (int ai = 0; ai < 2; ai++) {
            XjsRect abr = XjsMkSideActRect(vh, ai);
            bool hov = ai == s_mk.hoverAct;
            s_mk.rt->FillRoundedRectangle(XjsRoundedRectF(abr, SS(8), SS(8)),
                hov ? (XjsBrush*)s_mk.brPanel2 : (XjsBrush*)s_mk.brPanel);
            s_mk.rt->DrawRoundedRectangle(XjsRoundedRectF(abr, SS(8), SS(8)),
                hov ? (XjsBrush*)s_mk.brAccent : (XjsBrush*)s_mk.brBorder, SS(1.2f));
            const wchar_t* t = XjsT(ACT_KEYS[ai]);
            float tw = XjsMeasureText(t, s_mk.tfCat);
            float cx = (abr.left + abr.right - tw) / 2;
            s_mk.rt->DrawText(t, (UINT32)wcslen(t), s_mk.tfCat,
                XjsRectF(cx, abr.top, cx + tw, abr.bottom), hov ? s_mk.brText : s_mk.brDim);
        }
        s_mk.rt->FillRectangle(XjsRectF(XjsMkSideW(), headH + 1, XjsMkSideW() + 1, vh), s_mk.brBorder);
    }

    /* ---- 插件卡片栅格 ---- */
    for (int vi = 0; vi < (int)s_mk.view.size(); vi++) {
        XjsRect cr = XjsMkCardRect(g, vi, s_mk.scroll);
        if (cr.bottom < headH || cr.top > vh) continue;   /* 滚出视口的卡不画 */
        const XjsMkEntry& e = s_mk.entries[s_mk.view[vi]];
        bool hovered = vi == s_mk.hoverCard;
        s_mk.rt->FillRoundedRectangle(XjsRoundedRectF(cr, SS(10), SS(10)),
            hovered ? s_mk.brPanel2 : s_mk.brPanel);
        s_mk.rt->DrawRoundedRectangle(XjsRoundedRectF(cr, SS(10), SS(10)),
            hovered ? s_mk.brBorderStrong : s_mk.brBorder, SS(1.2f));
        /* 图标位: 无底色, 插件图标等比放大占满图标区 (清单 "图标" 装载; 演示卡/未声明/失败 = 矢量字形)。
           区顶对齐标题带 (12), 底缘止于简介上缘 (56) — 占满后图标不得压简介首行 */
        XjsRect ir = XjsRectF(cr.left + SS(16), cr.top + SS(12), cr.left + SS(60), cr.top + SS(56));
        XjsBitmap* icb = e.iconPath.empty() ? NULL : XjsMkIconGet(e.iconPath);
        if (icb) {
            XjsSizeU bs = icb->GetPixelSize();
            if (bs.width > 0 && bs.height > 0) {   /* 等比适配占满图标区 */
                float bw = (float)bs.width, bh = (float)bs.height;
                float k = std::min((ir.right - ir.left) / bw, (ir.bottom - ir.top) / bh);
                float dw = bw * k, dh = bh * k;
                float icx = (ir.left + ir.right - dw) / 2, icy = (ir.top + ir.bottom - dh) / 2;
                s_mk.rt->DrawBitmap(icb, XjsRectF(icx, icy, icx + dw, icy + dh), 1.0f, 2);
            }
        } else {
            XjsMkGlyphDraw(e.glyph, (ir.left + ir.right) / 2, (ir.top + ir.bottom) / 2);
        }
        /* 名称 + 版本·作者 (元信息行): 右界收到控件左侧留缝, 长名/长作者省略号让位右上控件 */
        float tx = cr.left + SS(72);
        XjsRect ctl = e.installed ? XjsMkSwitchRect(cr) : XjsMkBtnRect(cr);
        float txr = ctl.left - SS(10);
        XjsMkDrawEllText(e.name, XjsRectF(tx, cr.top + SS(12), txr, cr.top + SS(34)), s_mk.tfName, s_mk.brText);
        std::wstring meta;
        if (!e.version.empty()) meta = L"v" + e.version;
        if (!e.author.empty()) {
            if (!meta.empty()) meta += L" · ";
            meta += e.author;
        }
        if (!meta.empty())
            XjsMkDrawEllText(meta, XjsRectF(tx, cr.top + SS(34), txr, cr.top + SS(52)), s_mk.tfMeta, s_mk.brDim);
        /* 简介两行: 词换行 + 末行省略号, 布局盒高度硬界 — 清单简介再长也绝不溢出卡片
           (2026-10-07 用户口径); 有异常状态时警色替代 (错误比简介重要) */
        if (e.statusErr.empty())
            XjsMkDrawEllLines(e.desc,
                XjsRectF(cr.left + SS(16), cr.top + SS(56), cr.right - SS(16), cr.top + SS(56) + SS(30)),
                s_mk.tfDesc, s_mk.brDim);
        else
            XjsMkDrawEllLines(e.statusErr,
                XjsRectF(cr.left + SS(16), cr.top + SS(56), cr.right - SS(16), cr.top + SS(56) + SS(30)),
                s_mk.tfDesc, s_mk.brWarn);
        /* 右上角控件: 已安装 = 启用开关 (设置页同款, 可点, 启停走设置页同一执行端);
           未安装/演示卡 = "安装" 主行动钮 (accent 实底, 悬停 accent2, 松开触发)。
           高亮只认控件级悬停 hoverCtl — 整卡悬停点亮按钮违反直觉 (2026-10-06 用户口径) */
        if (e.installed) {
            XjsMkDrawSwitch(ctl, e.enabled);
        } else {
            bool armed = s_mk.hoverCtl && hovered && vi == s_mk.pressCard && !s_mk.pressSwitch;
            s_mk.rt->FillRoundedRectangle(XjsRoundedRectF(ctl, SS(6), SS(6)),
                armed ? (XjsBrush*)s_mk.brAccent2
                      : (s_mk.hoverCtl && hovered ? (XjsBrush*)s_mk.brAccent : (XjsBrush*)s_mk.brBorderStrong));
            const wchar_t* bt = XjsT(L"商城.安装");
            float tw = XjsMeasureText(bt, s_mk.tfBtn);
            float cx = (ctl.left + ctl.right - tw) / 2;
            s_mk.rt->DrawText(bt, (UINT32)wcslen(bt), s_mk.tfBtn,
                XjsRectF(cx, ctl.top, cx + tw, ctl.bottom), s_mk.brBg);
        }
    }

    /* ---- 空态 (分类无内容 / 搜索无匹配) ---- */
    if (s_mk.view.empty()) {
        const wchar_t* msg;
        if (!s_mk.searchEd.ed.text.empty()) msg = XjsT(L"商城.搜索无结果");
        else if (s_mk.cat == MK_CAT_INSTALLED) msg = XjsT(L"商城.尚未安装");
        else msg = XjsT(L"商城.暂无插件");
        s_mk.rt->DrawText(msg, (UINT32)wcslen(msg), s_mk.tfEmpty,
            XjsRectF(g.x0, headH, g.x1, headH + SS(120)), s_mk.brFaint);
    }

    /* ---- 右缘细滚动条 (内容不超高 = 不显示) ---- */
    if (maxScroll > 0 && vh > SS(8)) {
        float track = vh - SS(8);
        float tH = ximax(track * vh / s_mk.contentH, SS(24));
        float tY = SS(4) + (track - tH) * (s_mk.scroll / maxScroll);
        s_mk.rt->FillRoundedRectangle(
            XjsRoundedRectF(XjsRectF(w - SS(7), tY, w - SS(3), tY + tH), SS(2), SS(2)),
            s_mk.brBorderStrong);
    }

    XjsToastRender(hwnd, s_mk.rt, w, vh, SS(1));   /* Toast 组件 (宿主=商城窗, 浮于一切) */
    s_mk.rt->EndDraw();
}

/* ==================== 交互 ==================== */

static void XjsMkClampScroll(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    float maxScroll = s_mk.contentH - (float)(rc.bottom - rc.top);
    if (maxScroll < 0) maxScroll = 0;
    if (s_mk.scroll > maxScroll) s_mk.scroll = maxScroll;
    if (s_mk.scroll < 0) s_mk.scroll = 0;
}

static void XjsMkInvalidate(HWND hwnd) { InvalidateRect(hwnd, NULL, FALSE); }

/* 搜索词变化 → 重过滤 + 重绘 (路由层在编辑后置 ed.dirty) */
static void XjsMkOnSearchEdited(HWND hwnd) {
    if (!s_mk.searchEd.ed.dirty) return;
    s_mk.searchEd.ed.dirty = false;
    XjsMkRebuildView();
    XjsMkClampScroll(hwnd);
    XjsMkInvalidate(hwnd);
}

/* 悬停侧分类条目命中 (屏内 y; 未滚动坐标比对与绘制同源) */
static int XjsMkSideHit(float y) {
    float chh = SS(34);
    float y0 = XjsMkHeaderH() + SS(14);
    for (int si = 0; si < MK_CAT_N; si++)
        if (y >= y0 + si * chh && y < y0 + (si + 1) * chh) return si;
    return -1;
}

/* 悬停卡片命中 → view 下标 (点在卡片矩形上) */
static int XjsMkCardHit(const XjsMkGrid& g, float x, float y, float scroll) {
    if (x < g.x0 || x > g.x1) return -1;
    for (int vi = 0; vi < (int)s_mk.view.size(); vi++) {
        XjsRect cr = XjsMkCardRect(g, vi, scroll);
        if (x >= cr.left && x <= cr.right && y >= cr.top && y <= cr.bottom) return vi;
    }
    return -1;
}

static void XjsMkActivateDemoCard(HWND hwnd, int vi) {
    XjsWindowScope scope(XjsMarketOwner());   /* Toast 文案按 owner 语言解析 */
    (void)vi;
    XjsToastShow(hwnd, XjsT(L"商城.提示.市场未接入"), XTOAST_INFO, SS(1));
}

/* 启停执行端 (口径 = 设置-插件页 XjsSetPluginToggle 同一套): 禁用=闸门关+来源集变化重搜;
   启用=立即加载, 失败 toast 原因不写回; 两条路都落盘。条目按 id 现查注册表下标
   (重扫会移动下标, 卡片上不存索引) */
static int XjsMkPluginIndexById(const std::wstring& id) {
    for (int i = 0; i < XjsPluginCount(); i++) {
        XjsPluginBrief b;
        if (XjsPluginBriefAt(i, &b) && b.id == id) return i;
    }
    return -1;
}

static void XjsMkTogglePlugin(HWND hwnd, const XjsMkEntry& e) {
    int i = XjsMkPluginIndexById(e.id);
    if (i < 0) return;
    XjsPluginBrief b;
    if (!XjsPluginBriefAt(i, &b)) return;
    if (b.enabled) {
        XjsPluginDisable(i);
        XjsSearchNow(false);   /* 来源集可能变化 (插件搜索模式/托管词条) → 重搜整链 */
    } else {
        std::wstring err;
        if (!XjsPluginEnable(i, &err))
            XjsToastShow(hwnd, (XjsT(L"设置.插件.启用失败前缀") + err).c_str(), XTOAST_ERROR, SS(1));
    }
    XjsSaveConfig();
    s_mk.modelDirty = true;   /* 启用态以注册表为准, 下一帧重拉条目 */
    XjsMkInvalidate(hwnd);
}

static void XjsMkSwitchCat(HWND hwnd, int cat) {
    if (s_mk.cat == cat) return;
    s_mk.cat = cat;
    XjsMkRebuildView();
    s_mk.scroll = 0;
    XjsMkInvalidate(hwnd);
}

static LRESULT CALLBACK Xjs_MarketWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    XjsMarketState* p = &s_mk;
    /* 整个 WndProc 统一钉在 owner 上下文 (口径同设置窗): 语言/皮肤/SS() 按 owner 解析,
       owner 悬垂回落主窗 (XjsMarketOwner), 各搜索窗的定时器重绑不影响本窗作用域 */
    XjsWindowScope scope(XjsMarketOwner());
    switch (msg) {
        case WM_PAINT: {
            if (s_mkOwner) s_mkOwner->SyncSkin();   /* 画刷取色跟随 owner 皮肤 (g_skin 是竞态镜像) */
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            XjsMkPaint(hwnd);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (p->rt) { p->rt->Resize(ximax(LOWORD(lParam), 1), ximax(HIWORD(lParam), 1)); XjsMkInvalidate(hwnd); }
            XjsMkClampScroll(hwnd);
            return 0;
        case WM_GETMINMAXINFO: {   /* 拖边自由调整的下限 (侧栏 + 最窄一列卡片放得下) */
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            mmi->ptMinTrackSize.x = (LONG)SS(600);
            mmi->ptMinTrackSize.y = (LONG)SS(420);
            return 0;
        }
        case WM_DPICHANGED: {   /* 拖到不同缩放的显示器: 尺度更新 + 资源重建 + 系统建议矩形 */
            p->scale = HIWORD(wParam) / 96.0f;
            XjsMkFreeResources();
            if (lParam) {
                RECT* r = (RECT*)lParam;
                SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                    SWP_NOZORDER | SWP_NOACTIVATE);
            }
            XjsMkInvalidate(hwnd);
            return 0;
        }
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE: {   /* 分辨率广播不带 WM_DPICHANGED: 按每窗有效 DPI 自检对齐 */
            UINT dpi = XjsGetDpiForWindow(hwnd);
            if (dpi && (int)(p->scale * 96.0f + 0.5f) != (int)dpi) {
                p->scale = dpi / 96.0f;
                XjsMkFreeResources();
                XjsMkInvalidate(hwnd);
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (XjsEditFieldMouseMove(hwnd, pt)) return 0;   /* 输入字段拖选跟手 */
            { RECT crc; GetClientRect(hwnd, &crc);
              if (XjsToastMouseMove(hwnd, SS(1), (float)crc.right, (float)crc.bottom, pt)) return 0; }
            RECT crc;
            GetClientRect(hwnd, &crc);
            XjsMkGrid g = XjsMkGridGeom((float)crc.right, (float)crc.bottom);
            int hs = -1, hc = -1, ha = -1;
            bool ctl = false;
            if (pt.x < XjsMkSideW()) {
                hs = XjsMkSideHit((float)pt.y);
                if (hs < 0) ha = XjsMkSideActHit((float)crc.bottom, (float)pt.y);
            } else {
                hc = XjsMkCardHit(g, (float)pt.x, (float)pt.y, p->scroll);
                if (hc >= 0) {   /* 控件级悬停: 高亮/手型只认控件矩形, 整卡悬停不算 (用户口径 2026-10-06) */
                    const XjsMkEntry& en = s_mk.entries[p->view[hc]];
                    XjsRect crd = XjsMkCardRect(g, hc, p->scroll);
                    XjsRect r = en.installed ? XjsMkSwitchRect(crd) : XjsMkBtnRect(crd);
                    ctl = pt.x >= r.left && pt.x <= r.right && pt.y >= r.top && pt.y <= r.bottom;
                }
            }
            if (hs != p->hoverSide || hc != p->hoverCard || ha != p->hoverAct || ctl != p->hoverCtl) {
                p->hoverSide = hs;
                p->hoverCard = hc;
                p->hoverAct = ha;
                p->hoverCtl = ctl;
                XjsMkInvalidate(hwnd);
            }
            if (!p->trackingLeave) {
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                if (TrackMouseEvent(&tme)) p->trackingLeave = true;
            }
            /* 本处理直接 return 0 不过 DefWindowProc → WM_SETCURSOR 不会到来, 显式设光标 */
            SetCursor(LoadCursorW(NULL, (ctl || ha >= 0) ? IDC_HAND : IDC_ARROW));
            return 0;
        }
        case WM_MOUSELEAVE:
            p->trackingLeave = false;
            XjsToastHoverReset(hwnd);
            if (p->hoverSide != -1 || p->hoverCard != -1 || p->hoverAct != -1 || p->hoverCtl) {
                p->hoverSide = -1;
                p->hoverCard = -1;
                p->hoverAct = -1;
                p->hoverCtl = false;
                XjsMkInvalidate(hwnd);
            }
            return 0;
        case WM_MOUSEWHEEL: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            float delta = -((float)GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA) * SS(34) * 3.0f;
            float maxScroll = p->contentH - (float)(rc.bottom - rc.top);
            if (maxScroll < 0) maxScroll = 0;
            float ns = p->scroll + delta;
            if (ns < 0) ns = 0;
            if (ns > maxScroll) ns = maxScroll;
            if (ns != p->scroll) { p->scroll = ns; XjsMkInvalidate(hwnd); }
            return 0;
        }
        case WM_RBUTTONDOWN: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            XjsEditFieldContextMenu(hwnd, pt);   /* 输入字段右键=编辑菜单 (未中字段=无操作) */
            /* 卡片右键 (已安装) = 管理菜单: 打开该插件目录 (插件页撤销后的入口) */
            RECT crc;
            GetClientRect(hwnd, &crc);
            XjsMkGrid g = XjsMkGridGeom((float)crc.right, (float)crc.bottom);
            int hc = XjsMkCardHit(g, (float)pt.x, (float)pt.y, p->scroll);
            if (hc >= 0 && s_mk.entries[p->view[hc]].installed) {
                p->ctxId = s_mk.entries[p->view[hc]].id;
                XjsPopupItem dir, unins;
                dir.id = IDM_MKCTX_OPENDIR;
                dir.title = XjsT(L"设置.插件.插件目录");
                dir.icon = XMI_FOLDER;
                unins.id = IDM_MKCTX_UNINSTALL;
                unins.title = XjsT(L"商城.卸载");
                unins.icon = XMI_DELETE;
                unins.accent = true;   /* 破坏性动作以强调色示出 */
                std::vector<XjsPopupItem> items;
                items.push_back(std::move(dir));
                items.push_back(std::move(unins));
                POINT sp = pt;
                ClientToScreen(hwnd, &sp);
                XjsShowPopupMenu(hwnd, sp, items, SS(168));
            }
            return 0;
        }
        case WM_POPUP_RESULT: {
            int id = (int)wParam;
            if (id >= IDM_FCTX_BASE && id < IDM_FCTX_BASE + 5) {   /* 字段编辑菜单回传必须最先分流 */
                XjsEditFieldMenuCmd(hwnd, id - IDM_FCTX_BASE);
                return 0;
            }
            if (id == IDM_MKCTX_OPENDIR) {   /* 卡片右键·打开该插件目录 (按 ctxId 现查注册表下标) */
                int i = XjsMkPluginIndexById(s_mk.ctxId);
                if (i >= 0) XjsPluginOpenDir(i);
                return 0;
            }
            if (id == IDM_MKCTX_UNINSTALL) {   /* 卡片右键·卸载: 通用询问框二次确认后执行 */
                int i = XjsMkPluginIndexById(s_mk.ctxId);
                XjsPluginBrief b;
                if (i < 0 || !XjsPluginBriefAt(i, &b)) return 0;
                std::wstring desc = XjsFmt(XjsT(L"商城.卸载确认说明"), b.name);
                std::string askBtns = std::string("[{\"text\":\"") + XjsTUtf8(L"商城.卸载") +
                                      "\",\"style\":\"danger\"},{\"text\":\"" + XjsTUtf8(L"通用词.取消") + "\"}]";
                if (XjsShowAskDialog(hwnd, XjsT(L"商城.卸载"), desc.c_str(), askBtns.c_str()) != 0) return 0;
                std::wstring err;
                if (!XjsPluginUninstall(i, &err))
                    XjsToastShow(hwnd, (XjsT(L"商城.卸载失败前缀") + err).c_str(), XTOAST_ERROR, SS(1));
                p->modelDirty = true;   /* 下一帧重拉注册表 (被卸条目消失) */
                XjsMkInvalidate(hwnd);
                return 0;
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (XjsEditFieldDoubleClick(hwnd, pt)) return 0;   /* 字段内双击=选整词 */
        }
            [[fallthrough]];
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            { RECT crc; GetClientRect(hwnd, &crc);
              if (XjsToastMouseDown(hwnd, SS(1), (float)crc.right, (float)crc.bottom, pt)) return 0; }
            /* 输入字段: 点进=聚焦+点定位; 点字段外=失焦后继续常规处理 */
            if (XjsEditFieldMouseDown(hwnd, pt)) return 0;
            RECT crc;
            GetClientRect(hwnd, &crc);
            XjsMkGrid g = XjsMkGridGeom((float)crc.right, (float)crc.bottom);
            if (pt.x < XjsMkSideW()) {
                p->pressSide = XjsMkSideHit((float)pt.y);   /* 分类: 记待定, 松开触发 */
                if (p->pressSide < 0)
                    p->pressAct = XjsMkSideActHit((float)crc.bottom, (float)pt.y);   /* 底部动作钮同口径 */
                return 0;
            }
            int hc = XjsMkCardHit(g, (float)pt.x, (float)pt.y, p->scroll);
            if (hc >= 0) {
                const XjsMkEntry& en = s_mk.entries[p->view[hc]];
                XjsRect crd = XjsMkCardRect(g, hc, p->scroll);
                XjsRect ctl = en.installed ? XjsMkSwitchRect(crd) : XjsMkBtnRect(crd);
                if (pt.x >= ctl.left && pt.x <= ctl.right && pt.y >= ctl.top && pt.y <= ctl.bottom) {
                    p->pressCard = hc;   /* 卡片右下控件: 记待定, 松开触发 */
                    p->pressSwitch = en.installed;
                }
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            XjsEditFieldMouseUp(hwnd);   /* 字段拖选收尾+释放捕获 */
            { RECT crc; GetClientRect(hwnd, &crc);
              XjsToastMouseUp(hwnd, SS(1), (float)crc.right, (float)crc.bottom, pt); }
            /* 命令落地 (松开触发口径): 按下待定 + 松开仍命中同一目标才执行, 拖离=取消 */
            int pendingSide = p->pressSide;
            int pendingCard = p->pressCard;
            bool pendingSwitch = p->pressSwitch;
            int pendingAct = p->pressAct;
            p->pressSide = -1;
            p->pressCard = -1;
            p->pressSwitch = false;
            p->pressAct = -1;
            RECT crc;
            GetClientRect(hwnd, &crc);
            XjsMkGrid g = XjsMkGridGeom((float)crc.right, (float)crc.bottom);
            if (pendingSide >= 0 && pt.x < XjsMkSideW() && pendingSide == XjsMkSideHit((float)pt.y)) {
                static const int SIDE_CAT[MK_CAT_N] = { MK_CAT_ALL, MK_CAT_INSTALLED, MK_CAT_AVAILABLE };
                XjsMkSwitchCat(hwnd, SIDE_CAT[pendingSide]);
                return 0;
            }
            if (pendingAct >= 0 && pt.x < XjsMkSideW() &&
                pendingAct == XjsMkSideActHit((float)crc.bottom, (float)pt.y)) {
                if (pendingAct == 0) {   /* 重新扫描: 收新目录/刷新清单, 下一帧重拉条目 (设置页同口径) */
                    XjsPluginRescan();
                    p->modelDirty = true;
                    XjsMkInvalidate(hwnd);
                } else {
                    XjsPluginOpenDir(-1);   /* 打开插件根目录 (目录被删内部退回 exe 目录) */
                }
                return 0;
            }
            if (pendingCard >= 0) {
                const XjsMkEntry& en = s_mk.entries[p->view[pendingCard]];
                XjsRect crd = XjsMkCardRect(g, pendingCard, p->scroll);
                XjsRect ctl = pendingSwitch ? XjsMkSwitchRect(crd) : XjsMkBtnRect(crd);
                if (pt.x >= ctl.left && pt.x <= ctl.right && pt.y >= ctl.top && pt.y <= ctl.bottom) {
                    if (pendingSwitch) XjsMkTogglePlugin(hwnd, en);
                    else XjsMkActivateDemoCard(hwnd, pendingCard);
                }
            }
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            /* 输入字段聚焦: Enter/Esc=收束聚焦, 其余编辑键交组件路由 */
            if (XjsEditFocused(hwnd)) {
                if (wParam == VK_RETURN || wParam == VK_ESCAPE) {
                    XjsEditFocused(hwnd)->SetFocused(hwnd, false);
                    XjsMkInvalidate(hwnd);
                    return 0;
                }
                if (XjsEditRouteMsg(hwnd, msg, wParam, lParam)) { XjsMkOnSearchEdited(hwnd); return 0; }
            }
            if (wParam == VK_ESCAPE) {   /* 无聚焦字段: Esc 关窗 (设置窗同口径) */
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_CHAR:
            if (XjsEditRouteMsg(hwnd, msg, wParam, lParam)) { XjsMkOnSearchEdited(hwnd); return 0; }
            return 0;   /* 无其它字符行为, 吞掉防误触 */
        case WM_IME_CHAR:
            XjsEditRouteMsg(hwnd, msg, wParam, lParam);   /* 聚焦期吞掉防双份插入 */
            XjsMkOnSearchEdited(hwnd);
            return 0;
        case WM_IME_STARTCOMPOSITION:
            XjsEditRouteMsg(hwnd, msg, wParam, lParam);   /* 组字窗钉到字段 */
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case WM_IME_COMPOSITION: {
            if (XjsEditRouteImeResult(hwnd, lParam)) { XjsMkOnSearchEdited(hwnd); return 0; }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_SETCURSOR: {
            if (LOWORD(lParam) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                if (XjsEditFieldHit(hwnd, pt)) { SetCursor(LoadCursorW(NULL, IDC_IBEAM)); return TRUE; }
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_ACTIVATE:
            XjsCaretBlink::SetWindowActive(hwnd, LOWORD(wParam) != WA_INACTIVE);   /* 失活熄光标 */
            if (LOWORD(wParam) != WA_INACTIVE) p->modelDirty = true;   /* 激活即重拉: 插件可在设置页被启停 */
            return 0;
        case WM_TIMER:
            if (wParam == ID_TIMER_CARET) {
                XjsCaretBlink::TickWindow(hwnd);   /* 输入字段光标闪烁 */
                return 0;
            }
            if (wParam == ID_TIMER_TOAST) {
                if (!XjsToastTimerTick(hwnd, SS(1))) KillTimer(hwnd, ID_TIMER_TOAST);
                return 0;
            }
            break;
        case WM_DESTROY:
            XjsMkFreeResources();
            KillTimer(hwnd, ID_TIMER_TOAST);           /* Toast 组件定时器摘除 */
            XjsWindowComponentsDetach(hwnd);           /* 组件统一退登记 (Toast 条目/画刷 + 字段登记 + 闪烁驱动) */
            p->searchEd.area = {};
            p->hwnd = NULL;
            p->hoverSide = p->hoverCard = -1;
            p->hoverCtl = false;
            p->hoverAct = -1;
            p->pressSide = p->pressCard = -1;
            p->pressSwitch = false;
            p->pressAct = -1;
            p->ctxId.clear();
            p->scroll = 0;
            p->modelDirty = true;   /* 单例状态复位 (窗销毁后残留会在重开时"复活", 口径同设置窗) */
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void XjsRegisterMarketClass(HINSTANCE hInst) {
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Xjs_MarketWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.style = CS_DBLCLKS;   /* 缺它系统不发 WM_LBUTTONDBLCLK, 输入字段双击选词失效 */
    wc.hIcon = XjsAppIconBig();
    wc.hIconSm = XjsAppIconSmall();
    wc.lpszClassName = L"XJS_MarketWnd";
    RegisterClassExW(&wc);
}

void XjsMarketShow() {
    s_mkOwner = XjsSearchWindow::Cur();   /* 绑定发起窗口 (语言/皮肤按它解析) */
    if (s_mkOwner) s_mkOwner->SyncSkin(); /* g_skin 认回 owner: 首帧画刷即 owner 皮肤 */
    s_mk.modelDirty = true;               /* 打开即重拉注册表 (插件启停状态可能已变) */
    if (s_mk.hwnd) {
        SetWindowTextW(s_mk.hwnd, XjsT(L"应用.商城窗口标题"));
        InvalidateRect(s_mk.hwnd, NULL, FALSE);
        ShowWindow(s_mk.hwnd, SW_RESTORE);
        SetForegroundWindow(s_mk.hwnd);
        return;
    }
    s_mk.scale = g_s;   /* 初值取主窗尺度 (通常同屏); 跨屏由 WM_DPICHANGED 纠正 */
    s_mk.cat = MK_CAT_ALL;
    s_mk.scroll = 0;
    s_mk.searchEd.ed.SetText(L"", false);
    int w = (int)SS(880), h = (int)SS(640);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    if (w > sw - 40) w = sw - 40;
    if (h > sh - 100) h = sh - 100;
    RECT orr;
    GetWindowRect(g_hWnd, &orr);
    int x = orr.left + ((orr.right - orr.left) - w) / 2;
    int y = orr.top + ((orr.bottom - orr.top) - h) / 2;
    if (x < 8) x = 8;
    if (y < 8) y = 8;
    HWND hwnd = CreateWindowExW(0, L"XJS_MarketWnd", XjsT(L"应用.商城窗口标题"),
        WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME | WS_MAXIMIZEBOX, x, y, w, h,
        g_hWnd, NULL, GetModuleHandleW(NULL), NULL);
    if (!hwnd) return;
    s_mk.hwnd = hwnd;
    SendMessageW(hwnd, WM_SETICON, ICON_BIG,   (LPARAM)XjsAppIconBig());     /* 窗口级图标 (商城窗有任务栏按钮) */
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)XjsAppIconSmall());
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));   /* 深色标题栏 (20H1+) */
    DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));                                     /* 更早 build 兜底, 失败无碍 */
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    InvalidateRect(hwnd, NULL, FALSE);
}
