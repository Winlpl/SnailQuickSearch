/*
 * test_media_leak.cpp — 媒体引擎(每窗会话)内存探针
 * 目的: 判别 "视频预览内存上涨/关窗不降" 的层级 —
 *   宿主 C++ 侧释放链审计无泄漏 (XjsMediaFree 全字段 Release + delete), 本探针完全复刻
 *   xjs_media.cpp 的会话生命周期 (D3D11 设备 + DXGI 管理器 + MediaEngine 帧服务器
 *   + 装源 + TransferVideoFrame 回读 + Shutdown), 分四档实测内存曲线:
 *     模式A  仅 设备+引擎 建毁循环
 *     模式B  完整装载+取帧+卸载 建毁循环
 *     模式C  N 个会话并存(= 多窗各预览视频), 再逐个释放(= 逐窗关闭) — 看"关窗"回不回落
 *     模式D  单会话(同引擎)反复换源(= 同窗重复预览/换选中)
 *   判读: 私有提交单调上升 = 对应层级泄漏; 建毁/释放后回落到水位 = 平台正常复用。
 * 用法:
 *   test\test_media_leak.exe                — 模式A (默认 10 轮)
 *   test\test_media_leak.exe <视频> [轮数]  — 模式B
 *   test\test_media_leak.exe <视频> -c [N]  — 模式C (默认 8 会话)
 *   test\test_media_leak.exe <视频> -d [N]  — 模式D (默认 50 次换源)
 *   test\test_media_leak.exe <视频> -e [N]  — 模式E: 同 C 但纯软解 (无 D3D 设备) — 判别泄漏在驱动还是 mfplat
 *   test\test_media_leak.exe <视频> -f [N]  — 模式F: 同 C 但 N 引擎共享一个 D3D 设备 — 判别共享设备是否规避
 *   test\test_media_leak.exe <视频> -g [N]  — 模式G: 共享设备 + 卸载即 Shutdown 会话 (宿主修复方案预演)
 */
#include <windows.h>
#include <wincodecsdk.h>
#include <shlwapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <evr.h>
#include <mfmediaengine.h>
#include <d3d11_4.h>
#include <psapi.h>
#include <stdio.h>
#include <atomic>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "windowscodecs.lib")

enum { MEV_METADATA = 1, MEV_FRAMEDATA = 2, MEV_ERROR = 64 };

class ProbeNotify : public IMFMediaEngineNotify {
    std::atomic<unsigned long> m_refs{ 1 };
public:
    std::atomic<unsigned> flags{ 0 };
    HRESULT STDMETHODCALLTYPE EventNotify(DWORD ev, DWORD_PTR, DWORD) override {
        if (ev == MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA) flags.fetch_or(MEV_METADATA);
        else if (ev == MF_MEDIA_ENGINE_EVENT_LOADEDDATA || ev == MF_MEDIA_ENGINE_EVENT_FIRSTFRAMEREADY) flags.fetch_or(MEV_FRAMEDATA);
        else if (ev == MF_MEDIA_ENGINE_EVENT_ERROR) flags.fetch_or(MEV_ERROR);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IMFMediaEngineNotify) { *ppv = this; AddRef(); return S_OK; }
        *ppv = NULL; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = --m_refs; if (!r) delete this; return r; }
};

static void PrintMem(const char* tag) {
    PROCESS_MEMORY_COUNTERS_EX mc{};
    mc.cb = sizeof(mc);
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&mc, sizeof(mc));
    printf("[%s] workingset=%8.1f MB  privatecommit=%8.1f MB\n", tag,
           mc.WorkingSetSize / 1048576.0, mc.PrivateUsage / 1048576.0);
    fflush(stdout);
}

/* 一个会话 = 宿主每窗 media 的资源集合 (生命周期照 XjsMediaCtx) */
struct Session {
    ProbeNotify* ntf = NULL;
    IMFMediaEngine* eng = NULL;
    IMFMediaEngineEx* ex = NULL;
    ID3D11Device* d3d = NULL;
    ID3D11DeviceContext* dc = NULL;
    IMFDXGIDeviceManager* dxgi = NULL;
    bool soft = false;        /* 纯软解: 不建 D3D/DXGI (取帧走 WIC 位图目标, 同宿主软解路径) */
    bool shared = false;      /* d3d/dc/dxgi 借自共享 (SessionFree 不释放) */
    int round = 0;
};

/* 建会话 (设备+DXGI 管理器+引擎; teardown 由 SessionFree 做, 顺序照 XjsMediaFree)
   sharedFrom 非空 = 借它的 D3D 设备/DXGI 管理器 (不新建) */
static bool SessionCreate(Session* s, int round, const Session* sharedFrom = NULL) {
    s->round = round;
    if (sharedFrom) {
        s->shared = true;
        s->d3d = sharedFrom->d3d;
        s->dc = sharedFrom->dc;
        s->dxgi = sharedFrom->dxgi;
        s->ntf = new ProbeNotify();
    } else if (!s->soft) {
        UINT fl = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL got{};
        HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, fl, NULL, 0,
                                       D3D11_SDK_VERSION, &s->d3d, &got, &s->dc);
        if (FAILED(hr))
            hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, fl, NULL, 0,
                                   D3D11_SDK_VERSION, &s->d3d, &got, &s->dc);
        if (FAILED(hr)) { printf("round %d: D3D11CreateDevice failed 0x%08lX\n", round, (unsigned long)hr); return false; }
        ID3D11Multithread* mt = NULL;
        if (SUCCEEDED(s->dc->QueryInterface(&mt))) { mt->SetMultithreadProtected(TRUE); mt->Release(); }
        UINT token = 0;
        if (SUCCEEDED(MFCreateDXGIDeviceManager(&token, &s->dxgi))) s->dxgi->ResetDevice(s->d3d, token);
        s->ntf = new ProbeNotify();
    } else {
        s->ntf = new ProbeNotify();   /* 软解: 无 DXGI 管理器 */
    }
    IMFAttributes* at = NULL;
    if (FAILED(MFCreateAttributes(&at, 4))) return false;
    at->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, s->ntf);
    at->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, (UINT32)DXGI_FORMAT_B8G8R8A8_UNORM);
    if (s->dxgi) at->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, s->dxgi);
    IMFMediaEngineClassFactory* f = NULL;
    HRESULT hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    if (SUCCEEDED(hr)) { hr = f->CreateInstance(0, at, &s->eng); f->Release(); }
    at->Release();
    if (FAILED(hr) || !s->eng) { printf("round %d: CreateInstance failed 0x%08lX\n", round, (unsigned long)hr); return false; }
    s->eng->QueryInterface(&s->ex);
    s->eng->SetAutoPlay(FALSE);
    return true;
}

/* 装源 + 等元数据/首帧 + TransferVideoFrame 回读一次 (硬解纹理对路径, 同 MediaPullFrame) */
static void SessionLoadAndPull(Session* s, const wchar_t* path) {
    s->ntf->flags.store(0);
    IStream* st = NULL;
    bool loaded = false;
    if (SUCCEEDED(SHCreateStreamOnFileEx(path, STGM_READ | STGM_SHARE_DENY_NONE,
                                         FILE_ATTRIBUTE_NORMAL, FALSE, NULL, &st))) {
        IMFByteStream* bs = NULL;
        HRESULT hr;
        if (SUCCEEDED(MFCreateMFByteStreamOnStream(st, &bs))) {
            BSTR url = SysAllocString(path);
            hr = s->ex ? s->ex->SetSourceFromByteStream(bs, url) : E_NOINTERFACE;
            if (url) SysFreeString(url);
            bs->Release();
            loaded = SUCCEEDED(hr);
        }
        st->Release();
    }
    if (!loaded) { printf("round %d: open stream failed (tolerated)\n", s->round); return; }
    for (int i = 0; i < 750; i++) {   /* 15s 上限 */
        unsigned flg = s->ntf->flags.load();
        if (flg & (MEV_METADATA | MEV_FRAMEDATA | MEV_ERROR)) break;
        Sleep(20);
    }
    if ((s->ntf->flags.load() & (MEV_METADATA | MEV_ERROR)) == MEV_METADATA) {
        if (s->soft) {
            /* 软解: WIC 位图目标 + Lock 回读 (同宿主软解路径) */
            IWICImagingFactory* wic = NULL;
            if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
                IWICBitmap* bmp = NULL;
                if (SUCCEEDED(wic->CreateBitmap(640, 360, GUID_WICPixelFormat32bppBGRA, WICBitmapCacheOnDemand, &bmp)) && bmp) {
                    MFVideoNormalizedRect src = { 0, 0, 1, 1 };
                    RECT dst = { 0, 0, 640, 360 };
                    MFARGB border = { 0, 0, 0, 0 };
                    HRESULT hr = s->eng->TransferVideoFrame(bmp, &src, &dst, &border);
                    if (SUCCEEDED(hr)) {
                        IWICBitmapLock* lk = NULL;
                        WICRect full = { 0, 0, 640, 360 };
                        if (SUCCEEDED(bmp->Lock(&full, WICBitmapLockRead, &lk)) && lk) {
                            UINT stride = 0, sz = 0; BYTE* p = NULL;
                            if (SUCCEEDED(lk->GetStride(&stride)) && SUCCEEDED(lk->GetDataPointer(&sz, &p)) && p) {
                                std::vector<uint8_t> bits((size_t)640 * 4 * 360);
                                for (int y = 0; y < 360; y++) memcpy(&bits[(size_t)y * 640 * 4], p + (size_t)y * stride, (size_t)640 * 4);
                            }
                            lk->Release();
                        }
                    } else {
                        printf("round %d: soft TransferVideoFrame 0x%08lX (tolerated)\n", s->round, (unsigned long)hr);
                    }
                }
                if (bmp) bmp->Release();
                wic->Release();
            }
        } else {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = 640; td.Height = 360; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture2D* ftex = NULL; ID3D11Texture2D* stex = NULL; IDXGISurface* surf = NULL;
        if (SUCCEEDED(s->d3d->CreateTexture2D(&td, NULL, &ftex)) && ftex &&
            SUCCEEDED(ftex->QueryInterface(&surf)) && surf) {
            td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (SUCCEEDED(s->d3d->CreateTexture2D(&td, NULL, &stex)) && stex) {
                MFVideoNormalizedRect src = { 0, 0, 1, 1 };
                RECT dst = { 0, 0, 640, 360 };
                MFARGB border = { 0, 0, 0, 0 };
                HRESULT hr = s->eng->TransferVideoFrame(surf, &src, &dst, &border);
                if (SUCCEEDED(hr)) {
                    s->dc->CopyResource(stex, ftex);
                    D3D11_MAPPED_SUBRESOURCE m{};
                    if (SUCCEEDED(s->dc->Map(stex, 0, D3D11_MAP_READ, 0, &m))) s->dc->Unmap(stex, 0);
                } else {
                    printf("round %d: TransferVideoFrame 0x%08lX (tolerated)\n", s->round, (unsigned long)hr);
                }
            }
        }
        if (surf) surf->Release();
        if (stex) stex->Release();
        if (ftex) ftex->Release();
        }
    } else {
        printf("round %d: load timeout/error flg=%u (tolerated)\n", s->round, s->ntf->flags.load());
    }
}

/* 卸载源 (HTML src="" 语义, 同 MediaUnload); releaseEng = 停播即彻底 Shutdown 会话 (修复方案口径) */
static void SessionUnload(Session* s, bool releaseEng = false) {
    if (!s->eng) return;
    s->eng->Pause();
    BSTR empty = SysAllocStringLen(L"", 0);
    if (empty) { s->eng->SetSource(empty); SysFreeString(empty); }
    if (releaseEng) {
        s->eng->Shutdown();
        s->eng->Release();
        s->eng = NULL;
        if (s->ex) { s->ex->Release(); s->ex = NULL; }
    }
}

/* teardown — 顺序照 XjsMediaFree (共享设备不释放) */
static void SessionFree(Session* s) {
    SessionUnload(s);
    if (s->eng) s->eng->Shutdown();
    if (s->eng) s->eng->Release();
    if (s->ex) s->ex->Release();
    if (s->ntf) s->ntf->Release();
    if (s->dxgi && !s->shared) s->dxgi->Release();
    if (s->dc && !s->shared) s->dc->Release();
    if (s->d3d && !s->shared) s->d3d->Release();
    *s = Session{};
}

int wmain(int argc, wchar_t** argv) {
    const wchar_t* video = NULL;
    char mode = 'a';
    int rounds = 10;
    if (argc > 1 && argv[1][0]) video = argv[1];
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == L'-' && (argv[i][1] == L'c' || argv[i][1] == L'd' || argv[i][1] == L'e' ||
                                   argv[i][1] == L'f' || argv[i][1] == L'g' ||
                                   argv[i][1] == L'a' || argv[i][1] == L'b')) {
            mode = (char)argv[i][1];
            if (i + 1 < argc && argv[i + 1][0] != L'-') rounds = _wtoi(argv[i + 1]);
        } else if (i == 1) {
            /* 视频 */
        } else if (rounds == 10 && argv[i][0] != L'-') {
            rounds = _wtoi(argv[i]);
        }
    }
    if (rounds < 1) rounds = 10;
    if (mode != 'a' && !video) { printf("mode %c needs a video path\n", mode); return 1; }

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) { printf("MFStartup failed\n"); return 1; }
    printf("probe start: mode=%c rounds=%d\n", mode, rounds);
    PrintMem("base");

    if (mode == 'a' || mode == 'b') {
        for (int i = 1; i <= rounds; i++) {
            Session s{};
            if (!SessionCreate(&s, i)) break;
            if (mode == 'b') SessionLoadAndPull(&s, video);
            char tag[32];
            snprintf(tag, sizeof(tag), "cycle %2d", i);
            SessionFree(&s);
            PrintMem(tag);
        }
    } else if (mode == 'c' || mode == 'e' || mode == 'f') {
        /* N 会话并存 (= N 个窗口各预览视频) → 逐个释放 (= 逐窗关闭)
           e: 全部纯软解 (无 D3D 设备); f: 全部共享首个会话的 D3D 设备 */
        Session anchor{};   /* f: 共享设备锚 (只当设备用, 不装源不释放到 0 前的引擎) */
        if (mode == 'f') {
            if (!SessionCreate(&anchor, 0)) { printf("anchor failed\n"); return 1; }
            SessionUnload(&anchor);
        }
        std::vector<Session> ss((size_t)rounds);
        for (int i = 0; i < rounds; i++) {
            ss[(size_t)i].soft = (mode == 'e');
            if (!SessionCreate(&ss[(size_t)i], i + 1, mode == 'f' ? &anchor : NULL)) break;
            SessionLoadAndPull(&ss[(size_t)i], video);
            char tag[32];
            snprintf(tag, sizeof(tag), "open #%2d", i + 1);
            PrintMem(tag);
        }
        for (int i = 0; i < rounds; i++) {
            SessionFree(&ss[(size_t)i]);
            char tag[32];
            snprintf(tag, sizeof(tag), "close #%2d", i + 1);
            PrintMem(tag);
        }
        if (mode == 'f') { SessionFree(&anchor); }
        PrintMem("released");
    } else if (mode == 'g') {
        /* 宿主修复方案预演: 共享设备常驻 + 会话卸载即 Shutdown (开窗预览→关窗 循环) */
        Session anchor{};
        if (!SessionCreate(&anchor, 0)) { printf("anchor failed\n"); return 1; }
        SessionUnload(&anchor);
        PrintMem("device up");
        for (int i = 1; i <= rounds; i++) {
            Session s{};
            if (!SessionCreate(&s, i, &anchor)) break;
            SessionLoadAndPull(&s, video);
            SessionUnload(&s, /*releaseEng=*/true);   /* 停播/换选中/关窗: 会话彻底释放 */
            SessionFree(&s);                          /* ctx 拆毁 (共享设备不动) */
            char tag[32];
            snprintf(tag, sizeof(tag), "cycle %2d", i);
            PrintMem(tag);
        }
        SessionFree(&anchor);
        PrintMem("released");
    } else if (mode == 'd') {
        /* 单会话同引擎反复换源 (= 同窗重复预览/换选中; MediaSetFile 复用引擎口径) */
        Session s{};
        if (SessionCreate(&s, 1)) {
            for (int i = 1; i <= rounds; i++) {
                SessionUnload(&s);   /* 同 XjsPreviewUpdateSelection: 换目标先停播卸载 */
                SessionLoadAndPull(&s, video);
                char tag[32];
                snprintf(tag, sizeof(tag), "reload %2d", i);
                PrintMem(tag);
            }
            SessionFree(&s);
        }
    }

    MFShutdown();
    CoUninitialize();
    PrintMem("end");
    printf("probe end\n");
    return 0;
}
