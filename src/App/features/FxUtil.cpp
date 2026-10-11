#include "features/FxUtil.h"
#include "Localize.h"

#include <unordered_map>

#include "wvp/win32/Utf.h"

namespace app::fx {

namespace {
struct TimerEntry { std::function<void()> fn; bool repeat; };
std::unordered_map<UINT_PTR, TimerEntry>& Timers() { static std::unordered_map<UINT_PTR, TimerEntry> t; return t; }
UINT_PTR g_nextId = 0x7A00;

void CALLBACK TimerProc(HWND hwnd, UINT, UINT_PTR id, DWORD) {
    auto it = Timers().find(id);
    if (it == Timers().end()) { KillTimer(hwnd, id); return; }
    auto fn = it->second.fn;
    if (!it->second.repeat) { KillTimer(hwnd, id); Timers().erase(it); }
    if (fn) fn();
}

std::optional<wvp::FilterErrorInfo> g_lastError;
}

UINT_PTR SetUiTimer(HWND hwnd, UINT ms, std::function<void()> fn, bool repeat) {
    if (!hwnd) return 0;
    UINT_PTR id = g_nextId++;
    if (g_nextId > 0x7FFF) g_nextId = 0x7A00;
    Timers()[id] = {std::move(fn), repeat};
    if (!SetTimer(hwnd, id, ms, TimerProc)) { Timers().erase(id); return 0; }
    return id;
}

void KillUiTimer(HWND hwnd, UINT_PTR id) {
    if (!id) return;
    if (hwnd) KillTimer(hwnd, id);
    Timers().erase(id);
}

const std::optional<wvp::FilterErrorInfo>& LastFilterError() { return g_lastError; }
void SetFilterError(const wvp::FilterErrorInfo& e) { g_lastError = e; FilterErrorChanged().raise(); }
void ClearFilterError() { if (g_lastError) { g_lastError.reset(); FilterErrorChanged().raise(); } }
wvp::Event<>& FilterErrorChanged() { static wvp::Event<> e; return e; }

namespace {
constexpr int FfTag(unsigned a, unsigned b, unsigned c, unsigned d) { return -(int)(a | (b << 8) | (c << 16) | (d << 24)); }
}

std::wstring AvErrorText(int code) {
    struct E { int code; const wchar_t* text; };
    static const E table[] = {
        // FFERRTAG values from libavutil/error.h
        {FfTag(0xF8, 'B', 'S', 'F'), L"Bitstream filter not found"},
        {FfTag('B', 'U', 'G', '!'), L"Internal bug"},
        {FfTag('B', 'U', 'F', 'S'), L"Buffer too small"},
        {FfTag(0xF8, 'D', 'E', 'C'), L"Decoder not found"},
        {FfTag(0xF8, 'D', 'E', 'M'), L"Demuxer not found"},
        {FfTag(0xF8, 'E', 'N', 'C'), L"Encoder not found"},
        {FfTag('E', 'O', 'F', ' '), L"End of file"},
        {FfTag('E', 'X', 'I', 'T'), L"Immediate exit requested"},
        {FfTag('E', 'X', 'T', ' '), L"Generic error in an external library"},
        {FfTag(0xF8, 'F', 'I', 'L'), L"Filter not found"},
        {FfTag('I', 'N', 'D', 'A'), L"Invalid data found"},
        {FfTag(0xF8, 'M', 'U', 'X'), L"Muxer not found"},
        {FfTag(0xF8, 'O', 'P', 'T'), L"Option not found"},
        {FfTag('P', 'A', 'W', 'E'), L"Not implemented in FFmpeg"},
        {FfTag(0xF8, 'P', 'R', 'O'), L"Protocol not found"},
        {FfTag(0xF8, 'S', 'T', 'R'), L"Stream not found"},
        {FfTag('U', 'N', 'K', 'N'), L"Unknown error"},
        // AVERROR(errno), MSVC errno values
        {-2, L"No such file or directory"},
        {-5, L"Input/output error"},
        {-11, L"Resource temporarily unavailable"},
        {-12, L"Out of memory"},
        {-22, L"Invalid argument"},
        {-33, L"Argument out of domain"},
        {-34, L"Result too large"},
        {-40, L"Function not implemented"},
    };
    for (const auto& e : table) if (e.code == code) return e.text;
    return L"";
}

std::string FirstFilterName(const std::string& graph) {
    // "[in]name@label=args,next" -> "name"; empty when the graph chains several filters (the culprit is then unknown).
    int depth = 0, pieces = 0;
    std::string first;
    std::string cur;
    for (size_t i = 0; i <= graph.size(); i++) {
        char ch = i < graph.size() ? graph[i] : ',';
        if (i < graph.size() && ch == '\\') { cur.push_back(ch); if (i + 1 < graph.size()) cur.push_back(graph[++i]); continue; }
        if (i < graph.size() && ch == '[') depth++;
        if (i < graph.size() && ch == ']' && depth > 0) depth--;
        if ((ch == ',' || ch == ';') && depth == 0) {
            if (cur.find_first_not_of(" \t") != std::string::npos) {
                if (++pieces == 1) first = cur;
            }
            cur.clear();
        } else cur.push_back(ch);
    }
    if (pieces != 1) return "";
    size_t b = 0;
    while (b < first.size() && (first[b] == '[' || first[b] == ' ')) {
        if (first[b] == '[') { size_t e = first.find(']', b); if (e == std::string::npos) return ""; b = e + 1; }
        else b++;
    }
    size_t e = first.find_first_of("=@[ ", b);
    return first.substr(b, e == std::string::npos ? std::string::npos : e - b);
}

std::wstring FormatFilterError(const wvp::FilterErrorInfo& e) {
    std::wstring t = e.Type == "audio" ? Tr(L"Audio filter failed", L"소리 필터 실패") : Tr(L"Video filter failed", L"영상 필터 실패");
    std::wstring what = AvErrorText(e.Code);
    if (what.empty()) what = wvp::ToWide(e.Message);
    bool known = !what.empty();
    if (known) {
        t += L": " + what;
        if ((e.Code == FfTag(0xF8, 'F', 'I', 'L') || e.Code == FfTag(0xF8, 'O', 'P', 'T'))) {
            std::wstring n = wvp::ToWide(FirstFilterName(e.Filter));
            if (!n.empty()) t += L" ('" + n + L"')";
        }
    }
    if (!known && e.Code != 0) t += Tr(L" (error ", L" (오류 ") + std::to_wstring(e.Code) + L")";
    return t;
}

}
