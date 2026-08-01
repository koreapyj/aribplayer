#include "Callbacks.h"

namespace aribplayer {
namespace player {
namespace {

jmethodID FindMethod(JNIEnv* env,
                     jclass callbackClass,
                     const char* name,
                     const char* signature) {
    if (env == nullptr || callbackClass == nullptr) {
        return nullptr;
    }

    jmethodID method = env->GetMethodID(callbackClass, name, signature);
    if (method == nullptr && env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    return method;
}

}  // namespace

Callbacks::Callbacks(JavaVM* vm, JNIEnv* env, jobject callback)
        : vm_(vm) {
    if (vm_ == nullptr && env != nullptr) {
        env->GetJavaVM(&vm_);
    }
    Initialize(env, callback);
}

Callbacks::Callbacks(JNIEnv* env, jobject callback)
        : Callbacks(nullptr, env, callback) {}

Callbacks::~Callbacks() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (callback_ == nullptr) {
        return;
    }

    bool attached = false;
    JNIEnv* env = GetEnv(&attached);
    if (env != nullptr) {
        env->DeleteGlobalRef(callback_);
        ClearException(env);
        ReleaseEnv(attached);
    }

    // If the VM is already unavailable, there is no safe JNI operation left to
    // perform. Clear the handles so destruction remains idempotent.
    callback_ = nullptr;
    onPrepared_ = nullptr;
    onTracksChanged_ = nullptr;
    onVideoSize_ = nullptr;
    onState_ = nullptr;
    onPositionMs_ = nullptr;
    onError_ = nullptr;
    onDecoderInfo_ = nullptr;
    onFilterInfo_ = nullptr;
    onSubtitleInfo_ = nullptr;
    onEndOfStream_ = nullptr;
}

void Callbacks::Initialize(JNIEnv* env, jobject callback) {
    if (env == nullptr || callback == nullptr || vm_ == nullptr) {
        return;
    }

    callback_ = env->NewGlobalRef(callback);
    if (callback_ == nullptr) {
        ClearException(env);
        return;
    }

    jclass callbackClass = env->GetObjectClass(callback);
    if (callbackClass == nullptr) {
        ClearException(env);
        env->DeleteGlobalRef(callback_);
        callback_ = nullptr;
        return;
    }

    onPrepared_ = FindMethod(
            env, callbackClass, "onPrepared", "(JLjava/lang/String;Z)V");
    onTracksChanged_ = FindMethod(
            env, callbackClass, "onTracksChanged", "(Ljava/lang/String;)V");
    onVideoSize_ = FindMethod(
            env, callbackClass, "onVideoSize", "(IIII)V");
    onState_ = FindMethod(env, callbackClass, "onState", "(I)V");
    onPositionMs_ = FindMethod(
            env, callbackClass, "onPositionMs", "(J)V");
    onError_ = FindMethod(
            env, callbackClass, "onError", "(ILjava/lang/String;)V");
    onDecoderInfo_ = FindMethod(
            env, callbackClass, "onDecoderInfo", "(Ljava/lang/String;Z)V");
    onFilterInfo_ = FindMethod(
            env, callbackClass, "onFilterInfo", "(ILjava/lang/String;)V");
    onSubtitleInfo_ = FindMethod(
            env, callbackClass, "onSubtitleInfo", "(ZIZ)V");
    onEndOfStream_ = FindMethod(
            env, callbackClass, "onEndOfStream", "()V");

    env->DeleteLocalRef(callbackClass);

    if (!HasMethods()) {
        env->DeleteGlobalRef(callback_);
        callback_ = nullptr;
        onPrepared_ = nullptr;
        onTracksChanged_ = nullptr;
        onVideoSize_ = nullptr;
        onState_ = nullptr;
        onPositionMs_ = nullptr;
        onError_ = nullptr;
        onDecoderInfo_ = nullptr;
        onFilterInfo_ = nullptr;
        onEndOfStream_ = nullptr;
    }
}

JNIEnv* Callbacks::GetEnv(bool* attached) const {
    if (attached != nullptr) {
        *attached = false;
    }
    if (vm_ == nullptr) {
        return nullptr;
    }

    JNIEnv* env = nullptr;
    const jint getEnvResult =
            vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (getEnvResult == JNI_OK) {
        return env;
    }
    if (getEnvResult != JNI_EDETACHED) {
        return nullptr;
    }

    if (vm_->AttachCurrentThread(&env, nullptr) != JNI_OK) {
        return nullptr;
    }
    if (attached != nullptr) {
        *attached = true;
    }
    return env;
}

void Callbacks::ReleaseEnv(bool attached) const {
    if (attached && vm_ != nullptr) {
        vm_->DetachCurrentThread();
    }
}

bool Callbacks::HasMethods() const {
    return callback_ != nullptr &&
           onPrepared_ != nullptr &&
           onTracksChanged_ != nullptr &&
           onVideoSize_ != nullptr &&
           onState_ != nullptr &&
           onPositionMs_ != nullptr &&
           onError_ != nullptr &&
           onDecoderInfo_ != nullptr &&
           onFilterInfo_ != nullptr &&
           onSubtitleInfo_ != nullptr &&
           onEndOfStream_ != nullptr;
}

bool Callbacks::IsValid() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return vm_ != nullptr && HasMethods();
}

void Callbacks::ClearException(JNIEnv* env) {
    if (env != nullptr && env->ExceptionCheck()) {
        env->ExceptionClear();
    }
}

void Callbacks::onPrepared(std::int64_t durationMs,
                           const std::string& tracksJson,
                           bool seekable) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!HasMethods()) {
        return;
    }

    bool attached = false;
    JNIEnv* env = GetEnv(&attached);
    if (env == nullptr) {
        return;
    }

    jstring tracks = env->NewStringUTF(tracksJson.c_str());
    if (tracks == nullptr) {
        ClearException(env);
        ReleaseEnv(attached);
        return;
    }

    env->CallVoidMethod(callback_, onPrepared_, static_cast<jlong>(durationMs),
                        tracks, seekable ? JNI_TRUE : JNI_FALSE);
    env->DeleteLocalRef(tracks);
    ClearException(env);
    ReleaseEnv(attached);
}

void Callbacks::onTracksChanged(const std::string& tracksJson) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!HasMethods()) {
        return;
    }

    bool attached = false;
    JNIEnv* env = GetEnv(&attached);
    if (env == nullptr) {
        return;
    }

    jstring tracks = env->NewStringUTF(tracksJson.c_str());
    if (tracks == nullptr) {
        ClearException(env);
        ReleaseEnv(attached);
        return;
    }

    env->CallVoidMethod(callback_, onTracksChanged_, tracks);
    env->DeleteLocalRef(tracks);
    ClearException(env);
    ReleaseEnv(attached);
}

void Callbacks::onVideoSize(int width, int height, int sarNum, int sarDen) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!HasMethods()) {
        return;
    }

    bool attached = false;
    JNIEnv* env = GetEnv(&attached);
    if (env == nullptr) {
        return;
    }

    env->CallVoidMethod(callback_, onVideoSize_, static_cast<jint>(width),
                        static_cast<jint>(height), static_cast<jint>(sarNum),
                        static_cast<jint>(sarDen));
    ClearException(env);
    ReleaseEnv(attached);
}

void Callbacks::onState(int state) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!HasMethods()) {
        return;
    }

    bool attached = false;
    JNIEnv* env = GetEnv(&attached);
    if (env == nullptr) {
        return;
    }

    env->CallVoidMethod(callback_, onState_, static_cast<jint>(state));
    ClearException(env);
    ReleaseEnv(attached);
}

void Callbacks::onPositionMs(std::int64_t positionMs) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!HasMethods()) {
        return;
    }

    bool attached = false;
    JNIEnv* env = GetEnv(&attached);
    if (env == nullptr) {
        return;
    }

    env->CallVoidMethod(callback_, onPositionMs_,
                        static_cast<jlong>(positionMs));
    ClearException(env);
    ReleaseEnv(attached);
}

void Callbacks::onError(int code, const std::string& message) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!HasMethods()) {
        return;
    }

    bool attached = false;
    JNIEnv* env = GetEnv(&attached);
    if (env == nullptr) {
        return;
    }

    jstring errorMessage = env->NewStringUTF(message.c_str());
    if (errorMessage == nullptr) {
        ClearException(env);
        ReleaseEnv(attached);
        return;
    }

    env->CallVoidMethod(callback_, onError_, static_cast<jint>(code),
                        errorMessage);
    env->DeleteLocalRef(errorMessage);
    ClearException(env);
    ReleaseEnv(attached);
}

void Callbacks::onDecoderInfo(const std::string& message,
                              bool hardwareAccelerated) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!HasMethods()) {
        return;
    }

    bool attached = false;
    JNIEnv* env = GetEnv(&attached);
    if (env == nullptr) {
        return;
    }

    jstring decoderMessage = env->NewStringUTF(message.c_str());
    if (decoderMessage == nullptr) {
        ClearException(env);
        ReleaseEnv(attached);
        return;
    }

    env->CallVoidMethod(callback_, onDecoderInfo_, decoderMessage,
                        hardwareAccelerated ? JNI_TRUE : JNI_FALSE);
    env->DeleteLocalRef(decoderMessage);
    ClearException(env);
    ReleaseEnv(attached);
}

void Callbacks::onFilterInfo(int filterType, const std::string& message) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!HasMethods()) {
        return;
    }

    bool attached = false;
    JNIEnv* env = GetEnv(&attached);
    if (env == nullptr) {
        return;
    }

    jstring filterMessage = env->NewStringUTF(message.c_str());
    if (filterMessage == nullptr) {
        ClearException(env);
        ReleaseEnv(attached);
        return;
    }

    env->CallVoidMethod(callback_, onFilterInfo_, static_cast<jint>(filterType),
                        filterMessage);
    env->DeleteLocalRef(filterMessage);
    ClearException(env);
    ReleaseEnv(attached);
}

void Callbacks::onSubtitleInfo(bool hasSubtitles, int streamCount, bool fontOk) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!HasMethods()) {
        return;
    }

    bool attached = false;
    JNIEnv* env = GetEnv(&attached);
    if (env == nullptr) {
        return;
    }

    env->CallVoidMethod(callback_, onSubtitleInfo_,
                        hasSubtitles ? JNI_TRUE : JNI_FALSE,
                        static_cast<jint>(streamCount),
                        fontOk ? JNI_TRUE : JNI_FALSE);
    ClearException(env);
    ReleaseEnv(attached);
}

void Callbacks::onEndOfStream() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!HasMethods()) {
        return;
    }

    bool attached = false;
    JNIEnv* env = GetEnv(&attached);
    if (env == nullptr) {
        return;
    }

    env->CallVoidMethod(callback_, onEndOfStream_);
    ClearException(env);
    ReleaseEnv(attached);
}

}  // namespace player
}  // namespace aribplayer
