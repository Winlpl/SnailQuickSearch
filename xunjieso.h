/*
 * xunjieso.h
 * 迅捷搜 API 原生 C/C++ 头文件
 * 说明：文本编码统一采用 UTF-8
 * 程序必须使用管理员权限或者服务运行
 *
 * === 核心特性 ===
 * 1. 极致性能：采用并行计算架构
 * 2. 多模式搜索：支持通配符(拼音+首拼)、正则表达式、SQL语句等多种查询语法
 * 3. 实时同步：文件系统监控，即时感知创建、修改、移动、删除操作
 * 4. 自定义扩展：可添加自定义字段（如文件大小、创建时间、文件评分等）
 * 5. 异步操作：所有核心操作支持异步模式，不阻塞主线程
 * 6. 快速索引：软件采用解析MFT，实现快速索引全盘文件
 * 7. 安全：SQL语句仅支持SELECT语句，唯一有写入权限的只有 xjs_db_Save()
 *
 * === 注意事项 ===
 * 1.请勿在任何回调事件中, 对数据库进行写操作.

 * === 快速上手 ===
 * 1. xjs_Create()               创建引擎
 * 2. xjs_db_ScanPath()          遍历分区建立索引 (或 xjs_db_Load() 加载已有数据库)
 * 3. xjs_result_Create()        创建搜索结果对象
 * 4. xjs_result_SetCallback()   设置回调事件 (可选)
 * 5. xjs_result_Query()         发起搜索
 * 6. xjs_result_GetCount() / xjs_result_GetFileId() 读取结果
 * 7. 退出前: xjs_sync_AllStop() → xjs_db_Save() → xjs_Destroy()
 
 * === 技术特点 ===
 * - 线程安全：关键操作提供加锁机制
 * - 低内存占用：385 万文件仅占用约 188MB 内存，索引结构紧凑
 * - 高效存储：采用紧凑数据结构，支持海量文件管理
 * - 目前仅支持 Windows
 *
 * 所有返回 const char* 的函数不需要自己释放内存，内部会自动管理内存。
 *      任何情况下它都不会返回`nullptr`只会返回如果失败会返回可读内存 + '\0'
 *      意味着您可以直接使用 std::string() 来接收.
 * 扫描分区/加载数据库, 阶段, 无法读取数据库的文件名, 路径等信息, 统一会返回空文本, 或者失败值
 *      遍历磁盘/加载数据库期间(引擎写锁被全程持有), 所有需要加引擎锁的接口都会立即返回默认值/失败,
 *      不再阻塞等待, 防止调用线程假死; 失败原因: 60=正在遍历磁盘, 35=正在加载数据库(详见 xjs_GetLastError)
 * 需要注意的是，所有返回的数据指针，请第一时间获取或拷贝。
 * 版本：见 xjs_GetVersion()
 * 官网：https://www.xunjieso.com
 * 表结构: (需要注意的是, 部分需要自己添加)
CREATE TABLE alltable (
    ID         integer,    -- 编号
    ParentID   integer,    -- 父目录ID
    Path       text,       -- 完整文件路径
    FName      text,       -- 文件名（不含扩展名）
    Ext        text,       -- 扩展名
    ParentName text,       -- 直接父目录名称
    ParentPath text,       -- 直接父目录路径
    AnyParent  text,       -- 任意一级父目录名称
    Size       bigint,      -- 文件大小（字节数，支持 '100M'/'2G' 简写）
    CreateTime bigint,     -- 创建时间（OLE日期, 可能会没有创建此字段）
    ModTime    bigint,     -- 修改时间（OLE日期, 可能会没有创建此字段）
    AccessTime bigint,     -- 访问时间（OLE日期, 可能会没有创建此字段）
    FileType   text,       -- 文件类型分类（视频/音频/图片/文档/办公/程序/压缩/系统/其他）
    IsDir      integer,    -- 是否为目录 (1=是 0=否)
    Alias      text,       -- 别名 (可能会没有创建此字段)
    Score      smallint,   -- 文件评分 (可能会没有创建此字段)
    FAttr      text,       -- 文件属性字符串（如 "A__D" 表示存档+目录, 可能会没有创建此字段）
    FileContent text,       -- 文件内容（用于内容搜索，读取时有200MB上限）
    FileNumber  integer      -- 文件编号 (用于调试查看使用)
);
*/

#pragma once
#include <windef.h> // BOOL

// ============================================================================
// 1. 定义调用约定
// ============================================================================
#if defined(_WIN64) || defined(__x86_64__)
    // 64位下只有一种调用约定，无需指定
    #define XJS_CALL 
#else
    // 32位
    #define XJS_CALL __stdcall
#endif

// ============================================================================
// 2. 定义导出宏 (注意：这里不要包含 XJS_CALL)
// ============================================================================
#ifdef XJS_EXPORTS
    #define XJS_API __declspec(dllexport)
#else
    #define XJS_API __declspec(dllimport)
#endif


#ifdef __cplusplus
extern "C" {
#endif

typedef struct xjs_engine xjs_engine;
typedef struct xjs_result xjs_result;

// ============================================================================
// 枚举定义 (消除魔法数字)
// ============================================================================

/* 引擎回调事件类型 (xjs_SetCallback 的 eventType 参数)
   触发线程说明:
   - 重建数据库线程: 遍历/重建索引时的工作线程
   - 文件同步线程:   实时同步监控的工作线程(事件在引擎写锁内触发, 回调内禁止写库)
   - 加载线程:       异步加载数据库时的工作线程 */
enum xjs_event_type {
    XJS_EVENT_LOAD_START       = 1,  /* 正在加载数据库 (同步加载时在调用线程触发; 异步重建时当前版本不触发) */
    XJS_EVENT_LOAD_COMPLETE    = 2,  /* 数据库加载完成 (异步加载/重建时在加载线程触发; 同步加载时在调用线程触发) */
    XJS_EVENT_ENUM_PARTITION   = 3,  /* 正在枚举某分区 (重建数据库线程触发) */
    XJS_EVENT_ENUM_PROGRESS    = 4,  /* 枚举进度 (遍历进度提示线程触发, 约每50毫秒) */
    XJS_EVENT_ENUM_COMPLETE    = 5,  /* 所有盘符枚举完成 (重建数据库线程触发) */
    XJS_EVENT_ENUM_TERMINATED  = 6,  /* 遍历已终止 (重建数据库线程触发; 原因: 0=用户停止 1=程序退出) */
    XJS_EVENT_ENUM_FAILED      = 7,  /* 遍历失败 (无可用盘符, 遍历未开始即失败; 异步重建时在重建数据库线程触发, 同步重建时在调用线程触发) */
    XJS_EVENT_SYNC_CREATE      = 10, /* 同步_文件创建 (文件同步线程触发, 引擎写锁内) */
    XJS_EVENT_SYNC_MODIFY      = 11, /* 同步_文件修改 (文件同步线程触发, 引擎写锁内) */
    XJS_EVENT_SYNC_MOVE        = 12, /* 同步_文件移动 (文件同步线程触发, 引擎写锁内) */
    XJS_EVENT_SYNC_DELETE      = 13, /* 同步_文件删除 (文件同步线程触发, 引擎写锁内) */
    XJS_EVENT_SYNC_START       = 14, /* 同步_启动 (实时同步监控线程启动成功; 重建数据库路径在重建数据库线程触发, 加载数据库路径在调用线程触发) */
    XJS_EVENT_SYNC_STOP        = 15, /* 同步_停止 (实时同步监控停止; 由调用停止同步的线程触发, 该事件位于引擎写锁内) */
    XJS_EVENT_SYNC_AFTER       = 16, /* 同步后_文件变化 (文件同步处理完成后触发, 文件同步线程, 引擎读锁内, 回调内禁止写库/等待锁; 参数: 变化JSON(UTF-8文本); 返回值忽略) */
    XJS_EVENT_RESULT_CREATE    = 20, /* 搜索结果_已创建 (调用线程触发, 通常为主线程) */
    XJS_EVENT_RESULT_DELETE    = 21  /* 搜索结果_即将销毁 (一律在内部异步删除线程触发, 不区分调用线程) */
};

/* 数据库状态 (xjs_db_GetEngineState 的返回值) */
enum xjs_db_state {
    XJS_DB_STATE_IDLE    = 0, /* 空闲 */
    XJS_DB_STATE_LOADING = 1, /* 正在加载数据库 */
    XJS_DB_STATE_SAVING  = 2, /* 正在保存数据库 */
    XJS_DB_STATE_SCANNING = 3, /* 正在扫描磁盘(建立索引) */
    XJS_DB_STATE_SYNCING = 4, /* 正在同步文件变化 */
    XJS_DB_STATE_SEARCHING = 5  /* 正在搜索 */
};

/* 数据库加载状态码 (xjs_db_Load 的返回值; 一码一义: 负数=未发起加载, 正数=已发起但失败) */
enum xjs_load_state {
    XJS_LOAD_OK                  =  0, /* 成功(异步加载时=已提交, 真实成败见"加载完成"回调) */
    XJS_LOAD_INVALID_ENGINE      = -1, /* 引擎句柄为空 */
    XJS_LOAD_EMPTY_PATH          = -2, /* 路径为空(错误码30) */
    XJS_LOAD_FILE_NOT_FOUND      = -3, /* 数据库文件不存在(错误码31) */
    XJS_LOAD_SCANNING_OR_LOADING = -4, /* 正在遍历/加载, 不能再次加载(错误码60/35) */
    XJS_LOAD_BUSY                = -5, /* 数据库正忙, 正在保存/搜索等(错误码35) */
    XJS_LOAD_WRITELOCK_FAILED    = -6, /* 获取数据库写锁失败(超时/被占用, 错误码35) */
    XJS_LOAD_OPEN_FAILED         =  1, /* 文件打不开/无读取权限/映射创建失败 */
    XJS_LOAD_VERSION_MISMATCH    =  2, /* 格式版本不匹配(旧库拒载), 请重新建立索引 */
    XJS_LOAD_HWID_MISMATCH       =  3, /* 硬件码不匹配(文件来自其他电脑), 请重新建立索引 */
    XJS_LOAD_FIELDTABLE_CORRUPT  =  4, /* 字段表段损坏 */
    XJS_LOAD_ROWDATA_CORRUPT     =  5, /* 行数据段损坏 */
    XJS_LOAD_NAMEBLOB_CORRUPT    =  6, /* 文件名blob段损坏, 半载内存已清空, 请重新建立索引 */
    XJS_LOAD_ALIAS_CORRUPT       =  7, /* 别名溢出表段损坏, 已清库, 请重新建立索引 */
    XJS_LOAD_SYNC_RESTORE_FAILED =  8  /* 加载成功但USN同步监听恢复失败, 已清库, 需重新遍历 */
};

/* 搜索结果回调事件类型 (xjs_result_SetCallback 的 eventType 参数)
   触发线程说明:
   - 搜索线程:     后台检索线程(含TBB并行搜索的工作线程), 不是调用 xjs_result_Query 的线程
   - 文件同步线程: 实时同步监控的工作线程(引擎写锁内)
   - 异步图标线程: 独立的后台图标获取线程 */
enum xjs_result_event_type {
    XJS_RESULT_EVENT_BEFORE   = 1,  /* 即将搜索，返回非0拦截; 搜索线程触发 */
    XJS_RESULT_EVENT_PROCESS  = 2,  /* 搜索过程函数; 搜索线程(TBB工作线程)触发 */
    XJS_RESULT_EVENT_WAIT     = 3,  /* 搜索等待; 仅在 waitComplete=TRUE 时, 调用线程触发 */
    XJS_RESULT_EVENT_COMPLETE = 4,  /* 搜索完成; 搜索线程触发 (即使 waitComplete=TRUE 也不在调用线程触发) */
    XJS_RESULT_EVENT_CHANGE   = 10, /* 搜索结果变化; 文件同步线程触发(引擎写锁内) */
    XJS_RESULT_EVENT_DRAW_ICON = 11, /* 图标绘制事件; 异步图标线程触发(非主线程); 末尾参数: 回调信息(UTF-8, 见 xjs_result_GetFileIco) */
    XJS_RESULT_EVENT_FAILED   = 12,  /* 搜索失败(如正则创建失败); 搜索线程触发, 参数为错误JSON(UTF-8) */
    XJS_RESULT_EVENT_ICON_ASK = 13   /* 询问继续获取图标; 异步图标线程在真实获取图标之前触发(搜索结果读锁内, 回调内禁止写库/等待锁); 返回: 非0=继续获取, 0=放弃当前图标; 参数: 搜索指纹, 文件ID, 表项索引, 图标大小, 回调信息(UTF-8) */
};

/* 搜索关键词类型 (xjs_result_Query 的 keywordType 参数; 类型必须强制指定: 0/1/2 为强制类型, -2 为多重搜索, -3 为Lua脚本过滤, -4 为Lua执行(脚本即程序)) */
enum xjs_keyword_type {
    XJS_KEYWORD_LUA_EXEC = -4, /* Lua执行模式(脚本即程序, 经 xjs_result_Query 以本类型提交): 脚本自主遍历数据库/自主排序/return ID数组=最终结果 */
    XJS_KEYWORD_LUA      = -3, /* Lua脚本(过滤模式): 搜索词为Lua脚本, 每文件求值布尔谓词; 宿主多线程驱动, 可作多重搜索阶段 {"搜索模式":"Lua"}; 环境 f 表见 Lua脚本示例 目录 */
    XJS_KEYWORD_MULTI    = -2, /* 多重搜索(JSON链式搜索): 搜索词为JSON数组 [{"搜索模式":"通配符","搜索词":"你好"},...]; 先搜索A再以A的结果为基础继续搜索B; 多重搜索里不允许嵌套多重搜索 */
    XJS_KEYWORD_WILDCARD = 0,  /* 通配符(强制) */
    XJS_KEYWORD_REGEX    = 1,  /* 正则表达式(强制) */
    XJS_KEYWORD_SQL      = 2   /* SQL语句(强制) */
};
// ============================================================================
// 辅助函数
// ============================================================================
// 格式化文件大小(返回"5.2 GB")
// size: 文件大小(字节)
XJS_API const char* XJS_CALL xjs_util_FormatFileSize(long long size);


// 格式化时间(返回:"2023-10-24 15:30:00"; 传入0返回空字符串)
// msTimestamp: 毫秒时间戳
XJS_API const char* XJS_CALL xjs_util_FormatTimestamp(long long msTimestamp);

// ============================================================================
// 异常捕获
// ============================================================================
// 异常回调 (xjs_EnableException 的 exceptionCallback 参数)
// 签名同系统异常过滤器: 收到异常信息指针, 返回处理结果; 不处理的异常必须返回 EXCEPTION_CONTINUE_SEARCH(0) 放行
// 注意: 该回调会同时注册为"首个VEH+未处理异常过滤器", 因此也会先于 __try 收到首次机会异常
//      (含断点/单步/OutputDebugString 等良性异常), 需自行识别放行, 详见内置默认处理的白名单行为
typedef LONG (XJS_CALL* xjs_ExceptionCallback)(struct _EXCEPTION_POINTERS* exceptionPointers);

// 启用异常捕获 (全局SEH异常处理器, 对全部API/全部线程生效; 默认关闭)
// 默认处理: 捕获到异常时写崩溃日志(xunjieso_捕获崩溃N.txt)并弹出错误框, 避免进程直接崩溃; 捕获后继续执行旧异常处理器
// enableTry: 是否启用内部try-catch保护
// exceptionCallback: 外部自定义异常回调(见 xjs_ExceptionCallback); 传 NULL(0) 使用内置默认处理
XJS_API void XJS_CALL xjs_EnableException(BOOL enableTry, xjs_ExceptionCallback exceptionCallback);

// 禁用异常捕获 (卸载全局SEH异常处理器)
XJS_API void XJS_CALL xjs_DisableException();

// ============================================================================
// 引擎 API
// ============================================================================

// 创建引擎，返回文件引擎句柄
// 别名/筛选器配置不再在创建时传入, 改为运行期设置:
//   别名:   xjs_alias_SetAliasJSON / xjs_alias_GetAliasJSON
//   筛选器: xjs_filter_SetFilterJSON / xjs_filter_GetFilterJSON
XJS_API xjs_engine* XJS_CALL xjs_Create(void);

// 销毁引擎 (会先等待内部各线程退出, 并自动销毁所有未释放的 xjs_result*; 销毁后请勿再使用该引擎句柄)
XJS_API void XJS_CALL xjs_Destroy(xjs_engine* engine);

// 设置用户设定的默认文件引擎句柄 (仅供上层便捷使用, 内部API不隐式消费; 引擎销毁时会自动清空)
XJS_API void XJS_CALL xjs_SetDefaultEngine(xjs_engine* engine);

// 获取用户设定的默认文件引擎句柄 (未设置时返回 NULL)
XJS_API xjs_engine* XJS_CALL xjs_GetDefaultEngine(void);

// 获取版本号，读取模块版本资源, 格式如 "1.2.0.1"
XJS_API const char* XJS_CALL xjs_GetVersion(void);

// 获取最近的操作错误码 (线程独立; 错误码从不清零, 仅失败操作时写入)
// 取值参见 XJS_ERROR_* 宏定义(见文件底部"搜索类型与错误信息接口"区域)
XJS_API int XJS_CALL xjs_GetLastError(xjs_engine* engine);

// 获取最近的操作错误文本 (全部错误码均有专属文本, 未识别码返回"未知的错误")
XJS_API const char* XJS_CALL xjs_GetLastErrorMsg(xjs_engine* engine);

// 设置回调 (返回 FALSE 的情况: 引擎无效/搜索线程内调用/正在遍历磁盘或加载数据库(错误码60/35, 回调应在发起遍历前设置好))
XJS_API BOOL XJS_CALL xjs_SetCallback(
    xjs_engine* engine, 
    int eventType, /* 参见 xjs_event_type 枚举 */
    const void* callback, /*
                            XJS_EVENT_LOAD_START (1):
                                typedef void (*LoadStartCallback)(void* userData, xjs_engine* engine);
                            XJS_EVENT_LOAD_COMPLETE (2):
                                typedef void (*LoadCompleteCallback)(void* userData, xjs_engine* engine, int fileCount);
                            XJS_EVENT_ENUM_PARTITION (3):
                                typedef void (*EnumPartitionCallback)(void* userData, xjs_engine* engine, const char* driveLetter);
                            XJS_EVENT_ENUM_PROGRESS (4):
                                typedef INT (*EnumProgressCallback)(void* userData, xjs_engine* engine, const char* driveLetter, int totalCount, int enumeratedCount);
                                // 在遍历时，每隔50毫秒触发一次, 返回 非0,则会停止遍历
                            XJS_EVENT_ENUM_COMPLETE (5):
                                typedef void (*EnumCompleteCallback)(void* userData, xjs_engine* engine, int elapsedMs);
                            XJS_EVENT_ENUM_TERMINATED (6):
                                typedef void (*EnumTerminatedCallback)(void* userData, xjs_engine* engine, int reason);
                                // reason: 0=用户调用停止遍历(xjs_db_StopScan)主动终止, 1=程序退出
                            XJS_EVENT_ENUM_FAILED (7):
                                typedef void (*EnumFailedCallback)(void* userData, xjs_engine* engine);
                                // 没有可用盘符, 遍历未开始即失败
                            XJS_EVENT_SYNC_CREATE (10):
                                typedef INT (*SyncFileCreateCallback)(void* userData, xjs_engine* engine, const char* filePath);
                                // 返回 非0,则会拦截本次同步
                            XJS_EVENT_SYNC_MODIFY (11):
                                typedef INT (*SyncFileModifyCallback)(void* userData, xjs_engine* engine, const char* filePath);
                                // 返回 非0,则会拦截本次同步
                            XJS_EVENT_SYNC_MOVE (12):
                                typedef INT (*SyncFileMoveCallback)(void* userData, xjs_engine* engine, const char* srcPath, const char* destPath);
                                // 返回 非0,则会拦截本次同步
                            XJS_EVENT_SYNC_DELETE (13):
                                typedef INT (*SyncFileDeleteCallback)(void* userData, xjs_engine* engine, const char* filePath);
                                // 返回 非0,则会拦截本次同步
                            XJS_EVENT_SYNC_START (14):
                                typedef void (*SyncStartCallback)(void* userData, xjs_engine* engine);
                                // 实时同步监控线程启动成功(重建/加载数据库后触发)
                            XJS_EVENT_SYNC_STOP (15):
                                typedef void (*SyncStopCallback)(void* userData, xjs_engine* engine);
                                // 实时同步监控停止(遍历/加载/销毁引擎时触发); 该事件位于引擎写锁内, 回调内禁止写库
                            XJS_EVENT_SYNC_AFTER (16):
                                typedef INT (*SyncAfterCallback)(void* userData, xjs_engine* engine, const char* json);
                                // 文件同步处理完成后触发(文件同步线程, 引擎读锁内, 回调内禁止写库/等待锁), 返回值忽略
                                // json: {"变化类型":1新建/2删除/3修改/4重命名,"ID":..,"路径":"..","是否目录":bool,
                                //        "旧路径":".."(重命名),"被删除子ID数量":n(删除目录),
                                //        "大小前"/"大小后","修改时间前"/"修改时间后","创建时间前"/"创建时间后",
                                //        "访问时间前"/"访问时间后","文件属性前"/"文件属性后"} (UTF-8)
                                //        仅包含已开启的数据库字段; 时间为13位毫秒时间戳(自1970-01-01); 修改时"前"为同步前数据库旧值,"后"为磁盘新值
                            XJS_EVENT_RESULT_CREATE (20):
                                typedef void (*ResultCreateCallback)(void* userData, xjs_engine* engine, xjs_result* result);
                            XJS_EVENT_RESULT_DELETE (21):
                                typedef void (*ResultDeleteCallback)(void* userData, xjs_engine* engine, xjs_result* result);
        */
    void* userData
);

// 获取用户设定的值，运行时的值，不会保存到数据库中
XJS_API void* XJS_CALL xjs_GetUserValue(xjs_engine* engine);

// 设置用户设定的值，运行时的值，不会保存到数据库中(非线程安全)
// userData: 用户自定义值
XJS_API void XJS_CALL xjs_SetUserValue(xjs_engine* engine, void* userData);

/*
    锁类。通常是不需要锁的，除非需要完整的同步才需要考虑使用锁。通常来说只允许读。
    它是一个高级锁，支持锁升级：读过程中可以升级为写锁，但解锁时请务必配对解锁(类型与加锁一致, 否则锁状态会错乱)。
    加锁返回 FALSE 的情况: 引擎无效; 当前线程是搜索线程(检索线程/TBB工作线程); 写锁升级条件不满足;
                           正在遍历磁盘/加载数据库(遍历线程全程持有写锁, 等待会阻塞到遍历结束导致假死, 故直接失败, 错误码60/35)。
    如果您没有完全理解读写锁，请不要使用。
*/

// 判断当前线程是否再读锁内(如果在写锁内, 也返回TRUE)
XJS_API BOOL XJS_CALL xjs_IsReadLock(xjs_engine* engine);


// 判断当前线程是否再写锁内(如果在读锁内, 也返回FALSE)
XJS_API BOOL XJS_CALL xjs_IsWriteLock(xjs_engine* engine);


// 加锁
// isReadOnly: 真=读锁, 假=写锁
XJS_API BOOL XJS_CALL xjs_Lock(xjs_engine* engine, BOOL isReadOnly);

// 解锁
// isReadOnly: 解锁类型, 必须与加锁时一致
XJS_API BOOL XJS_CALL xjs_Unlock(xjs_engine* engine, BOOL isReadOnly);

// 判断一个搜索结果对象是否仍存在于文件引擎中(即`xjs_result*`是否有效; 搜索中调用安全)
// result: 要判断的搜索结果对象
XJS_API BOOL XJS_CALL xjs_ResultIsExist(xjs_engine* engine, xjs_result* result);

// 获取引擎内部当前正被搜索线程处理的结果对象(不一定是"最近发起搜索的"那个)
// 没有搜索在运行时返回 nullptr
XJS_API xjs_result* XJS_CALL xjs_GetCurrentSearchObject(xjs_engine* engine);

// 判断引擎是否正在枚举磁盘(加载数据库或扫描分区).返回 TRUE=正在枚举/加载
// 注意: 当前线程持有写锁时恒返回 FALSE
XJS_API BOOL XJS_CALL xjs_IsScanning(xjs_engine* engine);

// 判断引擎是否正在搜索.返回 TRUE=正在搜索
XJS_API BOOL XJS_CALL xjs_IsSearching(xjs_engine* engine);

// ============================================================================
// 数据库 API
// ============================================================================

// 添加数据库字段 (必须在遍历之前添加; 仅在下次遍历建表时生效; 重复添加同名字段会覆盖, 无副作用)
// 返回 FALSE 时可用 xjs_GetLastError()/xjs_GetLastErrorMsg() 获取失败原因(30=参数无效/35=数据库正忙)
XJS_API BOOL XJS_CALL xjs_db_AddField(
    xjs_engine* engine, 
    const char* fieldName, // 字段名: 文件大小 | 修改时间 | 文件评分 | 别名 | 文件属性 | 创建时间 | 访问时间
    const char* fieldType  // 保留参数, 目前自动忽略(存储类型由引擎硬编码), 传递任何值都没用
);

// 获取数据库已开启的全部字段, 返回 JSON 数组, 例如: ["文件名索引","文件大小","修改时间","文件评分"]
// 4个必建字段(文件名索引/父索引/文件类型/文件编号)排在前, 7个可选字段随后; 数据库尚未建立(未遍历/未加载)时返回空数组
XJS_API const char* XJS_CALL xjs_db_GetEnabledFields(xjs_engine* engine);

// 判断指定字段是否已开启 (可判断字段: 4个必建字段 + 7个可选字段)
// 返回 TRUE=已开启, FALSE=未开启或字段名无效
XJS_API BOOL XJS_CALL xjs_db_IsFieldEnabled(xjs_engine* engine, const char* fieldName);

// 获取数据库内存占用大小 (字节)
XJS_API long long XJS_CALL xjs_db_GetMemorySize(xjs_engine* engine);

// 获取数据库内存占用大小 (字符串, 多行详情报告, 含各分项内存与总占用)
XJS_API const char* XJS_CALL xjs_db_GetMemorySizeString(xjs_engine* engine);

// 加载数据库
// 返回加载状态码(enum xjs_load_state, 一码一义; 负数=未发起加载, 正数=已发起但失败):
//   XJS_LOAD_OK(0)=成功 / XJS_LOAD_INVALID_ENGINE(-1)=句柄为空 / XJS_LOAD_EMPTY_PATH(-2)=路径为空 /
//   XJS_LOAD_FILE_NOT_FOUND(-3)=文件不存在 / XJS_LOAD_SCANNING_OR_LOADING(-4)=正在遍历或加载 /
//   XJS_LOAD_BUSY(-5)=数据库正忙 / XJS_LOAD_WRITELOCK_FAILED(-6)=写锁获取失败 /
//   XJS_LOAD_OPEN_FAILED(1)=文件打不开 / XJS_LOAD_VERSION_MISMATCH(2)=格式版本不匹配 /
//   XJS_LOAD_HWID_MISMATCH(3)=硬件码不匹配 / XJS_LOAD_FIELDTABLE_CORRUPT(4)=字段表段损坏 /
//   XJS_LOAD_ROWDATA_CORRUPT(5)=行数据段损坏 / XJS_LOAD_NAMEBLOB_CORRUPT(6)=文件名blob段损坏 /
//   XJS_LOAD_ALIAS_CORRUPT(7)=别名溢出表段损坏 / XJS_LOAD_SYNC_RESTORE_FAILED(8)=USN同步监听恢复失败(已清库)
// 失败时亦可用 xjs_GetLastError()/xjs_GetLastErrorMsg() 获取错误码与详情
// 异步加载时立即返回 XJS_LOAD_OK, 真实成败需通过"加载完成"回调或后续查询判断
// 加载期间其他数据库接口暂不可用; 加载成功后自动按保存的断点恢复同步监控
// path: 数据库文件路径
// async: 真=异步加载, 不阻塞调用线程
XJS_API int XJS_CALL xjs_db_Load(xjs_engine* engine, const char* path, BOOL async);

// 保存数据库(如果程序要退出,那么请在保存之前,务必调用xjs_sync_AllStop,否则下次加载数据库不会断点续传的方式同步更新文件.)
// 保存期间持有写锁, 搜索等操作会被阻塞
// 返回假时可用 xjs_GetLastError()/xjs_GetLastErrorMsg() 获取失败原因(33=保存失败/35=数据库正忙)
// path: 数据库文件路径
XJS_API BOOL XJS_CALL xjs_db_Save(xjs_engine* engine, const char* path);

// ============================================================================
// 加载/保存性能统计 API (数据库加载/保存各阶段耗时统计, 诊断用)
// ============================================================================

/** @brief 设置"加载/保存性能统计"开关
    enable: 真=采集数据(默认开, 加载/保存为低频操作, 开销可忽略), 假=各阶段跳过采集零开销
    - 关闭后已累计的数据保留, 可配合 xjs_db_ClearPerformanceText 使用.
    @return 真=设置成功; 假=引擎无效/在搜索线程调用, 或引擎正忙(遍历/保存/加载中)未取得写锁——此时开关保持原值不变,
            可用 xjs_GetLastError() 确认(35=数据库正忙 / 30=参数无效), 稍后重试. */
XJS_API BOOL XJS_CALL xjs_db_SetPerformanceSwitch(xjs_engine* engine, BOOL enable);

/** @brief 获取"加载/保存性能统计"开关当前状态 */
XJS_API BOOL XJS_CALL xjs_db_IsPerformanceSwitch(xjs_engine* engine);

// 取数据库加载/保存各阶段的性能计时统计文本 (UTF-8, \r\n分隔; 每行: 阶段名: 总耗时 [次数] 平均:平均耗时)
// 统计阶段: 保存_等待写锁/属性表镜像/整理文件名/行区写出/文件名blob/默认查询顺序/落盘,
//           加载_文件载入/导入文件名/默认查询顺序/重置默认查询(罕见分支)/文件编号排序/排除目录恢复; 没发生过的阶段次数为0
// 开关可用 xjs_db_SetPerformanceSwitch 切换(默认开); 采样流程建议: Clear → Save/Load → Get
// 返回的文本位于调用线程的临时缓冲区, 下次在该线程调用返回文本的API前会失效, 需长期保存请自行复制
// 引擎无效或数据库正在遍历/加载中时返回空字符串""
XJS_API const char* XJS_CALL xjs_db_GetPerformanceText(xjs_engine* engine);

// 清零所有加载/保存性能统计阶段(诊断采样用): 建议采样流程=清零→触发一次保存/加载→取结果文本
// 引擎无效或数据库正在遍历/加载中时无效果
XJS_API void XJS_CALL xjs_db_ClearPerformanceText(xjs_engine* engine);

// ============================================================================
// 遍历性能统计 API (遍历分区/重建数据库各阶段耗时统计, 诊断用)
// ============================================================================

// 取遍历分区(xjs_db_ScanPath 重建数据库)各阶段的性能计时统计文本 (UTF-8, \r\n分隔)
// 树形带├/└前缀, 每行: 名称: 累计耗时 [次数] 平均:平均耗时 自身:自身耗时(不含子阶段) 占父:占父阶段%
// 统计树: 枚举_NTFS→MFT扫描→{回调_文件名转换/回调_后缀与大小/回调_取时间, 入库_添加文件→(查重/文件名池/行插入/父编号登记/映射表), 枚举后处理→(父目录ID与目录大小/刷新评分/Desktop别名/路径别名/EXE别名/lnk别名)};
//         顶层另有: 枚举_U盘, USN监听添加, 枚举收尾→(文件名映射排序/文件编号排序/排除目录重删/默认查询顺序), 结果顺序重建; 没发生过的阶段次数为0
// 多盘遍历各阶段为跨盘累计; U盘路径的刷新段也计入 枚举后处理 节点(此时不在 枚举_U盘 子树内)
// 开关默认开, 可用 xjs_db_SetScanPerformanceSwitch 切换(假=各阶段跳过采集零开销); 采样流程建议: ClearScan → ScanPath → GetScanPerformanceText
// 遍历进行中也可调用: 此时写锁被重建线程持有, 改为无锁直读统计表快照(仅诊断用途), 可在扫描过程中轮询观察各阶段耗时
// 返回的文本位于调用线程的临时缓冲区, 下次在该线程调用返回文本的API前会失效, 需长期保存请自行复制
// 引擎无效时返回空字符串""
XJS_API const char* XJS_CALL xjs_db_GetScanPerformanceText(xjs_engine* engine);

// 清零所有遍历性能统计阶段(诊断采样用): 建议采样流程=清零→遍历分区→取结果文本
// 引擎无效或数据库正在遍历/加载中时无效果
XJS_API void XJS_CALL xjs_db_ClearScanPerformanceText(xjs_engine* engine);

/** @brief 设置"遍历性能统计"开关 (统计阶段见 xjs_db_GetScanPerformanceText)
    enable: 真=采集数据(默认开; 逐文件仅2次时钟读, 开销可忽略), 假=各阶段跳过采集零开销
    - 关闭后已累计的数据保留, 可配合 xjs_db_ClearScanPerformanceText 使用.
    - 遍历进行中不可切换(开关在遍历线程持写锁时读取, 修改同样要求写锁).
    @return 真=设置成功; 假=引擎无效/在搜索线程调用, 或引擎正忙(遍历/保存/加载中)未取得写锁——此时开关保持原值不变,
            可用 xjs_GetLastError() 确认(35=数据库正忙 / 30=参数无效), 稍后重试. */
XJS_API BOOL XJS_CALL xjs_db_SetScanPerformanceSwitch(xjs_engine* engine, BOOL enable);

/** @brief 获取"遍历性能统计"开关当前状态 */
XJS_API BOOL XJS_CALL xjs_db_IsScanPerformanceSwitch(xjs_engine* engine);

// 清空数据库 (清空后文件ID从0重新分配; 用户注册的字段配置也会被清除)
// 返回假时可用 xjs_GetLastError()/xjs_GetLastErrorMsg() 获取失败原因(35=数据库正忙/60=正在遍历磁盘)
XJS_API BOOL XJS_CALL xjs_db_Clear(xjs_engine* engine);

// 遍历分区
// 遍历完成后会自动启动同步监控; 需要管理员权限; 遍历进行中重复调用返回假
// 返回假时可用 xjs_GetLastError()/xjs_GetLastErrorMsg() 获取失败原因(34=遍历失败/35=数据库正忙/50=不能重复建立/51=线程创建失败/53=需要管理员权限/500=程序需要退出)
XJS_API BOOL XJS_CALL xjs_db_ScanPath(
    xjs_engine* engine, 
    const char* pathjson, // 盘符JSON数组, 例如 ["C:\\", "D:\\"]; 传 nullptr 或空数组则遍历电脑上所有可用的磁盘分区并清空原有数据
    BOOL async            // 真=异步遍历, 不阻塞调用线程
);

// 停止当前的遍历/扫描操作 (仅通知扫描线程尽快退出, 不会等待其结束; 不在遍历过程中返回 false)
// 注意: 在写锁内或扫描线程内调用返回 false
// 返回假时可用 xjs_GetLastError()/xjs_GetLastErrorMsg() 获取失败原因(52=并没有在遍历磁盘)
XJS_API BOOL XJS_CALL xjs_db_StopScan(xjs_engine* engine);

// 取遍历进度 - 仅NTFS分区遍历时有效(非NTFS遍历进度恒为0, 完成后跳100); 未在遍历时返回 -1
XJS_API double XJS_CALL xjs_db_GetScanProgress(xjs_engine* engine);

/* 取当前数据库状态 (返回值参见 xjs_db_state 枚举; 多项操作并发时返回最后写入的状态) */
XJS_API int XJS_CALL xjs_db_GetEngineState(xjs_engine* engine);


// 从数据库中删除索引: 文件/目录 (返回实际删除的ID数量; 删除目录时其全部子项一并删除; 路径不存在返回0)
// 注意: 本方法只删一次索引, 若目录仍存在于磁盘且处于同步监控中, 其内新建的文件会被同步线程重新加入数据库,
//       如需"永久不再索引该目录", 请改用原生排除目录 API: xjs_db_AddExcludedDir (删除数据 + 同步拦截 + 遍历兜底 + 属性表持久化)
// 返回 0 且引擎可访问时, 可用 xjs_GetLastError()/xjs_GetLastErrorMsg() 获取失败原因(30=参数无效/35=数据库正忙); 路径不存在删除数量为0不视为失败
// Path: 要删除索引的文件/目录路径
XJS_API int XJS_CALL xjs_db_RemovePath(xjs_engine* engine, const char* Path);


// 删除索引: 文件/目录 (与 xjs_db_RemovePath 的区别: 可分别控制是否同步查询顺序/搜索结果)
// 返回实际删除的ID数量; 删除目录时其全部子项一并删除; 路径不存在返回0
// syncQueryOrder: 真=同步删除各搜索结果的查询顺序(推荐真, 防止删除后二分查找读到已释放的数据库行触发崩溃)
// syncResult:     真=同步从各搜索结果中移除被删ID并触发"搜索结果变化"事件(非SQL查询)
// 返回 0 且引擎可访问时, 可用 xjs_GetLastError()/xjs_GetLastErrorMsg() 获取失败原因(30=参数无效/35=数据库正忙)
// Path: 要删除索引的文件/目录路径
XJS_API int XJS_CALL xjs_db_RemovePathEx(xjs_engine* engine, const char* Path, BOOL syncQueryOrder, BOOL syncResult);


// 设置数据库级别的元信息文本 (key-value)
// 此接口用于写入数据库全局元数据，而非针对某个文件/目录。
// 可用于存储版本号、构建时间、摘要信息、插件扩展字段等。
// 注意：此元信息与文件索引无关，不会影响搜索结果。
// 元信息会随同数据库一起被保存。
// 返回 FALSE 时可用 xjs_GetLastError()/xjs_GetLastErrorMsg() 获取失败原因(30=参数无效)
XJS_API BOOL xjs_db_SetMetaText(xjs_engine* engine, const char* key, const char* value);


// 获取数据库级别的元信息文本 (key-value)
// 此接口用于读取数据库全局元数据，而非针对某个文件/目录。
// 可用于获取版本号、构建时间、摘要信息、插件扩展字段等。
// 注意：此元信息与文件索引无关，不会影响搜索结果。
// 返回值为 UTF-8 文本指针；若 key 不存在，返回 空字符串,而非 NULL。
// key 为 NULL 时同样返回空字符串, 且错误码为 30=参数无效。
// 元信息会随同数据库一起被保存。
XJS_API const char* xjs_db_GetMetaText(xjs_engine* engine, const char* key);


// 删除数据库级别的元信息文本 (key-value)
// 此接口用于删除数据库全局元数据，而非针对某个文件/目录。
// 可用于移除版本号、构建时间、摘要信息、插件扩展字段等。
// 注意：此元信息与文件索引无关，不会影响搜索结果。
// 元信息会随同数据库一起被保存。
// 返回 TRUE 表示删除成功；FALSE 表示 key 不存在或删除失败(可用 xjs_GetLastError 获取原因, key 为 NULL 时为 30=参数无效)。
XJS_API BOOL xjs_db_DelMeta (xjs_engine* engine, const char* key);


// 判断数据库级别的元信息文本 (key-value) 是否存在
// 此接口用于检测数据库全局元数据是否存在指定 key。
// 可用于判断版本号、构建时间、摘要信息、插件扩展字段等是否已写入。
// 注意：此元信息与文件索引无关，不会影响搜索结果。
// 元信息会随同数据库一起被保存。
// 返回 TRUE 表示存在；FALSE 表示不存在(可用 xjs_GetLastError 获取原因, key 为 NULL 时为 30=参数无效)。
XJS_API BOOL xjs_db_HasMeta(xjs_engine* engine, const char* key);



// 取文件总数 (包含文件夹、驱动器等; 为有效记录数)
// 取文件总数EX (文件ID槽位总数, 含已删除文件的空槽; 约等于当前最大文件ID+1)
XJS_API int XJS_CALL xjs_db_GetFileCount(xjs_engine* engine);
XJS_API int XJS_CALL xjs_db_GetFileCountEX(xjs_engine* engine);

// 复制所有文件ID (按ID升序, 自动跳过已删除的空槽; 返回已复制的文件ID数量)
// 返回 0 时可用 xjs_GetLastError()/xjs_GetLastErrorMsg() 获取失败原因(30=参数无效(idArray为NULL或bufferCount<=0)/35=数据库正忙)
// idArray: 输出缓冲区(整数数组)
// bufferCount: 缓冲区容量(元素个数)
XJS_API int XJS_CALL xjs_db_CopyAllFileId(xjs_engine* engine, int* idArray, int bufferCount);

// 判断文件ID是否为文件夹(驱动器); 无效ID返回 FALSE
XJS_API BOOL XJS_CALL xjs_db_IsDir(xjs_engine* engine, int fileId);

// 判断文件ID是否有效 (越界或已删除的空槽均无效)
XJS_API BOOL XJS_CALL xjs_db_IsFileIdValid(xjs_engine* engine, int fileId);

// 取文件大小 (无效ID返回 -1; 字段未开启返回 0)
XJS_API long long XJS_CALL xjs_db_GetFileSize(xjs_engine* engine, int fileId);

// 取文件修改时间 (毫秒时间戳, 无效ID或字段未开启返回 0)
XJS_API long long XJS_CALL xjs_db_GetModifyTime(xjs_engine* engine, int fileId);

// 取文件夹层数 (0: 当前文件夹下, >=1: 子文件, -1:不属于当前文件夹或文件夹ID无效)
// FolderId: 文件夹ID
// fileId: 要判断的文件ID
XJS_API int XJS_CALL xjs_db_GetPathDepth(xjs_engine* engine, int FolderId, int fileId);

// 取文件名 (无效ID返回空字符串; 返回的指针为内部缓冲, 请立即拷贝)
XJS_API const char* XJS_CALL xjs_db_GetName(xjs_engine* engine, int fileId);

// 取父目录 (返回直接父目录的完整路径; 无效ID或根目录返回空字符串)
XJS_API const char* XJS_CALL xjs_db_GetParentDirectory(xjs_engine* engine, int fileId);

// 取文件创建时间 (需要添加字段: "创建时间", 无效ID或字段未开启返回 0)
XJS_API long long XJS_CALL xjs_db_GetCreateTime(xjs_engine* engine, int fileId);

// 取文件访问时间 (需要添加字段: "访问时间", 无效ID或字段未开启返回 0)
XJS_API long long XJS_CALL xjs_db_GetAccessTime(xjs_engine* engine, int fileId);

// 取文件评分 (需要添加字段: "文件评分"; 字段未开启或无效ID返回 0)
XJS_API short XJS_CALL xjs_db_GetRating(xjs_engine* engine, int fileId);

// 增加文件评分 (需要添加字段: "文件评分"), 负数为扣分, 返回计算后的分数; 内部自动申请写入权限; 评分上限 32767
// 失败(引擎正忙/文件ID无效)时返回0, 可用 xjs_GetLastError()/xjs_GetLastErrorMsg() 获取原因(30=参数无效/35=数据库正忙)
XJS_API short XJS_CALL xjs_db_AllRating(xjs_engine* engine, int fileId, short Rating);

// 取文件别名 (需要添加字段: "别名"; 无别名或无效ID返回空字符串)
XJS_API const char* XJS_CALL xjs_db_GetAlias(xjs_engine* engine, int fileId);

// 取文件属性 (需要添加字段: "文件属性"; 值为Windows标准属性位, 字段未开启或无效ID返回 0)
XJS_API int XJS_CALL xjs_db_GetFileAttributes(xjs_engine* engine, int fileId);

// 置文件别名 (需要添加字段: "别名")
// 如果 alias 为 nullptr，那么则是删除当前文件的别名; 传入空字符串会写入空别名(不是删除)
// 别名 UTF-8 编码长度不得超过 32767 字节(数据库落盘长度字段仅15位可用, bit15为占位标记), 超长返回 FALSE(30=参数无效)
// 返回 FALSE 时可用 xjs_GetLastError()/xjs_GetLastErrorMsg() 获取失败原因(30=参数无效/35=数据库正忙)
XJS_API BOOL XJS_CALL xjs_db_SetAlias(
    xjs_engine* engine,
    int fileId, 
    const char* alias // 如果 alias 为 nullptr，那么则是删除当前文件的别名
);

// 取文件类型 (数值分类; 目录=255, 无效ID返回 0)
XJS_API unsigned char XJS_CALL xjs_db_GetFileType(xjs_engine* engine, int fileId);

// 取文件编号, 在NTFS分区下, 文件编号是唯一的.不同分区, 可能会有相同的文件编号, 无效ID返回0
// 一般是用不上的, 属于`xunjieso.dll`开发者调试使用
XJS_API uint64_t XJS_CALL xjs_db_GetFileNumber(xjs_engine* engine, int fileId);

// 取文件类型字符串 (目录返回"目录"; 未知类型或无效ID返回"全部")
XJS_API const char* XJS_CALL xjs_db_GetFileTypeStr(xjs_engine* engine, int fileId);

// 取文件扩展名 (如果是目录, 或者文件没有扩展名, 或ID无效, 将会返回空字符串)
XJS_API const char* XJS_CALL xjs_db_GetFileExt(xjs_engine* engine, int fileId);

// 取文件路径 (无效ID返回空字符串; 路径尾部无反斜杠, 目录也不带; 返回的指针为内部缓冲, 请立即拷贝)
XJS_API const char* XJS_CALL xjs_db_GetPath(xjs_engine* engine, int fileId);

// 获取某个文件夹下的子项ID (返回实际复制的ID数量; 文件夹ID无效或不是目录时返回0; buffer不足时返回截断后的数量)
// 数据库正忙(正在加载/保存/遍历)或读锁获取失败时返回0, 可用 xjs_GetLastError()/xjs_GetLastErrorMsg() 获取原因(35=数据库正忙)
// folderId: 文件夹ID
// recursive: 真=递归遍历所有子目录
// buffer: 接收文件ID的整数缓冲区
// bufferSize: 缓冲区容量(元素个数)
XJS_API int XJS_CALL xjs_db_GetChildrenIds(xjs_engine* engine, int folderId, BOOL recursive, int* buffer, int bufferSize);

// 子项遍历回调 (xjs_db_TraverseChildrenIds 的 callback 参数)
// 返回: 0=继续遍历, 1=停止遍历
// userData/param2/param3: 由 xjs_db_TraverseChildrenIds 的 userData/param2/param3 参数原样回传, 不用可忽略
typedef int (XJS_CALL* xjs_db_TraverseChildrenCallback)(xjs_engine* engine, int fileId, void* userData, void* param2, void* param3);

// 通过回调逐个枚举文件夹下的子项ID (不确定子项数量、不想预分配缓冲区时使用)
// 语义与 xjs_db_GetChildrenIds 一致: 不含 folderId 自身; recursive=真 时递归所有子目录
// 回调在引擎读锁内被同步调用: 回调内可调用其他只读接口(读锁可重入), 禁止调用写库接口/等待引擎锁, 禁止销毁引擎
// 返回本次回调被调用的ID数量(含回调返回1触发停止的那一个); 引擎无效/folderId无效或不是目录/callback为NULL时返回0(错误码30=参数无效)
// 数据库正忙(正在加载/保存/遍历)或读锁获取失败时返回0, 可用 xjs_GetLastError()/xjs_GetLastErrorMsg() 获取原因(35=数据库正忙/60=正在遍历磁盘)
// folderId: 文件夹ID
// recursive: 真=递归遍历所有子目录
// callback: 子项遍历回调函数, 见 xjs_db_TraverseChildrenCallback
// userData: 用户自定义参数, 原样作为回调的第3参数回传
// param2/param3: 原样作为回调的第4/5参数回传, 不用传 NULL
XJS_API int XJS_CALL xjs_db_TraverseChildrenIds(xjs_engine* engine, int folderId, BOOL recursive, xjs_db_TraverseChildrenCallback callback, void* userData, void* param2, void* param3);

// 统计文件夹内的子项数量 (不含 folderId 自身; 递归模式栈递归深入子目录, 零堆分配, O(子树行数); 非递归模式仅走一层子链, O(直接子项数))
// 返回统计数量; 引擎无效/folderId无效或不是目录/mode越界/数据库正忙时返回0, 可用 xjs_GetLastError()/xjs_GetLastErrorMsg() 获取原因(30=参数无效, 35=数据库正忙/60=正在遍历磁盘)
// folderId: 文件夹ID
// mode: 统计模式 0=全部文件(递归整个子树, 不含文件夹) 1=全部文件夹(递归整个子树) 2=全部文件夹+文件(递归整个子树) 3=当前目录下的文件(仅直接子项) 4=当前目录下的文件夹(仅直接子项)
XJS_API int XJS_CALL xjs_db_GetChildrenCount(xjs_engine* engine, int folderId, int mode);

// 取根目录ID (沿父链取根; 对根目录自身返回自身; 无效ID返回0, 引擎无效返回-1)
XJS_API int XJS_CALL xjs_db_GetRootDirectoryId(xjs_engine* engine, int fileId);

// 获取父目录ID (返回 -1 代表没有父目录了(根目录)或ID无效)
XJS_API int XJS_CALL xjs_db_GetParentDirectoryId(xjs_engine* engine, int fileId);

// 取文件路径ID (如果返回 -1，代表数据库中不存在这个路径。需要注意的是，它区分大小写; 路径尾部带反斜杠会查不到)
// path: 完整文件路径(区分大小写)
XJS_API int XJS_CALL xjs_db_GetFileIdByPath(xjs_engine* engine, const char* path);

// 取文件ID所在的盘符 (返回大写盘符字符, 例如 'C'; 未找到返回 '\0')
// 注意: 底层对无效ID无越界防护, 请先用 xjs_db_IsFileIdValid 确认ID有效
XJS_API char XJS_CALL xjs_db_GetDriveLetter(xjs_engine* engine, int fileId);

// 取文件ID所在的盘符根目录ID (例如 C:\ 的ID; 未找到返回 -1; 对根目录自身返回自身)
// 注意: 底层对无效ID无越界防护, 请先用 xjs_db_IsFileIdValid 确认ID有效
XJS_API int XJS_CALL xjs_db_GetDriveRootId(xjs_engine* engine, int fileId);

// 取盘符列表(JSON数组, UTF-8文本指针), 按 盘类型 过滤: 0=全部, 2=可移动磁盘(U盘), 3=固定硬盘, 4=网络盘, 5=光驱, 6=RAM磁盘(与GetDriveType取值一致); 其它值返回空并置错误码30
// 条目字段: 盘符("C:") + 盘类型(整数); 盘符表登记的盘(USN监视中的固定盘)额外带 USN会话/USN编号, 未登记的物理盘(U盘等)只有前两个字段
// 返回UTF-8文本指针, 例如: [{"盘符":"C:","盘类型":3,"USN会话":134099682460551943,"USN编号":0},{"盘符":"F:","盘类型":2}]
XJS_API const char* XJS_CALL xjs_db_GetDrivesJson(xjs_engine* engine, int driveType);

// 判断文件ID是否存在断层 (沿父链向上查找, 遇到无效ID/空槽视为断层)
// 返回 TRUE=有断层, FALSE=正常
XJS_API BOOL XJS_CALL xjs_db_IsFileIdBroken(xjs_engine* engine, int fileId);

// 取数据库结构版本号 (编译期常量, 当前1.38; 数据库结构改变时递增; 引擎无效返回 0)
XJS_API double XJS_CALL xjs_db_GetDatabaseVersion(xjs_engine* engine);


// ============================================================================
// 同步 API
// ============================================================================

/** @brief 同步_添加监控路径 (返回非0代表成功; 注意: 非硬盘盘符(光盘/软盘等)也会返回成功但不实际监控; 重复添加同一盘符返回成功且不覆盖) */
// 返回0时可用 xjs_GetLastError()/xjs_GetLastErrorMsg() 获取失败原因(61=添加监控失败)
// driveLetter: 盘符字符, 如 'C'
XJS_API BOOL XJS_CALL xjs_sync_AddPath(xjs_engine* engine, char driveLetter);

/** @brief 同步_移除监控路径 (停止该盘符的USN监视, 并从属性表盘符数组删除该盘的记录, 重启后不会再恢复该盘监视) */
// 只影响监视与盘符记录, 不删除该盘已入库的数据; 删除分区数据请配合 xjs_db_RemovePath(传盘符根, 如 "D:")
// 返回0时可用 xjs_GetLastError()/xjs_GetLastErrorMsg() 获取失败原因(61=移除监控失败)
// driveLetter: 盘符字符, 如 'C'
XJS_API BOOL XJS_CALL xjs_sync_RemovePath(xjs_engine* engine, char driveLetter);

// 同步_开始监控
// 通常不需要调用。遍历完分区后，程序会自动添加同步，除非您调用过 xjs_sync_RemovePath 或 xjs_sync_Stop
// 注意: 无防重入保护, 请勿重复调用
// 返回假时可用 xjs_GetLastError()/xjs_GetLastErrorMsg() 获取失败原因(51=线程创建失败)
XJS_API BOOL XJS_CALL xjs_sync_Start(xjs_engine* engine);

// 同步_暂停同步 
// 需要注意的是，暂停同步后，所有文件的变化信息会保留在内存中。如果不打算恢复同步，不应该使用此方法，
// 而应该使用：xjs_sync_RemovePath || xjs_sync_Start || xjs_sync_Stop
// pause: 真=暂停, 假=恢复
XJS_API void XJS_CALL xjs_sync_Pause(xjs_engine* engine, BOOL pause); 

// 全部停止监视 (停止监听并清空队列; 停机期间的文件变化会丢失, 之后可 AddPath+Start 重新开启)
// waitQueueComplete: 当前版本该参数无效, 无论真假都不等待队列
XJS_API void XJS_CALL xjs_sync_AllStop(xjs_engine* engine, BOOL waitQueueComplete); 

// 获取当前同步队列中积压的待处理文件数量 (用于判断磁盘高负载是否结束; 计数可能包含处于去抖等待中的事件)
XJS_API int XJS_CALL xjs_sync_GetPendingCount(xjs_engine* engine);

// 获取引擎当前所有仍在图标队列中等待处理的任务数量
// 已被异步图标线程弹出但尚未完成的任务不计入
XJS_API int XJS_CALL xjs_icon_GetPendingCount(xjs_engine* engine);

// 判断同步监视是否正在运行 (返回 TRUE=正在监视; 注意: 添加了NTFS盘符监听但尚未 Start 时也已返回TRUE)
XJS_API BOOL XJS_CALL xjs_sync_IsRunning(xjs_engine* engine);

// 获取数据库登记的全部盘符, 返回JSON数组, 带冒号, 例如: ["C:","D:"]
XJS_API const char* XJS_CALL xjs_sync_GetMonitoredDrives(xjs_engine* engine);

// 取文件同步各阶段的性能计时统计文本 (UTF-8, \r\n分隔; 每行: 阶段名: 总耗时秒 [次数] 平均:平均毫秒)
// 统计阶段: 取文件ID/取父目录ID/取文件路径(查库), 属性查询(磁盘I/O), 等待读锁/等待写锁(锁竞争),
//           同步处理(写锁内增删改), 同步后回调(事件通知), 事件总耗时; 没发生过的阶段次数为0不显示平均
// 返回的文本位于调用线程的临时缓冲区, 下次在该线程调用返回文本的API前会失效, 需长期保存请自行复制
// 引擎无效或数据库正在遍历/加载中时返回空字符串""
XJS_API const char* XJS_CALL xjs_sync_GetPerformanceText(xjs_engine* engine);

// 清零所有同步性能统计阶段(诊断采样用): 清零后重新累计, 便于对比优化前后
// 引擎无效或数据库正在遍历/加载中时无效果
XJS_API void XJS_CALL xjs_sync_ClearPerformanceText(xjs_engine* engine);

// ============================================================================
// 目录大小同步 API (目录占用大小是否级联同步到上级目录)
// ============================================================================

/** @brief 设置"同步上级目录大小"开关
    enable: 真=开启, 假=关闭(默认)
    - 开启后: 目录大小 = 其下所有后代文件大小之和(排除目录本身); 文件同步(创建/修改/删除/移动)时, 目录大小变更自动级联到所有上级目录.
    - 关闭(默认): 目录大小只统计当前目录下的直接子文件.
    - 遍历前设置效果最佳; 遍历中途或事后开启, 会立即对现有数据重算生效.
    @return 真=设置成功(开启时已重算); 假=引擎无效/在搜索线程调用, 或引擎正忙(遍历/保存/加载中)未取得写锁——此时开关保持原值不变,
            可用 xjs_GetLastError() 确认(35=数据库正忙 / 30=参数无效), 稍后重试或重新遍历. */
XJS_API BOOL XJS_CALL xjs_db_SetParentSizeSync(xjs_engine* engine, BOOL enable);

/** @brief 获取"同步上级目录大小"开关当前状态 */
XJS_API BOOL XJS_CALL xjs_db_IsParentSizeSync(xjs_engine* engine);

// ============================================================================
// 默认查询顺序落盘 API (空搜索词展示的默认顺序数组是否随数据库文件保存)
// ============================================================================

/** @brief 设置"默认查询顺序落盘"开关
    enable: 真=落盘, 假=不落盘(默认)
    - 开启(默认): 默认查询顺序已构建时随数据库保存, 下次加载直接使用, 零重建耗时;
      代价是数据库文件变大(约4字节/文件, 450万文件约18MB).
    - 关闭: 保存时只写"未落盘"标记, 数据库文件更小; 加载时按当前默认排序字段排序重建
      (450万行毫秒级~百毫秒级, 与遍历完成时的重建同路径).
    - 任何时候设置都即时生效, 影响下一次保存; 与 xjs_db_Save/自动保存配合使用.
    @return 真=设置成功; 假=引擎无效/在搜索线程调用, 或引擎正忙(遍历/保存/加载中)未取得写锁——此时开关保持原值不变,
            可用 xjs_GetLastError() 确认(35=数据库正忙 / 30=参数无效), 稍后重试. */
XJS_API BOOL XJS_CALL xjs_db_SetDefaultQuerySave(xjs_engine* engine, BOOL enable);

/** @brief 获取"默认查询顺序落盘"开关当前状态 */
XJS_API BOOL XJS_CALL xjs_db_IsDefaultQuerySave(xjs_engine* engine);

// ============================================================================
// 排除目录 API (指定目录不入索引: 该目录及其全部子内容不在搜索结果中显示)
// ============================================================================

/** @brief 添加一个排除目录.
    path: UTF-8文本, 支持 'F' / 'F:' / 'F:\' / 'F:\abc' / 'F:\abc\' 等写法, 盘符字母自动转大写,
            统一规范为 'F:' 或 'F:\abc' 形式(不含尾部反斜杠); 仅支持盘符绝对路径, 网络路径/相对路径返回失败.
    - 添加成功后立即生效: 该目录(含整个子树)在数据库中的已有数据被立即删除, 搜索结果/默认查询顺序/目录大小级联同步联动;
      删除数据期间持有引擎写锁, 大库(数百万文件)下可能耗时数秒, 此期间搜索与查找被阻塞属预期.
    - 文件同步实时生效: 同步线程遇到位于排除目录下(含排除目录本身被重新创建)的创建/重命名事件会直接跳过入库;
      重命名进入排除区域的文件会从索引中按删除语义移出.
    - 遍历兜底: 枚举扫描(MFT平铺遍历无法逐条跳过)重新带入库的排除目录数据, 会在遍历收尾统一重删.
    - 配置持久化: 排除目录列表保存在数据库属性表(附加数据, 键"排除目录"), 随数据库保存/加载自动持久化;
      重建/清库/加载失败等会清空属性表数据的路径均有镜像写回, 配置不会因清库丢失.
    - 幂等: 已存在同目录(忽略大小写/书写差异)时直接返回真, 不会重复添加或重复删除.
    @return 真=添加成功(已生效并删除已有数据); 假=引擎无效/路径不是盘符绝对路径(30=参数无效), 或
            引擎正忙(遍历/保存/加载中)未取得写锁(35=数据库正忙)——此时排除列表未改变, 可稍后重试. */
XJS_API BOOL XJS_CALL xjs_db_AddExcludedDir(xjs_engine* engine, const char* path);

/** @brief 移除一个排除目录(忽略大小写/书写差异匹配).
    - 移除后该目录重新参与索引, 但已删除的旧数据不会立即恢复, 需在下次遍历(重建索引)时才重新入索引.
    @return 真=移除成功; 假=目录不在排除列表中(错误码保持成功), 或引擎无效/参数非法(30=参数无效)/引擎正忙(35=数据库正忙). */
XJS_API BOOL XJS_CALL xjs_db_RemoveExcludedDir(xjs_engine* engine, const char* path);

/** @brief 获取全部排除目录列表.
    @return 返回JSON数组(UTF-8), 例如: ["D:\\temp","F:"]; 引擎无效或读取失败时返回空文本.
            返回的指针由内部管理, 无需释放, 请第一时间拷贝. */
XJS_API const char* XJS_CALL xjs_db_GetExcludedDirs(xjs_engine* engine);

// ============================================================================
// 路径别名配置 API (系统别名管理器: 路径别名与 desktop.ini 别名冲突时优先, 管理器为空时 desktop.ini 正常生效)
// ============================================================================

/** @brief 整体替换路径别名配置.
    json_utf8: UTF-8 JSON对象文本, 形如 {"D:\\WoNiu\\e.exe":"易语言"}.
    - 键=完整路径, 值=别名; 键支持 <系统盘> 与 <用户名> 占位符, 路径中的 '/' 自动统一为 '\\'.
    - 整体替换语义: 先解析, 解析成功才清空重建(失败时原配置保持不变并返回假).
    - 仅运行期生效: 不写回 Config\\路径别名.json, 引擎下次创建时仍从该文件加载.
    - 生效范围: 后续的搜索匹配与文件同步即时可见; 遍历期间调用会失败(扫描线程正并发读取).
    sync_existing_db: 是否同步现有数据库.
    - 假(默认): 仅替换配置, 已入库文件不变(旧行为).
    - 真: 配置生效后立即应用到现有库: 遍历全部已入库文件, 命中新配置且当前"无别名"的行写入该别名并评分+5;
      已有别名的行一律不动(不覆盖 desktop.ini/EXE/lnk/用户设置的现存别名);
      应用后重建默认查询顺序并刷新各搜索结果的排序数组(当前结果顺序不变, 重新搜索后生效).
    - 同步执行: 持引擎写锁遍历全库, 大库可能耗时数百毫秒~数秒, 期间并发 API 会等待; 别名功能未开启时此步无操作(仍返回真).
    @return 真=已生效; 假=引擎无效/JSON为空或解析失败(30=参数无效)/引擎正忙(遍历/保存/加载中, 35=数据库正忙). */
XJS_API BOOL XJS_CALL xjs_alias_SetAliasJSON(xjs_engine* engine, const char* alias_json, BOOL sync_existing_db);

/** @brief 获取当前路径别名配置.
    @return 返回JSON对象文本(UTF-8), 形如 {"D:\\WoNiu\\e.exe":"易语言"}; 路径为已展开的绝对路径
            ({C} 等占位符不保留). 引擎无效或读取失败时返回空文本.
            返回的指针由内部管理, 无需释放, 请第一时间拷贝. */
XJS_API const char* XJS_CALL xjs_alias_GetAliasJSON(xjs_engine* engine);

// ============================================================================
// 筛选器 API
// ============================================================================

/** @brief 整体替换筛选器配置(文件类型分类与后缀表).
    filter_json: UTF-8 JSON数组文本, 形如 [{"名称":"程序","类型":99,"后缀":"EXE,BAT,MSI"}, ...].
    - 类型: 0=全部文件(系统保留), 255=目录(系统保留), 其余 1~254 自定义且不可重复.
    - 解析失败时自动回退内置默认配置(仍返回真).
    sync_existing_db: 是否同步现有数据库.
    - 假(默认): 仅替换配置, 已入库文件的类型不会重算, 需重建索引才生效(旧行为).
    - 真: 配置生效后立即重算现有库全部非目录行的文件类型(目录保持255; 口径与枚举入库一致, 类型未变化的行不写),
      随后刷新各搜索结果的排序数组(当前结果顺序不变, 重新搜索后生效).
    - 同步执行: 持引擎写锁遍历全库, 大库可能耗时数百毫秒~数秒, 期间并发 API 会等待.
    - 遍历期间调用会失败(扫描/同步线程正并发读取).
    @return 真=已生效; 假=引擎无效/JSON为空(30=参数无效)/引擎正忙(遍历/保存/加载中, 35=数据库正忙). */
XJS_API BOOL XJS_CALL xjs_filter_SetFilterJSON(xjs_engine* engine, const char* filter_json, BOOL sync_existing_db);

/** @brief 获取当前筛选器配置.
    @return 返回JSON数组文本(UTF-8), 格式同 xjs_filter_SetFilterJSON 的入参.
            返回的指针由内部管理, 无需释放, 请第一时间拷贝. */
XJS_API const char* XJS_CALL xjs_filter_GetFilterJSON(xjs_engine* engine);

// ============================================================================
// Lua 扩展 API
// ============================================================================

/** Lua C 函数指针: 与 lua.h 的 lua_CFunction(int (*)(lua_State*)) 二进制兼容;
    火山侧即 @LUA_CALL 静态方法(参数 L 类型=LUA虚拟机, 取静态方法地址 后传入). */
typedef int (XJS_CALL* xjs_lua_CFunction)(void* L);

/** @brief 注册用户自定义 Lua C 函数(进程级).
    class_name: UTF-8, 挂载表名, 不允许含'.', 传"" = method_name 直接注册为全局变量;
    同 class_name 的多个方法自动归并到同一张表, 表在首次挂载时自动创建.
    method_name: UTF-8, 函数名, 不允许含'.'且不可为空;
    重复注册同 类名.方法名 后者覆盖前者(可遮蔽内置名, 自担).
    脚本内直接 类名.方法(...) 或 方法(...) 调用.
    func: C 函数指针(签名 int (*)(void* L) 即 lua_CFunction); 脚本调用期间可能处于搜索/执行线程,
    须遵守 Lua C API 约定(入参在栈上, 返回值=返回值个数), 不得长期阻塞(受看门狗约束).
    - 生效时机: 每个虚拟机随每次 Lua 搜索/执行创建(即用即毁), 注册对之后创建的虚拟机生效, 对运行中的脚本无影响.
    - 线程安全(内部锁); 不持引擎锁, 遍历/搜索期间也可调用; 注册表进程级共享, engine 仅用于校验与错误码.
    @return 真=已登记; 假=引擎无效/method_name为空/类名或方法名含'.'/func为空(30=参数无效). */
XJS_API BOOL XJS_CALL xjs_lua_RegisterFunction(xjs_engine* engine, const char* class_name, const char* method_name, xjs_lua_CFunction func);

/** @brief 注销进程级注册的用户自定义 Lua C 函数(xjs_lua_RegisterFunction 的逆操作).
    method_name 为 NULL = 注销该 class_name 下全部方法; class_name 不允许为 NULL(传"" = 全局变量区).
    - 生效时机: 对之后创建的虚拟机生效, 对运行中的脚本无影响(VM 即用即毁).
    - 线程安全(内部锁); 从未注册过返回假(不算错误).
    @return 真=至少注销一项; 假=引擎无效/class_name为NULL(30=参数无效)/无匹配. */
XJS_API BOOL XJS_CALL xjs_lua_UnregisterFunction(xjs_engine* engine, const char* class_name, const char* method_name);

/** @brief 注册用户自定义 Lua C 函数(结果对象级): 仅对该结果对象之后的 -3(Lua脚本过滤)/-4(Lua执行) 创建的虚拟机生效.
    不同结果对象互不相通(各挂各的); 同名 类名.方法名 时对象级覆盖进程级(装配时先挂进程级后挂对象级, 更具体的注册生效);
    对象销毁时其对象级注册随之消失, 无需逐个注销.
    其余语义(类名/方法名规则、func 签名、生效时机、线程安全)与 xjs_lua_RegisterFunction 完全一致.
    @return 真=已登记; 假=结果对象无效/method_name为空/类名或方法名含'.'/func为空(30=参数无效). */
XJS_API BOOL XJS_CALL xjs_result_LuaRegisterFunction(xjs_result* result, const char* class_name, const char* method_name, xjs_lua_CFunction func);

/** @brief 注销结果对象级注册的用户自定义 Lua C 函数(xjs_result_LuaRegisterFunction 的逆操作).
    method_name 为 NULL = 注销该 class_name 下全部方法; class_name 不允许为 NULL(传"" = 全局变量区).
    - 生效时机: 对该结果对象之后创建的虚拟机生效, 对运行中的脚本无影响; 线程安全(内部锁).
    @return 真=至少注销一项; 假=结果对象无效/class_name为NULL(30=参数无效)/无匹配. */
XJS_API BOOL XJS_CALL xjs_result_LuaUnregisterFunction(xjs_result* result, const char* class_name, const char* method_name);

// ---- Lua 栈辅助: 供注册函数(xjs_lua_CFunction)内部取参数/压返回值使用 ----
// 宿主静态编译拿不到引擎内部的 Lua 时, 用这组 API 代替 lua_* 操作栈;
// 栈索引遵守 Lua 惯例: 1=第1个参数, 负数=从栈顶倒数(-1=栈顶); L=注册函数收到的第一个参数原样传入.

// 栈顶索引(当前元素个数); C 函数被调用时 = 传入的参数个数
XJS_API int XJS_CALL xjs_lua_GetTop(void* L);

// 取栈上整数/数字; 不可转时返回 0
XJS_API long long XJS_CALL xjs_lua_ToInteger(void* L, int index);

// 取栈上数字(整数也可); 不可转时返回 0
XJS_API double XJS_CALL xjs_lua_ToNumber(void* L, int index);

// 取栈上文本的 UTF-8 指针(数字会临时转文本); 不可转时返回 NULL;
// 仅本次 C 函数调用内有效(栈变动/GC 后可能失效), 需保存请自行拷贝
XJS_API const char* XJS_CALL xjs_lua_ToString(void* L, int index);

// nil/假 返回假, 其余全为真
XJS_API BOOL XJS_CALL xjs_lua_ToBoolean(void* L, int index);

// 以下为压入(返回值/数据): C 函数的返回值 = 本次压入的个数
XJS_API void XJS_CALL xjs_lua_PushInteger(void* L, long long value);
XJS_API void XJS_CALL xjs_lua_PushNumber(void* L, double value);

// 压入 UTF-8 文本; utf8 必须以 '\0' 结尾, 传 NULL 则压入 nil; Lua 侧会拷贝一份, 调用后指针可立即释放
XJS_API void XJS_CALL xjs_lua_PushString(void* L, const char* utf8);

XJS_API void XJS_CALL xjs_lua_PushBoolean(void* L, BOOL value);
XJS_API void XJS_CALL xjs_lua_PushNil(void* L);

// 解析 JSON 文本(顶层为对象或数组)并压入对应的 Lua 表(嵌套结构整体转换, 深度上限64);
// 失败返回假且不压入任何值
XJS_API BOOL XJS_CALL xjs_lua_PushJson(void* L, const char* json_utf8);

// 把栈上指定值序列化为 JSON 文本(UTF-8, '\0'结尾)写入调用方缓冲; 表按内容转换:
// rawlen>0=数组, 否则=对象(非文本键跳过); 深度上限64; 函数/用户数据等不可序列化.
// buffer=NULL 或 bufferCount 不足 → 返回所需字节数(含'\0'), 可先传0量尺寸再调一次;
// 成功 → 返回实际写入字节数(含'\0'); 序列化失败 → 0
XJS_API int XJS_CALL xjs_lua_ToJson(void* L, int index, char* buffer, int bufferCount);

// 把本次 C 函数收到的全部参数([1..栈顶])序列化为 JSON 数组文本写入调用方缓冲("参数表");
// 返回契约同 xjs_lua_ToJson
XJS_API int XJS_CALL xjs_lua_ArgsToJson(void* L, char* buffer, int bufferCount);

/** @brief 取 Lua 脚本提示词文本(UTF-8, '\0'结尾).
    引擎内嵌的 Lua 脚本编写提示词(AI 投喂用), 按类型返回其一:
    0=搜索过程提示词(《搜索过程提示词.md》: -3 过滤模式脚本——宿主多线程逐文件调用的单文件谓词, f 表 API/两种脚本形态/性能规则);
    1=全局搜索提示词(《ExecuteLua脚本生成提示词.md》: -4 执行模式脚本——脚本自主遍历数据库/排序/生成结果, db/f/res 三表 API 全集).
    宿主可将其作为系统提示词投喂给大模型生成对应模式的脚本.
    promptType: 0=搜索过程, 1=全局搜索; 其它值返回空字符串("").
    @return UTF-8 文本指针; 指向进程级静态缓冲, 进程生命期内有效, 无需释放、请勿改写. */
XJS_API const char* XJS_CALL xjs_LUA_GetPprompt(int promptType);

// ============================================================================
// 搜索结果 API
// ============================================================================

// 创建搜索结果对象(如果程序处于遍历阶段, 将会返回nullptr)
// 每次搜索前创建; 一个引擎可同时持有多个搜索结果对象, 用于不同搜索任务
// 返回 nullptr 时可用 xjs_GetLastError()/xjs_GetLastErrorMsg() 获取失败原因(35=数据库正忙/60=正在遍历磁盘/70=结果对象无效)
XJS_API xjs_result* XJS_CALL xjs_result_Create(xjs_engine* engine);

// 搜索结果_设置回调 (非线程安全)   
XJS_API BOOL XJS_CALL xjs_result_SetCallback(
    xjs_result* result,
    int eventType, /* 参见 xjs_result_event_type 枚举
                    XJS_RESULT_EVENT_BEFORE (1): 即将搜索，返回非0拦截
                        typedef int (*SearchBeforeCallback)(void* userData, xjs_engine* engine, xjs_result* result, int searchFingerprint, const char* keyword);

                    XJS_RESULT_EVENT_PROCESS (2): 搜索过程函数，返回值: 0=继续搜索, -1=停止搜索(抛弃所有), 1=停止搜索(保留已搜索到的结果)
                        typedef int (*SearchProcessCallback)(void* userData, xjs_engine* engine, xjs_result* result, const char* keyword);

                    XJS_RESULT_EVENT_WAIT (3): 搜索等待，返回值: 0=继续等待, -1=停止等待(会继续搜索)
                        typedef int (*SearchWaitCallback)(void* userData, xjs_engine* engine, xjs_result* result, int elapsedMs);

                    XJS_RESULT_EVENT_COMPLETE (4): 搜索完成，异步线程触发
                        typedef int (*SearchCompleteCallback)(void* userData, xjs_engine* engine, xjs_result* result, int searchFingerprint, const char* keyword, BOOL discarded);

                    XJS_RESULT_EVENT_CHANGE (10): 搜索结果变化，异步线程触发
                        typedef int (*SearchChangeCallback)(void* userData, xjs_engine* engine, xjs_result* result, BOOL resetCount);

                    XJS_RESULT_EVENT_DRAW_ICON (11): 图标绘制事件，异步图标线程触发(非主线程)
                        typedef void (*SearchDrawIconCallback)(void* userData, xjs_engine* engine, xjs_result* result, int searchFingerprint, int id, int itemIndex, const void* iconData, int iconLength, const char* callbackInfo);
                        // iconData 指向内部管理的PNG数据, 回调返回后可能被释放, 请立即深拷贝后再跨线程使用
                        // callbackInfo: 调用 xjs_result_GetFileIco 时传入的异步回调信息(UTF-8)原样回传; 未提供时为空字符串, 指针仅在回调期间有效

                    XJS_RESULT_EVENT_FAILED (12): 搜索失败(正则创建失败/SQL解析或编译失败)，搜索线程触发
                        typedef int (*SearchFailedCallback)(void* userData, xjs_engine* engine, xjs_result* result, int searchFingerprint, const char* errorJson);
                        // errorJson: 错误信息JSON文本(UTF-8编码), 如 {"错误类型":"正则错误","错误信息":"正则表达式编译失败...","出错位置":5}
                        //            或 {"错误类型":"SQL错误","错误信息":"PG SQL解析失败..."} (SQL解析/编译失败时触发, 此时[搜索完成]事件不会触发)
                        //            SQL错误时额外携带: "出错位置":n, "出错长度":n —— 均为原始SQL文本(UTF-8)中的字节偏移/跨度,
                        //            用于在输入框/编辑器中高亮出错部分(位置=-1表示无法定位到具体节点, 仅以错误信息文字为准)
                        //            指针为回调期间有效, 请立即拷贝(或解析)后再使用

                    XJS_RESULT_EVENT_ICON_ASK (13): 询问继续获取图标，异步图标线程在真实获取图标之前触发(搜索结果读锁内, 回调内禁止写库/等待锁)
                        typedef INT (*IconAskCallback)(void* userData, xjs_engine* engine, xjs_result* result, int searchFingerprint, int fileId, int itemIndex, int iconSize, const char* callbackInfo);
                        // callbackInfo: 同[图标绘制事件], 调用 xjs_result_GetFileIco 时传入的异步回调信息(UTF-8)原样回传; 未提供时为空字符串
                        // 返回: 非0=继续获取图标, 0=放弃当前图标(不获取也不触发[图标绘制事件])
                        // 未设置此回调时默认继续获取
            */
    const void* callback, 
    const void* userData
);

// 销毁搜索结果对象 (一律由内部异步删除线程执行删除; 返回 FALSE 的情况: 正在遍历枚举中(错误码=60)/对象指针为空(错误码不会被写入))
XJS_API BOOL XJS_CALL xjs_result_Destroy(xjs_result* result);

// 搜索结果对象是否有效 (已进入销毁队列或已从引擎移除时, 返回 FALSE)
XJS_API BOOL XJS_CALL xjs_result_IsEffective(xjs_result* result);


// 搜索 (返回本次的搜索指纹; 指纹从1开始逐次递增, 用于停止/判断完成/判断是否被覆盖)
// 如果当前已经在搜索中, 会通知旧任务停止(旧任务的"搜索完成"回调仍会触发, 但 discarded=TRUE)
// 返回 -1 的情况(失败原因见 xjs_GetLastError()/xjs_GetLastErrorMsg()):
//   30=参数无效(keyword 为 NULL)   35=数据库正忙(正在加载/保存)   60=正在遍历磁盘   71=当前是主线程且 waitComplete=TRUE
// 注意: result 为 NULL 时引擎句柄不可得, 错误码不会被写入
XJS_API int XJS_CALL xjs_result_Query(
    xjs_result* result, 
    const char* keyword, // 搜索词(UTF-8, 不可为NULL)：keywordType=0/1/2 时 100% 按指定类型执行(类型必须强制指定)
                         //   通配符示例: "*.txt" 或 "文档"
                         //   正则示例:   "^[0-9]+$"
                         //   SQL示例:    "SELECT Path FROM alltable WHERE IsDir=0"
    int keywordType,     // 搜索词类型(必须强制指定)：参见 xjs_keyword_type 枚举 (-2=多重搜索(JSON链式搜索), -3=Lua脚本(过滤模式), -4=Lua执行(脚本即程序), 0=通配符(强制), 1=正则(强制), 2=SQL语句(强制))
                         // keywordType=-2 时 searchWord 为 JSON 阶段数组: [{"搜索模式":"通配符","搜索词":"你好"},{"搜索模式":"SQL","搜索词":"SELECT ..."},{"搜索模式":"正则","搜索词":"[0-9]+"}]
                         // 多重搜索先搜索A再以A的结果为基础继续搜索B, 依此类推, 最终结果=最后阶段的输出; 不允许嵌套多重搜索; 失败时触发 SearchFailed 事件(错误JSON含 错误类型="多重搜索错误"/错误信息/阶段)
                         // keywordType=-4 时 searchWord 为 Lua 脚本(UTF-8): 脚本自主遍历数据库(db表)/读取当前结果(res表)/自主排序, return 的 ID 数组(及其顺序)就是最终搜索结果;
                         // 搜索线程单线程驱动, 全程引擎读锁, 看门狗总预算10秒; 结果顺序=脚本自定义序(乱序), 删除同步走乱序位图, 新建/修改/重命名不同步(重新执行刷新);
                         // 失败触发 SearchFailed 事件(错误JSON 错误类型="Lua执行错误"); 脚本环境见 Lua脚本示例 目录
    BOOL waitComplete    // 真=阻塞等待搜索完成; 主线程禁止使用(会被拦截返回-1, 错误码=71)
);

// 取最新搜索指纹(每次 Query 递增; 若与某次 Query 的返回值不一致, 说明那次搜索已被覆盖)
XJS_API int XJS_CALL xjs_result_GetFingerprint(xjs_result* result);


// 取当前搜索词 (返回的指针为线程本地缓存, 同线程内再调用任何返回文本的API都会被覆盖, 请立即拷贝)
XJS_API const char* XJS_CALL xjs_result_GetKeyword(xjs_result* result);


// 停止搜索 (返回 FALSE 的情况: 结果对象无效、未在搜索、指纹不匹配或搜索已结束)
// 返回 FALSE 时可用 xjs_GetLastError()/xjs_GetLastErrorMsg() 获取失败原因(72=停止搜索失败)
// 注意: result 为 NULL 时引擎句柄不可得, 错误码不会被写入
// searchFingerprint: xjs_result_Query() 的返回值
// waitStop: 真=等待搜索线程完全停止后再返回(搜索线程内调用不会自等); 取消后已产生的结果保留
XJS_API BOOL XJS_CALL xjs_result_CancelSearch(xjs_result* result, int searchFingerprint, BOOL waitStop);

// 获取指定搜索轮次中仍在图标队列里等待处理的任务数量
// searchFingerprint: xjs_result_Query() 返回的搜索指纹; 已被异步线程弹出的任务不计入
XJS_API int XJS_CALL xjs_result_GetPendingIconCount(xjs_result* result, int searchFingerprint);

// 判断指定文件的图标任务是否仍在当前搜索轮次的队列中等待处理
// 返回 TRUE 只代表任务尚未被弹出; FALSE 也可能表示未入队、已被弹出或搜索结果无效, 不代表图标获取成功
XJS_API BOOL XJS_CALL xjs_result_IsIconPending(xjs_result* result, int searchFingerprint, int fileId);

// 获取文件图标 (PNG格式, 内部提供图标缓存, 返回的数据指针由内部管理, 无需释放)
// itemIndex>=0(异步模式)时, 首次取到的是临时默认图标, 真实图标会通过"图标绘制事件"(11)异步回调; itemIndex==-1(同步模式)时直接返回真实图标
// iconSize 无限制(未缓存时按尺寸动态获取); 返回的数据指针为线程本地缓存, 同线程内再次调用任何返回文本/数据的API都会被覆盖, 请立即拷贝
// callbackInfo: 异步回调信息(UTF-8, 可为NULL); 任务进入异步图标队列时内部深拷贝, 触发"询问继续获取图标"(13)/"图标绘制事件"(11)回调时原样回传
//               (同一文件在同一搜索轮次内重复入队会被去重, 以首次入队时提供的 callbackInfo 为准)
// SDK 内部已经在取图标时做了 CoInitializeEx 处理
XJS_API const void* XJS_CALL xjs_result_GetFileIco(
    xjs_result* result,
    int fileId,
    int itemIndex,      // 从0开始。如果提供 -1 代表同步获取，不会触发回调事件
    int iconSize,       // 16 或 32
    int* outIconLength,
    const char* callbackInfo  // 异步回调信息(UTF-8), 可为NULL; 同步模式(itemIndex==-1)下不使用
);


// 判断搜索是否完成 (指纹已被新的搜索覆盖、或该指纹从未开始搜索时, 也返回 TRUE)
XJS_API BOOL XJS_CALL xjs_result_IsCompleted(
    xjs_result* result, 
    int searchFingerprint // 搜索指纹为 xjs_result_Query() 的返回值
);

// 取迅捷搜引擎 (也就是父对象的句柄)
XJS_API xjs_engine* XJS_CALL xjs_result_GetXjsEngine(xjs_result* result);

// 取搜索耗时, 单位毫秒; 正在搜索时返回 -1
XJS_API int XJS_CALL xjs_result_GetElapsed(xjs_result* result);

// 取匹配关键词 (比如搜 *.txt，需要高亮 .txt) 返回 JSON: [".txt"]
//  文件名:"xunjieso1.0.dll"
//   正则:"[0-9]+" 返回:["1", "0"]
//   通配符:"*.txt" 返回:[".txt"]
//   SQL:"SELECT * WHERE Fname LIKE '%1.0%'" 返回:["1.0"] (LIKE/ILIKE按%_拆分取常量片段; ~正则按正则提取; =/IN等取整串)
// textToHighlight: 待高亮的原始文本(UTF-8)
XJS_API const char* XJS_CALL xjs_result_GetMatchKeywords(xjs_result* result, const char* textToHighlight);

// 取匹配关键词EX: 按字段分类返回高亮关键词, 返回 JSON 对象, 例如:
// {"文件名":[".txt"],"文件夹":["Windows"],"别名":[]}
//   "文件名": 文件名列需要高亮的关键词
//   "文件夹": 父目录路径需要高亮的关键词 (仅当搜索词含'\'、即搜索匹配路径时才非空)
//   "别名":   别名列需要高亮的关键词 (仅当开启了"别名"字段时才非空)
// 调用方只需在对应列对对应分组做高亮, 避免全部文本一起高亮
XJS_API const char* XJS_CALL xjs_result_GetMatchKeywordsEx(xjs_result* result, int fileId);

// 取搜索设置: 返回 JSON 对象文本, 字段名如下:
// {"支持首拼":true,"支持全拼":true,"拼音完整匹配":false,"支持星号":true,"支持问号":true,
//  "匹配全角":true,"区分大小写":false,"且":" ","或":"|","删搜索词首尾空":true,"搜索词为空时显示所有文件":true}
XJS_API const char* XJS_CALL xjs_result_GetSearchSettings(xjs_result* result);

// 置搜索设置: 传入 JSON 对象文本, 字段名与 xjs_result_GetSearchSettings 一致
// 可只包含需要修改的字段, 未包含的字段保持原值
// 返回 TRUE=设置成功, FALSE=参数无效或JSON解析失败或加锁失败(可用 xjs_GetLastError()/xjs_GetLastErrorMsg() 获取原因, 30=参数无效/35=数据库正忙)
// json: UTF-8 JSON对象文本
XJS_API BOOL XJS_CALL xjs_result_SetSearchSettings(xjs_result* result, const char* json);

// 取结果数量 (线程安全, 搜索中可调用; 搜索过程中返回0, 搜索完成后才返回最终数量)
// 配合 xjs_result_GetFileId() 按索引遍历全部结果
XJS_API int XJS_CALL xjs_result_GetCount(xjs_result* result);

// 按索引取文件ID (索引从0开始, 与列表显示顺序一致; 越界返回 -1)
// index: 结果索引, 从0开始; 配合 xjs_result_GetCount() 遍历结果
XJS_API int XJS_CALL xjs_result_GetFileId(xjs_result* result, int index);


// 根据文件ID取索引 (按当前排序字段二分查找, 找不到再线性查找; 未找到返回 -1)
// 用于文件数量变化时, 刷新UI, 保证显示用户当前显示/选中的表项
// FileId: 文件ID
XJS_API int XJS_CALL xjs_result_GetFileIdIndex (xjs_result* result, int FileId);


// 复制所有文件ID (返回已复制的数量, 即 min(bufferCount, 实际数量); bufferCount 不足时不会报错)
XJS_API int XJS_CALL xjs_result_CopyAllFileId(
    xjs_result* result, 
    int* idArray,       // 事先分配好的整数缓冲区
    int bufferCount     // 缓冲区成员数
);

// 按范围复制文件ID (完美适配 UI 虚拟列表/分页控件)
// 返回值：实际拷贝的数量; startIndex 越界或 bufferCount=0 时返回 0
XJS_API int XJS_CALL xjs_result_CopyFileIdsByRange(
    xjs_result* result, 
    int startIndex,     // 从第几个开始取 (比如下拉框滚到第 500 条，就传 500)
    int* buffer,        // 接收数据的缓冲区 (只需要分配 50 * 4 = 200 字节即可)
    int bufferCount     // 要读取多少个，就写多少个
);

// 搜索结果排序: 立即重排内部排序数组, 但当前结果不变, 也不触发任何事件, 需重新搜索后才按新顺序返回结果
// 并发约定: 搜索进行中调用会阻塞到该次搜索结束(内部取引擎写锁, 防止排序数组重建与并行扫描撕裂);
//           搜索线程回调内调用直接返回(错误码=数据库正忙), 遍历/加载期间调用直接返回(错误码60/35)
XJS_API void XJS_CALL xjs_result_SetSortField(
    xjs_result* result, 
    const char* fieldName, // 字段名: 文件评分|文件名|文件夹|创建时间|修改时间|访问时间|文件大小|文件属性|文件类型|别名
                          // 无效字段名先按"文件评分"处理; 文件评分字段未开启时降级为"文件名"; 别名功能未开启时"别名"降级为"文件名"
                          // "别名": 有别名的文件按别名排, 无别名回退文件名
    BOOL ascending        // 真=从小到大(升序), 假=从大到小(降序)
);


// 取排序字段 (默认为"文件评分"; 默认方向为从大到小/降序)
XJS_API const char* XJS_CALL xjs_result_GetSortField(xjs_result* result);

// 取排序方向: TRUE=从小到大(升序), FALSE=从大到小(降序); 默认 FALSE(降序)
XJS_API BOOL XJS_CALL xjs_result_GetSortway (xjs_result* result);

// 取全部排序字段, 返回JSON: ["文件评分","文件名","文件夹","创建时间","修改时间","访问时间","文件大小","文件属性","文件类型","别名"(仅别名功能开启时)]
XJS_API const char* XJS_CALL xjs_result_GetAllSortFieldArray(xjs_result* result);


// 置当前筛选分类 (需要注意的是，不会触发任何事件，也不会改变搜索结果。如果需要结果产生变化，需要重新进行搜索)
XJS_API BOOL XJS_CALL xjs_result_SetSelectedFilter(
    xjs_result* result, 
    const char* categoryName // 分类名："全部" || "文件夹"...
);

// 取当前筛选分类 (默认为'全部')
XJS_API const char* XJS_CALL xjs_result_GetSelectedFilter(xjs_result* result);



/* 取全部筛选分类 返回JSON:
[
    {
        "名称":"压缩包", 
        "类型":95, 
        "后缀":"RAR,ZIP,ZIPX,7Z,ISO,IMG,ISZ,CAB,ARJ,ACE,ALZ,UUE,TAR,GZ,GZIP,TGZ,TPZ,BZIP2,BZ2,BZ,TBZ,XPI,WIM,SWM,XAR,DEB,DMG,HFS,CPIO,LZMA,LZMA86,SPLIT,001"
    }
]
*/
XJS_API const char* XJS_CALL xjs_result_GetAllFilter(xjs_result* result);


// 根据路径调整评分, 需要注意的是, 当前搜索对象, 排序方式必须是:'文件评分', 且搜索词不为空, 如果是SQL语句会忽略当前排序.
// 负数是降低排名.需要注意的是, 添加后需要重新搜索才会生效.
// 可以是目录, 文件.注意, 区分大小写哦.也不支持通配符.目录内的所有子目录, 子文件都会被影响.
// 返回 -1 表示参数无效(结果对象为空/路径为空/增减评分为0), 可用 xjs_GetLastError()/xjs_GetLastErrorMsg() 获取原因(30=参数无效); 注意: 结果对象为 NULL 时错误码不会被写入
// Path: 目录或文件路径(区分大小写, 不支持通配符)
// delta_score: 增减分数, 负数为降低排名
XJS_API int XJS_CALL xjs_result_AddBoostPath(xjs_result* result, const char* Path, short delta_score);



// 根据扩展名调整评分, 需要注意的是, 当前搜索对象, 排序方式必须是:'文件评分', 且搜索词不为空, 如果是SQL语句会忽略当前排序.
// 负数是降低排名.需要注意的是, 添加后需要重新搜索才会生效.
// 扩展名, 不包含"."哦
// 返回 -1 表示参数无效(结果对象为空/扩展名为空/增减评分为0), 可用 xjs_GetLastError()/xjs_GetLastErrorMsg() 获取原因(30=参数无效); 注意: 结果对象为 NULL 时错误码不会被写入
// Ext: 扩展名(不含"."号)
// delta_score: 增减分数, 负数为降低排名
XJS_API int XJS_CALL xjs_result_AddBoostExt(xjs_result* result, const char* Ext, short delta_score);



// 移除搜索结果 从搜索结果中，移除指定ID, 返回实际移除数量.
// idArray: 文件ID数组
// count: 数组长度
XJS_API int XJS_CALL xjs_result_RemoveFileId(xjs_result* result, const int* idArray, int count);

// 重置搜索结果ID, 不可在搜索过程中调用.通常用于`搜索完成事件`
// 通常它用于对搜索结果, 自定义排序后, 设置进来.
// idArray: 新的文件ID数组
// count: 数组长度
XJS_API int XJS_CALL xjs_result_ResetFileId(xjs_result* result, const int* idArray, int count);

// 清空搜索结果 (清空结果数组/SQL输出/查询顺序, 不触发任何事件)
// releaseMemory: 真=同时释放内部大页内存(下次搜索时重新分配), 假=仅清空计数(容量保留, 下次搜索更快)
// 正在搜索/正在遍历时返回 FALSE (此时内部搜索线程持有锁, 获取写锁超时失败),
//   可用 xjs_GetLastError()/xjs_GetLastErrorMsg() 获取失败原因(35=数据库正忙)
XJS_API BOOL XJS_CALL xjs_result_Clear(xjs_result* result, BOOL releaseMemory);

// 设置搜索结果是否同步文件变化 (文件系统新建/删除/修改/重命名时, 是否自动更新本搜索结果并触发"搜索结果变化"事件)
// sync: 真=同步(默认), 假=不同步(本搜索结果保持静态, 文件同步不再影响它)
// 典型配合: 清空结果后置假, 可防止文件同步把新文件重新加入本搜索结果
// 返回 FALSE 仅表示结果对象无效
XJS_API BOOL XJS_CALL xjs_result_SetSyncFileChange(xjs_result* result, BOOL sync);

// 取搜索结果是否同步文件变化 (默认返回 TRUE)
XJS_API BOOL XJS_CALL xjs_result_IsSyncFileChange(xjs_result* result);


// ============================================================
// 选中状态接口
// 选中状态按"文件ID"存入数组, 与列表索引无关,
//   因此新建文件/重新排序/更换排序都不会影响已选中项(内存只随选中数量增长).
// 磁盘文件被删除后(文件同步/移除搜索结果/清空/重置), 对应选中位会自动清除.
// ============================================================

// 将指定文件ID加入选中集合
// FileId: 文件ID
// 返回 TRUE=新增成功, FALSE=已存在/ID无效/结果对象为空/加锁失败(加锁失败时错误码=35=数据库正忙)
XJS_API BOOL XJS_CALL xjs_result_SelectAdd(xjs_result* result, int FileId);

// 将指定文件ID从选中集合移除
// FileId: 文件ID
// 返回 TRUE=删除成功, FALSE=原本未选中/ID无效/结果对象为空/加锁失败(加锁失败时错误码=35=数据库正忙)
XJS_API BOOL XJS_CALL xjs_result_SelectRemove(xjs_result* result, int FileId);

// 清空全部选中状态 (不触发任何事件)
XJS_API void XJS_CALL xjs_result_SelectClear(xjs_result* result);

// 判断指定文件ID是否被选中
// FileId: 文件ID
// 返回 TRUE=已选中, FALSE=未选中/ID无效/结果对象为空
XJS_API BOOL XJS_CALL xjs_result_IsSelected(xjs_result* result, int FileId);

// 按列表索引判断该表项是否被选中 (绘制界面时用)
// index: 列表索引, 从0开始
// 返回 TRUE=已选中, FALSE=未选中/索引越界/结果对象为空
XJS_API BOOL XJS_CALL xjs_result_IsSelectedByIndex(xjs_result* result, int index);

// 取当前选中的文件数量
// 返回 选中数量; 结果对象为空返回 0
XJS_API int XJS_CALL xjs_result_GetSelectedCount(xjs_result* result);

// 复制所有选中文件ID到缓冲区 (返回已复制数量, 即 min(bufferCount, 选中数量); bufferCount 不足时不会报错)
XJS_API int XJS_CALL xjs_result_CopySelectedFileId(
    xjs_result* result,
    int* idArray,       // 事先分配好的整数缓冲区
    int bufferCount     // 缓冲区成员数
);

// 全选当前搜索结果的全部文件
// 返回 TRUE=成功, FALSE=结果对象为空/加锁失败(错误码=35=数据库正忙)
XJS_API BOOL XJS_CALL xjs_result_SelectAll(xjs_result* result);


// 获取用户设定的值，运行时的值，不会保存到数据库中
XJS_API void* XJS_CALL xjs_result_GetUserValue(xjs_result* result);

// 设置用户设定的值，运行时的值，不会保存到数据库中
// userData: 用户自定义值
XJS_API void XJS_CALL xjs_result_SetUserValue(xjs_result* result, void* userData);

// ============================================================
// SQL 输出列表接口
// 以下接口用于读取 SQL 查询的输出字段和值（SELECT 列名 / GROUP BY 聚合 等场景）
// 字段索引从 0 开始，行索引从 0 开始
// 内部已通过读写锁保护，线程安全
// 典型取值流程: 先用 xjs_result_IsSQLAggregate() 判断是否聚合查询,
//   非聚合查询(普通SELECT): 取字段数量 -> 逐字段取字段名/字段类型 -> 按行取文本值/整数值/长整数值
//   聚合查询(GROUP BY):     使用下方"SQL 聚合分组接口"取分组数据
// ============================================================

// 获取 SQL 查询输出字段的列数
// 返回值：字段列数；0 表示无 SQL 输出（普通文件名搜索）
// 典型取值: 取字段数量 -> 逐字段取字段名/字段类型 -> 按行索引取文本值/整数值/长整数值
XJS_API int XJS_CALL xjs_result_GetSQLFieldCount(xjs_result* result);

// 获取 SQL 查询输出的数据行数（即第一个字段数组的长度）
// 返回值：数据行数；0 表示无数据
XJS_API int XJS_CALL xjs_result_GetSQLRowCount(xjs_result* result);

// 按字段索引获取 SQL 输出字段的名称
// 参数 fieldIndex：字段列索引，从 0 开始
// 返回值：UTF-8 文本指针，无需释放；空字符串表示索引越界
XJS_API const char* XJS_CALL xjs_result_GetSQLFieldName(
    xjs_result* result,
    int fieldIndex          // 字段列索引，从 0 开始
);

// 按字段索引获取 SQL 输出字段的数据类型
// 参数 fieldIndex：字段列索引，从 0 开始
// 返回值：字段类型枚举值  0=未知  1=文本(text)  2=整数(int32)  3=长整数(int64)
XJS_API int XJS_CALL xjs_result_GetSQLFieldType(
    xjs_result* result,
    int fieldIndex          // 字段列索引，从 0 开始
);

// 按字段索引和行索引获取 SQL 输出的文本值
// 仅当字段类型为文本(1)时有效，否则返回空字符串
// 参数 fieldIndex：字段列索引，从 0 开始
// 参数 rowIndex：数据行索引，从 0 开始
// 返回值：UTF-8 文本指针，无需释放；空字符串表示索引越界或类型不匹配
XJS_API const char* XJS_CALL xjs_result_GetSQLTextValue(
    xjs_result* result,
    int fieldIndex,         // 字段列索引，从 0 开始
    int rowIndex            // 数据行索引，从 0 开始
);

// 按字段索引和行索引获取 SQL 输出的长整数值
// 仅当字段类型为长整数(3)时有效，否则返回 0
// 参数 fieldIndex：字段列索引，从 0 开始
// 参数 rowIndex：数据行索引，从 0 开始
// 返回值：长整数值；0 表示索引越界或类型不匹配
XJS_API int64_t XJS_CALL xjs_result_GetSQLInt64Value(
    xjs_result* result,
    int fieldIndex,         // 字段列索引，从 0 开始
    int rowIndex            // 数据行索引，从 0 开始
);

// 按字段索引和行索引获取 SQL 输出的整数值
// 仅当字段类型为整数(2)时有效，否则返回 0
// 参数 fieldIndex：字段列索引，从 0 开始
// 参数 rowIndex：数据行索引，从 0 开始
// 返回值：整数值；0 表示索引越界或类型不匹配
XJS_API int XJS_CALL xjs_result_GetSQLIntValue(
    xjs_result* result,
    int fieldIndex,         // 字段列索引，从 0 开始
    int rowIndex            // 数据行索引，从 0 开始
);

// ============================================================
// SQL 聚合分组接口
// 以下接口用于读取 GROUP BY 聚合分组的结果
// 分组索引从 0 开始；内部已通过读写锁保护，线程安全
// 使用步骤: 先 xjs_result_GetSQLGroupCount() 取分组数量,
//   再按分组索引取键/计数/求和/最值, 最后用"组内成员"系列展开每个分组的文件ID
// ============================================================

// 获取 GROUP BY 聚合分组数量
// 返回值：分组数量；0 表示无分组数据
XJS_API int XJS_CALL xjs_result_GetSQLGroupCount(xjs_result* result);

// 按分组索引获取整数分组键
// 文本键时返回该组首个文件ID（可用于 UI 显示）
// 参数 groupIndex：分组索引，从 0 开始
// 返回值：整数分组键；0 表示索引越界
XJS_API int64_t XJS_CALL xjs_result_GetSQLGroupKeyInt64(
    xjs_result* result,
    int groupIndex          // 分组索引，从 0 开始
);

// 按分组索引获取文本分组键
// 整数键时返回空字符串
// 参数 groupIndex：分组索引，从 0 开始
// 返回值：UTF-8 文本指针，无需释放；空字符串表示整数键或索引越界
XJS_API const char* XJS_CALL xjs_result_GetSQLGroupKeyText(
    xjs_result* result,
    int groupIndex          // 分组索引，从 0 开始
);

// 按分组索引获取 COUNT(*) 计数值
// 参数 groupIndex：分组索引，从 0 开始
// 返回值：COUNT 计数值；0 表示索引越界
XJS_API int64_t XJS_CALL xjs_result_GetSQLGroupCountValue(
    xjs_result* result,
    int groupIndex          // 分组索引，从 0 开始
);

// 按分组索引获取 SUM 求和值
// 参数 groupIndex：分组索引，从 0 开始
// 返回值：SUM 求和值；0 表示索引越界
XJS_API int64_t XJS_CALL xjs_result_GetSQLGroupSum(
    xjs_result* result,
    int groupIndex          // 分组索引，从 0 开始
);

// 按分组索引获取 MIN 最小值
// 参数 groupIndex：分组索引，从 0 开始
// 返回值：MIN 最小值；0 表示索引越界
XJS_API int64_t XJS_CALL xjs_result_GetSQLGroupMin(
    xjs_result* result,
    int groupIndex          // 分组索引，从 0 开始
);

// 按分组索引获取 MAX 最大值
// 参数 groupIndex：分组索引，从 0 开始
// 返回值：MAX 最大值；0 表示索引越界
XJS_API int64_t XJS_CALL xjs_result_GetSQLGroupMax(
    xjs_result* result,
    int groupIndex          // 分组索引，从 0 开始
);

// ============================================================
// SQL 聚合分组扩展字段接口
// 访问多聚合值数组、去重计数、组内成员ID
// ============================================================

// 按分组索引获取多聚合结果数组的长度
// 当 SQL 包含多个聚合函数（如 SUM + AVG + MIN）时，结果按顺序存储在数组中
// 参数 groupIndex：分组索引，从 0 开始
// 返回值：聚合值数量；0 表示无聚合值数组
XJS_API int XJS_CALL xjs_result_GetSQLGroupAggCount(
    xjs_result* result,
    int groupIndex          // 分组索引，从 0 开始
);

// 按分组索引+聚合索引获取多聚合结果值
// 聚合值按 SQL 中聚合函数的出现顺序存储（如 SUM=0, AVG=1, MIN=2）
// 参数 groupIndex：分组索引，从 0 开始
// 参数 aggIndex：聚合值索引，从 0 开始
// 返回值：聚合结果值；0 表示索引越界
XJS_API int64_t XJS_CALL xjs_result_GetSQLGroupAggValue(
    xjs_result* result,
    int groupIndex,         // 分组索引，从 0 开始
    int aggIndex            // 聚合值索引，从 0 开始
);

// 按分组索引获取 COUNT(DISTINCT 文本列) 的去重计数
// 参数 groupIndex：分组索引，从 0 开始
// 返回值：去重文本数量；0 表示无去重集合
XJS_API int64_t XJS_CALL xjs_result_GetSQLGroupDistinctTextCount(
    xjs_result* result,
    int groupIndex          // 分组索引，从 0 开始
);

// 按分组索引获取 COUNT(DISTINCT 数值列) 的去重计数
// 参数 groupIndex：分组索引，从 0 开始
// 返回值：去重整数数量；0 表示无去重集合
XJS_API int64_t XJS_CALL xjs_result_GetSQLGroupDistinctIntCount(
    xjs_result* result,
    int groupIndex          // 分组索引，从 0 开始
);

// 按分组索引获取组内文件ID数量
// 用于展开 GROUP BY 结果，获取每个分组内的所有文件ID
// 参数 groupIndex：分组索引，从 0 开始
// 返回值：成员数量；0 表示无成员ID数组
XJS_API int XJS_CALL xjs_result_GetSQLGroupMemberCount(
    xjs_result* result,
    int groupIndex          // 分组索引，从 0 开始
);

// 按分组索引+成员索引获取组内文件ID
// 参数 groupIndex：分组索引，从 0 开始
// 参数 memberIndex：组内成员索引，从 0 开始
// 返回值：文件ID；-1 表示索引越界
XJS_API int XJS_CALL xjs_result_GetSQLGroupMemberId(
    xjs_result* result,
    int groupIndex,         // 分组索引，从 0 开始
    int memberIndex         // 组内成员索引，从 0 开始
);

// ============================================================
// SQL 排序字段接口
// 用于读取 SQL ORDER BY 排序字段信息
// 排序索引从 0 开始
// 内部已通过读写锁保护，线程安全
// ============================================================

// 获取 SQL ORDER BY 排序列数量
// 返回值：排序列数量；0 表示无排序（普通文件名搜索或无 ORDER BY）
XJS_API int XJS_CALL xjs_result_GetSQLSortColumnCount(xjs_result* result);

// 按排序索引获取排序列的英文名
// 参数 sortIndex：排序列索引，从 0 开始
// 返回值：UTF-8 文本指针，无需释放；如 "Size"、"ModTime"、"Path" 等；空字符串表示索引越界
XJS_API const char* XJS_CALL xjs_result_GetSQLSortColumnName(
    xjs_result* result,
    int sortIndex           // 排序列索引，从 0 开始
);

// 按排序索引获取排序方向
// 参数 sortIndex：排序列索引，从 0 开始
// 返回值：1 = DESC（降序），0 = ASC（升序）
XJS_API int XJS_CALL xjs_result_GetSQLSortDescending(
    xjs_result* result,
    int sortIndex           // 排序列索引，从 0 开始
);

// ============================================================
// 搜索类型与错误信息接口
// ============================================================

// 搜索类型枚举值，由 xjs_result_GetSearchType 返回
#define XJS_SEARCH_TYPE_LUA_EXEC    -4  // Lua执行模式(脚本即程序, 经 xjs_result_Query 以本类型提交, 脚本自定义序)
#define XJS_SEARCH_TYPE_LUA         -3  // Lua脚本(过滤模式, 宿主驱动谓词过滤)
#define XJS_SEARCH_TYPE_MULTI       -2  // 多重搜索(JSON链式搜索, 见 xjs_keyword_type::XJS_KEYWORD_MULTI)
#define XJS_SEARCH_TYPE_WILDCARD    0   // 通配符搜索
#define XJS_SEARCH_TYPE_REGEX       1   // 正则搜索
#define XJS_SEARCH_TYPE_SQL         2   // SQL语句查询

// 错误码枚举值，由 xjs_GetLastError 返回
#define XJS_ERROR_OK                  0   // 成功
#define XJS_ERROR_UNKNOWN_CALLBACK   10   // 未知的回调常量
#define XJS_ERROR_SQL_COMPILE        20   // SQL编译失败(可用 xjs_result_GetSQLErrorMessage 获取详情; 用 xjs_result_GetSQLErrorPosition/GetSQLErrorLength 定位出错部分)
#define XJS_ERROR_INVALID_ARG        30   // 参数无效(参数为空或非法, 如文件路径为空)
#define XJS_ERROR_DB_FILE_NOT_FOUND  31   // 数据库文件不存在
#define XJS_ERROR_DB_LOAD_FAILED     32   // 数据库加载失败(文件损坏或加载失败)
#define XJS_ERROR_DB_SAVE_FAILED     33   // 数据库保存失败
#define XJS_ERROR_ENUM_FAILED        34   // 遍历失败(没有可用盘符, 遍历未开始即失败)
#define XJS_ERROR_DB_BUSY            35   // 数据库正忙(正在加载/保存, 暂不允许该操作; 遍历/加载期间对需要加锁的接口统一返回 60/35)
#define XJS_ERROR_DB_REBUILDING      50   // 正在建立数据库, 不能重复建立
#define XJS_ERROR_THREAD_CREATE      51   // 线程创建失败
#define XJS_ERROR_NOT_SCANNING       52   // 并没有在遍历磁盘
#define XJS_ERROR_ADMIN_REQUIRED     53   // 此操作需要管理员权限
#define XJS_ERROR_SCANNING           60   // 正在遍历磁盘(遍历/加载期间, 需要加引擎锁的接口立即返回默认值/失败并置此错误码, 不阻塞)
#define XJS_ERROR_SYNC_ADD_FAILED    61   // 同步_添加监控失败(添加/移除同步监控盘符失败)
#define XJS_ERROR_RESULT_INVALID     70   // 结果对象无效(搜索结果对象为空或已失效)
#define XJS_ERROR_MAIN_THREAD_WAIT   71   // 主线程禁止搜索等待(主线程调用搜索时不能等待搜索完成)
#define XJS_ERROR_STOP_SEARCH_FAILED 72   // 停止搜索失败(未在搜索/指纹不匹配/搜索已结束)
#define XJS_ERROR_EXIT_REQUIRED     500   // 程序需要退出

// 获取当前搜索结果的查询类型（搜索完成后才有效）
// 返回值：XJS_SEARCH_TYPE_LUA_EXEC(-4) / XJS_SEARCH_TYPE_LUA(-3) / XJS_SEARCH_TYPE_MULTI(-2) / XJS_SEARCH_TYPE_WILDCARD / XJS_SEARCH_TYPE_REGEX / XJS_SEARCH_TYPE_SQL
XJS_API int XJS_CALL xjs_result_GetSearchType(xjs_result* result);

// 判断 SQL 查询是否包含 GROUP BY 聚合（搜索完成后才有效）
// 返回值：1=聚合查询（含 GROUP BY）  0=非聚合查询（普通 SELECT *）
// 用途: 据此选择取数方式——聚合查询用"SQL 聚合分组接口", 非聚合查询用"SQL 输出列表接口"
XJS_API int XJS_CALL xjs_result_IsSQLAggregate(xjs_result* result);

// 获取 SQL 编译/执行错误信息
// 返回值：UTF-8 文本指针，无需释放；空字符串表示无错误（查询成功或非 SQL 查询）
XJS_API const char* XJS_CALL xjs_result_GetSQLErrorMessage(xjs_result* result);

// 获取 SQL 错误在原始SQL文本中的起始字节偏移（对应 PG AST 节点 location，-1=无位置信息）
// 用途：配合 xjs_result_GetSQLErrorLength 在输入框/编辑器中高亮 SQL 出错部分
// 注意：偏移/跨度均为 UTF-8 字节单位（与 PG AST location、正则错误的"出错位置"语义一致），文本按字符索引时需先转换
XJS_API int XJS_CALL xjs_result_GetSQLErrorPosition(xjs_result* result);

// 获取 SQL 错误跨度字节数（高亮长度；0=无跨度信息）
// 跨度从出错位置向后扫描到下一个顶层分隔符/AND/OR/子句关键字等为止（跳过括号内嵌套）
XJS_API int XJS_CALL xjs_result_GetSQLErrorLength(xjs_result* result);

// 获取 SQL 查询结果并序列化为 JSON 文本（分页/限量，防止一次性导出几百万行）
// 非聚合查询返回 "行" 数组；聚合查询返回 "分组" 数组（每组一行，不展开成员）
// 参数 startRow：起始行/分组索引，从 0 开始
// 参数 maxRows：本次最多返回的行/分组数量；<=0 按默认 1000，超过 100000 被截断
// 返回值：UTF-8 JSON 文本指针，无需释放；空字符串=搜索结果对象无效
// JSON 结构：
//   {"聚合":bool,"字段数量":N,"字段":[...],"类型":[...0未知/1文本/2整数/3长整数],
//    "总数量":T,"起始":s,"限制":k,
//    "行":[[v,v,...],...]}                                 // 非聚合
//    "分组":[{"键整数":..,"键文本":"..","计数":..,"求和":..,"最小值":..,"最大值":..,
//             "聚合值":[...],"成员数量":..},...]}           // 聚合
XJS_API const char* XJS_CALL xjs_result_GetSQLJson(
    xjs_result* result,
    int startRow,           // 起始行/分组索引，从 0 开始
    int maxRows             // 本次最多返回的行/分组数；<=0 默认1000，上限100000
);

#ifdef __cplusplus
}
#endif