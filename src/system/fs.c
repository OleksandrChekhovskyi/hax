/* SPDX-License-Identifier: MIT */
#include "system/fs.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "buf.h"
#include "xalloc.h"
#include "system/fd.h"
#include "system/path.h"
#include "text/diff.h"

int fs_mkdir_p(const char *path)
{
    if (!path || !*path)
        return 0;

    char *path_copy = xstrdup(path);
    size_t path_len = strlen(path_copy);
    int saved_errno;
    while (path_len > 1 && path_copy[path_len - 1] == '/')
        path_copy[--path_len] = '\0';

    for (size_t i = 1; i < path_len; i++) {
        if (path_copy[i] != '/')
            continue;
        path_copy[i] = '\0';
        if (mkdir(path_copy, 0755) < 0 && errno != EEXIST)
            goto error;
        path_copy[i] = '/';
    }

    if (mkdir(path_copy, 0755) < 0) {
        if (errno != EEXIST)
            goto error;

        /* EEXIST is success only when the final component resolves to a directory. */
        struct stat st;
        if (stat(path_copy, &st) < 0 || !S_ISDIR(st.st_mode)) {
            errno = ENOTDIR;
            goto error;
        }
    }

    free(path_copy);
    return 0;

error:
    saved_errno = errno;
    free(path_copy);
    errno = saved_errno;
    return -1;
}

static char *parent_dir(const char *path)
{
    /* dirname() may modify its argument and return storage owned by the C library. */
    char *path_copy = xstrdup(path);
    const char *parent = dirname(path_copy);
    char *result = xstrdup(parent && *parent ? parent : ".");
    free(path_copy);
    return result;
}

static char *read_symlink_target(const char *path)
{
    size_t capacity = 256;
    char *target = xmalloc(capacity + 1);

    for (;;) {
        ssize_t target_len = readlink(path, target, capacity);
        if (target_len < 0) {
            free(target);
            return NULL;
        }
        if ((size_t)target_len < capacity) {
            target[target_len] = '\0';
            return target;
        }
        if (capacity > SIZE_MAX / 2) {
            free(target);
            errno = ENAMETOOLONG;
            return NULL;
        }
        capacity *= 2;
        target = xrealloc(target, capacity + 1);
    }
}

char *fs_resolve_link_target(const char *path)
{
    enum { MAX_SYMLINK_HOPS = 32 };
    char *current_path = xstrdup(path);

    for (int hop = 0; hop < MAX_SYMLINK_HOPS; hop++) {
        struct stat st;
        if (lstat(current_path, &st) < 0) {
            if (errno == ENOENT)
                return current_path;
            free(current_path);
            return NULL;
        }
        if (!S_ISLNK(st.st_mode))
            return current_path;

        char *target = read_symlink_target(current_path);
        if (!target) {
            free(current_path);
            return NULL;
        }

        char *next_path;
        if (target[0] == '/') {
            next_path = target;
        } else {
            char *parent = parent_dir(current_path);
            next_path = path_join(parent, target);
            free(parent);
            free(target);
        }
        free(current_path);
        current_path = next_path;
    }

    free(current_path);
    errno = ELOOP;
    return NULL;
}

struct write_target {
    char *old_content;
    size_t old_content_len;
    mode_t mode;
    int existed;
};

static int inspect_write_target(const char *path, struct write_target *target, char **error)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        /* Special files may block on read and must not be replaced with regular files. */
        if (!S_ISREG(st.st_mode)) {
            *error = xasprintf("%s exists but is not a regular file", path);
            return -1;
        }

        target->existed = 1;
        target->mode = st.st_mode & 07777;
        target->old_content = fs_read_file(path, &target->old_content_len);
        if (!target->old_content) {
            *error = xasprintf("error reading %s: %s", path, strerror(errno));
            return -1;
        }
        return 0;
    }

    if (errno != ENOENT) {
        *error = xasprintf("stat %s: %s", path, strerror(errno));
        return -1;
    }

    mode_t mask = umask(0);
    umask(mask);
    target->mode = 0666 & ~mask;
    return 0;
}

static char *make_write_diff(const char *path, const char *old_content, size_t old_content_len,
                             const char *content, size_t content_len, int existed)
{
    int absolute_path = path[0] == '/';
    char *old_label = !existed        ? xstrdup("/dev/null")
                      : absolute_path ? xstrdup(path)
                                      : xasprintf("a/%s", path);
    char *new_label = absolute_path ? xstrdup(path) : xasprintf("b/%s", path);
    char *diff = make_unified_diff(existed ? old_content : "", existed ? old_content_len : 0,
                                   content, content_len, old_label, new_label);

    /* An empty new file still needs a visible creation result. */
    if (!existed && !*diff) {
        free(diff);
        diff = xasprintf("--- /dev/null\n+++ %s\n", new_label);
    }

    free(old_label);
    free(new_label);
    return diff;
}

static char *stage_write(const char *parent, const char *content, size_t content_len, mode_t mode,
                         char **error)
{
    char *temp_path = path_join(parent, ".hax-write-XXXXXX");
    int fd = mkstemp(temp_path);
    if (fd < 0) {
        *error = xasprintf("mkstemp: %s", strerror(errno));
        free(temp_path);
        return NULL;
    }

    if (fd_write_all(fd, content, content_len) < 0) {
        *error = xasprintf("write %s: %s", temp_path, strerror(errno));
        goto error;
    }

    /* write(2) may clear set-ID bits, so restore the destination mode afterward. */
    if (fchmod(fd, mode) < 0) {
        *error = xasprintf("chmod %s: %s", temp_path, strerror(errno));
        goto error;
    }
    /* Surface delayed-allocation failures before replacing the destination. */
    if (fsync(fd) < 0) {
        *error = xasprintf("fsync %s: %s", temp_path, strerror(errno));
        goto error;
    }
    if (close(fd) < 0) {
        fd = -1; /* A failed close leaves the descriptor state unspecified. */
        *error = xasprintf("close %s: %s", temp_path, strerror(errno));
        goto error;
    }
    return temp_path;

error:
    if (fd >= 0)
        close(fd);
    unlink(temp_path);
    free(temp_path);
    return NULL;
}

char *fs_write_with_diff(const char *path, const char *content, size_t content_len, char **error,
                         int *was_created)
{
    char *target_path = NULL;
    char *parent = NULL;
    char *temp_path = NULL;
    char *diff = NULL;
    struct write_target target = {0};

    *error = NULL;
    if (was_created)
        *was_created = 0;

    /* Write through symlinks, including dangling chains, rather than replacing the link itself. */
    target_path = fs_resolve_link_target(path);
    if (!target_path) {
        *error = xasprintf("resolving %s: %s", path, strerror(errno));
        goto out;
    }

    if (inspect_write_target(target_path, &target, error) < 0)
        goto out;

    diff = make_write_diff(path, target.old_content, target.old_content_len, content, content_len,
                           target.existed);

    /* Preserve inode identity, metadata, and hard links when the content is unchanged. */
    if (target.existed && !*diff)
        goto out;

    parent = parent_dir(target_path);
    if (fs_mkdir_p(parent) < 0) {
        *error = xasprintf("creating %s: %s", parent, strerror(errno));
        goto out;
    }

    temp_path = stage_write(parent, content, content_len, target.mode, error);
    if (!temp_path)
        goto out;
    if (rename(temp_path, target_path) < 0) {
        *error = xasprintf("rename to %s: %s", target_path, strerror(errno));
        unlink(temp_path);
        goto out;
    }

out:
    free(temp_path);
    free(parent);
    free(target.old_content);
    free(target_path);
    if (*error) {
        free(diff);
        return NULL;
    }
    if (was_created)
        *was_created = !target.existed;
    return diff;
}

static int is_executable_file(const char *path)
{
    struct stat st;
    /* Directories may pass access(X_OK), but cannot be executed as programs. */
    return access(path, X_OK) == 0 && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

char *fs_which(const char *name)
{
    if (!name || !*name)
        return NULL;
    if (strchr(name, '/'))
        return is_executable_file(name) ? xstrdup(name) : NULL;

    const char *path_env = getenv("PATH");
    if (!path_env)
        return NULL;

    const char *entry = path_env;
    for (;;) {
        const char *separator = strchr(entry, ':');
        size_t entry_len = separator ? (size_t)(separator - entry) : strlen(entry);
        if (entry_len > 0 && entry[0] == '/') {
            char *directory = xmalloc(entry_len + 1);
            memcpy(directory, entry, entry_len);
            directory[entry_len] = '\0';
            char *candidate = path_join(directory, name);
            free(directory);
            if (is_executable_file(candidate))
                return candidate;
            free(candidate);
        }
        if (!separator)
            return NULL;
        entry = separator + 1;
    }
}

char *fs_shell_head(const char *shell_cmd)
{
    if (!shell_cmd)
        return NULL;
    shell_cmd += strspn(shell_cmd, " \t");
    size_t head_len = strcspn(shell_cmd, " \t");
    if (head_len == 0)
        return NULL;
    if (strcspn(shell_cmd, "\"'`\\$;|&<>()=~*?[#") < head_len)
        return NULL;

    char *head = xmalloc(head_len + 1);
    memcpy(head, shell_cmd, head_len);
    head[head_len] = '\0';
    return head;
}

int fs_shell_head_resolves(const char *shell_cmd)
{
    if (!shell_cmd)
        return 0;
    shell_cmd += strspn(shell_cmd, " \t");
    if (*shell_cmd == '\0')
        return 0;

    char *head = fs_shell_head(shell_cmd);
    /* Quoting, expansion, assignments, redirection, or globs in the head defeat a plain PATH
     * lookup, so assume the shell can start such a command. */
    if (!head)
        return 1;
    char *path = fs_which(head);
    int resolves = path != NULL;
    free(path);
    free(head);
    return resolves;
}

static int regular_mode_or_error(mode_t mode)
{
    if (S_ISREG(mode))
        return 0;
    errno = S_ISDIR(mode) ? EISDIR : EINVAL;
    return -1;
}

int fs_check_regular(const char *path)
{
    struct stat status;
    if (stat(path, &status) < 0)
        return -1;
    return regular_mode_or_error(status.st_mode);
}

int fs_open_regular(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return -1;

    struct stat status;
    if (fstat(fd, &status) == 0 && regular_mode_or_error(status.st_mode) == 0)
        return fd;

    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return -1;
}

static ssize_t read_retry(int fd, void *data, size_t length)
{
    ssize_t bytes_read;
    do {
        bytes_read = read(fd, data, length);
    } while (bytes_read < 0 && errno == EINTR);
    return bytes_read;
}

char *fs_read_file(const char *path, size_t *out_len)
{
    int saved_errno;
    int fd = fs_open_regular(path);
    if (fd < 0)
        return NULL;

    struct buf contents;
    buf_init(&contents);
    char chunk[8192];
    for (;;) {
        ssize_t bytes_read = read_retry(fd, chunk, sizeof(chunk));
        if (bytes_read < 0)
            goto error;
        if (bytes_read == 0)
            break;
        buf_append(&contents, chunk, (size_t)bytes_read);
    }

    close(fd);
    if (out_len)
        *out_len = contents.len;
    return buf_steal(&contents);

error:
    saved_errno = errno;
    buf_free(&contents);
    close(fd);
    errno = saved_errno;
    return NULL;
}

char *fs_read_file_capped(const char *path, size_t cap, size_t *out_len, int *out_truncated)
{
    int saved_errno;
    int truncated = 0;
    int fd = fs_open_regular(path);
    if (fd < 0)
        return NULL;

    struct buf contents;
    buf_init(&contents);
    char chunk[8192];
    while (contents.len < cap) {
        size_t remaining = cap - contents.len;
        size_t request = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
        ssize_t bytes_read = read_retry(fd, chunk, request);
        if (bytes_read < 0)
            goto error;
        if (bytes_read == 0)
            break;
        buf_append(&contents, chunk, (size_t)bytes_read);
    }

    if (contents.len == cap) {
        char extra;
        ssize_t bytes_read = read_retry(fd, &extra, 1);
        if (bytes_read < 0)
            goto error;
        truncated = bytes_read > 0;
    }

    close(fd);
    if (out_len)
        *out_len = contents.len;
    if (out_truncated)
        *out_truncated = truncated;
    return buf_steal(&contents);

error:
    saved_errno = errno;
    buf_free(&contents);
    close(fd);
    errno = saved_errno;
    return NULL;
}
