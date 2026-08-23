/* SPDX-License-Identifier: MIT */
#ifdef _WIN32

#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>

#include "config.h"
#include "session.h"
#include "session_prune.h"
#include "util.h"
#include "system/bg_job.h"
#include "system/path.h"

#define SESSION_PRUNE_INTERVAL_S (24 * 60 * 60)

struct prune_args {
    char *sessions_dir;
    char *exclude_path;
    char *marker_path;
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

static int prune_tree(const char *sessions_dir, time_t cutoff, const char *exclude_path,
                      struct bg_job *job)
{
    DIR *sessions = opendir(sessions_dir);
    if (!sessions)
        return 0;

    for (struct dirent *project_entry; (project_entry = readdir(sessions));) {
        if (bg_job_cancel_requested(job)) {
            closedir(sessions);
            return -1;
        }
        if (project_entry->d_name[0] == '.')
            continue;

        char *project_path = path_join(sessions_dir, project_entry->d_name);
        struct stat project_stat;
        if (project_entry->d_type == DT_LNK || stat(project_path, &project_stat) != 0 ||
            !S_ISDIR(project_stat.st_mode)) {
            free(project_path);
            continue;
        }
        DIR *project = opendir(project_path);
        if (!project) {
            free(project_path);
            continue;
        }

        for (struct dirent *session_entry; (session_entry = readdir(project));) {
            if (bg_job_cancel_requested(job)) {
                closedir(project);
                free(project_path);
                closedir(sessions);
                return -1;
            }
            if (!session_path_is_standard(session_entry->d_name) || session_entry->d_type == DT_LNK)
                continue;

            char *session_path = path_join(project_path, session_entry->d_name);
            struct stat observed;
            if (stat(session_path, &observed) != 0 || !S_ISREG(observed.st_mode) ||
                observed.st_mtime >= cutoff ||
                (exclude_path && strcmp(session_path, exclude_path) == 0)) {
                free(session_path);
                continue;
            }

            int fd = open(session_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            struct stat locked;
            if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0 && fstat(fd, &locked) == 0 &&
                S_ISREG(locked.st_mode) && locked.st_mtime < cutoff &&
                locked.st_size == observed.st_size)
                (void)unlink(session_path);
            if (fd >= 0)
                close(fd);
            free(session_path);
        }
        closedir(project);
        (void)rmdir(project_path);
        free(project_path);
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
    free(args->marker_path);
    free(args);
}

static void prune_worker(struct bg_job *job, void *opaque)
{
    struct prune_args *args = opaque;
    if (prune_tree(args->sessions_dir, args->cutoff, args->exclude_path, job) == 0) {
        (void)ftruncate(args->marker_fd, 0);
        (void)lseek(args->marker_fd, 0, SEEK_SET);
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
    if (!sessions_dir || stat(sessions_dir, &sessions_stat) != 0 ||
        !S_ISDIR(sessions_stat.st_mode)) {
        free(sessions_dir);
        return;
    }

    char *marker_path = path_join(sessions_dir, ".prune");
    int marker_fd = open(marker_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (marker_fd < 0 || flock(marker_fd, LOCK_EX | LOCK_NB) != 0) {
        if (marker_fd >= 0)
            close(marker_fd);
        free(marker_path);
        free(sessions_dir);
        return;
    }

    struct stat marker_stat;
    time_t now = time(NULL);
    if (fstat(marker_fd, &marker_stat) == 0 && marker_stat.st_size > 0 &&
        (now <= marker_stat.st_mtime || now - marker_stat.st_mtime < SESSION_PRUNE_INTERVAL_S)) {
        (void)flock(marker_fd, LOCK_UN);
        close(marker_fd);
        free(marker_path);
        free(sessions_dir);
        return;
    }

    struct prune_args *args = xcalloc(1, sizeof(*args));
    args->sessions_dir = sessions_dir;
    args->exclude_path = exclude_path ? xstrdup(exclude_path) : NULL;
    args->marker_path = marker_path;
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

#endif /* _WIN32 */
