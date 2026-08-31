/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "harness.h"
#include "provider.h"
#include "session.h"
#include "session_prune.h"
#include "xalloc.h"
#include "system/path.h"

static struct item ONE_TURN[] = {
    {.kind = ITEM_TURN_BOUNDARY},
    {.kind = ITEM_USER_MESSAGE, .text = (char *)"hello"},
};

struct fixture {
    char original_cwd[4096];
    char *project_a;
    char *project_b;
};

static void fixture_init(struct fixture *f)
{
    memset(f, 0, sizeof(*f));
    EXPECT(getcwd(f->original_cwd, sizeof(f->original_cwd)) != NULL);
    char *projects = t_tempdir();
    f->project_a = xasprintf("%s/one", projects);
    f->project_b = xasprintf("%s/two", projects);
    EXPECT(mkdir(f->project_a, 0700) == 0);
    EXPECT(mkdir(f->project_b, 0700) == 0);
    setenv("XDG_STATE_HOME", t_tempdir(), 1);
    setenv("HAX_SESSION_RETENTION_DAYS", "30", 1);
    unsetenv("HAX_NO_SESSION");
    EXPECT(chdir(f->project_a) == 0);
}

static void fixture_free(struct fixture *f)
{
    session_prune_shutdown();
    EXPECT(chdir(f->original_cwd) == 0);
    free(f->project_a);
    free(f->project_b);
}

static char *write_session(const char *cwd, struct session_log **keep_open)
{
    EXPECT(chdir(cwd) == 0);
    struct session_log *log = session_log_open("test", "model", NULL, NULL, NULL);
    EXPECT(log != NULL);
    if (!log)
        return xstrdup("/nonexistent");
    char *path = xstrdup(session_log_path(log));
    session_log_append(log, ONE_TURN, sizeof(ONE_TURN) / sizeof(*ONE_TURN));
    if (keep_open)
        *keep_open = log;
    else
        session_log_close(log);
    return path;
}

static void set_mtime(const char *path, time_t when)
{
    struct timespec ts[2] = {{.tv_sec = when}, {.tv_sec = when}};
    EXPECT(utimensat(AT_FDCWD, path, ts, 0) == 0);
}

static char *parent_path(const char *path)
{
    char *out = xstrdup(path);
    char *slash = strrchr(out, '/');
    EXPECT(slash != NULL);
    if (slash)
        *slash = '\0';
    return out;
}

static void expect_missing(const char *path)
{
    errno = 0;
    EXPECT(access(path, F_OK) != 0 && errno == ENOENT);
}

static void test_listing_hides_expired_sessions(void)
{
    /* Expiration must affect every resume entry point immediately, without
     * waiting for the daily physical sweep. Disabling retention restores the
     * old enumeration behavior. */
    struct fixture f;
    fixture_init(&f);
    char *old_path = write_session(f.project_a, NULL);
    char *recent_path = write_session(f.project_a, NULL);
    set_mtime(old_path, time(NULL) - 31 * 24 * 60 * 60);

    struct session_entry *list;
    size_t n;
    session_list(f.project_a, &list, &n);
    EXPECT(n == 1);
    if (n == 1)
        EXPECT_STR_EQ(list[0].path, recent_path);
    session_list_free(list, n);

    setenv("HAX_SESSION_RETENTION_DAYS", "0", 1);
    session_list(f.project_a, &list, &n);
    EXPECT(n == 2);
    session_list_free(list, n);

    free(recent_path);
    free(old_path);
    fixture_free(&f);
}

static void test_touch_refreshes_selected_session(void)
{
    /* Selection is cross-process activity: refreshing mtime before load makes
     * a concurrent pruner's post-lock age check preserve the conversation. */
    struct fixture f;
    fixture_init(&f);
    char *path = write_session(f.project_a, NULL);
    time_t old = time(NULL) - 31 * 24 * 60 * 60;
    set_mtime(path, old);

    EXPECT(session_touch(path) == 0);
    struct stat st;
    EXPECT(stat(path, &st) == 0 && st.st_mtime > old);
    EXPECT(session_prune_before(time(NULL) - 30 * 24 * 60 * 60, NULL) == 0);
    EXPECT(access(path, F_OK) == 0);

    free(path);
    fixture_free(&f);
}

static void test_sweep_is_global_and_preserves_live_sessions(void)
{
    /* The sweep must reach abandoned cwd buckets, while preserving the path
     * selected for this startup and any file held by a live writer. Empty
     * buckets should disappear so project names do not accumulate forever. */
    struct fixture f;
    fixture_init(&f);
    char *old_path = write_session(f.project_a, NULL);
    char *recent_path = write_session(f.project_a, NULL);
    char *excluded_path = write_session(f.project_a, NULL);
    struct session_log *locked_log = NULL;
    char *locked_path = write_session(f.project_a, &locked_log);
    char *other_path = write_session(f.project_b, NULL);
    char *other_dir = parent_path(other_path);

    time_t now = time(NULL);
    time_t old = now - 31 * 24 * 60 * 60;
    set_mtime(old_path, old);
    set_mtime(excluded_path, old);
    set_mtime(locked_path, old);
    set_mtime(other_path, old);

    EXPECT(session_prune_before(now - 30 * 24 * 60 * 60, excluded_path) == 0);
    expect_missing(old_path);
    expect_missing(other_path);
    expect_missing(other_dir);
    EXPECT(access(excluded_path, F_OK) == 0);
    EXPECT(access(locked_path, F_OK) == 0);
    EXPECT(access(recent_path, F_OK) == 0);

    session_log_close(locked_log);
    EXPECT(session_prune_before(now - 30 * 24 * 60 * 60, NULL) == 0);
    expect_missing(excluded_path);
    expect_missing(locked_path);
    EXPECT(access(recent_path, F_OK) == 0);

    free(other_dir);
    free(other_path);
    free(locked_path);
    free(excluded_path);
    free(recent_path);
    free(old_path);
    fixture_free(&f);
}

static void test_sweep_leaves_unrelated_entries_untouched(void)
{
    /* Cleanup must never follow symlinks or infer ownership from a .jsonl
     * suffix alone: users may inspect or place other files in the state tree. */
    struct fixture f;
    fixture_init(&f);
    char *session_path = write_session(f.project_a, NULL);
    char *bucket = parent_path(session_path);
    char *unrelated = xasprintf("%s/backup_00000000-0000-4000-8000-000000000000.jsonl", bucket);
    int fd = open(unrelated, O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    EXPECT(fd >= 0);
    if (fd >= 0)
        close(fd);
    char *link =
        xasprintf("%s/2000-01-01T00-00-00Z_00000000-0000-4000-8000-000000000001.jsonl", bucket);
    EXPECT(symlink(session_path, link) == 0);
    EXPECT(session_path_is_standard(session_path));
    EXPECT(!session_path_is_standard(unrelated));
    EXPECT(session_path_is_standard(link));

    time_t old = time(NULL) - 31 * 24 * 60 * 60;
    set_mtime(session_path, old);
    set_mtime(unrelated, old);
    EXPECT(session_prune_before(time(NULL) - 30 * 24 * 60 * 60, NULL) == 0);
    expect_missing(session_path);
    EXPECT(access(unrelated, F_OK) == 0);
    struct stat lst;
    EXPECT(lstat(link, &lst) == 0 && S_ISLNK(lst.st_mode));

    free(link);
    free(unrelated);
    free(bucket);
    free(session_path);
    fixture_free(&f);
}

static void test_resume_does_not_recreate_removed_session(void)
{
    /* If cleanup wins the final race after a load, resume must not create an
     * empty append target that claims to have a header but has none. */
    struct fixture f;
    fixture_init(&f);
    char *path = write_session(f.project_a, NULL);
    EXPECT(unlink(path) == 0);
    struct session_log *log = session_log_resume(path, "test", "model", NULL, NULL, 0);
    EXPECT(log == NULL);
    expect_missing(path);

    free(path);
    fixture_free(&f);
}

static void test_background_sweep_marks_only_completion(void)
{
    /* A completed worker stamps the daily marker; the next launch must skip
     * even when another expired file appears. Waiting for both deletion and
     * the marker distinguishes completion from a partial, cancelled sweep. */
    struct fixture f;
    fixture_init(&f);
    time_t old = time(NULL) - 31 * 24 * 60 * 60;
    char *path = write_session(f.project_a, NULL);
    set_mtime(path, old);
    char *marker = xdg_hax_state_path("sessions/.prune");

    session_prune_start(NULL);
    int completed = 0;
    for (int i = 0; i < 100; i++) {
        struct stat st;
        if (access(path, F_OK) != 0 && stat(marker, &st) == 0 && st.st_size > 0) {
            completed = 1;
            break;
        }
        struct timespec pause = {.tv_nsec = 10 * 1000 * 1000};
        nanosleep(&pause, NULL);
    }
    session_prune_shutdown();
    EXPECT(completed);

    char *throttled_path = write_session(f.project_a, NULL);
    set_mtime(throttled_path, old);
    session_prune_start(NULL);
    session_prune_shutdown();
    EXPECT(access(throttled_path, F_OK) == 0);

    free(throttled_path);
    free(marker);
    free(path);
    fixture_free(&f);
}

int main(void)
{
    test_listing_hides_expired_sessions();
    test_touch_refreshes_selected_session();
    test_sweep_is_global_and_preserves_live_sessions();
    test_sweep_leaves_unrelated_entries_untouched();
    test_resume_does_not_recreate_removed_session();
    test_background_sweep_marks_only_completion();
    T_REPORT();
}
