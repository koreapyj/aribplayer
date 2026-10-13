#include "puimac/KeyMap.h"

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>   // kVK_* virtual key codes (header only)

#include "pui/Geometry.h"

namespace puimac {

int VkFromMacKey(unsigned short keyCode, unsigned int ch) {
    using namespace pui::key;
    switch (keyCode) {
    case kVK_Return: case kVK_ANSI_KeypadEnter: return Enter;
    case kVK_Tab: return Tab;
    case kVK_Space: return Space;
    case kVK_Delete: return Back;
    case kVK_ForwardDelete: return Delete;
    case kVK_Escape: return Escape;
    case kVK_Home: return Home;
    case kVK_End: return End;
    case kVK_PageUp: return PageUp;
    case kVK_PageDown: return PageDown;
    case kVK_LeftArrow: return Left;
    case kVK_RightArrow: return Right;
    case kVK_UpArrow: return Up;
    case kVK_DownArrow: return Down;
    case kVK_F1: return 0x70;
    case kVK_F2: return F2;
    case kVK_F3: return 0x72;
    case kVK_F4: return F4;
    case kVK_F5: return 0x74;
    case kVK_F6: return F6;
    case kVK_F7: return 0x76;
    case kVK_F8: return 0x77;
    case kVK_F9: return 0x78;
    case kVK_F10: return F10;
    case kVK_F11: return 0x7A;
    case kVK_F12: return 0x7B;
    default: break;
    }
    if (ch >= 'a' && ch <= 'z') return 0x41 + (int)(ch - 'a');
    if (ch >= 'A' && ch <= 'Z') return 0x41 + (int)(ch - 'A');
    if (ch >= '0' && ch <= '9') return 0x30 + (int)(ch - '0');
    switch (ch) {   // VK_OEM_* of a US layout
    case ';': return 0xBA;
    case '=': return 0xBB;
    case ',': return 0xBC;
    case '-': return 0xBD;
    case '.': return 0xBE;
    case '/': return 0xBF;
    case '`': return 0xC0;
    case '[': return 0xDB;
    case '\\': return 0xDC;
    case ']': return 0xDD;
    case '\'': return 0xDE;
    default: return 0;
    }
}

unsigned ModsFromFlags(unsigned long long f) {
    unsigned m = pui::ModNone;
    if (f & NSEventModifierFlagShift) m |= pui::ModShift;
    if (f & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) m |= pui::ModCtrl;
    if (f & NSEventModifierFlagOption) m |= pui::ModAlt;
    return m;
}

}
