// wvp::mac::BuildCapabilities: wvp::BuildCapabilities() (darwin ffmpeg-caps.inc) + the videotoolbox decoder lines
// ffcapsdump does not print (see MacVideoPlatform.h).
#include "wvp/mac/MacVideoPlatform.h"

#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

namespace wvp::mac {

namespace {

// ffcapsdump.c caps_has_hw / caps_find_decoder, verbatim in C++.
bool HasHw(const AVCodec* codec, AVHWDeviceType type) {
    const AVCodecHWConfig* cfg;
    for (int i = 0; (cfg = avcodec_get_hw_config(codec, i)); i++)
        if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && cfg->device_type == type) return true;
    return false;
}

const AVCodec* FindDecoder(AVCodecID id, AVHWDeviceType type) {
    const AVCodec* dflt = avcodec_find_decoder(id);
    if (dflt && HasHw(dflt, type)) return dflt;
    void* it = nullptr;
    while (const AVCodec* c = av_codec_iterate(&it))
        if (c->id == id && av_codec_is_decoder(c) && c != dflt && HasHw(c, type)) return c;
    return nullptr;
}

}

CapabilitySet BuildCapabilities() {
#if WVP_MAC_HAVE_BUILDCAPS
    CapabilitySet c = wvp::BuildCapabilities();
#else
    CapabilitySet c;
    c.source = "build";
#endif
    // ffcapsdump's codec list and output shape (FFCAPS_DECODER(codec, type, decoder-or-"")).
    static const struct { const char* name; AVCodecID id; } kCodecs[] = {
        {"h264", AV_CODEC_ID_H264}, {"hevc", AV_CODEC_ID_HEVC}, {"mpeg2video", AV_CODEC_ID_MPEG2VIDEO},
        {"vp9", AV_CODEC_ID_VP9}, {"av1", AV_CODEC_ID_AV1}, {"vc1", AV_CODEC_ID_VC1}, {"mpeg4", AV_CODEC_ID_MPEG4},
    };
    const AVHWDeviceType vt = av_hwdevice_find_type_by_name("videotoolbox");
    if (vt == AV_HWDEVICE_TYPE_NONE) return c;
    for (const auto& k : kCodecs) {
        if (c.Decoder(k.name, "videotoolbox")) continue;   // already listed by ffcapsdump
        const AVCodec* dec = FindDecoder(k.id, vt);
        DecoderCap d;
        d.codec = k.name;
        d.hwaccel = "videotoolbox";
        d.decoder = dec ? dec->name : "";
        d.ok = !d.decoder.empty();
        c.decoders.push_back(std::move(d));
    }
    return c;
}

}
