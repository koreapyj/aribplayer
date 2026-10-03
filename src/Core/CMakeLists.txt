file(GLOB PLAYER_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
file(GLOB PLAYER_CORE_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/wvp/*.h")

add_library(PlayerCore STATIC ${PLAYER_CORE_SOURCES} ${PLAYER_CORE_HEADERS})
wvp_output_dirs(PlayerCore)
# A static library has no link step, so its compiler PDB is the one that ships next to the .lib.
set_target_properties(PlayerCore PROPERTIES
    COMPILE_PDB_NAME PlayerCore
    COMPILE_PDB_OUTPUT_DIRECTORY "${WVP_BIN_ROOT}/$<CONFIG>/PlayerCore")
# Sources include ffplay.h; keep the import library ahead of the first compile.
add_dependencies(PlayerCore libffplay_implib)

# What the FFmpeg build fixes (filters, the decoder of each hwaccel, libffplay features), compiled into BuildCapabilities.cpp.
set(WVP_FFCAPS_INC "${CMAKE_CURRENT_BINARY_DIR}/generated/ffmpeg-caps.inc")
add_custom_command(
    OUTPUT "${WVP_FFCAPS_INC}"
    COMMAND "${WVP_FFPLAY_DIR}/ffcapsdump.exe" "${WVP_FFCAPS_INC}"
    WORKING_DIRECTORY "${WVP_FFPLAY_DIR}"
    DEPENDS "${WVP_FFPLAY_DIR}/ffcapsdump.exe" ${WVP_FFPLAY_DLLS}
    COMMENT "Generating ffmpeg-caps.inc from ffcapsdump.exe"
    VERBATIM)
target_sources(PlayerCore PRIVATE "${WVP_FFCAPS_INC}")
target_include_directories(PlayerCore PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/BuildCapabilities.cpp" PROPERTIES OBJECT_DEPENDS "${WVP_FFCAPS_INC}")
