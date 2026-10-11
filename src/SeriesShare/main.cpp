// mpc-share — "Спільний перегляд" for MPC-HC (series-player).
// Streams the player's video area and the player's own audio to a browser via WebRTC.
//
//   mpc-share.exe --hwnd <main window> [--view <video area window>] [--pid <player pid>] [--port 8765]

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <winrt/base.h>

#include <nlohmann/json.hpp>

#include "AudioCapture.h"
#include "HttpServer.h"
#include "RelayClient.h"
#include "Streamer.h"
#include "VideoCapture.h"
#include "viewer_html.h"

#include <cstdio>
#include <memory>
#include <sstream>
#include <string>

namespace {

enum : int { IDC_LINK = 101, IDC_COPY, IDC_OPEN, IDC_STOP, IDC_STATUS, IDC_STATS, IDC_CAPTION };

struct App {
    HWND player = nullptr;
    HWND view = nullptr;
    DWORD pid = 0;
    uint16_t port = 8765;
    int64_t clockStart = 0;

    std::unique_ptr<Streamer> streamer;
    std::unique_ptr<RelayClient> relay;
    std::string relayBase = "https://mpc-share.nashe-kino.workers.dev";
    bool useRelay = true;
    VideoCapture video;
    AudioCapture audio;
    HttpServer http;
    std::wstring url;
    std::wstring status;
    bool audioOk = false;

    HWND wnd = nullptr;
    HFONT font = nullptr, bold = nullptr;
} g;

std::wstring Widen(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

HWND FindPlayer()
{
    return FindWindowW(L"MediaPlayerClassicW", nullptr);
}

void ParseArgs()
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i + 1 < argc; i++) {
        const std::wstring a = argv[i];
        if (a == L"--hwnd") g.player = reinterpret_cast<HWND>(static_cast<uintptr_t>(wcstoull(argv[++i], nullptr, 0)));
        else if (a == L"--view") g.view = reinterpret_cast<HWND>(static_cast<uintptr_t>(wcstoull(argv[++i], nullptr, 0)));
        else if (a == L"--pid") g.pid = DWORD(wcstoul(argv[++i], nullptr, 0));
        else if (a == L"--port") g.port = uint16_t(wcstoul(argv[++i], nullptr, 0));
        else if (a == L"--relay") { std::wstring r = argv[++i]; g.relayBase.assign(r.begin(), r.end()); }
    }
    for (int i = 1; i < argc; i++) {
        if (std::wstring(argv[i]) == L"--local") g.useRelay = false;
    }
    LocalFree(argv);
    if (!g.player || !IsWindow(g.player)) {
        g.player = FindPlayer();
    }
    if (g.player && !g.pid) {
        GetWindowThreadProcessId(g.player, &g.pid);
    }
}

HttpResponse Handle(const HttpRequest& req)
{
    if (req.method == "GET" && (req.path == "/" || req.path == "/index.html")) {
        return {200, "text/html; charset=utf-8", kViewerHtml};
    }
    if (req.method == "POST" && req.path == "/offer") {
        auto offer = g.streamer->CreateOffer();
        if (!offer) {
            return {500, "text/plain", "offer failed"};
        }
        nlohmann::json j{{"id", offer->id}, {"sdp", offer->sdp}};
        return {200, "application/json", j.dump()};
    }
    if (req.method == "POST" && req.path == "/answer") {
        auto it = req.query.find("id");
        if (it == req.query.end() || !g.streamer->SetAnswer(it->second, req.body)) {
            return {410, "text/plain", "unknown session"};
        }
        return {204, "text/plain", ""};
    }
    return {404, "text/plain", "not found"};
}

void SetText(int id, const std::wstring& s)
{
    SetDlgItemTextW(g.wnd, id, s.c_str());
}

std::wstring FormatBytes(uint64_t b)
{
    wchar_t buf[64];
    if (b >= 1024ull * 1024 * 1024) swprintf_s(buf, L"%.2f ГБ", b / (1024.0 * 1024 * 1024));
    else swprintf_s(buf, L"%.1f МБ", b / (1024.0 * 1024));
    return buf;
}

void UpdateStats()
{
    if (!g.streamer) return;
    wchar_t buf[256], video[96];
    const int viewers = g.streamer->ViewerCount();
    if (g.video.Width() > 0) swprintf_s(video, L"%d×%d, %.0f кадр/с", g.video.Width(), g.video.Height(), g.video.CurrentFps());
    else swprintf_s(video, L"немає — відкрийте серію в плеєрі");
    swprintf_s(buf, L"Глядачів: %d   ·   Відео: %s   ·   Звук: %s   ·   Передано: %s", viewers, video,
               g.audioOk ? L"так" : L"ні", FormatBytes(g.streamer->BytesSent()).c_str());
    SetText(IDC_STATS, buf);
    if (g.relay && !g.relay->Connected()) {
        const std::string err = g.relay->LastError();
        SetText(IDC_STATUS, err.empty() ? L"○ Підключення до сервера…" : L"○ Немає зв'язку з сервером, повторюю… (" + Widen(err) + L")");
    } else {
        SetText(IDC_STATUS, viewers > 0 ? L"● Трансляція йде — глядач підключений" : L"● Трансляція запущена — чекаємо на глядача");
    }
    if (g.video.WindowClosed() || (g.player && !IsWindow(g.player))) {
        DestroyWindow(g.wnd);  // the player was closed
    }
}

void CopyLink()
{
    if (!OpenClipboard(g.wnd)) return;
    EmptyClipboard();
    const size_t bytes = (g.url.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    memcpy(GlobalLock(h), g.url.c_str(), bytes);
    GlobalUnlock(h);
    SetClipboardData(CF_UNICODETEXT, h);
    CloseClipboard();
    SetText(IDC_STATUS, L"Посилання скопійовано — надішліть його глядачу.");
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_COPY: CopyLink(); return 0;
                case IDC_OPEN: ShellExecuteW(hwnd, L"open", g.url.c_str(), nullptr, nullptr, SW_SHOWNORMAL); return 0;
                case IDC_STOP: DestroyWindow(hwnd); return 0;
            }
            break;
        case WM_TIMER:
            UpdateStats();
            return 0;
        case WM_CTLCOLORSTATIC:
            if (GetDlgCtrlID(reinterpret_cast<HWND>(lp)) == IDC_STATUS) {
                SetTextColor(reinterpret_cast<HDC>(wp), RGB(0, 128, 60));
                SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND CreateUi(HINSTANCE inst)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"MpcShareWindow";
    RegisterClassW(&wc);

    const UINT dpi = GetDpiForSystem();
    auto S = [dpi](int v) { return MulDiv(v, int(dpi), 96); };
    g.font = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g.bold = CreateFontW(-S(15), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

    RECT r{0, 0, S(560), S(210)};
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Спільний перегляд — MPC-HC", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    g.wnd = hwnd;

    auto add = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT f) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), hwnd, reinterpret_cast<HMENU>(INT_PTR(id)), inst, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
        return c;
    };
    add(L"STATIC", L"", 0, 16, 12, 528, 22, IDC_STATUS, g.bold);
    add(L"STATIC", L"Посилання для глядача (надішліть його — відкривається в браузері на планшеті чи телефоні):", 0, 16, 42, 528, 20, IDC_CAPTION, g.font);
    add(L"EDIT", g.url.c_str(), WS_BORDER | ES_READONLY | ES_AUTOHSCROLL, 16, 64, 528, 26, IDC_LINK, g.font);
    add(L"BUTTON", L"Копіювати посилання", BS_DEFPUSHBUTTON | WS_TABSTOP, 16, 102, 170, 32, IDC_COPY, g.font);
    add(L"BUTTON", L"Відкрити", WS_TABSTOP, 194, 102, 110, 32, IDC_OPEN, g.font);
    add(L"BUTTON", L"Зупинити трансляцію", WS_TABSTOP, 374, 102, 170, 32, IDC_STOP, g.font);
    add(L"STATIC", L"", 0, 16, 150, 528, 44, IDC_STATS, g.font);
    return hwnd;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    ParseArgs();
    if (!g.player) {
        MessageBoxW(nullptr, L"Спершу запустіть MPC-HC і відкрийте відео.", L"Спільний перегляд", MB_ICONINFORMATION);
        return 1;
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g.clockStart = now.QuadPart;

    g.streamer = std::make_unique<Streamer>(std::vector<std::string>{"stun:stun.cloudflare.com:3478", "stun:stun.l.google.com:19302"});
    g.streamer->onKeyframeNeeded = [] { g.video.RequestKeyframe(); };

    bool listening = false;
    for (int i = 0; i < 20 && !listening; i++) {
        listening = g.http.Start(uint16_t(g.port + i), true, Handle);
        if (listening) g.port = uint16_t(g.port + i);
    }
    if (!listening) {
        MessageBoxW(nullptr, L"Не вдалося запустити локальний сервер трансляції.", L"Спільний перегляд", MB_ICONERROR);
        return 1;
    }
    g.url = L"http://127.0.0.1:" + std::to_wstring(g.port) + L"/";
    if (g.useRelay) {
        auto id = ShareIdentity::LoadOrCreate(g.relayBase);
        g.relay = std::make_unique<RelayClient>(*g.streamer, id.relay, id.room, id.key);
        // The viewer may pause/resume playback and switch episodes (both sides see the same picture anyway)
        g.relay->onControl = [](const std::string& action) {
            // MPC-HC command IDs: ID_PLAY_PLAYPAUSE, ID_NAVIGATE_SKIPFORWARDFILE / SKIPBACKFILE (whole files,
            // not chapters: anime episodes often have chapters)
            const WPARAM cmd = action == "playpause" ? 889 : action == "next" ? 920 : action == "prev" ? 919 : 0;
            if (cmd && g.player && IsWindow(g.player)) PostMessageW(g.player, WM_COMMAND, cmd, 0);
        };
        g.relay->Start();
        g.url = Widen(g.relay->ViewerUrl());
    }

    std::wstring error;
    VideoSettings vs;
    if (!g.video.Start(g.player, g.view, vs, g.clockStart,
                       [](const uint8_t* d, size_t n, bool key, double t) { g.streamer->SendVideo(d, n, key, t); }, error)) {
        MessageBoxW(nullptr, error.c_str(), L"Спільний перегляд", MB_ICONERROR);
        return 1;
    }
    std::wstring audioError;
    g.audioOk = g.audio.Start(g.pid, g.clockStart, 128, [](const uint8_t* d, size_t n, double t) { g.streamer->SendAudio(d, n, t); }, audioError);

    HWND hwnd = CreateUi(inst);
    if (!g.audioOk) {
        SetText(IDC_STATS, L"Звук не транслюється: " + audioError);
    }
    ShowWindow(hwnd, show);
    UpdateStats();
    SetTimer(hwnd, 1, 1000, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (g.relay) g.relay->Stop();
    g.http.Stop();
    g.audio.Stop();
    g.video.Stop();
    g.streamer.reset();
    return 0;
}
