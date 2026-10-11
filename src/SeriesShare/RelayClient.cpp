#include "RelayClient.h"

#include "Streamer.h"

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <wincrypt.h>

#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace {

std::string RandomString(size_t n)
{
    static const char abc[] = "abcdefghijkmnopqrstuvwxyz23456789";  // no 0/1/l/o confusion
    std::string s(n, 'a');
    std::vector<unsigned char> buf(n);
    BCryptGenRandom(nullptr, buf.data(), ULONG(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    for (size_t i = 0; i < n; i++) s[i] = abc[buf[i] % (sizeof(abc) - 1)];
    return s;
}

std::string DataDir()
{
    wchar_t* path = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) {
        dir = std::wstring(path) + L"\\MPC-HC-series-player";
        CoTaskMemFree(path);
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, 0);
    WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

// Mbed TLS has no access to the Windows certificate store: export the trusted roots to a PEM file.
std::string ExportWindowsRoots()
{
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    std::string file = std::string(tmp) + "mpc-share-roots.pem";
    HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
    if (!store) return {};
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    PCCERT_CONTEXT ctx = nullptr;
    while ((ctx = CertEnumCertificatesInStore(store, ctx)) != nullptr) {
        DWORD len = 0;
        CryptBinaryToStringA(ctx->pbCertEncoded, ctx->cbCertEncoded, CRYPT_STRING_BASE64HEADER | CRYPT_STRING_NOCR, nullptr, &len);
        std::string pem(len, 0);
        if (CryptBinaryToStringA(ctx->pbCertEncoded, ctx->cbCertEncoded, CRYPT_STRING_BASE64HEADER | CRYPT_STRING_NOCR, pem.data(), &len)) {
            pem.resize(len);
            out << pem;
        }
    }
    CertCloseStore(store, 0);
    return file;
}

}  // namespace

ShareIdentity ShareIdentity::LoadOrCreate(const std::string& defaultRelay)
{
    ShareIdentity id;
    const std::string file = DataDir() + "\\share.json";
    try {
        std::ifstream in(file);
        if (in) {
            auto j = nlohmann::json::parse(in);
            id.room = j.value("room", "");
            id.key = j.value("key", "");
            id.relay = j.value("relay", "");
        }
    } catch (...) {
    }
    bool dirty = false;
    if (id.room.size() < 8) { id.room = RandomString(12); dirty = true; }
    if (id.key.size() < 16) { id.key = RandomString(32); dirty = true; }
    if (id.relay.empty()) { id.relay = defaultRelay; dirty = true; }
    if (dirty) {
        std::ofstream out(file, std::ios::trunc);
        out << nlohmann::json{{"room", id.room}, {"key", id.key}, {"relay", id.relay}}.dump(2);
    }
    return id;
}

RelayClient::RelayClient(Streamer& streamer, std::string baseUrl, std::string room, std::string key)
    : m_streamer(streamer), m_base(std::move(baseUrl)), m_room(std::move(room)), m_key(std::move(key))
{
    while (!m_base.empty() && m_base.back() == '/') m_base.pop_back();
}

RelayClient::~RelayClient()
{
    Stop();
}

std::string RelayClient::LastError() const
{
    std::lock_guard<std::mutex> l(m_errMutex);
    return m_lastError;
}

void RelayClient::Start()
{
    m_caFile = ExportWindowsRoots();
    m_running = true;
    m_thread = std::thread([this] { Loop(); });
}

void RelayClient::Stop()
{
    if (!m_running.exchange(false)) return;
    if (m_thread.joinable()) m_thread.join();
    std::lock_guard<std::mutex> l(m_mutex);
    if (m_ws) {
        try { m_ws->close(); } catch (...) {}
        m_ws.reset();
    }
}

void RelayClient::Loop()
{
    auto lastPing = std::chrono::steady_clock::now();
    int backoffMs = 1000;
    auto nextAttempt = std::chrono::steady_clock::now();
    while (m_running) {
        const auto now = std::chrono::steady_clock::now();
        if (m_reconnect && now >= nextAttempt) {
            m_reconnect = false;
            Connect();
            nextAttempt = now + std::chrono::milliseconds(backoffMs);
            backoffMs = std::min(backoffMs * 2, 30000);
        }
        if (m_connected) {
            backoffMs = 1000;
            if (m_needIce.exchange(false) || now - m_iceRequested > std::chrono::hours(4)) {
                Send(R"({"type":"get-ice"})");  // TURN credentials (TTL 6 h), refreshed every 4 h
                m_iceRequested = now;
            }
            if (now - lastPing > std::chrono::seconds(25)) {
                Send(R"({"type":"ping"})");
                lastPing = now;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

void RelayClient::Connect()
{
    rtc::WebSocketConfiguration cfg;
    if (!m_caFile.empty()) cfg.caCertificatePemFile = m_caFile;
    cfg.connectionTimeout = std::chrono::seconds(15);
    cfg.pingInterval = std::chrono::seconds(20);
    auto ws = std::make_shared<rtc::WebSocket>(cfg);
    std::weak_ptr<rtc::WebSocket> weak = ws;
    ws->onOpen([this] {
        m_connected = true;
        m_needIce = true;  // ask for TURN credentials right away
        std::lock_guard<std::mutex> l(m_errMutex);
        m_lastError.clear();
    });
    ws->onClosed([this, weak] {
        auto self = weak.lock();
        std::lock_guard<std::mutex> l(m_mutex);
        if (self && m_ws == self) {
            m_connected = false;
            m_reconnect = true;
        }
    });
    ws->onError([this](std::string err) {
        std::lock_guard<std::mutex> l(m_errMutex);
        m_lastError = err;
    });
    ws->onMessage([this](rtc::message_variant msg) {
        if (std::holds_alternative<std::string>(msg)) OnMessage(std::get<std::string>(msg));
    });

    std::string wsUrl = m_base;
    if (wsUrl.rfind("https://", 0) == 0) wsUrl = "wss://" + wsUrl.substr(8);
    else if (wsUrl.rfind("http://", 0) == 0) wsUrl = "ws://" + wsUrl.substr(7);
    wsUrl += "/host?room=" + m_room + "&key=" + m_key;
    {
        std::lock_guard<std::mutex> l(m_mutex);
        if (m_ws) {
            try { m_ws->forceClose(); } catch (...) {}
        }
        m_ws = ws;
    }
    try {
        ws->open(wsUrl);
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> l(m_errMutex);
        m_lastError = e.what();
        m_reconnect = true;
    }
}

void RelayClient::Send(const std::string& text)
{
    std::lock_guard<std::mutex> l(m_mutex);
    if (m_ws && m_ws->isOpen()) {
        try { m_ws->send(text); } catch (...) {}
    }
}

void RelayClient::OnMessage(const std::string& text)
{
    nlohmann::json msg;
    try { msg = nlohmann::json::parse(text); } catch (...) { return; }
    const std::string type = msg.value("type", "");
    if (type == "ice" && msg.contains("iceServers")) {
        m_streamer.SetIceServersJson(msg["iceServers"].dump());
        return;
    }
    const std::string reqId = msg.value("reqId", "");
    if (reqId.empty()) return;

    if (type == "offer") {
        // ICE gathering may take a few seconds: never block the WebSocket thread
        std::thread([this, reqId] {
            auto offer = m_streamer.CreateOffer();
            nlohmann::json reply{{"reqId", reqId}};
            if (offer) {
                reply["id"] = offer->id;
                reply["sdp"] = offer->sdp;
            } else {
                reply["error"] = "offer failed";
            }
            Send(reply.dump());
        }).detach();
    } else if (type == "answer") {
        const bool ok = m_streamer.SetAnswer(msg.value("id", ""), msg.value("sdp", ""));
        nlohmann::json reply{{"reqId", reqId}};
        if (ok) reply["ok"] = true;
        else reply["error"] = "unknown session";
        Send(reply.dump());
    }
}
