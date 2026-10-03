#pragma once
// Frame to clipboard: CF_DIBV5 (32bpp BI_BITFIELDS with alpha mask, bottom-up) plus the registered "PNG"
// format (WIC encoded). The pure encoders (BuildDibV5, EncodePng, DecodePng) never touch the clipboard.
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "PlayerTypes.h"

namespace wvp::FrameCapture {

constexpr int BitmapV5HeaderSize = 124;

/// Throws std::invalid_argument for a frame with bad dimensions / too small buffer.
void ValidateFrame(const BgraFrame& f);

/// BITMAPV5HEADER (124 bytes, masks B=0x000000FF G=0x0000FF00 R=0x00FF0000 A=0xFF000000, sRGB) followed by
/// bottom-up pixel rows with stride = width*4. Pure function.
std::vector<std::uint8_t> BuildDibV5(const BgraFrame& f);

/// PNG-encodes a frame with WIC (32bpp BGRA, straight alpha). Initializes COM on the calling thread if needed.
/// Throws std::invalid_argument / std::runtime_error.
std::vector<std::uint8_t> EncodePng(const BgraFrame& f);

/// Decodes a PNG into a BGRA frame (verification). nullopt on failure.
std::optional<BgraFrame> DecodePng(const std::vector<std::uint8_t>& png);

std::uint32_t PngFormat();

/// Runs fn on a dedicated STA thread and waits; rethrows its exception.
void RunOnSta(const std::function<void()>& fn);

/// Throws std::runtime_error if the clipboard is locked. TOUCHES THE USER'S CLIPBOARD.
void CopyToClipboard(const BgraFrame& frame);

}
