cmake_minimum_required(VERSION 3.22.1)
project(aribplayer LANGUAGES CXX)

set(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../..")
set(ABI_SYSROOT "${PROJECT_ROOT}/third_party/prebuilt/sysroot-${ANDROID_ABI}")
set(LEGACY_SYSROOT "${PROJECT_ROOT}/third_party/prebuilt/sysroot")
if(EXISTS "${ABI_SYSROOT}")
    set(FFMPEG_SYSROOT "${ABI_SYSROOT}")
else()
    # Preserve compatibility with the pre-ABI arm64 local build output.
    set(FFMPEG_SYSROOT "${LEGACY_SYSROOT}")
endif()
set(FFMPEG_INCLUDE_DIR "${FFMPEG_SYSROOT}/include")
set(PREBUILT_LIB_DIR "${CMAKE_CURRENT_LIST_DIR}/../jniLibs/${ANDROID_ABI}")

set(FFMPEG_LIBS avformat avcodec avfilter avutil swresample swscale aribcaption)
set(HAVE_FFMPEG_PREBUILTS TRUE)
foreach(ffmpeg_lib IN LISTS FFMPEG_LIBS)
    if(NOT EXISTS "${PREBUILT_LIB_DIR}/lib${ffmpeg_lib}.so")
        set(HAVE_FFMPEG_PREBUILTS FALSE)
    endif()
endforeach()

# Keep the M1 stub build usable while the separately-built FFmpeg sysroot is
# unavailable. Once the headers and complete shared-library bundle exist,
# compile the complete playback engine.
if(EXISTS "${FFMPEG_INCLUDE_DIR}/libavcodec/avcodec.h" AND HAVE_FFMPEG_PREBUILTS)
    file(GLOB_RECURSE ARIBPLAYER_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/jni/*.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/common/*.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/demux/*.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/player/*.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/video/*.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/subtitle/*.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/audio/*.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/sync/*.cpp")

    add_library(aribplayer SHARED ${ARIBPLAYER_SOURCES})
    target_compile_definitions(aribplayer PRIVATE HAVE_FFMPEG=1)
    target_include_directories(aribplayer PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}"
        "${FFMPEG_INCLUDE_DIR}")

    foreach(ffmpeg_lib IN LISTS FFMPEG_LIBS)
        add_library(${ffmpeg_lib} SHARED IMPORTED)
        set_target_properties(${ffmpeg_lib} PROPERTIES
            IMPORTED_LOCATION "${PREBUILT_LIB_DIR}/lib${ffmpeg_lib}.so")
    endforeach()

    find_library(log-lib log)
    find_library(android-lib android)
    find_library(aaudio-lib aaudio)
    find_library(egl-lib EGL)
    find_library(gles-lib GLESv3)

    target_link_libraries(aribplayer PRIVATE
        avformat avcodec avfilter swresample swscale avutil aribcaption
        ${aaudio-lib} ${gles-lib} ${egl-lib} ${android-lib} ${log-lib})
else()
    message(STATUS "FFmpeg sysroot not found; building aribplayer no-ffmpeg stub")
    add_library(aribplayer SHARED jni/NativePlayerJni.cpp)
    find_library(log-lib log)
    target_link_libraries(aribplayer PRIVATE ${log-lib})
endif()

target_compile_features(aribplayer PRIVATE cxx_std_17)
target_compile_options(aribplayer PRIVATE -Wall -Wextra -Wpedantic)
