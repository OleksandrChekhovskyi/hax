/* SPDX-License-Identifier: MIT */
#include "cred_store.h"

#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/file.h>

#include "util.h"
#include "system/fs.h"
#include "system/path.h"

char *cred_store_file_path(void)
{
    return xdg_hax_state_path("auth.json");
}

static int ensure_parent_dir(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash)
        return 0;
    char *parent = xstrdup(path);
    parent[slash - path] = '\0';
    int result = fs_mkdir_p(parent);
    free(parent);
    return result;
}

/* Serialize store writes across hax processes. The lock lives in a sidecar file because the store
 * itself is replaced by rename, which would silently split lockers between inodes. Returns the
 * lock's fd, or -1 when the lock cannot be taken — writers must fail then, or token-rotation
 * coordination silently degrades to lost updates. */
static int store_lock(const char *path)
{
    if (ensure_parent_dir(path) != 0)
        return -1;
    char *lock_path = xasprintf("%s.lock", path);
    int fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    free(lock_path);
    if (fd < 0)
        return -1;
    while (flock(fd, LOCK_EX) != 0) {
        if (errno != EINTR) {
            close(fd);
            return -1;
        }
    }
    return fd;
}

static void store_unlock(int lock_fd)
{
    if (lock_fd >= 0)
        close(lock_fd);
}

static json_t *load_root(const char *path)
{
    char *contents = slurp_file(path, NULL);
    if (!contents)
        return NULL;
    json_t *root = json_loads(contents, 0, NULL);
    free(contents);
    if (!json_is_object(root)) {
        json_decref(root);
        return NULL;
    }
    return root;
}

/* Flush the directory entry after a rename so a crash cannot resurrect the replaced file. */
static void sync_parent_dir(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash)
        return;
    char *parent = xstrdup(path);
    parent[slash - path] = '\0';
    int fd = open(parent, O_RDONLY | O_CLOEXEC);
    free(parent);
    if (fd < 0)
        return;
    /* Best-effort: not every filesystem supports directory fsync. */
    fsync(fd);
    close(fd);
}

/* mkstemp creates the temporary file with mode 0600, which is also the mode credentials need, so
 * the rename never exposes a readable window. The fsync before rename is load-bearing: a rotated
 * refresh token restored from a stale page after a crash is spent and unrecoverable. */
static int save_root(const char *path, json_t *root)
{
    if (ensure_parent_dir(path) != 0)
        return -1;

    char *body = json_dumps(root, JSON_INDENT(2) | JSON_SORT_KEYS);
    if (!body)
        return -1;

    char *temp_path = xasprintf("%s.XXXXXX", path);
    int fd = mkstemp(temp_path);
    if (fd < 0)
        goto err_body;

    if (write_all(fd, body, strlen(body)) != 0 || write_all(fd, "\n", 1) != 0)
        goto err_fd;
    if (fsync(fd) != 0)
        goto err_fd;
    if (close(fd) != 0) {
        fd = -1;
        goto err_fd;
    }
    fd = -1;
    if (rename(temp_path, path) != 0)
        goto err_fd;
    sync_parent_dir(path);

    free(temp_path);
    free(body);
    return 0;

err_fd:
    if (fd >= 0)
        close(fd);
    unlink(temp_path);
    free(temp_path);
err_body:
    free(body);
    return -1;
}

json_t *cred_store_get(const char *provider_id)
{
    char *path = cred_store_file_path();
    if (!path)
        return NULL;
    json_t *root = load_root(path);
    free(path);
    if (!root)
        return NULL;

    json_t *entry = json_object_get(root, provider_id);
    if (entry)
        json_incref(entry);
    json_decref(root);
    return entry;
}

int cred_store_set(const char *provider_id, json_t *entry)
{
    char *path = cred_store_file_path();
    if (!path)
        return -1;
    int lock_fd = store_lock(path);
    if (lock_fd < 0) {
        free(path);
        return -1;
    }

    json_t *root = load_root(path);
    if (!root)
        root = json_object();
    json_object_set(root, provider_id, entry);
    int result = save_root(path, root);
    json_decref(root);
    store_unlock(lock_fd);
    free(path);
    return result;
}

/* Shared removal transaction; `entry_out` may be NULL when the removed entry is not wanted. */
static int take_entry(const char *provider_id, json_t **entry_out)
{
    char *path = cred_store_file_path();
    if (!path)
        return -1;
    int lock_fd = store_lock(path);
    if (lock_fd < 0) {
        free(path);
        return -1;
    }

    errno = 0;
    json_t *root = load_root(path);
    if (!root) {
        store_unlock(lock_fd);
        free(path);
        /* A missing store has nothing to remove; an unreadable one must not be rewritten. */
        return errno == ENOENT ? 0 : -1;
    }
    json_t *entry = json_object_get(root, provider_id);
    if (!entry) {
        json_decref(root);
        store_unlock(lock_fd);
        free(path);
        return 0;
    }
    if (entry_out)
        *entry_out = json_incref(entry);
    json_object_del(root, provider_id);
    int result = save_root(path, root) == 0 ? 1 : -1;
    json_decref(root);
    store_unlock(lock_fd);
    free(path);
    if (result != 1 && entry_out) {
        json_decref(*entry_out);
        *entry_out = NULL;
    }
    return result;
}

int cred_store_delete(const char *provider_id)
{
    return take_entry(provider_id, NULL);
}

int cred_store_take(const char *provider_id, json_t **entry_out)
{
    *entry_out = NULL;
    return take_entry(provider_id, entry_out);
}

int cred_store_update(const char *provider_id, cred_store_update_fn update, void *ctx)
{
    char *path = cred_store_file_path();
    if (!path)
        return -1;
    int lock_fd = store_lock(path);
    if (lock_fd < 0) {
        free(path);
        return -1;
    }

    json_t *root = load_root(path);
    json_t *entry = root ? json_object_get(root, provider_id) : NULL;
    json_t *replacement = NULL;
    enum cred_store_verdict verdict = update(entry, &replacement, ctx);
    int result = 0;
    if (verdict == CRED_STORE_WRITE && replacement) {
        if (!root)
            root = json_object();
        json_object_set(root, provider_id, replacement);
        result = save_root(path, root) == 0 ? 1 : -1;
    } else if (verdict == CRED_STORE_REMOVE && entry) {
        json_object_del(root, provider_id);
        result = save_root(path, root) == 0 ? 1 : -1;
    }
    json_decref(replacement);
    json_decref(root);
    store_unlock(lock_fd);
    free(path);
    return result;
}
