/* SPDX-License-Identifier: MIT */
#ifndef HAX_WIN32_SYS_WAIT_H
#define HAX_WIN32_SYS_WAIT_H

#include <errno.h>
#include <unistd.h>

#define WNOHANG             1
#define WIFEXITED(status)   ((status) >= 0 && (status) < 256)
#define WEXITSTATUS(status) (status)
#define WIFSIGNALED(status) ((status) >= 256)
#define WTERMSIG(status)    ((status) - 256)

pid_t waitpid(pid_t pid, int *status, int options);

#endif /* HAX_WIN32_SYS_WAIT_H */
