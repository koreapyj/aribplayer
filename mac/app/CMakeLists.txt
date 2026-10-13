cmake_minimum_required(VERSION 3.21)
# ARIB Player for macOS (see ../README.md): "ARIB Player.app".
#   export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
#   mac/app/build-app.sh                                  # everything: libffplay (if missing), presenter, app, .dmg
#   cmake -S mac/app -B .tmp/build-mac-app -G Ninja && cmake --build .tmp/build-mac-app    # the app bundle only
# Needs the darwin FFmpeg build (ffmpeg/dist/darwin-arm64: dylibs, libffplay.dylib + ffplay.h, ffmpeg-caps.inc, include/)
# and the Metal presenter (mac/presenter: `make` there first). The engine is src/Core's FfPlayer (mac/core:
# wvp_core_ffplay) over the Mac VideoPlatform (mac/platform: wvp_platform_mac); the UI is src/App's shell and pages on
# pui (mac/ui: pui_mac).
project(aribplayer_mac LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "")
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "")
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(AP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
get_filename_component(AP_ROOT "${AP_ROOT}" ABSOLUTE)
set(AP_FFLIB "${AP_ROOT}/ffmpeg/dist/darwin-arm64" CACHE PATH "FFmpeg dylibs + libffplay (darwin build)")
set(AP_FFINC "${AP_FFLIB}/include" CACHE PATH "FFmpeg headers (darwin build)")
set(AP_PRESENTER "${AP_ROOT}/mac/presenter" CACHE PATH "mac/presenter (include/ + build/libffpmetalpresenter.dylib)")
# Version: ARIBPLAYER_VERSION as src/App/CMakeLists.txt (major.minor.build; CI passes it, local builds use 0.0.0).
set(ARIBPLAYER_VERSION "0.0.0" CACHE STRING "Version of the app bundle (major.minor.build)")
if(NOT ARIBPLAYER_VERSION MATCHES "^([0-9]+)[.]([0-9]+)[.]([0-9]+)$")
    message(FATAL_ERROR "ARIBPLAYER_VERSION '${ARIBPLAYER_VERSION}' is not major.minor.build")
endif()
set(ARIBPLAYER_VERSION_MAJOR ${CMAKE_MATCH_1})
set(ARIBPLAYER_VERSION_MINOR ${CMAKE_MATCH_2})
set(ARIBPLAYER_VERSION_PATCH ${CMAKE_MATCH_3})
# CFBundleIdentifier: keys the defaults domain and the code signature.
set(ARIBPLAYER_BUNDLE_ID "kr.dcmys.aribplayer" CACHE STRING "CFBundleIdentifier")

foreach(req "${AP_FFINC}/libavcodec/avcodec.h" "${AP_FFLIB}/libffplay.dylib" "${AP_FFLIB}/ffplay.h"
            "${AP_PRESENTER}/build/libffpmetalpresenter.dylib")
    if(NOT EXISTS "${req}")
        message(FATAL_ERROR "missing ${req}: run mac/app/build-app.sh (builds libffplay and mac/presenter first)")
    endif()
endforeach()

# --- pui (src/Ui) on CoreGraphics, src/Core, the Mac VideoPlatform ----------------------------------------
add_subdirectory("${AP_ROOT}/mac/ui" "${CMAKE_CURRENT_BINARY_DIR}/ui" EXCLUDE_FROM_ALL)
add_subdirectory("${AP_ROOT}/mac/core" "${CMAKE_CURRENT_BINARY_DIR}/core")
set(WVP_FFINC "${AP_FFINC}" CACHE PATH "FFmpeg headers (darwin build)")
set(WVP_FFLIB "${AP_FFLIB}" CACHE PATH "FFmpeg dylibs (darwin build)")
set(WVP_PRESENTER "${AP_PRESENTER}" CACHE PATH "mac/presenter")
add_subdirectory("${AP_ROOT}/mac/platform" "${CMAKE_CURRENT_BINARY_DIR}/platform" EXCLUDE_FROM_ALL)
if(NOT TARGET wvp_core_ffplay)
    message(FATAL_ERROR "mac/core found no libffplay (wvp_core_ffplay) at ${AP_FFLIB}")
endif()

set(APP_ROOT "${AP_ROOT}/src/App")

# --- src/App sources compiled as is (Win32 names from shim/) -----------------------------------------------------------------------------------------
set(AP_APP_SOURCES
    ${APP_ROOT}/shell/ShellRoot.cpp
    ${APP_ROOT}/shell/TitleBar.cpp
    ${APP_ROOT}/shell/PlayerBar.cpp
    ${APP_ROOT}/shell/PlaylistPanel.cpp
    ${APP_ROOT}/shell/ShellControls.cpp
    ${APP_ROOT}/pages/SettingsPage.cpp
    ${APP_ROOT}/pages/LicensesDialog.cpp
    ${APP_ROOT}/features/MiniBar.cpp
    ${APP_ROOT}/features/MiniLogic.cpp
    ${APP_ROOT}/features/BadgeLogic.cpp
    ${APP_ROOT}/features/ListsCommon.cpp
    ${APP_ROOT}/features/Menus.cpp
    ${APP_ROOT}/features/FiltersPanel.cpp
    ${APP_ROOT}/features/FxModel.cpp
    ${APP_ROOT}/features/FxProps.cpp
    ${APP_ROOT}/features/FxUtil.cpp
    ${APP_ROOT}/features/PlaybackExtras.cpp
    ${APP_ROOT}/features/PropertiesDialog.cpp
    ${APP_ROOT}/features/SettingsLogic.cpp
    ${APP_ROOT}/features/OpenLogic.cpp
    ${APP_ROOT}/features/DebugOverlayModel.cpp
    ${APP_ROOT}/features/PipelineOverlay.cpp
    ${APP_ROOT}/features/Logging.cpp
    ${APP_ROOT}/features/LoggingFormat.cpp
    ${APP_ROOT}/Commands.cpp)

# The bundled ARIB caption font (the Windows app ships it as fonts\WLCMARU2004ARIBU.TTF next to the exe) and the notices
# (app.rc embeds THIRD-PARTY-NOTICES.txt as RCDATA; here it is a resource file the LicensesDialog shim reads).
set(AP_ARIB_FONT "${AP_ROOT}/assets/fonts/WLCMARU2004ARIBU.TTF")
set(AP_NOTICES "${AP_ROOT}/THIRD-PARTY-NOTICES.txt")
set_source_files_properties("${AP_ARIB_FONT}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/fonts")
set_source_files_properties("${AP_NOTICES}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
file(GLOB_RECURSE AP_FFMPEG_LICENSES CONFIGURE_DEPENDS LIST_DIRECTORIES false "${AP_FFLIB}/licenses/*")
foreach(license IN LISTS AP_FFMPEG_LICENSES)
    file(RELATIVE_PATH license_rel "${AP_FFLIB}/licenses" "${license}")
    get_filename_component(license_dir "${license_rel}" DIRECTORY)
    set_source_files_properties("${license}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/licenses/${license_dir}")
endforeach()

# The icon: assets/icon/aribplayer.svg -> AppIcon.icns (sips + iconutil, both part of macOS).
set(AP_ICNS "${CMAKE_CURRENT_BINARY_DIR}/AppIcon.icns")
add_custom_command(OUTPUT "${AP_ICNS}"
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/make-icns.sh" "${AP_ROOT}/assets/icon/aribplayer.svg" "${AP_ICNS}"
    DEPENDS "${AP_ROOT}/assets/icon/aribplayer.svg" "${CMAKE_CURRENT_SOURCE_DIR}/make-icns.sh"
    COMMENT "AppIcon.icns from assets/icon/aribplayer.svg")
set_source_files_properties("${AP_ICNS}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources" GENERATED TRUE)

add_executable(aribplayer MACOSX_BUNDLE
    ${AP_APP_SOURCES}
    src/main.mm
    src/MainView.mm
    src/Engine.mm
    src/CaptionFont.mm
    src/ShellView.mm
    src/VideoView.mm
    src/PuiHostView.mm
    src/SettingsWindowMac.mm
    src/SettingsServiceMac.mm
    src/OpenFlowsMac.mm
    src/MiniPlayerMac.mm
    src/Localize.cpp
    src/NativeMenuMac.cpp
    src/FileDialogMac.mm
    src/Win32Names.mm
    src/LoggingMac.mm
    src/DebugOverlayMac.mm
    src/CaptionStyleMac.mm
    "${AP_ARIB_FONT}"
    "${AP_NOTICES}"
    ${AP_FFMPEG_LICENSES}
    "${AP_ICNS}")
# shim/ first: LayerRenderer.h ahead of src/App, and windows.h / shellapi.h / shobjidl.h / wrl (the Win32 names the App
# files mention; see shim/windows.h) ahead of the SDK.
target_include_directories(aribplayer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/shim
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${APP_ROOT}
    ${AP_PRESENTER}/include
    ${AP_FFLIB}
    ${AP_FFINC})
target_compile_options(aribplayer PRIVATE -Wall -Wextra -Wno-missing-field-initializers -Wno-overloaded-virtual -fobjc-arc)
# PlaylistPanel.cpp includes <Windows.h> (capital W) and lands on shim/windows.h.
set_source_files_properties(${AP_APP_SOURCES} PROPERTIES COMPILE_OPTIONS
    "-Wno-unused-parameter;-Wno-deprecated-declarations;-Wno-nonportable-include-path;-Wno-unused-private-field;-Wno-unused-function;-Wno-sign-compare")
get_filename_component(AP_FONTS_DIR "${AP_ARIB_FONT}" DIRECTORY)
target_compile_definitions(aribplayer PRIVATE "MV_ASSETS_FONTS_DIR=\"${AP_FONTS_DIR}\""
                                              "ARIBPLAYER_VERSION_STRING=\"${ARIBPLAYER_VERSION}\"")
target_link_libraries(aribplayer PRIVATE pui_mac wvp_platform_mac "-framework CoreText" "-framework Metal"
                                         "-framework UniformTypeIdentifiers" "-framework IOKit"
                                         "-framework MediaAccessibility")
set_target_properties(aribplayer PROPERTIES
    OUTPUT_NAME "ARIB Player"
    # Self-contained bundle: every dylib is copied into Contents/Frameworks (below) and found through this rpath only.
    BUILD_WITH_INSTALL_RPATH ON
    INSTALL_RPATH "@executable_path/../Frameworks"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_NAME "ARIB Player"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${ARIBPLAYER_BUNDLE_ID}"
    MACOSX_BUNDLE_ICON_FILE "AppIcon"
    MACOSX_BUNDLE_BUNDLE_VERSION "${ARIBPLAYER_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${ARIBPLAYER_VERSION}"
    MACOSX_BUNDLE_COPYRIGHT "ARIB Player contributors"
    # Info.plist and licence resources are copied without relinking; changes must run POST_BUILD to re-sign.
    # The stamp tracks the bundle id and licence list; LINK_DEPENDS also tracks the licence contents.
    LINK_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/bundle-id.stamp;${AP_FFMPEG_LICENSES}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/bundle-id.stamp" CONTENT "${ARIBPLAYER_BUNDLE_ID}\n${AP_FFMPEG_LICENSES}\n")

# --- Contents/Frameworks: libffplay, the libav*/libsw* dylibs it links, the Metal presenter; rpaths fixed; ad-hoc signed --
add_custom_command(TARGET aribplayer POST_BUILD
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/bundle-frameworks.sh" "$<TARGET_BUNDLE_DIR:aribplayer>" "${AP_FFLIB}"
            "${AP_PRESENTER}/build/libffpmetalpresenter.dylib"
    COMMENT "Frameworks + ad-hoc signature of ARIB Player.app"
    VERBATIM)
