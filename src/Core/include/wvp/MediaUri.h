#pragma once
// Classification of user input (paths, URLs, ffmpeg protocols).
#include <optional>
#include <string>
#include <string_view>

namespace wvp {

enum class MediaSourceKind { LocalPath, FileUri, Http, Https, Network, Empty };

struct MediaUriInfo {
    MediaSourceKind kind = MediaSourceKind::Empty;
    std::string original;                     // trimmed input
    std::optional<std::string> scheme;        // lowercase scheme, nullopt for plain paths
    std::optional<std::string> localPath;     // native path for LocalPath/FileUri
    std::string extension;                    // lowercase ext with dot ("" if none)

    bool IsRemote() const { return kind == MediaSourceKind::Http || kind == MediaSourceKind::Https || kind == MediaSourceKind::Network; }
    bool IsLocal() const { return kind == MediaSourceKind::LocalPath || kind == MediaSourceKind::FileUri; }
    bool IsHttp() const { return kind == MediaSourceKind::Http || kind == MediaSourceKind::Https; }
    bool IsUnc() const;
    bool HasPlaylistExtension() const;
    /// Local path for files / file URIs, otherwise the original URL.
    const std::string& FfmpegInput() const { return localPath ? *localPath : original; }
};

namespace MediaUri {
MediaUriInfo Parse(std::string_view input);
bool IsPlaylistExtension(std::string_view ext);
bool IsPlaylistContentType(std::string_view contentType);   // empty = false
/// %XX escapes decoded to bytes (invalid UTF-8 in the result becomes U+FFFD).
std::string PercentDecode(std::string_view s);
}

}
