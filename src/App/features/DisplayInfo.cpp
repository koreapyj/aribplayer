#include "features/DisplayInfo.h"

#include <dwmapi.h>

#include <cmath>

namespace app::disp {

double RateFromRatio(unsigned numerator, unsigned denominator) {
    if (numerator == 0 || denominator == 0) return 0;
    return (double)numerator / (double)denominator;
}

double RefreshRateHz(HWND hwnd) {
    HMONITOR mon = MonitorFromWindow(hwnd && IsWindow(hwnd) ? hwnd : nullptr, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    if (mon && GetMonitorInfoW(mon, &mi)) {
        DEVMODEW dm{};
        dm.dmSize = sizeof dm;
        // 0 and 1 mean "hardware default" and carry no rate.
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) {
            double hz = (double)dm.dmDisplayFrequency;
            // dmDisplayFrequency is an integer (60 for 59.94): refine with DWM when it agrees within 1 Hz.
            DWM_TIMING_INFO ti{};
            ti.cbSize = sizeof ti;
            if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti))) {
                double d = RateFromRatio(ti.rateRefresh.uiNumerator, ti.rateRefresh.uiDenominator);
                if (d > 0 && std::fabs(d - hz) < 1.0) hz = d;
            }
            return hz;
        }
    }
    DWM_TIMING_INFO ti{};
    ti.cbSize = sizeof ti;
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti))) {
        double d = RateFromRatio(ti.rateRefresh.uiNumerator, ti.rateRefresh.uiDenominator);
        if (d > 0) return d;
    }
    if (HDC dc = GetDC(nullptr)) {
        int v = GetDeviceCaps(dc, VREFRESH);
        ReleaseDC(nullptr, dc);
        if (v > 1) return (double)v;
    }
    return 0;
}

}
