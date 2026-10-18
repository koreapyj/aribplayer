#pragma once
// Plain-text system clipboard (Win32 CF_UNICODETEXT / NSPasteboard string).
#include <string>

namespace pui {

std::wstring ClipboardText();
void SetClipboardText(const std::wstring& s);

}
