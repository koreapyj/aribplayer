# The minimum macOS of every Mac target (mac/app, mac/ui, mac/platform, mac/core), included before project().
# 13.3: the shared Core formats floating-point numbers with std::to_chars (JsonWriter.cpp, FilterManager.cpp), which
# Apple's libc++ provides from macOS 13.3. mac/presenter/Makefile (MACOS_MIN) and the bundle check in
# mac/app/build-app.sh hold the same value.
#
# project() creates an empty CMAKE_OSX_DEPLOYMENT_TARGET cache entry when none exists, and a later
# set(... CACHE) never replaces an existing entry; this file therefore runs first and also fills an empty entry.
# -DCMAKE_OSX_DEPLOYMENT_TARGET may raise the value, not lower it.
include_guard(GLOBAL)
set(WVP_MAC_MIN_MACOS "13.3")
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "${WVP_MAC_MIN_MACOS}" CACHE STRING "Minimum macOS version" FORCE)
elseif(CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS WVP_MAC_MIN_MACOS)
    message(FATAL_ERROR "CMAKE_OSX_DEPLOYMENT_TARGET ${CMAKE_OSX_DEPLOYMENT_TARGET} is below ${WVP_MAC_MIN_MACOS}, the minimum of the Mac build")
endif()
# An API newer than the deployment target needs an @available / __builtin_available guard.
add_compile_options(-Werror=unguarded-availability-new)
