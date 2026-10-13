#pragma once

#include "features/OpenFlows.h"

namespace app {
// The same load flow with paths supplied by the host (the self-test never opens an OS dialog).
void LoadSubtitleFile(AppContext& ctx, const std::vector<std::wstring>& paths);
}
