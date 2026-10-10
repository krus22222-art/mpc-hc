#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <thread>

struct HttpRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string> query;
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::string contentType = "text/plain; charset=utf-8";
    std::string body;
};

// Minimal blocking HTTP/1.1 server (Connection: close) for the viewer page and WebRTC signaling.
class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    ~HttpServer() { Stop(); }
    bool Start(uint16_t port, bool loopbackOnly, Handler handler);
    void Stop();
    uint16_t Port() const { return m_port; }

private:
    void AcceptLoop();
    void Serve(uintptr_t sock);

    Handler m_handler;
    uintptr_t m_listen = ~uintptr_t(0);
    uint16_t m_port = 0;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
};
