#include "wvp/PlayerTypes.h"

#include "wvp/JsonLite.h"

namespace wvp {

std::string AribFontOption(const std::string& fontFile) {
    std::string v = fontFile.rfind("file:", 0) == 0 ? fontFile : "file:" + fontFile;
    for (size_t i = 5; i < v.size(); i++)
        if (v[i] == '\\') v[i] = '/';
    return v;
}

std::optional<MediaInfo> MediaInfo::Parse(const std::string& jsonText) {
    auto doc = json::Parse(jsonText);
    if (!doc || !doc->IsObject()) return std::nullopt;
    const json::Value& r = *doc;
    MediaInfo mi;
    if (auto* arr = r.Find("streams"); arr && arr->IsArray()) {
        for (const json::Value& s : arr->arr) {
            StreamInfo si;
            std::string type = s.Str("type");
            si.Type = type == "video" ? StreamType::Video
                    : type == "audio" ? StreamType::Audio
                    : type == "subtitle" ? StreamType::Subtitle : StreamType::Unknown;
            si.Index = s.Int("index");
            si.Codec = s.Str("codec");
            si.Selected = s.Bool("selected");
            si.Language = s.Str("language");
            si.Title = s.Str("title");
            si.AribType = s.Str("arib_type");
            si.ComponentTag = s.Find("component_tag") ? s.Int("component_tag") : -1;
            si.Width = s.Int("width");
            si.Height = s.Int("height");
            si.SarNum = s.Int("sar_num");
            si.SarDen = s.Int("sar_den");
            si.Fps = s.Dbl("fps");
            si.PixFmt = s.Str("pix_fmt");
            si.FourCC = s.Str("fourcc");
            si.ColorTransfer = s.Str("color_transfer");
            si.SampleRate = s.Int("sample_rate");
            si.Channels = s.Int("channels");
            mi.Streams.push_back(std::move(si));
        }
    }
    if (auto* a = r.Find("arib_audio"); a && a->IsObject()) {
        mi.AribAudio.Present = true;
        mi.AribAudio.Audio1 = a->Find("audio1") ? a->Int("audio1") : -1;
        mi.AribAudio.Audio2 = a->Find("audio2") ? a->Int("audio2") : -1;
    }
    mi.Url = r.Str("url");
    mi.Title = r.Str("title");
    mi.Format = r.Str("format");
    mi.Duration = r.Dbl("duration");
    mi.Seekable = r.Bool("seekable");
    mi.BitRate = (std::int64_t)r.Dbl("bit_rate");
    return mi;
}

}
