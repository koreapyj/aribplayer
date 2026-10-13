#include "puimac/CGRenderer.h"

#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>

#include "pui/Glyphs.h"
#include "puimac/SymbolMap.h"

using pui::Color;
using pui::FontKind;
using pui::ParaAlign;
using pui::PointF;
using pui::RectF;
using pui::SizeF;
using pui::TextAlign;
using pui::TextStyle;

namespace puimac {

namespace {

template <class T> struct CFRef {
    T ref = nullptr;
    CFRef() = default;
    explicit CFRef(T r) : ref(r) {}
    ~CFRef() { if (ref) CFRelease(ref); }
    CFRef(const CFRef&) = delete;
    CFRef& operator=(const CFRef&) = delete;
    T get() const { return ref; }
    explicit operator bool() const { return ref != nullptr; }
    void reset(T r) { if (ref) CFRelease(ref); ref = r; }
};

CFStringRef MakeCFString(std::wstring_view t) {
    // wchar_t is UTF-32 on macOS (UTF-16 on Windows); both are little-endian on every supported Apple platform.
    CFStringEncoding enc = sizeof(wchar_t) == 4 ? kCFStringEncodingUTF32LE : kCFStringEncodingUTF16LE;
    CFStringRef s = CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(t.data()),
                                            (CFIndex)(t.size() * sizeof(wchar_t)), enc, false);
    return s ? s : CFSTR("");
}

NSFontWeight WeightFor(int w) {
    if (w <= 100) return NSFontWeightUltraLight;
    if (w <= 200) return NSFontWeightThin;
    if (w <= 300) return NSFontWeightLight;
    if (w <= 400) return NSFontWeightRegular;
    if (w <= 500) return NSFontWeightMedium;
    if (w <= 600) return NSFontWeightSemibold;
    if (w <= 700) return NSFontWeightBold;
    if (w <= 800) return NSFontWeightHeavy;
    return NSFontWeightBlack;
}

CGRect ToCG(const RectF& r) { return CGRectMake(r.l, r.t, r.w(), r.h()); }

CGPathRef RoundRectPath(const RectF& r, float radius) {
    CGRect rc = CGRectStandardize(ToCG(r));
    float maxR = (float)std::min(rc.size.width, rc.size.height) * 0.5f;
    radius = std::clamp(radius, 0.f, maxR);
    if (radius <= 0.01f) return CGPathCreateWithRect(rc, nullptr);
    return CGPathCreateWithRoundedRect(rc, radius, radius, nullptr);
}

struct MeasureKey {
    std::wstring text;
    float size, maxWidth;
    int weight;
    bool wrap;
    bool operator==(const MeasureKey& o) const {
        return size == o.size && maxWidth == o.maxWidth && weight == o.weight && wrap == o.wrap && text == o.text;
    }
};
struct MeasureKeyHash {
    size_t operator()(const MeasureKey& k) const {
        size_t h = std::hash<std::wstring>()(k.text);
        h ^= std::hash<float>()(k.size) * 31 + std::hash<float>()(k.maxWidth) * 131 + (size_t)k.weight * 7 + (k.wrap ? 3 : 0);
        return h;
    }
};

struct TextLayout {
    CFRef<CTLineRef> line;              // single line (possibly truncated)
    CFRef<CTFrameRef> frame;            // wrapped
    CFRef<CTFramesetterRef> framesetter;
    double width = 0, ascent = 0, descent = 0, leading = 0, height = 0;
};

}   // namespace

struct CGRenderer::Impl {
    std::unordered_map<uint64_t, CTFontRef> fonts;
    std::unordered_map<MeasureKey, SizeF, MeasureKeyHash> measures;
    std::unordered_map<std::string, CGImageRef> symbols;   // tinted-later alpha templates, keyed by name|size|weight|scale
    std::unordered_map<std::string, CGSize> symbolSizes;

    ~Impl() {
        for (auto& [k, f] : fonts) CFRelease(f);
        for (auto& [k, i] : symbols) CGImageRelease(i);
    }

    CTFontRef Font(float size, int weight) {
        uint64_t key = ((uint64_t)std::lround(size * 64) << 16) | (uint64_t)(weight & 0xFFFF);
        auto it = fonts.find(key);
        if (it != fonts.end()) return it->second;
        NSFont* f = [NSFont systemFontOfSize:size weight:WeightFor(weight)];
        CTFontRef ct = (CTFontRef)CFBridgingRetain(f);
        fonts[key] = ct;
        return ct;
    }

    CFAttributedStringRef Attributed(std::wstring_view text, CTFontRef font, CGColorRef color, const TextStyle& s) {
        CFRef<CFStringRef> str(MakeCFString(text));
        CFRef<CFMutableDictionaryRef> attrs(CFDictionaryCreateMutable(kCFAllocatorDefault, 4, &kCFTypeDictionaryKeyCallBacks,
                                                                      &kCFTypeDictionaryValueCallBacks));
        CFDictionarySetValue(attrs.get(), kCTFontAttributeName, font);
        if (color) CFDictionarySetValue(attrs.get(), kCTForegroundColorAttributeName, color);
        if (s.wrap) {
            CTTextAlignment al = s.align == TextAlign::Center ? kCTTextAlignmentCenter
                                 : s.align == TextAlign::Trailing ? kCTTextAlignmentRight : kCTTextAlignmentLeft;
            CTLineBreakMode lb = kCTLineBreakByWordWrapping;
            CTParagraphStyleSetting settings[] = {
                {kCTParagraphStyleSpecifierAlignment, sizeof(al), &al},
                {kCTParagraphStyleSpecifierLineBreakMode, sizeof(lb), &lb},
            };
            CFRef<CTParagraphStyleRef> ps(CTParagraphStyleCreate(settings, 2));
            CFDictionarySetValue(attrs.get(), kCTParagraphStyleAttributeName, ps.get());
        }
        return CFAttributedStringCreate(kCFAllocatorDefault, str.get(), attrs.get());
    }

    /// Lays `text` out for `s` at `size` points. Single line: typographic bounds + optional end ellipsis within `width`.
    /// Wrapped (width > 0): CTFrame sized to the suggested height.
    void Layout(TextLayout& out, std::wstring_view text, const TextStyle& s, float size, CGColorRef color, float width, bool forDrawing) {
        CTFontRef font = Font(size, s.weight);
        CFRef<CFAttributedStringRef> as(Attributed(text, font, color, s));
        if (s.wrap && width > 0) {
            out.framesetter.reset(CTFramesetterCreateWithAttributedString(as.get()));
            CGSize fit = CTFramesetterSuggestFrameSizeWithConstraints(out.framesetter.get(), CFRangeMake(0, 0), nullptr,
                                                                      CGSizeMake(width, CGFLOAT_MAX), nullptr);
            out.width = std::min<double>(std::ceil(fit.width), width);
            out.height = std::ceil(fit.height);
            if (forDrawing) {
                CFRef<CGPathRef> path(CGPathCreateWithRect(CGRectMake(0, 0, width, out.height + 2), nullptr));
                out.frame.reset(CTFramesetterCreateFrame(out.framesetter.get(), CFRangeMake(0, 0), path.get(), nullptr));
            }
            return;
        }
        out.line.reset(CTLineCreateWithAttributedString(as.get()));
        CGFloat a, d, l;
        out.width = CTLineGetTypographicBounds(out.line.get(), &a, &d, &l);
        out.ascent = a; out.descent = d; out.leading = l;
        out.height = std::ceil(a + d + l);
        if (forDrawing && s.ellipsis && width > 0 && out.width > width + 0.01) {
            CFRef<CFAttributedStringRef> tok(Attributed(L"…", font, color, s));
            CFRef<CTLineRef> token(CTLineCreateWithAttributedString(tok.get()));
            CTLineRef trunc = CTLineCreateTruncatedLine(out.line.get(), width, kCTLineTruncationEnd, token.get());
            if (trunc) {
                out.line.reset(trunc);
                out.width = CTLineGetTypographicBounds(trunc, nullptr, nullptr, nullptr);
            }
        }
    }

    /// Alpha template of an SF Symbol rendered at `size` points x `scale`; nullptr when the symbol does not exist.
    CGImageRef Symbol(const SymbolEntry& e, float size, int weight, float scale, CGSize* ptSize) {
        char key[160];
        snprintf(key, sizeof key, "%s|%.2f|%d|%.2f|%.1f", e.symbol, size, weight, scale, e.rotation);
        auto it = symbols.find(key);
        if (it != symbols.end()) { *ptSize = symbolSizes[key]; return it->second; }
        NSImage* img = nil;
        for (const char* name : {e.symbol, e.fallback}) {
            if (!name) continue;
            img = [NSImage imageWithSystemSymbolName:[NSString stringWithUTF8String:name] accessibilityDescription:nil];
            if (img) break;
        }
        CGImageRef result = nullptr;
        CGSize sz = CGSizeZero;
        if (img) {
            NSImageSymbolConfiguration* cfg = [NSImageSymbolConfiguration configurationWithPointSize:size
                                                                                             weight:WeightFor(weight)
                                                                                              scale:NSImageSymbolScaleMedium];
            NSImage* sym = [img imageWithSymbolConfiguration:cfg];
            const CGSize natural = NSSizeToCGSize(sym.size);
            sz = natural;
            if (e.rotation != 0) {   // the rotated symbol's bounding box
                const double rad = e.rotation * M_PI / 180.0, c = std::fabs(std::cos(rad)), s = std::fabs(std::sin(rad));
                sz = CGSizeMake(natural.width * c + natural.height * s, natural.width * s + natural.height * c);
            }
            NSInteger pw = std::max<NSInteger>(1, (NSInteger)std::ceil(sz.width * scale));
            NSInteger ph = std::max<NSInteger>(1, (NSInteger)std::ceil(sz.height * scale));
            NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:pw pixelsHigh:ph
                                                                         bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
                                                                        colorSpaceName:NSCalibratedRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
            rep.size = NSMakeSize(sz.width, sz.height);
            [NSGraphicsContext saveGraphicsState];
            NSGraphicsContext* gc = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
            [NSGraphicsContext setCurrentContext:gc];
            if (e.rotation != 0) {   // clockwise on screen = negative in this y-up context, about the centre
                NSAffineTransform* t = [NSAffineTransform transform];
                [t translateXBy:sz.width * 0.5 yBy:sz.height * 0.5];
                [t rotateByDegrees:-e.rotation];
                [t concat];
            }
            const NSRect at = e.rotation != 0 ? NSMakeRect(-natural.width * 0.5, -natural.height * 0.5, natural.width, natural.height)
                                              : NSMakeRect(0, 0, sz.width, sz.height);
            [sym drawInRect:at fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1];
            [gc flushGraphics];
            [NSGraphicsContext restoreGraphicsState];
            result = CGImageRetain(rep.CGImage);
        }
        symbols[key] = result;
        symbolSizes[key] = sz;
        *ptSize = sz;
        return result;
    }
};

CGRenderer::CGRenderer() : impl_(new Impl) {}
CGRenderer::~CGRenderer() { delete impl_; }

void CGRenderer::Begin(CGContextRef ctx, float scale) {
    ctx_ = ctx;
    SetScale(scale);
    opacity_ = 1;
    opacityStack_.clear();
    clipDepth_ = 0;
    if (ctx_) {
        CGContextSetShouldAntialias(ctx_, true);
        CGContextSetShouldSmoothFonts(ctx_, true);
        CGContextSetInterpolationQuality(ctx_, kCGInterpolationHigh);
    }
}

void CGRenderer::End() {
    while (clipDepth_ > 0) PopClip();
    ctx_ = nullptr;
}

float CGRenderer::EffectiveSize(const TextStyle& s) const {
    if (s.font != FontKind::Text) return s.size;
    if (s.size == 14.f) return 13.f;    // Fluent Body        -> macOS body
    if (s.size == 12.f) return 11.f;    // Fluent Caption     -> macOS subheadline / caption
    if (s.size == 20.f) return 17.f;    // Fluent Subtitle    -> macOS title 2
    if (s.size == 28.f) return 22.f;    // Fluent Title       -> macOS title 1
    return s.size;
}

CGColorRef CGRenderer::MakeColor(Color c) const {
    return CGColorCreateSRGB(c.r(), c.g(), c.b(), c.a() * opacity_);
}

void CGRenderer::FillRect(const RectF& r, Color c) {
    if (!ctx_ || r.Empty() || (c.v >> 24) == 0) return;
    CFRef<CGColorRef> col(MakeColor(c));
    CGContextSetFillColorWithColor(ctx_, col.get());
    CGContextFillRect(ctx_, ToCG(r));
}

void CGRenderer::FillRoundRect(const RectF& r, float radius, Color c) {
    if (!ctx_ || r.Empty() || (c.v >> 24) == 0) return;
    CFRef<CGColorRef> col(MakeColor(c));
    CFRef<CGPathRef> path(RoundRectPath(r, radius));
    CGContextSetFillColorWithColor(ctx_, col.get());
    CGContextAddPath(ctx_, path.get());
    CGContextFillPath(ctx_);
}

void CGRenderer::StrokeRoundRect(const RectF& r, float radius, Color c, float width) {
    if (!ctx_ || r.Empty() || (c.v >> 24) == 0 || width <= 0) return;
    CFRef<CGColorRef> col(MakeColor(c));
    CFRef<CGPathRef> path(RoundRectPath(r, radius));
    CGContextSetStrokeColorWithColor(ctx_, col.get());
    CGContextSetLineWidth(ctx_, width);
    CGContextAddPath(ctx_, path.get());
    CGContextStrokePath(ctx_);
}

void CGRenderer::FillEllipse(PointF p, float rx, float ry, Color c) {
    if (!ctx_ || rx <= 0 || ry <= 0 || (c.v >> 24) == 0) return;
    CFRef<CGColorRef> col(MakeColor(c));
    CGContextSetFillColorWithColor(ctx_, col.get());
    CGContextFillEllipseInRect(ctx_, CGRectMake(p.x - rx, p.y - ry, 2 * rx, 2 * ry));
}

void CGRenderer::StrokeEllipse(PointF p, float rx, float ry, Color c, float width) {
    if (!ctx_ || rx <= 0 || ry <= 0 || (c.v >> 24) == 0 || width <= 0) return;
    CFRef<CGColorRef> col(MakeColor(c));
    CGContextSetStrokeColorWithColor(ctx_, col.get());
    CGContextSetLineWidth(ctx_, width);
    CGContextStrokeEllipseInRect(ctx_, CGRectMake(p.x - rx, p.y - ry, 2 * rx, 2 * ry));
}

void CGRenderer::Line(PointF a, PointF b, Color c, float width) {
    if (!ctx_ || (c.v >> 24) == 0 || width <= 0) return;
    CFRef<CGColorRef> col(MakeColor(c));
    CGContextSetStrokeColorWithColor(ctx_, col.get());
    CGContextSetLineWidth(ctx_, width);
    CGContextSetLineCap(ctx_, kCGLineCapButt);
    CGContextMoveToPoint(ctx_, a.x, a.y);
    CGContextAddLineToPoint(ctx_, b.x, b.y);
    CGContextStrokePath(ctx_);
}

void CGRenderer::Text(std::wstring_view text, const RectF& r, const TextStyle& s, Color c) {
    if (!ctx_ || text.empty() || (c.v >> 24) == 0) return;
    if (s.font == FontKind::Icon) { DrawIcon(text, r, s, c); return; }
    float size = EffectiveSize(s);
    CFRef<CGColorRef> col(MakeColor(c));
    TextLayout lay;
    impl_->Layout(lay, text, s, size, col.get(), r.w(), true);

    CGContextSaveGState(ctx_);
    if (lay.frame) {
        float y0 = s.valign == ParaAlign::Top ? r.t : s.valign == ParaAlign::Bottom ? r.b - (float)lay.height
                                                                                     : r.cy() - (float)lay.height * 0.5f;
        CGContextTranslateCTM(ctx_, r.l, y0 + lay.height + 2);
        CGContextScaleCTM(ctx_, 1, -1);
        CGContextSetTextMatrix(ctx_, CGAffineTransformIdentity);
        CTFrameDraw(lay.frame.get(), ctx_);
    } else if (lay.line) {
        float w = (float)lay.width;
        float x = s.align == TextAlign::Center ? r.cx() - w * 0.5f : s.align == TextAlign::Trailing ? r.r - w : r.l;
        float y0 = s.valign == ParaAlign::Top ? r.t : s.valign == ParaAlign::Bottom ? r.b - (float)lay.height
                                                                                     : r.cy() - (float)lay.height * 0.5f;
        float baseline = pui::SnapDip(y0 + (float)lay.ascent, scale_);
        CGContextSetTextMatrix(ctx_, CGAffineTransformMakeScale(1, -1));
        CGContextSetTextPosition(ctx_, x, baseline);
        CTLineDraw(lay.line.get(), ctx_);
    }
    CGContextRestoreGState(ctx_);
}

namespace {
CTFontRef MonoFont(float size) {
    static std::unordered_map<int, CTFontRef> fonts;   // keyed by size x 64; process lifetime
    const int key = (int)std::lround(size * 64);
    auto it = fonts.find(key);
    if (it != fonts.end()) return it->second;
    NSFont* f = [NSFont monospacedSystemFontOfSize:size weight:NSFontWeightRegular];
    CTFontRef ct = (CTFontRef)CFBridgingRetain(f);
    fonts[key] = ct;
    return ct;
}

CTLineRef MonoLine(std::wstring_view text, float size, CGColorRef color) {
    CFRef<CFStringRef> str(MakeCFString(text));
    CFRef<CFMutableDictionaryRef> attrs(CFDictionaryCreateMutable(kCFAllocatorDefault, 2, &kCFTypeDictionaryKeyCallBacks,
                                                                  &kCFTypeDictionaryValueCallBacks));
    CFDictionarySetValue(attrs.get(), kCTFontAttributeName, MonoFont(size));
    if (color) CFDictionarySetValue(attrs.get(), kCTForegroundColorAttributeName, color);
    CFRef<CFAttributedStringRef> as(CFAttributedStringCreate(kCFAllocatorDefault, str.get(), attrs.get()));
    return CTLineCreateWithAttributedString(as.get());
}
}

SizeF CGRenderer::MonoCell(float size) {
    CFRef<CTLineRef> line(MonoLine(L"0000000000", size, nullptr));
    CGFloat a = 0, d = 0, l = 0;
    const double w = CTLineGetTypographicBounds(line.get(), &a, &d, &l);
    return {std::max(0.1f, (float)(w / 10.0)), (float)std::ceil(a + d + l)};
}

void CGRenderer::MonoText(std::wstring_view text, const RectF& r, float size, Color c) {
    if (!ctx_ || text.empty() || (c.v >> 24) == 0 || r.Empty()) return;
    CFRef<CGColorRef> col(MakeColor(c));
    CFRef<CTLineRef> line(MonoLine(text, size, col.get()));
    CGFloat a = 0, d = 0, l = 0;
    CTLineGetTypographicBounds(line.get(), &a, &d, &l);
    CGContextSaveGState(ctx_);
    CGContextClipToRect(ctx_, ToCG(r));
    CGContextSetTextMatrix(ctx_, CGAffineTransformMakeScale(1, -1));
    CGContextSetTextPosition(ctx_, r.l, pui::SnapDip(r.t + (float)a, scale_));
    CTLineDraw(line.get(), ctx_);
    CGContextRestoreGState(ctx_);
}

SizeF CGRenderer::MeasureText(std::wstring_view text, const TextStyle& s, float maxWidth) {
    if (s.font == FontKind::Icon) {
        // One symbol per code point, drawn in a size x size box.
        float n = (float)std::max<size_t>(1, text.size());
        float w = s.size * n;
        return {maxWidth > 0 ? std::min(w, maxWidth) : w, s.size};
    }
    float size = EffectiveSize(s);
    MeasureKey key{std::wstring(text), size, s.wrap ? maxWidth : 0.f, s.weight, s.wrap};
    auto it = impl_->measures.find(key);
    if (it != impl_->measures.end()) {
        SizeF m = it->second;
        if (!s.wrap && maxWidth > 0) m.w = std::min(m.w, maxWidth);
        return m;
    }
    TextLayout lay;
    impl_->Layout(lay, text, s, size, nullptr, s.wrap ? maxWidth : 0.f, false);
    SizeF m{(float)std::ceil(lay.width), (float)lay.height};
    if (text.empty()) {
        CGFloat a = CTFontGetAscent(impl_->Font(size, s.weight)), d = CTFontGetDescent(impl_->Font(size, s.weight));
        m = {0, (float)std::ceil(a + d)};
    }
    if (impl_->measures.size() > 4096) impl_->measures.clear();
    impl_->measures.emplace(std::move(key), m);
    if (!s.wrap && maxWidth > 0) m.w = std::min(m.w, maxWidth);
    return m;
}

void CGRenderer::DrawIcon(std::wstring_view text, const RectF& r, const TextStyle& s, Color c) {
    float size = s.size;
    float totalW = size * (float)text.size();
    float x = s.align == TextAlign::Center ? r.cx() - totalW * 0.5f : s.align == TextAlign::Trailing ? r.r - totalW : r.l;
    float cy = s.valign == ParaAlign::Top ? r.t + size * 0.5f : s.valign == ParaAlign::Bottom ? r.b - size * 0.5f : r.cy();
    CFRef<CGColorRef> col(MakeColor(c));
    for (wchar_t ch : text) {
        float cx = x + size * 0.5f;
        x += size;
        const SymbolEntry* e = FindSymbol(ch);
        CGSize sz{};
        CGImageRef img = e ? impl_->Symbol(*e, size, std::max(s.weight, 500), scale_, &sz) : nullptr;
        if (!img) {
            // Placeholder: a thin rounded square, so an unmapped glyph stays visible.
            RectF box = RectF::XYWH(cx - size * 0.35f, cy - size * 0.35f, size * 0.7f, size * 0.7f);
            CFRef<CGPathRef> path(RoundRectPath(box, 2));
            CGContextSetStrokeColorWithColor(ctx_, col.get());
            CGContextSetLineWidth(ctx_, 1);
            CGContextAddPath(ctx_, path.get());
            CGContextStrokePath(ctx_);
            continue;
        }
        // Centre the symbol's natural box (points) on the slot; snap to device pixels so edges stay crisp.
        float dx = pui::SnapDip(cx - (float)sz.width * 0.5f, scale_), dy = pui::SnapDip(cy - (float)sz.height * 0.5f, scale_);
        CGContextSaveGState(ctx_);
        CGContextTranslateCTM(ctx_, dx, dy + sz.height);
        CGContextScaleCTM(ctx_, 1, -1);
        CGRect dst = CGRectMake(0, 0, sz.width, sz.height);
        CGContextClipToMask(ctx_, dst, img);   // alpha of the (black) template
        CGContextSetFillColorWithColor(ctx_, col.get());
        CGContextFillRect(ctx_, dst);
        CGContextRestoreGState(ctx_);
    }
}

void CGRenderer::PushClip(const RectF& r) {
    if (!ctx_) return;
    CGContextSaveGState(ctx_);
    CGRect rc = r.Empty() ? CGRectZero : ToCG(r);
    CGContextClipToRect(ctx_, rc);
    clipDepth_++;
}

void CGRenderer::PopClip() {
    if (!ctx_ || clipDepth_ <= 0) return;
    CGContextRestoreGState(ctx_);
    clipDepth_--;
}

void CGRenderer::PushOpacity(float o) {
    opacityStack_.push_back(opacity_);
    opacity_ *= std::clamp(o, 0.f, 1.f);
}

void CGRenderer::PopOpacity() {
    if (opacityStack_.empty()) return;
    opacity_ = opacityStack_.back();
    opacityStack_.pop_back();
}

}
