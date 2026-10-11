// Test runner. Usage: PlayerCore.Tests.exe [name-substring-filter] [--no-integration]
// Integration tests (libffplay) force SDL_AUDIODRIVER=dummy and use a hidden host window; nothing is shown.
// OS-specific process setup (failure dialogs, timer resolution, environment) is in platform/<os>/TestHost.cpp.
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

#include "TestHost.h"
#include "wvptest.h"

int main(int argc, char** argv) {
    wvptest::HostInit();
    std::string filter;
    bool noIntegration = false;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--no-integration") == 0) noIntegration = true;
        else filter = argv[i];
    }

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
