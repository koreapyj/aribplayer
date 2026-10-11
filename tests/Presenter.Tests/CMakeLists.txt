file(GLOB PLAYER_PRESENTER_TESTS_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.h")

add_executable(PlayerPresenter.Tests ${PLAYER_PRESENTER_TESTS_SOURCES} "${WVP_ROOT}/tests/common/wvptest.h")
wvp_link_executable(PlayerPresenter.Tests PROJECTS PlayerPresenter PlayerCore)
target_include_directories(PlayerPresenter.Tests BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${WVP_ROOT}/tests/common")
wvp_copy_ffplay_dlls(PlayerPresenter.Tests)
