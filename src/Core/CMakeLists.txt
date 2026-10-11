# PlayerCoreLogic: the platform-independent core (the platform layer is src/win32 or src/posix). PlayerCore adds what
# needs libffplay or Windows UI services.
if(WIN32)
    set(WVP_PLATFORM_DIR win32)
else()
    set(WVP_PLATFORM_DIR posix)
endif()
file(GLOB PLAYER_CORE_LOGIC_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(REMOVE_ITEM PLAYER_CORE_LOGIC_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/FfPlayer.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/src/BuildCapabilities.cpp")
list(APPEND PLAYER_CORE_LOGIC_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/${WVP_PLATFORM_DIR}/Platform.cpp")
file(GLOB PLAYER_CORE_LOGIC_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/wvp/*.h")
list(REMOVE_ITEM PLAYER_CORE_LOGIC_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include/wvp/FfPlayer.h")

add_library(PlayerCoreLogic STATIC ${PLAYER_CORE_LOGIC_SOURCES} ${PLAYER_CORE_LOGIC_HEADERS})
wvp_output_dirs(PlayerCoreLogic)
# A static library has no link step, so its compiler PDB is the one that ships next to the .lib.
set_target_properties(PlayerCoreLogic PROPERTIES
    COMPILE_PDB_NAME PlayerCoreLogic
    COMPILE_PDB_OUTPUT_DIRECTORY "${WVP_BIN_ROOT}/$<CONFIG>/PlayerCoreLogic")

# libcurl (HttpFetch). Windows: a static HTTP-only build on Schannel (the system TLS and certificate store).
if(WIN32)
    include(FetchContent)
    FetchContent_Declare(curl
        URL "https://curl.se/download/curl-8.22.0.tar.xz"
        URL_HASH SHA256=f7ef3ae8a22e521f289803fe93543eb64c329b58aa73a9e224dfd915a2a5f4f7
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        EXCLUDE_FROM_ALL)
    foreach(opt HTTP_ONLY CURL_USE_SCHANNEL BUILD_STATIC_LIBS CURL_DISABLE_INSTALL)
        set(${opt} ON)
    endforeach()
    foreach(opt BUILD_SHARED_LIBS BUILD_CURL_EXE BUILD_TESTING BUILD_EXAMPLES BUILD_LIBCURL_DOCS BUILD_MISC_DOCS
                ENABLE_CURL_MANUAL CURL_ENABLE_EXPORT_TARGET CURL_USE_OPENSSL CURL_ZLIB CURL_BROTLI CURL_ZSTD USE_NGHTTP2
                USE_LIBIDN2 USE_WIN32_IDN CURL_USE_LIBPSL CURL_USE_LIBSSH2 CURL_USE_LIBSSH)
        set(${opt} OFF)
    endforeach()
    # curl is C with its own flags: keep this project's directory-wide definitions, options and include paths out of it.
    foreach(prop COMPILE_DEFINITIONS COMPILE_OPTIONS INCLUDE_DIRECTORIES)
        get_directory_property(_wvp_saved_${prop} ${prop})
        set_directory_properties(PROPERTIES ${prop} "")
    endforeach()
    FetchContent_MakeAvailable(curl)
    foreach(prop COMPILE_DEFINITIONS COMPILE_OPTIONS INCLUDE_DIRECTORIES)
        set_directory_properties(PROPERTIES ${prop} "${_wvp_saved_${prop}}")
    endforeach()
    target_link_libraries(PlayerCoreLogic PUBLIC CURL::libcurl_static winhttp)
else()
    find_package(CURL REQUIRED)
    target_link_libraries(PlayerCoreLogic PUBLIC CURL::libcurl)
endif()

if(WIN32)
    target_sources(PlayerCoreLogic PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include/wvp/win32/ProxySettings.h")
endif()

if(WVP_PORTABLE_CORE)
    return()
endif()

file(GLOB PLAYER_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/win32/*.cpp")
list(REMOVE_ITEM PLAYER_CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/win32/Platform.cpp")
list(APPEND PLAYER_CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/FfPlayer.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/src/BuildCapabilities.cpp")
file(GLOB PLAYER_CORE_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/wvp/win32/*.h")
list(REMOVE_ITEM PLAYER_CORE_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include/wvp/win32/ProxySettings.h")
list(APPEND PLAYER_CORE_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include/wvp/FfPlayer.h")

add_library(PlayerCore STATIC ${PLAYER_CORE_SOURCES} ${PLAYER_CORE_HEADERS})
wvp_output_dirs(PlayerCore)
set_target_properties(PlayerCore PROPERTIES
    COMPILE_PDB_NAME PlayerCore
    COMPILE_PDB_OUTPUT_DIRECTORY "${WVP_BIN_ROOT}/$<CONFIG>/PlayerCore")
target_link_libraries(PlayerCore PUBLIC PlayerCoreLogic windowscodecs ole32 user32)
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
