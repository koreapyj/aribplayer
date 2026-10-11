# PlayerApp -> aribplayer.exe ("ARIB Player": shell, pages, features, --logictest, --headless-check).
# One glob per folder keeps the object (link) order root, shell, pages, features; one combined glob would sort them.
set(PLAYER_APP_SOURCES)
foreach(dir . shell pages features)
    file(GLOB sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/*.cpp")
    list(APPEND PLAYER_APP_SOURCES ${sources})
endforeach()
file(GLOB PLAYER_APP_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/shell/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/pages/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/features/*.h")

# Version resource: ARIBPLAYER_VERSION (major.minor.build, the MSI version; CI passes it, local builds use 0.0.0).
set(ARIBPLAYER_VERSION "0.0.0" CACHE STRING "Version written to aribplayer.exe's VERSIONINFO (major.minor.build)")
if(NOT ARIBPLAYER_VERSION MATCHES "^([0-9]+)[.]([0-9]+)[.]([0-9]+)$")
    message(FATAL_ERROR "ARIBPLAYER_VERSION '${ARIBPLAYER_VERSION}' is not major.minor.build")
endif()
set(ARIBPLAYER_VERSION_MAJOR ${CMAKE_MATCH_1})
set(ARIBPLAYER_VERSION_MINOR ${CMAKE_MATCH_2})
set(ARIBPLAYER_VERSION_PATCH ${CMAKE_MATCH_3})
configure_file(aribplayer_version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/aribplayer_version.h" @ONLY)

add_executable(PlayerApp WIN32 ${PLAYER_APP_SOURCES} ${PLAYER_APP_HEADERS} app.manifest app.rc)
set_target_properties(PlayerApp PROPERTIES OUTPUT_NAME aribplayer)
target_include_directories(PlayerApp PRIVATE
    "$<$<COMPILE_LANGUAGE:RC>:${CMAKE_CURRENT_BINARY_DIR}/generated>" "$<$<COMPILE_LANGUAGE:RC>:${WVP_ROOT}/assets/icon>" "$<$<COMPILE_LANGUAGE:RC>:${WVP_ROOT}>" "$<$<COMPILE_LANGUAGE:RC>:${CMAKE_CURRENT_SOURCE_DIR}>")
set_source_files_properties(app.rc PROPERTIES
    OBJECT_DEPENDS "${WVP_ROOT}/assets/icon/aribplayer.ico;${WVP_ROOT}/THIRD-PARTY-NOTICES.txt;${CMAKE_CURRENT_BINARY_DIR}/generated/aribplayer_version.h")
wvp_link_executable(PlayerApp
    LIBS d3d11.lib dxgi.lib d2d1.lib dwrite.lib dcomp.lib dwmapi.lib shell32.lib ole32.lib oleaut32.lib uiautomationcore.lib
         advapi32.lib windowscodecs.lib comctl32.lib shlwapi.lib uxtheme.lib
    PROJECTS PlayerPresenter PlayerCore PlayerUi)
target_include_directories(PlayerApp BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_options(PlayerApp PRIVATE $<$<CONFIG:Debug>:/INCREMENTAL:NO>)

wvp_copy_ffplay_dlls(PlayerApp)
wvp_copy_runtime_files(PlayerApp fonts "${WVP_ROOT}/assets/fonts/WLCMARU2004ARIBU.TTF")
