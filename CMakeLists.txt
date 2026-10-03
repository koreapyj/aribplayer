# ARIB Player: MSVC x64 only. Configure from a VS 2022 x64 developer environment
# (scripts\build.ps1 sets one up), e.g. `cmake --preset release && cmake --build --preset release`.
cmake_minimum_required(VERSION 3.25)

# Only the Visual Studio generator reads this; for Ninja the SDK comes from the developer environment (checked below).
set(WVP_WINDOWS_SDK_VERSION "10.0.26100.0" CACHE STRING "Windows SDK version the build must use")
if(NOT CMAKE_SYSTEM_VERSION)
    set(CMAKE_SYSTEM_VERSION "${WVP_WINDOWS_SDK_VERSION}")
endif()

project(ARIBPlayer LANGUAGES CXX RC)

if(NOT MSVC OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Build with MSVC x64 (cl.exe from a VS 2022 x64 developer environment).")
endif()
if(NOT CMAKE_GENERATOR MATCHES "Visual Studio")
    string(REGEX REPLACE "[\\\\/]+$" "" _wvp_sdk "$ENV{WindowsSDKVersion}")
    if(NOT _wvp_sdk STREQUAL WVP_WINDOWS_SDK_VERSION)
        message(FATAL_ERROR "Windows SDK '${_wvp_sdk}' in the developer environment, expected ${WVP_WINDOWS_SDK_VERSION}: "
                            "run vcvars64.bat ${WVP_WINDOWS_SDK_VERSION} (scripts\\build.ps1 does this).")
    endif()
endif()

set(WVP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(WVP_BIN_ROOT "${WVP_ROOT}/artifacts/bin" CACHE PATH "Final outputs go to <WVP_BIN_ROOT>/<Config>/<Target>/")
set(WVP_FFPLAY_DIR "${WVP_ROOT}/ffmpeg/dist/win64" CACHE PATH "libffplay + FFmpeg DLLs (ffmpeg/build-ffplay.ps1 output)")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<CONFIG:Debug>,EditAndContinue,ProgramDatabase>")

# Replace CMake's defaults (/DWIN32 /D_WINDOWS /GR, /Ob2, /Ob0) so the command line holds only what we ask for.
set(CMAKE_CXX_FLAGS "/EHsc")
set(CMAKE_CXX_FLAGS_RELEASE "/O2 /Oi /Gy /DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "/Od /RTC1 /JMC /D_DEBUG")
# C++ only: the resource compiler (app.rc) shares the directory options but not these flags.
foreach(opt /W4 /sdl /utf-8 /permissive- /Zc:inline /external:W4 /diagnostics:column /FC)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:${opt}>")
endforeach()
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
include_directories("${WVP_ROOT}/src/Core/include" "${WVP_ROOT}/src/Ui/include" "${WVP_FFPLAY_DIR}")

# The default Windows libraries are placed by wvp_link_executable() instead of CMake (which would put them last).
set(CMAKE_CXX_STANDARD_LIBRARIES "")
set(WVP_DEFAULT_LIBS kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib
    oleaut32.lib uuid.lib odbc32.lib odbccp32.lib)
set(CMAKE_EXE_LINKER_FLAGS "/machine:x64 /DEBUG")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "/INCREMENTAL:NO /OPT:REF /OPT:ICF")
set(CMAKE_EXE_LINKER_FLAGS_DEBUG "/INCREMENTAL")

function(wvp_output_dirs target)
    set(dir "${WVP_BIN_ROOT}/$<CONFIG>/${target}")
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${dir}"
        ARCHIVE_OUTPUT_DIRECTORY "${dir}"
        PDB_OUTPUT_DIRECTORY "${dir}")
endfunction()

if(NOT EXISTS "${WVP_FFPLAY_DIR}/ffplay.h")
    message(FATAL_ERROR "ffplay.h not found in ${WVP_FFPLAY_DIR} (build ffmpeg\\build-ffplay.ps1 first).")
endif()
if(NOT CMAKE_AR)
    message(FATAL_ERROR "lib.exe not found (CMAKE_AR is empty).")
endif()
set(WVP_FFPLAY_IMPLIB_DIR "${CMAKE_BINARY_DIR}/libffplay")
set(WVP_FFPLAY_DEF "${WVP_FFPLAY_IMPLIB_DIR}/libffplay.def")
set(WVP_FFPLAY_LIB "${WVP_FFPLAY_IMPLIB_DIR}/libffplay.lib")
add_custom_command(
    OUTPUT "${WVP_FFPLAY_DEF}" "${WVP_FFPLAY_LIB}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${WVP_FFPLAY_IMPLIB_DIR}"
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${WVP_ROOT}/cmake/gen-ffplay-def.ps1"
            -Header "${WVP_FFPLAY_DIR}/ffplay.h" -Def "${WVP_FFPLAY_DEF}"
    COMMAND "${CMAKE_AR}" /nologo "/def:${WVP_FFPLAY_DEF}" /machine:x64 "/out:${WVP_FFPLAY_LIB}"
    DEPENDS "${WVP_FFPLAY_DIR}/ffplay.h" "${WVP_ROOT}/cmake/gen-ffplay-def.ps1"
    COMMENT "Generating libffplay.lib from ffplay.h"
    VERBATIM)
add_custom_target(libffplay_implib DEPENDS "${WVP_FFPLAY_LIB}")

# Every executable links libffplay, user32 and gdi32 (whether it calls them or not; the linker drops unused imports).
# The order matters: the static libraries of this repo come last, after the Windows libraries. The linker continues its
# search for a library member's externals from that library onward, so the default libraries named by #pragma comment
# (windowsapp.lib, appended at the end) are found before kernel32.lib / uiautomationcore.lib for some imports, which
# then bind to api-ms-win-* / ext-ms-win-* API sets.
function(wvp_link_executable target)
    cmake_parse_arguments(arg "" "" "LIBS;PROJECTS" ${ARGN})
    wvp_output_dirs(${target})
    # The import library is linked by path (an INTERFACE target would move it to the end of the line).
    target_link_libraries(${target} PRIVATE ${arg_LIBS} "${WVP_FFPLAY_LIB}" user32.lib gdi32.lib ${WVP_DEFAULT_LIBS} ${arg_PROJECTS})
    add_dependencies(${target} libffplay_implib)
endfunction()

file(GLOB WVP_FFPLAY_DLLS CONFIGURE_DEPENDS "${WVP_FFPLAY_DIR}/*.dll")
if(NOT WVP_FFPLAY_DLLS)
    message(FATAL_ERROR "No DLLs in ${WVP_FFPLAY_DIR} (build ffmpeg\\build-ffplay.ps1 first).")
endif()
if(NOT EXISTS "${WVP_FFPLAY_DIR}/ffcapsdump.exe")
    message(FATAL_ERROR "ffcapsdump.exe not found in ${WVP_FFPLAY_DIR} (build ffmpeg\\build-ffplay.ps1 first).")
endif()

function(wvp_copy_runtime_files target subdir)
    set(dir "${WVP_BIN_ROOT}/$<CONFIG>/${target}")
    if(NOT subdir STREQUAL ".")
        string(APPEND dir "/${subdir}")
    endif()
    set(outputs)
    foreach(src IN LISTS ARGN)
        get_filename_component(name "${src}" NAME)
        add_custom_command(
            OUTPUT "${dir}/${name}"
            COMMAND "${CMAKE_COMMAND}" "-DSRC=${src}" "-DDST_DIR=${dir}" -P "${WVP_ROOT}/cmake/copy-runtime-file.cmake"
            DEPENDS "${src}" "${WVP_ROOT}/cmake/copy-runtime-file.cmake"
            VERBATIM)
        list(APPEND outputs "${dir}/${name}")
    endforeach()
    string(MAKE_C_IDENTIFIER "${target}_${subdir}" id)
    add_custom_target(${id}_runtime DEPENDS ${outputs})
    add_dependencies(${target} ${id}_runtime)
endfunction()

function(wvp_copy_ffplay_dlls target)
    wvp_copy_runtime_files(${target} "." ${WVP_FFPLAY_DLLS})
    set(keep)
    foreach(src IN LISTS WVP_FFPLAY_DLLS)
        get_filename_component(name "${src}" NAME)
        list(APPEND keep "${name}")
    endforeach()
    list(JOIN keep "|" keep)
    add_custom_target(${target}_prune_dlls
        COMMAND "${CMAKE_COMMAND}" "-DDIR=${WVP_BIN_ROOT}/$<CONFIG>/${target}" "-DKEEP=${keep}"
                -P "${WVP_ROOT}/cmake/prune-runtime-dlls.cmake"
        VERBATIM)
    add_dependencies(${target} ${target}_prune_dlls)
endfunction()

enable_testing()

add_subdirectory(src/Core)
add_subdirectory(src/Ui)
add_subdirectory(src/App)
add_subdirectory(tests)
