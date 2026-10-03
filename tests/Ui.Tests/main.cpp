// Test runner. Usage: PlayerUi.Tests.exe [name-substring-filter] [--no-integration]
// PlayerUi tests are pure logic (no window) or use hidden windows only.
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

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

    int run = 0, failed = 0, skipped = 0;
    for (auto& t : wvptest::Registry()) {
        if (!filter.empty() && std::string(t.name).find(filter) == std::string::npos) continue;
        if (t.integration && noIntegration) { skipped++; continue; }
        std::printf("[ RUN  ] %s\n", t.name);
        std::fflush(stdout);
        wvptest::Cur() = {};
        try { t.fn(); }
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
