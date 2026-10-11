#include "features/Logging.h"

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <vector>

#include "wvp/FfPlayer.h"
#include "wvp/win32/Utf.h"

namespace app::log {

std::optional<int> ParseLevel(std::wstring_view name) {
    std::wstring l(name);
    for (auto& c : l) c = (wchar_t)towlower(c);
    static const std::pair<const wchar_t*, int> names[] = {{L"quiet", kQuiet}, {L"panic", kPanic}, {L"fatal", kFatal}, {L"error", kError},
                                                           {L"warning", kWarning}, {L"warn", kWarning}, {L"info", kInfo},
                                                           {L"verbose", kVerbose}, {L"debug", kDebug}, {L"trace", kTrace}};
    for (auto& [n, v] : names) if (l == n) return v;
    if (l.empty()) return std::nullopt;
    size_t i = l[0] == L'-' ? 1 : 0;
    if (i >= l.size()) return std::nullopt;
    for (size_t k = i; k < l.size(); k++) if (l[k] < L'0' || l[k] > L'9') return std::nullopt;
    if (l.size() - i > 4) return std::nullopt;
    return std::stoi(l);
}

const char* LevelTag(int level) {
    if (level <= kQuiet) return "QUIET";
    if (level <= kPanic) return "PANIC";
    if (level <= kFatal) return "FATAL";
    if (level <= kError) return "ERROR";
    if (level <= kWarning) return "WARN ";
    if (level <= kInfo) return "INFO ";
    if (level <= kVerbose) return "VERB ";
    if (level <= kDebug) return "DEBUG";
    return "TRACE";
}

Stamp NowStamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return {st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds};
}

std::string FormatLine(const Stamp& t, int level, std::string_view src, std::string_view msg) {
    char head[64];
    std::snprintf(head, sizeof head, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%s] ", t.y, t.mo, t.d, t.h, t.mi, t.s, t.ms, LevelTag(level));
    std::string s = head;
    s.append(src);
    s += ": ";
    s.append(msg);
    return s;
}

std::wstring DebugLogPath(const std::wstring& dir, const Stamp& t) {
    wchar_t name[48];
    std::swprintf(name, 48, L"aribplayer-%04d%02d%02d.log", t.y, t.mo, t.d);
    std::wstring d = dir;
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    return d + L"\\" + name;
}

bool IsStatusLine(std::string_view line) {
    return line.find("A-V:") != std::string_view::npos || line.find("M-V:") != std::string_view::npos ||
           line.find("V-A:") != std::string_view::npos;
}

std::wstring UsageText() {
    return L"ARIB Player\r\n"
           L"\r\n"
           L"Usage: aribplayer.exe [options] [file | url | playlist ...]\r\n"
           L"\r\n"
           L"Diagnostics:\r\n"
           L"  --console              Show log output in the console (attaches to the parent console, or opens one).\r\n"
           L"  --log-level=<level>    libffplay/FFmpeg log level: quiet, error, warning, info, verbose, debug, trace\r\n"
           L"                         (default: warning; info while --console/--log-file/--stats/Debug logging is on).\r\n"
           L"  --stats                Enable ffplay's periodic status line (A-V, fd, aq, vq, sq ...) at info level.\r\n"
           L"  --log-file=<path>      Append timestamped log lines (libffplay + app events) to <path>.\r\n"
           L"  --d3d11-debug          D3D11 debug layer on the presenter's devices (needs Graphics Tools); messages in the log.\r\n"
           L"  --d3d11-force-caps=<key=0,...>  Mask presenter capabilities to exercise fallbacks: nv12_srv, p010_srv,\r\n"
           L"                         decoder_srv, p010, shader.\r\n"
           L"  --d3d11-route=vp|shader  Force the D3D11 presenter route.\r\n"
           L"  --help, -h, /?         Show this text.\r\n"
           L"\r\n"
           L"Tab toggles the debug overlay (decoder, presenter output, frame path, filter graphs, sync) in Now playing, full screen\r\n"
           L"and the mini player. Settings > Advanced > Debug logging writes\r\n"
           L"%LOCALAPPDATA%\\aribplayer\\logs\\aribplayer-YYYYMMDD.log.\r\n"
           L"\r\n"
           L"Test modes:\r\n"
           L"  --logictest [filter]                         Run the built-in self tests (no window).\r\n"
           L"  --headless-check <media> [--result=<json>]   Hidden-window check against the real libffplay.\r\n";
}

struct Sink::Impl {
    std::mutex mu;
    std::ofstream files[2];
    std::wstring paths[2];
    HANDLE console = nullptr;
    ULONGLONG lastStatus = 0;
    int statusLines = 0;
};

Sink::Sink() : impl_(std::make_unique<Impl>()) {}
Sink::~Sink() = default;

bool Sink::OpenFile(const std::wstring& path, bool debugSlot) {
    Impl& i = *impl_;
    std::lock_guard<std::mutex> lk(i.mu);
    size_t slot = debugSlot ? 1 : 0;
    if (i.files[slot].is_open()) i.files[slot].close();
    i.files[slot].clear();
    i.paths[slot].clear();
    try {
        std::filesystem::path p(path);
        if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    } catch (...) {}
    i.files[slot].open(path, std::ios::app | std::ios::binary);
    if (!i.files[slot].is_open()) return false;
    i.paths[slot] = path;
    return true;
}

void Sink::CloseFile(bool debugSlot) {
    Impl& i = *impl_;
    std::lock_guard<std::mutex> lk(i.mu);
    size_t slot = debugSlot ? 1 : 0;
    if (i.files[slot].is_open()) i.files[slot].close();
    i.paths[slot].clear();
}

void Sink::SetConsole(HANDLE h) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->console = h;
}

bool Sink::Active() const {
    Impl& i = *impl_;
    std::lock_guard<std::mutex> lk(i.mu);
    return i.console || i.files[0].is_open() || i.files[1].is_open();
}

bool Sink::HasFile(bool debugSlot) const {
    Impl& i = *impl_;
    std::lock_guard<std::mutex> lk(i.mu);
    return i.files[debugSlot ? 1 : 0].is_open();
}

std::wstring Sink::FilePath(bool debugSlot) const {
    Impl& i = *impl_;
    std::lock_guard<std::mutex> lk(i.mu);
    return i.paths[debugSlot ? 1 : 0];
}

int Sink::StatusLinesWritten() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->statusLines;
}

void Sink::Write(int level, std::string_view src, std::string_view msg) {
    Impl& i = *impl_;
    std::lock_guard<std::mutex> lk(i.mu);
    if (!i.console && !i.files[0].is_open() && !i.files[1].is_open()) return;
    Stamp t = NowStamp();
    size_t pos = 0;
    while (pos <= msg.size()) {
        size_t nl = msg.find('\n', pos);
        std::string_view one = msg.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = nl == std::string_view::npos ? msg.size() + 1 : nl + 1;
        while (!one.empty() && (one.back() == '\r' || one.back() == '\n')) one.remove_suffix(1);
        // ffplay refreshes the status line with '\r': keep only the last segment
        if (size_t cr = one.rfind('\r'); cr != std::string_view::npos) one.remove_prefix(cr + 1);
        if (one.empty()) continue;
        if (IsStatusLine(one)) {
            ULONGLONG now = GetTickCount64();
            if (i.lastStatus && now - i.lastStatus < 500) continue;
            i.lastStatus = now;
            i.statusLines++;
        }
        std::string line = FormatLine(t, level, src, one);
        line += "\r\n";
        for (auto& f : i.files)
            if (f.is_open()) { f.write(line.data(), (std::streamsize)line.size()); f.flush(); }
        if (i.console) {
            DWORD mode = 0, written = 0;
            if (GetConsoleMode(i.console, &mode)) {
                std::wstring w = wvp::ToWide(line);
                WriteConsoleW(i.console, w.data(), (DWORD)w.size(), &written, nullptr);
            } else {
                WriteFile(i.console, line.data(), (DWORD)line.size(), &written, nullptr);
            }
        }
    }
}

void Sink::Flush() {
    Impl& i = *impl_;
    std::lock_guard<std::mutex> lk(i.mu);
    for (auto& f : i.files) if (f.is_open()) f.flush();
}

Sink& Global() {
    static Sink* s = new Sink();
    return *s;
}

namespace {
Settings g_settings;
bool g_consoleOpened = false;
bool g_engineHooked = false;
bool g_debugLogging = false;

bool Invalid(HANDLE h) { return !h || h == INVALID_HANDLE_VALUE; }
}

void Configure(const Settings& s) {
    g_settings = s;
    if (!s.file.empty()) Global().OpenFile(s.file, false);
}

const Settings& Current() { return g_settings; }

void OpenConsole() {
    if (g_consoleOpened) return;
    g_consoleOpened = true;
    bool needOut = Invalid(GetStdHandle(STD_OUTPUT_HANDLE)), needErr = Invalid(GetStdHandle(STD_ERROR_HANDLE));
    if (needOut || needErr) {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
        FILE* f = nullptr;
        if (needOut) freopen_s(&f, "CONOUT$", "w", stdout);
        if (needErr) freopen_s(&f, "CONOUT$", "w", stderr);
        HANDLE con = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (!Invalid(con)) {
            if (needOut) SetStdHandle(STD_OUTPUT_HANDLE, con);
            if (needErr) SetStdHandle(STD_ERROR_HANDLE, con);
        }
    }
    if (g_settings.console) Global().SetConsole(GetStdHandle(STD_ERROR_HANDLE));   // --console: log lines go to stderr
}

void PrintToConsole(const std::wstring& text) {
    OpenConsole();
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (Invalid(h)) return;
    DWORD mode = 0, written = 0;
    if (GetConsoleMode(h, &mode)) {
        WriteConsoleW(h, text.data(), (DWORD)text.size(), &written, nullptr);
    } else {
        std::string u = wvp::ToUtf8(text);
        WriteFile(h, u.data(), (DWORD)u.size(), &written, nullptr);
    }
}

int EffectiveLevel() {
    if (g_settings.level) return g_settings.stats ? std::max(*g_settings.level, kInfo) : *g_settings.level;
    if (Global().Active() || g_settings.stats) return kInfo;
    return kWarning;
}

std::string LogsDir(const std::string& settingsFilePath) {
    size_t i = settingsFilePath.find_last_of("\\/");
    return (i == std::string::npos ? std::string(".") : settingsFilePath.substr(0, i)) + "\\logs";
}

void ApplyDebugLogging(bool on, const std::string& logsDir) {
    if (on == g_debugLogging && (!on || Global().HasFile(true))) return;
    g_debugLogging = on;
    if (on) {
        Global().OpenFile(DebugLogPath(wvp::ToWide(logsDir), NowStamp()), true);
        Event("log", "debug logging enabled: " + wvp::ToUtf8(Global().FilePath(true)));
    } else {
        Event("log", "debug logging disabled");
        Global().CloseFile(true);
    }
    InstallEngineHooks();
}

void InstallEngineHooks() {
    if (Global().Active()) {
        wvp::FfPlayer::SetLogLevel(EffectiveLevel());
        wvp::FfPlayer::SetLogCallback([](int level, const std::string& line) { Global().Write(level, "av", line); });
        g_engineHooked = true;
    } else if (g_engineHooked) {
        RemoveEngineHooks();
    }
}

void RemoveEngineHooks() {
    if (!g_engineHooked) return;
    wvp::FfPlayer::SetLogCallback(nullptr);
    g_engineHooked = false;
    if (!Global().Active()) wvp::FfPlayer::SetLogLevel(EffectiveLevel());
}

void Event(std::string_view src, std::string_view msg, int level) {
    if (level > EffectiveLevel()) return;
    Global().Write(level, src, msg);
}

void EventW(std::string_view src, const std::wstring& msg, int level) { Event(src, wvp::ToUtf8(msg), level); }

}
