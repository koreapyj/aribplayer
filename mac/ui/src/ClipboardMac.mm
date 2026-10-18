// macOS build of pui::ClipboardText / SetClipboardText (src/Ui/include/pui/Clipboard.h) on NSPasteboard.
#include "pui/Clipboard.h"

#import <AppKit/AppKit.h>

namespace pui {

std::wstring ClipboardText() {
    std::wstring out;
    NSString* s = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
    if (!s) return out;
    NSData* d = [s dataUsingEncoding:NSUTF32LittleEndianStringEncoding];
    out.assign(static_cast<const wchar_t*>(d.bytes), d.length / sizeof(wchar_t));
    return out;
}

void SetClipboardText(const std::wstring& s) {
    NSString* str = [[NSString alloc] initWithBytes:s.data() length:s.size() * sizeof(wchar_t) encoding:NSUTF32LittleEndianStringEncoding];
    NSPasteboard* pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    if (str) [pb setString:str forType:NSPasteboardTypeString];
}

}
