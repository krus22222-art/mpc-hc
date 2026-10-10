#pragma once
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct VideoSettings {
    int maxHeight = 1080;   // frames are scaled down to this height (keeping aspect ratio)
    int fps = 30;
    int bitrateKbps = 4500;
};

// Captures the player window (cropped to its video area), scales it and encodes H.264 (Annex B).
class VideoCapture {
public:
    // data/size: Annex-B access unit; key: IDR; seconds: capture time relative to the shared clock start.
    using OnEncoded = std::function<void(const uint8_t* data, size_t size, bool key, double seconds)>;

    VideoCapture();
    ~VideoCapture();

    bool Start(HWND window, HWND videoArea, const VideoSettings& settings, int64_t clockStartQpc, OnEncoded onEncoded, std::wstring& error);
    void Stop();
    void RequestKeyframe() { m_keyRequested = true; m_cv.notify_one(); }

    // Statistics for the UI
    int Width() const { return m_encWidth; }
    int Height() const { return m_encHeight; }
    double CurrentFps() const { return m_fps.load(); }
    bool WindowClosed() const { return m_windowClosed; }

private:
    struct Impl;
    void EncoderLoop();
    void OnFrame();  // called from the capture thread pool

    std::unique_ptr<Impl> m_impl;
    HWND m_window = nullptr;
    HWND m_videoArea = nullptr;
    VideoSettings m_settings;
    int64_t m_clockStartQpc = 0;
    int64_t m_qpcFreq = 1;
    OnEncoded m_onEncoded;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<uint8_t> m_pending;  // latest NV12 frame waiting for the encoder
    int m_pendingW = 0, m_pendingH = 0;
    double m_pendingTime = 0;
    bool m_hasPending = false;
    std::vector<uint8_t> m_last;     // last encoded frame (re-encoded as IDR when a viewer joins while paused)
    int m_lastW = 0, m_lastH = 0;

    std::atomic<bool> m_keyRequested{true};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_windowClosed{false};
    std::atomic<int> m_encWidth{0}, m_encHeight{0};
    std::atomic<double> m_fps{0};
    std::thread m_encoderThread;
};
