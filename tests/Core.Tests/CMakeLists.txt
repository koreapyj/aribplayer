file(GLOB PLAYER_CORE_TESTS_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")

add_executable(PlayerCore.Tests main.cpp wvptest.h ${PLAYER_CORE_TESTS_SOURCES})
wvp_link_executable(PlayerCore.Tests PROJECTS PlayerCore)
target_include_directories(PlayerCore.Tests BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
wvp_copy_ffplay_dlls(PlayerCore.Tests)
