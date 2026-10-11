#pragma once
// Process-wide diagnostic log: console (--console) and/or log files (--log-file=<path>, Settings > Advanced > Debug logging),
// fed by libffplay's av_log callback (FfPlayer::SetLogCallback, any thread) and by the app's own events. Thread-safe.
//
// Line format: "2026-10-04 12:34:56.789 [INFO ] src: message". Levels are AV_LOG_* values.
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include <windows.h>

namespace app::log {

constexpr int kQuiet = -8, kPanic = 0, kFatal = 8, kError = 16, kWarning = 24, kInfo = 32, kVerbose = 40, kDebug = 48, kTrace = 56;

/// quiet|panic|fatal|error|warning|info|verbose|debug|trace (case-insensitive) or a number.
std::optional<int> ParseLevel(std::wstring_view name);
const char* LevelTag(int level);   // 5-char tags: "ERROR", "WARN ", "INFO ", ...

struct Stamp { int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, ms = 0; };
Stamp NowStamp();
std::string FormatLine(const Stamp& t, int level, std::string_view src, std::string_view msg);
/// `dir\aribplayer-YYYYMMDD.log`
std::wstring DebugLogPath(const std::wstring& dir, const Stamp& t);
/// ffplay's "-stats" status line ("  12.31 A-V: -0.008 fd=   0 aq=   34KB vq=  890KB sq=    0B")?
bool IsStatusLine(std::string_view line);
std::wstring UsageText();

/// Thread-safe line sink: up to two files + the console handle. Cheap no-op while nothing is attached.
class Sink {
public:
    Sink();
    ~Sink();
    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;
    /// Appends to `path` (parent directories are created). false on failure. Slot 0 = --log-file, 1 = debug logging.
    bool OpenFile(const std::wstring& path, bool debugSlot);
    void CloseFile(bool debugSlot);
    void SetConsole(HANDLE h);   // nullptr detaches
    bool Active() const;
    bool HasFile(bool debugSlot) const;
    std::wstring FilePath(bool debugSlot) const;
    /// Writes one record per '\n'-separated line of `msg` (trailing CR/LF stripped, empties skipped). Status lines are
    /// throttled to 2 per second.
    void Write(int level, std::string_view src, std::string_view msg);
    void Flush();
    int StatusLinesWritten() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// The process-wide sink (leaked on purpose: log callbacks may arrive from other threads during shutdown).
Sink& Global();

struct Settings {
    bool console = false;
    bool stats = false;
    std::optional<int> level;
    std::wstring file;
};

/// Call from Main before the App exists.
void Configure(const Settings& s);
const Settings& Current();
/// Attaches to the parent console (AllocConsole fallback) for std streams that are not redirected, re-opens the CRT
/// stdout/stderr to CONOUT$ and hands the console to the sink. Idempotent.
void OpenConsole();
/// Prints to stdout (console or redirected). Also used for --help.
void PrintToConsole(const std::wstring& text);

/// Level libffplay is told to use: explicit, else info while any sink is active (stats forces at least info), else warning.
int EffectiveLevel();
/// `<folder of settings.json>\logs` (the log folder next to a settings file; tests pass temp paths).
std::string LogsDir(const std::string& settingsFilePath);
/// Settings > Advanced > Debug logging: opens/closes `<logsDir>\aribplayer-YYYYMMDD.log`.
void ApplyDebugLogging(bool on, const std::string& logsDir);
/// Installs the av_log callback + level (after libffplay.dll is loaded) when the sink is active; removes it otherwise.
void InstallEngineHooks();
void RemoveEngineHooks();

/// App events (info level unless stated). No-ops while the sink is inactive or the level filters them.
void Event(std::string_view src, std::string_view msg, int level = kInfo);
void EventW(std::string_view src, const std::wstring& msg, int level = kInfo);

}
