#pragma once
#include <string>
#include <utility>
// UI text in English and Korean, chosen once from the Windows UI language (Korean UI -> Korean, else English).
// Call sites keep both strings side by side: Tr(L"Playlist", L"재생목록").
// Tests can force a language with SetUiLanguage.

namespace app {

enum class UiLanguage { English, Korean };

/// Language used by Tr: the forced one if set, else detected from GetUserDefaultUILanguage on first use.
UiLanguage CurrentUiLanguage();
/// Forces the language (tests, diagnostics). Text already built keeps its old language.
void SetUiLanguage(UiLanguage lang);

inline const wchar_t* Tr(const wchar_t* en, const wchar_t* ko) {
    return CurrentUiLanguage() == UiLanguage::Korean ? ko : en;
}

/// Same choice for composed sentences where the word order differs: TrW(L"Added " + n, n + L" added").
inline std::wstring TrW(std::wstring en, std::wstring ko) {
    return CurrentUiLanguage() == UiLanguage::Korean ? std::move(ko) : std::move(en);
}

}
