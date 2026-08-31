/* SPDX-License-Identifier: MIT */
#ifndef HAX_TESTS_TOOLS_TASK_HELPERS_H
#define HAX_TESTS_TOOLS_TASK_HELPERS_H

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "buf.h"
#include "harness.h"
#include "tool.h"
#include "xalloc.h"

/* Producers must outlive the yield window to detach. Tests that also need initial output
 * captured before the transition hold it open with HAX_BASH_TRANSITION_MIN_BYTES instead of
 * betting a widened window against spawn latency. */
#define TEST_YIELD "50ms"
#define TEST_PAUSE "0.3"
#define TEST_GRACE "1s"

static char *call_bash_background(const char *escaped_command)
{
    char *args = xasprintf("{\"command\":\"%s\",\"background\":true}", escaped_command);
    char *out = TOOL_BASH.run(args, NULL);
    free(args);
    return out;
}

/* Return the owned "tN" id from a detachment report, or NULL. */
static char *extract_task_id(const char *result)
{
    const char *needle = "task t";
    const char *start = strstr(result, needle);
    if (!start)
        return NULL;
    start += strlen(needle) - 1; /* keep the 't' */
    size_t len = 1;
    while (start[len] >= '0' && start[len] <= '9')
        len++;
    if (len == 1)
        return NULL;
    char *id = xmalloc(len + 1);
    memcpy(id, start, len);
    id[len] = '\0';
    return id;
}

static char *wait_for_id(const char *id, int timeout_seconds)
{
    char *args;
    if (timeout_seconds > 0)
        args = xasprintf("{\"id\":\"%s\",\"timeout_seconds\":%d}", id, timeout_seconds);
    else
        args = xasprintf("{\"id\":\"%s\"}", id);
    char *out = TOOL_TASK_WAIT.run(args, NULL);
    free(args);
    return out;
}

/* Immediate kill-and-collect: task_wait with `kill` and no timeout. */
static char *kill_id(const char *id)
{
    char *args = xasprintf("{\"id\":\"%s\",\"kill\":true}", id);
    char *out = TOOL_TASK_WAIT.run(args, NULL);
    free(args);
    return out;
}

/* ESRCH on Linux or EPERM on Darwin means the process is gone; allow time for the kill to be
 * delivered and the orphan to be reaped. */
static int process_is_gone(int pid)
{
    /* kill(0, sig) and kill(-1, sig) target the caller's process group and every signalable
     * process; a pid from a failed extraction must fail the check, not probe those. */
    if (pid <= 0)
        return 0;
    time_t start = time(NULL);
    while (time(NULL) - start < 10) {
        if (kill(pid, 0) < 0)
            return 1;
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 5 * 1000000L};
        nanosleep(&ts, NULL);
    }
    kill(pid, SIGKILL);
    return 0;
}

/* Wait for a command to write its pid into `path` (bounded), so the test cannot act on the
 * process tree before it reached that point. Returns the pid, or -1. */
static int await_pid_file(const char *path)
{
    time_t start = time(NULL);
    while (time(NULL) - start < 10) {
        int pid = -1;
        FILE *f = fopen(path, "r");
        if (f) {
            if (fscanf(f, "%d", &pid) != 1)
                pid = -1;
            fclose(f);
        }
        if (pid > 0)
            return pid;
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 5 * 1000000L};
        nanosleep(&ts, NULL);
    }
    return -1;
}

/* A fifo the task blocks on with `read -r _ <gate`: it holds the task alive across the yield
 * window without timers, and releasing it lets the task finish instantly. */
static char *gate_create(void)
{
    char *path = xasprintf("%s/gate", t_tempdir());
    EXPECT(mkfifo(path, 0600) == 0);
    return path;
}

/* Nonblocking open with a deadline, so a task that failed to start cannot hang the test. */
static void gate_release(const char *path)
{
    int fd = -1;
    time_t start = time(NULL);
    while (time(NULL) - start < 10) {
        fd = open(path, O_WRONLY | O_NONBLOCK);
        if (fd >= 0 || errno != ENXIO)
            break;
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 3 * 1000000L};
        nanosleep(&ts, NULL);
    }
    EXPECT(fd >= 0);
    if (fd >= 0) {
        EXPECT(write(fd, "\n", 1) == 1);
        close(fd);
    }
}

struct display_capture {
    struct buf buf;
};

static void append_display(const char *bytes, size_t len, void *data)
{
    struct display_capture *capture = data;
    buf_append(&capture->buf, bytes, len);
}

#endif /* HAX_TESTS_TOOLS_TASK_HELPERS_H */
