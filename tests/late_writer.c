/* SPDX-License-Identifier: MIT */
#include "late_writer.h"

#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#include "harness.h"

static int write_text(int fd, const char *text)
{
    return write(fd, text, strlen(text)) == (ssize_t)strlen(text);
}

int late_writer_pipe(const char *early, const char *late, pid_t *writer)
{
    int fds[2];
    EXPECT(pipe(fds) == 0);
    *writer = fork();
    EXPECT(*writer >= 0);
    if (*writer == 0) {
        close(fds[0]);
        int written = write_text(fds[1], early);
        struct timespec delay = {.tv_nsec = 50 * 1000 * 1000};
        nanosleep(&delay, NULL);
        written = written && write_text(fds[1], late);
        _exit(written ? 0 : 1);
    }
    close(fds[1]);
    return fds[0];
}

void late_writer_wait(pid_t writer)
{
    int status;
    EXPECT(waitpid(writer, &status, 0) == writer && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
