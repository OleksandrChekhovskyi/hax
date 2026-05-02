/* SPDX-License-Identifier: MIT */
#include "spinner.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ansi.h"
#include "util.h"

/* Braille spinner — ten frames at 80ms each give ~12fps, enough to feel alive
 * without being distracting. The thread owns one terminal line: visible=1
 * means the cursor's column is mid-frame and the next write must erase first. */
static const char *const FRAMES[] = {
    "\xE2\xA0\x8B", "\xE2\xA0\x99", "\xE2\xA0\xB9", "\xE2\xA0\xB8", "\xE2\xA0\xBC",
    "\xE2\xA0\xB4", "\xE2\xA0\xA6", "\xE2\xA0\xA7", "\xE2\xA0\x87", "\xE2\xA0\x8F",
};
#define N_FRAMES          (sizeof(FRAMES) / sizeof(FRAMES[0]))
#define FRAME_INTERVAL_MS 80

/* After this long in inline mode, the thread transitions back to the
 * full line-mode spinner ("⠋ working..." / "⠋ thinking..."). Inline
 * mode has no label, so a long pause there leaves the user unable to
 * distinguish reasoning from a network stall — the transition surfaces
 * the label once the wait stops feeling interactive. Reset on every
 * spinner_show_inline call, so a cluster of fast back-to-back tool
 * dispatches stays inline indefinitely. */
#define INLINE_TIMEOUT_MS 3000

static long monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

struct spinner {
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    char *label;
    int visible;          /* drawn on terminal right now */
    int inline_mode;      /* glyph-only, no line-erase, backspace-redraw */
    int inline_drawn;     /* under inline_mode, a glyph is currently on screen */
    long inline_shown_at; /* CLOCK_MONOTONIC ms when inline mode last started */
    int stop;             /* thread should exit */
    size_t frame;
    int started; /* thread was successfully created */
    int enabled; /* stdout is a tty */
};

static void erase_line_locked(void)
{
    fputs("\r" ANSI_ERASE_LINE, stdout);
    fflush(stdout);
}

static void draw_frame_locked(struct spinner *s)
{
    if (s->inline_mode) {
        /* Inline mode: caller wrote a header (e.g. "[read] foo.c ")
         * and the cursor is positioned where the glyph should sit.
         * Backspace over the previous glyph if any so the new glyph
         * lands in the same cell — frame width is one terminal cell,
         * \b moves cursor back by one cell. */
        if (s->inline_drawn)
            fputc('\b', stdout);
        fputs(ANSI_DIM, stdout);
        fputs(FRAMES[s->frame], stdout);
        fputs(ANSI_RESET, stdout);
        s->inline_drawn = 1;
        fflush(stdout);
        return;
    }
    /* Erase first so a label swap (set_label while visible) doesn't leave
     * trailing chars from a longer previous label. The animation tick
     * doesn't strictly need this — frame width is constant — but a
     * single ANSI_ERASE_LINE per frame is cheap. */
    fputs("\r" ANSI_ERASE_LINE ANSI_DIM, stdout);
    fputs(FRAMES[s->frame], stdout);
    fputc(' ', stdout);
    fputs(s->label, stdout);
    fputs(ANSI_RESET, stdout);
    fflush(stdout);
}

static void *spinner_thread(void *arg)
{
    struct spinner *s = arg;

    /* Block all signals so SIGINT/SIGPIPE/SIGWINCH are delivered to the
     * main thread, which owns libedit, libcurl, and stdout's line state. */
    sigset_t mask;
    sigfillset(&mask);
    pthread_sigmask(SIG_SETMASK, &mask, NULL);

    pthread_mutex_lock(&s->mu);
    while (!s->stop) {
        if (s->visible) {
            /* Auto-transition inline → line if the inline glyph has
             * been visible without a refresh for too long. The user has
             * lost the ability to tell apart reasoning vs. a network
             * stall (no label inline); switching to the labeled line
             * spinner restores that signal. We do the transition under
             * the mutex so an in-flight spinner_hide caller observes
             * either fully-inline or fully-line state, never a torn
             * mid-transition snapshot.
             *
             * The transition writes \n directly to stdout, bypassing
             * disp's trail/held tracking. Callers that depend on disp
             * accounting (cluster_terminate, dispatch_tool_call_silent)
             * use spinner_hide's return to detect the post-transition
             * "cursor at col 0 of empty row" state and adjust trail
             * accordingly. */
            if (s->inline_mode && monotonic_ms() - s->inline_shown_at >= INLINE_TIMEOUT_MS) {
                if (s->inline_drawn) {
                    fputs("\b \b", stdout);
                    s->inline_drawn = 0;
                }
                fputc('\n', stdout);
                s->inline_mode = 0;
                /* Fall through to draw_frame_locked which now picks the
                 * line-mode branch and renders the labeled spinner. */
            }
            draw_frame_locked(s);
            s->frame = (s->frame + 1) % N_FRAMES;

            /* CLOCK_REALTIME: pthread_cond_timedwait portably uses it
             * (macOS lacks pthread_condattr_setclock). Wall-clock jumps
             * during a turn are negligible for 80ms-grain animation. */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += FRAME_INTERVAL_MS * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec += ts.tv_nsec / 1000000000L;
                ts.tv_nsec %= 1000000000L;
            }
            pthread_cond_timedwait(&s->cv, &s->mu, &ts);
        } else {
            pthread_cond_wait(&s->cv, &s->mu);
        }
    }
    pthread_mutex_unlock(&s->mu);
    return NULL;
}

struct spinner *spinner_new(const char *label)
{
    struct spinner *s = xcalloc(1, sizeof(*s));
    s->label = xstrdup(label && *label ? label : "working...");
    s->enabled = isatty(fileno(stdout));
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv, NULL);

    if (s->enabled) {
        if (pthread_create(&s->thread, NULL, spinner_thread, s) == 0)
            s->started = 1;
    }
    return s;
}

static void show_locked(struct spinner *s, int inline_mode)
{
    int was = s->visible;
    s->visible = 1;
    s->inline_mode = inline_mode;
    if (inline_mode)
        s->inline_shown_at = monotonic_ms();
    if (!was) {
        /* Draw frame 0 synchronously so the spinner is on screen the
         * moment this returns, instead of waiting for the thread to be
         * scheduled. The thread will redraw frame 0 on its next wake
         * (same glyph, no visible change), then animate at 80ms cadence. */
        s->frame = 0;
        s->inline_drawn = 0; /* fresh inline draw — no prior glyph to backspace over */
        draw_frame_locked(s);
    }
}

void spinner_show(struct spinner *s)
{
    if (!s || !s->started)
        return;
    pthread_mutex_lock(&s->mu);
    int was = s->visible;
    show_locked(s, 0);
    pthread_mutex_unlock(&s->mu);
    if (!was)
        pthread_cond_signal(&s->cv);
}

void spinner_show_inline(struct spinner *s)
{
    if (!s || !s->started)
        return;
    pthread_mutex_lock(&s->mu);
    int was = s->visible;
    show_locked(s, 1);
    pthread_mutex_unlock(&s->mu);
    if (!was)
        pthread_cond_signal(&s->cv);
}

void spinner_set_label(struct spinner *s, const char *label)
{
    if (!s)
        return;
    const char *next = label && *label ? label : "working...";
    pthread_mutex_lock(&s->mu);
    if (strcmp(s->label, next) != 0) {
        free(s->label);
        s->label = xstrdup(next);
        if (s->visible)
            draw_frame_locked(s);
    }
    pthread_mutex_unlock(&s->mu);
}

static void erase_inline_locked(struct spinner *s)
{
    if (!s->inline_drawn)
        return;
    /* Backspace + space + backspace: cursor moves back over the glyph
     * cell, overwrites it with a space (clearing it visually), and
     * returns to the original cursor position before show_inline. */
    fputs("\b \b", stdout);
    fflush(stdout);
    s->inline_drawn = 0;
}

int spinner_hide(struct spinner *s)
{
    if (!s)
        return 0;
    /* No thread (stdout not a TTY): nothing was drawn, so the cursor
     * is wherever the caller left it — same invariant as inline mode. */
    if (!s->started)
        return 1;
    pthread_mutex_lock(&s->mu);
    int was_inline = 0;
    if (s->visible) {
        was_inline = s->inline_mode;
        if (s->inline_mode)
            erase_inline_locked(s);
        else
            erase_line_locked();
        s->visible = 0;
        s->inline_mode = 0;
    }
    pthread_mutex_unlock(&s->mu);
    return was_inline;
}

void spinner_free(struct spinner *s)
{
    if (!s)
        return;
    if (s->started) {
        pthread_mutex_lock(&s->mu);
        if (s->visible) {
            if (s->inline_mode)
                erase_inline_locked(s);
            else
                erase_line_locked();
            s->visible = 0;
        }
        s->stop = 1;
        pthread_mutex_unlock(&s->mu);
        pthread_cond_signal(&s->cv);
        pthread_join(s->thread, NULL);
    }
    pthread_mutex_destroy(&s->mu);
    pthread_cond_destroy(&s->cv);
    free(s->label);
    free(s);
}
