#pragma once
// Engine integration tests without a window: a recording presenter that takes system-memory frames and shows nothing,
// and the test thread as the "UI thread" (TestUi: FfPlayer's UiPost queue, drained by Pump / WaitFor).
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include "ffplay.h"
#include "wvp/FfPlayer.h"

namespace wvptest {

class UiQueue {
public:
    wvp::UiPost Post() {
        return [this](std::function<void()> fn) {
            std::lock_guard<std::mutex> lk(mu_);
            q_.push_back(std::move(fn));
        };
    }
    // Runs every queued closure (and the ones they queue) on the calling thread.
    void Drain() {
        for (;;) {
            std::function<void()> fn;
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (q_.empty()) return;
                fn = std::move(q_.front());
                q_.pop_front();
            }
            fn();
        }
    }

private:
    std::mutex mu_;
    std::deque<std::function<void()>> q_;
};

inline UiQueue& TestUi() {
    static UiQueue q;
    return q;
}

inline void Pump() { TestUi().Drain(); }

template <class F>
bool WaitFor(F cond, double seconds = 10) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds((long long)(seconds * 1000));
    while (std::chrono::steady_clock::now() < end) {
        Pump();
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    Pump();
    return cond();
}

inline void Delay(int ms) { WaitFor([] { return false; }, ms / 1000.0); }

// Takes YUV420P / NV12 / P010 system frames (the engine downloads hardware frames and converts other formats), holds no
// frame after present() and reports every picture as shown: at its display size, or letterboxed into the view size set
// by Resize (the caption canvas follows that size).
class RecordingPresenter final : public wvp::Presenter {
public:
    struct Shown {
        double pts = 0;
        int serial = 0;
        int width = 0, height = 0, format = -1;
        bool redraw = false;
        std::int64_t dueUs = 0;
        int captionRects = 0, superimposeRects = 0;
        std::chrono::steady_clock::time_point at;   // when present() was called
    };

    // onlyGivenDevices false: a hwaccel open creates a default device of its type (decoding on the default GPU).
    explicit RecordingPresenter(bool onlyGivenDevices = true) : onlyGiven_(onlyGivenDevices) {
        table_.name = "recording";
        table_.opaque = this;
        table_.open = &Open;
        table_.present = &PresentFn;
        table_.clear = &Clear;
        table_.close = &Close;
    }

    const FFPPresenter* Native() const override { return &table_; }
    std::vector<AVBufferRef*> Devices() const override { return {}; }
    bool OnlyGivenDevices() const override { return onlyGiven_; }
    const std::string& AdapterId() const override { return adapterId_; }

    int Opens() const { return opens_.load(); }
    int Presents() const { return presents_.load(); }
    int Clears() const { return clears_.load(); }
    int Closes() const { return closes_.load(); }
    std::vector<Shown> History() const {
        std::lock_guard<std::mutex> lk(mu_);
        return history_;
    }
    std::optional<Shown> Last() const {
        std::lock_guard<std::mutex> lk(mu_);
        if (history_.empty()) return std::nullopt;
        return history_.back();
    }
    size_t HistorySize() const {
        std::lock_guard<std::mutex> lk(mu_);
        return history_.size();
    }
    // Blocks until a picture at History() index >= from has a serial other than `serial` and returns the first one;
    // nullopt when none came within `guard` (a hang guard, not a measurement).
    std::optional<Shown> WaitForSerialChange(size_t from, int serial, std::chrono::milliseconds guard) const {
        std::unique_lock<std::mutex> lk(mu_);
        std::optional<Shown> found;
        shownCv_.wait_for(lk, guard, [&] {
            for (size_t i = from; i < history_.size(); ++i)
                if (history_[i].serial != serial) { found = history_[i]; return true; }
            return false;
        });
        return found;
    }
    std::optional<FFPStreamInfo> LastStream() const {
        std::lock_guard<std::mutex> lk(mu_);
        return stream_;
    }
    // The next present() reports a failure through the sink (FFP_EVENT_PRESENTER_ERROR).
    void FailNextPresent(bool deviceLost) {
        failDeviceLost_ = deviceLost;
        failNext_ = true;
    }
    // A view of width x height output pixels: the shown picture is reported again at its new letterboxed size, as a
    // presenter does after redrawing a held picture (the engine then re-renders the captions and redraws while paused).
    void Resize(int width, int height) {
        FFPPresenterSink sink;
        FFPPresentedInfo shown{};
        bool redraw = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            viewW_ = width;
            viewH_ = height;
            sink = sink_;
            if (!history_.empty() && sink.presented) {
                const Shown& last = history_.back();
                shown.serial = last.serial;
                shown.pts = last.pts;
                Fit(last.width, last.height, lastSar_, shown.video_width, shown.video_height);
                redraw = true;
            }
        }
        if (redraw) sink.presented(sink.engine, &shown);
    }
    // The displayed video area of the last report.
    void VideoSize(int& width, int& height) const {
        std::lock_guard<std::mutex> lk(mu_);
        width = shownW_;
        height = shownH_;
    }

private:
    static int FFP_CALL Open(void* opaque, const FFPStreamInfo* info, const FFPPresenterSink* sink, FFPPresenterCaps* caps) {
        static const FFPPixFormat kFormats[] = {
            { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE }, { AV_PIX_FMT_NV12, AV_PIX_FMT_NONE }, { AV_PIX_FMT_P010LE, AV_PIX_FMT_NONE },
        };
        auto* self = static_cast<RecordingPresenter*>(opaque);
        {
            std::lock_guard<std::mutex> lk(self->mu_);
            self->sink_ = *sink;
            self->stream_ = *info;
            self->stream_->codec = nullptr;
            self->stream_->device_types = nullptr;
            self->stream_->nb_device_types = 0;
        }
        caps->formats = kFormats;
        caps->nb_formats = (int)(sizeof(kFormats) / sizeof(kFormats[0]));
        caps->devices = nullptr;
        caps->nb_devices = 0;
        caps->frames_held = 0;
        self->opens_++;
        return 0;
    }

    static int FFP_CALL PresentFn(void* opaque, const FFPPicture* pic) {
        auto* self = static_cast<RecordingPresenter*>(opaque);
        Shown s;
        s.at = std::chrono::steady_clock::now();
        s.pts = pic->pts;
        s.serial = pic->serial;
        s.redraw = pic->redraw != 0;
        s.dueUs = pic->due_us;
        if (pic->frame) { s.width = pic->frame->width; s.height = pic->frame->height; s.format = pic->frame->format; }
        if (pic->overlays) {
            const FFPOverlayLayer& c = pic->overlays[FFP_OVERLAY_CAPTION];
            const FFPOverlayLayer& o = pic->overlays[FFP_OVERLAY_SUPERIMPOSE];
            s.captionRects = c.visible ? c.rect_count : 0;
            s.superimposeRects = o.visible ? o.rect_count : 0;
        }
        FFPPresenterSink sink;
        FFPPresentedInfo shown{};
        {
            std::lock_guard<std::mutex> lk(self->mu_);
            self->history_.push_back(s);
            sink = self->sink_;
            if (pic->frame) self->lastSar_ = pic->frame->sample_aspect_ratio.den > 0 && pic->frame->sample_aspect_ratio.num > 0
                                                ? (double)pic->frame->sample_aspect_ratio.num / pic->frame->sample_aspect_ratio.den : 1.0;
            self->Fit(s.width, s.height, self->lastSar_, shown.video_width, shown.video_height);
        }
        self->shownCv_.notify_all();
        self->presents_++;
        if (self->failNext_.exchange(false)) {
            sink.failed(sink.engine, "test failure", self->failDeviceLost_ ? 1 : 0, self->failDeviceLost_ ? "0x887a0005" : nullptr);
            return 0;
        }
        shown.serial = pic->serial;
        shown.pts = pic->pts;
        if (sink.presented) sink.presented(sink.engine, &shown);
        return 0;
    }

    // Called with mu_ held.
    void Fit(int frameW, int frameH, double sar, int& outW, int& outH) {
        const double displayW = frameW * sar;
        if (viewW_ <= 0 || viewH_ <= 0 || frameW <= 0 || frameH <= 0) {
            outW = (int)(displayW + 0.5);
            outH = frameH;
        } else if (displayW * viewH_ > (double)frameH * viewW_) {
            outW = viewW_;
            outH = (int)(viewW_ * frameH / displayW + 0.5);
        } else {
            outH = viewH_;
            outW = (int)(viewH_ * displayW / frameH + 0.5);
        }
        shownW_ = outW;
        shownH_ = outH;
    }

    static void FFP_CALL Clear(void* opaque) { static_cast<RecordingPresenter*>(opaque)->clears_++; }
    static void FFP_CALL Close(void* opaque) { static_cast<RecordingPresenter*>(opaque)->closes_++; }

    FFPPresenter table_{};
    const bool onlyGiven_;
    const std::string adapterId_;
    mutable std::mutex mu_;
    mutable std::condition_variable shownCv_;   // a picture was added to history_
    FFPPresenterSink sink_{};
    std::optional<FFPStreamInfo> stream_;
    std::vector<Shown> history_;
    std::atomic<int> opens_{0}, presents_{0}, clears_{0}, closes_{0};
    std::atomic<bool> failNext_{false};
    bool failDeviceLost_ = false;
    int viewW_ = 0, viewH_ = 0, shownW_ = 0, shownH_ = 0;
    double lastSar_ = 1.0;
};

// An FfPlayer on a RecordingPresenter (returned in *presenter, owned by the player) that dispatches its events through
// TestUi, playing on SDL's dummy audio driver.
inline std::unique_ptr<wvp::FfPlayer> CreateTestPlayer(RecordingPresenter** presenter = nullptr, int* err = nullptr,
                                                       bool onlyGivenDevices = true) {
    auto p = std::make_unique<RecordingPresenter>(onlyGivenDevices);
    if (presenter) *presenter = p.get();
    return wvp::FfPlayer::Create(std::move(p), TestUi().Post(), err, "dummy");
}

}
