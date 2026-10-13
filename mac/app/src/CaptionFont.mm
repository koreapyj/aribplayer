#include "CaptionFont.h"

#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>

#include <set>

namespace mainview {

namespace {

constexpr const char* kFontFile = "WLCMARU2004ARIBU.TTF";
// libaribcaption's Japanese defaults on Apple platforms (renderer_impl.cpp LoadDefaultFontFamilies): kept after the
// bundled family because force_default replaces them.
constexpr const char* kFallbacks = "Hiragino Maru Gothic ProN,Hiragino Sans,sans-serif";

std::string Utf8(CFStringRef s) {
    if (!s) return {};
    return std::string(((__bridge NSString*)s).UTF8String ?: "");
}

std::string Canonical(const std::string& path) {
    if (path.empty()) return path;
    NSString* p = [NSString stringWithUTF8String:path.c_str()];
    return std::string(p.stringByResolvingSymlinksInPath.stringByStandardizingPath.UTF8String ?: "");
}

}

std::string BundledAribFontPath() {
    NSString* res = [NSBundle.mainBundle pathForResource:@"WLCMARU2004ARIBU" ofType:@"TTF" inDirectory:@"fonts"];
    if (res) return res.UTF8String;
#ifdef MV_ASSETS_FONTS_DIR
    NSString* repo = [@MV_ASSETS_FONTS_DIR stringByAppendingPathComponent:@(kFontFile)];
    if ([NSFileManager.defaultManager fileExistsAtPath:repo]) return repo.UTF8String;
#endif
    return {};
}

std::string ResolveLikeLibaribcaption(const std::string& family) {
    @autoreleasepool {
        // font_provider_coretext.cpp GetFontFace: family name + weight 0 -> CTFontCreateWithFontDescriptor -> URL.
        NSDictionary* traits = @{(__bridge NSString*)kCTFontWeightTrait : @(0.0f)};
        NSDictionary* attrs = @{(__bridge NSString*)kCTFontTraitsAttribute : traits,
                                (__bridge NSString*)kCTFontFamilyNameAttribute : @(family.c_str())};
        CTFontDescriptorRef want = CTFontDescriptorCreateWithAttributes((__bridge CFDictionaryRef)attrs);
        if (!want) return {};
        CTFontRef font = CTFontCreateWithFontDescriptor(want, 0, nullptr);
        CFRelease(want);
        if (!font) return {};
        CTFontDescriptorRef got = CTFontCopyFontDescriptor(font);
        CFRelease(font);
        if (!got) return {};
        CFURLRef url = (CFURLRef)CTFontDescriptorCopyAttribute(got, kCTFontURLAttribute);
        CFStringRef gotFamily = (CFStringRef)CTFontDescriptorCopyAttribute(got, kCTFontFamilyNameAttribute);
        CFRelease(got);
        std::string path;
        if (url) {
            path = std::string(((__bridge NSURL*)url).path.UTF8String ?: "");
            CFRelease(url);
        }
        // CoreText never fails the lookup: an unknown family resolves to a fallback face of another family.
        if (gotFamily) {
            if (Utf8(gotFamily) != family) path.clear();
            CFRelease(gotFamily);
        }
        return path;
    }
}

AribFont RegisterAribFont(const std::string& ttfPath) {
    static std::set<std::string> registered;
    AribFont f;
    f.file = Canonical(ttfPath);
    f.option = kFallbacks;
    if (f.file.empty()) { f.error = "no caption font file"; return f; }
    @autoreleasepool {
        NSURL* url = [NSURL fileURLWithPath:@(f.file.c_str())];
        NSArray* descs = CFBridgingRelease(CTFontManagerCreateFontDescriptorsFromURL((__bridge CFURLRef)url));
        if (descs.count == 0) { f.error = "not a font file: " + f.file; return f; }
        CTFontDescriptorRef d = (__bridge CTFontDescriptorRef)descs.firstObject;
        CFStringRef fam = (CFStringRef)CTFontDescriptorCopyAttribute(d, kCTFontFamilyNameAttribute);
        CFStringRef ps = (CFStringRef)CTFontDescriptorCopyAttribute(d, kCTFontNameAttribute);
        f.family = Utf8(fam);
        f.postscript = Utf8(ps);
        if (fam) CFRelease(fam);
        if (ps) CFRelease(ps);
        if (!registered.count(f.file)) {
            CFErrorRef err = nullptr;
            if (!CTFontManagerRegisterFontsForURL((__bridge CFURLRef)url, kCTFontManagerScopeProcess, &err)) {
                // kCTFontManagerErrorAlreadyRegistered (105) is fine: the family is visible either way
                const long code = err ? (long)CFErrorGetCode(err) : 0;
                if (err) CFRelease(err);
                if (code != kCTFontManagerErrorAlreadyRegistered) { f.error = "CTFontManagerRegisterFontsForURL failed (" + std::to_string(code) + ")"; return f; }
            }
            registered.insert(f.file);
        }
    }
    if (f.family.empty()) { f.error = "font has no family name"; return f; }
    f.resolved = Canonical(ResolveLikeLibaribcaption(f.family));
    if (f.resolved != f.file) {
        f.error = "CoreText resolves family '" + f.family + "' to '" + f.resolved + "'";
        return f;
    }
    f.option = f.family + "," + kFallbacks;
    return f;
}

const AribFont& BundledCaptionFont() {
    static const AribFont font = RegisterAribFont(BundledAribFontPath());
    return font;
}

}
