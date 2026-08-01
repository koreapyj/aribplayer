#include "player/PlayerCore.h"

#include "audio/AudioDecoder.h"
#include "audio/AudioSink.h"
#include "audio/PcmRing.h"
#include "common/MediaQueues.h"
#include "demux/FdAvio.h"
#include "player/Callbacks.h"
#include "subtitle/SubtitleDecoder.h"
#include "sync/PlaybackClock.h"
#include "video/GlRenderer.h"
#include "video/VideoDecoder.h"

#include <android/log.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <unistd.h>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
}

namespace aribplayer {
namespace {
constexpr char kTag[] = "aribplayer-core";
constexpr int kStateIdle = 0;
constexpr int kStateOpening = 1;
constexpr int kStatePrepared = 2;
constexpr int kStatePlaying = 3;
constexpr int kStatePaused = 4;
constexpr int kStateEnded = 5;
constexpr int kStateClosed = 6;
constexpr int kStateError = 7;

std::string AvError(int error) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(error, text, sizeof(text));
    return text;
}

std::string JsonEscape(const char* value) {
    if (value == nullptr) return {};
    std::ostringstream out;
    for (const unsigned char ch : std::string(value)) {
        switch (ch) {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(ch) << std::dec;
                } else {
                    out << static_cast<char>(ch);
                }
        }
    }
    return out.str();
}

AVCodecContext* OpenCodec(const AVStream* stream, const AVCodec* codec,
                          AVDictionary** options, std::string* error) {
    if (stream == nullptr || stream->codecpar == nullptr || codec == nullptr) return nullptr;
    AVCodecContext* context = avcodec_alloc_context3(codec);
    if (context == nullptr) {
        if (error) *error = "Unable to allocate decoder context";
        return nullptr;
    }
    int result = avcodec_parameters_to_context(context, stream->codecpar);
    if (result >= 0) {
        context->pkt_timebase = stream->time_base;
        result = avcodec_open2(context, codec, options);
    }
    if (result < 0) {
        if (error) *error = "Unable to open " + std::string(codec->name) + ": " + AvError(result);
        avcodec_free_context(&context);
        return nullptr;
    }
    return context;
}

AVCodecContext* OpenDecoder(const AVStream* stream, bool prefer_media_codec,
                            bool* is_hardware, AVDictionary** options,
                            std::string* error) {
    if (is_hardware) *is_hardware = false;
    if (stream == nullptr || stream->codecpar == nullptr) return nullptr;

    if (prefer_media_codec) {
        const char* hardware_name = nullptr;
        if (stream->codecpar->codec_id == AV_CODEC_ID_H264) hardware_name = "h264_mediacodec";
        else if (stream->codecpar->codec_id == AV_CODEC_ID_MPEG2VIDEO)
            hardware_name = "mpeg2_mediacodec";
        if (hardware_name != nullptr) {
            if (const AVCodec* hardware = avcodec_find_decoder_by_name(hardware_name)) {
                std::string hardware_error;
                if (AVCodecContext* context = OpenCodec(stream, hardware, options,
                                                        &hardware_error)) {
                    if (is_hardware) *is_hardware = true;
                    return context;
                }
                __android_log_print(ANDROID_LOG_INFO, kTag, "%s; using software decoder",
                                    hardware_error.c_str());
            }
        }
    }

    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (codec == nullptr) {
        if (error) *error = "No software decoder for " +
                std::string(avcodec_get_name(stream->codecpar->codec_id));
        return nullptr;
    }
    return OpenCodec(stream, codec, options, error);
}

AVCodecContext* OpenSubtitleDecoder(const AVStream* stream, int canvas_width,
                                    int canvas_height, const std::string& font_path,
                                    SubtitleSource source, bool ignore_background,
                                    bool force_outline_text, std::string* error) {
    if (stream == nullptr || stream->codecpar == nullptr) return nullptr;
    const AVCodec* codec = avcodec_find_decoder_by_name("libaribcaption");
    if (codec == nullptr) {
        if (error) *error = "libaribcaption decoder is unavailable";
        return nullptr;
    }
    AVCodecContext* context = avcodec_alloc_context3(codec);
    if (context == nullptr) {
        if (error) *error = "Unable to allocate ARIB caption decoder context";
        return nullptr;
    }
    int result = avcodec_parameters_to_context(context, stream->codecpar);
    if (result >= 0) {
        context->pkt_timebase = stream->time_base;
        context->width = std::max(1, canvas_width);
        context->height = std::max(1, canvas_height);
        result = av_opt_set(context->priv_data, "sub_type", "bitmap", 0);
    }
    if (result >= 0) {
        // Bundled ARIB font only. No system-font fallback: the bundled font
        // covers the ARIB repertoire, and a missing file should fail loudly
        // (onSubtitleInfo fontOk=false) instead of degrading silently.
        const std::string fonts = font_path.empty()
                ? "sans-serif" : "file:" + font_path;
        result = av_opt_set(context->priv_data, "font", fonts.c_str(), 0);
    }
    if (result >= 0) {
        result = av_opt_set(context->priv_data, "caption_encoding", "auto", 0);
    }
    if (result >= 0) {
        const char* caption_type = source == SubtitleSource::kSuperimpose
                ? "superimpose" : "caption";
        result = av_opt_set(context->priv_data, "caption_type", caption_type, 0);
    }
    if (result >= 0 && source == SubtitleSource::kCaption) {
        result = av_opt_set_int(context->priv_data, "ignore_background",
                                ignore_background ? 1 : 0, 0);
    }
    if (result >= 0 && source == SubtitleSource::kCaption) {
        result = av_opt_set_int(context->priv_data, "force_outline_text",
                                force_outline_text ? 1 : 0, 0);
    }
    if (result >= 0) {
        result = av_opt_set_image_size(context->priv_data, "canvas_size",
                                       context->width, context->height, 0);
    }
    if (result >= 0) result = avcodec_open2(context, codec, nullptr);
    if (result < 0) {
        if (error) *error = "Unable to open libaribcaption: " + AvError(result);
        avcodec_free_context(&context);
        return nullptr;
    }
    return context;
}

int64_t DurationMs(const AVFormatContext* format) {
    if (format->duration != AV_NOPTS_VALUE && format->duration > 0) {
        return format->duration / (AV_TIME_BASE / 1000);
    }
    int64_t best = 0;
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        const AVStream* stream = format->streams[i];
        if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
            best = std::max(best, av_rescale_q(stream->duration, stream->time_base,
                                               AVRational{1, 1000}));
        }
    }
    return best;
}

int64_t StartOffsetUs(const AVFormatContext* format, int video_index, int audio_index,
                      int subtitle_index, int superimpose_index) {
    if (format->start_time != AV_NOPTS_VALUE) return format->start_time;

    int64_t start_time = AV_NOPTS_VALUE;
    for (const int index : {video_index, audio_index, subtitle_index, superimpose_index}) {
        if (index < 0 || static_cast<unsigned>(index) >= format->nb_streams) continue;
        const AVStream* stream = format->streams[index];
        if (stream->start_time == AV_NOPTS_VALUE) continue;
        const int64_t stream_start = av_rescale_q(stream->start_time, stream->time_base,
                                                  AV_TIME_BASE_Q);
        start_time = start_time == AV_NOPTS_VALUE ? stream_start
                                                   : std::min(start_time, stream_start);
    }
    return start_time == AV_NOPTS_VALUE ? 0 : start_time;
}

bool IsSuperimposeStream(const AVStream* stream) {
    if (stream == nullptr) return false;
    const AVDictionaryEntry* marker =
            av_dict_get(stream->metadata, "arib_type", nullptr, 0);
    return marker != nullptr && marker->value != nullptr &&
           std::string(marker->value) == "superimpose";
}

bool IsUsableAudioStream(const AVFormatContext* format, unsigned stream_index) {
    if (format == nullptr || stream_index >= format->nb_streams) return false;
    const AVCodecParameters* parameters = format->streams[stream_index]->codecpar;
    return parameters != nullptr && parameters->codec_type == AVMEDIA_TYPE_AUDIO &&
           avcodec_find_decoder(parameters->codec_id) != nullptr;
}

int FindProgramAudioStream(AVFormatContext* format, int video_index,
                           int* selected_program_id) {
    if (selected_program_id != nullptr) *selected_program_id = -1;
    if (format == nullptr) return AVERROR_STREAM_NOT_FOUND;

    const AVProgram* selected_program = nullptr;
    for (unsigned program_index = 0; program_index < format->nb_programs; ++program_index) {
        const AVProgram* program = format->programs[program_index];
        for (unsigned stream_offset = 0; stream_offset < program->nb_stream_indexes;
             ++stream_offset) {
            if (static_cast<int>(program->stream_index[stream_offset]) == video_index) {
                selected_program = program;
                break;
            }
        }
        if (selected_program != nullptr) break;
    }

    if (selected_program != nullptr) {
        for (unsigned stream_offset = 0;
             stream_offset < selected_program->nb_stream_indexes; ++stream_offset) {
            const unsigned stream_index = selected_program->stream_index[stream_offset];
            if (IsUsableAudioStream(format, stream_index)) {
                if (selected_program_id != nullptr) {
                    *selected_program_id = selected_program->id;
                }
                return static_cast<int>(stream_index);
            }
        }
    }

    // Malformed or audio-only inputs may not associate the selected video with
    // a program. Preserve general container support as a final fallback.
    const int fallback = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1,
                                             video_index, nullptr, 0);
    if (fallback >= 0 && selected_program_id != nullptr) {
        for (unsigned program_index = 0; program_index < format->nb_programs;
             ++program_index) {
            const AVProgram* program = format->programs[program_index];
            for (unsigned stream_offset = 0; stream_offset < program->nb_stream_indexes;
                 ++stream_offset) {
                if (program->stream_index[stream_offset] == static_cast<unsigned>(fallback)) {
                    *selected_program_id = program->id;
                    return fallback;
                }
            }
        }
    }
    return fallback;
}

const AVProgram* FindProgramById(const AVFormatContext* format, int program_id) {
    if (format == nullptr || program_id < 0) return nullptr;
    for (unsigned i = 0; i < format->nb_programs; ++i) {
        if (format->programs[i] != nullptr && format->programs[i]->id == program_id) {
            return format->programs[i];
        }
    }
    return nullptr;
}

bool ProgramContainsStream(const AVFormatContext* format, int program_id,
                           int stream_index) {
    const AVProgram* program = FindProgramById(format, program_id);
    if (program == nullptr || stream_index < 0) return false;
    for (unsigned offset = 0; offset < program->nb_stream_indexes; ++offset) {
        if (program->stream_index[offset] == static_cast<unsigned>(stream_index)) return true;
    }
    return false;
}

int FindProgramIdForStream(const AVFormatContext* format, int stream_index) {
    if (format == nullptr || stream_index < 0) return -1;
    for (unsigned program_index = 0; program_index < format->nb_programs; ++program_index) {
        const AVProgram* program = format->programs[program_index];
        if (program != nullptr && ProgramContainsStream(format, program->id, stream_index)) {
            return program->id;
        }
    }
    return -1;
}

struct AribSubtitleStreams {
    int caption_index = -1;
    int superimpose_index = -1;
    int caption_count = 0;
    bool any = false;
};

AribSubtitleStreams FindAribSubtitleStreams(const AVFormatContext* format,
                                            int program_id) {
    AribSubtitleStreams result;
    if (format == nullptr) return result;
    for (unsigned index = 0; index < format->nb_streams; ++index) {
        AVStream* stream = format->streams[index];
        const AVCodecParameters* parameters = stream->codecpar;
        if (parameters == nullptr || parameters->codec_type != AVMEDIA_TYPE_SUBTITLE ||
            parameters->codec_id != AV_CODEC_ID_ARIB_CAPTION ||
            (program_id >= 0 && !ProgramContainsStream(
                    format, program_id, static_cast<int>(index)))) {
            continue;
        }
        result.any = true;
        if (IsSuperimposeStream(stream)) {
            if (result.superimpose_index < 0) {
                result.superimpose_index = static_cast<int>(index);
            }
        } else {
            if (result.caption_index < 0) result.caption_index = static_cast<int>(index);
            ++result.caption_count;
        }
    }
    return result;
}

std::vector<int> ProgramAudioStreams(const AVFormatContext* format, int program_id) {
    std::vector<int> streams;
    const AVProgram* program = FindProgramById(format, program_id);
    if (program != nullptr) {
        for (unsigned offset = 0; offset < program->nb_stream_indexes; ++offset) {
            const unsigned index = program->stream_index[offset];
            if (IsUsableAudioStream(format, index)) {
                streams.push_back(static_cast<int>(index));
            }
        }
    }
    return streams;
}

struct ProgramSnapshot {
    int id = -1;
    int pmt_version = -1;
    int pcr_pid = -1;
    std::vector<unsigned> stream_indexes;

    bool operator==(const ProgramSnapshot& other) const {
        return id == other.id && pmt_version == other.pmt_version &&
               pcr_pid == other.pcr_pid && stream_indexes == other.stream_indexes;
    }
    bool operator!=(const ProgramSnapshot& other) const { return !(*this == other); }
};

ProgramSnapshot CaptureProgramSnapshot(const AVFormatContext* format, int program_id) {
    ProgramSnapshot snapshot;
    const AVProgram* program = FindProgramById(format, program_id);
    if (program == nullptr) return snapshot;
    snapshot.id = program->id;
    snapshot.pmt_version = program->pmt_version;
    snapshot.pcr_pid = program->pcr_pid;
    snapshot.stream_indexes.assign(program->stream_index,
                                   program->stream_index + program->nb_stream_indexes);
    std::sort(snapshot.stream_indexes.begin(), snapshot.stream_indexes.end());
    return snapshot;
}

std::string BuildTrackJson(const AVFormatContext* format, int selected_program_id,
                           int video_index, int audio_index, int audio_dual_mono_mode,
                           int subtitle_index, int superimpose_index,
                           const std::unordered_set<int>& dual_mono_streams) {
    std::vector<int> program_ids(format->nb_streams, -1);
    for (unsigned p = 0; p < format->nb_programs; ++p) {
        const AVProgram* program = format->programs[p];
        for (unsigned s = 0; s < program->nb_stream_indexes; ++s) {
            if (program->stream_index[s] < format->nb_streams) {
                program_ids[program->stream_index[s]] = program->id;
            }
        }
    }

    const std::vector<int> selected_audio = ProgramAudioStreams(format, selected_program_id);
    std::vector<int> audio_positions(format->nb_streams, -1);
    for (std::size_t i = 0; i < selected_audio.size(); ++i) {
        audio_positions[static_cast<std::size_t>(selected_audio[i])] = static_cast<int>(i);
    }

    std::ostringstream out;
    out << "{\"selectedProgramId\":" << selected_program_id << ",\"programs\":[";
    for (unsigned p = 0; p < format->nb_programs; ++p) {
        if (p != 0) out << ',';
        const AVProgram* program = format->programs[p];
        out << "{\"id\":" << program->id
            << ",\"pmtVersion\":" << program->pmt_version
            << ",\"pcrPid\":" << program->pcr_pid
            << ",\"streams\":[";
        for (unsigned s = 0; s < program->nb_stream_indexes; ++s) {
            if (s != 0) out << ',';
            out << program->stream_index[s];
        }
        out << "]}";
    }
    out << "],\"streams\":[";
    bool first_entry = true;
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        const AVStream* stream = format->streams[i];
        const AVCodecParameters* parameters = stream->codecpar;
        if (parameters == nullptr) continue;
        const AVDictionaryEntry* language = av_dict_get(stream->metadata, "language", nullptr, 0);
        const AVDictionaryEntry* title = av_dict_get(stream->metadata, "title", nullptr, 0);
        const AVDictionaryEntry* component_tag =
                av_dict_get(stream->metadata, "component_tag", nullptr, 0);
        const char* media_type = av_get_media_type_string(parameters->codec_type);
        const char* codec = avcodec_get_name(parameters->codec_id);
        const bool is_selected_program_audio =
                parameters->codec_type == AVMEDIA_TYPE_AUDIO &&
                program_ids[i] == selected_program_id && audio_positions[i] >= 0;
        const bool is_dual_mono = is_selected_program_audio &&
                dual_mono_streams.count(static_cast<int>(i)) != 0;
        const int variants = is_dual_mono ? 2 : 1;
        for (int variant = 0; variant < variants; ++variant) {
            if (!first_entry) out << ',';
            first_entry = false;
            const int dual_mono_mode = is_dual_mono ? variant + 1 : -1;
            const bool selected = parameters->codec_type == AVMEDIA_TYPE_AUDIO
                    ? static_cast<int>(i) == audio_index &&
                      (is_dual_mono ? audio_dual_mono_mode == dual_mono_mode : true)
                    : (static_cast<int>(i) == video_index ||
                       static_cast<int>(i) == subtitle_index ||
                       static_cast<int>(i) == superimpose_index);
            out << "{\"index\":" << i
                << ",\"streamIndex\":" << i
                << ",\"programId\":" << program_ids[i]
                << ",\"type\":\"" << JsonEscape(media_type ? media_type : "unknown") << '"'
                << ",\"codec\":\"" << JsonEscape(codec) << '"'
                << ",\"disposition\":" << stream->disposition
                << ",\"selected\":" << (selected ? "true" : "false")
                << ",\"dualMono\":" << (is_dual_mono ? "true" : "false")
                << ",\"dualMonoMode\":" << dual_mono_mode;
            if (is_selected_program_audio) {
                out << ",\"pmtOrder\":" << audio_positions[i]
                    << ",\"role\":\""
                    << (audio_positions[i] == 0 ? "main" : "secondary") << '"';
            } else if (IsSuperimposeStream(stream)) {
                out << ",\"role\":\"superimpose\"";
            }
            if (language != nullptr) {
                out << ",\"language\":\"" << JsonEscape(language->value) << '"'
                    << ",\"lang\":\"" << JsonEscape(language->value) << '"';
            }
            if (title != nullptr) out << ",\"title\":\"" << JsonEscape(title->value) << '"';
            if (component_tag != nullptr) {
                out << ",\"componentTag\":\"" << JsonEscape(component_tag->value) << '"';
            }
            out << '}';
        }
    }
    out << "]}";
    return out.str();
}
} // namespace

class PlayerCore::Impl {
public:
    Impl(JavaVM* vm, JNIEnv* env, jobject callback)
            : callbacks_(std::make_unique<player::Callbacks>(vm, env, callback)),
              clock_(std::make_unique<PlaybackClock>()),
              control_thread_(&Impl::ControlLoop, this) {
        callbacks_->onState(kStateIdle);
    }

    ~Impl() {
        auto done = std::make_shared<std::promise<void>>();
        auto future = done->get_future();
        Enqueue(Command{CommandType::kShutdown, -1, {}, {}, 0, nullptr, done});
        future.wait();
        if (control_thread_.joinable()) control_thread_.join();
    }

    bool Open(int fd, std::string font, std::string name, int64_t start_position_ms) {
        if (fd < 0 || shutting_down_.load()) return false;
        Command command;
        command.type = CommandType::kOpen;
        command.fd = fd;
        command.text = std::move(font);
        command.display_name = std::move(name);
        command.value = std::max<int64_t>(0, start_position_ms);
        return Enqueue(std::move(command));
    }

    void SetSurface(ANativeWindow* window) {
        Command command;
        command.type = CommandType::kSetSurface;
        command.window = window;
        if (!Enqueue(std::move(command)) && window != nullptr) ANativeWindow_release(window);
    }

    void Play() { Enqueue(Command{CommandType::kPlay}); }
    void Pause() { Enqueue(Command{CommandType::kPause}); }
    void SeekUi(int64_t position_ms) {
        Command command{CommandType::kSeek};
        command.value = std::max<int64_t>(0, position_ms) +
                        start_offset_us_.load(std::memory_order_acquire) / (AV_TIME_BASE / 1000);
        Enqueue(std::move(command));
    }
    void SetVideoMode(int mode) {
        Command command{CommandType::kVideoMode};
        command.value = mode;
        Enqueue(std::move(command));
    }
    void SetSubtitles(bool enabled) {
        Command command{CommandType::kSubtitles};
        command.value = enabled ? 1 : 0;
        Enqueue(std::move(command));
    }
    void SetCaptionStyle(bool ignore_background, bool force_outline_text) {
        const bool background_changed =
                caption_ignore_background_.exchange(ignore_background,
                                                    std::memory_order_acq_rel) !=
                ignore_background;
        const bool outline_changed =
                caption_force_outline_.exchange(force_outline_text,
                                                std::memory_order_acq_rel) !=
                force_outline_text;
        if (background_changed || outline_changed) {
            caption_style_generation_.fetch_add(1, std::memory_order_release);
        }
    }
    void SelectAudioTrack(int stream_index, int dual_mono_mode) {
        Command command{CommandType::kAudioTrack};
        command.value = stream_index;
        command.secondary_value = dual_mono_mode;
        Enqueue(std::move(command));
    }
    int64_t Duration() const { return duration_ms_.load(std::memory_order_acquire); }

    std::string Stats() const {
        VideoDecoderStats video_stats;
        double av_sync_ms = 0.0;
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            if (video_decoder_) video_stats = video_decoder_->GetStats();
            else {
                video_stats.decoder = "none";
                video_stats.filter.requested_mode = video_mode_.load(std::memory_order_acquire);
                video_stats.filter.effective_mode = 0;
                video_stats.filter.backend = "none";
            }
            if (renderer_) av_sync_ms = renderer_->AvSyncMs();
        }
        std::ostringstream out;
        out << std::fixed << std::setprecision(2)
            << "{\"decoder\":\"" << JsonEscape(video_stats.decoder.c_str()) << '"'
            << ",\"decoderHw\":" << (video_stats.decoder_hw ? "true" : "false")
            << ",\"filterMode\":" << video_stats.filter.effective_mode
            << ",\"filterBackend\":\"" << JsonEscape(video_stats.filter.backend.c_str()) << '"'
            << ",\"inFps\":" << video_stats.filter.in_fps
            << ",\"outFps\":" << video_stats.filter.out_fps
            << ",\"avgFilterMs\":" << video_stats.filter.avg_filter_ms
            << ",\"avSyncMs\":" << av_sync_ms << '}';
        return out.str();
    }

    void Close() {
        if (!shutting_down_.load()) Enqueue(Command{CommandType::kClose});
    }

private:
    enum class CommandType {
        kOpen, kPlay, kPause, kSeek, kSetSurface, kVideoMode, kSubtitles, kAudioTrack,
        kClose, kShutdown
    };
    struct Command {
        CommandType type = CommandType::kPlay;
        int fd = -1;
        std::string text;
        std::string display_name;
        int64_t value = 0;
        ANativeWindow* window = nullptr;
        std::shared_ptr<std::promise<void>> done;
        int secondary_value = -1;
    };

    bool Enqueue(Command command) {
        std::lock_guard<std::mutex> lock(command_mutex_);
        if (shutting_down_.load() && command.type != CommandType::kShutdown) return false;
        commands_.push_back(std::move(command));
        command_cv_.notify_one();
        return true;
    }

    void ControlLoop() {
        auto last_position = std::chrono::steady_clock::now();
        bool exit = false;
        while (!exit) {
            Command command;
            bool have_command = false;
            {
                std::unique_lock<std::mutex> lock(command_mutex_);
                command_cv_.wait_for(lock, std::chrono::milliseconds(100),
                                     [this] { return !commands_.empty(); });
                if (!commands_.empty()) {
                    command = std::move(commands_.front());
                    commands_.pop_front();
                    have_command = true;
                }
            }
            if (have_command) {
                switch (command.type) {
                    case CommandType::kOpen:
                        CloseSession(false);
                        StartSession(command.fd, std::move(command.text),
                                     std::move(command.display_name), command.value);
                        command.fd = -1;
                        break;
                    case CommandType::kPlay: ApplyPlay(); break;
                    case CommandType::kPause: ApplyPause(); break;
                    case CommandType::kSeek:
                        if (seekable_.load() && prepared_.load()) {
                            seek_serial_.store(-1, std::memory_order_release);
                            seek_ms_.store(command.value, std::memory_order_release);
                        }
                        break;
                    case CommandType::kSetSurface: ApplySurface(command.window); command.window = nullptr; break;
                    case CommandType::kVideoMode: ApplyVideoMode(static_cast<int>(command.value)); break;
                    case CommandType::kSubtitles: ApplySubtitles(command.value != 0); break;
                    case CommandType::kAudioTrack:
                        RequestAudioTrack(static_cast<int>(command.value), command.secondary_value);
                        break;
                    case CommandType::kClose: CloseSession(true); break;
                    case CommandType::kShutdown:
                        shutting_down_.store(true);
                        CloseSession(false);
                        exit = true;
                        break;
                }
                if (command.window != nullptr) ANativeWindow_release(command.window);
                if (command.fd >= 0) ::close(command.fd);
                if (command.done) command.done->set_value();
            }

            const auto now = std::chrono::steady_clock::now();
            if (prepared_.load() && now - last_position >= std::chrono::milliseconds(500)) {
                const int64_t pts_ms = static_cast<int64_t>(
                        std::max(0.0, clock_->PositionSeconds()) * 1000.0);
                const int64_t offset_ms = start_offset_us_.load(std::memory_order_acquire) /
                                          (AV_TIME_BASE / 1000);
                callbacks_->onPositionMs(std::max<int64_t>(0, pts_ms - offset_ms));
                last_position = now;
            }
        }

        std::lock_guard<std::mutex> lock(command_mutex_);
        for (Command& command : commands_) {
            if (command.fd >= 0) ::close(command.fd);
            if (command.window != nullptr) ANativeWindow_release(command.window);
            if (command.done) command.done->set_value();
        }
        commands_.clear();
    }

    void StartSession(int fd, std::string font, std::string display_name,
                      int64_t start_position_ms) {
        stop_requested_.store(false);
        prepared_.store(false);
        seekable_.store(false);
        has_audio_master_.store(false);
        duration_ms_.store(0);
        seek_ms_.store(-1);
        seek_serial_.store(-1);
        serial_.store(0);
        audio_serial_.store(0);
        requested_audio_stream_.store(-1);
        requested_dual_mono_mode_.store(-1);
        audio_switch_generation_.store(0);
        detected_dual_mono_stream_.store(-1);
        active_audio_stream_.store(-1);
        active_dual_mono_mode_.store(-1);
        video_clock_anchored_.store(false);
        start_offset_us_.store(0, std::memory_order_release);
        font_path_ = std::move(font);
        display_name_ = std::move(display_name);
        video_packets_ = std::make_unique<PacketQueue>(64, 12U * 1024U * 1024U);
        audio_packets_ = std::make_unique<PacketQueue>(128, 4U * 1024U * 1024U);
        subtitle_packets_ = std::make_unique<PacketQueue>(32, 1U * 1024U * 1024U);
        superimpose_packets_ = std::make_unique<PacketQueue>(32, 1U * 1024U * 1024U);
        video_frames_ = std::make_unique<FrameQueue>(4);
        subtitle_events_ = std::make_unique<SubtitleQueue>(64);
        superimpose_events_ = std::make_unique<SubtitleQueue>(64);
        clock_->SetAudioSink(nullptr);
        clock_->Reset(0.0);
        callbacks_->onState(kStateOpening);
        demux_thread_ = std::thread(
                &Impl::DemuxLoop, this, fd, std::max<int64_t>(0, start_position_ms));
    }

    void CloseSession(bool notify) {
        stop_requested_.store(true, std::memory_order_release);
        want_playing_.store(false, std::memory_order_release);
        seek_ms_.store(-1);
        seek_serial_.store(-1);
        if (video_packets_) video_packets_->Abort();
        if (audio_packets_) audio_packets_->Abort();
        if (subtitle_packets_) subtitle_packets_->Abort();
        if (superimpose_packets_) superimpose_packets_->Abort();
        if (video_frames_) video_frames_->Abort();
        if (subtitle_events_) subtitle_events_->Abort();
        if (superimpose_events_) superimpose_events_->Abort();
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            if (audio_sink_) audio_sink_->Stop();
        }
        if (demux_thread_.joinable()) demux_thread_.join();

        std::lock_guard<std::mutex> lock(session_mutex_);
        if (video_decoder_) video_decoder_->Stop();
        if (audio_decoder_) audio_decoder_->Stop();
        if (subtitle_decoder_) subtitle_decoder_->Stop();
        if (superimpose_decoder_) superimpose_decoder_->Stop();
        if (renderer_) renderer_->Stop();
        if (audio_sink_) audio_sink_->Close();
        clock_->SetAudioSink(nullptr);
        renderer_.reset();
        video_decoder_.reset();
        audio_decoder_.reset();
        subtitle_decoder_.reset();
        superimpose_decoder_.reset();
        audio_sink_.reset();
        pcm_ring_.reset();
        video_frames_.reset();
        subtitle_events_.reset();
        superimpose_events_.reset();
        video_packets_.reset();
        audio_packets_.reset();
        subtitle_packets_.reset();
        superimpose_packets_.reset();
        if (pending_window_ != nullptr) {
            ANativeWindow_release(pending_window_);
            pending_window_ = nullptr;
        }
        prepared_.store(false);
        seekable_.store(false);
        has_audio_master_.store(false);
        duration_ms_.store(0);
        clock_->Pause();
        if (notify) callbacks_->onState(kStateClosed);
    }

    void ApplyPlay() {
        want_playing_.store(true);
        if (!prepared_.load()) return;
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (audio_sink_ && !audio_sink_->is_running() && !audio_sink_->Start()) {
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "AAudio start failed; continuing with video clock");
            has_audio_master_.store(false, std::memory_order_release);
            clock_->SetAudioSink(nullptr);
        }
        clock_->Resume();
        callbacks_->onState(kStatePlaying);
    }

    void ApplyPause() {
        want_playing_.store(false);
        if (!prepared_.load()) return;
        std::lock_guard<std::mutex> lock(session_mutex_);
        clock_->Pause();
        if (audio_sink_) audio_sink_->Pause();
        callbacks_->onState(kStatePaused);
    }

    void ApplySurface(ANativeWindow* window) {
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (renderer_) {
            renderer_->SetWindow(window);
        } else {
            if (pending_window_ != nullptr) ANativeWindow_release(pending_window_);
            pending_window_ = window;
        }
    }

    void RequestAudioTrack(int stream_index, int dual_mono_mode) {
        if (stream_index < 0 ||
            (dual_mono_mode != -1 && dual_mono_mode != 1 && dual_mono_mode != 2)) {
            return;
        }
        requested_audio_stream_.store(stream_index, std::memory_order_relaxed);
        requested_dual_mono_mode_.store(dual_mono_mode, std::memory_order_relaxed);
        audio_switch_generation_.fetch_add(1, std::memory_order_release);
    }

    void ApplySubtitles(bool enabled) {
        subtitles_enabled_.store(enabled, std::memory_order_release);
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (renderer_) renderer_->SetSubtitlesEnabled(enabled);
    }

    void ApplyVideoMode(int mode) {
        mode = std::clamp(mode, static_cast<int>(VideoMode::kOff),
                          static_cast<int>(VideoMode::kDeinterlace));
        const int previous = video_mode_.exchange(mode, std::memory_order_acq_rel);
        bool has_video_decoder = false;
        const bool is_prepared = prepared_.load(std::memory_order_acquire);
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            has_video_decoder = video_decoder_ != nullptr;
            if (video_decoder_) {
                video_decoder_->SetVideoMode(static_cast<VideoMode>(mode));
                if (!is_prepared && previous != mode) {
                    video_decoder_->Flush(serial_.load(std::memory_order_acquire));
                }
            }
        }
        if (previous == mode || !is_prepared || !has_video_decoder) return;

        const int64_t position_ms = static_cast<int64_t>(
                std::max(0.0, clock_->PositionSeconds()) * 1000.0);
        const int next_serial = serial_.fetch_add(1, std::memory_order_acq_rel) + 1;
        const bool seekable = seekable_.load(std::memory_order_acquire);
        if (seekable) {
            // PerformSeek applies the serial flush once after avformat_seek_file.
            // Flushing here as well causes an avoidable pause/start bounce and can
            // consume the first packet of the new generation.
            seek_serial_.store(next_serial, std::memory_order_release);
            seek_ms_.store(position_ms, std::memory_order_release);
            return;
        }
        FlushPipeline(next_serial, position_ms, false);
    }

    bool PutPacket(PacketQueue& queue, const AVPacket& packet, int serial) {
        while (!stop_requested_.load(std::memory_order_acquire)) {
            const QueueResult result = queue.Put(packet, serial, false);
            if (result == QueueResult::kOk) return true;
            if (result == QueueResult::kAborted) return false;
            if (seek_ms_.load(std::memory_order_acquire) >= 0) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }

    bool PutSubtitlePacket(PacketQueue& queue, const AVPacket& packet, int serial) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        while (!stop_requested_.load(std::memory_order_acquire)) {
            const QueueResult result = queue.Put(packet, serial, false);
            if (result == QueueResult::kOk) return true;
            if (result == QueueResult::kAborted) return false;
            if (seek_ms_.load(std::memory_order_acquire) >= 0) return false;
            if (std::chrono::steady_clock::now() >= deadline) {
                __android_log_print(ANDROID_LOG_WARN, kTag,
                                    "Subtitle packet queue remained full; dropping newest packet");
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }

    bool PutEof(PacketQueue& queue, int serial) {
        while (!stop_requested_.load(std::memory_order_acquire)) {
            const QueueResult result = queue.PutEof(serial, false);
            if (result == QueueResult::kOk) return true;
            if (result == QueueResult::kAborted) return false;
            if (seek_ms_.load(std::memory_order_acquire) >= 0) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }

    void FlushPipeline(int next_serial, int64_t position_ms, bool catch_up) {
        audio_serial_.store(next_serial, std::memory_order_release);
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (renderer_) renderer_->SetSerial(next_serial);
        if (audio_sink_) audio_sink_->Pause();
        if (video_decoder_) {
            if (catch_up) video_decoder_->FlushForSeek(next_serial, position_ms * 1000);
            else video_decoder_->Flush(next_serial);
        }
        if (audio_decoder_) {
            if (catch_up) audio_decoder_->FlushForSeek(next_serial, position_ms * 1000);
            else audio_decoder_->Flush(next_serial);
        }
        if (subtitle_decoder_) subtitle_decoder_->Flush(next_serial);
        if (superimpose_decoder_) superimpose_decoder_->Flush(next_serial);
        // Stop old-generation writes first, then make all old PCM unreachable,
        // and only then expose the new sink timeline generation to callbacks.
        if (pcm_ring_) pcm_ring_->Clear();
        if (audio_sink_) audio_sink_->ResetTimeline();
        clock_->Reset(position_ms / 1000.0);
        video_clock_anchored_.store(true, std::memory_order_release);
        if (want_playing_.load(std::memory_order_acquire)) {
            if (audio_sink_ && !audio_sink_->Resume()) {
                __android_log_print(ANDROID_LOG_ERROR, kTag,
                                    "AAudio resume failed after pipeline flush; using video clock");
                has_audio_master_.store(false, std::memory_order_release);
                clock_->SetAudioSink(nullptr);
            }
            clock_->Resume();
        }
    }

    bool PerformSeek(AVFormatContext* format, int64_t position_ms, int reserved_serial) {
        const int64_t target = av_rescale_q(position_ms, AVRational{1, 1000}, AV_TIME_BASE_Q);
        const int result = avformat_seek_file(format, -1, std::numeric_limits<int64_t>::min(),
                                              target, target, AVSEEK_FLAG_BACKWARD);
        if (result < 0) {
            callbacks_->onError(6, "Seek failed: " + AvError(result));
            return false;
        }
        const int next_serial = reserved_serial >= 0
                ? reserved_serial : serial_.fetch_add(1, std::memory_order_acq_rel) + 1;
        FlushPipeline(next_serial, position_ms, true);
        const int64_t offset_ms = start_offset_us_.load(std::memory_order_acquire) /
                                  (AV_TIME_BASE / 1000);
        callbacks_->onPositionMs(std::max<int64_t>(0, position_ms - offset_ms));
        return true;
    }

    bool OpenAudioPathLocked(AVFormatContext* format, int stream_index,
                             int dual_mono_mode, std::string* error,
                             int initial_serial = -1,
                             int64_t discard_before_us = AV_NOPTS_VALUE) {
        if (!IsUsableAudioStream(format, static_cast<unsigned>(stream_index))) {
            if (error) *error = "Selected audio stream is not decodable";
            return false;
        }

        AVStream* stream = format->streams[stream_index];
        AVDictionary* codec_options = nullptr;
        if (stream->codecpar->codec_id == AV_CODEC_ID_AAC) {
            if (dual_mono_mode == 1) {
                av_dict_set(&codec_options, "dual_mono_mode", "main", 0);
            } else if (dual_mono_mode == 2) {
                av_dict_set(&codec_options, "dual_mono_mode", "sub", 0);
            }
        }
        bool unused_hardware = false;
        AVCodecContext* decoder = OpenDecoder(stream, false, &unused_hardware,
                                              &codec_options, error);
        av_dict_free(&codec_options);
        if (decoder == nullptr) return false;

        const int sample_rate = decoder->sample_rate > 0 ? decoder->sample_rate : 48000;
        const bool replace_output = pcm_ring_ == nullptr ||
                pcm_ring_->sample_rate() != sample_rate || audio_sink_ == nullptr;
        if (replace_output) {
            if (audio_sink_) audio_sink_->Close();
            audio_sink_.reset();
            pcm_ring_ = std::make_unique<PcmRing>(sample_rate,
                                                  static_cast<std::size_t>(sample_rate));
            audio_sink_ = std::make_unique<AudioSink>(*pcm_ring_);
            if (!audio_sink_->Open()) {
                if (error) *error = "Unable to open AAudio output";
                audio_sink_.reset();
                pcm_ring_.reset();
                avcodec_free_context(&decoder);
                return false;
            }
        }

        audio_decoder_ = std::make_unique<AudioDecoder>(
                decoder, stream->time_base, *audio_packets_, *pcm_ring_,
                [this, stream_index] {
                    int expected = -1;
                    detected_dual_mono_stream_.compare_exchange_strong(
                            expected, stream_index, std::memory_order_acq_rel);
                });
        active_audio_stream_.store(stream_index, std::memory_order_release);
        active_dual_mono_mode_.store(dual_mono_mode, std::memory_order_release);
        has_audio_master_.store(true, std::memory_order_release);
        clock_->SetAudioSink(audio_sink_.get());
        if (initial_serial >= 0 && discard_before_us != AV_NOPTS_VALUE) {
            audio_decoder_->FlushForSeek(initial_serial, discard_before_us);
        }
        audio_decoder_->Start();
        return true;
    }

    bool SwitchAudioTrack(AVFormatContext* format, int selected_program_id,
                          int stream_index, int dual_mono_mode,
                          const std::unordered_set<int>& dual_mono_streams,
                          bool log_switch,
                          int64_t discard_before_us = AV_NOPTS_VALUE) {
        const std::vector<int> program_audio = ProgramAudioStreams(format, selected_program_id);
        const bool belongs_to_program = std::find(program_audio.begin(), program_audio.end(),
                                                  stream_index) != program_audio.end();
        if ((!program_audio.empty() && !belongs_to_program) ||
            !IsUsableAudioStream(format, static_cast<unsigned>(stream_index))) {
            callbacks_->onError(4, "Requested audio track is not in the selected program");
            return false;
        }
        if (dual_mono_mode != -1 && dual_mono_mode != 1 && dual_mono_mode != 2) return false;
        if (dual_mono_streams.count(stream_index) != 0 && dual_mono_mode == -1) {
            dual_mono_mode = 1;
        }

        const int previous_stream = active_audio_stream_.load(std::memory_order_acquire);
        const int previous_mode = active_dual_mono_mode_.load(std::memory_order_acquire);
        if (stream_index == previous_stream && dual_mono_mode == previous_mode) return true;

        const double current_position = std::max(0.0, clock_->PositionSeconds());
        const bool resume_audio = want_playing_.load(std::memory_order_acquire);
        const int next_audio_serial = audio_serial_.fetch_add(1, std::memory_order_acq_rel) + 1;
        std::string error;
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            if (audio_sink_) audio_sink_->Pause();
            if (audio_decoder_) audio_decoder_->Stop();
            audio_decoder_.reset();
            audio_packets_->Reset();
            if (pcm_ring_) pcm_ring_->Clear();
            if (audio_sink_) audio_sink_->ResetTimeline();

            if (!OpenAudioPathLocked(
                        format, stream_index, dual_mono_mode, &error,
                        next_audio_serial, discard_before_us)) {
                std::string rollback_error;
                if (previous_stream >= 0 &&
                    OpenAudioPathLocked(
                            format, previous_stream, previous_mode, &rollback_error,
                            next_audio_serial, discard_before_us)) {
                    __android_log_print(ANDROID_LOG_ERROR, kTag,
                                        "Audio track switch failed: %s; restored stream #%d",
                                        error.c_str(), previous_stream);
                    clock_->Reset(current_position);
                    if (resume_audio) {
                        if (audio_sink_->Resume()) clock_->Resume();
                    }
                } else {
                    has_audio_master_.store(false, std::memory_order_release);
                    clock_->SetAudioSink(nullptr);
                    __android_log_print(ANDROID_LOG_ERROR, kTag,
                                        "Audio track switch failed: %s; rollback failed: %s",
                                        error.c_str(), rollback_error.c_str());
                }
                callbacks_->onError(4, "Unable to switch audio track: " + error);
                return false;
            }
            audio_serial_.store(next_audio_serial, std::memory_order_release);
            clock_->Reset(current_position);
            video_clock_anchored_.store(true, std::memory_order_release);
            if (resume_audio) {
                if (!audio_sink_->Resume()) {
                    has_audio_master_.store(false, std::memory_order_release);
                    clock_->SetAudioSink(nullptr);
                    __android_log_print(ANDROID_LOG_ERROR, kTag,
                                        "AAudio resume failed after audio track switch; using video clock");
                }
                clock_->Resume();
            }
        }
        if (log_switch) {
            __android_log_print(ANDROID_LOG_INFO, kTag,
                                "audio track switched to #%d (dual_mono=%d)",
                                stream_index, dual_mono_mode);
        }
        return true;
    }

    void RequestSubtitleViewport(int width, int height) {
        if (width <= 0 || height <= 0) return;
        const int previous_width = subtitle_canvas_width_.exchange(
                width, std::memory_order_acq_rel);
        const int previous_height = subtitle_canvas_height_.exchange(
                height, std::memory_order_acq_rel);
        if (previous_width != width || previous_height != height) {
            subtitle_canvas_generation_.fetch_add(1, std::memory_order_release);
        }
    }

    std::pair<int, int> SubtitleCanvasSize(
            const AVCodecParameters* video_parameters) const {
        const int requested_width =
                subtitle_canvas_width_.load(std::memory_order_acquire);
        const int requested_height =
                subtitle_canvas_height_.load(std::memory_order_acquire);
        if (requested_width > 0 && requested_height > 0) {
            return {requested_width, requested_height};
        }
        return {
                video_parameters != nullptr && video_parameters->width > 0
                        ? video_parameters->width : 1920,
                video_parameters != nullptr && video_parameters->height > 0
                        ? video_parameters->height : 1080,
        };
    }

    bool RebindSubtitlePath(AVFormatContext* format, int video_index, int stream_index,
                            SubtitleSource source, int serial,
                            std::unique_ptr<SubtitleDecoder>& decoder,
                            PacketQueue& packets, SubtitleQueue& events) {
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (decoder) {
            decoder->Stop();
            decoder.reset();
        }
        packets.Reset();
        events.Reset();
        if (renderer_) renderer_->ClearSubtitleSource(source);

        bool enabled = false;
        if (stream_index >= 0 && video_index >= 0 && video_decoder_ && renderer_) {
            AVStream* stream = format->streams[stream_index];
            const AVCodecParameters* video_parameters = format->streams[video_index]->codecpar;
            const auto [canvas_width, canvas_height] =
                    SubtitleCanvasSize(video_parameters);
            const bool apply_user_style = source == SubtitleSource::kCaption;
            std::string error;
            AVCodecContext* context = OpenSubtitleDecoder(
                    stream, canvas_width, canvas_height, font_path_, source,
                    apply_user_style && caption_ignore_background_.load(
                            std::memory_order_acquire),
                    apply_user_style && caption_force_outline_.load(
                            std::memory_order_acquire),
                    &error);
            if (context == nullptr) {
                __android_log_print(ANDROID_LOG_ERROR, kTag,
                                    "ARIB %s decoder disabled during reconfiguration: %s",
                                    source == SubtitleSource::kSuperimpose
                                            ? "superimpose" : "caption",
                                    error.c_str());
            } else {
                decoder = std::make_unique<SubtitleDecoder>(
                        context, packets, events, source,
                        context->width, context->height);
                decoder->Start();
                enabled = true;
                __android_log_print(ANDROID_LOG_INFO, kTag,
                                    "Bound ARIB %s stream #%d at canvas %dx%d",
                                    source == SubtitleSource::kSuperimpose
                                            ? "superimpose" : "caption",
                                    stream_index, context->width, context->height);
            }
        }

        // Clear any retained overlay from the old ES. If a replacement decoder
        // was started above, Start() reset this queue before the clear is pushed.
        SubtitleEvent clear;
        clear.serial = serial;
        clear.source = source;
        double position_seconds = 0.0;
        if (clock_->TryPositionSeconds(&position_seconds) &&
            std::isfinite(position_seconds)) {
            clear.start_pts_us = static_cast<int64_t>(position_seconds * AV_TIME_BASE);
        }
        clear.end_pts_us = clear.start_pts_us;
        events.Push(std::move(clear), false);
        return enabled;
    }

    void DemuxLoop(int owned_fd, int64_t requested_start_position_ms) {
        std::unique_ptr<FdAvio> io = FdAvio::Create(owned_fd);
        if (!io) {
            Fail(1, "Unable to create file-descriptor input");
            return;
        }

        AVFormatContext* raw_format = avformat_alloc_context();
        if (raw_format == nullptr) {
            Fail(1, "Unable to allocate demuxer");
            return;
        }
        raw_format->pb = io->context();
        raw_format->flags |= AVFMT_FLAG_CUSTOM_IO;
        AVDictionary* options = nullptr;
        av_dict_set(&options, "scan_all_pmts", "1", 0);
        av_dict_set(&options, "merge_pmt_versions", "1", 0);
        av_dict_set(&options, "probesize", "10485760", 0);
        av_dict_set(&options, "analyzeduration", "5000000", 0);
        int result = avformat_open_input(&raw_format, nullptr, nullptr, &options);
        av_dict_free(&options);
        if (result < 0) {
            avformat_free_context(raw_format);
            Fail(2, "Unable to open " + display_name_ + ": " + AvError(result));
            return;
        }
        struct FormatCloser {
            void operator()(AVFormatContext* context) const {
                if (context != nullptr) avformat_close_input(&context);
            }
        };
        std::unique_ptr<AVFormatContext, FormatCloser> format(raw_format);

        result = avformat_find_stream_info(format.get(), nullptr);
        if (result < 0) {
            Fail(3, "Unable to read stream information: " + AvError(result));
            return;
        }
        if (stop_requested_.load()) return;

        const int video_index = av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1, -1,
                                                    nullptr, 0);
        int audio_program_id = -1;
        int audio_index = FindProgramAudioStream(format.get(), video_index,
                                                 &audio_program_id);
        int audio_dual_mono_mode = -1;
        std::unordered_set<int> dual_mono_streams;
        ProgramSnapshot program_snapshot =
                CaptureProgramSnapshot(format.get(), audio_program_id);
        uint64_t handled_audio_switch_generation = 0;
        if (audio_index >= 0) {
            __android_log_print(ANDROID_LOG_INFO, kTag,
                                "Selected audio stream: index=%d program_id=%d policy=first-pmt-order",
                                audio_index, audio_program_id);
        }
        const int video_program_id = FindProgramIdForStream(format.get(), video_index);
        int subtitle_program_id = video_program_id >= 0
                ? video_program_id : audio_program_id;
        ProgramSnapshot subtitle_program_snapshot =
                CaptureProgramSnapshot(format.get(), subtitle_program_id);
        const AribSubtitleStreams initial_subtitle_streams =
                FindAribSubtitleStreams(format.get(), subtitle_program_id);
        int subtitle_index = initial_subtitle_streams.caption_index;
        int superimpose_index = initial_subtitle_streams.superimpose_index;
        int subtitle_stream_count = initial_subtitle_streams.caption_count;
        bool has_arib_subtitle_stream = initial_subtitle_streams.any;
        if (video_index < 0 && audio_index < 0) {
            Fail(4, "No playable video or audio stream found");
            return;
        }

        const int64_t duration_ms = DurationMs(format.get());
        duration_ms_.store(duration_ms);
        const bool source_seekable = io->is_seekable();
        seekable_.store(source_seekable);
        const int64_t start_offset_us = StartOffsetUs(
                format.get(), video_index, audio_index, subtitle_index, superimpose_index);
        start_offset_us_.store(start_offset_us, std::memory_order_release);
        const int64_t start_offset_ms = start_offset_us / (AV_TIME_BASE / 1000);
        int64_t initial_position_ms = start_offset_ms;
        int64_t initial_catchup_target_us = AV_NOPTS_VALUE;
        if (requested_start_position_ms > 0 && source_seekable) {
            const int64_t maximum_resume_ms = duration_ms > 0
                    ? std::max<int64_t>(0, duration_ms - 1000)
                    : requested_start_position_ms;
            const int64_t resume_position_ms = std::min(
                    requested_start_position_ms, maximum_resume_ms);
            if (resume_position_ms > 0) {
                initial_position_ms = start_offset_ms + resume_position_ms;
                initial_catchup_target_us = av_rescale_q(
                        initial_position_ms, AVRational{1, 1000}, AV_TIME_BASE_Q);
                result = avformat_seek_file(
                        format.get(), -1, std::numeric_limits<int64_t>::min(),
                        initial_catchup_target_us, initial_catchup_target_us,
                        AVSEEK_FLAG_BACKWARD);
                if (result < 0) {
                    Fail(6, "Initial seek failed: " + AvError(result));
                    return;
                }
                callbacks_->onPositionMs(resume_position_ms);
            }
        }

        bool video_enabled = false;
        bool audio_enabled = false;
        bool subtitle_enabled = false;
        bool superimpose_enabled = false;
        const uint64_t initial_subtitle_canvas_generation =
                subtitle_canvas_generation_.load(std::memory_order_acquire);
        const uint64_t initial_caption_style_generation =
                caption_style_generation_.load(std::memory_order_acquire);
        const bool font_ok = !font_path_.empty() && ::access(font_path_.c_str(), R_OK) == 0;
        if (has_arib_subtitle_stream && !font_ok) {
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "ARIB caption font is missing or unreadable: %s; falling back to sans-serif",
                                font_path_.empty() ? "(empty path)" : font_path_.c_str());
        }
        if (!InitializeStreams(format.get(), video_index, audio_index, subtitle_index,
                               superimpose_index, initial_position_ms,
                               initial_catchup_target_us, &video_enabled, &audio_enabled,
                               &subtitle_enabled, &superimpose_enabled)) return;
        prepared_.store(true);
        callbacks_->onSubtitleInfo(subtitle_stream_count > 0, subtitle_stream_count, font_ok);
        callbacks_->onPrepared(duration_ms_.load(),
                               BuildTrackJson(format.get(), audio_program_id,
                                              video_index, audio_index,
                                              audio_dual_mono_mode, subtitle_index,
                                              superimpose_index, dual_mono_streams),
                               seekable_.load());
        if (want_playing_.load()) {
            ApplyPlay();
        } else {
            callbacks_->onState(kStatePrepared);
        }

        AVPacket* packet = av_packet_alloc();
        if (packet == nullptr) {
            Fail(5, "Unable to allocate demux packet");
            return;
        }
        bool eof_reported = false;
        uint64_t handled_subtitle_canvas_generation =
                initial_subtitle_canvas_generation;
        uint64_t handled_caption_style_generation =
                initial_caption_style_generation;
        int64_t audio_discard_before_us = initial_catchup_target_us;
        while (!stop_requested_.load(std::memory_order_acquire)) {
            const uint64_t requested_canvas_generation =
                    subtitle_canvas_generation_.load(std::memory_order_acquire);
            const uint64_t requested_style_generation =
                    caption_style_generation_.load(std::memory_order_acquire);
            const bool canvas_changed =
                    requested_canvas_generation != handled_subtitle_canvas_generation;
            const bool style_changed =
                    requested_style_generation != handled_caption_style_generation;
            if (canvas_changed || style_changed) {
                handled_subtitle_canvas_generation = requested_canvas_generation;
                handled_caption_style_generation = requested_style_generation;
                const int subtitle_serial = serial_.load(std::memory_order_acquire);
                if (subtitle_index >= 0) {
                    subtitle_enabled = RebindSubtitlePath(
                            format.get(), video_index, subtitle_index,
                            SubtitleSource::kCaption, subtitle_serial,
                            subtitle_decoder_, *subtitle_packets_, *subtitle_events_);
                }
                if (canvas_changed && superimpose_index >= 0) {
                    superimpose_enabled = RebindSubtitlePath(
                            format.get(), video_index, superimpose_index,
                            SubtitleSource::kSuperimpose, subtitle_serial,
                            superimpose_decoder_, *superimpose_packets_,
                            *superimpose_events_);
                }
            }

            bool tracks_changed = false;
            const int detected_dual_mono =
                    detected_dual_mono_stream_.exchange(-1, std::memory_order_acq_rel);
            if (detected_dual_mono >= 0 &&
                dual_mono_streams.insert(detected_dual_mono).second) {
                tracks_changed = true;
                if (detected_dual_mono == audio_index && audio_dual_mono_mode == -1 &&
                    SwitchAudioTrack(format.get(), audio_program_id, audio_index, 1,
                                     dual_mono_streams, true, audio_discard_before_us)) {
                    audio_dual_mono_mode = 1;
                }
            }

            const uint64_t requested_generation =
                    audio_switch_generation_.load(std::memory_order_acquire);
            if (requested_generation != handled_audio_switch_generation) {
                handled_audio_switch_generation = requested_generation;
                const int requested_stream =
                        requested_audio_stream_.load(std::memory_order_relaxed);
                const int requested_mode =
                        requested_dual_mono_mode_.load(std::memory_order_relaxed);
                if (SwitchAudioTrack(format.get(), audio_program_id, requested_stream,
                                     requested_mode, dual_mono_streams, true,
                                     audio_discard_before_us)) {
                    audio_index = requested_stream;
                    audio_dual_mono_mode = dual_mono_streams.count(audio_index) != 0 &&
                                                   requested_mode == -1
                            ? 1 : requested_mode;
                    audio_enabled = true;
                    tracks_changed = true;
                }
            }
            if (tracks_changed) {
                callbacks_->onTracksChanged(BuildTrackJson(
                        format.get(), audio_program_id, video_index, audio_index,
                        audio_dual_mono_mode, subtitle_index, superimpose_index,
                        dual_mono_streams));
            }

            const int64_t requested_seek = seek_ms_.exchange(-1, std::memory_order_acq_rel);
            if (requested_seek >= 0) {
                const int reserved_serial = seek_serial_.exchange(-1, std::memory_order_acq_rel);
                if (PerformSeek(format.get(), requested_seek, reserved_serial)) {
                    audio_discard_before_us = av_rescale_q(
                            requested_seek, AVRational{1, 1000}, AV_TIME_BASE_Q);
                }
                eof_reported = false;
                continue;
            }

            result = av_read_frame(format.get(), packet);
            if (result == AVERROR_EOF) {
                if (!eof_reported) {
                    const int serial = serial_.load();
                    if (video_enabled) PutEof(*video_packets_, serial);
                    if (audio_enabled) {
                        PutEof(*audio_packets_,
                               audio_serial_.load(std::memory_order_acquire));
                    }
                    if (subtitle_enabled) PutEof(*subtitle_packets_, serial);
                    if (superimpose_enabled) PutEof(*superimpose_packets_, serial);
                    WaitForDrainOrSeek(video_enabled, audio_enabled, subtitle_enabled,
                                       superimpose_enabled);
                    if (seek_ms_.load() < 0 && !stop_requested_.load()) {
                        callbacks_->onEndOfStream();
                        callbacks_->onState(kStateEnded);
                        eof_reported = true;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            if (result < 0) {
                if (result == AVERROR(EAGAIN)) continue;
                callbacks_->onError(5, "Demux error: " + AvError(result));
                break;
            }
            eof_reported = false;

            int observed_program_id = -1;
            const int observed_default_audio = FindProgramAudioStream(
                    format.get(), video_index, &observed_program_id);
            const ProgramSnapshot observed_snapshot =
                    CaptureProgramSnapshot(format.get(), observed_program_id);
            if (observed_snapshot != program_snapshot) {
                program_snapshot = observed_snapshot;
                audio_program_id = observed_program_id;
                const std::vector<int> current_audio =
                        ProgramAudioStreams(format.get(), audio_program_id);
                if (!current_audio.empty() &&
                    std::find(current_audio.begin(), current_audio.end(), audio_index) ==
                            current_audio.end() &&
                    SwitchAudioTrack(format.get(), audio_program_id, observed_default_audio, -1,
                                     dual_mono_streams, true, audio_discard_before_us)) {
                    audio_index = observed_default_audio;
                    audio_dual_mono_mode = -1;
                    audio_enabled = true;
                }
                callbacks_->onTracksChanged(BuildTrackJson(
                        format.get(), audio_program_id, video_index, audio_index,
                        audio_dual_mono_mode, subtitle_index, superimpose_index,
                        dual_mono_streams));
            }

            const int observed_video_program_id =
                    FindProgramIdForStream(format.get(), video_index);
            const int observed_subtitle_program_id = observed_video_program_id >= 0
                    ? observed_video_program_id : audio_program_id;
            const ProgramSnapshot observed_subtitle_snapshot =
                    CaptureProgramSnapshot(format.get(), observed_subtitle_program_id);
            if (observed_subtitle_program_id != subtitle_program_id ||
                observed_subtitle_snapshot != subtitle_program_snapshot) {
                subtitle_program_id = observed_subtitle_program_id;
                subtitle_program_snapshot = observed_subtitle_snapshot;
                const AribSubtitleStreams current_subtitle_streams =
                        FindAribSubtitleStreams(format.get(), subtitle_program_id);
                const bool selected_streams_changed =
                        current_subtitle_streams.caption_index != subtitle_index ||
                        current_subtitle_streams.superimpose_index != superimpose_index;
                const bool caption_count_changed =
                        current_subtitle_streams.caption_count != subtitle_stream_count;
                const int subtitle_serial = serial_.load(std::memory_order_acquire);
                if (current_subtitle_streams.caption_index != subtitle_index ||
                    (current_subtitle_streams.caption_index >= 0 && !subtitle_enabled)) {
                    subtitle_enabled = RebindSubtitlePath(
                            format.get(), video_index,
                            current_subtitle_streams.caption_index,
                            SubtitleSource::kCaption, subtitle_serial,
                            subtitle_decoder_, *subtitle_packets_, *subtitle_events_);
                    subtitle_index = current_subtitle_streams.caption_index;
                }
                if (current_subtitle_streams.superimpose_index != superimpose_index ||
                    (current_subtitle_streams.superimpose_index >= 0 &&
                     !superimpose_enabled)) {
                    superimpose_enabled = RebindSubtitlePath(
                            format.get(), video_index,
                            current_subtitle_streams.superimpose_index,
                            SubtitleSource::kSuperimpose, subtitle_serial,
                            superimpose_decoder_, *superimpose_packets_,
                            *superimpose_events_);
                    superimpose_index = current_subtitle_streams.superimpose_index;
                }
                if (caption_count_changed) {
                    subtitle_stream_count = current_subtitle_streams.caption_count;
                    callbacks_->onSubtitleInfo(subtitle_stream_count > 0,
                                               subtitle_stream_count, font_ok);
                }
                if (selected_streams_changed || caption_count_changed) {
                    callbacks_->onTracksChanged(BuildTrackJson(
                            format.get(), audio_program_id, video_index, audio_index,
                            audio_dual_mono_mode, subtitle_index, superimpose_index,
                            dual_mono_streams));
                }
            }

            const int serial = serial_.load(std::memory_order_acquire);
            if (video_enabled && packet->stream_index == video_index) {
                PutPacket(*video_packets_, *packet, serial);
            } else if (audio_enabled && packet->stream_index == audio_index) {
                bool discard_packet = false;
                const AVCodecParameters* audio_parameters =
                        format->streams[audio_index]->codecpar;
                if (audio_discard_before_us != AV_NOPTS_VALUE &&
                    audio_parameters != nullptr &&
                    audio_parameters->codec_id == AV_CODEC_ID_AAC) {
                    const int64_t packet_timestamp = packet->pts != AV_NOPTS_VALUE
                            ? packet->pts : packet->dts;
                    if (packet_timestamp != AV_NOPTS_VALUE) {
                        const int64_t packet_us = av_rescale_q(
                                packet_timestamp, format->streams[audio_index]->time_base,
                                AV_TIME_BASE_Q);
                        if (packet_us < audio_discard_before_us) {
                            discard_packet = true;
                        } else {
                            audio_discard_before_us = AV_NOPTS_VALUE;
                        }
                    }
                }
                if (!discard_packet) {
                    PutPacket(*audio_packets_, *packet,
                              audio_serial_.load(std::memory_order_acquire));
                }
            } else if (subtitle_enabled && packet->stream_index == subtitle_index) {
                PutSubtitlePacket(*subtitle_packets_, *packet, serial);
            } else if (superimpose_enabled &&
                       packet->stream_index == superimpose_index) {
                PutSubtitlePacket(*superimpose_packets_, *packet, serial);
            }
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
    }

    bool InitializeStreams(AVFormatContext* format, int video_index, int audio_index,
                           int subtitle_index, int superimpose_index,
                           int64_t initial_position_ms, int64_t initial_catchup_target_us,
                           bool* video_enabled, bool* audio_enabled,
                           bool* subtitle_enabled, bool* superimpose_enabled) {
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (video_enabled) *video_enabled = false;
        if (audio_enabled) *audio_enabled = false;
        if (subtitle_enabled) *subtitle_enabled = false;
        if (superimpose_enabled) *superimpose_enabled = false;
        clock_->Reset(initial_position_ms / 1000.0);
        if (video_index >= 0) {
            AVStream* stream = format->streams[video_index];
            std::string error;
            bool decoder_hardware = false;
            AVCodecContext* decoder = OpenDecoder(stream, true, &decoder_hardware,
                                                  nullptr, &error);
            if (decoder == nullptr) {
                callbacks_->onError(4, error);
            } else {
                AVRational frame_rate = av_guess_frame_rate(format, stream, nullptr);
                if (frame_rate.num <= 0 || frame_rate.den <= 0) frame_rate = stream->avg_frame_rate;
                if (frame_rate.num <= 0 || frame_rate.den <= 0) frame_rate = AVRational{30000, 1001};
                AVRational sar = stream->sample_aspect_ratio.num > 0 &&
                                 stream->sample_aspect_ratio.den > 0
                        ? stream->sample_aspect_ratio : decoder->sample_aspect_ratio;
                if (sar.num <= 0 || sar.den <= 0) sar = AVRational{1, 1};
                video_decoder_ = std::make_unique<VideoDecoder>(
                        decoder, decoder_hardware, stream->codecpar, stream->time_base,
                        frame_rate, sar, *video_packets_, *video_frames_,
                        [this](const std::string& name, bool hardware) {
                            callbacks_->onDecoderInfo(name, hardware);
                        },
                        [this](VideoMode mode, const std::string& backend) {
                            callbacks_->onFilterInfo(static_cast<int>(mode), backend);
                        },
                        [this](double position_seconds) {
                            if (!seekable_.load(std::memory_order_acquire) ||
                                stop_requested_.load(std::memory_order_acquire)) return false;
                            const double position = position_seconds > 0.0
                                    ? position_seconds : std::max(0.0, clock_->PositionSeconds());
                            seek_serial_.store(-1, std::memory_order_release);
                            seek_ms_.store(static_cast<int64_t>(position * 1000.0),
                                           std::memory_order_release);
                            return true;
                        },
                        [this](int width, int height, AVRational frame_sar) {
                            callbacks_->onVideoSize(width, height,
                                    frame_sar.num > 0 ? frame_sar.num : 1,
                                    frame_sar.den > 0 ? frame_sar.den : 1);
                        });
                video_decoder_->SetVideoMode(static_cast<VideoMode>(
                        video_mode_.load(std::memory_order_acquire)));
                video_decoder_->Prepare();
                if (initial_catchup_target_us != AV_NOPTS_VALUE) {
                    video_decoder_->FlushForSeek(
                            serial_.load(std::memory_order_acquire),
                            initial_catchup_target_us);
                }
                renderer_ = std::make_unique<GlRenderer>(
                        video_frames_.get(), subtitle_events_.get(),
                        superimpose_events_.get(),
                        [this](double* position) {
                            return clock_->TryPositionSeconds(position);
                        },
                        [this](double pts) { AnchorVideoClock(pts); },
                        [this](int width, int height) {
                            RequestSubtitleViewport(width, height);
                        });
                renderer_->SetSerial(serial_.load());
                renderer_->SetSubtitlesEnabled(
                        subtitles_enabled_.load(std::memory_order_acquire));
                if (pending_window_ != nullptr) {
                    renderer_->SetWindow(pending_window_);
                    pending_window_ = nullptr;
                }
                renderer_->Start();
                callbacks_->onVideoSize(decoder->width, decoder->height, sar.num, sar.den);
                video_decoder_->Start();
                if (video_enabled) *video_enabled = true;
            }
        }

        if (subtitle_index >= 0 && video_decoder_ && renderer_) {
            AVStream* stream = format->streams[subtitle_index];
            const AVCodecParameters* video_parameters = format->streams[video_index]->codecpar;
            const auto [canvas_width, canvas_height] =
                    SubtitleCanvasSize(video_parameters);
            std::string error;
            AVCodecContext* decoder = OpenSubtitleDecoder(
                    stream, canvas_width, canvas_height, font_path_,
                    SubtitleSource::kCaption,
                    caption_ignore_background_.load(std::memory_order_acquire),
                    caption_force_outline_.load(std::memory_order_acquire), &error);
            if (decoder == nullptr) {
                __android_log_print(ANDROID_LOG_ERROR, kTag,
                                    "ARIB caption decoder disabled: %s", error.c_str());
            } else {
                subtitle_decoder_ = std::make_unique<SubtitleDecoder>(
                        decoder, *subtitle_packets_, *subtitle_events_,
                        SubtitleSource::kCaption, decoder->width, decoder->height);
                subtitle_decoder_->Start();
                if (subtitle_enabled) *subtitle_enabled = true;
            }
        }

        if (superimpose_index >= 0 && video_decoder_ && renderer_) {
            AVStream* stream = format->streams[superimpose_index];
            const AVCodecParameters* video_parameters = format->streams[video_index]->codecpar;
            const auto [canvas_width, canvas_height] =
                    SubtitleCanvasSize(video_parameters);
            std::string error;
            AVCodecContext* decoder = OpenSubtitleDecoder(
                    stream, canvas_width, canvas_height, font_path_,
                    SubtitleSource::kSuperimpose, false, false, &error);
            if (decoder == nullptr) {
                __android_log_print(ANDROID_LOG_ERROR, kTag,
                                    "ARIB superimpose decoder disabled: %s", error.c_str());
            } else {
                superimpose_decoder_ = std::make_unique<SubtitleDecoder>(
                        decoder, *superimpose_packets_, *superimpose_events_,
                        SubtitleSource::kSuperimpose, decoder->width, decoder->height);
                superimpose_decoder_->Start();
                if (superimpose_enabled) *superimpose_enabled = true;
            }
        }

        if (audio_index >= 0) {
            std::string error;
            if (!OpenAudioPathLocked(
                        format, audio_index, -1, &error,
                        audio_serial_.load(std::memory_order_acquire),
                        initial_catchup_target_us)) {
                callbacks_->onError(4, error);
            } else if (audio_enabled) {
                *audio_enabled = true;
            }
        }

        if (!video_decoder_ && !audio_decoder_) {
            Fail(4, "Selected streams could not be decoded");
            return false;
        }
        return true;
    }

    void AnchorVideoClock(double pts) {
        if (has_audio_master_.load(std::memory_order_acquire)) {
            clock_->SetVideoFallbackAnchor(std::isfinite(pts) ? pts : 0.0);
            return;
        }
        bool expected = false;
        if (video_clock_anchored_.compare_exchange_strong(expected, true)) {
            clock_->Reset(std::isfinite(pts) ? pts : 0.0);
            if (want_playing_.load()) clock_->Resume();
        }
    }

    void WaitForDrainOrSeek(bool has_video, bool has_audio, bool has_subtitle,
                            bool has_superimpose) {
        while (!stop_requested_.load() && seek_ms_.load() < 0) {
            bool drained = (!has_video || video_packets_->Size() == 0) &&
                           (!has_audio || audio_packets_->Size() == 0) &&
                           (!has_subtitle || subtitle_packets_->Size() == 0) &&
                           (!has_superimpose || superimpose_packets_->Size() == 0);
            if (has_video && video_frames_) drained = drained && video_frames_->Size() == 0;
            if (has_video && video_decoder_) drained = drained && video_decoder_->eof_drained();
            if (has_subtitle && subtitle_decoder_) {
                drained = drained && subtitle_decoder_->eof_drained();
            }
            if (has_superimpose && superimpose_decoder_) {
                drained = drained && superimpose_decoder_->eof_drained();
            }
            if (has_video && renderer_) {
                drained = drained && renderer_->IsSerialComplete(
                        serial_.load(std::memory_order_acquire));
            }
            {
                std::lock_guard<std::mutex> lock(session_mutex_);
                if (has_audio && pcm_ring_) drained = drained && pcm_ring_->available_frames() == 0;
            }
            if (drained) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    void Fail(int code, const std::string& message) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "%s", message.c_str());
        callbacks_->onError(code, message);
        callbacks_->onState(kStateError);
    }

    std::unique_ptr<player::Callbacks> callbacks_;
    std::unique_ptr<PlaybackClock> clock_;
    mutable std::mutex command_mutex_;
    std::condition_variable command_cv_;
    std::deque<Command> commands_;
    std::thread control_thread_;
    std::atomic<bool> shutting_down_{false};

    mutable std::mutex session_mutex_;
    std::thread demux_thread_;
    std::atomic<bool> stop_requested_{true};
    std::atomic<bool> prepared_{false};
    std::atomic<bool> seekable_{false};
    std::atomic<bool> want_playing_{false};
    std::atomic<int64_t> seek_ms_{-1};
    std::atomic<int> seek_serial_{-1};
    std::atomic<int64_t> start_offset_us_{0};
    std::atomic<int64_t> duration_ms_{0};
    std::atomic<int> serial_{0};
    std::atomic<int> audio_serial_{0};
    std::atomic<int> requested_audio_stream_{-1};
    std::atomic<int> requested_dual_mono_mode_{-1};
    std::atomic<uint64_t> audio_switch_generation_{0};
    std::atomic<int> detected_dual_mono_stream_{-1};
    std::atomic<int> active_audio_stream_{-1};
    std::atomic<int> active_dual_mono_mode_{-1};
    std::atomic<int> video_mode_{0};
    std::atomic<bool> subtitles_enabled_{true};
    std::atomic<bool> caption_ignore_background_{false};
    std::atomic<bool> caption_force_outline_{false};
    std::atomic<int> subtitle_canvas_width_{0};
    std::atomic<int> subtitle_canvas_height_{0};
    std::atomic<uint64_t> subtitle_canvas_generation_{0};
    std::atomic<uint64_t> caption_style_generation_{0};
    std::atomic<bool> video_clock_anchored_{false};
    std::atomic<bool> has_audio_master_{false};
    std::string font_path_;
    std::string display_name_;

    std::unique_ptr<PacketQueue> video_packets_;
    std::unique_ptr<PacketQueue> audio_packets_;
    std::unique_ptr<PacketQueue> subtitle_packets_;
    std::unique_ptr<PacketQueue> superimpose_packets_;
    std::unique_ptr<FrameQueue> video_frames_;
    std::unique_ptr<SubtitleQueue> subtitle_events_;
    std::unique_ptr<SubtitleQueue> superimpose_events_;
    std::unique_ptr<PcmRing> pcm_ring_;
    std::unique_ptr<VideoDecoder> video_decoder_;
    std::unique_ptr<AudioDecoder> audio_decoder_;
    std::unique_ptr<SubtitleDecoder> subtitle_decoder_;
    std::unique_ptr<SubtitleDecoder> superimpose_decoder_;
    std::unique_ptr<AudioSink> audio_sink_;
    std::unique_ptr<GlRenderer> renderer_;
    ANativeWindow* pending_window_ = nullptr;
};

PlayerCore::PlayerCore(JavaVM* vm, JNIEnv* env, jobject callbackObject)
        : impl_(std::make_unique<Impl>(vm, env, callbackObject)) {}
PlayerCore::~PlayerCore() = default;
bool PlayerCore::open(int ownedFd, std::string fontPath, std::string displayName,
                      int64_t startPositionMs) {
    return impl_->Open(ownedFd, std::move(fontPath), std::move(displayName), startPositionMs);
}
void PlayerCore::setSurface(ANativeWindow* window) { impl_->SetSurface(window); }
void PlayerCore::play() { impl_->Play(); }
void PlayerCore::pause() { impl_->Pause(); }
void PlayerCore::seekTo(int64_t positionMs) { impl_->SeekUi(positionMs); }
void PlayerCore::setVideoMode(int mode) { impl_->SetVideoMode(mode); }
void PlayerCore::selectAudioTrack(int streamIndex, int dualMonoMode) {
    impl_->SelectAudioTrack(streamIndex, dualMonoMode);
}
void PlayerCore::setSubtitlesEnabled(bool enabled) { impl_->SetSubtitles(enabled); }
void PlayerCore::setCaptionStyle(bool ignoreBackground, bool forceOutlineText) {
    impl_->SetCaptionStyle(ignoreBackground, forceOutlineText);
}
int64_t PlayerCore::durationMs() const { return impl_->Duration(); }
std::string PlayerCore::getStats() const { return impl_->Stats(); }
void PlayerCore::close() { impl_->Close(); }

} // namespace aribplayer
