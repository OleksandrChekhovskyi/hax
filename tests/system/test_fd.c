/* SPDX-License-Identifier: MIT */
#include <string.h>
#include <unistd.h>

#include "harness.h"
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

int main(void)
{
    test_write_all();
    T_REPORT();
}
