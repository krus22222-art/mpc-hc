#include "HttpServer.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <sstream>

namespace {

std::string UrlDecode(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

const char* StatusText(int status)
{
    switch (status) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 410: return "Gone";
        case 413: return "Payload Too Large";
        default: return "Error";
    }
}

}  // namespace

bool HttpServer::Start(uint16_t port, bool loopbackOnly, Handler handler)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return false;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
    addr.sin_port = htons(port);
    BOOL excl = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof(excl));
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, 16) != 0) {
        closesocket(s);
        return false;
    }
    m_listen = s;
    m_port = port;
    m_handler = std::move(handler);
    m_running = true;
    m_thread = std::thread([this] { AcceptLoop(); });
    return true;
}

void HttpServer::Stop()
{
    if (!m_running.exchange(false)) {
        return;
    }
    closesocket(static_cast<SOCKET>(m_listen));
    if (m_thread.joinable()) {
        m_thread.join();
    }
    WSACleanup();
}

void HttpServer::AcceptLoop()
{
    while (m_running) {
        SOCKET c = accept(static_cast<SOCKET>(m_listen), nullptr, nullptr);
        if (c == INVALID_SOCKET) {
            continue;
        }
        std::thread([this, c] { Serve(c); }).detach();
    }
}

void HttpServer::Serve(uintptr_t sockValue)
{
    SOCKET sock = static_cast<SOCKET>(sockValue);
    DWORD timeout = 10000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    std::string data;
    char buf[8192];
    size_t headerEnd = std::string::npos;
    size_t contentLength = 0;
    HttpResponse resp;
    HttpRequest req;

    for (;;) {
        int n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) {
            closesocket(sock);
            return;
        }
        data.append(buf, n);
        if (headerEnd == std::string::npos) {
            headerEnd = data.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                std::istringstream hs(data.substr(0, headerEnd));
                std::string line, target, version;
                std::getline(hs, line);
                std::istringstream rl(line);
                rl >> req.method >> target >> version;
                size_t q = target.find('?');
                req.path = target.substr(0, q);
                if (q != std::string::npos) {
                    std::istringstream qs(target.substr(q + 1));
                    std::string kv;
                    while (std::getline(qs, kv, '&')) {
                        size_t eq = kv.find('=');
                        req.query[UrlDecode(kv.substr(0, eq))] = eq == std::string::npos ? "" : UrlDecode(kv.substr(eq + 1));
                    }
                }
                while (std::getline(hs, line)) {
                    std::string lower = line;
                    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return char(::tolower(ch)); });
                    if (lower.rfind("content-length:", 0) == 0) {
                        contentLength = std::stoul(line.substr(15));
                    }
                }
                if (contentLength > 1024 * 1024) {
                    resp = {413, "text/plain", "too large"};
                    break;
                }
            }
        }
        if (headerEnd != std::string::npos && data.size() >= headerEnd + 4 + contentLength) {
            req.body = data.substr(headerEnd + 4, contentLength);
            if (req.method == "OPTIONS") {
                resp = {204, "text/plain", ""};
            } else {
                try {
                    resp = m_handler(req);
                } catch (...) {
                    resp = {500, "text/plain", "internal error"};
                }
            }
            break;
        }
    }

    std::ostringstream out;
    out << "HTTP/1.1 " << resp.status << ' ' << StatusText(resp.status) << "\r\n"
        << "Content-Type: " << resp.contentType << "\r\n"
        << "Content-Length: " << resp.body.size() << "\r\n"
        << "Cache-Control: no-store\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        << "Access-Control-Allow-Headers: Content-Type\r\n"
        << "Connection: close\r\n\r\n"
        << resp.body;
    const std::string s = out.str();
    send(sock, s.data(), static_cast<int>(s.size()), 0);
    shutdown(sock, SD_SEND);
    closesocket(sock);
}
