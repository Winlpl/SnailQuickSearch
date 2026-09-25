/*
 * ai_agent.cpp — agent 大脑: 引擎直连工具执行 / 系统提示词组装 / 工具定义 /
 * 请求体构建 / SSE 轮 (function calling) / 工作线程循环
 * 私有结果对象 = 进程级单例 + 互斥串行, 不碰窗口结果对象; 五种模式统一 xjs_result_Query
 * 提交 (-4 执行模式也走 Query), waitComplete=FALSE 异步发起 + 轮询等完成 (80ms 一查) —— 不能用 waitComplete=TRUE
 * 一路阻塞: 阻在引擎里时"停止"按钮的 abort 无人看, 工具卡片永远"执行中"。
 * 轮询期 abort → CancelSearch 取消在途搜索; 120 秒兜底超时; 串行锁也轮询占用
 * (多窗排队等锁同样可被停止打断)。
 */
#include "ai_assistant.h"

/* ==================== 引擎直连 (agent 工具执行层) ====================
 * 插件进程 = 宿主进程, 加载器按模块名绑到宿主已加载的 xunjieso.dll 同一实例;
 * xjs_GetDefaultEngine() 取句柄。agent 的搜索全部打到插件私有结果对象 g_agentRes
 * (不碰窗口结果对象 = 不打扰用户正在看的搜索)。工具执行一律在 agent 工作线程 +
 * g_agentCs 串行 (多窗口同时让 AI 跑工具时排队, 排队等锁可被停止打断), 搜索用
 * waitComplete=FALSE 异步发起 + 轮询 IsCompleted, abort 即 CancelSearch。
 * 结果对象必须引擎空闲态创建 (忙时创建会被永久定成文件名序), 懒建 + 失效重建。 */

static CRITICAL_SECTION g_agentCs;
static xjs_result* g_agentRes = NULL;     /* agent 私有结果对象 (进程级一份, 串行使用) */
static std::vector<int> g_agentTopIds;    /* 最近一次 run_search 样本 fileId (open_file 按 1-based 序号取用) */

/* 搜索失败信息槽 (XJS_RESULT_EVENT_FAILED 在引擎搜索线程触发, 只带回当前指纹的;
 * 独立小锁 —— 回调若等 g_agentCs 会与轮询线程互等死锁)。SQL 解析/正则编译失败只
 * 触发 FAILED 不触发 COMPLETE, 靠它把真实错误喂回模型, 否则伪装成"命中 0 条"。 */
static CRITICAL_SECTION g_srchErrCs;
static std::wstring g_srchErr;            /* 错误 JSON 原文 */
static int g_srchErrFp = -1;              /* 所属搜索指纹 */

static int AgentOnSearchFailed(void* userData, xjs_engine* eng, xjs_result* res,
                               int searchFingerprint, const char* errorJson) {
    (void)userData; (void)eng; (void)res;
    EnterCriticalSection(&g_srchErrCs);
    g_srchErr = W8(errorJson ? errorJson : "");
    g_srchErrFp = searchFingerprint;
    LeaveCriticalSection(&g_srchErrCs);
    return 0;
}

/* ---- 结果同步 (对话区勾选 / 卡片右键"执行语句"; 2026-09-26 用户口径) ----
 * 在私有结果的 XJS_RESULT_EVENT_COMPLETE 回调里把 ID 全集 xjs_result_ResetFileId 进
 * 目标窗口的结果对象 — 引擎的事件机制自带线程与时机, 不经 UI 编组、不另起线程。
 * 布防 = 提交搜索前置位 g_syncArm (run_search 勾选开启时 / 手动执行时), 回调消费:
 * 只认未被更新搜索覆盖 (discarded=FALSE) 的完成, 目标窗已关 (IsEffective=FALSE) 即弃。
 * g_syncWin 由 UI 线程经 window.result 捕获 (AgentWindowResultOf); 引擎对窗口结果对象
 * 的行缓存/重绘链自理 (ResetFileId 触发其变化事件, 宿主照常刷新)。 */
static xjs_result* volatile g_syncWin = NULL;   /* 同步目标窗结果对象 (UI 线程写, 回调线程读) */
static volatile LONG g_syncArm = 0;             /* 布防标志: 1 = 下一次有效完成执行同步 (一次性) */

static int AgentOnSearchComplete(void* userData, xjs_engine* eng, xjs_result* res,
                                 int searchFingerprint, const char* keyword, BOOL discarded) {
    (void)userData; (void)eng; (void)searchFingerprint; (void)keyword;
    if (discarded) return 0;                       /* 被更新的搜索覆盖: 同步交给最新一次 */
    if (!InterlockedCompareExchange(&g_syncArm, 0, 0)) return 0;
    InterlockedExchange(&g_syncArm, 0);            /* 一次性: 消费布防 */
    xjs_result* win = (xjs_result*)g_syncWin;
    if (!win || win == res) return 0;
    if (!xjs_result_IsEffective(win)) return 0;    /* 目标窗已关 (结果对象进销毁队列) */
    int n = xjs_result_GetCount(res);
    std::vector<int> ids;
    ids.reserve((size_t)(n > 0 ? n : 0));
    for (int i = 0; i < n; i++) {
        int fid = xjs_result_GetFileId(res, i);
        if (fid >= 0) ids.push_back(fid);
    }
    xjs_result_ResetFileId(win, ids.empty() ? NULL : ids.data(), (int)ids.size());
    return 0;
}

/* ---- Lua 过程输出通道 (结果对象级注册 ai.print) ----
 * 引擎规范让统计型脚本"数据走 print", 但 print 只进引擎调试输出、SDK 无读取口 —— 统计
 * 数字物理上回不到模型。故在 g_agentRes 上注册 ai.print(...)(xjs_result_LuaRegisterFunction,
 * 仅本结果对象的 -3/-4 虚拟机生效, 随对象销毁自动消失), 脚本的过程/统计文本经它进缓冲,
 * 完成后并入工具结果 JSON 的 output 字段回喂模型。回调在引擎搜索/执行线程触发 —— 独立小锁,
 * 禁止碰 g_agentCs (轮询线程持锁等完成会互等死锁, 同 AgentOnSearchFailed 口径)。
 * 缓冲发起前清空、完成后整体取走 = 恰好本次运行 (工具串行锁保证期间无第二个 agent 搜索);
 * 16KB 封顶防失控脚本。 */
static CRITICAL_SECTION g_emitCs;
static std::string g_emitBuf;
static bool g_emitCut = false;
static size_t g_emitDrop = 0;    /* 截断丢弃的字节量 (读取点取走, 精确省略量回喂模型 —
                                    dsh output-retention 口径: 省略数恒给精确值) */
static std::string g_rowBuf;     /* ai.row 数据行 (逗号衔接的 JSON 行, 完成后包成 [..] 回喂; 同锁同封顶) */
static bool g_rowCut = false;
static int g_rowDrop = 0;        /* 装不下的行数 (精确省略行数回喂模型) */
static std::string g_srchSet8;   /* g_agentRes 搜索设置 JSON 快照 (创建时取一次, 注入系统提示词; 经 g_emitCs 护) */

/* UI 编组在飞登记 (声明提前 — AgentToolInit/Shutdown 在此就触碰; 机制本体见下方"宿主扩展 API"节) */
static CRITICAL_SECTION s_uiCs;
static std::vector<AiUiJob*> s_inflight;

static void EmitAppendLine(const std::string& line) {
    EnterCriticalSection(&g_emitCs);
    if (g_emitBuf.size() + line.size() <= 16384) {
        g_emitBuf += line;
    } else if (g_emitCut) {
        g_emitDrop += line.size();   /* 已截断过: 整行计弃 (首行按整行算, 误差一次以内) */
    } else {
        g_emitCut = true;
        g_emitBuf += "\n... (ai.print 输出过长, 已截断)";
        g_emitDrop += line.size();
    }
    LeaveCriticalSection(&g_emitCs);
}

/* ai.print 的 C 实现: 多参数按 print 惯例以 \t 连接; 字符串/数字直取 (ToString 指针只在
 * 本次调用内有效, 立即拷贝), 表等其余值经 xjs_lua_ToJson 序列化, 都不行退化为 true/nil。 */
static int AgentLuaPrint(void* L) {
    std::string line;
    int n = xjs_lua_GetTop(L);
    for (int i = 1; i <= n; i++) {
        if (i > 1) line += '\t';
        const char* s = xjs_lua_ToString(L, i);
        if (s) { line += s; continue; }
        int need = xjs_lua_ToJson(L, i, NULL, 0);
        if (need > 1 && need <= 8192) {
            std::string buf((size_t)need, 0);
            if (xjs_lua_ToJson(L, i, &buf[0], need) > 0) {
                line += buf.c_str();   /* c_str 吃掉结尾 '\0' */
                continue;
            }
        }
        line += xjs_lua_ToBoolean(L, i) ? "true" : "nil";
    }
    line += '\n';
    EmitAppendLine(line);
    return 0;
}

/* ai.row 的 C 实现: 数据行通道 — 脚本压 id + 动态字段名, 名称/属性值由插件按 id 直取
 * (xjs_db_Get* 只读查询, 引擎执行脚本期间可重入读 — 与脚本内 f.fpath() 同一性质)。
 * 行 = 只含请求字段的 JSON 对象 ({"id":..,"大小":..}), 不固定列序不填 null —
 * 没请求的字段一个 token 都不花; 索引未开启的字段整键省略, 完成回吐时 rows 首元素提示一次。
 * 签名: ai.row(id, "字段名", ...) — 字段名 = 输出 JSON 的键, 一套名字两用 (名称/路径/大小/
 * 修改时间/创建时间/访问时间/扩展名/目录/类型/属性/别名/评分); 无字段实参 = id+名称 (最常用)。
 * 回调在引擎搜索/执行线程 — 与 ai.print 同一把 g_emitCs 小锁, 禁止碰 g_agentCs;
 * lua_filter 并发求值时行序不定 (数据行一律建议 lua_exec)。 */
enum { ROWF_MT, ROWF_CT, ROWF_AT, ROWF_SZ, ROWF_ATTR, ROWF_ALIAS, ROWF_RATING, ROWF_COUNT };
static bool g_rowMiss[ROWF_COUNT];   /* 本次运行被省略的未开启字段 (g_emitCs 护) */
static const char* const ROW_MISS_NAME[ROWF_COUNT] = {
    "修改时间", "创建时间", "访问时间", "大小", "属性", "别名", "评分",
};

static int AgentLuaRow(void* L) {
    int n = xjs_lua_GetTop(L);
    if (n < 1) return 0;
    long long id = xjs_lua_ToInteger(L, 1);
    if (id <= 0) return 0;
    xjs_engine* eng = xjs_GetDefaultEngine();
    if (!eng) return 0;
    picojson::object row;
    row["id"] = JN(id);
    bool miss[ROWF_COUNT] = { false };
    int fn = n >= 2 ? n - 1 : 1;   /* 字段实参个数; 无实参 = 缺省"名称" */
    for (int k = 0; k < fn; k++) {
        const char* f = n >= 2 ? xjs_lua_ToString(L, k + 2) : "名称";
        if (!f || !*f) continue;
        const std::string s(f);
        if (s == "名称") {
            const char* v = xjs_db_GetName(eng, (int)id);
            if (v) row["名称"] = JS(W8(v));
        } else if (s == "路径") {
            const char* v = xjs_db_GetPath(eng, (int)id);
            if (v) row["路径"] = JS(W8(v));
        } else if (s == "大小") {
            if (xjs_db_IsFieldEnabled(eng, "文件大小"))
                row["大小"] = JN(xjs_db_GetFileSize(eng, (int)id));
            else miss[ROWF_SZ] = true;
        } else if (s == "修改时间") {
            if (xjs_db_IsFieldEnabled(eng, "修改时间"))
                row["修改时间"] = JN(xjs_db_GetModifyTime(eng, (int)id) / 1000);
            else miss[ROWF_MT] = true;
        } else if (s == "创建时间") {
            if (xjs_db_IsFieldEnabled(eng, "创建时间"))
                row["创建时间"] = JN(xjs_db_GetCreateTime(eng, (int)id) / 1000);
            else miss[ROWF_CT] = true;
        } else if (s == "访问时间") {
            if (xjs_db_IsFieldEnabled(eng, "访问时间"))
                row["访问时间"] = JN(xjs_db_GetAccessTime(eng, (int)id) / 1000);
            else miss[ROWF_AT] = true;
        } else if (s == "扩展名") {
            const char* v = xjs_db_GetFileExt(eng, (int)id);
            if (v) row["扩展名"] = JS(W8(v));
        } else if (s == "目录") {
            row["目录"] = JB(xjs_db_IsDir(eng, (int)id) != 0);
        } else if (s == "类型") {
            const char* v = xjs_db_GetFileTypeStr(eng, (int)id);
            if (v) row["类型"] = JS(W8(v));
        } else if (s == "属性") {
            if (xjs_db_IsFieldEnabled(eng, "文件属性"))
                row["属性"] = JN(xjs_db_GetFileAttributes(eng, (int)id));
            else miss[ROWF_ATTR] = true;
        } else if (s == "别名") {
            if (xjs_db_IsFieldEnabled(eng, "别名")) {
                const char* v = xjs_db_GetAlias(eng, (int)id);
                if (v) row["别名"] = JS(W8(v));
            } else miss[ROWF_ALIAS] = true;
        } else if (s == "评分") {
            if (xjs_db_IsFieldEnabled(eng, "文件评分"))
                row["评分"] = JN(xjs_db_GetRating(eng, (int)id));
            else miss[ROWF_RATING] = true;
        }
        /* 未识别的字段名静默忽略 (有效名单在提示词/描述里) */
    }
    EnterCriticalSection(&g_emitCs);
    std::string row8 = picojson::value(row).serialize();
    if (g_rowBuf.size() + row8.size() <= 16384) {
        if (!g_rowBuf.empty()) g_rowBuf += ',';
        g_rowBuf += row8;
    } else {
        g_rowCut = true;
        g_rowDrop++;   /* 精确省略行数 (回喂时告知模型) */
    }
    for (int f = 0; f < ROWF_COUNT; f++) g_rowMiss[f] = g_rowMiss[f] || miss[f];
    LeaveCriticalSection(&g_emitCs);
    return 0;
}

/* ---- Lua 文件导出通道 (ai.read / ai.write / ai.saveas; 结果对象级注册, 2026-09-25) ----
 * 引擎 Lua 执行环境 (-4) 的看门狗总预算 10 秒且全程持引擎读锁 —— 回调里绝不能弹对话框/
 * 等用户。故 ai.write/ai.saveas 只做 **登记** (内存序列化 + 目标存在性快查, 零阻塞): 脚本
 * 结束后由 worker (AgentToolRunSearch 收尾处) 统一执行真正的写出 —— 文件权限闸、覆盖
 * 确认框、保存对话框全在 worker/UI 线程完成 (不受看门狗约束), 结果记进 st->wrote
 * (卡片常显块 + 随历史落库) 并并入工具结果回喂模型。
 * 格式规则 (用户口径): 二维表 → CSV (UTF-8 BOM, Excel 直开; 字典行取键并集做表头);
 * 其它表 → JSON 原文; 字符串/数字/布尔 → 原样文本。仅 lua_exec 可用 (lua_filter 每文件
 * 并发求值, 写文件无意义且登记乱序); 每脚本 ≤32 项, 单项 ≤32MB。
 * 回调在引擎线程 — 与 ai.print 同一把 g_emitCs, 禁止碰 g_agentCs。 */

struct AiWriteItem {         /* 一条待写出登记 (脚本运行时收集, 脚本结束后 worker 执行) */
    bool saveDlg = false;    /* true = ai.saveas: 写出前弹通用保存对话框 (用户选定即授权) */
    std::wstring path;       /* saveDlg=false: 目标绝对路径; true: 默认文件名建议 */
    std::string data;        /* 序列化好的字节 (CSV 含 BOM / JSON / 原样文本) */
    std::wstring ext;        /* 类型提示 (txt/csv/json; saveas 默认文件名兜底用) */
};
static std::vector<AiWriteItem> g_writeBuf;   /* g_emitCs 护; 发起前清、完成后取走 */
static std::string g_writeNote;               /* 回调层拒绝/失败的留痕 (g_emitCs; 空=无) —
                                                 保证任何路径下导出经过都进 writesNote 回喂模型,
                                                 模型不会在无凭据时幻觉宣称"已导出" */
static volatile LONG g_luaIoMode = 0;         /* worker 发起前写: 0=非 lua_exec (文件回调全拒) */
static volatile LONG g_luaIoPolicy = 0;       /* worker 发起前写: 文件权限快照 (0/1 回调层拒写) */
static void WriteNoteAppend(const wchar_t* why, const wchar_t* path) {
    EnterCriticalSection(&g_emitCs);
    g_writeNote += U8(std::wstring(why) + (path && *path ? (L": " + std::wstring(path)) : L"") + L"\n");
    LeaveCriticalSection(&g_emitCs);
}

/* Lua 值 → 写出字节 (用户口径: 二维表=CSV / 其它表=JSON / 字符串·数字·布尔=原样文本)。
 * 成功返回 true; err = 拒绝原因。表序列化经 xjs_lua_ToJson 原文保真, 二维表判定在
 * 解析树上做: 数组且元素全为数组/对象 → CSV (对象行键并集做表头; 数组行直接逐值)。 */
static bool CsvCsvEscape(const std::string& f, std::string* out) {   /* CSV 字段转义 (含分隔符/引号/换行才包引号) */
    bool q = f.find_first_of(",\"\r\n") != std::string::npos;
    if (!q) { *out += f; return true; }
    *out += '"';
    for (char c : f) { if (c == '"') *out += '"'; *out += c; }
    *out += '"';
    return true;
}
static std::string CsvCellOf(const picojson::value& v) {   /* 单元格值 → 文本 (null=空; 数字保真序列化) */
    if (v.is<std::string>()) return v.get<std::string>();
    if (v.is<bool>()) return v.get<bool>() ? "true" : "false";
    if (v.is<double>()) return picojson::value(v.get<double>()).serialize();
    return "";
}
static bool LuaValueToBytes(void* L, int idx, std::string* out, std::wstring* ext, std::wstring* err) {
    const char* s = xjs_lua_ToString(L, idx);
    if (s) {   /* 字符串/数字 → 原样文本 (ToString 对 number 给十进制文本) */
        *out = s;
        *ext = L"txt";
        return true;
    }
    int need = xjs_lua_ToJson(L, idx, NULL, 0);
    if (need <= 1 || need > 32 * 1024 * 1024) { *err = L"内容无法序列化或超过 32MB 上限"; return false; }
    std::string buf((size_t)need, 0);
    if (xjs_lua_ToJson(L, idx, &buf[0], need) <= 0) { *err = L"内容序列化失败"; return false; }
    buf.resize(strlen(buf.c_str()));
    if (buf == "null") { *err = L"没有要写出的内容 (nil)"; return false; }
    if (buf == "true" || buf == "false") {   /* 布尔 → 文本 */
        *out = buf;
        *ext = L"txt";
        return true;
    }
    /* 表: 解析判定二维 (数组且元素全为数组/对象) → CSV; 否则 JSON 原文 */
    picojson::value pv;
    if (JParseU8(pv, buf) && pv.is<picojson::array>()) {
        const picojson::array& rows = pv.get<picojson::array>();
        bool two = !rows.empty();
        for (auto& r : rows)
            if (!r.is<picojson::array>() && !r.is<picojson::object>()) { two = false; break; }
        if (two) {
            std::vector<std::string> headers;
            for (auto& r : rows)
                if (r.is<picojson::object>())
                    for (auto& kv : r.get<picojson::object>())
                        if (std::find(headers.begin(), headers.end(), kv.first) == headers.end())
                            headers.push_back(kv.first);   /* 首现序 (picojson::object=map 已按键序, 稳定) */
            out->clear();
            *out += "\xEF\xBB\xBF";   /* UTF-8 BOM: Excel 双击直开不乱码 */
            auto emitRow = [&](const std::vector<std::string>& cells) {
                for (size_t c = 0; c < cells.size(); c++) {
                    if (c) *out += ',';
                    CsvCsvEscape(cells[c], out);
                }
                *out += "\r\n";
            };
            if (!headers.empty()) emitRow(headers);
            for (auto& r : rows) {
                std::vector<std::string> cells;
                if (r.is<picojson::array>())
                    for (auto& cv : r.get<picojson::array>()) cells.push_back(CsvCellOf(cv));
                else {
                    auto& o = r.get<picojson::object>();
                    for (auto& h : headers) {
                        auto it = o.find(h);
                        cells.push_back(it != o.end() ? CsvCellOf(it->second) : "");
                    }
                }
                emitRow(cells);
            }
            *ext = L"csv";
            return true;
        }
    }
    *out = buf;   /* 非二维表 → JSON 原文 (字节保真, 不二次序列化) */
    *ext = L"json";
    return true;
}

/* 路径校验: 必须绝对路径 (盘符或 UNC), 禁文件名字符, 防误写相对路径落临时目录难找 */
static bool WritePathOk(const std::wstring& p) {
    if (p.size() < 3 || p.size() > 1024) return false;
    bool abs = (p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')) || (p.rfind(L"\\\\", 0) == 0);
    if (!abs) return false;
    for (wchar_t c : p)
        if (c < 0x20 || wcschr(L"<>|\"?*", c)) return false;
    return true;
}

/* ai.write/ai.saveas 的回执: 成功 = true; 失败 = nil + 原因 (脚本可判可提示) */
static int LuaWriteRet(void* L, bool ok, const wchar_t* why) {
    if (ok) { xjs_lua_PushBoolean(L, TRUE); return 1; }
    xjs_lua_PushNil(L);
    xjs_lua_PushString(L, U8(why).c_str());
    return 2;
}
static int AgentLuaWriteCommon(void* L, bool saveDlg) {
    int n = xjs_lua_GetTop(L);
    if (InterlockedCompareExchange(&g_luaIoMode, 0, 0) != 1) {
        WriteNoteAppend(L"ai.write/ai.saveas 被拒绝 (仅 lua_exec 可用)", NULL);
        return LuaWriteRet(L, false, L"ai.write/ai.saveas 仅在 Lua 执行模式 (lua_exec) 可用");
    }
    int pol = InterlockedCompareExchange(&g_luaIoPolicy, 0, 0);
    if (pol == 0) {
        WriteNoteAppend(L"写出被拒绝 (用户文件权限=禁用)", NULL);
        return LuaWriteRet(L, false, L"文件功能已被用户禁用 (文件操作权限=禁用)");
    }
    if (pol == 1) {
        WriteNoteAppend(L"写出被拒绝 (用户文件权限=只读)", NULL);
        return LuaWriteRet(L, false, L"当前文件操作权限为只读, 不能写出文件 (请用户把权限切到「询问」或「允许」)");
    }
    std::wstring path, ext;
    if (!saveDlg) {   /* ai.write(路径, 内容) */
        if (n < 2) {
            WriteNoteAppend(L"ai.write 缺少参数 (路径, 内容)", NULL);
            return LuaWriteRet(L, false, L"缺少参数: ai.write(路径, 内容)");
        }
        const char* p = xjs_lua_ToString(L, 1);
        if (!p || !*p) {
            WriteNoteAppend(L"ai.write 路径为空", NULL);
            return LuaWriteRet(L, false, L"路径为空");
        }
        path = W8(p);
        if (!WritePathOk(path)) {
            WriteNoteAppend(L"ai.write 路径非法 (须绝对路径)", path.c_str());
            return LuaWriteRet(L, false, L"路径必须是合法绝对路径 (如 D:\\数据\\结果.csv), 且不含 <>|\"?* 等字符");
        }
    }
    AiWriteItem it;
    it.saveDlg = saveDlg;
    it.path = path;
    std::wstring err2;
    if (!LuaValueToBytes(L, saveDlg ? 1 : 2, &it.data, &ext, &err2)) {
        WriteNoteAppend((L"ai." + std::wstring(saveDlg ? L"saveas" : L"write") + L" 内容序列化失败 — " + err2).c_str(), NULL);
        return LuaWriteRet(L, false, err2.c_str());
    }
    if (saveDlg) {   /* ai.saveas(内容[, "默认文件名.csv"]) */
        if (n >= 2) {
            const char* nm = xjs_lua_ToString(L, 2);
            if (nm && *nm) {
                path = W8(nm);
                if (path.size() <= 260 && path.find_first_of(L"\\/:*?\"<>|") == std::wstring::npos)
                    it.path = path;   /* 仅当像文件名才采纳 (带路径/非法字符 = 忽略建议) */
            }
        }
        if (it.path.empty()) it.path = L"AI导出." + ext;
        else if (it.path.rfind(L'.') == std::wstring::npos) it.path += L"." + ext;
    }
    EnterCriticalSection(&g_emitCs);
    bool full = g_writeBuf.size() >= 32;
    if (!full) g_writeBuf.push_back(std::move(it));
    LeaveCriticalSection(&g_emitCs);
    if (full) {
        WriteNoteAppend(L"写出登记过多 (>32 项), 后续调用被拒", NULL);
        return LuaWriteRet(L, false, L"本脚本登记的写出任务过多 (>32), 请合并内容一次写出");
    }
    return LuaWriteRet(L, true, NULL);   /* 已受理 — 实际写出在脚本结束后执行 (结果见工具结果) */
}
static int AgentLuaWrite(void* L) { return AgentLuaWriteCommon(L, false); }
static int AgentLuaSaveAs(void* L) { return AgentLuaWriteCommon(L, true); }

/* ai.read(路径) → 文本内容 (原样字节; 失败 = nil + 原因)。仅文本文件 —
 * 内容含 '\0' 直接拒绝 (SDK 栈辅助按 C 串取值, 静默截断比报错更害人); 上限 8MB。 */
static int AgentLuaRead(void* L) {
    if (InterlockedCompareExchange(&g_luaIoMode, 0, 0) != 1)
        return LuaWriteRet(L, false, L"ai.read 仅在 Lua 执行模式 (lua_exec) 可用");
    if (InterlockedCompareExchange(&g_luaIoPolicy, 0, 0) == 0)
        return LuaWriteRet(L, false, L"文件功能已被用户禁用 (文件操作权限=禁用)");
    if (xjs_lua_GetTop(L) < 1) return LuaWriteRet(L, false, L"缺少参数: ai.read(路径)");
    const char* p = xjs_lua_ToString(L, 1);
    if (!p || !*p) return LuaWriteRet(L, false, L"路径为空");
    std::wstring path = W8(p);
    if (!WritePathOk(path)) return LuaWriteRet(L, false, L"路径必须是合法绝对路径");
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        std::wstring e = GetLastError() == ERROR_FILE_NOT_FOUND ? L"文件不存在" : L"无法打开文件 (被占用或无权限)";
        return LuaWriteRet(L, false, e.c_str());
    }
    LARGE_INTEGER sz;
    std::string data;
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart <= 8 * 1024 * 1024;
    if (ok) {
        data.resize((size_t)sz.QuadPart);
        DWORD rd = 0;
        ok = sz.QuadPart == 0 || (ReadFile(h, &data[0], (DWORD)data.size(), &rd, NULL) && rd == (DWORD)sz.QuadPart);
        if (ok) data.resize(rd);
    }
    CloseHandle(h);
    if (!ok) return LuaWriteRet(L, false, L"文件超过 8MB 上限 (ai.read 仅适合文本)");
    if (data.find('\0') != std::string::npos)
        return LuaWriteRet(L, false, L"内容含二进制数据, ai.read 仅支持文本文件");
    xjs_lua_PushString(L, data.c_str());
    return 1;
}

void AgentToolInit() {
    InitializeCriticalSectionAndSpinCount(&g_agentCs, 100);
    InitializeCriticalSectionAndSpinCount(&g_srchErrCs, 100);
    InitializeCriticalSectionAndSpinCount(&g_emitCs, 100);
    InitializeCriticalSectionAndSpinCount(&s_uiCs, 100);
}
void AgentToolShutdown() {
    if (g_agentRes) { xjs_result_Destroy(g_agentRes); g_agentRes = NULL; }
    g_agentTopIds.clear();
    g_emitBuf.clear();   /* 到此工作线程已全部 join, 无并发 */
    g_emitCut = false;
    g_emitDrop = 0;
    g_rowBuf.clear();
    g_rowCut = false;
    g_rowDrop = 0;
    for (int f = 0; f < ROWF_COUNT; f++) g_rowMiss[f] = false;
    for (auto* jb : s_inflight) {   /* 投递后无人认领的编组作业 (消息窗口先没了) 代为回收 */
        if (jb->done) CloseHandle(jb->done);
        delete jb;
    }
    s_inflight.clear();
    DeleteCriticalSection(&s_uiCs);
    DeleteCriticalSection(&g_srchErrCs);
    DeleteCriticalSection(&g_emitCs);
    DeleteCriticalSection(&g_agentCs);
}

/* 占用 agent 串行锁: 多窗同时跑工具时在此排队, 轮询等待期间响应停止。
 * 返回 false = 等锁期间被停止 (未持锁)。 */
static bool AgentCsEnter(AiJob* j) {
    while (!TryEnterCriticalSection(&g_agentCs)) {
        if (InterlockedCompareExchange(&j->abort, 0, 0)) return false;
        Sleep(40);
    }
    return true;
}

/* ==================== 宿主扩展 API (QueryApi) + UI 编组 ====================
 * settings/window/modes/plugins/skins 这组扩展 API 全部仅 UI 线程 (宿主闸门查线程,
 * 错线程 = ERR_THREAD), 而 agent 工具执行在工作者线程 — 一律经 AiUiJob 编组到
 * g_msgwnd (UI 线程) 执行: worker 投递+轮询等完成 (abort 可退, 15 秒兜底), UI 侧
 * wndproc 调 AgentUiDispatch 分派。所有权仲裁走 s_uiCs: worker 放弃时登记在飞表,
 * UI 执行完发现 orphan 代为 delete — 两侧都不悬垂不双删。 */

HostApi g_api;

void ApiResolveAll() {   /* UI 线程 (Init); 旧宿主 = 全 NULL, 相关工具报"不支持" */
    g_api = HostApi{};
    if (!g_host || !HOST_QAPI_OK || !g_host->QueryApi) return;
    g_api.settingsGet  = (XjsApiSettingsGet)g_host->QueryApi(g_ctx, XJS_API_SETTINGS_GET);
    g_api.settingsSet  = (XjsApiSettingsSet)g_host->QueryApi(g_ctx, XJS_API_SETTINGS_SET);
    g_api.globalGet    = (XjsApiGlobalGet)g_host->QueryApi(g_ctx, XJS_API_GLOBAL_GET);
    g_api.globalSet    = (XjsApiGlobalSet)g_host->QueryApi(g_ctx, XJS_API_GLOBAL_SET);
    g_api.windowsEnum  = (XjsApiWindowsEnum)g_host->QueryApi(g_ctx, XJS_API_WINDOWS_ENUM);
    g_api.windowState  = (XjsApiWindowState)g_host->QueryApi(g_ctx, XJS_API_WINDOW_STATE);
    g_api.windowCmd    = (XjsApiWindowCmd)g_host->QueryApi(g_ctx, XJS_API_WINDOW_CMD);
    g_api.windowCreate = (XjsApiWindowCreate)g_host->QueryApi(g_ctx, XJS_API_WINDOW_CREATE);
    g_api.modesList    = (XjsApiModesList)g_host->QueryApi(g_ctx, XJS_API_MODES_LIST);
    g_api.modesAdd     = (XjsApiModesAdd)g_host->QueryApi(g_ctx, XJS_API_MODES_ADD);
    g_api.modesRemove  = (XjsApiModesRemove)g_host->QueryApi(g_ctx, XJS_API_MODES_REMOVE);
    g_api.modesApply   = (XjsApiModesApply)g_host->QueryApi(g_ctx, XJS_API_MODES_APPLY);
    g_api.pluginsList  = (XjsApiPluginsList)g_host->QueryApi(g_ctx, XJS_API_PLUGINS_LIST);
    g_api.pluginsState = (XjsApiPluginsState)g_host->QueryApi(g_ctx, XJS_API_PLUGINS_STATE);
    g_api.msgSend      = (XjsApiMsgSend)g_host->QueryApi(g_ctx, XJS_API_MSG_SEND);
    g_api.skinsList    = (XjsApiSkinsList)g_host->QueryApi(g_ctx, XJS_API_SKINS_LIST);
    g_api.windowSel    = (XjsApiWindowSelection)g_host->QueryApi(g_ctx, XJS_API_WINDOW_SELECTION);
    g_api.langsList    = (XjsApiLangsList)g_host->QueryApi(g_ctx, XJS_API_LANGS_LIST);
    g_api.windowResult = (XjsApiWindowResult)g_host->QueryApi(g_ctx, XJS_API_WINDOW_RESULT);
}

/* UIW_* 分派码 (AiUiJob::kind) */
enum {
    UIW_LIST_WINDOWS = 1, UIW_WINDOW_STATE, UIW_GET_GLOBAL,
    UIW_SET_SEARCH, UIW_LIST_MODES, UIW_APPLY_MODE,
    UIW_ADD_MODE, UIW_REMOVE_MODE, UIW_LIST_PLUGINS, UIW_PLUGIN_STATE, UIW_SEND_MSG,
    UIW_LIST_SKINS, UIW_OPEN_FILE, UIW_WINDOW_SELECTION, UIW_GET_LANGUAGE, UIW_LIST_LANGS,
    UIW_SAVE_DIALOG,   /* lua 写出: 通用保存对话框 (s1=标题 s2=默认文件名) → {"path":..} / {} =取消 */
    UIW_ASK_WRITE,     /* lua 写出: 覆盖/新文件确认框 (s1=正文) → {"确认":bool}; 15s 无响应=拒绝 */
};

static std::wstring ApiErrText(int rc) {
    switch (rc) {
        case XJS_PLUGIN_OK:         return L"";
        case XJS_PLUGIN_ERR_ARG:     return L"参数非法 (键名/取值不被接受)";
        case XJS_PLUGIN_ERR_PERM:    return L"权限未声明或插件已被禁用 (manifest 权限闸)";
        case XJS_PLUGIN_ERR_THREAD:  return L"线程契约错误";
        case XJS_PLUGIN_ERR_STATE:   return L"状态不允许 (引擎扫描中/目标不可用)";
        case XJS_PLUGIN_ERR_NOTFOUND: return L"目标不存在 (窗口已关/名单里没有)";
        case XJS_PLUGIN_ERR_IO:      return L"读写失败";
        case XJS_PLUGIN_ERR_FAIL:    return L"操作失败";
        default:                     return L"宿主返回错误码 " + std::to_wstring(rc);
    }
}

/* 调用方缓冲两段式取 JSON (fn = 输出约定同宿主: buf==NULL 报所需长度) */
template <class F> static void ApiCallOut(std::string* out, std::wstring* err, F fn) {
    int need = fn(NULL, 0);
    if (need < 0) { *err = ApiErrText(need); return; }
    std::string s((size_t)need + 1, 0);
    int n = need ? fn(&s[0], need + 1) : 0;
    if (n < 0) { *err = ApiErrText(n); return; }
    s.resize((size_t)(n > need ? need : n));
    *out = s;
}
/* 单次调用取 JSON (固定大缓冲) — 供"每次调用都执行一遍操作"的宿主 API (如 DialogJson
 * 弯模态对话框): 两段式会把它执行两遍 (第一遍探长度就把框弹了, 第二遍再弹一次)。 */
template <class F> static void ApiCallOutFixed(std::string* out, std::wstring* err, F fn) {
    std::string s(16 * 1024, 0);
    int n = fn(&s[0], (int)s.size());
    if (n < 0) { *err = ApiErrText(n); return; }
    if (n >= (int)s.size()) { *err = L"结果超出缓冲"; return; }
    s.resize((size_t)n);
    *out = s;
}
template <class F> static void ApiCallRc(std::wstring* err, F fn) {
    int rc = fn();
    if (rc != XJS_PLUGIN_OK) *err = ApiErrText(rc);
}

/* 窗口名称 → 令牌 (UI 线程; 名称空 = 默认令牌=对话所在窗)。查无 = *err 设描述, 返 0 */
static long long UiWindowToken(const std::wstring& name, long long defTok, std::wstring* err) {
    if (name.empty()) return defTok;
    if (!g_api.windowsEnum) { *err = L"当前宿主不支持该操作"; return 0; }
    std::string j8;
    ApiCallOut(&j8, err, [&](char* b, int c) { return g_api.windowsEnum(g_ctx, b, c); });
    if (!err->empty()) return 0;
    Jv v = JsonParseW(W8(j8.c_str()));
    for (auto& w : v.arr) {
        if (w.t == 5 && w.S(L"名称") == name) {
            const Jv* t = w.Get(L"令牌");
            if (t && t->t == 2) return (long long)t->num;
        }
    }
    *err = L"窗口不存在: " + name + L" (用 list_windows 查有效名称)";
    return 0;
}

/* window.state + settings.get 合并 (后者独有键: 失焦行为/显示类开关/任务栏图标/鼠标打开/默认选中) */
static void UiStateMerged(long long tok, std::string* out, std::wstring* err) {
    std::string st8, se8;
    ApiCallOut(&st8, err, [&](char* b, int c) { return g_api.windowState(g_ctx, (XjsWindowToken)tok, b, c); });
    if (!err->empty()) return;
    std::wstring e2;
    ApiCallOut(&se8, &e2, [&](char* b, int c) { return g_api.settingsGet(g_ctx, (XjsWindowToken)tok, b, c); });
    picojson::value va, vb;
    if (!JParseU8(va, st8) || !va.is<picojson::object>()) { *err = L"窗口状态解析失败"; return; }
    picojson::object m = va.get<picojson::object>();
    if (!e2.empty() && JParseU8(vb, se8) && vb.is<picojson::object>())
        for (auto& kv : vb.get<picojson::object>())
            if (!m.count(kv.first)) m[kv.first] = kv.second;   /* 后者只补前者没有的键 */
    *out = picojson::value(m).serialize();
}

/* UI 线程分派 (g_msgwnd wndproc 调; 全部宿主扩展 API/宿主表 UI 函数都在这条线上) */
static void UiDispatchRun(AiUiJob* jb) {
    const XjsWindowToken defTok = (XjsWindowToken)jb->tok;
    std::wstring errw;
    long long tok = 0;
    switch (jb->kind) {
        case UIW_LIST_WINDOWS:
            if (!g_api.windowsEnum) { jb->err = L"当前宿主不支持该操作"; return; }
            ApiCallOut(&jb->out8, &jb->err, [&](char* b, int c) { return g_api.windowsEnum(g_ctx, b, c); });
            return;
        case UIW_WINDOW_STATE:
            if (!g_api.windowState) { jb->err = L"当前宿主不支持该操作"; return; }
            tok = UiWindowToken(jb->s1, (long long)defTok, &errw);
            if (!errw.empty()) { jb->err = errw; return; }
            UiStateMerged(tok, &jb->out8, &jb->err);
            return;
        case UIW_GET_GLOBAL:
            if (!g_api.globalGet) { jb->err = L"当前宿主不支持该操作"; return; }
            ApiCallOut(&jb->out8, &jb->err, [&](char* b, int c) { return g_api.globalGet(g_ctx, b, c); });
            return;
        case UIW_SET_SEARCH:
            tok = UiWindowToken(jb->s1, (long long)defTok, &errw);
            if (!errw.empty()) { jb->err = errw; return; }
            if (!g_host) { jb->err = L"宿主不可用"; return; }
            ApiCallRc(&jb->err, [&] {
                return g_host->SearchSetText(g_ctx, (XjsWindowToken)tok,
                                             jb->s2.empty() ? NULL : U8(jb->s2).c_str(),
                                             jb->s3.empty() ? NULL : U8(jb->s3).c_str(), (int)jb->n1);
            });
            return;
        case UIW_LIST_MODES:
            if (!g_api.modesList) { jb->err = L"当前宿主不支持该操作"; return; }
            tok = UiWindowToken(jb->s1, (long long)defTok, &errw);
            if (!errw.empty()) { jb->err = errw; return; }
            ApiCallOut(&jb->out8, &jb->err, [&](char* b, int c) { return g_api.modesList(g_ctx, (XjsWindowToken)tok, b, c); });
            return;
        case UIW_APPLY_MODE:
            if (!g_api.modesApply) { jb->err = L"当前宿主不支持该操作"; return; }
            tok = UiWindowToken(jb->s1, (long long)defTok, &errw);
            if (!errw.empty()) { jb->err = errw; return; }
            ApiCallRc(&jb->err, [&] {
                return g_api.modesApply(g_ctx, (XjsWindowToken)tok, U8(jb->s2).c_str(),
                                        jb->s3.empty() ? NULL : U8(jb->s3).c_str());
            });
            return;
        case UIW_ADD_MODE: {
            /* modes.add 有副作用 (每次调用真加一个模式), 不能走两段式取长 — 单次调用大缓冲 */
            if (!g_api.modesAdd) { jb->err = L"当前宿主不支持该操作"; return; }
            std::string s(4096, 0);
            int n = g_api.modesAdd(g_ctx, 0, U8(jb->s1).c_str(), &s[0], (int)s.size() - 1);
            if (n < 0) { jb->err = ApiErrText(n); return; }
            s.resize((size_t)n);
            jb->out8 = s;
            return;
        }
        case UIW_REMOVE_MODE:
            if (!g_api.modesRemove) { jb->err = L"当前宿主不支持该操作"; return; }
            ApiCallRc(&jb->err, [&] { return g_api.modesRemove(g_ctx, U8(jb->s1).c_str()); });
            return;
        case UIW_LIST_PLUGINS:
            if (!g_api.pluginsList) { jb->err = L"当前宿主不支持该操作"; return; }
            ApiCallOut(&jb->out8, &jb->err, [&](char* b, int c) { return g_api.pluginsList(g_ctx, b, c); });
            return;
        case UIW_PLUGIN_STATE:
            if (!g_api.pluginsState) { jb->err = L"当前宿主不支持该操作"; return; }
            ApiCallOut(&jb->out8, &jb->err, [&](char* b, int c) { return g_api.pluginsState(g_ctx, U8(jb->s1).c_str(), b, c); });
            return;
        case UIW_SEND_MSG: {
            /* msg.send 有副作用 (每次真发一条消息), 不能两段式取长 — 单次调用 64KB 收回复 */
            if (!g_api.msgSend) { jb->err = L"当前宿主不支持该操作"; return; }
            std::string s(64 * 1024, 0);
            int n = g_api.msgSend(g_ctx, U8(jb->s1).c_str(), U8(jb->s2).c_str(), &s[0], (int)s.size() - 1);
            if (n < 0) { jb->err = ApiErrText(n); return; }
            s.resize((size_t)n);
            jb->out8 = s.empty() ? "{\"回复\":null}" : s;
            return;
        }
        case UIW_LIST_SKINS:
            if (!g_api.skinsList) { jb->err = L"当前宿主不支持该操作"; return; }
            ApiCallOut(&jb->out8, &jb->err, [&](char* b, int c) { return g_api.skinsList(g_ctx, b, c); });
            return;
        case UIW_OPEN_FILE:
            tok = UiWindowToken(jb->s1, (long long)defTok, &errw);
            if (!errw.empty()) { jb->err = errw; return; }
            if (!g_host) { jb->err = L"宿主不可用"; return; }
            ApiCallRc(&jb->err, [&] {
                return g_host->OpenFile(g_ctx, (XjsWindowToken)tok, (int)jb->n1, (int)jb->n2);
            });
            return;
        case UIW_WINDOW_SELECTION:
            if (!g_api.windowSel) { jb->err = L"当前宿主不支持该操作"; return; }
            tok = UiWindowToken(jb->s1, (long long)defTok, &errw);
            if (!errw.empty()) { jb->err = errw; return; }
            ApiCallOut(&jb->out8, &jb->err, [&](char* b, int c) {
                return g_api.windowSel(g_ctx, (XjsWindowToken)tok, (int)jb->n1, b, c);
            });
            return;
        case UIW_GET_LANGUAGE: {
            if (!g_api.settingsGet) { jb->err = L"当前宿主不支持该操作"; return; }
            tok = UiWindowToken(jb->s1, (long long)defTok, &errw);
            if (!errw.empty()) { jb->err = errw; return; }
            std::string se8;
            ApiCallOut(&se8, &jb->err, [&](char* b, int c) { return g_api.settingsGet(g_ctx, (XjsWindowToken)tok, b, c); });
            if (!jb->err.empty()) return;
            Jv o = JsonParseW(W8(se8.c_str()));
            std::wstring wname = o.S(L"窗口名称");
            std::wstring code = o.S(L"语言");
            if (code.empty()) { jb->err = L"宿主未返回语言设置 (宿主版本过旧?)"; return; }
            picojson::object o2;
            o2[U8(L"窗口名称")] = JS(wname);
            o2[U8(L"语言")] = JS(code);
            o2[U8(L"说明")] = JS(L"auto=跟随系统; 切换用 set_language");
            jb->out8 = picojson::value(o2).serialize();
            return;
        }
        case UIW_LIST_LANGS:
            if (!g_api.langsList) { jb->err = L"当前宿主不支持该操作"; return; }
            ApiCallOut(&jb->out8, &jb->err, [&](char* b, int c) { return g_api.langsList(g_ctx, b, c); });
            return;
        case UIW_SAVE_DIALOG: {   /* 通用保存对话框 (宿主 DialogJson kind=save, 原生风格) */
            if (!g_host || !g_host->DialogJson ||
                g_host->size < offsetof(XjsPluginHost, DialogJson) + sizeof(void*)) {
                jb->err = L"当前宿主不支持保存对话框"; return;
            }
            picojson::object opts;
            opts[U8(L"title")] = JS(jb->s1.empty() ? L"AI 助手 - 导出文件" : jb->s1);
            opts[U8(L"initialName")] = JS(jb->s2);
            picojson::array filter;
            picojson::array all;
            all.push_back(JS(L"所有文件"));
            all.push_back(JS(L"*.*"));
            filter.push_back(picojson::value(all));
            opts[U8(L"filter")] = picojson::value(filter);
            std::string o8 = picojson::value(opts).serialize();
            ApiCallOutFixed(&jb->out8, &jb->err, [&](char* b, int c) {
                return g_host->DialogJson(g_ctx, "save", o8.c_str(), b, c);
            });
            return;
        }
        case UIW_ASK_WRITE: {   /* 覆盖/新文件写出确认 (原生消息框; 顶层置顶保证可见) */
            int go = MessageBoxW(NULL, jb->s1.c_str(), L"AI 助手 - 文件写出确认",
                                 MB_YESNO | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND) == IDYES;
            picojson::object o;
            o[U8(L"确认")] = JB(go != 0);
            jb->out8 = picojson::value(o).serialize();
            return;
        }
        default:
            jb->err = L"未知编组作业";
            return;
    }
}

void AgentUiDispatch(AiUiJob* jb) {   /* UI 线程 (g_msgwnd XJS_AI_UIJOB 分派) */
    if (!jb) return;
    UiDispatchRun(jb);
    EnterCriticalSection(&s_uiCs);
    jb->doneSignaled = 1;
    bool orphan = jb->orphan != 0;
    if (orphan) {   /* worker 已放弃: 摘出在飞表并由本线程代删 (不再 SetEvent, 句柄已被关) */
        for (size_t i = 0; i < s_inflight.size(); i++)
            if (s_inflight[i] == jb) { s_inflight.erase(s_inflight.begin() + i); break; }
    }
    LeaveCriticalSection(&s_uiCs);
    if (orphan) delete jb;
    else SetEvent(jb->done);
}

std::wstring AgentUiCall(AiJob* j, AiUiJob* jb, std::string* out8) {
    /* jb 所有权归本函数: 正常路径此处 delete; 放弃路径登记在飞由 UI 代删 */
    if (!g_msgwnd) { delete jb; return L"消息窗口未就绪 (面板已关闭?)"; }
    jb->done = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!jb->done) { delete jb; return L"同步对象创建失败"; }
    if (!PostMessageW(g_msgwnd, XJS_AI_UIJOB, 0, (LPARAM)jb)) {
        CloseHandle(jb->done);
        delete jb;
        return L"界面任务投递失败";
    }
    ULONGLONG t0 = GetTickCount64();
    ULONGLONG waitLimit = jb->waitMs > 0 ? (ULONGLONG)jb->waitMs : 15000;
    for (;;) {
        if (WaitForSingleObject(jb->done, 40) == WAIT_OBJECT_0) break;
        bool stop = InterlockedCompareExchange(&j->abort, 0, 0) != 0;
        bool slow = GetTickCount64() - t0 > waitLimit;   /* 界面线程被模态/长活卡住的兜底 */
        if (!stop && !slow) continue;
        bool doneByUi = false;
        EnterCriticalSection(&s_uiCs);   /* 清理权仲裁: UI 已完成 = 走正常收尾 */
        if (jb->doneSignaled) doneByUi = true;
        else { jb->orphan = 1; s_inflight.push_back(jb); }
        LeaveCriticalSection(&s_uiCs);
        if (doneByUi) break;
        CloseHandle(jb->done);
        return stop ? L"已停止" : L"界面操作超时 (主界面 15 秒未响应)";
    }
    *out8 = jb->out8;
    std::wstring err = jb->err;
    CloseHandle(jb->done);
    delete jb;
    return err;
}

long long AgentUiWindowToken(const std::wstring& name, long long defTok, std::wstring* err) {
    return UiWindowToken(name, defTok, err);   /* 仅 UI 线程 (内部调 windows.enum 扩展 API) */
}

std::wstring AgentApiErrText(int rc) { return ApiErrText(rc); }

/* 调整项展示值: 键+JSON 值 → 用户视角文案 (未识别的键原样透出; 与工具 description 同一套取值) */
static std::wstring AdjustValueText(const std::wstring& key, const Jv& v) {
    if (v.t == 1) return v.b ? L"开" : L"关";
    if (v.t == 2) {
        wchar_t b[40];
        if (key == L"页面缩放") { swprintf(b, 40, L"%.0f%%", v.num); return b; }
        if (key == L"失焦行为") return v.num == 0 ? L"无操作" : L"失焦时关闭窗口";
        if (key == L"鼠标打开") return v.num == 0 ? L"双击打开" : L"单击打开";
        if (key == L"默认选中") return v.num == 0 ? L"不自动选" : L"自动选第一项";
        swprintf(b, 40, L"%.0f", v.num);
        return b;
    }
    std::wstring s = v.t == 3 ? v.str : L"";
    if (key == L"视图") {
        if (s == L"list") return L"列表";
        if (s == L"details") return L"详情";
        if (s == L"medium") return L"中图标";
        if (s == L"large") return L"大图标";
    } else if (key == L"语言") {
        if (s == L"auto") return L"跟随系统";
        if (s == L"zh") return L"简体中文";
        if (s == L"zh-TW") return L"繁體中文";
        if (s == L"en") return L"English";
        if (s == L"ko") return L"한국어";
        if (s == L"th") return L"ไทย";
        if (s == L"ms") return L"Bahasa Melayu";
    } else if (key == L"绘制引擎") {
        if (s == L"d2d") return L"标准模式";
        if (s == L"gdiplus") return L"兼容模式 (重启生效)";
    }
    return s;
}

/* 把代办改动列成"待应用的调整"卡片 (不直接执行 — 用户在卡上逐项 应用/忽略,
 * 应用动作发生在用户点击后的 UI 线程, 见 ai_web.cpp WebCommand "adj")。
 * worker 只负责挂提案 + 给模型回执; 返回错误描述 (空=成功)。 */
static std::wstring ProposeAdjustCard(AiToolStep* st, int kind, const std::wstring& win,
                                      std::vector<AiAdjustItem> items) {
    if (items.empty()) return L"没有可提交的调整项";
    if (items.size() > 16) return L"一次提交的调整项过多 (≤16 项)";
    st->adj.kind = kind;
    st->adj.win = win;
    st->adj.items = std::move(items);
    std::wstring list;
    for (auto& it : st->adj.items) {
        if (!list.empty()) list += L"、";
        list += it.key + L" → " + it.val;
    }
    st->argz = L"待应用: " + list;
    if (st->argz.size() > 200) { st->argz.resize(200); st->argz += L"…"; }   /* 卡头截 200 (同 ArgzCut, 定义在其后故内联) */
    picojson::object o;
    o[U8(L"状态")] = JS(L"待用户应用");
    picojson::array items8;
    for (auto& it : st->adj.items) items8.push_back(JS(it.key + L" → " + it.val));
    o[U8(L"条目")] = picojson::value(items8);
    o[U8(L"说明")] = JS(L"改动已列在\"待应用的调整\"卡片, 用户点\"应用\"才会生效;"
                        L"用一句话请用户到卡片上确认 (可逐项应用或忽略), 绝不宣称已生效, 不要重复提交相同调整");
    st->res8 = picojson::value(o).serialize();
    return L"";
}

/* 代办类工具统一执行: 建 UI 作业 → 编组执行 → 结果/错误进 step。返回错误描述 (空=成功)。 */
static std::wstring AgentToolUi(AiJob* j, int uiKind, XjsWindowToken defTok,
                                const std::wstring& s1, const std::wstring& s2,
                                const std::wstring& s3, long long n1, long long n2,
                                AiToolStep* st) {
    AiUiJob* jb = new AiUiJob();
    jb->kind = uiKind;
    jb->tok = (long long)defTok;
    jb->s1 = s1;
    jb->s2 = s2;
    jb->s3 = s3;
    jb->n1 = n1;
    jb->n2 = n2;
    std::string out8;
    std::wstring err = AgentUiCall(j, jb, &out8);
    st->res8 = out8;
    return err;
}

/* ---- 临时调试观察口 (2026-09-25 用户口径): Lua 脚本执行前落盘 data\待运行.lua, 会话作业结束才删 ----
 * agent 工作线程在 Query 提交前经宿主 StorageSet 写入 (免权限/任意线程, 一键一文件,
 * 落 plugins\ai-assistant\data\待运行.lua), **每次调用覆盖** — 文件内容恒为最近一次
 * 提交的脚本; 作业守卫 (JobLuaDumpCleaner, 挂 WorkerMain 顶部) 析构时 StorageRemove 删除 —
 * 停止/失败/截断/轮数耗尽/正常完成 全部退出路径都删 (RAII 无漏)。
 * 作业进行中文件常驻, 观察者随时打开都能看到模型最近一次执行的脚本。 */
struct JobLuaDumpCleaner {
    JobLuaDumpCleaner() { g_host->StorageRemove(g_ctx, "待运行.lua"); }   /* 清上次进程中途被杀的残留 */
    ~JobLuaDumpCleaner() { g_host->StorageRemove(g_ctx, "待运行.lua"); }
};

/* ---- lua_exec 顶层 return 强制校验 (2026-09-25 用户口径: 强制写 return 数组) ----
 * 执行模式脚本 return 的 ID 数组 = 最终结果集 (res 表只读, 没有第二条产出通道); 缺 return
 * 引擎静默按 0 条处理, 与"真没搜到"不可区分, 外部(结果对象/工具卡)无从得知脚本选中了哪些。
 * 故提交前静态扫描, 缺顶层 return = 拒绝执行, 指示性错误喂回模型令其补写 (统计类任务同样
 * 要求把涉及的 ID 数组 return 回来, 数字本身走 ai.print)。
 * 扫描器只认结构: 跳过 行注释/--[=*[ 长注释 与 '…'/[[=*[ 长短字符串 后, 按块关键字配对计
 * 深度 —— function/if/for/while/do/repeat 各开一层, end/until 各闭一层; for/while 头部的
 * do 归构造本身不另计 (记下开层深度, 深度回落到该值的首个 do 才消费); 深度 0 处见 return
 * 即判定通过 —— 位置在脚本中段(提前返回)同样算数, 包在 if/function 里的 return 不算。 */
static bool LuaHasTopLevelReturn(const char* s) {
    auto IsWord = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    };
    auto IsKw = [](const char* w, int n, const char* kw) {
        while (*kw) { if (n-- <= 0 || *w++ != *kw++) return false; }
        return n == 0;
    };
    const char* p = s;
    int depth = 0;
    int pendingDoAt = 0;
    bool pendingDo = false;
    while (*p) {
        char c = *p;
        if (c == '-' && p[1] == '-') {                    /* 注释: 行注释 或 --[=*[ 长注释 */
            p += 2;
            int eq = 0;
            if (p[0] == '[') {
                const char* q = p + 1;
                while (*q == '=') { eq++; q++; }
                if (*q == '[') {                          /* 长注释, 找同级 ]=*=] */
                    p = q + 1;
                    while (*p) {
                        if (p[0] == ']') {
                            int e2 = 0;
                            while (p[1 + e2] == '=') e2++;
                            if (e2 == eq && p[1 + e2] == ']') { p += e2 + 2; break; }
                        }
                        p++;
                    }
                    if (!*p) break;
                    continue;
                }
            }
            while (*p && *p != '\n') p++;
            continue;
        }
        if (c == '\"' || c == '\'') {                     /* 短字符串: 找源码级收尾引号 */
            p++;
            while (*p && *p != c) {
                if (*p == '\\' && p[1]) p++;
                p++;
            }
            if (*p) p++;
            continue;
        }
        if (c == '[') {                                   /* 可能是长字符串 [[..]] / [=*[..]=*] */
            int eq = 0;
            const char* q = p + 1;
            while (*q == '=') { eq++; q++; }
            if (*q == '[') {
                p = q + 1;
                while (*p) {
                    if (p[0] == ']') {
                        int e2 = 0;
                        while (p[1 + e2] == '=') e2++;
                        if (e2 == eq && p[1 + e2] == ']') { p += e2 + 2; break; }
                    }
                    p++;
                }
                if (!*p) break;
                continue;
            }
            p++;
            continue;
        }
        if (IsWord(c)) {                                  /* 标识符/关键字 */
            const char* w = p;
            while (IsWord(*p)) p++;
            int n = (int)(p - w);
            if (IsKw(w, n, "return") && depth == 0) return true;
            if (IsKw(w, n, "function") || IsKw(w, n, "if") || IsKw(w, n, "for") ||
                IsKw(w, n, "while") || IsKw(w, n, "repeat")) {
                depth++;
                if (IsKw(w, n, "for") || IsKw(w, n, "while")) { pendingDo = true; pendingDoAt = depth; }
            } else if (IsKw(w, n, "do")) {
                if (pendingDo && depth == pendingDoAt) pendingDo = false;
                else depth++;
            } else if (IsKw(w, n, "end") || IsKw(w, n, "until")) {
                if (depth > 0) depth--;
            }
            continue;
        }
        p++;                                              /* 空白/标点/UTF-8 高位字节逐个跳过 */
    }
    return false;
}

/* pathcheck 的图标供给 (ai_web.cpp 在 UI 线程调): TryEnter g_agentCs — agent 正在跑工具就放弃
 * (本轮无图标, 之后的校验轮再试; 绝不等锁 — UI 线程阻塞等长查询 = 卡死, 返工级)。
 * 拿到锁才懒建私有结果对象 (与 run_search 同一门槛: 库加载/扫描期不建), itemIndex=-1 同步模式
 * 直接取真实图标 PNG (引擎自带图标缓存; 返回指针线程本地, 调用方必须立即拷贝)。 */
const void* AgentFetchFileIco(int fileId, int* outLen) {
    *outLen = 0;
    if (fileId <= 0) return NULL;
    if (!TryEnterCriticalSection(&g_agentCs)) return NULL;
    const void* png = NULL;
    xjs_engine* eng = xjs_GetDefaultEngine();
    if (eng) {
        if (g_agentRes && !xjs_result_IsEffective(g_agentRes)) g_agentRes = NULL;
        if (!g_agentRes) {
            int dbState = xjs_db_GetEngineState(eng);
            if (dbState != XJS_DB_STATE_LOADING && dbState != XJS_DB_STATE_SCANNING)
                g_agentRes = xjs_result_Create(eng);
        }
        if (g_agentRes)
            png = xjs_result_GetFileIco(g_agentRes, fileId, -1, 16, outLen, NULL);
    }
    LeaveCriticalSection(&g_agentCs);
    return png;
}

/* 懒建私有结果对象 (AgentToolRunSearch / AgentManualExec 共用; 调用方持 g_agentCs):
 * 引擎忙态 (载库/扫描) 拒建/拒用 — 忙时创建会被永久定成文件名序 (2026-09-15 实锤)。
 * AgentFetchFileIco 可能先建了裸对象, 这里补齐回调/Lua 注册 (SetCallback 幂等覆盖)。 */
static bool g_agentResWired = false;
static xjs_result* AgentEnsureResult() {
    xjs_engine* eng = xjs_GetDefaultEngine();
    if (!eng) return NULL;
    if (g_agentRes && !xjs_result_IsEffective(g_agentRes)) { g_agentRes = NULL; g_agentResWired = false; }
    if (g_agentRes && g_agentResWired) return g_agentRes;
    int dbState = xjs_db_GetEngineState(eng);
    if (dbState == XJS_DB_STATE_LOADING || dbState == XJS_DB_STATE_SCANNING) return NULL;
    if (!g_agentRes) {
        g_agentRes = xjs_result_Create(eng);
        if (!g_agentRes) return NULL;
    }
    xjs_result_SetCallback(g_agentRes, XJS_RESULT_EVENT_FAILED,   (const void*)AgentOnSearchFailed, NULL);
    xjs_result_SetCallback(g_agentRes, XJS_RESULT_EVENT_COMPLETE, (const void*)AgentOnSearchComplete, NULL);
    xjs_result_LuaRegisterFunction(g_agentRes, "ai", "print", AgentLuaPrint);
    xjs_result_LuaRegisterFunction(g_agentRes, "ai", "row", AgentLuaRow);
    xjs_result_LuaRegisterFunction(g_agentRes, "ai", "read", AgentLuaRead);
    xjs_result_LuaRegisterFunction(g_agentRes, "ai", "write", AgentLuaWrite);
    xjs_result_LuaRegisterFunction(g_agentRes, "ai", "saveas", AgentLuaSaveAs);
    const char* ss = xjs_result_GetSearchSettings(g_agentRes);
    EnterCriticalSection(&g_emitCs);
    g_srchSet8 = ss ? ss : "";
    LeaveCriticalSection(&g_emitCs);
    g_agentResWired = true;
    return g_agentRes;
}

xjs_result* AgentWindowResultOf(XjsWindowToken tok) {   /* 仅 UI 线程 (扩展 API 线程契约) */
    if (!g_api.windowResult) return NULL;               /* 旧宿主: 无此名字, 结果同步不可用 */
    return g_api.windowResult(g_ctx, tok);
}

/* 手动执行卡片语句 (卡片右键"执行语句"; UI 线程): 布防结果同步后, 私有对象上异步
 * 重放该语句 — 只占串行锁完成提交, 等完成/推送全在引擎完成事件回调里, 不占 UI 线程。
 * 返回错误描述 (空 = 已提交)。 */
std::wstring AgentManualExec(XjsWindowToken tok, const std::wstring& mode, const std::wstring& query) {
    if (mode.empty() || query.empty()) return L"语句为空";
    if (mode == L"lua_exec" && !LuaHasTopLevelReturn(U8(query).c_str()))
        return L"lua_exec 脚本缺少顶层 return (执行模式必须以顶层 return ID 数组结尾), 未提交引擎";
    int type;
    if (mode == L"wildcard")      type = 0;
    else if (mode == L"regex")    type = 1;
    else if (mode == L"sql")      type = 2;
    else if (mode == L"lua_filter") type = XJS_KEYWORD_LUA;
    else if (mode == L"lua_exec") type = XJS_KEYWORD_LUA_EXEC;
    else return L"未知搜索模式: " + mode;
    /* 同步目标先捕获 (窗已关 = NULL, 只执行不同步); agent 忙 = 拒绝 (工具循环正在跑) */
    xjs_result* win = AgentWindowResultOf(tok);
    if (!win) return L"目标窗口结果不可用 (窗口已关或宿主不支持结果同步)";
    if (!TryEnterCriticalSection(&g_agentCs)) return L"AI 正在执行工具, 请稍后再试";
    std::wstring err;
    if (!AgentEnsureResult()) err = L"索引未就绪 (正在加载数据库或建立索引), 请稍后重试";
    if (err.empty()) {
        bool luaMode = (type == XJS_KEYWORD_LUA || type == XJS_KEYWORD_LUA_EXEC);
        if (luaMode) {   /* 清缓冲: Query 返回后 VM 可能立刻开跑写 ai.print/登记导出 (同 run_search 口径) */
            EnterCriticalSection(&g_emitCs);
            g_emitBuf.clear(); g_emitCut = false; g_emitDrop = 0;
            g_rowBuf.clear(); g_rowCut = false; g_rowDrop = 0;
            g_writeBuf.clear(); g_writeNote.clear();
            for (int f = 0; f < ROWF_COUNT; f++) g_rowMiss[f] = false;
            LeaveCriticalSection(&g_emitCs);
            InterlockedExchange(&g_luaIoMode, type == XJS_KEYWORD_LUA_EXEC ? 1 : 0);
            InterlockedExchange(&g_luaIoPolicy, g_cfg.filePolicy);
        }
        InterlockedExchangePointer((volatile PVOID*)&g_syncWin, win);
        InterlockedExchange(&g_syncArm, 1);   /* 布防: 完成事件回调执行推送 */
        std::string q8 = U8(query);
        if (xjs_result_Query(g_agentRes, q8.c_str(), type, FALSE) < 0) {
            InterlockedExchange(&g_syncArm, 0);   /* 撤防 (发起失败无完成事件) */
            std::wstring e = W8(xjs_GetLastErrorMsg(xjs_GetDefaultEngine()));
            err = L"搜索发起失败: " + (e.empty() ? std::wstring(L"引擎拒绝") : e);
        }
    }
    LeaveCriticalSection(&g_agentCs);
    return err;
}

/* run_search 实体; 返回空串 = 成功, 否则 = 错误描述 (调用方持 g_agentCs) */
static std::string ExecuteLuaWrites(AiJob* j, AiToolStep* st);   /* lua 导出登记的写盘执行段 (定义在本函数后) */
static std::wstring AgentToolRunSearch(AiJob* j, const std::wstring& mode, const std::wstring& query, AiToolStep* st) {
    if (mode == L"lua_exec" && !LuaHasTopLevelReturn(U8(query).c_str()))
        return L"lua_exec 脚本缺少顶层 return, 已拒绝执行 (未提交引擎) — 执行模式必须以顶层 return ID 数组结尾, "
               L"return 的数组 = 最终结果集, 结果卡/外部靠它得知脚本选中了哪些文件; 缺了引擎只会静默给 0 条, 与真没搜到无法区分。"
               L"请在脚本结尾补上 return (如 return ids 或 return {...}) 后重新调用; "
               L"统计类任务同样把涉及/选中的文件 ID 数组 return 回来, 数字本身继续走 ai.print。";
    xjs_engine* eng = xjs_GetDefaultEngine();
    if (!AgentEnsureResult())
        return L"索引未就绪 (正在加载数据库或建立索引), 请稍后重试";
    std::string q8 = U8(query);
    EnterCriticalSection(&g_srchErrCs);
    g_srchErr.clear();
    g_srchErrFp = -1;
    LeaveCriticalSection(&g_srchErrCs);
    bool luaMode = (mode == L"lua_filter" || mode == L"lua_exec");
    if (luaMode) {   /* 清缓冲必须在发起前: Query 返回后 VM 可能立刻开跑并 ai.print/ai.row */
        EnterCriticalSection(&g_emitCs);
        g_emitBuf.clear();
        g_emitCut = false;
        g_emitDrop = 0;
        g_rowBuf.clear();
        g_rowCut = false;
        g_rowDrop = 0;
        g_writeBuf.clear();   /* 文件导出登记一并清 (上一作业残留不得执行) */
        g_writeNote.clear();
        for (int f = 0; f < ROWF_COUNT; f++) g_rowMiss[f] = false;
        LeaveCriticalSection(&g_emitCs);
        /* 文件回调环境快照: 仅 lua_exec 放行 + 文件权限闸 (回调层拦截, 引擎线程不碰 g_cfg) */
        InterlockedExchange(&g_luaIoMode, mode == L"lua_exec" ? 1 : 0);
        InterlockedExchange(&g_luaIoPolicy, InterlockedCompareExchange(&j->policy, 0, 0));
        /* 临时调试观察口: 先落盘再提交 (VM 可能 Query 返回即开跑) */
        g_host->StorageSet(g_ctx, "待运行.lua", q8.c_str(), (int)q8.size());
    }
    /* 结果同步布防 (2026-09-26 用户口径: 完成事件里推送, 不经 UI 编组/不另起线程):
     * 勾选开启且目标窗结果可得 → 置 g_syncArm, AgentOnSearchComplete (引擎搜索线程)
     * 在本次完成未被覆盖 (discarded=FALSE) 时把 ID 全集 ResetFileId 进目标窗。 */
    if (InterlockedCompareExchange(&j->syncRes, 0, 0) && j->syncWin) {
        InterlockedExchangePointer((volatile PVOID*)&g_syncWin, j->syncWin);
        InterlockedExchange(&g_syncArm, 1);
    } else {
        InterlockedExchange(&g_syncArm, 0);
    }
    if (mode != L"wildcard" && mode != L"regex" && mode != L"sql" &&
        mode != L"lua_filter" && mode != L"lua_exec")
        return L"未知搜索模式: " + mode;
    /* 结果同步布防 (2026-09-26 用户口径: 完成事件里推送, 不经 UI 编组/不另起线程):
     * 勾选开启且目标窗结果可得 → 置 g_syncArm, AgentOnSearchComplete (引擎搜索线程)
     * 在本次完成未被覆盖 (discarded=FALSE) 时把 ID 全集 ResetFileId 进目标窗。 */
    if (InterlockedCompareExchange(&j->syncRes, 0, 0) && j->syncWin) {
        InterlockedExchangePointer((volatile PVOID*)&g_syncWin, j->syncWin);
        InterlockedExchange(&g_syncArm, 1);
    } else {
        InterlockedExchange(&g_syncArm, 0);
    }
    int fp = -1;
    if (mode == L"wildcard")      fp = xjs_result_Query(g_agentRes, q8.c_str(), 0, FALSE);
    else if (mode == L"regex")    fp = xjs_result_Query(g_agentRes, q8.c_str(), 1, FALSE);
    else if (mode == L"sql")      fp = xjs_result_Query(g_agentRes, q8.c_str(), 2, FALSE);
    else if (mode == L"lua_filter") fp = xjs_result_Query(g_agentRes, q8.c_str(), XJS_KEYWORD_LUA, FALSE);
    else if (mode == L"lua_exec") fp = xjs_result_Query(g_agentRes, q8.c_str(), XJS_KEYWORD_LUA_EXEC, FALSE);
    if (fp < 0) {
        InterlockedExchange(&g_syncArm, 0);   /* 发起失败无完成事件, 撤防 (残留会推错下一次结果) */
        std::wstring e = W8(xjs_GetLastErrorMsg(eng));
        return L"搜索发起失败: " + (e.empty() ? std::wstring(L"引擎拒绝") : e);
    }
    /* 轮询等完成 (阻塞式 waitComplete=TRUE 会把"停止"冻死在引擎调用里):
     * abort → CancelSearch 在途搜索即停; 120 秒兜底防引擎侧万一永不完成;
     * IsCompleted 对"指纹被覆盖/从未开始"也返回 TRUE, 串行使用下不会误判。 */
    ULONGLONG t0 = GetTickCount64();
    for (;;) {
        if (InterlockedCompareExchange(&j->abort, 0, 0)) {
            if (xjs_result_IsEffective(g_agentRes)) xjs_result_CancelSearch(g_agentRes, fp, FALSE);
            return L"已停止";
        }
        if (xjs_result_IsCompleted(g_agentRes, fp)) break;
        if (!xjs_result_IsEffective(g_agentRes)) return L"结果对象已失效";
        if (GetTickCount64() - t0 > 120000) {
            if (xjs_result_IsEffective(g_agentRes)) xjs_result_CancelSearch(g_agentRes, fp, FALSE);
            return L"搜索超时 (120 秒未完成, 已取消) — 请缩小范围 (加目录前缀/限定字段) 重试";
        }
        Sleep(80);
    }
    EnterCriticalSection(&g_srchErrCs);
    bool failed = g_srchErrFp == fp && !g_srchErr.empty();
    std::wstring serr = g_srchErr;
    g_srchErr.clear();
    g_srchErrFp = -1;
    LeaveCriticalSection(&g_srchErrCs);
    if (failed) {   /* SQL/正则等编译失败: 抽"错误信息"喂回模型便于自行纠正 */
        Jv v = JsonParseW(serr);
        std::wstring msg = v.S(L"错误信息");
        if (msg.empty()) msg = v.S(L"错误类型");
        return msg.empty() ? (L"搜索失败: " + serr) : (L"搜索失败: " + msg);
    }
    st->count = xjs_result_GetCount(g_agentRes);
    st->elapsedMs = xjs_result_GetElapsed(g_agentRes);
    std::string emit8, rowsRaw;   /* lua 过程输出原文 (UTF-8) / 数据行 (插件自拼合法 JSON 数组) */
    if (luaMode) {   /* 取走本次 ai.print 过程/统计输出 + ai.row 数据行 (整段并入工具结果) */
        EnterCriticalSection(&g_emitCs);
        emit8 = g_emitBuf;
        g_emitBuf.clear();
        g_emitCut = false;
        size_t emitDrop = g_emitDrop;   /* 精确省略量随缓冲一并取走 */
        g_emitDrop = 0;
        int rowDrop = g_rowDrop;
        std::string missNote;
        for (int f = 0; f < ROWF_COUNT; f++) {
            if (!g_rowMiss[f]) continue;
            if (!missNote.empty()) missNote += "、";
            missNote += ROW_MISS_NAME[f];
        }
        if (!g_rowBuf.empty() || g_rowCut || !missNote.empty()) {
            std::string wrapped = "[";
            if (!missNote.empty()) wrapped += "\"(索引未开启字段, 已省略: " + missNote + ")\",";
            wrapped += g_rowBuf;
            if (g_rowCut) {
                if (!g_rowBuf.empty()) wrapped += ',';
                wrapped += "\"(行数过多, 后续已截断";
                if (rowDrop > 0) wrapped += ", 丢弃 " + std::to_string(rowDrop) + " 行";
                wrapped += ")\"";
            }
            wrapped += "]";
            rowsRaw = wrapped;   /* 合法 JSON, 拼装时直接并入不再转义 */
        }
        for (int f = 0; f < ROWF_COUNT; f++) g_rowMiss[f] = false;
        g_rowBuf.clear();
        g_rowCut = false;
        g_rowDrop = 0;
        LeaveCriticalSection(&g_emitCs);
        if (emitDrop > 0) {   /* 截断附精确丢弃量 (模型可自行决定缩小输出重跑) */
            emit8 += "\n(输出过长被截断, 丢弃约 " + std::to_string(emitDrop) + " 字节; "
                     "需要时把统计拆小或分批输出重跑)";
        }
    }
    /* 样本 TOP 20: 完整路径进 st->top (卡片展开显示/历史落库用); 喂模型的 JSON 现场拼进
     * res8 — 模型只拿 [FileId,文件名], **不拿路径** (回答里的文件链接只写 ID, 打开/定位/
     * 复制路径由程序按 ID 解析; GetPath/GetName 指针为线程本地缓存, 必须立即拷贝) */
    g_agentTopIds.clear();
    st->top.clear();
    picojson::array files;
    int n = st->count < 20 ? st->count : 20;
    for (int i = 0; i < n; i++) {
        int fid = xjs_result_GetFileId(g_agentRes, i);
        if (fid < 0) continue;
        g_agentTopIds.push_back(fid);
        const char* p = xjs_db_GetPath(eng, fid);
        st->top.push_back(p ? W8(p) : L"(路径不可用)");
        const char* nm = xjs_db_GetName(eng, fid);
        picojson::array pair;
        pair.push_back(JN(fid));
        pair.push_back(JS(nm && *nm ? W8(nm) : L"(未命名)"));
        files.push_back(picojson::value(pair));
    }
    /* 脚本登记的文件导出在此执行 (脚本已结束, 确认/对话框不受看门狗约束; abort 经 AgentUiCall 退出) */
    std::string wroteNote8 = mode == L"lua_exec" ? ExecuteLuaWrites(j, st) : std::string();
    picojson::object feed;
    feed["count"] = JN(st->count);
    feed["elapsedMs"] = JN(st->elapsedMs);
    feed["files"] = picojson::value(files);
    if (!emit8.empty()) {   /* lua 脚本 ai.print 的过程/统计输出 */
        feed["output"] = JS(W8(emit8.c_str()));
    }
    if (!rowsRaw.empty()) {   /* lua 脚本 ai.row 数据行 (拼装时已是合法 JSON 数组文本, 原样并入) */
        picojson::value rows;
        if (JParseU8(rows, rowsRaw)) feed["rows"] = rows;
    }
    if (!st->wrote.empty()) {   /* 导出的文件清单 (模型回答可引用; 前端 linkify 自动可点) */
        picojson::array wf;
        for (auto& w : st->wrote) wf.push_back(JS(w));
        feed["writtenFiles"] = picojson::value(wf);
    }
    if (!wroteNote8.empty()) {   /* 取消/拒绝/失败的逐项经过 (真实经过不粉饰) */
        feed["writesNote"] = JS(W8(wroteNote8.c_str()));
    }
    st->res8 = picojson::value(feed).serialize();
    return L"";
}

/* ---- lua_exec 文件导出的执行段 (脚本结束后 worker 侧; 2026-09-25) ----
 * Lua 回调只登记 (看门狗 10 秒禁阻塞), 真正写盘/确认在这里: 保存对话框与覆盖确认经
 * AgentUiCall 编组到 UI 线程 (15s 无响应/停止 = 放弃该项), 成功路径进 st->wrote
 * (卡片常显块 + 随历史落库)。返回给模型的附注 (UTF-8, 空=无)。 */
static std::wstring LuaWriteFileSync(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return L"创建文件失败 (错误码 " + std::to_wstring(GetLastError()) +
               L"; 目录不存在或无写入权限?)";
    DWORD wr = 0;
    BOOL ok = data.empty() || (WriteFile(h, data.data(), (DWORD)data.size(), &wr, NULL) && wr == (DWORD)data.size());
    CloseHandle(h);
    return ok ? L"" : L"写入失败 (磁盘满或被占用)";
}
/* 经 UI 编组问用户 (返回 true=允许; abort/超时=拒绝并带原因) */
static bool LuaWriteAsk(AiJob* j, const std::wstring& text, bool* stopped) {
    AiUiJob* jb = new AiUiJob();
    jb->kind = UIW_ASK_WRITE;
    jb->s1 = text;
    jb->waitMs = 90000;   /* 用户可能切屏后才发现确认框; 15s 默认会中途放弃 (框后点确认也不落盘) */
    std::string out8;
    std::wstring err = AgentUiCall(j, jb, &out8);
    if (!err.empty()) { *stopped = err == L"已停止"; return false; }   /* 超时/停止 = 不写 */
    Jv v = JsonParseW(W8(out8.c_str()));
    const Jv* ok = v.Get(L"确认");
    return ok && ok->t == 1 && ok->b;
}
static std::string ExecuteLuaWrites(AiJob* j, AiToolStep* st) {
    std::vector<AiWriteItem> items;
    std::string preNote;
    EnterCriticalSection(&g_emitCs);
    items.swap(g_writeBuf);
    preNote.swap(g_writeNote);   /* 回调层拒绝/失败留痕 (登记为空时也要回喂) */
    LeaveCriticalSection(&g_emitCs);
    if (items.empty() && preNote.empty()) return "";
    std::wstring note = W8(preNote.c_str());
    for (auto& it : items) {
        std::wstring path = it.path;
        if (it.saveDlg) {   /* 保存对话框: 用户选定即授权, 已存在目标由系统对话框内建确认 */
            AiUiJob* jb = new AiUiJob();
            jb->kind = UIW_SAVE_DIALOG;
            jb->s2 = it.path;   /* 默认文件名建议 */
            jb->waitMs = 180000;   /* 用户找目录可能要很久, 15s 默认会中途放弃 (框后点保存也不落盘) */
            std::string out8;
            std::wstring err = AgentUiCall(j, jb, &out8);
            if (!err.empty()) { note += L"保存对话框未完成 (" + err + L"): " + it.path + L"\n"; continue; }
            Jv v = JsonParseW(W8(out8.c_str()));
            std::wstring sel = v.S(L"path");
            if (sel.empty()) { note += L"用户取消了保存: " + it.path + L"\n"; continue; }
            path = sel;
        } else {
            bool exists = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
            int pol = InterlockedCompareExchange(&g_luaIoPolicy, 0, 0);
            if (exists || pol == 2) {   /* 覆盖恒确认; 询问档写新文件也确认 */
                bool stopped = false;
                std::wstring what = exists ? (L"AI 想要覆盖已存在的文件:\n\n" + path +
                                             L"\n\n覆盖后原内容无法恢复。覆盖它吗?\n(选「否」= 跳过这个文件, 其它文件继续)")
                                           : (L"AI 请求写出新文件:\n\n" + path +
                                             L"\n\n允许吗?\n(选「否」= 跳过这个文件)");
                if (!LuaWriteAsk(j, what, &stopped)) {
                    if (stopped) { note += L"已停止, 未写出的文件已跳过\n"; break; }
                    note += L"用户未确认, 已跳过: " + path + L"\n";
                    continue;
                }
            }
        }
        std::wstring werr = LuaWriteFileSync(path, it.data);
        if (werr.empty()) {
            st->wrote.push_back(path);
            note += L"已写出 (" + std::to_wstring(it.data.size()) + L" 字节): " + path + L"\n";
        } else {
            note += L"写出失败: " + path + L" (" + werr + L")\n";
        }
    }
    return U8(note);
}

/* open_file 实体: 打开/定位最近一次 run_search 样本中的文件 (走宿主打开行为)。
 * 宿主 OpenFile 是界面类 API (仅 UI 线程) — 经 UI 编组执行 (曾在工作者线程直调必吃 ERR_THREAD)。 */
static std::wstring AgentToolOpenFile(AiJob* j, int index, bool reveal, XjsWindowToken tok, AiToolStep* st) {
    if (index < 1 || index > (int)g_agentTopIds.size())
        return L"序号越界 (最近一次搜索样本共 " + std::to_wstring((unsigned long long)g_agentTopIds.size()) + L" 条)";
    int fid = g_agentTopIds[index - 1];
    return AgentToolUi(j, UIW_OPEN_FILE, tok, L"", L"", L"", fid, reveal ? 1 : 0, st);
}

/* copy_paths 实体: 最近一次结果前 100 条路径 (每行一条) 复制到剪贴板 */
static std::wstring AgentToolCopyPaths() {
    if (!g_agentRes || !xjs_result_IsEffective(g_agentRes)) return L"还没有已完成的搜索";
    xjs_engine* eng = xjs_result_GetXjsEngine(g_agentRes);
    int count = xjs_result_GetCount(g_agentRes);
    if (count <= 0) return L"当前结果为空";
    std::wstring text;
    int n = count < 100 ? count : 100;
    for (int i = 0; i < n; i++) {
        int fid = xjs_result_GetFileId(g_agentRes, i);
        if (fid < 0) continue;
        const char* p = xjs_db_GetPath(eng, fid);
        if (p) { text += W8(p); text += L"\r\n"; }
    }
    if (text.empty()) return L"没有可复制的路径";
    if (!g_host || g_host->ClipboardSetText(g_ctx, U8(text).c_str()) != XJS_PLUGIN_OK)
        return L"剪贴板写入失败";
    return L"";
}

/* ==================== run_command: 执行外部程序 (cmd/powershell) ====================
 * 管线口径对齐 DeepSeek Harness (dsh) 的 shell 执行器:
 *   - 进程树防孤儿 = kill-on-close Job Object; CREATE_SUSPENDED 先挂起、入 Job、再恢复
 *     (消除"子进程先拉孙进程再被入 Job"的竞态窗口), 终止 = TerminateJobObject 一次杀整棵树
 *     (Windows 没有优雅终止, dsh 同款立即强杀);
 *   - stdio = 匿名管道 (子端开继承位, 父端立即关子侧副本), stdout/stderr 各一条读线程阻塞
 *     ReadFile (管道死锁的第一防线是"父进程别攥着子端句柄", 第二是读不占等待循环);
 *   - 输出 = 每流 32KB 保尾弃头 (错误与最终结果聚在尾部, dsh tail-keep 口径), 丢弃量精确回喂;
 *   - 结果 = stdout 原文 + [stderr] 分节 + 末行 [exit code: N] (仅非零, dsh render 契约);
 *   - 凭据不外泄: 子进程环境剔除名字含 KEY/PASSWORD/SECRET/TOKEN/密钥/密码 的变量
 *     (dsh scrubbedParentEnv 口径), 另补 NO_COLOR=1 防颜色码污染解析。
 * 编码: cmd 前缀 chcp 65001 统一输出编码 (/d 跳过 AutoRun 脚本); powershell 用
 * -EncodedCommand (base64 UTF-16LE) 免引号转义地狱 + [Console]::OutputEncoding 前导;
 * 解码先按 UTF-8 严格试, 失败回落 OEM 页 (老工具不理会控制台码页时仍能读)。
 * 权限三道闸: ① manifest "exec" 权限 (宿主启用确认框告知) ② execPolicy 用户档位
 * (禁用/询问/允许, 询问 = 每条命令出确认卡) ③ 高危特征扫描 (ExecRiskText, 结果挂卡)。
 * "允许一次"只放行完全相同的调用键一次 (AiJob::execGrant), 不持久放权 — dsh 审批
 * 全部一次性 (allowed-once) 的口径。 */
static const size_t EXEC_STREAM_CAP = 32768;   /* 每流保尾字节量 */

static std::string B64EncStr(const unsigned char* d, size_t n) {   /* RFC 4648 (EncodedCommand 载荷) */
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string r;
    r.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)d[i] << 16;
        if (i + 1 < n) v |= (unsigned)d[i + 1] << 8;
        if (i + 2 < n) v |= (unsigned)d[i + 2];
        r += T[(v >> 18) & 63];
        r += T[(v >> 12) & 63];
        r += (i + 1 < n) ? T[(v >> 6) & 63] : '=';
        r += (i + 2 < n) ? T[v & 63] : '=';
    }
    return r;
}

/* 单流收集: 读线程独写 Push, 主线程收尾 Take (解码 UTF-8 → OEM 回落) */
struct ExecStream {
    CRITICAL_SECTION cs;
    std::string tail;
    size_t dropped = 0;                  /* 丢弃头部字节量 (精确回喂) */
    void Init() { InitializeCriticalSectionAndSpinCount(&cs, 100); }
    void Term() { DeleteCriticalSection(&cs); }
    void Push(const char* d, size_t n) {
        EnterCriticalSection(&cs);
        tail.append(d, n);
        if (tail.size() > EXEC_STREAM_CAP) {
            dropped += tail.size() - EXEC_STREAM_CAP;
            tail.erase(0, tail.size() - EXEC_STREAM_CAP);
        }
        LeaveCriticalSection(&cs);
    }
    std::wstring Take(size_t* droppedOut) {
        EnterCriticalSection(&cs);
        std::string raw = tail;
        size_t dp = dropped;
        LeaveCriticalSection(&cs);
        *droppedOut = dp;
        if (raw.empty()) return L"";
        int wl = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), (int)raw.size(), NULL, 0);
        if (wl > 0) {
            std::wstring w((size_t)wl, 0);
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), (int)raw.size(), &w[0], wl);
            return w;
        }
        UINT oem = GetOEMCP();
        wl = MultiByteToWideChar(oem, 0, raw.data(), (int)raw.size(), NULL, 0);
        std::wstring w(wl > 0 ? (size_t)wl : 0, 0);
        if (wl > 0) MultiByteToWideChar(oem, 0, raw.data(), (int)raw.size(), &w[0], wl);
        return w;
    }
};

/* 子进程环境块: 继承父环境但剔除疑似密钥变量, 补 NO_COLOR=1; 按 key 小写排序去重
 * (CreateProcessW 的 Unicode 环境块要求排序), 双 \0 结尾 */
static std::wstring BuildChildEnv() {
    auto lowStr = [](const std::wstring& s) {
        std::wstring r = s;
        for (auto& c : r) c = towlower(c);
        return r;
    };
    std::vector<std::pair<std::wstring, std::wstring>> kv;
    wchar_t* blk = GetEnvironmentStringsW();
    if (blk) {
        const wchar_t* p = blk;
        while (*p) {
            const wchar_t* eq = wcschr(p, L'=');
            if (eq && eq != p) {
                std::wstring up = lowStr(std::wstring(p, eq));
                /* 名字含任一敏感词即剔 — 宁可错杀 (monkey 之类误伤无害) */
                if (up.find(L"key") == std::wstring::npos &&
                    up.find(L"password") == std::wstring::npos &&
                    up.find(L"secret") == std::wstring::npos &&
                    up.find(L"token") == std::wstring::npos &&
                    up.find(L"密钥") == std::wstring::npos &&
                    up.find(L"密码") == std::wstring::npos)
                    kv.emplace_back(std::wstring(p, eq), std::wstring(eq + 1));
            }
            p += wcslen(p) + 1;
        }
        FreeEnvironmentStringsW(blk);
    }
    kv.emplace_back(L"NO_COLOR", L"1");
    std::sort(kv.begin(), kv.end(), [&lowStr](auto& a, auto& b) { return lowStr(a.first) < lowStr(b.first); });
    std::wstring out;
    for (size_t i = 0; i < kv.size(); i++) {
        if (i && lowStr(kv[i].first) == lowStr(kv[i - 1].first)) continue;
        out += kv[i].first;
        out += L'=';
        out += kv[i].second;
        out += L'\0';
    }
    out += L'\0';
    return out;
}

/* 高危命令特征扫描 (dsh 没有这层 — 它靠沙箱强隔离; 本插件与主程序同权限运行, 必须自建):
 * 命中即把特征挂上确认卡帮用户快速定位危险点, 只警示不替代用户判断 (小写子串匹配,
 * 误报可接受, 漏报由确认卡兜底)。 */
static std::wstring ExecRiskText(const std::wstring& cmdRaw) {
    static const struct { const wchar_t* pat; const wchar_t* why; } RISK[] = {
        { L"format ", L"格式化磁盘" },
        { L"shutdown", L"关机/重启" },
        { L"diskpart", L"磁盘分区操作" },
        { L"bcdedit", L"改启动配置" },
        { L"vssadmin", L"卷影副本(快照)操作" },
        { L"cipher /w", L"抹除磁盘空闲空间" },
        { L"reg add", L"写注册表" },
        { L"reg delete", L"删注册表" },
        { L"regedit /s", L"静默导入注册表" },
        { L"reg unload", L"卸载注册表配置单元" },
        { L"del /s", L"递归删除文件" },
        { L"rd /s", L"递归删除目录" },
        { L"rmdir /s", L"递归删除目录" },
        { L"remove-item", L"删除文件/目录 (PowerShell)" },
        { L"net user", L"用户账户操作" },
        { L"net localgroup", L"用户组操作" },
        { L"sc delete", L"删除系统服务" },
        { L"sc config", L"改系统服务配置" },
        { L"taskkill", L"强制结束进程" },
        { L"stop-process", L"强制结束进程 (PowerShell)" },
        { L"schtasks", L"计划任务操作" },
        { L"takeown", L"夺取文件所有权" },
        { L"icacls", L"改文件 ACL 权限" },
        { L"set-executionpolicy", L"改 PowerShell 脚本执行策略" },
        { L"invoke-expression", L"动态执行字符串代码" },
        { L"-encodedcommand", L"嵌套编码命令 (混淆特征)" },
        { L"start-process", L"拉起新进程" },
        { L"certutil -urlcache", L"从网络下载" },
        { L"bitsadmin /transfer", L"从网络下载" },
        { L"invoke-webrequest", L"从网络下载" },
        { L"invoke-restmethod", L"从网络下载" },
        { L"curl ", L"从网络下载" },
        { L"curl.exe", L"从网络下载" },
        { L"wget ", L"从网络下载" },
    };
    std::wstring s = cmdRaw;
    for (auto& c : s) c = towlower(c);
    std::wstring hits;
    for (auto& r : RISK) {
        if (s.find(r.pat) != std::wstring::npos) {
            if (!hits.empty()) hits += L"、";
            hits += r.why;
        }
    }
    return hits;
}

/* run_command 实体 (agent 工作线程直跑, 不碰引擎不持 g_agentCs)。返回错误描述
 * (空=成功; 超时/输出截断不算错误, 标记写进输出文本回喂模型)。 */
static std::wstring AgentToolRunCommand(AiJob* j, const std::wstring& shell,
                                        const std::wstring& command, const std::wstring& workdir,
                                        long long timeoutMs, AiToolStep* st) {
    if (command.empty()) return L"command 不能为空";
    if (shell != L"cmd" && shell != L"powershell") return L"shell 只接受 cmd | powershell";
    if (timeoutMs <= 0) timeoutMs = 120000;
    if (timeoutMs < 3000) timeoutMs = 3000;
    if (timeoutMs > 600000) timeoutMs = 600000;

    /* 工作目录: 必填时必须存在且是目录; 缺省落临时目录 (可写, 误操作伤害最小 —
     * 命令里的相对路径操作都发生在那里, description 已告知模型) */
    std::wstring cwd = workdir;
    if (!cwd.empty()) {
        DWORD at = GetFileAttributesW(cwd.c_str());
        if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY))
            return L"workdir 不存在或不是目录: " + cwd;
    } else {
        wchar_t tp[MAX_PATH + 1];
        DWORD n = GetTempPathW(MAX_PATH, tp);
        if (n > 0 && n <= MAX_PATH) cwd = tp;
    }

    std::wstring cmdline;
    if (shell == L"cmd") {
        cmdline = L"cmd.exe /d /s /c \"chcp 65001>nul & " + command + L"\"";
    } else {
        std::wstring ps = L"[Console]::OutputEncoding=[System.Text.Encoding]::UTF8; " + command;
        cmdline = L"powershell.exe -NoLogo -NoProfile -NonInteractive -EncodedCommand "
                  + W8(B64EncStr((const unsigned char*)ps.c_str(), ps.size() * sizeof(wchar_t)).c_str());
    }

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE outR = NULL, outW = NULL, errR = NULL, errW = NULL;
    if (!CreatePipe(&outR, &outW, &sa, 0) || !CreatePipe(&errR, &errW, &sa, 0)) {
        if (outR) CloseHandle(outR);
        if (outW) CloseHandle(outW);
        if (errR) CloseHandle(errR);
        if (errW) CloseHandle(errW);
        return L"管道创建失败";
    }
    /* 子端写句柄开继承, 父端读句柄关继承 (父侧多线程 + 孙进程都不该攥着读端, 否则 EOF 永不来) */
    SetHandleInformation(outW, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(errW, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);

    /* kill-on-close Job 先建好 (建失败 = 失去树杀能力, 进程不放行) */
    HANDLE job = CreateJobObjectW(NULL, NULL);
    bool jobOk = false;
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim = {};
        lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        jobOk = SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof(lim)) != FALSE;
    }

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = NULL;   /* 无 stdin: 误等输入的命令读到无效句柄即失败返回, 不会挂死 */
    si.hStdOutput = outW;
    si.hStdError = errW;

    std::wstring envBlk = BuildChildEnv();
    PROCESS_INFORMATION pi = {};
    BOOL created = CreateProcessW(NULL, &cmdline[0], NULL, NULL, TRUE,
                                  CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                                  (LPVOID)envBlk.c_str(), cwd.c_str(), &si, &pi);
    /* 父侧立即关子端写副本 — 不关的话子进程退出后管道也等不到 EOF (dsh 同款第一防线) */
    CloseHandle(outW); outW = NULL;
    CloseHandle(errW); errW = NULL;
    if (!created) {
        DWORD err = GetLastError();
        wchar_t msg[128];
        swprintf(msg, 128, L"进程创建失败 (Win32 错误 %lu; 命令不存在/路径有误?)", (unsigned long)err);
        CloseHandle(outR); CloseHandle(errR);
        if (job) CloseHandle(job);
        return msg;
    }
    if (!jobOk || AssignProcessToJobObject(job, pi.hProcess) == FALSE) {
        TerminateProcess(pi.hProcess, 1);   /* 入 Job 失败 = 杀不了树, 宁可不放行 */
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(outR); CloseHandle(errR);
        if (job) CloseHandle(job);
        return L"进程约束 (Job Object) 建立失败, 已终止该进程";
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    ExecStream so, se;
    so.Init();
    se.Init();
    struct Rw { HANDLE h; ExecStream* s; };
    auto reader = [](void* pv) -> DWORD {
        Rw* r = (Rw*)pv;
        char buf[8192];
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(r->h, buf, sizeof(buf), &got, NULL) || got == 0) break;
            r->s->Push(buf, got);
        }
        CloseHandle(r->h);   /* 读端由读线程关闭 (EOF 后) */
        return 0;
    };
    Rw ro = { outR, &so }, re = { errR, &se };
    HANDLE tOut = CreateThread(NULL, 0, reader, &ro, 0, NULL);
    HANDLE tErr = CreateThread(NULL, 0, reader, &re, 0, NULL);
    if (!tOut) { CloseHandle(outR); }   /* 线程建失败: 手动关读端防泄漏 (该流视为空) */
    if (!tErr) { CloseHandle(errR); }

    /* 等待: 停止/超时 → TerminateJobObject 杀整棵树; 正常退出后给残余孙进程 2 秒排水窗
     * (它们可能还攥着管道写端), 超窗同样杀树让读线程拿到 EOF */
    ULONGLONG t0 = GetTickCount64();
    ULONGLONG deadline = t0 + (ULONGLONG)timeoutMs;
    bool timedOut = false, stopped = false;
    for (;;) {
        if (InterlockedCompareExchange(&j->abort, 0, 0)) { stopped = true; break; }
        if (WaitForSingleObject(pi.hProcess, 40) == WAIT_OBJECT_0) break;
        if (GetTickCount64() >= deadline) { timedOut = true; break; }
    }
    if (timedOut || stopped) TerminateJobObject(job, 1);
    else {
        ULONGLONG drainT0 = GetTickCount64();
        while (GetTickCount64() - drainT0 < 2000) {
            bool outDone = !tOut || WaitForSingleObject(tOut, 0) == WAIT_OBJECT_0;
            bool errDone = !tErr || WaitForSingleObject(tErr, 0) == WAIT_OBJECT_0;
            if (outDone && errDone) break;
            Sleep(20);
        }
        TerminateJobObject(job, 1);
    }
    if (tOut) { WaitForSingleObject(tOut, 4000); CloseHandle(tOut); }
    if (tErr) { WaitForSingleObject(tErr, 4000); CloseHandle(tErr); }

    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(job);

    size_t dropO = 0, dropE = 0;
    std::wstring outTxt = so.Take(&dropO);
    std::wstring errTxt = se.Take(&dropE);
    so.Term();
    se.Term();
    if (stopped) return L"已停止";
    std::wstring out = outTxt;
    if (!errTxt.empty()) {
        if (!out.empty()) out += L'\n';
        out += L"[stderr]\n" + errTxt;
    }
    if (dropO > 0)
        out += L"\n(stdout 过长已截断: 仅保留尾部 32KB, 丢弃头部 " + std::to_wstring(dropO) +
               L" 字节 — 请改用更精确的命令缩小输出)";
    if (dropE > 0)
        out += L"\n(stderr 过长已截断: 仅保留尾部 32KB, 丢弃头部 " + std::to_wstring(dropE) + L" 字节)";
    if (out.empty()) out = L"(无输出)";
    if (timedOut) {
        wchar_t tb[96];
        swprintf(tb, 96, L"\n[timed out after %lld ms, 已终止进程树]", timeoutMs);
        out += tb;
    } else if (exitCode != 0) {   /* dsh render 契约: exit 标记仅非零出现, 恒为末行 */
        wchar_t eb[48];
        swprintf(eb, 48, L"\n[exit code: %lu]", (unsigned long)exitCode);
        out += eb;
    }
    st->res8 = U8(out);
    st->elapsedMs = (long long)(GetTickCount64() - t0);
    return L"";
}

/* ==================== 工具结果省 token (向成熟 agent harness 口径看齐) ====================
 * ① 旧轮工具输出中段裁剪 (ToolOutputPrune): 一轮对话里早先回喂过的超长输出 (get_lua_spec
 *   规范全文/大段 ai.print), 后续每轮重发时只留头尾+占位标记 — 原文已经送达过一次,
 *   规范可随时 get_lua_spec 重取; 本轮刚产出的批次不裁 (模型正要用)。
 * ② 环境快照移到请求尾部 (AgentBuildBody): instructions/tools/历史前缀逐字节稳定,
 *   provider 前缀缓存才命得到 — 快照带秒级时间, 拼在 instructions 里每请求必变 = 缓存全灭。
 * (路径不回喂模型是最大的省 token 项: 搜索/选中样本只回 [FileId,文件名], 见
 *   AgentToolRunSearch 尾部与 get_window_selection — 2026-09-25 用户口径。) */

/* 旧轮工具输出中段裁剪: 超过阈值只保留头尾, 断点回退 UTF-8 字符边界 (避免切碎多字节)。 */
static const size_t PRUNE_THRESHOLD = 3600, PRUNE_HEAD = 2400, PRUNE_TAIL = 900;
static size_t PruneUtf8Floor(const std::string& s, size_t pos) {
    while (pos > 0 && (unsigned char)s[pos] >= 0x80 && (unsigned char)s[pos] < 0xC0) pos--;
    return pos;
}
static std::string ToolOutputPrune(const std::string& s) {
    if (s.size() <= PRUNE_THRESHOLD) return s;
    size_t headEnd = PruneUtf8Floor(s, PRUNE_HEAD);
    size_t tailBegin = PruneUtf8Floor(s, s.size() - PRUNE_TAIL);
    std::string r = s.substr(0, headEnd);
    r += "\n…[中间省略 " + std::to_string(tailBegin - headEnd) +
         " 字节; 该输出此前已完整回喂过, 需要时重新调用同一工具取回]…\n";
    r += s.substr(tailBegin);
    return r;
}

/* 参数小工具 */
static std::wstring JWin(const Jv& v) {   /* window 参数 (窗口名称; 空 = 对话所在窗) */
    return TrimW(v.S(L"window"));
}
static bool JFlagTrue(const Jv& v, const wchar_t* key) {   /* 布尔参数缺省 = true */
    const Jv* b = v.Get(key);
    return !(b && b->t == 1 && !b->b);
}
static void ArgzCut(std::wstring* s) {
    if (s->size() > 200) { s->resize(200); *s += L"…"; }
}

/* 工具统一入口 (agent 工作线程调用): 解析参数 → 执行 → 结果填进 step; 返回错误描述 (空=成功)。
 * 串行锁轮询占用, 被停止打断返回"已停止"。
 * 搜索类 (run_search/copy_paths) 直连引擎/剪贴板任意线程; 其余一律 AgentToolUi 编组到 UI 线程。 */
static std::wstring AgentToolExec(AiJob* j, const std::string& name8, const std::string& args8,
                                  XjsWindowToken tok, AiToolStep* st) {
    Jv v = JsonParseW(W8(args8.c_str()));
    if (name8 == "run_search") {
        st->kind = 0;
        st->mode = v.S(L"mode");
        st->query = v.S(L"query");
        if (st->query.empty()) return L"query 不能为空";
        if (!AgentCsEnter(j)) return L"已停止";
        std::wstring err = AgentToolRunSearch(j, st->mode, st->query, st);
        LeaveCriticalSection(&g_agentCs);
        return err;
    }
    if (name8 == "open_file") {
        st->kind = 1;
        const Jv* rv = v.Get(L"reveal");
        bool reveal = rv && ((rv->t == 1 && rv->b) || (rv->t == 2 && rv->num != 0));
        int index = (int)wcstol(TrimW(v.S(L"index")).c_str(), NULL, 10);
        if (!AgentCsEnter(j)) return L"已停止";
        std::wstring err = AgentToolOpenFile(j, index, reveal, tok, st);
        LeaveCriticalSection(&g_agentCs);
        return err;
    }
    if (name8 == "copy_paths") {
        st->kind = 2;
        if (!AgentCsEnter(j)) return L"已停止";
        std::wstring err = AgentToolCopyPaths();
        LeaveCriticalSection(&g_agentCs);
        return err;
    }
    if (name8 == "get_lua_spec") {
        /* Lua 规范重读通道 (2026-09-25 起规范全文默认拼进系统提示词附录, 本工具供脚本报错
         * 排查时重读/附录疑似截断时取全文): 引擎内嵌文本工作者线程直读, 无需 UI 编组。
         * 优先合集 (promptType 2 = 0+1 合并去重版); 旧引擎无合集 = 按 mode 单取一份 (此时 mode 必填)。 */
        st->kind = 9;
        const char* p = xjs_Query_GetPrompt(2);
        if (p && *p) {
            st->argz = L"Lua 规范合集";
        } else {
            std::wstring wm = TrimW(v.S(L"mode"));
            if (wm == L"lua_filter" || wm == L"lua_exec")
                p = xjs_Query_GetPrompt(wm == L"lua_exec" ? 1 : 0);
            if (!p || !*p) return L"mode 必填 lua_filter | lua_exec (引擎无合集提示词)";
            st->argz = wm + L" 规范";
        }
        st->res8 = std::string(p);   /* 规范原文回喂 (纯文本) */
        return L"";
    }

    if (name8 == "get_author_and_donate") {
        /* 关于作者和捐赠: 作者/软件介绍 = README 权威事实 (模型据此回答, 不编造);
         * 二维码图片本体 (几百 KB base64) 绝不进对话通道烧 token — 只回引用语法,
         * md 渲染层把 xjs://donate?kind=.. 换成本地缓存真图 (DonateQrDataUrl; 工作者线程直调) */
        st->kind = 10;
        st->argz = L"作者与捐赠";
        bool w = !DonateQrDataUrl(0).empty();
        bool a = !DonateQrDataUrl(1).empty();
        picojson::object j;
        j[U8(L"软件")] = JS(L"蜗牛快搜 SnailQuickSearch — Windows 本地文件即时搜索工具:"
                            L"全盘索引, 输入即搜, 秒级定位, 同规模索引下常驻内存更低;"
                            L"纯 Direct2D 自绘界面, 内置可编程插件系统");
        j[U8(L"授权")] = JS(L"免费软件: 个人与商业环境均可免费使用; 成品层源码开放查看/修改/二次开发;"
                            L"内置 xunjieso 搜索引擎为版权人自研闭源组件, 随软件免费授权使用");
        j[U8(L"特性")] = JS(L"输入即搜(无防抖)/全盘索引/低内存占用/五种搜索模式(wildcard,regex,sql,lua过滤,lua执行)/"
                            L"多搜索窗口(每窗独立设置)/原生插件系统/六种语言界面/预览面板/文件操作(重命名,别名,剪切粘贴,拖出,定位打开)");
        j[U8(L"捐赠话术")] = JS(L"如果蜗牛快搜帮到了你, 欢迎请作者喝杯咖啡 — 捐赠完全自愿, 金额随意, 软件本身永久免费");
        if (w || a) {
            picojson::array qr;
            if (w) qr.push_back(JS(L"微信"));
            if (a) qr.push_back(JS(L"支付宝"));
            j[U8(L"可用二维码")] = picojson::value(qr);
            j[U8(L"二维码如何展示")] = JS(L"在回答正文里用图片语法引用 (只引上面列出的可用项):"
                 L" ![微信捐赠码](xjs://donate?kind=wechat) 或 ![支付宝捐赠码](xjs://donate?kind=alipay)。"
                 L"二维码会竖排显示在对话页, 微信优先放最前; 用户没点名要支付宝时可以只放微信。"
                 L"不要把 base64/文件路径写进回答, 不要用普通链接语法");
        } else {
            j[U8(L"二维码")] = JS(L"暂不可用 (安装目录缺二维码图片), 如实告知用户即可");
        }
        st->res8 = picojson::value(j).serialize();
        return L"";
    }

    /* ---- 代办类 (宿主扩展 API, 全部经 UI 编组; 宿主能力面见 xjs_plugin_sdk.h) ---- */
    if (!HOST_QAPI_OK) return L"宿主版本过旧, 不支持窗口/设置代办工具";
    std::wstring win = JWin(v);

    if (name8 == "list_windows") {
        st->kind = 4;
        st->argz = L"列出全部搜索窗口";
        return AgentToolUi(j, UIW_LIST_WINDOWS, tok, L"", L"", L"", 0, 0, st);
    }
    if (name8 == "get_window_state") {
        st->kind = 3;
        st->argz = win.empty() ? L"(当前窗口) 状态与设置" : (win + L" 状态与设置");
        return AgentToolUi(j, UIW_WINDOW_STATE, tok, win, L"", L"", 0, 0, st);
    }
    if (name8 == "set_window_settings") {
        st->kind = 3;
        const Jv* sv = v.Get(L"settings");
        if (!sv || sv->t != 5 || sv->obj.empty()) return L"settings 参数缺失 (须为非空对象)";
        std::vector<AiAdjustItem> items;
        for (auto& kv : sv->obj) {
            AiAdjustItem it;
            it.key = kv.first;
            it.val = AdjustValueText(kv.first, kv.second);
            picojson::object one;
            one[U8(kv.first)] = JvToP(kv.second);
            it.json = picojson::value(one).serialize();
            items.push_back(std::move(it));
        }
        return ProposeAdjustCard(st, 0, win, std::move(items));
    }
    if (name8 == "get_global_settings") {
        st->kind = 3;
        st->argz = L"读取全局设置";
        return AgentToolUi(j, UIW_GET_GLOBAL, tok, L"", L"", L"", 0, 0, st);
    }
    if (name8 == "set_global_settings") {
        st->kind = 3;
        const Jv* sv = v.Get(L"settings");
        if (!sv || sv->t != 5 || sv->obj.empty()) return L"settings 参数缺失 (须为非空对象)";
        std::vector<AiAdjustItem> items;
        for (auto& kv : sv->obj) {
            AiAdjustItem it;
            it.key = kv.first;
            it.val = AdjustValueText(kv.first, kv.second);
            picojson::object one;
            one[U8(kv.first)] = JvToP(kv.second);
            it.json = picojson::value(one).serialize();
            items.push_back(std::move(it));
        }
        return ProposeAdjustCard(st, 1, L"", std::move(items));
    }
    if (name8 == "list_skins") {
        st->kind = 8;
        st->argz = L"列出可用皮肤";
        return AgentToolUi(j, UIW_LIST_SKINS, tok, L"", L"", L"", 0, 0, st);
    }
    if (name8 == "get_window_selection") {
        st->kind = 3;
        st->argz = (win.empty() ? L"(当前窗口)" : win) + L" 选中文件";
        long long limit = 200;
        const Jv* lv = v.Get(L"limit");
        if (lv && lv->t == 2 && lv->num >= 1 && lv->num <= 10000) limit = (long long)lv->num;
        std::wstring err = AgentToolUi(j, UIW_WINDOW_SELECTION, tok, win, L"", L"", limit, 0, st);
        if (!err.empty()) return err;
        /* worker 直连引擎补名称 (GetName 指针为线程本地缓存, 必须立即拷贝);
           UI 编组只回 FileId (宿主口径: 引擎数据是事实源)。解析不出引擎时原样透传 ID 列表 */
        Jv o = JsonParseW(W8(st->res8.c_str()));
        const Jv* arr = o.Get(L"文件ID");
        xjs_engine* eng = xjs_GetDefaultEngine();
        if (arr == NULL || arr->t != 4 || arr->arr.empty() || !eng) return L"";
        long long total = 0;
        const Jv* tv = o.Get(L"选中数");
        if (tv && tv->t == 2) total = (long long)tv->num;
        /* 紧凑回喂: files=[[FileId,文件名],…] (同 run_search 口径: 模型不拿路径,
           引用文件一律用 ID。键名/逐对象包装对 200 条上限的清单是白烧 token) */
        picojson::object out;
        out[U8(L"win")] = JS(o.S(L"窗口名称"));
        out["total"] = JN(total);
        picojson::array fs;
        for (auto& f : arr->arr) {
            if (f.t != 2) continue;
            int fid = (int)f.num;
            const char* nm = xjs_db_GetName(eng, fid);
            picojson::array pair;
            pair.push_back(JN(fid));
            pair.push_back(JS(nm && *nm ? W8(nm) : L"(未命名)"));
            fs.push_back(picojson::value(pair));
        }
        out["files"] = picojson::value(fs);
        st->res8 = picojson::value(out).serialize();
        return L"";
    }
    if (name8 == "list_languages") {
        st->kind = 8;
        st->argz = L"列出界面语言";
        return AgentToolUi(j, UIW_LIST_LANGS, tok, L"", L"", L"", 0, 0, st);
    }
    if (name8 == "get_language") {
        st->kind = 3;
        st->argz = (win.empty() ? L"(当前窗口)" : win) + L" 界面语言";
        return AgentToolUi(j, UIW_GET_LANGUAGE, tok, win, L"", L"", 0, 0, st);
    }
    if (name8 == "set_language") {
        st->kind = 3;
        std::wstring code = TrimW(v.S(L"language"));
        if (code != L"auto" && code != L"zh" && code != L"zh-TW" && code != L"en" &&
            code != L"ko" && code != L"th" && code != L"ms")
            return L"language 只接受 auto | zh | zh-TW | en | ko | th | ms (list_languages 查母语名)";
        AiAdjustItem it;
        it.key = L"语言";
        Jv lv; lv.t = 3; lv.str = code;
        it.val = AdjustValueText(L"语言", lv);
        {
            picojson::object one;
            one[U8(L"语言")] = JS(code);
            it.json = picojson::value(one).serialize();
        }
        std::vector<AiAdjustItem> items;
        items.push_back(std::move(it));
        return ProposeAdjustCard(st, 0, win, std::move(items));
    }
    if (name8 == "control_window") {
        st->kind = 4;
        std::wstring act = TrimW(v.S(L"action"));
        if (act != L"show" && act != L"dismiss" && act != L"openSettings")
            return L"action 只接受 show | dismiss | openSettings";
        AiAdjustItem it;
        it.key = act == L"show" ? L"唤起窗口" : (act == L"dismiss" ? L"收起窗口" : L"打开设置窗口");
        it.val = win.empty() ? L"(当前窗口)" : win;
        it.json = U8(act);
        std::vector<AiAdjustItem> items;
        items.push_back(std::move(it));
        return ProposeAdjustCard(st, 2, win, std::move(items));
    }
    if (name8 == "create_window") {
        st->kind = 4;
        std::wstring profile = TrimW(v.S(L"profile"));
        std::wstring inherit = TrimW(v.S(L"inherit"));
        AiAdjustItem it;
        it.key = L"新建窗口";
        it.val = profile.empty() ? L"(空白档案)" : profile;
        it.json = U8(profile + L"|" + inherit);   /* 档案名|继承尺寸的窗名 (应用时拆) */
        std::vector<AiAdjustItem> items;
        items.push_back(std::move(it));
        return ProposeAdjustCard(st, 3, L"", std::move(items));
    }
    if (name8 == "set_search") {
        st->kind = 5;
        std::wstring kw = v.S(L"keyword");
        std::wstring mode = TrimW(v.S(L"mode"));
        if (!mode.empty() && mode != L"wildcard" && mode != L"regex" && mode != L"sql" &&
            mode != L"lua" && mode != L"lua-exec")
            return L"mode 只接受 wildcard | regex | sql | lua | lua-exec";
        if (mode == L"lua-exec" && !kw.empty() && !LuaHasTopLevelReturn(U8(kw).c_str()))
            return L"lua-exec 脚本缺少顶层 return, 未置入窗口 — 执行模式必须以顶层 return ID 数组结尾"
                   L"(return 的数组 = 窗口结果列表; 缺 return 执行后只会显示 0 条)。请补上 return 后重试。";
        st->argz = (win.empty() ? L"(当前窗口)" : win) + L" 搜: " + (kw.empty() ? L"(保持现词)" : kw);
        if (!mode.empty()) st->argz += L" [" + mode + L"]";
        ArgzCut(&st->argz);
        return AgentToolUi(j, UIW_SET_SEARCH, tok, win, kw, mode, JFlagTrue(v, L"execute") ? 1 : 0, 0, st);
    }
    if (name8 == "list_modes") {
        st->kind = 6;
        st->argz = (win.empty() ? L"(当前窗口)" : win) + L" 可用模式";
        return AgentToolUi(j, UIW_LIST_MODES, tok, win, L"", L"", 0, 0, st);
    }
    if (name8 == "apply_mode") {
        st->kind = 6;
        std::wstring mid = TrimW(v.S(L"mode_id"));
        if (mid.empty()) return L"mode_id 不能为空 (list_modes 查\"标识\")";
        std::wstring input = v.S(L"input");
        st->argz = mid + (input.empty() ? L"" : (L" 输入: " + input));
        ArgzCut(&st->argz);
        return AgentToolUi(j, UIW_APPLY_MODE, tok, win, mid, input, 0, 0, st);
    }
    if (name8 == "add_search_mode") {
        st->kind = 6;
        std::wstring mname = TrimW(v.S(L"name"));
        std::wstring mtpl = TrimW(v.S(L"template"));
        std::wstring mtype = TrimW(v.S(L"type"));
        std::wstring mdesc = v.S(L"desc");
        if (mname.empty()) return L"name 不能为空";
        if (mtpl.empty()) return L"template 不能为空";
        if (mtpl.find(L"<keyword>") == std::wstring::npos)
            return L"template 必须包含 <keyword> 占位符 (执行时替换为搜索框输入)";
        if (mtype.empty()) mtype = L"wildcard";
        if (mtype != L"wildcard" && mtype != L"regex" && mtype != L"sql" && mtype != L"lua")
            return L"type 只接受 wildcard | regex | sql | lua";
        picojson::object d;
        d[U8(L"名称")] = JS(mname);
        if (!mdesc.empty()) d[U8(L"简介")] = JS(mdesc);
        d[U8(L"类型")] = JS(mtype);
        d[U8(L"模板")] = JS(mtpl);
        st->argz = mname + L" (" + mtype + L")";
        return AgentToolUi(j, UIW_ADD_MODE, tok, W8(picojson::value(d).serialize().c_str()), L"", L"", 0, 0, st);
    }
    if (name8 == "remove_search_mode") {
        st->kind = 6;
        std::wstring mid = TrimW(v.S(L"mode_id"));
        if (mid.empty()) return L"mode_id 不能为空";
        st->argz = mid;
        return AgentToolUi(j, UIW_REMOVE_MODE, tok, mid, L"", L"", 0, 0, st);
    }
    if (name8 == "list_plugins") {
        st->kind = 7;
        st->argz = L"列出全部插件";
        return AgentToolUi(j, UIW_LIST_PLUGINS, tok, L"", L"", L"", 0, 0, st);
    }
    if (name8 == "send_plugin_message") {
        st->kind = 7;
        std::wstring pid = TrimW(v.S(L"plugin_id"));
        if (pid.empty()) return L"plugin_id 不能为空 (list_plugins 查\"标识\")";
        const Jv* pv = v.Get(L"payload");
        std::wstring pay;
        if (pv && pv->t == 5) pay = JDumpW(JvToP(*pv));
        else if (pv && pv->t == 3) pay = pv->str;   /* 字符串载荷原样 (约定为 JSON 文本) */
        else return L"payload 参数缺失 (JSON 对象)";
        st->argz = pid;
        return AgentToolUi(j, UIW_SEND_MSG, tok, pid, pay, L"", 0, 0, st);
    }
    return name8.empty() ? L"未知工具 (模型未给出工具名, 请从 tools 列表中选择)"
                         : (L"未知工具: " + W8(name8.c_str()));
}

/* 工具结果 → function_call_output 文本 (喂回模型的 JSON)。
 * run_search/get_window_selection 的结果 JSON 在各自完成时已拼进 res8 (样本只含
 * [FileId,文件名], 路径不回喂); 其余工具 res8 = 宿主扩展 API 原样, 空 = 无返回值。 */
static std::string AgentToolOutput(const std::wstring& err, const AiToolStep& st) {
    if (!err.empty()) {
        picojson::object o;
        o["error"] = JS(err);
        return picojson::value(o).serialize();
    }
    if (!st.res8.empty()) return st.res8;
    return "{\"ok\":true}";
}

/* 系统提示词 — "常驻骨架 + Lua 规范附录" (2026-09-25 用户口径: Lua 提示词默认进全局提示词):
 *   常驻 = 角色目标 / 工作方式 / 模糊联想(答案不是文件名时的四步流程) / 工具目录(只有名字+一句话, 细节在各工具 description 里) /
 *          跨工具规则 / 可点击输出 / 语法速查 / Lua 速查 — 全是"每轮都要用"的行为契约。
 *   附录 = 引擎内嵌 Lua 规范全文, 由 BuildInstructions 拼在骨架之后 (xjs_Query_GetPrompt(2)
 *          合集, 旧引擎无合集回落 0/1 拼接; 规范与骨架速查冲突时以规范为准的适配说明一并写入);
 *          get_lua_spec 工具保留为"重读"通道 (脚本报错排查/怀疑附录被截断时取全文)。
 *   新增工具一律: 加 tools JSON (描述里写全参数语义) + 工具目录加一行; 别再往这里堆细节。 */
static const wchar_t* AI_INSTRUCTIONS =
    L"## 角色与目标\n"
    L"你是“蜗牛快搜”内置的 AI 助手(agent 模式)。蜗牛快搜是 Windows 本地文件极速搜索工具：全盘秒级索引，"
    L"支持文件名/大小/时间/类型/别名/内容等搜索。全程用简体中文回答（包括所有面向用户的文字），简洁、准确、直接。\n"
    L"\n"
    L"## 工作方式\n"
    L"- 找文件/查文件/统计类任务：**先调用 run_search 实际执行搜索**，根据返回的条数/样本/ai.print 输出判断结果，"
    L"不符合就换口径再搜（先粗筛再精筛），绝不凭空编造结果；需要给用户看文件用 open_file，给路径清单用 copy_paths。\n"
    L"- 统计类必须**自己跑到出数**再回答（用户要的是数字与结论，不是脚本）；只有确实多次失败，"
    L"才把可粘贴的搜索式/脚本交给用户并说明卡在哪一步。\n"
    L"- run_command 是真实执行在用户机器上的命令：先想清楚再提交；权限为「询问」时该调用会**暂停等用户在确认卡上裁决**"
    L"（允许=自动继续执行并拿到输出，拒绝或 5 分钟未确认=本次调用失败）——等待期间不要重复调用同一工具；"
    L"被拒绝就放弃该思路并如实告知，绝不换写法绕过。\n"
    L"- 改设置/换皮肤/切语言/窗口管理：用对应代办工具提交——这类改动**不会直接生效**，而是列进\"待应用的调整\"卡片，"
    L"用户逐项点\"应用\"才执行。提交后用一句话请用户到卡片上确认，**绝不宣称已生效**，"
    L"**用户没让改就不要替用户提交任何调整**。搜索模式管理与任务类操作（搜索/打开文件/复制路径）仍直接执行。\n"
    L"- 工具调用轮数有限，别在一种写法上反复试错：同一口径连续两次拿不到有效数据，立即换搜索模式（或重读 Lua 规范附录）。\n"
    L"- 得到足够信息后用最终答复总结：找到什么、在哪、关键数据；推荐执行的搜索用 xjs:// 搜索链接给出（见《可点击输出》）。\n"
    L"- 用户消息可能附带图片/视频/音频（能否读取取决于模型能力）：图片直接看内容；涉及图片内容的问题据实描述，\n"
    L"  看不清或图里没有的信息如实说明，绝不编造。\n"
    L"- 与任务无关的问题直接回答，不要调用工具。\n"
    L"\n"
    L"## 模糊/联想类需求（答案不是文件名时）\n"
    L"\"帮我找一下黄圣依参演过的电影\"这类问题，答案不是文件名——把原句整段当搜索词必然搜不到东西。按四步走：\n"
    L"1. 提取实体：从问题里挑出关键对象（人物/作品/系列/歌手/作者/公司/地点等），例：黄圣依。\n"
    L"2. 联想候选：用你自己的知识把需求展开成**具体候选清单**——人物→其参演/执导/演唱的作品名；系列→各部名称；"
    L"歌手→专辑/歌名；作者/公司→著作/出品名。把握大的排前面。\n"
    L"3. 逐个实搜（run_search）：按候选名搜——wildcard 直接写名称（自动包含匹配）或 SQL `FName ILIKE '%片名%'`；"
    L"顺手把实体名本身也搜一遍兜底（文件名/目录里常带演员名/系列字样）；名字太常见、结果太宽泛时加条件收窄"
    L"（FileType/Ext/目录/年份），易混候选优先用带年份/副标题的完整名。一轮可以连发多个不同候选的搜索，省轮数。\n"
    L"4. 汇总诚实作答：只列磁盘真实命中的文件（FileId 链接），某候选命中多时报总数；知识里有但没搜到的明确写"
    L"\"索引中未找到\"；联想不确定的说明依据——绝不编造文件名、绝不把联想当搜索结果。\n"
    L"- 轮数有限：优先搜把握大的候选，结果足够成答就收尾，别耗在弱把握候选上。\n"
    L"\n"
    L"## 工具目录（只有名字与一句话；参数细节看各工具的 description，用前先读）\n"
    L"- get_lua_spec：重新取 Lua 脚本规范全文（规范已内置在本提示词文末附录，写脚本前先读附录；一般无需调用）——"
    L"只在脚本报错要重读规范、或怀疑附录被截断时调用。\n"
    L"- run_search：引擎内执行一次搜索（5 种模式，语法见《搜索语法速查》；Lua 统计经 ai.print、数据行经 ai.row 回传）。\n"
    L"- run_command：执行一条 Windows 命令（cmd/powershell）拿真实输出——诊断、系统信息、搜索覆盖不到的批量操作；"
    L"受用户\"命令执行\"权限档约束（禁用=拒绝，询问=命令出确认卡、用户点允许后自动继续执行，允许=直接执行）。\n"
    L"- open_file / copy_paths：把搜索样本中的文件打开/定位给用户看 / 复制路径清单到剪贴板。\n"
    L"- get_author_and_donate：关于作者/软件背景的权威介绍；用户想捐赠/赞赏时也用它取二维码引用（竖排显示在对话页）。\n"
    L"- list_windows / get_window_state / set_window_settings / control_window / create_window：窗口查看与代办（改动经\"待应用的调整\"卡片，用户点应用才生效）。\n"
    L"- get_global_settings / set_global_settings / list_skins：全局设置读写（改经卡片）/ 皮肤名清单。\n"
    L"- get_window_selection：读某窗口当前选中的文件 (FileId+文件名, 同 run_search 口径不含路径)。用户指\"选中的/这些文件\"要做判断、统计或批量操作建议时用它。\n"
    L"- list_languages / get_language / set_language：界面语言清单 / 查询 / 切换（代码 auto|zh|zh-TW|en|ko|th|ms，切换经卡片，用户点应用才生效）。\n"
    L"- set_search：把关键词置入用户窗口的搜索框并执行（run_search 是你的私有搜索，不动用户界面）。\n"
    L"- list_modes / apply_mode / add_search_mode / remove_search_mode：搜索模式查看/执行/增删。\n"
    L"- list_plugins / send_plugin_message：发现其它插件 / 与它们互发 JSON 消息。\n"
    L"\n"
    L"## 代办工具跨条规则\n"
    L"- window 参数一律填窗口名称（list_windows 查），留空 = 当前对话所在的窗口。\n"
    L"- 换皮肤先 list_skins 拿有效名；settings 里的搜索模式值是 lua|lua-exec（与 run_search 的 lua_filter|lua_exec 不同名，别混）。\n"
    L"- control_window 的 dismiss：主窗=藏托盘（不是退出），子窗=真关闭；create_window 在索引扫描期会被拒绝，让用户稍后再试。\n"
    L"- send_plugin_message 需对方插件已启用并实现收信口；对方无回复属正常（载荷约定取决于对方插件）。\n"
    L"\n"
    L"## 回答格式（GFM）\n"
    L"- 一律用 GFM（GitHub Flavored Markdown）输出：**表格前空一行**、表头下一行是 |---|---| 分隔行、表格后再空一行；\n"
    L"  支持删除线 ~~…~~、任务列表 - [x]、``` 围栏代码块；不要输出 HTML 标签（渲染层会丢弃）。\n"
    L"\n"
    L"## 可点击输出（你的回答会被渲染成交互界面）\n"
    L"你的回答里有四种元素会被渲染成用户可直接点击的控件。前两种一律用**标准 Markdown 链接语法**，\n"
    L"链接文字写成给用户看的动作指引（不要裸放搜索词/路径当文字）：\n"
    L"1. 搜索指令：[链接文字](xjs://search?text=<URL编码的搜索词>&mode=<wildcard|regex|sql|lua|lua-exec>)。\n"
    L"   用户点击即把搜索词置入搜索框并按该模式执行。mode 可省略 = 沿用窗口当前模式；\n"
    L"   **mode 只接受 wildcard|regex|sql|lua|lua-exec 五个值**（lua=逐文件过滤的 Lua 脚本；\n"
    L"   lua-exec=执行模式，脚本自主遍历 db 表并必须 return ID 数组——跨文件聚合/统计/自定义排序用这个）；\n"
    L"   lua_filter/lua_exec 是 run_search 工具的枚举值：lua_filter 链接里写 lua、lua_exec 链接里写 lua-exec。\n"
    L"   text 的值要 URL 编码（空格=%20，&=%26，#=%23；中文/通配符可直接写）。\n"
    L"   示例：[🔍 搜索所有 PNG 图片](xjs://search?text=*.png&mode=wildcard)、"
    L"[🔍 找出大于 100MB 的视频](xjs://search?text=SELECT%20Path%20FROM%20alltable%20WHERE%20Size%20%3E%20'100M'&mode=sql)。\n"
    L"   凡要给\"可执行的搜索式\"一律用这种链接；链接里的 lua 脚本写成**单行紧凑形式**（语句用分号衔接，\n"
    L"   不用 -- 行注释——搜索框是单行显示）；过长塞不进链接的脚本改用普通代码块给出。\n"
    L"2. 文件动作：[文件名](xjs://open?id=<FileId>)、[在资源管理器中定位 文件名](xjs://reveal?id=<FileId>)。\n"
    L"   id = 工具结果 files 里给出的 FileId 数字，**只能是纯数字，逐字照抄**——带任何其它字符\n"
    L"   （如\"4396180附近\"、\"≈4396180\"）的链接点不开；记不清/没看到的 ID 宁可不放链接，绝不估写。\n"
    L"   链接文字写文件名（照抄工具结果给的名称，不要自造），不要把 ID 数字或文件名裸放在正文里当引用。\n"
    L"3. 文件只按 ID 引用：工具结果给你的只有 [FileId, 文件名]，**没有、也不需要完整路径**——\n"
    L"   绝不在回答里拼凑、猜测或编造路径；打开/定位/复制路径都由程序按 ID 自动完成。\n"
    L"   样本只给前 20 条：排在其后的文件你**没有 ID**，不放链接，如实写明\"仅列前 20\"即可。\n"
    L"   确实需要路径细节时（如按目录写脚本）用 ai.row(id,\"路径\") 让脚本自取，回答里仍只放 ID 链接。\n"
    L"4. 工具结果里确实存在的完整绝对路径（如 ai.row 请求了\"路径\"字段）直接写出也会自动渲染为\n"
    L"   可点击链接：单击=打开，右键=打开/定位/复制路径。**路径是精确数据，逐字符照抄，绝不缩写**——\n"
    L"   不得用 ... / … 省略中间字符、不得增删空格或改写标点、不得繁简/全半角转换；表格再挤也写全\n"
    L"   （必要时让该列换行）。缩写过的路径在磁盘上不存在，点击打不开，等于没给。\n"
    L"   表格/清单里要可点击的条目优先用第 2 条的 ID 链接，不受路径精确性约束。\n"
    L"5. 网页链接照常 [标题](https://...)，点击用系统浏览器打开。\n"
    L"\n"
    L"## 搜索语法速查（run_search 的 mode）\n"
    L"- wildcard 通配符（日常默认）：* 任意长度、? 单个字符；不含 * ? 时自动按包含匹配；支持拼音首拼/全拼（wd 命中 文档.docx）；"
    L"空格=且，|=或；搜索词含 \\\\ 或 / 时按完整路径匹配。\n"
    L"- regex 正则（PCRE2）：如 ^[0-9]{4}-报告.*\\.docx$。\n"
    L"- sql（功能最强，类 PostgreSQL，仅单表 alltable）：SELECT Path FROM alltable WHERE Size > '100M' AND ModTime > NOW() - INTERVAL '7 days' ORDER BY Size DESC LIMIT 100。\n"
    L"  运算符：LIKE/NOT LIKE/ILIKE（% 任意长度、_ 单字符；字符串 = 比较默认区分大小写，不区分用 ILIKE）；"
    L"正则 ~（区分大小写）/~*（不区分）/~ !~（取反），如 FName ~ '^[0-9]{4}'；IN 仅常量列表；"
    L"GROUP BY/COUNT/HAVING（HAVING 仅单条 COUNT 比较）/CASE WHEN/CTE(WITH)；尺寸简写 '100M' '2G'；"
    L"时间：ModTime >= CURRENT_DATE - INTERVAL '7 days'（或 '1 month'），精确时间段用 CAST('2025-01-01' AS int)（不支持 '值'::类型）。\n"
    L"  常用字段：ID、ParentID、Path、FName（**不含扩展名**）、Ext（**扩展名不含点**，是 docx 不是 .docx）、"
    L"ParentName/ParentPath（直接父目录名/路径；**ParentPath = 是精确匹配只查直接子项，ParentPath LIKE 'D:\\\\x\\\\%' 才含全部后代**，"
    L"ParentPath LIKE '_:' = 各盘根）、AnyParent（任意层级父目录名，如 AnyParent ILIKE 'Work'）、Size、CreateTime、ModTime、"
    L"AccessTime、FileType（**只认 = / !=，不支持 LIKE**：视频/音频/图片/文档/办公/程序/压缩/系统/其他）、IsDir（1=目录 0=文件）、"
    L"Alias、Score、FAttr（属性串正则匹配：S=系统 H=隐藏 R=只读 D=目录…，**排除系统+隐藏 = FAttr !~ '[SH]'**——"
    L"用户没有特殊说明的统计/清单默认加上）、FileContent（**文件全文内容**，**严禁单独作 WHERE 条件**，见下方内容搜索规则）。\n"
    L"  不支持：窗口函数(OVER)/写操作(UPDATE/DELETE/INSERT)/DDL/多表 FROM/EXISTS/VALUES/SELECT INTO/多语句。\n"
    L"- lua_filter 过滤模式：对每个文件做一次真值判断的 Lua 脚本；**每个文件一条线程并发求值，文件间顺序不定**——"
    L"脚本必须无状态，聚合统计一律换 lua_exec。\n"
    L"- lua_exec 执行模式：脚本全权遍历数据库/跨文件聚合/自定义排序，return 的 ID 数组即结果。\n"
    L"  **脚本必须有顶层 return ID 数组**——插件提交前静态校验，缺顶层 return 直接拒绝执行（不提交引擎）；"
    L"统计类任务也要把涉及/选中的文件 ID 数组 return 回来（外部靠 return 的数组得知脚本选中了哪些），数字本身走 ai.print。\n"
    L"选择建议：找名字用 wildcard/regex；按字段组合筛选用 sql；逐文件自定义判断用 lua_filter；"
    L"**计数/分组/排名/占比等一切统计类问题直接用 lua_exec**（统计数字经 ai.print 拿回来）。\n"
    L"- SQL 聚合的结果到不了你手里：run_search 只回命中总数与样本清单，GROUP BY/COUNT 的结果行拿不回来"
    L"（聚合查询 count 恒为 1）——**一切要出数字的统计改用 lua_exec**（数字经 ai.print 拿回来）；"
    L"给用户手动执行的搜索不受此限（CASE WHEN/CTE/IN (SELECT…) 引擎都支持）。\n"
    L"- LIKE 里 \\ 是转义字符，**匹配路径分隔符 \\ 必须写成 \\\\**（单写一个 \\ 再跟普通字符会匹配 0 条）；"
    L"按目录前缀缩小范围：`Path LIKE 'D:\\\\蜗牛快搜\\\\蜗牛快搜(D2D)%'`。\n"
    L"- 文件内容搜索 = FileContent 条件。**严禁 FileContent 单独作 WHERE 条件**——如 "
    L"`SELECT * FROM alltable WHERE FileContent LIKE '%搜索%'` 这种没有任何前置条件的写法是**禁止**的：\n"
    L"  它会读取所有分区每一个文件的内容，极慢。**必须**先缩小范围，只允许两种形态：\n"
    L"  ① 加路径前置条件（推荐）：`SELECT * FROM alltable WHERE Path LIKE 'D:\\\\蜗牛快搜\\\\蜗牛快搜(D2D)%' AND FileContent LIKE '%搜索%'`；\n"
    L"  ② 指定少量 ID：`SELECT * FROM alltable WHERE ID IN (0,2,5) AND FileContent LIKE '%搜索%'`。\n"
    L"  用户没给范围时，先按文件名/目录粗筛定位目录，或如实告知需要范围；绝不发全盘内容搜索。\n"
    L"  内容条件**放 WHERE 末位**，先让便宜的属性条件过滤（如 `Ext IN ('txt','md') AND Path LIKE 'D:\\\\x\\\\%' AND FileContent ~* '关键词'`）；"
    L"内容也支持正则（~* 不区分大小写）；`GROUP BY MD5(FileContent)` 可按内容查重（流式 MD5，单文件读取上限 200MB）。\n"
    L"\n"
    L"## 文件与目录必须区分（分析口径）\n"
    L"索引同时收录**文件和目录（文件夹/盘符）**，搜索结果默认两类混排，count 与样本清单都是混合口径。"
    L"分析时必须分清对象究竟是文件还是目录，禁止把目录当文件、把文件当目录：\n"
    L"- 类型只能靠字段判断：SQL 用 `IsDir`，Lua 用 `f.isdir()`。**不要凭后缀猜**——目录名可以带点，"
    L"无后缀的名字不一定是目录；样本只有 FileId 和文件名，本身不带类型标志。\n"
    L"- 用户问\"文件\"=必须排除目录：SQL 加 `AND IsDir=0`；lua_filter 脚本开头 `if f.isdir() then return false end`；"
    L"lua_exec 统计时按 `f.isdir()` 把文件/目录分开计数。\n"
    L"- 用户问\"文件夹/目录\"=只算目录：SQL `IsDir=1`，Lua `f.isdir()`。\n"
    L"- 用户没指明类型（如\"这里有多少东西/占多大空间\"）时，把文件与目录分开说明或注明口径，别混作一个数。\n"
    L"- wildcard/regex 不能按类型过滤，其 count 是文件+目录混算，**不能直接当\"文件数/文件清单\"回答**；要文件口径就换 sql 或 lua 重查。\n"
    L"- 统计文件数/总大小/最大文件一律先排除目录（IsDir=0），目录条目不参与\"文件\"的计数与求和；"
    L"给用户的清单里目录行要标明是目录（如 📁 前缀），不要一律写成\"文件\"。\n"
    L"\n"
    L"## Lua 脚本速查（全文规范见本提示词文末附录，写脚本前先读附录；此处只列 agent 环境差异与高频要点）\n"
    L"- 文件属性经 f 表 / db 表访问器取：f.ext()=**不带点小写扩展名**（docx，无后缀空串）、f.isdir()、f.size()、f.fpath()（最贵放最后）；"
    L"lua_exec 全库遍历只用 db.ids()/db.files()，**禁止按数字范围枚举 ID**（ID 是稀疏槽位，必踩空槽漏文件）。\n"
    L"- 过程输出：引擎规范里\"数据走 print\"的说法对你不适用（print 进引擎调试输出，工具结果拿不到）；"
    L"你的工具环境注册了 **ai.print(...)**（与 print 同款多参数，可多次调用，参数可为字符串/数字/表），"
    L"输出会作为本次工具结果 JSON 的 output 字段原样回传——统计数字/逐目录计数/过程日志一律经它；\n"
    L"- 数据行：要把文件清单（含属性）给用户看时用 **ai.row(id, \"字段名\", ...)** 逐条压行。字段名可任意"
    L"组合：名称 / 路径 / 大小 / 修改时间 / 创建时间 / 访问时间 / 扩展名（不含点） / 目录 / 类型 / 属性 / 别名 / 评分"
    L"（不带字段实参 = id+名称；字段名就是输出 JSON 的键）。行进工具结果 JSON 的 rows 数组，"
    L"**每行是只含请求字段的 JSON 对象**（如 {\"id\":123,\"大小\":1048576,\"名称\":\"a.docx\"}，时间=epoch 秒）；"
    L"索引未开启的字段整键省略（rows 首元素有提示），行数过多会截断。清单展示优先 ai.row，别用 ai.print 手拼行。\n"
    L"- 文件导出（仅 lua_exec，仅 agent 工具环境）：**ai.read(路径)**=读文本文件返回内容（≤8MB，二进制拒绝）；"
    L"**ai.write(路径, 内容)**=登记写出（脚本结束后系统自动完成写盘，结果在工具结果 JSON 的 writtenFiles/writesNote）；"
    L"**ai.saveas(内容, \"默认文件名.csv\")**=弹系统保存对话框让用户选位置（用户选定=授权，取消=不写）。\n"
    L"  内容格式自动判定：**二维表（表的表）→ CSV**（UTF-8 带 BOM，Excel 直开；子表为字典时取键并集做表头，"
    L"统计/清单导出首选这种结构）；**其它表 → JSON**；**字符串/数字 → 原样文本**。返回值：成功=true，失败=nil+原因。\n"
    L"  路径必须是**绝对路径**；导出位置建议 桌面/下载/文档 下用户能找到的目录；目标已存在时用户会收到"
    L"覆盖确认（拒绝=该文件不写）；文件功能受用户\"文件操作\"权限档约束（禁用/只读=拒绝）。\n"
    L"  **ai.write/ai.saveas 必须检查返回值**（本地 ret = ai.saveas(...)；ret 为 nil 时把原因如实告诉用户并停止宣称导出）。\n"
    L"  是否真的写盘以**工具结果 JSON 的 writtenFiles（已写出清单）/ writesNote（取消·拒绝·失败经过）为准**——\n"
    L"  结果里没有 writtenFiles 就**绝不可以说\"已导出/已保存\"**（弹窗可能被用户错过或权限被拒，诚实报告经过）。\n"
    L"return 硬规则：lua_exec 必须以**顶层 return ID 数组**结尾（插件静态校验，缺顶层 return 拒绝执行；"
    L"包在 if/function 里的 return 不算——主流程必须有兜底 return）；lua_filter 逐文件返回真值。\n"
    L"- 脚本沙箱删除了 io/os 等库；API 全集以文末附录规范为准，绝不虚构函数。\n"
    L"- **交给用户运行的脚本**（写在回答里的代码块或 xjs://search 链接，不经你执行）：必须写 return ID 数组"
    L"（漏写 return 界面一条结果都不显示）；**禁止调用 ai.print / ai.read / ai.write / ai.saveas / ai.row"
    L"（用户侧没有这些函数，调用即报错）——要把数据导出给用户就自己 lua_exec 跑 ai.write/ai.saveas；"
    L"过滤模式脚本没有用户侧入口（搜索框的 lua 模式就是执行模式），别生成 lua_filter 的搜索链接；"
    L"把脚本交给用户仅限用户明确要脚本本身的场合。\n";

/* 工具定义 (Responses API tools 数组; 与 AgentToolExec 的名字/参数一一对应) */
static const char* AI_TOOLS_JSON = R"json([
  {"type":"function","name":"run_search","description":"在蜗牛快搜索引中执行一次搜索, 返回命中总数与前 20 条样本。结果 JSON: count=命中总数, elapsedMs=耗时毫秒, files=[[FileId,文件名],…] (FileId=引擎文件 ID, 是文件的唯一引用方式: 回答里的文件动作链接 xjs://open|reveal?id= 填它, 文件名仅用于展示), output=ai.print 输出 (仅 Lua 模式有)。结果同时含文件与目录(文件夹), count/files 均为混合口径: 涉及\"文件\"口径的分析必须先按 IsDir=0 / f.isdir() 过滤, 不得拿混合 count 当文件数。可多次调用逐步逼近目标 (先粗筛再精筛)。5 种 mode 的搜索词语法以系统提示词中的说明为准; lua 两种模式写脚本前先读系统提示词文末的 Lua 规范附录; lua_exec 脚本必须有顶层 return ID 数组, 缺顶层 return 会被拒绝执行 (不提交引擎)。Lua 模式脚本内用 ai.print(...) 输出的统计/过程信息附在结果 JSON 的 output 字段; 数据行用 ai.row(id,\"字段名\",...) 逐条压入 (字段=名称/路径/大小/修改时间/创建时间/访问时间/扩展名/目录/类型/属性/别名/评分, 不带字段实参=id+名称), 结果 JSON 的 rows 字段是行对象数组 (只含请求字段, 时间=epoch 秒, 索引未开启的字段省略并在首元素提示)。lua_exec 脚本内还可用 ai.read/ai.write/ai.saveas 读文件/导出结果 (二维表自动转 CSV, 覆盖需用户确认, 详见系统提示词); 写出经过以结果 JSON 的 writtenFiles/writesNote 字段回传, 未确认写出成功的文件不要向用户宣称已保存。用户开启「结果同步」时, 本次命中的全部 FileId 会自动重置进其窗口的搜索结果列表 (用户界面立即可见; 结果为 0 = 同步清空该列表)。","parameters":{"type":"object","properties":{"mode":{"type":"string","enum":["wildcard","regex","sql","lua_filter","lua_exec"],"description":"wildcard=通配符 regex=PCRE2正则 sql=SELECT语句 lua_filter=过滤模式(Lua 逐文件判断) lua_exec=执行模式(Lua 程序接管搜索)"},"query":{"type":"string","description":"搜索词/脚本全文 (lua 两种模式传完整脚本文本)"}},"required":["mode","query"]}},
  {"type":"function","name":"run_command","description":"执行一条 Windows 命令 (cmd 或 powershell, 静默后台运行不弹窗) 并返回真实输出。用于诊断 (ipconfig/ping/systeminfo)、系统信息查询、以及搜索工具覆盖不到的批量/外部操作。受用户命令执行权限档约束: 「禁用」一律拒绝; 「询问」时本次调用会**暂停**, 命令展示给用户出确认卡 — 用户点「允许一次」后自动继续执行并返回输出 (等待期间不要重复调用), 点「拒绝」或 5 分钟未确认则本次调用以失败返回; 失败后不要换写法重试同类命令, 直接说明并放弃。高危命令 (格式化/递归删除/改注册表/下载执行等) 会在确认卡上标记提醒用户。返回文本: stdout 原文; 有 stderr 时附 [stderr] 分节; 末行 [exit code: N] 仅在非零退出时出现; [timed out ...] = 超时已被强杀; 输出过长只保留尾部并注明丢弃量。相对路径操作发生在 workdir (默认临时目录)。","parameters":{"type":"object","properties":{"command":{"type":"string","description":"要执行的命令 (cmd 语法; shell=powershell 时传 PowerShell 语句)。多语句用 cmd 的 & 或 PowerShell 的 ; 连接"},"shell":{"type":"string","enum":["cmd","powershell"],"description":"cmd=cmd.exe (默认); powershell=Windows PowerShell"},"description":{"type":"string","description":"一句话说明这条命令做什么 (≤50 字; 会展示给用户帮助其判断是否放行)"},"workdir":{"type":"string","description":"工作目录 (绝对路径; 默认临时目录)。相对路径操作前先设好它"},"timeoutMs":{"type":"integer","description":"超时毫秒 (3000~600000, 默认 120000), 超时进程树被终止"}},"required":["command","description"]}},
  {"type":"function","name":"get_lua_spec","description":"重新获取 Lua 脚本规范全文 (纯文本)。规范全文已内置在系统提示词文末附录, 正常无需调用 — 仅在脚本报错需要重读规范、或怀疑附录被截断时调用。默认返回合集 (两种模式合并去重版); 引擎没有合集时才需要用 mode 单取一份。","parameters":{"type":"object","properties":{"mode":{"type":"string","enum":["lua_filter","lua_exec"],"description":"仅引擎无合集时才需要: 单取哪一份规范"}},"required":[]}},
  {"type":"function","name":"get_author_and_donate","description":"关于作者/软件背景的问题 (作者是谁/这是什么软件/授权与特性), 或用户想捐赠/赞赏/请作者喝咖啡时调用。返回软件与授权的权威介绍 (据此回答, 不编造) 与捐赠二维码的引用方式: 在回答正文里用图片语法 ![微信捐赠码](xjs://donate?kind=wechat) / ![支付宝捐赠码](xjs://donate?kind=alipay), 二维码竖排显示在对话页 (微信优先放最前)。只引用返回中列出的可用项; 图片本体不经过对话文本, 不要把 base64/文件路径写进回答。","parameters":{"type":"object","properties":{},"required":[]}},
  {"type":"function","name":"open_file","description":"打开最近一次 run_search 样本列表中的某个文件 (在用户屏幕上打开/定位), 用于让用户直接看到该文件。","parameters":{"type":"object","properties":{"index":{"type":"integer","description":"样本列表序号 (1 起)"},"reveal":{"type":"boolean","description":"true=只在资源管理器中定位, 不打开"}},"required":["index"]}},
  {"type":"function","name":"copy_paths","description":"把最近一次 run_search 的前 100 条完整路径 (每行一条) 复制到剪贴板, 供用户粘贴。","parameters":{"type":"object","properties":{},"required":[]}},
  {"type":"function","name":"list_windows","description":"列出当前全部搜索窗口 (令牌/名称/是否主窗/档案槽)。其它代办工具的 window 参数都填这里的\"名称\"。","parameters":{"type":"object","properties":{},"required":[]}},
  {"type":"function","name":"get_window_state","description":"查看一个搜索窗口的完整状态与设置 (视图/页面缩放/皮肤/预览/预览宽度/置顶/搜索模式/搜索词/结果数/选中数/失焦行为/显示开关/任务栏图标/鼠标打开/默认选中/窗口矩形等)。","parameters":{"type":"object","properties":{"window":{"type":"string","description":"窗口名称 (list_windows 查; 留空=当前对话所在窗口)"}},"required":[]}},
  {"type":"function","name":"set_window_settings","description":"提交对一个搜索窗口的设置修改。**不会直接生效**: 每个键列成\"待应用的调整\"卡片, 用户点\"应用\"才逐项执行 (可忽略)。settings 对象的键全部可选但必须合法, 一个未知键/非法值在应用时该键失败: 视图=list|details|medium|large; 页面缩放=50~200(百分数); 皮肤=皮肤名(先 list_skins 查); 预览=布尔; 预览宽度=160~2000; 置顶=布尔; 失焦行为=0(无)|1(失焦关闭窗口); 显示控制按钮/显示筛选框/显示状态栏/任务栏图标=布尔; 鼠标打开=0(双击)|1(单击); 默认选中=0(不选)|1(自动选第一个); 搜索模式=wildcard|regex|sql|lua|lua-exec; 语言=auto|zh|zh-TW|en|ko|th|ms。","parameters":{"type":"object","properties":{"window":{"type":"string","description":"窗口名称 (留空=当前对话所在窗口)"},"settings":{"type":"object","description":"要修改的设置键值对 (子集随意)"}},"required":["settings"]}},
  {"type":"function","name":"get_global_settings","description":"读取全局设置 (双击Ctrl目标/绘制引擎)。","parameters":{"type":"object","properties":{},"required":[]}},
  {"type":"function","name":"set_global_settings","description":"提交对全局设置的修改。**不会直接生效**: 列成\"待应用的调整\"卡片, 用户点\"应用\"才逐项执行。键: 双击Ctrl目标=\"\"(禁用)|\"默认窗口\"|档案名; 绘制引擎=\"d2d\"|\"gdiplus\"(应用后重启生效)。","parameters":{"type":"object","properties":{"settings":{"type":"object","description":"要修改的全局设置键值对"}},"required":["settings"]}},
  {"type":"function","name":"list_skins","description":"列出全部可用皮肤名 (set_window_settings 的\"皮肤\"键只接受这些名字)。","parameters":{"type":"object","properties":{},"required":[]}},
  {"type":"function","name":"get_window_selection","description":"读取一个搜索窗口当前选中的文件清单。结果 JSON: win=窗口名称, total=选中总数, files=[[引擎FileId,文件名],…] (FileId=文件的唯一引用方式, 回答里的文件动作链接 xjs://open|reveal?id= 填它; 不含路径); files 长度<total 时仅详列了前若干条。用户说\"我选中的这些/当前选中的文件\"要做判断、统计或给出批量操作建议时调用; 没有选中时 total=0。","parameters":{"type":"object","properties":{"window":{"type":"string","description":"窗口名称 (留空=当前对话所在窗口)"},"limit":{"type":"integer","description":"最多详列多少条 (默认 200; 选中数为全量, 超出部分不展开)"}},"required":[]}},
  {"type":"function","name":"list_languages","description":"列出全部可用界面语言 (代码 + 母语名称)。set_language 的 language 参数只接受这些代码 (另加 auto=跟随系统)。","parameters":{"type":"object","properties":{},"required":[]}},
  {"type":"function","name":"get_language","description":"查询一个搜索窗口当前的界面语言设置 (语言代码; auto=跟随系统)。","parameters":{"type":"object","properties":{"window":{"type":"string","description":"窗口名称 (留空=当前对话所在窗口)"}},"required":[]}},
  {"type":"function","name":"set_language","description":"提交切换一个搜索窗口的界面语言。**不会直接生效**: 列成\"待应用的调整\"卡片, 用户点\"应用\"才切换 (应用后所有窗口标题各自按新语言刷新并落盘)。语言代码先 list_languages 查 (用户说的是\"中文/英文/泰语\"这类母语名, 映射成代码再调)。","parameters":{"type":"object","properties":{"window":{"type":"string","description":"窗口名称 (留空=当前对话所在窗口)"},"language":{"type":"string","enum":["auto","zh","zh-TW","en","ko","th","ms"],"description":"语言代码 (auto=跟随系统)"}},"required":["language"]}},
  {"type":"function","name":"control_window","description":"提交对一个搜索窗口的界面动作。**不会直接生效**: 列成\"待应用的调整\"卡片, 用户点\"应用\"才执行。show=唤起到前台; dismiss=窗口消失 (主窗藏托盘/子窗真关闭); openSettings=打开设置窗口并绑定该窗口。","parameters":{"type":"object","properties":{"window":{"type":"string","description":"窗口名称 (留空=当前对话所在窗口)"},"action":{"type":"string","enum":["show","dismiss","openSettings"],"description":"界面动作"}},"required":["action"]}},
  {"type":"function","name":"create_window","description":"提交一个创建搜索窗口的提案。**不会直接生效**: 列成\"待应用的调整\"卡片, 用户点\"应用\"才创建 (该档案已打开则只激活; 索引扫描期应用会失败)。","parameters":{"type":"object","properties":{"profile":{"type":"string","description":"窗口档案名 (留空=新建空白档案)"},"inherit":{"type":"string","description":"继承尺寸的窗口名称 (留空=默认窗口)"}},"required":[]}},
  {"type":"function","name":"set_search","description":"把关键词置入用户搜索窗口的搜索框并执行搜索 (用户立即可见)。与 run_search 的区别: run_search 是你私有的搜索, 不动用户界面; 要把某个搜索放进用户的窗口时用它。","parameters":{"type":"object","properties":{"window":{"type":"string","description":"窗口名称 (留空=当前对话所在窗口)"},"keyword":{"type":"string","description":"搜索词 (留空=保持现词)"},"mode":{"type":"string","enum":["wildcard","regex","sql","lua","lua-exec"],"description":"搜索模式 (留空=沿用窗口当前模式; lua-exec 时 keyword 必须是含顶层 return ID 数组的完整脚本, 缺顶层 return 拒绝置入)"},"execute":{"type":"boolean","description":"false=只填词不搜索 (默认 true=立即搜索)"}},"required":[]}},
  {"type":"function","name":"list_modes","description":"列出一个窗口可用的全部搜索模式 (用户自定义+插件提供), 含标识/名称/类型/模板/来源。","parameters":{"type":"object","properties":{"window":{"type":"string","description":"窗口名称 (留空=当前对话所在窗口)"}},"required":[]}},
  {"type":"function","name":"apply_mode","description":"按搜索模式执行搜索 (语义=用户在药丸菜单点了该模式): 模板型把 input 置入搜索框转标签链执行; 插件接管型触发该插件。","parameters":{"type":"object","properties":{"window":{"type":"string","description":"窗口名称 (留空=当前对话所在窗口)"},"mode_id":{"type":"string","description":"模式标识 (list_modes 返回的\"标识\")"},"input":{"type":"string","description":"输入词 (留空=窗口现词)"}},"required":["mode_id"]}},
  {"type":"function","name":"add_search_mode","description":"添加一个会话级模板型搜索模式 (本次运行内有效, 重启后消失; 返回其\"标识\")。template 必须含 <keyword> 占位符, 执行时替换为搜索框输入文字。","parameters":{"type":"object","properties":{"name":{"type":"string","description":"模式名 (≤64字)"},"type":{"type":"string","enum":["wildcard","regex","sql","lua"],"description":"模式类型 (默认 wildcard)"},"template":{"type":"string","description":"模板, 必须含 <keyword> 占位符。含 FileContent 时**必须**带路径前置条件 (FileContent 单独作条件 = 全盘读所有分区文件内容, 极慢, 禁止), 如 Path LIKE 'D:\\\\资料%' AND FileContent LIKE '%<keyword>%'"},"desc":{"type":"string","description":"简介 (≤256字)"}},"required":["name","template"]}},
  {"type":"function","name":"remove_search_mode","description":"删除你自己经 add_search_mode 添加的运行时搜索模式 (用户自定义/清单声明的模式删不了)。","parameters":{"type":"object","properties":{"mode_id":{"type":"string","description":"add_search_mode 返回的\"标识\""}},"required":["mode_id"]}},
  {"type":"function","name":"list_plugins","description":"列出全部已扫描插件 (标识/名称/版本/作者/启用/已加载)。","parameters":{"type":"object","properties":{},"required":[]}},
  {"type":"function","name":"send_plugin_message","description":"向另一个插件发送 JSON 消息并等它的同步回复 (消息经宿主中转; 对方需已启用并实现收信口, 载荷结构约定看对方插件)。","parameters":{"type":"object","properties":{"plugin_id":{"type":"string","description":"目标插件标识 (list_plugins 查)"},"payload":{"type":"object","description":"消息载荷 (JSON 对象)"}},"required":["plugin_id","payload"]}}
])json";

/* 系统提示词组装 (进程一次): 常驻骨架 + 引擎内嵌 Lua 规范全文附录 (2026-09-25 用户口径)。
   优先合集 promptType=2 (0+1 合并去重版); 旧引擎无合集回落 0/1 两份顺序拼接。
   附录头写 agent 环境适配说明: 规范原文是"产出脚本给用户"的口吻 (-3/-4 模式编号、print、
   输出格式章节), 与本 agent "自己写脚本自己跑" 的用法差异都在这里一次性说清。 */
static std::string g_instrA;
void BuildInstructions() {
    std::string s = U8(AI_INSTRUCTIONS);
    std::string spec;
    const char* p = xjs_Query_GetPrompt(2);
    if (p && *p) {
        spec = p;
    } else {
        const char* a = xjs_Query_GetPrompt(0);
        const char* b = xjs_Query_GetPrompt(1);
        if (a && *a) spec = a;
        if (b && *b) { if (!spec.empty()) spec += "\n\n"; spec += b; }
    }
    if (!spec.empty()) {
        s += "\n## 附录：Lua 脚本规范全文（引擎内嵌，上面的《Lua 脚本速查》只是要点，写脚本以本附录为准）\n";
        s += "读法适配（规范按\"给用户产出脚本\"的口吻撰写，与你这个 agent 的用法差异如下）：\n";
        s += "- 文中\"过滤模式 -3\" = run_search 的 lua_filter，\"执行模式 -4\" = lua_exec。\n";
        s += "- 文中 print 在你的工具环境是 **ai.print(...)**（print 本体进引擎调试输出，工具结果拿不到）；"
              "数据行另用 ai.row(id, \"字段名\", ...)，见《Lua 脚本速查》。\n";
        s += "- 文中《输出格式》《风格基准》等\"向用户交付脚本\"的章节，只在你**把脚本交给用户**时适用；"
              "你自己执行时把脚本全文直接经 run_search 提交即可，不必在回答里贴代码。\n";
        s += "- \"lua_exec 必须 return ID 数组\"对本插件额外收紧为**顶层 return**（插件静态校验，缺顶层 return 拒绝执行），见《Lua 脚本速查》。\n\n";
        s += spec;
        s += "\n";
    }
    g_instrA = s;
}

/* 运行环境快照 (每次请求实时采集, 由 AgentBuildBody 作为注入型 user 项拼在 input 末尾):
 * 当前时间 / 索引规模与状态 / 可选字段开关 / 搜索设置。引擎 7 个可选字段 (大小/时间×3/评分/别名/属性) 未必全开,
 * 不注入这份清单, 模型就会对未开启字段照常写 SQL/Lua —— 用户问"文件何时创建"而
 * 创建时间字段未开启 = 查询报错或空结果, 模型只能瞎猜。全部为引擎只读查询, 工作线程可调。
 * 恒放请求尾部不放 instructions: 快照带秒级时间每请求必变, 混进前缀会灭掉 provider 前缀缓存。 */
static std::wstring BuildEnvSnapshot() {
    std::wstring s = L"\n## 运行环境快照 (每次请求实时采集, 时间与字段口径以此为准)\n";
    SYSTEMTIME st;
    GetLocalTime(&st);
    static const wchar_t* WK[7] = { L"日", L"一", L"二", L"三", L"四", L"五", L"六" };
    wchar_t tb[80];
    swprintf(tb, 80, L"- 当前时间: %04u-%02u-%02u %02u:%02u:%02u 星期%s (本地时间)\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, WK[st.wDayOfWeek % 7]);
    s += tb;
    xjs_engine* eng = xjs_GetDefaultEngine();
    if (!eng) return s;
    const wchar_t* stTxt;
    switch (xjs_db_GetEngineState(eng)) {
        case XJS_DB_STATE_LOADING:   stTxt = L"数据库加载中 (暂无可用索引)"; break;
        case XJS_DB_STATE_SAVING:    stTxt = L"正在保存数据库"; break;
        case XJS_DB_STATE_SCANNING:  stTxt = L"正在扫描建索引 (结果不完整)"; break;
        case XJS_DB_STATE_SYNCING:   stTxt = L"正在同步文件变化"; break;
        case XJS_DB_STATE_SEARCHING: stTxt = L"正在搜索"; break;
        default:                     stTxt = L"空闲"; break;
    }
    wchar_t ib[128];
    swprintf(ib, 128, L"- 索引库: %d 个条目 (含文件夹与盘符), 引擎状态=%s\n",
             xjs_db_GetFileCount(eng), stTxt);
    s += ib;
    /* 7 个可选字段: 中文名 = xjs_db_IsFieldEnabled 的键, 括号内 = SQL/Lua 侧列名 */
    static const struct { const wchar_t* cn; const char* key; const wchar_t* col; } OPT[] = {
        { L"文件大小", "文件大小", L"Size" },
        { L"修改时间", "修改时间", L"ModTime" },
        { L"创建时间", "创建时间", L"CreateTime" },
        { L"访问时间", "访问时间", L"AccessTime" },
        { L"文件评分", "文件评分", L"Score" },
        { L"别名",     "别名",     L"Alias" },
        { L"文件属性", "文件属性", L"FAttr" },
    };
    std::wstring on, off;
    for (const auto& f : OPT) {
        std::wstring item = std::wstring(f.cn) + L"(" + f.col + L")";
        if (xjs_db_IsFieldEnabled(eng, f.key)) { if (!on.empty()) on += L"、"; on += item; }
        else                                   { if (!off.empty()) off += L"、"; off += item; }
    }
    s += L"- 已开启字段: " + (on.empty() ? std::wstring(L"(以上必建字段之外一个都没开)") : on) + L"\n";
    s += L"- 未开启字段: " + (off.empty() ? std::wstring(L"(无)") : off) + L"\n";
    s += L"- 未开启字段在 SQL 条件/排序、Lua 取值中均不可用 (查询报错或无此数据), 绝不要编造此类数值;\n";
    s += L"  用户问题依赖未开启字段时, 如实说明\"索引未开启该字段, 需在设置中开启并重建索引后再查\"。\n";
    s += L"- 代办工具的 window 参数留空 = 作用于当前对话所在的窗口。\n";
    EnterCriticalSection(&g_emitCs);
    std::wstring setW = g_srchSet8.empty() ? std::wstring() : W8(g_srchSet8.c_str());
    LeaveCriticalSection(&g_emitCs);
    if (!setW.empty())
        s += L"- 搜索设置: " + setW + L" (首拼/全拼/大小写口径以此为准)\n";
    return s;
}

/* 一轮 SSE 事件里攒下的 function_call (模型要求执行的工具) */
struct AiCall {
    std::string callId, name, args;
    int outIdx = -1;   /* SSE output_index: 一轮输出里 reasoning 等其它项也占号,
                          只能按值匹配槽位, 禁止当数组下标扩容 (会造出空名幻影调用) */
};

/* 按 output_index 找/建调用槽位; oi 缺失 = 归入最后一槽 (单调用流兼容口径)。
 * 返回指针仅在本次事件内立即使用, 跨事件可能因扩容失效。 */
static AiCall* AgentCallSlot(std::vector<AiCall>* v, const Jv* oi) {
    if (oi && oi->t == 2) {
        int idx = (int)oi->num;
        for (auto& c : *v) if (c.outIdx == idx) return &c;
        AiCall c;
        c.outIdx = idx;
        v->push_back(c);
        return &v->back();
    }
    if (v->empty()) { AiCall c; v->push_back(c); return &v->back(); }
    return &v->back();
}

/* tools 数组常量 → picojson 值 (进程一次; 线程安全 magic-static, BuildBody 直接嵌入) */
static const picojson::value& ToolsP() {
    static picojson::value p = [] {
        picojson::value v;
        JParseU8(v, AI_TOOLS_JSON);
        return v;
    }();
    return p;
}

/* 每轮请求体: input = 对话快照 + 已发生的工具往返 (call→output 交错) + 环境快照 +
   循环护栏提醒 + 本轮模型产出; withTools=false = 收尾轮 (省略 tools 并明示直接回答)。
   省两个大头的口径 (见"工具结果省 token"节):
   - liveFrom 之后的输出 = 本轮刚产出, 原样回喂; 之前的 = 早先轮次已送达过, 中段裁剪;
   - instructions 恒为静态骨架, 环境快照 (带秒级时间, 每请求必变) 挪到 input 末尾 —
     instructions+tools+历史前缀逐字节稳定, provider 前缀缓存才命得到。
   - inject (重复调用提醒等护栏注入) 恒排快照之后 = 全请求最末位, 对下一轮显著性最高。 */
static std::string AgentBuildBody(AiJob* j, const std::vector<AiCall>& accCalls,
                                  const std::vector<std::string>& accOuts, size_t liveFrom,
                                  const std::wstring& inject, bool withTools) {
    auto userItem = [](const std::wstring& text) {   /* 注入型 user 项 (环境快照/护栏提醒) */
        picojson::object content;
        content["type"] = picojson::value("input_text");
        content["text"] = JS(text);
        picojson::array ca;
        ca.push_back(picojson::value(content));
        picojson::object item;
        item["role"] = picojson::value("user");
        item["content"] = picojson::value(ca);
        return picojson::value(item);
    };
    picojson::array input;
    for (auto& m : j->hist) {
        /* content 数组 = 文字 + 多模态附件 (图片→input_image; 视频/音频→input_file data URL)。
         * 附件按活动档案能力勾选取舍: 关了就不发对应 part (切档后的旧消息不炸请求);
         * dataUrl 空 = 已被历史存储预算清空的占位, 跳过。 */
        picojson::array ca;
        if (!m.text.empty()) {
            picojson::object content;
            content["type"] = picojson::value(m.role ? "output_text" : "input_text");
            content["text"] = JS(m.text);
            ca.push_back(picojson::value(content));
        }
        if (m.role == 0) {
            for (auto& a : m.atts) {
                if (a.dataUrl.empty()) continue;
                picojson::object part;
                if (a.kind == 0) {
                    if (!g_cfg.img) continue;
                    part["type"] = picojson::value("input_image");
                    part["image_url"] = JS(a.dataUrl);
                    part["detail"] = picojson::value("auto");
                } else {
                    if (a.kind == 1 ? !g_cfg.video : !g_cfg.audio) continue;
                    part["type"] = picojson::value("input_file");
                    part["filename"] = JS(a.name.empty() ? (a.kind == 1 ? std::wstring(L"video.bin")
                                                                        : std::wstring(L"audio.bin"))
                                                         : a.name);
                    part["file_data"] = JS(a.dataUrl);
                }
                ca.push_back(picojson::value(part));
            }
            if (ca.empty() && !m.atts.empty()) {   /* 有附件但全部不可发 (切到能力全关的档案): 文字占位 */
                picojson::object content;
                content["type"] = picojson::value("input_text");
                content["text"] = JS(std::wstring(L"(该条消息附带了图片/视频/音频, 但当前模型未开启对应输入能力, 内容不可见)"));
                ca.push_back(picojson::value(content));
            }
        }
        if (ca.empty()) continue;
        picojson::object item;
        item["role"] = picojson::value(m.role ? "assistant" : "user");
        item["content"] = picojson::value(ca);
        input.push_back(picojson::value(item));
    }
    for (size_t i = 0; i < accCalls.size() && i < accOuts.size(); i++) {
        picojson::object call;
        call["type"] = picojson::value("function_call");
        call["call_id"] = JS(W8(accCalls[i].callId.c_str()));
        call["name"] = JS(W8(accCalls[i].name.c_str()));
        call["arguments"] = JS(W8(accCalls[i].args.c_str()));
        input.push_back(picojson::value(call));
        std::string out8 = i < liveFrom ? ToolOutputPrune(accOuts[i]) : accOuts[i];
        picojson::object out;
        out["type"] = picojson::value("function_call_output");
        out["call_id"] = JS(W8(accCalls[i].callId.c_str()));
        out["output"] = JS(W8(out8.c_str()));
        input.push_back(picojson::value(out));
    }
    input.push_back(userItem(L"(以下为系统自动注入的环境快照, 非用户发言)" + BuildEnvSnapshot()));
    if (!inject.empty()) input.push_back(userItem(inject));
    if (!withTools)
        input.push_back(userItem(L"(工具调用次数已达上限，请直接根据已获得的信息回答)"));
    picojson::object body;
    body["model"] = JS(g_cfg.model);
    if (g_cfg.maxOut > 0)   /* 档案指定了最大输出才发送 (0 = 服务端默认); 与 model 同处请求
                               头部稳定段, 只在切档案时一起变, 前缀缓存不受损 */
        body["max_output_tokens"] = JN(g_cfg.maxOut);
    body["input"] = picojson::value(input);
    body["stream"] = JB(true);
    body["instructions"] = JS(W8(g_instrA.c_str()));   /* 恒定字节 = 跨请求前缀缓存的事实源 */
    if (withTools) body["tools"] = ToolsP();
    picojson::object reasoning;
    reasoning["effort"] = picojson::value(g_cfg.reasoning ? "high" : "none");
    body["reasoning"] = picojson::value(reasoning);
    return picojson::value(body).serialize();
}

/* ±10% 抖动 (dsh retry-policy jitterRatio 口径): 多窗同时被限流时退避不齐锋 */
static DWORD JitterMs(DWORD ms) {
    if (ms <= 100) return ms;
    DWORD span = ms / 5;
    return ms - span / 2 + GetTickCount() % (span + 1);
}

/* 过程状态条 (重试/自愈中; 空 = 清除): 写 j->note 并回泵一次, 前端在打字气泡/
 * 过程面板摘要处显示 — "长等待可观测" (dsh llm/retry 事件口径: 重试不许看起来像静默卡死) */
static void JobNote(AiJob* j, const std::wstring& t) {
    EnterCriticalSection(&j->cs);
    j->note = t;
    LeaveCriticalSection(&j->cs);
    if (g_msgwnd) PostMessageW(g_msgwnd, XJS_AI_STREAM, 0, (LPARAM)j);
}

/* 丢弃本轮已收到的半截文本并推进世代 (泵据此把尾部气泡文本重置为空,
 * 上一轮残文不留): 传输中断重试 / 空响应重发 / 超限自愈重试 共用 */
static void DiscardTurnText(AiJob* j) {
    EnterCriticalSection(&j->cs);
    j->out.clear();
    j->reason.clear();
    j->gen++;
    LeaveCriticalSection(&j->cs);
    if (g_msgwnd) PostMessageW(g_msgwnd, XJS_AI_STREAM, 0, (LPARAM)j);
}

/* HTTP 400 错误文本是否为上下文超限 (各家签名不一, 按常见措辞识别; 中文按
 * UTF-8 字节匹配 — 源码 /utf-8 编译, 服务端中文报错也是 UTF-8) */
static bool CtxOverflowMsg(const std::string& e) {
    if (e.empty()) return false;
    std::string l = e;
    for (auto& c : l) c = (char)tolower((unsigned char)c);
    return l.find("context length") != std::string::npos ||
           l.find("maximum context") != std::string::npos ||
           l.find("too many tokens") != std::string::npos ||
           l.find("reduce the length") != std::string::npos ||
           l.find("input length") != std::string::npos ||
           e.find("上下文长度") != std::string::npos ||
           e.find("超出上下文") != std::string::npos;
}

/* 退避等待: 500ms·2^attempt 封顶 4s, forceMs>0 = 服务端 Retry-After 指定 (仅 ≤4s 时
 * 传入, 更长的照常退避 — dsh retry-policy 口径: 不早于服务端要求, 也不无限等);
 * 期间响应"停止" (立即返回, 由重试环顶部判 abort) */
static void HttpRetryWait(AiJob* j, int attempt, DWORD forceMs = 0) {
    DWORD ms = forceMs;
    if (!ms) {
        ms = 500u << (attempt > 3 ? 3 : attempt);
        if (ms > 4000) ms = 4000;
        ms = JitterMs(ms);
    }
    ULONGLONG t0 = GetTickCount64();
    while (GetTickCount64() - t0 < ms) {
        if (InterlockedCompareExchange(&j->abort, 0, 0)) return;
        Sleep(40);
    }
}

/* 一轮对话: 发请求 → SSE 读流 → 文本增量进 j->out/reason, function_call 攒进 turnCalls。
   返回 false = 网络/HTTP 失败 (errMsg 已设); *failed = 流内协议失败;
   *transient = 传输级瞬态失败 (流中途断掉), 调用方可整轮重试 */
static bool AgentRunTurn(AiJob* j, HINTERNET hc, const std::vector<AiCall>& accCalls,
                         const std::vector<std::string>& accOuts, size_t liveFrom,
                         const std::wstring& repNote, bool withTools,
                         std::vector<AiCall>* turnCalls, bool* aborted, bool* truncated,
                         bool* failed, bool* transient, std::string* errMsg) {
    turnCalls->clear();
    std::string body = AgentBuildBody(j, accCalls, accOuts, liveFrom, repNote, withTools);
    /* 临时诊断 (2026-09-25 空答复排查): 每轮请求体/响应原文落 data\调试请求/调试响应 (后者只留
     * 最后一轮), 定位后连同 LuaDumpCleaner 一起拆掉 */
    if (g_host->StorageSet)
        g_host->StorageSet(g_ctx, "调试请求", body.c_str(), (int)body.size());
    wchar_t wpath[1024] = {};
    MultiByteToWideChar(CP_UTF8, 0, j->pathA.c_str(), -1, wpath, 1024);
    bool ok = true;
    std::wstring hdr = L"Content-Type: application/json\r\nAuthorization: Bearer ";
    hdr += W8(j->keyA.c_str());
    HINTERNET hr = NULL;
    /* 瞬态失败重试 (dsh retry-policy 口径): 429/5xx/传输失败 = 500ms·2^n (封顶 4s) 退避重发,
     * 最多 3 次; 4xx (鉴权/参数/配额) 是语义性失败, 立即放行不改写。退避期响应"停止"。 */
    for (int attempt = 0;; attempt++) {
        if (InterlockedCompareExchange(&j->abort, 0, 0)) { *aborted = true; ok = false; break; }
        hr = WinHttpOpenRequest(hc, L"POST", wpath, NULL, WINHTTP_NO_REFERER,
                                WINHTTP_DEFAULT_ACCEPT_TYPES,
                                j->secure ? WINHTTP_FLAG_SECURE : 0);
        if (!hr) { *errMsg = "open request failed"; ok = false; break; }
        EnterCriticalSection(&j->cs);
        j->hReq = hr;   /* UI"停止"并发关句柄打断阻塞读 */
        LeaveCriticalSection(&j->cs);
        BOOL sent = WinHttpAddRequestHeaders(hr, hdr.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD) &&
                    WinHttpSendRequest(hr, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                       (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) &&
                    WinHttpReceiveResponse(hr, NULL);
        if (!sent) {
            bool stopHit = InterlockedCompareExchange(&j->abort, 0, 0) != 0;
            EnterCriticalSection(&j->cs);
            if (j->hReq == hr) j->hReq = NULL;
            LeaveCriticalSection(&j->cs);
            WinHttpCloseHandle(hr);
            hr = NULL;
            if (stopHit) { *aborted = true; ok = false; break; }
            if (attempt >= 3) { *errMsg = "send/receive failed"; ok = false; break; }
            wchar_t nb[96];
            swprintf(nb, 96, L"连接异常, 正在重试 (第 %d/3 次)…", attempt + 1);
            JobNote(j, nb);
            HttpRetryWait(j, attempt);
            JobNote(j, L"");
            continue;
        }
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &sz, NULL);
        if (status == 429 || (status >= 500 && status <= 599)) {
            /* Retry-After 只在 ≤4s 时尊重 (更长的照常退避, dsh retry-policy 口径:
             * 不早于服务端要求, 也不按它无限等); 等待对前端可见 */
            DWORD raMs = 0;
            wchar_t ra[32] = {};
            DWORD ras = sizeof(ra);
            if (status == 429 &&
                WinHttpQueryHeaders(hr, WINHTTP_QUERY_RETRY_AFTER, NULL, ra, &ras, NULL) && ra[0]) {
                int sec = _wtoi(ra);
                if (sec > 0 && sec <= 4) raMs = (DWORD)sec * 1000;
            }
            EnterCriticalSection(&j->cs);
            if (j->hReq == hr) j->hReq = NULL;
            LeaveCriticalSection(&j->cs);
            WinHttpCloseHandle(hr);
            hr = NULL;
            if (attempt >= 3) {
                char mb[64];
                _snprintf_s(mb, sizeof(mb), _TRUNCATE,
                            "HTTP %lu (server/rate limit, retried 3 times)", (unsigned long)status);
                *errMsg = mb;
                ok = false;
                break;
            }
            wchar_t nb[128];
            if (raMs > 0) swprintf(nb, 128, L"请求过于频繁, 等待 %u 秒后重试 (第 %d/3 次)…",
                                   raMs / 1000, attempt + 1);
            else swprintf(nb, 128, L"服务繁忙 (HTTP %lu), 稍候自动重试 (第 %d/3 次)…",
                          (unsigned long)status, attempt + 1);
            JobNote(j, nb);
            HttpRetryWait(j, attempt, raMs);
            JobNote(j, L"");
            continue;
        }
        if (status != 200) {
            char eb[8192] = {};
            DWORD erd = 0, eofc = 0;
            while (erd < sizeof(eb) - 1 && WinHttpReadData(hr, eb + erd, sizeof(eb) - 1 - erd, &eofc) && eofc)
                erd += eofc;
            eb[erd] = 0;
            Jv v = JsonParseW(W8(eb));
            std::wstring msg;
            const Jv* em = v.Get(L"message");
            if (em && em->t == 3) msg = em->str;
            if (msg.empty()) {
                const Jv* eo = v.Get(L"error");
                if (eo && eo->t == 5) msg = eo->S(L"message");
            }
            std::string detail;
            if (!msg.empty()) detail = U8(msg);
            else {
                std::string ebStr = eb;
                detail = ebStr.size() > 200 ? ebStr.substr(0, 200) : ebStr;
            }
            char mb[512];
            _snprintf_s(mb, sizeof(mb), _TRUNCATE, "HTTP %lu: %s", (unsigned long)status, detail.c_str());
            *errMsg = mb;
            ok = false;
            break;
        }
        break;   /* 2xx: 锁定本次请求, 退出重试环进入读流 */
    }
    if (ok) {
        /* SSE 流式: 文本增量照旧; function_call 按 output_index 分组攒参 */
        std::string buf;
        std::string sseLog;   /* 临时诊断: 原始 data 行 (封顶 256KB, 轮末落 data\调试响应) */
        ULONGLONG lastPost = 0;
        int dbgAppend = 0, dbgFail = 0;   /* 临时诊断: delta 追加数 / 解析失败数 */
        std::string dbgFailHead;          /* 临时诊断: 首个解析失败载荷前 200 字节 */
        for (;;) {
            if (InterlockedCompareExchange(&j->abort, 0, 0)) { *aborted = true; break; }
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(hr, &avail)) {
                /* "停止"会并发关句柄把读打断 — 先查 abort 归类, 真断流才算瞬态
                 * (dsh TIMEOUT/TRANSPORT 口径: 传输失败可整轮重试; 流卡死由既有
                 * WinHttpSetTimeouts 收超时 120s 兜底, 这里只做失败归类不做计时) */
                if (InterlockedCompareExchange(&j->abort, 0, 0)) { *aborted = true; }
                else { *failed = true; *transient = true; *errMsg = "connection lost during stream"; }
                break;
            }
            if (!avail) break;
            std::string chunk((size_t)avail, 0);
            DWORD rd = 0;
            if (!WinHttpReadData(hr, &chunk[0], avail, &rd)) {
                if (InterlockedCompareExchange(&j->abort, 0, 0)) { *aborted = true; }
                else { *failed = true; *transient = true; *errMsg = "connection lost during stream"; }
                break;
            }
            if (InterlockedCompareExchange(&j->abort, 0, 0)) { *aborted = true; break; }
            chunk.resize(rd);
            buf += chunk;
            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.rfind("data:", 0) != 0) continue;
                std::string payload = line.substr(5);
                while (!payload.empty() && (payload[0] == ' ')) payload.erase(0, 1);
                if (payload == "[DONE]") continue;
                if (sseLog.size() < 262144) { sseLog += payload; sseLog += '\n'; }
                Jv ev = JsonParseW(W8(payload.c_str()));
                if (ev.t != 5) {
                    dbgFail++;
                    if (dbgFail == 1) dbgFailHead = payload.substr(0, 200);
                    continue;
                }
                std::wstring type = ev.S(L"type");
                EnterCriticalSection(&j->cs);
                if (type == L"response.output_text.delta") {
                    const Jv* d = ev.Get(L"delta");
                    if (d) { j->out += (d->t == 3 ? d->str : (d->t == 5 ? d->S(L"text") : L"")); dbgAppend++; }
                } else if (type == L"response.reasoning_text.delta") {
                    const Jv* d = ev.Get(L"delta");
                    if (d) j->reason += (d->t == 3 ? d->str : (d->t == 5 ? d->S(L"text") : L""));
                } else if (type == L"response.output_item.added") {
                    const Jv* item = ev.Get(L"item");
                    if (item && item->t == 5 && item->S(L"type") == L"function_call") {
                        AiCall* slot = AgentCallSlot(turnCalls, ev.Get(L"output_index"));
                        slot->name = U8(item->S(L"name"));
                        slot->callId = U8(item->S(L"call_id"));
                    }
                } else if (type == L"response.output_item.done") {
                    /* 收尾事件携带权威全量字段: 个别模型 added 缺 name/call_id, 从这里补齐 */
                    const Jv* item = ev.Get(L"item");
                    if (item && item->t == 5 && item->S(L"type") == L"function_call") {
                        AiCall* slot = AgentCallSlot(turnCalls, ev.Get(L"output_index"));
                        const Jv* n = item->Get(L"name");
                        const Jv* ci = item->Get(L"call_id");
                        const Jv* a = item->Get(L"arguments");
                        if (n && n->t == 3 && !n->str.empty()) slot->name = U8(n->str);
                        if (ci && ci->t == 3 && !ci->str.empty()) slot->callId = U8(ci->str);
                        if (a && a->t == 3 && !a->str.empty()) slot->args = U8(a->str);
                    }
                } else if (type == L"response.function_call_arguments.delta" ||
                           type == L"response.function_call_arguments.done") {
                    AiCall* slot = AgentCallSlot(turnCalls, ev.Get(L"output_index"));
                    /* added/done 之外的兜底补采 */
                    if (slot->name.empty()) {
                        const Jv* n = ev.Get(L"name");
                        if (n && n->t == 3) slot->name = U8(n->str);
                    }
                    if (slot->callId.empty()) {
                        const Jv* ci = ev.Get(L"call_id");
                        if (ci && ci->t == 3) slot->callId = U8(ci->str);
                    }
                    const Jv* d = ev.Get(L"delta");
                    const Jv* a = ev.Get(L"arguments");
                    if (type == L"response.function_call_arguments.done")
                        slot->args = a && a->t == 3 ? U8(a->str) : slot->args;
                    else if (d)
                        slot->args += (d->t == 3 ? U8(d->str) : std::string());
                } else if (type == L"response.completed") {
                    /* 用量统计 (对齐参考实现): input=计费输入, cached=前缀缓存命中 */
                    const Jv* rsp = ev.Get(L"response");
                    const Jv* us = (rsp && rsp->t == 5) ? rsp->Get(L"usage") : NULL;
                    if (us && us->t == 5) {
                        const Jv* x;
                        j->turnUsage.has = true;
                        if ((x = us->Get(L"input_tokens")) && x->t == 2) j->turnUsage.prompt = (long long)x->num;
                        if ((x = us->Get(L"output_tokens")) && x->t == 2) j->turnUsage.completion = (long long)x->num;
                        if ((x = us->Get(L"total_tokens")) && x->t == 2) j->turnUsage.total = (long long)x->num;
                        const Jv* dt = us->Get(L"input_tokens_details");
                        if (dt && dt->t == 5 && (x = dt->Get(L"cached_tokens")) && x->t == 2)
                            j->turnUsage.cacheHit = (long long)x->num;
                    }
                } else if (type == L"response.failed") {
                    const Jv* rsp = ev.Get(L"response");
                    if (rsp && rsp->t == 5) {
                        const Jv* er = rsp->Get(L"error");
                        if (er && er->t == 5) *errMsg = U8(er->S(L"message").c_str());
                    }
                    *failed = true;
                } else if (type == L"response.incomplete") {
                    *truncated = true;
                } else if (type == L"error") {
                    const Jv* er = ev.Get(L"error");
                    *errMsg = U8((er && er->t == 5 ? er->S(L"message") : ev.S(L"message")).c_str());
                    *failed = true;
                }
                LeaveCriticalSection(&j->cs);
            }
            ULONGLONG now = GetTickCount64();
            if (now - lastPost > 40 && g_msgwnd) {   /* 节流回泵 (UI 抽增量渲染) */
                lastPost = now;
                PostMessageW(g_msgwnd, XJS_AI_STREAM, 0, (LPARAM)j);
            }
        }
        /* 临时诊断: 本轮追加/解析失败统计 + 流结束时 out 长度 */
        if (g_host->StorageSet) {
            EnterCriticalSection(&j->cs);
            size_t outNow = j->out.size();
            LeaveCriticalSection(&j->cs);
            std::string dbg = "appends=" + std::to_string(dbgAppend) +
                              " parseFail=" + std::to_string(dbgFail) +
                              " outLenAtStreamEnd=" + std::to_string(outNow) +
                              " turnCalls=" + std::to_string(turnCalls->size()) +
                              "\nfailHead=" + dbgFailHead;
            g_host->StorageSet(g_ctx, "调试解析", dbg.c_str(), (int)dbg.size());
        }
        if (g_host->StorageSet)
            g_host->StorageSet(g_ctx, "调试响应", sseLog.c_str(), (int)sseLog.size());
    }
    EnterCriticalSection(&j->cs);
    if (j->hReq) { WinHttpCloseHandle(j->hReq); j->hReq = NULL; }
    LeaveCriticalSection(&j->cs);
    if (hr) WinHttpCloseHandle(hr);
    return ok;
}

/* 重复调用护栏的链键: 工具名 + 键名字典序规范化后的参数 (键序无关, dsh repeat-tool-reminder
 * 口径: {"a":1,"b":2} 与 {"b":2,"a":1} 视为同一次调用); picojson::object 本身就是 std::map,
 * 序列化天然按键字典序 (含嵌套层); 参数解析失败保留原文, 同文重复照样命中 */
static std::string AgentCallKey(const std::string& name8, const std::string& args8) {
    picojson::value v;
    std::string canon;
    if (JParseU8(v, args8) && v.is<picojson::object>()) canon = v.serialize();
    else canon = args8;
    return name8 + "|" + canon;
}

void WorkerMain(AiJob* j) {   /* agent 循环: SSE → 工具执行 → 结果回填 → 下一轮, 直到最终答复 */
    JobLuaDumpCleaner dumpCleaner;   /* data\待运行.lua 作业级守卫: 期间每次 lua 调用覆盖写入, 本函数任何出口删除 */
    wchar_t whost[512] = {};
    MultiByteToWideChar(CP_UTF8, 0, j->hostA.c_str(), -1, whost, 512);
    HINTERNET hs = WinHttpOpen(L"snail-quicksearch-ai-assistant", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!hs) hs = WinHttpOpen(L"snail-quicksearch-ai-assistant", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    HINTERNET hc = NULL;
    if (hs) {
        WinHttpSetTimeouts(hs, 15000, 30000, 30000, 120000);   /* 单轮超时; 多轮总时长由轮数×超时构成 */
        hc = WinHttpConnect(hs, whost, j->port, 0);
    }
    bool aborted = false, truncated = false, failed = false;
    std::string errMsg;
    if (!hs || !hc) { failed = true; errMsg = hs ? "connect failed" : "network init failed"; }
    else {
        std::vector<AiCall> accCalls;       /* 已执行的工具往返 (input 回填; 与 accOuts 一一对应) */
        std::vector<std::string> accOuts;
        size_t liveFrom = 0;                /* 本轮刚产出的输出起点 (这些不裁剪; 更早轮次的中段裁剪) */
        std::string repKey;                 /* 重复调用链键 + 连击计数 (dsh repeat-tool-reminder 口径;
                                               链随作业存活 = 每条用户消息自然重置) */
        int repCount = 0;
        std::wstring repNote;               /* 命中阈值 → 下一轮请求末尾注入的提醒 (随请求消费即清) */
        int emptyRetry = 0;                 /* 空响应 (无工具调用也无文本) 原样重发次数 */
        int txRetry = 0;                    /* 传输中断整轮重试计数 (成功轮归零) */
        bool overflowRetried = false;       /* 上下文超限自愈每作业只做一次 (dsh maxOverflowRetries=1) */
        for (int turn = 0; turn < AI_AGENT_MAX_TURNS; turn++) {
            if (InterlockedCompareExchange(&j->abort, 0, 0)) { aborted = true; break; }
            bool lastTurn = turn == AI_AGENT_MAX_TURNS - 1;
            std::vector<AiCall> turnCalls;
            ULONGLONG turnT0 = GetTickCount64();
            bool transient = false;
            bool okTurn = AgentRunTurn(j, hc, accCalls, accOuts, liveFrom, repNote, !lastTurn, &turnCalls,
                                       &aborted, &truncated, &failed, &transient, &errMsg);
            JobNote(j, L"");   /* 请求已返回, 状态条清掉 */
            repNote.clear();   /* 已随请求消费 */
            EnterCriticalSection(&j->cs);
            j->turnOutMs = GetTickCount64() - turnT0;   /* 速度 = 本轮输出 / 本轮耗时 (含首 token 等待) */
            LeaveCriticalSection(&j->cs);
            if (!okTurn) failed = true;
            if (aborted) break;
            if (failed) {
                /* 瞬态传输失败 (流中途断; 卡死由既有 WinHTTP 收超时兜底, 这里只归类):
                   丢半截流整轮重发 ≤3 次 (dsh TIMEOUT/TRANSPORT 口径; 重试可见 — note 推前端,
                   不许像静默卡死) */
                if (transient && txRetry < 3) {
                    txRetry++;
                    wchar_t nb[96];
                    swprintf(nb, 96, L"连接中断, 正在重新生成 (第 %d/3 次)…", txRetry);
                    JobNote(j, nb);
                    DiscardTurnText(j);
                    HttpRetryWait(j, txRetry - 1);
                    JobNote(j, L"");
                    failed = false;
                    errMsg.clear();
                    if (InterlockedCompareExchange(&j->abort, 0, 0)) { aborted = true; break; }
                    turn--;   /* 重跑同一轮 */
                    continue;
                }
                /* 上下文超限 (HTTP 400): 全部工具输出进入裁剪范围再试一次 (dsh overflow
                 * recovery 口径: 先做无模型的剪枝, 重试受 generations 推进闸约束 = 只一次;
                 * 仍超限 = 真失败, 用户看到原始报错) */
                if (!transient && !overflowRetried && !accCalls.empty() && CtxOverflowMsg(errMsg)) {
                    overflowRetried = true;
                    liveFrom = 0;
                    repNote = L"(系统提示: 上一次请求超出模型上下文上限, 早期工具输出已压缩为头尾摘要; "
                              L"若关键数据缺失请重新调用工具获取, 或直接基于已有信息作答)";
                    JobNote(j, L"上下文超限, 已压缩早期工具输出, 正在重试…");
                    DiscardTurnText(j);
                    failed = false;
                    errMsg.clear();
                    turn--;
                    continue;
                }
                break;
            }
            txRetry = 0;   /* 成功轮归零: 瞬态预算按"连续失败"计 */
            if (truncated) break;   /* 截断轮 (max-tokens): 半截 arguments 绝不执行 (dsh assembler 口径) */
            if (turnCalls.empty()) {
                /* 空响应防线 (dsh EMPTY_RESPONSE 口径): 流正常结束但无工具调用也无文本 = 退化完成,
                 * 原样重发最多 2 次, 仍空才按完成收尾。
                 * ⚠ 清空只允许发生在"确认重发"之后 — 曾无条件先清再判, 不重发的路径把刚流完的
                 *   正文一起清掉, 泵收尾读到空 = "输出过程可见、完成后消失" (2026-09-25 实锤) */
                std::wstring outNow;
                EnterCriticalSection(&j->cs);
                outNow = j->out;
                LeaveCriticalSection(&j->cs);
                if (TrimW(outNow).empty() && !lastTurn && emptyRetry < 2) {
                    emptyRetry++;
                    wchar_t nb[80];
                    swprintf(nb, 80, L"回复为空, 正在重新生成 (第 %d/2 次)…", emptyRetry);
                    JobNote(j, nb);
                    DiscardTurnText(j);
                    repNote = L"(系统提醒: 你上一条回复没有输出任何文字内容。若任务未完成请继续调用工具, "
                              L"若已完成请直接给出面向用户的最终回答)";
                    continue;
                }
                break;
            }
            /* 工具执行 (仍在本线程, 串行; 步骤镜像实时推进给泵渲染) */
            EnterCriticalSection(&j->cs);
            j->phase = 1;   /* 泵冻结当前文本气泡 (下一轮答复另起新气泡) */
            LeaveCriticalSection(&j->cs);
            if (g_msgwnd) PostMessageW(g_msgwnd, XJS_AI_STREAM, 0, (LPARAM)j);
            size_t batchStart = accOuts.size();   /* 本批输出起点 = 下一轮的 liveFrom (见循环尾) */
            std::vector<std::string> batchKeys;   /* 本批已执行的调用键 (完全重复的调用去重) */
            for (auto& c : turnCalls) {
                if (InterlockedCompareExchange(&j->abort, 0, 0)) break;
                /* 链键先行: 同批去重与跨轮重复护栏共用 (键序无关的规范化参数) */
                std::string k = AgentCallKey(c.name, c.args);
                bool dup = false;
                for (auto& pk : batchKeys)
                    if (pk == k) { dup = true; break; }
                if (!dup) batchKeys.push_back(k);
                AiToolStep local;
                local.state = 1;
                local.name = W8(c.name.c_str());
                /* 卡片字段在入队前先填好 — 曾在入队后才解析 run_command 参数, 挂起等裁决期
                 * 镜像里 kind 还是 0, 卡片渲染成"搜索 run_command"+文件询问按钮, 命令全文
                 * 看不见, 用户只能盲批 (2026-09-25 实锤) */
                int pol = InterlockedCompareExchange(&j->policy, 0, 0);
                int epol = InterlockedCompareExchange(&j->execPolicy, 0, 0);
                bool fileTool = (c.name == "open_file" || c.name == "copy_paths");
                bool execTool = (c.name == "run_command");
                std::wstring exShell, exCmd, exDir, exDesc;
                long long exTimeout = 120000;
                if (execTool) {
                    Jv cv = JsonParseW(W8(c.args.c_str()));
                    exCmd = TrimW(cv.S(L"command"));
                    exShell = TrimW(cv.S(L"shell"));
                    if (exShell.empty()) exShell = L"cmd";
                    exDir = TrimW(cv.S(L"workdir"));
                    exDesc = TrimW(cv.S(L"description"));
                    const Jv* tv = cv.Get(L"timeoutMs");
                    if (tv && tv->t == 2 && tv->num >= 3000 && tv->num <= 600000)
                        exTimeout = (long long)tv->num;
                    local.kind = 11;
                    local.mode = exShell;
                    local.query = exCmd;
                    local.argz = exDesc;
                    ArgzCut(&local.argz);
                }
                int sidx = -1;
                EnterCriticalSection(&j->cs);
                j->steps.push_back(local);
                sidx = (int)j->steps.size() - 1;
                j->stepsVersion++;
                LeaveCriticalSection(&j->cs);
                if (g_msgwnd) PostMessageW(g_msgwnd, XJS_AI_STREAM, 0, (LPARAM)j);
                /* 文件操作权限闸 (对齐参考实现"命令行权限"): 禁用/只读直接拒绝并回喂模型;
                   询问 = 先拒绝 + 卡片转询问态给确认按钮, 用户点"允许"后本作业后续调用放行 */
                std::wstring err;
                std::string output;
                if (dup) {
                    /* 同批完全重复 (模型惊慌连发): 只执行首个, 其余回执指向前一次结果 —
                       否则 N 份等量大结果原样回喂直接撑爆上下文 (get_lua_spec 9 连发实锤) */
                    local.state = 2;
                    {
                        picojson::object d;
                        d["duplicate"] = JB(true);
                        d["note"] = JS(L"与本批次中前面一次调用完全相同, 已去重未重复执行; 以上一次的执行结果为准");
                        output = picojson::value(d).serialize();
                    }
                } else if (fileTool && pol == 0) {
                    /* 拒绝回执写成可恢复指引 (dsh 口径: 拒绝原因要告诉模型怎么继续),
                       只描述事实 + 恢复路径, 不代用户做决定 */
                    err = L"已拒绝: 文件操作权限当前为「禁用」。请告知用户在对话输入框下方的"
                          L"文件操作权限选择器切换到「允许」或「询问」后, 再重新调用本工具";
                    local.state = 3;
                } else if (fileTool && pol == 1) {
                    err = L"已拒绝: 文件操作权限当前为「只读」。请告知用户把权限切换到「允许」"
                          L"(或「询问」逐次确认)后, 再重新调用本工具";
                    local.state = 3;
                } else if (fileTool && pol == 2) {
                    err = L"等待用户确认文件操作 (在下方卡片选择「允许」后我会重试)";
                    local.state = 4;
                } else if (execTool && epol == 0) {
                    err = L"已拒绝: 命令执行权限当前为「禁用」。请告知用户在对话输入框下方的"
                          L"命令执行权限选择器切换到「允许」或「询问」后, 再重新调用本工具";
                    local.state = 3;
                } else if (execTool && epol == 2) {
                    /* 询问档 = 挂起等用户裁决 (dsh approval 口径: 审批暂停回合、答复恢复 —
                     * 不做"先拒绝再指望模型自己重试"的死胡同: 模型被拒即结束回合说"请点允许",
                     * 之后无论点什么都没有执行体了)。卡片转询问态, worker 在此轮询
                     * 允许/拒绝/停止/超时; 允许 → 接着执行并回喂输出, 拒绝/超时 → 作为
                     * 工具失败回喂, 模型当场就能得体收尾。 */
                    std::wstring risk = ExecRiskText(exCmd);
                    local.state = 4;
                    local.err = L"等待用户确认命令执行" +
                                (exDesc.empty() ? std::wstring()
                                                : (L" · AI 说明: " + exDesc)) +
                                (risk.empty() ? std::wstring()
                                              : (L"; ⚠ 检测到高危特征: " + risk));
                    EnterCriticalSection(&j->cs);
                    InterlockedExchange(&j->execGrant, 0);
                    InterlockedExchange(&j->execDeny, 0);
                    if (sidx >= 0 && sidx < (int)j->steps.size()) {   /* 询问卡立即可见 (不等执行完的统一回写) */
                        j->steps[sidx].state = 4;
                        j->steps[sidx].err = local.err;
                        j->stepsVersion++;
                    }
                    LeaveCriticalSection(&j->cs);
                    if (g_msgwnd) PostMessageW(g_msgwnd, XJS_AI_STREAM, 0, (LPARAM)j);
                    bool granted = false;
                    bool stoppedAsk = false;
                    ULONGLONG askT0 = GetTickCount64();
                    for (;;) {
                        if (InterlockedCompareExchange(&j->abort, 0, 0)) { stoppedAsk = true; break; }
                        bool denied = false;
                        EnterCriticalSection(&j->cs);
                        if (j->execGrant) { j->execGrant = 0; granted = true; }
                        denied = j->execDeny != 0;
                        LeaveCriticalSection(&j->cs);
                        if (granted || denied || GetTickCount64() - askT0 > 300000) break;
                        Sleep(40);
                    }
                    if (stoppedAsk) {
                        err = L"已停止";
                        local.state = 3;
                    } else if (granted) {
                        local.state = 1;   /* 卡片转执行中 (执行完由尾部统一回写为完成/失败) */
                        local.err.clear();
                    } else if (InterlockedCompareExchange(&j->execDeny, 0, 0)) {
                        err = L"用户拒绝执行这条命令 — 不要再尝试相同或同类命令, 如实说明并按用户指示继续";
                        local.state = 3;
                    } else {
                        err = L"等待用户确认超时 (5 分钟未响应), 本次执行已取消 — "
                              L"可告知用户放行后重新提出";
                        local.state = 3;
                    }
                    /* 卡片即时转执行中/失败 (不等执行完) */
                    EnterCriticalSection(&j->cs);
                    if (sidx >= 0 && sidx < (int)j->steps.size()) {
                        j->steps[sidx].state = local.state;
                        j->steps[sidx].err = local.err;
                        j->stepsVersion++;
                    }
                    LeaveCriticalSection(&j->cs);
                    if (g_msgwnd) PostMessageW(g_msgwnd, XJS_AI_STREAM, 0, (LPARAM)j);
                    if (granted)
                        err = AgentToolRunCommand(j, exShell, exCmd, exDir, exTimeout, &local);
                } else if (execTool) {   /* 允许档: 直接执行 */
                    err = AgentToolRunCommand(j, exShell, exCmd, exDir, exTimeout, &local);
                } else {
                    err = AgentToolExec(j, c.name, c.args, j->tok, &local);
                }
                local.err = err;
                if (local.state == 1) local.state = err.empty() ? 2 : 3;
                if (output.empty()) output = AgentToolOutput(err, local);
                EnterCriticalSection(&j->cs);
                if (sidx >= 0 && sidx < (int)j->steps.size()) {
                    AiToolStep& dst = j->steps[sidx];
                    dst.kind = local.kind;
                    dst.state = local.state;
                    dst.mode = local.mode;
                    dst.query = local.query;
                    dst.argz = local.argz;
                    dst.count = local.count;
                    dst.elapsedMs = local.elapsedMs;
                    dst.err = local.err;
                    dst.top = local.top;   /* open 展开态归泵/用户, 不覆盖 */
                    dst.wrote = local.wrote;   /* 导出的文件 (卡片常显块+落库) */
                    dst.adj = local.adj;   /* 待应用的调整 (提案数据; 漏拷 = 卡片按钮区不渲染) */
                    j->stepsVersion++;
                }
                LeaveCriticalSection(&j->cs);
                /* 缺 name/call_id 的调用不回喂 (API 校验 call_id 非空且与 output 成对,
                 * 喂空串整轮 400 连累同轮正常调用); 卡片照常显示错误 */
                if (!c.name.empty() && !c.callId.empty()) {
                    accCalls.push_back(c);
                    accOuts.push_back(output);
                }
                /* 重复调用护栏 (dsh repeat-tool-reminder 口径): 同键连击 3/6 次各注入一次提醒,
                 * 只提醒不阻断 (被权限闸拒绝/去重的调用同样计数 — 反复锤正是要打断的循环) */
                if (k == repKey) repCount++;
                else { repKey = k; repCount = 1; }
                if (repCount == 3 || repCount == 6) {
                    if (repCount == 3) {
                        repNote = L"(系统提醒: 你正用完全相同的参数重复调用同一个工具, 这不会产生新信息 — "
                                  L"先仔细分析上一次结果, 换不同参数/不同口径再试; 若信息已足够就直接作答)";
                    } else {
                        /* 详细档带参数预览 (dsh argumentsPreviewChars 口径: 检测用全量,
                         * 只截模型可见的引用) */
                        std::string prev = c.args;
                        if (prev.size() > 200) prev.resize(PruneUtf8Floor(prev, 200));
                        repNote = L"(系统提醒: 工具 " + W8(c.name.c_str()) + L" 已用完全相同的参数连续调用 " +
                                  std::to_wstring(repCount) + L" 次。这些重复调用没有进展, 不要再用这些参数调用它 — "
                                  L"检查最近一次结果, 改用不同参数/不同工具/不同搜索模式, 或基于已有信息直接作答";
                        if (!prev.empty())
                            repNote += L"。最近一次参数: " + W8(prev.c_str());
                        repNote += L")";
                    }
                }
                if (g_msgwnd) PostMessageW(g_msgwnd, XJS_AI_STREAM, 0, (LPARAM)j);
            }
            /* liveFrom = 本批输出起点: 最新一批在下一轮请求里保持原文 (模型此刻才第一次读到它,
             * 裁掉它 = "刚取回的规范永远看不到全文"→反复重取→连发撑爆上下文, 2026-09-25 实锤);
             * 更早批次那时起进入中段裁剪范围 */
            liveFrom = batchStart;
            EnterCriticalSection(&j->cs);
            j->phase = 0;       /* 下一轮答复另起新文本气泡 */
            j->out.clear();
            j->reason.clear();
            LeaveCriticalSection(&j->cs);
        }
    }
    if (hc) WinHttpCloseHandle(hc);
    if (hs) WinHttpCloseHandle(hs);
    EnterCriticalSection(&j->cs);
    if (j->hReq) { WinHttpCloseHandle(j->hReq); j->hReq = NULL; }
    j->truncated = truncated;
    j->err = W8(errMsg.c_str());
    j->state = aborted ? 3 : (failed ? 2 : 1);
    /* 临时诊断 (空答复排查): 作业收尾时 out 的长度与开头 */
    if (g_host->StorageSet) {
        std::wstring dbg = L"state=" + std::to_wstring(j->state) +
                           L" truncated=" + std::to_wstring((int)truncated) +
                           L" outLen=" + std::to_wstring(j->out.size()) +
                           L"\noutHead=" + j->out.substr(0, 200);
        g_host->StorageSet(g_ctx, "调试作业末", U8(dbg).c_str(), (int)U8(dbg).size());
    }
    LeaveCriticalSection(&j->cs);
    if (g_msgwnd) PostMessageW(g_msgwnd, XJS_AI_STREAM, 0, (LPARAM)j);
}

