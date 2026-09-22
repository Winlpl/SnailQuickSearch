/*
 * ai_assistant.h — 「AI 助手」插件内部共享头 (结构定义 + 各编译单元接口声明)
 * ============================================================================
 * 文件分工 (一文件一职责, 与主程序 13 文件同口径):
 *   ai_core.cpp    基础设施: 编码转换 / 简易 JSON / 颜色几何 / 配置 / 多对话历史
 *   ai_agent.cpp   agent 大脑: 系统提示词组装 / 工具定义 / 引擎直连工具执行 /
 *                  请求体构建 / SSE 轮 (function calling) / 工作线程循环
 *   ai_session.cpp 会话层: 会话池 / 字体表面 / 发送入口 / 流泵 (UI 抽取) / 消息窗口
 *   ai_render.cpp  渲染层: markdown-lite / 排版断行 / 绘制小件 / 消息与工具卡片 /
 *                  输入框布局 / 整帧渲染 / 交付
 *   ai_input.cpp   交互层: 命中测试 / 鼠标滚轮键盘 IME / 输入框编辑 / 消息选区 /
 *                  接口设置对话框 / 历史侧栏操作
 *   ai_plugin.cpp  插件边界: GetInfo / Init / Shutdown / OnCommand / OnPanelEvent
 *
 * 跨文件符号一律在本头声明 (禁止各 cpp 互相前置声明); 文件内部实现保持 static。
 */
#ifndef AI_ASSISTANT_H
#define AI_ASSISTANT_H

#define NOMINMAX
#include <windows.h>
#include <gdiplus.h>
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

#include "../../xjs_plugin_sdk.h"
#include "../../xunjieso.h"   /* 引擎直连: 加载器按模块名绑到宿主进程已加载的同名 DLL 实例 */

/* ==================== 宿主接口 (Init 时存下, 指针终身有效; 定义在 ai_plugin.cpp) ==================== */

extern const XjsPluginHost* g_host;
extern XjsPluginCtx*        g_ctx;
extern ULONG_PTR            g_gdipToken;
extern unsigned             g_uiThread;
extern HWND                 g_msgwnd;    /* 消息窗口: 工作线程 PostMessage 回 UI 线程渲染 (定义在 ai_session.cpp) */

static bool HostHas(unsigned need) {   /* 宿主表按"只追加"扩字段: 取用前先验 size (旧宿主干净失败) */
    return g_host && g_host->size >= need;
}
#define HOST_PANEL_OK (HostHas(offsetof(XjsPluginHost, PanelSetCaret) + sizeof(void*)))

static const UINT XJS_AI_STREAM = WM_APP + 40;  /* 流式增量到达 (wParam=0, lParam=Job*) */
static const UINT XJS_AI_SWEEP  = WM_APP + 41;  /* 孤儿作业清扫 */

/* ==================== 基础工具 (实现 ai_core.cpp) ==================== */

std::wstring W8(const char* s);
std::string U8(const std::wstring& w);
std::wstring TrimW(const std::wstring& s);
size_t PrevCp(const std::wstring& s, size_t i);
size_t NextCp(const std::wstring& s, size_t i);
Gdiplus::Color HexCol(const std::wstring& hex, int alpha = 255);
Gdiplus::Color MixCol(Gdiplus::Color a, Gdiplus::Color b, float t);
Gdiplus::Color WithA(Gdiplus::Color c, BYTE a);
static float minf(float a, float b) { return a < b ? a : b; }
static float maxf(float a, float b) { return a > b ? a : b; }

/* ---- 简易 JSON (SSE 载荷/配置/历史 都是小型文档, 自带解析器零依赖) ---- */
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
struct JParser {
    const wchar_t* p;
    const wchar_t* end;
    bool ok = false;
    explicit JParser(const std::wstring& s) : p(s.c_str()), end(s.c_str() + s.size()) {}
    void Ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++; }
    Jv Num() {
        Jv v; v.t = 2;
        wchar_t* stop = NULL;
        v.num = wcstod(p, &stop);
        if (stop == p) { p++; return v; }
        p = stop;
        ok = true;
        return v;
    }
    Jv Val() {
        Jv v;
        Ws();
        if (p >= end) return v;
        wchar_t c = *p;
        if (c == '{') {
            v.t = 5; p++;
            Ws();
            if (p < end && *p == '}') { p++; ok = true; return v; }
            for (;;) {
                Ws();
                Jv k = (p < end && *p == '"') ? Str() : Jv();
                Ws();
                if (p < end && *p == ':') p++;
                v.obj.push_back({ k.str, Val() });
                Ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == '}') p++;
                break;
            }
            ok = true;
            return v;
        }
        if (c == '[') {
            v.t = 4; p++;
            Ws();
            if (p < end && *p == ']') { p++; ok = true; return v; }
            for (;;) {
                v.arr.push_back(Val());
                Ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == ']') p++;
                break;
            }
            ok = true;
            return v;
        }
        if (c == '"') return Str();
        if (c == 't') { p += 4; v.t = 1; v.b = true; ok = true; return v; }
        if (c == 'f') { p += 5; v.t = 1; v.b = false; ok = true; return v; }
        if (c == 'n') { p += 4; return v; }
        /* 裸词容错: 旧版转义函数漏引号写出的存量 (配置/历史) 值是裸文本 — 此前单字符跳过
           会让对象/数组解析提前断掉, 后续字段 (apiKeyEnc/会话消息) 全丢 = key 与会话记录
           "消失"。吞到分隔符整段取值: 纯数字按数, 其余按字符串; 新版写入恒为带引号合法
           JSON, 不走此径 */
        {
            const wchar_t* q = p;
            while (q < end && *q != L',' && *q != L']' && *q != L'}' && *q != L'\n' && *q != L'\r') q++;
            if (q > p) {
                wchar_t* stop = NULL;
                v.num = wcstod(p, &stop);
                if (stop == q) { v.t = 2; p = q; ok = true; return v; }
                v.t = 3;
                v.str.assign(p, q);
                p = q;
                ok = true;
                return v;
            }
        }
        return Num();
    }
    Jv Str() {
        Jv v; v.t = 3; p++;   /* 跳开引号 */
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) {
                p++;
                switch (*p) {
                    case '"': v.str += '"'; break;
                    case '\\': v.str += '\\'; break;
                    case '/': v.str += '/'; break;
                    case 'n': v.str += '\n'; break;
                    case 't': v.str += '\t'; break;
                    case 'r': v.str += '\r'; break;
                    case 'b': v.str += '\b'; break;
                    case 'f': v.str += '\f'; break;
                    case 'u': {
                        if (p + 4 < end) {
                            wchar_t cp = (wchar_t)wcstol(std::wstring(p + 1, p + 5).c_str(), NULL, 16);
                            v.str += cp;
                            p += 4;
                        }
                        break;
                    }
                    default: v.str += *p; break;
                }
                p++;
            } else {
                v.str += *p++;
            }
        }
        if (p < end) p++;   /* 闭引号 */
        ok = true;
        return v;
    }
};
std::string JsonEscapeUtf8(const std::wstring& s);   /* → 完整 JSON 字符串字面量 (含首尾引号) */
Jv JsonParseW(const std::wstring& text);

/* ---- 面板内坐标矩形 / 命中 id ---- */
struct FRect { float x = 0, y = 0, w = 0, h = 0;   /* 面板内坐标 (像素) */
               bool Hit(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; } };

enum {
    HIT_NONE = 0, HIT_STOPGEN = 1, HIT_HSET = 2, HIT_HHIST = 3, HIT_HNEW = 4,
    HIT_CLEAR = 5, HIT_INPUT = 6, HIT_SEND = 7, HIT_CLOSE = 8, HIT_INTHUMB = 9,
    HIT_SBROW = 20, HIT_SBDEL = 60,          /* +i: 侧栏行 / 行删除 */
    HIT_DURL = 100, HIT_DKEY = 101, HIT_DMODEL = 102, HIT_DCHK = 103, HIT_DCANCEL = 104, HIT_DSAVE = 105,
    HIT_THUMB = 120, HIT_TRACK = 121, HIT_REASON = 130,   /* +i: 推理块头 */
    HIT_STEPHEAD = 140                                    /* +i: 工具卡片头 (idxOut=消息下标; 点击=展开/收起样本) */
};

/* ==================== 配置 (存储键 "cfg"; 定义 ai_core.cpp) ==================== */

struct AiCfg {
    std::wstring baseUrl = L"https://api.deepseek.com";
    std::wstring model = L"deepseek-flash";   /* Responses API 仅新模型名可用 (deepseek-flash / deepseek-v4-pro;
                                                 deepseek-chat 等旧名在此端点被拒) */
    std::wstring apiKey;
    bool reasoning = false;   /* 深度思考: true=effort high, false=none (DeepSeek 须显式传) */
};
extern AiCfg g_cfg;
void CfgSave();
void CfgLoad();

/* ==================== 多对话历史 (存储键 "历史"; 定义 ai_core.cpp) ==================== */

struct AiToolStep {             /* 一次工具调用 (role==2 组内; 随历史落库, 载入时在途态折算为已中止) */
    int kind = 0;               /* 0=run_search 1=open_file 2=copy_paths (未知工具照显 name) */
    std::wstring name;          /* 工具名 (模型传回; 未知工具也照显) */
    int state = 0;              /* 0=排队 1=执行中 2=完成 3=失败 */
    std::wstring mode, query;   /* run_search 参数 */
    int count = -1;             /* run_search 命中总数 */
    long long elapsedMs = -1;
    std::wstring emit;          /* lua 两模式 ai.print 过程/统计输出 (并入工具结果 output 回喂模型; 不渲染不落库) */
    std::wstring err;           /* 失败原因 */
    std::vector<std::wstring> top;   /* 结果样本路径 (≤20; 展开显示) */
    bool open = false;          /* 样本列表展开态 (UI 态; 泵同步镜像时保留) */
};
struct AiMsg {
    int role = 0;               /* 0=user 1=assistant 2=工具步骤组 (随历史落库; 不重发给模型) */
    std::wstring text;
    std::wstring reason;        /* 推理过程 (只在折叠块显示, 从不发送/入库发送体) */
    bool reasonOpen = false;    /* 折叠块展开态 (流式中自动展开, 完成后收起) */
    std::vector<AiToolStep> steps;   /* role==2: 本组工具步骤 */
};
struct AiConv {
    unsigned long long id = 0;
    long long t = 0;            /* 最后活动时间 (time_t 秒) */
    std::wstring title;         /* 首条用户消息 (≤30 字) */
    std::vector<AiMsg> msgs;
};
extern std::vector<AiConv> g_hist;
extern unsigned long long g_nextConvId;
static const size_t AI_CONV_MAX = 30, AI_MSG_MAX = 200, AI_TEXT_MAX = 60000;
static const int AI_AGENT_MAX_TURNS = 12;  /* agent 工具循环上限 (最后一轮省略 tools 强制收尾); 12 轮给统计任务留够粗筛/纠错余量 */

void HistSave();
void HistLoad();
unsigned long long HistUpsert(unsigned long long curId, const std::vector<AiMsg>& msgs);

/* ==================== agent (实现 ai_agent.cpp) ==================== */

void AgentToolInit();
void AgentToolShutdown();
void BuildInstructions();      /* 系统提示词 = 角色说明 + 引擎内嵌 Lua 两规范 (进程一次) */

/* ==================== 会话 (定义 ai_session.cpp) ==================== */

struct AiJob {            /* 一次 agent 请求 (堆分配; 工作线程只摸它, UI 经消息泵抽取) */
    CRITICAL_SECTION cs;
    std::wstring out, reason;      /* 当前轮的文本增量 (worker 写, UI 抽; 每轮工具执行后清空重开) */
    int state = 0;                 /* 0=进行中 1=完成 2=失败 3=已中止 */
    int phase = 0;                 /* 0=流式中 1=工具执行中 (泵据此冻结/新开文本气泡) */
    std::vector<AiToolStep> steps; /* 工具步骤镜像 (泵同步进 msgs 的 role==2 消息; 保留 open 态) */
    int stepsVersion = 0;          /* steps 每次内容变化 +1 (泵据版本号决定重排) */
    std::wstring err;
    bool truncated = false;
    std::string hostA, pathA, keyA;   /* 请求要素 (UTF-8; worker 自取; 请求体每轮在 worker 构建) */
    INTERNET_PORT port = 443;
    bool secure = true;
    XjsWindowToken tok = 0;        /* 发起窗口 (open_file 走宿主 OpenFile 用) */
    std::vector<AiMsg> hist;       /* 发送时对话快照 (只含 role 0/1; worker 构造每轮 body 用) */
    volatile LONG abort = 0;
    HANDLE hReq = NULL;            /* UI 线程"停止"用它打断阻塞读 (并发关句柄=中断语义) */
    std::thread* th = NULL;
    bool thJoined = false;
    AiJob() { InitializeCriticalSectionAndSpinCount(&cs, 100); }
    ~AiJob() { DeleteCriticalSection(&cs); }
};
extern std::vector<AiJob*> s_orphans;   /* 会话已关而流未完的作业 (泵里清扫 join) */

/* 排版产物: 一行 = 若干 run (自带文本与样式, 渲染期不依赖临时块) */
struct AiLine {
    float h = 0;
    float prefixW = 0;    /* 列表前缀宽 (首行 runs 整体右移) */
    std::wstring prefix;  /* 列表前缀文本 ("• " / "1. ") */
    bool hard = true;     /* 逻辑行界 (真换行: 块首/代码行首); false = 软折行续行 (复制时不插 \n) */
    bool leadSpace = false; /* 软折行时被丢的行首空格 (复制拼接处补回, "ORDER BY" 不粘成 "ORDERBY") */
    struct Run { float x = 0, w = 0; std::wstring text; bool bold = false, code = false, accent = false, cjk = false; };
    std::vector<Run> runs;
    std::vector<float> tblCols;   /* 表格行: 列 x 边界 (气泡内容坐标系, 绘制列竖线; 空 = 普通行) */
    bool tblRuleAfter = false;    /* 表头末行: 本行底画横向分隔线 */
};

/* 最小断行单元 (排版/输入框布局共用) */
struct AiAtom {
    std::wstring t;
    float w = 0;
    bool code = false, bold = false, accent = false, cjk = false;
};

struct AiSess {
    bool inUse = false;
    XjsWindowToken tok = 0;
    long long serial = 0;
    int w = 0, h = 0;
    float scale = 1.0f;
    bool winActive = true;
    /* 皮肤 (GetSkinJson; 打开会话时取一次) */
    Gdiplus::Color cBg, cPanel, cText, cDim, cAccent, cLine;
    bool skinOk = false;
    /* 字体 (按 scale 重建) */
    Gdiplus::Font* fBody = NULL;      /* 12.5px 正文 (CJK = 雅黑; 拉丁 = Segoe, 同 em 双族 —
                                         单族画中英混排时拉丁字形观感突兀, 主程序 GDI+ 后端同口径) */
    Gdiplus::Font* fBodyB = NULL;
    Gdiplus::Font* fBodyL = NULL;     /* 拉丁版 (Segoe UI) */
    Gdiplus::Font* fBodyBL = NULL;
    Gdiplus::Font* fMono = NULL;      /* 11.5px 代码 */
    Gdiplus::Font* fTiny = NULL;      /* 10.5px 提示 */
    Gdiplus::Font* fTinyL = NULL;
    Gdiplus::Font* fTitle = NULL;     /* 15px 标题 (加粗) */
    float madeScale = 0.0f;
    Gdiplus::FontFamily* famUI = NULL;
    Gdiplus::FontFamily* famLat = NULL;   /* 拉丁家族 (不可用时共享 famUI; owns=false 不二次删) */
    bool famLatOwn = false;
    Gdiplus::FontFamily* famMono = NULL;
    /* 交付表面 (BGRA, 顶层不透明) */
    std::vector<uint8_t> px;
    int stride = 0, bw = 0, bh = 0;
    /* 数据 */
    bool loaded = false;
    std::vector<AiMsg> msgs;
    unsigned long long curId = 0;
    bool sending = false;
    AiJob* job = NULL;
    int lastStepsVer = -1;            /* 泵已同步到卡片的 stepsVersion (变化才重排) */
    int stepBase = 0;                 /* 本作业工具卡片起始下标 (SendCurrent 时定格; 历史恢复的
                                         role==2 卡片在其之前, 泉的步骤同步不碰它们) */
    int netStatus = 0;                /* 0=未配置/未知(灰) 1=正常(绿) 2=失败(红) */
    /* 输入框 */
    std::wstring input;
    size_t caret = 0;
    bool inputFocus = false;
    /* 输入框编辑 (对齐宿主搜索框 XjsLineEdit 口径): 选区/拖选/行导航期望列/单档撤销/右键编辑菜单 */
    size_t anchor = 0;                /* 选区锚点 (== caret = 无选区) */
    bool selDragging = false;         /* 按住拖选进行中 (宿主捕获中 MOVE 持续转发) */
    float expectCol = -1.0f;          /* ↑↓ 行导航的期望列 x (<0 = 无, 取当前列) */
    std::wstring undoText;            /* 单档撤销快照 (交换式) */
    size_t undoCaret = 0, undoAnchor = 0;
    bool undoSnap = false;
    int inScroll = 0;                 /* 输入框滚动: 首个可视行 (可视窗口 4 行, 溢出才 >0) */
    float inGrab = 0;                 /* 输入框滚动条拖拽: 抓点偏移 (>0 = 拖拽中) */
    FRect hInThumb;                   /* 输入框滚动条 (渲染回填; 无溢出 = 零矩形) */
    float inTrackY = 0, inTrackH = 0;
    bool menuOpen = false;            /* 输入框右键编辑菜单 (自绘浮层; 几何渲染回填) */
    FRect menuBox; std::vector<FRect> menuRows;
    /* 消息文本选区 (拖选复制; 单条消息内, atom 粒度 — 行/atom 两端点) */
    bool txtSel = false;
    bool txtDragging = false;
    int selMsg = -1;
    int selALine = -1, selAAtom = -1, selBLine = -1, selBAtom = -1;
    bool msgKeyBorrow = false;        /* 拖选开始时向宿主借的键盘 (Ctrl+C 复制用; 清选区即归还) */
    /* 滚动 */
    double scrollY = 0;
    bool sticky = true;               /* 吸底 (流式跟随); 向上滚即解除 */
    /* 历史侧栏 */
    bool sideOpen = false;
    double sideScroll = 0;
    bool clearArm = false;
    FRect sidePanel;                  /* 侧栏浮层矩形 (渲染回填; 点浮层外 = 关闭) */
    /* 接口设置对话框 */
    bool dlg = false;
    std::wstring dUrl, dKey, dModel;
    int dFocus = 0;                   /* 0无 1=URL 2=Key 3=Model */
    size_t dCaret = 0, dAnchor = 0;   /* 焦点字段的光标/选区锚 (UTF-16 下标, 密钥 ● 显示与值 1:1) */
    bool dSelDragging = false;        /* 对话框字段拖选进行中 (按住拖动扩选区) */
    bool dReason = false;
    /* 命中矩形 (渲染回填; 面板内坐标) */
    FRect hSet, hHist, hNew, hClose, hInput, hSend, hClear;
    FRect hThumb; float thumbTrackY = 0, thumbTrackH = 0;
    std::vector<FRect> hRows, hRowDel;
    FRect dUrlR, dKeyR, dModelR, dChkR, dCancelR, dSaveR;
    FRect dCardR;                     /* 对话框卡片整体 (卡内空白 = 标题/标签/按钮带空档, 点它不关闭) */
    int hover = HIT_NONE;             /* 高亮 hover (低频: 值变化才重渲染) */
    int press = HIT_NONE;
    float pressX = -1, pressY = -1;   /* 按下点 (蒙层取消判定: 按下与松开都在卡外才关闭) */
    int pressGrab = 0;                /* 滚动条拖拽: 抓点偏移 */
    /* 排版缓存 */
    struct MsgLayout {
        std::vector<AiLine> lines;
        std::vector<AiLine> reasonLines;   /* 推理块 (reason 非空才有) */
        float bodyH = 0, reasonH = 0, totalH = 0, bubbleW = 0;
        /* role==2 工具卡片组 */
        std::vector<float> stepHs;                       /* 每 step 卡片总高 */
        std::vector<std::vector<AiLine>> stepErrLines;   /* 失败信息折行 */
        std::vector<std::wstring> stepQueryCut;          /* 截断好的查询行 (排版期算定, 绘制期零测量) */
        std::vector<std::wstring> stepStat;              /* 右侧状态文字 (同上) */
        std::vector<float> stepStatW;
    };
    std::vector<MsgLayout> lay;
    std::vector<float> layOffsets;    /* 每条消息 y 起点 (排版产物) */
    float contentH = 0;               /* 消息流总高 */
    bool layDirty = true;
    int layW = 0;
    float layScale = 0;
};
extern AiSess g_sess[8];

AiSess* SessByTok(XjsWindowToken tok);
AiSess* SessFree();
void SessEnsureFonts(AiSess* s);
void SessEnsureSurface(AiSess* s);
void AbortSend(AiSess* s);
void SessSaveConv(AiSess* s);
void SessOpen(AiSess* s, XjsWindowToken tok, long long serial, int w, int h, float scale);
void SessClose(AiSess* s);
void SendCurrent(AiSess* s);
bool AiMsgWndCreate();
void WorkerMain(AiJob* j);     /* agent 工作线程入口 (SendCurrent 起线程; 实现ai_agent.cpp) */

/* ==================== 渲染 (定义 ai_render.cpp) ==================== */

static const float AI_HEAD_H = 42.0f;      /* ×scale */
static const float AI_LINE_H = 20.0f;      /* 正文行高 ×scale */
static const float AI_CODE_H = 16.5f;
static const float AI_TINY_H = 15.0f;

float AiMeasure(Gdiplus::Graphics& g, Gdiplus::Font* f, const std::wstring& t);
void RelayoutOne(AiSess* s, int mi);
void RenderDeliver(AiSess* s);   /* 渲染 + 交付 (UI 线程) */

/* 输入框布局 (行/字符位置表): 折行/光标/点定位/选区渲染/行导航五处同源 — 禁止各写一套折行累加 */
struct InpLine {
    size_t start = 0;             /* 行首字符偏移 */
    float w = 0;                  /* 行宽 */
    std::vector<AiAtom> atoms;
    std::vector<float> cum;       /* 各 atom 行内起点 x */
    size_t len() const { size_t n = 0; for (auto& a : atoms) n += a.t.size(); return n; }
};
void InputLayout(AiSess* s, float boxInnerW, std::vector<InpLine>* out);
void InputPosOf(AiSess* s, const std::vector<InpLine>& lay, float boxInnerW, size_t idx,
                int* outLine, float* outX);
size_t InputIndexFromPoint(AiSess* s, const std::vector<InpLine>& lay, float boxInnerW,
                           float px, float py, float boxX, float boxY);
int InputLineCount(AiSess* s, float boxInnerW);
void InputCaretPos(AiSess* s, float boxInnerW, int* outLine, float* outX);
int InOverflowLines(AiSess* s);
void InputGeom(AiSess* s, float* bx, float* by, float* bw, float* bh, float* inH);

/* ==================== 交互 (定义 ai_input.cpp) ==================== */

void PanelMouse(AiSess* s, int type, float x, float y, unsigned flags);
void PanelWheel(AiSess* s, float x, float y, int delta);
void PanelKey(AiSess* s, unsigned vk, unsigned flags);
void PanelChar(AiSess* s, unsigned int ch);
bool InputSelRange(AiSess* s, size_t* a, size_t* b);       /* 选区 [a,b) (渲染与交互共用) */
void MsgSelNorm(AiSess* s, int* aL, int* aA, int* bL, int* bA);   /* 消息选区两端归一 */
void MsgSelClear(AiSess* s);   /* 清消息选区并归还借用键盘 */
size_t FieldVisWindow(AiSess* s, const std::wstring& disp, float maxW, std::wstring* visOut);
float FieldWidthOf(AiSess* s, const std::wstring& t, size_t n);

#endif /* AI_ASSISTANT_H */
