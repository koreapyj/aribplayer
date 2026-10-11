#pragma once
// Scratch directory for file-based tests, removed with everything in it on destruction. Portable (std::filesystem).
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include "wvp/FileIo.h"

namespace wvptest {

struct TempDir {
    std::wstring path;

    explicit TempDir(const wchar_t* prefix = L"wvp_") {
        static int counter = 0;
        auto stamp = (unsigned long long)std::chrono::steady_clock::now().time_since_epoch().count();
        std::filesystem::path p = std::filesystem::temp_directory_path()
            / (std::wstring(prefix) + std::to_wstring(std::random_device{}()) + L"_" + std::to_wstring(stamp) + L"_" + std::to_wstring(counter++));
        std::filesystem::create_directories(p);
        path = p.wstring();
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(path), ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    /// path + separator + n (n may itself contain separators).
    std::wstring File(const std::wstring& n) const { return path + wvp::fileio::kSep + n; }
    std::wstring Sub(const std::wstring& n) const { return File(n); }

    static bool MakeDir(const std::wstring& dir) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(dir), ec);
        return !ec;
    }
    bool Write(const std::wstring& n, const std::vector<std::uint8_t>& data) const {
        return wvp::fileio::AtomicWriteFile(File(n), std::string_view((const char*)data.data(), data.size()), false);
    }
};

}
