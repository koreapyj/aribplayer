#pragma once
// SettingsPage's native-dialog declarations, not a Win32 ABI. SettingsWindowMac installs asynchronous pui
// font/colour handlers; an unadapted page treats the native entry points as Cancel and never opens an OS dialog.
#include "windows.h"

using LPCFHOOKPROC = UINT_PTR (CALLBACK *)(HWND, UINT, WPARAM, LPARAM);
struct CHOOSEFONTW {
    DWORD lStructSize;
    HWND hwndOwner;
    LOGFONTW* lpLogFont;
    DWORD Flags;
    LPCFHOOKPROC lpfnHook;
    COLORREF rgbColors;
    int nSizeMin, nSizeMax, iPointSize;
};
struct CHOOSECOLORW {
    DWORD lStructSize;
    HWND hwndOwner;
    COLORREF rgbResult;
    COLORREF* lpCustColors;
    DWORD Flags;
};
enum : DWORD {
    CF_SCREENFONTS = 0x1, CF_ENABLEHOOK = 0x8, CF_INITTOLOGFONTSTRUCT = 0x40, CF_EFFECTS = 0x100,
    CF_FORCEFONTEXIST = 0x10000, CF_LIMITSIZE = 0x2000, CF_NOSCRIPTSEL = 0x800000, CF_NOVERTFONTS = 0x1000000,
    CC_RGBINIT = 0x1, CC_FULLOPEN = 0x2,
};
inline int ChooseFontW(CHOOSEFONTW*) { return 0; }
inline int ChooseColorW(CHOOSECOLORW*) { return 0; }
