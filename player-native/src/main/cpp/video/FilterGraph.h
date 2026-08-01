#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "common/MediaQueues.h"

extern "C" {
#include <libavcodec/defs.h>
#include <libavfilter/avfilter.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/rational.h>
}

namespace aribplayer {

enum class VideoMode : int {
    kOff = 0,
    kAuto = 1,
    kIvtc = 2,
    kDeinterlace = 3,
};

struct VideoFilterStats {
    int requested_mode = 0;
    int effective_mode = 0;
    std::string backend = "none";
    double in_fps = 0.0;
    double out_fps = 0.0;
    double avg_filter_ms = 0.0;
    double filter_thread_cpu_ms = 0.0;
};

// Single-thread-owned FFmpeg video filter graph. All graph operations happen on
// the decoder thread; only mode requests and statistics snapshots cross threads.
class FilterGraph final {
public:
    using InfoCallback = std::function<void(VideoMode, const std::string&)>;

    FilterGraph(AVRational time_base, AVRational frame_rate, AVRational stream_sar,
                AVFieldOrder field_order, FrameQueue& output, InfoCallback info_callback);
    ~FilterGraph();

    FilterGraph(const FilterGraph&) = delete;
    FilterGraph& operator=(const FilterGraph&) = delete;

    void SetMode(VideoMode mode);
    // Warms the OpenCL device and processing kernels before playback packets
    // enter the decode path. The prototype needs only format metadata.
    void Prepare(const AVFrame& prototype);
    void Reset();
    bool Process(AVFrame* frame, int serial);
    bool EndOfStream(int serial);
    VideoFilterStats GetStats() const;

    struct GraphBundle {
        AVFilterGraph* graph = nullptr;
        AVFilterContext* source = nullptr;
        AVFilterContext* sink = nullptr;
        AVRational sink_time_base{0, 1};
    };

private:
    static constexpr int kAutoProbeFrames = 12;

    void ResetInternal();
    bool ProcessResolved(AVFrame* frame, int serial);
    bool PushBypass(const AVFrame& frame, int serial);
    bool BuildForFrame(const AVFrame& frame);
    bool BuildBundle(const AVFrame& frame, VideoMode mode, bool opencl,
                     GraphBundle* bundle, std::string* error);
    bool EnsureOpenCl(const AVFrame& frame, VideoMode mode);
    bool DrainSink(int serial, int* produced, double* queue_wait_ms = nullptr);
    void ResolveAutoIfReady(bool force);
    void ReportInfo();
    void RecordInput();
    void RecordOutput(int count);
    void RecordFilterTiming(double wall_ms, double cpu_ms);
    void RollStatsWindowLocked(std::chrono::steady_clock::time_point now) const;
    static bool IsSupportedSoftwareFormat(int format);
    static bool FrameIsTopFieldFirst(const AVFrame& frame, AVFieldOrder field_order);
    static double FramePtsSeconds(const AVFrame& frame, AVRational time_base);
    static int64_t ThreadCpuNanoseconds();

    const AVRational time_base_;
    const AVRational frame_rate_;
    const AVRational stream_sar_;
    const AVFieldOrder field_order_;
    FrameQueue& output_;
    InfoCallback info_callback_;

    std::atomic<int> requested_mode_{static_cast<int>(VideoMode::kOff)};
    VideoMode observed_mode_ = VideoMode::kOff;
    VideoMode effective_mode_ = VideoMode::kOff;
    bool effective_resolved_ = true;
    bool info_reported_ = false;
    std::string backend_ = "none";
    GraphBundle active_;
    int graph_width_ = 0;
    int graph_height_ = 0;
    int graph_format_ = -1;
    std::vector<AVFrame*> auto_frames_;
    bool auto_saw_repeat_ = false;
    bool auto_saw_interlaced_ = false;

    enum class OpenClState { kUnknown, kAvailable, kUnavailable };
    OpenClState opencl_state_ = OpenClState::kUnknown;
    AVBufferRef* opencl_device_ = nullptr;
    unsigned prepared_opencl_modes_ = 0;
    unsigned failed_opencl_modes_ = 0;

    mutable std::mutex stats_mutex_;
    int reported_effective_mode_ = 0;
    std::string reported_backend_ = "none";
    mutable std::chrono::steady_clock::time_point stats_started_ = std::chrono::steady_clock::now();
    mutable uint64_t input_frames_ = 0;
    mutable uint64_t output_frames_ = 0;
    mutable double rolling_in_fps_ = 0.0;
    mutable double rolling_out_fps_ = 0.0;
    uint64_t timed_frames_ = 0;
    double total_filter_ms_ = 0.0;
    double total_thread_cpu_ms_ = 0.0;
};

}  // namespace aribplayer
