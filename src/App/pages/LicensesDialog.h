#pragma once
// "Open source licenses": a modal in-app dialog (scrollable, read-only) showing THIRD-PARTY-NOTICES.txt, which is embedded in
// the exe as an RCDATA resource (IDR_THIRD_PARTY_NOTICES).
#include <string>

#include "pui/Widget.h"

namespace app {

/// The embedded notices text, converted from UTF-8 (BOM stripped); empty when the resource is missing.
std::wstring LoadThirdPartyNotices();

/// Opens the dialog as a modal popup in `host`; Close button / Esc closes it (Esc is consumed here).
void ShowLicensesDialog(pui::UiHost& host, pui::IRenderer& measurer);

}
