// mpc-introscan: finds the opening and ending shared by the episodes of one folder by comparing
// audio fingerprints (the approach of Jellyfin's Intro Skipper), for files that have no chapters.
//
//   mpc-introscan.exe "<folder>"
//
// Audio is decoded with the LAV Filters FFmpeg DLLs shipped with the player (LAVFilters64 folder).
// Fingerprint: Haitsma-Kalker (Philips) style, 32 bits per ~0.12 s of mono 11025 Hz audio.
// Result: %LOCALAPPDATA%\MPC-HC-series-player\skip\<folder hash>.txt, one line per episode:
//   <file name>\t<size>\t<opening start>\t<opening end>\t<ending start>\t<ending end>\t<duration>
// (seconds, -1 when not found; UTF-8). Nothing is written into the video folder.

#define NOMINMAX
#include <windows.h>
#include <intrin.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace {

constexpr int kRate = 11025;
constexpr int kFrame = 4096;            // 0.37 s analysis window
constexpr int kHop = 1365;              // ~0.124 s between fingerprint items
constexpr double kStep = double(kHop) / kRate;
constexpr int kBands = 33;              // 33 bands -> 32 difference bits
constexpr double kWindow = 600.0;       // seconds examined at the start and at the end
constexpr double kMinLen = 20.0;        // a shared segment must be at least this long
constexpr int kMaxBits = 9;             // items differing in <= this many bits match

std::string Utf8(const std::wstring& w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Decodes [start, start + length) seconds of the first audio stream to mono float at kRate.
std::vector<float> DecodeAudio(const std::string& path, double start, double length, double* duration)
{
    std::vector<float> out;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return out;
    if (avformat_find_stream_info(fmt, nullptr) < 0) { avformat_close_input(&fmt); return out; }
    if (duration) *duration = fmt->duration > 0 ? fmt->duration / double(AV_TIME_BASE) : 0;
    const int si = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (si < 0) { avformat_close_input(&fmt); return out; }
    AVStream* st = fmt->streams[si];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext* dec = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!dec || avcodec_parameters_to_context(dec, st->codecpar) < 0 || avcodec_open2(dec, codec, nullptr) < 0) {
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return out;
    }
    if (start > 0) av_seek_frame(fmt, -1, int64_t(start * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);

    SwrContext* swr = nullptr;
    AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
    swr_alloc_set_opts2(&swr, &mono, AV_SAMPLE_FMT_FLT, kRate, &dec->ch_layout, dec->sample_fmt, dec->sample_rate, 0, nullptr);
    if (!swr || swr_init(swr) < 0) {
        swr_free(&swr);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return out;
    }
    const size_t want = size_t(length * kRate);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    std::vector<float> buf;
    bool done = false;
    while (!done && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == si && avcodec_send_packet(dec, pkt) >= 0) {
            while (avcodec_receive_frame(dec, frame) >= 0) {
                const double t = frame->pts != AV_NOPTS_VALUE ? frame->pts * av_q2d(st->time_base) : start;
                if (t + double(frame->nb_samples) / frame->sample_rate < start) continue;  // before the window
                const int cap = swr_get_out_samples(swr, frame->nb_samples);
                buf.resize(size_t(std::max(cap, 0)));
                uint8_t* dst = reinterpret_cast<uint8_t*>(buf.data());
                const int n = swr_convert(swr, &dst, cap, const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
                if (n > 0) out.insert(out.end(), buf.begin(), buf.begin() + n);
                if (out.size() >= want) { done = true; break; }
            }
        }
        av_packet_unref(pkt);
    }
    if (out.size() > want) out.resize(want);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    swr_free(&swr);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    return out;
}

void Fft(std::vector<std::complex<double>>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * 3.14159265358979323846 / double(len);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (size_t k = 0; k < len / 2; k++) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// Haitsma-Kalker: energies of 33 log-spaced bands (300-2000 Hz); bit m of item n is the sign of
// (E[n][m] - E[n][m+1]) - (E[n-1][m] - E[n-1][m+1]).
std::vector<uint32_t> Fingerprint(const std::vector<float>& pcm)
{
    std::vector<uint32_t> fp;
    if (pcm.size() < size_t(kFrame)) return fp;
    std::vector<double> window(kFrame);
    for (int i = 0; i < kFrame; i++) window[i] = 0.5 - 0.5 * std::cos(2 * 3.14159265358979323846 * i / (kFrame - 1));
    int edges[kBands + 1];
    for (int b = 0; b <= kBands; b++) {
        const double f = 300.0 * std::pow(2000.0 / 300.0, double(b) / kBands);
        edges[b] = int(f * kFrame / kRate);
    }
    std::vector<double> prev(kBands, 0.0), cur(kBands);
    std::vector<std::complex<double>> spec(kFrame);
    bool first = true;
    for (size_t pos = 0; pos + kFrame <= pcm.size(); pos += kHop) {
        for (int i = 0; i < kFrame; i++) spec[i] = pcm[pos + i] * window[i];
        Fft(spec);
        for (int b = 0; b < kBands; b++) {
            double e = 0;
            for (int k = edges[b]; k < std::max(edges[b + 1], edges[b] + 1); k++) e += std::norm(spec[k]);
            cur[b] = e;
        }
        if (!first) {
            uint32_t bits = 0;
            for (int m = 0; m < 32; m++) {
                const double d = (cur[m] - cur[m + 1]) - (prev[m] - prev[m + 1]);
                if (d > 0) bits |= 1u << m;
            }
            fp.push_back(bits);
        }
        first = false;
        prev.swap(cur);
    }
    return fp;
}

struct Segment { double startA, startB, length; };

std::optional<Segment> Shared(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b)
{
    std::unordered_map<uint32_t, std::vector<int>> index;
    for (int j = 0; j < int(b.size()); j++) index[b[j] >> 12].push_back(j);
    std::map<int, int> shifts;
    for (int i = 0; i < int(a.size()); i++) {
        auto it = index.find(a[i] >> 12);
        if (it != index.end()) for (int j : it->second) shifts[j - i]++;
    }
    std::vector<std::pair<int, int>> top(shifts.begin(), shifts.end());
    std::sort(top.begin(), top.end(), [](auto& x, auto& y) { return x.second > y.second; });
    if (top.size() > 30) top.resize(30);
    std::optional<Segment> best;
    auto consider = [&](int s, int e, int shift) {
        if (!best || (e - s) * kStep > best->length) best = Segment{s * kStep, (s + shift) * kStep, (e - s) * kStep};
    };
    for (auto& [shift, count] : top) {
        int runStart = -1, lastGood = -1;
        for (int i = std::max(0, -shift); i < int(a.size()) && i + shift < int(b.size()); i++) {
            if (__popcnt(a[i] ^ b[i + shift]) <= unsigned(kMaxBits)) {
                if (runStart < 0) runStart = i;
                lastGood = i;
            } else if (runStart >= 0 && i - lastGood > 12) {   // allow ~1.5 s of mismatch inside a run
                consider(runStart, lastGood, shift);
                runStart = -1;
            }
        }
        if (runStart >= 0) consider(runStart, lastGood, shift);
    }
    if (best && best->length < kMinLen) best.reset();
    return best;
}

struct Episode {
    std::wstring name;
    uint64_t size = 0;
    double duration = 0, tailStart = 0;
    std::vector<uint32_t> head, tail;
};

std::optional<std::pair<double, double>> Intersect(const std::vector<std::optional<std::pair<double, double>>>& v)
{
    std::optional<std::pair<double, double>> r;
    for (auto& s : v) {
        if (!s) continue;
        if (!r) r = s;
        else r = std::make_pair(std::max(r->first, s->first), std::min(r->second, s->second));
    }
    if (r && r->second - r->first < kMinLen) r.reset();
    return r;
}

std::wstring CacheFile(const std::wstring& folder)
{
    std::wstring key = folder;
    while (!key.empty() && (key.back() == L'\\' || key.back() == L'/')) key.pop_back();
    CharLowerBuffW(key.data(), DWORD(key.size()));
    uint64_t h = 1469598103934665603ull;  // FNV-1a of the lower-case folder path
    for (wchar_t c : key) { h ^= uint16_t(c); h *= 1099511628211ull; }
    wchar_t* base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        dir = std::wstring(base) + L"\\MPC-HC-series-player";
        CoTaskMemFree(base);
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\skip";
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t name[32];
    swprintf_s(name, L"\\%016llx.txt", (unsigned long long)h);
    return dir + name;
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        fwprintf(stderr, L"usage: mpc-introscan <folder>\n");
        return 2;
    }
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    // The FFmpeg DLLs are delay-loaded from the player's LAVFilters64 folder
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    PathRemoveFileSpecW(self);
    SetDllDirectoryW((std::wstring(self) + L"\\LAVFilters64").c_str());
    av_log_set_level(AV_LOG_QUIET);
    const std::wstring folder = argv[1];

    std::vector<Episode> eps;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((folder + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring n = fd.cFileName, ext = n.substr(n.find_last_of(L'.') + 1);
            CharLowerBuffW(ext.data(), DWORD(ext.size()));
            if (ext == L"mkv" || ext == L"mp4" || ext == L"avi" || ext == L"m4v" || ext == L"ts" || ext == L"webm") {
                Episode e;
                e.name = n;
                e.size = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
                eps.push_back(e);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(eps.begin(), eps.end(), [](auto& x, auto& y) { return StrCmpLogicalW(x.name.c_str(), y.name.c_str()) < 0; });
    if (eps.size() < 2) return 1;

    for (auto& e : eps) {
        const std::string path = Utf8(folder + L"\\" + e.name);
        e.head = Fingerprint(DecodeAudio(path, 0, kWindow, &e.duration));
        e.tailStart = std::max(0.0, e.duration - kWindow);
        e.tail = Fingerprint(DecodeAudio(path, e.tailStart, kWindow, nullptr));
    }

    std::string out;
    for (size_t k = 0; k < eps.size(); k++) {
        std::vector<std::optional<std::pair<double, double>>> ops, eds;
        for (size_t j : {k - 1, k + 1}) {
            if (j >= eps.size()) continue;  // k - 1 wraps to a huge value for k == 0
            if (auto s = Shared(eps[k].head, eps[j].head)) ops.push_back(std::make_pair(s->startA, s->startA + s->length));
            else ops.push_back(std::nullopt);
            if (auto s = Shared(eps[k].tail, eps[j].tail))
                eds.push_back(std::make_pair(eps[k].tailStart + s->startA, eps[k].tailStart + s->startA + s->length));
            else eds.push_back(std::nullopt);
        }
        auto op = Intersect(ops), ed = Intersect(eds);
        // Fingerprint windows smear the edges by up to ~2 s: shrink the segments so that no story is cut
        // (skipping a second less of the song is harmless, cutting a line of dialogue is not)
        if (op) { op->first += 1.0; op->second -= 1.5; }
        if (ed) { ed->first += 2.0; ed->second -= 1.0; }
        char line[160];
        sprintf_s(line, "\t%llu\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\n", (unsigned long long)eps[k].size,
                  op ? op->first : -1.0, op ? op->second : -1.0, ed ? ed->first : -1.0, ed ? ed->second : -1.0, eps[k].duration);
        out += Utf8(eps[k].name) + line;
        printf("%s", (Utf8(eps[k].name) + line).c_str());
    }

    const std::wstring cache = CacheFile(folder), tmp = cache + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") == 0 && f) {
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
        MoveFileExW(tmp.c_str(), cache.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    return 0;
}
