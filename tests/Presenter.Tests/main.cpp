// Test runner of the D3D11 presenter library. Usage: PlayerPresenter.Tests.exe [name-substring-filter]
// Every test plays through the presenter on a hidden host window with SDL's dummy audio driver: nothing is shown or heard.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#include <windows.h>

#include "wvp/win32/TestGuard.h"
#include "wvptest.h"

int main(int argc, char** argv) {
    wvp::SilenceFailureDialogs();
    std::string filter;
    for (int i = 1; i < argc; i++) filter = argv[i];
    _putenv_s("SDL_AUDIODRIVER", "dummy");
    // Hidden windows and inaudible audio may make Windows ignore the 1 ms timer resolution SDL asks for (the dummy driver
    // paces playback with SDL_Delay): always honour it.
    PROCESS_POWER_THROTTLING_STATE timer{};
    timer.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    timer.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    timer.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &timer, sizeof(timer));

    int run = 0, failed = 0, skipped = 0;
    for (auto& t : wvptest::Registry()) {
        if (!filter.empty() && std::string(t.name).find(filter) == std::string::npos) continue;
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
