/*
 * xjs_settings.cpp — 设置窗口: 独立顶层窗口, 照正式版 settings.html 同构
 * 左侧两层分类导航 + 右侧分组卡片 (名称/描述 + 右侧开关/选项/按钮), 内容超屏滚轮滚动
 * 自带 DPI 尺度 (每显示器 V2, 96=100%), 换肤经 g_skinEpoch 重建画刷;
 * 命令类控件松开才触发 (按下只记待定 pressAct/pressSide/pressDlg, 拖离=取消)
 */
#include "xjs_app.h"
#include <ntsecapi.h>   /* LSA 账户权限 (内存页锁定: SeLockMemoryPrivilege 授予/移除), advapi32 */
#include <memory>       /* 表格编辑字段池 (unique_ptr 保指针稳定: 路由登记表存裸指针) */
#include <cwctype>

/* ==================== 行/卡片模型 ==================== */

enum XjsSetCtrl { CT_INFO = 0, CT_SWITCH, CT_OPTION, CT_BUTTON, CT_PILL, CT_IMAGE, CT_INPUT, CT_TROW, CT_MDDOC };
enum XjsSetAct {
    ACT_NONE = 0, ACT_PREVIEW, ACT_AUTOSTART, ACT_REBUILD, ACT_GITHUB, ACT_SITE, ACT_DONORS, ACT_GLM,
    ACT_COPYVER = 8,  /* 关于页: 复制版本信息 (排查问题时直接粘给对方) */
    ACT_VIEW = 200,   /* +0..3 视图模式 */
    ACT_SKIN = 100,   /* +皮肤索引 (g_skinMenuNames) */
    ACT_MATCH = 300,  /* +0..7 搜索匹配开关 (下发 SetSearchSettings) */
    ACT_EXCL_ADD = 310,
    ACT_DCCTRL = 311,
    ACT_ENGINE = 321,      /* 绘制引擎 D2D/GDI+ (进程共享, 重启生效; 菜单回传 901/902) */
    ACT_LANG = 322,        /* 界面语言 (每窗 = owner, 即时生效; 菜单回传 919=自动 920..925=zh/zh-TW/en/ko/th/ms) */
    ACT_DRIVEPROG = 312,   /* 绘制驱动器占用进度条 (每窗) */
    ACT_ROWHOVER = 313,    /* 高亮鼠标经过行 (每窗) */
    ACT_HOVERFADE = 314,   /* 鼠标经过残影 (每窗, 依赖 ACT_ROWHOVER) */
    ACT_WINNAME = 315,     /* 重命名窗口 (每窗, 主窗固定名不可改) */
    /* 窗口管理: 删除窗口档案 (1900+档案槽, 已打开则连窗销毁+卸热键)。
       基址曾在 316: 段宽 64 横穿 320..379 上 13 个动作常量, 显式 case 优先于范围判定 —
       删第 5 档案 (槽4) 实际执行 ACT_CLEARHIST 清空搜索历史, 属数据丢失级撞号, 已挪到最高空闲段 */
    ACT_WINGM_DEL = 1900,
    ACT_WINGM_REN = 1440,  /* 窗口管理: 重命名窗口档案 (1440+档案槽, 上限 59 — 避开 EXCL_DEL 400..1423
                              与 OPEN 1500; 作用对象 = 档案槽: 存活窗改窗口名, 未打开档案改档案名,
                              与 窗口名称行 同一入口 XjsSetApplyWindowRename) */
    ACT_CLEARHIST = 320,
    ACT_HOTKEY = 330,    /* 全局快捷键: 点击进入录制, 按下新组合键生效 (Delete 清除取消注册, Esc 取消) */
    /* 窗口行为 (每窗, 2026-09-17): 走自绘菜单选择的项 = id 基址+选项下标, 其余为开关 */
    ACT_BLUR = 331,        /* +0 无 / +1 关闭窗口 (菜单回传) */
    ACT_CTRLBTN = 337,     /* 显示控制按钮 (最小化/最大化/关闭; 隐藏后左侧填充) */
    ACT_FILTERBOX = 338,   /* 显示筛选框 (隐藏后搜索框填充) */
    ACT_STATUSBAR = 339,   /* 显示状态栏 */
    ACT_TASKBAR = 340,     /* 任务栏显示图标 (关 = WS_EX_TOOLWINDOW) */
    ACT_ZOOM = 341,        /* +0..15 页面缩放 (每窗: 50%..200% 步进 10%, 菜单回传) */
    ACT_MOUSEOPEN = 357,   /* +0 双击打开 / +1 单击打开 (每窗, 菜单回传) */
    ACT_CREATEFILL = 359,  /* +0 清空 / +1 用户指定关键词 / +2 上一次输入的搜索词 (每窗, 菜单回传) */
    ACT_CREATEKW = 362,    /* 创建关键词: 点击弹输入对话框 (每窗) */
    ACT_SORT = 363,        /* +0..6 默认排序预设 (每窗, 菜单回传) */
    ACT_DEFSEL = 370,      /* +0 默认不选 / +1 第一个表项 (每窗, 菜单回传) */
    ACT_APPEAR = 380,      /* +档位序 0..11 出现位置 (序见 xjs_app.h XJS_APPEAR_*; 菜单回传)。
                              基址不能挪回 333: 段宽 12 会盖住 341..356 的 ACT_ZOOM 回传值 (下拉结果
                              先按 ACT_APPEAR 判, 缩放选中会变成改位置) */
    /* 内存/性能分析 (全局, 2026-09-21): 单动作段 392..399 (APPEAR 12 档之后的尾隙) */
    ACT_MEMLOCK = 392,      /* 内存页锁定: LSA 授予/移除本账户 "锁定内存页" 权限 (系统级, 不入配置) */
    ACT_PERF_LSSW = 393,    /* 加载/保存性能统计 开关 (xjs_db_SetPerformanceSwitch) */
    ACT_PERF_LSCLR = 394,   /* 加载/保存统计 清零 */
    ACT_PERF_SCNSW = 395,   /* 遍历性能统计 开关 (xjs_db_SetScanPerformanceSwitch) */
    ACT_PERF_SCNCLR = 396,  /* 遍历统计 清零 */
    ACT_PERF_SYNCLR = 397,  /* 同步统计 清零 (xjs_sync_ClearPerformanceText, 无开关 API) */
    ACT_EXCL_DEL = 400,  /* +排除目录索引 (上限 1024) */
    ACT_OPEN = 1500,     /* +0..2 打开文件行为 (每窗: 继承管理员/异步线程/打开后隐藏) */
    /* 插件管理 (全局, 2026-09-19): 段 1600..1666 上方空闲无邻段 */
    ACT_PLUGINS_OPENDIR = 1600,  /* 打开插件目录 */
    ACT_PLUGINS_RESCAN  = 1601,  /* 重新扫描 plugins\ */
    ACT_PLUGINS_TOGGLE  = 1602,  /* +插件下标 启用/禁用 (上限 200, 同票号上限; 首次启用过确认框) */
    /* 文件分类/别名 表格编辑 (全局, 2026-09-22 抄自正式版设置): 段 2000.. 上方空闲 */
    ACT_TSAVE = 2000,            /* 表格保存 (按当前分类下发 文件分类/别名) */
    ACT_TADD  = 2001,            /* 表格添加一行 (按当前分类; 分类行自动预填下一个空闲类型号) */
    ACT_FDEL  = 2002,            /* +行下标 删除文件分类行 (上限 256 行) */
    ACT_ADEL  = 2302             /* +行下标 删除别名行 (上限 1024 行) */
};
static_assert(ACT_APPEAR + XJS_APPEAR_COUNT <= ACT_EXCL_DEL, "ACT_APPEAR 档位段越界, 与 ACT_EXCL_DEL 重叠");
static_assert(ACT_APPEAR + XJS_APPEAR_COUNT <= ACT_MEMLOCK, "出现位置档位段越界, 与内存/性能动作段重叠");
static_assert(ACT_PERF_SYNCLR + 1 <= ACT_EXCL_DEL, "内存/性能动作段越界, 与 ACT_EXCL_DEL 重叠");
/* 段位互斥锁死 (枚举值即运行时命令 id, 曾发生 ACT_WINGM_DEL 段横穿后加常量 = 点删除执行别的动作):
   任何新动作段必须落在已锁段之外并在此补断言 */
static_assert(ACT_PLUGINS_TOGGLE + 200 <= ACT_WINGM_DEL, "插件段与窗口管理删除段重叠");
static_assert(ACT_WINGM_DEL + 64 <= ACT_TSAVE, "窗口管理删除段与 文件分类/别名 表格段重叠");
static_assert(ACT_FDEL + 256 <= ACT_ADEL, "文件分类删除段与别名删除段重叠");
static_assert(ACT_ADEL + 1024 <= 4096, "别名删除段越出保留区");
static_assert(ACT_EXCL_DEL + 1024 <= ACT_WINGM_REN, "排除目录删除段与重命名段重叠");
static_assert(ACT_WINGM_REN + 60 <= ACT_OPEN, "窗口重命名段 (上限 59 档案) 与打开行为段重叠");

/* 出现位置档位文字 (设置行当前值 + 下拉菜单同源; 档位序 = xjs_app.h XJS_APPEAR_*)。
   合成串走函数内 static 缓冲: 两处调用都即时拷进 std::wstring, 不跨调用持有 */
static const wchar_t* XjsAppearPosLabel(int pos) {
    const wchar_t* const SPOT_T[XJS_SPOT_COUNT] = { XjsT(L"位置.居中"), XjsT(L"位置.左上角"), XjsT(L"位置.右上角"), XjsT(L"位置.左下角"), XjsT(L"位置.右下角") };
    const wchar_t* const SCREEN_T[XJS_SCR_COUNT] = { XjsT(L"位置.主屏"), XjsT(L"位置.鼠标所在屏幕") };
    if (pos == XJS_APPEAR_FOLLOW) return XjsT(L"位置.跟随鼠标");
    if (pos >= XJS_APPEAR_SCREEN_BASE && pos < XJS_APPEAR_COUNT) {
        static std::wstring buf;
        const int scr = (pos - XJS_APPEAR_SCREEN_BASE) / XJS_SPOT_COUNT;
        const int spot = (pos - XJS_APPEAR_SCREEN_BASE) % XJS_SPOT_COUNT;
        buf.assign(1, L'(');
        buf += SCREEN_T[scr];
        buf += L") ";
        buf += SPOT_T[spot];
        return buf.c_str();
    }
    return XjsT(L"位置.之前的位置");   /* XJS_APPEAR_NONE + 兜底 */
}

/* 本窗输入对话框的 resultId (WM_INPUT_DONE wParam; 按 owner 路由, 不与主窗别名通道 IDM_CTX_BASE+30 冲突) */
static const int XJS_SET_INPUT_RENAME = 1;
static const int XJS_SET_INPUT_KW = 2;   /* 创建关键词 (创建-用户指定关键词) */

/* 默认排序预设 (每窗, 列表框分类): (label, 结果对象排序字段 utf8, 升序?)
   label 存点分 i18n 主键 (文件级 static 禁调 XjsT), 读取点 XjsT(label) 取文案 */
static const struct { const wchar_t* label; const char* field; bool asc; } SORT_PRESETS[7] = {
    { L"排序预设.评分从高到低", "文件评分", false },
    { L"排序预设.文件名AZ",        "文件名",   true  },
    { L"排序预设.文件名ZA",        "文件名",   false },
    { L"排序预设.修改时间新旧",    "修改时间", false },
    { L"排序预设.修改时间旧新",    "修改时间", true  },
    { L"排序预设.大小大小",        "文件大小", false },
    { L"排序预设.大小小大",        "文件大小", true  },
};

struct XjsSetRow {
    int act = ACT_NONE;
    XjsSetCtrl ctrl = CT_INFO;
    std::wstring name, desc, value;   /* value: 按钮文字 / 信息胶囊文字 */
    bool checked = false;
    bool danger = false;              /* 危险按钮样式 (红描边, 同源样式 .btn-danger) */
    bool disabled = false;            /* 置灰不可点 (依赖项未开启, 如"残影"依赖"高亮鼠标经过行") */
    int img = 0;                      /* CT_IMAGE: 1=微信 2=支付宝 3=双栏并排 (捐赠二维码) */
    int trowIdx = -1;                 /* CT_TROW: 本行对应表格数据行下标 (字段池 s_filterEds/s_aliasEds 按它取格) */
    int tblCols = 0;                  /* CT_TROW: 列数 (3=文件分类 名称/类型/后缀, 2=别名 路径/别名); 列几何同源 XjsSetTblColRect */
    bool centerText = false;          /* 引导语等: 名称/描述水平居中 (用 tfNameC/tfDescC) */
    bool link = false;                /* 链接行: 名称 accent 色 + 手型光标, 整行点击触发 act (关于页 GLM 官网) */
    bool block = false;               /* 长文本块行: 名称固定在行顶, 说明占其余整块 (性能统计多行明细) —
                                         默认的 名称/说明 上下两半垂直居中排布会把多行文本挤出行框 */
    bool warnText = false;            /* 说明文字用警告色 (内存页锁定"需注销生效/授权已移除"状态行) */
    XjsMdDoc* md = NULL;              /* CT_MDDOC: 本行承载的 md 文档实例 (xjs_md.cpp) */
    float ctrlW = 0;                  /* 行尾控件宽 (构建时按 value 实测; 绘制/命中/文本右缘三处同源) */
    /* 第二行尾按钮 (act2 非空才有; 目前唯一用户 = 窗口管理行 "重命名"+"删除" 两钮)。
       右锚定次序: value 贴右缘, value2 在其左 SS(10); 宽度/命中/绘制同 ctrlW 口径 */
    int act2 = ACT_NONE;
    std::wstring value2;
    float ctrlW2 = 0;
    bool danger2 = false;
    float y = 0, h = 0;               /* 内容坐标 (h 含说明文字换行实测, 不固定) */
};

struct XjsSetCard {
    std::wstring title;
    std::vector<XjsSetRow> rows;
    float y = 0, h = 0;
};

/* 分类下标 (行模型 switch 与 SET_TREE 共用; 重排只动这两处) */
enum { SC_GENERAL = 0,  /* 通用 = 全局设置 (缩放/双击Ctrl/自启) */
       SC_WIN,          /* ─ 窗口设置·窗口 (快捷键/名称/失焦动作/激活位置/标题栏与状态栏可见性, 每窗) */
       SC_APPEAR,       /* ─ 窗口设置·外观 (皮肤, 每窗) */
       SC_LIST,         /* ─ 窗口设置·列表框 (每窗) */
       SC_MATCH,        /* ─ 窗口设置·搜索匹配 (每窗, 2026-09-17 起移出通用) */
       SC_OPEN,         /* ─ 窗口设置·打开 (每窗) */
       SC_CREATE,       /* ─ 窗口设置·创建 (每窗: 新窗口的初始搜索词/关键词) */
       SC_MANAGE,       /* 窗口管理 (全局: 档案列表, 可删除) */
       SC_DATA,         /* 数据维护 (全局) */
       SC_FILTER,       /* 文件分类 (全局: 引擎筛选器表格编辑, 2026-09-22 抄自正式版) */
       SC_ALIAS,        /* 别名 (全局: 引擎路径别名表格编辑, 2026-09-22 抄自正式版) */
       SC_MEMORY,       /* 内存 (全局: 大页内存权限 + 引擎索引内存占用, 2026-09-21) */
       SC_PERF,         /* 性能分析 (全局: 引擎队列实时状态 + 各阶段性能统计采样, 2026-09-21) */
       SC_PLUGINS,      /* 插件 (全局: 原生插件管理, 2026-09-19) */
       SC_DONATE, SC_MODES, SC_LICENSE, SC_ABOUT, SC_N };

/* 左侧分类树 (两层): "窗口设置"分组节点 (grp, 可折叠, 不可选中) 下的子级缩进一级 ——
   全局设置与每窗设置一眼可分 (子级全部作用到 owner 窗口)。
   name 存点分 i18n 主键 (文件级 static 禁调 XjsT), 读取点 XjsT(nd.name) 取文案 */
struct XjsSetCatNode { const wchar_t* name; int cat; bool grp; int lvl; };
static const XjsSetCatNode SET_TREE[] = {
    { L"设置分类.通用",     SC_GENERAL, false, 0 },
    { L"设置分类.窗口设置", -1,         true,  0 },
    { L"设置分类.窗口",     SC_WIN,     false, 1 },
    { L"设置分类.外观",     SC_APPEAR,  false, 1 },
    { L"设置分类.列表",     SC_LIST,    false, 1 },
    { L"设置分类.搜索匹配", SC_MATCH,   false, 1 },
    { L"设置分类.打开",     SC_OPEN,    false, 1 },
    { L"设置分类.新建窗口", SC_CREATE,  false, 1 },
    { L"设置分类.窗口管理", SC_MANAGE,  false, 0 },
    { L"设置分类.数据维护", SC_DATA,    false, 0 },
    { L"设置分类.文件分类", SC_FILTER,  false, 0 },
    { L"设置分类.别名",     SC_ALIAS,   false, 0 },
    { L"设置分类.内存",     SC_MEMORY,  false, 0 },
    { L"设置分类.性能分析", SC_PERF,    false, 0 },
    { L"设置分类.插件",     SC_PLUGINS, false, 0 },
    { L"设置分类.捐赠",     SC_DONATE,  false, 0 },
    { L"设置分类.搜索模式", SC_MODES,   false, 0 },
    { L"设置分类.开源许可", SC_LICENSE, false, 0 },
    { L"设置分类.关于",     SC_ABOUT,   false, 0 },
};
static const int SET_TREE_N = (int)(sizeof(SET_TREE) / sizeof(SET_TREE[0]));

/* 重建对话框: 7 个可选字段 (源样式 rebuild-opts 同序同名), 默认勾 评分/大小/修改/别名。
   这里存中文原文键 (文件级 static 禁调 XjsT — 跨编译单元初始化顺序未定), 读取点 XjsT() 取译文 */
static const wchar_t* const RB_FIELDS[7] = { L"重建字段.评分", L"重建字段.文件大小", L"重建字段.修改时间",
                                             L"重建字段.创建时间", L"重建字段.访问时间", L"重建字段.文件属性", L"重建字段.别名" };
/* 对话框命中区 id */
enum { DHB_NONE = 0, DHB_MASK, DHB_CANCEL, DHB_OK, DHB_FIELD = 100, DHB_DRIVE = 200 };

struct XjsSettingsState {
    HWND hwnd = NULL;
    XjsHwndRt* rt = NULL;
    XjsSolidBrush *brBg = NULL, *brBorder = NULL, *brHover = NULL, *brText = NULL,
        *brDim = NULL, *brFaint = NULL, *brAccent = NULL, *brPanel = NULL, *brPanel2 = NULL,
        *brBorderStrong = NULL, *brDanger = NULL, *brWhite = NULL, *brAccentSoft = NULL,
        *brMask = NULL, *brOkFill = NULL, *brOkHover = NULL, *brWarn = NULL;
    XjsGradBrush* brBgGrad = NULL;   /* 窗口底 bg1→bg2 纵渐变 (同源样式 body) */
    XjsFormat *tfName = NULL, *tfDesc = NULL, *tfTitle = NULL, *tfCat = NULL, *tfBtn = NULL,
        *tfNameC = NULL, *tfDescC = NULL;   /* C 变体 = 水平居中 (捐赠页引导语/二维码标题) */
    int brushEpoch = -1;
    float unit = 0;              /* 资源创建时的 SS 单位 (缩放/DPI 变了须重建文本格式) */
    float scale = 1.0f;          /* 本窗口显示器 DPI 尺度 (与主窗 g_s 独立) */
    std::vector<XjsSetCard> cards;
    float contentH = 0;
    float scroll = 0;
    bool sbDrag = false;         /* 右缘滚动条拖拽中 (几何与渲染同源 XjsSetSbGeom) */
    float sbGrabOff = 0;         /* 按下点相对 thumb 顶缘的偏移 (拖拽保持相对位移) */
    float sideScroll = 0;        /* 左侧分类栏滚动 (分类数多且窗口矮时内容超高; 滚动条按需显示) */
    bool sbSideDrag = false;     /* 分类栏滚动条拖拽中 (几何与渲染同源 XjsSetSideSbGeom) */
    float sbSideGrabOff = 0;     /* 分类栏 thumb 按下点相对顶缘偏移 */
    int sbHover = 0;             /* 悬停的滚动条 (0=无 1=右缘内容条 2=分类栏条; thumb 提亮一档) */
    int cat = 0;                 /* 当前分类 (SC_* 常量) */
    std::wstring liveSig;        /* 内存/性能分析页 1s 采样签名 (变了才置脏重建, 见 XjsSetLiveSignature) */
    bool winTreeOpen = true;     /* 左侧"窗口设置"分组展开态 */
    int hoverRow = -1, hoverSide = -1;   /* hoverRow = 卡序×1000+卡内行序; hoverSide = 侧边栏可见条目下标 */
    bool trackingLeave = false;
    bool rowsDirty = true;       /* 行模型脏标记 (避免每次绘制扫盘枚举皮肤) */
    bool recHotkey = false;      /* 热键录制中: 下个非修饰键组合即新热键 (Esc 取消, Delete 清除) */
    /* 输入字段组件 (排除目录手动输入): 组件自带 键盘/IME/I-beam/光标闪烁 路由, 宿主只接线 */
    XjsEditField pathEd;
    /* 捐赠二维码 (RCDATA 编进 EXE, 解码到本窗口 RT; 换肤/RT 重建后随之重建) */
    XjsBitmap* imgWechat = NULL;
    XjsBitmap* imgAlipay = NULL;
    bool imgTried = false;
    /* 重建确认对话框 (遮罩层, 源样式 rebuildMask 同构) */
    bool dlgOpen = false;
    bool dlgField[7] = { true, true, true, false, false, false, true };
    struct DlgDrive { wchar_t letter = 0; std::wstring label; bool checked = true; };
    std::vector<DlgDrive> dlgDrives;
    int dlgHover = -1;
    std::wstring dlgErr;
    XjsRect dlgRect = {};
    struct DlgHit { XjsRect rc; int id; float rad = 0; };   /* rad>0 = 按圆角形状判定 (角部圆外不可点) */
    std::vector<DlgHit> dlgHits;   /* 绘制时填充 */
    /* 命令类按下待定 (松开触发口径, 2026-09-17): 行/侧边分类/对话框控件 按下只记,
       松开仍命中同一目标才执行; 文本字段点定位与滚动条拖拽保持按下语义 */
    int pressAct = 0;      /* 行待定 (行 act 值, 0=无) */
    int pressSide = -1;    /* 侧边待定 (SET_TREE 下标) */
    int pressDlg = 0;      /* 对话框控件待定 (DHB id) */
};
static XjsSettingsState s_set;

/* ==================== 文件分类 / 别名 表格编辑 (2026-09-22 抄自正式版设置) ====================
 * 正式版 = 行内输入框表格 + [保存][添加] 按整体下发引擎; 本地同构: 每数据行一格一输入框
 * (XjsEditField 池, 文本即活数据 — 保存时从池收值校验), 行几何由行模型 CT_TROW 现算。
 * 池必须指针稳定 (路由层登记表存裸指针): unique_ptr, 删行/重载即析构自动摘登记。
 * reload 标记: 打开设置窗/保存成功后置位, 行模型重建时从引擎拉回生效配置 (规范化/占位符展开后回显)。 */
static std::vector<std::unique_ptr<XjsEditField>> s_filterEds;   /* 文件分类: 每行 3 格 名称/类型/后缀 */
static std::vector<std::unique_ptr<XjsEditField>> s_aliasEds;    /* 别名: 每行 2 格 路径/别名 */
static bool s_filterReload = true, s_aliasReload = true;

static int XjsSetFilterRows() { return (int)s_filterEds.size() / 3; }
static int XjsSetAliasRows()  { return (int)s_aliasEds.size() / 2; }

static void XjsSetFilterRowsLoad() {
    s_filterEds.clear();   /* 析构自动摘登记 (路由表/闪烁驱动) */
    std::vector<XjsFilterItem> rows;
    XjsFilterConfigLoad(&rows);
    for (size_t i = 0; i < rows.size() * 3; i++) s_filterEds.push_back(std::make_unique<XjsEditField>());
    for (int r = 0; r < (int)rows.size(); r++) {
        s_filterEds[r * 3 + 0]->ed.text = rows[r].name;
        s_filterEds[r * 3 + 1]->ed.text = std::to_wstring(rows[r].type);
        s_filterEds[r * 3 + 2]->ed.text = rows[r].ext;
    }
}

static void XjsSetAliasRowsLoad() {
    s_aliasEds.clear();
    std::vector<XjsAliasItem> rows;
    XjsAliasConfigLoad(&rows);
    for (size_t i = 0; i < rows.size() * 2; i++) s_aliasEds.push_back(std::make_unique<XjsEditField>());
    for (int r = 0; r < (int)rows.size(); r++) {
        s_aliasEds[r * 2 + 0]->ed.text = rows[r].path;
        s_aliasEds[r * 2 + 1]->ed.text = rows[r].alias;
    }
}

/* 类型文本 → 整数 (纯数字才有效; 空串/带杂字符 = -1, 由保存侧报错) */
static int XjsSetTblTypeParse(const std::wstring& raw) {
    std::wstring t = XjsTrimWs(raw);
    if (t.empty()) return -1;
    int v = 0;
    for (wchar_t c : t) {
        if (c < L'0' || c > L'9') return -1;
        v = v * 10 + (c - L'0');
        if (v > 99999) return -1;   /* 超界即止, 保存侧按范围报错 */
    }
    return v;
}

/* 系统保留分类 (0=全部 255=文件夹): 只填名称, 无后缀 (正式版同款: 输入框禁用态 + "无需后缀") */
static bool XjsSetTblTypeReserved(const std::wstring& raw) {
    int v = XjsSetTblTypeParse(raw);
    return v == 0 || v == 255;
}

/* 后缀归一化 (正式版 normExts 同口径): 中英文逗号/分号/空白分隔 → 大写英文逗号串 "EXE,BAT,MSI" */
static std::wstring XjsSetTblNormExts(const std::wstring& raw) {
    static const wchar_t* const SEP = L",，;； \t\r\n";
    std::wstring out;
    size_t i = 0, n = raw.size();
    while (i < n) {
        while (i < n && wcschr(SEP, raw[i])) i++;
        size_t b = i;
        while (i < n && !wcschr(SEP, raw[i])) i++;
        if (i > b) {
            if (!out.empty()) out += L',';
            for (size_t k = b; k < i; k++) out += (wchar_t)towupper(raw[k]);
        }
    }
    return out;
}

static float SS(float v) { return v * s_set.scale * XjsUiZoom(); }
static float s_rowsUnitBuilt = 0;   /* 上次行模型构建时的 SS 单位 (DPI 尺度×页面缩放), 变了行高全错须重建 */

/* 行尾控件组总宽 (主钮 + 第二钮; 文本右缘让位 = 构建/绘制两处同源) */
static float XjsSetRowCtrlSpan(const XjsSetRow& r) {
    return r.ctrlW + (r.ctrlW2 > 0 ? SS(10) + r.ctrlW2 : 0);
}


/* 侧边栏可见条目 (渲染/悬停/点击三处同源): 窗口分组折叠时跳过其子级 */
struct XjsSetSideItem { int idx; float y; };
static int XjsSetSideItems(XjsSetSideItem* out, int cap) {
    int n = 0;
    float y = SS(14);
    for (int i = 0; i < SET_TREE_N && n < cap; i++) {
        if (SET_TREE[i].lvl > 0 && !s_set.winTreeOpen) continue;
        out[n].idx = i;
        out[n].y = y;
        n++;
        y += SS(34) + SS(2);
    }
    return n;
}

/* 侧边栏宽度自适应 (单一事实源: 渲染/命中/点击/滚动四处同调) =
   最宽可见分类名实测 + 左内边距 + 缩进 + 文本左偏 + 右缘, 夹在 [SS(132), SS(240)]。
   中文 4~5 字分类 = 旧固定宽 132; 英文长分类 (Window management 等) 自适应展宽不截断。
   tfCat 未就绪 (首帧命中先于绘制) 时回旧固定宽 */
static float XjsSetSideW() {
    if (!s_set.tfCat) return SS(132);
    float need = 0;
    XjsSetSideItem side[SET_TREE_N];
    int sn = XjsSetSideItems(side, SET_TREE_N);
    for (int i = 0; i < sn; i++) {
        const XjsSetCatNode& nd = SET_TREE[side[i].idx];
        float ind = nd.lvl ? SS(14) : 0;
        float tw = XjsMeasureText(XjsT(nd.name), s_set.tfCat);
        need = xf_max(need, SS(10) + ind + SS(12) + tw + SS(10));
    }
    return xf_min(SS(240), xf_max(SS(132), need));
}

/* 热键组合 → "Ctrl + Alt + A" 文本 (字母/数字/F键 直显, 其余 GetKeyNameText)
   主窗启动提醒也用 (xjs_app.h 声明), 勿改回 static */
std::wstring XjsHotkeyText(UINT mod, UINT vk) {
    std::wstring s;
    if (mod & MOD_CONTROL) s += L"Ctrl + ";
    if (mod & MOD_ALT)     s += L"Alt + ";
    if (mod & MOD_SHIFT)   s += L"Shift + ";
    if (mod & MOD_WIN)     s += L"Win + ";
    if (vk >= 'A' && vk <= 'Z') { s += (wchar_t)vk; return s; }
    if (vk >= '0' && vk <= '9') { s += (wchar_t)vk; return s; }
    if (vk >= VK_F1 && vk <= VK_F12) { s += L"F" + std::to_wstring(vk - VK_F1 + 1); return s; }
    wchar_t name[32] = L"?";
    LONG sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (sc) {
        LPARAM lp = (LPARAM)1 | ((LPARAM)sc << 16);
        if (!GetKeyNameTextW((LONG)lp, name, 32)) wcscpy_s(name, L"?");
    }
    s += name;
    return s;
}

/* 录制中实时回显当前按住的修饰键组合 (Ctrl/Alt/Shift/Win 可任意叠加 + 一个主键) */
static std::wstring XjsHotkeyLiveText() {
    UINT mod = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) mod |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) & 0x8000) mod |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) & 0x8000) mod |= MOD_SHIFT;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) mod |= MOD_WIN;
    if (!mod) return XjsT(L"设置.窗口.录制占位");
    std::wstring s;
    if (mod & MOD_CONTROL) s += L"Ctrl + ";
    if (mod & MOD_ALT)     s += L"Alt + ";
    if (mod & MOD_SHIFT)   s += L"Shift + ";
    if (mod & MOD_WIN)     s += L"Win + ";
    return s + L"…";
}

/* ==================== 行构建 (内容坐标分配) ==================== */

/* 设置窗绑定"打开它的搜索窗口" (owner): 皮肤/视图/预览等每窗设置都作用到 owner。
   owner 可能已被关闭 → 校验存活, 失效则回落主窗 */
static XjsSearchWindow* s_setOwner = NULL;
static XjsSearchWindow* XjsSetOwner() {
    if (!XjsSearchWindow::Alive(s_setOwner)) s_setOwner = XjsSearchWindow::Main();
    return s_setOwner;
}

/* 内置 md 文档页 (main.rc RCDATA → 通用引擎 xjs_md.cpp): 203=《搜索模式简介》 204=《第三方组件声明》。
   资源块随进程存活, 锁定指针可长期使用; 实例随设置窗销毁释放 (XjsSetFreeMdDocs) */
static const struct { int resId; } MD_PAGES[] = { { 203 }, { 204 } };
static XjsMdDoc* s_mdDoc[(int)(sizeof(MD_PAGES) / sizeof(MD_PAGES[0]))] = {};
static bool s_mdPending[(int)(sizeof(MD_PAGES) / sizeof(MD_PAGES[0]))] = {};
                   /* 行模型构建时 RT 未就绪, 文档行高未知 → 首帧重建行模型 */
static XjsMdDoc* XjsSetMdPageDoc(int pageIdx) {
    if (pageIdx < 0 || pageIdx >= (int)(sizeof(MD_PAGES) / sizeof(MD_PAGES[0]))) return NULL;
    if (!s_mdDoc[pageIdx]) {
        s_mdDoc[pageIdx] = XjsMdCreate();
        HRSRC rs = FindResourceW(NULL, MAKEINTRESOURCEW(MD_PAGES[pageIdx].resId), RT_RCDATA);
        HGLOBAL hg = rs ? LoadResource(NULL, rs) : NULL;
        const char* p = hg ? (const char*)LockResource(hg) : NULL;
        DWORD n = hg ? SizeofResource(NULL, rs) : 0;
        if (p && n) XjsMdSetText(s_mdDoc[pageIdx], p, (unsigned)n);
    }
    return s_mdDoc[pageIdx];
}

/* 切换分类唯一入口 (侧栏点击与"打开设置直达某页"共用): 归零滚动/悬停、行模型重建、
   旧 md 页占位行作废 (防每帧重建)、页上输入字段失焦 (切页后矩形失效, 防隐形输入框)。
   同分类重复调用 = 无操作 (侧栏点击原口径) */
static void XjsSetSwitchCat(HWND hwnd, int cat) {
    if (s_set.cat == cat) return;
    s_set.cat = cat; s_set.scroll = 0; s_set.hoverRow = -1; s_set.rowsDirty = true;
    memset(s_mdPending, 0, sizeof(s_mdPending));
    s_set.pathEd.SetFocused(hwnd, false);
    for (auto& f : s_filterEds) f->SetFocused(hwnd, false);
    for (auto& f : s_aliasEds) f->SetFocused(hwnd, false);
    InvalidateRect(hwnd, NULL, FALSE);
}

/* md 文档分类页: 一张卡片 + 一条通栏文档行 (行高 = 文档排版总高, 行上挂文档实例供绘制取用) */
static void XjsSetAppendMdPage(int pageIdx, const wchar_t* cardTitle, const wchar_t* docName) {
    XjsSetCard c;
    c.title = cardTitle;
    s_set.cards.push_back(c);   /* 卡位由构建尾部统一分配 */
    XjsSetRow r;
    r.ctrl = CT_MDDOC;
    RECT crc;
    GetClientRect(s_set.hwnd, &crc);
    if (s_set.rt && crc.right > 100) {
        s_mdPending[pageIdx] = false;
        float contentW = (float)crc.right - XjsSetSideW() - SS(24) - SS(28) - SS(36);   /* 内容区宽 (侧栏自适应同源) - 卡片内边距×2 */
        float docH = XjsMdLayout(XjsSetMdPageDoc(pageIdx), s_set.rt, contentW, SS(1));
        r.md = s_mdDoc[pageIdx];
        if (docH > 0) {
            r.h = docH;
        } else {   /* 文档缺失: 占位说明行 */
            r.ctrl = CT_INFO;
            r.name = XjsT(L"设置.关于.文档缺失");
            r.desc = XjsFmt(XjsT(L"设置.关于.文档缺失.说明"), docName);
            r.h = SS(62);
        }
    } else {   /* RT 未就绪 (行模型先于首帧被鼠标消息触发): 占位行, 首帧经 s_mdPending 重建 */
        r.md = XjsSetMdPageDoc(pageIdx);
        r.h = SS(200);
    }
    s_set.cards.back().rows.push_back(r);
}

/* ==================== 插件页 (全局分类, 2026-09-19) ==================== */

/* 权限/能力徽章文本 (用户视角审计; 行模型与启用确认框共用) */
static std::wstring XjsSetPluginPermsText(unsigned m) {
    std::wstring s;
    auto add = [&s](bool on, const wchar_t* key) {
        if (!on) return;
        if (!s.empty()) s += L" / ";
        s += XjsT(key);
    };
    add(m & XPP_READ, L"设置.插件.权限.读文件");
    add(m & XPP_WRITE, L"设置.插件.权限.写文件");
    add(m & XPP_EXEC, L"设置.插件.权限.执行程序");
    add(m & XPP_UI, L"设置.插件.权限.界面操作");
    return s.empty() ? XjsT(L"设置.插件.权限.无") : s;
}
static std::wstring XjsSetPluginCapsText(unsigned m) {
    std::wstring s;
    auto add = [&s](bool on, const wchar_t* key) {
        if (!on) return;
        if (!s.empty()) s += XjsT(L"设置.插件.徽章分隔");
        s += XjsT(key);
    };
    add(m & XPC_FILECTX, L"设置.插件.能力.文件右键");
    add(m & XPC_SEARCHBOXMENU, L"设置.插件.能力.搜索框菜单");
    add(m & XPC_SEARCHMODES, L"设置.插件.能力.搜索模式");
    add(m & XPC_HOSTED, L"设置.插件.能力.托管词条");
    add(m & XPC_INPUTINTERCEPT, L"设置.插件.能力.输入拦截");
    add(m & XPC_STATUSBAR, L"设置.插件.能力.状态栏");
    add(m & XPC_EVENTS, L"设置.插件.能力.事件");
    add(m & XPC_PREVIEW, L"设置.插件.能力.预览");
    add(m & XPC_PANEL, L"设置.插件.能力.面板接管");
    add(m & XPC_BATCHRENAME, L"设置.插件.能力.批量重命名");
    return s.empty() ? XjsT(L"设置.插件.能力.无") : s;
}

/* 状态文本 (单一来源): 清单错误/已禁用/加载失败/需重启生效/已加载/已启用(纯声明式) */
static std::wstring XjsSetPluginStatusText(const XjsPluginBrief& b) {
    if (!b.declared) return XjsT(L"设置.插件.状态.清单错误");
    if (!b.enabled) return XjsT(L"设置.插件.状态.已禁用");
    if (!b.hasDll) return XjsT(L"设置.插件.状态.已启用");
    if (!b.loaded) return XjsT(L"设置.插件.状态.加载失败");
    return b.staleDll ? XjsT(L"设置.插件.状态.需重启生效") : XjsT(L"设置.插件.状态.已加载");
}

/* 启用开关执行端: 禁用=闸门关 (已注入菜单下次构建自然消失, 不置灰 — 照源样式口径), 即时落盘;
   启用=首次/版本变化先过风险确认框 (原生插件与主程序同权限, 诚实口径原文必须出现),
   通过后立即加载; 失败 toast + 状态列显示原因, 不写回启用态 (设计稿 §2.2) */
static void XjsSetPluginToggle(int i) {
    XjsPluginBrief b;
    if (!XjsPluginBriefAt(i, &b)) return;
    if (b.enabled) {
        XjsPluginDisable(i);
        XjsSearchNow(false);   /* 来源集可能变化 (插件搜索模式/托管词条) → 重搜整链 */
        XjsSaveConfig();
        s_set.rowsDirty = true;
        InvalidateRect(s_set.hwnd, NULL, FALSE);
        return;
    }
    if (XjsPluginNeedsConfirm(i)) {
        std::wstring dllPath = b.hasDll ? (b.dir + L"\\" + b.dllFile) : XjsT(L"设置.插件.确认.无DLL");
        std::wstring nl = L"\n";
        std::wstring desc = XjsT(L"设置.插件.确认.行版本") + (b.version.empty() ? L"-" : b.version) + nl
                          + XjsT(L"设置.插件.确认.行作者") + (b.author.empty() ? L"-" : b.author) + nl
                          + XjsT(L"设置.插件.确认.行DLL") + dllPath + nl
                          + XjsT(L"设置.插件.确认.行权限") + XjsSetPluginPermsText(b.perms) + nl
                          + XjsT(L"设置.插件.确认.行能力") + XjsSetPluginCapsText(b.caps) + nl + nl
                          + XjsT(L"设置.插件.确认.风险原文");
        int r = XjsShowAskDialog(s_set.hwnd,
                                 (XjsT(L"设置.插件.确认.标题前缀") + b.name).c_str(),
                                 desc.c_str(),
                                 ("[{\"text\":\"" + XjsTUtf8(L"设置.插件.确认.启用按钮") + "\",\"style\":\"danger\"},{\"text\":\""
                                  + XjsTUtf8(L"通用词.取消") + "\"}]").c_str());
        if (r != 0) return;
        XjsPluginMarkConfirmed(i);   /* 记"已确认版本", 同版本不再打扰 (调用方落盘) */
    }
    std::wstring err;
    if (!XjsPluginEnable(i, &err))
        XjsToastShow(s_set.hwnd, (XjsT(L"设置.插件.启用失败前缀") + err).c_str(), XTOAST_ERROR, SS(1));
    XjsSaveConfig();
    s_set.rowsDirty = true;
    InvalidateRect(s_set.hwnd, NULL, FALSE);
}

/* 下拉选择器行 (行尾按钮带 ∨ 箭头, 点击弹窗锚定控件左下角 — 与主窗筛选器同口径):
   新增下拉行 = 此处加 act + XjsSetActivateRow 加菜单分支, 禁止再走 GetCursorPos 锚点 */
static bool XjsSetActIsDropdown(int act) {
    switch (act) {
        case ACT_ENGINE: case ACT_DCCTRL: case ACT_BLUR: case ACT_APPEAR:
        case ACT_ZOOM: case ACT_SORT: case ACT_DEFSEL: case ACT_MOUSEOPEN:
        case ACT_CREATEFILL:
            return true;
    }
    return false;
}

/* 描述文本按可用宽度换行后的实际高度 (DirectWrite 实测; 行高不固定, 随字体×宽度自适应)。
   资源未就绪时返回单行估计值, 调用方 (XjsSetBuildRows) 会置 rowsDirty 下一帧重测 */
static float XjsSetMeasureDescHeight(const std::wstring& desc, float maxW) {
    if (!s_set.tfDesc || desc.empty() || maxW <= 10) return SS(16);
    XjsTextLayout* lay = NULL;
    g_dw->CreateTextLayout(desc.c_str(), (UINT32)desc.length(), s_set.tfDesc, maxW, 10000.0f, &lay);
    float h = SS(16);
    if (lay) {
        XjsTextMetrics m = {};
        if (SUCCEEDED(lay->GetMetrics(&m)) && m.height > 0) h = m.height;
        lay->Release();
    }
    return h;
}

/* ==================== 内存页锁定 (LSA "锁定内存页" 账户权限, 原版同款状态机) ====================
 * 状态 = 策略侧 (本地安全策略是否给本账户授予 SeLockMemoryPrivilege) × 令牌侧 (本进程令牌
 * 是否已持有该特权) 的组合; 授予/移除即时写策略, 令牌须注销重新登录才刷新。
 * 不持久化任何配置 —— 系统策略本身就是事实源, 开关勾选态每次按查询结果现画。 */
enum {   /* 与原版"内存大页开启状态"常量语义一一对应 */
    XMLK_ST_ENABLED     = 1,   /* 策略已授予 + 令牌已持有且启用 = 当前进程可用大页 */
    XMLK_ST_GRANTED     = 2,   /* 策略已授予 + 令牌还没有 (需注销重新登录) — 警示 */
    XMLK_ST_GRANTED_OFF = 0,   /* 策略已授予 + 令牌已持有但未启用 */
    XMLK_ST_REMOVED     = 3,   /* 策略已移除 + 令牌仍持权 (随注销失效) — 警示 */
    XMLK_ST_NEVER       = -1,  /* 未配置 */
    XMLK_ST_FAILED      = -2   /* 系统调用失败 */
};
#ifndef STATUS_OBJECT_NAME_NOT_FOUND
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xC0000034L)   /* 账户尚无任何权限时 LsaEnumerateAccountRights 的返回 */
#endif

/* 令牌侧: 进程令牌是否持有 SeLockMemoryPrivilege (*enabled = 特权是否已启用) */
static bool XjsMemLockTokenHas(bool* enabled) {
    *enabled = false;
    LUID luid = {};
    if (!LookupPrivilegeValueW(NULL, SE_LOCK_MEMORY_NAME, &luid)) return false;
    HANDLE tok = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    bool has = false;
    DWORD n = 0;
    GetTokenInformation(tok, TokenPrivileges, NULL, 0, &n);
    if (n) {
        std::vector<BYTE> buf(n);
        if (GetTokenInformation(tok, TokenPrivileges, buf.data(), n, &n)) {
            const auto* tp = (const TOKEN_PRIVILEGES*)buf.data();
            for (DWORD i = 0; i < tp->PrivilegeCount; i++) {
                if (tp->Privileges[i].Luid.HighPart == luid.HighPart &&
                    tp->Privileges[i].Luid.LowPart == luid.LowPart) {
                    has = true;
                    *enabled = (tp->Privileges[i].Attributes & SE_PRIVILEGE_ENABLED) != 0;
                    break;
                }
            }
        }
    }
    CloseHandle(tok);
    return has;
}

/* 策略侧查询: 组合出状态常量 (见枚举注释) */
static int XjsMemLockQuery() {
    bool tokOn = false;
    bool tokHas = XjsMemLockTokenHas(&tokOn);
    int st = XMLK_ST_FAILED;
    HANDLE tok = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        DWORD n = 0;
        GetTokenInformation(tok, TokenUser, NULL, 0, &n);
        if (n) {
            std::vector<BYTE> buf(n);
            if (GetTokenInformation(tok, TokenUser, buf.data(), n, &n)) {
                PSID sid = ((TOKEN_USER*)buf.data())->User.Sid;
                LSA_HANDLE pol = NULL;
                LSA_OBJECT_ATTRIBUTES oa = { sizeof(oa) };
                if (LsaOpenPolicy(NULL, &oa, POLICY_LOOKUP_NAMES, &pol) == 0) {
                    PLSA_UNICODE_STRING rights = NULL;
                    ULONG cnt = 0;
                    NTSTATUS sr = LsaEnumerateAccountRights(pol, sid, &rights, &cnt);
                    if (sr == 0 || sr == STATUS_OBJECT_NAME_NOT_FOUND) {
                        bool polHas = false;
                        for (ULONG i = 0; rights && i < cnt; i++) {
                            ULONG len = rights[i].Length / sizeof(WCHAR);
                            if (len == wcslen(SE_LOCK_MEMORY_NAME) &&
                                _wcsnicmp(rights[i].Buffer, SE_LOCK_MEMORY_NAME, len) == 0) { polHas = true; break; }
                        }
                        if (!polHas)          st = tokHas ? XMLK_ST_REMOVED : XMLK_ST_NEVER;
                        else if (!tokHas)     st = XMLK_ST_GRANTED;
                        else                  st = tokOn ? XMLK_ST_ENABLED : XMLK_ST_GRANTED_OFF;
                    }
                    if (rights) LsaFreeMemory(rights);
                    LsaClose(pol);
                }
            }
        }
        CloseHandle(tok);
    }
    return st;
}

/* 授予/移除本账户的 SeLockMemoryPrivilege (写本地安全策略; 程序以管理员运行才可用) */
static bool XjsMemLockApply(bool enable) {
    bool ok = false;
    HANDLE tok = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        DWORD n = 0;
        GetTokenInformation(tok, TokenUser, NULL, 0, &n);
        if (n) {
            std::vector<BYTE> buf(n);
            if (GetTokenInformation(tok, TokenUser, buf.data(), n, &n)) {
                PSID sid = ((TOKEN_USER*)buf.data())->User.Sid;
                LSA_HANDLE pol = NULL;
                LSA_OBJECT_ATTRIBUTES oa = { sizeof(oa) };
                if (LsaOpenPolicy(NULL, &oa, POLICY_ALL_ACCESS, &pol) == 0) {
                    LSA_UNICODE_STRING r;
                    r.Buffer = (PWSTR)SE_LOCK_MEMORY_NAME;
                    r.Length = (USHORT)(wcslen(SE_LOCK_MEMORY_NAME) * sizeof(WCHAR));
                    r.MaximumLength = (USHORT)(r.Length + sizeof(WCHAR));
                    NTSTATUS sr = enable ? LsaAddAccountRights(pol, sid, &r, 1)
                                         : LsaRemoveAccountRights(pol, sid, FALSE, &r, 1);
                    ok = (sr == 0);
                    LsaClose(pol);
                }
            }
        }
        CloseHandle(tok);
    }
    return ok;
}

static const wchar_t* XjsMemLockStateText(int st) {
    switch (st) {
        case XMLK_ST_ENABLED:     return XjsT(L"设置.内存.状态已启用");
        case XMLK_ST_GRANTED:     return XjsT(L"设置.内存.状态已授予");
        case XMLK_ST_GRANTED_OFF: return XjsT(L"设置.内存.状态未启用");
        case XMLK_ST_REMOVED:     return XjsT(L"设置.内存.状态已移除");
        case XMLK_ST_NEVER:       return XjsT(L"设置.内存.状态未配置");
        default:                  return XjsT(L"设置.内存.状态未知");
    }
}

/* 引擎统计文本 (UTF-8, \r\n 分行) → 宽字符行块: 丢 \r, 去尾部空行 */
static std::wstring XjsStatTextW(const char* utf8) {
    std::wstring s = utf8 ? Utf8ToUtf16(utf8) : L"";
    size_t w = 0;
    for (size_t i = 0; i < s.size(); i++)
        if (s[i] != L'\r') s[w++] = s[i];
    s.resize(w);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L' ')) s.pop_back();
    return s;
}

/* 字节数 → "1.23 GB" 人类可读 (索引内存占用行) */
static std::wstring XjsFmtMemBytes(long long b) {
    static const wchar_t* const U[5] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double v = (double)b;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; u++; }
    wchar_t buf[40];
    if (u == 0) _snwprintf(buf, 40, L"%lld B", b);
    else        _snwprintf(buf, 40, L"%.1f %s", v, U[u]);
    return buf;
}

/* 当前搜索待渲染图标: 全部窗口求和 (各窗只统计自己结果轮次) — 行模型与 1s 采样签名共用 */
static long long s_pendSum;   /* ForEach 无上下文参数; UI 线程 1s 采样串行调用, 文件级累加器够用 */
static void XjsSetPendSumRun(XjsSearchWindow* w) {
    if (w->result && w->searchFingerprint.load() >= 0)
        s_pendSum += xjs_result_GetPendingIconCount(w->result, w->searchFingerprint.load());
}
static long long XjsSetResultPendSum() {
    s_pendSum = 0;
    XjsSearchWindow::ForEach(&XjsSetPendSumRun);
    return s_pendSum;
}

/* 实时页数据签名 (内存/性能分析 1s 刷新): 把两页所有动态值拼成一行, 定时器先比对签名,
   变了才置脏重建 —— 无条件重建会周期性踩掉 "按下→松开" 的命令回放窗口 (WM_LBUTTONUP
   回放带 !rowsDirty 守卫, 行模型在两击之间被重建 = 该次点击被吞) */
static std::wstring XjsSetLiveSignature() {
    if (!g_engine) return L"0";
    std::wstring s = XjsStatTextW(xjs_db_GetMemorySizeString(g_engine));
    s += L"#";
    s += std::to_wstring(xjs_db_GetMemorySize(g_engine));
    s += L"#";
    s += std::to_wstring(xjs_icon_GetPendingCount(g_engine));
    s += L"#";
    s += std::to_wstring(xjs_sync_GetPendingCount(g_engine));
    s += L"#";
    s += std::to_wstring(XjsSetResultPendSum());
    s += L"#";
    s += std::to_wstring(xjs_db_IsPerformanceSwitch(g_engine) != FALSE);
    s += std::to_wstring(xjs_db_IsScanPerformanceSwitch(g_engine) != FALSE);
    s += L"#";
    s += XjsStatTextW(xjs_db_GetPerformanceText(g_engine));
    s += L"#";
    s += XjsStatTextW(xjs_db_GetScanPerformanceText(g_engine));
    s += L"#";
    s += XjsStatTextW(xjs_sync_GetPerformanceText(g_engine));
    return s;
}

static bool XjsSetBuildRows() {
    XjsWindowScope scope(XjsSetOwner());   /* 行模型读 owner 的界面设置 (皮肤/视图/预览勾选态) */
    if (s_setOwner) s_setOwner->SyncSkin();   /* 皮肤列表勾选态比对 g_skinName: 全局镜像须先认回 owner */
    if (s_rowsUnitBuilt != SS(1)) s_set.rowsDirty = true;   /* 缩放/DPI 变化 → 全部行高失效, 强制重建 */
    if (!s_set.rowsDirty && !s_set.cards.empty()) return false;   /* 干净: 复用 (皮肤枚举扫盘只在重建时做) */
    s_rowsUnitBuilt = SS(1);
    s_set.rowsDirty = false;
    s_set.cards.clear();   /* 必清空: 否则切分类后旧卡片残留, 全部叠画 */
    RECT brc;
    GetClientRect(s_set.hwnd, &brc);
    const float cx1Build = (float)brc.right - SS(28);   /* 内容区右缘 (与绘制侧 cx1 同一公式) */
    const float cx0Build = XjsSetSideW() + SS(24);      /* 内容区左缘 (与绘制侧 cx0 同一公式; 侧栏宽自适应) */
    const bool canMeasure = s_set.tfDesc != NULL && s_set.tfBtn != NULL;
    const float rowGap = SS(2), cardGap = SS(16);
    float y = SS(20);
    const float rowHPlain = SS(44), rowHDesc = SS(62), titleH = SS(40);

    /* 行尾控件宽度 (构建时按 value 实测; 与绘制/命中/文本右缘让位同一来源 —
       固定预留 80px 的旧口径会被宽按钮/宽胶囊压住说明文字)。
       单槽记忆 (局部量, 每次重建自然复位): 表格页整页同文案 ("删除") 逐行实测 =
       每行一次 DWrite 排版, 别名页 ~700 行纯重复 — 同键直接复用 */
    std::wstring memoCtrlV; XjsSetCtrl memoCtrlC = CT_INFO; int memoCtrlA = -1; float memoCtrlW = 0;
    auto rowCtrlW = [&](int act, XjsSetCtrl ctrl, const std::wstring& value) -> float {
        if (ctrl != CT_BUTTON) act = 0;   /* act 只影响按钮的下拉加宽: 其余控件 (CT_TROW 行号 act 每行不同) 不进键 */
        if (ctrl == memoCtrlC && act == memoCtrlA && value == memoCtrlV && memoCtrlC != CT_INFO) return memoCtrlW;
        float w;
        switch (ctrl) {
            case CT_SWITCH: w = SS(42); break;
            case CT_PILL:   w = s_set.tfBtn ? xf_max(XjsMeasureText(value.c_str(), s_set.tfBtn) + SS(24), SS(56)) : SS(90); break;
            case CT_BUTTON: w = s_set.tfBtn ? XjsMeasureText(value.c_str(), s_set.tfBtn) + SS(36)
                                                 + (XjsSetActIsDropdown(act) ? SS(12) : 0) : SS(90); break;
            case CT_TROW:   w = s_set.tfBtn ? XjsMeasureText(value.c_str(), s_set.tfBtn) + SS(36) : SS(90); break;   /* 行尾"删除"钮, 同按钮几何 */
            case CT_INPUT:  w = SS(272); break;
            default:        return 0;   /* 未知控件不进记忆 (每次 0 语义不变) */
        }
        memoCtrlC = ctrl; memoCtrlA = act; memoCtrlV = value; memoCtrlW = w;
        return w;
    };

    auto newCard = [&](const wchar_t* title) {
        XjsSetCard c;
        c.title = title;
        c.y = y;
        s_set.cards.push_back(c);
    };
    auto addRow = [&](int act, XjsSetCtrl ctrl, const wchar_t* name, const wchar_t* desc,
                      const std::wstring& value, bool checked, bool danger, bool disabled = false,
                      int act2 = ACT_NONE, const wchar_t* value2 = NULL, bool danger2 = false) {
        XjsSetRow r;
        r.act = act; r.ctrl = ctrl; r.name = name ? name : L""; r.desc = desc ? desc : L"";
        r.value = value; r.checked = checked; r.danger = danger; r.disabled = disabled;
        r.ctrlW = rowCtrlW(act, ctrl, value);
        r.act2 = act2; r.danger2 = danger2;
        if (value2) { r.value2 = value2; r.ctrlW2 = rowCtrlW(act2, CT_BUTTON, r.value2); }
        if (r.desc.empty()) {
            r.h = rowHPlain;
        } else {
            /* 行高随说明文字换行实测自适应: 文本区右缘按行尾控件组实际宽度让位;
               textL 与绘制侧 (cx0 + 选项槽/左缩进) 同源 — 侧栏自适应后不再抄旧常量 132 */
            float textL = cx0Build + (ctrl == CT_OPTION ? SS(44) : SS(18));
            float span = XjsSetRowCtrlSpan(r);
            float textR = cx1Build - SS(18) - (span > 0 ? span + SS(14) : 0);
            float descH = XjsSetMeasureDescHeight(r.desc, textR - textL);
            r.h = xf_max(rowHDesc, SS(8) + SS(18) + descH + SS(10));   /* 单行=旧固定高, 多行自动长高 */
        }
        y += r.h + rowGap;
        s_set.cards.back().rows.push_back(r);
    };

    switch (s_set.cat) {
        case SC_GENERAL: {  /* 通用 = 全局设置 (窗口分组之外的都随进程共享) */
            newCard(XjsT(L"设置分类.通用"));
            addRow(ACT_ENGINE, CT_BUTTON, XjsT(L"设置.通用.界面显示模式"),
                   XjsT(L"设置.通用.界面显示模式.说明"),
                   g_gfxEngine == 1 ? XjsT(L"通用词.兼容模式") : XjsT(L"通用词.标准模式"), false, false);
            addRow(ACT_DCCTRL, CT_BUTTON, XjsT(L"设置.通用.双击Ctrl"),
                   XjsT(L"设置.通用.双击Ctrl.说明"),
                   g_doubleCtrlTarget.empty() ? XjsT(L"通用词.禁用") : g_doubleCtrlTarget.c_str(), false, false);
            bool autoOn = XjsIsAutoStartEnabled() != 0;
            addRow(ACT_AUTOSTART, CT_SWITCH, XjsT(L"设置.通用.开机自启动"), XjsT(L"设置.通用.开机自启动.说明"), L"", autoOn, false);
            break;
        }
        case SC_WIN: {  /* 窗口 (每窗: 快捷键+名称+窗口行为; 主窗固定"默认窗口"不可改) */
            newCard(XjsT(L"设置分类.窗口"));
            XjsSearchWindow* ow = XjsSetOwner();
            bool fixedName = ow && ow->isMain;
            /* 窗口名称第一项 (2026-09-17 用户口径): 命名是本窗身份, 放最前 */
            addRow(ACT_WINNAME, CT_BUTTON, g_name.c_str(),
                   fixedName ? XjsT(L"设置.窗口.主窗名说明")
                             : XjsT(L"设置.窗口.窗口名称.说明"),
                   XjsT(L"右键菜单.重命名"), false, false, fixedName);
            /* 全局快捷键 (每窗, 无内置默认): 设置过但注册失败 (被其它程序占用) = 警告态 */
            const bool hkBad = ow && ow->hotkeyVk && !s_set.recHotkey && !ow->hotkeyActive;
            std::wstring hkText = (ow && ow->hotkeyVk) ? XjsHotkeyText(ow->hotkeyMod, ow->hotkeyVk)
                                                       : XjsT(L"通用词.未设置");
            addRow(ACT_HOTKEY, CT_PILL, XjsT(L"设置.窗口.全局快捷键"),
                   hkBad ? XjsT(L"设置.窗口.快捷键占用说明")
                         : XjsT(L"设置.窗口.快捷键说明"),
                   s_set.recHotkey ? XjsHotkeyLiveText()
                                   : (hkText + (hkBad ? XjsT(L"通用词.未生效") : L"")),
                   false, false);
            /* 窗口行为: 失焦动作 / 激活与创建位置 (菜单选择), 标题栏·状态栏可见性 (开关) */
            const wchar_t* const BLUR_T[2] = { XjsT(L"通用词.无"), XjsT(L"通用词.关闭窗口") };
            addRow(ACT_BLUR, CT_BUTTON, XjsT(L"设置.窗口.失焦行为"),
                   XjsT(L"设置.窗口.失焦行为.说明"),
                   BLUR_T[g_blurAction & 1], false, false);
            addRow(ACT_APPEAR, CT_BUTTON, XjsT(L"设置.窗口.出现位置"),
                   XjsT(L"设置.窗口.出现位置.说明"),
                   XjsAppearPosLabel(g_appearPos), false, false);
            addRow(ACT_CTRLBTN, CT_SWITCH, XjsT(L"设置.窗口.显示控制按钮"),
                   XjsT(L"设置.窗口.显示控制按钮.说明"),
                   L"", g_showCtrlBtns, false);
            addRow(ACT_FILTERBOX, CT_SWITCH, XjsT(L"设置.窗口.显示筛选框"),
                   XjsT(L"设置.窗口.显示筛选框.说明"),
                   L"", g_showFilterBox, false);
            addRow(ACT_STATUSBAR, CT_SWITCH, XjsT(L"设置.窗口.显示状态栏"),
                   XjsT(L"设置.窗口.显示状态栏.说明"),
                   L"", g_showStatusbar, false);
            addRow(ACT_TASKBAR, CT_SWITCH, XjsT(L"设置.窗口.任务栏图标"),
                   XjsT(L"设置.窗口.任务栏图标.说明"),
                   L"", g_taskbarIcon, false);
            break;
        }
        case SC_MANAGE: {  /* 窗口管理 (全局): 全部命名窗口档案, 可删除 (已打开连窗销毁+卸热键) */
            newCard(XjsT(L"设置分类.窗口管理"));
            for (int k = 0; k < XjsUiProfileCount(); k++) {
                XjsUiProfile* prof = XjsUiProfileAt(k);
                if (!prof) continue;
                XjsSearchWindow* w = XjsSearchWindow::AtSlot(k);
                std::wstring title = w ? w->name : (prof->name.empty() ? XjsT(L"菜单.未命名窗口") : prof->name);
                std::wstring desc = (w ? XjsT(L"设置.窗口管理.打开中") : XjsT(L"设置.窗口管理.未打开"));
                if (k == 0)
                    desc += XjsT(L"设置.窗口管理.主窗后缀");
                else if (prof->hotkeyVk)
                    desc += XjsT(L"设置.窗口管理.快捷键前缀") + XjsHotkeyText(prof->hotkeyMod, prof->hotkeyVk);
                bool disabled = (k == 0);   /* 默认窗口 (主窗): 名称固定不可改也不可删 */
                /* 不可改删的行用 CT_INFO (无行尾控件): 曾给它空文字的 CT_BUTTON, 画出一个
                   无字空描边矩形横在行尾, 很碍眼 (2026-09-17 用户反馈); 行名仍走 disabled 置灰。
                   其余行 = 重命名 (主钮) + 删除 (第二钮, 红描边); 未打开档案同样可改名 */
                addRow(ACT_WINGM_REN + k, disabled ? CT_INFO : CT_BUTTON, title.c_str(), desc.c_str(),
                       disabled ? L"" : XjsT(L"右键菜单.重命名"), false, false, disabled,
                       disabled ? ACT_NONE : ACT_WINGM_DEL + k, disabled ? NULL : XjsT(L"通用词.删除"), true);
            }
            break;
        }
        case SC_MATCH: {  /* 搜索匹配 (每窗: 各窗口独立一份, 随 uiWindows 档案持久化) */
            newCard(XjsT(L"设置分类.搜索匹配"));
            addRow(ACT_MATCH + 0, CT_SWITCH, XjsT(L"设置.搜索匹配.空词显示全部"), XjsT(L"设置.搜索匹配.空词显示全部.说明"), L"", g_match.emptyShowsAll, false);
            addRow(ACT_MATCH + 1, CT_SWITCH, XjsT(L"设置.搜索匹配.区分大小写"), XjsT(L"设置.搜索匹配.区分大小写.说明"), L"", g_match.caseSensitive, false);
            addRow(ACT_MATCH + 2, CT_SWITCH, XjsT(L"设置.搜索匹配.支持全拼"), XjsT(L"设置.搜索匹配.支持全拼.说明"), L"", g_match.pinyinFull, false);
            addRow(ACT_MATCH + 3, CT_SWITCH, XjsT(L"设置.搜索匹配.支持首拼"), XjsT(L"设置.搜索匹配.支持首拼.说明"), L"", g_match.pinyinInitial, false);
            addRow(ACT_MATCH + 4, CT_SWITCH, XjsT(L"设置.搜索匹配.拼音整字匹配"), XjsT(L"设置.搜索匹配.拼音整字匹配.说明"), L"", g_match.pinyinExact, false);
            addRow(ACT_MATCH + 5, CT_SWITCH, XjsT(L"设置.搜索匹配.问号通配符"), XjsT(L"设置.搜索匹配.问号通配符.说明"), L"", g_match.wildcardQuestion, false);
            addRow(ACT_MATCH + 6, CT_SWITCH, XjsT(L"设置.搜索匹配.星号通配符"), XjsT(L"设置.搜索匹配.星号通配符.说明"), L"", g_match.wildcardStar, false);
            addRow(ACT_MATCH + 7, CT_SWITCH, XjsT(L"设置.搜索匹配.全角半角"), XjsT(L"设置.搜索匹配.全角半角.说明"), L"", g_match.matchFullWidth, false);
            break;
        }
        case SC_APPEAR: {  /* 外观 (每窗: 界面语言+界面缩放+主题皮肤) */
            newCard(XjsT(L"设置分类.外观"));
            addRow(ACT_LANG, CT_BUTTON, XjsT(L"设置.外观.界面语言"),
                   XjsT(L"设置.外观.界面语言.说明"),
                   g_lang == XLANG_AUTO ? XjsLangAutoLabel() : XjsLangLabel(g_lang), false, false);
            addRow(ACT_ZOOM, CT_BUTTON, XjsT(L"设置.外观.界面缩放"),
                   XjsT(L"设置.外观.界面缩放.说明"),
                   std::to_wstring(g_uiZoomTenths * 10) + L"%", false, false);
            newCard(XjsT(L"设置.外观.主题皮肤"));
            g_skinMenuNames = XjsSkinEnumerate();
            int maxSkin = (int)g_skinMenuNames.size() < 16 ? (int)g_skinMenuNames.size() : 16;
            for (int i = 0; i < maxSkin; i++)
                addRow(ACT_SKIN + i, CT_OPTION, g_skinMenuNames[i].c_str(), L"",
                       L"", g_skinMenuNames[i] == g_skinName, false);
            if (maxSkin == 0)
                addRow(ACT_NONE, CT_INFO, XjsT(L"设置.外观.无皮肤"), XjsT(L"设置.外观.无皮肤.说明"), L"", false, false);
            break;
        }
        case SC_LIST: {  /* 列表 (每窗) */
            newCard(XjsT(L"设置分类.列表"));
            {   /* 默认排序 (每窗): 事实源 = 结果对象, 这里只匹配预设显示当前值 */
                const char* sf = g_result ? xjs_result_GetSortField(g_result) : NULL;
                bool sw = g_result ? xjs_result_GetSortway(g_result) != FALSE : false;
                int si = -1;
                for (int i = 0; i < 7; i++)
                    if (sf && !strcmp(SORT_PRESETS[i].field, sf) && SORT_PRESETS[i].asc == sw) { si = i; break; }
                addRow(ACT_SORT, CT_BUTTON, XjsT(L"设置.列表.默认排序"),
                       XjsT(L"设置.列表.默认排序.说明"),
                       si >= 0 ? XjsT(SORT_PRESETS[si].label) : XjsT(L"通用词.自定义"), false, false);
            }
            addRow(ACT_DEFSEL, CT_BUTTON, XjsT(L"设置.列表.默认选中"),
                   XjsT(L"设置.列表.默认选中.说明"),
                   g_defaultSel == 1 ? XjsT(L"通用词.自动选中") : XjsT(L"通用词.不自动选中"), false, false);
            addRow(ACT_PREVIEW, CT_SWITCH, XjsT(L"菜单.预览面板"), XjsT(L"设置.列表.预览面板.说明"), L"", g_previewVisible, false);
            addRow(ACT_DRIVEPROG, CT_SWITCH, XjsT(L"设置.列表.驱动器容量条"),
                   XjsT(L"设置.列表.驱动器容量条.说明"), L"", g_driveProgress, false);
            addRow(ACT_ROWHOVER, CT_SWITCH, XjsT(L"设置.列表.悬停高亮"),
                   XjsT(L"设置.列表.悬停高亮.说明"), L"", g_rowHover, false);
            addRow(ACT_HOVERFADE, CT_SWITCH, XjsT(L"设置.列表.悬停残影"),
                   XjsT(L"设置.列表.悬停残影.说明"), L"", g_rowHoverFade, false, !g_rowHover);
            const wchar_t* const VN[4] = { XjsT(L"通用词.紧凑视图"), XjsT(L"通用词.详情视图"),
                                                  XjsT(L"通用词.中等图标"), XjsT(L"通用词.大图标") };
            for (int m = 0; m < 4; m++)
                addRow(ACT_VIEW + m, CT_OPTION, VN[m], L"", L"", g_viewMode == m, false);
            break;
        }
        case SC_DATA: {  /* 数据维护 */
            newCard(XjsT(L"设置分类.数据维护"));
            addRow(ACT_REBUILD, CT_BUTTON, XjsT(L"设置.数据维护.重建索引"), XjsT(L"设置.数据维护.重建索引.说明"), XjsT(L"重建.重建按钮"), false, true);
            addRow(ACT_CLEARHIST, CT_BUTTON, XjsT(L"菜单.清空搜索历史"), XjsT(L"设置.数据维护.清空历史"), XjsT(L"通用词.清空"), false, false);
            newCard(XjsT(L"设置.数据维护.排除目录"));
            addRow(ACT_EXCL_ADD, CT_BUTTON, XjsT(L"设置.数据维护.添加排除目录"), XjsT(L"设置.数据维护.排除目录.说明"), XjsT(L"设置.数据维护.添加目录"), false, false);
            addRow(ACT_NONE, CT_INPUT, XjsT(L"设置.数据维护.手动输入路径"), XjsT(L"设置.数据维护.手动输入路径.说明"), L"", false, false);
            {
                std::vector<std::wstring> dirs;
                XjsJsonStringArray(g_engine ? xjs_db_GetExcludedDirs(g_engine) : NULL, &dirs);
                if (dirs.empty())
                    addRow(ACT_NONE, CT_INFO, XjsT(L"设置.数据维护.尚未排除"), XjsT(L"设置.数据维护.点击添加提示"), L"", false, false);
                else
                    for (int i = 0; i < (int)dirs.size(); i++)
                        addRow(ACT_EXCL_DEL + i, CT_BUTTON, dirs[i].c_str(), L"", XjsT(L"通用词.删除"), false, false);
            }
            break;
        }
        case SC_FILTER: {  /* 文件分类 (全局: 引擎筛选器表格, 保存整体下发 xjs_filter_SetFilterJSON) */
            if (s_filterReload) { XjsSetFilterRowsLoad(); s_filterReload = false; }
            newCard(XjsT(L"设置分类.文件分类"));
            {   /* 说明: 两段合一 (名称行顶 + 说明占整块, 同内存页 lockDesc 配方) */
                std::wstring desc = std::wstring(XjsT(L"设置.文件分类.说明")) + L"\n" + XjsT(L"设置.文件分类.生效说明");
                addRow(ACT_NONE, CT_INFO, XjsT(L"设置.文件分类.说明标题"), desc.c_str(), L"", false, false);
                s_set.cards.back().rows.back().block = true;
            }
            {   /* 分类列表行: 计数说明 + [保存][添加分类] 行尾双钮 (钮序同正式版: 保存左, 添加右) */
                addRow(ACT_TADD, CT_BUTTON, XjsT(L"设置.文件分类.分类列表"),
                       XjsFmt(XjsT(L"设置.文件分类.计数"), std::to_wstring(XjsSetFilterRows())).c_str(),
                       XjsT(L"设置.文件分类.添加分类"), false, false, false, ACT_TSAVE, XjsT(L"通用词.保存"), false);
            }
            int fn = XjsSetFilterRows();
            if (fn == 0)
                addRow(ACT_NONE, CT_INFO, XjsT(L"设置.文件分类.尚未配置"), L"", L"", false, false);
            for (int i = 0; i < fn; i++) {
                addRow(ACT_FDEL + i, CT_TROW, L"", L"", XjsT(L"通用词.删除"), false, false);
                s_set.cards.back().rows.back().trowIdx = i;
                s_set.cards.back().rows.back().tblCols = 3;
            }
            break;
        }
        case SC_ALIAS: {  /* 别名 (全局: 引擎路径别名表格, 保存整体下发 xjs_alias_SetAliasJSON sync=TRUE) */
            if (s_aliasReload) { XjsSetAliasRowsLoad(); s_aliasReload = false; }
            newCard(XjsT(L"设置分类.别名"));
            {
                std::wstring desc = std::wstring(XjsT(L"设置.别名.说明")) + L"\n" + XjsT(L"设置.别名.生效说明");
                addRow(ACT_NONE, CT_INFO, XjsT(L"设置.别名.说明标题"), desc.c_str(), L"", false, false);
                s_set.cards.back().rows.back().block = true;
            }
            {
                addRow(ACT_TADD, CT_BUTTON, XjsT(L"设置.别名.别名列表"),
                       XjsFmt(XjsT(L"设置.别名.计数"), std::to_wstring(XjsSetAliasRows())).c_str(),
                       XjsT(L"设置.别名.添加别名"), false, false, false, ACT_TSAVE, XjsT(L"通用词.保存"), false);
            }
            int an = XjsSetAliasRows();
            if (an == 0)
                addRow(ACT_NONE, CT_INFO, XjsT(L"设置.别名.尚未配置"), L"", L"", false, false);
            for (int i = 0; i < an; i++) {
                addRow(ACT_ADEL + i, CT_TROW, L"", L"", XjsT(L"通用词.删除"), false, false);
                s_set.cards.back().rows.back().trowIdx = i;
                s_set.cards.back().rows.back().tblCols = 2;
            }
            break;
        }
        case SC_MEMORY: {  /* 内存 (全局): 大页内存权限 + 引擎索引内存占用 (引擎实时报告, 1s 刷新) */
            newCard(XjsT(L"设置分类.内存"));
            /* 内存页锁定: 勾选态与状态行都以真实权限状态为准 (原版口径: 不入配置, 系统策略即事实源) */
            int st = XjsMemLockQuery();
            bool memOn = (st == XMLK_ST_ENABLED || st == XMLK_ST_GRANTED_OFF || st == XMLK_ST_GRANTED);
            std::wstring lockDesc = std::wstring(XjsT(L"设置.内存.内存页锁定.说明")) + L"\n"
                                  + XjsT(L"设置.内存.当前状态") + XjsMemLockStateText(st);
            addRow(ACT_MEMLOCK, CT_SWITCH, XjsT(L"设置.内存.内存页锁定"), lockDesc.c_str(), L"", memOn, false);
            s_set.cards.back().rows.back().warnText = (st == XMLK_ST_GRANTED || st == XMLK_ST_REMOVED);
            if (g_engine) {
                addRow(ACT_NONE, CT_INFO, XjsT(L"设置.内存.索引内存占用"),
                       (XjsT(L"设置.内存.索引内存占用.值前缀") + XjsFmtMemBytes(xjs_db_GetMemorySize(g_engine))).c_str(),
                       L"", false, false);
                std::wstring detail = XjsStatTextW(xjs_db_GetMemorySizeString(g_engine));
                if (!detail.empty()) {
                    addRow(ACT_NONE, CT_INFO, XjsT(L"设置.内存.内存明细"), detail.c_str(), L"", false, false);
                    s_set.cards.back().rows.back().block = true;   /* 多行明细: 名称行顶 + 文本占整块 */
                }
            } else {
                addRow(ACT_NONE, CT_INFO, XjsT(L"设置.内存.索引内存占用"),
                       XjsT(L"设置.性能.引擎未就绪"), L"", false, false);
            }
            break;
        }
        case SC_PERF: {  /* 性能分析 (全局): 引擎队列实时状态 + 各阶段统计采样 (原版"资源占用"窗口内化) */
            newCard(XjsT(L"设置.性能.实时状态"));
            addRow(ACT_NONE, CT_INFO, XjsT(L"设置.性能.图标任务队列"),
                   XjsFmt(XjsT(L"设置.性能.待处理N"),
                          std::to_wstring(g_engine ? xjs_icon_GetPendingCount(g_engine) : 0)).c_str(), L"", false, false);
            addRow(ACT_NONE, CT_INFO, XjsT(L"设置.性能.文件同步队列"),
                   XjsFmt(XjsT(L"设置.性能.待处理N"),
                          std::to_wstring(g_engine ? xjs_sync_GetPendingCount(g_engine) : 0)).c_str(), L"", false, false);
            {   /* 当前搜索待渲染图标: 全部窗口求和 (行模型与 1s 采样签名共用 XjsSetResultPendSum) */
                addRow(ACT_NONE, CT_INFO, XjsT(L"设置.性能.当前搜索图标"),
                       XjsFmt(XjsT(L"设置.性能.待处理N"),
                              std::to_wstring(XjsSetResultPendSum())).c_str(), L"", false, false);
            }
            newCard(XjsT(L"设置.性能.统计开关"));
            bool lsOn = g_engine && xjs_db_IsPerformanceSwitch(g_engine) != FALSE;
            addRow(ACT_PERF_LSSW, CT_SWITCH, XjsT(L"设置.性能.加载保存统计"),
                   XjsT(L"设置.性能.加载保存统计.说明"), L"", lsOn, false,
                   false, ACT_PERF_LSCLR, XjsT(L"通用词.清零"), false);
            bool scnOn = g_engine && xjs_db_IsScanPerformanceSwitch(g_engine) != FALSE;
            addRow(ACT_PERF_SCNSW, CT_SWITCH, XjsT(L"设置.性能.遍历统计"),
                   XjsT(L"设置.性能.遍历统计.说明"), L"", scnOn, false,
                   false, ACT_PERF_SCNCLR, XjsT(L"通用词.清零"), false);
            addRow(ACT_PERF_SYNCLR, CT_BUTTON, XjsT(L"设置.性能.同步统计"),
                   XjsT(L"设置.性能.同步统计.说明"), XjsT(L"通用词.清零"), false, false);
            newCard(XjsT(L"设置.性能.统计明细"));
            {   /* 三份统计原文 (引擎多行文本): 开着采样且对应动作跑过后才有内容 */
                std::wstring t = XjsStatTextW(g_engine ? xjs_db_GetPerformanceText(g_engine) : NULL);
                addRow(ACT_NONE, CT_INFO, XjsT(L"设置.性能.加载保存明细"),
                       t.empty() ? XjsT(L"设置.性能.暂无数据") : t.c_str(), L"", false, false);
                s_set.cards.back().rows.back().block = true;
                t = XjsStatTextW(g_engine ? xjs_db_GetScanPerformanceText(g_engine) : NULL);
                addRow(ACT_NONE, CT_INFO, XjsT(L"设置.性能.遍历明细"),
                       t.empty() ? XjsT(L"设置.性能.暂无数据") : t.c_str(), L"", false, false);
                s_set.cards.back().rows.back().block = true;
                t = XjsStatTextW(g_engine ? xjs_sync_GetPerformanceText(g_engine) : NULL);
                addRow(ACT_NONE, CT_INFO, XjsT(L"设置.性能.同步明细"),
                       t.empty() ? XjsT(L"设置.性能.暂无数据") : t.c_str(), L"", false, false);
                s_set.cards.back().rows.back().block = true;
            }
            break;
        }
        case SC_PLUGINS: {  /* 插件 (全局: 原生插件管理; 目录发现/加载/闸门在 xjs_plugin.cpp) */
            newCard(XjsT(L"设置分类.插件"));
            addRow(ACT_PLUGINS_OPENDIR, CT_BUTTON, XjsT(L"设置.插件.插件目录"),
                   XjsT(L"设置.插件.插件目录.说明"), XjsT(L"通用词.打开"), false, false);
            addRow(ACT_PLUGINS_RESCAN, CT_BUTTON, XjsT(L"设置.插件.重新扫描"),
                   XjsT(L"设置.插件.重新扫描.说明"), XjsT(L"通用词.刷新"), false, false);
            int n = XjsPluginCount();
            if (n == 0) {
                addRow(ACT_NONE, CT_INFO, XjsT(L"设置.插件.无插件"), XjsT(L"设置.插件.无插件.说明"), L"", false, false);
                break;
            }
            for (int i = 0; i < n; i++) {
                XjsPluginBrief b;
                if (!XjsPluginBriefAt(i, &b)) continue;
                /* 行 1 = 启用开关: 名称 + 版本·作者·状态 */
                std::wstring meta = L"v" + (b.version.empty() ? std::wstring(L"-") : b.version)
                                  + (b.author.empty() ? L""
                                                      : XjsT(L"设置.插件.作者分隔") + b.author)
                                  + XjsT(L"设置.插件.状态分隔") + XjsSetPluginStatusText(b);
                addRow(ACT_PLUGINS_TOGGLE + i, CT_SWITCH, b.name.c_str(), meta.c_str(), L"", b.enabled, false);
                /* 行 2 = 审计: 能力 + 权限徽章 / 简介 */
                std::wstring audit = XjsT(L"设置.插件.审计.能力前缀") + XjsSetPluginCapsText(b.caps)
                                   + XjsT(L"设置.插件.审计.分隔")
                                   + XjsT(L"设置.插件.审计.权限前缀") + XjsSetPluginPermsText(b.perms);
                addRow(ACT_NONE, CT_INFO, audit.c_str(),
                       b.description.empty() ? XjsT(L"设置.插件.无简介") : b.description.c_str(), L"", false, false);
                /* 行 3 = 仅异常时: 清单错误 / 加载失败原因 / 需重启说明 */
                if (!b.declared)
                    addRow(ACT_NONE, CT_INFO, (XjsT(L"设置.插件.清单错误前缀") + b.manifestErr).c_str(), L"", L"", false, false);
                else if (b.enabled && b.hasDll && !b.loaded && !b.loadErr.empty())
                    addRow(ACT_NONE, CT_INFO, (XjsT(L"设置.插件.加载失败前缀") + b.loadErr).c_str(), L"", L"", false, false);
                else if (b.enabled && b.staleDll)
                    addRow(ACT_NONE, CT_INFO, XjsT(L"设置.插件.需重启说明"), L"", L"", false, false);
            }
            break;
        }
        case SC_OPEN: {  /* 打开 (每窗配置: 各窗口独立一份, 随 uiWindows 档案持久化) */
            newCard(XjsT(L"通用词.打开文件"));
            addRow(ACT_MOUSEOPEN, CT_BUTTON, XjsT(L"设置.打开.鼠标打开方式"),
                   XjsT(L"设置.打开.鼠标打开方式.说明"),
                   g_mouseOpen == 1 ? XjsT(L"通用词.单击打开") : XjsT(L"通用词.双击打开"), false, false);
            addRow(ACT_OPEN + 0, CT_SWITCH, XjsT(L"设置.打开.以管理员权限打开"),
                   XjsT(L"设置.打开.以管理员权限打开.说明"), L"", g_openElevated, false);
            addRow(ACT_OPEN + 1, CT_SWITCH, XjsT(L"设置.打开.后台打开文件"),
                   XjsT(L"设置.打开.后台打开文件.说明"), L"", g_openAsync, false);
            addRow(ACT_OPEN + 2, CT_SWITCH, XjsT(L"设置.打开.打开后隐藏窗口"),
                   XjsT(L"设置.打开.打开后隐藏窗口.说明"), L"", g_openHideWindow, false);
            break;
        }
        case SC_CREATE: {  /* 新建窗口 (每窗: 新窗口创建时的初始搜索状态; 显示前即执行一次首搜) */
            newCard(XjsT(L"设置分类.新建窗口"));
            const wchar_t* const FILL_T[3] = { XjsT(L"通用词.清空"), XjsT(L"通用词.指定关键词"), XjsT(L"通用词.上一次搜索的词") };
            addRow(ACT_CREATEFILL, CT_BUTTON, XjsT(L"设置.新建窗口.初始搜索词"),
                   XjsT(L"设置.新建窗口.初始搜索词.说明"),
                   FILL_T[g_createFill & 3], false, false);
            addRow(ACT_CREATEKW, CT_BUTTON, XjsT(L"设置.新建窗口.指定关键词"),
                   XjsT(L"设置.新建窗口.指定关键词.说明"),
                   XjsT(L"通用词.修改"), false, false, g_createFill != 1);
            break;
        }
        case SC_DONATE: {  /* 捐赠 (图片编译进 EXE 资源, 运行期解码) */
            newCard(XjsT(L"设置分类.捐赠"));
            addRow(ACT_NONE, CT_INFO, XjsT(L"设置.捐赠.引导语"), XjsT(L"设置.捐赠.扫码标题"), L"", false, false);
            s_set.cards.back().rows.back().centerText = true;
            addRow(ACT_NONE, CT_IMAGE, L"", L"", L"", false, false);
            s_set.cards.back().rows.back().img = 3;   /* 3 = 微信+支付宝并排两栏 */
            s_set.cards.back().rows.back().h = SS(420);
            addRow(ACT_DONORS, CT_BUTTON, XjsT(L"设置.捐赠.查看名单"), XjsT(L"设置.捐赠.名单说明"), XjsT(L"通用词.打开"), false, false);
            break;
        }
        case SC_MODES:   /* 搜索模式 (内置《搜索模式简介》md 文档, 通用引擎见 xjs_md.cpp) */
            XjsSetAppendMdPage(0, XjsT(L"菜单.搜索模式"), XjsT(L"设置.搜索模式.简介页标题"));
            break;
        case SC_LICENSE:   /* 许可证 (内置《第三方组件声明》md 文档) */
            XjsSetAppendMdPage(1, XjsT(L"设置分类.开源许可"), XjsT(L"设置.开源许可.第三方声明"));
            break;
        case SC_ABOUT: {  /* 关于 */
            newCard(XjsT(L"设置分类.关于"));
            const char* ver = xjs_GetVersion();
            std::wstring v = ver ? Utf8ToUtf16(ver) : XjsT(L"设置.关于.未知");
            std::wstring about = XjsT(L"设置.关于.搜索核心版本前缀") + v;
            addRow(ACT_NONE, CT_INFO, XjsT(L"应用.名称"), about.c_str(), L"", false, false);
            /* 编译时间 (报障时先看这行: 确认手上是哪一个构建) */
            std::wstring buildTime = XjsBuildTimeText();
            addRow(ACT_NONE, CT_INFO, XjsT(L"设置.关于.编译时间"), buildTime.c_str(), L"", false, false);
            /* 运行环境: 系统/位数/权限/显示模式 (显示模式为"重启生效"设置, 这里显示的是本次实际生效的;
               技术细节 (D2D/GDI+) 留在"复制版本信息"的剪贴板文本里供报障用) */
            std::wstring env = XjsOsVersionText() + L" · "
                             + (sizeof(void*) == 8 ? XjsT(L"设置.关于.64位") : XjsT(L"设置.关于.32位")) + L" · "
                             + (XjsIsRunningAsAdmin() ? XjsT(L"设置.关于.管理员权限") : XjsT(L"设置.关于.标准权限")) + L" · "
                             + (g_gfxEngine == 1 ? XjsT(L"通用词.兼容模式") : XjsT(L"通用词.标准模式"));
            addRow(ACT_NONE, CT_INFO, XjsT(L"设置.关于.运行环境"), env.c_str(), L"", false, false);
            addRow(ACT_COPYVER, CT_BUTTON, XjsT(L"设置.关于.复制版本信息"),
                   XjsT(L"设置.关于.复制版本信息.说明"),
                   XjsT(L"通用词.复制"), false, false);
            addRow(ACT_GITHUB, CT_BUTTON, XjsT(L"设置.关于.GitHub仓库"), L"", XjsT(L"通用词.打开"), false, false);
            addRow(ACT_SITE, CT_BUTTON, XjsT(L"设置.关于.官方网站"), L"", XjsT(L"通用词.打开"), false, false);
            newCard(XjsT(L"设置.关于.制作声明"));
            addRow(ACT_GLM, CT_INFO, XjsT(L"设置.关于.GLM制作"),
                   XjsT(L"设置.关于.GLM感谢"), L"", false, false);
            s_set.cards.back().rows.back().link = true;   /* 整行可点 → ACT_GLM (手型光标/accent 名) */
            addRow(ACT_NONE, CT_INFO, XjsT(L"设置.关于.非遗古法"), L"", L"", false, false);
            addRow(ACT_NONE, CT_INFO, XjsT(L"设置.关于.完全离线"), XjsT(L"设置.关于.无联网说明"), L"", false, false);
            newCard(XjsT(L"设置.关于.开发初衷"));
            addRow(ACT_NONE, CT_INFO, XjsT(L"设置.关于.Everything之问"),
                   XjsT(L"设置.关于.MFC不好看"), L"", false, false);
            addRow(ACT_NONE, CT_INFO, XjsT(L"设置.关于.自己写一个"), XjsT(L"设置.关于.目标一句话"), L"", false, false);
            newCard(XjsT(L"设置.关于.关于作者"));
            addRow(ACT_NONE, CT_INFO, XjsT(L"设置.关于.网贷经历"),
                   XjsT(L"设置.关于.停更预告"), L"", false, false);
            addRow(ACT_NONE, CT_INFO, XjsT(L"设置.关于.作者署名"), L"", L"", false, false);
            break;
        }
    }
    /* 卡片几何: 顺序摆放 (卡头 + 行块 + 内边距), 卡位由本遍统一分配, 不依赖构建期游标 */
    float cy = SS(20);
    for (auto& c : s_set.cards) {
        c.y = cy;
        float bottom = c.y + titleH;
        for (auto& r : c.rows) { r.y = bottom; bottom += r.h + rowGap; }
        c.h = (bottom - rowGap) - c.y + SS(10);
        cy = c.y + c.h + cardGap;
    }
    s_set.contentH = cy + SS(6);
    /* 输入字段几何随行模型失效: 重建后的模型里没有 CT_INPUT 行 (= 不在"数据维护"分类) 即字段不可见,
     * 须即刻失焦并清渲染矩形, 否则残留矩形仍被 XjsEditFieldMouseDown/Hit 命中 ——
     * 表现 = 在其它分类点旧位置聚焦到隐形输入框, 打字/回车全进它 (2026-09-15 实锤) */
    bool hasInput = false;
    for (auto& c : s_set.cards) {
        for (auto& r : c.rows)
            if (r.ctrl == CT_INPUT) { hasInput = true; break; }
        if (hasInput) break;
    }
    if (!hasInput) {
        s_set.pathEd.SetFocused(s_set.hwnd, false);
        s_set.pathEd.area = {};
    }
    if (!canMeasure) s_set.rowsDirty = true;   /* 资源未就绪的兜底构建: 首帧资源就绪后重测行高 */
    return true;
}

/* ==================== 捐赠二维码 (EXE RCDATA → 本窗口 RT 位图) ==================== */

static void XjsSetLoadDonateImages() {
    s_set.imgTried = true;
    const struct { int id; XjsBitmap** out; } entries[] = { { 201, &s_set.imgWechat }, { 202, &s_set.imgAlipay } };
    for (const auto& e : entries) {
        if (*e.out) continue;
        HRSRC rs = FindResourceW(NULL, MAKEINTRESOURCEW(e.id), RT_RCDATA);
        HGLOBAL hRes = rs ? LoadResource(NULL, rs) : NULL;
        const void* p = hRes ? LockResource(hRes) : NULL;
        DWORD sz = hRes ? SizeofResource(NULL, rs) : 0;
        if (!p || sz == 0) continue;
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sz);   /* 资源块不能直接交 CreateStreamOnHGlobal, 拷贝一份 */
        if (!hMem) continue;
        void* q = GlobalLock(hMem);
        if (!q) { GlobalFree(hMem); continue; }
        memcpy(q, p, sz);
        GlobalUnlock(hMem);
        IStream* stream = NULL;
        if (CreateStreamOnHGlobal(hMem, TRUE, &stream) != S_OK) { GlobalFree(hMem); continue; }
        IWICBitmapDecoder* dec = NULL;
        IWICBitmapFrameDecode* frame = NULL;
        IWICFormatConverter* conv = NULL;
        if (SUCCEEDED(g_wic->CreateDecoderFromStream(stream, NULL, WICDecodeMetadataCacheOnDemand, &dec)) && dec &&
            SUCCEEDED(dec->GetFrame(0, &frame)) && frame &&
            SUCCEEDED(g_wic->CreateFormatConverter(&conv)) && conv &&
            SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom))) {
            s_set.rt->CreateBitmapFromWicBitmap(conv, NULL, e.out);
        }
        if (conv) conv->Release();
        if (frame) frame->Release();
        if (dec) dec->Release();
        stream->Release();
    }
}

/* 单栏: 标题水平居中在上, 位图等比适配居中在下 */
static void XjsSetDrawImageCol(const wchar_t* title, XjsBitmap* bmp, float x0, float x1, float y0, float y1) {
    s_set.rt->DrawText(title, (UINT32)wcslen(title), s_set.tfNameC,
        XjsRectF(x0, y0, x1, y0 + SS(28)), s_set.brText);
    float availT = y0 + SS(32), availH = y1 - availT, availW = x1 - x0;
    if (!bmp || availW <= 0 || availH <= 0) return;
    XjsSizeU isz = bmp->GetPixelSize();
    if (!isz.width || !isz.height) return;
    float k = availW / isz.width < availH / isz.height ? availW / isz.width : availH / isz.height;
    float dw = isz.width * k, dh = isz.height * k;
    float dx = x0 + (availW - dw) / 2, dy = availT + (availH - dh) / 2;
    s_set.rt->DrawBitmap(bmp, XjsRectF(dx, dy, dx + dw, dy + dh));
}

/* 捐赠图片行: img=3 = 微信+支付宝并排两栏; img=1/2 = 单图整行 (整行自绘, 不走通用文本) */
static void XjsSetDrawImageRow(const XjsSetRow& r, float x0, float x1, float y0, float y1) {
    if (r.img == 3) {
        float mid = (x0 + x1) / 2, gap = SS(16);
        XjsSetDrawImageCol(XjsT(L"设置.捐赠.微信"), s_set.imgWechat, x0 + SS(18), mid - gap / 2, y0 + SS(6), y1 - SS(10));
        XjsSetDrawImageCol(XjsT(L"设置.捐赠.支付宝"), s_set.imgAlipay, mid + gap / 2, x1 - SS(18), y0 + SS(6), y1 - SS(10));
        return;
    }
    XjsBitmap* bmp = r.img == 1 ? s_set.imgWechat : (r.img == 2 ? s_set.imgAlipay : NULL);
    XjsSetDrawImageCol(r.name.c_str(), bmp, x0 + SS(18), x1 - SS(18), y0 + SS(6), y1 - SS(10));
}

/* ==================== 资源 (画刷/字体, 纪元+尺度变更重建) ==================== */

/* md 文档实例的生命周期 = 设置窗本身, 只随 WM_DESTROY 释放: 文档内容与 RT 无关,
   RT 绑定缓存由 xjs_md 按 RT/纪元/尺度自失效重建; 画刷重建路径 (换肤/DPI) 不置 rowsDirty,
   在此释放会让行模型 r.md 悬垂, 下一帧 XjsMdPaint 踩已释放内存 (2026-09-17 实锤崩溃) */
static void XjsSetFreeMdDocs() {
    for (size_t i = 0; i < sizeof(s_mdDoc) / sizeof(s_mdDoc[0]); i++)
        if (s_mdDoc[i]) { XjsMdFree(s_mdDoc[i]); s_mdDoc[i] = NULL; }
}

static void XjsSetFreeResources() {
    if (s_set.rt) { s_set.rt->Release(); s_set.rt = NULL; }
    if (s_set.imgWechat) { s_set.imgWechat->Release(); s_set.imgWechat = NULL; }
    if (s_set.imgAlipay) { s_set.imgAlipay->Release(); s_set.imgAlipay = NULL; }
    s_set.imgTried = false;
    /* 纯色画刷是独立 COM 对象 (不随 RT 释放), 只置空 = 每次换肤/DPI 变化泄漏一轮 */
    if (s_set.brBg) { s_set.brBg->Release(); }
    if (s_set.brBorder) { s_set.brBorder->Release(); }
    if (s_set.brHover) { s_set.brHover->Release(); }
    if (s_set.brText) { s_set.brText->Release(); }
    if (s_set.brDim) { s_set.brDim->Release(); }
    if (s_set.brFaint) { s_set.brFaint->Release(); }
    if (s_set.brAccent) { s_set.brAccent->Release(); }
    if (s_set.brPanel) { s_set.brPanel->Release(); }
    if (s_set.brPanel2) { s_set.brPanel2->Release(); }
    if (s_set.brBorderStrong) { s_set.brBorderStrong->Release(); }
    if (s_set.brDanger) { s_set.brDanger->Release(); }
    if (s_set.brWhite) { s_set.brWhite->Release(); }
    if (s_set.brAccentSoft) { s_set.brAccentSoft->Release(); }
    if (s_set.brMask) { s_set.brMask->Release(); }
    if (s_set.brOkFill) { s_set.brOkFill->Release(); }
    if (s_set.brOkHover) { s_set.brOkHover->Release(); }
    if (s_set.brWarn) { s_set.brWarn->Release(); }
    s_set.brBg = s_set.brBorder = s_set.brHover = s_set.brText = s_set.brDim = NULL;
    s_set.brFaint = s_set.brAccent = s_set.brPanel = s_set.brPanel2 = NULL;
    s_set.brBorderStrong = s_set.brDanger = s_set.brWhite = s_set.brAccentSoft = NULL;
    s_set.brMask = s_set.brOkFill = s_set.brOkHover = s_set.brWarn = NULL;
    if (s_set.brBgGrad) { s_set.brBgGrad->Release(); s_set.brBgGrad = NULL; }
    if (s_set.tfName) { s_set.tfName->Release(); s_set.tfName = NULL; }
    if (s_set.tfDesc) { s_set.tfDesc->Release(); s_set.tfDesc = NULL; }
    if (s_set.tfTitle) { s_set.tfTitle->Release(); s_set.tfTitle = NULL; }
    if (s_set.tfCat) { s_set.tfCat->Release(); s_set.tfCat = NULL; }
    if (s_set.tfBtn) { s_set.tfBtn->Release(); s_set.tfBtn = NULL; }
    if (s_set.tfNameC) { s_set.tfNameC->Release(); s_set.tfNameC = NULL; }
    if (s_set.tfDescC) { s_set.tfDescC->Release(); s_set.tfDescC = NULL; }
    s_set.brushEpoch = -1;
}

static void XjsSetEnsureResources(HWND hwnd) {
    float unit = SS(1);
    if (s_set.rt && s_set.brushEpoch == g_skinEpoch && s_set.unit == unit) return;
    XjsSetFreeResources();
    RECT rc;
    GetClientRect(hwnd, &rc);
    /* 后端窗口表面创建 (D2D=HwndRT/GDI+=内存位图; DPI 钉 96 的口径在后端内实现) */
    g_gfx->CreateWindowRt(hwnd, ximax(rc.right, 1), ximax(rc.bottom, 1), &s_set.rt);
    if (!s_set.rt) return;
    XjsColor bg1 = g_skin.bg1; bg1.a = 1.0f;   /* HwndRT 无逐像素 alpha */
    XjsColor bg2 = g_skin.bg2; bg2.a = 1.0f;
    s_set.rt->CreateSolidColorBrush(bg1, &s_set.brBg);
    s_set.rt->CreateSolidColorBrush(g_skin.border, &s_set.brBorder);
    s_set.rt->CreateSolidColorBrush(g_skin.rowHover, &s_set.brHover);
    s_set.rt->CreateSolidColorBrush(g_skin.text, &s_set.brText);
    s_set.rt->CreateSolidColorBrush(g_skin.textDim, &s_set.brDim);
    s_set.rt->CreateSolidColorBrush(g_skin.textFaint, &s_set.brFaint);
    s_set.rt->CreateSolidColorBrush(g_skin.accent, &s_set.brAccent);
    s_set.rt->CreateSolidColorBrush(g_skin.accentSoft, &s_set.brAccentSoft);
    /* 对话框专属: 画刷必须建在本窗口 RT 上, 跨 RT 用主窗画刷 = EndDraw 失败整帧不上屏 */
    s_set.rt->CreateSolidColorBrush(XjsCol(0x000000, 0.45f), &s_set.brMask);
    s_set.rt->CreateSolidColorBrush(XjsCol(0xe0442e), &s_set.brOkFill);
    s_set.rt->CreateSolidColorBrush(XjsCol(0xc93a26), &s_set.brOkHover);
    s_set.rt->CreateSolidColorBrush(g_skin.warn, &s_set.brWarn);
    s_set.rt->CreateSolidColorBrush(g_skin.panel, &s_set.brPanel);
    s_set.rt->CreateSolidColorBrush(g_skin.panel2, &s_set.brPanel2);
    s_set.rt->CreateSolidColorBrush(g_skin.borderStrong, &s_set.brBorderStrong);
    s_set.rt->CreateSolidColorBrush(XjsCol(0xe0442e), &s_set.brDanger);   /* 官方固定红 */
    s_set.rt->CreateSolidColorBrush(XjsColorF(1, 1, 1, 1), &s_set.brWhite);
    XjsGradientStop gs[2] = { { 0.0f, bg1 }, { 1.0f, bg2 } };
    s_set.rt->CreateLinearGradientBrush(XjsPoint2F(0, 0), XjsPoint2F(0, (FLOAT)rc.bottom + 1), gs, 2, &s_set.brBgGrad);
    float px = SS(1);
    g_dw->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 13 * px, L"zh-cn", &s_set.tfName);
    g_dw->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 11 * px, L"zh-cn", &s_set.tfDesc);
    g_dw->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 12 * px, L"zh-cn", &s_set.tfTitle);
    g_dw->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 12 * px, L"zh-cn", &s_set.tfCat);
    g_dw->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 12 * px, L"zh-cn", &s_set.tfBtn);
    /* 全部垂直居中: DirectWrite 默认顶对齐, 漏设=每行文字吊在行框上部 (弹窗菜单有设, 此处曾漏) */
    XjsFormat* fmts[] = { s_set.tfName, s_set.tfDesc, s_set.tfTitle, s_set.tfCat, s_set.tfBtn };
    for (auto* f : fmts)
        if (f) f->SetParagraphAlignment(XJS_PARA_CENTER);
    /* 单行文本关自动换行 (按钮/标签类): 实测宽矩形会因亚像素取整把末字挤到第二行; 说明文字 tfDesc 保留换行 */
    XjsFormat* nowrap[] = { s_set.tfName, s_set.tfTitle, s_set.tfCat, s_set.tfBtn };
    for (auto* f : nowrap)
        if (f) f->SetWordWrapping(XJS_WRAP_NONE);
    /* 说明文字 = 词换行 (显式钉死, 不依赖 CreateTextFormat 默认): 英文按空格断行不劈词,
       DWrite WORD 模式对中文仍逐字断行; GDI+ 后端同义 (GdiWrapLines 已按空格细分) */
    if (s_set.tfDesc) s_set.tfDesc->SetWordWrapping(XJS_WRAP_WORD);
    if (s_set.tfDescC) s_set.tfDescC->SetWordWrapping(XJS_WRAP_WORD);
    /* 水平居中变体: 捐赠页引导语 + 二维码栏标题 */
    g_dw->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 13 * px, L"zh-cn", &s_set.tfNameC);
    g_dw->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 11 * px, L"zh-cn", &s_set.tfDescC);
    if (s_set.tfNameC) {
        s_set.tfNameC->SetParagraphAlignment(XJS_PARA_CENTER);
        s_set.tfNameC->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        s_set.tfNameC->SetWordWrapping(XJS_WRAP_NONE);
    }
    if (s_set.tfDescC) {
        s_set.tfDescC->SetParagraphAlignment(XJS_PARA_CENTER);
        s_set.tfDescC->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    s_set.brushEpoch = g_skinEpoch;
    s_set.unit = unit;   /* 单位尺度入键: 页面缩放变化后文本格式按新倍率重建 (与 g_skinEpoch 同款失效) */
}

/* ==================== 控件绘制 ==================== */

/* 开关 (源样式 .switch: 42×24 胶囊轨道 + 18 白圆钮, 选中轨道=accent 钮右移);
   disabled=依赖项未开启的置灰态: 轨道用弱描边色、钮用弱文字色 (不可点) */
static void XjsSetDrawSwitch(float xRight, float cy, bool checked, bool disabled = false) {
    float tw = SS(42), th = SS(24);
    XjsRect tr = XjsRectF(xRight - tw, cy - th / 2, xRight, cy + th / 2);
    s_set.rt->FillRoundedRectangle(XjsRoundedRectF(tr, th / 2, th / 2),
        checked ? (disabled ? s_set.brBorder : s_set.brAccent) : (disabled ? s_set.brPanel2 : s_set.brBorderStrong));
    float d = SS(18);
    float cx = checked ? tr.right - SS(3) - d / 2 : tr.left + SS(3) + d / 2;
    s_set.rt->FillEllipse(XjsEllipseF(XjsPoint2F(cx, cy), d / 2, d / 2), disabled ? s_set.brFaint : s_set.brWhite);
}

/* 下拉弹窗锚点 = 行尾控件左下角 (客户区→屏幕; GetCursorPos 会随手位置漂移) */
static POINT XjsSetDropdownAnchor(const XjsSetRow& r) {
    RECT crc;
    GetClientRect(s_set.hwnd, &crc);
    float xRight = (float)crc.right - SS(28) - SS(18);   /* 与绘制/命中同式: 内容右缘 - 行尾留白 */
    float cy = r.y + r.h / 2 - s_set.scroll;             /* 内容坐标 → 客户区 */
    POINT a = { (LONG)(xRight - r.ctrlW), (LONG)(cy + SS(15)) };
    ClientToScreen(s_set.hwnd, &a);
    return a;
}

/* 信息胶囊 (热键/缩放值: panel-2 底 + 描边圆角, 同源样式 .hotkey-input); 文字在胶囊内水平居中
   warn=警告款 (已设置的全局热键注册失败): 描边与文字用警告色 */
static void XjsSetDrawPill(float xRight, float cy, const std::wstring& text, bool warn = false) {
    float textW = XjsMeasureText(text.c_str(), s_set.tfBtn);
    float tw = xf_max(textW + SS(24), SS(56)), th = SS(28);
    XjsRect tr = XjsRectF(xRight - tw, cy - th / 2, xRight, cy + th / 2);
    s_set.rt->FillRoundedRectangle(XjsRoundedRectF(tr, SS(8), SS(8)), s_set.brPanel2);
    s_set.rt->DrawRoundedRectangle(XjsRoundedRectF(tr, SS(8), SS(8)),
        warn ? (XjsBrush*)s_set.brWarn : (XjsBrush*)s_set.brBorder, 1.0f);
    s_set.rt->DrawText(text.c_str(), (UINT32)text.length(), s_set.tfBtn,
        XjsRectF(tr.left + (tw - textW) / 2, tr.top, tr.left + (tw + textW) / 2, tr.bottom),
        warn ? (XjsBrush*)s_set.brWarn : (XjsBrush*)s_set.brDim);
}

/* 按钮 (重建=红描边危险款, 链接=常规款, 同源样式 .btn / .btn-danger); 文字水平居中;
   dropdown=下拉选择器款: 右缘画 ∨ 箭头 (同主窗筛选器, 点击弹窗锚定控件左下) */
static void XjsSetDrawButton(float xRight, float cy, const std::wstring& text, bool danger, bool dropdown) {
    float textW = XjsMeasureText(text.c_str(), s_set.tfBtn);
    float tw = textW + SS(36) + (dropdown ? SS(12) : 0), th = SS(30);
    XjsRect tr = XjsRectF(xRight - tw, cy - th / 2, xRight, cy + th / 2);
    s_set.rt->DrawRoundedRectangle(XjsRoundedRectF(tr, SS(9), SS(9)),
        danger ? (XjsBrush*)s_set.brDanger : (XjsBrush*)s_set.brBorder, 1.0f);
    /* CJK 字形在行框内天然偏上 ~1.5px, 文本矩形整体下移补正 (主窗时间徽章同款手法) */
    s_set.rt->DrawText(text.c_str(), (UINT32)text.length(), s_set.tfBtn,
        XjsRectF(tr.left + SS(18), tr.top, tr.left + SS(18) + textW, tr.bottom),
        danger ? (XjsBrush*)s_set.brDanger : (XjsBrush*)s_set.brText);
    if (dropdown) {
        XjsBrush* ic = (XjsBrush*)s_set.brDim;
        float cx = xRight - SS(12);
        s_set.rt->DrawLine(XjsPoint2F(cx - SS(3), cy - SS(1.5f)), XjsPoint2F(cx, cy + SS(1.5f)), ic, 1.3f);
        s_set.rt->DrawLine(XjsPoint2F(cx, cy + SS(1.5f)), XjsPoint2F(cx + SS(3), cy - SS(1.5f)), ic, 1.3f);
    }
}

/* ==================== 重建确认对话框 (源样式 rebuildMask 一比一) ==================== */

static bool XjsSetDriveMemoed(const std::wstring& memo, wchar_t letter) {
    if (memo.empty()) return true;   /* 无记录 = 默认全勾 */
    for (size_t i = 0; i < memo.size(); i++)
        if (towupper(memo[i]) == letter) return true;
    return false;
}

static void XjsSetOpenRebuildDialog() {
    /* NTFS 固定分区枚举 (源样式由宿主 config.rebuildDrives 下发, 仅列 NTFS) */
    s_set.dlgDrives.clear();
    DWORD bits = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(bits & (1u << i))) continue;
        wchar_t letter = (wchar_t)(L'A' + i);
        wchar_t root[4] = { letter, L':', L'\\', 0 };
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        wchar_t label[64] = L"", fs[32] = L"";
        if (!GetVolumeInformationW(root, label, 64, NULL, NULL, NULL, fs, 32)) continue;
        if (_wcsicmp(fs, L"NTFS") != 0) continue;
        XjsSettingsState::DlgDrive d;
        d.letter = letter;
        d.label = label;
        s_set.dlgDrives.push_back(d);
    }
    /* 记住上次 (内存镜像, XjsSaveConfig 落盘 xjs_config.json); 无记录 = 默认勾 评分/大小/修改/别名 */
    for (int i = 0; i < 7; i++)
        s_set.dlgField[i] = g_rbSaved ? g_rbFields[i] : (i == 0 || i == 1 || i == 2 || i == 6);
    for (auto& d : s_set.dlgDrives) d.checked = XjsSetDriveMemoed(g_rbDrives, d.letter);
    s_set.dlgOpen = true;
    s_set.dlgErr.clear();
    s_set.dlgHover = -1;
    s_set.dlgHits.clear();
}

static void XjsSetCloseRebuildDialog(HWND hwnd) {
    s_set.dlgOpen = false;
    s_set.dlgErr.clear();
    if (hwnd) InvalidateRect(hwnd, NULL, FALSE);
}

static void XjsSetConfirmRebuild(HWND hwnd) {
    /* 校验 (源样式: 有勾选框且全未勾 → 拦截提示) */
    if (!s_set.dlgDrives.empty()) {
        bool any = false;
        for (auto& d : s_set.dlgDrives) any = any || d.checked;
        if (!any) {
            s_set.dlgErr = XjsT(L"重建.请勾选驱动器");
            InvalidateRect(hwnd, NULL, FALSE);
            return;
        }
    }
    /* 记住本次勾选 (内存镜像, 随 XjsSaveConfig 落盘 xjs_config.json) */
    for (int i = 0; i < 7; i++) g_rbFields[i] = s_set.dlgField[i];
    std::wstring dv;
    for (auto& d : s_set.dlgDrives)
        if (d.checked) { if (!dv.empty()) dv += L','; dv += d.letter; }
    g_rbDrives = dv;
    g_rbSaved = true;
    /* 盘符 JSON: ["C:\\","D:\\"] (空数组 = 全盘, ScanPath 内部处理) */
    std::wstring json = L"[";
    bool first = true;
    for (auto& d : s_set.dlgDrives) {
        if (!d.checked) continue;
        if (!first) json += L',';
        first = false;
        json += L"\"" + std::wstring(1, d.letter) + L":\\\\\"";
    }
    json += L"]";
    bool fields[7];
    for (int i = 0; i < 7; i++) fields[i] = s_set.dlgField[i];
    XjsSetCloseRebuildDialog(hwnd);
    XjsEngineRebuildEx(fields, json);
}

/* 复选框 (源样式 15px 圆角方, 选中 accent 底 + 白✓) */
static void XjsSetDrawCheckbox(XjsRect box, bool checked) {
    s_set.rt->FillRoundedRectangle(XjsRoundedRectF(box, SS(4), SS(4)),
        checked ? (XjsBrush*)s_set.brAccent : (XjsBrush*)s_set.brPanel2);
    s_set.rt->DrawRoundedRectangle(XjsRoundedRectF(box, SS(4), SS(4)),
        checked ? (XjsBrush*)s_set.brAccent : (XjsBrush*)s_set.brBorderStrong, 1.0f);
    if (checked) {
        XjsPoint2 c = { (box.left + box.right) / 2, (box.top + box.bottom) / 2 };
        float r = SS(3.6f);
        s_set.rt->DrawLine(XjsPoint2F(c.x - r, c.y), XjsPoint2F(c.x - r * 0.2f, c.y + r * 0.8f), s_set.brWhite, 1.8f);
        s_set.rt->DrawLine(XjsPoint2F(c.x - r * 0.2f, c.y + r * 0.8f), XjsPoint2F(c.x + r * 0.9f, c.y - r * 0.7f), s_set.brWhite, 1.8f);
    }
}

static void XjsSetDrawRebuildDialog(float w, float vh) {
    s_set.dlgHits.clear();
    const float dlgW = SS(430), padV = SS(18), padH = SS(20);
    const float itemH = SS(20), cb = SS(15);
    const int rowsF = 4;
    const int rowsD = (int)((s_set.dlgDrives.size() + 1) / 2);
    const float fieldsH = SS(12) * 2 + rowsF * itemH + (rowsF - 1) * SS(8);
    const float drivesH = s_set.dlgDrives.empty()
        ? SS(12) * 2 + SS(18) + SS(16) + SS(9) + itemH
        : SS(12) * 2 + SS(18) + SS(16) + SS(9) + rowsD * itemH + (rowsD - 1) * SS(7);
    const float dlgH = padV * 2 + SS(22) + SS(8) + SS(38) + SS(12) + fieldsH + SS(14) + drivesH + SS(14) + SS(30);
    float x0 = (w - dlgW) / 2, y0 = (vh - dlgH) / 2;
    if (y0 < SS(8)) y0 = SS(8);
    XjsRect dr = XjsRectF(x0, y0, x0 + dlgW, y0 + dlgH);
    s_set.dlgRect = dr;
    /* 遮罩层 (源样式 rgba(0,0,0,.45)) — 画刷必须是本 RT 的 */
    s_set.rt->FillRectangle(XjsRectF(0, 0, w, vh), s_set.brMask);
    /* 对话框本体 */
    s_set.rt->FillRoundedRectangle(XjsRoundedRectF(dr, SS(12), SS(12)), s_set.brPanel);
    s_set.rt->DrawRoundedRectangle(XjsRoundedRectF(dr, SS(12), SS(12)), s_set.brBorder, 1.0f);
    float cx = dr.left + padH, cy = dr.top + padV;
    /* 标题: 红色警告三角 + "重建索引" */
    {
        float ax = cx + SS(8), ayMid = cy + SS(15);
        float up = SS(1), dn = SS(12), half = SS(8.5f);
        XjsPoint2 top = { ax, ayMid - SS(6.5f) - up };
        XjsPoint2 lb = { ax - half, ayMid + dn - SS(6.5f) };
        XjsPoint2 rb = { ax + half, ayMid + dn - SS(6.5f) };
        s_set.rt->DrawLine(top, lb, s_set.brDanger, 1.6f);
        s_set.rt->DrawLine(lb, rb, s_set.brDanger, 1.6f);
        s_set.rt->DrawLine(rb, top, s_set.brDanger, 1.6f);
        s_set.rt->DrawLine(XjsPoint2F(ax, ayMid - SS(2.5f)), XjsPoint2F(ax, ayMid + SS(2)), s_set.brDanger, 1.6f);
        s_set.rt->FillEllipse(XjsEllipseF(XjsPoint2F(ax, ayMid + SS(4)), SS(1), SS(1)), s_set.brDanger);
        { const wchar_t* t = XjsT(L"设置.数据维护.重建索引");   /* 长度必须 wcslen: 硬编码 4 = 中文恰巧, 译文被截断 */
          s_set.rt->DrawText(t, (UINT32)wcslen(t), s_set.tfName,
            XjsRectF(cx + SS(24), cy - SS(2), dr.right - padH, cy + SS(22)), s_set.brText); }
    }
    cy += SS(22) + SS(8);
    /* 说明文字 */
    {
        const wchar_t* sub = XjsT(L"重建.确认说明");
        s_set.rt->DrawText(sub, (UINT32)wcslen(sub), s_set.tfDesc,
            XjsRectF(cx, cy, dr.right - padH, cy + SS(38)), s_set.brDim);
    }
    cy += SS(38) + SS(12);
    /* 字段勾选面板 (2 列) */
    {
        XjsRect fp = XjsRectF(cx, cy, dr.right - padH, cy + fieldsH);
        s_set.rt->FillRoundedRectangle(XjsRoundedRectF(fp, SS(9), SS(9)), s_set.brPanel2);
        s_set.rt->DrawRoundedRectangle(XjsRoundedRectF(fp, SS(9), SS(9)), s_set.brBorder, 1.0f);
        float colW = (fp.right - fp.left - SS(24) - SS(14)) / 2;
        for (int i = 0; i < 7; i++) {
            float bx = fp.left + SS(12) + (i % 2) * (colW + SS(14));
            float by = fp.top + SS(12) + (i / 2) * (itemH + SS(8));
            XjsRect box = XjsRectF(bx, by + (itemH - cb) / 2, bx + cb, by + (itemH + cb) / 2);
            XjsSetDrawCheckbox(box, s_set.dlgField[i]);
            { const wchar_t* nm_ = XjsT(RB_FIELDS[i]);
              s_set.rt->DrawText(nm_, (UINT32)wcslen(nm_), s_set.tfBtn,
                XjsRectF(box.right + SS(7), by, bx + colW, by + itemH), s_set.brText); }
            s_set.dlgHits.push_back({ XjsRectF(bx - SS(4), by - SS(2), bx + colW, by + itemH + SS(2)), DHB_FIELD + i, 0 });
        }
    }
    cy += fieldsH + SS(14);
    /* 驱动器面板 */
    {
        XjsRect dp = XjsRectF(cx, cy, dr.right - padH, cy + drivesH);
        s_set.rt->FillRoundedRectangle(XjsRoundedRectF(dp, SS(9), SS(9)), s_set.brPanel2);
        s_set.rt->DrawRoundedRectangle(XjsRoundedRectF(dp, SS(9), SS(9)), s_set.brBorder, 1.0f);
        float dy = dp.top + SS(12);
        { const wchar_t* t = XjsT(L"重建.选择驱动器");
          s_set.rt->DrawText(t, (UINT32)wcslen(t), s_set.tfName,
            XjsRectF(dp.left + SS(12), dy, dp.right - SS(12), dy + SS(18)), s_set.brText); }
        dy += SS(18) + SS(4);
        s_set.rt->DrawText(XjsT(L"重建.仅NTFS"), (UINT32)wcslen(XjsT(L"重建.仅NTFS")),
            s_set.tfDesc, XjsRectF(dp.left + SS(12), dy, dp.right - SS(12), dy + SS(16)), s_set.brDim);
        dy += SS(16) + SS(9);
        if (s_set.dlgDrives.empty()) {
            s_set.rt->DrawText(XjsT(L"重建.未检测到磁盘"), (UINT32)wcslen(XjsT(L"重建.未检测到磁盘")),
                s_set.tfBtn, XjsRectF(dp.left + SS(12), dy, dp.right - SS(12), dy + itemH), s_set.brDim);
        } else {
            float colW = (dp.right - dp.left - SS(24) - SS(14)) / 2;
            for (int i = 0; i < (int)s_set.dlgDrives.size(); i++) {
                auto& d = s_set.dlgDrives[i];
                float bx = dp.left + SS(12) + (i % 2) * (colW + SS(14));
                float by = dp.top + SS(12) + SS(18) + SS(4) + SS(16) + SS(9) + (i / 2) * (itemH + SS(7));
                XjsRect box = XjsRectF(bx, by + (itemH - cb) / 2, bx + cb, by + (itemH + cb) / 2);
                XjsSetDrawCheckbox(box, d.checked);
                std::wstring txt = std::wstring(1, d.letter) + L":\\" + (d.label.empty() ? L"" : (L" (" + d.label + L")"));
                s_set.rt->DrawText(txt.c_str(), (UINT32)txt.length(), s_set.tfBtn,
                    XjsRectF(box.right + SS(7), by, bx + colW, by + itemH), s_set.brText);
                s_set.dlgHits.push_back({ XjsRectF(bx - SS(4), by - SS(2), bx + colW, by + itemH + SS(2)), DHB_DRIVE + i, 0 });
            }
        }
    }
    cy += drivesH + SS(14);
    /* 按钮行 (右对齐): 取消 / 开始重建 (红实底); 文字按实测宽水平居中 (tfBtn 保持左对齐供勾选标签用) */
    {
        float btnH = SS(30), okW = SS(96), cancelW = SS(76);
        XjsRect okR = XjsRectF(dr.right - padH - okW, cy, dr.right - padH, cy + btnH);
        XjsRect cnR = XjsRectF(okR.left - SS(10) - cancelW, cy, okR.left - SS(10), cy + btnH);
        s_set.rt->FillRoundedRectangle(XjsRoundedRectF(cnR, SS(9), SS(9)),
            s_set.dlgHover == DHB_CANCEL ? (XjsBrush*)s_set.brHover : (XjsBrush*)s_set.brPanel2);
        s_set.rt->DrawRoundedRectangle(XjsRoundedRectF(cnR, SS(9), SS(9)),
            s_set.dlgHover == DHB_CANCEL ? (XjsBrush*)s_set.brBorderStrong : (XjsBrush*)s_set.brBorder, 1.0f);
        {
            const wchar_t* t = XjsT(L"通用词.取消");
            float tw = XjsMeasureText(t, s_set.tfBtn);
            s_set.rt->DrawText(t, (UINT32)wcslen(t), s_set.tfBtn,
                XjsRectF(cnR.left + (cancelW - tw) / 2, cnR.top, cnR.left + (cancelW + tw) / 2, cnR.bottom),
                s_set.brText);
        }
        s_set.rt->FillRoundedRectangle(XjsRoundedRectF(okR, SS(9), SS(9)),
            s_set.dlgHover == DHB_OK ? (XjsBrush*)s_set.brOkHover : (XjsBrush*)s_set.brOkFill);
        {
            const wchar_t* t = XjsT(L"重建.开始重建");
            float tw = XjsMeasureText(t, s_set.tfBtn);
            s_set.rt->DrawText(t, (UINT32)wcslen(t), s_set.tfBtn,
                XjsRectF(okR.left + (okW - tw) / 2, okR.top, okR.left + (okW + tw) / 2, okR.bottom),
                s_set.brWhite);
        }
        s_set.dlgHits.push_back({ cnR, DHB_CANCEL, SS(9) });
        s_set.dlgHits.push_back({ okR, DHB_OK, SS(9) });
    }
    /* 校验错误提示 (warn 色, 按钮行左侧) */
    if (!s_set.dlgErr.empty())
        s_set.rt->DrawText(s_set.dlgErr.c_str(), (UINT32)s_set.dlgErr.length(), s_set.tfBtn,
            XjsRectF(cx, cy + SS(4), x0 + dlgW - padH - SS(200), cy + SS(26)), s_set.brWarn);
}

/* ==================== 绘制 ==================== */

/* 右缘滚动条几何 (渲染/命中/拖拽同源, 唯一事实源): 返回 false = 无滚动条 (内容不超高) */
static bool XjsSetSbGeom(float w, float vh, float& thumbY, float& thumbH, float& maxScroll) {
    maxScroll = s_set.contentH - vh;
    if (maxScroll <= 0 || vh <= 1) return false;
    float trackH = vh - SS(8);
    thumbH = xf_max(SS(24), vh / s_set.contentH * trackH);
    thumbY = SS(4) + (float)((double)s_set.scroll / maxScroll * (double)(trackH - thumbH));
    return true;
}

/* 分类栏内容总高 (渲染/滚轮/滚动条几何同源, 与 XjsSetSideItems 的 y 累进公式一致):
   顶距 + n×(行高+间隙) + 底距 */
static float XjsSetSideContentH(int sn) {
    return sn > 0 ? SS(14) + sn * (SS(34) + SS(2)) + SS(10) : 0;
}

/* 分类栏右缘滚动条几何 (渲染/命中/拖拽同源): 返回 false = 分类不超高 → 无滚动条 (按需显示) */
static bool XjsSetSideSbGeom(float vh, float& thumbY, float& thumbH, float& maxScroll) {
    XjsSetSideItem side[SET_TREE_N];
    int sn = XjsSetSideItems(side, SET_TREE_N);
    maxScroll = XjsSetSideContentH(sn) - vh;
    if (maxScroll <= 0 || vh <= 1) return false;
    float trackH = vh - SS(8);
    thumbH = xf_max(SS(24), vh / XjsSetSideContentH(sn) * trackH);
    thumbY = SS(4) + (float)((double)s_set.sideScroll / maxScroll * (double)(trackH - thumbH));
    return true;
}

/* 滚动条悬停命中 (0=无 1=右缘内容条 2=分类栏条): thumb 矩形精确判定, 几何与渲染同源 */
static int XjsSetSbHoverAt(HWND hwnd, POINT pt) {
    RECT crc; GetClientRect(hwnd, &crc);
    float w = (float)crc.right, vh = (float)crc.bottom;
    float tY, tH, maxS;
    if (pt.x >= w - SS(6) && pt.x <= w - SS(3) && XjsSetSbGeom(w, vh, tY, tH, maxS) &&
        pt.y >= tY && pt.y <= tY + tH)
        return 1;
    float sideW = XjsSetSideW();
    if (pt.x >= sideW - SS(6) && pt.x <= sideW - SS(3) && XjsSetSideSbGeom(vh, tY, tH, maxS) &&
        pt.y >= tY && pt.y <= tY + tH)
        return 2;
    return 0;
}

/* CT_TROW 表格行第 col 格输入框矩形 (渲染 / 禁用格点选否决 两处同源)。
   cx0/cx1 = 内容区左右缘 (与绘制侧 sideW+SS(24) / w-SS(28) 同公式); 框区右缘让位行尾"删除"钮。
   3 列 (文件分类): 名称 26% / 类型 16% / 后缀 58%; 2 列 (别名): 路径 54% / 别名 46%; 格间距 SS(8) */
static void XjsSetTblColRect(const XjsSetRow& r, int col, float cx0, float cx1, float ry0, float ry1, XjsRect* out) {
    float boxR = cx1 - SS(18) - (r.ctrlW > 0 ? r.ctrlW + SS(10) : 0);
    float total = boxR - (cx0 + SS(18)) - SS(8) * (r.tblCols - 1);
    float x = cx0 + SS(18), w = 0;
    if (r.tblCols == 3) {
        static const float FR[3] = { 0.26f, 0.16f, 0.58f };
        for (int c = 0; c < col; c++) x += total * FR[c] + SS(8);
        w = total * FR[col];
    } else {
        static const float FR2[2] = { 0.54f, 0.46f };
        for (int c = 0; c < col; c++) x += total * FR2[c] + SS(8);
        w = total * FR2[col];
    }
    float fh = SS(30), cy = (ry0 + ry1) / 2;
    *out = XjsRectF(x, cy - fh / 2, x + w, cy + fh / 2);
}

/* 保留分类 (类型 0/255) 的"后缀"格 = 禁用态: 点它不聚焦不触发 (正式版 disabled 输入框口径)。
   行坐标按当前行模型现查 (与 WM_LBUTTONDOWN 的行命中同源) */
static bool XjsSetTblDisabledHit(HWND hwnd, POINT pt) {
    RECT crc;
    GetClientRect(hwnd, &crc);
    float cx0 = XjsSetSideW() + SS(24), cx1 = (float)crc.right - SS(28);
    float y = pt.y + s_set.scroll;
    for (auto& card : s_set.cards)
        for (auto& r : card.rows) {
            if (r.ctrl != CT_TROW || r.tblCols != 3 || y < r.y || y >= r.y + r.h) continue;
            if (!XjsSetTblTypeReserved(s_filterEds[r.trowIdx * 3 + 1]->ed.text)) return false;
            XjsRect box;
            XjsSetTblColRect(r, 2, cx0, cx1, r.y, r.y + r.h, &box);
            return XjsPtIn(box, pt);
        }
    return false;
}

static void XjsSetPaint(HWND hwnd) {
    XjsWindowScope scope(XjsSetOwner());   /* 绘制期作用域: XjsT/宏按 owner 解析 (语言每窗化, 分类树等在 BuildRows 作用域外也画) */
    XjsSetEnsureResources(hwnd);
    if (!s_set.rt) return;
    if (!s_set.pathEd.blink.Attached())   /* 输入字段组件: 绑定承载窗口 (局布重绘回调) */
        s_set.pathEd.Attach(hwnd, [hwnd] { InvalidateRect(hwnd, NULL, FALSE); });
    if (s_set.rt) {   /* md 行高已可排 (上次构建时 RT 未就绪): 重建行模型 */
        bool anyPending = false;
        for (bool b : s_mdPending) anyPending = anyPending || b;
        if (anyPending) s_set.rowsDirty = true;
    }
    XjsSetBuildRows();
    /* 捐赠二维码按需解码: 本帧行模型里出现图片行 (捐赠页) 才解 — 不按分类下标判定,
     * 分类插入/换序后旧下标会静默失灵 (曾是 cat==4, "打开"分类插入后 4=数据维护,
     * 首进捐赠页永不解码; 切到数据维护再回来才显图, 2026-09-16 实锤)。
     * 换肤/RT 重建后 imgTried 复位会重解码 */
    bool hasImageRow = false;
    for (auto& c : s_set.cards) {
        for (auto& r : c.rows)
            if (r.ctrl == CT_IMAGE) { hasImageRow = true; break; }
        if (hasImageRow) break;
    }
    if (hasImageRow && !s_set.imgTried) XjsSetLoadDonateImages();
    XjsSizeU sz = s_set.rt->GetPixelSize();
    float w = (float)sz.width, vh = (float)sz.height;
    float maxScroll = s_set.contentH - vh;
    if (maxScroll < 0) maxScroll = 0;
    if (s_set.scroll > maxScroll) s_set.scroll = maxScroll;
    if (s_set.scroll < 0) s_set.scroll = 0;

    float sideW = XjsSetSideW();
    float cx0 = sideW + SS(24), cx1 = w - SS(28);

    /* 分类栏滚动夹取 (折叠/分类增删后内容变矮即归位; 不超高恒 0 = 滚动条隐藏) */
    XjsSetSideItem side[SET_TREE_N];
    int sn = XjsSetSideItems(side, SET_TREE_N);
    {
        float smax = XjsSetSideContentH(sn) - vh;
        if (smax < 0) smax = 0;
        if (s_set.sideScroll > smax) s_set.sideScroll = smax;
        if (s_set.sideScroll < 0) s_set.sideScroll = 0;
    }

    s_set.rt->BeginDraw();
    if (s_set.brBgGrad) s_set.rt->FillRectangle(XjsRectF(0, 0, w, vh), s_set.brBgGrad);
    else s_set.rt->Clear(s_set.brBg->GetColor());

    /* ---- 侧边栏分类 (两层树: 窗口设置分组可折叠, 子级缩进; 内容超高随 sideScroll 滚动) ---- */
    s_set.rt->FillRectangle(XjsRectF(sideW, 0, sideW + 1, vh), s_set.brBorder);
    /* 树连接线 (画在条目下层: 分组行底→末子级行中心竖线 + 每子级中心横向短线, 末端触子级行左缘;
       颜色/线宽同分组折叠箭头 brDim/1.2; 坐标同条目绘制随 sideScroll 平移, RT 边界自动裁剪) */
    {
        const float v = SS(10) + SS(7);        /* 竖线 x = 子级缩进 (SS14) 中点 */
        const float tickL = SS(10) + SS(14);   /* 横线右端 = 子级行左缘 (连接到节点) */
        for (int si = 0; si < sn; si++) {
            const XjsSetCatNode& nd = SET_TREE[side[si].idx];
            if (!nd.grp) continue;
            int lastSi = si;
            for (int c = si + 1; c < sn && SET_TREE[side[c].idx].lvl > 0; c++) lastSi = c;
            if (lastSi == si) continue;   /* 分组收起 (无可见子级): 不画线 */
            float y0 = side[si].y - s_set.sideScroll + SS(34);          /* 分组行底 */
            float y1 = side[lastSi].y - s_set.sideScroll + SS(34) / 2;  /* 末子级行中心 */
            s_set.rt->DrawLine(XjsPoint2F(v, y0), XjsPoint2F(v, y1), s_set.brDim, SS(1.2f));
            for (int c = si + 1; c <= lastSi; c++) {
                float cy = side[c].y - s_set.sideScroll + SS(34) / 2;
                s_set.rt->DrawLine(XjsPoint2F(v, cy), XjsPoint2F(tickL, cy), s_set.brDim, SS(1.2f));
            }
        }
    }
    for (int si = 0; si < sn; si++) {
        const XjsSetCatNode& nd = SET_TREE[side[si].idx];
        float chh = SS(34);
        float sy = side[si].y - s_set.sideScroll;   /* 条目随分类栏滚动平移 */
        if (sy + chh < 0 || sy > vh) continue;      /* 滚出视口的条目不画 */
        float ind = nd.lvl ? SS(14) : 0;   /* 子级缩进一级 (树形层次) */
        XjsRect br = XjsRectF(SS(10) + ind, sy, sideW - SS(10), sy + chh);
        bool sel = nd.cat == s_set.cat;
        if (sel) {
            s_set.rt->FillRoundedRectangle(XjsRoundedRectF(br, SS(8), SS(8)), s_set.brAccentSoft);
            { const wchar_t* nm_ = XjsT(nd.name);
              s_set.rt->DrawText(nm_, (UINT32)wcslen(nm_), s_set.tfCat,
                XjsRectF(br.left + SS(12), br.top, br.right, br.bottom), s_set.brAccent); }
        } else {
            if (si == s_set.hoverSide)
                s_set.rt->FillRoundedRectangle(XjsRoundedRectF(br, SS(8), SS(8)), s_set.brHover);
            { const wchar_t* nm_ = XjsT(nd.name);
              s_set.rt->DrawText(nm_, (UINT32)wcslen(nm_), s_set.tfCat,
                XjsRectF(br.left + SS(12), br.top, br.right, br.bottom),
                nd.grp ? s_set.brText : s_set.brDim); }
        }
        /* 分组节点的折叠箭头 (文本左侧小折线: 展开=向下 V, 收起=向右 >) */
        if (nd.grp) {
            float ax = br.left + SS(5), ay = sy + chh / 2;
            float dx = SS(3), dy = SS(2);
            XjsPoint2 apex = s_set.winTreeOpen ? XjsPoint2F(ax, ay + dy) : XjsPoint2F(ax + dx, ay);
            XjsPoint2 tail = s_set.winTreeOpen ? XjsPoint2F(ax - dx, ay - dy) : XjsPoint2F(ax - dx, ay - dy);
            XjsPoint2 tip  = s_set.winTreeOpen ? XjsPoint2F(ax + dx, ay - dy) : XjsPoint2F(ax - dx, ay + dy);
            s_set.rt->DrawLine(apex, tail, s_set.brDim, SS(1.2f));
            s_set.rt->DrawLine(apex, tip, s_set.brDim, SS(1.2f));
        }
    }

    /* ---- 分类栏右缘细滚动条 (分类不超高 = 不显示, 按需; 样式同右缘内容条, 贴分隔线内侧) ---- */
    {
        float sbSY, sbSH, sbSMax;
        if (XjsSetSideSbGeom(vh, sbSY, sbSH, sbSMax))
            s_set.rt->FillRoundedRectangle(
                XjsRoundedRectF(XjsRectF(sideW - SS(6), sbSY, sideW - SS(3), sbSY + sbSH), SS(1.5f), SS(1.5f)),
                s_set.sbHover == 2 ? s_set.brFaint : s_set.brBorderStrong);
    }

    /* ---- 内容卡片 ---- */
    /* 帧边界即清输入字段几何: 矩形只在本帧真画到输入行时由 Render 回写 —
     * 行被滚出视口/不在本分类的帧结束后矩形为空, 命中/I-beam 不再落在看不见的输入框上 */
    s_set.pathEd.area = {};
    for (auto& f : s_filterEds) f->area = {};
    for (auto& f : s_aliasEds) f->area = {};
    for (auto& card : s_set.cards) {
        float cy0 = card.y - s_set.scroll, cy1 = cy0 + card.h;
        if (cy1 < 0 || cy0 > vh) continue;
        XjsRect cr = XjsRectF(cx0, cy0, cx1, cy1);
        s_set.rt->FillRoundedRectangle(XjsRoundedRectF(cr, SS(12), SS(12)), s_set.brPanel);
        s_set.rt->DrawRoundedRectangle(XjsRoundedRectF(cr, SS(12), SS(12)), s_set.brBorder, 1.0f);
        /* 卡片标题: 小字 600 faint + 底部细分隔线 (源样式 .group-title) */
        s_set.rt->DrawText(card.title.c_str(), (UINT32)card.title.length(), s_set.tfTitle,
            XjsRectF(cx0 + SS(18), cy0 + SS(8), cx1 - SS(18), cy0 + SS(34)), s_set.brFaint);
        s_set.rt->FillRectangle(XjsRectF(cx0 + SS(18), cy0 + SS(38), cx1 - SS(18), cy0 + SS(38) + 1), s_set.brBorder);
        for (auto& r : card.rows) {
            float ry0 = r.y - s_set.scroll, ry1 = ry0 + r.h;
            if (ry1 < cy0 || ry0 > cy1) continue;
            /* 视口外行一律不画: 卡带判界对"整页一卡"的表格页是空裁 (行必在卡带内),
               别名页 ~700 行曾逐帧全量渲染 1400 个输入框 = 滚动/悬停绘制卡顿 (2026-09-22 实锤) */
            if (ry1 < 0 || ry0 > vh) continue;
            /* hoverRow = 卡序×1000 + 卡内行序 (与 WM_MOUSEMOVE 编码一致) */
            bool hovered = ((&card - &s_set.cards[0]) * 1000 + (&r - &card.rows[0])) == s_set.hoverRow;
            if (hovered) {
                s_set.rt->FillRoundedRectangle(
                    XjsRoundedRectF(XjsRectF(cx0 + SS(8), ry0 + 1, cx1 - SS(8), ry1 - 1), SS(8), SS(8)), s_set.brHover);
            }
            float rcy = (ry0 + ry1) / 2;
            /* 行尾右侧控件 (先画, 文本避让) */
            switch (r.ctrl) {
                case CT_SWITCH: XjsSetDrawSwitch(cx1 - SS(18), rcy, r.checked, r.disabled); break;
                case CT_PILL:   XjsSetDrawPill(cx1 - SS(18), rcy, r.value,
                                      r.act == ACT_HOTKEY && XjsSetOwner() &&
                                      XjsSetOwner()->hotkeyVk && !XjsSetOwner()->hotkeyActive); break;
                case CT_BUTTON: XjsSetDrawButton(cx1 - SS(18), rcy, r.value, r.danger, XjsSetActIsDropdown(r.act)); break;
                case CT_IMAGE:  XjsSetDrawImageRow(r, cx0, cx1, ry0, ry1); break;
                case CT_MDDOC:  /* 内置 md 文档整行自绘 (xjs_md.cpp), 裁剪到本行可视带 */
                    if (r.md)
                        XjsMdPaint(r.md, s_set.rt, cx0 + SS(18), ry0, SS(1), xf_max(ry0, 0.0f), xf_min(ry1, vh));
                    break;
                case CT_INPUT: {   /* 输入字段组件: 宿主画底框, 字段自绘 文本/选区/光标 */
                    float fw = SS(260), fh = SS(30);
                    XjsRect fr = XjsRectF(cx1 - SS(18) - fw, rcy - fh / 2, cx1 - SS(18), rcy + fh / 2);
                    s_set.rt->FillRoundedRectangle(XjsRoundedRectF(fr, SS(8), SS(8)), s_set.brPanel2);
                    s_set.rt->DrawRoundedRectangle(XjsRoundedRectF(fr, SS(8), SS(8)),
                        s_set.pathEd.focused ? (XjsBrush*)s_set.brAccent : (XjsBrush*)s_set.brBorderStrong, 1.0f);
                    s_set.pathEd.Render(s_set.rt,
                        XjsRectF(fr.left + SS(10), fr.top, fr.right - SS(10), fr.bottom),
                        s_set.tfName, s_set.brText, s_set.brAccentSoft, s_set.brText,
                        L"F:\\example", s_set.brFaint);
                    break;
                }
                case CT_TROW: {   /* 表格编辑行: 每格一个 XjsEditField (文本即活数据), 行尾"删除"钮走通用绘制 */
                    auto& pool = (r.tblCols == 3) ? s_filterEds : s_aliasEds;
                    const wchar_t* ph3[3] = { XjsT(L"设置.文件分类.占位.名称"), XjsT(L"设置.文件分类.占位.类型"), XjsT(L"设置.文件分类.占位.后缀") };
                    const wchar_t* ph2[2] = { XjsT(L"设置.别名.占位.路径"), XjsT(L"设置.别名.占位.别名") };
                    for (int col = 0; col < r.tblCols; col++) {
                        XjsEditField* f = pool[r.trowIdx * r.tblCols + col].get();
                        if (!f->blink.Attached())   /* 设置窗重开: 组件统一退登记后按需重挂 (文本保留无碍) */
                            f->Attach(hwnd, [hwnd] { InvalidateRect(hwnd, NULL, FALSE); });
                        bool reserved = (r.tblCols == 3 && col == 2 &&
                                         XjsSetTblTypeReserved(pool[r.trowIdx * 3 + 1]->ed.text));
                        XjsRect box;
                        XjsSetTblColRect(r, col, cx0, cx1, ry0, ry1, &box);
                        s_set.rt->FillRoundedRectangle(XjsRoundedRectF(box, SS(8), SS(8)), s_set.brPanel2);
                        s_set.rt->DrawRoundedRectangle(XjsRoundedRectF(box, SS(8), SS(8)),
                            f->focused ? (XjsBrush*)s_set.brAccent : (XjsBrush*)s_set.brBorderStrong, 1.0f);
                        f->Render(s_set.rt,
                            XjsRectF(box.left + SS(10), box.top, box.right - SS(10), box.bottom),
                            s_set.tfName, s_set.brText, s_set.brAccentSoft, s_set.brText,
                            reserved ? XjsT(L"设置.文件分类.无需后缀") : (r.tblCols == 3 ? ph3[col] : ph2[col]),
                            s_set.brFaint);
                    }
                    XjsSetDrawButton(cx1 - SS(18), rcy, r.value, r.danger, false);   /* 行尾"删除" */
                    break;
                }
                default: break;
            }
            /* 第二行尾按钮: 主钮左侧 SS(10) (几何与命中 XjsSetRowControl2Hit / 让位 XjsSetRowCtrlSpan 同源) */
            if (r.act2 != ACT_NONE && r.ctrlW2 > 0)
                XjsSetDrawButton(cx1 - SS(18) - r.ctrlW - SS(10), rcy, r.value2, r.danger2, false);
            if (r.ctrl == CT_IMAGE || r.ctrl == CT_MDDOC) continue;   /* 图片/文档行整行自绘, 不走通用文本 */
            float textL = cx0 + (r.ctrl == CT_OPTION ? SS(44) : SS(18));   /* 选项行留 ✓ 槽位 */
            float ctrlSpan = XjsSetRowCtrlSpan(r);
            float textR = (r.ctrl == CT_INFO || r.ctrl == CT_OPTION) ? cx1 - SS(18)
                        : (r.ctrl == CT_INPUT ? cx1 - SS(18) - SS(272)
                                              : cx1 - SS(18) - (ctrlSpan > 0 ? ctrlSpan + SS(14) : 0));   /* 与构建同源: 按控件组实宽让位 */
            /* 名称 + 描述 (源样式 .row-label; centerText 行用居中变体; link 行名称 accent 色 = 链接观感;
               disabled 行整体置灰 = 依赖项未开启) */
            XjsBrush* nameBr = r.disabled ? (XjsBrush*)s_set.brFaint
                              : r.link ? (XjsBrush*)s_set.brAccent
                              : (r.ctrl == CT_OPTION && r.checked) ? (XjsBrush*)s_set.brAccent
                              : (XjsBrush*)s_set.brText;
            if (r.desc.empty()) {
                XjsRect nr = XjsRectF(textL, ry0, textR, ry1);
                s_set.rt->DrawText(r.name.c_str(), (UINT32)r.name.length(),
                    r.centerText ? s_set.tfNameC : s_set.tfName, nr, nameBr);
            } else if (r.block) {
                /* 长文本块行: 名称固定行顶, 说明占其余整块 — 上下两半居中排布会把多行文本挤出行框 */
                s_set.rt->DrawText(r.name.c_str(), (UINT32)r.name.length(),
                    r.centerText ? s_set.tfNameC : s_set.tfName,
                    XjsRectF(textL, ry0 + SS(6), textR, ry0 + SS(26)), nameBr);
                s_set.rt->DrawText(r.desc.c_str(), (UINT32)r.desc.length(),
                    r.centerText ? s_set.tfDescC : s_set.tfDesc,
                    XjsRectF(textL, ry0 + SS(26), textR, ry1 - SS(6)),
                    r.warnText ? (XjsBrush*)s_set.brWarn : (XjsBrush*)s_set.brFaint);
            } else {
                float mid = ry0 + (ry1 - ry0) / 2 - SS(2);
                s_set.rt->DrawText(r.name.c_str(), (UINT32)r.name.length(),
                    r.centerText ? s_set.tfNameC : s_set.tfName,
                    XjsRectF(textL, ry0 + SS(8), textR, mid), nameBr);
                s_set.rt->DrawText(r.desc.c_str(), (UINT32)r.desc.length(),
                    r.centerText ? s_set.tfDescC : s_set.tfDesc,
                    XjsRectF(textL, mid - SS(2), textR, ry1 - SS(6)),
                    r.warnText ? (XjsBrush*)s_set.brWarn : (XjsBrush*)s_set.brFaint);
            }
            /* 选项行 ✓ (视图/皮肤当前项, 同勾选画法) */
            if (r.ctrl == CT_OPTION && r.checked) {
                XjsPoint2 c = { cx0 + SS(28), rcy };
                float rk = SS(4);
                s_set.rt->DrawLine(XjsPoint2F(c.x - rk, c.y), XjsPoint2F(c.x - rk * 0.2f, c.y + rk * 0.8f), s_set.brAccent, 2.0f);
                s_set.rt->DrawLine(XjsPoint2F(c.x - rk * 0.2f, c.y + rk * 0.8f), XjsPoint2F(c.x + rk * 0.9f, c.y - rk * 0.7f), s_set.brAccent, 2.0f);
            }
        }
    }

    /* ---- 右缘细滚动条 (内容超高时, 同源样式细 thumb; 几何与命中/拖拽同源) ---- */
    float sbY, sbH, sbMax;
    if (XjsSetSbGeom(w, vh, sbY, sbH, sbMax))
        s_set.rt->FillRoundedRectangle(
            XjsRoundedRectF(XjsRectF(w - SS(6), sbY, w - SS(3), sbY + sbH), SS(1.5f), SS(1.5f)),
            s_set.sbHover == 1 ? s_set.brFaint : s_set.brBorderStrong);
    /* ---- 重建确认对话框 (遮罩层置顶) ---- */
    if (s_set.dlgOpen) XjsSetDrawRebuildDialog(w, vh);
    XjsToastRender(hwnd, s_set.rt, w, vh, SS(1));   /* Toast 组件 (宿主=设置窗, 浮于一切) */
    s_set.rt->EndDraw();
}

/* ==================== 交互 ==================== */

/* 圆角矩形形状命中: 先过包围矩形, 四角 1/4 圆弧带内用圆方程判定 (rad=高/2 的
   胶囊全圆端自动覆盖); 视觉圆角外(直角死区)不可点 */
static bool XjsSetRoundedHit(float x, float y, float l, float t, float r, float b, float rad) {
    if (x < l || x > r || y < t || y > b) return false;
    float mx = (x - l < r - x) ? (x - l) : (r - x);
    float my = (y - t < b - y) ? (y - t) : (b - y);
    if (mx >= rad || my >= rad) return true;   /* 中央区: 不在任何角的圆弧带内 */
    float dx = rad - mx, dy = rad - my;
    return dx * dx + dy * dy <= rad * rad;
}

/* 对话框命中区判定 (rad>0 = 圆角形状, 0 = 普通矩形) */
static bool XjsSetDlgHitTest(const XjsSettingsState::DlgHit& h, float x, float y) {
    if (h.rad > 0)
        return XjsSetRoundedHit(x, y, h.rc.left, h.rc.top, h.rc.right, h.rc.bottom, h.rad);
    return x >= h.rc.left && x <= h.rc.right && y >= h.rc.top && y <= h.rc.bottom;
}

/* 行尾右侧控件的命中区 — 与绘制几何严格同源 (XjsSetDrawSwitch/Pill/Button 都是
   行内垂直居中的小块): 开关 42×24 胶囊(全圆端) / 胶囊 max(文本+24,56)×28 圆角8 /
   按钮 文本+36×30 圆角9, 右缘都在 cx1-SS(18)。命中=控件矩形+圆角形状, 不再外扩:
   行内控件上下的空白曾是整行高命中 → 点开关上方/下方空白也触发 (误触), 四角
   圆角外的直角死区也不可点。点击行文本区仍不触发行动作 */
static bool XjsSetRowControlHit(const XjsSetRow& r, float x, float y, float cx1) {
    float xRight = cx1 - SS(18);
    float h, rad;
    switch (r.ctrl) {
        case CT_SWITCH: h = SS(24); rad = h / 2; break;
        case CT_PILL:   h = SS(28); rad = SS(8); break;
        case CT_BUTTON: h = SS(30); rad = SS(9); break;
        case CT_TROW:   h = SS(30); rad = SS(9); break;   /* 行尾"删除"钮, 与按钮同几何 */
        default:
            return false;   /* INFO/OPTION/IMAGE/INPUT/TROW其余区 无右侧控件 */
    }
    if (r.ctrlW <= 0) return false;   /* 构建时实测的控件宽 (绘制/命中/文本右缘同源) */
    float cy = r.y + r.h / 2;   /* 绘制处 rcy=(ry0+ry1)/2 的内容坐标 (scroll 两侧抵消) */
    return XjsSetRoundedHit(x, y, xRight - r.ctrlW, cy - h / 2, xRight, cy + h / 2, rad);
}

/* 第二行尾按钮的命中区 (CT_BUTTON 专属; 几何与绘制同源: 主钮左侧 SS(10) 等高同圆角) */
static bool XjsSetRowControl2Hit(const XjsSetRow& r, float x, float y, float cx1) {
    if (r.act2 == ACT_NONE || r.ctrlW2 <= 0) return false;
    float xRight = cx1 - SS(18);
    float h = SS(30), rad = SS(9);
    float cy = r.y + r.h / 2;
    return XjsSetRoundedHit(x, y, xRight - r.ctrlW - SS(10) - r.ctrlW2, cy - h / 2,
                            xRight - r.ctrlW - SS(10), cy + h / 2, rad);
}

/* 排除目录手动输入: Enter/调用入口 — 规范化后直接下发引擎 (即时生效) */
static void XjsSetAddExcludedPath(HWND hwnd) {
    std::wstring p = s_set.pathEd.ed.text;
    /* 去首尾空白与引号 (资源管理器"复制文件地址"带引号) */
    size_t b = p.find_first_not_of(L" \t\"");
    if (b == std::wstring::npos) return;
    size_t e = p.find_last_not_of(L" \t\"");
    p = p.substr(b, e - b + 1);
    if (xjs_db_AddExcludedDir(g_engine, Utf16ToUtf8(p.c_str()).c_str())) {
        s_set.pathEd.ed.Clear();
        s_set.pathEd.ed.dirty = false;   /* dirty 语义是"文本被改", 路径框无观察者, 复位防误读 */
        s_set.rowsDirty = true;
        XjsToastShow(s_set.hwnd, XjsT(L"重建.排除目录提示已排除"), XTOAST_SUCCESS, SS(1));
    } else {
        XjsToastShow(s_set.hwnd, XjsT(L"重建.添加失败"), XTOAST_WARN, SS(1));
    }
    InvalidateRect(hwnd, NULL, FALSE);
}

/* 表格保存失败提示 (正式版同款分错误码; Apply 假 = 引擎拒绝, xjs_GetLastError 取码) */
static void XjsSetTblSaveFailToast(int err) {
    if (err == 30)
        XjsToastShow(s_set.hwnd, XjsT(L"错误.表格配置无效"), XTOAST_WARN, SS(1));
    else if (err == 35)
        XjsToastShow(s_set.hwnd, XjsT(L"错误.数据库正忙"), XTOAST_WARN, SS(1));
    else
        XjsToastShow(s_set.hwnd, XjsT(L"错误.表格保存失败"), XTOAST_WARN, SS(1));
}

/* 保存文件分类: 收池内文本逐行校验 (正式版同款报错口径) → 整体下发; 成功 = 回读生效配置回显 */
static void XjsSetFilterSave() {
    int n = XjsSetFilterRows();
    std::vector<XjsFilterItem> rows;
    bool seen[256] = {};
    for (int i = 0; i < n; i++) {
        XjsFilterItem it;
        it.name = XjsTrimWs(s_filterEds[i * 3 + 0]->ed.text);
        int t = XjsSetTblTypeParse(s_filterEds[i * 3 + 1]->ed.text);
        if (it.name.empty()) {
            XjsToastShow(s_set.hwnd, XjsT(L"错误.分类名称为空"), XTOAST_WARN, SS(1));
            return;
        }
        if (t < 0 || t > 255) {
            XjsToastShow(s_set.hwnd, XjsT(L"错误.分类类型范围"), XTOAST_WARN, SS(1));
            return;
        }
        if (t != 0 && t != 255 && seen[t]) {   /* 0=全部 255=文件夹 系统保留, 1~254 自定义不可重复 */
            XjsToastShow(s_set.hwnd, XjsFmt(XjsT(L"错误.分类类型重复"), std::to_wstring(t)).c_str(), XTOAST_WARN, SS(1));
            return;
        }
        if (t >= 0 && t <= 255) seen[t] = true;
        it.type = t;
        it.ext = (t == 0 || t == 255) ? L"" : XjsSetTblNormExts(s_filterEds[i * 3 + 2]->ed.text);
        rows.push_back(it);
    }
    if (XjsFilterConfigApply(rows)) {
        s_filterReload = true;   /* 行模型重建时回读引擎生效配置 (类型排序/后缀规范化后回显) */
        XjsToastShow(s_set.hwnd, XjsT(L"提示.分类已保存"), XTOAST_SUCCESS, SS(1));
    } else {
        XjsSetTblSaveFailToast(g_engine ? xjs_GetLastError(g_engine) : 0);
    }
}

/* 保存别名: 路径去空格非空 + 不重复 (正式版同款) → 整体下发 (sync=TRUE 立即应用现有库) */
static void XjsSetAliasSave() {
    int n = XjsSetAliasRows();
    std::vector<XjsAliasItem> rows;
    for (int i = 0; i < n; i++) {
        XjsAliasItem it;
        it.path = XjsTrimWs(s_aliasEds[i * 2 + 0]->ed.text);
        it.alias = XjsTrimWs(s_aliasEds[i * 2 + 1]->ed.text);
        if (it.path.empty()) {
            XjsToastShow(s_set.hwnd, XjsT(L"错误.别名路径为空"), XTOAST_WARN, SS(1));
            return;
        }
        bool dup = false;
        for (auto& r : rows) if (r.path == it.path) { dup = true; break; }   /* 行数少, 线性查重足够 */
        if (dup) {
            XjsToastShow(s_set.hwnd, XjsFmt(XjsT(L"错误.别名路径重复"), it.path).c_str(), XTOAST_WARN, SS(1));
            return;
        }
        rows.push_back(it);
    }
    if (XjsAliasConfigApply(rows)) {
        s_aliasReload = true;   /* 回读引擎生效配置 (占位符已展开为绝对路径) */
        XjsToastShow(s_set.hwnd, XjsT(L"提示.别名已保存"), XTOAST_SUCCESS, SS(1));
    } else {
        XjsSetTblSaveFailToast(g_engine ? xjs_GetLastError(g_engine) : 0);
    }
}

/* 链接行命中 (关于页 GLM 官网行): 手型光标用。按行模型现查 (y 补回滚动量),
   x 须在内容区 (侧边栏纵列上不因行 y 相同而误显手型) */
static bool XjsSetLinkRowHit(HWND hwnd, POINT pt) {
    float sideW = XjsSetSideW();
    if (pt.x < sideW) return false;
    RECT crc;
    GetClientRect(hwnd, &crc);
    if (pt.x >= crc.right - SS(10)) return false;   /* 右缘滚动条带不算 */
    float y = pt.y + s_set.scroll;
    for (auto& c : s_set.cards)
        for (auto& r : c.rows)
            if (r.link && y >= r.y && y < r.y + r.h) return true;
    return false;
}

/* 设置窗标题跟随 owner 窗口名称 (重命名/删除档案后同步刷新); 无 owner 回落主窗固定名 */
static void XjsSetBuildTitle(wchar_t* out, int cap) {
    _snwprintf(out, cap, XjsT(L"应用.设置窗口标题"),
               s_setOwner ? s_setOwner->name.c_str() : XJS_MAIN_WIN_NAME);
}

/* 设置窗正打开且作用于该搜索窗 (主窗"失焦关闭"豁免判定): 设置窗抢走焦点不该把
   正在配置的窗口关掉; owner 已关时 XjsSetOwner 回落主窗, 判定自然为假 */
bool XjsSettingsOpenFor(HWND ownerSearchHwnd) {
    if (!s_set.hwnd || !IsWindowVisible(s_set.hwnd)) return false;
    XjsSearchWindow* ow = XjsSetOwner();
    return ow && ow->hWnd == ownerSearchHwnd;
}

/* ==================== 窗口改名 (唯一入口) ====================
 * 窗口名称行 (ACT_WINNAME, 作用 owner) 与 窗口管理行 (ACT_WINGM_REN, 作用所选档案槽)
 * 共用; 作用对象 = 档案槽: 槽上有存活窗口改窗口名, 未打开档案改档案名 (此前无处改名)。
 * 校验: 空白/保留名/超长/重名 (存活窗口 + 全部档案都查, 名称全局唯一)。
 * 随迁: 专属搜索模式按档案名绑定 (XjsCmRenameWindow); 双击 Ctrl 目标按名绑定,
 * 旧名失效 = 禁用并卸钩子 (XjsDoubleCtrlApply, 与删除档案同一回落口径)。 */
static int s_renSlot = -1;   /* 待改名档案槽 (对话框打开时捕获; 槽持久, 不悬垂) */
static void XjsSetApplyWindowRename(HWND hwnd, int slot, const std::wstring& raw) {
    XjsUiProfile* prof = (slot > 0) ? XjsUiProfileAt(slot) : NULL;
    if (!prof) return;        /* 主窗(槽0)/无效槽: 调用方已置灰, 不可达兜底 */
    std::wstring nn = XjsTrimWs(raw, L" \t");   /* 窗口名只去空格/制表位 (换行不在名称语义内) */
    if (nn.empty()) {
        XjsToastShow(hwnd, XjsT(L"错误.窗口名称为空"), XTOAST_WARN, SS(1));
        return;
    }
    if (nn == XJS_MAIN_WIN_NAME) {
        XjsToastShow(hwnd, XjsT(L"错误.保留名称"), XTOAST_WARN, SS(1));
        return;
    }
    if (nn.length() > 40) {
        XjsToastShow(hwnd, XjsT(L"错误.名称过长"), XTOAST_WARN, SS(1));
        return;
    }
    XjsSearchWindow* w = XjsSearchWindow::AtSlot(slot);
    std::wstring oldName = w ? w->name : prof->name;
    if (XjsSearchWindow::NameInUse(nn.c_str(), w)) {
        XjsToastShow(hwnd, XjsT(L"错误.名称重复"), XTOAST_WARN, SS(1));
        return;
    }
    for (int i = 0; i < XjsUiProfileCount(); i++) {   /* 未打开档案也占名 (含主窗槽 0) */
        XjsUiProfile* o = XjsUiProfileAt(i);
        if (i != slot && o && o->name == nn) {
            XjsToastShow(hwnd, XjsT(L"错误.名称重复"), XTOAST_WARN, SS(1));
            return;
        }
    }
    prof->name = nn;          /* 档案名立即改 (存活窗口下次落盘刷回同值, 两处一致) */
    if (w) w->name = nn;
    XjsCmRenameWindow(oldName, nn);
    XjsSaveConfig();
    XjsDoubleCtrlApply();
    wchar_t t[64];            /* 改的是 owner 时标题随动 (改别窗 = 标题本就不含它, 重设无害) */
    XjsSetBuildTitle(t, 64);
    SetWindowTextW(hwnd, t);
    s_set.rowsDirty = true;   /* 窗口管理/窗口名称 行刷新 */
    InvalidateRect(hwnd, NULL, FALSE);
    XjsToastShow(hwnd, (XjsT(L"状态栏.已重命名前缀") + nn + L"」").c_str(), XTOAST_SUCCESS, SS(1));
}

/* 一行"取值下拉"的公共实现: 锚点取行尾控件下方, 项 id = base+i, 选中项打勾。
   labels 为空时用 XjsAppearPosLabel 生成 (出现位置 12 档的文字唯一来源)。
   调用方只给 base/文字表/档数/当前档/菜单最小宽 —— 曾 6 处各写一遍同构的 push_back 循环 */
static void XjsSetShowChoiceMenu(const XjsSetRow& r, int base, const wchar_t* const* labels, int n,
                                 int cur, float minW) {
    std::vector<XjsPopupItem> items;
    for (int i = 0; i < n; i++)
        items.push_back({ base + i, labels ? labels[i] : XjsAppearPosLabel(i), L"", cur == i,
                          false, false, false, false });
    XjsShowPopupMenu(s_set.hwnd, XjsSetDropdownAnchor(r), items, minW);
}

/* 行激活 (命令类松开触发口径的执行端)。actOverride: 双钮行指定跑主钮(act)还是第二钮(act2),
   其余调用方传 0 = 用 r.act 本行动作 */
static void XjsSetActivateRow(const XjsSetRow& r, int actOverride = 0) {
    int act = actOverride ? actOverride : r.act;
    XjsWindowScope scope(XjsSetOwner());   /* 每窗设置 (皮肤/视图/预览) 作用到 owner, 与"当前窗"无关 */
    if (s_setOwner) s_setOwner->SyncSkin();   /* 皮肤行勾选判断比对 g_skinName: 先认回 owner 再比 */
    /* 开关行公共动作: 取反 → 落盘 → 重绘 owner (scope 内 Cur() = owner)。
       ownerRepaint=false 用于"自身有更精确的生效路径"的行 (任务栏图标: 改样式而非重绘) */
    auto toggle = [](bool& v, bool ownerRepaint = true) {
        v = !v;
        XjsSaveConfig();
        if (ownerRepaint) XjsSearchWindow::Cur()->Invalidate();
    };
    switch (act) {
        case ACT_PREVIEW:   XjsPreviewToggle(); break;
        case ACT_HOTKEY:    s_set.recHotkey = true; break;   /* 下个非修饰键组合成为新热键 (WM_KEYDOWN 捕获) */
        case ACT_ZOOM: {
            /* 页面缩放 (每窗): 自绘菜单选择 50%~200% 步进 10% (id = ACT_ZOOM+档下标) */
            POINT anchor = XjsSetDropdownAnchor(r);
            std::vector<XjsPopupItem> items;
            for (int z = 50; z <= 200; z += 10)
                items.push_back({ ACT_ZOOM + (z - 50) / 10, std::to_wstring(z) + L"%",
                                  L"", g_uiZoomTenths == z / 10, false, false, false, false });
            XjsShowPopupMenu(s_set.hwnd, anchor, items, XSF(140));
            break;
        }
        case ACT_MOUSEOPEN: {
            /* 鼠标打开 (每窗): 双击/单击 菜单选择 */
            const wchar_t* const T[2] = { XjsT(L"通用词.双击打开"), XjsT(L"通用词.单击打开") };
            XjsSetShowChoiceMenu(r, ACT_MOUSEOPEN, T, 2, g_mouseOpen, XSF(160));
            break;
        }
        case ACT_CREATEFILL: {
            /* 新窗口初始搜索词 (每窗): 菜单选择 */
            const wchar_t* const T[3] = { XjsT(L"通用词.清空"), XjsT(L"通用词.指定关键词"), XjsT(L"通用词.上一次搜索的词") };
            XjsSetShowChoiceMenu(r, ACT_CREATEFILL, T, 3, g_createFill, XSF(220));
            break;
        }
        case ACT_CREATEKW: {
            /* 指定的关键词: 输入对话框 (结果回 s_set.hwnd, WM_INPUT_DONE 的 XJS_SET_INPUT_KW 分支) */
            XjsSearchWindow* ow = XjsSearchWindow::Cur();
            if (!ow) break;
            XjsShowInputDialog(s_set.hwnd, XjsT(L"设置.新建窗口.指定关键词"),
                               XjsT(L"设置.新建窗口.执行首搜说明"),
                               ow->createKeyword, XJS_SET_INPUT_KW);
            break;
        }
        case ACT_SORT: {
            /* 默认排序 (每窗): 预设菜单选择; 事实源 = 结果对象, 档案只存初始默认 */
            const char* sf = g_result ? xjs_result_GetSortField(g_result) : NULL;
            bool sw = g_result ? xjs_result_GetSortway(g_result) != FALSE : false;
            POINT anchor = XjsSetDropdownAnchor(r);
            std::vector<XjsPopupItem> items;
            for (int i = 0; i < 7; i++)
                items.push_back({ ACT_SORT + i, XjsT(SORT_PRESETS[i].label), L"",
                                  sf && !strcmp(SORT_PRESETS[i].field, sf) && SORT_PRESETS[i].asc == sw,
                                  false, false, false, false });
            XjsShowPopupMenu(s_set.hwnd, anchor, items, XSF(220));
            break;
        }
        case ACT_DEFSEL: {
            /* 自动选中第一项 (每窗): 菜单选择 */
            const wchar_t* const T[2] = { XjsT(L"通用词.不自动选中"), XjsT(L"通用词.自动选中") };
            XjsSetShowChoiceMenu(r, ACT_DEFSEL, T, 2, g_defaultSel, XSF(180));
            break;
        }
        case ACT_BLUR: {
            /* 窗口失去焦点动作: 自绘菜单选择 (owner=设置窗, 结果回 WM_POPUP_RESULT) */
            const wchar_t* const T[2] = { XjsT(L"通用词.无"), XjsT(L"通用词.关闭窗口") };
            XjsSetShowChoiceMenu(r, ACT_BLUR, T, 2, g_blurAction, XSF(160));
            break;
        }
        case ACT_APPEAR: {
            /* 出现位置: 自绘菜单选择 (12 档 = 之前的位置 / 跟随鼠标 / 主屏五落点 / 鼠标所在屏幕五落点;
               档位序与 xjs_app.h XJS_APPEAR_* 及 ApplyAppearPos 落点算式同源, 文字由 XjsAppearPosLabel 给) */
            XjsSetShowChoiceMenu(r, ACT_APPEAR, NULL, XJS_APPEAR_COUNT, g_appearPos, XSF(210));
            break;
        }
        case ACT_CTRLBTN:   toggle(g_showCtrlBtns); break;   /* owner 标题栏立即重排 */
        case ACT_FILTERBOX: toggle(g_showFilterBox); break;
        case ACT_STATUSBAR: toggle(g_showStatusbar); break;
        case ACT_PLUGINS_OPENDIR: XjsPluginOpenDir(-1); break;          /* 全局: exe\plugins */
        case ACT_PLUGINS_RESCAN:
            XjsPluginRescan();
            s_set.rowsDirty = true;                                     /* 清单/新目录即时上列表 */
            InvalidateRect(s_set.hwnd, NULL, FALSE);
            break;
        case ACT_TASKBAR:
            toggle(g_taskbarIcon, false);
            XjsSearchWindow::Cur()->ApplyTaskbarIcon();   /* 立即装卸 WS_EX_TOOLWINDOW (scope 内 Cur()=owner) */
            break;
        case ACT_DCCTRL: {
            /* 下拉选择器 = 复用自绘弹窗菜单 (owner=设置窗, 结果 WM_POPUP_RESULT 回设置窗):
               禁用 / 默认窗口(槽0) / 各命名档案 (✓=当前目标) */
            POINT anchor = XjsSetDropdownAnchor(r);
            std::vector<XjsPopupItem> items;
            items.push_back({ 1, XjsT(L"通用词.禁用"), L"", g_doubleCtrlTarget.empty(), false, false, false, false });
            items.push_back({ 2, XJS_MAIN_WIN_NAME, L"", g_doubleCtrlTarget == XJS_MAIN_WIN_NAME, false, false, false, false });
            for (int k = 1; k < XjsUiProfileCount(); k++) {
                XjsUiProfile* p = XjsUiProfileAt(k);
                if (!p || p->name.empty()) continue;
                XjsPopupItem c;
                c.id = 2 + k;
                c.title = p->name;
                c.checked = (g_doubleCtrlTarget == p->name);
                items.push_back(c);
            }
            XjsShowPopupMenu(s_set.hwnd, anchor, items, XSF(200));
            break;
        }
        case ACT_ENGINE: {
            /* 界面显示模式下拉 (进程共享, 重启生效; 菜单回传 901=标准 902=兼容) */
            POINT anchor = XjsSetDropdownAnchor(r);
            std::vector<XjsPopupItem> items;
            items.push_back({ 901, XjsT(L"通用词.标准模式"), L"", g_gfxEngine == 0, false, false, false, false });
            items.push_back({ 902, XjsT(L"通用词.兼容模式"), L"", g_gfxEngine == 1, false, false, false, false });
            XjsShowPopupMenu(s_set.hwnd, anchor, items, XSF(200));
            break;
        }
        case ACT_LANG: {
            /* 界面语言下拉 (每窗 = owner; 菜单回传 919=自动 920..925=zh/zh-TW/en/ko/th/ms)。
               语言名恒用各自母语显示 (XjsLangLabel), 不随当前语言翻译 — 用户切不回来时认得出 */
            POINT anchor = XjsSetDropdownAnchor(r);
            std::vector<XjsPopupItem> items;
            items.push_back({ 919, XjsLangAutoLabel(), L"", g_lang == XLANG_AUTO, false, false, false, false });
            for (int l = XLANG_ZH; l < XLANG_N; l++)
                items.push_back({ 920 + l, XjsLangLabel(l), L"",
                                  g_lang != XLANG_AUTO && g_lang == l, false, false, false, false });
            XjsShowPopupMenu(s_set.hwnd, anchor, items, XSF(200));
            break;
        }
        case ACT_AUTOSTART:
            if (!XjsSetAutoStart(!XjsIsAutoStartEnabled()))
                XjsToastShow(s_set.hwnd, XjsT(L"错误.开机自启动失败"), XTOAST_WARN, SS(1));
            break;
        case ACT_MEMLOCK: {
            /* 内存页锁定: 动作按真实状态决定 (开关显示即真实态), 成功后回执新状态行 (原版同款) */
            int cur = XjsMemLockQuery();
            bool enable = !(cur == XMLK_ST_ENABLED || cur == XMLK_ST_GRANTED_OFF || cur == XMLK_ST_GRANTED);
            if (XjsMemLockApply(enable)) {
                int ns = XjsMemLockQuery();
                XjsToastShow(s_set.hwnd,
                             (std::wstring(XjsT(L"设置.内存.当前状态")) + XjsMemLockStateText(ns)).c_str(),
                             XTOAST_SUCCESS, SS(1));
            } else {
                XjsToastShow(s_set.hwnd, XjsT(L"设置.内存.操作失败"), XTOAST_WARN, SS(1));
            }
            break;
        }
        case ACT_PERF_LSSW:     /* 统计采样开关 (引擎侧默认开; 关=各阶段跳过采集零开销) */
            if (g_engine) xjs_db_SetPerformanceSwitch(g_engine, xjs_db_IsPerformanceSwitch(g_engine) ? FALSE : TRUE);
            break;
        case ACT_PERF_SCNSW:
            if (g_engine) xjs_db_SetScanPerformanceSwitch(g_engine, xjs_db_IsScanPerformanceSwitch(g_engine) ? FALSE : TRUE);
            break;
        case ACT_PERF_LSCLR:    /* 清零: 明细区随之变"尚未采集", 不另弹提示 */
            if (g_engine) xjs_db_ClearPerformanceText(g_engine);
            break;
        case ACT_PERF_SCNCLR:
            if (g_engine) xjs_db_ClearScanPerformanceText(g_engine);
            break;
        case ACT_PERF_SYNCLR:
            if (g_engine) xjs_sync_ClearPerformanceText(g_engine);
            break;
        case ACT_DRIVEPROG: toggle(g_driveProgress); break;   /* owner 列表立即生效 */
        case ACT_ROWHOVER:  toggle(g_rowHover); break;
        case ACT_HOVERFADE: toggle(g_rowHoverFade); break;
        case ACT_WINNAME: {
            /* 重命名窗口 (owner): 输入对话框结果回 s_set.hwnd (WM_INPUT_DONE), 不落主窗别名通道;
               与 窗口管理行 共用 XjsSetApplyWindowRename (按槽定位) */
            XjsSearchWindow* ow = XjsSearchWindow::Cur();
            if (!ow || ow->isMain) break;   /* 主窗固定名 (行已置灰, 兜底) */
            s_renSlot = ow->uiIndex;
            XjsShowInputDialog(s_set.hwnd, XjsT(L"设置.窗口管理.重命名按钮"),
                               XjsT(L"设置.窗口管理.重命名说明"),
                               ow->name, XJS_SET_INPUT_RENAME);
            break;
        }
        case ACT_REBUILD:   XjsSetOpenRebuildDialog(); break;
        case ACT_EXCL_ADD: {
            std::wstring dir;
            if (XjsPickFolder(s_set.hwnd, dir)) {
                if (xjs_db_AddExcludedDir(g_engine, Utf16ToUtf8(dir.c_str()).c_str()))
                    XjsToastShow(s_set.hwnd, XjsT(L"重建.排除目录提示已排除"), XTOAST_SUCCESS, SS(1));
                else
                    XjsToastShow(s_set.hwnd, XjsT(L"重建.添加失败"), XTOAST_WARN, SS(1));
            }
            break;
        }
        case ACT_CLEARHIST:
            g_history.clear();
            XjsSaveHistory();
            XjsToastShow(s_set.hwnd, XjsT(L"提示.历史已清空"), XTOAST_SUCCESS, SS(1));
            break;
        case ACT_TADD: {
            /* 表格添加一行 (按当前分类): 追加空行并聚焦首格 (正式版同款);
               文件分类预填下一个空闲类型号 1..254 (用尽 = 留空手填) */
            const bool isFilter = (s_set.cat == SC_FILTER);
            auto& pool = isFilter ? s_filterEds : s_aliasEds;
            const int stride = isFilter ? 3 : 2;
            const int rows = (int)pool.size() / stride;
            if (rows >= (isFilter ? 256 : 1024)) break;   /* 删除动作段上限, 到顶即不再加 */
            for (int k = 0; k < stride; k++) pool.push_back(std::make_unique<XjsEditField>());
            if (isFilter) {
                bool used[255] = {};
                for (int i = 0; i < rows; i++) {
                    int t = XjsSetTblTypeParse(pool[i * 3 + 1]->ed.text);
                    if (t >= 1 && t <= 254) used[t] = true;
                }
                int next = 1;
                while (next <= 254 && used[next]) next++;
                if (next <= 254) pool[rows * 3 + 1]->ed.text = std::to_wstring(next);
            }
            pool[rows * stride]->SetFocused(s_set.hwnd, true);   /* 新行首格 (area 下帧渲染回写) */
            break;
        }
        case ACT_TSAVE:
            if (s_set.cat == SC_FILTER) XjsSetFilterSave();
            else XjsSetAliasSave();
            break;
        case ACT_COPYVER: {   /* 复制版本信息 (排查问题用, 直接粘贴给对方; 含显示模式技术名) */
            const char* ver = xjs_GetVersion();
            std::wstring txt = XjsT(L"设置.关于.版本信息模板")
                             + (ver ? Utf8ToUtf16(ver) : XjsT(L"设置.关于.未知"))
                             + XjsT(L"设置.关于.换行编译时间") + XjsBuildTimeText()
                             + XjsT(L"设置.关于.换行运行环境") + XjsOsVersionText()
                             + (sizeof(void*) == 8 ? XjsT(L"设置.关于.后缀64位") : XjsT(L"设置.关于.后缀32位"))
                             + (XjsIsRunningAsAdmin() ? XjsT(L"设置.关于.换行权限管理员") : XjsT(L"设置.关于.换行权限标准"))
                             + (g_gfxEngine == 1 ? XjsT(L"设置.关于.换行模式兼容") : XjsT(L"设置.关于.换行模式标准"));
            XjsCopyClipboard(txt);
            XjsToastShow(s_set.hwnd, XjsT(L"设置.关于.版本已复制"), XTOAST_SUCCESS, SS(1));
            break;
        }
        case ACT_GITHUB:    ShellExecuteW(NULL, L"open", L"https://github.com/Winlpl/xunjieso", NULL, NULL, SW_SHOWNORMAL); break;
        case ACT_SITE:      ShellExecuteW(NULL, L"open", L"https://www.xunjieso.com/", NULL, NULL, SW_SHOWNORMAL); break;
        case ACT_DONORS:    ShellExecuteW(NULL, L"open", L"https://www.xunjieso.com/donate", NULL, NULL, SW_SHOWNORMAL); break;   /* 捐赠名单页 (后续可指到专页) */
        case ACT_GLM:       ShellExecuteW(NULL, L"open", L"https://open.bigmodel.cn/", NULL, NULL, SW_SHOWNORMAL); break;  /* GLM 官网 (智谱开放平台 BigModel) */
        default:
            if (act >= ACT_PLUGINS_TOGGLE && act < ACT_PLUGINS_TOGGLE + 200) {
                XjsSetPluginToggle(act - ACT_PLUGINS_TOGGLE);   /* 插件启用开关 (含首次启用确认框) */
                break;
            }
            if (act >= ACT_MATCH && act < ACT_MATCH + 8) {
                /* 与 搜索匹配 组行序一致; 每窗设置: 切换只下发 owner 的结果对象并落盘 (scope 内 Cur()=owner)。
                   成员指针而非 &g_xxx: static 表只初始化一次, 写 &g_match.xxx 会把地址钉在"首次执行时
                   的 Cur()"上 —— 换窗后写错对象; 那个窗口若已关闭就是写已释放内存 (2026-09-18 定案) */
                static bool XjsMatchSettings::* const kFields[8] = {
                    &XjsMatchSettings::emptyShowsAll, &XjsMatchSettings::caseSensitive, &XjsMatchSettings::pinyinFull,
                    &XjsMatchSettings::pinyinInitial, &XjsMatchSettings::pinyinExact, &XjsMatchSettings::wildcardQuestion,
                    &XjsMatchSettings::wildcardStar, &XjsMatchSettings::matchFullWidth };
                bool XjsMatchSettings::* const mem = kFields[act - ACT_MATCH];
                XjsMatchSettings& m = XjsSearchWindow::Cur()->match;
                m.*mem = !(m.*mem);
                XjsApplyMatchSettingsFor(XjsSearchWindow::Cur());
                XjsSaveConfig();
                break;
            }
            if (act >= ACT_OPEN && act < ACT_OPEN + 3) {
                /* 打开文件行为 (每窗): 切换即落盘 uiWindows 档案 (同上去 Cur() 内取字段, 不缓存地址) */
                static bool XjsSearchWindow::* const kOpenFields[3] = {
                    &XjsSearchWindow::openElevated, &XjsSearchWindow::openAsync, &XjsSearchWindow::openHideWindow };
                bool XjsSearchWindow::* const mem = kOpenFields[act - ACT_OPEN];
                XjsSearchWindow* ow = XjsSearchWindow::Cur();
                ow->*mem = !(ow->*mem);
                XjsSaveConfig();
                break;
            }
            if (act >= ACT_WINGM_REN && act < ACT_WINGM_REN + 60) {
                /* 窗口管理·重命名: 作用于档案槽 (槽上有存活窗口=改窗口名, 未打开=改档案名 —
                   未打开档案此前无处改名)。对话框结果经 XJS_SET_INPUT_RENAME 通道回
                   WM_INPUT_DONE, 与 窗口名称行 共用同一入口 XjsSetApplyWindowRename */
                int slot = act - ACT_WINGM_REN;
                XjsUiProfile* prof = XjsUiProfileAt(slot);
                if (slot == 0 || !prof) break;   /* 主窗固定名 (行已置灰, 兜底) */
                XjsSearchWindow* w = XjsSearchWindow::AtSlot(slot);
                s_renSlot = slot;
                XjsShowInputDialog(s_set.hwnd, XjsT(L"设置.窗口管理.重命名按钮"),
                                   XjsT(L"设置.窗口管理.重命名说明"),
                                   w ? w->name : prof->name, XJS_SET_INPUT_RENAME);
                break;
            }
            if (act >= ACT_WINGM_DEL && act < ACT_WINGM_DEL + 64) {
                /* 窗口管理·删除: 先经通用询问框二次确认 (禁系统 MessageBoxW), 通过后执行 ——
                   档案已打开 = 先真销毁窗口 (含结果对象, 快捷键随重注册卸载), 再删档案;
                   未打开 = 仅删档案。主窗槽 0 行已置灰, 不可达。
                   双击 Ctrl 目标是被删窗口时, 目标名失效回落禁用 —— 复用 XjsDoubleCtrlApply
                   的现有装卸逻辑实现卸载, 不另写卸载代码 */
                int slot = act - ACT_WINGM_DEL;
                if (slot == 0) break;
                XjsUiProfile* prof = XjsUiProfileAt(slot);
                XjsSearchWindow* w = XjsSearchWindow::AtSlot(slot);
                std::wstring name = w ? w->name : (prof && !prof->name.empty() ? prof->name : XjsT(L"菜单.未命名窗口"));
                std::wstring desc = XjsT(L"设置.窗口管理.删除确认前缀") + name + XjsT(L"设置.窗口管理.删除确认后缀");
                if (w) desc += XjsT(L"设置.窗口管理.删除确认.正在打开");
                if (g_doubleCtrlTarget == name) desc += XjsT(L"设置.窗口管理.删除确认.双击Ctrl目标");
                int cmN = XjsCmWindowCount(name);   /* 专属搜索模式按档案名绑定, 删档连带删 (先计数用于提示) */
                if (cmN > 0) desc += XjsT(L"设置.窗口管理.删除确认.专属模式前缀") + std::to_wstring(cmN) + XjsT(L"设置.窗口管理.删除确认.专属模式后缀");
                int askR = XjsShowAskDialog(s_set.hwnd, XjsT(L"设置.窗口管理.删除按钮"), desc.c_str(),
                                     R"([{"text":"删除","style":"danger"},{"text":"取消"}])");   /* 未确认 (取消/失活/Esc) */
                if (askR != 0) break;
                if (w) XjsDismissWindow(w->hWnd);   /* 同步销毁 (含结果对象) */
                if (g_doubleCtrlTarget == name) {   /* 双击 Ctrl 目标失效 → 禁用 (复用装卸逻辑) */
                    g_doubleCtrlTarget.clear();
                    XjsDoubleCtrlApply();
                }
                XjsCmPurgeWindow(name);   /* 连带删除该窗口专属搜索模式 (上方确认已提示条数) */
                XjsUiProfilesRemove(slot);
                XjsHotkeysRegisterAll();   /* 其余槽快捷键按新槽位重注册 (被删槽自动卸载) */
                XjsSaveConfig();
                if (!XjsSearchWindow::Alive(s_setOwner)) s_setOwner = XjsSearchWindow::Main();   /* 删的是 owner: 回落主窗 */
                wchar_t t[64];
                XjsSetBuildTitle(t, 64);
                SetWindowTextW(s_set.hwnd, t);
                s_set.rowsDirty = true;
                InvalidateRect(s_set.hwnd, NULL, FALSE);
                break;
            }
            if (act >= ACT_EXCL_DEL && act < ACT_EXCL_DEL + 1024) {
                int i = act - ACT_EXCL_DEL;
                std::vector<std::wstring> dirs;
                XjsJsonStringArray(g_engine ? xjs_db_GetExcludedDirs(g_engine) : NULL, &dirs);
                if (i < (int)dirs.size() && !xjs_db_RemoveExcludedDir(g_engine, Utf16ToUtf8(dirs[i].c_str()).c_str()))
                    XjsToastShow(s_set.hwnd, XjsT(L"错误.删除失败"), XTOAST_WARN, SS(1));
                break;
            }
            if (act >= ACT_FDEL && act < ACT_FDEL + 256) {   /* 文件分类行删除 (字段析构自动摘登记) */
                int i = act - ACT_FDEL;
                if (i < XjsSetFilterRows())
                    s_filterEds.erase(s_filterEds.begin() + i * 3, s_filterEds.begin() + i * 3 + 3);
                break;
            }
            if (act >= ACT_ADEL && act < ACT_ADEL + 1024) {   /* 别名行删除 */
                int i = act - ACT_ADEL;
                if (i < XjsSetAliasRows())
                    s_aliasEds.erase(s_aliasEds.begin() + i * 2, s_aliasEds.begin() + i * 2 + 2);
                break;
            }
            if (act >= ACT_VIEW && act < ACT_VIEW + 4) { XjsSetViewMode(act - ACT_VIEW); break; }
            if (act >= ACT_SKIN && act < ACT_SKIN + (int)g_skinMenuNames.size()) {
                int si = act - ACT_SKIN;
                if (g_skinMenuNames[si] != g_skinName) {
                    /* 皮肤每窗独立: 写到打开设置的窗口 (s_curWin=opener), 全局镜像随当前窗走 */
                    XjsSearchWindow::Cur()->skinName = g_skinMenuNames[si];
                    g_skinName = g_skinMenuNames[si];
                    XjsSkinLoad(g_skinName.c_str());
                    XjsSkinApply();   /* 纪元+1 → 本窗口画刷随绘制重建 */
                    XjsSaveConfig();  /* 每窗档案落盘 xjs_config.json (uiWindows) */
                    XjsPluginOnSkinChanged(XjsPluginCurWindowToken());   /* 已订阅插件 (自建窗口) 取新皮肤重绘 */
                }
            }
            break;
    }
    s_set.rowsDirty = true;   /* 勾选态/视图/皮肤状态可能已变, 下次绘制重建行模型 */
    if (s_set.hwnd) InvalidateRect(s_set.hwnd, NULL, FALSE);
}

static LRESULT CALLBACK Xjs_SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    XjsSettingsState* p = &s_set;
    /* 整个 WndProc 统一钉在 owner 上下文 (与 WM_PAINT 同口径): 各搜索窗的 100ms 同步定时器
       会不停把 Cur 重绑到别的窗, 不钉则命中/滚动的 SS() (→Cur()->uiZoom) 与绘制侧 (owner)
       两套尺度 — 画好的开关点不中; owner 悬垂回落主窗也顺带消除了裸 SS() 的空指针窗口 */
    XjsWindowScope scope(XjsSetOwner());
    switch (msg) {
        case WM_PAINT: {
            /* 设置窗是独立 hwnd: 画刷/行模型取色跟随 owner 窗口皮肤 (多窗皮肤各异, g_skin 是竞态镜像)。
               作用域已在入口统一钉住, 这里只须显式认回皮肤 (EnsureBrushes 按它取色); 已同步时零开销 */
            if (s_setOwner) s_setOwner->SyncSkin();
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            XjsSetPaint(hwnd);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_INPUT_DONE: {
            /* 输入对话框收尾: lParam=new std::wstring*(lParam=NULL=取消=什么都不做)。
               通道 1 = 重命名窗口; 通道 2 = 创建关键词 (创建-用户指定关键词 的填入内容) */
            std::wstring* s = (std::wstring*)lParam;
            if (s && (int)wParam == XJS_SET_INPUT_KW && s_setOwner && XjsSearchWindow::Alive(s_setOwner)) {
                std::wstring kw = XjsTrimWs(*s);
                if (kw.length() > 200) {
                    XjsToastShow(hwnd, XjsT(L"错误.关键词过长"), XTOAST_WARN, SS(1));
                } else {
                    XjsWindowScope scope(s_setOwner);
                    g_createKeyword = kw;
                    XjsSaveConfig();
                    s_set.rowsDirty = true;
                    InvalidateRect(hwnd, NULL, FALSE);
                    XjsToastShow(hwnd, kw.empty() ? XjsT(L"提示.关键词已清空") : XjsT(L"提示.关键词已保存"), XTOAST_SUCCESS, SS(1));
                }
            }
            if (s && (int)wParam == XJS_SET_INPUT_RENAME) {
                XjsWindowScope scope(XjsSetOwner());   /* XjsT 按 owner 语言解析 (本函数其余分支同口径) */
                XjsSetApplyWindowRename(hwnd, s_renSlot, *s);   /* 唯一入口: 校验+改名+随迁 (按槽) */
            }
            delete s;
            return 0;
        }
        case WM_SIZE:
            if (p->rt) { p->rt->Resize(ximax(LOWORD(lParam), 1), ximax(HIWORD(lParam), 1)); InvalidateRect(hwnd, NULL, FALSE); }
            p->rowsDirty = true;   /* 宽度变 → 行模型重排 (md 文档行高随内容区宽) */
            return 0;
        case WM_GETMINMAXINFO: {   /* 拖边自由调整的下限, 防止布局挤爆 */
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            mmi->ptMinTrackSize.x = (LONG)SS(520);
            mmi->ptMinTrackSize.y = (LONG)SS(400);
            return 0;
        }
        case WM_DPICHANGED: {
            /* 拖到不同缩放的显示器: 尺度更新 + 资源重建 + 系统建议矩形 */
            p->scale = HIWORD(wParam) / 96.0f;
            XjsSetFreeResources();
            p->rowsDirty = true;   /* 行几何含 SS(), 尺度变了须重排 */
            if (lParam) {
                RECT* r = (RECT*)lParam;
                SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                    SWP_NOZORDER | SWP_NOACTIVATE);
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE: {
            /* 分辨率/系统级显示广播不带 WM_DPICHANGED (改分辨率、RDP 会话重连等):
               设置窗没有逐帧视口自愈, 静止时不适应 → 立即按每窗有效 DPI
               (系统随 WM_DPICHANGED 同步维护) 自检对齐 */
            UINT dpi = XjsGetDpiForWindow(hwnd);
            if (dpi && (int)(p->scale * 96.0f + 0.5f) != (int)dpi) {
                p->scale = dpi / 96.0f;
                XjsSetFreeResources();
                p->rowsDirty = true;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (p->sbDrag) {   /* 右缘滚动条拖拽中: thumb 跟手 (保持按下点相对位移), 其余悬停逻辑不走 */
                RECT crc; GetClientRect(hwnd, &crc);
                float tY, tH, maxS;
                if (!(GetKeyState(VK_LBUTTON) & 0x8000) ||
                    !XjsSetSbGeom((float)crc.right, (float)crc.bottom, tY, tH, maxS)) {
                    p->sbDrag = false;   /* 捕获中途丢失/内容变矮: 拖拽自然结束 */
                    if (GetCapture() == hwnd) ReleaseCapture();
                } else {
                    float trackH = (float)crc.bottom - SS(8);
                    if (trackH - tH > 1) {
                        float ns = ((float)pt.y - p->sbGrabOff - SS(4)) / (trackH - tH) * maxS;
                        if (ns < 0) ns = 0;
                        if (ns > maxS) ns = maxS;
                        if (ns != p->scroll) { p->scroll = ns; InvalidateRect(hwnd, NULL, FALSE); }
                    }
                }
                return 0;
            }
            if (p->sbSideDrag) {   /* 分类栏滚动条拖拽中: thumb 跟手 (口径同右缘内容条) */
                RECT crc; GetClientRect(hwnd, &crc);
                float tY, tH, maxS;
                if (!(GetKeyState(VK_LBUTTON) & 0x8000) ||
                    !XjsSetSideSbGeom((float)crc.bottom, tY, tH, maxS)) {
                    p->sbSideDrag = false;   /* 捕获中途丢失/分类变矮: 拖拽自然结束 */
                    if (GetCapture() == hwnd) ReleaseCapture();
                } else {
                    float trackH = (float)crc.bottom - SS(8);
                    if (trackH - tH > 1) {
                        float ns = ((float)pt.y - p->sbSideGrabOff - SS(4)) / (trackH - tH) * maxS;
                        if (ns < 0) ns = 0;
                        if (ns > maxS) ns = maxS;
                        if (ns != p->sideScroll) { p->sideScroll = ns; InvalidateRect(hwnd, NULL, FALSE); }
                    }
                }
                return 0;
            }
            if (XjsEditFieldMouseMove(hwnd, pt)) return 0;   /* 输入字段拖选跟手 (MouseDown 已取捕获) */
            { RECT crc; GetClientRect(hwnd, &crc);
              if (XjsToastMouseMove(hwnd, SS(1), (float)crc.right, (float)crc.bottom, pt)) return 0; }   /* Toast 卡片浮于一切之上 */
            if (p->dlgOpen) {
                /* 对话框打开: 只有对话框内控件可悬停, 底层页面冻结 */
                int h = DHB_NONE;
                for (auto& hit : p->dlgHits)
                    if (XjsSetDlgHitTest(hit, (float)pt.x, (float)pt.y)) { h = hit.id; break; }
                if (h != p->dlgHover) { p->dlgHover = h; InvalidateRect(hwnd, NULL, FALSE); }
                return 0;
            }
            XjsSetBuildRows();
            float sideW = XjsSetSideW();
            int hc = -1, hr = -1;
            if (pt.x < sideW && pt.x >= 0) {
                XjsSetSideItem side[SET_TREE_N];
                int sn = XjsSetSideItems(side, SET_TREE_N);
                float sy = pt.y + p->sideScroll;   /* 命中按未滚动坐标比对 (与绘制偏移同源) */
                for (int si = 0; si < sn; si++)
                    if (sy >= side[si].y && sy < side[si].y + SS(34)) { hc = si; break; }
            } else {
                float y = pt.y + p->scroll;
                for (int ci = 0; ci < (int)p->cards.size() && hr < 0; ci++)
                    for (int ri = 0; ri < (int)p->cards[ci].rows.size(); ri++) {
                        const XjsSetRow& r = p->cards[ci].rows[ri];
                        if (r.act == ACT_NONE) continue;
                        if (y >= r.y && y < r.y + r.h) { hr = ci * 1000 + ri; break; }
                    }
            }
            int sbh = XjsSetSbHoverAt(hwnd, pt);   /* 滚动条悬停增亮 (thumb 上提亮一档, 变化才失效) */
            if (hc != p->hoverSide || hr != p->hoverRow || sbh != p->sbHover) {
                p->hoverSide = hc;
                p->hoverRow = hr;
                p->sbHover = sbh;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            if (!p->trackingLeave) {
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                if (TrackMouseEvent(&tme)) p->trackingLeave = true;
            }
            /* 光标随移动显式设置 (本处理直接 return 0 不过 DefWindowProc → WM_SETCURSOR
               不会到来, 与主窗 WM_MOUSEMOVE 同款兜底): 链接行=手型, 其余=箭头 */
            SetCursor(LoadCursorW(NULL, XjsSetLinkRowHit(hwnd, pt) ? IDC_HAND : IDC_ARROW));
            return 0;
        }
        case WM_MOUSELEAVE:
            p->trackingLeave = false;
            XjsToastHoverReset(hwnd);
            if (p->hoverSide != -1 || p->hoverRow != -1 || p->sbHover) {
                p->hoverSide = -1; p->hoverRow = -1; p->sbHover = 0; InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        case WM_MOUSEWHEEL: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            float delta = -((float)GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA) * SS(34) * 3.0f;
            /* 指针在左侧分类栏上 = 滚分类栏 (内容超高才有得滚); 其余滚内容卡片。
               滚轮消息的 lParam 是屏幕坐标 (与其它鼠标消息不同), 须先转客户区 */
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &pt);
            if ((float)pt.x < XjsSetSideW()) {
                XjsSetSideItem side[SET_TREE_N];
                int sn = XjsSetSideItems(side, SET_TREE_N);
                float smax = XjsSetSideContentH(sn) - (float)(rc.bottom - rc.top);
                if (smax < 0) smax = 0;
                float ns = p->sideScroll + delta;
                if (ns < 0) ns = 0;
                if (ns > smax) ns = smax;
                if (ns != p->sideScroll) { p->sideScroll = ns; InvalidateRect(hwnd, NULL, FALSE); }
                return 0;
            }
            float maxScroll = p->contentH - (float)(rc.bottom - rc.top);
            if (maxScroll < 0) maxScroll = 0;
            float ns = p->scroll + delta;
            if (ns < 0) ns = 0;
            if (ns > maxScroll) ns = maxScroll;
            if (ns != p->scroll) { p->scroll = ns; InvalidateRect(hwnd, NULL, FALSE); }
            return 0;
        }
        case WM_RBUTTONDOWN:
        case WM_POPUP_RESULT: {
            if (msg == WM_POPUP_RESULT) {
                /* 窗口行为菜单回传 (owner=设置窗, 作用到 owner 窗): ACT_BLUR+0..1 失焦动作,
                   ACT_APPEAR+档位序 出现位置 (12 档), ACT_ZOOM+0..15 页面缩放, ACT_MOUSEOPEN+0..1 鼠标打开 —
                   选择即写 owner 字段 → 落盘 → 行模型刷新 */
                int id = (int)wParam;
                /* 输入字段编辑菜单回传 (IDM_FCTX_BASE..+4): 必须最先分流 —
                   否则落进下方双击 Ctrl 链的兜底 else 被吞 (该处原为死分支, 菜单项点击全无效果) */
                if (id >= IDM_FCTX_BASE && id < IDM_FCTX_BASE + 5) {
                    XjsEditFieldMenuCmd(hwnd, id - IDM_FCTX_BASE);
                    return 0;
                }
                /* 取值下拉回传的公共动作: 作用到 owner 窗 → 写字段 → 落盘 → 行模型刷新
                   (档位行同一套路; 只声明"基址/档数/目标字段"三件事, 动作不重复)。
                   ZOOM 走 XjsApplyZoom (自带落盘+格式重建) 与 SORT (另下发结果对象) 语义特殊, 各自单写 */
                /* 成员指针 (不是 int*): 变量地址必须落到 owner 窗 —— 直接写 &g_xxx 会在作用域
                   切换*之前*求值成"当前窗"的字段, 与 owner 不是同一窗时改错对象 */
                static const struct { int base, count; int XjsSearchWindow::* mem; } kChoiceRows[] = {
                    { ACT_BLUR,       2,                &XjsSearchWindow::blurAction  },
                    { ACT_APPEAR,     XJS_APPEAR_COUNT, &XjsSearchWindow::appearPos   },
                    { ACT_MOUSEOPEN,  2,                &XjsSearchWindow::mouseOpen   },
                    { ACT_CREATEFILL, 3,                &XjsSearchWindow::createFill  },
                    { ACT_DEFSEL,     2,                &XjsSearchWindow::defaultSel  },
                };
                for (const auto& cr : kChoiceRows) {
                    if (id < cr.base || id >= cr.base + cr.count) continue;
                    XjsWindowScope scope(XjsSetOwner());
                    XjsSearchWindow::Cur()->*(cr.mem) = id - cr.base;
                    XjsSaveConfig();
                    s_set.rowsDirty = true;
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
                if (id >= ACT_ZOOM && id < ACT_ZOOM + 16) {
                    XjsWindowScope scope(XjsSetOwner());
                    /* 菜单项 id = 基址+档下标 (0=50%…15=200%); XjsApplyZoom 吃的是"十分位"
                       (5..20), 传百分比会被上限钳成 20 = 每次都变 200% (2026-09-17 实锤) */
                    XjsApplyZoom(5 + (id - ACT_ZOOM));   /* 写 owner 字段+落盘+按新倍率重建格式 */
                    s_set.rowsDirty = true;
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
                if (id >= ACT_SORT && id < ACT_SORT + 7) {
                    /* 默认排序 (每窗): 写档案初始默认 + 立即下发 owner 结果对象并重搜 */
                    XjsWindowScope scope(XjsSetOwner());
                    XjsSearchWindow* ow = XjsSearchWindow::Cur();
                    XjsUiProfile* prof = ow ? XjsUiProfileAt(ow->uiIndex) : NULL;
                    if (prof) {
                        prof->sortField = Utf8ToUtf16(SORT_PRESETS[id - ACT_SORT].field);
                        prof->sortWay = SORT_PRESETS[id - ACT_SORT].asc;
                    }
                    if (ow && ow->result)
                        xjs_result_SetSortField(ow->result, SORT_PRESETS[id - ACT_SORT].field,
                                                SORT_PRESETS[id - ACT_SORT].asc ? TRUE : FALSE);
                    XjsSaveConfig();
                    XjsSearchNow(false);
                    s_set.rowsDirty = true;
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
                /* 界面显示模式下拉结果 (owner=设置窗): 901=标准 902=兼容。落盘 + 重启生效提示 */
                if (id == 901 || id == 902) {
                    XjsWindowScope scope(XjsSetOwner());   /* XjsT 按 owner 语言解析 (Toast 文案) */
                    int want = (id == 902) ? 1 : 0;
                    if (g_gfxEngine != want) {
                        g_gfxEngine = want;
                        XjsSaveConfig();
                        XjsToastShow(s_set.hwnd,
                            want == 1 ? XjsT(L"提示.切换兼容模式") : XjsT(L"提示.切换标准模式"),
                            XTOAST_SUCCESS, SS(1));
                        s_set.rowsDirty = true;
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                    return 0;
                }
                /* 界面语言下拉结果 (每窗 = owner): 919=自动, 920..925=zh/zh-TW/en/ko/th/ms。
                   写 owner 字段 → 落盘; 各搜索窗标题按**各自语言**刷新 (回调内 XjsWindowScope 切换),
                   托盘提示随主窗语言; 本窗行模型按 owner 新语言重建 */
                if (id >= 919 && id <= 925) {
                    XjsWindowScope scope(XjsSetOwner());
                    g_lang = (id == 919) ? XLANG_AUTO : id - 920;
                    XjsSaveConfig();
                    struct LangRefresh { static void Run(XjsSearchWindow* w) {
                        XjsWindowScope ws(w);   /* XjsT 解析到 w 自己的语言 */
                        const std::wstring& st = w->searchEd.text;
                        SetWindowTextW(w->hWnd, st.empty() ? XjsT(L"应用.名称")
                                                           : (st + L" - " + XjsT(L"应用.名称")).c_str());
                        w->Invalidate();
                    }};
                    XjsSearchWindow::ForEach(&LangRefresh::Run);
                    if (XjsSetOwner()->isMain)
                        XjsTrayAdd(XjsSearchWindow::MainHwnd());   /* 已入托盘时 = NIM_MODIFY 刷新提示 */
                    s_set.rowsDirty = true;
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
                /* 双击 Ctrl 下拉选择器结果 (owner=设置窗): 1=禁用; 2=默认窗口(槽0); 2+k=档案槽k。
                   选择即改目标 → 实时装卸钩子 → 落盘; 目标名不存在/被删 = 禁用 (空目标) */
                if (id == 1) {
                    g_doubleCtrlTarget.clear();
                } else if (id >= 2 && id < 2 + XjsUiProfileCount()) {
                    int slot = id - 2;
                    XjsUiProfile* prof = XjsUiProfileAt(slot);
                    if (slot == 0) g_doubleCtrlTarget = XJS_MAIN_WIN_NAME;
                    else if (prof && !prof->name.empty()) g_doubleCtrlTarget = prof->name;
                    else g_doubleCtrlTarget.clear();   /* 名称不存在 (未命名档案) = 禁用 */
                } else {
                    return 0;
                }
                XjsDoubleCtrlApply();   /* 实时装卸钩子 (禁用 = 立即卸载) */
                XjsSaveConfig();
                s_set.rowsDirty = true;   /* 行尾值 (当前目标) 刷新 */
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (msg == WM_RBUTTONDOWN && !p->dlgOpen)
                XjsEditFieldContextMenu(hwnd, pt);   /* 输入字段右键=编辑菜单 (未中字段=无操作, 原行为) */
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (p->sbDrag) { p->sbDrag = false; if (GetCapture() == hwnd) ReleaseCapture(); }   /* 滚动条拖拽收尾 */
            if (p->sbSideDrag) { p->sbSideDrag = false; if (GetCapture() == hwnd) ReleaseCapture(); }   /* 分类栏滚动条拖拽收尾 */
            XjsEditFieldMouseUp(hwnd);   /* 字段拖选收尾+释放捕获 (无拖选=无操作) */
            { RECT crc; GetClientRect(hwnd, &crc);
              XjsToastMouseUp(hwnd, SS(1), (float)crc.right, (float)crc.bottom, pt); }   /* Toast 按钮: 松开触发 */
            /* 命令落地 (松开触发口径): 按下待定 + 松开仍命中同一目标才执行 */
            if (p->dlgOpen) {
                if (p->pressDlg) {
                    int hit = DHB_NONE;
                    for (auto& h2 : p->dlgHits)
                        if (XjsSetDlgHitTest(h2, (float)pt.x, (float)pt.y)) { hit = h2.id; break; }
                    if (hit == p->pressDlg) {
                        if (hit == DHB_CANCEL) XjsSetCloseRebuildDialog(hwnd);
                        else if (hit == DHB_OK) XjsSetConfirmRebuild(hwnd);
                        else if (hit >= DHB_FIELD && hit < DHB_FIELD + 7) {
                            p->dlgField[hit - DHB_FIELD] = !p->dlgField[hit - DHB_FIELD];
                            p->dlgErr.clear();
                        } else if (hit >= DHB_DRIVE && hit < DHB_DRIVE + (int)p->dlgDrives.size()) {
                            auto& d = p->dlgDrives[hit - DHB_DRIVE];
                            d.checked = !d.checked;
                            p->dlgErr.clear();
                        }
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                    p->pressDlg = 0;
                }
                return 0;
            }
            RECT crc;
            GetClientRect(hwnd, &crc);
            float w = (float)crc.right;
            float sideW = XjsSetSideW();
            if (p->pressSide >= 0 && pt.x < sideW) {
                XjsSetSideItem side[SET_TREE_N];
                int sn = XjsSetSideItems(side, SET_TREE_N);
                float sy = pt.y + p->sideScroll;   /* 松开命中按未滚动坐标比对 (与按下/绘制同源) */
                for (int si = 0; si < sn; si++) {
                    if (side[si].idx != p->pressSide) continue;
                    if (sy >= side[si].y && sy < side[si].y + SS(34)) {
                        const XjsSetCatNode& nd = SET_TREE[side[si].idx];
                        if (nd.grp) {   /* 分组节点: 折叠/展开 (不是分类, 不可选中) */
                            p->winTreeOpen = !p->winTreeOpen;
                            InvalidateRect(hwnd, NULL, FALSE);
                        } else if (p->cat != nd.cat) {
                            XjsSetSwitchCat(hwnd, nd.cat);
                        }
                    }
                    break;
                }
                p->pressSide = -1;
                return 0;
            }
            p->pressSide = -1;
            if (p->pressAct && !p->rowsDirty) {
                /* 整行行 (选项/链接) 的松开回放要排除 侧栏/滚动条区 (与按下侧两个早退区同口径):
                   否则按住行文本水平拖到侧栏/滚动条上松开仍回放 = 皮肤/视图被误切换 (拖离=取消被破坏) */
                RECT crcX; GetClientRect(hwnd, &crcX);
                if ((float)pt.x >= XjsSetSideW() && (float)pt.x < (float)crcX.right - SS(10)) {
                    float y = pt.y + p->scroll;
                    for (auto& card : p->cards)
                        for (auto& r : card.rows) {
                            if (r.act != p->pressAct && r.act2 != p->pressAct) continue;
                            if (y < r.y || y >= r.y + r.h) continue;
                            bool second = (r.act2 != ACT_NONE && p->pressAct == r.act2);
                            /* 开关/按钮/胶囊/表格行: 须点在行尾控件上 (与按下同判, 第二钮判第二钮),
                               点行文本/输入格空白不触发 (表格行尤其如此: 行内大部分是输入框) */
                            if ((r.ctrl == CT_SWITCH || r.ctrl == CT_BUTTON || r.ctrl == CT_PILL || r.ctrl == CT_TROW) &&
                                (second ? !XjsSetRowControl2Hit(r, (float)pt.x, y, w - SS(28))
                                        : !XjsSetRowControlHit(r, (float)pt.x, y, w - SS(28))))
                                break;
                            XjsSetActivateRow(r, second ? r.act2 : r.act);
                            break;
                        }
                }
                p->pressAct = 0;
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (XjsEditFieldDoubleClick(hwnd, pt)) return 0;   /* 字段内双击=选整词 */
        }
            [[fallthrough]];   /* 字段外沿用单击语义 (补 CS_DBLCLKS 前双击本就是两次 down: 开关行=两次翻转) */
        case WM_LBUTTONDOWN: {
            /* 命令类松开触发 (2026-09-17 用户口径): 按下只记待定 (press*), 松开仍命中同一
               目标才执行; 字段点定位/滚动条拖拽/遮罩关闭 保持按下语义 */
            SetFocus(hwnd);
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            { RECT crc; GetClientRect(hwnd, &crc);
              if (XjsToastMouseDown(hwnd, SS(1), (float)crc.right, (float)crc.bottom, pt)) return 0; }   /* Toast 卡片: 吃点击 (按钮松开触发) */
            if (p->dlgOpen) {
                int hit = DHB_NONE;
                for (auto& h2 : p->dlgHits)
                    if (XjsSetDlgHitTest(h2, (float)pt.x, (float)pt.y)) { hit = h2.id; break; }
                if (hit == DHB_NONE) {
                    /* 点在对话框本体上 (非控件) 不动作; 点在遮罩上关闭 (源样式 mousedown target==mask 关闭) */
                    bool inside = (float)pt.x >= p->dlgRect.left && (float)pt.x <= p->dlgRect.right &&
                                  (float)pt.y >= p->dlgRect.top && (float)pt.y <= p->dlgRect.bottom;
                    if (!inside) XjsSetCloseRebuildDialog(hwnd);
                }
                else p->pressDlg = hit;   /* 按钮/字段/驱动器: 记待定 */
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            XjsSetBuildRows();
            /* 保留分类的"后缀"禁用格: 不可聚焦不触发 (正式版 disabled 输入框口径) */
            if (XjsSetTblDisabledHit(hwnd, pt)) return 0;
            /* 输入字段: 点进=聚焦+点定位 (吃掉点击, 不触发行); 点字段外=失焦后继续常规处理 (重绘组件自负责) */
            if (XjsEditFieldMouseDown(hwnd, pt)) return 0;
            /* 右缘滚动条: thumb 上按下即拖 (几何与渲染同源 XjsSetSbGeom; ±2px 容差);
               点条带空白处吞掉不触发行 (无轨道翻页, 同源样式细条口径) */
            {
                RECT crc; GetClientRect(hwnd, &crc);
                float tY, tH, maxS;
                if ((float)pt.x >= (float)crc.right - SS(10) &&
                    XjsSetSbGeom((float)crc.right, (float)crc.bottom, tY, tH, maxS)) {
                    if ((float)pt.y >= tY - SS(2) && (float)pt.y <= tY + tH + SS(2)) {
                        p->sbDrag = true;
                        p->sbGrabOff = (float)pt.y - tY;
                        SetCapture(hwnd);
                    }
                    return 0;
                }
            }
            /* 分类栏右缘滚动条: thumb 上按下即拖 (几何与渲染同源 XjsSetSideSbGeom; ±2px 容差);
               条带区域 (不超高时判定为 false) 吞掉不落分类, 口径同右缘内容条 */
            {
                RECT crc; GetClientRect(hwnd, &crc);
                float tY, tH, maxS;
                if ((float)pt.x >= XjsSetSideW() - SS(10) && (float)pt.x < XjsSetSideW() &&
                    XjsSetSideSbGeom((float)crc.bottom, tY, tH, maxS)) {
                    if ((float)pt.y >= tY - SS(2) && (float)pt.y <= tY + tH + SS(2)) {
                        p->sbSideDrag = true;
                        p->sbSideGrabOff = (float)pt.y - tY;
                        SetCapture(hwnd);
                    }
                    return 0;
                }
            }
            float sideW = XjsSetSideW();
            if (pt.x < sideW) {
                XjsSetSideItem side[SET_TREE_N];
                int sn = XjsSetSideItems(side, SET_TREE_N);
                float sy = pt.y + p->sideScroll;   /* 按下命中按未滚动坐标比对 (与松开/绘制同源) */
                for (int si = 0; si < sn; si++) {
                    if (!(sy >= side[si].y && sy < side[si].y + SS(34))) continue;
                    p->pressSide = side[si].idx;   /* 分类/分组节点: 记待定, 松开触发 */
                    return 0;
                }
                return 0;
            }
            float y = pt.y + p->scroll;
            RECT crc;
            GetClientRect(hwnd, &crc);
            float w = (float)crc.right;
            for (auto& card : p->cards)
                for (auto& r : card.rows) {
                    if (r.act == ACT_NONE || r.disabled || y < r.y || y >= r.y + r.h) continue;
                    /* 开关/按钮/胶囊/表格行: 必须点在行尾控件上才待定 (点击行文本/空白/输入格不触发);
                       选项行(视图/皮肤)整行待定; 双钮行按落点判主/第二钮 */
                    if (r.ctrl == CT_SWITCH || r.ctrl == CT_BUTTON || r.ctrl == CT_PILL || r.ctrl == CT_TROW) {
                        if (XjsSetRowControl2Hit(r, (float)pt.x, y, w - SS(28))) {
                            if (r.act2 == ACT_NONE) return 0;
                            p->pressAct = r.act2;   /* 第二钮: 记待定, 松开触发 */
                            return 0;
                        }
                        if (!XjsSetRowControlHit(r, (float)pt.x, y, w - SS(28))) return 0;
                    }
                    p->pressAct = r.act;   /* 记待定, 松开触发 */
                    return 0;
                }
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (p->recHotkey) {
                XjsWindowScope scope(XjsSetOwner());   /* Toast 落到打开设置的窗口 */
                UINT vk = (UINT)wParam;
                if (vk == VK_ESCAPE) {   /* 取消录制, 保留原热键 */
                    p->recHotkey = false;
                    p->rowsDirty = true;
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
                if (vk == VK_DELETE) {   /* 清除已设热键并取消注册 (2026-09-17 用户口径: 可取消注册) */
                    XjsSearchWindow* ow = XjsSetOwner();
                    HWND mainH = XjsSearchWindow::MainHwnd();
                    if (ow && mainH) {
                        UnregisterHotKey(mainH, ID_HOTKEY_SHOW + ow->uiIndex);
                        ow->hotkeyMod = 0;
                        ow->hotkeyVk = 0;
                        ow->hotkeyActive = false;
                        XjsSaveConfig();
                        XjsToastShow(s_set.hwnd, (L"「" + ow->name + XjsT(L"提示.模式快捷键已清除")).c_str(), XTOAST_SUCCESS, SS(1));
                    }
                    p->recHotkey = false;
                    p->rowsDirty = true;
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
                /* 纯修饰键: 等待组合中的普通键, 实时回显已按住的组合 (抬起也随之刷新)。
                   必须 rowsDirty 强制重建行模型 — XjsSetBuildRows 在 !rowsDirty 时早退,
                   只 InvalidateRect 画不出新的组合文字 (实时回显曾整体失效) */
                if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
                    vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
                    vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
                    vk == VK_LWIN || vk == VK_RWIN) {
                    p->rowsDirty = true;
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
                UINT mod = 0;
                if (GetKeyState(VK_CONTROL) & 0x8000) mod |= MOD_CONTROL;
                if (GetKeyState(VK_MENU) & 0x8000) mod |= MOD_ALT;
                if (GetKeyState(VK_SHIFT) & 0x8000) mod |= MOD_SHIFT;
                if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) mod |= MOD_WIN;
                p->recHotkey = false;
                /* 快捷键每窗: 写入 owner 的 hotkeyMod/Vk, 登记在主窗 hwnd (id = ID_HOTKEY_SHOW+档案槽) */
                XjsSearchWindow* ow = XjsSetOwner();
                HWND mainH = XjsSearchWindow::MainHwnd();   /* 热键登记在主窗 (随主窗角色迁移) */
                if (ow && mainH) {
                    int id = ID_HOTKEY_SHOW + ow->uiIndex;
                    UnregisterHotKey(mainH, id);
                    if (RegisterHotKey(mainH, id, mod, vk)) {
                        ow->hotkeyMod = mod;
                        ow->hotkeyVk = vk;
                        ow->hotkeyActive = true;
                        XjsSaveConfig();
                        XjsToastShow(s_set.hwnd, (L"「" + ow->name + XjsT(L"提示.模式快捷键已更新") + XjsHotkeyText(mod, vk)).c_str(), XTOAST_SUCCESS, SS(1));
                    } else {
                        ow->hotkeyActive = (RegisterHotKey(mainH, id, ow->hotkeyMod, ow->hotkeyVk) != FALSE);   /* 复原旧热键 (顺带刷新生效态) */
                        XjsToastShow(s_set.hwnd, XjsT(L"提示.快捷键被占用"), XTOAST_WARN, SS(1));
                    }
                }
                p->rowsDirty = true;
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;   /* 吞掉按键: 避免组合键触发系统菜单/提示音 */
            }
            /* 输入字段聚焦时: 排除目录框 Enter=添加路径; 表格输入格 Enter/Esc=收束聚焦
               (文本即活数据留在字段里, 保存走行尾"保存"钮); 其余编辑键交组件路由 */
            if (XjsEditFocused(hwnd)) {
                XjsEditField* ef = XjsEditFocused(hwnd);
                if (wParam == VK_RETURN) {
                    if (ef == &s_set.pathEd) { XjsSetAddExcludedPath(hwnd); return 0; }
                    ef->SetFocused(hwnd, false);
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
                if (wParam == VK_ESCAPE) {
                    ef->SetFocused(hwnd, false);
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
                if (XjsEditRouteMsg(hwnd, msg, wParam, lParam)) return 0;
            }
            if (wParam == VK_ESCAPE) {
                if (p->dlgOpen) XjsSetCloseRebuildDialog(hwnd);
                else DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_CHAR:
            if (p->recHotkey) return 0;   /* 录制热键时字符不入输入框 */
            if (XjsEditRouteMsg(hwnd, msg, wParam, lParam)) return 0;
            return 0;   /* 设置窗无其它字符行为, 吞掉防误触 */
        case WM_IME_CHAR:
            if (!p->recHotkey) XjsEditRouteMsg(hwnd, msg, wParam, lParam);   /* 聚焦期吞掉防双份插入 */
            return 0;
        case WM_IME_STARTCOMPOSITION:
            XjsEditRouteMsg(hwnd, msg, wParam, lParam);   /* 组字窗钉到字段 */
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case WM_IME_COMPOSITION: {
            if (XjsEditRouteImeResult(hwnd, lParam)) { InvalidateRect(hwnd, NULL, FALSE); return 0; }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (p->recHotkey) {   /* 修饰键抬起: 刷新实时组合回显 (须重建行模型, 同按下分支) */
                p->rowsDirty = true;
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            break;
        case WM_SETCURSOR: {
            if (LOWORD(lParam) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                if (XjsEditFieldHit(hwnd, pt)) { SetCursor(LoadCursorW(NULL, IDC_IBEAM)); return TRUE; }
                if (XjsSetLinkRowHit(hwnd, pt)) { SetCursor(LoadCursorW(NULL, IDC_HAND)); return TRUE; }
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_ACTIVATE:
            XjsCaretBlink::SetWindowActive(hwnd, LOWORD(wParam) != WA_INACTIVE);   /* 失活熄光标 (组件统一口径) */
            break;
        case WM_TIMER:
            if (wParam == ID_TIMER_CARET) {
                XjsCaretBlink::TickWindow(hwnd);   /* 输入字段光标闪烁 */
                return 0;
            }
            if (wParam == ID_TIMER_TOAST) {
                if (!XjsToastTimerTick(hwnd, SS(1))) KillTimer(hwnd, ID_TIMER_TOAST);
                return 0;
            }
            if (wParam == ID_TIMER_SETLIVE) {
                /* 内存/性能分析页: 实时数据 1s 采样 — 签名比对, 数据变了才重建行模型
                   (对话框开着时冻结底层; 其余分类空转零开销) */
                if ((p->cat == SC_MEMORY || p->cat == SC_PERF) && !p->dlgOpen) {
                    std::wstring sig = XjsSetLiveSignature();
                    if (sig != p->liveSig) {
                        p->liveSig = std::move(sig);
                        p->rowsDirty = true;
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                }
                return 0;
            }
            break;
        case WM_DESTROY:
            XjsSetFreeResources();
            XjsSetFreeMdDocs();   /* md 文档实例随窗销毁释放 (见 XjsSetFreeMdDocs 注) */
            KillTimer(hwnd, ID_TIMER_TOAST);   /* Toast 组件定时器摘除 */
            KillTimer(hwnd, ID_TIMER_SETLIVE);   /* 实时数据刷新摘除 */
            XjsWindowComponentsDetach(hwnd);   /* 组件统一退登记 (Toast 条目/画刷 + 输入字段登记 + 闪烁驱动) */
            s_filterEds.clear();   /* 表格字段池随窗释放 (unique_ptr 析构摘登记); 重开时 reload 从引擎重建 */
            s_aliasEds.clear();
            p->pathEd.SetFocused(hwnd, false);   /* 同切分类口径: 失焦 + 清几何 (残留 area = 隐形输入框命中) */
            p->pathEd.area = {};
            p->hwnd = NULL;
            p->hoverRow = p->hoverSide = -1;
            p->scroll = 0;
            /* 瞬态状态全部复位 (s_set 是文件级单例, 窗销毁后残留会在重开时"复活"):
               recHotkey 残留 = 重开后首次按键即注册全局热键; press 待定 / dlgOpen 残留 =
               重开首帧画重建遮罩对话框 / 首次 WM_LBUTTONUP 误回放执行命令 */
            p->recHotkey = false;
            p->dlgOpen = false;
            p->pressAct = 0;
            p->pressSide = -1;
            p->pressDlg = 0;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void XjsRegisterSettingsClass(HINSTANCE hInst) {
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Xjs_SettingsWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.style = CS_DBLCLKS;   /* 缺它系统不发 WM_LBUTTONDBLCLK, 输入字段双击选词失效 */
    wc.hIcon = XjsAppIconBig();      /* 应用图标 (exe 资源 32512), 与搜索窗同一来源 */
    wc.hIconSm = XjsAppIconSmall();  /* 小图标: 任务栏悬停预览标题行取它 */
    wc.lpszClassName = L"XJS_SettingsWnd";
    RegisterClassExW(&wc);
}

void XjsSettingsShow(int cat) {
    s_setOwner = XjsSearchWindow::Cur();   /* 绑定发起设置的窗口 (换肤/视图/预览作用到它) */
    if (s_setOwner) s_setOwner->SyncSkin();   /* g_skin 认回 owner: 首帧画刷即 owner 皮肤 (不等首次 WM_PAINT) */
    s_filterReload = s_aliasReload = true;   /* 打开即重拉: 表格回显引擎当前生效配置 (正式版同款) */
    if (s_set.hwnd) {
        wchar_t t[64];
        _snwprintf(t, 64, XjsT(L"应用.设置窗口标题"), s_setOwner ? s_setOwner->name.c_str() : XJS_MAIN_WIN_NAME);
        SetWindowTextW(s_set.hwnd, t);
        s_set.rowsDirty = true;   /* 行勾选态按新 owner 重建 */
        if (cat >= 0) XjsSetSwitchCat(s_set.hwnd, cat);   /* 直达指定分类页 (同分类 = 无操作) */
        /* owner 换人: 皮肤配色/行勾选/标题都变了, 必须整窗失效 —— 已可见窗口 ShowWindow(SW_RESTORE)
           不产生 WM_PAINT, 旧画面会一直挂到鼠标滑过才重绘 */
        InvalidateRect(s_set.hwnd, NULL, FALSE);
        ShowWindow(s_set.hwnd, SW_RESTORE);
        SetForegroundWindow(s_set.hwnd);
        SetTimer(s_set.hwnd, ID_TIMER_SETLIVE, 1000, NULL);   /* 内存/性能分析页实时刷新 (其余分类空转) */
        return;
    }
    s_set.scale = g_s;   /* 初值取主窗尺度 (通常同屏); 跨屏由 WM_DPICHANGED 纠正 */
    s_set.rowsDirty = true;
    if (cat >= 0) {   /* 新窗直达: 首次建行模型即目标分类 (旧 md 占位行一并作废, 口径同切分类) */
        s_set.cat = cat;
        memset(s_mdPending, 0, sizeof(s_mdPending));
    } else {
        s_set.cat = 0;
    }
    s_set.scroll = 0;
    int w = (int)SS(980), h = (int)SS(740);   /* 默认大窗 (捐赠二维码要看得清), 可拖边自由调整/最大化 */
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    if (w > sw - 40) w = sw - 40;
    if (h > sh - 100) h = sh - 100;
    RECT orr;
    GetWindowRect(g_hWnd, &orr);
    int x = orr.left + ((orr.right - orr.left) - w) / 2;
    int y = orr.top + ((orr.bottom - orr.top) - h) / 2;
    if (x < 8) x = 8;
    if (y < 8) y = 8;
    wchar_t title[64];
    _snwprintf(title, 64, XjsT(L"应用.设置窗口标题"), s_setOwner ? s_setOwner->name.c_str() : XJS_MAIN_WIN_NAME);
    HWND hwnd = CreateWindowExW(0, L"XJS_SettingsWnd", title,
        WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME | WS_MAXIMIZEBOX, x, y, w, h, g_hWnd, NULL, GetModuleHandleW(NULL), NULL);
    if (!hwnd) return;
    s_set.hwnd = hwnd;
    SetTimer(hwnd, ID_TIMER_SETLIVE, 1000, NULL);   /* 内存/性能分析页实时刷新 (其余分类空转) */
    SendMessageW(hwnd, WM_SETICON, ICON_BIG,   (LPARAM)XjsAppIconBig());     /* 窗口级图标 (设置窗有任务栏按钮) */
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)XjsAppIconSmall());
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));   /* 深色标题栏 (20H1+) */
    DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));                                     /* 更早 build 兜底, 失败无碍 */
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    InvalidateRect(hwnd, NULL, FALSE);
}

/* ☰菜单"捐赠"入口: 打开/前置设置窗并直达捐赠页。
   分类 id (SC_*) 收在设置模块内不经公共头外泄, 外部经此命名入口, 防下标漂移静默指错页 */
void XjsSettingsShowDonate() {
    XjsSettingsShow(SC_DONATE);
}
