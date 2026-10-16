#pragma once

#include "features/OpenFlows.h"

namespace app {
// The same load flow with paths supplied by the host (the self-test never opens an OS dialog).
void LoadSubtitleFile(AppContext& ctx, const std::vector<std::wstring>& paths);
/// An app-modal dialog (NSAlert / NSOpenPanel, both NSWindow*) owned by `owner`: runModal leaves it at the normal level,
/// so it takes the owner's level (an always-on-top player or the mini player floats) and comes up in front of it, as
/// Windows keeps an owned dialog above its owner.
void RaiseDialogToOwner(void* dialogWindow, void* ownerWindow);
}
