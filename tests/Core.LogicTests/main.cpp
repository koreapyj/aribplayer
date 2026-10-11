// Test runner. Usage: PlayerCore.LogicTests [name-substring-filter]
#include <cstdio>
#include <exception>
#include <string>

#ifdef _WIN32
#include "wvp/win32/TestGuard.h"
#endif
#include "wvptest.h"

int main(int argc, char** argv) {
#ifdef _WIN32
    wvp::SilenceFailureDialogs();
#endif
    std::string filter;
    if (argc > 1) filter = argv[1];

    int run = 0, failed = 0;
    for (auto& t : wvptest::Registry()) {
        if (!filter.empty() && std::string(t.name).find(filter) == std::string::npos) continue;
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
    std::printf("\n%d test(s) run, %d failed\n", run, failed);
    return failed ? 1 : 0;
}
