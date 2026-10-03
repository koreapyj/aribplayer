#pragma once
// Classification of user input (paths, URLs, ffmpeg protocols).
#include <optional>
#include <string>
#include <string_view>

namespace wvp {

enum class MediaSourceKind { LocalPath, FileUri, Http, Https, Network, Empty };

struct MediaUriInfo {
    MediaSourceKind kind = MediaSourceKind::Empty;
    std::wstring original;                    // trimmed input
    std::optional<std::wstring> scheme;       // lowercase scheme, nullopt for plain paths
    std::optional<std::wstring> localPath;    // Windows path for LocalPath/FileUri
    std::wstring extension;                   // lowercase ext with dot ("" if none)

    bool IsRemote() const { return kind == MediaSourceKind::Http || kind == MediaSourceKind::Https || kind == MediaSourceKind::Network; }
    bool IsLocal() const { return kind == MediaSourceKind::LocalPath || kind == MediaSourceKind::FileUri; }
    bool IsHttp() const { return kind == MediaSourceKind::Http || kind == MediaSourceKind::Https; }
    bool IsUnc() const;
    bool HasPlaylistExtension() const;
    /// Local path for files / file URIs, otherwise the original URL.
    const std::wstring& FfmpegInput() const { return localPath ? *localPath : original; }
};

namespace MediaUri {
MediaUriInfo Parse(std::wstring_view input);
bool IsPlaylistExtension(std::wstring_view ext);
bool IsPlaylistContentType(std::wstring_view contentType);   // empty = false
}

// Windows path helpers with System.IO.Path semantics (exposed for the playlist code and tests).
namespace WinPath {
std::wstring GetDirectoryName(const std::wstring& p);   // "" when none
std::wstring GetPathRoot(const std::wstring& p);
std::wstring Combine(const std::wstring& a, const std::wstring& b);
std::wstring GetFullPath(const std::wstring& p);        // falls back to p on failure
std::wstring GetExtension(const std::wstring& p);       // not lowercased
std::wstring ToLower(std::wstring s);
}

}
