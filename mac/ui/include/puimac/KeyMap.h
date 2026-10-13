#pragma once
// pui key codes are Win32 VK values (Geometry.h, namespace pui::key). The Mac host translates NSEvent key codes here.
namespace puimac {

/// Win32 virtual-key code for a macOS virtual key code (NSEvent.keyCode) and the event's charactersIgnoringModifiers
/// (first UTF-32 code point, 0 when none). 0 = no mapping. Letters/digits/punctuation follow the keyboard layout (like
/// Windows), navigation/function keys follow the hardware key code.
int VkFromMacKey(unsigned short keyCode, unsigned int firstChar);

/// pui::Mods from NSEvent.modifierFlags: Command and Control -> ModCtrl (the pui shortcuts are Ctrl-based on Windows,
/// Command-based here), Option -> ModAlt, Shift -> ModShift.
unsigned ModsFromFlags(unsigned long long nsModifierFlags);

}
