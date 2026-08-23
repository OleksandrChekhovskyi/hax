/* SPDX-License-Identifier: MIT */
#ifndef HAX_WIN32_SYS_FILE_H
#define HAX_WIN32_SYS_FILE_H

#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8
int flock(int fd, int operation);

#endif /* HAX_WIN32_SYS_FILE_H */
