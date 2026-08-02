#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "common/MediaQueues.h"
#include "video/FilterGraph.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
}

namespace aribplayer {

struct VideoDecoderStats {
    std::string decoder;
    bool decoder_hw = false;
    VideoFilterStats filter;
};

// Video decoder with MediaCodec-first selection supplied by PlayerCore, a
// software fallback path, and an optional single-thread-owned FilterGraph.
class VideoDecoder final {
public:
    using DecoderInfoCallback = std::function<void(const std::string&, bool)>;
    using FilterInfoCallback = std::function<void(VideoMode, const std::string&)>;
    using FallbackSeekCallback = std::function<bool(double)>;
    using VideoSizeCallback = std::function<void(int, int, AVRational)>;

    VideoDecoder(AVCodecContext* decoder, bool decoder_is_hardware,
                 const AVCodecParameters* codec_parameters,
                 AVRational stream_time_base, AVRational frame_rate,
                 AVRational stream_sar, PacketQueue& packets, FrameQueue& frames,
                 DecoderInfoCallback decoder_info,
                 FilterInfoCallback filter_info,
                 FallbackSeekCallback fallback_seek,
                 VideoSizeCallback video_size);
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    void Start();
    // Performs filter/OpenCL preparation synchronously before PlayerCore reports
    // the session prepared.
    void Prepare();
    // Signals all queue waits before waiting for the decode thread. Returns false
    // after detaching a non-responsive thread; the owner must abandon this decoder.
    bool Stop(int64_t timeout_ms = 500);
    // Drops queued packets/frames and flushes codec/filter state on the decode thread.
    void Flush(int serial);
    // Flushes for a seek and drops decoded frames before the absolute stream target.
    void FlushForSeek(int serial, int64_t target_us);
    void SetVideoMode(VideoMode mode);
    VideoDecoderStats GetStats() const;

    bool running() const { return running_.load(std::memory_order_acquire); }
    bool eof_drained() const { return eof_drained_.load(std::memory_order_acquire); }
    int last_error() const { return last_error_.load(std::memory_order_acquire); }

private:
    void DecodeLoop();
    void FinishThread();
    bool SendPacket(const AVPacket* packet, int serial);
    bool DrainDecoder(int serial, bool end_of_stream);
    void ApplyFlush(int serial);
    void SetCatchUpOptions(bool enabled);
    void FinishCatchUp(int64_t reached_timestamp);
    bool FallBackToSoftware(double position_seconds);
    AVCodecContext* OpenSoftwareDecoder(std::string* error) const;
    void ReportDecoderInfo();
    static bool IsRenderableFormat(int format);
    static double FramePtsSeconds(const AVFrame& frame, AVRational time_base);

    AVCodecContext* decoder_ = nullptr;
    AVCodecParameters* codec_parameters_ = nullptr;
    const AVRational time_base_;
    const AVRational frame_rate_;
    const AVRational stream_sar_;
    PacketQueue& packets_;
    FrameQueue& frames_;
    DecoderInfoCallback decoder_info_callback_;
    FallbackSeekCallback fallback_seek_callback_;
    VideoSizeCallback video_size_callback_;
    std::unique_ptr<FilterGraph> filter_;

    mutable std::mutex info_mutex_;
    std::string decoder_name_;
    bool decoder_is_hardware_ = false;
    bool received_hardware_frame_ = false;
    AVDiscard default_skip_loop_filter_ = AVDISCARD_DEFAULT;
    AVDiscard default_skip_frame_ = AVDISCARD_DEFAULT;
    bool catchup_active_ = false;
    int64_t catchup_target_us_ = AV_NOPTS_VALUE;
    int64_t catchup_target_timestamp_ = AV_NOPTS_VALUE;
    int64_t landed_keyframe_timestamp_ = AV_NOPTS_VALUE;
    int catchup_frames_ = 0;
    std::chrono::steady_clock::time_point catchup_started_{};
    bool waiting_for_new_serial_ = false;
    int last_width_ = 0;
    int last_height_ = 0;
    AVRational last_sar_{0, 1};

    std::thread thread_;
    std::mutex thread_mutex_;
    std::condition_variable thread_cv_;
    bool thread_finished_ = true;
    bool thread_abandoned_ = false;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> eof_drained_{false};
    std::atomic<int> requested_flush_serial_{-1};
    std::atomic<int> requested_catchup_serial_{-1};
    std::atomic<int64_t> requested_catchup_target_us_{AV_NOPTS_VALUE};
    std::atomic<int> requested_mode_{static_cast<int>(VideoMode::kDeinterlace)};
    std::atomic<int> last_error_{0};
    int active_serial_ = -1;
};

}  // namespace aribplayer
