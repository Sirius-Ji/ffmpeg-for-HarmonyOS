#ifndef FF_FDIO_H
#define FF_FDIO_H

// 共用的 fd <-> AVIO 回调：decoder（读）和 transcoder（读 + 写）都用这一份。
#include <cerrno>
#include <cstdint>
#include <unistd.h>

extern "C" {
#include <libavformat/avformat.h>
}

namespace ffio {

static inline int ReadPacket(void *opaque, uint8_t *buf, int size) {
    const int fd = (int)(intptr_t)opaque;
    for (;;) {
        const ssize_t n = ::read(fd, buf, (size_t)size);
        if (n >= 0) return (int)n;
        if (errno != EINTR) return AVERROR(errno);
    }
}

static inline int64_t Seek(void *opaque, int64_t offset, int whence) {
    const int fd = (int)(intptr_t)opaque;
    if (whence == AVSEEK_SIZE) {
        const off_t cur = ::lseek(fd, 0, SEEK_CUR);
        const off_t end = ::lseek(fd, 0, SEEK_END);
        if (cur >= 0) ::lseek(fd, cur, SEEK_SET);
        return end;
    }
    whence &= ~AVSEEK_FORCE;
    int w = SEEK_SET;
    if (whence == SEEK_CUR) {
        w = SEEK_CUR;
    } else if (whence == SEEK_END) {
        w = SEEK_END;
    } else if (whence != SEEK_SET) {
        return AVERROR(EINVAL);
    }
    const off_t r = ::lseek(fd, (off_t)offset, w);
    return r < 0 ? AVERROR(errno) : (int64_t)r;
}

static inline int WritePacket(void *opaque, const uint8_t *buf, int size) {
    const int fd = (int)(intptr_t)opaque;
    for (;;) {
        const ssize_t n = ::write(fd, buf, (size_t)size);
        if (n >= 0) return (int)n;
        if (errno != EINTR) return AVERROR(errno);
    }
}

}  // namespace ffio

#endif  // FF_FDIO_H
