/* SPDX-License-Identifier: MIT */
#ifndef HAX_SYSTEM_FD_H
#define HAX_SYSTEM_FD_H

#include <stddef.h>

/* Write exactly length bytes, retrying interrupted and short writes. Returns 0 on success or -1
 * with errno set. */
int fd_write_all(int fd, const void *data, size_t length);

/* What fd_read_all does when a nonblocking descriptor has no data yet. */
enum fd_read_wait {
    FD_READ_FAIL_FAST, /* fail with EAGAIN */
    FD_READ_WAIT,      /* wait for data or EOF */
};

/* Read until EOF or `cap` bytes (SIZE_MAX for no cap) into an allocated, NUL-terminated buffer,
 * retrying interrupted reads. On success, optional `out_len` receives the length excluding the
 * terminator. With `out_truncated`, reaching the cap reads, and so consumes, one more byte to
 * report whether more data exists. Returns NULL with errno set on failure. `fd` stays open
 * either way. */
char *fd_read_all(int fd, size_t cap, enum fd_read_wait wait, size_t *out_len, int *out_truncated);

/* Wait up to `timeout_ms` (negative: indefinitely) for `fd` to have data or reach EOF; hangup
 * counts as ready, so the next read sees EOF. An interrupted wait restarts the timeout. Returns 1
 * when ready, 0 on timeout, and -1 with errno set on failure, including EBADF for a descriptor
 * that is not open. */
int fd_wait_readable(int fd, int timeout_ms);

#endif /* HAX_SYSTEM_FD_H */
