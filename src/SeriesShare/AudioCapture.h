#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

// Captures only the audio of one process (the player, incl. child processes) and encodes Opus 48 kHz stereo.
class AudioCapture {
public:
    // packet: one Opus frame (20 ms); seconds: capture time relative to the shared clock start.
    using OnPacket = std::function<void(const uint8_t* data, size_t size, double seconds)>;

    ~AudioCapture() { Stop(); }
    bool Start(DWORD processId, int64_t clockStartQpc, int bitrateKbps, OnPacket onPacket, std::wstring& error);
    void Stop();
    bool Active() const { return m_active; }

private:
    void Loop();

    DWORD m_pid = 0;
    int64_t m_clockStartQpc = 0;
    int m_bitrate = 128;
    OnPacket m_onPacket;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_active{false};
    std::thread m_thread;
    HANDLE m_started = nullptr;
    std::wstring m_error;
};
