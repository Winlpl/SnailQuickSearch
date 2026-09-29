/*
 * xjs_media.cpp — 预览面板 媒体预览 (视频/音频) 后端
 * 系统自带 Media Foundation Media Engine 帧服务器模式 (无窗口、无浏览器组件、按需加载):
 *   - 音频由引擎自己渲染; 视频帧经 OnVideoStreamTick + TransferVideoFrame 搬进 WIC 位图
 *     (引擎内做缩放与信箱黑边), Lock 后 CPU 拷贝成 BGRA 字节 — 绘制端与图片/插件交付同路
 *     (UI 线程零解码, 解码在引擎线程; 本线程只做 GPU→CPU 帧拷贝)
 *   - 本地文件经 IStream → IMFByteStream 装载 (中文路径零 URL 编码; SHARE_DENY_NONE 不锁文件,
 *     预览期间照常改名/删除); 无 DXGI 管理器时引擎自动软解, 硬解只是增益不是依赖
 *   - 会话 = XjsSearchWindow::media (每窗一份, 可维护性红线), 引擎对象懒建于首次媒体预览
 *   - 泵 = ID_TIMER_MEDIA (40ms): 装载期轮询就绪态 / 播放期取帧与进度 / 音量浮标收尾,
 *     全部空闲即摘表 (无媒体活动零计时器)
 * 绘制/命中/交互在 xjs_preview.cpp (媒体卡); 本文件只管引擎状态与帧搬运, 不画任何 UI。
 */
#include "xjs_app.h"
/* mfmediaengine.h 全部接口体门控在 WINVER>=0x0602 (Win8), 工程全局钉 0x0601 —
   本 TU 局部抬升 (媒体引擎最低 Win8, 再低的系统上 CoCreateInstance 失败=干净回落错误卡) */
#undef WINVER
#undef _WIN32_WINNT
#define WINVER 0x0602
#define _WIN32_WINNT 0x0602
#include <mfapi.h>
#include <mfidl.h>
#include <evr.h>
#include <mfmediaengine.h>
#include <d3d11_4.h>   /* ID3D11Multithread 在 d3d11_4.h (d3d11.h 无此接口) */
#include <shlwapi.h>
#include <oleauto.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "d3d11.lib")

/* 帧搬运目标尺寸上限 (面板显示矩形等比缩到此内; 控制每帧 GPU→CPU 回读量, 播放期 25fps) */
static const int XJS_MEDIA_FRAME_CAP_W = 1280;
static const int XJS_MEDIA_FRAME_CAP_H = 720;
/* 装载超时 (系统编解码器缓慢/盘阵迟滞的兜底护栏, 到点按失败呈现) */
static const unsigned long long XJS_MEDIA_LOAD_TIMEOUT_MS = 15000;

/* 事件位 (IMFMediaEngineNotify 回调在引擎线程, 只置原子标志; 消费在 UI 泵) */
enum {
    MEV_METADATA = 1 << 0,   /* LOADEDMETADATA: 时长/尺寸可知 */
    MEV_FRAMEDATA = 1 << 1,  /* LOADEDDATA / FIRSTFRAMEREADY: 可取帧 */
    MEV_PLAY = 1 << 2,
    MEV_PAUSE = 1 << 3,
    MEV_ENDED = 1 << 4,
    MEV_ERROR = 1 << 5,
};

/* 引擎事件收报器: 不持会话指针 (通知对象生命周期独立, 引擎析构竞态不悬垂) */
class XjsMediaNotify : public IMFMediaEngineNotify {
    std::atomic<unsigned long> m_refs{ 1 };
public:
    std::atomic<unsigned> flags{ 0 };
    std::atomic<unsigned> errCode{ 0 };   /* MF_MEDIA_ENGINE_ERR */
    std::atomic<unsigned> errHr{ 0 };     /* 扩展 HRESULT */

    HRESULT STDMETHODCALLTYPE EventNotify(DWORD ev, DWORD_PTR p1, DWORD p2) override {
        switch (ev) {
            case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA: flags.fetch_or(MEV_METADATA); break;
            case MF_MEDIA_ENGINE_EVENT_LOADEDDATA:
            case MF_MEDIA_ENGINE_EVENT_FIRSTFRAMEREADY: flags.fetch_or(MEV_FRAMEDATA); break;
            case MF_MEDIA_ENGINE_EVENT_PLAY:  flags.fetch_or(MEV_PLAY); break;
            case MF_MEDIA_ENGINE_EVENT_PAUSE: flags.fetch_or(MEV_PAUSE); break;
            case MF_MEDIA_ENGINE_EVENT_ENDED: flags.fetch_or(MEV_ENDED); break;
            case MF_MEDIA_ENGINE_EVENT_ERROR:
                errCode.store((unsigned)p1);
                errHr.store((unsigned)p2);
                flags.fetch_or(MEV_ERROR);
                break;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IMFMediaEngineNotify) { *ppv = this; AddRef(); return S_OK; }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --m_refs;
        if (!r) delete this;
        return r;
    }
};

struct XjsMediaCtx {
    explicit XjsMediaCtx(HWND h) : hwnd(h) {}
    HWND hwnd = NULL;
    XjsMediaNotify* notify = NULL;    /* 本方持一份引用 (引擎持另一份), Release 在引擎之后 */
    IMFMediaEngine* eng = NULL;
    IMFMediaEngineEx* engEx = NULL;   /* SetSourceFromByteStream */
    ID3D11Device* d3d = NULL;         /* 硬解设备 (建不成 = 引擎软解, 可省) */
    ID3D11DeviceContext* d3dc = NULL;
    IMFDXGIDeviceManager* dxgi = NULL;

    /* 装载/播放态 (UI 线程独占读写; 事件只写 notify 原子) */
    int fileId = -1;
    std::wstring path;
    double dur = 0, pos = 0;
    bool hasVideo = false;
    int vidW = 0, vidH = 0;
    bool loading = false;
    bool playing = false;
    bool ended = false;
    bool failed = false;
    unsigned long long loadTick = 0;
    bool wantPoster = false;          /* 需要补抓一帧 (首帧海报/暂停寻位后) */
    int posterTries = 0;              /* 补帧尝试次数 (放弃线 250 拍 ≈10s) */

    /* 帧搬运 (UI 线程)。
       目标面二选一, 由"引擎是否带 DXGI 管理器 (硬解)"决定 (2026-09-30 探针实锤):
       硬解引擎 → TransferVideoFrame 只认 DXGI surface (WIC 目标恒 E_NOINTERFACE, 静默不出图
       = "有声无画"根因), 走 目标纹理+staging 回读; 软解引擎 (D3D 建不成, 无 DXGI 管理器) →
       WIC 位图目标可用。两条路产出都是 BGRA 字节, 绘制端无感 */
    ID3D11Texture2D* frameTex = NULL;     /* 硬解: TransferVideoFrame 目标 (BIND_RENDER_TARGET) */
    ID3D11Texture2D* stageTex = NULL;     /* 硬解: staging 回读 (CPU_READ) */
    IDXGISurface* frameSurf = NULL;       /* frameTex 的 surface 视图 (引擎入参) */
    IWICBitmap* frameBmp = NULL;          /* 软解: WIC 位图目标 */
    bool wicPbgra = false;                /* WIC BGRA 被拒时的一次性格式回退 */
    int frameW = 0, frameH = 0;
    std::vector<uint8_t> bits;            /* BGRA 字节 (stride = frameW*4) */
    unsigned long long rev = 0;           /* 新帧序号 (绘制缓存失效依据) */
    XjsBitmap* cache = NULL;              /* bits → 本 RT 域位图缓存 (跨渲染域铁律) */
    XjsRt* cacheRt = NULL;
    unsigned long long cacheRev = 0;
    int dispW = 0, dispH = 0;             /* 最近一帧画面矩形尺寸 (绘制端回写, 取帧目标) */

    /* 音量/浮标 */
    float vol = 1.0f;
    bool mute = false;
    unsigned long long volBadgeTick = 0;
    unsigned long long lastScrubTick = 0;   /* 拖动寻位后留泵收尾 (补帧) */

    /* 失效去重快照 */
    unsigned long long invRev = (unsigned long long)-1;
    double invPos = -1;
    int invState = -1;
};

static bool s_mfUp = false;   /* MFStartup 一次 (进程级; 配对 MFShutdown 在主窗销毁) */

static void MediaDropFrameTarget(XjsMediaCtx* c);   /* 帧搬运节; 装载/卸载路径先用到 */

/* ==================== 引擎生命周期 ==================== */

static bool MediaEnsureEngine(XjsMediaCtx* c) {
    if (c->eng) return true;
    if (!s_mfUp) {
        if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return false;   /* 下次再试 */
        s_mfUp = true;
    }
    if (!c->notify) c->notify = new XjsMediaNotify();
    /* D3D11 设备 (VIDEO_SUPPORT 供硬解; 失败不阻断 — 无 DXGI 管理器 = 引擎软解) */
    if (!c->d3d) {
        UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL got{};
        if (SUCCEEDED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags, NULL, 0,
                                        D3D11_SDK_VERSION, &c->d3d, &got, &c->d3dc)) ||
            SUCCEEDED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, flags, NULL, 0,
                                        D3D11_SDK_VERSION, &c->d3d, &got, &c->d3dc))) {
            ID3D11Multithread* mt = NULL;   /* MF 与本线程共用立即上下文, 多线程保护必开 */
            if (SUCCEEDED(c->d3dc->QueryInterface(&mt))) { mt->SetMultithreadProtected(TRUE); mt->Release(); }
            UINT token = 0;
            if (SUCCEEDED(MFCreateDXGIDeviceManager(&token, &c->dxgi)))
                c->dxgi->ResetDevice(c->d3d, token);
        } else {
            c->d3d = NULL;
            c->d3dc = NULL;
        }
    }
    IMFAttributes* at = NULL;
    if (FAILED(MFCreateAttributes(&at, 4))) return false;
    at->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, c->notify);
    at->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, (UINT32)DXGI_FORMAT_B8G8R8A8_UNORM);
    if (c->dxgi) at->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, c->dxgi);
    IMFMediaEngineClassFactory* f = NULL;
    HRESULT hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, NULL, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&f));
    if (SUCCEEDED(hr)) {
        hr = f->CreateInstance(0, at, &c->eng);   /* flags=0 = 帧服务器模式 (默认; 不设
                                                     PLAYBACK_HWND/PLAYBACK_VISUAL 即无窗口) */
        f->Release();
    }
    at->Release();
    if (FAILED(hr)) return false;
    c->eng->QueryInterface(&c->engEx);
    c->eng->SetAutoPlay(FALSE);   /* 播放一律用户显式触发 (选中原地出声违背直觉) */
    return true;
}

/* 装载媒体文件 (换源): 旧源先停声, 流打不开/引擎建不起 = failed 呈现 */
static void MediaLoadFile(XjsMediaCtx* c, int fileId, const std::wstring& path) {
    if (!MediaEnsureEngine(c)) { c->fileId = fileId; c->path = path; c->failed = true; return; }
    c->fileId = fileId;
    c->path = path;
    c->dur = c->pos = 0;
    c->hasVideo = false;
    c->vidW = c->vidH = 0;
    c->playing = c->ended = c->failed = false;
    c->loading = true;
    c->loadTick = GetTickCount64();
    c->wantPoster = false;
    c->posterTries = 0;
    if (c->notify) { c->notify->flags.store(0); c->notify->errCode.store(0); c->notify->errHr.store(0); }
    /* 换源清旧帧: 上一部影片的画面不得垫显 */
    c->bits.clear();
    c->rev++;
    MediaDropFrameTarget(c);
    c->eng->SetVolume(c->vol);
    c->eng->SetMuted(c->mute ? TRUE : FALSE);
    c->eng->Pause();
    HRESULT hr = E_FAIL;
    IStream* st = NULL;
    if (SUCCEEDED(SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_NONE,
                                         FILE_ATTRIBUTE_NORMAL, FALSE, NULL, &st))) {
        IMFByteStream* bs = NULL;
        hr = MFCreateMFByteStreamOnStream(st, &bs);   /* 中文路径零 URL 编码的唯一可靠通道 */
        st->Release();
        if (SUCCEEDED(hr)) {
            BSTR url = SysAllocString(path.c_str());
            hr = c->engEx ? c->engEx->SetSourceFromByteStream(bs, url) : E_NOINTERFACE;
            if (url) SysFreeString(url);
            bs->Release();
        }
    } else {
        hr = HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION);   /* 打不开按错误呈现 (被独占等) */
    }
    if (FAILED(hr)) {
        c->loading = false;
        c->failed = true;
    }
}

/* 停播并卸载源 (HTML src="" 语义; 卸载失败无害 — 状态标志已清, 流不带独占锁) */
static void MediaUnload(XjsMediaCtx* c) {
    if (c->eng) {
        c->eng->Pause();
        BSTR empty = SysAllocStringLen(L"", 0);
        if (empty) {
            c->eng->SetSource(empty);
            SysFreeString(empty);
        }
    }
    c->fileId = -1;
    c->path.clear();
    c->loading = c->playing = c->ended = c->failed = false;
    c->dur = c->pos = 0;
    c->hasVideo = false;
    c->wantPoster = false;
    c->bits.clear();
    c->rev++;
    MediaDropFrameTarget(c);
    if (c->hwnd) KillTimer(c->hwnd, ID_TIMER_MEDIA);
}

/* 只失效预览区 (同图片装载/插件交付口径; 布局为空 = 整窗兜底)。
   全屏层例外: 它铺满整窗 (控制条伸进状态栏带), 局部矩形会漏刷 → 整窗失效 */
static void MediaInvalidate(XjsSearchWindow* w) {
    if (w->mediaFull) { w->Invalidate(); return; }
    if (w->hWnd) {
        XjsRect b = w->layout.preview;
        RECT r = { (int)b.left, (int)b.top, (int)b.right, (int)b.bottom };
        if (r.right > r.left && r.bottom > r.top) { InvalidateRect(w->hWnd, &r, FALSE); return; }
    }
    w->Invalidate();
}

/* ==================== 帧搬运 ==================== */

/* 释放帧目标面 (换源/尺寸变化/析构; frameW/H 一并归零 = 下次重建) */
static void MediaDropFrameTarget(XjsMediaCtx* c) {
    if (c->frameSurf) { c->frameSurf->Release(); c->frameSurf = NULL; }
    if (c->frameTex) { c->frameTex->Release(); c->frameTex = NULL; }
    if (c->stageTex) { c->stageTex->Release(); c->stageTex = NULL; }
    if (c->frameBmp) { c->frameBmp->Release(); c->frameBmp = NULL; }
    c->frameW = c->frameH = 0;
}

/* 确保帧目标面存在且匹配显示尺寸 (±3px 滞回, 拖宽面板不逐帧重建):
   硬解引擎 = D3D 纹理对 (目标 BIND_RENDER_TARGET + staging CPU_READ) + surface 视图;
   软解引擎 (无 DXGI 管理器) = WIC 位图。硬解引擎建不出纹理 = 无备胎 (WIC 对它恒拒绝),
   返回假等下拍重试 */
static bool MediaEnsureFrameTarget(XjsMediaCtx* c, int w, int h) {
    bool sized = c->frameW > 0 && abs(c->frameW - w) <= 3 && abs(c->frameH - h) <= 3;
    if (sized && (c->frameSurf || c->frameBmp)) return true;
    MediaDropFrameTarget(c);
    if (c->dxgi && c->d3d && c->d3dc) {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = (UINT)w;
        d.Height = (UINT)h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(c->d3d->CreateTexture2D(&d, NULL, &c->frameTex))) return false;
        if (FAILED(c->frameTex->QueryInterface(&c->frameSurf))) { MediaDropFrameTarget(c); return false; }
        d.BindFlags = 0;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(c->d3d->CreateTexture2D(&d, NULL, &c->stageTex))) { MediaDropFrameTarget(c); return false; }
        c->frameW = w;
        c->frameH = h;
        return true;
    }
    /* 软解引擎: WIC 位图目标 (2026-09-30 探针 case B 实锤可用) */
    if (!g_wic) return false;
    const GUID& fmt = c->wicPbgra ? GUID_WICPixelFormat32bppPBGRA : GUID_WICPixelFormat32bppBGRA;
    if (FAILED(g_wic->CreateBitmap((UINT)w, (UINT)h, fmt, WICBitmapCacheOnDemand, &c->frameBmp)))
        return false;
    c->frameW = w;
    c->frameH = h;
    return true;
}

/* 取当前帧 → bits (引擎把帧缩放到目标矩形并填信箱边; 边透明, 绘制端看不到) */
static bool MediaPullFrame(XjsMediaCtx* c) {
    int w = c->dispW, h = c->dispH;
    if (!c->eng || !c->hasVideo || w <= 0 || h <= 0) return false;
    if (w > XJS_MEDIA_FRAME_CAP_W) { h = ximax(1, h * XJS_MEDIA_FRAME_CAP_W / w); w = XJS_MEDIA_FRAME_CAP_W; }
    if (h > XJS_MEDIA_FRAME_CAP_H) { w = ximax(1, w * XJS_MEDIA_FRAME_CAP_H / h); h = XJS_MEDIA_FRAME_CAP_H; }
    if (!MediaEnsureFrameTarget(c, w, h)) return false;
    MFVideoNormalizedRect src = { 0, 0, 1, 1 };
    RECT dst = { 0, 0, c->frameW, c->frameH };
    MFARGB border = { 0, 0, 0, 0 };
    if (c->frameSurf) {
        /* 硬解: GPU 内缩放进目标纹理 → staging 回读 (同 UI 线程; 设备已开多线程保护) */
        if (FAILED(c->eng->TransferVideoFrame(c->frameSurf, &src, &dst, &border))) return false;
        c->d3dc->CopyResource(c->stageTex, c->frameTex);
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (FAILED(c->d3dc->Map(c->stageTex, 0, D3D11_MAP_READ, 0, &m))) return false;
        bool ok = false;
        if (m.pData && m.RowPitch >= (UINT)c->frameW * 4) {
            c->bits.resize((size_t)c->frameW * 4 * c->frameH);
            const UINT rowBytes = (UINT)c->frameW * 4;
            for (int y = 0; y < c->frameH; y++)
                memcpy(&c->bits[(size_t)y * rowBytes], (const BYTE*)m.pData + (size_t)y * m.RowPitch, rowBytes);
            c->rev++;
            ok = true;
        }
        c->d3dc->Unmap(c->stageTex, 0);
        return ok;
    }
    if (!c->frameBmp) return false;
    /* 软解: WIC 位图目标 */
    HRESULT hr = c->eng->TransferVideoFrame(c->frameBmp, &src, &dst, &border);
    if (FAILED(hr) && !c->wicPbgra) {   /* 一次性格式回退 (个别系统对 BGRA 目标挑剔) */
        c->wicPbgra = true;
        MediaDropFrameTarget(c);
        if (!MediaEnsureFrameTarget(c, w, h)) return false;
        dst = { 0, 0, c->frameW, c->frameH };
        hr = c->eng->TransferVideoFrame(c->frameBmp, &src, &dst, &border);
    }
    if (FAILED(hr)) return false;
    IWICBitmapLock* lk = NULL;
    WICRect full = { 0, 0, c->frameW, c->frameH };
    if (FAILED(c->frameBmp->Lock(&full, WICBitmapLockRead, &lk))) return false;
    bool ok = false;
    UINT stride = 0;
    BYTE* p = NULL;
    UINT sz = 0;
    if (SUCCEEDED(lk->GetStride(&stride)) && SUCCEEDED(lk->GetDataPointer(&sz, &p)) && p && stride >= (UINT)c->frameW * 4) {
        c->bits.resize((size_t)c->frameW * 4 * c->frameH);
        const UINT rowBytes = (UINT)c->frameW * 4;
        for (int y = 0; y < c->frameH; y++)
            memcpy(&c->bits[(size_t)y * rowBytes], p + (size_t)y * stride, rowBytes);
        c->rev++;
        ok = true;
    }
    lk->Release();
    return ok;
}

/* ==================== UI 泵 ==================== */

static int MediaStateCode(XjsMediaCtx* c) {
    return (c->loading ? 1 : 0) | (c->playing ? 2 : 0) | (c->failed ? 4 : 0) |
           (c->ended ? 8 : 0) | (c->hasVideo ? 16 : 0);
}

void XjsSearchWindow::MediaTick() {
    XjsMediaCtx* c = media;
    if (!c || c->fileId < 0 || !c->eng) {
        if (hWnd) KillTimer(hWnd, ID_TIMER_MEDIA);
        return;
    }
    unsigned f = c->notify ? c->notify->flags.exchange(0) : 0;
    bool invalidate = false;
    /* 装载期: 等元数据 → 记时长/尺寸; 超时或引擎报错 = failed */
    if (c->loading) {
        invalidate = true;   /* 装载期逐拍失效 (载入指示动起来) */
        if (f & MEV_ERROR) {
            c->loading = false;
            c->failed = true;
        } else {
            USHORT rs = c->eng->GetReadyState();
            if ((f & MEV_METADATA) || rs >= (USHORT)MF_MEDIA_ENGINE_READY_HAVE_METADATA) {
                DWORD dw2 = 0, dh2 = 0;
                c->eng->GetNativeVideoSize(&dw2, &dh2);
                c->vidW = (int)dw2;
                c->vidH = (int)dh2;
                c->hasVideo = c->eng->HasVideo() && c->vidW > 0 && c->vidH > 0;
                double d = c->eng->GetDuration();
                if (d == d && d > 0) c->dur = d;
                if (rs >= (USHORT)MF_MEDIA_ENGINE_READY_HAVE_CURRENT_DATA) {
                    c->loading = false;
                    c->wantPoster = c->hasVideo;   /* 首帧海报 */
                }
            } else if (GetTickCount64() - c->loadTick > XJS_MEDIA_LOAD_TIMEOUT_MS) {
                c->loading = false;
                c->failed = true;
            }
        }
    }
    if (!c->loading) {
        if (f & MEV_ERROR) { c->failed = true; c->playing = false; }
        if (f & MEV_ENDED) { c->playing = false; c->ended = true; }
        if (f & MEV_PLAY)  { c->playing = true; c->ended = false; }
        if (f & MEV_PAUSE) c->playing = false;
        /* 进度/时长缓存 (绘制直接读) */
        double p = c->eng->GetCurrentTime();
        c->pos = (p == p && p > 0) ? p : 0;
        double d = c->eng->GetDuration();
        if (d == d && d > 0) c->dur = d;
        /* 帧推进: 播放中每拍取一帧; 暂停态只补抓 wantPoster (首帧/寻位后/面板改尺寸后) */
        if (c->hasVideo && !c->playing && c->dispW > 0 &&
            (abs(c->frameW - c->dispW) > 3 || abs(c->frameH - c->dispH) > 3))
            c->wantPoster = true;
        bool pulled = false;
        if (c->hasVideo) {
            LONGLONG pts = 0;
            HRESULT hr = c->eng->OnVideoStreamTick(&pts);
            if (hr == S_OK) pulled = MediaPullFrame(c);
            if (!pulled && c->wantPoster && c->eng->GetReadyState() >= (USHORT)MF_MEDIA_ENGINE_READY_HAVE_CURRENT_DATA &&
                c->posterTries < 250) {
                c->posterTries++;
                pulled = MediaPullFrame(c);
                if (pulled) c->wantPoster = false;
            } else if (pulled) {
                c->wantPoster = false;
                c->posterTries = 0;
            }
        }
        if (c->rev != c->invRev) invalidate = true;
        else if ((int)(c->pos * 10) != (int)(c->invPos * 10)) invalidate = true;   /* 进度按 0.1s 量化 (音频播放不逐帧重绘) */
    }
    /* 计时器自管理: 有活动才续, 全空闲摘表 */
    ULONGLONG now = GetTickCount64();
    bool need = c->loading || c->playing || c->wantPoster ||
                now < c->volBadgeTick || now - c->lastScrubTick < 600;
    if (need) {
        if (hWnd) SetTimer(hWnd, ID_TIMER_MEDIA, 40, NULL);
    } else if (hWnd) {
        KillTimer(hWnd, ID_TIMER_MEDIA);
    }
    /* 失效去重: 状态/进度/新帧变了才重绘 (暂停态静止不烧 CPU) */
    int st = MediaStateCode(c);
    if (invalidate || (int)(c->pos * 10) != (int)(c->invPos * 10) || st != c->invState) {
        c->invRev = c->rev;
        c->invPos = c->pos;
        c->invState = st;
        MediaInvalidate(this);
    }
}

/* ==================== 窗口级操作 ==================== */

void XjsSearchWindow::MediaSetFile(int fileId, const std::wstring& path) {
    if (media && media->fileId == fileId && !media->failed &&
        (media->loading || media->eng) && media->path == path)
        return;   /* 同文件 (扫描刷新/程序性重选): 不重载不打断播放 */
    if (!media) {
        media = XjsMediaCreate(hWnd);
        if (!media) return;
    }
    media->vol = mediaVol;
    media->mute = mediaMute;
    MediaLoadFile(media, fileId, path);
    if (hWnd) SetTimer(hWnd, ID_TIMER_MEDIA, 40, NULL);   /* 装载期泵 */
    media->invState = -1;
    MediaInvalidate(this);
}

void XjsSearchWindow::MediaTogglePlay() {
    if (!media || !media->eng || media->fileId < 0) return;
    if (media->playing) {
        media->eng->Pause();
        media->playing = false;
    } else {
        /* 单声道原则: 开始播放即暂停其它窗口的媒体 (引擎各自独立, 不处理会叠音) */
        XjsSearchWindow::ForEach([](XjsSearchWindow* o) {
            if (o && o->media && o->media->playing && o->media->eng) {
                o->media->eng->Pause();
                o->media->playing = false;
            }
        });
        if (media->ended) { media->eng->SetCurrentTime(0); media->ended = false; }
        media->eng->Play();
        media->playing = true;   /* 本地立即置 (EV_PLAY 仅校准) */
    }
    media->invState = -1;
    if (hWnd) SetTimer(hWnd, ID_TIMER_MEDIA, 40, NULL);
    MediaInvalidate(this);
}

void XjsSearchWindow::MediaToggleMute() {
    mediaMute = !mediaMute;
    if (media) {
        media->mute = mediaMute;
        if (media->eng) media->eng->SetMuted(mediaMute ? TRUE : FALSE);
        media->volBadgeTick = GetTickCount64() + 1200;
        media->invState = -1;
    }
    XjsSaveConfig();
    if (hWnd) SetTimer(hWnd, ID_TIMER_MEDIA, 40, NULL);   /* 浮标收尾 */
    MediaInvalidate(this);
}

void XjsSearchWindow::MediaAdjustVolume(int dir) {
    mediaVol += (dir > 0 ? 0.05f : -0.05f);
    if (mediaVol > 1.0f) mediaVol = 1.0f;
    if (mediaVol < 0.0f) mediaVol = 0.0f;
    if (mediaVol > 0 && mediaMute) {   /* 有声滚动顺手解除静音 (调节即在乎声音) */
        mediaMute = false;
        if (media && media->eng) media->eng->SetMuted(FALSE);
    }
    if (media) {
        media->vol = mediaVol;
        media->mute = mediaMute;
        if (media->eng) media->eng->SetVolume(mediaVol);
        media->volBadgeTick = GetTickCount64() + 1200;
    }
    XjsSaveConfig();
    if (hWnd) SetTimer(hWnd, ID_TIMER_MEDIA, 40, NULL);
    MediaInvalidate(this);
}

void XjsSearchWindow::MediaSeekFrac(double frac) {
    if (!media || !media->eng || media->fileId < 0 || media->dur <= 0) return;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    media->pos = frac * media->dur;   /* 立即回显 (引擎异步行进) */
    media->eng->SetCurrentTime(media->pos);
    media->ended = false;
    if (media->hasVideo) media->wantPoster = true;   /* 暂停态寻位后补帧 */
    media->lastScrubTick = GetTickCount64();
    media->invPos = -1;
    if (hWnd) SetTimer(hWnd, ID_TIMER_MEDIA, 40, NULL);
    MediaInvalidate(this);
}

void XjsSearchWindow::MediaStop() {
    if (!media) { mediaFull = false; return; }
    MediaUnload(media);
    mediaFull = false;   /* 全屏层随停播收起 (绘制/输入入口按本字段短路) */
    media->invState = -1;
    MediaInvalidate(this);
}

/* ==================== 绘制取用 ==================== */

XjsMediaCtx* XjsMediaCreate(HWND hwnd) {
    return new XjsMediaCtx(hwnd);
}

void XjsMediaFree(XjsMediaCtx* c) {
    if (!c) return;
    MediaDropFrameTarget(c);   /* 帧目标面 (DXGI 纹理对/surface/WIC 位图) 先于设备释放 */
    if (c->eng) c->eng->Shutdown();   /* 先停引擎 (断掉全部回调线程) 再释放 */
    if (c->eng) c->eng->Release();
    if (c->engEx) c->engEx->Release();
    if (c->notify) c->notify->Release();
    if (c->dxgi) c->dxgi->Release();
    if (c->d3dc) c->d3dc->Release();
    if (c->d3d) c->d3d->Release();
    if (c->cache) c->cache->Release();
    delete c;
}

void XjsMediaGlobalShutdown() {
    if (s_mfUp) {
        MFShutdown();
        s_mfUp = false;
    }
}

XjsBitmap* XjsMediaFrameBitmap(XjsMediaCtx* c) {
    if (!c || c->bits.empty() || c->frameW <= 0 || c->frameH <= 0) return NULL;
    if (!c->cache || c->cacheRt != g_rt || c->cacheRev != c->rev) {
        if (c->cache) { c->cache->Release(); c->cache = NULL; }
        c->cache = XjsBitmapFromBgra(c->bits.data(), c->frameW, c->frameH, c->frameW * 4);
        c->cacheRt = g_rt;
        c->cacheRev = c->rev;
    }
    return c->cache;
}

bool XjsMediaHasVideo(XjsMediaCtx* c) { return c && c->hasVideo; }
bool XjsMediaActive(XjsMediaCtx* c) { return c && c->fileId >= 0; }   /* 会话装载着文件 (含装载中) */
bool XjsMediaIsTarget(XjsMediaCtx* c, int fileId) { return c && c->fileId == fileId; }   /* 会话是否装载着该文件 */
void XjsMediaSetFrameTarget(XjsMediaCtx* c, int w, int h) {   /* 绘制端回写画面矩形 (UI 泵取帧目标尺寸) */
    if (!c) return;
    c->dispW = ximax(1, w);
    c->dispH = ximax(1, h);
}
int XjsMediaVidW(XjsMediaCtx* c) { return c ? c->vidW : 0; }
int XjsMediaVidH(XjsMediaCtx* c) { return c ? c->vidH : 0; }
bool XjsMediaLoading(XjsMediaCtx* c) { return c && c->loading; }
bool XjsMediaFailed(XjsMediaCtx* c) { return c && c->failed; }
bool XjsMediaPlaying(XjsMediaCtx* c) { return c && c->playing; }
bool XjsMediaEnded(XjsMediaCtx* c) { return c && c->ended; }
double XjsMediaPos(XjsMediaCtx* c) { return c ? c->pos : 0; }
double XjsMediaDur(XjsMediaCtx* c) { return c ? c->dur : 0; }
float XjsMediaVolume(XjsMediaCtx* c) { return c ? c->vol : 1.0f; }
bool XjsMediaMuted(XjsMediaCtx* c) { return c ? c->mute : false; }
float XjsMediaVolumeBadge(XjsMediaCtx* c) {
    if (!c || GetTickCount64() >= c->volBadgeTick) return -1.0f;
    return c->mute ? 0.0f : c->vol;
}
