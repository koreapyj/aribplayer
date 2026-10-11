#pragma once
// OS file primitives used by the settings, MRU, playlist and M3U code. Implemented once per platform in
// src/platform/<os>/FileIo.cpp; everything above this header is platform-neutral.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wvp::fileio {

/// Native separator for paths the app builds itself ('\\' on Windows, '/' elsewhere).
extern const wchar_t kSep;

bool ReadAllBytes(const std::wstring& path, std::string& out);
std::optional<std::uint64_t> FileSize(const std::wstring& path);
bool IsRegularFile(const std::wstring& path);
bool EnsureDirectory(const std::wstring& dir);
/// Writes path + ".tmp" then replaces path atomically (MoveFileEx REPLACE_EXISTING|WRITE_THROUGH / rename). Creates the parent directory.
/// durable = false skips the flush: still an atomic replace (old or new content, never a torn file), but a power loss
/// may lose the last write. For frequent, non-critical state written on the UI thread.
bool AtomicWriteFile(const std::wstring& path, std::string_view bytes, bool durable = true);
/// Fails when `to` already exists.
bool RenameNoReplace(const std::wstring& from, const std::wstring& to);
bool RemoveFile(const std::wstring& path);

struct DirEntry {
    std::wstring name;        // file name without directory
    std::int64_t modifiedMs;  // last write time, Unix ms
};
/// Regular files directly inside dir (no subdirectories); empty when dir cannot be read.
std::vector<DirEntry> ListFiles(const std::wstring& dir);

/// Absolute, normalized form of a local path (GetFullPathName semantics); returns p unchanged on failure.
std::wstring FullPath(const std::wstring& p);

/// Per-user data directory: %LOCALAPPDATA%\aribplayer, ~/Library/Application Support/aribplayer, or $XDG_DATA_HOME/aribplayer.
std::wstring LocalAppDataApp();

}
