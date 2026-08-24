/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "harness.h"
#include "system/tempfiles.h"

static int path_is_under(const char *path, const char *dir)
{
    size_t path_len = strlen(path);
    size_t dir_len = strlen(dir);
    return path_len > dir_len && strncmp(path, dir, dir_len) == 0 && path[dir_len] == '/';
}

static char *parent_dir(const char *path)
{
    char *parent = strdup(path);
    char *separator = strrchr(parent, '/');
    if (separator)
        *separator = '\0';
    return parent;
}

static void test_create_tracks_files_and_cleanup_removes_them(void)
{
    char *tmpdir = t_tempdir();
    setenv("TMPDIR", tmpdir, 1);

    char *plain_path = NULL;
    char *image_path = NULL;
    int plain_fd = tempfile_create("t-", "", &plain_path);
    int image_fd = tempfile_create("t-", ".png", &image_path);
    EXPECT(plain_fd >= 0 && plain_path != NULL);
    EXPECT(image_fd >= 0 && image_path != NULL);
    EXPECT(strcmp(plain_path, image_path) != 0);
    EXPECT(path_is_under(plain_path, tmpdir));
    EXPECT(strstr(plain_path, "/hax-") != NULL);
    EXPECT(strstr(plain_path, "/t-") != NULL);
    EXPECT(strlen(image_path) >= 4 && strcmp(image_path + strlen(image_path) - 4, ".png") == 0);

    char *plain_dir = parent_dir(plain_path);
    char *image_dir = parent_dir(image_path);
    EXPECT_STR_EQ(plain_dir, image_dir);

    struct stat st;
#ifdef _WIN32
    EXPECT(stat(plain_dir, &st) == 0);
    EXPECT(stat(plain_path, &st) == 0);
#else
    EXPECT(stat(plain_dir, &st) == 0 && (st.st_mode & 0777) == 0700);
    EXPECT(stat(plain_path, &st) == 0 && (st.st_mode & 0777) == 0600);
#endif
    EXPECT(write(plain_fd, "x", 1) == 1);
    close(plain_fd);
    close(image_fd);

    tempfiles_cleanup();
    EXPECT(stat(plain_path, &st) < 0);
    EXPECT(stat(image_path, &st) < 0);
    EXPECT(stat(plain_dir, &st) < 0);

    free(plain_dir);
    free(image_dir);
    free(plain_path);
    free(image_path);
    unsetenv("TMPDIR");
}

static void test_untracked_file_survives_cleanup(void)
{
    char *tmpdir = t_tempdir();
    setenv("TMPDIR", tmpdir, 1);

    char *kept_path = NULL;
    char *removed_path = NULL;
    int kept_fd = tempfile_create("t-", "", &kept_path);
    int removed_fd = tempfile_create("t-", "", &removed_path);
    EXPECT(kept_fd >= 0 && removed_fd >= 0);
    close(kept_fd);
    close(removed_fd);

    tempfile_untrack(kept_path);
    tempfiles_cleanup();

    struct stat st;
    EXPECT(stat(kept_path, &st) == 0);
    EXPECT(stat(removed_path, &st) < 0);

    unlink(kept_path);
    tempfiles_cleanup();
    free(kept_path);
    free(removed_path);
    unsetenv("TMPDIR");
}

static void test_untracking_unknown_path_is_noop(void)
{
    tempfile_untrack("/nonexistent/not-tracked");
    tempfiles_cleanup();
}

static void test_tmpdir_change_uses_new_directory(void)
{
    char *first_tmpdir = t_tempdir();
    char *second_tmpdir = t_tempdir();

    setenv("TMPDIR", first_tmpdir, 1);
    char *first_path = NULL;
    int first_fd = tempfile_create("t-", "", &first_path);
    EXPECT(first_fd >= 0);
    close(first_fd);
    EXPECT(path_is_under(first_path, first_tmpdir));

    setenv("TMPDIR", second_tmpdir, 1);
    char *second_path = NULL;
    int second_fd = tempfile_create("t-", "", &second_path);
    EXPECT(second_fd >= 0);
    close(second_fd);
    EXPECT(path_is_under(second_path, second_tmpdir));

    char *first_container = parent_dir(first_path);
    tempfiles_cleanup();

    struct stat st;
    EXPECT(stat(first_path, &st) < 0);
    EXPECT(stat(second_path, &st) < 0);
    EXPECT(stat(first_container, &st) < 0);

    free(first_container);
    free(first_path);
    free(second_path);
    unsetenv("TMPDIR");
}

static void test_cleanup_retries_retired_nonempty_directory(void)
{
    char *first_tmpdir = t_tempdir();
    char *second_tmpdir = t_tempdir();

    setenv("TMPDIR", first_tmpdir, 1);
    char *kept_path = NULL;
    int kept_fd = tempfile_create("t-", "", &kept_path);
    EXPECT(kept_fd >= 0);
    close(kept_fd);
    tempfile_untrack(kept_path);
    char *first_container = parent_dir(kept_path);

    setenv("TMPDIR", second_tmpdir, 1);
    char *removed_path = NULL;
    int removed_fd = tempfile_create("t-", "", &removed_path);
    EXPECT(removed_fd >= 0);
    close(removed_fd);

    tempfiles_cleanup();

    struct stat st;
    EXPECT(stat(kept_path, &st) == 0);
    EXPECT(stat(first_container, &st) == 0);
    EXPECT(stat(removed_path, &st) < 0);

    unlink(kept_path);
    tempfiles_cleanup();
    EXPECT(stat(first_container, &st) < 0);

    free(first_container);
    free(kept_path);
    free(removed_path);
    unsetenv("TMPDIR");
}

static void test_cleanup_forgets_externally_removed_directory(void)
{
    char *tmpdir = t_tempdir();
    setenv("TMPDIR", tmpdir, 1);

    char *first_path = NULL;
    int first_fd = tempfile_create("t-", "", &first_path);
    EXPECT(first_fd >= 0);
    close(first_fd);

    char *removed_container = parent_dir(first_path);
    unlink(first_path);
    EXPECT(rmdir(removed_container) == 0);
    tempfiles_cleanup();

    char *second_path = NULL;
    int second_fd = tempfile_create("t-", "", &second_path);
    EXPECT(second_fd >= 0 && second_path != NULL);
    if (second_fd >= 0) {
        char *second_container = parent_dir(second_path);
        EXPECT(strcmp(second_container, removed_container) != 0);
        free(second_container);
        close(second_fd);
    }

    tempfiles_cleanup();
    free(first_path);
    free(second_path);
    free(removed_container);
    unsetenv("TMPDIR");
}

static void test_create_recovers_from_externally_removed_directory(void)
{
    char *tmpdir = t_tempdir();
    setenv("TMPDIR", tmpdir, 1);

    char *first_path = NULL;
    int first_fd = tempfile_create("t-", "", &first_path);
    EXPECT(first_fd >= 0);
    close(first_fd);

    char *removed_container = parent_dir(first_path);
    unlink(first_path);
    EXPECT(rmdir(removed_container) == 0);

    char *second_path = NULL;
    int second_fd = tempfile_create("t-", "", &second_path);
    EXPECT(second_fd >= 0 && second_path != NULL);
    if (second_fd >= 0) {
        char *second_container = parent_dir(second_path);
        EXPECT(strcmp(second_container, removed_container) != 0);
        free(second_container);
        close(second_fd);
    }

    tempfiles_cleanup();
    free(first_path);
    free(second_path);
    free(removed_container);
    unsetenv("TMPDIR");
}

static void test_invalid_utf8_tmpdir_falls_back_to_tmp(void)
{
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

static void test_invalid_name_fragments_are_rejected(void)
{
    char sentinel;
    char *path = &sentinel;
    errno = 0;
    EXPECT(tempfile_create(NULL, "", &path) == -1);
    EXPECT(path == NULL);
    EXPECT(errno == EINVAL);

    path = &sentinel;
    errno = 0;
    EXPECT(tempfile_create("t-", NULL, &path) == -1);
    EXPECT(path == NULL);
    EXPECT(errno == EINVAL);

    path = &sentinel;
    errno = 0;
    EXPECT(tempfile_create("../escape-", "", &path) == -1);
    EXPECT(path == NULL);
    EXPECT(errno == EINVAL);

    path = &sentinel;
    errno = 0;
    EXPECT(tempfile_create("t-", "/suffix", &path) == -1);
    EXPECT(path == NULL);
    EXPECT(errno == EINVAL);

    path = &sentinel;
    errno = 0;
    EXPECT(tempfile_create("\xff-invalid-", "", &path) == -1);
    EXPECT(path == NULL);
    EXPECT(errno == EINVAL);
}

static void test_create_reports_tmpdir_error(void)
{
    char missing_tmpdir[512];
    snprintf(missing_tmpdir, sizeof(missing_tmpdir), "%s/missing", t_tempdir());
    setenv("TMPDIR", missing_tmpdir, 1);

    char sentinel;
    char *path = &sentinel;
    errno = 0;
    EXPECT(tempfile_create("t-", "", &path) == -1);
    EXPECT(path == NULL);
    EXPECT(errno == ENOENT);

    unsetenv("TMPDIR");
}

int main(void)
{
    test_create_tracks_files_and_cleanup_removes_them();
    test_untracked_file_survives_cleanup();
    test_untracking_unknown_path_is_noop();
    test_tmpdir_change_uses_new_directory();
    test_cleanup_retries_retired_nonempty_directory();
    test_cleanup_forgets_externally_removed_directory();
    test_create_recovers_from_externally_removed_directory();
    test_invalid_utf8_tmpdir_falls_back_to_tmp();
    test_invalid_name_fragments_are_rejected();
    test_create_reports_tmpdir_error();
    T_REPORT();
}
