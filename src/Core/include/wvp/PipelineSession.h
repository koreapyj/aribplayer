#pragma once
// Decision logic that turns runtime events into planner actions (no Win32):
//  * per-URL content cache (LRU 64) so the NEXT open of a file is planned with its real codec/size/bit depth;
//  * the blacklist of elements that failed at runtime;
//  * fallbacks: a plan whose hwaccel / hwframes=keep / hw filter did not work in practice -> the blacklist key to add.
// Event flow per open "generation": BeforeOpen -> OnPrepared -> OnDebugInfo (polled) / OnFilterError. At most one fallback per
// generation and one content-driven reopen per url chain, so replanning can never loop.
#include <list>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>

#include "DebugInfo.h"
#include "PipelinePlanner.h"
#include "PlayerTypes.h"

namespace wvp {

class PipelineSession {
public:
    static constexpr std::size_t kCacheSize = 64;
    static constexpr long long kWarmupFrames = 40;

    struct Fallback {
        std::string key;      // "hwaccel:cuda", "hwframes_keep", "filter:ivtc_cuda"
        std::string reason;   // human readable
    };
    struct Prepared {
        ContentInfo content;  // from the media info (unknown when there is no video stream)
        bool changed = false; // differs from what the current plan was made with
    };

    /// Starts a new open generation and returns the content to plan with (cached for this url, else unknown).
    ContentInfo BeforeOpen(const std::wstring& url);
    /// Content from the prepared media info (first selected video stream); cached when known.
    Prepared OnPrepared(const std::wstring& url, const MediaInfo& info);
    /// True once per url chain: allows the single content-driven reopen (guards against loops).
    bool ClaimContentReopen(const std::wstring& url);

    /// Warm-up (>= kWarmupFrames decoded frames) checks of the plan against the measured debug info. At most one per generation.
    std::optional<Fallback> OnDebugInfo(const Plan& plan, const DebugInfo& info);
    /// FILTER_ERROR: hwframes_fallback -> "hwframes_keep"; else the first hardware filter step of the plan. At most one per generation.
    std::optional<Fallback> OnFilterError(const Plan& plan, const std::string& eventJson);
    std::optional<Fallback> OnFilterError(const Plan& plan, const FilterErrorInfo& info);

    /// PRESENTER_ERROR (libffplay already fell back to SDL): key "presenter:d3d11" (the presenter name from the event). At most one
    /// per generation, like the other fallbacks; an empty presenter name is read as d3d11. A device loss (DeviceLost) is no fallback.
    std::optional<Fallback> OnPresenterError(const Plan& plan, const PresenterErrorInfo& info);

    /// Bit depth from a pixel format name: contains "10"/"p010" -> 10, "12" -> 12, else 8.
    static int BitDepthOf(const std::string& pixFmt);
    /// Chroma subsampling from a pixel format name: yuv422*/nv16/p210/y210/yuyv422 -> 4:2:2, yuv444*/nv24/vuyx/xv30/y410/gbr*/rgb -> 4:4:4,
    /// else (and unknown) 4:2:0.
    static Chroma ChromaOf(const std::string& pixFmt);
    static ContentInfo ContentOf(const MediaInfo& info);

    const std::set<std::string>& Blacklist() const { return blacklist_; }
    void Clear() { blacklist_.clear(); }
    int Generation() const { return generation_; }
    std::size_t CacheSize() const { return cache_.size(); }
    std::optional<ContentInfo> Cached(const std::wstring& url) const;

private:
    std::optional<Fallback> Take(Fallback f);
    void Put(const std::wstring& url, const ContentInfo& c);

    using Entry = std::pair<std::wstring, ContentInfo>;
    std::list<Entry> cache_;   // most recent first
    std::unordered_map<std::wstring, std::list<Entry>::iterator> index_;
    std::set<std::string> blacklist_;
    ContentInfo used_;         // what the current plan was made with
    int generation_ = 0;
    bool fallbackDone_ = false;
    std::wstring contentReopenUrl_;
};

}
