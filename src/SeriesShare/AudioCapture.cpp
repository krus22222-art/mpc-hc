#include "AudioCapture.h"

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <opus.h>

#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

class ActivationHandler
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, Microsoft::WRL::FtmBase,
                                          IActivateAudioInterfaceCompletionHandler> {
public:
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT result = E_FAIL;
    ComPtr<IAudioClient> client;

    ~ActivationHandler() { CloseHandle(done); }

    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* op) override
    {
        HRESULT hrActivate = E_FAIL;
        ComPtr<IUnknown> unk;
        result = op->GetActivateResult(&hrActivate, &unk);
        if (SUCCEEDED(result)) {
            result = hrActivate;
        }
        if (SUCCEEDED(result)) {
            result = unk.As(&client);
        }
        SetEvent(done);
        return S_OK;
    }
};

constexpr int kRate = 48000;
constexpr int kChannels = 2;
constexpr int kFrame = 960;  // 20 ms

}  // namespace

bool AudioCapture::Start(DWORD processId, int64_t clockStartQpc, int bitrateKbps, OnPacket onPacket, std::wstring& error)
{
    m_pid = processId;
    m_clockStartQpc = clockStartQpc;
    m_bitrate = bitrateKbps;
    m_onPacket = std::move(onPacket);
    m_started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_running = true;
    m_thread = std::thread([this] { Loop(); });
    WaitForSingleObject(m_started, 5000);
    CloseHandle(m_started);
    m_started = nullptr;
    if (!m_active) {
        error = m_error.empty() ? L"Не вдалося почати захоплення звуку плеєра." : m_error;
        return false;
    }
    return true;
}

void AudioCapture::Stop()
{
    if (!m_running.exchange(false)) {
        return;
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void AudioCapture::Loop()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto fail = [this](const wchar_t* msg) {
        m_error = msg;
        if (m_started) SetEvent(m_started);
    };

    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = m_pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT pv{};
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(params);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    auto handler = Microsoft::WRL::Make<ActivationHandler>();
    ComPtr<IActivateAudioInterfaceAsyncOperation> op;
    if (FAILED(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, handler.Get(), &op)) ||
        WaitForSingleObject(handler->done, 5000) != WAIT_OBJECT_0 || FAILED(handler->result)) {
        fail(L"Захоплення звуку окремої програми потребує Windows 10 версії 2004 або новішої.");
        CoUninitialize();
        return;
    }
    ComPtr<IAudioClient> client = handler->client;

    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = kChannels;
    wfx.nSamplesPerSec = kRate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ComPtr<IAudioCaptureClient> capture;
    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                  2000000, 0, &wfx, nullptr)) ||
        FAILED(client->SetEventHandle(ready)) || FAILED(client->GetService(IID_PPV_ARGS(&capture))) || FAILED(client->Start())) {
        fail(L"Не вдалося ініціалізувати захоплення звуку плеєра.");
        CloseHandle(ready);
        CoUninitialize();
        return;
    }

    int err = 0;
    OpusEncoder* enc = opus_encoder_create(kRate, kChannels, OPUS_APPLICATION_AUDIO, &err);
    if (!enc) {
        fail(L"Не вдалося створити кодувальник Opus.");
        CloseHandle(ready);
        CoUninitialize();
        return;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(m_bitrate * 1000));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(8));

    m_active = true;
    if (m_started) SetEvent(m_started);

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    std::vector<int16_t> pcm;
    pcm.reserve(kFrame * kChannels * 4);
    double pcmStart = -1;  // capture time of pcm[0]
    unsigned char packet[4000];

    while (m_running) {
        if (WaitForSingleObject(ready, 100) != WAIT_OBJECT_0) {
            continue;
        }
        UINT32 frames = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&frames)) && frames > 0) {
            BYTE* data = nullptr;
            DWORD flags = 0;
            UINT64 qpcPos = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, &qpcPos))) {
                break;
            }
            // qpcPos is in 100 ns units
            const double t = (double(qpcPos) - double(m_clockStartQpc) * 1e7 / double(freq.QuadPart)) / 1e7;
            const double expected = pcmStart + double(pcm.size() / kChannels) / kRate;
            if (pcmStart < 0 || t - expected > 0.06 || expected - t > 0.06) {
                pcm.clear();  // discontinuity (pause/seek or first packet): restart the timeline
                pcmStart = t;
            }
            const size_t n = size_t(frames) * kChannels;
            const size_t old = pcm.size();
            pcm.resize(old + n);
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                std::fill(pcm.begin() + old, pcm.end(), int16_t(0));
            } else {
                memcpy(pcm.data() + old, data, n * sizeof(int16_t));
            }
            capture->ReleaseBuffer(frames);

            while (pcm.size() >= size_t(kFrame) * kChannels) {
                const int bytes = opus_encode(enc, pcm.data(), kFrame, packet, sizeof(packet));
                if (bytes > 0) {
                    m_onPacket(packet, size_t(bytes), pcmStart);
                }
                pcm.erase(pcm.begin(), pcm.begin() + kFrame * kChannels);
                pcmStart += double(kFrame) / kRate;
            }
        }
    }

    client->Stop();
    opus_encoder_destroy(enc);
    CloseHandle(ready);
    m_active = false;
    CoUninitialize();
}
