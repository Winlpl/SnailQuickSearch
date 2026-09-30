/*
 * test_pdf_preview.cpp — PDF 首页预览后端 (xjs_pdf.cpp) 纯逻辑单测
 * 链 编译中间产物\xjs_pdf.obj (根目录 build.bat 产物), 不起引擎不碰 UI;
 * xjs_pdf.obj 只外部引用 g_wic — 本 TU 提供该全局并自建 WIC factory。
 * 样本 = 程序化生成的最小合法 PDF (A4, 内容流画 蓝+黑 两矩形), 无外部夹具文件。
 * 通过=输出 0 失败, 非零=失败数。
 * 覆盖: dims (合法/缺失/垃圾) / 渲染 (尺寸·stride·内容非空·旋转宽高互换) / 参数闸。
 * 注: XjsDecodeMetaFilePixels/XjsMetaFileDims 在 xjs_d2d.obj (依赖宿主全局), 不进本单测。
 * 构建: **test\build_test_pdf_preview.bat** 一键编译+运行 (前置: 跑过根目录 build.bat)。
 */
#include "xjs_app.h"
#include <stdio.h>
#include <string>
#include <vector>

/* xjs_pdf.obj 引用的全局 (宿主里由 xjs_app.cpp 定义) — 测试自建 WIC factory */
IWICImagingFactory* g_wic = NULL;

static int fails = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); fails++; } \
} while (0)

/* ==================== 最小 PDF 夹具 ==================== */

/* A4 595x842, 单页, 内容 = 蓝矩形 + 黑矩形 (验渲染非空内容用) */
static bool MakeMinimalPdf(const std::wstring& path) {
    std::string content = "q 0.2 0.4 0.9 rg 60 60 200 150 re f 0 0 0 rg 300 600 150 100 re f Q\n";
    std::string objs[5] = {
        "1 0 obj\n<</Type/Catalog/Pages 2 0 R>>\nendobj\n",
        "2 0 obj\n<</Type/Pages/Kids[3 0 R]/Count 1>>\nendobj\n",
        "3 0 obj\n<</Type/Page/Parent 2 0 R/MediaBox[0 0 595 842]/Contents 4 0 R"
        "/Resources<</Font<</F1 5 0 R>>>>>>\nendobj\n",
        "4 0 obj\n<</Length " + std::to_string(content.size()) + ">>\nstream\n" + content + "endstream\nendobj\n",
        "5 0 obj\n<</Type/Font/Subtype/Type1/BaseFont/Helvetica>>\nendobj\n",
    };
    std::string out = "%PDF-1.4\n";
    UINT32 offs[5] = { 0, 0, 0, 0, 0 };
    for (int i = 0; i < 5; i++) {
        offs[i] = (UINT32)out.size();
        out += objs[i];
    }
    UINT32 xref = (UINT32)out.size();
    out += "xref\n0 6\n0000000000 65535 f \n";
    char line[32];
    for (int i = 0; i < 5; i++) {
        sprintf_s(line, "%010u 00000 n \n", offs[i]);
        out += line;
    }
    out += "trailer\n<</Size 6/Root 1 0 R>>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, out.data(), (DWORD)out.size(), &written, NULL) && written == out.size();
    CloseHandle(h);
    return ok;
}

static bool ReadAllBytes(const std::wstring& path, std::string* out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof(buf), &n, NULL) && n > 0) out->append(buf, n);
    CloseHandle(h);
    return true;
}

/* 打开链逐步探针 (复刻 PdfOpenFirstPage, 打印每步 HRESULT — 夹具/组件问题定位用) */
#include <windows.foundation.h>
#include <windows.foundation.collections.h>
#include <asyncinfo.h>
#include <windows.storage.streams.h>
#include <windows.ui.h>
#include <windows.data.pdf.h>
#include <activation.h>
#include <winstring.h>
namespace wf = ABI::Windows::Foundation;
namespace wss = ABI::Windows::Storage::Streams;
namespace pdf = ABI::Windows::Data::Pdf;

/* CompletedHandler 实验: 判别回调派发 vs 状态翻转 (轮询永不完成时定位用) */
struct PdfLoadHandler : wf::__FIAsyncOperationCompletedHandler_1_Windows__CData__CPdf__CPdfDocument_t {
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    ULONG ref = 1;
    DWORD threadId = 0;
    STDMETHODIMP QueryInterface(REFIID riid, void** pp) override {
        if (!pp) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(wf::__FIAsyncOperationCompletedHandler_1_Windows__CData__CPdf__CPdfDocument_t)) {
            *pp = this; AddRef(); return S_OK;
        }
        *pp = NULL; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref; }
    STDMETHODIMP_(ULONG) Release() override { ULONG r = --ref; if (!r) { if (ev) CloseHandle(ev); delete this; } return r; }
    HRESULT STDMETHODCALLTYPE Invoke(wf::IAsyncOperation<pdf::PdfDocument*>*, wf::AsyncStatus) override {
        threadId = GetCurrentThreadId();
        SetEvent(ev);
        return S_OK;
    }
};

static void ProbePdfOpen(const std::wstring& path) {
    HMODULE cb = GetModuleHandleW(L"combase.dll");
    auto pRoGet = (HRESULT (WINAPI*)(HSTRING, REFIID, void**))GetProcAddress(cb, "RoGetActivationFactory");
    auto pCreate = (HRESULT (WINAPI*)(PCWSTR, UINT32, HSTRING*))GetProcAddress(cb, "WindowsCreateString");
    auto pDel = (HRESULT (WINAPI*)(HSTRING))GetProcAddress(cb, "WindowsDeleteString");
    auto pRoInit = (HRESULT (WINAPI*)(int))GetProcAddress(cb, "RoInitialize");
    HRESULT hri = pRoInit ? pRoInit(1 /*RO_INIT_MULTITHREADED*/) : -1;
    printf("  [probe] RoInitialize hr=0x%08lX\n", (unsigned long)hri);
    HMODULE sc = LoadLibraryW(L"shcore.dll");
    printf("  [probe] shcore=%p combase=%p\n", (void*)sc, (void*)cb);
    if (!sc || !pRoGet || !pCreate) { printf("  [probe] apis missing\n"); return; }
    auto pRasOnFile = (HRESULT (WINAPI*)(PCWSTR, DWORD, REFIID, void**))GetProcAddress(sc, "CreateRandomAccessStreamOnFile");
    printf("  [probe] RasOnFile=%p\n", (void*)pRasOnFile);
    HSTRING hs = NULL;
    HRESULT hrc = pCreate(L"Windows.Data.Pdf.PdfDocument", 28, &hs);
    printf("  [probe] create hstring hr=0x%08lX\n", (unsigned long)hrc);
    pdf::IPdfDocumentStatics* st = NULL;
    hrc = pRoGet(hs, __uuidof(pdf::IPdfDocumentStatics), (void**)&st);
    printf("  [probe] RoGetActivationFactory hr=0x%08lX st=%p\n", (unsigned long)hrc, (void*)st);
    pDel(hs);
    if (!st) return;
    wss::IRandomAccessStream* ras = NULL;
    hrc = pRasOnFile(path.c_str(), 0, __uuidof(wss::IRandomAccessStream), (void**)&ras);
    printf("  [probe] RasOnFile hr=0x%08lX ras=%p\n", (unsigned long)hrc, (void*)ras);
    if (!ras) return;
    typedef wf::__FIAsyncOperation_1_Windows__CData__CPdf__CPdfDocument_t LoadOp;
    LoadOp* op = NULL;
    hrc = st->LoadFromStreamAsync(ras, &op);
    printf("  [probe] LoadFromStreamAsync hr=0x%08lX op=%p\n", (unsigned long)hrc, (void*)op);
    if (op) {
        /* A. CompletedHandler 等待 8s (回调派发实验) */
        PdfLoadHandler* h = new PdfLoadHandler();
        hrc = op->put_Completed(h);
        printf("  [probe] put_Completed hr=0x%08lX\n", (unsigned long)hrc);
        DWORD wres = WaitForSingleObject(h->ev, 8000);
        printf("  [probe] handler wait=%lu (0=signaled) invokeThread=%u mainThread=%u\n",
               (unsigned long)wres, h->threadId, GetCurrentThreadId());
        wf::IAsyncInfo* info = NULL;
        op->QueryInterface(__uuidof(wf::IAsyncInfo), (void**)&info);
        wf::AsyncStatus as = wf::AsyncStatus::Started;
        if (info) {
            info->get_Status(&as);
            printf("  [probe] QI-IAsyncInfo status=%d\n", (int)as);
        } else {
            printf("  [probe] QI IAsyncInfo FAILED\n");
        }
        pdf::IPdfDocument* doc = NULL;
        if (as == wf::AsyncStatus::Completed) {
            hrc = op->GetResults(&doc);
            printf("  [probe] GetResults hr=0x%08lX doc=%p\n", (unsigned long)hrc, (void*)doc);
            if (doc) {
                UINT32 n = 0;
                hrc = doc->get_PageCount(&n);
                printf("  [probe] PageCount hr=0x%08lX n=%u\n", (unsigned long)hrc, n);
                doc->Release();
            }
        }
        if (info) info->Release();
        op->Release();
        if (h->ref > 1) h->Release();   /* 宿主持引未释放时手动平衡 (探针即抛) */
    }
    ras->Release();
}

/* ==================== 断言 ==================== */

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);   /* 段错误也要看到已打印的步进 */
    (void)argc;
    /* wmain 后第一参 = 任意 PDF 路径: 只跑打开链探针 (真实文档定位夹具/组件问题用) */
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool coInit = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    if (argc > 1) {
        ProbePdfOpen(argv[1]);
        int w0 = 0, h0 = 0;
        bool ok = XjsPdfPageDims(argv[1], &w0, &h0);
        printf("  [probe] XjsPdfPageDims ok=%d %dx%d\n", ok ? 1 : 0, w0, h0);
        return 0;
    }
    bool coInit2 = coInit;
    (void)coInit2;
    CHECK(coInit, "CoInitializeEx MTA");
    CHECK(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                     __uuidof(IWICImagingFactory), (void**)&g_wic)) && g_wic,
          "WIC factory");

    wchar_t tmp[1024];
    GetEnvironmentVariableW(L"TEMP", tmp, 1024);
    std::wstring dir = std::wstring(tmp) + L"\\tpdf";
    CreateDirectoryW(dir.c_str(), NULL);
    std::wstring pdfPath = dir + L"\\sample.pdf";
    std::wstring junkPath = dir + L"\\junk.pdf";
    CHECK(MakeMinimalPdf(pdfPath), "make minimal PDF");
    {   HANDLE h = CreateFileW(junkPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD w = 0; WriteFile(h, "this is not a pdf at all", 24, &w, NULL); CloseHandle(h); }

    /* ---- dims ---- */
    int dw = 0, dh = 0;
    CHECK(XjsPdfPageDims(pdfPath, &dw, &dh), "dims ok");
    /* MediaBox 595x842 是 pt (72dpi), get_Size 回 96dpi DIP: x96/72 = 793x1123 */
    CHECK(dw >= 792 && dw <= 794 && dh >= 1122 && dh <= 1124, "dims = A4 @96dpi 793x1123");
    printf("  [dump] dims=%dx%d\n", dw, dh);
    dw = dh = 0;
    CHECK(!XjsPdfPageDims(dir + L"\\no-such.pdf", &dw, &dh) && dw == 0 && dh == 0, "dims missing file = false");
    CHECK(!XjsPdfPageDims(junkPath, &dw, &dh), "dims junk = false");
    CHECK(!XjsPdfPageDims(L"", &dw, &dh), "dims empty path = false");

    /* ---- render (半页目标, 内容非空) ---- */
    std::vector<uint8_t> bgra;
    int stride = 0;
    CHECK(XjsPdfRenderPixels(pdfPath, 298, 421, 0, &bgra, &stride), "render ok");
    CHECK((int)bgra.size() == 298 * 421 * 4 && stride == 298 * 4, "render size/stride");
    if (bgra.size() == (size_t)298 * 421 * 4) {
        int dark = 0, bright = 0;
        for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
            int lum = (bgra[i] + bgra[i + 1] * 2 + bgra[i + 2]) / 4;   /* BGRA → 粗亮度 */
            if (lum < 128) dark++;
            else if (lum > 230) bright++;
        }
        CHECK(dark > 500, "render has drawn content (dark px)");
        CHECK(bright > (int)bgra.size() / 16, "render has white page background");
        printf("  [dump] dark=%d bright=%d\n", dark, bright);
    }
    /* ---- render rot=1 (转 90°: 转后 421x298, 内容尺寸互换) ---- */
    std::vector<uint8_t> rot1;
    stride = 0;
    CHECK(XjsPdfRenderPixels(pdfPath, 421, 298, 1, &rot1, &stride), "render rot=1 ok");
    CHECK((int)rot1.size() == 421 * 298 * 4 && stride == 421 * 4, "render rot=1 size/stride");
    if (rot1.size() == (size_t)421 * 298 * 4) {
        int dark = 0;
        for (size_t i = 0; i + 3 < rot1.size(); i += 4) {
            int lum = (rot1[i] + rot1[i + 1] * 2 + rot1[i + 2]) / 4;
            if (lum < 128) dark++;
        }
        CHECK(dark > 500, "render rot=1 has drawn content");
    }

    /* ---- 参数闸 ---- */
    CHECK(!XjsPdfRenderPixels(pdfPath, 0, 100, 0, &bgra, &stride), "render w=0 rejected");
    CHECK(!XjsPdfRenderPixels(pdfPath, 5000, 100, 0, &bgra, &stride), "render >4096 rejected");
    CHECK(!XjsPdfRenderPixels(junkPath, 298, 421, 0, &bgra, &stride), "render junk = false");
    CHECK(!XjsPdfRenderPixels(dir + L"\\no-such.pdf", 298, 421, 0, &bgra, &stride), "render missing = false");

    DeleteFileW(pdfPath.c_str());
    DeleteFileW(junkPath.c_str());
    RemoveDirectoryW(dir.c_str());
    (void)ReadAllBytes;
    printf("----\n%d failed\n", fails);
    if (coInit && SUCCEEDED(hr)) CoUninitialize();
    return fails;
}
