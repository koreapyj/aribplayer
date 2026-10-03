#include "wvp/PipelineSession.h"

#include <cctype>

#include "wvp/JsonLite.h"
#include "wvp/Utf.h"

namespace wvp {

namespace {
bool Same(const ContentInfo& a, const ContentInfo& b) {
    return a == b;
}
std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}

int PipelineSession::BitDepthOf(const std::string& pixFmt) {
    // "yuv420p10le", "p010le", "gray10le" -> 10; "yuv422p12le", "p012" -> 12; "nv12" / "yuv420p" -> 8 (the 12 of nv12 is a layout, not a depth)
    std::string p = Lower(pixFmt);
    if (p.size() > 2 && (p.compare(p.size() - 2, 2, "le") == 0 || p.compare(p.size() - 2, 2, "be") == 0)) p.resize(p.size() - 2);
    std::size_t d = p.size();
    while (d > 0 && std::isdigit(static_cast<unsigned char>(p[d - 1]))) d--;
    if (d == 0 || d == p.size() || (p[d - 1] != 'p' && p[d - 1] != 'y')) return 8;
    std::string digits = p.substr(d);
    if (digits == "10" || digits == "010") return 10;
    if (digits == "12" || digits == "012" || digits == "14" || digits == "16" || digits == "016") return 12;
    return 8;
}

Chroma PipelineSession::ChromaOf(const std::string& pixFmt) {
    std::string p = Lower(pixFmt);
    auto starts = [&](const char* x) { return p.rfind(x, 0) == 0; };
    if (starts("yuv422") || starts("yuvj422") || starts("yuva422") || starts("nv16") || starts("nv20") || starts("p21") ||
        starts("y21") || starts("yuyv422") || starts("uyvy422") || starts("yvyu422") || starts("y212"))
        return Chroma::Yuv422;
    if (starts("yuv444") || starts("yuvj444") || starts("yuva444") || starts("nv24") || starts("nv42") || starts("p41") ||
        starts("gbr") || starts("vuy") || starts("xv3") || starts("xv4") || starts("y41") || starts("ayuv") || starts("rgb") ||
        starts("bgr") || starts("x2rgb") || starts("x2bgr"))
        return Chroma::Yuv444;
    return Chroma::Yuv420;
}

ContentInfo PipelineSession::ContentOf(const MediaInfo& info) {
    const StreamInfo* v = nullptr;
    for (const StreamInfo& s : info.Streams) {
        if (s.Type != StreamType::Video) continue;
        if (!v || (s.Selected && !v->Selected)) v = &s;
    }
    ContentInfo c;
    if (!v) return c;
    c.codec = Lower(ToUtf8(v->Codec));
    c.width = v->Width;
    c.height = v->Height;
    c.bitDepth = BitDepthOf(ToUtf8(v->PixFmt));
    c.chroma = ChromaOf(ToUtf8(v->PixFmt));
    return c;
}

void PipelineSession::Put(const std::wstring& url, const ContentInfo& c) {
    auto it = index_.find(url);
    if (it != index_.end()) cache_.erase(it->second);
    cache_.emplace_front(url, c);
    index_[url] = cache_.begin();
    while (cache_.size() > kCacheSize) {
        index_.erase(cache_.back().first);
        cache_.pop_back();
    }
}

std::optional<ContentInfo> PipelineSession::Cached(const std::wstring& url) const {
    auto it = index_.find(url);
    if (it == index_.end()) return std::nullopt;
    return it->second->second;
}

ContentInfo PipelineSession::BeforeOpen(const std::wstring& url) {
    generation_++;
    fallbackDone_ = false;
    ContentInfo c;
    auto it = index_.find(url);
    if (it != index_.end()) {
        cache_.splice(cache_.begin(), cache_, it->second);
        c = it->second->second;
    }
    used_ = c;
    return c;
}

PipelineSession::Prepared PipelineSession::OnPrepared(const std::wstring& url, const MediaInfo& info) {
    Prepared r;
    r.content = ContentOf(info);
    if (!r.content.Known()) return r;
    Put(url, r.content);
    r.changed = !Same(r.content, used_);
    used_ = r.content;
    return r;
}

bool PipelineSession::ClaimContentReopen(const std::wstring& url) {
    if (contentReopenUrl_ == url) return false;
    contentReopenUrl_ = url;
    return true;
}

std::optional<PipelineSession::Fallback> PipelineSession::Take(Fallback f) {
    if (fallbackDone_ || blacklist_.count(f.key)) return std::nullopt;   // an already known key cannot be the cause again
    fallbackDone_ = true;
    blacklist_.insert(f.key);
    return f;
}

std::optional<PipelineSession::Fallback> PipelineSession::OnDebugInfo(const Plan& plan, const DebugInfo& d) {
    if (fallbackDone_ || !plan.ok || !d.Valid || !d.HasVideo) return std::nullopt;
    const DebugVideo& v = d.Video;
    long long frames = 0;
    if (d.Fps.FramesDecoded) frames = *d.Fps.FramesDecoded;
    else frames = v.FramesHwDirect.value_or(0) + v.FramesHwDownload.value_or(0) + v.FramesSoftware.value_or(0);
    if (frames < kWarmupFrames) return std::nullopt;
    if (!plan.hwaccel.empty() && !v.HwaccelDevice.empty() && v.HwaccelDevice != plan.hwaccel)
        return Take({ "hwaccel:" + plan.hwaccel, "planned " + plan.hwaccel + " but decoding uses " + v.HwaccelDevice +
                                                    (v.HwaccelError.empty() ? "" : " (" + v.HwaccelError + ")") });
    if (plan.hwframesKeep && v.Hwframes == "download")
        return Take({ "hwframes_keep", "planned hwframes=keep but frames are downloaded" });
    return std::nullopt;
}

std::optional<PipelineSession::Fallback> PipelineSession::OnFilterError(const Plan& plan, const FilterErrorInfo& info) {
    if (info.HwframesFallback) return Take({ "hwframes_keep", "libffplay fell back to hwframes=download" });
    if (!plan.ok) return std::nullopt;
    for (const PlanStep& s : plan.steps) {
        if (s.kind != PlanStep::Kind::Filter || s.text.empty()) continue;
        if (!IsHardware(s.in) && !IsHardware(s.out)) continue;
        return Take({ "filter:" + s.name, "filter error with hardware filter " + s.name });
    }
    return std::nullopt;
}

std::optional<PipelineSession::Fallback> PipelineSession::OnPresenterError(const Plan& plan, const PresenterErrorInfo& info) {
    (void)plan;
    if (info.DeviceLost) return std::nullopt;   // the device is gone, not unusable: the app rebinds instead
    std::string name = ToUtf8(info.Presenter);
    if (name.empty()) name = "d3d11";
    return Take({ "presenter:" + name, ToUtf8(info.Error) });
}

std::optional<PipelineSession::Fallback> PipelineSession::OnFilterError(const Plan& plan, const std::string& eventJson) {
    FilterErrorInfo info;
    if (auto doc = json::Parse(eventJson); doc && doc->IsObject()) {
        info.Type = ToWide(doc->Str("type"));
        info.Filter = ToWide(doc->Str("filter"));
        info.Code = doc->Int("code");
        info.Message = ToWide(doc->Str("message"));
        info.HwframesFallback = doc->Bool("hwframes_fallback");
    }
    return OnFilterError(plan, info);
}

}
