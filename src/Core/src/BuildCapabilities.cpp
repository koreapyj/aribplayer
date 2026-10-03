#include "wvp/CapabilitySet.h"

namespace wvp {

namespace {

FilterCap::Formats FormatsOf(const char* name) {
    std::string n = name;
    if (n == "list") return FilterCap::Formats::List;
    if (n == "any") return FilterCap::Formats::Any;
    return FilterCap::Formats::Dynamic;
}

}

CapabilitySet BuildCapabilities() {
    CapabilitySet c;
    c.source = "build";
#define FFCAPS_STRS(...) std::vector<std::string>{__VA_ARGS__}
#define FFCAPS_VERSION(v) c.ffmpegVersion = v;
#define FFCAPS_FEATURE(f) c.features.push_back(f);
#define FFCAPS_DECODER(codec_, hwaccel_, decoder_) \
    { DecoderCap d; d.codec = codec_; d.hwaccel = hwaccel_; d.decoder = decoder_; d.ok = !d.decoder.empty(); c.decoders.push_back(std::move(d)); }
#define FFCAPS_FILTER(name_, formats_, pix_, optionsKnown_, opts_, hwDevice_) \
    { FilterCap f; f.name = name_; f.formats = FormatsOf(formats_); f.pixFmts = FFCAPS_STRS pix_; f.optionsKnown = (optionsKnown_) != 0; \
      f.options = FFCAPS_STRS opts_; f.hwDevice = (hwDevice_) != 0; c.filters.push_back(std::move(f)); }
#include "ffmpeg-caps.inc"
#undef FFCAPS_FILTER
#undef FFCAPS_DECODER
#undef FFCAPS_FEATURE
#undef FFCAPS_VERSION
#undef FFCAPS_STRS
    return c;
}

}
