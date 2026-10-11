#include "features/FxProps.h"
#include "Localize.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>

#include "features/FxModel.h"
#include "wvp/win32/Utf.h"

namespace app::fx {

namespace {
std::wstring Fmt(const wchar_t* f, double v) {
    wchar_t b[64];
    swprintf(b, 64, f, v);
    return b;
}
std::wstring Group3(std::int64_t v) {
    std::wstring s = std::to_wstring(v), r;
    int c = 0;
    for (size_t i = s.size(); i-- > 0;) {
        r.insert(r.begin(), s[i]);
        if (++c % 3 == 0 && i > 0) r.insert(r.begin(), L',');
    }
    return r;
}
std::wstring FileName(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? p : p.substr(i + 1);
}
}

std::wstring FormatDurationText(double s) {
    if (!(s > 0) || !std::isfinite(s)) return Tr(L"Unknown", L"알 수 없음");
    long long t = (long long)s;
    long long h = t / 3600, m = (t / 60) % 60, sec = t % 60;
    wchar_t b[32];
    if (h > 0) swprintf(b, 32, L"%lld:%02lld:%02lld", h, m, sec);
    else swprintf(b, 32, L"%lld:%02lld", m, sec);
    return b;
}

std::wstring FormatBitRateText(std::int64_t bps) {
    if (bps <= 0) return Tr(L"Unknown", L"알 수 없음");
    if (bps >= 1000000) return Fmt(L"%.1f", bps / 1e6) + L" Mbps";
    return std::to_wstring((bps + 500) / 1000) + L" kbps";
}

std::wstring FormatFileSizeText(std::int64_t b) {
    if (b < 0) return Tr(L"Unknown", L"알 수 없음");
    if (b < 1024) return std::to_wstring(b) + Tr(L" bytes", L" 바이트");
    double v = (double)b;
    if (b < 1024 * 1024) return Fmt(L"%.1f", v / 1024) + L" KB (" + Group3(b) + L" bytes)";
    if (b < 1024LL * 1024 * 1024) return Fmt(L"%.1f", v / (1024.0 * 1024)) + L" MB (" + Group3(b) + L" bytes)";
    return Fmt(L"%.2f", v / (1024.0 * 1024 * 1024)) + L" GB (" + Group3(b) + L" bytes)";
}

std::wstring FormatResolution(int w, int h) { return std::to_wstring(w) + L" x " + std::to_wstring(h); }

std::wstring FormatSar(int n, int d) {
    if (n <= 0 || d <= 0) return L"1:1";
    int g = std::gcd(n, d);
    return std::to_wstring(n / g) + L":" + std::to_wstring(d / g);
}

std::wstring FormatDar(int w, int h, int sn, int sd) {
    if (w <= 0 || h <= 0) return Tr(L"Unknown", L"알 수 없음");
    if (sn <= 0 || sd <= 0) { sn = 1; sd = 1; }
    long long a = (long long)w * sn, b = (long long)h * sd;
    long long g = std::gcd(a, b);
    double r = (double)a / (double)b;
    struct C { int n, d; };
    static const C common[] = {{16, 9}, {4, 3}, {21, 9}, {16, 10}, {1, 1}, {3, 2}, {5, 4}, {185, 100}, {235, 100}, {239, 100}};
    for (auto& c : common) {
        double cr = (double)c.n / c.d;
        if (std::fabs(r - cr) / cr < 0.01) {
            if (c.d == 100) return Fmt(L"%.2f", cr) + L":1";
            return std::to_wstring(c.n) + L":" + std::to_wstring(c.d);
        }
    }
    if (a / g <= 99 && b / g <= 99) return std::to_wstring(a / g) + L":" + std::to_wstring(b / g);
    return Fmt(L"%.2f", r) + L":1";
}

std::wstring FormatFrameRate(double fps) {
    if (!(fps > 0)) return Tr(L"Unknown", L"알 수 없음");
    std::wstring t = Fmt(L"%.3f", fps);
    while (!t.empty() && t.back() == L'0') t.pop_back();
    if (!t.empty() && t.back() == L'.') t.pop_back();
    return t + L" fps";
}

std::wstring FormatChannels(int c) {
    switch (c) {
    case 1: return Tr(L"Mono", L"모노");
    case 2: return Tr(L"Stereo", L"스테레오");
    case 6: return L"5.1";
    case 8: return L"7.1";
    default: return c > 0 ? std::to_wstring(c) + Tr(L" channels", L"채널") : Tr(L"Unknown", L"알 수 없음");
    }
}

std::wstring FormatSampleRate(int hz) {
    if (hz <= 0) return Tr(L"Unknown", L"알 수 없음");
    if (hz % 1000 == 0) return std::to_wstring(hz / 1000) + L" kHz";
    return Fmt(L"%.1f", hz / 1000.0) + L" kHz";
}

bool IsLocalPath(const std::wstring& s) {
    if (s.size() >= 3 && s[1] == L':' && (s[2] == L'\\' || s[2] == L'/')) return true;
    return s.size() >= 2 && s[0] == L'\\' && s[1] == L'\\';
}

std::vector<PropSection> BuildProperties(const wvp::MediaInfo& info, std::optional<std::int64_t> size) {
    std::vector<PropSection> out;
    PropSection file{Tr(L"File", L"파일"), {}};
    const std::wstring url = wvp::ToWide(info.Url);
    std::wstring name = !info.Title.empty() ? wvp::ToWide(info.Title) : FileName(url);
    if (!name.empty()) file.rows.push_back({Tr(L"Title", L"제목"), name});
    if (!info.Url.empty()) file.rows.push_back({IsLocalPath(url) ? Tr(L"Path", L"경로") : Tr(L"URL", L"주소"), url});
    if (!info.Format.empty()) file.rows.push_back({Tr(L"Container", L"컨테이너"), wvp::ToWide(info.Format)});
    file.rows.push_back({Tr(L"Duration", L"재생 시간"), FormatDurationText(info.Duration)});
    file.rows.push_back({Tr(L"Bit rate", L"비트 전송률"), FormatBitRateText(info.BitRate)});
    if (size) file.rows.push_back({Tr(L"Size", L"크기"), FormatFileSizeText(*size)});
    file.rows.push_back({Tr(L"Seekable", L"탐색 가능"), info.Seekable ? Tr(L"Yes", L"예") : Tr(L"No", L"아니요")});
    out.push_back(std::move(file));

    auto addStreams = [&](wvp::StreamType t, const wchar_t* title, auto&& rowsFor) {
        auto list = info.Of(t);
        if (list.empty()) return;
        PropSection sec{title, {}};
        for (size_t i = 0; i < list.size(); i++) {
            const auto& s = list[i];
            if (list.size() > 1) sec.rows.push_back({Tr(L"Stream ", L"스트림 ") + std::to_wstring(i + 1), s.Selected ? Tr(L"(playing)", L"(재생 중)") : L""});
            rowsFor(sec, s);
            std::wstring lang = LanguageName(s.Language);
            if (lang.empty() && !s.Language.empty() && s.Language != "und") lang = wvp::ToWide(s.Language);
            if (!lang.empty()) sec.rows.push_back({Tr(L"Language", L"언어"), lang});
            if (!s.Title.empty()) sec.rows.push_back({Tr(L"Stream title", L"스트림 제목"), wvp::ToWide(s.Title)});
        }
        out.push_back(std::move(sec));
    };
    addStreams(wvp::StreamType::Video, Tr(L"Video", L"영상"), [](PropSection& sec, const wvp::StreamInfo& s) {
        sec.rows.push_back({Tr(L"Codec", L"코덱"), wvp::ToWide(s.Codec)});
        sec.rows.push_back({Tr(L"Resolution", L"해상도"), FormatResolution(s.Width, s.Height)});
        sec.rows.push_back({Tr(L"Pixel aspect (SAR)", L"픽셀 가로세로 비 (SAR)"), FormatSar(s.SarNum, s.SarDen)});
        sec.rows.push_back({Tr(L"Display aspect (DAR)", L"화면 가로세로 비 (DAR)"), FormatDar(s.Width, s.Height, s.SarNum, s.SarDen)});
        sec.rows.push_back({Tr(L"Frame rate", L"프레임 속도"), FormatFrameRate(s.Fps)});
        if (!s.PixFmt.empty()) sec.rows.push_back({Tr(L"Pixel format", L"픽셀 형식"), wvp::ToWide(s.PixFmt)});
    });
    addStreams(wvp::StreamType::Audio, Tr(L"Audio", L"소리"), [](PropSection& sec, const wvp::StreamInfo& s) {
        sec.rows.push_back({Tr(L"Codec", L"코덱"), wvp::ToWide(s.Codec)});
        sec.rows.push_back({Tr(L"Sample rate", L"샘플 속도"), FormatSampleRate(s.SampleRate)});
        sec.rows.push_back({Tr(L"Channels", L"채널 수"), FormatChannels(s.Channels)});
    });
    addStreams(wvp::StreamType::Subtitle, Tr(L"Subtitles", L"자막"), [](PropSection& sec, const wvp::StreamInfo& s) {
        sec.rows.push_back({Tr(L"Codec", L"코덱"), wvp::ToWide(s.Codec)});
    });
    return out;
}

}
