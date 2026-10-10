#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace rtc {
class PeerConnection;
class Track;
struct RtpPacketizationConfig;
}  // namespace rtc

// WebRTC fan-out: one PeerConnection per viewer, same encoded stream for everybody.
class Streamer {
public:
    struct Offer {
        std::string id;
        std::string sdp;
    };

    explicit Streamer(std::vector<std::string> iceServers);
    ~Streamer();

    std::optional<Offer> CreateOffer();                          // new viewer: our offer (ICE gathered)
    bool SetAnswer(const std::string& id, const std::string& sdp);

    void SendVideo(const uint8_t* data, size_t size, bool key, double seconds);
    void SendAudio(const uint8_t* data, size_t size, double seconds);

    int ViewerCount() const;
    uint64_t BytesSent() const { return m_bytesSent; }

    std::function<void()> onKeyframeNeeded;

private:
    struct Viewer;
    void Cleanup();

    std::vector<std::string> m_iceServers;
    mutable std::mutex m_mutex;
    std::map<std::string, std::shared_ptr<Viewer>> m_viewers;
    std::atomic<uint64_t> m_bytesSent{0};
    uint32_t m_nextSsrc = 1000;
};
