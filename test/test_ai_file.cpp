/*
 * test_ai_file.cpp — read_file/AiTextToUtf8 纯逻辑单元验证
 * 链 ai_core.obj + ai_file.obj (插件示例\ai-assistant\build.bat 产物), 不起引擎不碰 UI;
 * 样本由 test_fixtures.js 造在 %TEMP%\aft。通过=输出 0 失败, 非零=失败数。
 * 构建 (仓库根执行, 先跑过插件 build.bat 让 .obj 在位; /I 解决 include 定位; 库集同插件 build.bat):
 *   cl /nologo /EHsc /std:c++20 /utf-8 /MT /DUNICODE /D_UNICODE /Fotest\ /I插件示例\ai-assistant test\test_ai_file.cpp 插件示例\ai-assistant\ai_core.obj 插件示例\ai-assistant\ai_file.obj ^
 *      /Fe:test\test_ai_file.exe /link winhttp.lib user32.lib gdi32.lib shell32.lib advapi32.lib ole32.lib oleaut32.lib uuid.lib gdiplus.lib windowscodecs.lib propsys.lib runtimeobject.lib xunjieso.lib
 *   (注意 /Fotest\ 不带引号 — /Fo"test\" 的 \" 会被解析成转义引号吞掉后续源文件, 报 D8003)
 */
#include "ai_assistant.h"
#include <stdio.h>

/* ai_plugin.cpp / ai_agent.cpp 未参与链接, 补最小外部符号 (测试只用纯函数) */
const XjsPluginHost* g_host = NULL;
XjsPluginCtx* g_ctx = NULL;
unsigned g_uiThread = 0;
HWND g_msgwnd = NULL;

static int fails = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); fails++; } \
} while (0)

static void DumpCase(const char* name, const std::wstring& err, const std::wstring& res8) {
    printf("  [dump %s] err=%s\n", name, U8(err).c_str());
    std::wstring head = res8.substr(0, res8.find(L"content") + 160);
    printf("  [dump %s] res8head=%s\n", name, U8(head).c_str());
}

static std::wstring RunRead(const wchar_t* file, std::wstring* err) {
    AiToolStep st;
    Jv v;
    v.t = 5;
    Jv pv;
    pv.t = 3;
    pv.str = std::wstring(L"%TEMP%\\aft\\") + file;
    /* 展开 %TEMP% */
    wchar_t tmp[1024];
    DWORD n = GetEnvironmentVariableW(L"TEMP", tmp, 1024);
    pv.str = std::wstring(tmp) + L"\\aft\\" + file;
    v.obj.push_back({ L"path", std::move(pv) });
    *err = ReadFileToolExec(v, &st);
    return W8(st.res8.c_str());
}
static Jv Res8(const std::wstring& json) {
    return JsonParseW(json);
}

int main() {
    /* ---- 编码识别 (直调) ---- */
    {
        std::wstring enc;
        std::string out = AiTextToUtf8(std::string("\xD6\xD0\xCE\xC4\xB2\xE2\xCA\xD4", 8), &enc);
        std::wstring w = W8(out.c_str());
        CHECK(w == L"中文测试", "gbk-bytes-to-utf8");
        CHECK(enc.find(L"ANSI") != std::wstring::npos, "gbk-detect-ansi");
        out = AiTextToUtf8(std::string("\xEF\xBB\xBFhi"), &enc);
        CHECK(W8(out.c_str()) == L"hi" && enc == L"UTF-8", "utf8-bom-strip");
        std::string u16 = "\xFF\xFE";
        {
            std::wstring src = L"宽A";
            u16.append((const char*)src.c_str(), src.size() * 2);
        }
        out = AiTextToUtf8(u16, &enc);
        CHECK(W8(out.c_str()) == L"宽A" && enc == L"UTF-16LE", "utf16le-detect");
        out = AiTextToUtf8(std::string("plain ascii"), &enc);
        CHECK(W8(out.c_str()) == L"plain ascii" && enc == L"UTF-8", "ascii-as-utf8");
        CHECK(AiB64Enc((const unsigned char*)"foo", 3) == "Zm9v", "b64-basic");
    }

    /* ---- 文本文件 ---- */
    {
        std::wstring err, r = RunRead(L"gbk.txt", &err);
        CHECK(err.empty(), "gbk-file-no-error");
        Jv v = Res8(r);
        CHECK(v.S(L"content").find(L"中文测试行一") != std::wstring::npos &&
              v.S(L"content").find(L"第二行") != std::wstring::npos, "gbk-file-content");
        CHECK(v.S(L"格式或编码").find(L"ANSI") != std::wstring::npos, "gbk-file-enc");
    }
    {
        std::wstring err, r = RunRead(L"u16.txt", &err);
        Jv v = Res8(r);
        CHECK(err.empty() && v.S(L"content").find(L"宽字符 hello") != std::wstring::npos &&
              v.S(L"content").find(L"第二行") != std::wstring::npos, "utf16le-file");
    }
    {
        std::wstring err, r = RunRead(L"u8bom.txt", &err);
        Jv v = Res8(r);
        CHECK(err.empty() && v.S(L"content") == L"BOM中文OK", "utf8-bom-file");
    }
    {
        std::wstring err, r = RunRead(L"big.txt", &err);
        Jv v = Res8(r);
        const std::wstring& c = v.S(L"content");
        bool head = c.find(L"AAAAAAAA") != std::wstring::npos;   /* 头部窗口 = 全 A */
        bool tail = c.find(L"FINALTAIL") != std::wstring::npos;
        bool omit = c.find(L"中间省略") != std::wstring::npos;
        CHECK(err.empty() && head && tail && omit, "big-file-head-tail-precise-omit");
        if (!(err.empty() && head && tail && omit)) DumpCase("big", err, r);
    }
    {
        std::wstring err, r = RunRead(L"binary.bin", &err);
        CHECK(!err.empty() && err.find(L"二进制") != std::wstring::npos, "binary-reject");
    }
    {
        std::wstring err, r = RunRead(L"fake.pdf", &err);
        CHECK(!err.empty() && err.find(L"暂不支持") != std::wstring::npos, "pdf-explicit-degrade");
    }
    {
        std::wstring err, r = RunRead(L"missing.txt", &err);
        CHECK(!err.empty() && err.find(L"不存在") != std::wstring::npos, "missing-file");
    }

    /* ---- Office 解包 (ZIP + inflate + XML 抽取) ---- */
    {
        std::wstring err, r = RunRead(L"test.docx", &err);
        Jv v = Res8(r);
        const std::wstring& c = v.S(L"content");
        CHECK(err.empty(), "docx-no-error");
        CHECK(c.find(L"第一段\tA&B") != std::wstring::npos, "docx-para-tab-entity");
        CHECK(c.find(L"行一\n行二") != std::wstring::npos, "docx-br-newline");
        CHECK(v.S(L"格式或编码").find(L"docx") != std::wstring::npos, "docx-kind");
    }
    {
        std::wstring err, r = RunRead(L"stored.docx", &err);
        Jv v = Res8(r);
        bool ok = err.empty() && v.S(L"content").find(L"存储条目测试OK") != std::wstring::npos;
        CHECK(ok, "zip-stored-entry-and-stored-blocks");
        if (!ok) DumpCase("stored", err, r);
    }
    {
        std::wstring err, r = RunRead(L"test.xlsx", &err);
        Jv v = Res8(r);
        const std::wstring& c = v.S(L"content");
        CHECK(err.empty(), "xlsx-no-error");
        CHECK(c.find(L"苹果\t123.5") != std::wstring::npos, "xlsx-shared-string-and-number");
        CHECK(c.find(L"香蕉\tformula") != std::wstring::npos, "xlsx-richtext-and-str");
        CHECK(c.find(L"工作表 1") != std::wstring::npos, "xlsx-sheet-label");
    }
    {
        std::wstring err, r = RunRead(L"test.pptx", &err);
        Jv v = Res8(r);
        const std::wstring& c = v.S(L"content");
        CHECK(err.empty() && c.find(L"标题一") != std::wstring::npos &&
              c.find(L"副标") != std::wstring::npos, "pptx-slide1");
        /* 页序 = 数字序 (slide10 排在 slide2 后, 字典序会排错) */
        CHECK(c.find(L"第 2 页") != std::wstring::npos && c.find(L"第二页内容") != std::wstring::npos,
              "pptx-slide2");
        CHECK(c.find(L"第 10 页") != std::wstring::npos && c.find(L"第十页") != std::wstring::npos,
              "pptx-slide10-numeric-order");
        if (err.empty() ? c.size() < 10 : true) DumpCase("pptx", err, r);
    }

    /* ---- file_op 逐项更改记录 (真实 SHFileOperation; permanent 删除不进回收站) ---- */
    {
        wchar_t tmp[1024];
        GetEnvironmentVariableW(L"TEMP", tmp, 1024);
        /* 每次进程用全新目录: fixtures 目录跨运行持久, 上次的 chg-* 残留会让
         * rename/mkdir 撞"目标已存在/已存在同名"而假失败 */
        std::wstring base = std::wstring(tmp) + L"\\aft-chg-" + std::to_wstring(GetCurrentProcessId());
        CreateDirectoryW(base.c_str(), NULL);
        std::wstring dst = base + L"\\chg-dst";
        CreateDirectoryW(dst.c_str(), NULL);
        std::wstring src = base + L"\\chg-a.txt";
        {
            HANDLE h = CreateFileW(src.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD w = 0;
                WriteFile(h, "x", 1, &w, NULL);
                CloseHandle(h);
            }
        }
        AiJob j;
        /* rename: from→to 各就各位 */
        {
            std::wstring a = src, b = base + L"\\chg-b.txt";
            AiToolStep st;
            AiFileOp op;
            Jv v;
            v.t = 5;
            Jv act;
            act.t = 3;
            act.str = L"rename";
            v.obj.push_back({ L"action", std::move(act) });
            Jv rns;
            rns.t = 4;
            Jv r1;
            r1.t = 5;
            Jv f1, t1;
            f1.t = 3;
            f1.str = a;
            t1.t = 3;
            t1.str = b;
            r1.obj.push_back({ L"from", std::move(f1) });
            r1.obj.push_back({ L"to", std::move(t1) });
            rns.arr.push_back(std::move(r1));
            v.obj.push_back({ L"renames", std::move(rns) });
            std::wstring err = FileOpPrepare(v, &op);
            err = err.empty() ? FileOpExecute(&j, op, &st) : err;
            bool ok = err.empty() && st.chg.size() == 1 && st.chg[0].act == 2 &&
                      st.chg[0].from == a && st.chg[0].to == b;
            CHECK(ok, "fileop-rename-change-record");
            if (!ok) printf("  [dump rename] err=%ls chg=%zu res8=%s\n", err.c_str(), st.chg.size(),
                            st.res8.substr(0, 200).c_str());
            /* res8 changes 回执给模型 */
            Jv r8 = Res8(W8(st.res8.c_str()));
            const Jv* chgs = r8.Get(L"changes");
            CHECK(chgs && chgs->t == 4 && chgs->arr.size() == 1 &&
                  chgs->arr[0].S(L"to") == b, "fileop-res8-changes-receipt");
            /* 源已不存在 = 该项失败 → 工具不报错但**不留记录** */
            AiToolStep st2;
            AiFileOp op2;
            Jv v2;
            v2.t = 5;
            Jv act2;
            act2.t = 3;
            act2.str = L"rename";
            v2.obj.push_back({ L"action", std::move(act2) });
            Jv rns2;
            rns2.t = 4;
            Jv r2;
            r2.t = 5;
            Jv f2, t2;
            f2.t = 3;
            f2.str = a;   /* chg-a.txt 已被上一段改名 → 不存在 */
            t2.t = 3;
            t2.str = base + L"\\chg-b2.txt";
            r2.obj.push_back({ L"from", std::move(f2) });
            r2.obj.push_back({ L"to", std::move(t2) });
            rns2.arr.push_back(std::move(r2));
            v2.obj.push_back({ L"renames", std::move(rns2) });
            /* 源已不存在 = 该项失败 → 工具不报错, 但**失败项也记 chg (带原因)** — 不静默丢弃 */
            std::wstring err2 = FileOpPrepare(v2, &op2);
            err2 = err2.empty() ? FileOpExecute(&j, op2, &st2) : err2;
            CHECK(err2.empty() && st2.chg.size() == 1 && !st2.chg[0].ok &&
                  st2.chg[0].from == a && st2.chg[0].to == base + L"\\chg-b2.txt" &&
                  !st2.chg[0].err.empty() && st2.res8.find("failed") != std::string::npos,
                  "fileop-failure-recorded");
            /* from + id 同给: from 胜出 (id 不再被反查 — 测试环境无引擎, 反查必报
             * "FileId 不在索引中"; 成功即证明走了 from)。改名前名字照记录原样保留 */
            std::wstring f = base + L"\\chg - f.txt", g = base + L"\\chg-g.txt";
            { HANDLE h = CreateFileW(f.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
            AiToolStep st4;
            AiFileOp op4;
            Jv v4;
            v4.t = 5;
            Jv act4;
            act4.t = 3;
            act4.str = L"rename";
            v4.obj.push_back({ L"action", std::move(act4) });
            Jv rns4;
            rns4.t = 4;
            Jv r4;
            r4.t = 5;
            Jv f4, t4, i4;
            f4.t = 3;
            f4.str = f;
            t4.t = 3;
            t4.str = g;
            i4.t = 2;
            i4.num = 7;
            r4.obj.push_back({ L"id", std::move(i4) });
            r4.obj.push_back({ L"from", std::move(f4) });
            r4.obj.push_back({ L"to", std::move(t4) });
            rns4.arr.push_back(std::move(r4));
            v4.obj.push_back({ L"renames", std::move(rns4) });
            std::wstring err4 = FileOpPrepare(v4, &op4);
            err4 = err4.empty() ? FileOpExecute(&j, op4, &st4) : err4;
            CHECK(err4.empty() && st4.chg.size() == 1 && st4.chg[0].from == f && st4.chg[0].to == g,
                  "fileop-rename-from-beats-id");
            /* 只给 id 不给 from = 直接拒绝 (id 反查恒为索引最新名, 当不了"改名前"记录) */
            {
                AiToolStep st7;
                AiFileOp op7;
                Jv v7;
                v7.t = 5;
                Jv act7;
                act7.t = 3;
                act7.str = L"rename";
                v7.obj.push_back({ L"action", std::move(act7) });
                Jv rns7;
                rns7.t = 4;
                Jv r7;
                r7.t = 5;
                Jv i7, t7;
                i7.t = 2;
                i7.num = 7;
                t7.t = 3;
                t7.str = base + L"\\chg-x.txt";
                r7.obj.push_back({ L"id", std::move(i7) });
                r7.obj.push_back({ L"to", std::move(t7) });
                rns7.arr.push_back(std::move(r7));
                v7.obj.push_back({ L"renames", std::move(rns7) });
                std::wstring err7 = FileOpPrepare(v7, &op7);
                CHECK(!err7.empty() && err7.find(L"from") != std::wstring::npos,
                      "fileop-rename-id-only-rejected");
            }
            /* 改名到自己 (文件已是目标名后重复提交) = unchanged: 不执行不计成功不留记录 */
            {
                AiToolStep st5;
                AiFileOp op5;
                Jv v5;
                v5.t = 5;
                Jv act5;
                act5.t = 3;
                act5.str = L"rename";
                v5.obj.push_back({ L"action", std::move(act5) });
                Jv rns5;
                rns5.t = 4;
                Jv r5;
                r5.t = 5;
                Jv f5, t5;
                f5.t = 3;
                f5.str = g;   /* chg-g.txt 已存在, 改名为自己 */
                t5.t = 3;
                t5.str = g;
                r5.obj.push_back({ L"from", std::move(f5) });
                r5.obj.push_back({ L"to", std::move(t5) });
                rns5.arr.push_back(std::move(r5));
                v5.obj.push_back({ L"renames", std::move(rns5) });
                std::wstring err5 = FileOpPrepare(v5, &op5);
                err5 = err5.empty() ? FileOpExecute(&j, op5, &st5) : err5;
                Jv r85 = Res8(W8(st5.res8.c_str()));
                const Jv* un = r85.Get(L"unchanged");
                const Jv* dn = r85.Get(L"done");
                CHECK(err5.empty() && st5.chg.empty() && un && un->t == 2 && un->num == 1 &&
                      dn && dn->t == 2 && dn->num == 0,
                      "fileop-rename-samepath-unchanged");
            }
            /* copy 到原目录 (dst==src) = unchanged */
            {
                AiToolStep st6;
                AiFileOp op6;
                Jv v6;
                v6.t = 5;
                Jv act6;
                act6.t = 3;
                act6.str = L"copy";
                v6.obj.push_back({ L"action", std::move(act6) });
                Jv ps6;
                ps6.t = 4;
                Jv p6;
                p6.t = 3;
                p6.str = g;
                ps6.arr.push_back(std::move(p6));
                v6.obj.push_back({ L"paths", std::move(ps6) });
                Jv tg6;
                tg6.t = 3;
                tg6.str = base;   /* 原目录 → dst == src */
                v6.obj.push_back({ L"target", std::move(tg6) });
                std::wstring err6 = FileOpPrepare(v6, &op6);
                err6 = err6.empty() ? FileOpExecute(&j, op6, &st6) : err6;
                Jv r86 = Res8(W8(st6.res8.c_str()));
                const Jv* un6 = r86.Get(L"unchanged");
                CHECK(err6.empty() && st6.chg.empty() && un6 && un6->t == 2 && un6->num == 1,
                      "fileop-copy-samepath-unchanged");
            }
        }
        /* mkdir: 只 to */
        {
            AiToolStep st;
            AiFileOp op;
            Jv v;
            v.t = 5;
            Jv act;
            act.t = 3;
            act.str = L"mkdir";
            v.obj.push_back({ L"action", std::move(act) });
            Jv tg;
            tg.t = 3;
            tg.str = base + L"\\chg-new\\deep";
            v.obj.push_back({ L"target", std::move(tg) });
            std::wstring err = FileOpPrepare(v, &op);
            err = err.empty() ? FileOpExecute(&j, op, &st) : err;
            CHECK(err.empty() && st.chg.size() == 1 && st.chg[0].act == 4 &&
                  st.chg[0].from.empty() && st.chg[0].to == base + L"\\chg-new\\deep",
                  "fileop-mkdir-change-record");
        }
        /* copy: 源→目标目录\同名; delete(permanent): 只 from */
        {
            std::wstring a = base + L"\\chg-c.txt";
            { HANDLE h = CreateFileW(a.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
            AiToolStep st;
            AiFileOp op;
            Jv v;
            v.t = 5;
            Jv act;
            act.t = 3;
            act.str = L"copy";
            v.obj.push_back({ L"action", std::move(act) });
            Jv ps;
            ps.t = 4;
            Jv p1;
            p1.t = 3;
            p1.str = a;
            ps.arr.push_back(std::move(p1));
            v.obj.push_back({ L"paths", std::move(ps) });
            Jv tg;
            tg.t = 3;
            tg.str = dst;
            v.obj.push_back({ L"target", std::move(tg) });
            std::wstring err = FileOpPrepare(v, &op);
            err = err.empty() ? FileOpExecute(&j, op, &st) : err;
            CHECK(err.empty() && st.chg.size() == 1 && st.chg[0].act == 0 &&
                  st.chg[0].to == dst + L"\\chg-c.txt", "fileop-copy-change-record");
            AiToolStep st3;
            AiFileOp op3;
            Jv v3;
            v3.t = 5;
            Jv act3;
            act3.t = 3;
            act3.str = L"delete";
            v3.obj.push_back({ L"action", std::move(act3) });
            Jv pm;
            pm.t = 1;
            pm.b = true;
            v3.obj.push_back({ L"permanent", std::move(pm) });
            Jv ps3;
            ps3.t = 4;
            Jv q1;
            q1.t = 3;
            q1.str = a;
            ps3.arr.push_back(std::move(q1));
            Jv q2;
            q2.t = 3;
            q2.str = dst + L"\\chg-c.txt";
            ps3.arr.push_back(std::move(q2));
            v3.obj.push_back({ L"paths", std::move(ps3) });
            std::wstring err3 = FileOpPrepare(v3, &op3);
            err3 = err3.empty() ? FileOpExecute(&j, op3, &st3) : err3;
            CHECK(err3.empty() && st3.chg.size() == 2 && st3.chg[0].act == 3 &&
                  st3.chg[0].from == a && st3.chg[0].to.empty(), "fileop-delete-change-record");
        }
    }

    printf("\n%d failed\n", fails);
    return fails;
}
