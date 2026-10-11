#pragma once
// Small helpers shared by the menus / filters / properties / playback-extras files: callback timers on the main window
// (ids 0x7A00.., never the shell's 1 and 2) and the process-wide "latest FILTER_ERROR" store.
#include <windows.h>

#include <functional>
#include <optional>
#include <string>

#include "wvp/Event.h"
#include "wvp/PlayerTypes.h"

namespace app::fx {

/// Timer with a callback (UI thread, dispatched by DispatchMessage without involving the window procedure).
/// repeat=false fires once. Returns the id for KillUiTimer (0 on failure).
UINT_PTR SetUiTimer(HWND hwnd, UINT ms, std::function<void()> fn, bool repeat);
void KillUiTimer(HWND hwnd, UINT_PTR id);

/// Latest FILTER_ERROR seen by InitPlaybackExtras (filter panel shows it inline).
const std::optional<wvp::FilterErrorInfo>& LastFilterError();
void SetFilterError(const wvp::FilterErrorInfo& e);
void ClearFilterError();
wvp::Event<>& FilterErrorChanged();
/// Readable text for an AVERROR code (local FFERRTAG / errno table), empty when unknown.
std::wstring AvErrorText(int code);
/// Name of the only filter of a one-filter graph ("[in]bogus_filter=a=1" -> "bogus_filter"), empty otherwise.
std::string FirstFilterName(const std::string& graph);
/// "Video filter failed: Invalid argument" / "Video filter failed: Filter not found ('bogus_filter')" / "Audio filter failed: ...".
std::wstring FormatFilterError(const wvp::FilterErrorInfo& e);

}
