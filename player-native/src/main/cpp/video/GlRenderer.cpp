#include "video/GlRenderer.h"

#include "common/MediaQueues.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace aribplayer {
namespace {
constexpr char kTag[] = "aribplayer-render";

constexpr char kVertexShader[] = R"(#version 300 es
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
void main() {
    gl_Position = vec4(aPosition, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";

constexpr char kFragment420[] = R"(#version 300 es
precision mediump float;
in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;
uniform sampler2D texY;
uniform sampler2D texU;
uniform sampler2D texV;
uniform vec4 conversion;
uniform vec2 greenCoefficients;
void main() {
    float y = (texture(texY, vTexCoord).r - conversion.x) * conversion.y;
    float u = texture(texU, vTexCoord).r - 0.5;
    float v = texture(texV, vTexCoord).r - 0.5;
    outColor = vec4(y + conversion.z * v,
                    y - greenCoefficients.x * u - greenCoefficients.y * v,
                    y + conversion.w * u, 1.0);
}
)";

constexpr char kFragmentNv12[] = R"(#version 300 es
precision mediump float;
in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;
uniform sampler2D texY;
uniform sampler2D texUV;
uniform vec4 conversion;
uniform vec2 greenCoefficients;
void main() {
    float y = (texture(texY, vTexCoord).r - conversion.x) * conversion.y;
    vec2 uv = texture(texUV, vTexCoord).rg - vec2(0.5);
    outColor = vec4(y + conversion.z * uv.y,
                    y - greenCoefficients.x * uv.x - greenCoefficients.y * uv.y,
                    y + conversion.w * uv.x, 1.0);
}
)";

constexpr char kFragmentRgba[] = R"(#version 300 es
precision mediump float;
in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;
uniform sampler2D texRgba;
void main() {
    outColor = texture(texRgba, vTexCoord);
}
)";

GLuint CompileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        char log[512]{};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        __android_log_print(ANDROID_LOG_ERROR, kTag, "Shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint BuildProgram(const char* fragment) {
    GLuint vertex = CompileShader(GL_VERTEX_SHADER, kVertexShader);
    GLuint pixel = CompileShader(GL_FRAGMENT_SHADER, fragment);
    if (vertex == 0 || pixel == 0) {
        if (vertex != 0) glDeleteShader(vertex);
        if (pixel != 0) glDeleteShader(pixel);
        return 0;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, pixel);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(pixel);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        char log[512]{};
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        __android_log_print(ANDROID_LOG_ERROR, kTag, "Program link failed: %s", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}
} // namespace

class GlRenderer::Impl {
public:
    Impl(FrameQueue* frames, SubtitleQueue* caption_events,
         SubtitleQueue* superimpose_events, ClockPosition clock,
         VideoClockAnchor anchor, SubtitleViewportChanged viewport_changed)
            : frames_(frames),
              caption_layer_{caption_events, SubtitleSource::kCaption},
              superimpose_layer_{superimpose_events, SubtitleSource::kSuperimpose},
              clock_(std::move(clock)),
              anchor_(std::move(anchor)),
              subtitle_viewport_changed_(std::move(viewport_changed)) {}

    ~Impl() { Stop(); }

    bool Start() {
        if (running_.exchange(true)) return true;
        if (frames_ == nullptr) {
            running_ = false;
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(thread_mutex_);
            if (thread_abandoned_) {
                running_ = false;
                return false;
            }
            thread_finished_ = false;
        }
        thread_ = std::thread(&Impl::Run, this);
        return true;
    }

    bool Stop(int64_t timeout_ms = 500) {
        running_.store(false, std::memory_order_release);
        if (frames_ != nullptr) frames_->Abort();
        if (thread_.joinable()) {
            std::unique_lock<std::mutex> lock(thread_mutex_);
            const bool finished = thread_cv_.wait_for(
                    lock, std::chrono::milliseconds(std::max<int64_t>(0, timeout_ms)),
                    [this] { return thread_finished_; });
            lock.unlock();
            if (!finished) {
                thread_.detach();
                std::lock_guard<std::mutex> abandoned_lock(thread_mutex_);
                thread_abandoned_ = true;
                __android_log_print(ANDROID_LOG_ERROR, kTag,
                                    "teardown: detaching render thread after %lldms timeout",
                                    static_cast<long long>(timeout_ms));
                return false;
            }
            thread_.join();
        }
        std::lock_guard<std::mutex> lock(window_mutex_);
        if (pending_window_ != nullptr) {
            ANativeWindow_release(pending_window_);
            pending_window_ = nullptr;
        }
        return true;
    }

    void SetWindow(ANativeWindow* window) {
        std::lock_guard<std::mutex> lock(window_mutex_);
        if (pending_window_ != nullptr) ANativeWindow_release(pending_window_);
        pending_window_ = window;
        ++window_generation_;
    }

    void SetSerial(int serial) { serial_.store(serial, std::memory_order_release); }
    void SetSubtitlesEnabled(bool enabled) {
        subtitles_enabled_.store(enabled, std::memory_order_release);
    }
    void ClearSubtitleSource(SubtitleSource source) {
        if (source == SubtitleSource::kSuperimpose) {
            superimpose_reset_generation_.fetch_add(1, std::memory_order_release);
        } else {
            caption_reset_generation_.fetch_add(1, std::memory_order_release);
        }
    }
    double AvSyncMs() const { return av_sync_ms_.load(std::memory_order_acquire); }
    bool IsSerialComplete(int serial) const {
        return completed_serial_.load(std::memory_order_acquire) == serial;
    }

private:
    struct SubtitleLayer {
        SubtitleLayer(SubtitleQueue* event_queue, SubtitleSource event_source)
                : queue(event_queue), source(event_source) {}

        SubtitleQueue* queue = nullptr;
        SubtitleSource source = SubtitleSource::kCaption;
        int applied_serial = -1;
        uint64_t applied_reset_generation = 0;
        std::deque<SubtitleEvent> pending;
        std::optional<SubtitleEvent> active;
        std::vector<GLuint> textures;
        std::vector<GLuint> texture_garbage;
        bool textures_uploaded = false;
    };

    void Run() {
        while (running_.load(std::memory_order_acquire)) {
            ApplyWindow();
            FrameItem item;
            const QueueResult queue_result = frames_->Pop(&item, false);
            const int active_serial = serial_.load(std::memory_order_acquire);
            if (queue_result == QueueResult::kAborted) break;
            if (queue_result != QueueResult::kOk) {
                TickSubtitleRendering(active_serial);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (item.serial != active_serial) continue;
            if (item.end_of_stream) {
                completed_serial_.store(item.serial, std::memory_order_release);
                TickSubtitleRendering(active_serial);
                continue;
            }
            if (item.frame == nullptr) continue;

            if (anchor_) anchor_(item.pts_seconds);
            while (running_.load(std::memory_order_acquire)) {
                ApplyWindow();
                const int current_serial = serial_.load(std::memory_order_acquire);
                if (item.serial != current_serial) break;
                TickSubtitleRendering(current_serial);
                double now = 0.0;
                if (!clock_ || !clock_(&now) || !std::isfinite(now)) {
                    av_sync_ms_.store(0.0, std::memory_order_release);
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                const double delta = item.pts_seconds - now;
                av_sync_ms_.store(delta * 1000.0, std::memory_order_release);
                if (delta < -0.100) break;
                if (delta <= 0.002) {
                    RetainFrame(item.frame, item.serial);
                    Render(item.frame);
                    break;
                }
                const auto sleep_ms = static_cast<int>(std::clamp(delta * 1000.0, 1.0, 10.0));
                std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
            }
        }
        DestroyEgl();
        {
            std::lock_guard<std::mutex> lock(thread_mutex_);
            thread_finished_ = true;
        }
        thread_cv_.notify_all();
    }

    void ApplyWindow() {
        ANativeWindow* next = nullptr;
        uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(window_mutex_);
            generation = window_generation_;
            if (generation == applied_generation_) return;
            next = pending_window_;
            pending_window_ = nullptr;
            applied_generation_ = generation;
        }

        DestroySurface();
        if (window_ != nullptr) ANativeWindow_release(window_);
        window_ = next;
        if (window_ == nullptr) return;
        if (!EnsureContext()) return;
        surface_ = eglCreateWindowSurface(display_, config_, window_, nullptr);
        if (surface_ == EGL_NO_SURFACE ||
            eglMakeCurrent(display_, surface_, surface_, context_) != EGL_TRUE) {
            __android_log_print(ANDROID_LOG_ERROR, kTag, "Unable to create EGL window surface");
            DestroySurface();
        } else {
            needs_redraw_ = true;
        }
    }

    bool EnsureContext() {
        if (context_ != EGL_NO_CONTEXT) return true;
        display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display_ == EGL_NO_DISPLAY || eglInitialize(display_, nullptr, nullptr) != EGL_TRUE) return false;
        const EGLint attributes[] = {
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                EGL_NONE};
        EGLint count = 0;
        if (eglChooseConfig(display_, attributes, &config_, 1, &count) != EGL_TRUE || count == 0) return false;
        const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, context_attributes);
        if (context_ == EGL_NO_CONTEXT) return false;
        return true;
    }

    void EnsureGlObjects() {
        if (program_420_ != 0) return;
        program_420_ = BuildProgram(kFragment420);
        program_nv12_ = BuildProgram(kFragmentNv12);
        program_rgba_ = BuildProgram(kFragmentRgba);
        constexpr GLfloat vertices[] = {
                -1.f, -1.f, 0.f, 1.f,
                 1.f, -1.f, 1.f, 1.f,
                -1.f,  1.f, 0.f, 0.f,
                 1.f,  1.f, 1.f, 0.f};
        glGenVertexArrays(1, &vao_);
        glGenBuffers(1, &vbo_);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), nullptr);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                              reinterpret_cast<void*>(2 * sizeof(GLfloat)));
        glGenTextures(3, textures_);
        for (GLuint texture : textures_) {
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
    }

    static void UploadPlane(GLuint texture, int unit, GLint format, GLenum external_format,
                            int width, int height, int row_length, const uint8_t* data,
                            int signed_stride) {
        if (data == nullptr || height <= 0 || signed_stride == 0) return;
        std::vector<uint8_t> normalized;
        if (signed_stride < 0) {
            const int stride = -signed_stride;
            normalized.resize(static_cast<std::size_t>(stride) * height);
            for (int row = 0; row < height; ++row) {
                std::memcpy(normalized.data() + static_cast<std::size_t>(row) * stride,
                            data + static_cast<std::ptrdiff_t>(row) * signed_stride,
                            static_cast<std::size_t>(stride));
            }
            data = normalized.data();
        }
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, texture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
        glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0,
                     external_format, GL_UNSIGNED_BYTE, data);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }

    void QueueSubtitleTextureDeletion(SubtitleLayer& layer) {
        layer.texture_garbage.insert(layer.texture_garbage.end(),
                                     layer.textures.begin(), layer.textures.end());
        layer.textures.clear();
        layer.textures_uploaded = false;
    }

    void ResetSubtitleState(SubtitleLayer& layer, int serial) {
        QueueSubtitleTextureDeletion(layer);
        layer.pending.clear();
        layer.active.reset();
        layer.applied_serial = serial;
    }

    void ActivateSubtitle(SubtitleLayer& layer, SubtitleEvent event) {
        QueueSubtitleTextureDeletion(layer);
        layer.active = std::move(event);
    }

    bool ProcessSubtitleEvents(SubtitleLayer& layer, int64_t clock_us,
                               bool clock_available, int serial) {
        bool changed = false;
        const uint64_t reset_generation = layer.source == SubtitleSource::kSuperimpose
                ? superimpose_reset_generation_.load(std::memory_order_acquire)
                : caption_reset_generation_.load(std::memory_order_acquire);
        if (layer.applied_serial != serial ||
            layer.applied_reset_generation != reset_generation) {
            ResetSubtitleState(layer, serial);
            layer.applied_reset_generation = reset_generation;
            changed = true;
        }
        if (layer.queue != nullptr) {
            SubtitleEvent event;
            while (layer.queue->Pop(&event, false) == QueueResult::kOk) {
                if (event.serial == serial && event.source == layer.source) {
                    layer.pending.emplace_back(std::move(event));
                } else if (event.serial == serial) {
                    __android_log_print(ANDROID_LOG_WARN, kTag,
                                        "Dropping subtitle event routed to wrong overlay layer");
                }
                event = SubtitleEvent{};
            }
        }
        // A clock with no established raw PTS must neither show a PTS-zero
        // event nor advance expiry. The video scheduler follows the same rule.
        if (!clock_available) return changed;
        while (!layer.pending.empty() && layer.pending.front().start_pts_us <= clock_us) {
            ActivateSubtitle(layer, std::move(layer.pending.front()));
            layer.pending.pop_front();
            changed = true;
        }
        if (layer.active.has_value() &&
            layer.active->end_pts_us != kSubtitleEndIndefinite &&
            clock_us >= layer.active->end_pts_us) {
            QueueSubtitleTextureDeletion(layer);
            layer.active.reset();
            changed = true;
        }
        return changed;
    }

    void DeleteSubtitleTextureGarbage(SubtitleLayer& layer) {
        if (!layer.texture_garbage.empty()) {
            glDeleteTextures(static_cast<GLsizei>(layer.texture_garbage.size()),
                             layer.texture_garbage.data());
            layer.texture_garbage.clear();
        }
    }

    void UploadSubtitleTextures(SubtitleLayer& layer) {
        if (layer.textures_uploaded || !layer.active.has_value()) return;
        const auto& rects = layer.active->rects;
        layer.textures.assign(rects.size(), 0);
        if (!layer.textures.empty()) {
            glGenTextures(static_cast<GLsizei>(layer.textures.size()),
                          layer.textures.data());
        }
        for (std::size_t index = 0; index < rects.size(); ++index) {
            const SubtitleRect& rect = rects[index];
            if (layer.textures[index] == 0 || rect.width <= 0 || rect.height <= 0) {
                continue;
            }
            const std::size_t pixel_count =
                    static_cast<std::size_t>(rect.width) * rect.height;
            if (pixel_count > std::numeric_limits<std::size_t>::max() / 4 ||
                rect.rgba.size() != pixel_count * 4) {
                continue;
            }
            glBindTexture(GL_TEXTURE_2D, layer.textures[index]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rect.width, rect.height, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, rect.rgba.data());
        }
        layer.textures_uploaded = true;
    }

    void DrawSubtitleLayer(SubtitleLayer& layer, bool visible, const AVFrame* frame,
                           int crop_left, int crop_top,
                           int display_width, int display_height,
                           int viewport_width, int viewport_height) {
        DeleteSubtitleTextureGarbage(layer);
        if (!visible || !layer.active.has_value() || layer.active->rects.empty() ||
            program_rgba_ == 0 || frame == nullptr) {
            return;
        }
        UploadSubtitleTextures(layer);
        const SubtitleEvent& event = *layer.active;
        if (event.canvas_width <= 0 || event.canvas_height <= 0) return;

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(program_rgba_);
        glUniform1i(glGetUniformLocation(program_rgba_, "texRgba"), 0);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);

        for (std::size_t index = 0; index < event.rects.size(); ++index) {
            if (index >= layer.textures.size() || layer.textures[index] == 0) continue;
            const SubtitleRect& rect = event.rects[index];
            const double full_left = static_cast<double>(rect.x) * frame->width /
                                     event.canvas_width;
            const double full_right = static_cast<double>(rect.x + rect.width) * frame->width /
                                      event.canvas_width;
            const double full_top = static_cast<double>(rect.y) * frame->height /
                                    event.canvas_height;
            const double full_bottom = static_cast<double>(rect.y + rect.height) * frame->height /
                                       event.canvas_height;
            const GLfloat left = static_cast<GLfloat>(
                    -1.0 + 2.0 * (full_left - crop_left) / display_width);
            const GLfloat right = static_cast<GLfloat>(
                    -1.0 + 2.0 * (full_right - crop_left) / display_width);
            const GLfloat top = static_cast<GLfloat>(
                    1.0 - 2.0 * (full_top - crop_top) / display_height);
            const GLfloat bottom = static_cast<GLfloat>(
                    1.0 - 2.0 * (full_bottom - crop_top) / display_height);
            const GLfloat vertices[] = {
                    left,  bottom, 0.f, 1.f,
                    right, bottom, 1.f, 1.f,
                    left,  top,    0.f, 0.f,
                    right, top,    1.f, 0.f};
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, layer.textures[index]);
            const double target_width =
                    (full_right - full_left) * viewport_width / display_width;
            const double target_height =
                    (full_bottom - full_top) * viewport_height / display_height;
            const bool scaled = std::abs(target_width - rect.width) > 0.01 ||
                                std::abs(target_height - rect.height) > 0.01;
            const GLint filter = scaled ? GL_LINEAR : GL_NEAREST;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        glDisable(GL_BLEND);
    }

    void ReportSubtitleViewport(int width, int height) {
        if (!subtitle_viewport_changed_ || width <= 0 || height <= 0) return;
        // libaribcaption scales its fixed virtual plane into this pixel frame. Re-open only for
        // material viewport changes so glyphs stay near 1:1 without decoder churn from rounding.
        const auto changed_significantly = [](int current, int previous) {
            if (previous <= 0) return true;
            return std::abs(static_cast<double>(current - previous)) / previous > 0.10;
        };
        if (!changed_significantly(width, reported_viewport_width_) &&
            !changed_significantly(height, reported_viewport_height_)) {
            return;
        }
        reported_viewport_width_ = width;
        reported_viewport_height_ = height;
        subtitle_viewport_changed_(width, height);
    }

    bool TryClockUs(int64_t* clock_us) const {
        if (clock_us == nullptr) return false;
        double seconds = 0.0;
        if (!clock_ || !clock_(&seconds) || !std::isfinite(seconds)) return false;
        constexpr double kMicrosecondsPerSecond = 1000000.0;
        const double timestamp_us = seconds * kMicrosecondsPerSecond;
        if (timestamp_us >= static_cast<double>(std::numeric_limits<int64_t>::max())) {
            *clock_us = std::numeric_limits<int64_t>::max();
        } else if (timestamp_us <= static_cast<double>(std::numeric_limits<int64_t>::min())) {
            *clock_us = std::numeric_limits<int64_t>::min();
        } else {
            *clock_us = static_cast<int64_t>(timestamp_us);
        }
        return true;
    }

    void ClearRetainedFrame() {
        av_frame_free(&retained_frame_);
        retained_frame_serial_ = -1;
    }

    void RetainFrame(const AVFrame* frame, int serial) {
        if (frame == nullptr) return;
        if (retained_frame_ == nullptr) retained_frame_ = av_frame_alloc();
        if (retained_frame_ == nullptr) return;
        av_frame_unref(retained_frame_);
        if (av_frame_ref(retained_frame_, frame) < 0) {
            ClearRetainedFrame();
            return;
        }
        retained_frame_serial_ = serial;
    }

    void TickSubtitleRendering(int serial) {
        if (retained_frame_serial_ != serial && retained_frame_ != nullptr) {
            ClearRetainedFrame();
        }
        int64_t clock_us = 0;
        const bool clock_available = TryClockUs(&clock_us);
        bool redraw = ProcessSubtitleEvents(caption_layer_, clock_us, clock_available, serial);
        redraw = ProcessSubtitleEvents(superimpose_layer_, clock_us, clock_available, serial) ||
                 redraw;
        const bool enabled = subtitles_enabled_.load(std::memory_order_acquire);
        if (enabled != applied_subtitles_enabled_) {
            applied_subtitles_enabled_ = enabled;
            redraw = true;
        }
        if ((redraw || needs_redraw_) && retained_frame_ != nullptr &&
            retained_frame_serial_ == serial && surface_ != EGL_NO_SURFACE) {
            Render(retained_frame_);
            needs_redraw_ = false;
        }
    }

    void Render(const AVFrame* frame) {
        if (frame == nullptr) return;
        int64_t clock_us = 0;
        const bool clock_available = TryClockUs(&clock_us);
        ProcessSubtitleEvents(caption_layer_, clock_us, clock_available,
                              serial_.load(std::memory_order_acquire));
        ProcessSubtitleEvents(superimpose_layer_, clock_us, clock_available,
                              serial_.load(std::memory_order_acquire));
        if (surface_ == EGL_NO_SURFACE || frame->data[0] == nullptr) return;
        const bool nv12 = frame->format == AV_PIX_FMT_NV12;
        const bool yuv420 = frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P;
        if (!nv12 && !yuv420) return;
        EnsureGlObjects();
        GLuint program = nv12 ? program_nv12_ : program_420_;
        if (program == 0) return;

        EGLint surface_width = 0;
        EGLint surface_height = 0;
        eglQuerySurface(display_, surface_, EGL_WIDTH, &surface_width);
        eglQuerySurface(display_, surface_, EGL_HEIGHT, &surface_height);
        const int crop_left = std::clamp<int>(frame->crop_left, 0, std::max(0, frame->width - 1));
        const int crop_right = std::clamp<int>(frame->crop_right, 0,
                                               std::max(0, frame->width - crop_left - 1));
        const int crop_top = std::clamp<int>(frame->crop_top, 0, std::max(0, frame->height - 1));
        const int crop_bottom = std::clamp<int>(frame->crop_bottom, 0,
                                                std::max(0, frame->height - crop_top - 1));
        const int display_width = std::max(1, frame->width - crop_left - crop_right);
        const int display_height = std::max(1, frame->height - crop_top - crop_bottom);
        const int sar_num = frame->sample_aspect_ratio.num > 0 ? frame->sample_aspect_ratio.num : 1;
        const int sar_den = frame->sample_aspect_ratio.den > 0 ? frame->sample_aspect_ratio.den : 1;
        const double video_aspect = static_cast<double>(display_width) * sar_num /
                                    (static_cast<double>(display_height) * sar_den);
        const double surface_aspect = surface_height > 0
                ? static_cast<double>(surface_width) / surface_height : video_aspect;
        int view_width = surface_width;
        int view_height = surface_height;
        int view_x = 0;
        int view_y = 0;
        if (surface_aspect > video_aspect) {
            view_width = static_cast<int>(surface_height * video_aspect);
            view_x = (surface_width - view_width) / 2;
        } else {
            view_height = static_cast<int>(surface_width / video_aspect);
            view_y = (surface_height - view_height) / 2;
        }
        ReportSubtitleViewport(view_width, view_height);
        glViewport(0, 0, surface_width, surface_height);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glViewport(view_x, view_y, std::max(1, view_width), std::max(1, view_height));

        glUseProgram(program);
        const bool full_range = frame->color_range == AVCOL_RANGE_JPEG;
        const bool bt709 = frame->colorspace == AVCOL_SPC_BT709 ||
                           (frame->colorspace == AVCOL_SPC_UNSPECIFIED && frame->height >= 720);
        const float y_offset = full_range ? 0.f : 16.f / 255.f;
        const float y_scale = full_range ? 1.f : 255.f / 219.f;
        const float rv = bt709 ? 1.5748f : 1.4020f;
        const float gu = bt709 ? 0.1873f : 0.3441f;
        const float gv = bt709 ? 0.4681f : 0.7141f;
        const float bu = bt709 ? 1.8556f : 1.7720f;
        glUniform4f(glGetUniformLocation(program, "conversion"), y_offset, y_scale, rv, bu);
        glUniform2f(glGetUniformLocation(program, "greenCoefficients"), gu, gv);

        UploadPlane(textures_[0], 0, GL_R8, GL_RED, frame->width, frame->height,
                    std::abs(frame->linesize[0]), frame->data[0], frame->linesize[0]);
        glUniform1i(glGetUniformLocation(program, "texY"), 0);
        if (nv12) {
            UploadPlane(textures_[1], 1, GL_RG8, GL_RG, (frame->width + 1) / 2,
                        (frame->height + 1) / 2, std::abs(frame->linesize[1]) / 2,
                        frame->data[1], frame->linesize[1]);
            glUniform1i(glGetUniformLocation(program, "texUV"), 1);
        } else {
            UploadPlane(textures_[1], 1, GL_R8, GL_RED, (frame->width + 1) / 2,
                        (frame->height + 1) / 2, std::abs(frame->linesize[1]), frame->data[1],
                        frame->linesize[1]);
            UploadPlane(textures_[2], 2, GL_R8, GL_RED, (frame->width + 1) / 2,
                        (frame->height + 1) / 2, std::abs(frame->linesize[2]), frame->data[2],
                        frame->linesize[2]);
            glUniform1i(glGetUniformLocation(program, "texU"), 1);
            glUniform1i(glGetUniformLocation(program, "texV"), 2);
        }
        const GLfloat left = static_cast<GLfloat>(crop_left) / frame->width;
        const GLfloat right = static_cast<GLfloat>(frame->width - crop_right) / frame->width;
        const GLfloat top = static_cast<GLfloat>(crop_top) / frame->height;
        const GLfloat bottom = static_cast<GLfloat>(frame->height - crop_bottom) / frame->height;
        const GLfloat vertices[] = {
                -1.f, -1.f, left,  bottom,
                 1.f, -1.f, right, bottom,
                -1.f,  1.f, left,  top,
                 1.f,  1.f, right, top};
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        DrawSubtitleLayer(caption_layer_,
                          subtitles_enabled_.load(std::memory_order_acquire),
                          frame, crop_left, crop_top, display_width, display_height,
                          std::max(1, view_width), std::max(1, view_height));
        // ARIB TR-B14 superimpose is operational/emergency information, not CC;
        // keep it visible when the user's caption toggle is off and draw it last.
        DrawSubtitleLayer(superimpose_layer_, true, frame, crop_left, crop_top,
                          display_width, display_height,
                          std::max(1, view_width), std::max(1, view_height));
        eglSwapBuffers(display_, surface_);
        applied_subtitles_enabled_ = subtitles_enabled_.load(std::memory_order_acquire);
        needs_redraw_ = false;
    }

    void DestroySurface() {
        if (display_ != EGL_NO_DISPLAY) eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (display_ != EGL_NO_DISPLAY && surface_ != EGL_NO_SURFACE) eglDestroySurface(display_, surface_);
        surface_ = EGL_NO_SURFACE;
    }

    void DestroyEgl() {
        DestroySurface();
        if (display_ != EGL_NO_DISPLAY && context_ != EGL_NO_CONTEXT) {
            // GL objects belong to the context and are reclaimed with it.
            eglDestroyContext(display_, context_);
        }
        if (display_ != EGL_NO_DISPLAY) eglTerminate(display_);
        context_ = EGL_NO_CONTEXT;
        display_ = EGL_NO_DISPLAY;
        config_ = nullptr;
        program_420_ = program_nv12_ = program_rgba_ = vao_ = vbo_ = 0;
        std::memset(textures_, 0, sizeof(textures_));
        for (SubtitleLayer* layer : {&caption_layer_, &superimpose_layer_}) {
            layer->textures.clear();
            layer->texture_garbage.clear();
            layer->textures_uploaded = false;
            layer->active.reset();
            layer->pending.clear();
        }
        ClearRetainedFrame();
        if (window_ != nullptr) {
            ANativeWindow_release(window_);
            window_ = nullptr;
        }
    }

    FrameQueue* frames_;
    SubtitleLayer caption_layer_;
    SubtitleLayer superimpose_layer_;
    ClockPosition clock_;
    VideoClockAnchor anchor_;
    SubtitleViewportChanged subtitle_viewport_changed_;
    std::atomic<bool> running_{false};
    std::atomic<bool> subtitles_enabled_{true};
    std::atomic<int> serial_{0};
    std::atomic<int> completed_serial_{-1};
    std::atomic<uint64_t> caption_reset_generation_{0};
    std::atomic<uint64_t> superimpose_reset_generation_{0};
    std::atomic<double> av_sync_ms_{0.0};
    std::thread thread_;
    std::mutex thread_mutex_;
    std::condition_variable thread_cv_;
    bool thread_finished_ = true;
    bool thread_abandoned_ = false;

    std::mutex window_mutex_;
    ANativeWindow* pending_window_ = nullptr;
    ANativeWindow* window_ = nullptr;
    uint64_t window_generation_ = 0;
    uint64_t applied_generation_ = 0;

    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLConfig config_ = nullptr;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;
    GLuint program_420_ = 0;
    GLuint program_nv12_ = 0;
    GLuint program_rgba_ = 0;
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint textures_[3]{};

    bool applied_subtitles_enabled_ = true;
    bool needs_redraw_ = false;
    int reported_viewport_width_ = 0;
    int reported_viewport_height_ = 0;
    AVFrame* retained_frame_ = nullptr;
    int retained_frame_serial_ = -1;
};

GlRenderer::GlRenderer(FrameQueue* frames, SubtitleQueue* caption_events,
                       SubtitleQueue* superimpose_events,
                       ClockPosition clock_position,
                       VideoClockAnchor anchor_video_clock,
                       SubtitleViewportChanged subtitle_viewport_changed)
        : impl_(std::make_unique<Impl>(frames, caption_events, superimpose_events,
                                      std::move(clock_position),
                                      std::move(anchor_video_clock),
                                      std::move(subtitle_viewport_changed))) {}
GlRenderer::~GlRenderer() = default;
bool GlRenderer::Start() { return impl_->Start(); }
bool GlRenderer::Stop(int64_t timeout_ms) { return impl_->Stop(timeout_ms); }
void GlRenderer::SetWindow(ANativeWindow* window) { impl_->SetWindow(window); }
void GlRenderer::SetSerial(int serial) { impl_->SetSerial(serial); }
void GlRenderer::SetSubtitlesEnabled(bool enabled) { impl_->SetSubtitlesEnabled(enabled); }
void GlRenderer::ClearSubtitleSource(SubtitleSource source) {
    impl_->ClearSubtitleSource(source);
}
double GlRenderer::AvSyncMs() const { return impl_->AvSyncMs(); }
bool GlRenderer::IsSerialComplete(int serial) const { return impl_->IsSerialComplete(serial); }

} // namespace aribplayer
