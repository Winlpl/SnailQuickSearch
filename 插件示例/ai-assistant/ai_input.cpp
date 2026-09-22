/*
 * ai_input.cpp — 交互层: 命中测试 / 鼠标滚轮键盘 IME / 输入框编辑 (选区/撤销/
 * 右键菜单) / 消息文本选区 / 接口设置对话框 / 历史侧栏操作。
 */
#include "ai_assistant.h"

/* ==================== 接口设置对话框字段编辑 (单行全模型: 点定位/框选/全选/剪贴板 — 主输入框同款) ==================== */

/* 字段显示串 (密钥字段 = 等长 ● 串, 一个 UTF-16 单元一点, 与编辑下标 1:1) */
static std::wstring FieldDispOf(AiSess* s, int id) {
    const std::wstring& v = id == 1 ? s->dUrl : id == 2 ? s->dKey : s->dModel;
    if (id != 2) return v;
    return std::wstring(minf((size_t)14, v.size()), L'●');
}

/* 逐 UTF-16 单元累计宽 (末点归一到整串实宽) — atom 词内按比例插值曾偏差 1-2 字符, 点定位/光标 x 不准 */
static void FieldCumWidths(AiSess* s, const std::wstring& t, std::vector<float>* cum) {
    cum->assign(t.size() + 1, 0.0f);
    if (t.empty()) return;
    Gdiplus::Bitmap tmp(1, 1, PixelFormat32bppPARGB);
    Gdiplus::Graphics mg(&tmp);
    float acc = 0;
    for (size_t i = 0; i < t.size(); ) {
        size_t n = ((t[i] & 0xFC00) == 0xD800 && i + 1 < t.size()) ? 2 : 1;   /* 代理对整体 */
        float w = AiMeasure(mg, s->fBody, t.substr(i, n));
        (*cum)[i + n] = acc + w;
        if (n == 2) (*cum)[i + 1] = acc + w * 0.5f;   /* 代理对中间点 (点击落在其内=取前) */
        acc += w;
        i += n;
    }
    float total = AiMeasure(mg, s->fBody, t);
    if (acc > 0 && total > 0) {
        float k = total / acc;
        for (auto& v : *cum) v *= k;   /* 逐字测量无排版微调 — 末点对齐整串实宽, 内部线性归一 */
    }
}

/* 显示串前 n 个 UTF-16 单元的宽 (选区底/光标 x) */
float FieldWidthOf(AiSess* s, const std::wstring& t, size_t n) {
    if (n == 0) return 0;
    if (n > t.size()) n = t.size();
    std::vector<float> cum;
    FieldCumWidths(s, t, &cum);
    return cum[n];
}

/* 点定位: x (相对文本原点) → 字符边界 (逐字实测宽, 半宽取前/后) */
static size_t FieldIndexFromX(AiSess* s, const std::wstring& t, float px) {
    if (px <= 0) return 0;
    std::vector<float> cum;
    FieldCumWidths(s, t, &cum);
    for (size_t i = 0; i < t.size(); ) {
        size_t n = ((t[i] & 0xFC00) == 0xD800 && i + 1 < t.size()) ? 2 : 1;
        if (px < cum[i + n])
            return (px - cum[i]) * 2.0f < cum[i + n] - cum[i] ? i : i + n;
        i += n;
    }
    return t.size();
}

/* 可视窗口: 整串放得下就全显; 溢出左裁看尾 (密钥习惯); 光标被裁掉时改锚光标向右裁 — 渲染/点定位同源 */
size_t FieldVisWindow(AiSess* s, const std::wstring& disp, float maxW, std::wstring* visOut) {
    Gdiplus::Bitmap tmp(1, 1, PixelFormat32bppPARGB);
    Gdiplus::Graphics mg(&tmp);
    std::wstring vis = disp;
    size_t skip = 0;
    while (vis.size() > 1 && AiMeasure(mg, s->fBody, vis) > maxW) { vis.erase(0, 1); skip++; }
    if (s->dCaret < skip) {   /* 光标在窗口左缘外: 改从光标起向右裁 */
        skip = s->dCaret;
        vis = disp.substr(skip);
        size_t tail = vis.size();
        while (tail > 1 && AiMeasure(mg, s->fBody, vis.substr(0, tail)) > maxW) tail = PrevCp(vis, tail);
        vis.erase(tail);
    }
    *visOut = vis;
    return skip;
}

/* 点定位: x (相对文本原点) → 字符边界 — FieldIndexFromX / FieldWidthOf 见上 (逐字实测宽同源) */

/* 对话框字段 IME 锚点 (面板坐标; 组字/候选窗跟随字段光标) */
static void DlgUpdateIme(AiSess* s) {
    if (!HOST_PANEL_OK || !g_host || s->dFocus < 1 || s->dFocus > 3) return;
    FRect* rc = s->dFocus == 1 ? &s->dUrlR : s->dFocus == 2 ? &s->dKeyR : &s->dModelR;
    std::wstring disp = FieldDispOf(s, s->dFocus);
    if (s->dCaret > disp.size()) s->dCaret = disp.size();
    std::wstring vis;
    size_t skip = FieldVisWindow(s, disp, rc->w - 16.0f * s->scale, &vis);
    float cx = FieldWidthOf(s, vis, s->dCaret - skip);
    g_host->PanelSetCaret(g_ctx, s->tok, (int)(rc->x + 8.0f * s->scale + cx), (int)(rc->y + rc->h / 2));
}

/* ==================== 交互 (命中 / 鼠标 / 滚轮 / 键盘) ==================== */


/* 输入框几何 (渲染/命中/IME 锚点三处同源) */
static void InputGeom(AiSess* s, float* bx, float* by, float* bw, float* bh, float* inH) {
    float k = s->scale;
    float pad = 12.0f * k, sendW = 56.0f * k;
    float boxInnerW = s->w - pad * 2 - sendW - 8.0f * k - 10.0f * k * 2;
    int inLines = InputLineCount(s, boxInnerW);
    float boxH = inLines * AI_LINE_H * k + 10.0f * k * 2;
    float ih = boxH + 16.0f * k + 12.0f * k;
    if (bx) *bx = pad;
    if (by) *by = s->bh - ih + 4.0f * k;
    if (bw) *bw = s->w - pad * 2 - sendW - 8.0f * k;
    if (bh) *bh = boxH;
    if (inH) *inH = ih;
}

/* 光标所在行滚入输入框可视窗口 (4 行); 未溢出归零。光标移动/内容变化路径经 UpdateImeAnchor 调 */
static void InEnsureCaret(AiSess* s) {
    float bx, by, bw2, bh2;
    InputGeom(s, &bx, &by, &bw2, &bh2, NULL);
    std::vector<InpLine> lay;
    InputLayout(s, bw2 - 20.0f * s->scale, &lay);
    int line; float x;
    InputPosOf(s, lay, bw2 - 20.0f * s->scale, s->caret, &line, &x);
    if ((int)lay.size() <= 4) { s->inScroll = 0; return; }
    if (line < s->inScroll) s->inScroll = line;
    if (line > s->inScroll + 3) s->inScroll = line - 3;
    if (s->inScroll > (int)lay.size() - 4) s->inScroll = (int)lay.size() - 4;
    if (s->inScroll < 0) s->inScroll = 0;
}

/* 输入框溢出行数 (>0 = 内容超出 4 行可视窗, 可滚动) */
static int InOverflowLines(AiSess* s) {
    float bx, by, bw2, bh2;
    InputGeom(s, &bx, &by, &bw2, &bh2, NULL);
    std::vector<InpLine> lay;
    InputLayout(s, bw2 - 20.0f * s->scale, &lay);
    int over = (int)lay.size() - 4;
    return over > 0 ? over : 0;
}

static int HitTest(AiSess* s, float x, float y, int* idxOut) {
    *idxOut = -1;
    if (s->dlg) {   /* 对话框开着: 只认对话框 */
        if (s->dSaveR.Hit(x, y)) return HIT_DSAVE;
        if (s->dCancelR.Hit(x, y)) return HIT_DCANCEL;
        if (s->dChkR.Hit(x, y)) return HIT_DCHK;
        if (s->dUrlR.Hit(x, y)) return HIT_DURL;
        if (s->dKeyR.Hit(x, y)) return HIT_DKEY;
        if (s->dModelR.Hit(x, y)) return HIT_DMODEL;
        return HIT_NONE;
    }
    if (s->hClose.Hit(x, y)) return HIT_CLOSE;
    if (s->hSet.Hit(x, y)) return HIT_HSET;
    if (s->hHist.Hit(x, y)) return HIT_HHIST;
    if (s->hNew.Hit(x, y)) return HIT_HNEW;
    if (s->sideOpen) {
        bool inSide = s->sidePanel.Hit(x, y);
        if (!inSide && y >= AI_HEAD_H * s->scale)
            return HIT_NONE;   /* 浮层开着: 浮层外的内容区不透 hover/点击 (底下的发送/消息/输入框不亮) */
        if (inSide) {
            if (s->hClear.Hit(x, y)) return HIT_CLEAR;
            for (int i = 0; i < (int)s->hRows.size(); i++) {
                if (s->hRows[i].Hit(x, y)) {
                    if (i < (int)s->hRowDel.size() && s->hRowDel[i].Hit(x, y)) { *idxOut = i; return HIT_SBDEL + i; }
                    *idxOut = i;
                    return HIT_SBROW + i;   /* 行号进命中码 — hover 高亮按码+行号比对 (曾裸返回基码 = 悬停永远亮第 0 行) */
                }
            }
            return HIT_NONE;   /* 浮层内空白: 无命中, 也不落到浮层底下 */
        }
    }
    if (s->hSend.Hit(x, y)) return HIT_SEND;
    if (s->hThumb.w > 0 && s->hThumb.Hit(x, y)) return HIT_THUMB;
    if (s->hInThumb.w > 0 && s->hInThumb.Hit(x, y)) return HIT_INTHUMB;
    float bx, by, bw, bh;
    InputGeom(s, &bx, &by, &bw, &bh, NULL);
    if (x >= bx && x < bx + bw && y >= by && y < by + bh) return HIT_INPUT;
    if (s->hThumb.w > 0 && x >= s->hThumb.x - 4.0f * s->scale &&
        x < s->hThumb.x + s->hThumb.w + 4.0f * s->scale &&
        y >= s->thumbTrackY && y < s->thumbTrackY + s->thumbTrackH) return HIT_TRACK;
    /* 推理块头: 消息流区域内, 按气泡几何反推 (宽=气泡, 高=头部行 24*scale) */
    if (y >= AI_HEAD_H * s->scale) {
        for (int i = 0; i < (int)s->msgs.size(); i++) {
            const AiMsg& m = s->msgs[i];
            if (m.reason.empty() || (size_t)i >= s->layOffsets.size()) continue;
            float k = s->scale;
            float gy = AI_HEAD_H * k + s->layOffsets[i] - (float)s->scrollY;
            if (y < gy + 10.0f * k || y > gy + 10.0f * k + 24.0f * k) continue;
            float bubW = s->lay[i].bubbleW;
            float bubX = m.role == 0 ? (s->w - 12.0f * k - 24.0f * k - 8.0f * k - bubW)
                                     : (12.0f * k + 24.0f * k + 8.0f * k);
            if (x >= bubX && x <= bubX + bubW) { *idxOut = i; return HIT_REASON; }
        }
        /* 工具卡片头 (点击=展开/收起样本列表): 遍历 role==2 消息, 气泡内逐 step 定位头行 */
        {
            float k = s->scale;
            for (int i = 0; i < (int)s->msgs.size(); i++) {
                const AiMsg& m = s->msgs[i];
                if (m.role != 2 || (size_t)i >= s->layOffsets.size()) continue;
                const AiSess::MsgLayout& L = s->lay[i];
                if ((int)L.stepHs.size() != (int)m.steps.size()) continue;   /* 排版未同步 */
                float gy = AI_HEAD_H * k + s->layOffsets[i] - (float)s->scrollY;
                float bubX = 12.0f * k + 24.0f * k + 8.0f * k;   /* AI 气泡恒左侧 */
                if (x < bubX || x > bubX + L.bubbleW) continue;
                float curY = gy + 10.0f * k;
                for (int si = 0; si < (int)m.steps.size(); si++) {
                    if (y >= curY && y < curY + 22.0f * k) { *idxOut = i; return HIT_STEPHEAD + si; }
                    curY += L.stepHs[si] + 6.0f * k;
                }
            }
        }
    }
    return HIT_NONE;
}

/* ---- 会话级操作 ---- */

/* ---- 对话框 ---- */
/* 对话框键盘让渡 (面板接管宿主闸: plugPanelKey 不开 = 键盘根本不进插件 —
   开对话框即请求, 关闭后按输入框聚焦态归还) */
static void DlgKeyboard(AiSess* s, int want) {
    if (HOST_PANEL_OK && g_host) g_host->PanelSetFocus(g_ctx, s->tok, want);
}
static int DlgKeyboardAfter(AiSess* s) {
    return s->inputFocus ? 1 : 0;   /* 关对话框后: 输入框仍聚焦则键盘留给输入框 */
}
static void DlgOpen(AiSess* s) {
    s->dlg = true;
    s->dUrl = g_cfg.baseUrl;
    s->dKey = g_cfg.apiKey;
    s->dModel = g_cfg.model;
    s->dReason = g_cfg.reasoning;
    s->dFocus = 1;   /* 默认聚焦接口地址 (键盘已让渡, Tab 可切) */
    s->dCaret = s->dAnchor = s->dUrl.size();
    DlgKeyboard(s, 1);
}
static void DlgClose(AiSess* s) {
    s->dlg = false;
    DlgKeyboard(s, DlgKeyboardAfter(s));
}
static void DlgSave(AiSess* s) {
    std::wstring url = TrimW(s->dUrl);
    std::wstring model = TrimW(s->dModel);
    if (!url.empty()) g_cfg.baseUrl = url;
    if (!model.empty()) g_cfg.model = model;
    g_cfg.apiKey = TrimW(s->dKey);
    g_cfg.reasoning = s->dReason;
    CfgSave();
    DlgClose(s);
    RenderDeliver(s);
}

/* ---- 历史 ---- */
/* 命中/悬停值 = 视觉行号 (最新在上), g_hist 按时间序存储 (最新在尾) — 视觉行号 → 数据下标 */
static int HistRowToData(int row) { return (int)g_hist.size() - 1 - row; }
static void LoadConv(AiSess* s, int i) {
    if (i < 0 || i >= (int)g_hist.size()) return;
    AbortSend(s);
    SessSaveConv(s);   /* 当前未存对话先存 */
    s->msgs = g_hist[i].msgs;
    MsgSelClear(s);   /* 会话切换, 选区失效 (含键盘归还) */
    s->curId = g_hist[i].id;
    s->layDirty = true;
    s->sticky = true;
    s->sideOpen = false;
    s->clearArm = false;
}
static void DeleteConv(AiSess* s, int i) {
    if (i < 0 || i >= (int)g_hist.size()) return;
    unsigned long long id = g_hist[i].id;
    g_hist.erase(g_hist.begin() + i);
    HistSave();
    if (s->curId == id) { s->curId = 0; s->msgs.clear(); s->layDirty = true; }
}
static void ClearHist(AiSess* s) {
    g_hist.clear();
    HistSave();
    s->curId = 0;
    s->msgs.clear();
    s->layDirty = true;
    s->clearArm = false;
    s->sideOpen = false;
}
static void NewConv(AiSess* s) {
    AbortSend(s);
    SessSaveConv(s);
    s->msgs.clear();
    s->curId = 0;
    s->layDirty = true;
    s->sticky = true;
    s->sideOpen = false;
}

/* ---- 输入编辑 (选区/撤销; 对齐搜索框: 输入替换选区, Backspace/Delete 删选区, 单档撤销) ---- */
bool InputSelRange(AiSess* s, size_t* a, size_t* b) {
    if (s->anchor == s->caret) return false;
    *a = s->anchor < s->caret ? s->anchor : s->caret;
    *b = s->anchor < s->caret ? s->caret : s->anchor;
    return true;
}
static void InputSnapUndo(AiSess* s) {
    if (s->undoSnap) return;   /* 同一次修改序列只快照一次 (Ctrl+Z 恢复后置 false, 再改再快照) */
    s->undoText = s->input;
    s->undoCaret = s->caret;
    s->undoAnchor = s->anchor;
    s->undoSnap = true;
}
static void InsertText(AiSess* s, const std::wstring& t) {
    InputSnapUndo(s);
    size_t a, b;
    if (InputSelRange(s, &a, &b)) {   /* 有选区: 输入替换选区 */
        s->input.erase(a, b - a);
        s->caret = s->anchor = a;
    }
    if (s->input.size() + t.size() > 8000) return;
    s->input.insert(s->caret, t);
    s->caret += t.size();
    s->anchor = s->caret;
}
static void InputEraseSel(AiSess* s) {
    size_t a, b;
    if (!InputSelRange(s, &a, &b)) return;
    InputSnapUndo(s);
    s->input.erase(a, b - a);
    s->caret = s->anchor = a;
}
static void InputSelectAll(AiSess* s) { s->anchor = 0; s->caret = s->input.size(); }
static void InputCopySel(AiSess* s, bool cut) {
    size_t a, b;
    if (!InputSelRange(s, &a, &b)) return;
    if (HOST_PANEL_OK && g_host) g_host->ClipboardSetText(g_ctx, U8(s->input.substr(a, b - a)).c_str());
    if (cut) InputEraseSel(s);
}
static void InputPaste(AiSess* s) {
    char buf[65536];
    int n = g_host ? g_host->ClipboardGetText(g_ctx, buf, (int)sizeof(buf) - 1) : 0;
    if (n <= 0) return;
    buf[n] = 0;
    std::wstring t = W8(buf);
    for (auto& c : t) if (c == L'\r') c = L'\n';   /* CRLF→LF (粘贴保留换行, 搜索框口径) */
    InsertText(s, t);
}
static void InputUndo(AiSess* s) {
    if (!s->undoSnap) return;
    std::wstring ti = s->input; size_t c1 = s->caret, a1 = s->anchor;
    s->input = s->undoText; s->caret = s->undoCaret; s->anchor = s->undoAnchor;
    s->undoText = ti; s->undoCaret = c1; s->undoAnchor = a1;   /* 交换式: 再按 Z 回到当前 */
    s->undoSnap = false;
    if (s->caret > s->input.size()) s->caret = s->input.size();
    if (s->anchor > s->input.size()) s->anchor = s->input.size();
}
/* 双击选词: 码点类型 (0空白 1CJK/全角 2拉丁) 两侧扩展; 代理对经 PrevCp/NextCp 整体步进 */
static void InputSelectWord(AiSess* s, size_t idx) {
    if (s->input.empty()) return;
    if (idx >= s->input.size()) idx = s->input.size() - 1;
    if ((s->input[idx] & 0xFC00) == 0xDC00 && idx > 0) idx--;   /* 低代理归前对 */
    auto kind = [](wchar_t c) { return (c == L' ' || c == L'\n') ? 0 : ((c & 0xFC00) == 0xD800 || c >= 0x2E80) ? 1 : 2; };
    int k0 = kind(s->input[idx]);
    size_t lo = idx, hi = NextCp(s->input, idx);
    while (lo > 0 && k0 != 0 && kind(s->input[PrevCp(s->input, lo)]) == k0) lo = PrevCp(s->input, lo);
    while (hi < s->input.size() && k0 != 0 && kind(s->input[hi]) == k0) hi = NextCp(s->input, hi);
    s->anchor = lo;
    s->caret = hi;
}
/* 右键编辑菜单命令 (0剪切 1复制 2粘贴 3全选 4删除; 禁用态由调用方判) */
static void InputMenuCmd(AiSess* s, int mi) {
    switch (mi) {
        case 0: InputCopySel(s, true); break;
        case 1: InputCopySel(s, false); break;
        case 2: InputPaste(s); break;
        case 3: InputSelectAll(s); break;
        case 4: InputEraseSel(s); break;
    }
}

/* IME 锚点 → 宿主 (输入框内光标点位; 渲染几何同源换算)。
   兼作"光标滚入可视窗口"汇点 — 凡改光标/内容的路径都经这里 (漏接的路径 = 滚动不跟光标) */
static void UpdateImeAnchor(AiSess* s) {
    if (!HOST_PANEL_OK || !g_host || !s->inUse) return;
    InEnsureCaret(s);
    float bx, by, bw, bh;
    InputGeom(s, &bx, &by, &bw, &bh, NULL);
    int line = 0;
    float cx = 0;
    InputCaretPos(s, bw - 10.0f * s->scale * 2, &line, &cx);
    g_host->PanelSetCaret(g_ctx, s->tok, (int)(bx + 10.0f * s->scale + cx),
                          (int)(by + 10.0f * s->scale + (line - s->inScroll) * AI_LINE_H * s->scale));
}

/* ---- 消息文本选区 (拖选复制; 单条消息内 atom 粒度) ---- */
/* 事件点 → 消息正文 atom 定位 (行尾 = 行 atoms 数; 推理块/头像区不参与; 工具卡片组无正文) */
static bool MsgHitAtom(AiSess* s, float x, float y, int* mOut, int* lOut, int* aOut) {
    float k = s->scale;
    float pad = 12.0f * k, av = 24.0f * k, gap = 8.0f * k, bp = 10.0f * k;
    float msgsTop = AI_HEAD_H * k;
    for (int i = 0; i < (int)s->msgs.size(); i++) {
        if ((size_t)i >= s->lay.size() || (size_t)i >= s->layOffsets.size()) break;
        if (s->msgs[i].role == 2) continue;   /* 工具卡片组: 无正文 atom, 不参与文本选区 */
        float gy = msgsTop + s->layOffsets[i] - (float)s->scrollY;
        if (y < gy || y > gy + s->lay[i].totalH) continue;
        const AiSess::MsgLayout& L = s->lay[i];
        const AiMsg& m = s->msgs[i];
        float bubbleX = m.role == 0 ? (s->w - pad - av - gap - L.bubbleW) : (pad + av + gap);
        float yCur = gy + bp;
        if (!m.reason.empty()) yCur += (m.reasonOpen ? L.reasonH + 4.0f * k : 24.0f * k);
        for (int li = 0; li < (int)L.lines.size(); li++) {
            const AiLine& ln = L.lines[li];
            bool last = li == (int)L.lines.size() - 1;
            if (ln.runs.empty()) { yCur += ln.h; continue; }   /* 垫行并入下一可见行 */
            if (y <= yCur + ln.h || last) {
                float rel = x - (bubbleX + bp);
                *mOut = i; *lOut = li;
                int ai = (int)ln.runs.size();   /* 默认行尾 */
                for (int a2 = 0; a2 < (int)ln.runs.size(); a2++) {
                    if (rel < ln.runs[a2].x + ln.runs[a2].w * 0.5f) { ai = a2; break; }   /* 过半归后 (点定位同口径) */
                }
                *aOut = ai;
                return true;
            }
            yCur += ln.h;
        }
    }
    return false;
}
/* 选区两端归一 (小端在前) */
void MsgSelNorm(AiSess* s, int* aL, int* aA, int* bL, int* bA) {
    if (s->selALine > s->selBLine || (s->selALine == s->selBLine && s->selAAtom > s->selBAtom)) {
        *aL = s->selBLine; *aA = s->selBAtom; *bL = s->selALine; *bA = s->selAAtom;
    } else {
        *aL = s->selALine; *aA = s->selAAtom; *bL = s->selBLine; *bA = s->selBAtom;
    }
}
/* 清消息选区: 借用的键盘一并归还宿主 (选区没了, 键盘回搜索框/列表) */
void MsgSelClear(AiSess* s) {
    s->txtSel = false;
    if (s->msgKeyBorrow && !s->inputFocus) {
        s->msgKeyBorrow = false;
        if (HOST_PANEL_OK && g_host) g_host->PanelSetFocus(g_ctx, s->tok, 0);
    }
}
/* 复制选区文本 (行界加换行; runs 即渲染文本 — markdown 标记已剥离) */
static void MsgCopySel(AiSess* s) {
    if (!s->txtSel || s->selMsg < 0 || (size_t)s->selMsg >= s->lay.size() || !g_host) return;
    int aL, aA, bL, bA;
    MsgSelNorm(s, &aL, &aA, &bL, &bA);
    const AiSess::MsgLayout& L = s->lay[s->selMsg];
    std::wstring out;
    for (int li = aL; li <= bL && li < (int)L.lines.size(); li++) {
        const AiLine& ln = L.lines[li];
        int lo = li == aL ? aA : 0;
        int hi = li == bL ? bA : (int)ln.runs.size();
        if (li > aL) {
            /* 只在逻辑行界插 \n (块首/代码行首/垫行); 软折行续行直接拼接,
               排版时被丢的行首空格在此补回 ("ORDER BY" 不裂成两行/不粘成 "ORDERBY") */
            if (ln.hard) out += L"\n";
            else if (ln.leadSpace) out += L" ";
        }
        for (int ai = lo; ai < hi && ai < (int)ln.runs.size(); ai++) out += ln.runs[ai].text;
    }
    if (!out.empty()) {
        int rc = g_host->ClipboardSetText(g_ctx, U8(out).c_str());
        if (rc == XJS_PLUGIN_OK) g_host->Toast(g_ctx, s->tok, "已复制", XJS_PLUGIN_TOAST_SUCCESS);
        else g_host->Toast(g_ctx, s->tok, "复制失败 (剪贴板写入被拒)", XJS_PLUGIN_TOAST_WARN);
    }
}

/* 历史侧栏 = 浮层: 点内容区浮层之外 (头部按钮除外) = 点外关闭, 点击消费不透传底层 —
   同宿主菜单"点外按下即关"口径; 返回真 = 已关闭, 调用方渲染后直接返回 */
static bool SideDismissOutside(AiSess* s, float x, float y) {
    if (!s->sideOpen) return false;
    if (y < AI_HEAD_H * s->scale || s->sidePanel.Hit(x, y)) return false;
    s->sideOpen = false;
    s->clearArm = false;
    return true;
}

/* 鼠标/滚轮/键盘事件 (OnPanelEvent 转发; 全 UI 线程) */
void PanelMouse(AiSess* s, int type, float x, float y, unsigned flags) {
    bool shift = (flags & 2) != 0;   /* Shift+点击 = 扩展选区 (位定义同 PanelKey) */
    int idx = -1;
    int hit = x < 0 ? HIT_NONE : HitTest(s, x, y, &idx);
    switch (type) {
        case XJS_PANEL_MOUSE_MOVE: {
            if (s->pressGrab && s->hThumb.w > 0) {   /* 滚动条拖拽 */
                float trackH = s->thumbTrackH - s->hThumb.h;
                float maxScroll = s->contentH - s->thumbTrackH;
                if (trackH > 0 && maxScroll > 0) {
                    s->scrollY = maxScroll * (double)((y - s->thumbTrackY - s->pressGrab) / trackH);
                    if (s->scrollY < 0) s->scrollY = 0;
                    if (s->scrollY > maxScroll) s->scrollY = maxScroll;
                    /* 手动滚动态: 拖离底部解除吸底 (sticky=true 时渲染每帧强制回底, 拖动会被覆盖 = 纹丝不动),
                       拖回底部恢复跟随 */
                    s->sticky = (s->scrollY >= maxScroll);
                }
                RenderDeliver(s);
                return;
            }
            if (s->inGrab > 0 && s->hInThumb.w > 0 && s->inTrackH > 0) {   /* 输入框滚动条拖拽 */
                float bx, by, bw2, bh2;
                InputGeom(s, &bx, &by, &bw2, &bh2, NULL);
                std::vector<InpLine> lay;
                InputLayout(s, bw2 - 20.0f * s->scale, &lay);
                int over = (int)lay.size() - 4;
                if (over > 0) {
                    float t = (y - s->inTrackY - s->inGrab) / maxf(1.0f, s->inTrackH - s->hInThumb.h);
                    s->inScroll = (int)(over * t + 0.5f);
                    if (s->inScroll < 0) s->inScroll = 0;
                    if (s->inScroll > over) s->inScroll = over;
                }
                RenderDeliver(s);
                return;
            }
            if (s->dSelDragging) {   /* 对话框字段拖选 (anchor 固定, caret 随点走; 拖出字段=夹到 0/末尾) */
                if (s->dFocus >= 1 && s->dFocus <= 3) {
                    FRect* rc = s->dFocus == 1 ? &s->dUrlR : s->dFocus == 2 ? &s->dKeyR : &s->dModelR;
                    std::wstring disp = FieldDispOf(s, s->dFocus);
                    std::wstring vis;
                    size_t skip = FieldVisWindow(s, disp, rc->w - 16.0f * s->scale, &vis);
                    size_t i = FieldIndexFromX(s, vis, x - rc->x - 8.0f * s->scale) + skip;
                    if (i > disp.size()) i = disp.size();
                    s->dCaret = i;
                    DlgUpdateIme(s);
                }
                RenderDeliver(s);
                return;
            }
            if (s->selDragging) {   /* 输入框拖选 (anchor 固定, caret 随点走) */
                float bx, by, bw2, bh2;
                InputGeom(s, &bx, &by, &bw2, &bh2, NULL);
                std::vector<InpLine> lay;
                InputLayout(s, bw2 - 20.0f * s->scale, &lay);
                s->caret = InputIndexFromPoint(s, lay, bw2 - 20.0f * s->scale, x, y, bx, by);
                s->expectCol = -1;
                UpdateImeAnchor(s);
                RenderDeliver(s);
                return;
            }
            if (s->txtDragging) {   /* 消息文本拖选 (起点固定, 终点随点走) */
                int mI, lI, aI;
                if (MsgHitAtom(s, x, y, &mI, &lI, &aI) && mI == s->selMsg) {
                    s->selBLine = lI;
                    s->selBAtom = aI;
                }
                RenderDeliver(s);
                return;
            }
            if (hit != s->hover) { s->hover = hit; RenderDeliver(s); }
            return;
        }
        case XJS_PANEL_LDOWN: {
            if (s->menuOpen) {   /* 编辑菜单开着: 点项执行 (禁用态忽略), 点外关闭; 不穿透 */
                int cmd = -1;
                for (int i = 0; i < (int)s->menuRows.size(); i++)
                    if (s->menuRows[i].Hit(x, y)) { cmd = i; break; }
                s->menuOpen = false;
                if (cmd >= 0) {
                    bool sel = s->anchor != s->caret;
                    bool en[5] = { sel, sel, true, !s->input.empty(), sel };
                    if (en[cmd]) InputMenuCmd(s, cmd);
                    UpdateImeAnchor(s);   /* 粘贴/剪切/删除动过光标: 滚入可见区 + 锚点跟手 */
                }
                RenderDeliver(s);
                return;
            }
            if (SideDismissOutside(s, x, y)) { RenderDeliver(s); return; }
            s->press = hit;
            s->pressX = x;
            s->pressY = y;
            if (s->dlg) {
                /* 对话框开着: 点字段 = 聚焦 + 点定位 (Shift+点 = 扩展选区; 主输入框同款) */
                if (hit == HIT_DURL) s->dFocus = 1;
                else if (hit == HIT_DKEY) s->dFocus = 2;
                else if (hit == HIT_DMODEL) s->dFocus = 3;
                if (hit == HIT_DURL || hit == HIT_DKEY || hit == HIT_DMODEL) {
                    FRect* rc = hit == HIT_DURL ? &s->dUrlR : hit == HIT_DKEY ? &s->dKeyR : &s->dModelR;
                    std::wstring disp = FieldDispOf(s, s->dFocus);
                    if (s->dAnchor > disp.size()) s->dAnchor = disp.size();
                    std::wstring vis;
                    size_t skip = FieldVisWindow(s, disp, rc->w - 16.0f * s->scale, &vis);
                    size_t i = FieldIndexFromX(s, vis, x - rc->x - 8.0f * s->scale) + skip;
                    if (i > disp.size()) i = disp.size();
                    s->dCaret = i;
                    if (!shift) s->dAnchor = i;
                    s->dSelDragging = true;   /* 按住拖动 = 扩选区 (LUP 收) */
                    DlgUpdateIme(s);
                }
            } else if (hit == HIT_INPUT) {
                s->inputFocus = true;
                if (HOST_PANEL_OK && g_host) g_host->PanelSetFocus(g_ctx, s->tok, 1);
                {   /* 点定位 (普通点击 = 定位+折叠; Shift+点 = 从 anchor 扩展到点击处) */
                    float bx, by, bw2, bh2;
                    InputGeom(s, &bx, &by, &bw2, &bh2, NULL);
                    std::vector<InpLine> lay;
                    InputLayout(s, bw2 - 20.0f * s->scale, &lay);
                    size_t hitIdx = InputIndexFromPoint(s, lay, bw2 - 20.0f * s->scale, x, y, bx, by);
                    if (shift) s->caret = hitIdx;
                    else { s->caret = hitIdx; s->anchor = hitIdx; }
                }
                s->selDragging = true;
                s->expectCol = -1;
                UpdateImeAnchor(s);
            } else if (hit == HIT_THUMB) {
                s->pressGrab = y - s->hThumb.y;
                s->sticky = false;   /* 抓住滑块 = 手动滚动态 (否则渲染吸底覆盖, 拖不动) */
            } else if (hit == HIT_TRACK && s->hThumb.w > 0) {
                float maxScroll = s->contentH - s->thumbTrackH;
                if (maxScroll > 0) {
                    s->scrollY = maxScroll * (double)((y - s->thumbTrackY) / s->thumbTrackH);
                    s->sticky = (y >= s->thumbTrackY + s->thumbTrackH - s->hThumb.h);   /* 点轨道底部 = 回底恢复吸底 */
                }
                s->pressGrab = s->hThumb.h / 2;
            } else if (hit == HIT_INTHUMB) {   /* 输入框滚动条: 记抓点拖拽 */
                s->inGrab = y - s->hInThumb.y;
            } else if (s->dlg) {
                /* 对话框开着: 蒙层/卡片空白按下只清输入焦点, 底层消息不可拖选 (背景惰性) */
                s->inputFocus = false;
            } else {
                /* 消息流/空白: 命中消息正文 = 拖选起点 (向宿主借键盘, Ctrl+C 可复制); 未命中 = 清选区归还键盘 */
                bool wasInput = s->inputFocus;
                s->inputFocus = false;
                int mI = -1, lI = -1, aI = -1;
                if (MsgHitAtom(s, x, y, &mI, &lI, &aI)) {
                    s->txtSel = true;
                    s->selMsg = mI;
                    s->selALine = s->selBLine = lI;
                    s->selAAtom = s->selBAtom = aI;
                    s->txtDragging = true;
                    s->msgKeyBorrow = true;
                    if (HOST_PANEL_OK && g_host) g_host->PanelSetFocus(g_ctx, s->tok, 1);
                } else if (s->msgKeyBorrow) {
                    MsgSelClear(s);
                } else if (wasInput && HOST_PANEL_OK && g_host) {
                    g_host->PanelSetFocus(g_ctx, s->tok, 0);
                }
            }
            RenderDeliver(s);
            return;
        }
        case XJS_PANEL_LUP: {
            int press = s->press;
            float pX = s->pressX, pY = s->pressY;
            s->press = HIT_NONE;
            s->pressX = s->pressY = -1.0f;
            s->pressGrab = 0;
            s->inGrab = 0;
            s->dSelDragging = false;   /* 对话框字段拖选结束 (选区保留待复制) */
            s->selDragging = false;   /* 输入框拖选结束 (caret 已在 MOVE 跟点) */
            s->txtDragging = false;   /* 消息拖选结束 (选区保留待复制) */
            /* 对话框开着点蒙层空白 (按下与松开都在卡片外) = 取消 (原版 ai-chat 点遮罩取消口径);
               卡内空白 (标题/字段标签/按钮带空档) 不得关闭 */
            if (s->dlg && press == HIT_NONE && hit == HIT_NONE &&
                !s->dCardR.Hit(x, y) && !s->dCardR.Hit(pX, pY)) {
                DlgClose(s);
                RenderDeliver(s);
                return;
            }
            bool same = (hit == press) || (press == HIT_TRACK);
            if (!same) { RenderDeliver(s); return; }
            /* 侧栏行命中码带行号 (HIT_SBROW+i / HIT_SBDEL+i, hover 同码) — 范围判别, 不能进 switch */
            if (press >= HIT_SBROW && press < HIT_SBROW + (int)AI_CONV_MAX) { LoadConv(s, HistRowToData(idx)); }
            else if (press >= HIT_SBDEL && press < HIT_SBDEL + (int)AI_CONV_MAX) { DeleteConv(s, HistRowToData(idx)); }
            else switch (press) {
                case HIT_CLOSE:
                    /* 结束接管会话 (宿主按打开前状态恢复预览; CLOSE 事件随后回来收尾存库) */
                    if (HOST_PANEL_OK && g_host) g_host->PanelClose(g_ctx, s->tok);
                    break;
                case HIT_HSET: DlgOpen(s); break;
                case HIT_HHIST: s->sideOpen = !s->sideOpen; s->clearArm = false; s->sideScroll = 0; break;
                case HIT_HNEW: NewConv(s); break;
                case HIT_CLEAR:
                    if (s->clearArm) ClearHist(s);
                    else s->clearArm = true;
                    break;
                case HIT_REASON:
                    if (idx >= 0 && idx < (int)s->msgs.size()) {
                        s->msgs[idx].reasonOpen = !s->msgs[idx].reasonOpen;
                        MsgSelClear(s);   /* 折叠切换重排行结构, 旧选区失效 */
                        RelayoutOne(s, idx);
                    }
                    break;
                default:
                    if (press >= HIT_STEPHEAD && press < HIT_STEPHEAD + 8 &&
                        idx >= 0 && idx < (int)s->msgs.size() &&
                        s->msgs[idx].role == 2) {
                        int si = press - HIT_STEPHEAD;
                        if (si >= 0 && si < (int)s->msgs[idx].steps.size()) {
                            AiToolStep& st = s->msgs[idx].steps[si];
                            if (st.state >= 2 && !st.top.empty()) {   /* 完成且带样本才有展开 */
                                st.open = !st.open;
                                MsgSelClear(s);
                                RelayoutOne(s, idx);
                            }
                        }
                    }
                    break;
                case HIT_SEND:
                    if (s->sending) AbortSend(s);
                    else SendCurrent(s);
                    break;
                /* 对话框命令类控件: 松开触发 (同全 UI 口径) */
                case HIT_DSAVE: DlgSave(s); return;   /* DlgSave 内已渲染交付 */
                case HIT_DCANCEL: DlgClose(s); break;
                case HIT_DCHK: s->dReason = !s->dReason; break;
                case HIT_DURL: s->dFocus = 1; break;
                case HIT_DKEY: s->dFocus = 2; break;
                case HIT_DMODEL: s->dFocus = 3; break;
            }
            RenderDeliver(s);
            return;
        }
        case XJS_PANEL_DBLCLK: {   /* 双击选词 (搜索框口径: 编辑框内双击不穿透) */
            if (SideDismissOutside(s, x, y)) { RenderDeliver(s); return; }   /* 侧栏浮层外 = 关闭 */
            if (hit == HIT_INPUT) {
                s->inputFocus = true;
                if (HOST_PANEL_OK && g_host) g_host->PanelSetFocus(g_ctx, s->tok, 1);
                float bx, by, bw2, bh2;
                InputGeom(s, &bx, &by, &bw2, &bh2, NULL);
                std::vector<InpLine> lay;
                InputLayout(s, bw2 - 20.0f * s->scale, &lay);
                InputSelectWord(s, InputIndexFromPoint(s, lay, bw2 - 20.0f * s->scale, x, y, bx, by));
                UpdateImeAnchor(s);
                RenderDeliver(s);
            }
            return;
        }
        case XJS_PANEL_RUP: {   /* 输入框右键 = 编辑菜单; 消息选区右键 = 直接复制 */
            if (SideDismissOutside(s, x, y)) { RenderDeliver(s); return; }   /* 侧栏浮层外 = 关闭 */
            if (hit == HIT_INPUT) {
                s->inputFocus = true;
                if (HOST_PANEL_OK && g_host) g_host->PanelSetFocus(g_ctx, s->tok, 1);
                float k = s->scale;
                float mw = 124.0f * k, mh = 5 * 26.0f * k + 8.0f * k;
                float mx = x + 4.0f * k, my = y + 4.0f * k;
                if (mx + mw > s->w - 4.0f * k) mx = s->w - 4.0f * k - mw;
                if (my + mh > s->bh - 4.0f * k) my = s->bh - 4.0f * k - mh;
                if (mx < 4.0f * k) mx = 4.0f * k;
                if (my < 4.0f * k) my = 4.0f * k;
                s->menuBox = FRect{ mx, my, mw, mh };
                s->menuRows.clear();
                s->menuOpen = true;
                RenderDeliver(s);
            } else if (s->txtSel) {   /* 消息选区右键 = 复制 */
                MsgCopySel(s);
                RenderDeliver(s);
            }
            return;
        }
        default: return;   /* RDOWN 预留 */
    }
}

void PanelWheel(AiSess* s, float x, float y, int delta) {
    float k = s->scale;
    if (s->sideOpen && x >= s->w - minf(320.0f * k, s->w * 0.62f)) {
        s->sideScroll -= delta / 120.0 * 3 * 15.0f * k;
        float rows = (float)g_hist.size() * 50.0f * k - (s->bh - AI_HEAD_H * k - 40.0f * k);
        if (s->sideScroll < 0) s->sideScroll = 0;
        if (s->sideScroll > rows) s->sideScroll = rows;
    } else if (s->hInput.Hit(x, y)) {   /* 悬停输入框: 滚溢出内容 (一格 2 行; 未溢出不动) */
        int over = InOverflowLines(s);
        if (over > 0) {
            s->inScroll += delta < 0 ? 2 : -2;
            if (s->inScroll < 0) s->inScroll = 0;
            if (s->inScroll > over) s->inScroll = over;
        }
    } else {
        s->scrollY -= delta / 120.0 * 3 * (double)AI_LINE_H * k;
        if (s->scrollY < 0) s->scrollY = 0;
        /* 向上滚 (delta>0, 看历史) = 解除吸底 — 曾写反 (向上滚置 sticky=true), sticky=true 时
           渲染每帧强制回底, 滚动被整个吞掉 = "滚轮没反应"; 向下滚不改 sticky (滚到底后新消息
           跟随由发送/流式路径恢复) */
        if (delta > 0) s->sticky = false;
    }
    RenderDeliver(s);
}

void PanelKey(AiSess* s, unsigned vk, unsigned flags) {
    bool ctrl = (flags & 1) != 0, shift = (flags & 2) != 0;
    if (vk == VK_PROCESSKEY) return;   /* IME 组合中的键 (宿主原样转发) */
    if (s->dlg) {
        /* 对话框字段编辑 (单行全模型: 点定位/框选/全选/剪贴板; 主输入框同款语义) */
        std::wstring* f = s->dFocus == 1 ? &s->dUrl : s->dFocus == 2 ? &s->dKey : s->dFocus == 3 ? &s->dModel : NULL;
        if (vk == VK_TAB) {
            s->dFocus = s->dFocus == 3 ? 1 : s->dFocus + 1;
            std::wstring* nf = s->dFocus == 1 ? &s->dUrl : s->dFocus == 2 ? &s->dKey : &s->dModel;
            s->dCaret = s->dAnchor = nf->size();
        } else if (vk == VK_ESCAPE) {
            DlgClose(s);
            RenderDeliver(s);
            return;
        } else if (vk == VK_RETURN) {
            DlgSave(s);
        } else if (f) {
            size_t sz = f->size();
            if (s->dCaret > sz) s->dCaret = sz;
            if (s->dAnchor > sz) s->dAnchor = sz;
            bool sel = s->dAnchor != s->dCaret;
            size_t lo = s->dAnchor < s->dCaret ? s->dAnchor : s->dCaret;
            size_t hi = s->dAnchor < s->dCaret ? s->dCaret : s->dAnchor;
            switch (vk) {
                case VK_LEFT:
                    if (shift) s->dCaret = s->dCaret ? PrevCp(*f, s->dCaret) : 0;
                    else { s->dCaret = sel ? lo : (s->dCaret ? PrevCp(*f, s->dCaret) : 0); s->dAnchor = s->dCaret; }
                    break;
                case VK_RIGHT:
                    if (shift) s->dCaret = s->dCaret < sz ? NextCp(*f, s->dCaret) : sz;
                    else { s->dCaret = sel ? hi : (s->dCaret < sz ? NextCp(*f, s->dCaret) : sz); s->dAnchor = s->dCaret; }
                    break;
                case VK_HOME: s->dCaret = 0; if (!shift) s->dAnchor = 0; break;
                case VK_END: s->dCaret = sz; if (!shift) s->dAnchor = sz; break;
                case VK_BACK:
                    if (sel) { f->erase(lo, hi - lo); s->dCaret = s->dAnchor = lo; }
                    else if (s->dCaret) { size_t p = PrevCp(*f, s->dCaret); f->erase(p, s->dCaret - p); s->dCaret = s->dAnchor = p; }
                    break;
                case VK_DELETE:
                    if (sel) { f->erase(lo, hi - lo); s->dCaret = s->dAnchor = lo; }
                    else if (s->dCaret < sz) { size_t n2 = NextCp(*f, s->dCaret); f->erase(s->dCaret, n2 - s->dCaret); s->dAnchor = s->dCaret; }
                    break;
                case 'A':
                    if (ctrl) { s->dAnchor = 0; s->dCaret = sz; }
                    break;
                case 'C':
                    if (ctrl && sel && HOST_PANEL_OK && g_host)
                        g_host->ClipboardSetText(g_ctx, U8(f->substr(lo, hi - lo)).c_str());
                    break;
                case 'X':
                    if (ctrl && sel) {
                        if (HOST_PANEL_OK && g_host) g_host->ClipboardSetText(g_ctx, U8(f->substr(lo, hi - lo)).c_str());
                        f->erase(lo, hi - lo);
                        s->dCaret = s->dAnchor = lo;
                    }
                    break;
                case 'V':
                    if (ctrl) {
                        char buf[8192];
                        int n = (HOST_PANEL_OK && g_host) ? g_host->ClipboardGetText(g_ctx, buf, (int)sizeof(buf) - 1) : 0;
                        if (n > 0) {
                            buf[n] = 0;
                            std::wstring clean;
                            for (wchar_t c : W8(buf)) if (c != L'\r' && c != L'\n') clean += c;   /* 单行字段: 剔除换行 */
                            if (f->size() + clean.size() > 2048) clean.erase(2048 > f->size() ? 2048 - f->size() : 0);
                            if (sel) { f->erase(lo, hi - lo); s->dCaret = s->dAnchor = lo; }
                            f->insert(s->dCaret, clean);
                            s->dCaret += clean.size();
                            s->dAnchor = s->dCaret;
                        }
                    }
                    break;
            }
            DlgUpdateIme(s);
        }
        RenderDeliver(s);
        return;
    }
    if (!s->inputFocus) {
        /* 拖选借用键盘期: Ctrl+C = 复制选区; Esc = 清选区归还; 其它键 = 归还键盘后丢弃
           (打字意图 → 键盘回宿主搜索框, 用户重按一次即正常) */
        if (s->msgKeyBorrow) {
            if (vk == 'C' && ctrl) { MsgCopySel(s); RenderDeliver(s); return; }
            s->msgKeyBorrow = false;
            s->txtSel = false;
            if (HOST_PANEL_OK && g_host) g_host->PanelSetFocus(g_ctx, s->tok, 0);
            if (vk != VK_ESCAPE) return;
        }
        if (vk == VK_ESCAPE && s->sideOpen) { s->sideOpen = false; s->clearArm = false; RenderDeliver(s); }
        else if (vk == 'C' && ctrl && s->txtSel) { MsgCopySel(s); RenderDeliver(s); }
        return;
    }
    if (s->menuOpen) {   /* 编辑菜单开着: Esc 关闭, 其余先关菜单再处理 */
        s->menuOpen = false;
        if (vk == VK_ESCAPE) { RenderDeliver(s); return; }
    }
    /* 可视行导航/行首尾共用的布局快照 (按需) */
    std::vector<InpLine> lay;
    auto needLay = [&]() { if (lay.empty()) InputLayout(s, s->hInput.w - 20.0f * s->scale, &lay); };
    switch (vk) {
        case VK_LEFT:
            if (shift) { s->caret = PrevCp(s->input, s->caret); }
            else { size_t a, b; if (InputSelRange(s, &a, &b)) s->caret = a; else s->caret = PrevCp(s->input, s->caret); s->anchor = s->caret; }
            s->expectCol = -1; UpdateImeAnchor(s); break;
        case VK_RIGHT:
            if (shift) { s->caret = NextCp(s->input, s->caret); }
            else { size_t a, b; if (InputSelRange(s, &a, &b)) s->caret = b; else s->caret = NextCp(s->input, s->caret); s->anchor = s->caret; }
            s->expectCol = -1; UpdateImeAnchor(s); break;
        case VK_UP: case VK_DOWN: {   /* 多行可视行导航 (保持期望列) */
            needLay();
            float boxInnerW = s->hInput.w - 20.0f * s->scale;
            int line; float x;
            InputPosOf(s, lay, boxInnerW, s->caret, &line, &x);
            if (s->expectCol >= 0) x = s->expectCol;
            int tgt = vk == VK_UP ? line - 1 : line + 1;
            if (tgt < 0 || tgt > (int)lay.size() - 1) break;   /* 已在首/末行 */
            const InpLine& ln = lay[tgt];   /* 目标行内按 x 定位 */
            size_t idx = ln.start;
            bool hit = false;
            for (size_t i = 0; i < ln.atoms.size(); i++) {
                const AiAtom& a = ln.atoms[i];
                if (x < ln.cum[i] + a.w) {
                    if (a.t.size() > 1) idx += (size_t)((x - ln.cum[i]) / a.w * (float)a.t.size() + 0.5f);
                    else if (x >= ln.cum[i] + a.w * 0.5f) idx += a.t.size();
                    hit = true;
                    break;
                }
                idx += a.t.size();
            }
            if (!hit) idx = ln.start + ln.len();
            s->caret = idx;
            s->expectCol = x;
            if (!shift) s->anchor = s->caret;
            UpdateImeAnchor(s);
            break;
        }
        case VK_HOME: case VK_END: {   /* 可视行首/尾 (折行后) */
            needLay();
            float boxInnerW = s->hInput.w - 20.0f * s->scale;
            int line; float x;
            InputPosOf(s, lay, boxInnerW, s->caret, &line, &x);
            s->caret = vk == VK_HOME ? lay[line].start : lay[line].start + lay[line].len();
            if (!shift) s->anchor = s->caret;
            s->expectCol = -1; UpdateImeAnchor(s); break;
        }
        case VK_BACK:
            InputSnapUndo(s);
            if (s->anchor != s->caret) { InputEraseSel(s); }
            else if (s->caret > 0) {
                size_t p = PrevCp(s->input, s->caret);
                s->input.erase(p, s->caret - p);
                s->caret = s->anchor = p;
            }
            UpdateImeAnchor(s); break;
        case VK_DELETE:
            InputSnapUndo(s);
            if (s->anchor != s->caret) { InputEraseSel(s); }
            else if (s->caret < s->input.size()) {
                size_t n2 = NextCp(s->input, s->caret);
                s->input.erase(s->caret, n2 - s->caret);
            }
            UpdateImeAnchor(s); break;
        case VK_ESCAPE:   /* 搜索框口径: 选区→折叠, 再清空, 空则失焦 */
            if (s->anchor != s->caret) s->anchor = s->caret;
            else if (!s->input.empty()) { InputSnapUndo(s); s->input.clear(); s->caret = s->anchor = 0; }
            else s->inputFocus = false;
            UpdateImeAnchor(s); break;
        case VK_RETURN:
            if (shift) { InsertText(s, L"\n"); UpdateImeAnchor(s); }
            else { SendCurrent(s); }
            break;
        case 'A': if (ctrl) InputSelectAll(s); break;
        case 'C': if (ctrl) InputCopySel(s, false); break;
        case 'X': if (ctrl) InputCopySel(s, true); break;
        case 'Z': if (ctrl) { InputUndo(s); UpdateImeAnchor(s); } break;
        case 'V':
            if (ctrl) { InputPaste(s); UpdateImeAnchor(s); }
            break;
        default: break;
    }
    RenderDeliver(s);
}

void PanelChar(AiSess* s, unsigned int ch) {
    if (s->dlg) {
        if (ch >= 0x20 && s->dFocus >= 1 && s->dFocus <= 3) {
            std::wstring* f = s->dFocus == 1 ? &s->dUrl : s->dFocus == 2 ? &s->dKey : &s->dModel;
            if (s->dCaret > f->size()) s->dCaret = f->size();
            if (s->dAnchor > f->size()) s->dAnchor = f->size();
            if (s->dAnchor != s->dCaret) {   /* 输入替换选区 */
                size_t lo = s->dAnchor < s->dCaret ? s->dAnchor : s->dCaret;
                size_t hi = s->dAnchor < s->dCaret ? s->dCaret : s->dAnchor;
                f->erase(lo, hi - lo);
                s->dCaret = s->dAnchor = lo;
            }
            if (f->size() < 2048) { f->insert(s->dCaret, 1, (wchar_t)ch); s->dCaret++; s->dAnchor = s->dCaret; }
            DlgUpdateIme(s);
        }
        RenderDeliver(s);
        return;
    }
    if (!s->inputFocus) return;
    if (ch == L'\r' || ch == L'\t' || ch == 0x08) return;   /* Enter/Tab/退格 走 KEY_DOWN */
    if (ch >= 0x20 || ch == L'\n') {
        InsertText(s, std::wstring(1, (wchar_t)ch));
        UpdateImeAnchor(s);
        RenderDeliver(s);
    }
}
