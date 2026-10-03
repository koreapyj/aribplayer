#pragma once
// AppContext::SetMiniPlayer updates the mode state and then calls these hooks.
#include "ShellLogic.h"

namespace app {
class AppContext;
/// Drops the caption but keeps the resize border. Idempotent.
void EnterMiniPlayer(AppContext& ctx);
void ExitMiniPlayer(AppContext& ctx);

}
