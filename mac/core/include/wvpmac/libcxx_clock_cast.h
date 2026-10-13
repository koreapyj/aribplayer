#pragma once
// Force-included (-include) when src/Core is compiled on macOS.
//
// src/Core/src/posix/Platform.cpp (ListFiles) calls std::chrono::clock_cast<std::chrono::system_clock>(file_time), a C++20
// function Apple's libc++ does not ship (its <version> reports __cpp_lib_chrono 201611L; only std::chrono::file_clock::to_sys /
// from_sys exist).
//
// Guarded by the feature-test macro: on a standard library with the C++20 chrono additions (__cpp_lib_chrono >= 201907L) this
// header is empty, as it is for a translation unit compiled below C++20 (CMake's compiler probes). Only the one instantiation
// Core uses is provided (file_clock -> system_clock); it is implemented with the library's own file_clock::to_sys, so the
// result is what a conforming clock_cast returns.
#if defined(__cplusplus) && __cplusplus >= 202002L
#include <version>
#endif

#if defined(__cplusplus) && __cplusplus >= 202002L && defined(_LIBCPP_VERSION) && \
    (!defined(__cpp_lib_chrono) || __cpp_lib_chrono < 201907L)
#include <chrono>
#include <type_traits>

namespace std::chrono {

template <class Dest, class Duration>
    requires std::is_same_v<Dest, system_clock>
auto clock_cast(const time_point<file_clock, Duration>& t) {
    return file_clock::to_sys(t);
}

}
#endif
