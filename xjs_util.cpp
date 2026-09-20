/*
 * xjs_util.cpp — 通用工具: 编码/时间/数字/剪贴板/文件操作/自启动/提权
 */
#include "xjs_app.h"
#include <shlobj.h>      /* IFileDialog 选文件夹 (CLSID_FileDialog/SIGDN_FILESYSPATH) */
#include <shlwapi.h>     /* AssocQueryString: 打开文件关联命令解析 (降权通道) */
#include <exdisp.h>      /* IShellWindows/IShellDispatch2: 非管理员令牌打开 (借 Explorer 通道) */
#include <thread>        /* 异步线程打开文件 (设置-打开) */

std::wstring XjsGetExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    wchar_t* p = wcsrchr(path, L'\\');
    if (p) *p = L'\0';
    return path;
}

/* 每窗有效 DPI 唯一入口。事实源 = GetDpiForWindow (系统随 WM_DPICHANGED 同步维护);
   仅在其不可用 (Win10 1607 以下) 时回落 GetDeviceCaps — 后者在 RDP 改缩放时不实时,
   禁止当事实源用 (见 XjsSyncViewport 注)。hwnd=NULL = 桌面口径 (建窗前取默认尺寸/字体用)。 */
UINT XjsWindowDpi(HWND hwnd) {
    UINT dpi = XjsGetDpiForWindow(hwnd);
    if (dpi) return dpi;
    HDC sdc = GetDC(hwnd);
    dpi = sdc ? (UINT)GetDeviceCaps(sdc, LOGPIXELSX) : 96;
    if (sdc) ReleaseDC(hwnd, sdc);
    return dpi;
}

std::wstring Utf8ToUtf16(const char* str) {
    if (!str) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, str, -1, NULL, 0);
    if (len <= 0) return L"";
    std::wstring r(len - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, str, -1, &r[0], len);
    return r;
}

std::string Utf16ToUtf8(const wchar_t* str) {
    if (!str) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, str, -1, NULL, 0, NULL, NULL);
    if (len <= 0) return "";
    std::string r(len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, str, -1, &r[0], len, NULL, NULL);
    return r;
}

/* 去首尾空白 (默认 \r\n\t 空格): 全库唯一实现 —— 设置关键词/窗口名、搜索模式名称、
   托管标签、皮肤解析等处共用 (曾各自复制一份)。set 参数供只想去空格制表位的变体使用。 */
std::wstring XjsTrimWs(const std::wstring& s, const wchar_t* set) {
    size_t b = s.find_first_not_of(set);
    if (b == std::wstring::npos) return L"";
    size_t e = s.find_last_not_of(set);
    return s.substr(b, e - b + 1);
}

/* 相对时间徽章 (正式版 time-ago: 刚刚/N分钟/N小时/N天/N个月/N年) */
std::wstring XjsTimeBadge(long long ms) {
    if (ms <= 0) return L"-";
    long long ftVal = ms * 10000LL + 116444736000000000LL;
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    long long now = ((long long)nowFt.dwHighDateTime << 32) | nowFt.dwLowDateTime;
    long long diffSec = (now - ftVal) / 10000000LL;
    if (diffSec < 0) diffSec = 0;
    wchar_t buf[32];
    if (diffSec < 60) return XjsT(L"时间.刚刚");
    if (diffSec < 3600) return XjsFmt(XjsT(L"时间.N分钟"), XjsNumText(diffSec / 60));
    if (diffSec < 86400) return XjsFmt(XjsT(L"时间.N小时"), XjsNumText(diffSec / 3600));
    long long days = diffSec / 86400;
    if (days < 30) return XjsFmt(XjsT(L"时间.N天"), XjsNumText(days));
    long long months = days / 30;
    if (months < 12) return XjsFmt(XjsT(L"时间.N个月"), XjsNumText(months));
    return XjsFmt(XjsT(L"时间.N年"), XjsNumText(months / 12));
}

/* 文件属性位 → 标签 (源样式 formatAttrs: 目录/只读/隐藏/系统/归档; 0/全不认识 = '-') */
std::wstring XjsAttrsText(unsigned long a) {
    if (!a) return L"-";
    std::wstring s;
    if (a & 0x10) { s += XjsT(L"属性.目录"); s += L' '; }   // FILE_ATTRIBUTE_DIRECTORY
    if (a & 0x01) { s += XjsT(L"属性.只读"); s += L' '; }   // FILE_ATTRIBUTE_READONLY
    if (a & 0x02) { s += XjsT(L"属性.隐藏"); s += L' '; }   // FILE_ATTRIBUTE_HIDDEN
    if (a & 0x04) { s += XjsT(L"属性.系统"); s += L' '; }   // FILE_ATTRIBUTE_SYSTEM
    if (a & 0x20) { s += XjsT(L"属性.归档"); s += L' '; }   // FILE_ATTRIBUTE_ARCHIVE
    while (!s.empty() && s.back() == L' ') s.pop_back();
    return s.empty() ? L"-" : s;
}

/* {0}{1}{2} 占位替换 (i18n 模板键: 语序随语言变, 拼接必须过模板) */
std::wstring XjsFmt(const wchar_t* tpl, const std::wstring& a0,
                    const std::wstring& a1, const std::wstring& a2) {
    std::wstring s = tpl;
    const std::wstring* args[3] = { &a0, &a1, &a2 };
    for (int i = 0; i < 3; i++) {
        wchar_t tag[8]; _snwprintf(tag, 8, L"{%d}", i);
        size_t pos;
        while ((pos = s.find(tag)) != std::wstring::npos) {
            s.replace(pos, 3, *args[i]);
        }
    }
    return s;
}

/* 完整时间 (预览面板信息行用) */
std::wstring XjsTimeText(long long ms) {
    if (ms <= 0) return L"-";
    long long ftVal = ms * 10000LL + 116444736000000000LL;
    FILETIME ft = { (DWORD)ftVal, (DWORD)(ftVal >> 32) };
    SYSTEMTIME st, local;
    FileTimeToSystemTime(&ft, &st);
    SystemTimeToTzSpecificLocalTime(NULL, &st, &local);
    wchar_t buf[64];
    _snwprintf(buf, 64, L"%04d-%02d-%02d %02d:%02d", local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute);
    return buf;
}

std::wstring XjsNumText(long long n) {
    wchar_t buf[32], out[48] = {0};
    _snwprintf(buf, 32, L"%lld", n);
    int len = (int)wcslen(buf), o = 0;
    for (int i = 0; i < len; i++) {
        out[o++] = buf[i];
        int rem = len - 1 - i;
        if (rem > 0 && rem % 3 == 0) out[o++] = L',';
    }
    out[o] = 0;
    return out;
}

/* 正式版状态栏格式: 4,483,452 (448.3万); 万位整除不带小数 (源样式 formatCount: 4480万)。
   "万" 单位走 i18n (英文系 {0}0K: 448.3万 → 448.30K, 数值口径不变) */
std::wstring XjsWanText(long long n) {
    std::wstring s = XjsNumText(n);
    if (n >= 10000) {
        double wan = (double)n / 10000.0;
        wchar_t num[32];
        if (wan == (double)(long long)wan) _snwprintf(num, 32, L"%.0f", wan);
        else _snwprintf(num, 32, L"%.1f", wan);
        s += XjsFmt(XjsT(L"时间.N万"), num);
    }
    return s;
}

void XjsCopyClipboard(const std::wstring& text) {
    if (!g_hWnd || !OpenClipboard(g_hWnd)) return;
    EmptyClipboard();
    size_t sz = (text.length() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sz);
    if (hMem) {
        void* lk = GlobalLock(hMem);   /* 极端内存压力下可为 NULL, 直写 NULL = 崩溃 */
        if (lk) {
            memcpy(lk, text.c_str(), sz);
            GlobalUnlock(hMem);
            if (!SetClipboardData(CF_UNICODETEXT, hMem)) GlobalFree(hMem);   /* 未被接管才释放 */
        } else {
            GlobalFree(hMem);
        }
    }
    CloseClipboard();
    g_clipSeqOurs = GetClipboardSequenceNumber();   /* 剪贴板监听忽略自己 */
}

std::wstring XjsItemPath(int idx) {
    if (!g_engine || !g_result || idx < 0) return L"";
    int fileId = g_result ? xjs_result_GetFileId(g_result, idx) : -1;
    if (fileId < 0) return L"";
    return Utf8ToUtf16(xjs_db_GetPath(g_engine, fileId));
}

/* ============ 非管理员令牌打开 (每窗"继承管理员权限=关" 时使用) ============
 * 本程序恒以管理员运行, 直接 ShellExecute 的子进程继承管理员令牌。
 * 通道 1 (XjsOpenViaUserToken): 取 Explorer (中完整性) 进程令牌 → DuplicateTokenEx →
 *   CreateProcessWithTokenW 降权启动; 文件关联命令经 AssocQueryString 解析 (%1 → 目标),
 *   无关联时以 explorer.exe 落地。不依赖 IShellWindows (Win11 24H2+ 其类型库编组失效)。
 * 通道 2 (XjsOpenViaExplorerCom): 桌面 ShellView 后台对象 → IShellDispatch2::ShellExecute,
 *   老 Win10 的 IShellWindows 备用通道。 */
static const CLSID x_CLSID_ShellWindows     = { 0x9BA05972,0xF6A8,0x11CF,{0xA4,0x42,0x00,0xA0,0xC9,0x0A,0x8F,0x39} };
static const IID    x_IID_IShellWindows     = { 0x85BAB64C,0x7DE0,0x11D0,{0x8D,0x26,0x00,0xA0,0xC9,0x0A,0x8F,0x39} };
static const IID    x_IID_IServiceProvider  = { 0x6D5140C1,0x7436,0x11CE,{0x80,0x34,0x00,0xAA,0x00,0x60,0x09,0xFA} };
static const IID    x_SID_STopLevelBrowser  = { 0x4C96BE40,0x915C,0x11CF,{0x99,0xD3,0x00,0xAA,0x00,0x4A,0xE8,0x37} };
static const IID    x_IID_IShellBrowser     = { 0x000214E2,0x0000,0x0000,{0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46} };
static const IID    x_IID_IDispatch         = { 0x00020400,0x0000,0x0000,{0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46} };
static const IID    x_IID_IShellDispatch2   = { 0xA4C6892B,0x3C89,0x4FC2,{0xBD,0x21,0x5A,0x60,0xA8,0x2D,0x3E,0x84} };

static bool XjsOpenViaExplorerCom(const std::wstring& file) {
    IShellWindows* psw = NULL;
    HRESULT hrCci = CoCreateInstance(x_CLSID_ShellWindows, NULL, CLSCTX_LOCAL_SERVER,
                                     x_IID_IShellWindows, (void**)&psw);
    if (FAILED(hrCci)) {
        return false;
    }
    /* 桌面定位: VT_I4 + CSIDL_DESKTOP(0) (VT_EMPTY 在部分系统上 FindWindowSW 不命中) */
    VARIANT vLoc; VariantInit(&vLoc);
    vLoc.vt = VT_I4; vLoc.lVal = 0;
    VARIANT vEmpty; VariantInit(&vEmpty);
    long deskHwnd = 0;
    IDispatch* disp = NULL;
    HRESULT hrFind = psw->FindWindowSW(&vLoc, &vEmpty, SWC_DESKTOP, &deskHwnd, SWFO_NEEDDISPATCH, &disp);
    if (FAILED(hrFind)) {
        psw->Release();
        return false;
    }
    IServiceProvider* sp = NULL;
    if (FAILED(disp->QueryInterface(x_IID_IServiceProvider, (void**)&sp))) {
        disp->Release(); psw->Release();
        return false;
    }
    IShellBrowser* browser = NULL;
    if (FAILED(sp->QueryService(x_SID_STopLevelBrowser, x_IID_IShellBrowser, (void**)&browser))) {
        sp->Release(); disp->Release(); psw->Release();
        return false;
    }
    IShellView* view = NULL;
    if (FAILED(browser->QueryActiveShellView(&view))) {
        browser->Release(); sp->Release(); disp->Release(); psw->Release();
        return false;
    }
    IDispatch* vdisp = NULL;
    if (FAILED(view->GetItemObject(SVGIO_BACKGROUND, x_IID_IDispatch, (void**)&vdisp))) {
        view->Release(); browser->Release(); sp->Release(); disp->Release(); psw->Release();
        return false;
    }
    IShellDispatch2* disp2 = NULL;
    if (FAILED(vdisp->QueryInterface(x_IID_IShellDispatch2, (void**)&disp2))) {
        vdisp->Release(); view->Release(); browser->Release(); sp->Release(); disp->Release(); psw->Release();
        return false;
    }
    BSTR f = SysAllocString(file.c_str());
    VARIANT vOp, vArgs, vDir, vShow;
    VariantInit(&vOp); VariantInit(&vArgs); VariantInit(&vDir);
    vShow.vt = VT_I4; vShow.lVal = SW_SHOWNORMAL;
    HRESULT hrExec = disp2->ShellExecuteW(f, vOp, vArgs, vDir, vShow);
    SysFreeString(f);
    VariantClear(&vOp); VariantClear(&vArgs); VariantClear(&vDir); VariantClear(&vShow);
    disp2->Release(); vdisp->Release(); view->Release();
    browser->Release(); sp->Release(); disp->Release(); psw->Release();
    return SUCCEEDED(hrExec);
}

/* 通道 1: Explorer 进程令牌 + CreateProcessWithTokenW 降权启动 */
static bool XjsOpenViaUserToken(const std::wstring& path) {
    HWND shell = GetShellWindow();
    DWORD pid = 0;
    if (!shell || !GetWindowThreadProcessId(shell, &pid) || !pid) {
        return false;
    }
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) {
        return false;
    }
    HANDLE tok = NULL;
    if (!OpenProcessToken(proc, TOKEN_DUPLICATE | TOKEN_QUERY, &tok)) {
        CloseHandle(proc);
        return false;
    }
    HANDLE primary = NULL;
    BOOL dup = DuplicateTokenEx(tok, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenPrimary, &primary);
    CloseHandle(tok);
    CloseHandle(proc);
    if (!dup) {
        return false;
    }
    /* 启用 SeImpersonatePrivilege (CreateProcessWithTokenW 要求, 管理员组默认持有) */
    HANDLE ptok = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &ptok)) {
        TOKEN_PRIVILEGES tp = {};
        tp.PrivilegeCount = 1;
        if (LookupPrivilegeValueW(NULL, SE_IMPERSONATE_NAME, &tp.Privileges[0].Luid)) {
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(ptok, FALSE, &tp, 0, NULL, NULL);
        }
        CloseHandle(ptok);
    }
    /* 文件关联 open 命令展开 (照 shell 口径): %1/%L/%V → 目标路径。模板已带引号时整组
       `"%1"` 替换防双引号 —— exefile 模板是 "%1" %*, 曾把 %1 换成带引号路径得到
       ""path"" %*, CreateProcessWithTokenW 报 87 拒收 → 两级通道全灭 → 回落直开继承了
       管理员 (exe 打开必现, txt 因模板是裸 %1 幸免; 2026-09-16 实锤)。
       %* (附加参数) 展开为空 (单文件打开无附加参数); 模板无路径参数 = 视同无关联 */
    std::wstring cmd;
    wchar_t abuf[1024] = {};
    DWORD alen = 1024;
    bool substituted = false;
    if (SUCCEEDED(AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_COMMAND, path.c_str(), L"open", abuf, &alen))) {
        std::wstring tpl = abuf;
        std::wstring quoted = L"\"" + path + L"\"";
        for (size_t i = 0; i < tpl.size(); ) {
            if (tpl[i] == L'%' && i + 1 < tpl.size()) {
                wchar_t c = tpl[i + 1];
                if (c == L'1' || c == L'L' || c == L'V') {           /* 路径类参数 */
                    bool wrapped = !cmd.empty() && cmd.back() == L'"'
                                   && i + 2 < tpl.size() && tpl[i + 2] == L'"';
                    if (wrapped) { cmd.pop_back(); i += 3; }         /* 模板引号并入整组替换 */
                    else i += 2;
                    cmd += quoted;
                    substituted = true;
                    continue;
                }
                if (c == L'*') { i += 2; continue; }                 /* %* → 无附加参数 */
            }
            cmd += tpl[i++];
        }
    }
    bool viaAssoc = !cmd.empty() && substituted;
    if (!viaAssoc) cmd = L"explorer.exe \"" + path + L"\"";
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    std::wstring mutableCmd = cmd;
    BOOL ok = CreateProcessWithTokenW(primary, LOGON_WITH_PROFILE, NULL, &mutableCmd[0],
                                      CREATE_UNICODE_ENVIRONMENT, NULL, NULL, &si, &pi);
    CloseHandle(primary);
    if (ok) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    return ok != FALSE;
}

void XjsOpenFile(const std::wstring& path) {
    if (path.empty()) return;
    /* 打开前统一存在性检查 (入口一处, 所有打开动作都经此): 目标已不存在时弹询问框并中止 ——
       "打开后隐藏窗口"开启时窗口随即藏进托盘, 打开失败的任何提示用户都看不到,
       必须赶在打开/隐藏之前先拦 (移动盘/网络盘拔出同此) */
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        XjsShowAskDialog(g_hWnd, XjsT(L"对话框.无法打开"),
                         XjsFmt(XjsT(L"对话框.文件不存在"), path).c_str(),
                         ("[{\"text\":\"" + XjsTUtf8(L"确定") + "\",\"style\":\"primary\"}]").c_str());
        return;
    }
    /* 打开行为 = 每窗配置 (设置-打开): 异步线程防关联程序响应卡住主线程;
       继承管理员=开: 直接以本程序 (管理员) 令牌打开, 目标进程继承管理员;
       继承管理员=关: 借 Explorer (标准权限) 代为打开, 目标程序不带管理员权限;
       Explorer 通道失败时回落直接打开 (宁以管理员开, 不开不出来) */
    bool async = g_openAsync, elevated = g_openElevated, hideWin = g_openHideWindow;
    auto doOpen = [path, elevated]() {
        HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        bool launched = false;
        if (!elevated) {
            launched = XjsOpenViaUserToken(path);
            if (!launched) launched = XjsOpenViaExplorerCom(path);   /* 老 Win10 备用通道 */
        }
        if (!launched) {
            SHELLEXECUTEINFOW sei = { sizeof(sei) };
            sei.fMask = SEE_MASK_NOASYNC;   /* 工作线程随即退出, 必须等调用完成 (否则跨线程 marshalling 崩) */
            sei.lpFile = path.c_str();
            sei.nShow = SW_SHOWNORMAL;
            ShellExecuteExW(&sei);
        }
        if (SUCCEEDED(hr)) CoUninitialize();
    };
    if (async) std::thread(doOpen).detach();
    else doOpen();
    /* 打开后隐藏到托盘 (窗口保留: 双击 Ctrl/托盘可再次呼出, 结果对象不销毁) */
    if (hideWin && g_hWnd) ShowWindow(g_hWnd, SW_HIDE);
}

void XjsOpenFolderAndSelect(const std::wstring& path) {
    if (path.empty()) return;
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
    if (pidl) {
        SHOpenFolderAndSelectItems(pidl, 0, NULL, 0);
        ILFree(pidl);
    } else {
        size_t pos = path.rfind(L'\\');
        if (pos != std::wstring::npos)
            ShellExecuteW(NULL, L"explore", path.substr(0, pos).c_str(), NULL, NULL, SW_SHOWNORMAL);
    }
}

void XjsDeleteSelected() {
    std::vector<int> idxs = XjsSelIndices();
    if (idxs.empty()) return;
    /* 删除确认 = 通用询问框 (文案照源样式 deleteMask), 禁系统 MessageBoxW */
    if (XjsShowAskDialog(g_hWnd, XjsT(L"通用词.删除"),
                         XjsFmt(XjsT(L"对话框.删除确认"),
                                XjsNumText((long long)idxs.size())).c_str(),
                         ("[{\"text\":\"" + XjsTUtf8(L"删除") + "\",\"style\":\"danger\"},{\"text\":\"" +
                           XjsTUtf8(L"取消") + "\"}]").c_str()) != 0) return;
    std::wstring list;
    for (int idx : idxs) {
        std::wstring p = XjsItemPath(idx);
        if (!p.empty()) list += p + L'\0';
    }
    if (list.empty()) return;
    list += L'\0';
    SHFILEOPSTRUCTW op = {0};
    op.hwnd = g_hWnd;
    op.wFunc = FO_DELETE;
    op.pFrom = list.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
    SHFileOperationW(&op);   // 引擎 USN 同步自动移除索引
}

void XjsCopySelected(bool namesOnly) {
    std::wstring out;
    for (int idx : XjsSelIndices()) {
        std::wstring p = XjsItemPath(idx);
        if (namesOnly) {
            size_t pos = p.rfind(L'\\');
            if (pos != std::wstring::npos) p = p.substr(pos + 1);
        }
        if (!out.empty()) out += L"\r\n";
        out += p;
    }
    XjsCopyClipboard(out);
}

/* ==================== 文件剪贴板 (CF_HDROP + Preferred DropEffect, 源样式剪切/复制文件口径) ==================== */

/* 选中行 → 有效路径集 (剔除驱动器与空路径, 同源样式 dragOut "剔除驱动器") */
static std::vector<std::wstring> XjsSelPaths(bool excludeDrives) {
    std::vector<std::wstring> paths;
    for (int idx : XjsSelIndices()) {
        if (excludeDrives && g_engine && XjsIsDriveRow(idx)) continue;
        std::wstring p = XjsItemPath(idx);
        if (!p.empty()) paths.push_back(p);
    }
    return paths;
}

/* DROPFILES 头 + 双 \0 结尾宽字符路径列表 (剪贴板 CF_HDROP 与 OLE 拖出共用) */
static HGLOBAL XjsBuildHDropGlobal(const std::vector<std::wstring>& paths) {
    size_t chars = 1;
    for (auto& p : paths) chars += p.length() + 1;
    size_t bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);
    HGLOBAL hDrop = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hDrop) return NULL;
    DROPFILES* df = (DROPFILES*)GlobalLock(hDrop);
    if (!df) { GlobalFree(hDrop); return NULL; }
    df->pFiles = sizeof(DROPFILES);
    df->pt.x = df->pt.y = 0;
    df->fNC = FALSE;
    df->fWide = TRUE;
    wchar_t* w = (wchar_t*)((BYTE*)df + sizeof(DROPFILES));
    for (auto& p : paths) {
        size_t n = p.length() + 1;
        memcpy(w, p.c_str(), n * sizeof(wchar_t));
        w += n;
    }
    *w = L'\0';
    GlobalUnlock(hDrop);
    return hDrop;
}

static void XjsSetClipboardFiles(const std::vector<std::wstring>& paths, bool cut) {
    if (paths.empty() || !g_hWnd || !OpenClipboard(g_hWnd)) return;
    EmptyClipboard();
    HGLOBAL hDrop = XjsBuildHDropGlobal(paths);
    HGLOBAL hFx = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (hDrop && hFx) {
        DWORD* fx = (DWORD*)GlobalLock(hFx);
        if (!fx) { GlobalFree(hDrop); GlobalFree(hFx); CloseClipboard(); return; }
        *fx = cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
        GlobalUnlock(hFx);
        if (!SetClipboardData(CF_HDROP, hDrop)) GlobalFree(hDrop);   /* 未被接管才释放 */
        FORMATETC fe = { (CLIPFORMAT)RegisterClipboardFormatW(L"Preferred DropEffect"), NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM sm = {};
        sm.tymed = TYMED_HGLOBAL;
        sm.hGlobal = hFx;
        if (!SetClipboardData(fe.cfFormat, hFx)) GlobalFree(hFx);   /* 未被接管才释放 */
    } else {
        if (hDrop) GlobalFree(hDrop);
        if (hFx) GlobalFree(hFx);
    }
    CloseClipboard();
    g_clipSeqOurs = GetClipboardSequenceNumber();   /* 记下自己的写入口径 (剪贴板监听忽略) */
}

void XjsCopyFilesToClipboard(const std::vector<int>& idxs, bool cut) {
    (void)idxs;   /* 统一取当前选中集 (源样式 Ctrl+C/Ctrl+X 口径) */
    std::vector<std::wstring> paths;
    for (int idx : XjsSelIndices()) {
        if (XjsIsDriveRow(idx)) continue;
        std::wstring p = XjsItemPath(idx);
        if (!p.empty()) paths.push_back(p);
    }
    XjsSetClipboardFiles(paths, cut);
}

void XjsCutSelected() {
    std::vector<int> order = XjsSelIndices();
    std::vector<std::wstring> paths;
    for (int idx : order) {
        if (XjsIsDriveRow(idx)) continue;
        std::wstring p = XjsItemPath(idx);
        if (!p.empty()) paths.push_back(p);
    }
    if (paths.empty()) return;
    XjsSetClipboardFiles(paths, true);
    g_cutSet.clear();
    for (int idx : order) g_cutSet.insert(idx);
    XjsSearchWindow::Cur()->Invalidate();
}

/* ==================== OLE 拖出文件 (源样式 dragOut: 按住已选行拖动, 多选拖整个集合) ==================== */

class XjsDropSource : public IDropSource {
    LONG m_ref = 1;
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropSource) { *ppv = (IDropSource*)this; AddRef(); return S_OK; }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override { ULONG r = InterlockedDecrement(&m_ref); if (!r) delete this; return r; }
    STDMETHODIMP QueryContinueDrag(BOOL esc, DWORD keys) override {
        if (esc) return DRAGDROP_S_CANCEL;
        if (!(keys & (MK_LBUTTON | MK_RBUTTON))) return DRAGDROP_S_DROP;
        return S_OK;
    }
    STDMETHODIMP GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};

void XjsDragOutSelected() {
    std::vector<std::wstring> paths = XjsSelPaths(true);
    if (paths.empty() || !g_hWnd) return;
    /* 绝对 PIDL 数组 → Shell 项数组 → BHID_DataObject (现代标准路径: 数据对象按 shell 项渲染
       CF_HDROP, 跨文件夹多选成立)。SHCreateDataObject(NULL, n, 绝对PIDL, ...) 已实证不可用:
       pidlFolder=NULL 时绝对 PIDL 被按"桌面子项"语义解释, 拖出去是空壳 */
    std::vector<PIDLIST_ABSOLUTE> pidls;
    for (auto& p : paths) {
        PIDLIST_ABSOLUTE pl = ILCreateFromPathW(p.c_str());
        if (pl) pidls.push_back(pl);
    }
    IDataObject* dto = NULL;
    HRESULT hr = E_FAIL;
    IShellItemArray* sia = NULL;
    if (!pidls.empty())
        hr = SHCreateShellItemArrayFromIDLists((UINT)pidls.size(), (PCIDLIST_ABSOLUTE_ARRAY)pidls.data(), &sia);
    if (SUCCEEDED(hr) && sia) {
        hr = sia->BindToHandler(NULL, BHID_DataObject, IID_PPV_ARGS(&dto));
        sia->Release();
    }
    if (SUCCEEDED(hr) && dto) {
        /* 偏好效果=复制 (剪切走 Ctrl+X 剪贴板语义, 不在拖出里表达) */
        FORMATETC feFx = { (CLIPFORMAT)RegisterClipboardFormatW(L"Preferred DropEffect"), NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM smFx = {};
        HGLOBAL hFx = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
        if (hFx) {
            *(DWORD*)GlobalLock(hFx) = DROPEFFECT_COPY;
            GlobalUnlock(hFx);
            smFx.tymed = TYMED_HGLOBAL;
            smFx.hGlobal = hFx;
            /* fRelease=TRUE 仅在成功时移交所有权: 失败须自释 (否则每次拖出漏一个 HGLOBAL) */
            if (FAILED(dto->SetData(&feFx, &smFx, TRUE))) GlobalFree(hFx);
        }
        /* 拖动跟随图 (资源管理器同款): 拖影管理器注入首个文件的缩略图/图标位图 —
           32bpp ARGB DIB, 光标居中; 位图所有权归拖影管理器, 成功后勿释放 */
        IDragSourceHelper* helper = NULL;
        if (SUCCEEDED(CoCreateInstance(CLSID_DragDropHelper, NULL, CLSCTX_ALL, IID_PPV_ARGS(&helper)))) {
            IShellItemImageFactory* factory = NULL;
            if (SUCCEEDED(SHCreateItemFromParsingName(paths[0].c_str(), NULL, IID_PPV_ARGS(&factory)))) {
                SIZE sz = { 120, 120 };
                HBITMAP bmp = NULL;
                if (SUCCEEDED(factory->GetImage(sz, SIIGBF_RESIZETOFIT, &bmp)) && bmp) {
                    BITMAP bm = {};
                    GetObject(bmp, sizeof(bm), &bm);   /* GetImage 实际尺寸可能 ≠ 请求尺寸 */
                    SHDRAGIMAGE di = {};
                    di.sizeDragImage = { bm.bmWidth, bm.bmHeight };
                    di.ptOffset = { bm.bmWidth / 2, bm.bmHeight / 2 };
                    di.hbmpDragImage = bmp;
                    di.crColorKey = 0xFFFFFFFF;   /* 走位图 alpha 通道 */
                    /* 位图所有权归拖影管理器 (成功后勿释放); InitializeFromBitmap 失败不接管 → 自释 */
                    if (FAILED(helper->InitializeFromBitmap(&di, dto))) DeleteObject(bmp);
                }
                factory->Release();
            }
            helper->Release();
        }
        XjsDropSource* src = new XjsDropSource();
        DWORD effect = 0;
        hr = DoDragDrop(dto, src, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
        dto->Release();
        src->Release();   /* 初始引用归调用方释放 (DoDragDrop 只管它自己 AddRef 的那份) */
    }
    for (auto pl : pidls) ILFree(pl);
}

/* ==================== 行内重命名 (F2/右键; MoveFile, 引擎 USN 同步自动跟进) ====================
 * 编辑状态每窗一份 (XjsSearchWindow 字段, 宏重定向) */
#define s_renActive   (XjsSearchWindow::Cur()->renActive)
#define s_renIdx      (XjsSearchWindow::Cur()->renIdx)
#define s_renEd       (XjsSearchWindow::Cur()->renEd)
#define s_renBlink    (XjsSearchWindow::Cur()->renBlink)
#define s_renOldPath  (XjsSearchWindow::Cur()->renOldPath)
#define s_renOldName  (XjsSearchWindow::Cur()->renOldName)
#define s_renIsDir    (XjsSearchWindow::Cur()->renIsDir)

bool XjsRenameActive() { return s_renActive; }

/* 编辑框内容区 (外框内缩: 左 8px 右 4px) —— 绘制文本与鼠标命中必须用同一矩形,
   否则命中 x 与文本 x 差一个内边距, 选中位置就会偏移 (鼠标选字不准的根因) */
static XjsRect XjsRenameTextRect() {
    XjsRect r = XjsRenameEditRect();
    return XjsRectF(r.left + XSF(8), r.top, r.right - XSF(4), r.bottom);
}

/* Windows 文件名非法字符 + 结尾点/空格 (合法性校验, 源样式提交前校验同款) */
static bool XjsRenameValid(const std::wstring& n, std::wstring* why) {
    if (n.empty()) { *why = XjsT(L"错误.文件名为空"); return false; }
    if (n == L"." || n == L"..") { *why = XjsT(L"错误.文件名点"); return false; }
    static const wchar_t* bad = L"<>:\"/\\|?*";
    for (wchar_t c : n) {
        if (wcschr(bad, c)) { *why = XjsT(L"文件名不能包含字符  <> : \" / \\ | ? *"); return false; }
        if ((unsigned short)c < 0x20) { *why = XjsT(L"错误.文件名非法字符"); return false; }
    }
    if (n.back() == L'.' || n.back() == L' ') { *why = XjsT(L"错误.文件名结尾"); return false; }
    return true;
}

bool XjsRenameStart(int idx) {
    if (idx < 0 || !g_result || !g_engine || s_renActive) return false;
    if (XjsIsDriveRow(idx)) return false;   /* 驱动器不可重命名 (同源样式禁用) */
    int fileId = xjs_result_GetFileId(g_result, idx);
    if (fileId < 0) return false;
    s_renOldPath = XjsItemPath(idx);
    s_renOldName = Utf8ToUtf16(xjs_db_GetName(g_engine, fileId));
    if (s_renOldPath.empty() || s_renOldName.empty()) return false;
    s_renIsDir = (xjs_db_GetFileType(g_engine, fileId) == 255);
    s_renIdx = idx;
    s_renEd.SetText(s_renOldName, true);
    /* 主体名不含扩展名 (同源样式): 选中到扩展名点之前; 目录/无扩展名=全选 */
    if (!s_renIsDir) {
        size_t dot = s_renOldName.rfind(L'.');
        if (dot != std::wstring::npos && dot > 0) s_renEd.caret = (int)dot;
    }
    s_renActive = true;
    XjsSearchFocus(false);   /* 焦点交给行内编辑 (搜索框失焦熄光标) */
    /* 长文件名光标可能落在框外: 进入时先把编辑区滚到光标处 (与搜索框行为一致) */
    s_renEd.EnsureCaretVisible(XjsRenameTextRect(), g_tfRow);
    /* 光标闪烁驱动器: 绑定主窗 + 局部重绘; 主窗 WM_ACTIVATE 广播驱动 激活/失活 */
    if (g_hWnd) {
        HWND hw = g_hWnd;
        s_renBlink.Attach(hw, [hw]() {
            XjsRect r = XjsRenameEditRect();
            RECT rr = { (LONG)(r.left - XSF(2)) - 2, (LONG)r.top - 2, (LONG)r.right + 2, (LONG)r.bottom + 2 };
            InvalidateRect(hw, &rr, FALSE);
        });
        s_renBlink.Focus(true);
    }
    XjsSearchWindow::Cur()->Invalidate();
    return true;
}

void XjsRenameFinish(bool commit) {
    if (!s_renActive) return;
    std::wstring nn = s_renEd.text;
    if (commit && nn != s_renOldName) {
        std::wstring why;
        if (!XjsRenameValid(nn, &why)) {
            g_errText = why;
            XjsSearchWindow::Cur()->Invalidate();
            return;   /* 校验失败留在编辑态 (源样式同口径), 下次按键清错误 */
        }
        std::wstring newPath = s_renOldPath.substr(0, s_renOldPath.length() - s_renOldName.length()) + nn;
        if (MoveFileW(s_renOldPath.c_str(), newPath.c_str())) {
            g_statusText = XjsFmt(XjsT(L"状态栏.已重命名"), nn);
            g_errText.clear();
            XjsClearRowCache();   /* 行缓存持旧名, 清掉重取 (引擎 USN 同步异步跟进) */
        } else {
            g_errText = XjsT(L"状态栏.重命名失败");
            XjsSearchWindow::Cur()->Invalidate();
            return;
        }
    }
    s_renActive = false;
    s_renIdx = -1;
    s_renEd.dragging = false;
    s_renBlink.Detach();   /* 退登记 + 停表 (编辑态结束, 不再闪) */
    /* 退出编辑态一律整窗重画: 取消时(点搜索框/标题栏等)也必须抹掉行内编辑框,
       否则旧编辑框会残留成幽灵 (原先只在 ok 时重画) */
    XjsSearchWindow::Cur()->Invalidate();
}

bool XjsRenameKey(WPARAM vk) {
    if (!s_renActive) return false;
    if (vk == VK_ESCAPE) { XjsRenameFinish(false); return true; }
    if (vk == VK_RETURN) { XjsRenameFinish(true); return true; }
    if (s_renEd.Key(vk)) {
        s_renEd.EnsureCaretVisible(XjsRenameTextRect(), g_tfRow);
        s_renEd.UpdateImeAnchor(g_hWnd, XjsRenameTextRect(), g_tfRow);
        s_renBlink.Reset();
        XjsSearchWindow::Cur()->Invalidate();
        return true;
    }
    return true;   /* 编辑态吞掉其余按键 (方向之外的不漏给列表) */
}

bool XjsRenameChar(wchar_t ch) {
    if (!s_renActive) return false;
    if (s_renEd.Char(ch)) {
        g_errText.clear();
        s_renEd.EnsureCaretVisible(XjsRenameTextRect(), g_tfRow);
        s_renEd.UpdateImeAnchor(g_hWnd, XjsRenameTextRect(), g_tfRow);
        s_renBlink.Reset();
        XjsSearchWindow::Cur()->Invalidate();
    }
    return true;
}

bool XjsRenameImeResult(HWND hwnd, LPARAM lParam) {
    if (!s_renActive) return false;
    if (!s_renEd.ImeResult(hwnd, lParam)) return false;
    g_errText.clear();
    s_renEd.EnsureCaretVisible(XjsRenameTextRect(), g_tfRow);
    s_renEd.UpdateImeAnchor(hwnd, XjsRenameTextRect(), g_tfRow);
    s_renBlink.Reset();
    XjsSearchWindow::Cur()->Invalidate();
    return true;
}

void XjsRenameMouseDown(POINT pt) {
    if (!s_renActive) return;
    s_renEd.MouseDown(pt, XjsRenameTextRect(), g_tfRow);   /* 点定位 + 起拖 (选区扩展) */
    s_renEd.UpdateImeAnchor(g_hWnd, XjsRenameTextRect(), g_tfRow);
    s_renBlink.Reset();
    XjsSearchWindow::Cur()->Invalidate();
}

void XjsRenameMouseMove(POINT pt) {
    if (!s_renActive || !s_renEd.dragging) return;
    int before = s_renEd.caret;
    s_renEd.MouseMove(pt, XjsRenameTextRect(), g_tfRow);
    if (s_renEd.caret != before) XjsSearchWindow::Cur()->Invalidate();
}

void XjsRenameMouseUp() {
    if (!s_renActive) return;
    s_renEd.MouseUp();
}

bool XjsRenameDragging() { return s_renActive && s_renEd.dragging; }

/* 双击编辑框 = 选整词 (不打开文件): 返回 true 表示已消费该双击 */
bool XjsRenameDoubleClick(POINT pt) {
    if (!s_renActive) return false;
    if (!XjsPtIn(XjsRenameEditRect(), pt)) return false;
    s_renEd.MouseDoubleClick(pt, XjsRenameTextRect(), g_tfRow);
    XjsSearchWindow::Cur()->Invalidate();
    return true;
}

/* 重命名编辑框右键菜单命令 (0剪切 1复制 2粘贴 3全选 4删除): 编辑操作走共享 XjsEditMenuApplyCmd */
void XjsRenameMenuCmd(int cmd) {
    if (!s_renActive) return;
    XjsEditMenuApplyCmd(s_renEd, cmd);
    s_renEd.EnsureCaretVisible(XjsRenameTextRect(), g_tfRow);
    s_renEd.UpdateImeAnchor(g_hWnd, XjsRenameTextRect(), g_tfRow);
    s_renBlink.Reset();
    XjsSearchWindow::Cur()->Invalidate();
}

/* 重命名编辑框右键菜单 (源样式重命名输入框同款基础编辑项)。
   菜单表走共享 XjsEditMenuAppendItems (与搜索框/路由层字段同一份), ID 段 = IDM_CTX_BASE+40..44 */
bool XjsShowRenameBoxMenu(POINT screenPt) {
    if (!s_renActive) return false;
    std::vector<XjsPopupItem> items;
    XjsEditMenuAppendItems(s_renEd, items, IDM_CTX_BASE + 40);
    XjsShowPopupMenu(g_hWnd, screenPt, items, XSF(150));
    return true;
}

int XjsRenameTargetIdx() { return s_renActive ? s_renIdx : -1; }

/* 行内重命名编辑框绘制 (XjsListRender 末尾调用; 裁剪在列表区内) */
void XjsRenameRender() {
    if (!s_renActive || !g_rt) return;
    XjsRect r = XjsRenameEditRect();
    if (r.right <= r.left || r.bottom <= r.top) return;
    if (r.bottom <= g_layout.list.top || r.top >= g_layout.list.bottom) return;   /* 滚出视口不画 */
    g_rt->PushAxisAlignedClip(g_layout.list, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    XjsRoundedRect rr = XjsRoundedRectF(r, XSF(5), XSF(5));
    g_rt->FillRoundedRectangle(rr, g_br[XTH_PANEL2]);
    g_rt->DrawRoundedRectangle(rr, g_br[XTH_ACCENT], 1.2f);
    XjsRect inner = XjsRenameTextRect();
    XjsBrush* selBr = XjsTempBrush(XjsColorF(0.22f, 0.48f, 0.95f, 0.80f));
    /* 占位符("输入新名称")与光标由组件统一画: 空文本也显示光标 (与搜索框/别名框一致)。
       光标用文字色: 选中底色是蓝的, 用强调色/白色光标都会同色看不见; 文字色天然与其底色对比 */
    s_renEd.Render(g_rt, inner, g_tfRow, g_br[XTH_TEXT], selBr, g_br[XTH_TEXT],
        XjsT(L"对话框.重命名占位符"), g_br[XTH_TEXT_FAINT], s_renBlink.On());
    g_rt->PopAxisAlignedClip();
}

/* ==================== 别名 (xjs_db_SetAlias; 空串=删除) ==================== */

void XjsApplyAlias(const std::vector<int>& idxs, const std::wstring& alias) {
    if (!g_engine || idxs.empty()) return;
    std::string a;
    if (!alias.empty()) a = Utf16ToUtf8(alias.c_str());
    for (int idx : idxs) {
        int fileId = g_result ? xjs_result_GetFileId(g_result, idx) : -1;
        if (fileId >= 0)
            xjs_db_SetAlias(g_engine, fileId, alias.empty() ? NULL : a.c_str());
    }
    XjsClearRowCache();
    g_statusText = alias.empty() ? XjsT(L"状态栏.已清除别名") : XjsT(L"状态栏.已设置别名");
    XjsSearchWindow::Cur()->Invalidate();
}

void XjsShowAliasDialog() {
    std::vector<int> order = XjsSelIndices();
    if (order.empty() || !g_result || !g_engine) return;
    if (!xjs_db_IsFieldEnabled(g_engine, "别名")) {
        g_errText = XjsT(L"状态栏.别名字段未开启");
        XjsSearchWindow::Cur()->Invalidate();
        return;
    }
    std::wstring init;
    if (order.size() == 1) {
        int fileId = xjs_result_GetFileId(g_result, order[0]);
        if (fileId >= 0) init = Utf8ToUtf16(xjs_db_GetAlias(g_engine, fileId));
    }
    std::wstring desc = order.size() > 1
        ? XjsFmt(XjsT(L"对话框.批量别名说明"), XjsNumText((long long)order.size()))
        : (XjsIsDriveRow(order[0]) ? XjsT(L"对话框.驱动器别名说明") : XjsT(L"对话框.文件别名说明"));
    XjsShowInputDialog(g_hWnd, XjsT(L"右键菜单.设置别名"), desc.c_str(), init, IDM_CTX_BASE + 30);
}

/* ==================== 文本预览分行 ==================== */

std::vector<std::wstring> XjsSplitLines(const std::wstring& text) {
    std::vector<std::wstring> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find(L'\n', start);
        if (nl == std::wstring::npos) { lines.push_back(text.substr(start)); break; }
        std::wstring line = text.substr(start, nl - start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        lines.push_back(line);
        start = nl + 1;
        if (lines.size() >= 20000) { lines.push_back(XjsT(L"对话框.截断提示")); break; }
    }
    return lines;
}

/* ==================== 开机自启动 (计划任务, 源样式 schtasks /xml 同构) ====================
 * 登录触发 + RunLevel=HighestAvailable + 不限运行时长 (ExecutionTimeLimit=PT0S, 取消
 * "运行时间限制/强行停止") + IgnoreNew; 任务附加 --autostart 参数, 程序据此以隐藏方式启动
 * (仅托盘, 经托盘/双击 Ctrl 唤起)。创建/删除任务需管理员权限 (本程序恒管理员)。 */
#define XJS_AUTOSTART_TASK L"蜗牛快搜_autostart"

static int XjsRunSchtasks(const std::wstring& args) {
    /* 注意: 字面量先转 std::wstring 再拼接, 避免 const wchar_t* + const wchar_t* 双指针相加 */
    std::wstring cmd = std::wstring(L"schtasks.exe ") + args;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(NULL, &cmd[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, 15000);   /* 创建任务一般秒回, 15 秒兜底 */
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

BOOL XjsIsAutoStartEnabled() {
    return XjsRunSchtasks(std::wstring(L"/query /tn \"") + XJS_AUTOSTART_TASK + L"\"") == 0;
}

BOOL XjsSetAutoStart(BOOL enable) {
    if (!enable) {
        /* 删除不存在任务返回非0属正常, 不提示 (源样式口径) */
        XjsRunSchtasks(std::wstring(L"/delete /tn \"") + XJS_AUTOSTART_TASK + L"\" /f");
        return TRUE;
    }
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    auto esc = [](const std::wstring& s) {   /* XML 转义 (路径可能含 & < > 等) */
        std::wstring o;
        for (wchar_t c : s) {
            if (c == L'&') o += L"&amp;";
            else if (c == L'<') o += L"&lt;";
            else if (c == L'>') o += L"&gt;";
            else if (c == L'"') o += L"&quot;";
            else o += c;
        }
        return o;
    };
    std::wstring xml;
    xml += L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n";
    xml += L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n";
    xml += L"  <RegistrationInfo>\r\n";
    xml += L"    <Description>" + esc(L"蜗牛快搜开机自启动 (登录时触发)") + L"</Description>\r\n";
    xml += L"  </RegistrationInfo>\r\n";
    xml += L"  <Triggers>\r\n    <LogonTrigger>\r\n      <Enabled>true</Enabled>\r\n    </LogonTrigger>\r\n  </Triggers>\r\n";
    xml += L"  <Principals>\r\n    <Principal id=\"Author\">\r\n      <LogonType>InteractiveToken</LogonType>\r\n";
    xml += L"      <RunLevel>HighestAvailable</RunLevel>\r\n";
    xml += L"    </Principal>\r\n  </Principals>\r\n";
    xml += L"  <Settings>\r\n";
    xml += L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n";
    xml += L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n";
    xml += L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n";
    xml += L"    <AllowHardTerminate>true</AllowHardTerminate>\r\n";
    xml += L"    <StartWhenAvailable>false</StartWhenAvailable>\r\n";
    xml += L"    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>\r\n";
    xml += L"    <IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>\r\n";
    xml += L"    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n";
    xml += L"    <Enabled>true</Enabled>\r\n";
    xml += L"    <Hidden>false</Hidden>\r\n";
    xml += L"    <RunOnlyIfIdle>false</RunOnlyIfIdle>\r\n";
    xml += L"    <WakeToRun>false</WakeToRun>\r\n";
    xml += L"    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\r\n";   /* 不限运行时间 */
    xml += L"    <Priority>7</Priority>\r\n";
    xml += L"  </Settings>\r\n";
    xml += L"  <Actions Context=\"Author\">\r\n    <Exec>\r\n";
    xml += L"      <Command>" + esc(exePath) + L"</Command>\r\n";
    xml += L"      <Arguments>--autostart</Arguments>\r\n";
    xml += L"    </Exec>\r\n  </Actions>\r\n";
    xml += L"</Task>\r\n";
    /* 临时 XML (UTF-16 LE + BOM, schtasks /xml 要求) */
    wchar_t tempDir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempDir);
    std::wstring xmlPath = std::wstring(tempDir) + L"xjs_task.xml";
    HANDLE hf = CreateFileW(xmlPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return FALSE;
    BYTE bom[2] = { 0xFF, 0xFE };
    DWORD wr = 0;
    WriteFile(hf, bom, 2, &wr, NULL);
    WriteFile(hf, xml.data(), (DWORD)(xml.size() * sizeof(wchar_t)), &wr, NULL);
    CloseHandle(hf);
    return XjsRunSchtasks(std::wstring(L"/create /tn \"") + XJS_AUTOSTART_TASK + L"\" /xml \"" + xmlPath + L"\" /f") == 0;
}

/* 文件夹选择 (IFileDialog FOS_PICKFOLDERS): 返回 true 时 folder=所选目录 */
bool XjsPickFolder(HWND owner, std::wstring& folder) {
    bool ok = false;
    IFileDialog* fd = NULL;
    /* coclass FileDialog 的 GUID 直写 (CLSID_FileDialog 符号随 SDK 版本时有时无) */
    static const CLSID kFileDialogClsid = { 0xDC1C5A9C, 0xE88A, 0x4DDE, { 0xA5, 0xA1, 0x60, 0xF8, 0x2A, 0x20, 0xAE, 0xF7 } };
    if (SUCCEEDED(CoCreateInstance(kFileDialogClsid, NULL, CLSCTX_INPROC_SERVER,
                                   __uuidof(IFileDialog), (void**)&fd)) && fd) {
        DWORD opts = 0;
        if (SUCCEEDED(fd->GetOptions(&opts)))
            fd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        if (SUCCEEDED(fd->Show(owner))) {
            IShellItem* item = NULL;
            if (SUCCEEDED(fd->GetResult(&item)) && item) {
                PWSTR path = NULL;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    folder = path;
                    ok = true;
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        fd->Release();
    }
    return ok;
}

/* XjsJsonStringArray 已随配置门面收进 xjs_engine.cpp (JSON 解析不出引擎文件) */

/* ==================== 双击 Ctrl 全局唤起 (设置-通用 可关, 默认关) ====================
 * 开启时才注册 键盘+鼠标两个 LL 钩子 (独立线程, 自带消息循环, 回调在该线程串行被调):
 * 任意前台窗口下都能触发, 且不占 UI 线程输入路径。全局钩子易被杀毒软件误报,
 * 故默认关闭且关闭期一个钩子都不挂 (设置里关掉 = 立即摘钩子)。
 * 判双击口径 = 两次"干净"的 Ctrl 按下 (间隔<1000ms): Ctrl 按住期间或两次按下之间
 * 混入过 任何其它按键 (Ctrl+V/Alt+Tab 等) 或 鼠标点击 (Ctrl+点击/拖选), 即视为组合
 * 用途, 本次抬起不武装、下次按下不触发 —— 否则连按两次 Ctrl+V、两次 Ctrl+点击
 * 都会被误判成双击 Ctrl (2026-09-15 实锤)。 */
static HANDLE s_dcThread = NULL;
static HHOOK s_dcKeyHook = NULL;
static HHOOK s_dcMouseHook = NULL;
static bool s_dcCtrlDown = false;       /* Ctrl 物理按住中 (按住不放的自动重复 keydown 不算第二次按下) */
static bool s_dcDirty = false;          /* 本次按住期间/距上次干净抬起后 混入过其它键或鼠标点击 */
static bool s_dcFired = false;          /* 本轮已触发: 触发的那次按下被消耗, 其抬起不再武装 —— 否则双击后补按一下 Ctrl 会被当成新一轮双击 (2026-09-15 实锤) */
static ULONGLONG s_dcLastCleanUp = 0;   /* 上次"干净"Ctrl 抬起时刻 (0=未武装) */

static void XjsDcPollute() {            /* 组合信号: 本次按下不干净; 若正处于两次按下之间则撤销武装 */
    s_dcDirty = true;
    if (!s_dcCtrlDown) s_dcLastCleanUp = 0;
}

static LRESULT CALLBACK XjsDcKeyProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_doubleCtrl.load()) {
        KBDLLHOOKSTRUCT* k = (KBDLLHOOKSTRUCT*)lp;
        bool up = (wp == WM_KEYUP || wp == WM_SYSKEYUP);
        if (k->vkCode == VK_LCONTROL || k->vkCode == VK_RCONTROL) {
            if (up) {
                if (s_dcFired) s_dcFired = false;                     /* 触发键的抬起: 消耗掉, 不武装 */
                else if (!s_dcDirty) s_dcLastCleanUp = GetTickCount64();   /* 干净抬起 → 武装 */
                s_dcCtrlDown = false;
            } else if (!s_dcCtrlDown) {          /* 物理首次按下 */
                s_dcCtrlDown = true;
                s_dcDirty = false;               /* 新的一次按住: 先视为干净, 混入组合再污染 */
                ULONGLONG now = GetTickCount64();
                if (s_dcLastCleanUp != 0 && now - s_dcLastCleanUp < 1000) {
                    s_dcLastCleanUp = 0;         /* 触发即清: 新一轮双击须重新走"干净抬起→按下" */
                    s_dcFired = true;
                    HWND main = XjsSearchWindow::MainHwnd();   /* 跨线程读: 最坏过期句柄, PostMessage 无害失败 */
                    if (main) PostMessageW(main, WM_DOUBLE_CTRL, 0, 0);
                }
            }
        } else {
            XjsDcPollute();
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static LRESULT CALLBACK XjsDcMouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_doubleCtrl.load()) {
        switch (wp) {   /* 只看按键类事件 (移动不算, 否则鼠标轻抖即漏判); 键盘钩子看不见的 Ctrl+点击 全靠它 */
            case WM_LBUTTONDOWN: case WM_LBUTTONUP:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP:
            case WM_MBUTTONDOWN: case WM_MBUTTONUP:
            case WM_XBUTTONDOWN: case WM_XBUTTONUP:
                XjsDcPollute();
                break;
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static DWORD WINAPI XjsDcThreadProc(LPVOID) {
    s_dcKeyHook = SetWindowsHookExW(WH_KEYBOARD_LL, XjsDcKeyProc, GetModuleHandleW(NULL), 0);
    s_dcMouseHook = SetWindowsHookExW(WH_MOUSE_LL, XjsDcMouseProc, GetModuleHandleW(NULL), 0);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { /* 钩子线程只跑消息循环, WM_QUIT 退出 */ }
    if (s_dcKeyHook) { UnhookWindowsHookEx(s_dcKeyHook); s_dcKeyHook = NULL; }
    if (s_dcMouseHook) { UnhookWindowsHookEx(s_dcMouseHook); s_dcMouseHook = NULL; }
    return 0;
}

/* 双击 Ctrl 目标解析 (UI 线程): 空/名称不存在或被删 = -1 (禁用); "默认窗口" = 槽 0; 其余按档案名匹配 */
int XjsDoubleCtrlResolveSlot() {
    if (g_doubleCtrlTarget.empty()) return -1;
    if (g_doubleCtrlTarget == XJS_MAIN_WIN_NAME) return 0;
    for (int i = 1; i < XjsUiProfileCount(); i++) {
        XjsUiProfile* p = XjsUiProfileAt(i);
        if (p && p->name == g_doubleCtrlTarget) return i;
    }
    return -1;
}

/* 目标有效性变化后的实时启停 (下拉切换/窗口改名失效/启动载入 后调用; 禁用 = 立即卸载钩子) */
void XjsDoubleCtrlApply() {
    g_doubleCtrl = (XjsDoubleCtrlResolveSlot() >= 0);
    if (g_doubleCtrl.load()) XjsDoubleCtrlStart();
    else XjsDoubleCtrlStop();
}

void XjsDoubleCtrlStart() {
    if (s_dcThread || !g_doubleCtrl.load()) return;   /* 未开启 = 不注册任何钩子 (防杀毒软件误报) */
    s_dcThread = CreateThread(NULL, 0, XjsDcThreadProc, NULL, 0, NULL);
}

void XjsDoubleCtrlStop() {
    if (s_dcThread) {
        PostThreadMessageW(GetThreadId(s_dcThread), WM_QUIT, 0, 0);
        WaitForSingleObject(s_dcThread, 3000);
        CloseHandle(s_dcThread);
        s_dcThread = NULL;
    }
}

BOOL XjsIsRunningAsAdmin() {
    BOOL isAdmin = FALSE;
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION elev; DWORD cb = sizeof(elev);
        if (GetTokenInformation(hToken, TokenElevation, &elev, sizeof(elev), &cb))
            isAdmin = elev.TokenIsElevated;
        CloseHandle(hToken);
    }
    return isAdmin;
}

/* 编译时间 (设置-关于 显示): 读自身 PE 头的 TimeDateStamp = 链接时刻 (Unix 秒, UTC) 再转本地时间。
   读 PE 头而不是 __DATE__/__TIME__ 宏: 宏记的是那个 .cpp 被编译的时刻, 增量重编时会停在旧日期,
   PE 头恒等于手上这个 exe 的构建时刻 (含夏令时折算走 FileTimeToLocalFileTime) */
std::wstring XjsBuildTimeText() {
    const BYTE* base = (const BYTE*)GetModuleHandleW(NULL);
    if (!base) return XjsT(L"设置.关于.未知");
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return XjsT(L"设置.关于.未知");
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return XjsT(L"设置.关于.未知");
    /* Unix 秒 → FILETIME (起点 1601-01-01, 相差 11644473600 秒) */
    ULONGLONG t = (ULONGLONG)nt->FileHeader.TimeDateStamp * 10000000ULL + 116444736000000000ULL;
    FILETIME ft = { (DWORD)(t & 0xFFFFFFFFu), (DWORD)(t >> 32) }, lft = {};
    SYSTEMTIME st = {};
    if (!FileTimeToLocalFileTime(&ft, &lft) || !FileTimeToSystemTime(&lft, &st)) return XjsT(L"设置.关于.未知");
    wchar_t buf[40];
    _snwprintf(buf, 40, L"%04u-%02u-%02u %02u:%02u:%02u",
               (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
               (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond);
    return buf;
}

/* 系统版本文本 (设置-关于): 必须走 ntdll 的 RtlGetVersion —— 没有兼容性清单时 GetVersionExW
   会谎报 6.2 (Win8) 而不管实际系统 */
std::wstring XjsOsVersionText() {
    typedef LONG (WINAPI* RtlGetVersionFn)(OSVERSIONINFOW*);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionFn fn = nt ? (RtlGetVersionFn)GetProcAddress(nt, "RtlGetVersion") : NULL;
    OSVERSIONINFOW vi = {};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (!fn || fn(&vi) != 0) return XjsT(L"设置.关于.未知");
    /* 版本号 → 名称只做保守映射 (Win10 与 Win11 同为 10.x, 以 Build 22000 分界;
       未列举的系统只显示版本号, 不猜名字) */
    const wchar_t* name = NULL;
    if (vi.dwMajorVersion == 10 && vi.dwMinorVersion == 0)
        name = (vi.dwBuildNumber >= 22000) ? L"Windows 11" : L"Windows 10";
    else if (vi.dwMajorVersion == 6 && vi.dwMinorVersion == 3) name = L"Windows 8.1";
    else if (vi.dwMajorVersion == 6 && vi.dwMinorVersion == 2) name = L"Windows 8";
    else if (vi.dwMajorVersion == 6 && vi.dwMinorVersion == 1) name = L"Windows 7";
    wchar_t buf[64];
    if (name) _snwprintf(buf, 64, L"%s (Build %u)", name, (unsigned)vi.dwBuildNumber);
    else      _snwprintf(buf, 64, L"Windows %u.%u (Build %u)",
                         (unsigned)vi.dwMajorVersion, (unsigned)vi.dwMinorVersion, (unsigned)vi.dwBuildNumber);
    return buf;
}

/* 应用图标 = exe 资源 32512 (main.rc 的 snailsearch.ico), 搜索窗/设置窗共用这一处来源。
   Windows 各处取图标尺寸不同: 任务栏按钮/Alt+Tab 用 ICON_BIG (SM_CXICON),
   任务栏悬停预览的标题行 / 窗口缩略图列表用 ICON_SMALL (SM_CXSMICON) —— 窗口一个图标都不设时,
   任务栏按钮会回退 exe 图标(看起来"有图标"), 而 ICON_SMALL 取不到就画空白 (2026-09-18 用户反馈)。
   .ico 只含 32x32 一帧, LoadImage 按请求尺寸缩放; 句柄进程内缓存复用, 调用方不得 DestroyIcon。 */
HICON XjsAppIconBig() {
    static HICON ico = NULL;
    if (!ico) ico = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(32512), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    if (!ico) ico = LoadIconW(NULL, IDI_APPLICATION);   /* 资源缺失兜底 (仍好过无图标) */
    return ico;
}

HICON XjsAppIconSmall() {
    static HICON ico = NULL;
    if (!ico) ico = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(32512), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!ico) ico = LoadIconW(NULL, IDI_APPLICATION);
    return ico;
}

void XjsElevateAndRestart() {
    wchar_t szPath[MAX_PATH];
    if (GetModuleFileNameW(NULL, szPath, MAX_PATH)) {
        /* ShellExecuteEx 要求调用线程先初始化 COM; 此处是启动最早期, 主流程的
           OleInitialize 还没执行到, 而 runas 链路 (AppInfo/consent UAC) 走 COM/RPC */
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        SHELLEXECUTEINFOW sei = {0};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOASYNC;   /* 同步等启动请求完成: 无此标志时请求异步投递,
                                           后面的 ExitProcess 会与提权链路赛跑, 偶发
                                           "无 UAC 弹窗、新进程也没起来" */
        sei.lpVerb = L"runas";
        sei.lpFile = szPath;
        sei.nShow = SW_NORMAL;
        /* 透传命令行参数 (GetCommandLineW 首段=本程序路径, lpFile 已给, 只拼其后参数):
           不透传则 --autostart 等开关在自提权重启后丢失, 隐藏启动变成弹窗启动 */
        {
            LPCWSTR args = GetCommandLineW();
            if (*args == L'"') {                       /* 跳过带引号的 argv[0] */
                LPCWSTR end = wcschr(args + 1, L'"');
                args = end ? end + 1 : args + wcslen(args);
            } else {
                LPCWSTR sp = wcschr(args, L' ');
                args = sp ? sp : args + wcslen(args);
            }
            while (*args == L' ') args++;
            if (*args) sei.lpParameters = args;
        }
        if (!ShellExecuteExW(&sei)) {
            if (GetLastError() == ERROR_CANCELLED)
                MessageBoxW(NULL, L"已在授权提示中选择否，程序未启动。", L"权限不足", MB_OK | MB_ICONINFORMATION);
            else
                MessageBoxW(NULL, L"此程序需要管理员权限才能扫描所有文件。", L"权限不足", MB_OK | MB_ICONERROR);
        }
        ExitProcess(0);
    }
}
