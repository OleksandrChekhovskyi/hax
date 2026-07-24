/* SPDX-License-Identifier: MIT */
#include "system/tempfiles.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"
#include "system/path.h"
#include "text/utf8.h"

static char **tracked;
static size_t tracked_n;
static size_t tracked_cap;
static int atexit_registered;

/* The per-process container directory, created on first use. `dir_base`
 * remembers which $TMPDIR it was created under so a base change starts
 * a fresh directory rather than filing under the stale one; superseded
 * directories park in `old_dirs` until cleanup can rmdir them (their
 * tracked files may still be live when the switch happens). */
static char *dir_path;
static char *dir_base;
static unsigned dir_seq;
static char **old_dirs;
static size_t old_dirs_n;
static size_t old_dirs_cap;

void tempfiles_cleanup(void)
{
    for (size_t i = 0; i < tracked_n; i++) {
        if (tracked[i]) {
            unlink(tracked[i]);
            free(tracked[i]);
        }
    }
    tracked_n = 0;
    for (size_t i = old_dirs_n; i-- > 0;) {
        if (rmdir(old_dirs[i]) == 0 || errno == ENOENT) {
            free(old_dirs[i]);
            old_dirs[i] = old_dirs[--old_dirs_n];
        }
    }
    if (dir_path) {
        /* Empty now unless a caller holds an untracked survivor inside;
         * then the rmdir fails and we keep the handle so later creates
         * reuse the directory instead of stranding it. ENOENT means a
         * tmp reaper removed it underneath us: drop the handle so the
         * next create starts a fresh directory rather than failing. */
        if (rmdir(dir_path) == 0 || errno == ENOENT) {
            free(dir_path);
            free(dir_base);
            dir_path = NULL;
            dir_base = NULL;
        }
    }
}

static void track(const char *path)
{
    if (!atexit_registered) {
        atexit(tempfiles_cleanup);
        atexit_registered = 1;
    }
    if (tracked_n == tracked_cap) {
        tracked_cap = tracked_cap ? tracked_cap * 2 : 4;
        tracked = xrealloc(tracked, tracked_cap * sizeof(*tracked));
    }
    tracked[tracked_n++] = xstrdup(path);
}

void tempfile_untrack(const char *path)
{
    for (size_t i = 0; i < tracked_n; i++) {
        if (tracked[i] && strcmp(tracked[i], path) == 0) {
            free(tracked[i]);
            /* Order doesn't matter for cleanup, so swap-with-last is the
             * cheapest removal. */
            tracked_n--;
            tracked[i] = tracked[tracked_n];
            tracked[tracked_n] = NULL;
            return;
        }
    }
}

static int is_valid_utf8(const char *s, size_t len)
{
    size_t i = 0;
    while (i < len) {
        int sl = utf8_seq_len((unsigned char)s[i]);
        if (sl <= 0 || i + (size_t)sl > len || !utf8_seq_valid(s + i, sl))
            return 0;
        i += (size_t)sl;
    }
    return 1;
}

static const char *ensure_dir(void)
{
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp || !is_valid_utf8(tmp, strlen(tmp)))
        tmp = "/tmp";
    if (dir_path && strcmp(dir_base, tmp) == 0)
        return dir_path;

    char *tmpl = path_join(tmp, "hax-XXXXXX");
    if (!mkdtemp(tmpl)) {
        free(tmpl);
        return NULL;
    }
    /* Tracking is by full path, so files under a prior directory keep
     * getting reclaimed across the switch; the directory itself parks
     * in old_dirs for cleanup to reap. */
    if (dir_path) {
        if (old_dirs_n == old_dirs_cap) {
            old_dirs_cap = old_dirs_cap ? old_dirs_cap * 2 : 4;
            old_dirs = xrealloc(old_dirs, old_dirs_cap * sizeof(*old_dirs));
        }
        old_dirs[old_dirs_n++] = dir_path;
    }
    free(dir_base);
    dir_path = tmpl;
    dir_base = xstrdup(tmp);
    return dir_path;
}

int tempfile_create(const char *prefix, const char *suffix, char **out_path)
{
    *out_path = NULL;
    const char *dir = ensure_dir();
    if (!dir)
        return -1;
    /* mkdtemp made the directory 0700 and unguessable, so entries need
     * no randomness of their own — a plain sequence number keeps names
     * short and readable in the prompt marker. */
    for (int tries = 0; tries < 100; tries++) {
        char *path = xasprintf("%s/%s%u%s", dir, prefix, ++dir_seq, suffix);
        int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd >= 0) {
            track(path);
            *out_path = path;
            return fd;
        }
        int open_errno = errno; /* free() may clobber errno */
        free(path);
        if (open_errno == ENOENT) {
            /* The container was reaped underneath us (a tmp cleaner on
             * a long-lived session): drop the cached handle and retry
             * in a fresh directory. */
            free(dir_path);
            free(dir_base);
            dir_path = NULL;
            dir_base = NULL;
            dir = ensure_dir();
            if (!dir)
                return -1;
            continue;
        }
        if (open_errno != EEXIST)
            return -1;
    }
    return -1;
}
