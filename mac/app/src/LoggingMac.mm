#include "LoggingMac.h"

#import <Foundation/Foundation.h>

#include <filesystem>
#include <system_error>

#include "features/Logging.h"
#include "wvp/Platform.h"
#include "wvp/win32/Utf.h"

namespace mainview::logmac {

namespace {
bool g_debugLogging = false;

std::string Canonical(const std::string& p) {
    std::error_code ec;
    std::filesystem::path c = std::filesystem::weakly_canonical(std::filesystem::path(p), ec);
    std::string s = (ec ? std::filesystem::path(p) : c).lexically_normal().string();
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}
}

std::string LogsDir(const std::string& settingsFilePath) {
    const std::filesystem::path settingsDir = std::filesystem::path(settingsFilePath).parent_path();
    if (Canonical(settingsDir.string()) == Canonical(wvp::platform::AppDataDir())) {
        NSArray<NSString*>* lib = NSSearchPathForDirectoriesInDomains(NSLibraryDirectory, NSUserDomainMask, YES);
        NSString* base = lib.firstObject ?: [NSHomeDirectory() stringByAppendingPathComponent:@"Library"];
        return std::string([base stringByAppendingPathComponent:@"Logs/aribplayer"].UTF8String);
    }
    return (settingsDir / "logs").string();
}

std::string DebugLogPath(const std::string& dir) {
    // log::DebugLogPath joins with '\'; its file name part is the day's "aribplayer-YYYYMMDD.log".
    const std::wstring win = app::log::DebugLogPath(L"", app::log::NowStamp());
    const size_t sep = win.find_last_of(L"\\/");
    const std::string name = wvp::ToUtf8(sep == std::wstring::npos ? win : win.substr(sep + 1));
    return (std::filesystem::path(dir) / name).string();
}

void ApplyDebugLogging(bool on, const std::string& logsDir) {
    using namespace app::log;
    if (on == g_debugLogging && (!on || Global().HasFile(true))) return;
    g_debugLogging = on;
    if (on) {
        Global().OpenFile(wvp::ToWide(DebugLogPath(logsDir)), true);
        Event("log", "debug logging enabled: " + wvp::ToUtf8(Global().FilePath(true)));
    } else {
        Event("log", "debug logging disabled");
        Global().CloseFile(true);
    }
    InstallEngineHooks();
}

std::string CurrentDebugLogPath() { return wvp::ToUtf8(app::log::Global().FilePath(true)); }

}
