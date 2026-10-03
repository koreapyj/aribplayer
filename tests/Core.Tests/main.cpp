// Test runner. Usage: PlayerCore.Tests.exe [name-substring-filter] [--no-integration]
// Integration tests (libffplay) force SDL_AUDIODRIVER=dummy and use a hidden host window; nothing is shown.
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#include <windows.h>

#include "wvp/TestGuard.h"
#include "wvptest.h"

int main(int argc, char** argv) {
    wvp::SilenceFailureDialogs();
    std::string filter;
    bool noIntegration = false;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--no-integration") == 0) noIntegration = true;
        else filter = argv[i];
    }
    // Must be set before libffplay/SDL initialise (SDL reads it through the CRT environment).
    _putenv_s("SDL_AUDIODRIVER", "dummy");
    _putenv_s("SDL_AUDIO_DRIVER", "dummy");
    // The dummy audio driver paces playback with SDL_Delay, so the 1 ms timer resolution SDL requests decides the
    // audio master clock rate. Windows 11 may ignore that request for a process whose windows are hidden and whose
    // audio is inaudible (both true here), which made long runs play at ~91 % speed (Sleep(42) -> 46.9 ms) and
    // failed the timing tests. Always honour the request, like a visible app with real audio gets.
    // WVP_TEST_DEFAULT_TIMER=1 keeps the Windows default, to reproduce that.
    char* keepDefault = nullptr;
    size_t keepLen = 0;
    const bool optOut = !(_dupenv_s(&keepDefault, &keepLen, "WVP_TEST_DEFAULT_TIMER") == 0 && keepDefault && keepDefault[0] == '1');
    std::free(keepDefault);
    if (optOut) {
        PROCESS_POWER_THROTTLING_STATE timer{};
        timer.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        timer.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        timer.StateMask = 0;   // 0 = do not ignore: timer resolution requests are always honoured
        if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &timer, sizeof(timer)))
            std::printf("[timer] SetProcessInformation(IGNORE_TIMER_RESOLUTION off) failed: %lu\n", GetLastError());
    }
    std::printf("[timer] honour timer resolution requests: %s\n", optOut ? "always" : "Windows default");

    int run = 0, failed = 0, skipped = 0;
    for (auto& t : wvptest::Registry()) {
        if (!filter.empty() && std::string(t.name).find(filter) == std::string::npos) continue;
        if (t.integration && noIntegration) { skipped++; continue; }
        std::printf("[ RUN  ] %s\n", t.name);
        std::fflush(stdout);
        wvptest::Cur() = {};
        try { t.fn(); }
        catch (const wvptest::SkipFailure& e) {
            skipped++;
            std::printf("[ SKIP ] %s: %s\n", t.name, e.reason.c_str());
            std::fflush(stdout);
            continue;
        }
        catch (const wvptest::RequireFailure&) {}
        catch (const std::exception& e) { wvptest::Fail(__FILE__, __LINE__, std::string("exception: ") + e.what()); }
        catch (...) { wvptest::Fail(__FILE__, __LINE__, "unknown exception"); }
        run++;
        if (wvptest::Cur().failures) { failed++; std::printf("[ FAIL ] %s\n", t.name); }
        else std::printf("[  OK  ] %s\n", t.name);
        std::fflush(stdout);
    }
    std::printf("\n%d test(s) run, %d failed, %d skipped\n", run, failed, skipped);
    return failed ? 1 : 0;
}
