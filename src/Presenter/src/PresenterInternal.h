#pragma once
// Shared by the presenter and the platform: the core configuration from an adapter id and the options.
#include <string>

#include "presenter_core.h"
#include "wvp/win32/WindowsVideoPlatform.h"

namespace wvp::win32 {

// adapterId: "HHHHHHHH:LLLLLLLL"; "" = the default adapter. false with *error for an invalid id or option.
bool MakeConfig(const std::string& adapterId, const PresenterOptions& options, PrConfig& cfg, std::string* error);

}
