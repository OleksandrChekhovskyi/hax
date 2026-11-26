/* SPDX-License-Identifier: MIT */
#include "harness.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

int t_failures;
int t_skips;

/* Variables through which a hax parent or the user's shell would configure every test. */
static int t_is_inherited_setting(const char *name, size_t length)
{
    static const char api_key_suffix[] = "_API_KEY";
    static const char home_suffix[] = "_HOME";
    size_t api_key_length = sizeof(api_key_suffix) - 1;
    size_t home_length = sizeof(home_suffix) - 1;

    if (strncmp(name, "HAX_", 4) == 0)
        return 1;
    if (strncmp(name, "XDG_", 4) == 0 && length > home_length &&
        memcmp(name + length - home_length, home_suffix, home_length) == 0)
        return 1;
    return length > api_key_length &&
           memcmp(name + length - api_key_length, api_key_suffix, api_key_length) == 0;
}

static void t_clear_inherited_settings(void)
{
    extern char **environ;
    size_t count = 0;
    while (environ[count])
        count++;
    /* unsetenv rewrites environ, so collect the names first. */
    char **names = calloc(count + 1, sizeof(*names));
    if (!names)
        abort();
    size_t name_count = 0;
    for (size_t i = 0; i < count; i++) {
        const char *equals = strchr(environ[i], '=');
        if (equals && t_is_inherited_setting(environ[i], (size_t)(equals - environ[i])))
            names[name_count++] = strndup(environ[i], (size_t)(equals - environ[i]));
    }
    for (size_t i = 0; i < name_count; i++) {
        if (!names[i])
            abort();
        unsetenv(names[i]);
        free(names[i]);
    }
    free(names);
}

/* Anything beyond the loopback fixtures would reach a real provider, or hang where outbound packets
 * are dropped, so it goes to a closed port instead. libcurl prefers these lowercase names over the
 * uppercase and all_proxy forms. */
static void t_confine_network(void)
{
    setenv("http_proxy", "http://127.0.0.1:1", 1);
    setenv("https_proxy", "http://127.0.0.1:1", 1);
    setenv("no_proxy", "127.0.0.1,localhost,::1", 1);
}

__attribute__((constructor)) static void t_isolate_environment(void)
{
    t_clear_inherited_settings();
    setenv("HOME", t_tempdir(), 1);
    t_confine_network();
}

/* Ownership is per pid: a forked child that never calls t_tempdir() must not remove its parent's
 * dirs, and one that does removes only entries from t_tmpdir_first on, since an ancestor created
 * (and removes) the ones before. */
static char **t_tmpdirs;
static size_t t_n_tmpdirs;
static size_t t_tmpdir_first;
static pid_t t_tmpdir_owner;

/* Remove `name` under `dir_fd` recursively without following symlinks. Directories get u+rwx before
 * descending, so fixtures locked down to provoke EACCES don't defeat removal; file modes stay
 * untouched (unlink ignores them), so a hard link in a fixture can't rewrite an outside inode's
 * mode. In-process because spawning rm costs more than most tests do. */
static int remove_tree(int dir_fd, const char *name)
{
    struct stat st;
    if (fstatat(dir_fd, name, &st, AT_SYMLINK_NOFOLLOW) < 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode))
        return unlinkat(dir_fd, name, 0);
    if ((st.st_mode & S_IRWXU) != S_IRWXU && fchmodat(dir_fd, name, st.st_mode | S_IRWXU, 0) < 0)
        return -1;
    int fd = openat(dir_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return -1;
    DIR *dir = fdopendir(fd);
    if (!dir) {
        close(fd);
        return -1;
    }
    int failed = 0;
    size_t removed;
    /* Unlinking while reading may make readdir skip entries on some filesystems, so rescan until a
     * pass finds nothing left to remove. */
    do {
        removed = 0;
        rewinddir(dir);
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;
            if (remove_tree(dirfd(dir), entry->d_name) < 0)
                failed = 1;
            else
                removed++;
        }
    } while (!failed && removed > 0);
    closedir(dir);
    if (failed)
        return -1;
    return unlinkat(dir_fd, name, AT_REMOVEDIR);
}

static void tempdir_cleanup(void)
{
    if (getpid() != t_tmpdir_owner)
        return;
    for (size_t i = 0; i < t_n_tmpdirs; i++) {
        if (i >= t_tmpdir_first && remove_tree(AT_FDCWD, t_tmpdirs[i]) < 0)
            fprintf(stderr, "t_tempdir: failed to remove %s: %s\n", t_tmpdirs[i], strerror(errno));
        /* Inherited entries are freed only at exit: tests hold pointers into them. */
        free(t_tmpdirs[i]);
    }
    free(t_tmpdirs);
    t_tmpdirs = NULL;
    t_n_tmpdirs = 0;
    t_tmpdir_first = 0;
}

char *t_tempdir(void)
{
    if (t_tmpdir_owner != getpid()) {
        /* First call in this process. Registering the handler again in a fork child is harmless:
         * handlers run LIFO, so the inherited registration finds the list already emptied. */
        t_tmpdir_first = t_n_tmpdirs;
        t_tmpdir_owner = getpid();
        atexit(tempdir_cleanup);
    }
    char *dir = strdup("/tmp/hax_test_XXXXXX");
    if (!dir || !mkdtemp(dir)) {
        fprintf(stderr, "t_tempdir: %s\n", strerror(errno));
        abort();
    }
    /* A /tmp spelling would surface as a puzzling mismatch in whichever test compares it against
     * getcwd(), far from the cause. */
    char *real = realpath(dir, NULL);
    if (!real) {
        fprintf(stderr, "t_tempdir: realpath(%s): %s\n", dir, strerror(errno));
        abort();
    }
    free(dir);
    char **grown = realloc(t_tmpdirs, (t_n_tmpdirs + 1) * sizeof(*grown));
    if (!grown)
        abort();
    t_tmpdirs = grown;
    t_tmpdirs[t_n_tmpdirs++] = real;
    return real;
}

char *t_path_replace(const char *value)
{
    const char *current = getenv("PATH");
    char *saved = NULL;
    if (current) {
        saved = strdup(current);
        if (!saved)
            abort();
    }
    if (value)
        setenv("PATH", value, 1);
    else
        unsetenv("PATH");
    return saved;
}

char *t_path_prepend(const char *dir)
{
    const char *current = getenv("PATH");
    if (!current || !*current)
        return t_path_replace(dir);
    size_t len = strlen(dir) + 1 + strlen(current) + 1;
    char *combined = malloc(len);
    if (!combined)
        abort();
    snprintf(combined, len, "%s:%s", dir, current);
    char *saved = t_path_replace(combined);
    free(combined);
    return saved;
}

void t_path_restore(char *saved)
{
    if (saved)
        setenv("PATH", saved, 1);
    else
        unsetenv("PATH");
    free(saved);
}
