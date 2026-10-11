file(GLOB PLAYER_CORE_TESTS_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
# Runner setup and tests that need OS services (Winsock/WinHTTP, timer resolution): one directory per platform.
file(GLOB PLAYER_CORE_TESTS_PLATFORM_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/${WVP_PLATFORM}/*.cpp")

add_executable(PlayerCore.Tests main.cpp wvptest.h TempDir.h TestHost.h ${PLAYER_CORE_TESTS_SOURCES} ${PLAYER_CORE_TESTS_PLATFORM_SOURCES})
wvp_link_executable(PlayerCore.Tests PROJECTS PlayerCore)
target_include_directories(PlayerCore.Tests BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
wvp_copy_ffplay_dlls(PlayerCore.Tests)
