/*
 * ai_assistant.h — 「AI 助手」插件内部共享头 (结构定义 + 各编译单元接口声明)
 * ============================================================================
 * 文件分工 (一文件一职责, 与主程序 13 文件同口径):
 *   ai_core.cpp    基础设施: 编码转换 / 简易 JSON / 颜色 / 配置 / 多对话历史
 *   ai_agent.cpp   agent 大脑: 系统提示词组装 / 工具定义 / 引擎直连工具执行 /
 *                  请求体构建 / SSE 轮 (function calling) / 工作线程循环
 *   ai_session.cpp 会话层: 会话池 / 发送入口 / 流泵 (UI 抽取 → Web 增量同步) / 消息窗口
 *   ai_web.cpp     Web 前端宿主: 系统 WebView2 生命周期 (环境/控制器/子窗口) /
 *                  C++↔JS JSON 桥 / 皮肤调色派生 / markdown→HTML (md4c) / 消息 HTML 生成
 *   ai_web_ui.cpp  嵌入式前端: 整套对话 UI 的单文件 HTML+CSS+JS (资源内嵌, 免外部文件)
 *   ai_plugin.cpp  插件边界: GetInfo / Init / Shutdown / OnCommand / OnPanelEvent
 *
 * 渲染口径 (2026-09-23): 整块 UI 交给系统 WebView2 (Edge 运行时) — 插件建真子窗口
 * 盖住面板内容区 (宿主表 PanelGetRect 定位), HTML/CSS/JS 全权负责排版/输入/选区/IME/
 * 滚动; C++ 只留业务状态 (会话/作业/工具/存储) 并经 PostWebMessage 增量推送。
 * 旧 litehtml+GDI+ 渲染层 (ai_render/ai_input/ai_html) 已整体退役。
 *
 * 跨文件符号一律在本头声明 (禁止各 cpp 互相前置声明); 文件内部实现保持 static。
 */
#ifndef AI_ASSISTANT_H
#define AI_ASSISTANT_H

#define NOMINMAX
#include <windows.h>
#include <gdiplus.h>   /* 仅用 Color 值类型 (皮肤色), 不做 GDI+ 初始化/绘制 */
#include <winhttp.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <cstring>
#include <atomic>
#include <thread>

#include "../../picojson.h"   /* JSON 唯一入口 (插件侧 include 收口本头): 解析/序列化都是模块的活 */
#include "../../xjs_plugin_sdk.h"
#include "../../xunjieso.h"   /* 引擎直连: 加载器按模块名绑到宿主进程已加载的同名 DLL 实例 */

/* ==================== 宿主接口 (Init 时存下, 指针终身有效; 定义在 ai_plugin.cpp) ==================== */

extern const XjsPluginHost* g_host;
extern XjsPluginCtx*        g_ctx;
extern unsigned             g_uiThread;
extern HWND                 g_msgwnd;    /* 消息窗口: 工作线程 PostMessage 回 UI 线程泵 (定义在 ai_session.cpp) */

static bool HostHas(unsigned need) {   /* 宿主表按"只追加"扩字段: 取用前先验 size (旧宿主干净失败) */
    return g_host && g_host->size >= need;
}
#define HOST_PANEL_OK   (HostHas(offsetof(XjsPluginHost, PanelSetCaret) + sizeof(void*)))
#define HOST_RECT_OK    (HostHas(offsetof(XjsPluginHost, PanelGetRect) + sizeof(void*)))
#define HOST_QAPI_OK    (HostHas(offsetof(XjsPluginHost, QueryApi) + sizeof(void*)))

static const UINT XJS_AI_STREAM = WM_APP + 40;  /* 流式增量到达 (wParam=0, lParam=Job*) */
static const UINT XJS_AI_SWEEP  = WM_APP + 41;  /* 孤儿作业清扫 */
static const UINT XJS_AI_UIJOB  = WM_APP + 42;  /* 工具编组: worker → UI 线程执行宿主扩展 API (lParam=AiUiJob*) */

/* ==================== 宿主扩展 API (QueryApi 按名解析; 全部仅 UI 线程) ====================
 * Init 时解析一次 (ApiResolveAll; host->size 先验), 存函数指针 — 未知名/旧宿主 = NULL,
 * 对应工具报"宿主不支持"干净降级。这些 API 摸窗口/设置状态, agent 工作线程**禁直调**:
 * 一律经 AiUiJob 编组到 g_msgwnd 的 UI 线程执行 (错线程宿主回 ERR_THREAD)。
 * 各指针类型/JSON 键/权限见 xjs_plugin_sdk.h 的名称式扩展 API 节。 */
struct HostApi {
    XjsApiSettingsGet   settingsGet;
    XjsApiSettingsSet   settingsSet;
    XjsApiGlobalGet     globalGet;
    XjsApiGlobalSet     globalSet;
    XjsApiWindowsEnum   windowsEnum;
    XjsApiWindowState   windowState;
    XjsApiWindowCmd     windowCmd;
    XjsApiWindowCreate  windowCreate;
    XjsApiModesList     modesList;
    XjsApiModesAdd      modesAdd;
    XjsApiModesRemove   modesRemove;
    XjsApiModesApply    modesApply;
    XjsApiPluginsList   pluginsList;
    XjsApiPluginsState  pluginsState;
    XjsApiMsgSend       msgSend;
    XjsApiSkinsList     skinsList;
    XjsApiWindowSelection windowSel;
    XjsApiLangsList     langsList;
    XjsApiWindowResult  windowResult;   /* window.result: 窗口结果对象裸指针 (旧宿主 = NULL) */
};
extern HostApi g_api;
void ApiResolveAll();   /* UI 线程 (Init) 解析全部名字; 旧宿主全 NULL */

/* ---- 工具的 UI 编组作业 (worker 堆分配 → PostMessage(XJS_AI_UIJOB) → UI 执行 → 事件回告) ----
 * 所有权: 正常路径 worker 等 done 后读结果并 delete; worker 超时/被停止放弃时登记进
 * 在飞表 (orphan=1), UI 执行完发现 orphan 自行 delete — 两侧都不悬垂 (实现 ai_agent.cpp)。 */
struct AiUiJob {
    int kind = 0;                  /* UIW_* 分派码 (ai_agent.cpp) */
    std::wstring s1, s2, s3;       /* 字符串参数 (窗口名/mode/json/…) */
    long long n1 = 0, n2 = 0;      /* 数值参数 (execute/reveal/fileId) */
    long long tok = 0;             /* 默认目标窗口令牌 (window 名留空时 = 对话所在窗) */
    std::string out8;              /* 成功: 结果 JSON (UTF-8) */
    std::wstring err;              /* 失败: 错误描述 (空 = 成功) */
    volatile LONG orphan = 0;      /* worker 已放弃 (UI 执行完代为 delete, 不再 SetEvent) */
    int doneSignaled = 0;          /* UI 已执行完 (s_uiCs 内读写; worker 放弃判定用) */
    int waitMs = 0;                /* 等待上限毫秒 (0=默认 15s; 保存对话框等用户交互的作业给长) */
    HANDLE done = NULL;            /* 一次性信号 */
};
struct AiJob;   /* 会话层作业 (下文定义; AgentUiCall 挂起等待期间要读它的 abort) */
void AgentUiDispatch(AiUiJob* jb);        /* UI 线程执行 (g_msgwnd wndproc 调; 内部 delete jb) */
std::wstring AgentUiCall(AiJob* j, AiUiJob* jb, std::string* out8);
                                          /* worker 侧: 投递+等完成; 返回错误描述 (空=成功, *out8=结果) */
long long AgentUiWindowToken(const std::wstring& name, long long defTok, std::wstring* err);
                                          /* 窗口名 → 令牌 (仅 UI 线程; 空=defTok, 查无=设 *err 返 0)。
                                             WebCommand "adj" 应用调整卡时用 (提案时只存名, 应用时才解析) */
std::wstring AgentApiErrText(int rc);     /* 扩展 API 错误码 → 短描述 (调整卡失败项展示用) */
xjs_result* AgentWindowResultOf(XjsWindowToken tok);
                                          /* 窗口令牌 → 该窗结果对象 (仅 UI 线程; 旧宿主/无窗 = NULL)。
                                             结果同步的捕获口: 发送/勾选/手动执行三处在 UI 线程调它 */
std::wstring AgentManualExec(XjsWindowToken tok, const std::wstring& mode, const std::wstring& query);
                                          /* 卡片右键"执行语句": 私有对象上异步重放该语句, 布防
                                             结果同步 — 完成事件回调把 ID 全集推进目标窗列表。
                                             仅 UI 线程; 只占串行锁完成提交 (agent 忙 = 拒绝);
                                             返回错误描述 (空 = 已提交, 结果经完成事件落窗) */

/* ==================== 基础工具 (实现 ai_core.cpp) ==================== */

std::wstring W8(const char* s);
std::string U8(const std::wstring& w);
std::wstring TrimW(const std::wstring& s);
size_t PrevCp(const std::wstring& s, size_t i);
size_t NextCp(const std::wstring& s, size_t i);
Gdiplus::Color HexCol(const std::wstring& hex, int alpha = 255);
Gdiplus::Color MixCol(Gdiplus::Color a, Gdiplus::Color b, float t);
Gdiplus::Color WithA(Gdiplus::Color c, BYTE a);

/* ---- JSON (统一收口根目录 picojson, 插件不手写解析器/不手拼文档) ----
 * 读侧: JsonParseW = picojson::parse 之后机械转换成本树的 Jv (宽字符视图),
 * 全库既有 v.S/v.Get/v.arr 读法零改动。写侧: 直接建 picojson::value (辅助 JS/JN/JB),
 * 序列化经 JDumpW/JvToP — 转义与数字格式全归 picojson, 禁止再手拼 "{\"x\":" 式文档。 */
struct Jv {
    int t = 0;   /* 0=null 1=bool 2=num 3=string 4=array 5=object */
    bool b = false;
    double num = 0;
    std::wstring str;
    std::vector<Jv> arr;
    std::vector<std::pair<std::wstring, Jv>> obj;
    const Jv* Get(const wchar_t* key) const {
        if (t != 5) return NULL;
        for (auto& kv : obj) if (kv.first == key) return &kv.second;
        return NULL;
    }
    std::wstring S(const wchar_t* key, const wchar_t* def = L"") const {
        const Jv* v = Get(key);
        return (v && v->t == 3) ? v->str : def;
    }
};
Jv JsonParseW(const std::wstring& text);              /* 宽 JSON 文本 → Jv (picojson 解析, 失败=null) */
bool JParseU8(picojson::value& out, const std::string& u8);   /* UTF-8 JSON 文本 → picojson (失败=false) */
picojson::value JvToP(const Jv& v);                   /* Jv 树 → picojson 值 (键/串转 UTF-8, 机械转换) */
inline picojson::value JS(const std::wstring& s) { return picojson::value(U8(s)); }   /* 宽串 → JSON 串 */
inline picojson::value JN(long long n) { return picojson::value((double)n); }         /* 整数 */
inline picojson::value JB(bool b) { return picojson::value(b); }                      /* 布尔 */
inline std::wstring JDumpW(const picojson::value& v) { return W8(v.serialize().c_str()); }
                                                      /* picojson 值 → 宽 JSON 文本 */

/* ==================== 配置 (存储键 "cfg"; 定义 ai_core.cpp) ====================
 * 模型档案 (多模型切换): 每条档案是一套完整接口配置 —— 跨服务商时地址与密钥也随档案走。
 * profiles/activeId 是事实源; 下面 baseUrl..maxOut 五项是**活动档案的派生镜像**,
 * 请求构造与用量显示只读它们 → "切换模型"只需 CfgApplyActive 重算镜像, 请求代码零改动。
 * 密钥不出宿主 (前端只见 hasKey): 复制/切换档案时密钥在 C++ 侧搬运。 */

struct AiProfile {
    std::wstring id;         /* "p"+时间戳+序号 (CfgGenProfileId 生成, 前端原样回传) */
    std::wstring name;       /* 显示名 (空 = 用模型名兜底, 再空 = "未命名模型") */
    std::wstring baseUrl;
    std::wstring apiKey;     /* 内存明文; 落盘加密 (XorSecret+本机 GUID), 明文永不回传前端 */
    std::wstring model;
    long long ctx = 0;       /* 上下文长度 token; 0 = 按模型名推断 (只影响用量"剩余"显示) */
    long long maxOut = 0;    /* 最大输出 token; 0 = 不发送 max_output_tokens 参数 */
    /* 多模态输入能力 (按模型实际支持勾选; 默认全关 = 显式声明, 旧档案缺键同样视为关):
     * 只管"用户能不能附这种输入" — 历史里已有的附件在请求里按同一开关取舍 (关了就不发对应 part)。 */
    bool img = false;        /* 图片输入 (粘贴/附带图片 → input_image) */
    bool video = false;      /* 视频输入 (附带视频文件 → input_file data URL) */
    bool audio = false;      /* 音频输入 (附带音频文件 → input_file data URL) */
};

struct AiCfg {
    std::wstring baseUrl = L"https://api.deepseek.com";
    std::wstring model = L"deepseek-flash";   /* Responses API 仅新模型名可用 (deepseek-flash / deepseek-v4-pro;
                                                 deepseek-chat 等旧名在此端点被拒) */
    std::wstring apiKey;
    long long ctx = 0;        /* 活动档案的上下文长度 (派生镜像) */
    long long maxOut = 0;     /* 活动档案的最大输出 (派生镜像) */
    bool img = false;         /* 活动档案的多模态能力 (派生镜像, 同 ctx/maxOut 口径) */
    bool video = false;
    bool audio = false;
    bool reasoning = false;   /* 深度思考: true=effort high, false=none (DeepSeek 须显式传; 进程级, 不随档案) */
    int filePolicy = 2;       /* 文件操作权限 (对话区下方分段控件): 0=禁用 1=只读 2=询问 3=允许;
                                 禁用/只读拒绝 open_file 与 copy_paths, 询问先拒后给确认卡, 允许直接执行 */
    int execPolicy = 2;       /* 命令执行权限 (run_command 外部程序): 0=禁用 2=询问 3=允许 (没有"只读"
                                 — 执行没有读取档, 默认询问 = 每条命令出确认卡, 用户"允许一次"只放行
                                 本条命令的完全相同重试, 不持久放权) */
    bool syncResults = false; /* 结果同步 (对话区勾选): 开启后每次 run_search 完成把结果 FileId 全集
                                 经宿主 window.resetResult 重置进发起窗口的列表 (AI 搜到什么, 用户
                                 左侧列表就是什么; 含 lua_exec 的顶层 return 集合) */
    std::vector<AiProfile> profiles;   /* 档案表 = 事实源 (上限 50, AiProfileMax) */
    std::wstring activeId;             /* 当前使用档案的 id (失效回落第一条, 与前端同规) */
};
extern AiCfg g_cfg;
constexpr int AiProfileMax = 50;
void CfgSave();
void CfgLoad();
AiProfile* CfgActive();                    /* activeId 校验失效回落第一条; 空表 = NULL */
void CfgApplyActive();                     /* 活动档案 → 派生镜像 (切档/保存后必须调) */
std::wstring CfgDisplayName(const AiProfile* p);   /* name || model || 未命名模型 (前端同款兜底) */
std::wstring CfgGenProfileId();
long long CfgClampTok(double v);           /* token 长度夹取 (0=未指定, 上限 1e8) */

/* ==================== 多对话历史 (按会话拆分存储; 定义 ai_core.cpp) ====================
 * 索引 = 存储键 "历史索引" ([{"会话标题","文件名","id","时间"}], 只有元数据 — 打开面板零消息体载入);
 * 会话正文 = 每会话一个存储文件 (索引."文件名" 指向 data\ 下的 "会话-<id>.json"), load/落库时按需读写。
 * 旧版全量键 "历史" (所有会话堆一个文件) 在索引缺失时一次性搬入分文件+索引后删除。 */

/* 待应用的调整 (AI 提案 → 用户逐项裁决, 不直接生效):
   worker 只生成提案挂进步骤; 用户点卡片按钮 → WebCommand "adj" 在 UI 线程应用。
   状态活在会话份步骤上 — 泵的步骤比对不含 adj 字段, UI 裁决不会被 worker 镜像回写。 */
struct AiAdjustItem {
    std::wstring key;           /* 显示名 (设置键名 / 动作名) */
    std::wstring val;           /* 显示值 (用户视角文案, 如 "详情"/"150%"/"简体中文") */
    std::string json;           /* 应用参数 (UTF-8): kind 0/1=单成员 JSON; 2=动作串; 3="档案名|继承窗名" */
    int state = 0;              /* 0=待确认 1=已应用 2=已忽略 3=失败 (err 带原因) */
    std::wstring err;
};
struct AiAdjust {
    int kind = 0;               /* 0=settings.set 1=settings.global.set 2=window.cmd 3=window.create */
    std::wstring win;           /* 目标窗口名 (空 = 当前对话所在窗; 应用时再解析令牌 — 关窗后应用报错不悬垂) */
    std::vector<AiAdjustItem> items;
};
struct AiToolStep {             /* 一次工具调用 (role==2 组内; 随历史落库, 载入时在途态折算为已中止) */
    int kind = 0;               /* 0=run_search 1=open_file 2=copy_paths 3=设置 4=窗口 5=搜索框
                                   6=模式 7=插件 8=皮肤 9=规范 10=捐赠 11=run_command 命令 (未知工具照显 name) */
    std::wstring name;          /* 工具名 (模型传回; 未知工具也照显) */
    int state = 0;              /* 0=排队 1=执行中 2=完成 3=失败 4=策略询问 (被权限闸拒绝, 卡上带确认按钮) */
    std::wstring mode, query;   /* run_search 参数 */
    std::wstring argz;          /* 代办类工具的参数摘要 (卡片头展示; 随历史落库) */
    std::string res8;           /* 回喂模型的 JSON (瞬时; 不渲染不落库) — run_search/get_window_selection
                                   在完成时现场拼装 (样本=[FileId,文件名], 路径不回喂), 其余=宿主扩展 API 原样 */
    int count = -1;             /* run_search 命中总数 */
    long long elapsedMs = -1;
    std::wstring err;           /* 失败原因 */
    std::vector<std::wstring> top;   /* 结果样本完整路径 (≤20; 卡片展开显示用, 不回喂模型) */
    std::vector<std::wstring> wrote; /* lua_exec 脚本成功写出的文件路径 (ai.write/ai.saveas;
                                         卡片常显块+随历史落库; 不回喂模型 — 模型经结果 JSON 得知) */
    bool open = false;          /* 样本列表展开态 (纯前端 UI 态, JS 自持; C++ 不再同步) */
    AiAdjust adj;               /* 待应用的调整 (非空 = 卡上带逐项 应用/忽略 按钮; 随历史落库) */
};
/* 用户消息附带的多模态输入 (随历史落库; 图片经前端压缩, 视频/音频直接 data URL)。
 * dataUrl = "data:<mime>;base64,<...>" — 请求体 (input_image/input_file) 与
 * 气泡渲染 (<img src>) 共用同一份; 落库后被存储预算裁剪清空 = 只留占位 (不再上请求)。 */
struct AiAttach {
    int kind = 0;             /* 0=图片 1=视频 2=音频 */
    std::wstring name;        /* 原始文件名 (input_file 的 filename 与卡片文案; 图片可为空) */
    std::wstring dataUrl;
};
struct AiMsg {
    int role = 0;               /* 0=user 1=assistant 2=工具步骤组 (随历史落库; 不重发给模型) */
    std::wstring text;
    std::vector<AiAttach> atts; /* role==0: 附带的图片/视频/音频 (多模态输入, 上限 AI_ATT_MAX 个) */
    std::wstring reason;        /* 推理过程 (只在折叠块显示, 从不发送/入库发送体) */
    bool reasonOpen = false;    /* 折叠块展开态 (流式中自动展开, 完成后收起) */
    bool err = false;           /* 失败消息 (气泡转错误配色) */
    std::vector<AiToolStep> steps;   /* role==2: 本组工具步骤 */
};
struct AiConvRef {                  /* 历史索引条目 (会话正文在自己的文件里, 索引只存元数据) */
    unsigned long long id = 0;
    long long t = 0;            /* 最后活动时间 (time_t 秒) */
    std::wstring title;         /* 首条用户消息 (≤30 字) */
    std::wstring file;          /* 会话正文存储键/文件名 (形如 "会话-<id>.json") */
};
struct AiConv {
    unsigned long long id = 0;
    long long t = 0;            /* 最后活动时间 (time_t 秒) */
    std::wstring title;         /* 首条用户消息 (≤30 字) */
    std::vector<AiMsg> msgs;
};
extern std::vector<AiConvRef> g_hist;   /* 历史索引 (侧栏列表只读这里的元数据; 正文按需 HistGet) */
extern unsigned long long g_nextConvId;
static const size_t AI_CONV_MAX = 30, AI_MSG_MAX = 200, AI_TEXT_MAX = 60000;
static const int AI_AGENT_MAX_TURNS = 12;  /* agent 工具循环上限 (最后一轮省略 tools 强制收尾); 12 轮给统计任务留够粗筛/纠错余量 */
/* 多模态附件上限 (dataUrl 字符数; 前端已压缩图片, 这里是硬闸 — 单条消息总量与历史存储都要够得着):
 * 历史按会话还有总预算 AI_ATT_HIST_BUDGET (HistUpsert 里最旧的先清成占位), 防大视频把
 * 存储/请求体撑爆。 */
static const int AI_ATT_MAX = 4;
static const size_t AI_ATT_IMG_MAX = 4u * 1024 * 1024;
static const size_t AI_ATT_VIDEO_MAX = 24u * 1024 * 1024;
static const size_t AI_ATT_AUDIO_MAX = 12u * 1024 * 1024;
static const size_t AI_ATT_HIST_BUDGET = 48u * 1024 * 1024;

void HistLoad();                    /* 打开面板时只读索引 (旧全量键首次自动迁移); 定义 ai_core.cpp */
unsigned long long HistUpsert(unsigned long long curId, const std::vector<AiMsg>& msgs);
                                    /* 当前会话落盘 = 写自己的会话文件 + 索引原位更新/追加 */
bool HistGet(unsigned long long id, AiConv& out);   /* 按需读取一个会话正文 (侧栏 load 入口) */
void HistRemove(unsigned long long id);             /* 删除会话 = 正文文件 + 索引条目一并移除 */
void HistClearAll();                                /* 清空历史 = 全部正文文件 + 索引 */

/* ==================== agent (实现 ai_agent.cpp) ==================== */

void AgentToolInit();
void AgentToolShutdown();
void BuildInstructions();      /* 系统提示词 = 常驻骨架 + 引擎内嵌 Lua 规范全文附录 (进程一次) */

/* ==================== 会话 (定义 ai_session.cpp) ==================== */

struct AiJob {            /* 一次 agent 请求 (堆分配; 工作线程只摸它, UI 经消息泵抽取) */
    CRITICAL_SECTION cs;
    std::wstring out, reason;      /* 当前轮的文本增量 (worker 写, UI 抽; 每轮工具执行后清空重开) */
    std::wstring note;             /* 过程状态条 (重试/超限自愈中; worker 写, status 推送带走, 空=无) */
    int gen = 0;                   /* 文本世代: worker 丢弃半截流重新生成时 +1 (泵据此把
                                      尾部气泡文本重置为当前 out, 不留上一轮残文) */
    int state = 0;                 /* 0=进行中 1=完成 2=失败 3=已中止 */
    int phase = 0;                 /* 0=流式中 1=工具执行中 (泵据此冻结/新开文本气泡) */
    std::vector<AiToolStep> steps; /* 工具步骤镜像 (泵同步进 msgs 的 role==2 消息) */
    int stepsVersion = 0;          /* steps 每次内容变化 +1 (泵据版本号决定同步) */
    std::wstring err;
    bool truncated = false;
    std::string hostA, pathA, keyA;   /* 请求要素 (UTF-8; worker 自取; 请求体每轮在 worker 构建) */
    INTERNET_PORT port = 443;
    bool secure = true;
    XjsWindowToken tok = 0;        /* 发起窗口 (open_file 走宿主 OpenFile 用) */
    volatile LONG policy = 2;      /* 文件操作权限快照 (发送时定格; 确认卡"允许"后由 UI 更新,
                                      之后的工具调用即时放行; g_cfg.filePolicy 为持久事实源) */
    volatile LONG execPolicy = 2;  /* 命令执行权限快照 (同上, g_cfg.execPolicy 的发送时定格) */
    volatile LONG syncRes = 0;     /* 结果同步勾选 (g_cfg.syncResults 发送时定格; 勾选切换即时更新
                                      在跑的作业, 下一次 run_search 起按新值同步) */
    xjs_result* syncWin = NULL;    /* 同步目标窗的结果对象 (发送时在 UI 线程经 window.result 捕获;
                                      完成事件回调里用前自查 IsEffective, 窗口已关即弃) */
    /* run_command 询问档的裁决通道 (dsh approval 口径: 审批**挂起**工具调用等用户裁决,
     * 答复恢复 — 不是"拒绝后指望模型自己重试"的死胡同): worker 出询问卡后在此轮询,
     * eallow/edeny 置位, worker 消费 (允许=继续执行, 拒绝=作为工具失败回喂模型)。
     * 一次只有一张询问卡 (worker 挂起期间模型无法再发调用), 裸标志即可, j->cs 护。 */
    volatile LONG execGrant = 0;   /* 用户点了「允许一次」 */
    volatile LONG execDeny = 0;    /* 用户点了「拒绝」 */
    struct TurnUsage {             /* 最近一轮 SSE 的用量 (response.completed.usage; 泵累计进会话) */
        bool has = false;
        long long prompt = 0, completion = 0, total = 0, cacheHit = 0;
    } turnUsage;
    bool turnUsageTaken = false;   /* 泵已把 turnUsage 累计进会话 (每轮取一次) */
    ULONGLONG turnOutMs = 0;       /* 本轮输出耗时 (速度 = completion/秒, 不跨轮平均) */
    std::vector<AiMsg> hist;       /* 发送时对话快照 (只含 role 0/1; worker 构造每轮 body 用) */
    volatile LONG abort = 0;
    HANDLE hReq = NULL;            /* UI 线程"停止"用它打断阻塞读 (并发关句柄=中断语义) */
    std::thread* th = NULL;
    bool thJoined = false;
    AiJob() { InitializeCriticalSectionAndSpinCount(&cs, 100); }
    ~AiJob() { DeleteCriticalSection(&cs); }
};
extern std::vector<AiJob*> s_orphans;   /* 会话已关而流未完的作业 (泵里清扫 join) */

struct AiSess {
    bool inUse = false;
    XjsWindowToken tok = 0;
    long long serial = 0;
    int w = 0, h = 0;
    float scale = 1.0f;
    /* 皮肤 (GetSkinJson; 打开会话时取一次, 皮肤事件后重取) */
    Gdiplus::Color cBg, cPanel, cText, cDim, cAccent, cLine;
    bool skinOk = false;
    /* 数据 */
    bool loaded = false;
    std::vector<AiMsg> msgs;
    unsigned long long curId = 0;
    bool sending = false;
    AiJob* job = NULL;
    int lastStepsVer = -1;            /* 泵已同步到卡片的 stepsVersion (变化才拷镜像) */
    int stepBase = 0;                 /* 本作业工具卡片起始下标 (SendCurrent 时定格; 历史恢复的
                                         role==2 卡片在其之前, 泵的步骤同步不碰它们) */
    int syncGen = -1;                 /* 泵已对齐的文本世代 (job->gen 变化 = worker 丢了半截流重生成,
                                         泵把尾部气泡文本重置为当前 out, 上一轮残文不留) */
    int netStatus = 0;                /* 0=未配置/未知(灰) 1=正常(绿) 2=失败(红) */
    /* 用量 (对齐参考实现: 累计=计费量; 上下文占用只认最近一次请求) */
    long long uPrompt = 0, uCompletion = 0, uTotal = 0, uCacheHit = 0, uCacheWrite = 0;
    long long uLastPrompt = 0, uLastCompletion = 0, uLastCacheHit = 0;
    double uTokPerSec = 0;
    bool usageHas = false;
    /* Web 前端 (实现 ai_web.cpp; 不透明指针 — 共享头不 include WebView2) */
    struct AiWebCtx* web = NULL;      /* 会话 Web 上下文 (SessOpen 建, SessClose 收) */
    long long pushStamp = 0;          /* 结构性变化 +1 (新消息/卡片/收尾等非流式文本变动) → 全量重推 */
    long long syncStamp = -1;         /* 前端已收到的 pushStamp */
    int syncN = -1;                   /* 前端已收到的消息数 */
    size_t syncText = 0, syncReason = 0;   /* 前端已收到的末条 text/reason 长度 (流式增量判定) */
    bool histSynced = false;          /* 前端历史列表与 g_hist 一致 */
    bool bootDone = false;            /* JS 已 ready 且 boot 快照已推 */
};
extern AiSess g_sess[8];

AiSess* SessByTok(XjsWindowToken tok);
AiSess* SessFree();
void SessLoadSkinOf(AiSess* s);                /* 皮肤五色 ← GetSkinJsonOf(会话所属窗) */
void AbortSend(AiSess* s);
void SessSaveConv(AiSess* s);
void SessOpen(AiSess* s, XjsWindowToken tok, long long serial, int w, int h, float scale);
void SessClose(AiSess* s);
void SendCurrent(AiSess* s, const std::wstring& text, const std::vector<AiAttach>* atts = NULL);
                                  /* atts = 本条消息附带的多模态输入 (NULL=无); 越闸项剔除并提示 */
bool AiMsgWndCreate();
void WorkerMain(AiJob* j);     /* agent 工作线程入口 (SendCurrent 起线程; 实现ai_agent.cpp) */

/* ==================== 皮肤调色 (实现 ai_web.cpp; 参考 AI 对话框的调色派生) ====================
 * 白/黑透明叠加类 (hover/divider/边框) 一律从文字色取 alpha — 深浅皮肤两用 */
struct AiPal {
    Gdiplus::Color bg, panel, text, dim, accent;   /* 皮肤五色 (别名) */
    Gdiplus::Color t3;             /* text-tertiary (三级文字) */
    Gdiplus::Color hover;          /* btn-secondary-hover (悬停叠加) */
    Gdiplus::Color divider;        /* 分隔线 */
    Gdiplus::Color border;         /* glass-border */
    Gdiplus::Color borderStrong;   /* overlay-border */
    Gdiplus::Color cyan, emerald, amber, red, ok;  /* 语义色 (信息/成功/警告/危险/在线) */
    Gdiplus::Color userAcc;        /* 用户气泡暖橙 */
};
AiPal PalOf(AiSess* s);
std::wstring ColHex(const Gdiplus::Color& c);      /* → "#rrggbb" */
std::wstring ColHexA(const Gdiplus::Color& c);     /* → "#rrggbbaa" */

/* ==================== Web 前端宿主 (实现 ai_web.cpp; 全部 UI 线程) ====================
 * 推送协议 (C++ → JS, PostWebMessageAsJson):
 *   {t:"boot",...}   JS ready 后的会话全量快照 (cfg/调色/历史/当前对话/用量/状态)
 *   {t:"pal",...}    皮肤调色 (EVT_SKIN / 打开会话)
 *   {t:"cfg",...}    接口配置变化 (保存后)
 *   {t:"convs",...}  历史列表 (最新在前由 JS 排; 载入/增删/清空/落库后)
 *   {t:"msgs",...}   当前对话全量 (含每条消息 HTML; 消息数或结构性变化后)
 *   {t:"last",...}   流式中的末条助手消息 (正文/推理 HTML 全量重推 — 部分增量无法转 md)
 *   {t:"usage",...}  用量计数
 *   {t:"status",...} 发送中/网络状态 (工具栏状态点)
 *   {t:"toast",...}  页面内提示 (面板被浏览器子窗盖住, 宿主 Toast 不可见 — WebToast)
 * 命令协议 (JS → C++, postMessage): 见 WebCommand (ai_web.cpp) — send/stop/close/
 *   profSave/profNew/profDel/profActive (模型档案: 保存/新建·复制/删除/切换) /
 *   policy/pallow/pdeny/retry/new/load/del/clearHist/copy/openurl/ready +
 *   search/searchfill (搜索卡片: 词+模式, 区分是否立即执行) / open/reveal/copypath
 *   (文件路径链接: 单击打开 / 右键定位·复制)。
 * 安全面: 模型输出永不产生活 HTML (md4c 转换层 HTML/实体按旧口径裁剪转义), CSP 关
 *   fetch/XHR/表单/导航, 外链只经 openurl 命令走 ShellExecute。 */
void WebInit();                                   /* 进程一次: 子窗口类注册等 */
void WebShutdown();                               /* 全部会话控制器/子窗收尾 (Shutdown 调, SessClose 之前) */
void WebSessionCreate(AiSess* s);                 /* OPEN: 建子窗口 + 异步建控制器 + ready 后 boot */
void WebSessionDestroy(AiSess* s);                /* CLOSE: 收控制器与子窗口 (幂等) */
void WebSessionRect(AiSess* s);                   /* OPEN/RESIZE: 按宿主矩形重定位子窗口 + ZoomFactor */
void WebSyncSession(AiSess* s);                   /* 泵/命令后: 按同步状态推增量 (msgs/last/status/usage) */
void WebSyncHist();                               /* g_hist 变化后向全部活跃会话推 convs */
void WebTouch(AiSess* s);                         /* 会话数据结构性变化登记 (→ 全量重推) */
void WebToast(AiSess* s, const char* utf8, int kind);
                                                  /* 页面内提示 (kind=XJS_PLUGIN_TOAST_*; 面板开着时宿主 Toast 被浏览器子窗盖住) */
void WebCommand(AiSess* s, const Jv& msg);        /* JS 命令分发 (WebMessageReceived 回调) */
const void* AgentFetchFileIco(int fileId, int* outLen);
                                                  /* UI 线程取引擎图标 PNG (pathcheck 用, 定义 ai_agent.cpp):
                                                   * agent 忙 TryEnter 失败 = NULL, 本轮放弃下轮再试 */
void WebPushSkin(AiSess* s);                      /* 皮肤变化后向该会话重推调色 (EVT_SKIN) */
std::wstring WebPaletteJson(AiSess* s);           /* 调色 → {"bg":"#..",...} */
std::wstring WebCfgJson();                        /* g_cfg → 活动派生值 + 档案表 (密钥只出 hasKey) */
void MsgHtmlOf(AiSess* s, const AiMsg& m, int mi, bool thinking, std::wstring* out);
                                                  /* 消息气泡 HTML (含工具卡片; data-act 点击路由) */
void WebMsgObj(AiSess* s, const AiMsg& m, int mi, bool thinking, bool withHtml, std::string* out);
                                                  /* 单条消息 → JSON 对象字面量 (msgs/last 共用; UTF-8 组装) */

/* markdown → HTML (md4c; 表格/任务列表/删除线; 模型裸 HTML 不渲染 — 实现收口本文件) */
bool MdToHtml(const std::wstring& text, std::wstring* out);

/* 捐赠二维码 data URL (0=微信 1=支付宝; 空串=不可用; get_donate_qr 工具与 md 渲染层共用;
 * 进程内缓存一次, SRWLOCK 护双线程 — 实现 ai_web.cpp) */
std::wstring DonateQrDataUrl(int kind);

/* 嵌入式前端整文档 (实现 ai_web_ui.cpp; 两段宽字面量拼接 — MSVC 单字面量 32767 字符上限) */
const wchar_t* AiWebUiHtml();

#endif /* AI_ASSISTANT_H */
