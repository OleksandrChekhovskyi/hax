/* SPDX-License-Identifier: MIT */
#ifndef HAX_SYSTEM_WIN32_INCLUDE_SYS_WAIT_H
#define HAX_SYSTEM_WIN32_INCLUDE_SYS_WAIT_H

#include <errno.h>
#include <unistd.h>

#define WNOHANG             1
#define WIFEXITED(status)   ((status) >= 0 && (status) < 256)
#define WEXITSTATUS(status) (status)
#define WIFSIGNALED(status) ((status) >= 256)
#define WTERMSIG(status)    ((status) - 256)

#endif /* HAX_SYSTEM_WIN32_INCLUDE_SYS_WAIT_H */
