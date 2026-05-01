/* SPDX-License-Identifier: MIT */
#include "interrupt.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define ESC_TIMEOUT_MS 50

struct watcher {
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int armed;       /* protected by mu */
    int paused;      /* protected by mu — 1 while watcher is in outer cond_wait */
    int wake_pipe_r; /* read end watched alongside stdin */
    int wake_pipe_w; /* write end pinged on disarm to break the watcher's poll */
    atomic_int requested;
    /* 1 while the classifier is in IC_PENDING_ESC — \x1b seen, awaiting
     * the follow-up byte that decides CSI/SS3 vs bare Esc. Used by
     * interrupt_settle to let agent decisions wait out the window
     * before acting. Lockless atomic — written only by the watcher
     * thread, read by anyone. */
    atomic_int pending;
    int started;

    /* Captured at interrupt_init() — the canonical baseline restored by
     * atexit and signal handlers. Not modified during arm/disarm. */
    struct termios saved_termios;
    int saved_termios_valid;
    int raw_mode_active; /* tracks whether termios currently differs from saved */
};

/* Single global instance — there's one tty and one process. Static handlers
 * (atexit, signal) reach it without plumbing. */
static struct watcher W;

/* ---------- classifier ---------- */

void interrupt_classifier_init(struct interrupt_classifier *c)
{
    c->state = IC_IDLE;
}

int interrupt_classifier_feed(struct interrupt_classifier *c, unsigned char byte)
{
    switch (c->state) {
    case IC_IDLE:
        if (byte == 0x1b)
            c->state = IC_PENDING_ESC;
        return 0;
    case IC_PENDING_ESC:
        /* '[' or 'O' confirm a CSI / SS3 escape sequence — consume bytes
         * silently until the sequence ends. Anything else means the prior
         * \x1b was a bare Esc: fire for it now and re-classify the
         * current byte. The Esc-Esc case in particular must fire (so a
         * user mashing Esc isn't swallowed when the second arrives
         * within the timeout window). */
        if (byte == '[') {
            c->state = IC_CSI;
            return 0;
        }
        if (byte == 'O') {
            c->state = IC_SS3;
            return 0;
        }
        c->state = (byte == 0x1b) ? IC_PENDING_ESC : IC_IDLE;
        return 1;
    case IC_CSI:
        /* CSI parameter / intermediate bytes are 0x20-0x3F; final byte
         * is 0x40-0x7E. Anything else (NUL, control char, high bit set)
         * aborts the sequence — defensive against truncated input. */
        if (byte >= 0x40 && byte <= 0x7E)
            c->state = IC_IDLE;
        else if (byte < 0x20 || byte > 0x7E)
            c->state = IC_IDLE;
        return 0;
    case IC_SS3:
        /* SS3 is \x1bO followed by exactly one byte. */
        c->state = IC_IDLE;
        return 0;
    }
    return 0;
}

int interrupt_classifier_timeout(struct interrupt_classifier *c)
{
    if (c->state == IC_PENDING_ESC) {
        c->state = IC_IDLE;
        return 1;
    }
    return 0;
}

/* ---------- tty mode helpers ---------- */

static void enter_raw_mode(void)
{
    if (!W.saved_termios_valid || W.raw_mode_active)
        return;
    struct termios raw = W.saved_termios;
    /* Non-canonical (byte-at-a-time), no echo. Leave ISIG enabled so
     * Ctrl-C still raises SIGINT — our interrupt key is Esc, not C-c. */
    raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    /* Set the flag BEFORE tcsetattr so a signal arriving mid-call still
     * triggers restore_tty_only(). If tcsetattr fails, the tty is still
     * canonical and a redundant restore is a harmless no-op; we then
     * clear the flag below. The reverse ordering had a window where
     * SIGINT could leave the terminal stuck in raw mode. */
    W.raw_mode_active = 1;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
        W.raw_mode_active = 0;
}

static void leave_raw_mode(void)
{
    if (!W.saved_termios_valid || !W.raw_mode_active)
        return;
    tcsetattr(STDIN_FILENO, TCSANOW, &W.saved_termios);
    W.raw_mode_active = 0;
}

/* ---------- watcher thread ---------- */

/* Returns 0 on timeout, >0 on data, -1 on error (errno set). Sets *src
 * to 0 if stdin became readable, 1 if the wake pipe did. When both are
 * ready, the wake pipe wins — disarm must take priority over stdin so
 * we don't consume bytes the user typed for the upcoming readline. */
static int wait_input(int timeout_ms, int *src)
{
    struct pollfd pfds[2] = {
        {.fd = STDIN_FILENO, .events = POLLIN},
        {.fd = W.wake_pipe_r, .events = POLLIN},
    };
    int pr = poll(pfds, 2, timeout_ms);
    if (pr <= 0)
        return pr;
    if (pfds[1].revents & POLLIN) {
        *src = 1;
        return 1;
    }
    if (pfds[0].revents & POLLIN) {
        *src = 0;
        return 1;
    }
    /* POLLHUP / POLLERR — treat as wake so we re-evaluate state. */
    *src = 1;
    return 1;
}

static void drain_wake_pipe(void)
{
    char buf[64];
    while (read(W.wake_pipe_r, buf, sizeof(buf)) > 0)
        /* drain */;
}

static void *watcher_thread(void *arg)
{
    (void)arg;

    /* Block all signals on this thread so SIGINT/SIGTERM/etc. land on the
     * main thread — same convention the spinner uses. */
    sigset_t mask;
    sigfillset(&mask);
    pthread_sigmask(SIG_SETMASK, &mask, NULL);

    struct interrupt_classifier ic;
    interrupt_classifier_init(&ic);

    /* Runs until the process exits — there's no clean shutdown path; the
     * thread is reaped when _exit() tears down the address space. */
    for (;;) {
        pthread_mutex_lock(&W.mu);
        /* Mark paused and broadcast so any disarm waiting for the
         * acknowledgement can wake. The cv is shared with the arm
         * notification, so cond_wait's spurious-wakeup loop is on
         * `armed`, not `paused`. */
        W.paused = 1;
        pthread_cond_broadcast(&W.cv);
        while (!W.armed)
            pthread_cond_wait(&W.cv, &W.mu);
        W.paused = 0;
        pthread_mutex_unlock(&W.mu);

        /* Reset classifier on each (re-)arm so a stale PENDING_ESC from
         * a previous arm window can't fire spuriously. Pending mirrors
         * the classifier's IC_PENDING_ESC state and must be cleared in
         * lockstep. */
        interrupt_classifier_init(&ic);
        atomic_store(&W.pending, 0);

        for (;;) {
            int timeout = (ic.state == IC_PENDING_ESC) ? ESC_TIMEOUT_MS : -1;
            int src = 0;
            int pr = wait_input(timeout, &src);
            if (pr < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (pr == 0) {
                /* Timeout while a \x1b was pending — confirmed bare Esc. */
                if (interrupt_classifier_timeout(&ic))
                    atomic_store(&W.requested, 1);
                atomic_store(&W.pending, ic.state == IC_PENDING_ESC);
                continue;
            }
            if (src == 1) {
                /* Wake pipe — disarm fired. Drain and re-check armed. */
                drain_wake_pipe();
                pthread_mutex_lock(&W.mu);
                int still_armed = W.armed;
                pthread_mutex_unlock(&W.mu);
                if (!still_armed)
                    break;
                continue;
            }
            /* Stdin readable. Re-check armed before reading: poll might
             * have returned with stdin ready before the wake byte made
             * it into the pipe, and reading here would steal a byte the
             * user already typed for the upcoming readline. */
            pthread_mutex_lock(&W.mu);
            int still_armed = W.armed;
            pthread_mutex_unlock(&W.mu);
            if (!still_armed)
                break;
            unsigned char buf[64];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (n == 0)
                break; /* EOF on stdin — stop watching */
            for (ssize_t i = 0; i < n; i++) {
                if (interrupt_classifier_feed(&ic, buf[i]))
                    atomic_store(&W.requested, 1);
            }
            atomic_store(&W.pending, ic.state == IC_PENDING_ESC);
        }
    }
    return NULL;
}

/* ---------- restoration on exit / signal ---------- */

static void restore_tty_only(void)
{
    /* Best-effort restoration of the canonical baseline. Used by
     * atexit and the signal handler — both terminal paths (the signal
     * handler re-raises with SIG_DFL right after this), so the bytes
     * we write are the last bytes the tty sees from us; interleaving
     * with a concurrent paint() in input.c is fine because the process
     * is about to die and stdio buffers don't matter. Must be
     * async-signal-safe in the signal case: tcsetattr and write(2) are
     * in the POSIX async-safe list.
     *
     * Always restores when we have a saved baseline, regardless of the
     * watcher's raw_mode_active flag: the prompt editor (input.c) also
     * puts the tty in raw mode without going through interrupt_arm, so
     * this path must unwind that case too. Restoring an already-
     * canonical tty is a harmless no-op. Bracketed paste is also
     * disabled unconditionally so it doesn't leak to the parent
     * shell. */
    if (!W.saved_termios_valid)
        return;
    tcsetattr(STDIN_FILENO, TCSANOW, &W.saved_termios);
    W.raw_mode_active = 0;
    static const char paste_off[] = "\x1b[?2004l";
    (void)!write(STDOUT_FILENO, paste_off, sizeof(paste_off) - 1);
}

static void atexit_handler(void)
{
    restore_tty_only();
}

static void signal_restore_and_reraise(int sig)
{
    restore_tty_only();
    /* Re-raise with default disposition so the parent shell sees the
     * expected exit signal (e.g. 130 for SIGINT). signal() back to
     * SIG_DFL is async-signal-safe. */
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_restore_and_reraise;
    sigemptyset(&sa.sa_mask);
    int sigs[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT};
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
        sigaction(sigs[i], &sa, NULL);
}

/* ---------- public API ---------- */

void interrupt_init(void)
{
    if (W.started)
        return;

    pthread_mutex_init(&W.mu, NULL);
    pthread_cond_init(&W.cv, NULL);
    atomic_store(&W.requested, 0);

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return;

    if (tcgetattr(STDIN_FILENO, &W.saved_termios) == 0)
        W.saved_termios_valid = 1;

    int fds[2];
    if (pipe(fds) < 0)
        return;
    /* Close-on-exec on both ends so forked tools (notably bash) don't
     * inherit our internal pipe — a child holding the read end could
     * race the watcher for the disarm wake byte, and write access from
     * a misbehaving descendant could spuriously wake the watcher.
     * pipe2(O_CLOEXEC) isn't portable to macOS, so set it via fcntl. */
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    /* Non-blocking so drain_wake_pipe never blocks if multiple writes
     * coalesced. The write side stays blocking — writes are 1 byte. */
    int fl = fcntl(fds[0], F_GETFL, 0);
    if (fl >= 0)
        fcntl(fds[0], F_SETFL, fl | O_NONBLOCK);
    W.wake_pipe_r = fds[0];
    W.wake_pipe_w = fds[1];

    if (pthread_create(&W.thread, NULL, watcher_thread, NULL) != 0) {
        close(W.wake_pipe_r);
        close(W.wake_pipe_w);
        W.wake_pipe_r = W.wake_pipe_w = -1;
        return;
    }
    W.started = 1;

    atexit(atexit_handler);
    install_signal_handlers();
}

static void wake_watcher(void)
{
    if (W.wake_pipe_w < 0)
        return;
    char b = 0;
    /* EAGAIN/EINTR ignored — even one already-pending byte is enough to
     * wake poll. */
    (void)write(W.wake_pipe_w, &b, 1);
}

void interrupt_arm(void)
{
    if (!W.started)
        return;
    pthread_mutex_lock(&W.mu);
    int was_armed = W.armed;
    if (!was_armed) {
        enter_raw_mode();
        W.armed = 1;
        /* Broadcast so the watcher's outer cond_wait wakes. The cv is
         * shared with the disarm-wait-for-paused path, but disarm only
         * waits while watcher is in the inner loop, never concurrently
         * with arm — broadcast is harmless either way. */
        pthread_cond_broadcast(&W.cv);
    }
    pthread_mutex_unlock(&W.mu);
}

void interrupt_disarm(void)
{
    if (!W.started)
        return;
    pthread_mutex_lock(&W.mu);
    int was_armed = W.armed;
    if (was_armed)
        W.armed = 0;
    pthread_mutex_unlock(&W.mu);
    if (!was_armed)
        return;

    wake_watcher();

    /* Wait for the watcher to acknowledge it has stopped reading stdin.
     * Without this we could leave_raw_mode while it's still in poll/read
     * (next read would block on the canonical-mode line buffer), or it
     * could consume bytes the user typed for the upcoming readline. */
    pthread_mutex_lock(&W.mu);
    while (!W.paused)
        pthread_cond_wait(&W.cv, &W.mu);
    leave_raw_mode();
    pthread_mutex_unlock(&W.mu);

    /* Discard any input bytes that landed in the kernel buffer during the
     * armed window — including the Esc itself and any partial CSI tail
     * — so they don't reappear in the next readline call. */
    tcflush(STDIN_FILENO, TCIFLUSH);
}

int interrupt_requested(void)
{
    if (!W.started)
        return 0;
    return atomic_load(&W.requested);
}

/* Zero-timeout poll on stdin — true if there are bytes the watcher
 * hasn't read yet. Multiple concurrent pollers are fine on POSIX; we
 * never read from this fd, only ask the kernel about its readiness. */
static int stdin_has_pending(void)
{
    struct pollfd p = {.fd = STDIN_FILENO, .events = POLLIN};
    int rc = poll(&p, 1, 0);
    return rc > 0 && (p.revents & POLLIN);
}

void interrupt_settle(void)
{
    if (!W.started)
        return;
    /* Up to ESC_TIMEOUT_MS plus slack to absorb three boundary cases:
     *   1. Bytes queued in the tty but not yet read by the watcher
     *      (stdin_has_pending) — the byte arrived just as we got here
     *      and the watcher hasn't been scheduled to read() it.
     *   2. \x1b already classified as PENDING_ESC, awaiting follow-up
     *      or timeout (W.pending).
     *   3. Bare Esc just confirmed and latched (W.requested).
     * Polled in 5ms steps — the work is a syscall plus a few atomic
     * loads, and avoids cv-coordination cost on the watcher's hot
     * path. Returns as soon as everything has settled. */
    int budget_ms = ESC_TIMEOUT_MS + 10;
    while (budget_ms > 0) {
        if (atomic_load(&W.requested))
            return;
        if (!atomic_load(&W.pending) && !stdin_has_pending())
            return;
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 5000000L};
        nanosleep(&ts, NULL);
        budget_ms -= 5;
    }
}

void interrupt_clear(void)
{
    atomic_store(&W.requested, 0);
}
