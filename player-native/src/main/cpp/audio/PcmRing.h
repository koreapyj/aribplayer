#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace aribplayer {

// Lock-free single-producer/single-consumer stereo PCM ring. Each stored PCM
// frame retains its media PTS, so audio-clock callers can map device progress
// to the stream timeline even after seeks and discontinuities.
class PcmRing final {
public:
    PcmRing(int sample_rate, std::size_t capacity_frames = 0);

    PcmRing(const PcmRing&) = delete;
    PcmRing& operator=(const PcmRing&) = delete;

    int sample_rate() const { return sample_rate_; }
    std::size_t capacity_frames() const { return capacity_frames_; }
    std::size_t available_frames() const;
    std::size_t free_frames() const;

    // Writes as much as currently fits. first_pts_seconds denotes the first
    // input sample; NaN is accepted when the demuxer supplied no timestamp.
    std::size_t Write(const int16_t* interleaved_stereo, std::size_t frames,
                      double first_pts_seconds);

    // Reads up to requested_frames and returns the actual sample count. The
    // timestamp points at the first returned frame (NaN if unavailable).
    std::size_t Read(int16_t* interleaved_stereo, std::size_t requested_frames,
                     double* first_pts_seconds);
    void Clear();

private:
    const int sample_rate_;
    const std::size_t capacity_frames_;
    std::vector<int16_t> samples_;
    std::vector<double> timestamps_;
    alignas(64) std::atomic<uint64_t> write_index_{0};
    alignas(64) std::atomic<uint64_t> read_index_{0};
    std::atomic<uint64_t> generation_{0};
};

}  // namespace aribplayer
