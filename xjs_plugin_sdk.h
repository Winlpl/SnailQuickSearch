#ifndef XJS_PLUGIN_SDK_H
#define XJS_PLUGIN_SDK_H
/* ============================================================================
 * xjs_plugin_sdk.h — 蜗牛快搜(D2D) 原生插件 SDK (纯 C ABI, v3)
 * ============================================================================
 * 插件作者只需要本头文件 (+ 可选的 xunjieso.h / xunjieso.lib 直连搜索引擎)。
 * v2 变更: 订阅事件不再携带 JSON 数据载荷 (曾把全选打成几百 MB paths JSON = 卡顿源),
 *   OnEvent 只传 (事件类型, 窗口令牌, 结果对象), 数据一律直连引擎自取; 宿主表删除
 *   SelectionJson (同理, 插件自取选中集)。
 * v3 变更: 命令/动态菜单/预览接管/搜索模式/输入拦截回调全部 C 化, 文件上下文传
 *   引擎 FileId 数组 (不再传 paths JSON), 预览只传 fileId (路径/名称/扩展名经
 *   xjs_db_GetPath/GetName 自取); 宿主表 OpenPath → OpenFile (按 fileId 打开)。
 * v4 变更: 新增"面板接管"能力 (清单 能力:"preview-panel") — 插件整体接管预览面板内容区,
 *   宿主转发 鼠标/滚轮/键盘/IME (XjsPlugin_OnPanelEvent), 插件交付整块位图 (PanelDeliver
 *   Bitmap)。原 preview 能力只交付静态位图/文本, 撑不起可交互 UI (AI 助手等聊天面板)。
 *   宿主表按"只追加"纪律扩了 6 个 Panel* 指针, 插件取用前先校验 host->size。
 *
 * 形态 = 清单 + DLL 双件:
 *   plugins\<插件id>\manifest.json   声明 (权限/能力/菜单/搜索模式…)
 *   plugins\<插件id>\<名称>.dll      代码 (manifest "动态库" 字段指名; 可缺省 = 纯声明式插件)
 *
 * 硬契约 (违反必崩必返工):
 *   1. 边界上只传 C 类型与不透明句柄; 禁 STL/类/虚表/宽字符跨边界。
 *   2. 字符串一律 UTF-8、NUL 结尾; 长度参数 int(字节), 大整数 long long。
 *   3. 输出 = 调用方缓冲: int Fn(..., char* buf, int cap) — buf==NULL 或 cap==0 时
 *      返回"所需字节数"(不含 NUL), 否则返回实际写入字节数; 负值 = 错误码。
 *      宿主不分配任何需要插件释放的内存, 插件同样不得要求宿主释放。
 *   4. 结构体只追加字段: 插件先校验 host->size 够不够自己要用的最后一个指针再调
 *      (旧宿主跑新插件 = 干净失败, 不崩)。
 *   5. 异常禁止越过边界 (双方都不做 try); 崩溃由宿主 VEH 取证设施记录 (模块+偏移)。
 *   6. 插件是外部输入: 返回给宿主的长度/JSON/路径都会被校验, 越界即拒绝。
 *
 * 线程契约 (错线程调用返回 XJS_PLUGIN_ERR_THREAD, 不排队不阻塞不崩):
 *   - 宿主→插件回调: 只在 UI 线程、绝不在 WM_PAINT 内; 重活请自开线程 (Shutdown 里 join)。
 *   - 插件→宿主: 文件/存储/Log/Subscribe 任意线程; 界面/对话框/Toast/GetSkinJson 仅 UI 线程。
 *   - 插件自建窗口 (type:"app") 的生命周期完全自理, 宿主不代管。
 *
 * 直连搜索引擎 (本 SDK 特有口径, 比经宿主封装更灵活):
 *   include "xunjieso.h" 并链接 xunjieso.lib — 宿主进程已加载 xunjieso.dll,
 *   加载器按模块名绑定到同一实例; xjs_GetDefaultEngine() 即取引擎句柄。
 *   注意: 结果对象要在引擎空闲态创建 (xjs_db_GetEngineState()==0), 库未就绪时创建的
 *   结果对象会被永久定成文件名序。
 *   权限闸只作用于本表宿主 API; 引擎直连物理上不经权限闸 (原生插件非沙箱, 诚实口径)。
 * ============================================================================ */

#define XJS_PLUGIN_ABI_VERSION 4

/* x64 下调用约定为空 (与 xunjieso.h 的 XJS_CALL 同口径) */
#define XJS_PLUGIN_CALL

typedef struct XjsPluginCtx XjsPluginCtx;      /* 宿主创建的插件身份 (所有回调/API 首参, 原样回传) */
typedef unsigned long long XjsWindowToken;     /* 不透明窗口令牌 (只在宿主回调参数里产生, 原样回传;
                                                  0 = 主窗缺省); 插件不得缓存/伪造, 失效 = ERR_NOTFOUND */

/* 错误码 (所有 API 通用; 返回"长度"的 API 以负值为错) */
enum {
    XJS_PLUGIN_OK = 0,
    XJS_PLUGIN_ERR_ARG = -1,        /* 参数非法 (路径/键名/JSON 越界) */
    XJS_PLUGIN_ERR_PERM = -2,       /* 未声明所需权限 (宿主闸门, 照 manifest "权限") */
    XJS_PLUGIN_ERR_THREAD = -3,     /* 错线程调用 (仅 UI 线程的 API) */
    XJS_PLUGIN_ERR_STATE = -4,      /* 状态不允许 (如引擎忙) */
    XJS_PLUGIN_ERR_NOTFOUND = -5,   /* 目标不存在 (路径/窗口令牌失效) */
    XJS_PLUGIN_ERR_IO = -6,         /* 文件读写失败 */
    XJS_PLUGIN_ERR_OVERFLOW = -7,   /* 输出缓冲不够 (返回所需长度) */
    XJS_PLUGIN_ERR_FAIL = -8        /* 其它失败 */
};

/* 事件掩码 (Subscribe; 事件 = 纯信号, 只带 (事件类型, 窗口令牌, 结果对象), 不打包任何数据 —
   搜索结果对象就是引擎对象, 选中集/结果集/路径一律经 xunjieso.h 直连自取) */
enum {
    XJS_PLUGIN_EVT_SEARCH_COMPLETE = 1 << 0,   /* 搜索完成 (window/result = 所属窗口; count 自取 xjs_result_GetCount) */
    XJS_PLUGIN_EVT_SELECTION       = 1 << 1,   /* 选中变化 (window/result = 所属窗口; 选中自取 GetSelectedCount/CopySelectedFileId) */
    XJS_PLUGIN_EVT_SYNC            = 1 << 2,   /* 文件同步变化 (进程级: window=0, result=NULL) */
    XJS_PLUGIN_EVT_DBSTATE         = 1 << 3,   /* 库状态 (预留; 进程级: window=0, result=NULL) */
    XJS_PLUGIN_EVT_SKIN            = 1 << 4    /* 皮肤变化 (window = 换肤窗口令牌; 自建窗口取新皮肤重绘, 配合 GetSkinJsonOf) */
};

/* Toast 类型 */
enum { XJS_PLUGIN_TOAST_INFO = 0, XJS_PLUGIN_TOAST_SUCCESS = 1, XJS_PLUGIN_TOAST_WARN = 2, XJS_PLUGIN_TOAST_ERROR = 3 };

/* 面板接管事件类型 (OnPanelEvent 的 ev->type; 全部 UI 线程) */
enum {
    XJS_PANEL_OPEN = 1,         /* 会话开启 (serial/w/h/scale 有效; 渲染首帧并交付) */
    XJS_PANEL_CLOSE = 2,        /* 会话结束 (预览头 ✕ / 窗口销毁 / 宿主强制; 收尾自绘状态与线程) */
    XJS_PANEL_RESIZE = 3,       /* 面板像素尺寸变化 (serial 已递增; 按新 w/h/scale 重排重交付) */
    XJS_PANEL_MOUSE_MOVE = 4,   /* x/y = 面板内容区内像素坐标 (按下拖拽中面板外也持续转发; x=y=-1 = 指针已离开面板, 悬停态应复位) */
    XJS_PANEL_LDOWN = 5,  XJS_PANEL_LUP = 6,  XJS_PANEL_RDOWN = 7, XJS_PANEL_RUP = 8,
    XJS_PANEL_DBLCLK = 9,
    XJS_PANEL_WHEEL = 10,       /* delta = 滚轮增量 (正值向上, WHEEL_DELTA 整数倍) */
    XJS_PANEL_KEY_DOWN = 11,    /* delta = 虚拟键码 (仅插件 PanelSetFocus(1) 期间投递) */
    XJS_PANEL_KEY_CHAR = 12,    /* ch = UTF-32 码点 (键盘字符与 IME 上屏均拆码点投递) */
    XJS_PANEL_FOCUS = 13,       /* delta = 1 宿主窗口激活 / 0 失活 (熄自绘光标用) */
    XJS_PANEL_CAPTURE_LOST = 14,/* 鼠标捕获被系统夺走 (拖拽态应复位) */
    XJS_PANEL_KEY_BLUR = 15     /* 宿主收回键盘让渡 (用户点击了面板内容区以外的宿主 UI):
                                   输入框/对话框字段应失焦熄光标; 键盘此后不再投递 KEY_* */
};

/* 面板事件载荷 (纯 C 值; x/y 坐标 = 面板内容区左上为原点的物理像素) */
typedef struct XjsPanelEvent {
    unsigned int structSize;   /* = sizeof(XjsPanelEvent) */
    int          type;         /* XJS_PANEL_* */
    long long    serial;       /* 面板世代 (OPEN/RESIZE 递增; 交付按它对齐) */
    int          w, h;         /* OPEN/RESIZE: 内容区像素尺寸 */
    float        scale;        /* OPEN/RESIZE: dpi×页面缩放 (96dpi=1.0); 字号/几何按它缩放 */
    int          x, y;         /* 鼠标: 面板内容区内像素坐标 */
    int          delta;        /* WHEEL: 滚轮增量; KEY_DOWN: 虚拟键码; FOCUS: 1/0 */
    unsigned int flags;        /* 修饰键: 1=Ctrl 2=Shift 4=Alt */
    unsigned int ch;           /* KEY_CHAR: UTF-32 码点 */
} XjsPanelEvent;

/* 命令/动态菜单的文件上下文类别 (OnCommand/BuildMenu 的 kind 参数) */
enum {
    XJS_PLUGIN_CTX_NONE  = 0,   /* 无文件上下文 (状态栏项/搜索框命令) */
    XJS_PLUGIN_CTX_FILE  = 1,
    XJS_PLUGIN_CTX_DIR   = 2,
    XJS_PLUGIN_CTX_DRIVE = 3
};

/* 插件信息 (GetInfo 返回; 指针必须指向插件内静态存储, 宿主只读不释放) */
typedef struct XjsPluginInfo {
    unsigned int abiVersion;   /* 插件编译时的 XJS_PLUGIN_ABI_VERSION, 必须 <= 宿主支持上限 */
    unsigned int structSize;   /* = sizeof(XjsPluginInfo) */
    const char*  id;           /* UTF-8; 必须与目录名/manifest "标识" 一致 (宿主交叉校验, 不一致拒载) */
    const char*  version;
} XjsPluginInfo;

/* 宿主 API 函数表 (Init 传入; 指针终身有效) */
struct XjsPluginHost {
    unsigned int abiVersion;   /* = XJS_PLUGIN_ABI_VERSION */
    unsigned int size;         /* = sizeof(XjsPluginHost); 插件先校验再取用后面的指针 */

    /* ---- 文件 (任意线程) ---- */
    /* 路径信息: {"exists":true,"dir":false,"size":123,"mtime":..,"ctime":..,"atime":..,"attrs":33} */
    int (XJS_PLUGIN_CALL *PathInfoJson)(XjsPluginCtx*, const char* path, char* buf, int cap);              /* file.read */
    /* 分块读: 返回实际读到的字节数; *eof=1 表示到文件尾; offset<0 = 从当前位置语义不支持, 一律绝对偏移 */
    int (XJS_PLUGIN_CALL *ReadFile)(XjsPluginCtx*, const char* path, long long offset, int len,
                                    char* buf, int cap, int* eof);                                          /* file.read */
    int (XJS_PLUGIN_CALL *WriteFile)(XjsPluginCtx*, const char* path, const void* data, int len);           /* file.write */
    /* 列目录: [{"name":"a.txt","dir":false,"size":1}] (不含 "." "..") */
    int (XJS_PLUGIN_CALL *ListDirJson)(XjsPluginCtx*, const char* path, char* buf, int cap);                /* file.read */
    /* pathsJson = ["..",".."] → 删除到回收站; 返回 JSON {"ok":n,"fail":n} */
    int (XJS_PLUGIN_CALL *DeleteToRecycle)(XjsPluginCtx*, const char* pathsJson, char* buf, int cap);       /* file.write */
    /* copy 非 0 = 复制否则移动; 返回 {"ok":n,"fail":n} */
    int (XJS_PLUGIN_CALL *MoveCopyJson)(XjsPluginCtx*, const char* pathsJson, const char* destDir,
                                        int copy, char* buf, int cap);                                       /* file.write */
    /* 改名/移动到 newPath (同盘改名或跨路径移动); newPath 已存在 = ERR_NOTFOUND 不覆盖 */
    int (XJS_PLUGIN_CALL *RenameTo)(XjsPluginCtx*, const char* path, const char* newPath, char* buf, int cap); /* file.write */

    /* ---- 系统 (任意线程) ---- */
    /* 直接 CreateProcessW 执行 (不走 cmd.exe, 防注入); argsJson = ["arg1","arg2"] (逐参传递);
       cwd 可 NULL; timeoutMs <=0 = 无限等。返回 {"exitCode":n,"stdout":"..","stderr":"..","timedOut":bool}
       stdout/stderr 上限 4MB, 超出截断并附 "truncated":true */
    int (XJS_PLUGIN_CALL *ExecJson)(XjsPluginCtx*, const char* exe, const char* argsJson,
                                    const char* cwd, int timeoutMs, char* buf, int cap);                     /* exec */
    int (XJS_PLUGIN_CALL *ClipboardGetText)(XjsPluginCtx*, char* buf, int cap);                              /* file.read */
    int (XJS_PLUGIN_CALL *ClipboardSetText)(XjsPluginCtx*, const char* utf8);                                /* file.write */
    /* 文件/目录选择对话框 (仅 UI 线程): kind = "file"|"files"|"folder"|"save";
       optsJson = {"title":"..","filter":[["标签","*.png;*.jpg"],…],"initialDir":"..","initialName":".."}
       返回 {"paths":[…]} / {"path":".."} (save) / {} = 取消 */
    int (XJS_PLUGIN_CALL *DialogJson)(XjsPluginCtx*, const char* kind, const char* optsJson, char* buf, int cap);

    /* ---- 界面 (仅 UI 线程; 标注者需 ui 权限, 其余免权限) ---- */
    /* 置入搜索词并触发搜索; kw=NULL 保持现词; execute=0 只填不搜; window=0 = 主窗 */
    int (XJS_PLUGIN_CALL *SearchSetText)(XjsPluginCtx*, XjsWindowToken window,
                                         const char* kwUtf8, const char* modeUtf8, int execute);              /* ui */
    /* 打开索引文件 (走宿主打开行为: 提权/异步/打开后隐藏) ; reveal 非 0 = 资源管理器定位;
       fileId = 引擎文件ID (路径宿主经 xjs_db_GetPath 自取; 非索引路径走插件自建进程执行) */
    int (XJS_PLUGIN_CALL *OpenFile)(XjsPluginCtx*, XjsWindowToken window, int fileId, int reveal);  /* ui */
    int (XJS_PLUGIN_CALL *ShowMainWindow)(XjsPluginCtx*);                                                     /* ui */
    /* 右下角 Toast 通知 (免费, 照正式版 notify 口径) */
    int (XJS_PLUGIN_CALL *Toast)(XjsPluginCtx*, XjsWindowToken window, const char* utf8, int type);
    /* 唤起任意顶层窗口到前台 (宿主"借前台线程"实现, 插件自建窗口启动后用它上前台) */
    int (XJS_PLUGIN_CALL *Summon)(XjsPluginCtx*, void* hwnd);
    /* 皮肤/尺度 (仅 UI 线程): {"dpi":120,"zoom":1.25,"font":"Microsoft YaHei UI",
       "bg1":"#14171f","bg2":"#1b2030","panel":"#…","text":"#…","dim":"#…","accent":"#…","line":"#…"}
       供插件自建窗口画同风格 UI; 颜色 #RRGGBB */
    int (XJS_PLUGIN_CALL *GetSkinJson)(XjsPluginCtx*, char* buf, int cap);
    /* 预览接管异步交付 (P2 preview 能力; requestId 必须来自 OnPreview 的 reqJson, 过期请求宿主静默丢弃)
       位图 = 32bpp BGRA 自上而下, stride 为字节行距 */
    int (XJS_PLUGIN_CALL *PreviewDeliverBitmap)(XjsPluginCtx*, int requestId, int w, int h,
                                                const void* bgra, int stride);
    int (XJS_PLUGIN_CALL *PreviewDeliverText)(XjsPluginCtx*, int requestId, const char* utf8);

    /* ---- 事件订阅 (任意线程调 Subscribe, 交付恒在 UI 线程; 掩码见 XJS_PLUGIN_EVT_*) ---- */
    int (XJS_PLUGIN_CALL *Subscribe)(XjsPluginCtx*, unsigned int mask);

    /* ---- 插件私有存储 (任意线程; 一文件一键, 落 plugins\<id>\data\<key>;
            键 = UTF-8 任意文字 (建议中文主键, 与配置文件口径一致), 禁控制字符与 \/:*?"<>|, ≤128 字节;
            值为任意字节 (约定 UTF-8)) ---- */
    int (XJS_PLUGIN_CALL *StorageGet)(XjsPluginCtx*, const char* key, char* buf, int cap);
    int (XJS_PLUGIN_CALL *StorageSet)(XjsPluginCtx*, const char* key, const char* data, int len);
    int (XJS_PLUGIN_CALL *StorageRemove)(XjsPluginCtx*, const char* key);

    /* ---- 调试 (任意线程; 只进调试器 OutputDebugString, 不落盘) ---- */
    void (XJS_PLUGIN_CALL *Log)(XjsPluginCtx*, int level /*0 debug 1 info 2 warn 3 error*/, const char* utf8);

    /* ---- 面板接管 (v4 追加; 声明 能力:"preview-panel" 且实现 OnPanelEvent 才有效) ----
       插件整块接管预览面板体 (含头部带, 宿主不画头部; 关闭按钮 = 插件自绘 ✕, 点击调
       PanelClose 结束接管并恢复接管前预览)。
       交互模型: 宿主转发鼠标/滚轮/键盘/IME (OnPanelEvent), 插件渲染整块位图交付; 渲染前先
       PanelGetInfo 取当前 serial/尺寸 (任意线程), 交付 serial 不符即被静默丢弃 — 尺寸变化
       (窗口缩放/预览宽拖/页面缩放/DPI) 后按 RESIZE 事件的尺寸重排重交付即可。 */
    /* 打开/激活本插件对该窗口的接管 (仅 UI 线程)。预览面板未开时先展开 (关闭时按打开前状态
       恢复 — 预览本来就关着, 关聊天时连预览一起关); 已激活时幂等。OPEN 事件随后送达 */
    int (XJS_PLUGIN_CALL *PanelOpen)(XjsPluginCtx*, XjsWindowToken window);
    /* 结束接管 (仅 UI 线程; 恢复接管前预览状态)。插件自绘 ✕ 与窗口销毁宿主也会代发 CLOSE */
    int (XJS_PLUGIN_CALL *PanelClose)(XjsPluginCtx*, XjsWindowToken window);
    /* 当前世代/像素尺寸/缩放 (任意线程; 渲染前取用) */
    int (XJS_PLUGIN_CALL *PanelGetInfo)(XjsPluginCtx*, XjsWindowToken window,
                                        long long* serial, int* w, int* h, float* scale);
    /* 交付整块面板位图 (任意线程; 32bpp BGRA 预乘 alpha 自上而下, stride=字节行距;
       serial/w/h 与当前世代不符 = 静默丢弃; 宿主 CPU 拷贝进本域 — 跨渲染域铁律) */
    int (XJS_PLUGIN_CALL *PanelDeliverBitmap)(XjsPluginCtx*, XjsWindowToken window, long long serial,
                                              int w, int h, const void* bgra, int stride);
    /* 键盘路由开关 (仅 UI 线程): want=1 插件输入框聚焦, 宿主把键盘/IME 让给面板;
       want=0 归还列表/搜索框 (点击面板输入框外时插件应主动归还) */
    int (XJS_PLUGIN_CALL *PanelSetFocus)(XjsPluginCtx*, XjsWindowToken window, int want);
    /* IME 组字/候选窗锚点 (仅 UI 线程; x/y = 面板内容区内像素坐标, 光标移动时调用) */
    int (XJS_PLUGIN_CALL *PanelSetCaret)(XjsPluginCtx*, XjsWindowToken window, int x, int y);

    /* 指定窗口的皮肤 (v4 追加, 与 Panel* 同口径: 取用前先校验 host->size; 免权限, 仅 UI 线程):
       window=0 = 默认窗口, 其余 = 窗口令牌; JSON 同 GetSkinJson。
       自建窗口要"跟随某窗口皮肤"用它取色, 再订阅 EVT_SKIN 在皮肤变化后重取重绘 */
    int (XJS_PLUGIN_CALL *GetSkinJsonOf)(XjsPluginCtx*, XjsWindowToken window, char* buf, int cap);

    /* 面板内容区矩形 (v4 追加, 与 Panel* 同口径: 取用前先校验 host->size; 仅 UI 线程):
       取本插件对面板的接管矩形 — hwnd = 所属搜索窗口 HWND, x/y/w/h = 面板内容区在该窗口
       客户区内的物理像素矩形 (与 OPEN/RESIZE 事件的 w/h 同口径)。供"真子窗口"型面板
       (WebView2 等自带输入体系的渲染层) 定位/缩放自建子窗口 — 子窗口盖住面板后鼠标键盘
       自然归它, 宿主转发事件与位图交付都不再需要。会话未激活 = ERR_STATE。 */
    int (XJS_PLUGIN_CALL *PanelGetRect)(XjsPluginCtx*, XjsWindowToken window, void** hwnd,
                                        int* x, int* y, int* w, int* h);
};

/* ==================== 插件导出面 ==================== */

/* 固定导出 (必须三个都有, 缺一拒载) */
extern "C" __declspec(dllexport) const XjsPluginInfo* XJS_PLUGIN_CALL XjsPlugin_GetInfo(void);
extern "C" __declspec(dllexport) int  XJS_PLUGIN_CALL XjsPlugin_Init(XjsPluginCtx* ctx, const XjsPluginHost* host);
extern "C" __declspec(dllexport) void XJS_PLUGIN_CALL XjsPlugin_Shutdown(XjsPluginCtx* ctx);

/* 能力回调 (按 manifest capabilities 选实现; 宿主 GetProcAddress 探测, 缺失 = 该动态能力无效)。
   参数一律 UTF-8 + C 数值; 除注明返回值外全部 void。
   fileIds = 引擎 FileId 数组 (只在本次调用期间有效, 插件只读; 路径/名称/扩展名经
   xjs_db_GetPath/GetName 自取 — 宿主不传路径, 引擎数据就是事实源) */
extern "C" __declspec(dllexport) void XJS_PLUGIN_CALL XjsPlugin_OnCommand(XjsPluginCtx*, const char* cmdIdUtf8,
                                                                          XjsWindowToken window,
                                                                          const int* fileIds, int count, int kind);
/* 动态菜单 (searchBoxMenu/fileContextMenu 构建期调用): kindUtf8 = "fileCtx"|"searchBox";
   fileCtx 带 fileIds/count (input=NULL), searchBox 带 inputUtf8 (fileIds=NULL);
   返回菜单项 JSON [{"标识":"..","文字":"..","顺序":0},..] 写入 buf (声明式配置数据, 中文键,
   与 manifest "菜单" 项同构); 返回值同"调用方缓冲"约定; 无导出或返回 0 项 = 只用 manifest 静态菜单 */
extern "C" __declspec(dllexport) int  XJS_PLUGIN_CALL XjsPlugin_BuildMenu(XjsPluginCtx*, const char* kindUtf8,
                                                                          XjsWindowToken window,
                                                                          const int* fileIds, int count,
                                                                          const char* inputUtf8,
                                                                          char* buf, int cap);
/* 搜索模式接管 (无 template 的 searchModes): 返回 1 = 已接管 (本次搜索由插件负责), 0 = 不处理 */
extern "C" __declspec(dllexport) int  XJS_PLUGIN_CALL XjsPlugin_OnSearchMode(XjsPluginCtx*, const char* modeIdUtf8,
                                                                             XjsWindowToken window,
                                                                             const char* inputUtf8);
/* 搜索输入拦截 (searchInputIntercept): 返回 1 = 拦截本次原生搜索 */
extern "C" __declspec(dllexport) int  XJS_PLUGIN_CALL XjsPlugin_OnInput(XjsPluginCtx*, XjsWindowToken window,
                                                                        const char* inputUtf8);
/* 预览接管 (preview): fileId = 预览目标引擎文件ID (路径/名称自取); 返回 1 = 已接管 (将异步
   交付), 0 = 不处理 (宿主回落内置预览); 世代过期宿主只丢结果不再通知 */
extern "C" __declspec(dllexport) int  XJS_PLUGIN_CALL XjsPlugin_OnPreview(XjsPluginCtx*, int requestId,
                                                                          XjsWindowToken window, int fileId);
/* 订阅事件派发 ( eventType = XJS_PLUGIN_EVT_* 之一; window = 所属窗口令牌 (进程级事件 = 0);
   result = 所属窗口的引擎结果对象, 即 xjs_result* 不透明句柄 — 直连引擎的插件直接当
   xjs_result* 用 (GetSelectedCount/CopySelectedFileId/xjs_db_GetPath… 自取全部数据),
   进程级事件 (SYNC/DBSTATE) 为 NULL。宿主不打包任何数据载荷) */
extern "C" __declspec(dllexport) void XJS_PLUGIN_CALL XjsPlugin_OnEvent(XjsPluginCtx*, int eventType,
                                                                        XjsWindowToken window, void* result);
/* 面板接管事件 (preview-panel 能力; 仅 UI 线程, 绝不在 WM_PAINT 内 — 事件里只更新状态并
   交付位图, 渲染慢可回自己线程, 但交付字节必须是稳定快照) */
extern "C" __declspec(dllexport) void XJS_PLUGIN_CALL XjsPlugin_OnPanelEvent(XjsPluginCtx*,
                                                                             XjsWindowToken window,
                                                                             const XjsPanelEvent* ev);
/* 宿主即将退出 (引擎尚未销毁的最后通知; 之后 DLL 不卸载, 线程必须已在此前收尾) */
extern "C" __declspec(dllexport) void XJS_PLUGIN_CALL XjsPlugin_OnHostGone(XjsPluginCtx*);

#endif /* XJS_PLUGIN_SDK_H */
