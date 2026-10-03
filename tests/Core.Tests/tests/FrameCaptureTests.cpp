// NEVER touches the clipboard: only the pure encoders and the STA helper are exercised.
#include "wvptest.h"

#include <windows.h>
#include <objbase.h>

#include <cstring>
#include <stdexcept>

#include "wvp/FrameCapture.h"

using namespace wvp;

namespace {

// Frame with a padded stride and varied colour/alpha so row/channel mix-ups are detected.
BgraFrame MakeFrame(int w, int h, int strideExtra = 8, bool varyAlpha = false) {
    BgraFrame f;
    f.Width = w; f.Height = h; f.Stride = w * 4 + strideExtra;
    f.Data.assign((size_t)f.Stride * h, 0);
    unsigned seed = 1234;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            size_t o = (size_t)y * f.Stride + x * 4;
            seed = seed * 1664525u + 1013904223u;
            f.Data[o + 0] = (std::uint8_t)(x * 7 + y);
            f.Data[o + 1] = (std::uint8_t)(y * 13 + 5);
            f.Data[o + 2] = (std::uint8_t)(seed >> 24);
            f.Data[o + 3] = varyAlpha ? (std::uint8_t)(1 + (x + y) % 254) : 255;
        }
    return f;
}

bool SamePixels(const BgraFrame& e, const BgraFrame& a) {
    if (e.Width != a.Width || e.Height != a.Height) return false;
    for (int y = 0; y < e.Height; y++)
        if (std::memcmp(e.Data.data() + (size_t)y * e.Stride, a.Data.data() + (size_t)y * a.Stride, (size_t)e.Width * 4) != 0) return false;
    return true;
}

std::uint32_t U32(const std::vector<std::uint8_t>& b, size_t o) { std::uint32_t v; std::memcpy(&v, b.data() + o, 4); return v; }

}

TEST_CASE(FrameCapture_PngRoundTripsPixelsWithWic) {
    struct Row { int w, h, extra; bool alpha; };
    const Row rows[] = {{1, 1, 0, false}, {33, 17, 8, false}, {64, 48, 0, true}};
    for (auto& r : rows) {
        BgraFrame f = MakeFrame(r.w, r.h, r.extra, r.alpha);
        auto png = FrameCapture::EncodePng(f);
        REQUIRE(png.size() > 8);
        const std::uint8_t sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
        CHECK(std::memcmp(png.data(), sig, 8) == 0);
        auto back = FrameCapture::DecodePng(png);
        REQUIRE(back.has_value());
        CHECK_MSG(SamePixels(f, *back), "pixel mismatch for " + std::to_string(r.w) + "x" + std::to_string(r.h));
    }
}

TEST_CASE(FrameCapture_InvalidFramesAreRejected) {
    bool threw1 = false, threw2 = false;
    try { BgraFrame f; f.Width = 10; f.Height = 10; f.Stride = 40; f.Data.resize(10); FrameCapture::EncodePng(f); } catch (const std::invalid_argument&) { threw1 = true; }
    try { BgraFrame f; f.Width = 0; f.Height = 10; FrameCapture::BuildDibV5(f); } catch (const std::invalid_argument&) { threw2 = true; }
    CHECK(threw1);
    CHECK(threw2);
}

TEST_CASE(FrameCapture_DibV5LayoutIsBottomUpBitfieldsWithAlphaMask) {
    BgraFrame f = MakeFrame(5, 3, 4, true);
    auto b = FrameCapture::BuildDibV5(f);
    REQUIRE(b.size() == 124 + 5 * 4 * 3);
    CHECK_EQ(U32(b, 0), 124u);
    CHECK_EQ((int)U32(b, 4), 5);
    CHECK_EQ((int)U32(b, 8), 3);                       // positive = bottom-up
    std::uint16_t planes, bits;
    std::memcpy(&planes, b.data() + 12, 2); std::memcpy(&bits, b.data() + 14, 2);
    CHECK_EQ((int)planes, 1);
    CHECK_EQ((int)bits, 32);
    CHECK_EQ(U32(b, 16), 3u);                          // BI_BITFIELDS
    CHECK_EQ(U32(b, 20), 60u);                         // SizeImage
    CHECK_EQ(U32(b, 40), 0x00FF0000u);
    CHECK_EQ(U32(b, 44), 0x0000FF00u);
    CHECK_EQ(U32(b, 48), 0x000000FFu);
    CHECK_EQ(U32(b, 52), 0xFF000000u);
    CHECK_EQ(U32(b, 56), 0x73524742u);                 // LCS_sRGB
    bool rowsOk = true;
    for (int y = 0; y < 3; y++)
        for (int i = 0; i < 20; i++)
            if (f.Data[(size_t)(2 - y) * f.Stride + i] != b[124 + y * 20 + i]) rowsOk = false;
    CHECK(rowsOk);                                     // first stored row is the LAST source row
}

TEST_CASE(FrameCapture_StaRunnerRunsOnStaThread) {
    HRESULT hrSta = E_FAIL;
    FrameCapture::RunOnSta([&] {
        // CoInitializeEx(STA) already done by the helper; re-initializing as MTA must report a mode change
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        hrSta = hr;
        if (SUCCEEDED(hr)) CoUninitialize();
    });
    CHECK_EQ((long)hrSta, (long)RPC_E_CHANGED_MODE);
    bool threw = false;
    try { FrameCapture::RunOnSta([] { throw std::runtime_error("x"); }); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

TEST_CASE(FrameCapture_PngFormatIsRegistered) {
    // RegisterClipboardFormat does not open the clipboard.
    CHECK(FrameCapture::PngFormat() >= 0xC000u);
}
