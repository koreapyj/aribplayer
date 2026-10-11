#include "wvp/DebugInfo.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <utility>

#include "wvp/JsonLite.h"

namespace wvp {

namespace {

using json::Value;

std::optional<double> Num(const Value& o, const char* key) {
    const Value* v = o.Find(key);
    if (!v || v->kind != Value::Kind::Number || !std::isfinite(v->n)) return std::nullopt;
    return v->n;
}
std::optional<int> NumI(const Value& o, const char* key) {
    auto d = Num(o, key);
    if (!d) return std::nullopt;
    if (*d > 2147483647.0) return 2147483647;
    if (*d < -2147483648.0) return (-2147483647 - 1);
    return (int)*d;
}
std::optional<std::int64_t> NumL(const Value& o, const char* key) {
    auto d = Num(o, key);
    if (!d) return std::nullopt;
    return (std::int64_t)*d;
}
int StreamIdx(const Value& o) {
    auto v = NumI(o, "stream_index");
    return v ? *v : -1;
}
std::vector<std::string> StrArray(const Value& o, const char* key) {
    std::vector<std::string> r;
    const Value* v = o.Find(key);
    if (v && v->IsArray())
        for (auto& e : v->arr)
            if (e.kind == Value::Kind::String) r.push_back(e.s);
    return r;
}
std::vector<DebugFilterNode> Nodes(const Value& o, const char* key) {
    std::vector<DebugFilterNode> r;
    const Value* v = o.Find(key);
    if (v && v->IsArray())
        for (auto& e : v->arr)
            if (e.IsObject()) r.push_back({e.Str("name"), e.Str("instance"), e.Str("args"), e.Str("hw_device")});
    return r;
}
std::string Dump(const Value& v) {
    switch (v.kind) {
    case Value::Kind::Null: return "null";
    case Value::Kind::Bool: return v.b ? "true" : "false";
    case Value::Kind::Number: { char b[40]; std::snprintf(b, sizeof b, "%.10g", v.n); return b; }
    case Value::Kind::String: {
        std::string o = "\"";
        for (char c : v.s) {
            if (c == '"' || c == '\\') { o += '\\'; o += c; }
            else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
            else o += c;
        }
        return o + "\"";
    }
    case Value::Kind::Array: {
        std::string o = "[";
        for (size_t i = 0; i < v.arr.size(); i++) { if (i) o += ','; o += Dump(v.arr[i]); }
        return o + "]";
    }
    case Value::Kind::Object: {
        std::string o = "{";
        for (size_t i = 0; i < v.keys.size(); i++) { if (i) o += ','; o += "\"" + v.keys[i] + "\":" + Dump(v.vals[i]); }
        return o + "}";
    }
    }
    return "null";
}
const Value* Section(const Value& o, const char* key) {
    const Value* v = o.Find(key);
    return v && v->IsObject() ? v : nullptr;
}
const Value* FirstMember(const Value& o, std::initializer_list<const char*> keys) {
    for (const char* key : keys) if (const Value* v = o.Find(key)) return v;
    return nullptr;
}
std::optional<int> FirstInt(const Value& o, std::initializer_list<const char*> keys) {
    for (const char* key : keys) if (auto v = NumI(o, key)) return v;
    return std::nullopt;
}
std::optional<double> FirstDouble(const Value& o, std::initializer_list<const char*> keys) {
    for (const char* key : keys) if (auto v = Num(o, key)) return v;
    return std::nullopt;
}
std::optional<std::int64_t> FirstLong(const Value& o, std::initializer_list<const char*> keys) {
    for (const char* key : keys) if (auto v = NumL(o, key)) return v;
    return std::nullopt;
}
std::string FirstString(const Value& o, std::initializer_list<const char*> keys) {
    for (const char* key : keys) if (const Value* v = o.Find(key)) {
        if (v->kind == Value::Kind::String) return v->s;
        if (v->IsObject() || v->IsArray()) return Dump(*v);
    }
    return {};
}
DebugSubtitleLayer ReadSubtitleLayer(const Value& value) {
    DebugSubtitleLayer out;
    if (!value.IsObject()) return out;
    out.Present = true;
    out.StreamIndex = StreamIdx(value);
    if (out.StreamIndex < 0) {
        if (const Value* stream = value.Find("stream")) {
            if (stream->kind == Value::Kind::Number) out.StreamIndex = (int)stream->n;
            else if (stream->IsObject()) {
                out.StreamIndex = StreamIdx(*stream);
                if (out.StreamIndex < 0) out.StreamIndex = FirstInt(*stream, {"index", "stream_index"}).value_or(-1);
            }
        }
    }
    const Value* streamInfo = value.Find("stream");
    out.Codec = FirstString(value, {"codec"});
    if (out.Codec.empty() && streamInfo && streamInfo->IsObject()) out.Codec = FirstString(*streamInfo, {"codec"});
    out.DecoderOptions = FirstString(value, {"decoder_options", "options"});
    out.ResolvedFont = FirstString(value, {"resolved_font", "font_resolved", "font"});
    const Value* cue = FirstMember(value, {"active_cue", "cue"});
    if (!cue || !cue->IsObject()) cue = &value;
    out.RectCount = FirstInt(*cue, {"rect_count", "rects_count"});
    if (!out.RectCount) {
        if (const Value* rects = cue->Find("rects")) {
            if (rects->IsArray()) out.RectCount = (int)rects->arr.size();
            else if (rects->kind == Value::Kind::Number) out.RectCount = (int)rects->n;
        }
    }
    out.CueStart = FirstDouble(*cue, {"start", "start_time", "cue_start"});
    out.CueEnd = FirstDouble(*cue, {"end", "end_time", "cue_end"});
    const Value* canvas = FirstMember(value, {"canvas", "canvas_size"});
    if (canvas && canvas->IsObject()) {
        out.CanvasWidth = FirstInt(*canvas, {"width", "w"});
        out.CanvasHeight = FirstInt(*canvas, {"height", "h"});
    } else if (canvas && canvas->kind == Value::Kind::String) {
        int w = 0, h = 0;
        if (std::sscanf(canvas->s.c_str(), "%dx%d", &w, &h) == 2) { out.CanvasWidth = w; out.CanvasHeight = h; }
    }
    if (!out.CanvasWidth) out.CanvasWidth = FirstInt(value, {"canvas_width", "canvas_w"});
    if (!out.CanvasHeight) out.CanvasHeight = FirstInt(value, {"canvas_height", "canvas_h"});
    const Value* counts = Section(value, "counts");
    if (!counts) counts = Section(value, "counters");
    const Value& c = counts ? *counts : value;
    out.CuesShown = FirstLong(c, {"cues_shown", "shown"});
    out.CuesCleared = FirstLong(c, {"cues_cleared", "cleared"});
    out.BitmapsUploaded = FirstLong(c, {"bitmaps_uploaded", "cues_uploaded", "uploaded", "uploads"});
    out.Rerenders = FirstLong(c, {"rerenders", "re_renders"});
    if (const Value* history = Section(value, "history")) {
        out.HistoryEvents = FirstLong(*history, {"events"});
        out.HistoryBytes = FirstLong(*history, {"bytes"});
        out.HistoryAppendedBytes = FirstLong(*history, {"appended_bytes"});
    }
    return out;
}
void ReadSubtitleLayers(const Value& root, DebugInfo& d) {
    const Value* layers = Section(root, "subtitle_layers");
    if (!layers) layers = Section(root, "captions");
    if (!layers) layers = Section(root, "subtitles");
    if (!layers) {
        const Value* subtitle = Section(root, "subtitle");
        if (subtitle && (subtitle->Find("caption") || subtitle->Find("superimpose"))) layers = subtitle;
    }
    if (!layers) return;
    if (const Value* nested = Section(*layers, "layers")) layers = nested;
    if (const Value* caption = layers->Find("caption")) d.CaptionLayer = ReadSubtitleLayer(*caption);
    if (const Value* super = layers->Find("superimpose")) d.SuperimposeLayer = ReadSubtitleLayer(*super);
}
void Queue(const Value& q, const char* key, DebugQueue& out) {
    if (const Value* s = Section(q, key)) {
        out.Packets = NumL(*s, "packets");
        out.Bytes = NumL(*s, "bytes");
        out.Duration = Num(*s, "duration");
    }
}
void FrameMark(const Value& root, const char* key, DebugFrameMark& out) {
    if (const Value* v = Section(root, key)) {
        out.Count = NumL(*v, "count");
        out.Pts = Num(*v, "pts");
        out.Serial = NumL(*v, "serial");
        out.AtUs = NumL(*v, "at_us");
    }
}
void ReadSeek(const Value& root, DebugInfo& out) {
    const Value* v = Section(root, "seek");
    if (!v) return;
    out.Seek.Valid = v->Bool("valid");
    out.Seek.Direction = v->Str("direction");
    out.Seek.Requested = Num(*v, "requested");
    out.Seek.Keyframe = Num(*v, "keyframe");
    out.Seek.DistanceMs = Num(*v, "distance_ms");
    out.Seek.GopMs = Num(*v, "gop_ms");
}
void FrameTrace(const Value& root, DebugInfo& out) {
    const Value* v = Section(root, "frame_trace");
    if (!v) return;
    DebugFrameTrace& t = out.FrameTrace;
    t.Present = true;
    FrameMark(*v, "decoded", t.Decoded);
    FrameMark(*v, "filtered", t.Filtered);
    FrameMark(*v, "presented", t.Presented);
    FrameMark(*v, "early_drop", t.EarlyDrop);
    FrameMark(*v, "late_drop", t.LateDrop);
    if (const Value* events = v->Find("events"); events && events->IsArray()) {
        for (const Value& event : events->arr) {
            if (!event.IsObject()) continue;
            DebugFrameEvent e;
            e.Kind = event.Str("kind");
            e.Count = NumL(event, "count");
            e.Pts = Num(event, "pts");
            e.Serial = NumL(event, "serial");
            e.AtUs = NumL(event, "at_us");
            t.Events.emplace_back(std::move(e));
        }
    }
}

}

DebugInfo ParseDebugInfo(const std::string& text) {
    DebugInfo d;
    auto doc = json::Parse(text);
    if (!doc || !doc->IsObject()) return d;
    const Value& r = *doc;
    d.Valid = true;
    d.Position = Num(r, "position");
    d.Duration = Num(r, "duration");
    d.ClockUs = NumL(r, "clock_us");
    d.Paused = r.Bool("paused");
    d.Speed = Num(r, "speed");

    if (const Value* v = Section(r, "video")) {
        d.HasVideo = true;
        DebugVideo& o = d.Video;
        o.StreamIndex = StreamIdx(*v);
        o.Codec = v->Str("codec"); o.Decoder = v->Str("decoder"); o.Hwaccel = v->Str("hwaccel");
        o.PixFmt = v->Str("pix_fmt"); o.HwPixFmt = v->Str("hw_pix_fmt"); o.SwPixFmt = v->Str("sw_pix_fmt");
        o.Width = NumI(*v, "width"); o.Height = NumI(*v, "height");
        o.Sar = v->Str("sar"); o.FrameRate = Num(*v, "frame_rate");
        o.OutWidth = NumI(*v, "out_width"); o.OutHeight = NumI(*v, "out_height");
        o.OutPixFmt = v->Str("out_pix_fmt"); o.TimeBase = v->Str("time_base");
        o.StartTime = Num(*v, "start_time");
        o.HwaccelRequested = v->Str("hwaccel_requested"); o.HwaccelDevice = v->Str("hwaccel_device");
        o.HwaccelError = v->Str("hwaccel_error"); o.FramePath = v->Str("frame_path");
        if (const Value* c = Section(*v, "frame_path_counts")) {
            o.FramesHwDirect = NumL(*c, "hw_direct"); o.FramesHwDownload = NumL(*c, "hw_download");
            o.FramesSoftware = NumL(*c, "software");
            o.FramesSinkDownload = NumL(*c, "sink_download");
        }
        o.HwframesRequested = v->Str("hwframes_requested"); o.Hwframes = v->Str("hwframes");
        o.PixFmtDecoded = v->Str("pix_fmt_decoded"); o.PixFmtAfterTransfer = v->Str("pix_fmt_after_transfer");
    }
    if (const Value* v = Section(r, "audio")) {
        d.HasAudio = true;
        DebugAudio& o = d.Audio;
        o.StreamIndex = StreamIdx(*v);
        o.Codec = v->Str("codec"); o.Decoder = v->Str("decoder");
        o.SampleRate = NumI(*v, "sample_rate"); o.Channels = NumI(*v, "channels");
        o.ChannelLayout = v->Str("channel_layout"); o.SampleFmt = v->Str("sample_fmt");
        o.TimeBase = v->Str("time_base"); o.StartTime = Num(*v, "start_time");
        o.OutDevice = v->Str("out_device");
        o.OutSampleRate = NumI(*v, "out_sample_rate"); o.OutChannels = NumI(*v, "out_channels");
        o.HwBufSize = NumI(*v, "hw_buf_size");
        o.DiffAvg = Num(*v, "diff_avg"); o.LatencyMs = Num(*v, "latency_ms");
    }
    if (const Value* v = Section(r, "arib_audio")) {
        DebugAribAudio& o = d.AribAudio;
        o.Present = true;
        o.Preference = v->Str("preference"); o.Playing = v->Str("playing");
        o.Switches = NumL(*v, "switches"); o.Rebinds = NumL(*v, "rebinds"); o.SilenceMs = Num(*v, "silence_ms");
        if (const Value* roles = v->Find("roles"); roles && roles->IsArray()) {
            for (const Value& rv : roles->arr) {
                DebugAribRole role;
                role.Role = rv.Str("role");
                role.Present = rv.Bool("present");
                role.StreamIndex = rv.Find("stream_index") ? rv.Int("stream_index") : -1;
                role.Pid = rv.Find("pid") ? rv.Int("pid") : -1;
                role.ComponentTag = rv.Find("component_tag") ? rv.Int("component_tag") : -1;
                role.Starved = rv.Bool("starved");
                role.BufferedMs = Num(rv, "buffered_ms");
                if (role.Role == "audio1") o.Audio1 = role;
                else if (role.Role == "audio2") o.Audio2 = role;
            }
        }
    }
    if (const Value* v = Section(r, "subtitle")) {
        d.HasSubtitle = true;
        d.Subtitle.StreamIndex = StreamIdx(*v);
        d.Subtitle.Codec = v->Str("codec");
        d.Subtitle.TimeBase = v->Str("time_base");
        d.Subtitle.StartTime = Num(*v, "start_time");
    }
    ReadSubtitleLayers(r, d);
    if (const Value* v = Section(r, "filters")) {
        DebugFilters& o = d.Filters;
        o.VfRequested = v->Str("vf_requested"); o.AfRequested = v->Str("af_requested");
        o.VfGraph = v->Str("vf_graph"); o.AfGraph = v->Str("af_graph");
        o.VfFilters = StrArray(*v, "vf_filters"); o.AfFilters = StrArray(*v, "af_filters");
        o.VfNodes = Nodes(*v, "vf_nodes"); o.AfNodes = Nodes(*v, "af_nodes");
    }
    if (const Value* v = Section(r, "sync")) {
        DebugSync& o = d.Sync;
        o.Master = v->Str("master");
        o.AudioClock = Num(*v, "audio_clock"); o.VideoClock = Num(*v, "video_clock");
        o.ExternalClock = Num(*v, "external_clock"); o.MasterClock = Num(*v, "master_clock");
        o.AvDiffMs = Num(*v, "av_diff_ms");
        o.FrameDropsEarly = NumL(*v, "frame_drops_early"); o.FrameDropsLate = NumL(*v, "frame_drops_late");
        o.FramesDuplicated = NumL(*v, "frames_duplicated");
        o.FrameTimerDriftMs = Num(*v, "frame_timer_drift_ms");
    }
    if (const Value* v = Section(r, "fps")) {
        DebugFps& o = d.Fps;
        o.Present = true;
        o.StreamAvg = Num(*v, "stream_avg"); o.StreamR = Num(*v, "stream_r");
        o.FilterOut = Num(*v, "filter_out");
        o.FilterOutTimeBase = v->Str("filter_out_time_base"); o.FilterOutSource = v->Str("filter_out_source");
        o.Decoded = Num(*v, "decoded"); o.Filtered = Num(*v, "filtered"); o.Displayed = Num(*v, "displayed");
        o.FramesDecoded = NumL(*v, "frames_decoded"); o.FramesFiltered = NumL(*v, "frames_filtered");
        o.FramesDisplayed = NumL(*v, "frames_displayed");
        o.DropsEarly = NumL(*v, "drops_early"); o.DropsLate = NumL(*v, "drops_late");
    }
    ReadSeek(r, d);
    FrameTrace(r, d);
    if (const Value* v = Section(r, "presenter")) {
        DebugPresenter& o = d.Presenter;
        o.Present = true;
        o.Name = v->Str("name"); o.Error = v->Str("error");
        o.FramesDirect = NumL(*v, "frames_direct"); o.FramesUploaded = NumL(*v, "frames_uploaded");
        const Value empty;
        const Value* st = Section(*v, "stats");
        const Value& s = st ? *st : empty;
        o.Renderer = s.Str("renderer");
        o.Route = s.Str("route");
        o.FramesShader = NumL(s, "frames_shader"); o.FramesD3d11Copied = NumL(s, "frames_d3d11_copied");
        if (const Value* a = Section(s, "adapter")) {
            o.AdapterName = a->Str("name"); o.AdapterLuid = a->Str("luid"); o.AdapterVendor = NumI(*a, "vendor_id");
        }
        if (const Value* b = s.Find("debug_layer"); b && b->kind == Value::Kind::Bool) o.DebugLayer = b->b;
        o.DebugMessages = NumL(s, "debug_messages");
        o.Fallbacks = StrArray(s, "fallbacks");
        if (const Value* f = Section(s, "frame_stats")) {
            if (const Value* valid = f->Find("dxgi_valid"); valid && valid->kind == Value::Kind::Bool) o.DxgiValid = valid->b;
            o.DxgiPresentCount = NumL(*f, "dxgi_present_count");
            o.DxgiPresentRefreshCount = NumL(*f, "dxgi_present_refresh_count");
            o.DxgiSyncRefreshCount = NumL(*f, "dxgi_sync_refresh_count");
            o.DxgiSyncQpcTime = NumL(*f, "dxgi_sync_qpc_time");
            o.DxgiSyncGpuTime = NumL(*f, "dxgi_sync_gpu_time");
            o.D3dGpuTotalUs = NumL(*f, "gpu_total_us");
            o.D3dGpuFrames = NumL(*f, "gpu_frames");
            o.D3dGpuLastUs = NumL(*f, "gpu_last_us");
        }
        if (const Value* a = Section(s, "acrylic")) {
            o.Acrylic.Present = true;
            o.Acrylic.Regions = NumI(*a, "regions");
            o.Acrylic.FramesBlurred = NumL(*a, "frames_blurred");
            o.Acrylic.Failures = NumL(*a, "failures");
            o.Acrylic.BlurMs = Num(*a, "blur_ms");
        }
    }
    if (const Value* v = Section(r, "queues")) {
        Queue(*v, "audio", d.AudioQueue);
        Queue(*v, "video", d.VideoQueue);
        Queue(*v, "subtitle", d.SubtitleQueue);
    }
    return d;
}

}
