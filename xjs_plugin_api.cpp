/*
 * xjs_plugin_api.cpp — 插件扩展 API: EXE 设置 / 界面操作 / 搜索模式管理 (2026-09-24)
 * ============================================================================
 * 插件侧入口 = 宿主表尾追加的 host->QueryApi(name) 名称解析 (xjs_plugin_sdk.h):
 * 拿到函数指针后按 XjsApi* 类型调用。设计口径:
 *   - 宿主表自此冻结 (v4 只追加纪律的最后一件), 以后新 API 一律"加名字 + 加类型", 不再扩表。
 *   - 权限: 读免权限 (照 GetSkinJson 口径); 写设置/增删模式 = 清单 "权限" 加 "settings";
 *     界面动作/按模式执行 = "ui"。闸门唯一入口 = xjs_plugin.cpp 的 XjsPluginApiGate
 *     (PluginApiCheck 薄包装), 本文件不做任何旁路校验。
 *   - 全部仅 UI 线程 (摸的都是窗口/UI 状态; 引擎数据照旧直连 xunjieso, 不在本表面)。
 *     每窗状态一律先 XjsWindowScope 认窗再动 (可维护性红线: 禁依赖"当前窗"隐式状态)。
 *   - 设置写入 = 改窗口字段 / 调既有应用入口 (与设置页同一落点) + XjsSaveConfig 落盘
 *     (审计口径: 所有设置修改点必须即时落盘)。
 *   - 运行时搜索模式 = 会话级 (不入 xjs_config.json; 插件要持久模式用清单 "搜索模式"
 *     声明)。与清单模式共用合并视图 — srcId 仍 "p:<插件id>:<序>", 序号从
 *     XJS_PLUGIN_RT_MODE_BASE 起 (清单模式序永远到不了), 药丸菜单/托管标签链/prune
 *     全走既有路径, 引擎零改动。
 *   - JSON 键为中文主键 (与配置文件/清单口径一致); 输出 = 调用方缓冲约定 (PluginBufOut)。
 */
#include "xjs_plugin_api.h"
#include "xjs_plugin_sdk.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

/* ==================== 运行时搜索模式存储 (会话级, owner = 插件 id) ==================== */

struct XjsRtMode {
    std::wstring owner;       /* 添加者插件 id (目录名); 禁用 = 闸门自然隐藏, 删除 = 本表剔除 */
    int srcIdx = 0;           /* srcId 序号段 (≥ XJS_PLUGIN_RT_MODE_BASE), 自增不复用 — 删除不漂移 */
    XjsPluginModeDef def;     /* def.id = 完整 srcId (与清单模式的"标识"语义对齐) */
};
static std::vector<XjsRtMode> s_rtModes;
static int s_rtSeq = XJS_PLUGIN_RT_MODE_BASE;   /* 全局递增 (srcId 全局唯一即可) */

static const int XJS_RT_PER_PLUGIN_MAX = 64;    /* 每插件运行时模式上限 (清单模式另有清单自身约束) */

static std::wstring XjsRtOwnerId(int pluginIdx) {
    XjsPluginBrief b;
    return XjsPluginBriefAt(pluginIdx, &b) ? b.id : std::wstring();
}

int XjsPluginApiRtModeCount(int pluginIdx) {
    std::wstring id = XjsRtOwnerId(pluginIdx);
    if (id.empty()) return 0;
    int n = 0;
    for (auto& r : s_rtModes)
        if (r.owner == id) n++;
    return n;
}

const XjsPluginModeDef* XjsPluginApiRtModeDefAt(int pluginIdx, int srcIdx) {
    std::wstring id = XjsRtOwnerId(pluginIdx);
    if (id.empty()) return NULL;
    for (auto& r : s_rtModes)
        if (r.owner == id && r.srcIdx == srcIdx) return &r.def;
    return NULL;
}

void XjsPluginApiPruneOwners() {
    if (s_rtModes.empty()) return;
    std::vector<std::wstring> live;
    for (int i = 0; i < XjsPluginCount(); i++) {
        XjsPluginBrief b;
        if (XjsPluginBriefAt(i, &b)) live.push_back(b.id);
    }
    s_rtModes.erase(std::remove_if(s_rtModes.begin(), s_rtModes.end(),
                                   [&](const XjsRtMode& r) {
                                       return std::find(live.begin(), live.end(), r.owner) == live.end();
                                   }),
                    s_rtModes.end());
}

/* ==================== 名称↔值映射 (视图 / 关键词模式, 与配置文件取值同串) ==================== */

static const char* const XJS_VIEW_NAMES[4] = { "list", "details", "medium", "large" };
static const wchar_t* const XJS_MODE_NAMES[5] = { L"wildcard", L"regex", L"sql", L"lua", L"lua-exec" };

static std::string ViewNameUtf8(int vm) {
    return (vm >= 0 && vm < 4) ? XJS_VIEW_NAMES[vm] : XJS_VIEW_NAMES[0];
}
static int ViewIndexFromUtf8(const std::wstring& s) {
    for (int i = 0; i < 4; i++)
        if (s == Utf8ToUtf16(XJS_VIEW_NAMES[i])) return i;
    return -1;
}
static std::string KeyModeNameUtf8(int m) {
    std::wstring n = (m >= 0 && m < 5) ? XJS_MODE_NAMES[m] : XJS_MODE_NAMES[0];
    return Utf16ToUtf8(n.c_str());
}
static int KeyModeIndexFromUtf8(const std::wstring& s) {
    for (int i = 0; i < 5; i++)
        if (s == XJS_MODE_NAMES[i]) return i;
    return -1;
}
int XjsPluginApiKeyModeIndexFromUtf8(const std::wstring& s) {
    return KeyModeIndexFromUtf8(s);
}

/* ==================== 小工具 ==================== */

static std::string JBool(bool b) { return b ? "true" : "false"; }
static std::string JNum(long long v) { return std::to_string(v); }

/* 成员取值 (XjsPluginJsonMembers 产物; 类型不符 = 假) */
static bool MemberBool(const XjsJsonMember& m, bool* v) { if (m.type != 1) return false; *v = m.b; return true; }
static bool MemberInt(const XjsJsonMember& m, int* v) {
    if (m.type != 2 || m.num != m.num) return false;   /* NaN 拒收 */
    *v = (int)m.num;
    return true;
}
static bool MemberStr(const XjsJsonMember& m, std::wstring* v) { if (m.type != 3) return false; *v = m.str; return true; }

/* ==================== settings.get / settings.set (每窗设置) ==================== */

static int ApiSettingsGet(XjsPluginCtx* ctx, XjsWindowToken window, char* buf, int cap) {
    int e;
    XjsSearchWindow* w = XjsPluginApiWindow(ctx, window, &e);
    if (!w) return e;
    XjsWindowScope scope(w);   /* 皮肤/搜索词等读的是"当前窗"镜像字段, 先认窗 */
    std::string j = "{";
    j += "\"窗口名称\":" + PluginJsonStr(Utf16ToUtf8(w->name.c_str()));
    j += ",\"主窗\":" + JBool(w->isMain);
    j += ",\"档案槽\":" + JNum(w->uiIndex);
    j += ",\"视图\":\"" + ViewNameUtf8((int)w->viewMode) + "\"";
    j += ",\"页面缩放\":" + JNum(w->uiZoom * 10);          /* 十分位 → 50..200 (%) */
    j += ",\"皮肤\":" + PluginJsonStr(Utf16ToUtf8(w->skinName.c_str()));
    j += ",\"预览\":" + JBool(w->previewVisible);
    j += ",\"预览宽度\":" + JNum(w->previewWidth);
    j += ",\"置顶\":" + JBool(w->topmost);
    j += ",\"失焦行为\":" + JNum(w->blurAction);
    j += ",\"显示控制按钮\":" + JBool(w->showCtrlBtns);
    j += ",\"显示筛选框\":" + JBool(w->showFilterBox);
    j += ",\"显示状态栏\":" + JBool(w->showStatusbar);
    j += ",\"任务栏图标\":" + JBool(w->taskbarIcon);
    j += ",\"鼠标打开\":" + JNum(w->mouseOpen);
    j += ",\"默认选中\":" + JNum(w->defaultSel);
    j += ",\"语言\":\"" + std::string(XjsLangCodeUtf8(w->lang)) + "\"";
    j += ",\"搜索模式\":\"" + KeyModeNameUtf8(w->mode) + "\"";
    j += "}";
    return PluginBufOut(buf, cap, j);
}

/* 皮肤应用 = 设置页换肤同一落点 (写窗字段 + 全局镜像 + 载入 + 应用 + 落盘 + 插件换肤事件) */
static void ApplySkin(XjsSearchWindow* w, const std::wstring& name) {
    w->skinName = name;
    g_skinName = name;
    XjsSkinLoad(g_skinName.c_str());
    XjsSkinApply();   /* 纪元+1 → 本窗口画刷随绘制重建 */
    XjsSaveConfig();
    XjsPluginOnSkinChanged(XjsPluginApiTokenOf(w));
    w->Invalidate();
}

/* 预览开关 = XjsPreviewToggle 的定向版 (藏面板先结束接管会话; 开启回填当前选中) */
static void ApplyPreviewVisible(XjsSearchWindow* w, bool v) {
    if (w->previewVisible == v) return;
    if (!v && w->plugPanelOn) XjsPreviewPanelCloseForToggle();
    w->previewVisible = v;
    if (v) {
        w->previewFileId = -1;   /* 破去重: 隐藏期间的选中真正重载 */
        XjsPreviewUpdateSelection();
    }
    XjsSaveConfig();
    XjsClampScroll();
    w->Invalidate();
}

static int ApiSettingsSet(XjsPluginCtx* ctx, XjsWindowToken window, const char* membersJsonUtf8) {
    int e;
    if ((e = XjsPluginApiGate(ctx, XPP_SETTINGS)) != XJS_PLUGIN_OK) return e;
    std::vector<XjsJsonMember> ms;
    if (!XjsPluginJsonMembers(membersJsonUtf8, &ms) || ms.empty()) return XJS_PLUGIN_ERR_ARG;
    XjsSearchWindow* w = XjsPluginApiWindow(ctx, window, &e);
    if (!w) return e;

    /* 先整体验证 (全部键已知 + 值域合法), 再统一应用 — 不做半套 */
    for (auto& m : ms) {
        bool b; int n; std::wstring s;
        if (m.key == L"视图") { if (!MemberStr(m, &s) || ViewIndexFromUtf8(s) < 0) return XJS_PLUGIN_ERR_ARG; }
        else if (m.key == L"页面缩放") { if (!MemberInt(m, &n) || n < 50 || n > 200) return XJS_PLUGIN_ERR_ARG; }
        else if (m.key == L"皮肤") {
            if (!MemberStr(m, &s) || s.empty()) return XJS_PLUGIN_ERR_ARG;
            bool hit = false;
            for (auto& k : XjsSkinEnumerate()) if (k == s) { hit = true; break; }
            if (!hit) return XJS_PLUGIN_ERR_NOTFOUND;
        }
        else if (m.key == L"预览") { if (!MemberBool(m, &b)) return XJS_PLUGIN_ERR_ARG; }
        else if (m.key == L"预览宽度") { if (!MemberInt(m, &n)) return XJS_PLUGIN_ERR_ARG; }
        else if (m.key == L"置顶") { if (!MemberBool(m, &b)) return XJS_PLUGIN_ERR_ARG; }
        else if (m.key == L"失焦行为") { if (!MemberInt(m, &n) || n < 0 || n > 1) return XJS_PLUGIN_ERR_ARG; }
        else if (m.key == L"显示控制按钮" || m.key == L"显示筛选框" || m.key == L"显示状态栏") {
            if (!MemberBool(m, &b)) return XJS_PLUGIN_ERR_ARG;
        }
        else if (m.key == L"任务栏图标") { if (!MemberBool(m, &b)) return XJS_PLUGIN_ERR_ARG; }
        else if (m.key == L"鼠标打开" || m.key == L"默认选中") { if (!MemberInt(m, &n) || n < 0 || n > 1) return XJS_PLUGIN_ERR_ARG; }
        else if (m.key == L"语言") {
            if (!MemberStr(m, &s)) return XJS_PLUGIN_ERR_ARG;
            if (s != L"auto" && XjsLangIndexFromCode(s.c_str()) == XLANG_AUTO)
                return XJS_PLUGIN_ERR_NOTFOUND;   // 未知代码 (有效值 = langs.list; auto 恒合法)
        }
        else if (m.key == L"搜索模式") { if (!MemberStr(m, &s) || KeyModeIndexFromUtf8(s) < 0) return XJS_PLUGIN_ERR_ARG; }
        else return XJS_PLUGIN_ERR_ARG;   /* 未知键显式拒绝 (不静默) */
    }

    XjsWindowScope scope(w);
    for (auto& m : ms) {
        bool b = false; int n = 0; std::wstring s;
        if (m.key == L"视图") {
            MemberStr(m, &s);
            XjsSetViewMode(ViewIndexFromUtf8(s));   /* 锚点保持 + 落盘 + 重绘 (设置页同款入口) */
        } else if (m.key == L"页面缩放") {
            MemberInt(m, &n);
            XjsApplyZoom(n / 10);                   /* 内含夹取/落盘/文本格式重建/重绘 */
        } else if (m.key == L"皮肤") {
            MemberStr(m, &s);
            ApplySkin(w, s);
        } else if (m.key == L"预览") {
            MemberBool(m, &b);
            ApplyPreviewVisible(w, b);
        } else if (m.key == L"预览宽度") {
            MemberInt(m, &n);
            if (n < 160) n = 160;
            if (n > 2000) n = 2000;
            w->previewWidth = n;
            XjsSaveConfig();
            w->Invalidate();
        } else if (m.key == L"置顶") {
            MemberBool(m, &b);
            w->topmost = b;
            SetWindowPos(w->hWnd, b ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            XjsSaveConfig();
            w->Invalidate();
        } else if (m.key == L"失焦行为") {
            MemberInt(m, &n);
            w->blurAction = n;
            XjsSaveConfig();
        } else if (m.key == L"显示控制按钮") {
            MemberBool(m, &b); w->showCtrlBtns = b; XjsSaveConfig(); w->Invalidate();
        } else if (m.key == L"显示筛选框") {
            MemberBool(m, &b); w->showFilterBox = b; XjsSaveConfig(); w->Invalidate();
        } else if (m.key == L"显示状态栏") {
            MemberBool(m, &b); w->showStatusbar = b; XjsSaveConfig(); w->Invalidate();
        } else if (m.key == L"任务栏图标") {
            MemberBool(m, &b);
            w->taskbarIcon = b;
            w->ApplyTaskbarIcon();
            XjsSaveConfig();
            w->Invalidate();
        } else if (m.key == L"鼠标打开") {
            MemberInt(m, &n); w->mouseOpen = n; XjsSaveConfig();
        } else if (m.key == L"默认选中") {
            MemberInt(m, &n); w->defaultSel = n; XjsSaveConfig();
        } else if (m.key == L"语言") {
            MemberStr(m, &s);
            XjsApplyUiLang(w, (s == L"auto") ? XLANG_AUTO : XjsLangIndexFromCode(s.c_str()));
        } else if (m.key == L"搜索模式") {
            MemberStr(m, &s);
            w->mode = KeyModeIndexFromUtf8(s);
            XjsSaveConfig();
            XjsSearchNow(false);                    /* 切换即生效: 按新模式重搜当前词 (药丸菜单同口径) */
        }
    }
    return XJS_PLUGIN_OK;
}

/* ==================== settings.global.get / settings.global.set ==================== */

static int ApiGlobalGet(XjsPluginCtx* ctx, char* buf, int cap) {
    int e = XjsPluginApiGate(ctx, 0);
    if (e != XJS_PLUGIN_OK) return e;
    std::string j = "{";
    j += "\"双击Ctrl目标\":" + PluginJsonStr(Utf16ToUtf8(g_doubleCtrlTarget.c_str()));
    j += ",\"绘制引擎\":\"" + std::string(g_gfxEngine == 1 ? "gdiplus" : "d2d") + "\"";
    j += "}";
    return PluginBufOut(buf, cap, j);
}

static int ApiGlobalSet(XjsPluginCtx* ctx, const char* membersJsonUtf8) {
    int e;
    if ((e = XjsPluginApiGate(ctx, XPP_SETTINGS)) != XJS_PLUGIN_OK) return e;
    std::vector<XjsJsonMember> ms;
    if (!XjsPluginJsonMembers(membersJsonUtf8, &ms) || ms.empty()) return XJS_PLUGIN_ERR_ARG;
    for (auto& m : ms) {
        std::wstring s;
        if (m.key == L"双击Ctrl目标") {
            if (!MemberStr(m, &s)) return XJS_PLUGIN_ERR_ARG;
            if (!s.empty() && s != XJS_MAIN_WIN_NAME) {   /* 非空必须命中现存档案名 */
                bool hit = false;
                for (int i = 0; i < XjsUiProfileCount(); i++) {
                    XjsUiProfile* p = XjsUiProfileAt(i);
                    if (p && p->name == s) { hit = true; break; }
                }
                if (!hit) return XJS_PLUGIN_ERR_NOTFOUND;
            }
        } else if (m.key == L"绘制引擎") {
            if (!MemberStr(m, &s) || (s != L"d2d" && s != L"gdiplus")) return XJS_PLUGIN_ERR_ARG;
        } else return XJS_PLUGIN_ERR_ARG;
    }
    for (auto& m : ms) {
        std::wstring s;
        MemberStr(m, &s);
        if (m.key == L"双击Ctrl目标") {
            g_doubleCtrlTarget = s;
            XjsDoubleCtrlApply();   /* 按目标有效性实时装卸钩子 (设置页下拉同口径) */
            XjsSaveConfig();
        } else if (m.key == L"绘制引擎") {
            g_gfxEngine = (s == L"gdiplus") ? 1 : 0;   /* 重启生效 */
            XjsSaveConfig();
        }
    }
    return XJS_PLUGIN_OK;
}

/* ==================== windows.enum / window.state / window.cmd / window.create ==================== */

static int ApiWindowsEnum(XjsPluginCtx* ctx, char* buf, int cap) {
    int e = XjsPluginApiGate(ctx, 0);
    if (e != XJS_PLUGIN_OK) return e;
    std::string j = "[";
    for (int i = 0; i < XjsSearchWindow::Count(); i++) {
        XjsSearchWindow* w = XjsSearchWindow::At(i);
        if (!w) continue;
        if (j.size() > 1) j += ",";
        j += "{\"令牌\":" + JNum((long long)XjsPluginApiTokenOf(w));
        j += ",\"名称\":" + PluginJsonStr(Utf16ToUtf8(w->name.c_str()));
        j += ",\"主窗\":" + JBool(w->isMain);
        j += ",\"档案槽\":" + JNum(w->uiIndex);
        j += "}";
    }
    j += "]";
    return PluginBufOut(buf, cap, j);
}

static int ApiWindowState(XjsPluginCtx* ctx, XjsWindowToken window, char* buf, int cap) {
    int e;
    XjsSearchWindow* w = XjsPluginApiWindow(ctx, window, &e);
    if (!w) return e;
    XjsWindowScope scope(w);
    std::string j = "{";
    j += "\"令牌\":" + JNum((long long)XjsPluginApiTokenOf(w));
    j += ",\"名称\":" + PluginJsonStr(Utf16ToUtf8(w->name.c_str()));
    j += ",\"主窗\":" + JBool(w->isMain);
    j += ",\"档案槽\":" + JNum(w->uiIndex);
    j += ",\"视图\":\"" + ViewNameUtf8((int)w->viewMode) + "\"";
    j += ",\"页面缩放\":" + JNum(w->uiZoom * 10);
    j += ",\"皮肤\":" + PluginJsonStr(Utf16ToUtf8(w->skinName.c_str()));
    j += ",\"预览\":" + JBool(w->previewVisible);
    j += ",\"预览宽度\":" + JNum(w->previewWidth);
    j += ",\"置顶\":" + JBool(w->topmost);
    j += ",\"搜索模式\":\"" + KeyModeNameUtf8(w->mode) + "\"";
    j += ",\"搜索词\":" + PluginJsonStr(Utf16ToUtf8(XjsSearchGetText().c_str()));
    j += ",\"结果数\":" + JNum(w->resultCount);
    j += ",\"选中数\":" + JNum(g_result ? xjs_result_GetSelectedCount(g_result) : 0);
    j += ",\"可见\":" + JBool(IsWindowVisible(w->hWnd) != FALSE);
    j += ",\"最小化\":" + JBool(IsIconic(w->hWnd) != FALSE);
    j += ",\"最大化\":" + JBool(IsZoomed(w->hWnd) != FALSE);
    RECT rc;
    if (GetWindowRect(w->hWnd, &rc))
        j += ",\"窗口矩形\":[" + JNum(rc.left) + "," + JNum(rc.top) + "," + JNum(rc.right) + "," + JNum(rc.bottom) + "]";
    j += "}";
    return PluginBufOut(buf, cap, j);
}

/* 界面动作: "show"=唤起到前台 / "dismiss"=窗口消失统一策略 (主窗藏托盘, 子窗真销毁) /
   "openSettings"=打开设置窗并绑定本窗口 */
static int ApiWindowCmd(XjsPluginCtx* ctx, XjsWindowToken window, const char* cmdUtf8) {
    int e;
    if ((e = XjsPluginApiGate(ctx, XPP_UI)) != XJS_PLUGIN_OK) return e;
    if (!cmdUtf8 || !*cmdUtf8) return XJS_PLUGIN_ERR_ARG;
    XjsSearchWindow* w = XjsPluginApiWindow(ctx, window, &e);
    if (!w) return e;
    std::string cmd = cmdUtf8;
    if (cmd == "show") {
        XjsSummonActivate(w->hWnd);
        return XJS_PLUGIN_OK;
    }
    if (cmd == "dismiss") {
        XjsDismissWindow(w->hWnd);   /* 统一策略入口 (主窗藏托盘/子窗销毁), 禁 SW_HIDE 直调 */
        return XJS_PLUGIN_OK;
    }
    if (cmd == "openSettings") {
        XjsWindowScope scope(w);
        XjsSettingsShow();           /* owner = Cur(), 作用域内即本窗口 */
        return XJS_PLUGIN_OK;
    }
    return XJS_PLUGIN_ERR_ARG;
}

/* 按档案建窗 (profileNameUtf8 = 档案名, NULL/空 = 新建空白档案; 遍历扫描中拒绝)。
   档案已有存活窗口 = 幂等激活并回传其令牌 (启动器点击同口径)。inheritFrom 的尺寸被新窗继承。 */
static int ApiWindowCreate(XjsPluginCtx* ctx, XjsWindowToken inheritFrom,
                           const char* profileNameUtf8, XjsWindowToken* tokenOut) {
    int e;
    if ((e = XjsPluginApiGate(ctx, XPP_UI)) != XJS_PLUGIN_OK) return e;
    if (g_isScanning.load()) return XJS_PLUGIN_ERR_STATE;   /* 遍历中禁止创建新窗口 */
    int slot = -1;
    if (profileNameUtf8 && *profileNameUtf8) {
        std::wstring want = Utf8ToUtf16(profileNameUtf8);
        for (int i = 0; i < XjsUiProfileCount(); i++) {
            XjsUiProfile* p = XjsUiProfileAt(i);
            if (p && p->name == want) { slot = i; break; }
        }
        if (slot < 0) return XJS_PLUGIN_ERR_NOTFOUND;
        if (XjsSearchWindow* alive = XjsSearchWindow::AtSlot(slot)) {   /* 已打开 = 激活 (启动器口径) */
            XjsSummonActivate(alive->hWnd);
            if (tokenOut) *tokenOut = XjsPluginApiTokenOf(alive);
            return XJS_PLUGIN_OK;
        }
    }
    XjsSearchWindow* from = XjsPluginApiWindow(ctx, inheritFrom, &e);   /* 尺寸继承发起窗 (0 = 主窗) */
    if (!from) return e;
    {
        XjsWindowScope scope(from);
        XjsSearchWindow::OpenNew(slot);
    }
    int wantSlot = (slot >= 0) ? slot : XjsUiProfileCount() - 1;   /* 新空白档案 = 追加的末槽 */
    XjsSearchWindow* nw = XjsSearchWindow::AtSlot(wantSlot);
    if (!nw) return XJS_PLUGIN_ERR_STATE;
    if (tokenOut) *tokenOut = XjsPluginApiTokenOf(nw);
    return XJS_PLUGIN_OK;
}

/* ==================== modes.list / modes.add / modes.remove / modes.apply ==================== */

/* 该窗口可见的全部模式 = 用户自定义 (按作用范围过滤) + 插件模式 (清单 + 运行时, 统一视图)。
   标识 = 模式来源 id (用户模式原样 id; 插件模式 "p:<插件id>:<序>"), modes.apply 就吃它。 */
static int ApiModesList(XjsPluginCtx* ctx, XjsWindowToken window, char* buf, int cap) {
    int e;
    XjsSearchWindow* w = XjsPluginApiWindow(ctx, window, &e);
    if (!w) return e;
    XjsWindowScope scope(w);
    std::string j = "[";
    for (auto& cm : g_customModes) {
        if (!XjsCmApplies(cm, w->name)) continue;
        if (j.size() > 1) j += ",";
        j += "{\"标识\":" + PluginJsonStr(Utf16ToUtf8(cm.id.c_str()));
        j += ",\"名称\":" + PluginJsonStr(Utf16ToUtf8(cm.name.c_str()));
        j += ",\"简介\":" + PluginJsonStr(Utf16ToUtf8(cm.desc.c_str()));
        j += ",\"类型\":" + PluginJsonStr(Utf16ToUtf8(cm.type.c_str()));
        j += ",\"模板\":" + PluginJsonStr(Utf16ToUtf8(cm.tpl.c_str()));
        j += ",\"来源\":\"用户\"}";
    }
    int total = XjsPluginModeCount();
    for (int i = 0; i < total; i++) {
        XjsPluginModeRef r;
        if (!XjsPluginModeAt(i, &r)) break;
        const XjsPluginModeDef* d = XjsPluginModeDefAt(r);
        if (!d) continue;
        XjsPluginBrief b;
        if (!XjsPluginBriefAt(r.plugin, &b)) continue;
        std::wstring srcId = XjsPluginModeSrcId(b.id, r.modeIdx);
        if (j.size() > 1) j += ",";
        j += "{\"标识\":" + PluginJsonStr(Utf16ToUtf8(srcId.c_str()));
        j += ",\"名称\":" + PluginJsonStr(Utf16ToUtf8(d->name.c_str()));
        j += ",\"简介\":" + PluginJsonStr(Utf16ToUtf8(d->desc.c_str()));
        j += ",\"类型\":" + PluginJsonStr(Utf16ToUtf8(d->type.c_str()));
        j += ",\"模板\":" + PluginJsonStr(d->tplUtf8);
        j += ",\"来源\":" + PluginJsonStr("插件:" + Utf16ToUtf8(b.id.c_str()));
        j += "}";
    }
    j += "]";
    return PluginBufOut(buf, cap, j);
}

/* 运行时添加模板型模式 (会话级; 与清单模式同一合并视图, 药丸菜单/托管标签链即时可见)。
   defJson = {"名称":"..(必填)","简介":"..","类型":"wildcard|regex|sql|lua","模板":"..(必填,<keyword> 占位)"}
   回传 {"标识":"p:<插件id>:<序>"} — modes.remove / modes.appy 就吃这个 id。 */
static int ApiModesAdd(XjsPluginCtx* ctx, XjsWindowToken window, const char* defJsonUtf8,
                       char* buf, int cap) {
    int e = XjsPluginApiGate(ctx, XPP_SETTINGS, XPC_SEARCHMODES);   /* 权限 + 声明过搜索模式能力 */
    if (e != XJS_PLUGIN_OK) return e;
    (void)window;   /* 运行时模式一律全局生效 (与清单模式同口径), 窗口参数仅预留 */
    std::vector<XjsJsonMember> ms;
    if (!XjsPluginJsonMembers(defJsonUtf8, &ms)) return XJS_PLUGIN_ERR_ARG;
    XjsPluginModeDef d;
    d.type = L"wildcard";
    for (auto& m : ms) {
        std::wstring v;
        if (m.key == L"名称") { if (!MemberStr(m, &v)) return XJS_PLUGIN_ERR_ARG; d.name = v; }
        else if (m.key == L"简介") { if (!MemberStr(m, &v)) return XJS_PLUGIN_ERR_ARG; d.desc = v; }
        else if (m.key == L"类型") {
            if (!MemberStr(m, &v) || KeyModeIndexFromUtf8(v) < 0) return XJS_PLUGIN_ERR_ARG;
            d.type = v;
        } else if (m.key == L"模板") { if (!MemberStr(m, &v)) return XJS_PLUGIN_ERR_ARG; d.tplUtf8 = Utf16ToUtf8(v.c_str()); }
        else return XJS_PLUGIN_ERR_ARG;
    }
    if (d.name.empty() || d.name.size() > 64) return XJS_PLUGIN_ERR_ARG;
    if (d.desc.size() > 256 || d.tplUtf8.empty() || d.tplUtf8.size() > 2048) return XJS_PLUGIN_ERR_ARG;
    std::wstring owner;
    XjsPluginApiPluginId(ctx, &owner);
    if (owner.empty()) return XJS_PLUGIN_ERR_ARG;
    {
        /* 每插件上限 (运行时模式计数按 owner) */
        int mine = 0;
        for (auto& r : s_rtModes) if (r.owner == owner) mine++;
        if (mine >= XJS_RT_PER_PLUGIN_MAX) return XJS_PLUGIN_ERR_STATE;
    }
    XjsRtMode rt;
    rt.owner = owner;
    rt.srcIdx = ++s_rtSeq;
    rt.def = d;
    rt.def.id = L"p:" + owner + L":" + std::to_wstring(rt.srcIdx);
    s_rtModes.push_back(rt);
    return PluginBufOut(buf, cap, "{\"标识\":" + PluginJsonStr(Utf16ToUtf8(rt.def.id.c_str())) + "}");
}

/* 删除运行时模式 (只能删自己加的; 用户自定义/清单模式不归 API 管)。
   标签链里引用它的来源一并剔除并重搜 (XjsHostedPurgeMode, 模式菜单 ✕ 同口径)。 */
static int ApiModesRemove(XjsPluginCtx* ctx, const char* modeIdUtf8) {
    int e;
    if ((e = XjsPluginApiGate(ctx, XPP_SETTINGS)) != XJS_PLUGIN_OK) return e;
    if (!modeIdUtf8 || !*modeIdUtf8) return XJS_PLUGIN_ERR_ARG;
    std::wstring id = Utf8ToUtf16(modeIdUtf8);
    std::wstring owner;
    XjsPluginApiPluginId(ctx, &owner);
    if (owner.empty()) return XJS_PLUGIN_ERR_ARG;
    for (auto it = s_rtModes.begin(); it != s_rtModes.end(); ++it) {
        if (it->def.id == id) {
            if (it->owner != owner) return XJS_PLUGIN_ERR_PERM;   /* 别人的运行时模式不可删 */
            s_rtModes.erase(it);
            XjsHostedPurgeMode(id);
            return XJS_PLUGIN_OK;
        }
    }
    return XJS_PLUGIN_ERR_NOTFOUND;
}

/* 按模式执行搜索 (语义 = 用户在药丸菜单点了这个模式):
   模板型 → 输入词置入搜索框 (execute 语义) 后转托管标签并执行标签链;
   接管型 (插件清单模式, 无模板) → 回调该插件 OnSearchMode, 插件自驱。 */
static int ApiModesApply(XjsPluginCtx* ctx, XjsWindowToken window, const char* modeIdUtf8,
                         const char* inputUtf8) {
    int e;
    if ((e = XjsPluginApiGate(ctx, XPP_UI)) != XJS_PLUGIN_OK) return e;
    if (!modeIdUtf8 || !*modeIdUtf8) return XJS_PLUGIN_ERR_ARG;
    XjsSearchWindow* w = XjsPluginApiWindow(ctx, window, &e);
    if (!w) return e;
    XjsWindowScope scope(w);
    std::wstring srcId = Utf8ToUtf16(modeIdUtf8);
    XjsCustomMode v;
    if (!XjsModeViewById(srcId, &v)) return XJS_PLUGIN_ERR_NOTFOUND;   /* 已删/插件已禁用 */
    if (v.tpl.empty()) {
        if (srcId.rfind(L"p:", 0) != 0) return XJS_PLUGIN_ERR_ARG;   /* 用户模式必有模板 */
        std::wstring input = (inputUtf8 && *inputUtf8) ? Utf8ToUtf16(inputUtf8) : XjsSearchGetText();
        XjsPluginFireSearchModeBySrc(srcId, input);
        return XJS_PLUGIN_OK;
    }
    std::wstring input = (inputUtf8 && *inputUtf8) ? Utf8ToUtf16(inputUtf8) : XjsSearchGetText();
    if (!input.empty() && input != XjsSearchGetText()) XjsSearchSetTextQuiet(input);
    XjsHostedWordAsTag(v.name, false);   /* 转标签并执行链 (框内文字 = 链尾, 药丸菜单同路径) */
    return XJS_PLUGIN_OK;
}

/* ==================== 插件互操作桥梁: plugins.list / plugins.state / msg.send / msg.broadcast ====================
 * 发现 + 消息全部免权限; 消息经宿主中转 (xjs_plugin.cpp 的派发落点), 插件之间不直连 —
 * 启停闸门 / fromId 身份 (不可伪造) / 线程契约宿主统一把守。收信口 = 插件可选导出
 * XjsPlugin_OnPluginMessage; 同伴上线/下线经 XJS_PLUGIN_EVT_PLUGINS 纯信号通知 (重查 list)。 */

static int ApiPluginsList(XjsPluginCtx* ctx, char* buf, int cap) {
    int e = XjsPluginApiGate(ctx, 0);
    if (e != XJS_PLUGIN_OK) return e;
    std::string j = "[";
    for (int i = 0; i < XjsPluginCount(); i++) {
        XjsPluginBrief b;
        if (!XjsPluginBriefAt(i, &b)) continue;
        if (j.size() > 1) j += ",";
        j += "{\"标识\":" + PluginJsonStr(Utf16ToUtf8(b.id.c_str()));
        j += ",\"名称\":" + PluginJsonStr(Utf16ToUtf8(b.name.c_str()));
        j += ",\"版本\":" + PluginJsonStr(Utf16ToUtf8(b.version.c_str()));
        j += ",\"作者\":" + PluginJsonStr(Utf16ToUtf8(b.author.c_str()));
        j += ",\"简介\":" + PluginJsonStr(Utf16ToUtf8(b.description.c_str()));
        j += ",\"类型\":\"" + std::string(b.type == 1 ? "app" : "library") + "\"";
        j += ",\"启用\":" + JBool(b.enabled);
        j += ",\"已加载\":" + JBool(b.loaded);
        j += "}";
    }
    j += "]";
    return PluginBufOut(buf, cap, j);
}

/* id 未扫描到也返回 OK ({"存在":false}) — "有没有"本身是查询结果, 不走异常错误码 */
static int ApiPluginsState(XjsPluginCtx* ctx, const char* idUtf8, char* buf, int cap) {
    int e = XjsPluginApiGate(ctx, 0);
    if (e != XJS_PLUGIN_OK) return e;
    if (!idUtf8 || !*idUtf8) return XJS_PLUGIN_ERR_ARG;
    std::wstring want = Utf8ToUtf16(idUtf8);
    std::string j = "{\"存在\":false}";
    for (int i = 0; i < XjsPluginCount(); i++) {
        XjsPluginBrief b;
        if (!XjsPluginBriefAt(i, &b) || b.id != want) continue;
        j = "{\"存在\":true";
        j += ",\"启用\":" + JBool(b.enabled);
        j += ",\"已加载\":" + JBool(b.loaded);
        j += ",\"名称\":" + PluginJsonStr(Utf16ToUtf8(b.name.c_str()));
        j += ",\"版本\":" + PluginJsonStr(Utf16ToUtf8(b.version.c_str()));
        j += ",\"作者\":" + PluginJsonStr(Utf16ToUtf8(b.author.c_str()));
        j += ",\"简介\":" + PluginJsonStr(Utf16ToUtf8(b.description.c_str()));
        j += ",\"类型\":\"" + std::string(b.type == 1 ? "app" : "library") + "\"}";
        break;
    }
    return PluginBufOut(buf, cap, j);
}

static int ApiMsgSend(XjsPluginCtx* ctx, const char* targetIdUtf8, const char* jsonUtf8, char* buf, int cap) {
    int e;
    if ((e = XjsPluginApiGate(ctx, 0)) != XJS_PLUGIN_OK) return e;   /* 免权限: 启用闸 + UI 线程照查 */
    return XjsPluginApiMsgSend(ctx, targetIdUtf8, jsonUtf8, buf, cap);
}

static int ApiMsgBroadcast(XjsPluginCtx* ctx, const char* jsonUtf8) {
    int e;
    if ((e = XjsPluginApiGate(ctx, 0)) != XJS_PLUGIN_OK) return e;
    return XjsPluginApiMsgBroadcast(ctx, jsonUtf8);
}

/* ==================== skins.list ====================
 * 可用皮肤名清单 (XjsSkinEnumerate 扫描结果原样透出)。换肤本身走 settings.set 的
 * "皮肤" 键 (写窗字段+全局镜像+落盘+插件换肤事件, 设置页同落点); 插件先经这里查
 * 有效名再写, 免得对未知名盲试 (settings.set 对未知名整体拒绝 ERR_NOTFOUND)。 */

static int ApiSkinsList(XjsPluginCtx* ctx, char* buf, int cap) {
    int e = XjsPluginApiGate(ctx, 0);
    if (e != XJS_PLUGIN_OK) return e;
    std::string j = "[";
    for (auto& k : XjsSkinEnumerate()) {
        if (j.size() > 1) j += ",";
        j += PluginJsonStr(Utf16ToUtf8(k.c_str()));
    }
    j += "]";
    return PluginBufOut(buf, cap, j);
}

/* ==================== window.selection ====================
 * 某窗口当前选中集 (免权限读面)。FileId 直出 (引擎为事实源, 照 OnCommand 的 FileId
 * 口径 — 路径/名称插件自取), maxIds 截断防巨选区 (全选 451 万时全量拼 JSON 必爆缓冲);
 * "选中数" 恒为全量, 调用方按它与 len(文件ID) 自知是否截断。 */
static int ApiWindowSelection(XjsPluginCtx* ctx, XjsWindowToken window, int maxIds, char* buf, int cap) {
    int e;
    XjsSearchWindow* w = XjsPluginApiWindow(ctx, window, &e);
    if (!w) return e;
    XjsWindowScope scope(w);
    std::vector<int> ids;
    int total = g_result ? xjs_result_GetSelectedCount(g_result) : 0;
    if (g_result && (maxIds <= 0 || maxIds > total)) {
        if (maxIds <= 0) maxIds = total;
        ids.reserve((size_t)maxIds);
        int n = xjs_result_GetCount(g_result);
        for (int i = 0; i < n && (int)ids.size() < maxIds; i++) {
            if (!xjs_result_IsSelectedByIndex(g_result, i)) continue;
            int fid = xjs_result_GetFileId(g_result, i);
            if (fid >= 0) ids.push_back(fid);
        }
    }
    std::string j = "{";
    j += "\"窗口名称\":" + PluginJsonStr(Utf16ToUtf8(w->name.c_str()));
    j += ",\"选中数\":" + JNum(total);
    j += ",\"文件ID\":[";
    for (size_t i = 0; i < ids.size(); i++) {
        if (i) j += ",";
        j += JNum(ids[i]);
    }
    j += "]}";
    return PluginBufOut(buf, cap, j);
}

/* ==================== langs.list ====================
 * 可用界面语言清单 (免权限读面; 名称恒母语)。与 skins.list 同分工: 这里查有效代码,
 * 读写走 settings.get / settings.set 的 "语言" 键 ("auto"=跟随系统, 恒合法不在此列)。 */
static int ApiLangsList(XjsPluginCtx* ctx, char* buf, int cap) {
    int e = XjsPluginApiGate(ctx, 0);
    if (e != XJS_PLUGIN_OK) return e;
    std::string j = "[";
    for (int l = XLANG_ZH; l < XLANG_N; l++) {
        if (j.size() > 1) j += ",";
        j += "{\"代码\":\"" + std::string(XjsLangCodeUtf8(l)) + "\"";
        j += ",\"名称\":" + PluginJsonStr(Utf16ToUtf8(XjsLangLabel(l))) + "}";
    }
    j += "]";
    return PluginBufOut(buf, cap, j);
}

/* ==================== window.result ====================
 * 某窗口的结果对象裸指针 (照 OnEvent 的 result 口径: 直连引擎的插件自己调引擎,
 * 典型 = 在私有结果 COMPLETE 事件回调里 xjs_result_ResetFileId 进窗口列表)。
 * 只发指针不做包装 — 行缓存/重绘链是引擎结果对象自身的变化事件链, 宿主不代劳。 */
static xjs_result* ApiWindowResult(XjsPluginCtx* ctx, XjsWindowToken window) {
    int e;
    if (XjsPluginApiGate(ctx, XPP_UI) != XJS_PLUGIN_OK) return NULL;
    XjsSearchWindow* w = XjsPluginApiWindow(ctx, window, &e);
    if (!w) return NULL;
    XjsWindowScope scope(w);
    return g_result;
}

/* ==================== 名称解析器 (宿主表尾 QueryApi 的落点) ==================== */

void* XJS_PLUGIN_CALL XjsPluginApiQuery(XjsPluginCtx* ctx, const char* name) {
    if (!ctx || !name) return NULL;   /* ctx 有效性由各 API 入口闸门负责, 这里只挡明显乱传 */
    static const struct { const char* name; void* fn; } T[] = {
        { XJS_API_SETTINGS_GET,  (void*)&ApiSettingsGet },
        { XJS_API_SETTINGS_SET,  (void*)&ApiSettingsSet },
        { XJS_API_GLOBAL_GET,    (void*)&ApiGlobalGet },
        { XJS_API_GLOBAL_SET,    (void*)&ApiGlobalSet },
        { XJS_API_WINDOWS_ENUM,  (void*)&ApiWindowsEnum },
        { XJS_API_WINDOW_STATE,  (void*)&ApiWindowState },
        { XJS_API_WINDOW_CMD,    (void*)&ApiWindowCmd },
        { XJS_API_WINDOW_CREATE, (void*)&ApiWindowCreate },
        { XJS_API_MODES_LIST,    (void*)&ApiModesList },
        { XJS_API_MODES_ADD,     (void*)&ApiModesAdd },
        { XJS_API_MODES_REMOVE,  (void*)&ApiModesRemove },
        { XJS_API_MODES_APPLY,   (void*)&ApiModesApply },
        { XJS_API_PLUGINS_LIST,  (void*)&ApiPluginsList },
        { XJS_API_PLUGINS_STATE, (void*)&ApiPluginsState },
        { XJS_API_MSG_SEND,      (void*)&ApiMsgSend },
        { XJS_API_MSG_BROADCAST, (void*)&ApiMsgBroadcast },
        { XJS_API_SKINS_LIST,    (void*)&ApiSkinsList },
        { XJS_API_WINDOW_SELECTION, (void*)&ApiWindowSelection },
        { XJS_API_LANGS_LIST,    (void*)&ApiLangsList },
        { XJS_API_WINDOW_RESULT, (void*)&ApiWindowResult },
    };
    for (auto& t : T)
        if (!strcmp(t.name, name)) return t.fn;
    return NULL;   /* 未知名 = 旧宿主/新 API, 插件干净降级 */
}
