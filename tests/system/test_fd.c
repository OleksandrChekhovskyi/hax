/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "harness.h"
#include "late_writer.h"
#include "system/fd.h"

static void test_write_all(void)
{
    int pipe_fds[2];
    int pipe_result = pipe(pipe_fds);
    EXPECT(pipe_result == 0);
    if (pipe_result != 0)
        return;

    const char data[] = "complete write";
    EXPECT(fd_write_all(pipe_fds[1], data, sizeof(data) - 1) == 0);
    close(pipe_fds[1]);

    char result[sizeof(data)] = {0};
    ssize_t bytes_read = read(pipe_fds[0], result, sizeof(result));
    EXPECT(bytes_read == (ssize_t)sizeof(data) - 1);
    if (bytes_read >= 0)
        EXPECT_MEM_EQ(result, (size_t)bytes_read, data, sizeof(data) - 1);
    close(pipe_fds[0]);
}

/* A parent sharing stdin may leave it nonblocking; a pause must not fail or discard the read. */
static void test_read_all_waits_on_nonblocking_pipe(void)
{
    pid_t writer;
    int input = late_writer_pipe("first ", "second", &writer);
    EXPECT(fcntl(input, F_SETFL, fcntl(input, F_GETFL) | O_NONBLOCK) == 0);
    char *contents = fd_read_all(input, SIZE_MAX, FD_READ_WAIT, NULL, NULL);
    EXPECT_STR_EQ(contents, "first second");
    free(contents);
    close(input);
    late_writer_wait(writer);
}

static void test_read_all_fails_fast_without_data(void)
{
    int fds[2];
    EXPECT(pipe(fds) == 0);
    EXPECT(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
    errno = 0;
    EXPECT(fd_read_all(fds[0], SIZE_MAX, FD_READ_FAIL_FAST, NULL, NULL) == NULL);
    EXPECT(errno == EAGAIN || errno == EWOULDBLOCK);
    close(fds[0]);
    close(fds[1]);
}

/* Without a truncation report, a capped read leaves everything past the cap unread. */
static void test_read_all_cap_without_probe(void)
{
    int fds[2];
    EXPECT(pipe(fds) == 0);
    EXPECT(fd_write_all(fds[1], "abc", 3) == 0);
    close(fds[1]);

    char *contents = fd_read_all(fds[0], 1, FD_READ_FAIL_FAST, NULL, NULL);
    EXPECT_STR_EQ(contents, "a");
    free(contents);
    contents = fd_read_all(fds[0], SIZE_MAX, FD_READ_FAIL_FAST, NULL, NULL);
    EXPECT_STR_EQ(contents, "bc");
    free(contents);
    close(fds[0]);
}

static void test_wait_readable(void)
{
    int fds[2];
    EXPECT(pipe(fds) == 0);
    EXPECT(fd_wait_readable(fds[0], 0) == 0);
    EXPECT(fd_write_all(fds[1], "x", 1) == 0);
    EXPECT(fd_wait_readable(fds[0], 0) == 1);
    close(fds[0]);
    close(fds[1]);

    errno = 0;
    EXPECT(fd_wait_readable(fds[0], 0) == -1 && errno == EBADF);
    errno = 0;
    EXPECT(fd_wait_readable(-1, 0) == -1 && errno == EBADF);
}

int main(void)
{
    test_write_all();
    test_read_all_waits_on_nonblocking_pipe();
    test_read_all_fails_fast_without_data();
    test_read_all_cap_without_probe();
    test_wait_readable();
    T_REPORT();
}
