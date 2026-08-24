/* SPDX-License-Identifier: MIT */
#include "tools/task_registry.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "tool.h"
#include "util.h"
#include "system/bg_job.h"
#include "system/spawn.h"
#include "terminal/interrupt.h"
#include "text/utf8_sanitize.h"
#include "text/width.h"
#include "tools/bash_output.h"
#include "tools/bash_process.h"
#include "tools/output_cap.h"

#define TASK_POLL_INTERVAL_MS   20
#define TASK_COMMAND_HEAD_CELLS 60
/* Margin past the SIGTERM grace for SIGKILL delivery and the exit to be observed. */
#define TASK_KILL_MARGIN_MS 3000

#define TASK_NAME_MAX 32

struct task {
    struct task *next;
    char id[TASK_NAME_MAX + 1];
    char *command;
    struct spawn_process *process;
    int pipe_fd;  /* -1 once the drainer is joined */
    int spool_fd; /* -1 when the spool could not be created */
    char *spool_path;
    long started_ms;
    long finished_ms;
    struct bg_job *drainer; /* NULL when the pipe was already drained at adopt */

    pthread_mutex_t lock; /* guards the drainer-shared block below */
    size_t total_bytes;   /* bytes the command produced */
    size_t spooled_bytes; /* prefix of total_bytes actually written to the spool */
    int binary;
    int eof;
    long eof_ms; /* when the drainer saw EOF — the closest stamp to the real finish */
    int spool_write_failed;
    int overflow; /* produced past BASH_OUTPUT_DRAIN_LIMIT; excess discarded */

    /* Main thread only. */
    int exit_seen;      /* shell exit observed unreaped; the zombie keeps the group signalable */
    int done;           /* shell reaped after pipe EOF; status valid; no further termination */
    int orphans_killed; /* descendants outlived the shell holding the pipe and were killed */
    struct spawn_status status;
    long kill_deadline_ms; /* nonzero after SIGTERM until escalation */
    /* Completion is announced once (notified) but the task stays collectable until a wait
     * delivers its remaining output (collected); only then is it forgotten. */
    int notified;
    int collected;
    size_t delivered_bytes;
};

static struct task *tasks;
static int next_task_number = 1;

struct task_shared_snapshot {
    size_t total_bytes;
    size_t spooled_bytes;
    int binary;
    int eof;
    long eof_ms;
    int spool_write_failed;
    int overflow;
};

static void task_snapshot(struct task *t, struct task_shared_snapshot *snap)
{
    pthread_mutex_lock(&t->lock);
    snap->total_bytes = t->total_bytes;
    snap->spooled_bytes = t->spooled_bytes;
    snap->binary = t->binary;
    snap->eof = t->eof;
    snap->eof_ms = t->eof_ms;
    snap->spool_write_failed = t->spool_write_failed;
    snap->overflow = t->overflow;
    pthread_mutex_unlock(&t->lock);
}

/* The worker owns and frees only the boxed pointer; the task itself outlives the join. */
static void task_drain(struct bg_job *job, void *arg)
{
    struct task *t = *(struct task **)arg;
    free(arg);
    char chunk[4096];
    for (;;) {
        if (bg_job_cancel_requested(job))
            break;
        struct pollfd pfd = {.fd = t->pipe_fd, .events = POLLIN};
        int poll_result = poll(&pfd, 1, 100);
        if (poll_result < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (poll_result == 0)
            continue;
        ssize_t bytes_read = read(t->pipe_fd, chunk, sizeof(chunk));
        if (bytes_read < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (bytes_read == 0)
            break;

        pthread_mutex_lock(&t->lock);
        if (!t->binary && memchr(chunk, '\0', (size_t)bytes_read))
            t->binary = 1;
        t->total_bytes += (size_t)bytes_read;
        int just_overflowed = 0;
        if (!t->overflow && t->total_bytes >= (size_t)BASH_OUTPUT_DRAIN_LIMIT) {
            t->overflow = 1;
            just_overflowed = 1;
        }
        int write_spool = t->spool_fd >= 0 && !t->spool_write_failed && !t->overflow;
        pthread_mutex_unlock(&t->lock);

        /* Stop a runaway producer here rather than on the main thread's next poll, which may
         * be minutes away. Safe: the reap is gated on the EOF this thread has not set yet. */
        if (just_overflowed)
            spawn_process_terminate(t->process, SIGKILL);

        /* write(2) outside the lock: the fd is drainer-owned for writing, and readers use
         * pread. Failure and progress land back under the lock. */
        int write_failed = 0;
        if (write_spool && write_all(t->spool_fd, chunk, (size_t)bytes_read) < 0)
            write_failed = 1;
        pthread_mutex_lock(&t->lock);
        if (write_spool && !write_failed)
            t->spooled_bytes += (size_t)bytes_read;
        if (write_failed)
            t->spool_write_failed = 1;
        pthread_mutex_unlock(&t->lock);
    }
    pthread_mutex_lock(&t->lock);
    t->eof = 1;
    t->eof_ms = monotonic_ms();
    pthread_mutex_unlock(&t->lock);
}

/* Advance main-thread state: observe a shell exit, escalate an expired SIGTERM, and finish the
 * task. The task is done only when the exited shell's pipe also hit EOF, so group members that
 * outlive the shell (a slow TERM handler's children, orphans of a raced adoption) stay watched
 * and killable. Never terminates after the consuming wait; deferring that wait until EOF keeps
 * the owned process tree addressable for this whole transition. */
static void task_poll(struct task *t)
{
    long now = monotonic_ms();
    struct task_shared_snapshot snap;
    task_snapshot(t, &snap);

    if (!t->exit_seen) {
        if (spawn_process_exit_seen(t->process, &t->exit_seen) < 0 && errno == ECHILD)
            t->exit_seen = 1;
        /* Orphans holding the pipe past the shell's exit are killed, matching the launch
         * path's policy; an armed TERM grace defers to the escalation instead. */
        if (t->exit_seen && !snap.eof && !t->kill_deadline_ms) {
            spawn_process_terminate(t->process, SIGKILL);
            t->orphans_killed = 1;
        }
    }
    if (!t->done && t->kill_deadline_ms && now >= t->kill_deadline_ms) {
        spawn_process_terminate(t->process, SIGKILL);
        t->kill_deadline_ms = 0;
    }
    if (t->drainer && snap.eof) {
        bg_job_join(t->drainer);
        t->drainer = NULL;
        close(t->pipe_fd);
        t->pipe_fd = -1;
    }
    if (!t->done && t->exit_seen && snap.eof) {
        /* An armed TERM grace outranks the completion kill: unobservable children may be
         * mid-cleanup, so completion waits for the escalation at the deadline. */
        if (t->kill_deadline_ms)
            return;
        /* Anything still in the group closed its output and is untrackable; kill it while
         * the process object still owns the tree. Usually a no-op on the exited root. */
        spawn_process_terminate(t->process, SIGKILL);
        bash_live_process_retract(t->process);
        if (spawn_process_wait(t->process, &t->status) < 0)
            t->status = (struct spawn_status){.end = SPAWN_END_FORCED};
        t->process = NULL;
        t->done = 1;
        /* The reap may run long after the exit (the next prompt, minutes later); the
         * drainer's EOF stamp is the closest observation of the real finish. */
        t->finished_ms = snap.eof_ms ? snap.eof_ms : now;
        t->kill_deadline_ms = 0;
    }
}

static void task_poll_all(void)
{
    for (struct task *t = tasks; t; t = t->next)
        task_poll(t);
}

static void task_free(struct task *t)
{
    if (t->drainer) {
        bg_job_cancel(t->drainer);
        bg_job_join(t->drainer);
    }
    if (t->pipe_fd >= 0)
        close(t->pipe_fd);
    if (t->spool_fd >= 0)
        close(t->spool_fd);
    pthread_mutex_destroy(&t->lock);
    free(t->command);
    free(t->spool_path);
    free(t);
}

/* Forget tasks whose remaining output has been collected and whose drainer is done. The spool
 * file stays on disk: its path is in the conversation and tempfiles cleanup owns it. */
static void task_sweep(void)
{
    struct task **link = &tasks;
    while (*link) {
        struct task *t = *link;
        if (t->collected && t->done) {
            *link = t->next;
            task_free(t);
        } else {
            link = &t->next;
        }
    }
}

static struct task *task_find(const char *id)
{
    for (struct task *t = tasks; t; t = t->next)
        if (!t->collected && strcmp(t->id, id) == 0)
            return t;
    return NULL;
}

static int task_id_in_use(const char *id)
{
    for (struct task *t = tasks; t; t = t->next)
        if (!t->collected && strcmp(t->id, id) == 0)
            return 1;
    return 0;
}

static int name_is_reserved_shape(const char *name)
{
    if (name[0] != 't' || name[1] == '\0')
        return 0;
    for (size_t i = 1; name[i]; i++)
        if (name[i] < '0' || name[i] > '9')
            return 0;
    return 1;
}

char *task_name_error(const char *name)
{
    if (!name || !*name)
        return xstrdup("'name' must not be empty");
    if (strlen(name) > TASK_NAME_MAX)
        return xasprintf("'name' too long (max %d characters)", TASK_NAME_MAX);
    for (size_t i = 0; name[i]; i++) {
        char c = name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == '-' || c == '_';
        if (!ok)
            return xstrdup("'name' may contain only letters, digits, '-' and '_'");
    }
    if (name_is_reserved_shape(name))
        return xstrdup("'name' must not look like an automatic task id (t<number>)");
    if (task_id_in_use(name))
        return xasprintf("task name '%s' is already in use", name);
    return NULL;
}

const char *task_adopt(struct spawn_process *process, int pipe_fd, const char *command,
                       const char *name, long started_ms, int spool_fd, char *spool_path,
                       size_t spooled_bytes, int binary, int pipe_eof)
{
    /* The cap also keeps the fatal-cleanup pgid table from ever filling. */
    if (task_running_count() >= (size_t)config_int("task.max_running"))
        return NULL;
    struct task *t = xcalloc(1, sizeof(*t));
    /* The counter advances for named tasks too, so unnamed ids stay positional. */
    if (name && *name && !task_id_in_use(name))
        snprintf(t->id, sizeof(t->id), "%s", name);
    else
        snprintf(t->id, sizeof(t->id), "t%d", next_task_number);
    next_task_number++;
    t->command = xstrdup(command ? command : "");
    t->process = process;
    t->pipe_fd = pipe_fd;
    t->spool_fd = spool_fd;
    t->spool_path = spool_path;
    t->started_ms = started_ms;
    t->total_bytes = spooled_bytes;
    t->spooled_bytes = spooled_bytes;
    t->binary = binary;
    t->eof = pipe_eof;
    pthread_mutex_init(&t->lock, NULL);

#ifndef _WIN32
    /* Task descriptors outlive this tool call, so later commands must not inherit them. Windows
     * creates both descriptors non-inheritable. */
    fcntl(pipe_fd, F_SETFD, FD_CLOEXEC);
    if (spool_fd >= 0)
        fcntl(spool_fd, F_SETFD, FD_CLOEXEC);
#endif

    if (pipe_eof) {
        close(pipe_fd);
        t->pipe_fd = -1;
    } else {
        struct task **boxed = xmalloc(sizeof(*boxed));
        *boxed = t;
        t->drainer = bg_job_spawn(task_drain, boxed);
        if (!t->drainer) {
            /* The spool fd and path stay with the caller, which reattaches them so the
             * captured output survives the fallback to the kill path. */
            free(boxed);
            pthread_mutex_destroy(&t->lock);
            free(t->command);
            free(t);
            return NULL;
        }
    }

    /* Append in id order: reports and /tasks read oldest-first. */
    struct task **link = &tasks;
    while (*link)
        link = &(*link)->next;
    *link = t;
    return t->id;
}

static void append_status_phrase(struct buf *out, struct task *t)
{
    long end_ms = t->done ? t->finished_ms : monotonic_ms();
    char elapsed[32];
    format_duration(elapsed, sizeof(elapsed), end_ms - t->started_ms);

    char phrase[64];
    if (!t->done)
        snprintf(phrase, sizeof(phrase), "still running (%s)", elapsed);
    else if (t->status.end == SPAWN_END_SIGNALED)
        snprintf(phrase, sizeof(phrase), "killed (signal %d) after %s", t->status.code, elapsed);
    else if (t->status.end == SPAWN_END_FORCED)
        snprintf(phrase, sizeof(phrase), "killed (forced) after %s", elapsed);
    else
        snprintf(phrase, sizeof(phrase), "finished (exit %d) after %s", t->status.code, elapsed);
    buf_append_str(out, phrase);
    if (t->orphans_killed)
        buf_append_str(out, "; orphaned processes killed");
}

static void append_sanitized_path(struct buf *out, const char *path)
{
    char *clean = utf8_sanitize(path ? path : "?", strlen(path ? path : "?"));
    buf_append_str(out, clean);
    free(clean);
}

static void append_gap_marker(struct buf *body, size_t omitted_bytes, const char *spool_path)
{
    char size[16];
    bash_format_byte_size(size, sizeof(size), omitted_bytes);
    char marker[64];
    snprintf(marker, sizeof(marker), "... [output truncated: omitted %s; full output: ", size);
    buf_append_str(body, marker);
    append_sanitized_path(body, spool_path);
    buf_append_str(body, "] ...\n");
}

/* Head/tail cap over the undelivered range [from, to), reusing the sync bash slicers: keep a
 * small leading slice (the continuation point, often the first error) and spend most of the
 * budget on the trailing results, with the gap marker naming the log. */
static void append_capped_range(struct task *t, size_t from, size_t to, struct buf *body)
{
    size_t range = to - from;
    size_t cap = output_cap_bytes();
    size_t head_cap = cap / BASH_OUTPUT_HEAD_DIVISOR;
    size_t head_lines_cap = OUTPUT_CAP_LINES / BASH_OUTPUT_HEAD_DIVISOR;

    struct buf tail;
    buf_init(&tail);
    size_t tail_bytes = 0, tail_lines = 0;
    if (bash_read_tail_slice(t->spool_fd, (off_t)from, range, cap - head_cap,
                             OUTPUT_CAP_LINES - head_lines_cap, &tail, &tail_bytes,
                             &tail_lines) != 0) {
        buf_free(&tail);
        buf_append_str(body, "[output unavailable: log read failed]");
        return;
    }
    off_t tail_offset = (off_t)(to - tail_bytes);

    struct buf head;
    buf_init(&head);
    size_t head_bytes = 0, head_lines = 0;
    bash_read_head_slice(t->spool_fd, (off_t)from, head_cap, head_lines_cap, tail_offset, &head,
                         &head_bytes, &head_lines);

    size_t gap_bytes = (size_t)(tail_offset - (off_t)from) - head_bytes;
    if (head_lines > 0 && gap_bytes > 0) {
        bash_output_append_sanitized(body, head.data, head.len);
        append_gap_marker(body, gap_bytes, t->spool_path);
        bash_output_append_sanitized(body, tail.data ? tail.data : "", tail.len);
    } else {
        /* No usable head: the tail reclaims the full budget. A short range comes back whole
         * here and earns no marker. */
        if (tail_bytes < range) {
            buf_reset(&tail);
            tail_bytes = 0;
            tail_lines = 0;
            bash_read_tail_slice(t->spool_fd, (off_t)from, range, cap, OUTPUT_CAP_LINES, &tail,
                                 &tail_bytes, &tail_lines);
        }
        if (tail_bytes < range)
            append_gap_marker(body, range - tail_bytes, t->spool_path);
        bash_output_append_sanitized(body, tail.data ? tail.data : "", tail.len);
    }
    buf_free(&head);
    buf_free(&tail);
}

/* Collect the output produced since the last delivery and advance the cursor. `body` receives
 * spooled bytes a live display already streamed; `marker` receives the note for withheld
 * content (binary, spool failure, overflow), which the stream never carries and must reach the
 * display even after streamed output. The log path is advertised only in the markers, matching
 * the sync bash convention. Returns nonzero when either buffer was filled. */
static int collect_new_output(struct task *t, struct buf *body, struct buf *marker)
{
    struct task_shared_snapshot snap;
    task_snapshot(t, &snap);
    size_t from = t->delivered_bytes;

    /* The suppression markers repeat only when bytes actually arrived since the last delivery;
     * a wait that saw nothing new must say so, not re-report the same marker. */
    if (snap.binary) {
        if (snap.total_bytes <= from)
            return 0;
        t->delivered_bytes = snap.total_bytes;
        char head[96];
        snprintf(head, sizeof(head),
                 "[binary output suppressed: %zu bytes total; log: ", snap.total_bytes);
        buf_append_str(marker, head);
        append_sanitized_path(marker, t->spool_path);
        buf_append_str(marker, "]");
        return 1;
    }
    if (t->spool_fd < 0 || snap.spool_write_failed) {
        if (snap.total_bytes <= from)
            return 0;
        t->delivered_bytes = snap.total_bytes;
        buf_append_str(marker, "[output unavailable: spool write failed]");
        return 1;
    }
    /* Bytes past spooled_bytes are discarded only once overflow is set; otherwise they are
     * still in flight to the spool and must stay undelivered for the next collection. */
    size_t settled = snap.overflow ? snap.total_bytes : snap.spooled_bytes;
    if (settled <= from)
        return 0;
    t->delivered_bytes = settled;

    if (from < snap.spooled_bytes)
        append_capped_range(t, from, snap.spooled_bytes, body);
    if (snap.overflow && snap.total_bytes > snap.spooled_bytes) {
        char head[96];
        snprintf(head, sizeof(head),
                 "\n[output limit reached: %zu further bytes discarded, task stopped; log: ",
                 snap.total_bytes - snap.spooled_bytes);
        buf_append_str(marker, head);
        append_sanitized_path(marker, t->spool_path);
        buf_append_str(marker, "]");
    }
    return 1;
}

/* One announce-only line: "[task t2 finished (exit 0) after 3m; 2.1K output]". The size lets
 * the model skip collecting empty results; a finished task with nothing left to deliver is
 * collected outright. */
static void append_status_note(struct buf *out, struct task *t)
{
    struct task_shared_snapshot snap;
    task_snapshot(t, &snap);
    size_t pending = snap.total_bytes - t->delivered_bytes;

    buf_append_str(out, "[task ");
    buf_append_str(out, t->id);
    buf_append_str(out, " ");
    append_status_phrase(out, t);
    char clause[48];
    if (snap.total_bytes == 0) {
        snprintf(clause, sizeof(clause), "; no output]");
    } else if (pending == 0) {
        snprintf(clause, sizeof(clause), "; no new output]");
    } else {
        char size[16];
        bash_format_byte_size(size, sizeof(size), pending);
        snprintf(clause, sizeof(clause), "; %s%s output]", size, snap.binary ? " binary" : "");
    }
    buf_append_str(out, clause);

    if (t->done) {
        t->notified = 1;
        if (pending == 0)
            t->collected = 1;
    }
}

char *task_report_output(const char *id, char **marker_out)
{
    *marker_out = NULL;
    struct task *t = task_find(id);
    if (!t)
        return NULL;
    task_poll(t);
    struct buf body;
    buf_init(&body);
    struct buf marker;
    buf_init(&marker);
    collect_new_output(t, &body, &marker);
    if (marker.len > 0)
        *marker_out = buf_steal(&marker);
    else
        buf_free(&marker);
    return buf_steal(&body);
}

/* Resolve requested ids into `targets`, skipping unknown ids and duplicates. With no ids,
 * every live task is a target. Returns the target count. */
static size_t resolve_targets(const char *const *ids, size_t n_ids, struct task ***targets_out)
{
    size_t count = 0;
    struct task **targets;
    if (n_ids == 0) {
        for (struct task *t = tasks; t; t = t->next)
            if (!t->collected)
                count++;
        targets = xmalloc(count * sizeof(*targets));
        count = 0;
        for (struct task *t = tasks; t; t = t->next)
            if (!t->collected)
                targets[count++] = t;
    } else {
        targets = xmalloc(n_ids * sizeof(*targets));
        for (size_t i = 0; i < n_ids; i++) {
            struct task *t = ids[i] ? task_find(ids[i]) : NULL;
            if (!t)
                continue;
            int duplicate = 0;
            for (size_t j = 0; j < count; j++)
                if (targets[j] == t)
                    duplicate = 1;
            if (!duplicate)
                targets[count++] = t;
        }
    }
    *targets_out = targets;
    return count;
}

static void sleep_poll_interval(void)
{
    poll(NULL, 0, TASK_POLL_INTERVAL_MS);
}

static long deadline_after(long now_ms, long duration_ms)
{
    if (duration_ms < 0)
        duration_ms = 0;
    return duration_ms > LONG_MAX - now_ms ? LONG_MAX : now_ms + duration_ms;
}

/* SIGTERM with the bash grace window armed (task_poll escalates to SIGKILL at the deadline),
 * or SIGKILL outright when the grace is zero. */
static void signal_task_stop(struct task *t, long now)
{
#ifdef _WIN32
    (void)now;
    spawn_process_terminate(t->process, SIGKILL);
#else
    long grace_ms = config_duration_ms("bash.timeout_grace");
    if (grace_ms > 0) {
        spawn_process_terminate(t->process, SIGTERM);
        t->kill_deadline_ms = deadline_after(now, grace_ms);
    } else {
        spawn_process_terminate(t->process, SIGKILL);
    }
#endif
}

/* Forward the not-yet-displayed spooled bytes to the live display, exactly like a foreground
 * bash stream; the renderer applies its own elision. */
static void stream_new_spool(struct task *t, const struct task_shared_snapshot *snap,
                             tool_display_fn display, void *display_data, size_t *cursor,
                             int *displayed_body)
{
    if (!display || snap->binary || t->spool_fd < 0)
        return;
    char chunk[8192];
    while (*cursor < snap->spooled_bytes) {
        size_t want = snap->spooled_bytes - *cursor;
        ssize_t bytes_read =
            pread(t->spool_fd, chunk, want < sizeof(chunk) ? want : sizeof(chunk), (off_t)*cursor);
        if (bytes_read < 0 && errno == EINTR)
            continue;
        if (bytes_read <= 0)
            break;
        display(chunk, (size_t)bytes_read, display_data);
        *displayed_body = 1;
        *cursor += (size_t)bytes_read;
    }
}

enum wait_stop {
    WAIT_TARGET_DONE,
    WAIT_OTHER_DONE,
    WAIT_TIMED_OUT,
    WAIT_INTERRUPTED,
};

char *task_wait_stream(const char *id, long timeout_ms, int kill_on_timeout,
                       tool_display_fn display, void *display_data)
{
    task_poll_all();
    struct task *t = id && *id ? task_find(id) : NULL;
    if (!t) {
        char *clean = utf8_sanitize(id ? id : "(none)", strlen(id ? id : "(none)"));
        char *flat = flatten_for_display(clean);
        free(clean);
        char *head = truncate_for_display(flat, TASK_COMMAND_HEAD_CELLS);
        free(flat);
        char *error = xasprintf("no such task: %s", head);
        free(head);
        return error;
    }

    long deadline = deadline_after(monotonic_ms(), timeout_ms);
    int kill_sent = 0;
    size_t display_cursor = t->delivered_bytes;
    int displayed_body = 0;
    enum wait_stop stop = WAIT_TARGET_DONE;
    for (;;) {
        /* Poll everything: foreign completions end the wait so their notes ride this same
         * round trip instead of aging until the timeout. */
        task_poll_all();
        struct task_shared_snapshot snap;
        task_snapshot(t, &snap);
        stream_new_spool(t, &snap, display, display_data, &display_cursor, &displayed_body);
        if (t->done)
            break;
        /* The deadline outranks foreign completions: a kill request must fire even when a
         * pending completion could otherwise end the wait first. */
        long now = monotonic_ms();
        if (now >= deadline) {
            if (kill_on_timeout && !kill_sent) {
                signal_task_stop(t, now);
                kill_sent = 1;
                /* Extend the wait so the escalation and exit are observed here, not by a
                 * registry call minutes later. */
                deadline = deadline_after(t->kill_deadline_ms ? t->kill_deadline_ms : now,
                                          TASK_KILL_MARGIN_MS);
                continue;
            }
            stop = WAIT_TIMED_OUT;
            break;
        }
        int other_finished = 0;
        for (struct task *other = tasks; other; other = other->next)
            if (other != t && other->done && !other->notified)
                other_finished = 1;
        /* A signalled kill commits this wait to observing the exit. */
        if (other_finished && !kill_sent) {
            stop = WAIT_OTHER_DONE;
            break;
        }
        if (interrupt_abort_requested()) {
            stop = WAIT_INTERRUPTED;
            break;
        }
        sleep_poll_interval();
    }

    /* Bytes spooled between the loop's last snapshot and the stop still belong on screen. */
    struct task_shared_snapshot final_snap;
    task_snapshot(t, &final_snap);
    stream_new_spool(t, &final_snap, display, display_data, &display_cursor, &displayed_body);

    struct buf out;
    buf_init(&out);
    struct buf body;
    buf_init(&body);
    struct buf marker;
    buf_init(&marker);
    int has_body = collect_new_output(t, &body, &marker);
    if (has_body) {
        if (body.len > 0)
            buf_append(&out, body.data, body.len);
        if (marker.len > 0)
            buf_append(&out, marker.data, marker.len);
        if (out.len == 0 || out.data[out.len - 1] != '\n')
            buf_append_str(&out, "\n");
        /* The body was already shown if anything streamed live; the marker never streams, so
         * it must reach the display either way. */
        if (display && !displayed_body && body.len > 0) {
            display(body.data, body.len, display_data);
            displayed_body = 1;
        }
        if (display && marker.len > 0) {
            if (displayed_body && marker.data[0] != '\n')
                display("\n", 1, display_data);
            display(marker.data, marker.len, display_data);
            displayed_body = 1;
        }
    }
    buf_free(&marker);
    buf_free(&body);

    struct buf footer;
    buf_init(&footer);
    buf_append_str(&footer, "[");
    buf_append_str(&footer, t->id);
    buf_append_str(&footer, " ");
    append_status_phrase(&footer, t);
    if (kill_on_timeout && stop == WAIT_OTHER_DONE)
        buf_append_str(&footer, "; not killed");
    if (!has_body)
        buf_append_str(&footer, "; no new output");
    if (stop == WAIT_TIMED_OUT)
        buf_append_str(&footer, kill_sent ? " — did not exit after SIGKILL" : " — wait timed out");
    else if (stop == WAIT_INTERRUPTED)
        buf_append_str(&footer, " — wait interrupted");
    else if (stop == WAIT_OTHER_DONE)
        buf_append_str(&footer, " — another task finished");
    buf_append_str(&footer, "]");

    /* The renderer shows only displayed bytes once a stream begins, so the footer must reach
     * the display too (mirrors the sync bash suffix). */
    if (display) {
        if (displayed_body)
            display("\n", 1, display_data);
        display(footer.data, footer.len, display_data);
    }
    buf_append(&out, footer.data, footer.len);
    buf_free(&footer);

    if (t->done) {
        t->notified = 1;
        t->collected = 1;
    }
    task_sweep();
    return buf_steal(&out);
}

/* SIGTERM (grace permitting), then wait for the batch to exit, escalating to SIGKILL through
 * task_poll when the grace deadline lapses. Returns the number of tasks signalled. */
static size_t stop_targets(struct task **targets, size_t n_targets)
{
    long now = monotonic_ms();
    size_t signalled = 0;
    for (size_t i = 0; i < n_targets; i++) {
        struct task *t = targets[i];
        if (t->done)
            continue;
        signal_task_stop(t, now);
        signalled++;
    }

    long grace_ms = config_duration_ms("bash.timeout_grace");
    long deadline = deadline_after(deadline_after(now, grace_ms), TASK_KILL_MARGIN_MS);
    while (signalled > 0 && monotonic_ms() < deadline) {
        size_t still_running = 0;
        for (size_t i = 0; i < n_targets; i++) {
            task_poll(targets[i]);
            if (!targets[i]->done)
                still_running++;
        }
        if (still_running == 0)
            break;
        sleep_poll_interval();
    }
    return signalled;
}

size_t task_stop(const char *const *ids, size_t n_ids)
{
    task_poll_all();
    struct task **targets = NULL;
    size_t n_targets = resolve_targets(ids, n_ids, &targets);
    size_t stopped = stop_targets(targets, n_targets);
    free(targets);
    return stopped;
}

char *task_collect_notes(void)
{
    task_poll_all();
    struct buf out;
    buf_init(&out);
    for (struct task *t = tasks; t; t = t->next) {
        if (t->notified || !t->done)
            continue;
        if (out.len > 0)
            buf_append_str(&out, "\n");
        append_status_note(&out, t);
    }
    task_sweep();
    if (out.len == 0)
        return NULL;
    return buf_steal(&out);
}

size_t task_list(struct task_info **rows_out)
{
    task_poll_all();
    size_t count = 0;
    for (struct task *t = tasks; t; t = t->next)
        if (!t->collected)
            count++;
    struct task_info *rows = xcalloc(count ? count : 1, sizeof(*rows));
    size_t i = 0;
    for (struct task *t = tasks; t; t = t->next) {
        if (t->collected)
            continue;
        struct task_shared_snapshot snap;
        task_snapshot(t, &snap);
        rows[i].id = t->id;
        rows[i].command = t->command;
        rows[i].spool_path = snap.spool_write_failed ? NULL : t->spool_path;
        rows[i].running = !t->done;
        rows[i].exit_code = t->done && t->status.end == SPAWN_END_EXITED ? t->status.code : 0;
        rows[i].term_signal = t->done && t->status.end == SPAWN_END_SIGNALED ? t->status.code : 0;
        rows[i].forced = t->done && t->status.end == SPAWN_END_FORCED;
        rows[i].elapsed_ms = (t->done ? t->finished_ms : monotonic_ms()) - t->started_ms;
        rows[i].total_bytes = snap.total_bytes;
        i++;
    }
    *rows_out = rows;
    return count;
}

char *task_exit_note(void)
{
    task_poll_all();
    struct buf out;
    buf_init(&out);
    for (struct task *t = tasks; t; t = t->next) {
        if (t->collected)
            continue;
        if (out.len > 0)
            buf_append_str(&out, "\n");
        buf_append_str(&out, "[task ");
        buf_append_str(&out, t->id);
        buf_append_str(&out, " ");
        if (!t->done) {
            buf_append_str(&out, "killed at exit]");
            continue;
        }
        append_status_phrase(&out, t);
        struct task_shared_snapshot snap;
        task_snapshot(t, &snap);
        size_t pending = snap.total_bytes - t->delivered_bytes;
        if (pending > 0) {
            char size[16];
            bash_format_byte_size(size, sizeof(size), pending);
            char clause[48];
            snprintf(clause, sizeof(clause), "; %s output discarded", size);
            buf_append_str(&out, clause);
        }
        buf_append_str(&out, "]");
    }
    if (out.len == 0) {
        buf_free(&out);
        return NULL;
    }
    return buf_steal(&out);
}

size_t task_running_count(void)
{
    task_poll_all();
    size_t count = 0;
    for (struct task *t = tasks; t; t = t->next)
        if (!t->done)
            count++;
    return count;
}

void task_registry_shutdown(void)
{
    while (tasks) {
        struct task *t = tasks;
        tasks = t->next;
        /* Stop the drainer — the only other thread that may terminate — before consuming the
         * process object. */
        if (t->drainer) {
            bg_job_cancel(t->drainer);
            bg_job_join(t->drainer);
            t->drainer = NULL;
        }
        if (!t->done) {
            spawn_process_terminate(t->process, SIGKILL);
            bash_live_process_retract(t->process);
            if (spawn_process_wait(t->process, &t->status) < 0)
                t->status = (struct spawn_status){.end = SPAWN_END_FORCED};
            t->process = NULL;
            t->done = 1;
        }
        task_free(t);
    }
    /* Ids read as conversation-scoped, so a conversation started after a shutdown counts from
     * t1 again. */
    next_task_number = 1;
}
