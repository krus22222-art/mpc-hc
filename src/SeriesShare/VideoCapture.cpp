#include "VideoCapture.h"

#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <codecapi.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <libyuv.h>

#include <algorithm>
#include <chrono>
#include <cstring>

using Microsoft::WRL::ComPtr;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;

struct VideoCapture::Impl {
    // capture
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::mutex contextMutex;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrtDevice{nullptr};
    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    winrt::Windows::Graphics::SizeInt32 poolSize{};
    ComPtr<ID3D11Texture2D> staging;
    UINT stagingW = 0, stagingH = 0;
    std::vector<uint8_t> scaled;

    // encoder
    ComPtr<IMFTransform> mft;
    ComPtr<ICodecAPI> codec;
    int encW = 0, encH = 0;
    DWORD outBufferSize = 0;
    bool mftProvidesSamples = false;
    std::vector<uint8_t> sequenceHeader;
    std::vector<uint8_t> out;
};

namespace {

bool StartsWithSps(const uint8_t* p, size_t n)
{
    for (size_t i = 0; i + 4 < n && i < 64; i++) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            return (p[i + 3] & 0x1F) == 7;
        }
    }
    return false;
}

void SetCodecUInt(ICodecAPI* api, const GUID& key, ULONG value)
{
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = value;
    api->SetValue(&key, &v);
}

}  // namespace

VideoCapture::VideoCapture() : m_impl(std::make_unique<Impl>())
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    m_qpcFreq = f.QuadPart;
}

VideoCapture::~VideoCapture()
{
    Stop();
}

bool VideoCapture::Start(HWND window, HWND videoArea, const VideoSettings& settings, int64_t clockStartQpc, OnEncoded onEncoded, std::wstring& error)
{
    m_window = window;
    m_videoArea = videoArea;
    m_settings = settings;
    m_clockStartQpc = clockStartQpc;
    m_onEncoded = std::move(onEncoded);

    if (!wgc::GraphicsCaptureSession::IsSupported()) {
        error = L"Ця версія Windows не підтримує захоплення вікна (потрібна Windows 10 1903 або новіша).";
        return false;
    }

    auto& d = *m_impl;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &d.device, nullptr, &d.context))) {
        error = L"Не вдалося ініціалізувати Direct3D 11.";
        return false;
    }
    ComPtr<IDXGIDevice> dxgi;
    d.device.As(&dxgi);
    winrt::com_ptr<::IInspectable> inspectable;
    if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()))) {
        error = L"Не вдалося створити пристрій для захоплення.";
        return false;
    }
    d.winrtDevice = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

    try {
        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(interop->CreateForWindow(window, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(d.item)));
        d.poolSize = d.item.Size();
        d.pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(d.winrtDevice, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, d.poolSize);
        d.pool.FrameArrived([this](auto&&, auto&&) { OnFrame(); });
        d.item.Closed([this](auto&&, auto&&) { m_windowClosed = true; });
        d.session = d.pool.CreateCaptureSession(d.item);
        try { d.session.IsCursorCaptureEnabled(false); } catch (...) {}
        try { d.session.IsBorderRequired(false); } catch (...) {}  // Windows 11: no yellow border
        d.session.StartCapture();
    } catch (const winrt::hresult_error& e) {
        error = L"Не вдалося почати захоплення вікна плеєра: " + std::wstring(e.message());
        return false;
    }

    m_running = true;
    m_encoderThread = std::thread([this] { EncoderLoop(); });
    return true;
}

void VideoCapture::Stop()
{
    if (!m_running.exchange(false)) {
        return;
    }
    m_cv.notify_all();
    if (m_encoderThread.joinable()) {
        m_encoderThread.join();
    }
    auto& d = *m_impl;
    try {
        if (d.session) d.session.Close();
        if (d.pool) d.pool.Close();
    } catch (...) {
    }
    d.session = nullptr;
    d.pool = nullptr;
    d.item = nullptr;
}

void VideoCapture::OnFrame()
{
    auto& d = *m_impl;
    std::lock_guard<std::mutex> lock(d.contextMutex);
    wgc::Direct3D11CaptureFrame frame{nullptr};
    try {
        frame = d.pool.TryGetNextFrame();
    } catch (...) {
        return;
    }
    if (!frame) {
        return;
    }

    const auto contentSize = frame.ContentSize();
    if (contentSize.Width != d.poolSize.Width || contentSize.Height != d.poolSize.Height) {
        d.poolSize = contentSize;
        try {
            d.pool.Recreate(d.winrtDevice, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, contentSize);
        } catch (...) {
        }
    }

    auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(access->GetInterface(IID_PPV_ARGS(&tex)))) {
        return;
    }
    D3D11_TEXTURE2D_DESC desc;
    tex->GetDesc(&desc);
    const LONG texW = std::min<LONG>(desc.Width, contentSize.Width);
    const LONG texH = std::min<LONG>(desc.Height, contentSize.Height);

    // Crop to the player's video area (no menus, buttons or playlist in the stream).
    RECT crop{0, 0, texW, texH};
    if (m_videoArea && IsWindow(m_videoArea) && IsWindowVisible(m_videoArea)) {
        RECT frameBounds{}, view{};
        if (SUCCEEDED(DwmGetWindowAttribute(m_window, DWMWA_EXTENDED_FRAME_BOUNDS, &frameBounds, sizeof(frameBounds))) && GetWindowRect(m_videoArea, &view)) {
            RECT c{view.left - frameBounds.left, view.top - frameBounds.top, view.right - frameBounds.left, view.bottom - frameBounds.top};
            c.left = std::clamp<LONG>(c.left, 0, texW);
            c.top = std::clamp<LONG>(c.top, 0, texH);
            c.right = std::clamp<LONG>(c.right, 0, texW);
            c.bottom = std::clamp<LONG>(c.bottom, 0, texH);
            if (c.right - c.left >= 64 && c.bottom - c.top >= 64) {
                crop = c;
            }
        }
    }
    const int cw = (crop.right - crop.left) & ~1;
    const int ch = (crop.bottom - crop.top) & ~1;
    if (cw < 16 || ch < 16) {
        return;
    }

    if (!d.staging || d.stagingW < UINT(cw) || d.stagingH < UINT(ch)) {
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = std::max<UINT>(cw, d.stagingW);
        sd.Height = std::max<UINT>(ch, d.stagingH);
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d.staging.Reset();
        if (FAILED(d.device->CreateTexture2D(&sd, nullptr, &d.staging))) {
            return;
        }
        d.stagingW = sd.Width;
        d.stagingH = sd.Height;
    }
    D3D11_BOX box{UINT(crop.left), UINT(crop.top), 0, UINT(crop.left + cw), UINT(crop.top + ch), 1};
    d.context->CopySubresourceRegion(d.staging.Get(), 0, 0, 0, 0, tex.Get(), 0, &box);

    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(d.context->Map(d.staging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
        return;
    }

    // Target size: limit height, keep aspect ratio, even dimensions.
    int th = std::min(ch, m_settings.maxHeight) & ~1;
    int tw = int(double(cw) * th / ch + 0.5) & ~1;
    tw = std::max(tw, 16);
    th = std::max(th, 16);

    const uint8_t* src = static_cast<const uint8_t*>(map.pData);
    int srcStride = int(map.RowPitch);
    if (tw != cw || th != ch) {
        d.scaled.resize(size_t(tw) * th * 4);
        libyuv::ARGBScale(src, srcStride, cw, ch, d.scaled.data(), tw * 4, tw, th, libyuv::kFilterLinear);
        src = d.scaled.data();
        srcStride = tw * 4;
    }

    {
        std::lock_guard<std::mutex> l(m_mutex);
        m_pending.resize(size_t(tw) * th * 3 / 2);
        uint8_t* y = m_pending.data();
        uint8_t* uv = y + size_t(tw) * th;
        libyuv::ARGBToNV12(src, srcStride, y, tw, uv, tw, tw, th);
        m_pendingW = tw;
        m_pendingH = th;
        const int64_t start100ns = int64_t(double(m_clockStartQpc) * 1e7 / double(m_qpcFreq));
        m_pendingTime = double(frame.SystemRelativeTime().count() - start100ns) / 1e7;
        m_hasPending = true;
    }
    d.context->Unmap(d.staging.Get(), 0);
    m_cv.notify_one();
}

void VideoCapture::EncoderLoop()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    auto& d = *m_impl;
    std::vector<uint8_t> frame;
    int fw = 0, fh = 0;
    double ft = 0;
    auto lastEncode = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    const auto interval = std::chrono::microseconds(1000000 / std::max(1, m_settings.fps));
    int framesThisSecond = 0;
    auto fpsWindow = std::chrono::steady_clock::now();

    auto qpcSeconds = [this] {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return double(now.QuadPart - m_clockStartQpc) / double(m_qpcFreq);
    };

    auto createEncoder = [&](int w, int h) -> bool {
        d.mft.Reset();
        d.codec.Reset();
        d.sequenceHeader.clear();
        if (FAILED(CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d.mft)))) {
            return false;
        }
        if (SUCCEEDED(d.mft.As(&d.codec))) {
            SetCodecUInt(d.codec.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
            SetCodecUInt(d.codec.Get(), CODECAPI_AVEncCommonMeanBitRate, ULONG(m_settings.bitrateKbps) * 1000);
            SetCodecUInt(d.codec.Get(), CODECAPI_AVEncMPVGOPSize, ULONG(m_settings.fps) * 4);
            SetCodecUInt(d.codec.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
            SetCodecUInt(d.codec.Get(), CODECAPI_AVLowLatencyMode, TRUE);
            SetCodecUInt(d.codec.Get(), CODECAPI_AVEncCommonQualityVsSpeed, 0);  // favour speed: real-time at 1080p
            SetCodecUInt(d.codec.Get(), CODECAPI_AVEncNumWorkerThreads, 0);      // 0 = automatic (all cores)
        }
        ComPtr<IMFMediaType> outType, inType;
        MFCreateMediaType(&outType);
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        outType->SetUINT32(MF_MT_AVG_BITRATE, UINT32(m_settings.bitrateKbps) * 1000);
        MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, m_settings.fps, 1);
        MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
        if (FAILED(d.mft->SetOutputType(0, outType.Get(), 0))) {
            return false;
        }
        MFCreateMediaType(&inType);
        inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, m_settings.fps, 1);
        MFSetAttributeRatio(inType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (FAILED(d.mft->SetInputType(0, inType.Get(), 0))) {
            return false;
        }
        MFT_OUTPUT_STREAM_INFO info{};
        d.mft->GetOutputStreamInfo(0, &info);
        d.outBufferSize = std::max<DWORD>(info.cbSize, DWORD(w) * h * 3 / 2);
        d.mftProvidesSamples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        d.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        d.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        ComPtr<IMFMediaType> current;
        if (SUCCEEDED(d.mft->GetOutputCurrentType(0, &current))) {
            UINT32 size = 0;
            if (SUCCEEDED(current->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size) {
                d.sequenceHeader.resize(size);
                current->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, d.sequenceHeader.data(), size, nullptr);
            }
        }
        d.encW = w;
        d.encH = h;
        m_encWidth = w;
        m_encHeight = h;
        return true;
    };

    // After a key frame request the last picture is fed at the full frame rate for a while: the encoder
    // only emits output after ~20 input frames, which at the paused 2 fps would take ten seconds.
    auto boostUntil = std::chrono::steady_clock::now();
    while (m_running) {
        bool haveFrame = false;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            const bool boost = std::chrono::steady_clock::now() < boostUntil;
            m_cv.wait_for(lock, boost ? interval : std::chrono::microseconds(200000), [this] { return !m_running || m_hasPending || m_keyRequested; });
            if (!m_running) {
                break;
            }
            if (m_hasPending) {
                frame.swap(m_pending);
                fw = m_pendingW;
                fh = m_pendingH;
                ft = m_pendingTime;
                m_hasPending = false;
                haveFrame = true;
            } else if (!m_last.empty() && (m_keyRequested || std::chrono::steady_clock::now() - lastEncode >=
                                           (std::chrono::steady_clock::now() < boostUntil ? std::chrono::duration_cast<std::chrono::microseconds>(interval) : std::chrono::microseconds(500000)))) {
                // Nothing new on screen (paused): repeat the last picture (twice a second, or at full rate
                // right after a key frame request) so the encoder keeps producing output and a viewer joining
                // during a pause gets the picture. Repeated frames cost almost nothing.
                frame = m_last;
                fw = m_lastW;
                fh = m_lastH;
                ft = qpcSeconds();
                haveFrame = true;
            }
        }
        if (!haveFrame) {
            continue;
        }

        // pace to the configured frame rate (newer frames simply replace the pending one)
        const auto now = std::chrono::steady_clock::now();
        if (now - lastEncode < interval && !m_keyRequested) {
            std::this_thread::sleep_for(interval - (now - lastEncode));
        }
        lastEncode = std::chrono::steady_clock::now();

        if (fw != d.encW || fh != d.encH || !d.mft) {
            if (!createEncoder(fw, fh)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            m_keyRequested = true;
        }
        const bool forceKey = m_keyRequested.exchange(false);
        if (forceKey) {
            if (d.codec) SetCodecUInt(d.codec.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
            boostUntil = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        }

        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        MFCreateSample(&sample);
        MFCreateMemoryBuffer(DWORD(frame.size()), &buffer);
        BYTE* p = nullptr;
        buffer->Lock(&p, nullptr, nullptr);
        memcpy(p, frame.data(), frame.size());
        buffer->Unlock();
        buffer->SetCurrentLength(DWORD(frame.size()));
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(LONGLONG(ft * 1e7));
        sample->SetSampleDuration(LONGLONG(1e7 / m_settings.fps));
        if (FAILED(d.mft->ProcessInput(0, sample.Get(), 0))) {
            d.mft.Reset();  // recreate on next frame
            continue;
        }
        m_last = frame;
        m_lastW = fw;
        m_lastH = fh;

        for (;;) {
            MFT_OUTPUT_DATA_BUFFER ob{};
            ComPtr<IMFSample> outSample;
            if (!d.mftProvidesSamples) {
                ComPtr<IMFMediaBuffer> ob2;
                MFCreateSample(&outSample);
                MFCreateMemoryBuffer(d.outBufferSize, &ob2);
                outSample->AddBuffer(ob2.Get());
                ob.pSample = outSample.Get();
            }
            DWORD status = 0;
            HRESULT hr = d.mft->ProcessOutput(0, 1, &ob, &status);
            if (ob.pEvents) {
                ob.pEvents->Release();
            }
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                break;
            }
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                ComPtr<IMFMediaType> t;
                if (SUCCEEDED(d.mft->GetOutputAvailableType(0, 0, &t))) {
                    d.mft->SetOutputType(0, t.Get(), 0);
                }
                continue;
            }
            if (FAILED(hr)) {
                break;
            }
            IMFSample* produced = ob.pSample;
            ComPtr<IMFMediaBuffer> contiguous;
            if (produced && SUCCEEDED(produced->ConvertToContiguousBuffer(&contiguous))) {
                BYTE* data = nullptr;
                DWORD len = 0;
                contiguous->Lock(&data, nullptr, &len);
                const bool key = MFGetAttributeUINT32(produced, MFSampleExtension_CleanPoint, FALSE) != FALSE;
                if (key && !d.sequenceHeader.empty() && !StartsWithSps(data, len)) {
                    d.out.assign(d.sequenceHeader.begin(), d.sequenceHeader.end());
                    d.out.insert(d.out.end(), data, data + len);
                    m_onEncoded(d.out.data(), d.out.size(), true, ft);
                } else {
                    m_onEncoded(data, len, key, ft);
                }
                contiguous->Unlock();
                framesThisSecond++;
            }
            if (d.mftProvidesSamples && produced) {
                produced->Release();
            }
        }

        const auto t = std::chrono::steady_clock::now();
        if (t - fpsWindow >= std::chrono::seconds(1)) {
            m_fps = framesThisSecond / std::chrono::duration<double>(t - fpsWindow).count();
            framesThisSecond = 0;
            fpsWindow = t;
        }
    }
    d.mft.Reset();
    d.codec.Reset();
    MFShutdown();
    CoUninitialize();
}
