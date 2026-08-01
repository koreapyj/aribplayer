#pragma once

#include <android/native_window.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

namespace aribplayer {

class FrameQueue;
class SubtitleQueue;
enum class SubtitleSource;

// GLES3 software-frame renderer. The supplied ANativeWindow reference is owned
// by the renderer after SetWindow returns.
class GlRenderer final {
public:
    using ClockPosition = std::function<bool(double*)>;
    using VideoClockAnchor = std::function<void(double)>;
    using SubtitleViewportChanged = std::function<void(int, int)>;

    GlRenderer(FrameQueue* frames,
               SubtitleQueue* caption_events,
               SubtitleQueue* superimpose_events,
               ClockPosition clock_position,
               VideoClockAnchor anchor_video_clock,
               SubtitleViewportChanged subtitle_viewport_changed);
    ~GlRenderer();

    GlRenderer(const GlRenderer&) = delete;
    GlRenderer& operator=(const GlRenderer&) = delete;

    bool Start();
    // Returns false after detaching a non-responsive render thread; the owner
    // must abandon this renderer and its queues.
    bool Stop(int64_t timeout_ms = 500);
    void SetWindow(ANativeWindow* window);
    void SetSerial(int serial);
    void SetSubtitlesEnabled(bool enabled);
    void ClearSubtitleSource(SubtitleSource source);
    double AvSyncMs() const;
    bool IsSerialComplete(int serial) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aribplayer
