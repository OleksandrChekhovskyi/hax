/* SPDX-License-Identifier: MIT */
#ifndef HAX_SYSTEM_FD_H
#define HAX_SYSTEM_FD_H

#include <stddef.h>

/* Write exactly length bytes, retrying interrupted and short writes. Returns 0 on success or -1
 * with errno set. */
int fd_write_all(int fd, const void *data, size_t length);

#endif /* HAX_SYSTEM_FD_H */
