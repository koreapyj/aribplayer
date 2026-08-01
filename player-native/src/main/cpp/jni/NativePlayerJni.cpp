#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <new>
#include <string>

#ifdef HAVE_FFMPEG
#include "player/PlayerCore.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/jni.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}
#endif

namespace {
constexpr char kLogTag[] = "aribplayer";
JavaVM* gVm = nullptr;

#ifdef HAVE_FFMPEG
using NativeHandle = aribplayer::PlayerCore;
#else
struct NativeHandle {
    NativeHandle(JNIEnv* env, jobject callback) : callback(env->NewGlobalRef(callback)) {}
    ~NativeHandle() {
        if (callback == nullptr || gVm == nullptr) return;
        JNIEnv* env = nullptr;
        bool attached = false;
        if (gVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
            if (gVm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
            attached = true;
        }
        env->DeleteGlobalRef(callback);
        if (attached) gVm->DetachCurrentThread();
    }
    jobject callback = nullptr;
    int64_t durationMs = 0;
};
#endif

NativeHandle* fromHandle(jlong handle) {
    return reinterpret_cast<NativeHandle*>(static_cast<intptr_t>(handle));
}

jstring newStringUtf(JNIEnv* env, const std::string& value) {
    if (env == nullptr) return nullptr;

    jstring string = env->NewStringUTF(value.c_str());
    if (string == nullptr && env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    return string;
}

#ifdef HAVE_FFMPEG
std::string toUtf8(JNIEnv* env, jstring value) {
    if (value == nullptr) return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr) {
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        return {};
    }
    std::string result(chars);
    env->ReleaseStringUTFChars(value, chars);
    return result;
}
#endif
} // namespace

extern "C" JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM* vm, void*) {
#ifdef HAVE_FFMPEG
    const int setVmResult = av_jni_set_java_vm(vm, nullptr);
    if (setVmResult < 0) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "Failed to register JavaVM with FFmpeg: %d",
                            setVmResult);
        return JNI_ERR;
    }
#endif
    gVm = vm;
#ifdef HAVE_FFMPEG
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Loaded native player with FFmpeg");
#else
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Loaded native player stub (no ffmpeg)");
#endif
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT jlong JNICALL
Java_kr_dcmys_android_aribplayer_nativeplayer_NativePlayer_nativeCreate(
        JNIEnv* env, jobject, jobject callbackObject) {
    if (callbackObject == nullptr) return 0;
    try {
#ifdef HAVE_FFMPEG
        auto* player = new NativeHandle(gVm, env, callbackObject);
#else
        auto* player = new NativeHandle(env, callbackObject);
#endif
        return static_cast<jlong>(reinterpret_cast<intptr_t>(player));
    } catch (...) {
        return 0;
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_kr_dcmys_android_aribplayer_nativeplayer_NativePlayer_nativeOpen(
        JNIEnv* env, jobject, jlong handle, jint fd, jstring fontPath, jstring displayName,
        jlong startPositionMs) {
    NativeHandle* player = fromHandle(handle);
    if (player == nullptr || fd < 0) return JNI_FALSE;

    // This synchronous dup is the ownership boundary promised to Kotlin.
    const int ownedFd = dup(fd);
    if (ownedFd < 0) return JNI_FALSE;
#ifdef HAVE_FFMPEG
    const bool accepted = player->open(
            ownedFd, toUtf8(env, fontPath), toUtf8(env, displayName),
            std::max<int64_t>(0, static_cast<int64_t>(startPositionMs)));
    if (!accepted) close(ownedFd);
    return accepted ? JNI_TRUE : JNI_FALSE;
#else
    close(ownedFd);
    (void)env;
    (void)fontPath;
    (void)displayName;
    (void)startPositionMs;
    return JNI_FALSE;
#endif
}

extern "C" JNIEXPORT void JNICALL
Java_kr_dcmys_android_aribplayer_nativeplayer_NativePlayer_nativeSetSurface(
        JNIEnv* env, jobject, jlong handle, jobject surface) {
    NativeHandle* player = fromHandle(handle);
    if (player == nullptr) return;
#ifdef HAVE_FFMPEG
    ANativeWindow* window = surface == nullptr ? nullptr : ANativeWindow_fromSurface(env, surface);
    player->setSurface(window);
#else
    (void)env;
    (void)surface;
#endif
}

extern "C" JNIEXPORT void JNICALL
Java_kr_dcmys_android_aribplayer_nativeplayer_NativePlayer_nativePlay(
        JNIEnv*, jobject, jlong handle) {
#ifdef HAVE_FFMPEG
    if (auto* player = fromHandle(handle)) player->play();
#else
    (void)handle;
#endif
}

extern "C" JNIEXPORT void JNICALL
Java_kr_dcmys_android_aribplayer_nativeplayer_NativePlayer_nativePause(
        JNIEnv*, jobject, jlong handle) {
#ifdef HAVE_FFMPEG
    if (auto* player = fromHandle(handle)) player->pause();
#else
    (void)handle;
#endif
}

extern "C" JNIEXPORT void JNICALL
Java_kr_dcmys_android_aribplayer_nativeplayer_NativePlayer_nativeSeekTo(
        JNIEnv*, jobject, jlong handle, jlong positionMs) {
#ifdef HAVE_FFMPEG
    if (auto* player = fromHandle(handle)) player->seekTo(positionMs);
#else
    (void)handle;
    (void)positionMs;
#endif
}

extern "C" JNIEXPORT void JNICALL
Java_kr_dcmys_android_aribplayer_nativeplayer_NativePlayer_nativeSetVideoMode(
        JNIEnv*, jobject, jlong handle, jint mode) {
#ifdef HAVE_FFMPEG
    if (auto* player = fromHandle(handle)) player->setVideoMode(mode);
#else
    (void)handle;
    (void)mode;
#endif
}

extern "C" JNIEXPORT void JNICALL
Java_kr_dcmys_android_aribplayer_nativeplayer_NativePlayer_nativeSelectAudioTrack(
        JNIEnv*, jobject, jlong handle, jint streamIndex, jint dualMonoMode) {
#ifdef HAVE_FFMPEG
    if (auto* player = fromHandle(handle)) {
        player->selectAudioTrack(streamIndex, dualMonoMode);
    }
#else
    (void)handle;
    (void)streamIndex;
    (void)dualMonoMode;
#endif
}

extern "C" JNIEXPORT void JNICALL
Java_kr_dcmys_android_aribplayer_nativeplayer_NativePlayer_nativeSetSubtitlesEnabled(
        JNIEnv*, jobject, jlong handle, jboolean enabled) {
#ifdef HAVE_FFMPEG
    if (auto* player = fromHandle(handle)) player->setSubtitlesEnabled(enabled == JNI_TRUE);
#else
    (void)handle;
    (void)enabled;
#endif
}

extern "C" JNIEXPORT void JNICALL
Java_kr_dcmys_android_aribplayer_nativeplayer_NativePlayer_nativeSetCaptionStyle(
        JNIEnv*, jobject, jlong handle, jboolean ignoreBackground,
        jboolean forceOutlineText) {
#ifdef HAVE_FFMPEG
    if (auto* player = fromHandle(handle)) {
        player->setCaptionStyle(ignoreBackground == JNI_TRUE,
                                forceOutlineText == JNI_TRUE);
    }
#else
    (void)handle;
    (void)ignoreBackground;
    (void)forceOutlineText;
#endif
}

extern "C" JNIEXPORT jlong JNICALL
Java_kr_dcmys_android_aribplayer_nativeplayer_NativePlayer_nativeGetDurationMs(
        JNIEnv*, jobject, jlong handle) {
    NativeHandle* player = fromHandle(handle);
    if (player == nullptr) return 0;
#ifdef HAVE_FFMPEG
    return static_cast<jlong>(player->durationMs());
#else
    return static_cast<jlong>(player->durationMs);
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_kr_dcmys_android_aribplayer_nativeplayer_NativePlayer_nativeGetStats(
        JNIEnv* env, jobject, jlong handle) {
#ifdef HAVE_FFMPEG
    std::string stats = "{}";
    if (const auto* player = fromHandle(handle)) {
        try {
            stats = player->getStats();
        } catch (...) {
            stats = "{}";
        }
    }
    return newStringUtf(env, stats.empty() ? "{}" : stats);
#else
    (void)handle;
    return newStringUtf(env, "{}");
#endif
}

extern "C" JNIEXPORT void JNICALL
Java_kr_dcmys_android_aribplayer_nativeplayer_NativePlayer_nativeClose(
        JNIEnv*, jobject, jlong handle) {
#ifdef HAVE_FFMPEG
    if (auto* player = fromHandle(handle)) player->close();
#else
    (void)handle;
#endif
}

extern "C" JNIEXPORT void JNICALL
Java_kr_dcmys_android_aribplayer_nativeplayer_NativePlayer_nativeRelease(
        JNIEnv*, jobject, jlong handle) {
    NativeHandle* player = fromHandle(handle);
    if (player == nullptr) return;
#ifdef HAVE_FFMPEG
    player->close();
#endif
    delete player;
}

extern "C" JNIEXPORT jstring JNICALL
Java_kr_dcmys_android_aribplayer_nativeplayer_NativePlayer_nativeGetVersionInfo(
        JNIEnv* env, jobject) {
#ifdef HAVE_FFMPEG
    const std::string info = std::string("aribplayer-native 0.3; FFmpeg ") +
            av_version_info() + "; avformat=" + std::to_string(avformat_version()) +
            "; avcodec=" + std::to_string(avcodec_version());
    return newStringUtf(env, info);
#else
    return newStringUtf(env, "aribplayer-native 0.3 (no ffmpeg)");
#endif
}
