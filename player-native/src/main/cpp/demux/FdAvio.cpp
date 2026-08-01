#include "demux/FdAvio.h"

#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

namespace aribplayer {

std::unique_ptr<FdAvio> FdAvio::Create(int owned_fd) {
    if (owned_fd < 0) {
        return nullptr;
    }
    // Ownership is transferred even if AVIO initialization fails. JNI has
    // already duplicated the Java descriptor before the open call returns.
    std::unique_ptr<FdAvio> avio(new FdAvio(owned_fd));
    if (!avio->Initialize()) {
        return nullptr;
    }
    return avio;
}

FdAvio::FdAvio(int owned_fd) : fd_(owned_fd) {}

FdAvio::~FdAvio() {
    // avio_context_free also releases the av_malloc buffer passed to
    // avio_alloc_context.
    avio_context_free(&context_);
    if (fd_ >= 0) {
        close(fd_);
    }
}

bool FdAvio::Initialize() {
    uint8_t* buffer = static_cast<uint8_t*>(av_malloc(kBufferSize));
    if (buffer == nullptr) {
        return false;
    }
    context_ = avio_alloc_context(buffer, kBufferSize, 0, this, &FdAvio::Read,
                                  nullptr, &FdAvio::Seek);
    if (context_ == nullptr) {
        av_free(buffer);
        return false;
    }

    seekable_ = ProbeSeekable();
    context_->seekable = seekable_ ? AVIO_SEEKABLE_NORMAL : 0;
    return true;
}

bool FdAvio::ProbeSeekable() {
    errno = 0;
    const off_t position = lseek(fd_, 0, SEEK_CUR);
    return position != static_cast<off_t>(-1);
}

int64_t FdAvio::size() const {
    struct stat status {};
    if (fstat(fd_, &status) == 0 && S_ISREG(status.st_mode)) {
        return static_cast<int64_t>(status.st_size);
    }
    if (!seekable_) {
        return AVERROR(ESPIPE);
    }

    const off_t current = lseek(fd_, 0, SEEK_CUR);
    if (current == static_cast<off_t>(-1)) {
        return AVERROR(errno);
    }
    const off_t end = lseek(fd_, 0, SEEK_END);
    const int seek_error = errno;
    if (lseek(fd_, current, SEEK_SET) == static_cast<off_t>(-1)) {
        return AVERROR(errno);
    }
    return end == static_cast<off_t>(-1) ? AVERROR(seek_error)
                                          : static_cast<int64_t>(end);
}

int FdAvio::Read(void* opaque, uint8_t* buffer, int buffer_size) {
    return static_cast<FdAvio*>(opaque)->ReadImpl(buffer, buffer_size);
}

int FdAvio::ReadImpl(uint8_t* buffer, int buffer_size) {
    if (buffer == nullptr || buffer_size <= 0) {
        return AVERROR(EINVAL);
    }
    ssize_t bytes_read;
    do {
        bytes_read = read(fd_, buffer, static_cast<size_t>(buffer_size));
    } while (bytes_read < 0 && errno == EINTR);

    if (bytes_read == 0) {
        return AVERROR_EOF;
    }
    return bytes_read < 0 ? AVERROR(errno) : static_cast<int>(bytes_read);
}

int64_t FdAvio::Seek(void* opaque, int64_t offset, int whence) {
    return static_cast<FdAvio*>(opaque)->SeekImpl(offset, whence);
}

int64_t FdAvio::SeekImpl(int64_t offset, int whence) {
    if ((whence & AVSEEK_SIZE) != 0) {
        return size();
    }
    if (!seekable_) {
        return AVERROR(ESPIPE);
    }

    // AVSEEK_FORCE is an FFmpeg hint, not a POSIX whence value.
    const int posix_whence = whence & ~AVSEEK_FORCE;
    if (posix_whence != SEEK_SET && posix_whence != SEEK_CUR &&
        posix_whence != SEEK_END) {
        return AVERROR(EINVAL);
    }

    off_t result;
    do {
        result = lseek(fd_, static_cast<off_t>(offset), posix_whence);
    } while (result == static_cast<off_t>(-1) && errno == EINTR);
    return result == static_cast<off_t>(-1) ? AVERROR(errno)
                                             : static_cast<int64_t>(result);
}

}  // namespace aribplayer
