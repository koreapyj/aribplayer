#include "TestHost.h"

#include <cstdio>
#include <cstdlib>

#include <windows.h>

#include "wvp/TestGuard.h"

namespace wvptest {

void HostInit() {
    wvp::SilenceFailureDialogs();
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
}

}
