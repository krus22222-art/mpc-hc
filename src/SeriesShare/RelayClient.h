#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class Streamer;
namespace rtc {
class WebSocket;
}

// Keeps a WebSocket to the signaling relay (Cloudflare Worker) and answers viewers' offer/answer
// requests for this player's room. Reconnects automatically.
class RelayClient {
public:
    RelayClient(Streamer& streamer, std::string baseUrl, std::string room, std::string key);
    ~RelayClient();

    void Start();
    void Stop();
    bool Connected() const { return m_connected; }
    std::string ViewerUrl() const { return m_base + "/r/" + m_room; }
    std::string LastError() const;

    // A viewer asked for a player action ("playpause"); called on the WebSocket thread.
    std::function<void(const std::string& action)> onControl;

private:
    void Loop();
    void Connect();
    void Send(const std::string& text);
    void OnMessage(const std::string& text);

    Streamer& m_streamer;
    std::string m_base, m_room, m_key;
    std::string m_caFile;
    std::shared_ptr<rtc::WebSocket> m_ws;
    std::mutex m_mutex;
    mutable std::mutex m_errMutex;
    std::string m_lastError;
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_reconnect{true};
    std::atomic<bool> m_needIce{true};
    std::chrono::steady_clock::time_point m_iceRequested{};  // Loop thread only
    std::thread m_thread;
};

// Persistent room id + host key (%LOCALAPPDATA%\MPC-HC-series-player\share.json).
struct ShareIdentity {
    std::string room;
    std::string key;
    std::string relay;
    static ShareIdentity LoadOrCreate(const std::string& defaultRelay);
};
