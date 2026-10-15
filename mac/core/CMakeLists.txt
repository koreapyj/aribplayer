# wvp_core_logic: the platform-independent core (src/Core: PlayerCoreLogic) built for macOS with the POSIX
# platform layer src/Core/src/posix/Platform.cpp (same file selection as src/Core/CMakeLists.txt). Consumers:
#   add_subdirectory(<repo>/mac/core <binary dir>)   and   target_link_libraries(<target> PRIVATE wvp_core_logic)
# The include directory (src/Core/include) and libcurl (HttpFetch) are propagated. FfPlayer.cpp (the libffplay binding) is
# the separate, optional wvp_core_ffplay below. Not built here: BuildCapabilities.cpp (table generated from the FFmpeg
# dist), src/win32.
#
# include/wvpmac/libcxx_clock_cast.h is force-included into this target only: Apple's libc++ lacks
# std::chrono::clock_cast, which posix/Platform.cpp uses (the header is empty on a library that has it).
if(TARGET wvp_core_logic)
    return()
endif()

set(WVP_CORE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../src/Core")
set(WVP_MAC_CORE_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(WVP_MAC_CLOCK_CAST_HEADER "${WVP_MAC_CORE_INCLUDE}/wvpmac/libcxx_clock_cast.h")

file(GLOB WVP_CORE_LOGIC_SOURCES CONFIGURE_DEPENDS "${WVP_CORE_ROOT}/src/*.cpp")
list(REMOVE_ITEM WVP_CORE_LOGIC_SOURCES "${WVP_CORE_ROOT}/src/FfPlayer.cpp" "${WVP_CORE_ROOT}/src/BuildCapabilities.cpp")
list(APPEND WVP_CORE_LOGIC_SOURCES "${WVP_CORE_ROOT}/src/posix/Platform.cpp")

add_library(wvp_core_logic STATIC ${WVP_CORE_LOGIC_SOURCES})
target_include_directories(wvp_core_logic PUBLIC "${WVP_CORE_ROOT}/include")
target_include_directories(wvp_core_logic SYSTEM PRIVATE "${WVP_CORE_ROOT}/../../third_party")
target_compile_features(wvp_core_logic PUBLIC cxx_std_20)
target_compile_options(wvp_core_logic PRIVATE -Wall -Wextra "-include${WVP_MAC_CLOCK_CAST_HEADER}")
set_source_files_properties("${WVP_CORE_ROOT}/src/posix/Platform.cpp" PROPERTIES OBJECT_DEPENDS "${WVP_MAC_CLOCK_CAST_HEADER}")

# libcurl: the SDK's (/usr/lib/libcurl.tbd) unless CMake finds another one first.
find_package(CURL REQUIRED)
target_link_libraries(wvp_core_logic PUBLIC CURL::libcurl)

# --- wvp_core_ffplay: src/Core/src/FfPlayer.cpp (IMediaPlayer over libffplay, FfPlayer::ProbeEngine), over the
# macOS libffplay (ffmpeg/darwin -> ffmpeg/dist/darwin-arm64: libffplay.dylib + ffplay.h + the FFmpeg dylibs). Optional:
# only defined when that dist exists. Consumers check `if(TARGET wvp_core_ffplay)`. The dylibs are found at run time through
# @rpath: give the executable BUILD_RPATH "${WVP_FFPLAY_DIR}".
set(WVP_FFPLAY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../ffmpeg/dist/darwin-arm64" CACHE PATH "macOS libffplay dist (libffplay.dylib, ffplay.h)")
get_filename_component(WVP_FFPLAY_DIR "${WVP_FFPLAY_DIR}" ABSOLUTE)
if(EXISTS "${WVP_FFPLAY_DIR}/libffplay.dylib" AND EXISTS "${WVP_FFPLAY_DIR}/ffplay.h")
    add_library(libffplay SHARED IMPORTED GLOBAL)
    set_target_properties(libffplay PROPERTIES IMPORTED_LOCATION "${WVP_FFPLAY_DIR}/libffplay.dylib"
                                               INTERFACE_INCLUDE_DIRECTORIES "${WVP_FFPLAY_DIR}")
    add_library(wvp_core_ffplay STATIC "${WVP_CORE_ROOT}/src/FfPlayer.cpp")
    target_compile_options(wvp_core_ffplay PRIVATE -Wall -Wextra)
    target_link_libraries(wvp_core_ffplay PUBLIC wvp_core_logic libffplay)
    message(STATUS "mac/core: libffplay at ${WVP_FFPLAY_DIR} (wvp_core_ffplay)")
else()
    message(STATUS "mac/core: no libffplay at ${WVP_FFPLAY_DIR}; wvp_core_ffplay not built")
endif()

# --- wvp_core_buildcaps: src/Core/src/BuildCapabilities.cpp, over the darwin dist's ffmpeg-caps.inc (what the
# macOS FFmpeg build fixes: filters, build features, the hwaccel decoders ffcapsdump lists). Windows generates the .inc at
# build time from ffcapsdump.exe; build-ffplay.sh (ffmpeg/darwin) ships the darwin one in the dist. Optional like
# wvp_core_ffplay. Note: ffcapsdump only lists the d3d11va / d3d12va / dxva2 decoders; mac/platform's
# wvp::mac::BuildCapabilities() adds the videotoolbox ones with ffcapsdump's own rule (see MacVideoPlatform.h).
if(EXISTS "${WVP_FFPLAY_DIR}/ffmpeg-caps.inc")
    add_library(wvp_core_buildcaps STATIC "${WVP_CORE_ROOT}/src/BuildCapabilities.cpp")
    target_include_directories(wvp_core_buildcaps PRIVATE "${WVP_FFPLAY_DIR}")
    target_compile_options(wvp_core_buildcaps PRIVATE -Wall -Wextra)
    target_link_libraries(wvp_core_buildcaps PUBLIC wvp_core_logic)
    set_source_files_properties("${WVP_CORE_ROOT}/src/BuildCapabilities.cpp" PROPERTIES OBJECT_DEPENDS "${WVP_FFPLAY_DIR}/ffmpeg-caps.inc")
    message(STATUS "mac/core: ffmpeg-caps.inc at ${WVP_FFPLAY_DIR} (wvp_core_buildcaps)")
endif()
