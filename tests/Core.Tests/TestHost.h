#pragma once
// Process setup the test runner needs before any test (and before libffplay/SDL initialise). One implementation per
// platform in platform/<os>/TestHost.cpp.
namespace wvptest {

/// Silences failure dialogs, forces the dummy SDL audio driver, and applies any OS timer settings the timing tests rely on.
void HostInit();

}
