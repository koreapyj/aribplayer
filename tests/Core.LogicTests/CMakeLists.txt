# PlayerCore.LogicTests: the portable core tests. Usage: PlayerCore.LogicTests [name-substring-filter]
file(GLOB PLAYER_CORE_LOGIC_TESTS_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
# Platform subsets: expectations that depend on the native path syntax and file system.
if(WIN32)
    file(GLOB PLAYER_CORE_LOGIC_TESTS_PLATFORM_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/win32/*.cpp")
else()
    file(GLOB PLAYER_CORE_LOGIC_TESTS_PLATFORM_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/posix/*.cpp")
    find_package(Threads REQUIRED)
    set(PLAYER_CORE_LOGIC_TESTS_LIBS Threads::Threads)
endif()

add_executable(PlayerCore.LogicTests main.cpp ${PLAYER_CORE_LOGIC_TESTS_SOURCES} ${PLAYER_CORE_LOGIC_TESTS_PLATFORM_SOURCES}
    "${WVP_ROOT}/tests/common/wvptest.h" "${WVP_ROOT}/tests/common/TempDir.h")
wvp_link_executable(PlayerCore.LogicTests NO_FFPLAY LIBS ${PLAYER_CORE_LOGIC_TESTS_LIBS} PROJECTS PlayerCoreLogic)
target_include_directories(PlayerCore.LogicTests BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${WVP_ROOT}/tests/common")
