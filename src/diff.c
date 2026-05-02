/* SPDX-License-Identifier: MIT */
#include "diff.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "utf8_sanitize.h"
#include "util.h"

static char *tmpdir(void)
{
    const char *t = getenv("TMPDIR");
    if (!t || !*t)
        t = "/tmp";
    return xasprintf("%s/hax-diff-XXXXXX", t);
}

char *make_unified_diff(const char *a, size_t a_len, const char *b, size_t b_len,
                        const char *a_label, const char *b_label)
{
    char *path_a = tmpdir();
    char *path_b = tmpdir();
    int fd_a = -1, fd_b = -1;
    int pipefd[2] = {-1, -1};
    pid_t pid = -1;
    struct buf out;
    buf_init(&out);

    fd_a = mkstemp(path_a);
    if (fd_a < 0)
        goto err;
    fd_b = mkstemp(path_b);
    if (fd_b < 0)
        goto err;
    if (write_all(fd_a, a, a_len) < 0)
        goto err;
    if (write_all(fd_b, b, b_len) < 0)
        goto err;
    close(fd_a);
    fd_a = -1;
    close(fd_b);
    fd_b = -1;

    if (pipe(pipefd) < 0)
        goto err;

    pid = fork();
    if (pid < 0)
        goto err;
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        /* -a forces text mode so a stray NUL byte doesn't trip diff(1)
         * into "Binary files X and Y differ" — that would leave the
         * file modified but the tool result un-renderable as a diff.
         * Embedded NULs are then mopped up by sanitize_utf8 below.
         *
         * No --label here: it's a GNU/BSD extension and absent from
         * POSIX, busybox, etc. Since this path is now load-bearing for
         * every successful write/edit, we instead post-process the
         * first two header lines below to inject our labels. */
        execlp("diff", "diff", "-u", "-a", path_a, path_b, (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);
    pipefd[1] = -1;
    char chunk[4096];
    for (;;) {
        ssize_t r = read(pipefd[0], chunk, sizeof(chunk));
        if (r < 0) {
            if (errno == EINTR)
                continue;
            goto err;
        }
        if (r == 0)
            break;
        buf_append(&out, chunk, (size_t)r);
    }
    close(pipefd[0]);
    pipefd[0] = -1;

    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            goto err;
    }
    pid = -1;

    unlink(path_a);
    unlink(path_b);
    free(path_a);
    free(path_b);

    /* diff(1) exit codes: 0 = identical, 1 = different, >1 = error. */
    if (!WIFEXITED(status) || WEXITSTATUS(status) > 1) {
        buf_free(&out);
        return NULL;
    }

    if (!out.data)
        return xstrdup("");

    /* Replace the first two header lines with our labels. diff(1) emits
     * the temp paths there ("--- /tmp/hax-diff-XXX\tDATE\n+++ ..."); we
     * rewrite them to "--- a/<a_label>\n+++ b/<b_label>\n" — same shape
     * git would produce and what the LLM tool result wants. Falls back
     * to the raw output if the buffer doesn't have at least two newlines
     * (defensive; standard diff(1) always emits both). */
    const char *eol1 = memchr(out.data, '\n', out.len);
    if (eol1) {
        size_t after1 = (size_t)(eol1 - out.data) + 1;
        const char *eol2 = memchr(out.data + after1, '\n', out.len - after1);
        if (eol2) {
            size_t after2 = (size_t)(eol2 - out.data) + 1;
            struct buf relabeled;
            buf_init(&relabeled);
            buf_append_str(&relabeled, "--- ");
            buf_append_str(&relabeled, a_label);
            buf_append_str(&relabeled, "\n+++ ");
            buf_append_str(&relabeled, b_label);
            buf_append(&relabeled, "\n", 1);
            buf_append(&relabeled, out.data + after2, out.len - after2);
            buf_free(&out);
            out.data = relabeled.data;
            out.len = relabeled.len;
            out.cap = relabeled.cap;
        }
    }

    /* The new side is already valid UTF-8 (model passed it as a JSON
     * string), but the old side copies raw bytes from disk into '-' and
     * context lines. Latin-1 or otherwise non-UTF-8 input would otherwise
     * propagate into the tool result and break jansson encoding on the
     * next turn. Replacing invalid sequences with U+FFFD here keeps the
     * line structure intact (only byte counts change). */
    char *clean = sanitize_utf8(out.data, out.len);
    buf_free(&out);
    return clean;

err: {
    int saved = errno;
    if (fd_a >= 0)
        close(fd_a);
    if (fd_b >= 0)
        close(fd_b);
    if (pipefd[0] >= 0)
        close(pipefd[0]);
    if (pipefd[1] >= 0)
        close(pipefd[1]);
    if (pid > 0)
        waitpid(pid, NULL, 0);
    unlink(path_a);
    unlink(path_b);
    free(path_a);
    free(path_b);
    buf_free(&out);
    errno = saved;
    return NULL;
}
}
