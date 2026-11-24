/* SPDX-License-Identifier: MIT */
#include "system/fd.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stddef.h>
#include <unistd.h>

#include "buf.h"

int fd_write_all(int fd, const void *data, size_t length)
{
    const char *cursor = data;
    while (length > 0) {
        size_t request = length > (size_t)SSIZE_MAX ? (size_t)SSIZE_MAX : length;
        ssize_t written = write(fd, cursor, request);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (written == 0) {
            errno = EIO;
            return -1;
        }
        cursor += written;
        length -= (size_t)written;
    }
    return 0;
}

static ssize_t read_retry(int fd, void *data, size_t length, enum fd_read_wait wait)
{
    for (;;) {
        ssize_t bytes_read = read(fd, data, length);
        if (bytes_read >= 0)
            return bytes_read;
        if (errno == EINTR)
            continue;
        if (wait != FD_READ_WAIT || (errno != EAGAIN && errno != EWOULDBLOCK))
            return -1;
        if (fd_wait_readable(fd, -1) < 0)
            return -1;
    }
}

char *fd_read_all(int fd, size_t cap, enum fd_read_wait wait, size_t *out_len, int *out_truncated)
{
    int saved_errno;
    int truncated = 0;
    struct buf contents;
    buf_init(&contents);
    char chunk[8192];
    while (contents.len < cap) {
        size_t remaining = cap - contents.len;
        size_t request = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
        ssize_t bytes_read = read_retry(fd, chunk, request, wait);
        if (bytes_read < 0)
            goto error;
        if (bytes_read == 0)
            break;
        buf_append(&contents, chunk, (size_t)bytes_read);
    }

    if (out_truncated && contents.len == cap) {
        char extra;
        ssize_t bytes_read = read_retry(fd, &extra, 1, wait);
        if (bytes_read < 0)
            goto error;
        truncated = bytes_read > 0;
    }

    if (out_len)
        *out_len = contents.len;
    if (out_truncated)
        *out_truncated = truncated;
    return buf_steal(&contents);

error:
    saved_errno = errno;
    buf_free(&contents);
    errno = saved_errno;
    return NULL;
}

int fd_wait_readable(int fd, int timeout_ms)
{
    /* poll() skips negative descriptors rather than flagging them POLLNVAL. */
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    struct pollfd readable = {.fd = fd, .events = POLLIN};
    int ready;
    do {
        ready = poll(&readable, 1, timeout_ms);
    } while (ready < 0 && errno == EINTR);
    if (ready > 0 && (readable.revents & POLLNVAL)) {
        errno = EBADF;
        return -1;
    }
    return ready > 0 ? 1 : ready;
}
