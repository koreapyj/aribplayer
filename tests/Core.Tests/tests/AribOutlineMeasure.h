// Header-only helper: measures ARIB caption outline thickness by comparing a caption-visible frame
// with the same frame captured with the caption layer hidden. Pixels are 4 bytes each, B,G,R,A order.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace aribmeasure {

struct OutlineStats {
    size_t samples = 0;
    double median = 0.0;   // coverage-weighted outline width in pixels
};

namespace detail {

inline double Luma(const uint8_t* p) { return 0.114 * p[0] + 0.587 * p[1] + 0.299 * p[2]; }

// Per-pixel classification relative to the hidden frame.
//   fill:     caption text colour (any hue, e.g. white or green): not darker than the background and
//             clearly recoloured
//   coverage: how much darker than the hidden frame (0..1); only defined when the hidden luma is bright enough
struct Px {
    bool fill = false;
    double coverage = 0.0;
};

inline Px Classify(const uint8_t* vis, const uint8_t* hid) {
    Px px;
    const double lv = Luma(vis);
    const double lh = Luma(hid);
    const double colourDistance = std::abs(vis[0] - hid[0]) + std::abs(vis[1] - hid[1]) + std::abs(vis[2] - hid[2]);
    if (lv >= 0.8 * lh && lv >= 64.0 && colourDistance >= 96.0) {
        px.fill = true;
        return px;
    }
    if (lh >= 32.0) px.coverage = std::clamp(1.0 - lv / lh, 0.0, 1.0);
    return px;
}

// Walks one scanline (n pixels, strides given per frame) and appends one width sample per
// fill-run side that is bordered by a dark outline run which ends in untouched background.
inline void ScanLine(const uint8_t* vis, const uint8_t* hid, ptrdiff_t visStep, ptrdiff_t hidStep, int n, std::vector<double>& out) {
    constexpr int kMaxOutline = 24;
    constexpr double kOutlineMin = 0.05;
    std::vector<Px> px(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) px[i] = Classify(vis + i * visStep, hid + i * hidStep);

    const auto measureSide = [&](int start, int dir) {
        // Skip a single anti-aliased transition pixel (neither fill nor outline) next to the fill.
        int i = start;
        double sum = 0.0;
        int count = 0;
        bool skipped = false;
        for (; i >= 0 && i < n && count < kMaxOutline; i += dir) {
            if (px[i].fill) return;                       // another glyph part: not a clean outline
            if (px[i].coverage > kOutlineMin) { sum += px[i].coverage; ++count; continue; }
            if (count == 0 && !skipped) { skipped = true; continue; }
            break;                                        // reached untouched background
        }
        if (count == 0 || count >= kMaxOutline) return;   // no outline, or a dark panel rather than an outline
        if (i < 0 || i >= n) return;
        if (sum >= 0.3) out.push_back(sum);
    };

    int x = 0;
    while (x < n) {
        if (!px[x].fill) { ++x; continue; }
        int end = x;
        while (end + 1 < n && px[end + 1].fill) ++end;
        measureSide(x - 1, -1);
        measureSide(end + 1, +1);
        x = end + 1;
    }
}

}

inline bool WriteBmp(const char* path, const uint8_t* bgra, int stride, int w, int h) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) return false;
    const uint32_t imageSize = static_cast<uint32_t>(w) * static_cast<uint32_t>(h) * 4u;
    uint8_t header[54] = { 'B', 'M' };
    const auto put32 = [&](int at, uint32_t v) { for (int i = 0; i < 4; ++i) header[at + i] = static_cast<uint8_t>(v >> (8 * i)); };
    put32(2, 54u + imageSize);
    put32(10, 54u);
    put32(14, 40u);
    put32(18, static_cast<uint32_t>(w));
    put32(22, static_cast<uint32_t>(-h));   // negative height: top-down rows
    header[26] = 1;
    header[28] = 32;
    put32(34, imageSize);
    bool ok = fwrite(header, 1, sizeof(header), f) == sizeof(header);
    for (int y = 0; ok && y < h; ++y)
        ok = fwrite(bgra + static_cast<size_t>(y) * stride, 1, static_cast<size_t>(w) * 4, f) == static_cast<size_t>(w) * 4;
    return fclose(f) == 0 && ok;
}

inline OutlineStats MeasureOutline(const uint8_t* vis, int visStride, const uint8_t* hid, int hidStride,
                                   int x0, int y0, int w, int h) {
    std::vector<double> samples;
    for (int y = y0; y < y0 + h; ++y)
        detail::ScanLine(vis + static_cast<size_t>(y) * visStride + static_cast<size_t>(x0) * 4,
                         hid + static_cast<size_t>(y) * hidStride + static_cast<size_t>(x0) * 4, 4, 4, w, samples);
    for (int x = x0; x < x0 + w; ++x)
        detail::ScanLine(vis + static_cast<size_t>(y0) * visStride + static_cast<size_t>(x) * 4,
                         hid + static_cast<size_t>(y0) * hidStride + static_cast<size_t>(x) * 4,
                         visStride, hidStride, h, samples);
    OutlineStats stats;
    stats.samples = samples.size();
    if (!samples.empty()) {
        std::nth_element(samples.begin(), samples.begin() + samples.size() / 2, samples.end());
        stats.median = samples[samples.size() / 2];
    }
    return stats;
}

}
