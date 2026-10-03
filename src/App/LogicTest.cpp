#include "LogicTest.h"
#include "Localize.h"
#include "features/DialogGuard.h"

#include <exception>

namespace app::lt {

int RunAll(const std::string& filter) {
    SetUiLanguage(UiLanguage::English);   // tests compare the English texts, whatever the Windows UI language is
    OsDialogsBlocked() = true;            // tests never show an OS dialog on the desktop
    int run = 0, failed = 0;
    for (const Case& c : Registry()) {
        if (!filter.empty() && std::string(c.name).find(filter) == std::string::npos) continue;
        Cur() = {};
        try { c.fn(); }
        catch (const std::exception& e) { Fail(__FILE__, __LINE__, std::string("exception: ") + e.what()); }
        catch (...) { Fail(__FILE__, __LINE__, "unknown exception"); }
        run++;
        if (Cur().failures) { failed++; std::printf("FAIL  %s\n", c.name); }
        else std::printf("PASS  %s\n", c.name);
        std::fflush(stdout);
    }
    std::printf("\nlogictest: %d case(s), %d passed, %d failed\n", run, run - failed, failed);
    return failed ? 1 : 0;
}

}
