/* SPDX-License-Identifier: MIT */
#ifndef HAX_WIN32_COMPAT_H
#define HAX_WIN32_COMPAT_H

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0a00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#if defined(_MSC_VER) && !defined(__clang__)
#define __attribute__(attributes)
#endif

/* bcrypt.h relies on the Win32 base types and annotations. */
#include <windows.h>
#if defined(_WIN32)
#include <bcrypt.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "unistd.h"

#ifndef PATH_MAX
#define PATH_MAX 32768
#endif
#ifndef NAME_MAX
#define NAME_MAX 255
#endif
#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_IFLNK
#define S_IFLNK 0120000
#endif
#ifndef S_ISLNK
#define S_ISLNK(mode) (((mode) & S_IFLNK) == S_IFLNK)
#endif
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
#endif
#ifndef SIGQUIT
#define SIGQUIT 3
#endif
#ifndef SIGHUP
#define SIGHUP 1
#endif
#ifndef SIGPIPE
#define SIGPIPE 13
#endif
#ifndef SIGCHLD
#define SIGCHLD 17
#endif
#ifndef SIGTSTP
#define SIGTSTP 20
#endif
#ifndef SIGKILL
#define SIGKILL 9
#endif
#ifndef SIG_BLOCK
#define SIG_BLOCK   0
#define SIG_SETMASK 1
#endif
#ifndef SA_RESTART
#define SA_RESTART 0
#endif
#ifndef UTIME_OMIT
#define UTIME_OMIT ((long)1073741822L)
#define AT_FDCWD   (-100)
#endif

#define strdup                     _strdup
#define strcasecmp                 _stricmp
#define strncasecmp                _strnicmp
#define strtok_r(str, delim, save) strtok_s((str), (delim), (save))

struct sigaction {
    void (*sa_handler)(int);
    int sa_flags;
    unsigned long sa_mask;
};
typedef unsigned long sigset_t;

static inline int sigemptyset(sigset_t *set)
{
    *set = 0;
    return 0;
}

static inline int sigfillset(sigset_t *set)
{
    *set = ~0UL;
    return 0;
}

static inline int sigaddset(sigset_t *set, int signal_number)
{
    if (signal_number >= 0 && signal_number < 32)
        *set |= 1UL << signal_number;
    return 0;
}

static inline int hax_signal_supported(int signal_number)
{
    return signal_number == SIGABRT || signal_number == SIGFPE || signal_number == SIGILL ||
           signal_number == SIGINT || signal_number == SIGSEGV || signal_number == SIGTERM;
}

static inline int sigaction(int signal_number, const struct sigaction *action,
                            struct sigaction *old_action)
{
    void (*previous)(int) = SIG_DFL;
    if (hax_signal_supported(signal_number)) {
        previous = signal(signal_number, action ? action->sa_handler : SIG_DFL);
        if (previous == SIG_ERR)
            return -1;
    }
    if (old_action) {
        memset(old_action, 0, sizeof(*old_action));
        old_action->sa_handler = previous;
    }
    return 0;
}

static inline int hax_raise(int signal_number)
{
    return hax_signal_supported(signal_number) ? raise(signal_number) : 0;
}

static inline int sigprocmask(int how, const sigset_t *set, sigset_t *old_set)
{
    (void)how;
    (void)set;
    if (old_set)
        *old_set = 0;
    return 0;
}

int clock_gettime(int clock_id, struct timespec *time_value);
int nanosleep(const struct timespec *request, struct timespec *remaining);
int hax_kill(pid_t pid, int signal_number);
ssize_t hax_getline(char **line, size_t *capacity, FILE *stream);
struct tm *hax_gmtime_r(const time_t *time_value, struct tm *result);
struct tm *hax_localtime_r(const time_t *time_value, struct tm *result);
ssize_t hax_readlink(const char *path, char *buffer, size_t capacity);
int futimens(int fd, const struct timespec times[2]);
int hax_pipe(int fds[2]);
int hax_fcntl(int fd, int command, ...);
int hax_wcwidth(wchar_t codepoint);

void win32_process_init(int *argc, char ***argv);
void win32_terminal_acquire(void);
void win32_terminal_release(void);

int hax_open(const char *path, int flags, ...);
FILE *hax_fopen(const char *path, const char *mode);
FILE *hax_freopen(const char *path, const char *mode, FILE *stream);
FILE *hax_open_memstream(char **buffer, size_t *length);
int hax_fclose(FILE *stream);
int hax_fflush(FILE *stream);
int hax_access(const char *path, int mode);
int hax_chdir(const char *path);
int hax_chmod(const char *path, int mode);
char *hax_getcwd(char *buffer, int size);
int hax_mkdir(const char *path);
int hax_rename(const char *old_path, const char *new_path);
int hax_remove(const char *path);
int hax_rmdir(const char *path);
int hax_stat(const char *path, struct stat *status);
int hax_lstat(const char *path, struct stat *status);
int hax_unlink(const char *path);
int hax_symlink(const char *target, const char *link_path);
int hax_utimensat(int directory_fd, const char *path, const struct timespec times[2], int flags);
char *hax_realpath(const char *path, char *resolved);
char *hax_search_path(const char *name);
int hax_mkstemp(char *path_template);
char *hax_mkdtemp(char *path_template);

#define open                hax_open
#define fopen               hax_fopen
#define freopen             hax_freopen
#define open_memstream      hax_open_memstream
#define fclose              hax_fclose
#define fflush              hax_fflush
#define access              hax_access
#define chdir               hax_chdir
#define chmod               hax_chmod
#define getcwd              hax_getcwd
#define kill                hax_kill
#define raise               hax_raise
#define getline             hax_getline
#define gmtime_r            hax_gmtime_r
#define localtime_r         hax_localtime_r
#define readlink            hax_readlink
#define pipe                hax_pipe
#define fcntl               hax_fcntl
#define wcwidth             hax_wcwidth
#define lstat(path, status) hax_lstat((path), (status))
#define mkdir(path, mode)   hax_mkdir(path)
#define mkdtemp             hax_mkdtemp
#define mkstemp             hax_mkstemp
#define realpath            hax_realpath
#define remove              hax_remove
#define rename              hax_rename
#define rmdir               hax_rmdir
#define stat(path, status)  hax_stat((path), (status))
#define unlink              hax_unlink
#define symlink             hax_symlink
#define utimensat           hax_utimensat

#ifndef F_SETFD
#define F_SETFD    1
#define F_GETFL    2
#define F_SETFL    3
#define FD_CLOEXEC 1
#endif

#endif /* HAX_WIN32_COMPAT_H */
