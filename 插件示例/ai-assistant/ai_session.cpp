/*
 * ai_session.cpp — 会话层: 会话池 / 字体表面 / 发送入口 / 流泵 (UI 抽取增量与
 * 同步工具卡片) / 消息窗口。会话状态全在 AiSess (一窗一份)。
 */
#include "ai_assistant.h"

HWND g_msgwnd = NULL;    /* 消息窗口: 工作线程 PostMessage 回 UI 线程渲染 (渲染不进工作线程) */

AiSess g_sess[8];

AiSess* SessByTok(XjsWindowToken tok) {
    for (auto& s : g_sess) if (s.inUse && s.tok == tok) return &s;
    return NULL;
}
AiSess* SessFree() {
    for (auto& s : g_sess) if (!s.inUse) return &s;
    return NULL;
}

/* ---- 字体 / 表面 ---- */
static void SessFreeFonts(AiSess* s) {
    delete s->fBody; delete s->fBodyB; delete s->fBodyL; delete s->fBodyBL;
    delete s->fMono; delete s->fTiny; delete s->fTinyL; delete s->fTitle;
    s->fBody = s->fBodyB = s->fBodyL = s->fBodyBL = s->fMono = s->fTiny = s->fTinyL = s->fTitle = NULL;
    delete s->famUI; delete s->famMono;
    if (s->famLatOwn) delete s->famLat;   /* 回退共享 famUI 时只删一次 */
    s->famUI = s->famMono = NULL;
    s->famLat = NULL; s->famLatOwn = false;
    s->madeScale = 0.0f;
}
void SessEnsureFonts(AiSess* s) {
    if (s->madeScale == s->scale && s->fBody) return;
    SessFreeFonts(s);
    s->famUI = new Gdiplus::FontFamily(L"Microsoft YaHei UI");
    if (!s->famUI->IsAvailable()) { delete s->famUI; s->famUI = new Gdiplus::FontFamily(L"Microsoft YaHei"); }
    if (!s->famUI->IsAvailable()) { delete s->famUI; s->famUI = new Gdiplus::FontFamily(L"Segoe UI"); }
    s->famLat = new Gdiplus::FontFamily(L"Segoe UI");
    s->famLatOwn = s->famLat->IsAvailable();
    if (!s->famLatOwn) { delete s->famLat; s->famLat = s->famUI; }   /* 无 Segoe = 拉丁同 CJK 族 */
    s->famMono = new Gdiplus::FontFamily(L"Consolas");
    if (!s->famMono->IsAvailable()) { delete s->famMono; s->famMono = new Gdiplus::FontFamily(L"Courier New"); }
    float k = s->scale;
    s->fBody = new Gdiplus::Font(s->famUI, 12.5f * k, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    s->fBodyB = new Gdiplus::Font(s->famUI, 12.5f * k, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    s->fBodyL = new Gdiplus::Font(s->famLat, 12.5f * k, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    s->fBodyBL = new Gdiplus::Font(s->famLat, 12.5f * k, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    s->fMono = new Gdiplus::Font(s->famMono, 11.0f * k, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    s->fTiny = new Gdiplus::Font(s->famUI, 10.5f * k, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    s->fTinyL = new Gdiplus::Font(s->famLat, 10.5f * k, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    s->fTitle = new Gdiplus::Font(s->famUI, 15.0f * k, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    s->madeScale = s->scale;
}
void SessEnsureSurface(AiSess* s) {
    int bw = s->w > 0 ? s->w : 1, bh = s->h > 0 ? s->h : 1;
    if (s->bw == bw && s->bh == bh) return;
    s->stride = bw * 4;
    s->px.assign((size_t)s->stride * bh, 0);
    s->bw = bw;
    s->bh = bh;
}

/* ---- 皮肤 ---- */
static void SessLoadSkin(AiSess* s) {
    s->skinOk = false;
    if (!g_host) return;
    char buf[1024];
    int n = g_host->GetSkinJson(g_ctx, buf, (int)sizeof(buf) - 1);
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
    s->winActive = true;
    s->layDirty = true;
    static bool s_dataLoaded = false;   /* cfg/历史进程级一份, 多窗共享 */
    if (!s_dataLoaded) { s_dataLoaded = true; CfgLoad(); HistLoad(); }
    SessLoadSkin(s);
}

void SessClose(AiSess* s) {
    AbortSend(s);
    SessSaveConv(s);   /* 未落库的当前对话保存 (关面板不丢) */
    s->inUse = false;
    if (s->job) { s_orphans.push_back(s->job); s->job = NULL; }   /* 流未完 → 孤儿 (泵清扫 join) */
    s->msgs.clear();
    s->curId = 0;
    s->input.clear();
    s->lay.clear();
    s->layOffsets.clear();
    s->px.clear();
    s->px.shrink_to_fit();
    s->bw = s->bh = 0;
    SessFreeFonts(s);
}

void SendCurrent(AiSess* s) {
    if (s->sending || !g_host) return;
    std::wstring text = TrimW(s->input);
    if (text.empty()) return;
    if (g_cfg.apiKey.empty()) {
        g_host->Toast(g_ctx, s->tok, "尚未配置接口密钥 — 请点右上角 接口设置 填写", XJS_PLUGIN_TOAST_WARN);
        return;
    }
    AiMsg um;
    um.role = 0;
    um.text = text;
    s->msgs.push_back(um);
    s->input.clear();
    s->caret = 0;
    s->anchor = 0;
    s->inScroll = 0;
    s->layDirty = true;
    s->sticky = true;
    MsgSelClear(s);   /* 新消息入列重排, 旧选区失效 (含键盘归还) */
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
    /* 对话快照 (只含 role 0/1 文本消息; 工具往返由 worker 在循环中累计) */
    for (auto& m : s->msgs)
        if (m.role != 2 && !m.text.empty()) j->hist.push_back(m);
    if (j->hist.size() > 30) j->hist.erase(j->hist.begin(), j->hist.end() - 30);
    while (!j->hist.empty() && j->hist.front().role != 0) j->hist.erase(j->hist.begin());
    s->job = j;
    s->sending = true;
    s->stepBase = (int)s->msgs.size();   /* 本作业工具卡片起点 (历史恢复的 role==2 卡片在其之前) */
    s->netStatus = g_cfg.apiKey.empty() ? 0 : s->netStatus;
    j->th = new std::thread([j]() { WorkerMain(j); });
    RenderDeliver(s);
}

static void PumpStreams() {
    for (auto& s : g_sess) {
        AiJob* j = s.job;
        if (!s.inUse || !j) continue;
        std::wstring out, reason;
        int state = 0, phase = 0, stepsVer = 0;
        std::wstring err;
        bool truncated = false;
        std::vector<AiToolStep> steps;
        EnterCriticalSection(&j->cs);
        out = j->out;
        reason = j->reason;
        state = j->state;
        phase = j->phase;
        err = j->err;
        truncated = j->truncated;
        stepsVer = j->stepsVersion;
        if (stepsVer != s.lastStepsVer) steps = j->steps;   /* 有变化才拷 (少一次全量复制) */
        LeaveCriticalSection(&j->cs);
        if (state == 0) {
            /* 工具卡片同步: steps 镜像 → 本作业 (stepBase 起) 的 role==2 消息 (追加只增;
             * 内容按版本对齐; 保留 open)。历史恢复的 role==2 卡片在 stepBase 之前,
             * 不得被新作业的步骤误配覆盖。 */
            if (!steps.empty()) {
                int have = 0;
                for (int i = s.stepBase; i < (int)s.msgs.size(); i++)
                    if (s.msgs[i].role == 2) have++;
                /* 挂新卡片前摘掉尾部空文本气泡 (模型无文字直出工具的轮次留下的占位泡) */
                if (!s.msgs.empty() && s.msgs.back().role == 1 &&
                    s.msgs.back().text.empty() && s.msgs.back().reason.empty()) {
                    s.msgs.pop_back();
                    s.layDirty = true;
                }
                while (have < (int)steps.size()) {
                    AiMsg cm;
                    cm.role = 2;
                    s.msgs.push_back(cm);
                    have++;
                    s.layDirty = true;
                }
                int seen = 0;
                for (int i = s.stepBase; i < (int)s.msgs.size(); i++) {
                    AiMsg& m = s.msgs[i];
                    if (m.role != 2 || seen >= (int)steps.size()) continue;
                    bool changed = (int)m.steps.size() != 1 ||
                                   m.steps[0].state != steps[seen].state ||
                                   m.steps[0].count != steps[seen].count ||
                                   m.steps[0].err != steps[seen].err ||
                                   m.steps[0].top != steps[seen].top ||
                                   m.steps[0].name != steps[seen].name ||
                                   m.steps[0].mode != steps[seen].mode ||
                                   m.steps[0].query != steps[seen].query;
                    if (changed) {
                        bool wasOpen = !m.steps.empty() && m.steps[0].open;
                        steps[seen].open = wasOpen;
                        m.steps.assign(1, steps[seen]);
                        RelayoutOne(&s, i);
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
                    s.layDirty = true;
                }
            } else if (!out.empty() || !reason.empty() || s.msgs.empty() || s.msgs.back().role != 1) {
                if (s.msgs.empty() || s.msgs.back().role != 1) {
                    AiMsg am;
                    am.role = 1;
                    am.reasonOpen = true;
                    s.msgs.push_back(am);
                    s.layDirty = true;
                }
                AiMsg& back = s.msgs.back();
                if (back.text != out || back.reason != reason) {
                    bool reasonGrew = reason.size() > back.reason.size();
                    back.text = out;
                    back.reason = reason;
                    if (reasonGrew) back.reasonOpen = true;
                    RelayoutOne(&s, (int)s.msgs.size() - 1);
                }
                if (back.reason.empty() && back.reasonOpen) back.reasonOpen = false;
            }
        } else {
            /* 收尾: 状态落消息 + 中止的执行中卡片落败 + 落库 (role==2 工具卡片一并入库) + join + 清作业 */
            for (auto& m : s.msgs)
                for (auto& st : m.steps)
                    if (st.state == 0 || st.state == 1) { st.state = 3; st.err = L"已中止"; }
            if (s.msgs.empty() || s.msgs.back().role != 1) {
                AiMsg am;
                am.role = 1;
                s.msgs.push_back(am);
            }
            AiMsg& back = s.msgs.back();
            back.text = out;
            back.reason = reason;
            back.reasonOpen = !reason.empty();   /* 收尾默认展开推理 (有内容才展开; 用户可点收) */
            if (state == 3) {
                if (back.text.empty()) back.text = L"已停止生成";
                else back.text += L"\n\n*(已停止生成)*";
                if (!err.empty()) s.netStatus = 2;
            } else if (state == 2) {
                s.netStatus = 2;
                std::wstring et = err;
                back.text = back.text.empty() ? (L"请求失败: " + et) : (back.text + L"\n\n请求失败: " + et);
            } else {
                s.netStatus = 1;
                if (truncated) back.text += L"\n\n*(回答已截断)*";
            }
            s.sending = false;
            s.layDirty = true;
            MsgSelClear(&s);   /* 收尾重排, 选区失效 (含键盘归还) */
            s.curId = HistUpsert(s.curId, s.msgs);
            s.job = NULL;
            s.lastStepsVer = -1;
            if (j->th) {
                j->th->join();   /* state 已置 = 线程将尽, join 只等收尾微秒 */
                delete j->th;
                j->th = NULL;
            }
            delete j;
        }
        RenderDeliver(&s);
    }
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

/* ==================== 消息窗口 (流式泵 + 光标闪烁节拍) ==================== */

static LRESULT CALLBACK AiMsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case XJS_AI_STREAM:
            PumpStreams();
            return 0;
        case WM_TIMER:   /* 光标闪烁: 输入框聚焦的会话重渲染 (530ms 相位在渲染里取 GetTickCount64) */
            if (wParam == 1)
                for (auto& s : g_sess)
                    if (s.inUse && (s.inputFocus || s.dlg || s.sending)) RenderDeliver(&s);
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
