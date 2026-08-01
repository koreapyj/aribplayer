#pragma once

#include <cstdint>
#include <memory>

extern "C" {
#include <libavformat/avio.h>
}

namespace aribplayer {

// Bridges an Android ParcelFileDescriptor/raw file descriptor into FFmpeg. The
// supplied descriptor is already a caller-owned duplicate; FdAvio takes
// ownership and closes it when destroyed.
class FdAvio final {
public:
    static constexpr int kBufferSize = 128 * 1024;

    static std::unique_ptr<FdAvio> Create(int owned_fd);
    ~FdAvio();

    FdAvio(const FdAvio&) = delete;
    FdAvio& operator=(const FdAvio&) = delete;

    AVIOContext* context() const { return context_; }
    bool is_seekable() const { return seekable_; }
    int64_t size() const;

private:
    explicit FdAvio(int owned_fd);
    bool Initialize();
    bool ProbeSeekable();

    static int Read(void* opaque, uint8_t* buffer, int buffer_size);
    static int64_t Seek(void* opaque, int64_t offset, int whence);
    int ReadImpl(uint8_t* buffer, int buffer_size);
    int64_t SeekImpl(int64_t offset, int whence);

    int fd_ = -1;
    AVIOContext* context_ = nullptr;
    bool seekable_ = false;
};

}  // namespace aribplayer
