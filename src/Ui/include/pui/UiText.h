#pragma once
// Localised default texts of the UI library (InfoBar close name, switch On/Off, ...). The library has no language of its
// own: the host registers a chooser once (SetUiTextChooser), otherwise the English text is used.

namespace pui {

using UiTextChooser = const wchar_t* (*)(const wchar_t* en, const wchar_t* ko);

inline UiTextChooser& UiTextChooserRef() {
    static UiTextChooser c = nullptr;
    return c;
}
inline void SetUiTextChooser(UiTextChooser c) { UiTextChooserRef() = c; }
inline const wchar_t* UiText(const wchar_t* en, const wchar_t* ko) {
    UiTextChooser c = UiTextChooserRef();
    return c ? c(en, ko) : en;
}

}
