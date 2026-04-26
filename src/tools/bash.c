/* SPDX-License-Identifier: MIT */
#include "tool.h"

#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "interrupt.h"
#include "util.h"

#define OUTPUT_CAP                  (100 * 1024)
#define BASH_TIMEOUT_DEFAULT_MS     (120L * 1000L)
#define BASH_TIMEOUT_MAX_DEFAULT_MS (1800L * 1000L)
#define BASH_GRACE_DEFAULT_MS       (2L * 1000L)

/* Read a duration env var (ms/s/m/h suffix or bare seconds). 0 disables
 * the guard; unset or unparseable falls back to `fallback_ms`. */
static long parse_timeout_env_ms(const char *name, long fallback_ms)
{
    const char *s = getenv(name);
    if (!s || !*s)
        return fallback_ms;
    long v = parse_duration_ms(s);
    return v < 0 ? fallback_ms : v;
}

static long resolve_default_timeout_ms(void)
{
    return parse_timeout_env_ms("HAX_BASH_TIMEOUT", BASH_TIMEOUT_DEFAULT_MS);
}

static long resolve_max_timeout_ms(void)
{
    return parse_timeout_env_ms("HAX_BASH_TIMEOUT_MAX", BASH_TIMEOUT_MAX_DEFAULT_MS);
}

static long resolve_grace_ms(void)
{
    return parse_timeout_env_ms("HAX_BASH_TIMEOUT_GRACE", BASH_GRACE_DEFAULT_MS);
}

static long monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Saturating add for non-negative longs. A configured duration near
 * LONG_MAX (e.g. HAX_BASH_TIMEOUT="9223372036854775000ms" or a per-call
 * arg with the max ceiling disabled) would otherwise overflow `now +
 * delta` — UB, and on typical wraparound the result is a large negative
 * deadline that fires the timeout on iteration zero. */
static long sat_add(long now, long delta)
{
    return delta > LONG_MAX - now ? LONG_MAX : now + delta;
}

/* Render the configured timeout for the model. Whole seconds use "Ns"
 * for readability; sub-second durations stay in "Nms" so they aren't
 * silently rounded to "0s". */
static void format_timeout_for_model(char *buf, size_t buflen, long timeout_ms)
{
    if (timeout_ms % 1000 == 0)
        snprintf(buf, buflen, "%lds", timeout_ms / 1000);
    else
        snprintf(buf, buflen, "%ldms", timeout_ms);
}

/* Build the final tool result from the captured raw output and exit
 * metadata. Consumes `raw` (calls buf_free on it). */
static char *format_run_output(struct buf *raw, int truncated, int timed_out, int interrupted,
                               long timeout_ms, int status)
{
    /* Sanitize output to valid UTF-8 before annotating — binary output
     * and non-UTF-8 bytes would otherwise break JSON serialization. */
    char *clean = sanitize_utf8(raw->data ? raw->data : "", raw->len);
    buf_free(raw);

    struct buf out;
    buf_init(&out);
    buf_append_str(&out, clean);
    free(clean);

    if (truncated)
        buf_append_str(&out, "\n[output truncated]");

    /* Interrupt takes precedence over timeout in the footer — if both
     * fire (rare: deadline hits during the grace window of a user-Esc),
     * the user-driven cause is the more useful explanation. */
    if (interrupted) {
        buf_append_str(&out, "\n[interrupted]");
    } else if (timed_out) {
        char human[32];
        format_timeout_for_model(human, sizeof(human), timeout_ms);
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "\n[timed out after %s]", human);
        buf_append_str(&out, tmp);
    } else if (WIFEXITED(status)) {
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

/* Runs in the forked child. Replaces this process with /bin/sh -c cmd,
 * with stdout/stderr going to write_fd and stdin pinned to /dev/null.
 * Never returns. */
static void exec_shell_child(int read_fd, int write_fd, const char *cmd)
{
    /* Put the shell in its own process group so we can signal all of
     * its descendants (including jobs backgrounded with `&`) at once. */
    setpgid(0, 0);
    close(read_fd);
    dup2(write_fd, STDOUT_FILENO);
    dup2(write_fd, STDERR_FILENO);
    close(write_fd);
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

static char *run_shell(const char *cmd, long timeout_ms)
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
    if (pid == 0)
        exec_shell_child(fds[0], fds[1], cmd); /* never returns */

    /* Mirror setpgid in the parent to close the race where we reach
     * waitpid / kill(-pid) before the child's setpgid runs. EACCES after
     * exec is fine — child already has its own group. */
    (void)setpgid(pid, pid);
    close(fds[1]);

    long deadline = timeout_ms > 0 ? sat_add(monotonic_ms(), timeout_ms) : 0;
    long grace_ms = resolve_grace_ms();
    long grace_deadline = 0;
    int timed_out = 0;
    int interrupted = 0;
    int term_sent = 0;
    int shell_exited = 0;
    int status = 0;

    struct buf raw;
    buf_init(&raw);
    int truncated = 0;
    char chunk[4096];

    for (;;) {
        long now = monotonic_ms();

        /* User-Esc interrupt: same two-stage shutdown as the timeout path
         * (SIGTERM → grace → SIGKILL). Checked before the deadline so the
         * footer correctly attributes the cause when both fire. The flag
         * latches, so once set we don't re-enter this branch. */
        if (!term_sent && interrupt_requested()) {
            interrupted = 1;
            term_sent = 1;
            if (shell_exited)
                break;
            if (grace_ms > 0) {
                kill(-pid, SIGTERM);
                grace_deadline = sat_add(now, grace_ms);
            } else {
                kill(-pid, SIGKILL);
                break;
            }
        }

        /* First-stage timeout: a runaway command (slow build, hung network
         * call, infinite loop) would otherwise pin the agent forever. Send
         * SIGTERM so well-behaved commands flush output, drop locks, and
         * unwind cleanly; the loop keeps draining output during the grace
         * window. With grace disabled we go straight to SIGKILL. */
        if (!term_sent && deadline > 0 && now >= deadline) {
            timed_out = 1;
            term_sent = 1;
            if (shell_exited)
                break;
            if (grace_ms > 0) {
                kill(-pid, SIGTERM);
                grace_deadline = sat_add(now, grace_ms);
            } else {
                kill(-pid, SIGKILL);
                break;
            }
        }

        /* Second-stage timeout: grace expired. SIGKILL unconditionally
         * — we deferred the post-shell-exit pgroup-collapse during grace
         * (see below), so stragglers may still be alive even when
         * shell_exited is set. */
        if (term_sent && grace_deadline > 0 && now >= grace_deadline) {
            kill(-pid, SIGKILL);
            break;
        }

        /* Check shell status on every iteration — a backgrounded writer
         * like `yes &` keeps the pipe readable indefinitely, so we can't
         * rely on poll() ever timing out. Once the shell is gone, kill
         * the process group so read() can reach EOF — but skip that
         * during the grace window, when backgrounded children may be
         * mid-cleanup (e.g. `(trap '...' TERM; sleep) & wait` where the
         * outer shell exits immediately on SIGTERM but the subshell's
         * trap is still running). EOF arrives naturally when they
         * finish; the second-stage check above SIGKILLs any stragglers. */
        if (!shell_exited) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) {
                shell_exited = 1;
                if (!term_sent)
                    kill(-pid, SIGKILL);
            } else if (w < 0 && errno != EINTR) {
                break;
            }
        }

        /* 10ms baseline keeps deadline checks responsive without
         * meaningful CPU cost — poll() with no fd activity blocks in
         * the kernel. The clamp below shortens it further when a
         * deadline is approaching. */
        struct pollfd pfd = {.fd = fds[0], .events = POLLIN};
        int poll_ms = 10;
        long active_deadline = term_sent ? grace_deadline : deadline;
        if (active_deadline > 0) {
            long remaining = active_deadline - monotonic_ms();
            if (remaining < 0)
                remaining = 0;
            if (remaining < poll_ms)
                poll_ms = (int)remaining;
        }
        int pr = poll(&pfd, 1, poll_ms);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0)
            continue; /* re-check shell status / deadline */

        ssize_t r = read(fds[0], chunk, sizeof(chunk));
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (r == 0) {
            /* EOF — all pipe writers closed. SIGKILL the pgroup
             * unconditionally so we don't leak redirected/detached
             * descendants. The motivating case is `sleep 300
             * >/dev/null 2>&1 &`: shell forks sleep with redirected
             * fds, backgrounds, exits. Pipe closes (shell was the
             * last writer), EOF arrives in parent, but kernel-level
             * fd close can propagate to us before waitpid(WNOHANG)
             * sees the zombie — without this kill, sleep would
             * survive past our return. ESRCH/EPERM on an empty
             * pgroup is harmless, and the prior waitpid status
             * (when present) is preserved since SIGKILL on a zombie
             * is a no-op. The edge case of a deliberately detached
             * shell with self-redirected stdout (`exec >/dev/null;
             * long_cmd`) also gets killed here — acceptable since
             * the tool is meant for output capture and the global
             * timeout would kill it anyway. */
            kill(-pid, SIGKILL);
            break;
        }
        if (raw.len < OUTPUT_CAP) {
            size_t room = OUTPUT_CAP - raw.len;
            size_t take = (size_t)r < room ? (size_t)r : room;
            buf_append(&raw, chunk, take);
            if ((size_t)r <= take)
                continue;
        }
        /* Cap hit and more data is still arriving. Draining a never-ending
         * producer (yes, tail -f, …) would pin the agent forever, so kill
         * the process group and stop. SIGKILL unconditionally: when we're
         * in the grace window we deliberately deferred the pgroup-collapse
         * so cleanup could finish, but a runaway descendant ignoring
         * SIGTERM can still flood past OUTPUT_CAP. ESRCH on an empty
         * pgroup is harmless. */
        truncated = 1;
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

    return format_run_output(&raw, truncated, timed_out, interrupted, timeout_ms, status);
}

/* Resolve the timeout in ms from the JSON args, falling back to the env
 * default. The arg is integer seconds (clean model UX); env-var max is
 * parsed in ms so tests and ops can use any duration unit. The schema
 * advertises the harness ceiling, but the model can't read env vars,
 * so we silently clamp rather than reject and force a retry. Returns
 * NULL on success (with *out_ms set), or an allocated error message
 * the caller surfaces to the model. */
static char *resolve_call_timeout_ms(json_t *root, long *out_ms)
{
    json_t *jt = json_object_get(root, "timeout_seconds");
    if (!jt) {
        *out_ms = resolve_default_timeout_ms();
        return NULL;
    }
    if (!json_is_integer(jt))
        return xstrdup("'timeout_seconds' must be an integer");
    long secs = (long)json_integer_value(jt);
    if (secs < 1)
        return xstrdup("'timeout_seconds' must be >= 1");
    /* Saturate before multiplying — signed overflow is UB and could wrap
     * to a negative timeout_ms that silently disables the timeout. The
     * clamp below brings this back down if a max is configured. */
    long timeout_ms = secs > LONG_MAX / 1000L ? LONG_MAX : secs * 1000L;
    long max_ms = resolve_max_timeout_ms();
    if (max_ms > 0 && timeout_ms > max_ms)
        timeout_ms = max_ms;
    *out_ms = timeout_ms;
    return NULL;
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

    long timeout_ms = 0;
    char *err = resolve_call_timeout_ms(root, &timeout_ms);
    if (err) {
        json_decref(root);
        return err;
    }

    char *out = run_shell(cmd, timeout_ms);
    json_decref(root);
    return out;
}

const struct tool TOOL_BASH = {
    .def =
        {
            .name = "bash",
            .description =
                "Run a shell command via /bin/sh -c. Returns combined stdout+stderr plus "
                "exit code. Default timeout is 120s; pass `timeout_seconds` for slow "
                "commands (test suites, builds). The harness enforces a hard ceiling.",
            .parameters_schema_json =
                "{\"type\":\"object\","
                "\"properties\":{"
                "\"command\":{\"type\":\"string\","
                "\"description\":\"Shell command to run.\"},"
                "\"timeout_seconds\":{\"type\":\"integer\",\"minimum\":1,"
                "\"description\":\"Optional override of the default timeout. "
                "Use a higher value for slow builds or test suites; the harness "
                "clamps to a configured maximum.\"}"
                "},"
                "\"required\":[\"command\"]}",
            .display_arg = "command",
        },
    .run = run,
};
