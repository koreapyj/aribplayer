# PlayerUi.Tests: console runner. Usage: PlayerUi.Tests.exe [name-substring-filter]
file(GLOB PLAYER_UI_TESTS_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")

add_executable(PlayerUi.Tests main.cpp ${PLAYER_UI_TESTS_SOURCES})
wvp_link_executable(PlayerUi.Tests
    LIBS d3d11.lib dxgi.lib d2d1.lib dwrite.lib dcomp.lib uiautomationcore.lib ole32.lib oleaut32.lib
    PROJECTS PlayerUi)
# wvptest.h lives in Core.Tests.
target_include_directories(PlayerUi.Tests BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${WVP_ROOT}/tests/Core.Tests")
