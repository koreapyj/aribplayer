#pragma once

#include <jni.h>
#include <android/native_window.h>

#include <cstdint>
#include <memory>
#include <string>

namespace aribplayer {

class PlayerCore {
public:
    PlayerCore(JavaVM* vm, JNIEnv* env, jobject callbackObject);
    ~PlayerCore();

    PlayerCore(const PlayerCore&) = delete;
    PlayerCore& operator=(const PlayerCore&) = delete;

    // Takes ownership of ownedFd. The JNI layer duplicates the Java descriptor
    // before calling this method so the caller may close its ParcelFileDescriptor.
    bool open(int ownedFd, std::string fontPath, std::string displayName,
              int64_t startPositionMs);
    void setSurface(ANativeWindow* window); // Takes ownership of one window reference.
    void play();
    void pause();
    void seekTo(int64_t positionMs);
    void setVideoMode(int mode);
    void selectAudioTrack(int streamIndex, int dualMonoMode);
    void setSubtitlesEnabled(bool enabled);
    void setCaptionStyle(bool ignoreBackground, bool forceOutlineText);
    int64_t durationMs() const;
    std::string getStats() const;
    void close();
    // Waits only for the bounded shutdown budget. False means an active native
    // thread was abandoned, so the caller must retain this PlayerCore until process exit.
    bool release(int64_t timeoutMs);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aribplayer
