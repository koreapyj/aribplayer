#pragma once
// Operating system services of the core: files, the per-user data folder, path syntax and system-dependent text.
// Every string is UTF-8. Paths use the native syntax: drive letters, UNC roots and '\' on Windows; '/' elsewhere.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wvp::platform {

enum class ReadResult { Ok, Failed, TooLarge };

/// Reads a whole file (other processes may keep it open for reading, writing or deleting). TooLarge (nothing read) when
/// the file is bigger than `limit` bytes.
ReadResult ReadAllBytes(const std::string& path, std::string& out, std::uint64_t limit);
/// Creates the directory and its missing parents. True when it exists afterwards.
bool EnsureDirectory(const std::string& dir);
/// Writes path + ".tmp", then renames it over path; creates the parent directory. durable = false skips the flush and the
/// write-through rename: still an atomic replace (old or new content, never a torn file), but a power loss may lose the
/// last write. For frequent, non-critical state written on the UI thread.
bool AtomicWriteFile(const std::string& path, std::string_view bytes, bool durable = true);
/// True for an existing file that is not a directory.
bool Exists(const std::string& path);
bool RemoveFile(const std::string& path);
/// Fails when `to` exists (a rename that only changes the case of the name succeeds).
bool RenameNoReplace(const std::string& from, const std::string& to);

struct FileEntry {
    std::string name;              // file name with extension
    std::int64_t modifiedMs = 0;   // last write time, Unix epoch ms
};
/// Files (not directories) directly in `dir` whose name ends in `ext` (ASCII case-insensitive, e.g. ".m3u8").
std::vector<FileEntry> ListFiles(const std::string& dir, std::string_view ext);

/// Per-user application data folder: %LOCALAPPDATA%\aribplayer on Windows, ~/Library/Application Support/aribplayer
/// on macOS, $XDG_DATA_HOME/aribplayer (default ~/.local/share/aribplayer) elsewhere. Not created.
std::string AppDataDir();

/// Absolute, normalized form of a path relative to the current directory; `p` itself when that fails.
std::string FullPath(const std::string& p);
/// Everything before the last separator ("" when there is none or `p` is a root).
std::string DirectoryName(const std::string& p);
std::string PathRoot(const std::string& p);
/// `b` when it is rooted, else `a` + separator + `b`.
std::string Combine(const std::string& a, const std::string& b);
/// Extension with the dot, not lowercased ("" when none).
std::string Extension(const std::string& p);
/// True for a path with a root that does not depend on the current drive or directory.
bool IsAbsolutePath(std::string_view p);
/// A typed or dropped local path in native form (separators normalized).
std::string NativePath(std::string_view p);
/// Local path of a file URI ("file:..."); the path part is percent-decoded.
std::string FileUriToPath(std::string_view uri);

/// Case-insensitive comparison of file names, as the file system compares them.
bool NamesEqualIgnoreCase(std::string_view a, std::string_view b);
/// Lowercase mapping of one code point, as used for the natural sort order.
char32_t FoldCase(char32_t c);
/// Text in the system's legacy (ANSI) code page as UTF-8; nullopt where there is none or the bytes do not decode.
std::optional<std::string> DecodeSystemCodePage(std::string_view bytes);

/// The proxy the system's manual proxy settings choose for a request to `host` over `scheme` ("http" / "https"), as a
/// curl proxy string; "" = connect directly. Automatic detection and configuration scripts are not used. nullopt where
/// the system keeps no proxy settings of its own (curl then reads http_proxy / https_proxy / no_proxy).
std::optional<std::string> SystemProxyFor(std::string_view scheme, std::string_view host);

}
