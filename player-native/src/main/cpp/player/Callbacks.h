#ifndef ARIBPLAYER_PLAYER_CALLBACKS_H
#define ARIBPLAYER_PLAYER_CALLBACKS_H

#include <jni.h>

#include <cstdint>
#include <mutex>
#include <string>

namespace aribplayer {
namespace player {

/**
 * Delivers player events to a Java/Kotlin callback object.
 *
 * The callback object is retained as a global JNI reference. Dispatch methods
 * may be called from any native thread; a detached thread is attached for the
 * duration of the call and detached again before returning.
 */
class Callbacks final {
public:
    Callbacks(JavaVM* vm, JNIEnv* env, jobject callback);
    Callbacks(JNIEnv* env, jobject callback);
    ~Callbacks();

    Callbacks(const Callbacks&) = delete;
    Callbacks& operator=(const Callbacks&) = delete;

    bool IsValid() const;

    void onPrepared(std::int64_t durationMs,
                    const std::string& tracksJson,
                    bool seekable);
    void onTracksChanged(const std::string& tracksJson);
    void onVideoSize(int width, int height, int sarNum, int sarDen);
    void onState(int state);
    void onPositionMs(std::int64_t positionMs);
    void onError(int code, const std::string& message);
    void onDecoderInfo(const std::string& message, bool hardwareAccelerated);
    void onFilterInfo(int filterType, const std::string& message);
    void onSubtitleInfo(bool hasSubtitles, int streamCount, bool fontOk);
    void onEndOfStream();

private:
    void Initialize(JNIEnv* env, jobject callback);
    JNIEnv* GetEnv(bool* attached) const;
    void ReleaseEnv(bool attached) const;
    bool HasMethods() const;
    static void ClearException(JNIEnv* env);

    mutable std::recursive_mutex mutex_;
    JavaVM* vm_ = nullptr;
    jobject callback_ = nullptr;

    jmethodID onPrepared_ = nullptr;
    jmethodID onTracksChanged_ = nullptr;
    jmethodID onVideoSize_ = nullptr;
    jmethodID onState_ = nullptr;
    jmethodID onPositionMs_ = nullptr;
    jmethodID onError_ = nullptr;
    jmethodID onDecoderInfo_ = nullptr;
    jmethodID onFilterInfo_ = nullptr;
    jmethodID onSubtitleInfo_ = nullptr;
    jmethodID onEndOfStream_ = nullptr;
};

}  // namespace player
}  // namespace aribplayer

#endif  // ARIBPLAYER_PLAYER_CALLBACKS_H
