/* SPDX-License-Identifier: MIT */
#include "system/fd.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <unistd.h>

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
