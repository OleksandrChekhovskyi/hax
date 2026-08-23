/* SPDX-License-Identifier: MIT */
#ifndef HAX_WIN32_POLL_H
#define HAX_WIN32_POLL_H

#include <windows.h>

#define POLLIN   0x0001
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020

struct pollfd {
    int fd;
    short events;
    short revents;
};

typedef unsigned long nfds_t;
int poll(struct pollfd *fds, nfds_t count, int timeout_ms);

#endif /* HAX_WIN32_POLL_H */
