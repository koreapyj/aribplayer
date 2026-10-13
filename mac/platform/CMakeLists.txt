cmake_minimum_required(VERSION 3.21)
# wvp_platform_mac: the macOS wvp::VideoPlatform (src/Core/include/wvp/VideoPlatform.h) over mac/presenter.
#   export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
#   cmake -S mac/platform -B .tmp/build-mac-platform -G Ninja && cmake --build .tmp/build-mac-platform
#   .tmp/build-mac-platform/wvp_platform_mac_test            # unit tests
# Also usable with add_subdirectory(<repo>/mac/platform <bindir>); link `wvp_platform_mac`.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(wvp_platform_mac LANGUAGES CXX OBJCXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_OBJCXX_STANDARD 20)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "")
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE RelWithDebInfo)
    endif()
    enable_testing()
endif()

set(WVP_PLATFORM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
get_filename_component(WVP_PLATFORM_ROOT "${WVP_PLATFORM_ROOT}" ABSOLUTE)
set(WVP_FFLIB "${WVP_PLATFORM_ROOT}/ffmpeg/dist/darwin-arm64" CACHE PATH "FFmpeg dylibs (darwin build)")
set(WVP_FFINC "${WVP_FFLIB}/include" CACHE PATH "FFmpeg headers (darwin build)")
set(WVP_PRESENTER "${WVP_PLATFORM_ROOT}/mac/presenter" CACHE PATH "mac/presenter (include/ + build/libffpmetalpresenter.dylib)")

if(NOT EXISTS "${WVP_FFINC}/libavutil/hwcontext.h")
    message(FATAL_ERROR "FFmpeg headers not found at ${WVP_FFINC} (set WVP_FFINC)")
endif()
if(NOT EXISTS "${WVP_PRESENTER}/build/libffpmetalpresenter.dylib")
    message(FATAL_ERROR "mac/presenter is not built (${WVP_PRESENTER}/build/libffpmetalpresenter.dylib): run `make -C mac/presenter` first")
endif()

if(NOT TARGET wvp_core_logic)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../core" "${CMAKE_CURRENT_BINARY_DIR}/core")
endif()

# The presenter dylib, imported as built by mac/presenter/Makefile (install_name @rpath/libffpmetalpresenter.dylib).
if(NOT TARGET ffpmetalpresenter)
    add_library(ffpmetalpresenter SHARED IMPORTED GLOBAL)
    set_target_properties(ffpmetalpresenter PROPERTIES IMPORTED_LOCATION "${WVP_PRESENTER}/build/libffpmetalpresenter.dylib")
endif()

add_library(wvp_platform_mac STATIC
    src/MacVideoPlatform.mm
    src/MetalPresenter.mm
    src/BuildCapabilitiesMac.cpp
    include/wvp/mac/MacVideoPlatform.h)
target_include_directories(wvp_platform_mac
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" "${WVP_FFLIB}" "${WVP_PRESENTER}/include"
    PRIVATE "${WVP_FFINC}")
target_compile_options(wvp_platform_mac PRIVATE -Wall -Wextra -fobjc-arc)
target_link_directories(wvp_platform_mac PUBLIC "${WVP_FFLIB}")
target_link_libraries(wvp_platform_mac PUBLIC
    wvp_core_logic ffpmetalpresenter avcodec avutil
    "-framework Foundation" "-framework Metal" "-framework QuartzCore" "-framework VideoToolbox" "-framework CoreMedia"
    "-framework CoreVideo" "-framework CoreGraphics" "-framework IOKit" "-framework AppKit")
# libffplay (mac/core's wvp_core_ffplay, when ffmpeg/dist/darwin-arm64 has it): Probe merges FfPlayer::ProbeEngine.
if(TARGET wvp_core_ffplay)
    target_link_libraries(wvp_platform_mac PUBLIC wvp_core_ffplay)
    target_compile_definitions(wvp_platform_mac PUBLIC WVP_MAC_HAVE_LIBFFPLAY=1)
else()
    message(WARNING "wvp_platform_mac: no libffplay; Probe returns the platform probe without the engine merge")
endif()
# BuildCapabilities (mac/core's wvp_core_buildcaps over the darwin ffmpeg-caps.inc): wvp::mac::BuildCapabilities.
if(TARGET wvp_core_buildcaps)
    target_link_libraries(wvp_platform_mac PUBLIC wvp_core_buildcaps)
    target_compile_definitions(wvp_platform_mac PRIVATE WVP_MAC_HAVE_BUILDCAPS=1)
endif()

# --- unit tests (adapter list, probe content incl. the libffplay merge, presenter creation) -----------------------------
add_executable(wvp_platform_mac_test test/main.mm)
target_include_directories(wvp_platform_mac_test PRIVATE "${WVP_PLATFORM_ROOT}/tests/common" "${WVP_FFINC}")
target_compile_options(wvp_platform_mac_test PRIVATE -Wall -Wextra -fobjc-arc)
target_link_libraries(wvp_platform_mac_test PRIVATE wvp_platform_mac)
set_target_properties(wvp_platform_mac_test PROPERTIES BUILD_RPATH "${WVP_FFLIB};${WVP_FFPLAY_DIR};${WVP_PRESENTER}/build")
add_test(NAME wvp_platform_mac_test COMMAND wvp_platform_mac_test)
