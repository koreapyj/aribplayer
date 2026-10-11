file(GLOB PLAYER_CORE_TESTS_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")

add_executable(PlayerCore.Tests main.cpp "${WVP_ROOT}/tests/common/wvptest.h" "${WVP_ROOT}/tests/common/TestPlayer.h"
    ${PLAYER_CORE_TESTS_SOURCES})
wvp_link_executable(PlayerCore.Tests PROJECTS PlayerCore)
target_include_directories(PlayerCore.Tests BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${WVP_ROOT}/tests/common")
# TestPlayer.h reads AVFrame fields: FFmpeg's public headers of the package (no avutil import library needed).
target_include_directories(PlayerCore.Tests SYSTEM PRIVATE "${WVP_FFPLAY_DIR}/include")
wvp_copy_ffplay_dlls(PlayerCore.Tests)
