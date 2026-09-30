/*
 * xjs_pdf.cpp — PDF 首页预览后端 (系统自带 Windows.Data.Pdf 组件, 免第三方库)
 *
 * 组件为 WinRT 激活工厂, 全部动态加载零新链接依赖: combase.dll (RoGetActivationFactory/
 * WindowsCreateString — 进程内必有) + shcore.dll (CreateRandomAccessStreamOnFile/OverStream,
 * Win8.1+)。任一缺失 → 一次性记失败 → 预览干净落既有文件信息卡 (降级安全)。
 *
 * 一律工作线程调用 (UI 线程零解码红线, 同 xjs_preview 图片异步管线): 打开文档/渲染都是
 * WinRT 异步操作, worker 在 MTA 里轮询状态等待 (带超时); 页面渲成 PNG 流 → WIC 解码 →
 * PBGRA → WIC Fant 重采样到目标尺寸 → FlipRotator (rot 1..3 同图片口径: 本函数收"转后"
 * 目标宽高, 内部换回转前喂渲染) → 产出裸字节, UI 线程经 XjsBitmapFromBgra 转本域。
 * 页面尺寸 (dims) 同样只有 worker 能取 (WIC 读不了 PDF 头), 经 XjsPreviewImgAsk 的
 * dims-only 作业 (tw=th=0) 异步回填 g_previewImgW/H, 之后与普通图片完全同管线
 * (渲染/灯箱/缩放/旋转)。
 */
#include "xjs_app.h"
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
typedef wf::__FIAsyncOperation_1_Windows__CData__CPdf__CPdfDocument_t PdfLoadOp;

/* ==================== 动态加载 (进程内一次) ==================== */

typedef HRESULT (WINAPI *PFN_RoGetActivationFactory)(HSTRING, REFIID, void**);
typedef HRESULT (WINAPI *PFN_WindowsCreateString)(PCWSTR, UINT32, HSTRING*);
typedef HRESULT (WINAPI *PFN_WindowsDeleteString)(HSTRING);
typedef HRESULT (WINAPI *PFN_CreateRandomAccessStreamOnFile)(PCWSTR, DWORD, REFIID, void**);
typedef HRESULT (WINAPI *PFN_CreateRandomAccessStreamOverStream)(IStream*, DWORD, REFIID, void**);

static PFN_RoGetActivationFactory s_pRoGetActivationFactory = NULL;
static PFN_WindowsCreateString s_pWindowsCreateString = NULL;
static PFN_WindowsDeleteString s_pWindowsDeleteString = NULL;
static PFN_CreateRandomAccessStreamOnFile s_pCreateRasOnFile = NULL;
static PFN_CreateRandomAccessStreamOverStream s_pCreateRasOverStream = NULL;
static bool s_pdfApisFailed = false;   /* 组件缺失记忆 (每次选择 PDF 文件不再重复探测) */

static bool PdfEnsureApis() {
    if (s_pRoGetActivationFactory) return true;
    if (s_pdfApisFailed) return false;
    /* combase.dll: worker 已 CoInitializeEx(MTA), 模块必在进程内 */
    HMODULE cb = GetModuleHandleW(L"combase.dll");
    if (!cb) cb = LoadLibraryW(L"combase.dll");
    HMODULE sc = LoadLibraryW(L"shcore.dll");   /* Win8.1+; 以下系统干净失败落信息卡 */
    s_pRoGetActivationFactory = cb ? (PFN_RoGetActivationFactory)GetProcAddress(cb, "RoGetActivationFactory") : NULL;
    s_pWindowsCreateString = cb ? (PFN_WindowsCreateString)GetProcAddress(cb, "WindowsCreateString") : NULL;
    s_pWindowsDeleteString = cb ? (PFN_WindowsDeleteString)GetProcAddress(cb, "WindowsDeleteString") : NULL;
    s_pCreateRasOnFile = sc ? (PFN_CreateRandomAccessStreamOnFile)GetProcAddress(sc, "CreateRandomAccessStreamOnFile") : NULL;
    s_pCreateRasOverStream = sc ? (PFN_CreateRandomAccessStreamOverStream)GetProcAddress(sc, "CreateRandomAccessStreamOverStream") : NULL;
    if (!s_pRoGetActivationFactory || !s_pWindowsCreateString || !s_pWindowsDeleteString ||
        !s_pCreateRasOnFile || !s_pCreateRasOverStream) {
        s_pdfApisFailed = true;
        return false;
    }
    return true;
}

/* HSTRING 便捷构造 (用完必须 PdfHsFree) */
struct PdfHs {
    HSTRING h = NULL;
    bool Make(const wchar_t* s) { return SUCCEEDED(s_pWindowsCreateString(s, (UINT32)wcslen(s), &h)); }
    ~PdfHs() { if (h) s_pWindowsDeleteString(h); }
};

/* WinRT async 完成等待 — 实测定案: 纯轮询 get_Status 在本组件上永不翻转 (Started 挂死),
   put_Completed 注册后工作才在线程池跑完并回调; 终态必须 QI IAsyncInfo 读 (参数化接口
   IAsyncOperation`1 的 vtable 不含 IAsyncInfo, reinterpret 槽位错位且出参字节宽不符)。 */
struct PdfActionDone : wf::IAsyncActionCompletedHandler {
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    ULONG ref = 1;
    STDMETHODIMP QueryInterface(REFIID riid, void** pp) override {
        if (!pp) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(wf::IAsyncActionCompletedHandler)) {
            *pp = this; AddRef(); return S_OK;
        }
        *pp = NULL; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref; }
    STDMETHODIMP_(ULONG) Release() override { ULONG r = --ref; if (!r) { if (ev) CloseHandle(ev); delete this; } return r; }
    HRESULT STDMETHODCALLTYPE Invoke(wf::IAsyncAction*, wf::AsyncStatus) override {
        SetEvent(ev);
        return S_OK;
    }
};
struct PdfLoadDone : wf::__FIAsyncOperationCompletedHandler_1_Windows__CData__CPdf__CPdfDocument_t {
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    ULONG ref = 1;
    STDMETHODIMP QueryInterface(REFIID riid, void** pp) override {
        if (!pp) return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(wf::__FIAsyncOperationCompletedHandler_1_Windows__CData__CPdf__CPdfDocument_t)) {
            *pp = this; AddRef(); return S_OK;
        }
        *pp = NULL; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref; }
    STDMETHODIMP_(ULONG) Release() override { ULONG r = --ref; if (!r) { if (ev) CloseHandle(ev); delete this; } return r; }
    HRESULT STDMETHODCALLTYPE Invoke(wf::IAsyncOperation<pdf::PdfDocument*>*, wf::AsyncStatus) override {
        SetEvent(ev);
        return S_OK;
    }
};
/* 终态读取: QI IAsyncInfo (组件对象真身实现它) */
static bool PdfAsyncCompleted(IUnknown* op) {
    wf::IAsyncInfo* info = NULL;
    if (FAILED(op->QueryInterface(__uuidof(wf::IAsyncInfo), (void**)&info))) return false;
    wf::AsyncStatus st;
    bool ok = SUCCEEDED(info->get_Status(&st)) && st == wf::AsyncStatus::Completed;
    info->Release();
    return ok;
}
static bool PdfWaitLoad(PdfLoadOp* op, DWORD timeoutMs) {
    if (!op) return false;
    PdfLoadDone* h = new PdfLoadDone();
    if (FAILED(op->put_Completed(h))) { h->Release(); return false; }
    DWORD w = WaitForSingleObject(h->ev, timeoutMs);
    h->Release();   /* 只放自持初引; op 侧引用随 op 释放在后 */
    return w == WAIT_OBJECT_0 && PdfAsyncCompleted(op);
}
static bool PdfWaitAction(wf::IAsyncAction* act, DWORD timeoutMs) {
    if (!act) return false;
    PdfActionDone* h = new PdfActionDone();
    if (FAILED(act->put_Completed(h))) { h->Release(); return false; }
    DWORD w = WaitForSingleObject(h->ev, timeoutMs);
    h->Release();
    return w == WAIT_OBJECT_0 && PdfAsyncCompleted(act);
}

/* 静态工厂 (进程一次): PdfDocument 的 LoadFromStreamAsync 入口 */
static pdf::IPdfDocumentStatics* PdfStatics() {
    static pdf::IPdfDocumentStatics* s_st = NULL;
    static bool s_tried = false;
    if (!s_tried) {
        s_tried = true;
        if (PdfEnsureApis()) {
            PdfHs hs;
            if (hs.Make(L"Windows.Data.Pdf.PdfDocument"))
                s_pRoGetActivationFactory(hs.h, __uuidof(pdf::IPdfDocumentStatics), (void**)&s_st);
        }
    }
    return s_st;
}

/* 渲染选项 (每份新建): 目标宽 + 白色不透明底 (PDF 无背景区透明画在面板暗底上花) */
static pdf::IPdfPageRenderOptions* PdfRenderOptions(UINT32 dstW) {
    pdf::IPdfPageRenderOptions* opts = NULL;
    if (!PdfEnsureApis()) return NULL;
    PdfHs hs;
    IActivationFactory* af = NULL;
    IInspectable* inst = NULL;
    if (hs.Make(L"Windows.Data.Pdf.PdfPageRenderOptions") &&
        SUCCEEDED(s_pRoGetActivationFactory(hs.h, __uuidof(IActivationFactory), (void**)&af)) && af &&
        SUCCEEDED(af->ActivateInstance(&inst)) && inst &&
        SUCCEEDED(inst->QueryInterface(__uuidof(pdf::IPdfPageRenderOptions), (void**)&opts)) && opts) {
        opts->put_DestinationWidth(dstW);
        ABI::Windows::UI::Color white = {};
        white.A = 255; white.R = 255; white.G = 255; white.B = 255;
        opts->put_BackgroundColor(white);
    }
    if (inst) inst->Release();
    if (af) af->Release();
    if (!opts) return NULL;
    return opts;
}

/* 打开文档取首页 (dims 与渲染共用; 懒解析, 打开本身轻量) */
static pdf::IPdfPage* PdfOpenFirstPage(const std::wstring& path) {
    pdf::IPdfDocumentStatics* st = PdfStatics();
    if (!st) return NULL;
    wss::IRandomAccessStream* ras = NULL;
    pdf::IPdfPage* page = NULL;
    if (SUCCEEDED(s_pCreateRasOnFile(path.c_str(), 0 /*ACCESS_READ*/, __uuidof(wss::IRandomAccessStream), (void**)&ras)) && ras) {
        PdfLoadOp* op = NULL;
        if (SUCCEEDED(st->LoadFromStreamAsync(ras, &op)) && op && PdfWaitLoad(op, 8000)) {
            pdf::IPdfDocument* doc = NULL;   /* AggregateType: GetResults 直出默认接口 */
            if (SUCCEEDED(op->GetResults(&doc)) && doc) {
                UINT32 count = 0;
                if (SUCCEEDED(doc->get_PageCount(&count)) && count > 0)
                    doc->GetPage(0, &page);
                doc->Release();
            }
        }
        if (op) op->Release();
        ras->Release();
    }
    return page;
}

/* ==================== 对外入口 (全部 worker 线程调) ==================== */

/* 首页显示尺寸 (96dpi 基准像素; 已含页面 /Rotate 的显示向) */
bool XjsPdfPageDims(const std::wstring& path, int* w, int* h) {
    if (w) *w = 0;
    if (h) *h = 0;
    if (path.empty()) return false;
    pdf::IPdfPage* page = PdfOpenFirstPage(path);
    if (!page) return false;
    bool ok = false;
    wf::Size sz = {};
    if (SUCCEEDED(page->get_Size(&sz)) && sz.Width > 1 && sz.Height > 1) {
        if (w) *w = (int)(sz.Width + 0.5f);
        if (h) *h = (int)(sz.Height + 0.5f);
        ok = true;
    }
    page->Release();
    return ok;
}

/* 首页渲到目标尺寸 PBGRA 字节 (签名/口径同 xjs_d2d 的 XjsDecodeFileImagePixels:
   dstW/dstH 为 rot 后内容尺寸, rot 1..3 内部换回转前; 失败 = false 且 out 清空) */
bool XjsPdfRenderPixels(const std::wstring& path, int dstW, int dstH, int rot,
                        std::vector<uint8_t>* out, int* stride) {
    if (out) out->clear();
    if (!g_wic || path.empty() || dstW <= 0 || dstH <= 0 || dstW > 4096 || dstH > 4096 ||
        !out || !stride) return false;
    if (rot < 0 || rot > 3) rot = 0;
    *stride = dstW * 4;
    out->assign((size_t)*stride * dstH, 0);
    /* 旋转 90°/270° 时渲染喂转前尺寸 (同 XjsDecodeFileImagePixels 内部换算) */
    UINT scW = (rot % 2 == 1) ? (UINT)dstH : (UINT)dstW;
    UINT scH = (rot % 2 == 1) ? (UINT)dstW : (UINT)dstH;

    pdf::IPdfPage* page = PdfOpenFirstPage(path);
    if (!page) { out->clear(); return false; }

    bool ok = false;
    pdf::IPdfPageRenderOptions* opts = PdfRenderOptions(scW);
    IStream* stm = NULL;
    wss::IRandomAccessStream* ras = NULL;
    wf::IAsyncAction* act = NULL;
    IWICBitmapDecoder* dec = NULL;
    IWICBitmapFrameDecode* frame = NULL;
    IWICFormatConverter* conv = NULL;
    IWICBitmapScaler* scaler = NULL;
    IWICBitmapFlipRotator* rotator = NULL;
    if (opts &&
        SUCCEEDED(CreateStreamOnHGlobal(NULL, TRUE, &stm)) && stm &&
        PdfEnsureApis() &&
        SUCCEEDED(s_pCreateRasOverStream(stm, 0 /*BSOS_DEFAULT*/, __uuidof(wss::IRandomAccessStream), (void**)&ras)) && ras &&
        SUCCEEDED(page->RenderWithOptionsToStreamAsync(ras, opts, &act)) && act &&
        PdfWaitAction(act, 15000)) {
        LARGE_INTEGER zero = {};
        stm->Seek(zero, STREAM_SEEK_SET, NULL);
        /* 渲染输出为编码位图流 — WIC 任意解码器认格式 (不赌具体编码) */
        if (SUCCEEDED(g_wic->CreateDecoderFromStream(stm, NULL, WICDecodeMetadataCacheOnDemand, &dec)) && dec &&
            SUCCEEDED(dec->GetFrame(0, &frame)) && frame &&
            SUCCEEDED(g_wic->CreateFormatConverter(&conv)) && conv &&
            SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom)) &&
            SUCCEEDED(g_wic->CreateBitmapScaler(&scaler)) && scaler &&
            SUCCEEDED(scaler->Initialize(conv, scW, scH, WICBitmapInterpolationModeFant))) {
            IWICBitmapSource* src = scaler;   /* rot=0 直接出图; 其余过 FlipRotator (无损 90° 搬转) */
            if (rot != 0) {
                if (SUCCEEDED(g_wic->CreateBitmapFlipRotator(&rotator)) && rotator &&
                    SUCCEEDED(rotator->Initialize(scaler,
                        rot == 1 ? WICBitmapTransformRotate90 : rot == 2 ? WICBitmapTransformRotate180
                                                                         : WICBitmapTransformRotate270)))
                    src = rotator;
                else
                    src = NULL;
            }
            ok = src != NULL && SUCCEEDED(src->CopyPixels(NULL, *stride, (UINT)out->size(), out->data()));
        }
    }
    if (rotator) rotator->Release();
    if (scaler) scaler->Release();
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (act) act->Release();
    if (ras) ras->Release();
    if (stm) stm->Release();
    if (opts) opts->Release();
    page->Release();
    if (!ok) out->clear();
    return ok;
}
