/* SPDX-License-Identifier: MIT */
#include <libgen.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "harness.h"
#include "system/tempfiles.h"

static void test_create_tracks_and_cleanup_unlinks(void)
{
    char *dir = t_tempdir();
    setenv("TMPDIR", dir, 1);

    char *a = NULL;
    char *b = NULL;
    int fda = tempfile_create("t-", "", &a);
    int fdb = tempfile_create("t-", ".png", &b);
    EXPECT(fda >= 0 && a != NULL);
    EXPECT(fdb >= 0 && b != NULL);
    EXPECT(strcmp(a, b) != 0);

    /* Layout: <TMPDIR>/hax-XXXXXX/<prefix><seq><suffix>, container 0700,
     * entries 0600. */
    EXPECT(strncmp(a, dir, strlen(dir)) == 0);
    EXPECT(strstr(a, "/hax-") != NULL);
    EXPECT(strstr(a, "/t-") != NULL);
    EXPECT(strstr(b, ".png") != NULL && strcmp(b + strlen(b) - 4, ".png") == 0);

    struct stat st;
    char *acopy = strdup(a);
    EXPECT(stat(dirname(acopy), &st) == 0 && (st.st_mode & 0777) == 0700);
    free(acopy);
    EXPECT(stat(a, &st) == 0 && (st.st_mode & 0777) == 0600);
    EXPECT(write(fda, "x", 1) == 1);
    close(fda);
    close(fdb);

    /* Both files share one container dir; cleanup removes it too. */
    char *bcopy = strdup(b);
    char *container = strdup(dirname(bcopy));
    free(bcopy);
    EXPECT(strncmp(a, container, strlen(container)) == 0);

    tempfiles_cleanup();
    EXPECT(stat(a, &st) < 0);
    EXPECT(stat(b, &st) < 0);
    EXPECT(stat(container, &st) < 0);
    free(container);
    free(a);
    free(b);
    unsetenv("TMPDIR");
}

static void test_untrack_survives_cleanup(void)
{
    char *dir = t_tempdir();
    setenv("TMPDIR", dir, 1);

    char *keep = NULL;
    char *drop = NULL;
    int fdk = tempfile_create("t-", "", &keep);
    int fdd = tempfile_create("t-", "", &drop);
    EXPECT(fdk >= 0 && fdd >= 0);
    close(fdk);
    close(fdd);

    tempfile_untrack(keep);
    tempfiles_cleanup();

    struct stat st;
    EXPECT(stat(keep, &st) == 0); /* untracked: registry no longer owns it */
    EXPECT(stat(drop, &st) < 0);
    unlink(keep);
    free(keep);
    free(drop);
    unsetenv("TMPDIR");
}

static void test_untrack_unknown_path_is_noop(void)
{
    tempfile_untrack("/nonexistent/not-tracked");
    tempfiles_cleanup(); /* no tracked entries: must not crash */
}

static void test_tmpdir_change_starts_fresh_dir(void)
{
    char *first = t_tempdir();
    char *second = t_tempdir();

    setenv("TMPDIR", first, 1);
    char *a = NULL;
    int fda = tempfile_create("t-", "", &a);
    EXPECT(fda >= 0);
    close(fda);
    EXPECT(strncmp(a, first, strlen(first)) == 0);

    setenv("TMPDIR", second, 1);
    char *b = NULL;
    int fdb = tempfile_create("t-", "", &b);
    EXPECT(fdb >= 0);
    close(fdb);
    EXPECT(strncmp(b, second, strlen(second)) == 0);

    /* Cleanup still reclaims both files — tracking is by full path —
     * and reaps the superseded container directory, not just the
     * current one. */
    char *acopy = strdup(a);
    char *old_container = strdup(dirname(acopy));
    free(acopy);
    tempfiles_cleanup();
    struct stat st;
    EXPECT(stat(a, &st) < 0);
    EXPECT(stat(b, &st) < 0);
    EXPECT(stat(old_container, &st) < 0);
    free(old_container);
    free(a);
    free(b);
    unsetenv("TMPDIR");
}

static void test_externally_removed_dir_recovers(void)
{
    char *dir = t_tempdir();
    setenv("TMPDIR", dir, 1);

    char *a = NULL;
    int fda = tempfile_create("t-", "", &a);
    EXPECT(fda >= 0);
    close(fda);

    /* Simulate a tmp reaper: the container vanishes underneath us. The
     * cleanup's rmdir then fails with ENOENT, which must drop the
     * cached handle — a kept stale handle would make every later
     * create fail until process exit. */
    char *acopy = strdup(a);
    char *container = strdup(dirname(acopy));
    free(acopy);
    unlink(a);
    EXPECT(rmdir(container) == 0);
    tempfiles_cleanup();

    char *b = NULL;
    int fdb = tempfile_create("t-", "", &b);
    EXPECT(fdb >= 0 && b != NULL);
    if (fdb >= 0) {
        struct stat st;
        EXPECT(stat(b, &st) == 0);
        EXPECT(strncmp(b, container, strlen(container)) != 0); /* fresh dir */
        close(fdb);
    }
    tempfiles_cleanup();
    free(a);
    free(b);
    free(container);
    unsetenv("TMPDIR");
}

static void test_reaped_dir_recovers_on_create(void)
{
    char *dir = t_tempdir();
    setenv("TMPDIR", dir, 1);

    char *a = NULL;
    int fda = tempfile_create("t-", "", &a);
    EXPECT(fda >= 0);
    close(fda);

    /* Reap between creates, with no cleanup in between: the very next
     * create must detect ENOENT, drop the cached handle, and land in a
     * fresh container instead of failing until the next flush. */
    char *acopy = strdup(a);
    char *container = strdup(dirname(acopy));
    free(acopy);
    unlink(a);
    EXPECT(rmdir(container) == 0);

    char *b = NULL;
    int fdb = tempfile_create("t-", "", &b);
    EXPECT(fdb >= 0 && b != NULL);
    if (fdb >= 0) {
        struct stat st;
        EXPECT(stat(b, &st) == 0);
        EXPECT(strncmp(b, container, strlen(container)) != 0);
        close(fdb);
    }
    tempfiles_cleanup();
    free(a);
    free(b);
    free(container);
    unsetenv("TMPDIR");
}

static void test_invalid_utf8_tmpdir_falls_back(void)
{
    /* A $TMPDIR with a raw non-UTF-8 byte must not end up in the path the
     * model sees; creation falls back to /tmp. */
    setenv("TMPDIR", "/tmp/\xff-bogus", 1);
    char *path = NULL;
    int fd = tempfile_create("t-", "", &path);
    EXPECT(fd >= 0 && path != NULL);
    EXPECT(strncmp(path, "/tmp/hax-", 9) == 0);
    close(fd);
    tempfiles_cleanup();
    free(path);
    unsetenv("TMPDIR");
}

int main(void)
{
    test_create_tracks_and_cleanup_unlinks();
    test_untrack_survives_cleanup();
    test_untrack_unknown_path_is_noop();
    test_tmpdir_change_starts_fresh_dir();
    test_externally_removed_dir_recovers();
    test_reaped_dir_recovers_on_create();
    test_invalid_utf8_tmpdir_falls_back();
    T_REPORT();
}
