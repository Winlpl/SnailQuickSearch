/*
 * ai_session.cpp — 会话层: 会话池 / 发送入口 / 流泵 (UI 抽取增量 → Web 增量同步) /
 * 消息窗口。会话状态全在 AiSess (一窗一份); 界面归 WebView2 前端 (ai_web.cpp)。
 */
#include "ai_assistant.h"

HWND g_msgwnd = NULL;    /* 消息窗口: 工作线程 PostMessage 回 UI 线程泵 (渲染不进工作线程) */

AiSess g_sess[8];

AiSess* SessByTok(XjsWindowToken tok) {
    for (auto& s : g_sess) if (s.inUse && s.tok == tok) return &s;
    return NULL;
}
AiSess* SessFree() {
    for (auto& s : g_sess) if (!s.inUse) return &s;
    return NULL;
}

/* ---- 皮肤 (GetSkinJsonOf 按会话所属窗取; 打开会话时 / 皮肤事件后) ---- */
void SessLoadSkinOf(AiSess* s) {
    s->skinOk = false;
    if (!g_host) return;
    char buf[1024];
    int n = HOST_PANEL_OK ? g_host->GetSkinJsonOf(g_ctx, s->tok, buf, (int)sizeof(buf) - 1)
                          : g_host->GetSkinJson(g_ctx, buf, (int)sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = 0;
    Jv v = JsonParseW(W8(buf));
    if (v.t != 5) return;
    s->cBg = HexCol(v.S(L"bg1", L"#14171f"));
    s->cPanel = HexCol(v.S(L"panel", L"#1b2030"));
    s->cText = HexCol(v.S(L"text", L"#e8eaf0"));
    s->cDim = HexCol(v.S(L"dim", L"#9aa3b5"));
    s->cAccent = HexCol(v.S(L"accent", L"#4f7cff"));
    s->cLine = HexCol(v.S(L"line", L"#2a3040"));
    s->skinOk = true;
}

std::vector<AiJob*> s_orphans;   /* 会话已关而流未完的作业 (泵里清扫 join) */

void AbortSend(AiSess* s) {
    if (!s->job) return;
    AiJob* j = s->job;
    InterlockedExchange(&j->abort, 1);
    EnterCriticalSection(&j->cs);
    if (j->hReq) { WinHttpCloseHandle(j->hReq); j->hReq = NULL; }   /* 并发关句柄 = 打断阻塞读 */
    LeaveCriticalSection(&j->cs);
}

void SessSaveConv(AiSess* s) {
    if (s->msgs.empty()) return;
    s->curId = HistUpsert(s->curId, s->msgs);
}

void SessOpen(AiSess* s, XjsWindowToken tok, long long serial, int w, int h, float scale) {
    s->inUse = true;
    s->tok = tok;
    s->serial = serial;
    s->w = w;
    s->h = h;
    s->scale = scale > 0 ? scale : 1.0f;
    if (!s->web) {
        static bool s_dataLoaded = false;   /* cfg/历史进程级一份, 多窗共享 */
        if (!s_dataLoaded) { s_dataLoaded = true; CfgLoad(); HistLoad(); }
    }
    SessLoadSkinOf(s);
    WebSessionCreate(s);   /* 子窗口 + WebView2 控制器 (异步; ready 后 JS 拉 boot) */
}

void SessClose(AiSess* s) {
    AbortSend(s);
    SessSaveConv(s);   /* 未落库的当前对话保存 (关面板不丢) */
    s->inUse = false;
    if (s->job) { s_orphans.push_back(s->job); s->job = NULL; }   /* 流未完 → 孤儿 (泵清扫 join) */
    s->msgs.clear();
    s->curId = 0;
    WebSessionDestroy(s);
}

void SendCurrent(AiSess* s, const std::wstring& textIn, const std::vector<AiAttach>* attsIn) {
    if (s->sending || !g_host) return;
    std::wstring text = TrimW(textIn);
    /* 附件闸门 (前端已挡一轮, 这里兜底): 能力勾选/类型/大小/数量 — 越闸项剔除并逐项提示 */
    std::vector<AiAttach> atts;
    if (attsIn) {
        for (const AiAttach& a : *attsIn) {
            if (atts.size() >= AI_ATT_MAX) {
                WebToast(s, "每条消息最多 4 个附件, 多余的已忽略", XJS_PLUGIN_TOAST_WARN);
                break;
            }
            static const wchar_t* const MIME[3] = { L"data:image/", L"data:video/", L"data:audio/" };
            static const wchar_t* const KIND[3] = { L"图片", L"视频", L"音频" };
            static const size_t CAP[3] = { AI_ATT_IMG_MAX, AI_ATT_VIDEO_MAX, AI_ATT_AUDIO_MAX };
            if (a.kind < 0 || a.kind > 2 || a.dataUrl.rfind(MIME[a.kind], 0) != 0) continue;   /* 坏条目静默丢 */
            bool capOn = a.kind == 0 ? g_cfg.img : a.kind == 1 ? g_cfg.video : g_cfg.audio;   /* 活动档案镜像 */
            if (!capOn) {
                WebToast(s, U8(std::wstring(L"当前模型未开启") + KIND[a.kind]
                               + L"输入 (接口设置中勾选), 该附件已忽略").c_str(), XJS_PLUGIN_TOAST_WARN);
                continue;
            }
            if (a.dataUrl.size() > CAP[a.kind]) {
                WebToast(s, U8(std::wstring(KIND[a.kind]) + L"过大 (超上限), 该附件已忽略").c_str(),
                         XJS_PLUGIN_TOAST_WARN);
                continue;
            }
            atts.push_back(a);
        }
    }
    if (text.empty() && atts.empty()) return;
    if (g_cfg.apiKey.empty()) {
        WebToast(s, "尚未配置接口密钥 — 请点右上角 接口设置 填写", XJS_PLUGIN_TOAST_WARN);
        return;
    }
    AiMsg um;
    um.role = 0;
    um.text = text;
    um.atts = std::move(atts);
    s->msgs.push_back(um);
    WebTouch(s);
    /* 请求要素快照 (线程只读这些; 请求体每轮在 worker 构建 — input 随工具往返增长) */
    AiJob* j = new AiJob();
    j->keyA = U8(g_cfg.apiKey);
    /* baseUrl → host/port/path (https 默认) */
    std::wstring base = TrimW(g_cfg.baseUrl);
    while (!base.empty() && base.back() == L'/') base.pop_back();
    bool secure = true;
    if (base.rfind(L"https://", 0) == 0) { secure = true; base = base.substr(8); }
    else if (base.rfind(L"http://", 0) == 0) { secure = false; base = base.substr(7); }
    size_t slash = base.find(L'/');
    std::wstring hostpart = slash == std::wstring::npos ? base : base.substr(0, slash);
    std::wstring path = slash == std::wstring::npos ? L"" : base.substr(slash);
    j->secure = secure;
    j->port = secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    {
        std::wstring host = hostpart;
        size_t colon = host.rfind(L':');
        if (colon != std::wstring::npos) {
            int p = _wtoi(host.substr(colon + 1).c_str());
            if (p > 0) j->port = (INTERNET_PORT)p;
            host = host.substr(0, colon);
        }
        j->hostA = U8(host);
    }
    j->pathA = U8(path + L"/responses");
    j->tok = s->tok;   /* open_file 走宿主 OpenFile 的目标窗口 */
    InterlockedExchange(&j->policy, g_cfg.filePolicy);   /* 权限快照 (确认卡"允许"由 UI 更新) */
    InterlockedExchange(&j->execPolicy, g_cfg.execPolicy);
    InterlockedExchange(&j->syncRes, g_cfg.syncResults ? 1 : 0);   /* 结果同步勾选快照 */
    /* 同步目标窗结果对象 (UI 线程捕获; 完成事件回调用前自查 IsEffective) */
    j->syncWin = g_cfg.syncResults ? AgentWindowResultOf(s->tok) : NULL;
    InterlockedExchange(&j->execGrant, 0);   /* 新作业不带上一条消息的裁决标志 */
    InterlockedExchange(&j->execDeny, 0);
    /* 对话快照 (只含 role 0/1; 有附件的 role 0 即使无文字也要进上下文; 工具往返由 worker 在循环中累计) */
    for (auto& m : s->msgs)
        if (m.role != 2 && (!m.text.empty() || !m.atts.empty())) j->hist.push_back(m);
    if (j->hist.size() > 30) j->hist.erase(j->hist.begin(), j->hist.end() - 30);
    while (!j->hist.empty() && j->hist.front().role != 0) j->hist.erase(j->hist.begin());
    s->job = j;
    s->sending = true;
    s->stepBase = (int)s->msgs.size();   /* 本作业工具卡片起点 (历史恢复的 role==2 卡片在其之前) */
    s->syncGen = -1;                     /* 世代对齐复位 (新作业 gen 从 0 起) */
    s->netStatus = g_cfg.apiKey.empty() ? 0 : s->netStatus;
    j->th = new std::thread([j]() { WorkerMain(j); });
    WebTouch(s);
    WebSyncSession(s);   /* msgs (新用户消息) + status (发送中) 即时跟手 */
}

static void PumpStreams() {
    bool histChanged = false;
    for (auto& s : g_sess) {
        AiJob* j = s.job;
        if (!s.inUse || !j) continue;
        std::wstring out, reason;
        int state = 0, phase = 0, stepsVer = 0;
        std::wstring err;
        bool truncated = false;
        int gen = 0;
        std::vector<AiToolStep> steps;
        EnterCriticalSection(&j->cs);
        out = j->out;
        reason = j->reason;
        state = j->state;
        phase = j->phase;
        err = j->err;
        truncated = j->truncated;
        gen = j->gen;
        stepsVer = j->stepsVersion;
        if (stepsVer != s.lastStepsVer) steps = j->steps;   /* 有变化才拷 (少一次全量复制) */
        LeaveCriticalSection(&j->cs);
        if (state == 0) {
            if (gen != s.syncGen) {
                /* 世代变化 = worker 丢弃半截流重新生成 (传输中断/空响应重试/超限自愈):
                 * 尾部文本气泡重置为当前 out (通常已清空) — 泵只在文本变化时覆写尾部气泡,
                 * 不重置的话重生成的流会叠加在上一轮残文后面 */
                s.syncGen = gen;
                if (!s.msgs.empty() && s.msgs.back().role == 1) {
                    AiMsg& back = s.msgs.back();
                    if (back.text != out || back.reason != reason) {
                        back.text = out;
                        back.reason = reason;
                        WebTouch(&s);
                    }
                }
            }
            /* 用量累计: 每轮 response.completed 的 usage 取一次 (对齐参考实现:
               累计=计费量; 上下文占用/速度只认最近一轮) */
            bool takeUsage = false;
            AiJob::TurnUsage tu;
            ULONGLONG outMs = 0;
            EnterCriticalSection(&j->cs);
            if (j->turnUsage.has && !j->turnUsageTaken) {
                j->turnUsageTaken = true;
                tu = j->turnUsage;
                outMs = j->turnOutMs;
                takeUsage = true;
            }
            LeaveCriticalSection(&j->cs);
            if (takeUsage) {
                s.uPrompt += tu.prompt;
                s.uCompletion += tu.completion;
                s.uTotal += tu.total ? tu.total : (tu.prompt + tu.completion);
                s.uCacheHit += tu.cacheHit;
                s.uLastPrompt = tu.prompt;
                s.uLastCompletion = tu.completion;
                s.uLastCacheHit = tu.cacheHit;
                s.uTokPerSec = (outMs > 200 && tu.completion > 0)
                    ? (double)tu.completion / ((double)outMs / 1000.0) : 0.0;
                s.usageHas = s.usageHas || tu.prompt > 0 || tu.completion > 0;
            }
            /* 工具卡片同步: steps 镜像 → 本作业 (stepBase 起) 的 role==2 消息 (追加只增;
             * 内容按版本对齐; 用户已点过确认卡的 (state 4→3) 不回写 — UI 裁决优先)。
             * 历史恢复的 role==2 卡片在 stepBase 之前, 不得被新作业的步骤误配覆盖。 */
            if (!steps.empty()) {
                int have = 0;
                for (int i = s.stepBase; i < (int)s.msgs.size(); i++)
                    if (s.msgs[i].role == 2) have++;
                /* 挂新卡片前摘掉尾部空文本气泡 (模型无文字直出工具的轮次留下的占位泡) */
                if (!s.msgs.empty() && s.msgs.back().role == 1 &&
                    s.msgs.back().text.empty() && s.msgs.back().reason.empty()) {
                    s.msgs.pop_back();
                    WebTouch(&s);
                }
                while (have < (int)steps.size()) {
                    AiMsg cm;
                    cm.role = 2;
                    s.msgs.push_back(cm);
                    have++;
                    WebTouch(&s);
                }
                int seen = 0;
                for (int i = s.stepBase; i < (int)s.msgs.size(); i++) {
                    AiMsg& m = s.msgs[i];
                    if (m.role != 2 || seen >= (int)steps.size()) continue;
                    bool uiResolved = (m.steps.size() == 1 && m.steps[0].state == 3 &&
                                       steps[seen].state == 4);   /* 确认卡已允许/拒绝: 保持 UI 态 */
                    bool changed = (int)m.steps.size() != 1 ||
                                   m.steps[0].state != steps[seen].state ||
                                   m.steps[0].count != steps[seen].count ||
                                   m.steps[0].err != steps[seen].err ||
                                   m.steps[0].top != steps[seen].top ||
                                   m.steps[0].wrote != steps[seen].wrote ||
                                   m.steps[0].name != steps[seen].name ||
                                   m.steps[0].mode != steps[seen].mode ||
                                   m.steps[0].query != steps[seen].query;
                    if (changed && !uiResolved) {
                        m.steps.assign(1, steps[seen]);
                        WebTouch(&s);
                    }
                    seen++;
                }
                s.lastStepsVer = stepsVer;
            }
            /* 工具执行期: 冻结文本气泡; 若尾部挂着空文本气泡 (本轮无文字输出) 摘掉 */
            if (phase == 1) {
                if (!s.msgs.empty() && s.msgs.back().role == 1 &&
                    s.msgs.back().text.empty() && s.msgs.back().reason.empty()) {
                    s.msgs.pop_back();
                    WebTouch(&s);
                }
            } else if (!out.empty() || !reason.empty() || s.msgs.empty() || s.msgs.back().role != 1) {
                if (s.msgs.empty() || s.msgs.back().role != 1) {
                    AiMsg am;
                    am.role = 1;
                    s.msgs.push_back(am);
                    WebTouch(&s);
                }
                AiMsg& back = s.msgs.back();
                if (back.text != out || back.reason != reason) {
                    back.text = out;
                    back.reason = reason;
                    /* 不 WebTouch: 末条流式增长走 WebSyncSession 的 last 增量径 */
                }
            }
        } else {
            /* 收尾: 状态落消息 + 中止的执行中/询问中卡片落败 (state 4 = 挂起等裁决的
               run_command 卡, 作业没了就再无人裁决) + 落库 + join + 清作业 */
            for (auto& m : s.msgs)
                for (auto& st : m.steps)
                    if (st.state == 0 || st.state == 1 || st.state == 4) { st.state = 3; st.err = L"已中止"; }
            if (s.msgs.empty() || s.msgs.back().role != 1) {
                AiMsg am;
                am.role = 1;
                s.msgs.push_back(am);
            }
            AiMsg& back = s.msgs.back();
            back.text = out;
            back.reason = reason;
            if (state == 3) {
                if (back.text.empty()) back.text = L"已停止生成";
                else back.text += L"\n\n*(已停止生成)*";
                if (!err.empty()) s.netStatus = 2;
            } else if (state == 2) {
                s.netStatus = 2;
                back.err = true;
                std::wstring et = err;
                back.text = back.text.empty() ? (L"请求失败: " + et) : (back.text + L"\n\n请求失败: " + et);
            } else {
                s.netStatus = 1;
                if (truncated) back.text += L"\n\n*(回答已截断)*";
            }
            s.sending = false;
            /* 临时诊断 (空答复排查): 泵收尾时快照到的 out 长度与开头 */
            if (g_host && g_host->StorageSet) {
                std::wstring dbg = L"state=" + std::to_wstring(state) +
                                   L" outLen=" + std::to_wstring(out.size()) +
                                   L" msgs=" + std::to_wstring(s.msgs.size()) +
                                   L"\noutHead=" + out.substr(0, 200);
                g_host->StorageSet(g_ctx, "调试收尾", U8(dbg).c_str(), (int)U8(dbg).size());
            }
            WebTouch(&s);
            s.curId = HistUpsert(s.curId, s.msgs);
            histChanged = true;
            s.job = NULL;
            s.lastStepsVer = -1;
            if (j->th) {
                j->th->join();   /* state 已置 = 线程将尽, join 只等收尾微秒 */
                delete j->th;
                j->th = NULL;
            }
            delete j;
        }
        WebSyncSession(&s);
    }
    if (histChanged) WebSyncHist();
    /* 孤儿清扫 (会话已关而流未完) */
    for (size_t i = 0; i < s_orphans.size();) {
        AiJob* j = s_orphans[i];
        bool done = false;
        EnterCriticalSection(&j->cs);
        done = j->state != 0;
        LeaveCriticalSection(&j->cs);
        if (done) {
            if (j->th) { j->th->join(); delete j->th; }
            delete j;
            s_orphans.erase(s_orphans.begin() + i);
        } else {
            i++;
        }
    }
}

/* ==================== 消息窗口 (流式泵) ==================== */

static LRESULT CALLBACK AiMsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case XJS_AI_STREAM:
            PumpStreams();
            return 0;
        case XJS_AI_UIJOB:
            AgentUiDispatch((AiUiJob*)lParam);   /* 工具编组: agent 工作线程投递的宿主扩展 API 调用 */
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

bool AiMsgWndCreate() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = AiMsgWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"XjsAiAssistantMsg";
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    g_msgwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"XjsAiAssistantMsg", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL,
                               wc.hInstance, NULL);
    return g_msgwnd != NULL;
}
