/* SPDX-License-Identifier: MIT */
#include "fs.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "diff.h"
#include "path.h"
#include "util.h"

/* mkdir -p: create path and any missing intermediate components. EEXIST is
 * treated as success — the caller doesn't care if a dir is new or old. */
int fs_mkdir_p(const char *path)
{
    if (!path || !*path)
        return 0;
    char *p = xstrdup(path);
    size_t len = strlen(p);
    while (len > 1 && p[len - 1] == '/')
        p[--len] = '\0';

    for (size_t i = 1; i < len; i++) {
        if (p[i] == '/') {
            p[i] = '\0';
            if (mkdir(p, 0755) < 0 && errno != EEXIST) {
                int saved = errno;
                free(p);
                errno = saved;
                return -1;
            }
            p[i] = '/';
        }
    }
    if (mkdir(p, 0755) < 0 && errno != EEXIST) {
        int saved = errno;
        free(p);
        errno = saved;
        return -1;
    }
    free(p);
    return 0;
}

static char *parent_dir_of(const char *path)
{
    /* dirname() may modify its argument and may return a static buffer —
     * always copy the result before touching the duplicate again. */
    char *dup = xstrdup(path);
    const char *p = dirname(dup);
    char *copy = xstrdup((p && *p) ? p : ".");
    free(dup);
    return copy;
}

/* Walk the symlink chain at `path` and return where its ultimate target
 * lives — without requiring that target to exist. realpath(3) refuses
 * dangling chains, but write/edit are documented to create missing files,
 * so for `link -> real` we want to land on `real`'s path even when `real`
 * doesn't exist yet. Stops on the first non-symlink (existing or not),
 * resolving relative link targets against the link's own directory.
 * Capped at MAX_HOPS to break loops; returns NULL with errno set on
 * hard failure (readlink error, loop, etc.). */
static char *resolve_link_target(const char *path)
{
    enum { MAX_HOPS = 32 };
    char *current = xstrdup(path);
    for (int hops = 0; hops < MAX_HOPS; hops++) {
        struct stat lst;
        if (lstat(current, &lst) < 0) {
            /* Final component is absent — the "create new file" case.
             * Hand back the path so the caller can stage a tempfile
             * next to it. Anything other than ENOENT is a real error. */
            if (errno == ENOENT)
                return current;
            free(current);
            return NULL;
        }
        if (!S_ISLNK(lst.st_mode))
            return current;

        char buf[4096];
        ssize_t n = readlink(current, buf, sizeof(buf) - 1);
        if (n < 0) {
            free(current);
            return NULL;
        }
        buf[n] = '\0';

        char *next;
        if (buf[0] == '/') {
            next = xstrdup(buf);
        } else {
            char *parent = parent_dir_of(current);
            next = path_join(parent, buf);
            free(parent);
        }
        free(current);
        current = next;
    }
    free(current);
    errno = ELOOP;
    return NULL;
}

char *fs_write_with_diff(const char *path, const char *content, size_t content_len, char **errmsg,
                         int *out_was_new)
{
    char *target = NULL, *old = NULL, *parent = NULL, *tmp = NULL, *diff = NULL;
    char *a_label = NULL, *b_label = NULL;
    int fd = -1;
    int file_existed = 0;
    int tmp_created = 0; /* mkstemp succeeded — there's a file at tmp to clean up */
    int committed = 0;   /* rename succeeded — tmp is gone from disk */
    int ok = 0;
    size_t old_len = 0;
    mode_t mode = 0;
    struct stat st;

    *errmsg = NULL;
    /* Default to "no", overwritten on the success exit. Any failure path
     * goes through `goto out` with this still 0 — callers must check the
     * NULL return / errmsg before consulting was_new, but at least they
     * won't see a stale 1 from an aborted create attempt. */
    if (out_was_new)
        *out_was_new = 0;

    /* If `path` is a symlink, resolve it so we update the target file's
     * contents and leave the link intact — matches what vim/emacs/etc do.
     * Replacing the symlink with a regular file would be surprising for
     * the common ~/.config-style "symlink into a repo" setup. The
     * resolver tolerates dangling chains so a `link -> new` pattern can
     * still create `new`. Diff labels keep the model-supplied path so the
     * user sees what they asked for, not an internal canonicalization. */
    target = resolve_link_target(path);
    if (!target) {
        *errmsg = xasprintf("resolving %s: %s", path, strerror(errno));
        goto out;
    }

    if (stat(target, &st) == 0) {
        /* Refuse FIFOs, sockets, devices, directories upfront. The
         * eventual rename would replace the special node with a plain
         * file, which is surprising. slurp_file's own guard would also
         * reject these now, but a tool-specific "not a regular file"
         * error gives the model a much clearer hint than the
         * "Invalid argument" it would otherwise see. */
        if (!S_ISREG(st.st_mode)) {
            *errmsg = xasprintf("%s exists but is not a regular file", target);
            goto out;
        }
        file_existed = 1;
        /* 07777 (not 0777) preserves setuid/setgid/sticky bits on
         * existing files. Standard "owned helper" patterns like 04755
         * would otherwise silently lose their elevation through a
         * write/edit. */
        mode = st.st_mode & 07777;
        old = slurp_file(target, &old_len);
        if (!old) {
            *errmsg = xasprintf("error reading %s: %s", target, strerror(errno));
            goto out;
        }
    } else if (errno == ENOENT) {
        /* Genuinely new file. Anything else (EACCES, EOVERFLOW, EIO,
         * ELOOP, …) means the file likely exists but we couldn't
         * inspect it — falling into the create path would diff against
         * /dev/null and rename over real content. Bail with the errno
         * instead. */
        mode_t um = umask(0);
        umask(um);
        mode = 0666 & ~um;
    } else {
        *errmsg = xasprintf("stat %s: %s", target, strerror(errno));
        goto out;
    }

    parent = parent_dir_of(target);
    if (fs_mkdir_p(parent) < 0) {
        *errmsg = xasprintf("creating %s: %s", parent, strerror(errno));
        goto out;
    }

    tmp = path_join(parent, ".hax-write-XXXXXX");
    fd = mkstemp(tmp);
    if (fd < 0) {
        *errmsg = xasprintf("mkstemp: %s", strerror(errno));
        goto out;
    }
    tmp_created = 1;
    if (write_all(fd, content, content_len) < 0) {
        *errmsg = xasprintf("write %s: %s", tmp, strerror(errno));
        goto out;
    }
    /* fchmod must happen *after* write: Linux's write(2) clears S_ISUID
     * and S_ISGID when an unprivileged process writes to a file, so a
     * chmod beforehand would silently lose those bits. Best-effort —
     * mkstemp's 0600 is the fallback if this somehow fails on a weird
     * filesystem. */
    (void)fchmod(fd, mode);
    /* fsync forces writeback so delayed-allocation failures (ENOSPC, EIO,
     * EDQUOT on ext4 et al.) surface here rather than silently corrupting
     * the file we're about to rename into place. close() is checked too —
     * NFS in particular can report errors only at close. */
    if (fsync(fd) < 0) {
        *errmsg = xasprintf("fsync %s: %s", tmp, strerror(errno));
        goto out;
    }
    if (close(fd) < 0) {
        fd = -1; /* close failure leaves fd unspecified; don't double-close */
        *errmsg = xasprintf("close %s: %s", tmp, strerror(errno));
        goto out;
    }
    fd = -1;

    a_label = file_existed ? xasprintf("a/%s", path) : xstrdup("/dev/null");
    b_label = xasprintf("b/%s", path);
    diff = make_unified_diff(file_existed ? old : "", file_existed ? old_len : 0, content,
                             content_len, a_label, b_label);
    if (!diff) {
        *errmsg = xstrdup("diff(1) failed");
        goto out;
    }

    /* Creating an empty file would otherwise yield "" — visually
     * identical to "no changes" and confusing for the model writing
     * sentinel files like __init__.py or .keep. Synthesize a minimal
     * headers-only diff so both UI and model see "file created at path,
     * no content". */
    if (!file_existed && !*diff) {
        free(diff);
        diff = xasprintf("--- /dev/null\n+++ b/%s\n", path);
    }

    /* Byte-identical content on an existing file: skip the rename so the
     * file's inode, mtime, ownership, and any hard links are preserved.
     * The unwind below unlinks tmp. For a *missing* file with empty
     * content, an empty diff still has to land on disk — otherwise
     * "create empty file" silently no-ops. */
    if (file_existed && !*diff) {
        ok = 1;
        goto out;
    }

    if (rename(tmp, target) < 0) {
        *errmsg = xasprintf("rename to %s: %s", target, strerror(errno));
        goto out;
    }
    committed = 1;
    ok = 1;

out:
    if (fd >= 0)
        close(fd);
    if (tmp_created && !committed)
        unlink(tmp);
    free(a_label);
    free(b_label);
    free(tmp);
    free(parent);
    free(old);
    free(target);
    if (!ok) {
        free(diff);
        return NULL;
    }
    /* Success — file_existed captures the pre-call state. The byte-
     * identical short-circuit also lands here with file_existed=1, so
     * the !file_existed branch covers exactly the create cases. */
    if (out_was_new)
        *out_was_new = !file_existed;
    return diff;
}
