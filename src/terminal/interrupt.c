/* SPDX-License-Identifier: MIT */
#include "terminal/interrupt.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "terminal/ansi.h"

#define ESCAPE_TIMEOUT_MS       50
#define ESCAPE_POLL_INTERVAL_MS 5

struct interrupt_watcher {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t state_changed;
    int armed;
    int idle;
    int wake_read_fd;
    int wake_write_fd;
    atomic_int pause_requested;
    atomic_int abort_requested;
    atomic_int escape_pending;
    int initialized;
    int started;
    struct termios saved_termios;
    volatile sig_atomic_t saved_termios_valid;
    volatile sig_atomic_t raw_mode_active;
    volatile sig_atomic_t interactive_terminal;
};

/* Exit and signal handlers require process-wide terminal state. */
static struct interrupt_watcher watcher = {
    .wake_read_fd = -1,
    .wake_write_fd = -1,
};

void interrupt_classifier_init(struct interrupt_classifier *classifier)
{
    classifier->state = INTERRUPT_CLASSIFIER_IDLE;
}

int interrupt_classifier_feed(struct interrupt_classifier *classifier, unsigned char byte)
{
    switch (classifier->state) {
    case INTERRUPT_CLASSIFIER_IDLE:
        if (byte == 0x1b)
            classifier->state = INTERRUPT_CLASSIFIER_ESCAPE_PENDING;
        return 0;
    case INTERRUPT_CLASSIFIER_ESCAPE_PENDING:
        if (byte == '[') {
            classifier->state = INTERRUPT_CLASSIFIER_CSI;
            return 0;
        }
        if (byte == 'O') {
            classifier->state = INTERRUPT_CLASSIFIER_SS3;
            return 0;
        }
        /* The previous Esc was bare; classify a second Esc as a new candidate. */
        classifier->state =
            byte == 0x1b ? INTERRUPT_CLASSIFIER_ESCAPE_PENDING : INTERRUPT_CLASSIFIER_IDLE;
        return 1;
    case INTERRUPT_CLASSIFIER_CSI:
        /* CSI final bytes are 0x40-0x7e; control and non-ASCII bytes invalidate the sequence. */
        if ((byte >= 0x40 && byte <= 0x7e) || byte < 0x20 || byte > 0x7e)
            classifier->state = INTERRUPT_CLASSIFIER_IDLE;
        return 0;
    case INTERRUPT_CLASSIFIER_SS3:
        classifier->state = INTERRUPT_CLASSIFIER_IDLE;
        return 0;
    }
    return 0;
}

int interrupt_classifier_timeout(struct interrupt_classifier *classifier)
{
    if (classifier->state != INTERRUPT_CLASSIFIER_ESCAPE_PENDING)
        return 0;
    classifier->state = INTERRUPT_CLASSIFIER_IDLE;
    return 1;
}

static int enter_raw_mode(void)
{
    if (!watcher.saved_termios_valid)
        return -1;
    if (watcher.raw_mode_active)
        return 0;

    struct termios raw = watcher.saved_termios;
    /* ISIG remains enabled so Ctrl-C continues to raise SIGINT. */
    raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    /* Publish the changed state first so a signal during tcsetattr still restores the terminal. */
    watcher.raw_mode_active = 1;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
        watcher.raw_mode_active = 0;
        return -1;
    }
    return 0;
}

static void leave_raw_mode(void)
{
    if (!watcher.saved_termios_valid || !watcher.raw_mode_active)
        return;
    tcsetattr(STDIN_FILENO, TCSANOW, &watcher.saved_termios);
    watcher.raw_mode_active = 0;
}

static void latch_pause_or_abort(void)
{
    if (atomic_exchange(&watcher.pause_requested, 1))
        atomic_store(&watcher.abort_requested, 1);
}

enum input_event {
    INPUT_ERROR = -1,
    INPUT_TIMEOUT,
    INPUT_STDIN,
    INPUT_WAKE,
};

static enum input_event wait_for_input(int timeout_ms)
{
    struct pollfd fds[] = {
        {.fd = STDIN_FILENO, .events = POLLIN},
        {.fd = watcher.wake_read_fd, .events = POLLIN},
    };
    int result = poll(fds, 2, timeout_ms);
    if (result < 0)
        return INPUT_ERROR;
    if (result == 0)
        return INPUT_TIMEOUT;
    /* Disarming takes precedence so the watcher cannot steal input from the next prompt. */
    if (fds[1].revents)
        return INPUT_WAKE;
    if (fds[0].revents & POLLIN)
        return INPUT_STDIN;
    errno = EIO;
    return INPUT_ERROR;
}

static void drain_wake_pipe(void)
{
    char bytes[64];
    for (;;) {
        struct pollfd wake = {.fd = watcher.wake_read_fd, .events = POLLIN};
        if (poll(&wake, 1, 0) <= 0 || !(wake.revents & POLLIN))
            return;
        if (read(watcher.wake_read_fd, bytes, sizeof(bytes)) <= 0)
            return;
    }
}

static int watcher_is_armed(void)
{
    pthread_mutex_lock(&watcher.mutex);
    int armed = watcher.armed;
    pthread_mutex_unlock(&watcher.mutex);
    return armed;
}

static void wait_until_armed(void)
{
    pthread_mutex_lock(&watcher.mutex);
    watcher.idle = 1;
    pthread_cond_broadcast(&watcher.state_changed);
    while (!watcher.armed)
        pthread_cond_wait(&watcher.state_changed, &watcher.mutex);
    watcher.idle = 0;
    pthread_mutex_unlock(&watcher.mutex);
}

static void wait_until_disarmed(void)
{
    pthread_mutex_lock(&watcher.mutex);
    while (watcher.armed)
        pthread_cond_wait(&watcher.state_changed, &watcher.mutex);
    pthread_mutex_unlock(&watcher.mutex);
}

static void watch_armed_input(void)
{
    struct interrupt_classifier classifier;
    interrupt_classifier_init(&classifier);
    atomic_store(&watcher.escape_pending, 0);

    for (;;) {
        int timeout_ms =
            classifier.state == INTERRUPT_CLASSIFIER_ESCAPE_PENDING ? ESCAPE_TIMEOUT_MS : -1;
        enum input_event event = wait_for_input(timeout_ms);
        if (event == INPUT_ERROR) {
            if (errno == EINTR)
                continue;
            wait_until_disarmed();
            return;
        }
        if (event == INPUT_TIMEOUT) {
            if (interrupt_classifier_timeout(&classifier))
                latch_pause_or_abort();
            atomic_store(&watcher.escape_pending,
                         classifier.state == INTERRUPT_CLASSIFIER_ESCAPE_PENDING);
            continue;
        }
        if (event == INPUT_WAKE) {
            drain_wake_pipe();
            if (!watcher_is_armed())
                return;
            continue;
        }

        /* Read only after rechecking ownership; disarm may race poll's stdin result. */
        if (!watcher_is_armed())
            return;
        unsigned char bytes[64];
        ssize_t bytes_read = read(STDIN_FILENO, bytes, sizeof(bytes));
        if (bytes_read < 0) {
            if (errno == EINTR)
                continue;
            wait_until_disarmed();
            return;
        }
        if (bytes_read == 0) {
            wait_until_disarmed();
            return;
        }
        for (ssize_t i = 0; i < bytes_read; i++) {
            if (interrupt_classifier_feed(&classifier, bytes[i]))
                latch_pause_or_abort();
        }
        atomic_store(&watcher.escape_pending,
                     classifier.state == INTERRUPT_CLASSIFIER_ESCAPE_PENDING);
    }
}

static void *watcher_thread(void *unused)
{
    (void)unused;

    /* Keep process signals on the main thread, which owns terminal rendering and cleanup. */
    sigset_t signals;
    sigfillset(&signals);
    pthread_sigmask(SIG_SETMASK, &signals, NULL);

    for (;;) {
        wait_until_armed();
        watch_armed_input();
    }
    return NULL;
}

static void restore_terminal(void)
{
    /* Fatal-signal cleanup must remain allocation-free and avoid stdio. */
    if (watcher.saved_termios_valid) {
        tcsetattr(STDIN_FILENO, TCSANOW, &watcher.saved_termios);
        watcher.raw_mode_active = 0;
    }
    if (watcher.interactive_terminal) {
        static const char restore_sequence[] =
            ANSI_BRACKETED_PASTE_DISABLE ANSI_CURSOR_SHOW ANSI_SYNC_END;
        (void)!write(STDOUT_FILENO, restore_sequence, sizeof(restore_sequence) - 1);
    }
}

static void (*volatile fatal_signal_hook)(void);

void interrupt_set_fatal_signal_hook(void (*hook)(void))
{
    fatal_signal_hook = hook;
}

static void restore_and_reraise_signal(int signal_number)
{
    if (fatal_signal_hook)
        fatal_signal_hook();
    restore_terminal();
    signal(signal_number, SIG_DFL);
    raise(signal_number);
}

void interrupt_install_fatal_signal_handlers(void)
{
    struct sigaction action = {0};
    action.sa_handler = restore_and_reraise_signal;
    sigemptyset(&action.sa_mask);

    const int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT};
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++)
        sigaction(signals[i], &action, NULL);
}

static int create_wake_pipe(void)
{
    int fds[2];
    /* Windows creates non-inheritable pipes and readiness-probes before each read; POSIX needs
     * explicit CLOEXEC and nonblocking state to close the poll/read race. */
    if (pipe(fds) < 0)
        return -1;
#ifndef _WIN32
    if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(fds[1], F_SETFD, FD_CLOEXEC) < 0)
        goto fail;

    int flags = fcntl(fds[0], F_GETFL, 0);
    if (flags < 0 || fcntl(fds[0], F_SETFL, flags | O_NONBLOCK) < 0)
        goto fail;
#endif

    watcher.wake_read_fd = fds[0];
    watcher.wake_write_fd = fds[1];
    return 0;

#ifndef _WIN32
fail:
    close(fds[0]);
    close(fds[1]);
    return -1;
#endif
}

void interrupt_init(void)
{
    if (watcher.initialized)
        return;
    watcher.initialized = 1;

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return;

    /* Install restoration before later initialization steps can fail. */
    watcher.interactive_terminal = 1;
    atexit(restore_terminal);
    interrupt_install_fatal_signal_handlers();

    if (tcgetattr(STDIN_FILENO, &watcher.saved_termios) < 0)
        return;
    watcher.saved_termios_valid = 1;

    if (pthread_mutex_init(&watcher.mutex, NULL) != 0)
        return;
    if (pthread_cond_init(&watcher.state_changed, NULL) != 0)
        goto destroy_mutex;
    if (create_wake_pipe() < 0)
        goto destroy_condition;
    if (pthread_create(&watcher.thread, NULL, watcher_thread, NULL) != 0)
        goto close_pipe;

    watcher.started = 1;
    return;

close_pipe:
    close(watcher.wake_read_fd);
    close(watcher.wake_write_fd);
    watcher.wake_read_fd = -1;
    watcher.wake_write_fd = -1;
destroy_condition:
    pthread_cond_destroy(&watcher.state_changed);
destroy_mutex:
    pthread_mutex_destroy(&watcher.mutex);
}

static void wake_watcher(void)
{
    char byte = 0;
    ssize_t result;
    do {
        result = write(watcher.wake_write_fd, &byte, 1);
    } while (result < 0 && errno == EINTR);
}

void interrupt_arm(void)
{
    if (!watcher.started)
        return;

    pthread_mutex_lock(&watcher.mutex);
    if (!watcher.armed && enter_raw_mode() == 0) {
        watcher.armed = 1;
        pthread_cond_broadcast(&watcher.state_changed);
    }
    pthread_mutex_unlock(&watcher.mutex);
}

void interrupt_disarm(void)
{
    if (!watcher.started)
        return;

    pthread_mutex_lock(&watcher.mutex);
    int was_armed = watcher.armed;
    watcher.armed = 0;
    pthread_cond_broadcast(&watcher.state_changed);
    pthread_mutex_unlock(&watcher.mutex);
    if (!was_armed)
        return;

    wake_watcher();

    /* Restoring canonical mode before the watcher stops could block its next read indefinitely. */
    pthread_mutex_lock(&watcher.mutex);
    while (!watcher.idle)
        pthread_cond_wait(&watcher.state_changed, &watcher.mutex);
    leave_raw_mode();
    pthread_mutex_unlock(&watcher.mutex);

    tcflush(STDIN_FILENO, TCIFLUSH);
}

int interrupt_pause_requested(void)
{
    return atomic_load(&watcher.pause_requested);
}

int interrupt_abort_requested(void)
{
    return atomic_load(&watcher.abort_requested);
}

static int stdin_has_pending_input(void)
{
    struct pollfd stdin_poll = {.fd = STDIN_FILENO, .events = POLLIN};
    int result = poll(&stdin_poll, 1, 0);
    return result > 0 && (stdin_poll.revents & POLLIN);
}

void interrupt_resolve_pending_escape(void)
{
    if (!watcher.started)
        return;

    /* Account for input queued before the watcher publishes its classifier state. */
    int remaining_ms = ESCAPE_TIMEOUT_MS + 2 * ESCAPE_POLL_INTERVAL_MS;
    const struct timespec interval = {
        .tv_sec = 0,
        .tv_nsec = ESCAPE_POLL_INTERVAL_MS * 1000000L,
    };
    while (remaining_ms > 0) {
        if (atomic_load(&watcher.abort_requested))
            return;
        if (!atomic_load(&watcher.escape_pending) && !stdin_has_pending_input())
            return;
        nanosleep(&interval, NULL);
        remaining_ms -= ESCAPE_POLL_INTERVAL_MS;
    }
}

void interrupt_clear_requests(void)
{
    atomic_store(&watcher.abort_requested, 0);
    atomic_store(&watcher.pause_requested, 0);
}
