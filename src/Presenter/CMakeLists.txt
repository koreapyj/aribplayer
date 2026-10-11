# PlayerPresenter: the Windows VideoPlatform (DXGI adapters, the per-adapter probe) and the D3D11 presenter that
# implements libffplay's FFPPresenter. Built against FFmpeg's public headers and the avutil import library of the
# FFmpeg package (ffmpeg\dist\win64: include\, lib\).
enable_language(C)

set(WVP_FFMPEG_INCLUDE_DIR "${WVP_FFPLAY_DIR}/include" CACHE PATH "FFmpeg public headers (libavutil)")
find_library(WVP_AVUTIL_LIB NAMES avutil libavutil.dll.a PATHS "${WVP_FFPLAY_DIR}/lib" NO_DEFAULT_PATH
             DOC "avutil import library of the FFmpeg package")
if(NOT EXISTS "${WVP_FFMPEG_INCLUDE_DIR}/libavutil/hwcontext_d3d11va.h")
    message(FATAL_ERROR "FFmpeg headers not found in ${WVP_FFMPEG_INCLUDE_DIR} (build ffmpeg\\build-ffplay.ps1 first).")
endif()
if(NOT WVP_AVUTIL_LIB)
    message(FATAL_ERROR "No avutil import library in ${WVP_FFPLAY_DIR}/lib (build ffmpeg\\build-ffplay.ps1 first).")
endif()

# d3d11.c includes d3d11_device.c and d3d11_shader.c (one translation unit).
add_library(PlayerPresenter STATIC
    src/d3d11.c
    src/probe.c
    src/D3D11Presenter.cpp
    src/WindowsVideoPlatform.cpp
    src/presenter_core.h
    src/PresenterInternal.h
    include/wvp/win32/WindowsVideoPlatform.h)
set_source_files_properties(src/d3d11_device.c src/d3d11_shader.c PROPERTIES HEADER_FILE_ONLY ON)
target_sources(PlayerPresenter PRIVATE src/d3d11_device.c src/d3d11_shader.c)
target_compile_options(PlayerPresenter PRIVATE "$<$<COMPILE_LANGUAGE:C>:/std:c17;/utf-8>")
target_include_directories(PlayerPresenter PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(PlayerPresenter SYSTEM PRIVATE "${WVP_FFMPEG_INCLUDE_DIR}")
target_link_libraries(PlayerPresenter PUBLIC PlayerCore "${WVP_AVUTIL_LIB}" dxgi gdi32 comctl32)
wvp_output_dirs(PlayerPresenter)
set_target_properties(PlayerPresenter PROPERTIES
    COMPILE_PDB_NAME PlayerPresenter
    COMPILE_PDB_OUTPUT_DIRECTORY "${WVP_BIN_ROOT}/$<CONFIG>/PlayerPresenter")
