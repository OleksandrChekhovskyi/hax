/* SPDX-License-Identifier: MIT */
#include "session_prune.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>

#include "config.h"
#include "session.h"
#include "util.h"
#include "system/bg_job.h"
#include "system/fs.h"
#include "system/path.h"

#define SESSION_PRUNE_INTERVAL_S (24 * 60 * 60)

struct prune_args {
    char *sessions_dir;
    char *exclude_path;
    time_t cutoff;
    int marker_fd;
};

static struct bg_job *active_prune_job;

time_t session_retention_cutoff(void)
{
    int days = config_int("session_retention_days");
    if (days <= 0)
        return 0;
    return time(NULL) - (time_t)days * 24 * 60 * 60;
}

/* Session storage is exactly two levels deep: one encoded-cwd directory,
 * then session files. Descriptor-relative operations with O_NOFOLLOW keep a
 * swapped symlink from redirecting cleanup outside that tree. A cancelled
 * walk returns -1 so its caller does not stamp the completion marker. */
static int prune_tree(const char *sessions_dir, time_t cutoff, const char *exclude_path,
                      struct bg_job *job)
{
    int sessions_fd = open(sessions_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (sessions_fd < 0)
        return 0;
    DIR *sessions = fdopendir(sessions_fd);
    if (!sessions) {
        close(sessions_fd);
        return 0;
    }
    sessions_fd = dirfd(sessions);
    struct dirent *project_entry;

    while ((project_entry = readdir(sessions))) {
        if (bg_job_cancel_requested(job)) {
            closedir(sessions);
            return -1;
        }
        if (project_entry->d_name[0] == '.' &&
            (project_entry->d_name[1] == '\0' ||
             (project_entry->d_name[1] == '.' && project_entry->d_name[2] == '\0')))
            continue;

        struct stat project_stat;
        if (fstatat(sessions_fd, project_entry->d_name, &project_stat, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISDIR(project_stat.st_mode))
            continue;
        int project_fd = openat(sessions_fd, project_entry->d_name,
                                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (project_fd < 0)
            continue;
        DIR *project = fdopendir(project_fd);
        if (!project) {
            close(project_fd);
            continue;
        }

        struct dirent *session_entry;
        while ((session_entry = readdir(project))) {
            if (bg_job_cancel_requested(job)) {
                closedir(project);
                closedir(sessions);
                return -1;
            }
            if (!session_path_is_standard(session_entry->d_name))
                continue;

            struct stat session_stat;
            if (fstatat(project_fd, session_entry->d_name, &session_stat, AT_SYMLINK_NOFOLLOW) !=
                    0 ||
                !S_ISREG(session_stat.st_mode) || session_stat.st_mtime >= cutoff)
                continue;

            char *session_path =
                xasprintf("%s/%s/%s", sessions_dir, project_entry->d_name, session_entry->d_name);
            int excluded = exclude_path && strcmp(session_path, exclude_path) == 0;
            free(session_path);
            if (excluded)
                continue;

            int session_fd =
                openat(project_fd, session_entry->d_name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (session_fd < 0)
                continue;

            /* Writers hold a shared lock for the file's lifetime. After the
             * exclusive lock, verify that the opened inode is still old and
             * is the same candidate observed during enumeration. */
            struct stat locked_stat;
            if (flock(session_fd, LOCK_EX | LOCK_NB) == 0 && fstat(session_fd, &locked_stat) == 0 &&
                S_ISREG(locked_stat.st_mode) && locked_stat.st_mtime < cutoff &&
                locked_stat.st_dev == session_stat.st_dev &&
                locked_stat.st_ino == session_stat.st_ino)
                (void)unlinkat(project_fd, session_entry->d_name, 0);
            close(session_fd);
        }
        closedir(project);
        /* rmdir semantics preserve buckets containing a live session or an
         * unrelated file; only genuinely empty project buckets disappear. */
        (void)unlinkat(sessions_fd, project_entry->d_name, AT_REMOVEDIR);
    }
    closedir(sessions);
    return 0;
}

int session_prune_before(time_t cutoff, const char *exclude_path)
{
    char *sessions_dir = xdg_hax_state_path("sessions");
    if (!sessions_dir)
        return 0;
    int result = prune_tree(sessions_dir, cutoff, exclude_path, NULL);
    free(sessions_dir);
    return result;
}

static void prune_args_free(struct prune_args *args)
{
    if (!args)
        return;
    free(args->sessions_dir);
    free(args->exclude_path);
    free(args);
}

static void prune_worker(struct bg_job *job, void *arg)
{
    struct prune_args *args = arg;
    if (prune_tree(args->sessions_dir, args->cutoff, args->exclude_path, job) == 0) {
        /* A zero-length marker means no sweep has completed yet. Truncate
         * first so a crash during this update causes an early retry. */
        if (ftruncate(args->marker_fd, 0) == 0 && lseek(args->marker_fd, 0, SEEK_SET) == 0)
            (void)write_all(args->marker_fd, "1", 1);
    }
    (void)flock(args->marker_fd, LOCK_UN);
    close(args->marker_fd);
    prune_args_free(args);
}

void session_prune_start(const char *exclude_path)
{
    if (active_prune_job)
        return;
    time_t cutoff = session_retention_cutoff();
    if (!cutoff)
        return;

    char *sessions_dir = xdg_hax_state_path("sessions");
    struct stat sessions_stat;
    if (!sessions_dir || lstat(sessions_dir, &sessions_stat) != 0 ||
        !S_ISDIR(sessions_stat.st_mode)) {
        free(sessions_dir);
        return;
    }

    /* The marker lock elects one pruner across processes; its mtime records
     * the last completed sweep. The worker keeps the lock until it finishes,
     * and a zero-length marker means the prior attempt never completed. */
    char *marker_path = xasprintf("%s/.prune", sessions_dir);
    int marker_fd = open(marker_path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    free(marker_path);
    if (marker_fd < 0 || flock(marker_fd, LOCK_EX | LOCK_NB) != 0) {
        if (marker_fd >= 0)
            close(marker_fd);
        free(sessions_dir);
        return;
    }
    (void)fchmod(marker_fd, 0600);

    struct stat marker_stat;
    time_t now = time(NULL);
    /* Treat a future mtime as fresh: a wall-clock rollback should not make
     * every launch repeat the sweep until time catches up. */
    if (fstat(marker_fd, &marker_stat) == 0 && marker_stat.st_size > 0 &&
        (now <= marker_stat.st_mtime || now - marker_stat.st_mtime < SESSION_PRUNE_INTERVAL_S)) {
        (void)flock(marker_fd, LOCK_UN);
        close(marker_fd);
        free(sessions_dir);
        return;
    }

    struct prune_args *args = xcalloc(1, sizeof(*args));
    args->sessions_dir = sessions_dir;
    /* Startup resolves --resume before spawning us; protect that path until
     * its writer has opened the file and acquired the shared lock. */
    args->exclude_path = exclude_path ? xstrdup(exclude_path) : NULL;
    args->cutoff = cutoff;
    args->marker_fd = marker_fd;
    active_prune_job = bg_job_spawn(prune_worker, args);
    if (!active_prune_job) {
        (void)flock(marker_fd, LOCK_UN);
        close(marker_fd);
        prune_args_free(args);
    }
}

void session_prune_shutdown(void)
{
    if (!active_prune_job)
        return;
    bg_job_cancel(active_prune_job);
    bg_job_join(active_prune_job);
    active_prune_job = NULL;
}
