#pragma once
// Refresh rate of the monitor that hosts a window (for the debug overlay's fps line and judder-cadence notes).
#include <windows.h>

namespace app::disp {

/// Current refresh rate in Hz of the monitor hosting `hwnd` (nearest monitor; the primary one when hwnd is null or
/// invalid): EnumDisplaySettingsW(dmDisplayFrequency) of that monitor's device, falling back to
/// DwmGetCompositionTimingInfo (fractional rates such as 59.94) and then GetDeviceCaps(VREFRESH). 0 when unknown.
double RefreshRateHz(HWND hwnd);

/// Pure: DWM timing rate (numerator/denominator, rateRefresh) -> Hz; 0 for an invalid ratio.
double RateFromRatio(unsigned numerator, unsigned denominator);

}
