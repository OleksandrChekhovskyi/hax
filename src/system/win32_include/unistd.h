/* SPDX-License-Identifier: MIT */
#ifndef HAX_SYSTEM_WIN32_INCLUDE_UNISTD_H
#define HAX_SYSTEM_WIN32_INCLUDE_UNISTD_H

#include <io.h>
#include <process.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#ifndef STDIN_FILENO
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#endif
#ifndef F_OK
#define F_OK 0
#define X_OK 0
#define W_OK 2
#define R_OK 4
#endif
/* Keep compatibility flags outside UCRT's _O_* range so hax_open can enforce them before
 * handing the native handle to the CRT. */
#ifndef O_CLOEXEC
#define O_CLOEXEC 0x00100000
#endif
#ifndef O_NOCTTY
#define O_NOCTTY 0x00200000
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x00400000
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0x00800000
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0x01000000
#endif
#ifndef SSIZE_MAX
#define SSIZE_MAX INT_MAX
#endif

typedef intptr_t ssize_t;
typedef int mode_t;
typedef long off_t;

#define close     _close
#define dup       _dup
#define dup2      _dup2
#define fileno    _fileno
#define getpid    _getpid
#define isatty    hax_isatty
#define lseek     _lseek
#define read      _read
#define write     _write
#define fdopen    _fdopen
#define ftruncate _chsize_s
#define fsync     _commit
#define fchmod    hax_fchmod
#define pread     hax_pread

int hax_fchmod(int fd, mode_t mode);
ssize_t hax_pread(int fd, void *buffer, size_t count, off_t offset);
int hax_isatty(int fd);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
void hax_sleep_ms(long milliseconds);

#endif /* HAX_SYSTEM_WIN32_INCLUDE_UNISTD_H */
