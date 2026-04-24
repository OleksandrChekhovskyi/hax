/* SPDX-License-Identifier: MIT */
#include "tool.h"

#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util.h"

#define OUTPUT_CAP (100 * 1024)

static char *run_shell(const char *cmd)
{
    int fds[2];
    if (pipe(fds) < 0)
        return xasprintf("pipe: %s", strerror(errno));

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return xasprintf("fork: %s", strerror(errno));
    }
    if (pid == 0) {
        /* Put the shell in its own process group so we can signal all of
         * its descendants (including jobs backgrounded with `&`) at once. */
        setpgid(0, 0);
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        /* Detach stdin so commands that try to read from the terminal
         * (cat, git commit, python REPL, …) get immediate EOF instead
         * of blocking on the agent's controlling tty. */
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    /* Mirror setpgid in the parent to close the race where we reach
     * waitpid / kill(-pid) before the child's setpgid runs. EACCES after
     * exec is fine — child already has its own group. */
    (void)setpgid(pid, pid);

    close(fds[1]);

    struct buf raw;
    buf_init(&raw);
    int truncated = 0;
    char chunk[4096];
    int shell_exited = 0;
    int status = 0;

    for (;;) {
        /* Check shell status on every iteration — a backgrounded writer
         * like `yes &` keeps the pipe readable indefinitely, so we can't
         * rely on poll() ever timing out. Once the shell is gone, kill
         * the process group so read() can reach EOF. */
        if (!shell_exited) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) {
                shell_exited = 1;
                kill(-pid, SIGKILL);
            } else if (w < 0 && errno != EINTR) {
                break;
            }
        }

        struct pollfd pfd = {.fd = fds[0], .events = POLLIN};
        int pr = poll(&pfd, 1, shell_exited ? 1000 : 200);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0)
            continue; /* re-check shell status */

        ssize_t r = read(fds[0], chunk, sizeof(chunk));
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (r == 0)
            break; /* EOF — all writers gone */
        if (raw.len < OUTPUT_CAP) {
            size_t room = OUTPUT_CAP - raw.len;
            size_t take = (size_t)r < room ? (size_t)r : room;
            buf_append(&raw, chunk, take);
            if ((size_t)r <= take)
                continue;
        }
        /* Cap hit and more data is still arriving. Draining a never-ending
         * producer (yes, tail -f, …) would pin the agent forever, so kill
         * the process group and stop — the post-loop waitpid will reap. */
        truncated = 1;
        if (!shell_exited)
            kill(-pid, SIGKILL);
        break;
    }
    close(fds[0]);

    if (!shell_exited) {
        while (waitpid(pid, &status, 0) < 0) {
            if (errno != EINTR)
                break;
        }
    }

    /* Sanitize output to valid UTF-8 before annotating — binary output
     * and non-UTF-8 bytes would otherwise break JSON serialization. */
    char *clean = sanitize_utf8(raw.data ? raw.data : "", raw.len);
    buf_free(&raw);

    struct buf out;
    buf_init(&out);
    buf_append_str(&out, clean);
    free(clean);

    if (truncated)
        buf_append_str(&out, "\n[output truncated]");

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code != 0) {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "\n[exit %d]", code);
            buf_append_str(&out, tmp);
        }
    } else if (WIFSIGNALED(status)) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "\n[signal %d]", WTERMSIG(status));
        buf_append_str(&out, tmp);
    }

    if (out.len == 0)
        buf_append_str(&out, "(no output)");

    return buf_steal(&out);
}

static char *run(const char *args_json)
{
    json_error_t jerr;
    json_t *root = json_loads(args_json ? args_json : "{}", 0, &jerr);
    if (!root)
        return xasprintf("invalid arguments: %s", jerr.text);

    const char *cmd = json_string_value(json_object_get(root, "command"));
    if (!cmd || !*cmd) {
        json_decref(root);
        return xstrdup("missing 'command' argument");
    }

    char *out = run_shell(cmd);
    json_decref(root);
    return out;
}

const struct tool TOOL_BASH = {
    .def =
        {
            .name = "bash",
            .description = "Run a shell command via /bin/sh -c. Returns combined "
                           "stdout+stderr plus exit code.",
            .parameters_schema_json = "{\"type\":\"object\","
                                      "\"properties\":{\"command\":{\"type\":\"string\","
                                      "\"description\":\"Shell command to run.\"}},"
                                      "\"required\":[\"command\"]}",
            .display_arg = "command",
        },
    .run = run,
};
