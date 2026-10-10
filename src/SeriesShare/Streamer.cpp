#include "Streamer.h"

#include <rtc/rtc.hpp>
#include <psa/crypto.h>

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <future>
#include <random>

namespace {
constexpr int kVideoPt = 96;
constexpr int kAudioPt = 111;
// Constrained Baseline, level 4.0 (up to 1080p30), works in Safari (iPad/iPhone) and Chrome/Android
const char* kH264Profile = "profile-level-id=42e028;packetization-mode=1;level-asymmetry-allowed=1";

std::string RandomId()
{
    static std::mt19937_64 rng{std::random_device{}()};
    static const char* abc = "abcdefghijkmnopqrstuvwxyz23456789";
    std::string s;
    for (int i = 0; i < 12; i++) {
        s += abc[rng() % 33];
    }
    return s;
}
}  // namespace

struct Streamer::Viewer {
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> video, audio;
    std::shared_ptr<rtc::RtpPacketizationConfig> videoCfg, audioCfg;
    std::atomic<bool> connected{false};
    std::atomic<bool> waitingForKey{true};
    std::atomic<bool> closed{false};
    std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();
};

Streamer::Streamer(std::vector<std::string> iceServers) : m_iceServers(std::move(iceServers))
{
    // Diagnostics go to %TEMP%\mpc-share.log (local only).
    static std::mutex logMutex;
    char path[MAX_PATH];
    const DWORD n = GetTempPathA(MAX_PATH, path);
    static std::string logPath = std::string(path, n) + "mpc-share.log";
    std::remove(logPath.c_str());
    // Mbed TLS 3.6 performs (D)TLS key exchange through PSA Crypto, which must be initialised once.
    psa_crypto_init();
    rtc::InitLogger(rtc::LogLevel::Warning, [](rtc::LogLevel level, std::string message) {
        std::lock_guard<std::mutex> l(logMutex);
        if (FILE* f = std::fopen(logPath.c_str(), "a")) {
            std::fprintf(f, "[%d] %s\n", int(level), message.c_str());
            std::fclose(f);
        }
    });
}

Streamer::~Streamer()
{
    std::map<std::string, std::shared_ptr<Viewer>> viewers;
    {
        std::lock_guard<std::mutex> l(m_mutex);
        viewers.swap(m_viewers);
    }
    for (auto& [id, v] : viewers) {
        v->pc->close();
    }
}

std::optional<Streamer::Offer> Streamer::CreateOffer()
{
    Cleanup();

    rtc::Configuration config;
    for (const auto& s : m_iceServers) {
        config.iceServers.emplace_back(s);
    }
    config.disableAutoNegotiation = true;

    auto v = std::make_shared<Viewer>();
    v->pc = std::make_shared<rtc::PeerConnection>(config);

    uint32_t vssrc, assrc;
    {
        std::lock_guard<std::mutex> l(m_mutex);
        vssrc = m_nextSsrc++;
        assrc = m_nextSsrc++;
    }
    const std::string cname = "mpc-share";

    rtc::Description::Video vdesc("video", rtc::Description::Direction::SendOnly);
    vdesc.addH264Codec(kVideoPt, std::string(kH264Profile));
    vdesc.addSSRC(vssrc, cname, "stream", "video");
    v->video = v->pc->addTrack(vdesc);

    rtc::Description::Audio adesc("audio", rtc::Description::Direction::SendOnly);
    adesc.addOpusCodec(kAudioPt);
    adesc.addSSRC(assrc, cname, "stream", "audio");
    v->audio = v->pc->addTrack(adesc);

    v->videoCfg = std::make_shared<rtc::RtpPacketizationConfig>(vssrc, cname, kVideoPt, rtc::H264RtpPacketizer::ClockRate);
    auto vpack = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::LongStartSequence, v->videoCfg);
    vpack->addToChain(std::make_shared<rtc::RtcpSrReporter>(v->videoCfg));
    vpack->addToChain(std::make_shared<rtc::RtcpNackResponder>(2048));
    std::weak_ptr<Viewer> weak = v;
    vpack->addToChain(std::make_shared<rtc::PliHandler>([this, weak] {
        if (auto vv = weak.lock()) vv->waitingForKey = true;
        if (onKeyframeNeeded) onKeyframeNeeded();
    }));
    v->video->setMediaHandler(vpack);

    v->audioCfg = std::make_shared<rtc::RtpPacketizationConfig>(assrc, cname, kAudioPt, rtc::OpusRtpPacketizer::DefaultClockRate);
    auto apack = std::make_shared<rtc::OpusRtpPacketizer>(v->audioCfg);
    apack->addToChain(std::make_shared<rtc::RtcpSrReporter>(v->audioCfg));
    apack->addToChain(std::make_shared<rtc::RtcpNackResponder>());
    v->audio->setMediaHandler(apack);

    v->video->onOpen([this, weak] {
        if (auto vv = weak.lock()) {
            vv->connected = true;
            vv->waitingForKey = true;
        }
        if (onKeyframeNeeded) onKeyframeNeeded();
    });
    v->pc->onStateChange([weak](rtc::PeerConnection::State s) {
        if (auto vv = weak.lock()) {
            if (s == rtc::PeerConnection::State::Failed || s == rtc::PeerConnection::State::Closed ||
                s == rtc::PeerConnection::State::Disconnected) {
                vv->closed = true;
                vv->connected = false;
            }
        }
    });

    auto gathered = std::make_shared<std::promise<void>>();
    auto gatheredFuture = gathered->get_future();
    v->pc->onGatheringStateChange([gathered](rtc::PeerConnection::GatheringState s) {
        if (s == rtc::PeerConnection::GatheringState::Complete) {
            try { gathered->set_value(); } catch (...) {}
        }
    });
    v->pc->setLocalDescription(rtc::Description::Type::Offer);
    gatheredFuture.wait_for(std::chrono::seconds(5));

    auto desc = v->pc->localDescription();
    if (!desc) {
        v->pc->close();
        return std::nullopt;
    }
    // We must be the DTLS *client*: modern browsers send a ClientHello larger than one datagram
    // (post-quantum key share), and Mbed TLS cannot reassemble a fragmented ClientHello as a server.
    // "active" in our offer makes the browser answer "passive", so libdatachannel takes the client role.
    std::string sdp = std::string(*desc);
    for (size_t pos; (pos = sdp.find("a=setup:actpass")) != std::string::npos;) {
        sdp.replace(pos, 15, "a=setup:active");
    }
    Offer offer{RandomId(), sdp};
    {
        std::lock_guard<std::mutex> l(m_mutex);
        m_viewers[offer.id] = v;
    }
    return offer;
}

bool Streamer::SetAnswer(const std::string& id, const std::string& sdp)
{
    std::shared_ptr<Viewer> v;
    {
        std::lock_guard<std::mutex> l(m_mutex);
        auto it = m_viewers.find(id);
        if (it == m_viewers.end()) {
            return false;
        }
        v = it->second;
    }
    try {
        v->pc->setRemoteDescription(rtc::Description(sdp, rtc::Description::Type::Answer));
    } catch (...) {
        return false;
    }
    return true;
}

void Streamer::SendVideo(const uint8_t* data, size_t size, bool key, double seconds)
{
    std::vector<std::shared_ptr<Viewer>> list;
    {
        std::lock_guard<std::mutex> l(m_mutex);
        for (auto& [id, v] : m_viewers) {
            if (v->connected && v->video && v->video->isOpen()) list.push_back(v);
        }
    }
    for (auto& v : list) {
        if (v->waitingForKey) {
            if (!key) continue;  // a viewer can only start decoding at a key frame
            v->waitingForKey = false;
        }
        try {
            v->video->sendFrame(reinterpret_cast<const rtc::byte*>(data), size, rtc::FrameInfo(std::chrono::duration<double>(seconds)));
            m_bytesSent += size;
        } catch (...) {
        }
    }
}

void Streamer::SendAudio(const uint8_t* data, size_t size, double seconds)
{
    std::vector<std::shared_ptr<Viewer>> list;
    {
        std::lock_guard<std::mutex> l(m_mutex);
        for (auto& [id, v] : m_viewers) {
            if (v->connected && v->audio && v->audio->isOpen()) list.push_back(v);
        }
    }
    for (auto& v : list) {
        try {
            v->audio->sendFrame(reinterpret_cast<const rtc::byte*>(data), size, rtc::FrameInfo(std::chrono::duration<double>(seconds)));
            m_bytesSent += size;
        } catch (...) {
        }
    }
}

int Streamer::ViewerCount() const
{
    std::lock_guard<std::mutex> l(m_mutex);
    int n = 0;
    for (auto& [id, v] : m_viewers) {
        if (v->connected && !v->closed) n++;
    }
    return n;
}

void Streamer::Cleanup()
{
    std::vector<std::shared_ptr<Viewer>> dead;
    {
        std::lock_guard<std::mutex> l(m_mutex);
        const auto now = std::chrono::steady_clock::now();
        for (auto it = m_viewers.begin(); it != m_viewers.end();) {
            const bool stale = !it->second->connected && now - it->second->created > std::chrono::seconds(60);
            if (it->second->closed || stale) {
                dead.push_back(it->second);
                it = m_viewers.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& v : dead) {
        v->pc->close();
    }
}
