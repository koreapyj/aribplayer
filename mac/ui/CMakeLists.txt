cmake_minimum_required(VERSION 3.21)
# macOS build of the shared self-drawn widget toolkit "pui" (src/Ui) plus its Mac pieces (mac/ui).
#   cmake -S mac/ui -B .tmp/build-mac-ui -G Ninja && cmake --build .tmp/build-mac-ui
# Platform-neutral src/Ui sources are compiled as-is; the Win32-only ones are excluded (below).
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/MacDeploymentTarget.cmake")
project(pui_mac LANGUAGES CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(PUI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../src/Ui")

# --- src/Ui sources that build on macOS as-is -----------------------------------------------------------------------
set(PUI_NEUTRAL_SOURCES
    ${PUI_ROOT}/src/Buttons.cpp
    ${PUI_ROOT}/src/ComboBox.cpp
    ${PUI_ROOT}/src/InfoBar.cpp
    ${PUI_ROOT}/src/Menu.cpp
    ${PUI_ROOT}/src/Panels.cpp
    ${PUI_ROOT}/src/ScrollViewer.cpp
    ${PUI_ROOT}/src/Slider.cpp
    ${PUI_ROOT}/src/TextBox.cpp
    ${PUI_ROOT}/src/Toggles.cpp
    ${PUI_ROOT}/src/ToolTipStyle.cpp
    ${PUI_ROOT}/src/VirtualList.cpp
    ${PUI_ROOT}/src/Widget.cpp)
# Excluded (Win32 only):      D2dRenderer.cpp (Direct2D), Uia.cpp + UiaProviders.cpp (UI Automation / COM), Clipboard.cpp (Win32
# clipboard; replaced by mac/ui/src/ClipboardMac.mm).

add_library(pui_mac STATIC
    ${PUI_NEUTRAL_SOURCES}
    src/CGRenderer.mm
    src/SymbolMap.mm
    src/MacTheme.mm
    src/KeyMap.mm
    src/MenuPopup.cpp
    src/ClipboardMac.mm)
target_include_directories(pui_mac PUBLIC ${PUI_ROOT}/include ${CMAKE_CURRENT_SOURCE_DIR}/include)
# -Wno-missing-field-initializers: Renderer.h RecordingRenderer aggregate inits; -Wno-overloaded-virtual: Slider.h:28
# SetValue(double) hides Widget::SetValue(const wstring&).
target_compile_options(pui_mac PRIVATE -Wall -Wextra -Wno-missing-field-initializers -Wno-overloaded-virtual -fobjc-arc)
target_link_libraries(pui_mac PUBLIC "-framework AppKit" "-framework CoreGraphics" "-framework CoreText" "-framework QuartzCore"
                                     "-framework Carbon")

# Standalone UI build: each process resolves AppKit's accent from its own argument domain, without changing preferences.
if(PROJECT_IS_TOP_LEVEL)
    enable_testing()
    add_executable(pui_mac_theme_test tests/MacThemeTests.mm)
    target_compile_options(pui_mac_theme_test PRIVATE -fobjc-arc)
    target_link_libraries(pui_mac_theme_test PRIVATE pui_mac)
    foreach(accent IN ITEMS -2 4 5 6 0 1 2 3 -1)
        if(accent EQUAL -1)
            set(aqua_variant 6)
        else()
            set(aqua_variant 1)
        endif()
        add_test(NAME mac_theme_accent_${accent} COMMAND pui_mac_theme_test
                 -AppleAccentColor ${accent} -AppleAquaColorVariant ${aqua_variant})
    endforeach()
endif()
