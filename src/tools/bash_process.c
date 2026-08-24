/* SPDX-License-Identifier: MIT */
#include "tools/bash_process.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "tool.h"
#include "util.h"
#include "system/spawn.h"
#include "terminal/interrupt.h"
#include "tools/bash_env.h"
#include "tools/bash_output.h"
#include "tools/bash_shell.h"
#include "tools/output_cap.h"
#include "tools/task_registry.h"

struct shell_process {
    struct spawn_process *process;
    int output_fd;
};

static long deadline_after(long now_ms, long duration_ms)
{
    return duration_ms > LONG_MAX - now_ms ? LONG_MAX : now_ms + duration_ms;
}

static void bash_signal_process_tree(struct spawn_process *process, int signal_number)
{
    spawn_process_terminate(process, signal_number);
}

static char *start_shell(const char *command, struct shell_process *process)
{
    char **envp = bash_build_child_env();
    char *shell = NULL;
    const char *argv0 = NULL;
#ifndef _WIN32
    shell = bash_resolve_shell();
    argv0 = strrchr(shell, '/');
    argv0 = argv0 ? argv0 + 1 : shell;
#endif
    process->process = spawn_process_start_bash(shell, argv0, command, envp, &process->output_fd);
    free(shell);
    free(envp);
    if (!process->process)
        return xasprintf("starting Bash: %s", strerror(errno));
    bash_live_process_publish(process->process);
    return NULL;
}

/* Return the grace deadline, or 0 when the process tree was killed immediately. */
static long start_shutdown(struct spawn_process *process, long now_ms, long grace_ms)
{
#ifdef _WIN32
    (void)now_ms;
    (void)grace_ms;
    bash_signal_process_tree(process, SIGKILL);
    return 0;
#else
    if (grace_ms <= 0) {
        bash_signal_process_tree(process, SIGKILL);
        return 0;
    }
    bash_signal_process_tree(process, SIGTERM);
    return deadline_after(now_ms, grace_ms);
#endif
}

static int bash_process_exit_seen(struct spawn_process *process, int *exit_seen)
{
    return spawn_process_exit_seen(process, exit_seen);
}

/* Generous: a loaded machine may schedule the exiting shell late, and the stall lands only on
 * the rare command that closes its output and keeps running. */
#define EOF_EXIT_OBSERVE_MS 1000
/* Short: a background yield's fast-exit check delays the launch report. */
#define YIELD_EXIT_OBSERVE_MS 200

/* Test seam: suites shrink the yield/timeout below process-spawn latency, which turns
 * "initial output was captured before the transition" into a race. Holding the transition
 * until the expected bytes arrive keeps such tests event-driven; the cap bounds a command
 * that never produces them. Env-only on purpose — not a user tunable. */
#define TRANSITION_HOLD_CAP_MS 10000

static size_t transition_min_bytes(void)
{
    const char *value = getenv("HAX_BASH_TRANSITION_MIN_BYTES");
    return value ? (size_t)strtoul(value, NULL, 10) : 0;
}

/* Wait up to `timeout_ms` for the shell's exit to become observable without consuming the
 * process object, so its tree remains terminable. Returns -1 when the wait itself fails. */
static int observe_shell_exit(struct spawn_process *process, int *exit_seen, long timeout_ms)
{
    long deadline = deadline_after(monotonic_ms(), timeout_ms);
    while (!*exit_seen) {
        if (bash_process_exit_seen(process, exit_seen) < 0 && errno != EINTR)
            return -1;
        if (*exit_seen || monotonic_ms() >= deadline)
            break;
        poll(NULL, 0, 10);
    }
    return 0;
}

/* An exiting shell closes its pipe end before its exit becomes waitable, so once the exit is
 * observed a readable pipe carries the shell's final output or EOF, not orphan chatter; it must
 * be read before survivors can be ruled orphans. Bounded so an orphan streaming into the
 * inherited pipe cannot stall the transition indefinitely. */
static int exited_pipe_flush_pending(int output_fd, long *flush_deadline)
{
    long now_ms = monotonic_ms();
    if (!*flush_deadline)
        *flush_deadline = deadline_after(now_ms, YIELD_EXIT_OBSERVE_MS);
    if (now_ms >= *flush_deadline)
        return 0;
    struct pollfd poll_fd = {.fd = output_fd, .events = POLLIN};
    return poll(&poll_fd, 1, 0) > 0;
}

static int poll_timeout_ms(long deadline)
{
    const int default_poll_ms = 10;
    if (deadline <= 0)
        return default_poll_ms;

    long remaining_ms = deadline - monotonic_ms();
    if (remaining_ms <= 0)
        return 0;
    return remaining_ms < default_poll_ms ? (int)remaining_ms : default_poll_ms;
}

static void display_suffix(tool_display_fn display, void *display_data, size_t total_bytes,
                           int binary, int displayed_body, enum bash_stop_reason reason,
                           long timeout_ms, const struct spawn_status *status)
{
    char *suffix =
        bash_output_format_suffix(total_bytes, binary, displayed_body, reason, timeout_ms, status);

    /* A newline aborts any unterminated escape sequence before the binary marker's '['. */
    if (binary && displayed_body)
        display("\n", 1, display_data);
    if (*suffix)
        display(suffix, strlen(suffix), display_data);
    free(suffix);
}

/* Hand the still-running command over to the task registry and build the model-facing launch
 * report. Returns NULL when the handoff is impossible (no spool, no drainer thread); the caller
 * then still owns the process and `output` remains usable, so the kill fallback reports the
 * captured output normally. On success the registry owns the process and the pipe, and `output`
 * may only be destroyed. */
static char *adopt_running_command(const struct shell_process *process, const char *command,
                                   const char *name, struct bash_output *output, int binary,
                                   int background, long started_ms, int pipe_eof,
                                   tool_display_fn display, void *display_data, int displayed_body)
{
    char *spool_path = NULL;
    int spool_fd = bash_output_detach_file(output, &spool_path);
    if (spool_fd < 0)
        return NULL;
    const char *id = task_adopt(process->process, process->output_fd, command, name, started_ms,
                                spool_fd, spool_path, bash_output_size(output), binary, pipe_eof);
    if (!id) {
        bash_output_reattach_file(output, spool_fd, spool_path);
        return NULL;
    }

    struct buf out;
    buf_init(&out);
    char *marker = NULL;
    char *body = task_report_output(id, &marker);
    if (body && *body) {
        buf_append_str(&out, body);
        if (display && !displayed_body) {
            display(body, strlen(body), display_data);
            displayed_body = 1;
        }
    }
    if (marker && *marker) {
        buf_append_str(&out, marker);
        /* The stream never carries the marker, so it is shown even after streamed output. */
        if (display) {
            if (displayed_body && *marker != '\n')
                display("\n", 1, display_data);
            display(marker, strlen(marker), display_data);
            displayed_body = 1;
        }
    }
    if (out.len > 0 && out.data[out.len - 1] != '\n')
        buf_append_str(&out, "\n");
    free(marker);
    free(body);
    /* One compact line, displayed verbatim and stored verbatim, so the live view and the
     * history replay of this result cannot diverge. The notification contract lives in the
     * prompts; the log path is delivered by the truncation markers when content is withheld. */
    char footer[96];
    if (background) {
        snprintf(footer, sizeof(footer), "[detached as task %s]", id);
    } else {
        char elapsed[32];
        format_duration(elapsed, sizeof(elapsed), monotonic_ms() - started_ms);
        snprintf(footer, sizeof(footer), "[detached as task %s after %s timeout]", id, elapsed);
    }
    buf_append_str(&out, footer);

    if (display) {
        if (displayed_body)
            display("\n", 1, display_data);
        display(footer, strlen(footer), display_data);
    }
    return buf_steal(&out);
}

char *bash_run_command(const char *command, long timeout_ms, int background, const char *name,
                       struct tool_run_ctx *ctx)
{
    tool_display_fn display = ctx ? ctx->display : NULL;
    void *display_data = ctx ? ctx->display_data : NULL;
    int tasks_enabled = !config_bool("no_tasks");
    if (!tasks_enabled)
        background = 0;

    struct shell_process process = {0};
    char *error = start_shell(command, &process);
    if (error)
        return error;

    long started_ms = monotonic_ms();
    long transition_ms = background ? config_duration_ms("bash.background_yield") : timeout_ms;
    long deadline = background ? deadline_after(started_ms, transition_ms > 0 ? transition_ms : 0)
                    : timeout_ms > 0 ? deadline_after(started_ms, timeout_ms)
                                     : 0;
    long grace_ms = config_duration_ms("bash.timeout_grace");
    size_t hold_min_bytes = transition_min_bytes();
    long hold_cap = hold_min_bytes ? deadline_after(started_ms, TRANSITION_HOLD_CAP_MS) : 0;
    long grace_deadline = 0;
    long exit_flush_deadline = 0;
    enum bash_stop_reason stop_reason = BASH_STOP_NONE;
    int shell_exited = 0;
    struct spawn_status status = {.end = SPAWN_END_FORCED};
    int binary = 0;
    int displayed_body = 0;
    struct bash_output *output = bash_output_create(output_cap_bytes());
    char chunk[4096];

    for (;;) {
        long now_ms = monotonic_ms();

        /* A held transition stretches toward the cap until the expected bytes arrive. */
        long transition_deadline = deadline;
        if (deadline > 0 && bash_output_size(output) < hold_min_bytes && hold_cap > deadline)
            transition_deadline = hold_cap;

        /* Probe before the deadline check so adoption never takes a shell that has already
         * exited. The process object stays owned until all tree termination is done. */
        if (!shell_exited) {
            int status_result = bash_process_exit_seen(process.process, &shell_exited);
            /* Reaping stragglers on shell exit would defeat an explicit background request,
             * where lingering children are the point. */
            if (shell_exited && stop_reason == BASH_STOP_NONE && !background)
                bash_signal_process_tree(process.process, SIGKILL);
            else if (status_result < 0 && errno != EINTR)
                break;
        }

        /* User interruption wins if it coincides with the timeout. */
        if (stop_reason == BASH_STOP_NONE && interrupt_abort_requested()) {
            stop_reason = BASH_STOP_INTERRUPT;
            if (shell_exited) {
                /* Background suppressed the shell-exit kill; orphans die on the way out. */
                if (background)
                    bash_signal_process_tree(process.process, SIGKILL);
                break;
            }
            grace_deadline = start_shutdown(process.process, now_ms, grace_ms);
            if (grace_deadline == 0)
                break;
        }
        if (stop_reason == BASH_STOP_NONE && transition_deadline > 0 &&
            now_ms >= transition_deadline) {
            /* A held transition releases on the arrival of bytes an exiting shell wrote
             * moments before its exit becomes waitable; let the exit land so the
             * adopt-vs-orphan choice below reflects the shell's state, not that race. */
            if (background && hold_min_bytes && !shell_exited)
                observe_shell_exit(process.process, &shell_exited, YIELD_EXIT_OBSERVE_MS);
            int flush_pending = background && shell_exited &&
                                exited_pipe_flush_pending(process.output_fd, &exit_flush_deadline);
            if (!flush_pending) {
                /* Adoption needs a live shell: the registry watches and kills the task
                 * through it. Once it has exited, only orphans hold the pipe, and they must
                 * not outlive the call untracked. */
                if (tasks_enabled && !shell_exited) {
                    char *adopted =
                        adopt_running_command(&process, command, name, output, binary, background,
                                              started_ms, 0, display, display_data, displayed_body);
                    if (adopted) {
                        bash_output_destroy(output);
                        return adopted;
                    }
                }
                stop_reason = BASH_STOP_TIMEOUT;
                if (shell_exited) {
                    if (background) {
                        stop_reason = BASH_STOP_ORPHANED;
                        bash_signal_process_tree(process.process, SIGKILL);
                    }
                    break;
                }
                grace_deadline = start_shutdown(process.process, now_ms, grace_ms);
                if (grace_deadline == 0)
                    break;
            }
        }
        if (stop_reason != BASH_STOP_NONE && grace_deadline > 0 && now_ms >= grace_deadline) {
            bash_signal_process_tree(process.process, SIGKILL);
            break;
        }

        long active_deadline = stop_reason == BASH_STOP_NONE ? transition_deadline : grace_deadline;
        struct pollfd poll_fd = {.fd = process.output_fd, .events = POLLIN};
        int poll_result = poll(&poll_fd, 1, poll_timeout_ms(active_deadline));
        if (poll_result < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (poll_result == 0)
            continue;

        ssize_t bytes_read = read(process.output_fd, chunk, sizeof(chunk));
        if (bytes_read < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (bytes_read == 0) {
            /* A backgrounded command's descendants may outlive the pipe on purpose. */
            if (background && stop_reason == BASH_STOP_NONE)
                break;
            /* EOF cannot yield more cleanup output, so no grace period remains useful. But a
             * clean exit closes the pipe before it is waitable (coreutils fcloses stdout in an
             * atexit handler); a kill in that window would rewrite the exit status to SIGKILL,
             * so let a finishing shell's exit land first. */
            if (stop_reason == BASH_STOP_NONE)
                observe_shell_exit(process.process, &shell_exited, EOF_EXIT_OBSERVE_MS);
            bash_signal_process_tree(process.process, SIGKILL);
            break;
        }

        if (!binary && memchr(chunk, '\0', (size_t)bytes_read))
            binary = 1;
        if (display && !binary) {
            display(chunk, (size_t)bytes_read, display_data);
            displayed_body = 1;
        }
        bash_output_append(output, chunk, (size_t)bytes_read);
        if (bash_output_size(output) >= (size_t)BASH_OUTPUT_DRAIN_LIMIT) {
            bash_signal_process_tree(process.process, SIGKILL);
            break;
        }
    }
    /* Background EOF leaves the tree unsignalled: give a fast exit a moment to be observed
     * and return a plain synchronous result; a survivor (a daemon that closed its output)
     * detaches as a task instead. */
    if (background && stop_reason == BASH_STOP_NONE) {
        int observe_failed =
            observe_shell_exit(process.process, &shell_exited, YIELD_EXIT_OBSERVE_MS) < 0;
        if (!shell_exited && !observe_failed) {
            char *adopted =
                adopt_running_command(&process, command, name, output, binary, background,
                                      started_ms, 1, display, display_data, displayed_body);
            if (adopted) {
                bash_output_destroy(output);
                return adopted;
            }
        }
        /* An exited shell may leave orphans that redirected their output elsewhere; nothing
         * can track them, so kill the group while the unreaped zombie still reserves it. */
        bash_signal_process_tree(process.process, SIGKILL);
    }

    close(process.output_fd);

    bash_live_process_retract(process.process);
    (void)spawn_process_wait(process.process, &status);
    process.process = NULL;

    /* A background request that completed inside the yield window never created a task; name
     * the handle the model might otherwise wait on. The note is model-only: the user never saw
     * the background request, so their view reads as an ordinary synchronous call. */
    char task_footer[96] = "";
    if (background && stop_reason == BASH_STOP_NONE) {
        if (name)
            snprintf(task_footer, sizeof(task_footer),
                     "\n[finished during launch; task %s not created]", name);
        else
            snprintf(task_footer, sizeof(task_footer),
                     "\n[finished during launch; no task created]");
    }

    /* Status suffixes report the deadline that was actually armed: the background yield for
     * background runs, the timeout otherwise (transition_ms covers both). */
    if (display)
        display_suffix(display, display_data, bash_output_size(output), binary, displayed_body,
                       stop_reason, transition_ms, &status);
    char *result = bash_output_finish(output, binary, stop_reason, transition_ms, &status);
    if (*task_footer) {
        char *with_footer = xasprintf("%s%s", result, task_footer);
        free(result);
        result = with_footer;
        if (ctx)
            ctx->output_hidden_tail = strlen(task_footer);
    }
    bash_output_destroy(output);
    return result;
}

/* Exceeds task.max_running's ceiling (64) plus the one foreground shell, so a free slot always
 * exists and publish cannot silently drop a shell. */
#define LIVE_PROCESS_TABLE_SIZE 128

/* Single-writer (the tool-dispatch thread); fatal cleanup may read concurrently. Every pointer
 * remains valid until it is retracted immediately before the consuming wait. */
static struct spawn_process *volatile live_processes[LIVE_PROCESS_TABLE_SIZE];

void bash_live_process_publish(struct spawn_process *process)
{
    for (size_t i = 0; i < LIVE_PROCESS_TABLE_SIZE; i++) {
        if (!live_processes[i]) {
            live_processes[i] = process;
            return;
        }
    }
}

void bash_live_process_retract(struct spawn_process *process)
{
    for (size_t i = 0; i < LIVE_PROCESS_TABLE_SIZE; i++) {
        if (live_processes[i] == process) {
            live_processes[i] = NULL;
            return;
        }
    }
}

void bash_shell_pgids_kill(void)
{
    for (size_t i = 0; i < LIVE_PROCESS_TABLE_SIZE; i++) {
        struct spawn_process *process = live_processes[i];
        if (process)
            bash_signal_process_tree(process, SIGKILL);
    }
}
