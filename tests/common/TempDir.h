#pragma once
// A fresh directory under the system temp folder, removed with its contents on destruction. Paths are UTF-8.
#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

#include "wvp/Platform.h"

namespace wvptest {

inline std::filesystem::path FsPath(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

inline std::string Utf8Path(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

struct TempDir {
    std::string path;
    explicit TempDir(const char* prefix = "wvp_") {
        static std::atomic<int> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto dir = std::filesystem::temp_directory_path() /
                         (std::string(prefix) + std::to_string(stamp) + "_" + std::to_string(counter++));
        std::filesystem::create_directories(dir);
        path = Utf8Path(dir);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(FsPath(path), ec);
    }
    template <class... Rest>
    std::string File(const std::string& name, const Rest&... rest) const {
        std::string result = wvp::platform::Combine(path, name);
        ((result = wvp::platform::Combine(result, rest)), ...);
        return result;
    }
};

}
