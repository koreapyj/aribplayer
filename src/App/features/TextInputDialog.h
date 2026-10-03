#pragma once
// Modal text prompt backed by a separate owned dialog window with a native EDIT control (never drawn by us, never under
// the composition layers).
#include <windows.h>

#include <functional>
#include <optional>
#include <string>

#include "Localize.h"
#include "pui/Theme.h"

namespace app {
class AppContext;

struct TextPromptOptions {
    std::wstring title, label, initial, okText = Tr(L"OK", L"확인"), placeholder;
    std::function<std::wstring(const std::wstring&)> validate;   // returns error text or empty
};

/// Blocks (nested message loop) until OK/Cancel; nullopt = cancelled.
std::optional<std::wstring> PromptText(AppContext& ctx, const TextPromptOptions& options);

struct TextDialogRect {
    int x = 0, y = 0, w = 0, h = 0;
    int right() const { return x + w; }
    int bottom() const { return y + h; }
    bool operator==(const TextDialogRect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};
struct TextDialogLayout {
    int clientW = 0, clientH = 0;
    TextDialogRect label, edit, error, ok, cancel;
};
/// Client-area layout in device pixels for `dpi`. labelHeightPx = measured (wrapped) label height at that width.
TextDialogLayout ComputeTextDialogLayout(int dpi, int labelHeightPx);

struct PromptState {
    bool okEnabled = false;
    std::wstring error;   // text to show (empty = none)
};
/// Live validation: validate==null accepts any non-blank text; an invalid empty text disables OK but shows no error yet.
PromptState EvaluatePrompt(const std::function<std::wstring(const std::wstring&)>& validate, const std::wstring& text);

struct DialogColors {
    COLORREF background, editBackground, text, secondaryText, error;
};
DialogColors ComputeDialogColors(const pui::Theme& theme);

/// Test hook: builds the dialog window hidden (never shown, no loop) and returns it; owner matches the production dialog path.
HWND CreateTextDialogWindowHiddenForTest(const TextPromptOptions& options, const pui::Theme& theme, int dpi, HWND owner);
void DestroyTextDialogWindowHiddenForTest(HWND hwnd);
}
