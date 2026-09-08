/* SPDX-License-Identifier: MIT */
#include "system/tempfiles.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "xalloc.h"
#include "system/clock.h"
#include "system/path.h"
#include "text/utf8.h"

#define TEMPFILE_MAX_ATTEMPTS 100

struct path_list {
    char **items;
    size_t count;
    size_t capacity;
};

static struct path_list tracked_files;
static struct path_list retired_dirs;
static char *active_dir;
static char *active_tmpdir;
static unsigned next_file_id;
static int cleanup_registered;

static void path_list_add(struct path_list *list, const char *path)
{
    if (list->count == list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 4;
        list->items = xrealloc(list->items, list->capacity * sizeof(*list->items));
    }
    list->items[list->count++] = xstrdup(path);
}

static void path_list_remove(struct path_list *list, size_t index)
{
    free(list->items[index]);
    list->count--;
    list->items[index] = list->items[list->count];
}

static void forget_active_dir(void)
{
    free(active_dir);
    free(active_tmpdir);
    active_dir = NULL;
    active_tmpdir = NULL;
}

void tempfiles_cleanup(void)
{
    for (size_t i = 0; i < tracked_files.count; i++) {
        unlink(tracked_files.items[i]);
        free(tracked_files.items[i]);
    }
    tracked_files.count = 0;

    for (size_t i = retired_dirs.count; i-- > 0;) {
        if (rmdir(retired_dirs.items[i]) == 0 || errno == ENOENT)
            path_list_remove(&retired_dirs, i);
    }

    if (active_dir && (rmdir(active_dir) == 0 || errno == ENOENT))
        forget_active_dir();
}

static void track_file(const char *path)
{
    if (!cleanup_registered) {
        atexit(tempfiles_cleanup);
        cleanup_registered = 1;
    }
    path_list_add(&tracked_files, path);
}

void tempfile_untrack(const char *path)
{
    for (size_t i = 0; i < tracked_files.count; i++) {
        if (strcmp(tracked_files.items[i], path) == 0) {
            path_list_remove(&tracked_files, i);
            return;
        }
    }
}

static int string_is_valid_utf8(const char *text)
{
    return utf8_is_valid(text, strlen(text));
}

static int name_fragment_is_valid(const char *fragment)
{
    return fragment && !strchr(fragment, '/') && string_is_valid_utf8(fragment);
}

static const char *ensure_temp_dir(void)
{
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || !*tmpdir || !string_is_valid_utf8(tmpdir))
        tmpdir = "/tmp";
    if (active_dir && strcmp(active_tmpdir, tmpdir) == 0)
        return active_dir;

    char *template = path_join(tmpdir, "hax-XXXXXX");
    if (!mkdtemp(template)) {
        int saved_errno = errno;
        free(template);
        errno = saved_errno;
        return NULL;
    }

    if (active_dir) {
        path_list_add(&retired_dirs, active_dir);
        free(active_dir);
    }
    free(active_tmpdir);
    active_dir = template;
    active_tmpdir = xstrdup(tmpdir);
    return active_dir;
}

int tempfile_create(const char *prefix, const char *suffix, char **path_out)
{
    *path_out = NULL;
    if (!name_fragment_is_valid(prefix) || !name_fragment_is_valid(suffix)) {
        errno = EINVAL;
        return -1;
    }

    const char *dir = ensure_temp_dir();
    if (!dir)
        return -1;

    /* The private directory supplies the randomness; sequential entry names stay readable. */
    for (int attempt = 0; attempt < TEMPFILE_MAX_ATTEMPTS; attempt++) {
        char *path = xasprintf("%s/%s%u%s", dir, prefix, ++next_file_id, suffix);
        int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd >= 0) {
            track_file(path);
            *path_out = path;
            return fd;
        }

        int open_errno = errno;
        free(path);
        if (open_errno == ENOENT) {
            /* A temporary-file reaper may remove the cached directory during long sessions. */
            forget_active_dir();
            dir = ensure_temp_dir();
            if (!dir)
                return -1;
            continue;
        }
        if (open_errno != EEXIST) {
            errno = open_errno;
            return -1;
        }
    }

    errno = EEXIST;
    return -1;
}

/* Liveness heartbeat (HTPR-6251): the agent worker kills a turn whose watched run dir goes
 * quiet, but a healthy turn can be silent for many minutes while a model stream or a tool
 * call runs. Ticks call this so the run dir keeps showing life. Deliberately silent on
 * failure: a diagnostic here would itself reset the worker's output-based idle timer and
 * hide the stall it is meant to reveal. */
#define HEARTBEAT_THROTTLE_MS 10000

static char *heartbeat_path;
static long long heartbeat_last_attempt_ms;

static int heartbeat_create(void)
{
    char *path = NULL;
    int fd = tempfile_create("heartbeat-", ".json", &path);
    if (fd < 0)
        return -1;
    char payload[32];
    int len = snprintf(payload, sizeof(payload), "{\"pid\":%ld}\n", (long)getpid());
    if (write(fd, payload, (size_t)len) < 0 && errno == EINTR) {
        /* Best effort only; the mtime is the signal, not the content. */
    }
    close(fd);
    free(heartbeat_path);
    heartbeat_path = path;
    return 0;
}

void tempfiles_touch_heartbeat(void)
{
    long long now = monotonic_ms();
    /* One attempt per throttle window even on failure: a permanently failing heartbeat must
     * not spin on create/touch, and a transient one must recover. */
    if (heartbeat_last_attempt_ms && now - heartbeat_last_attempt_ms < HEARTBEAT_THROTTLE_MS)
        return;
    heartbeat_last_attempt_ms = now;

    if (!heartbeat_path || (utimensat(AT_FDCWD, heartbeat_path, NULL, 0) != 0 &&
                            errno == ENOENT)) {
        /* First use, or a reaper removed the run dir (or the file): forget the cached
         * directory so tempfile_create makes a fresh one and re-creates the heartbeat. */
        free(heartbeat_path);
        heartbeat_path = NULL;
        forget_active_dir();
        if (heartbeat_create() != 0)
            return;
    }
    (void)utimensat(AT_FDCWD, heartbeat_path, NULL, 0);
}
