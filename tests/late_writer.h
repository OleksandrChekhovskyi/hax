/* SPDX-License-Identifier: MIT */
#ifndef HAX_TESTS_LATE_WRITER_H
#define HAX_TESTS_LATE_WRITER_H

#include <sys/types.h>

/* Return the read end of a pipe whose forked writer sends `early` at once and `late` after a short
 * pause, then exits, so a reader must keep waiting past the first gap. Pass the writer to
 * late_writer_wait once the read end is drained. */
int late_writer_pipe(const char *early, const char *late, pid_t *writer);

/* Reap the writer and EXPECT that it wrote both parts. */
void late_writer_wait(pid_t writer);

#endif /* HAX_TESTS_LATE_WRITER_H */
