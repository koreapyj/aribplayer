#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

#include "audio/PcmRing.h"
#include "common/MediaQueues.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/rational.h>
#include <libswresample/swresample.h>
}

namespace aribplayer {

// Software audio decoder. Takes ownership of an opened AVCodecContext and
// writes interleaved signed-16 stereo PCM to the supplied SPSC ring.
class AudioDecoder final {
public:
    AudioDecoder(AVCodecContext* decoder, AVRational stream_time_base,
                 PacketQueue& packets, PcmRing& pcm_ring,
                 std::function<void()> on_dual_mono_detected = {});
    ~AudioDecoder();

    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    void Start();
    void Stop();
    // Drops queued work and asynchronously flushes codec/resampler state on
    // the decoder thread. serial must be the new demux seek generation.
    void Flush(int serial);
    void FlushForSeek(int serial, int64_t target_us);

    bool running() const { return running_.load(std::memory_order_acquire); }
    int output_sample_rate() const { return output_sample_rate_; }
    int last_error() const { return last_error_.load(std::memory_order_acquire); }

private:
    void DecodeLoop();
    bool DrainDecoder(int serial);
    bool ConvertFrame(const AVFrame& frame, int serial);
    bool ConfigureResampler(const AVFrame& frame, int serial);
    bool DrainResampler(int serial);
    bool WritePcm(const int16_t* samples, int frames, double pts_seconds, int serial);
    void ApplyFlush(int serial);

    AVCodecContext* decoder_ = nullptr;
    const AVRational time_base_;
    PacketQueue& packets_;
    PcmRing& pcm_ring_;
    const int output_sample_rate_;
    SwrContext* resampler_ = nullptr;
    AVChannelLayout input_layout_{};
    int input_rate_ = 0;
    AVSampleFormat input_format_ = AV_SAMPLE_FMT_NONE;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<int> requested_flush_serial_{-1};
    std::atomic<int> requested_discard_serial_{-1};
    std::atomic<int64_t> requested_discard_before_us_{AV_NOPTS_VALUE};
    std::atomic<int> desired_serial_{-1};
    std::atomic<int> last_error_{0};
    std::function<void()> on_dual_mono_detected_;
    bool dual_mono_packet_seen_ = false;
    bool dual_mono_reported_ = false;
    int active_serial_ = -1;
    int64_t discard_before_timestamp_ = AV_NOPTS_VALUE;
    double next_pts_seconds_ = 0.0;
};

}  // namespace aribplayer
