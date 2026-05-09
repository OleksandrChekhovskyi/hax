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

/* Braille spinner — ten frames at 80ms each give ~12fps, enough to
 * feel alive without being distracting. The frame index is derived
 * from monotonic time at draw time (see spinner_glyph_now), so
 * multiple paints within the same 80ms window show the same glyph
 * and the visible animation tracks wall time rather than tick count. */
static const char *const FRAMES[] = {
    "\xE2\xA0\x8B", "\xE2\xA0\x99", "\xE2\xA0\xB9", "\xE2\xA0\xB8", "\xE2\xA0\xBC",
    "\xE2\xA0\xB4", "\xE2\xA0\xA6", "\xE2\xA0\xA7", "\xE2\xA0\x87", "\xE2\xA0\x8F",
};
#define N_FRAMES          (sizeof(FRAMES) / sizeof(FRAMES[0]))
#define FRAME_INTERVAL_MS 80

/* After this long in SPINNER_INLINE_HEADER, the thread transitions
 * back to SPINNER_LINE. Inline mode has no label, so a long pause
 * there leaves the user unable to distinguish reasoning from a
 * network stall — the transition surfaces the label once the wait
 * stops feeling interactive. Reset on every show, so a cluster of
 * fast back-to-back tool dispatches stays inline indefinitely. */
#define INLINE_TIMEOUT_MS 3000

struct spinner {
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    char *label;
    enum spinner_mode mode;    /* SPINNER_OFF when not visible */
    int inline_pad;            /* SPINNER_INLINE_TEXT: draw a leading space cell */
    int inline_drawn;          /* SPINNER_INLINE_*: glyph currently on screen */
    long inline_shown_at;      /* SPINNER_INLINE_HEADER: ms when shown (auto-transition) */
    char *tool_status_content; /* SPINNER_TOOL_STATUS: row content */
    int stop;                  /* thread should exit */
    int started;               /* thread was successfully created */
    int enabled;               /* stdout is a tty */
};

static int is_inline_mode(enum spinner_mode m)
{
    return m == SPINNER_INLINE_HEADER || m == SPINNER_INLINE_TEXT;
}

const char *spinner_glyph_now(void)
{
    long ms = monotonic_ms();
    if (ms < 0)
        ms = 0;
    size_t frame = (size_t)(ms / FRAME_INTERVAL_MS) % N_FRAMES;
    return FRAMES[frame];
}

static void erase_line_locked(void)
{
    fputs("\r" ANSI_ERASE_LINE, stdout);
    fflush(stdout);
}

/* Erase the inline glyph (and the leading-space cell when with_pad is
 * set) and return the cursor to the position the caller occupied
 * before show. Backspace + space + backspace per cell. */
static void erase_inline_glyph(int with_pad)
{
    if (with_pad)
        fputs("\b\b  \b\b", stdout);
    else
        fputs("\b \b", stdout);
}

/* Draw a frame for the current mode. Frame glyph is clock-derived.
 *
 * The !started gate skips writes for the thread-driven and label-mode
 * draws on non-tty stdout (avoids spinner noise in piped output). The
 * SPINNER_TOOL_STATUS branch deliberately writes regardless of
 * started: tool_render delegates its row painting here, and tests /
 * piped agent runs need the bytes to appear. The thread itself isn't
 * created in non-tty mode (started stays 0), so animation only happens
 * synchronously on content updates in that path — same volume of
 * output as tool_render's previous inline-paint design. */
static void draw_frame_locked(struct spinner *s)
{
    const char *glyph = spinner_glyph_now();
    switch (s->mode) {
    case SPINNER_OFF:
        return;
    case SPINNER_INLINE_HEADER:
    case SPINNER_INLINE_TEXT: {
        if (!s->started)
            return;
        /* Caller wrote header / model text and the cursor sits where
         * the glyph belongs. Backspace over the previous glyph if any
         * so the new one lands in the same cell. inline_pad emits a
         * leading space before the very first glyph, then leaves it
         * alone on subsequent animation frames (\b only steps back
         * over the glyph cell, the space stays). */
        if (!s->inline_drawn && s->inline_pad)
            fputc(' ', stdout);
        if (s->inline_drawn)
            fputc('\b', stdout);
        /* SPINNER_INLINE_TEXT glyphs ride on top of model text that
         * may be inside an open markdown style (inline code, bold,
         * italic, code fence). Emitting our own ANSI_DIM + ANSI_RESET
         * would clear the terminal's SGR mid-span while md_renderer
         * still thinks the style is open — subsequent text would
         * render unstyled until md closes the span. So the glyph
         * draws plain and inherits whatever style is active. The
         * SPINNER_INLINE_HEADER path lives in agent display
         * territory outside md, so dim there is fine. */
        if (s->mode == SPINNER_INLINE_HEADER)
            fputs(ANSI_DIM, stdout);
        fputs(glyph, stdout);
        if (s->mode == SPINNER_INLINE_HEADER)
            fputs(ANSI_RESET, stdout);
        s->inline_drawn = 1;
        fflush(stdout);
        return;
    }
    case SPINNER_LINE: {
        if (!s->started)
            return;
        /* Erase first so a label swap (set_label while visible) doesn't
         * leave trailing chars from a longer previous label. */
        fputs("\r" ANSI_ERASE_LINE ANSI_DIM, stdout);
        fputs(glyph, stdout);
        fputc(' ', stdout);
        fputs(s->label, stdout);
        fputs(ANSI_RESET, stdout);
        fflush(stdout);
        return;
    }
    case SPINNER_TOOL_STATUS: {
        /* Two-tone gutter row: cyan glyph (matches the tool block's
         * box-drawing strip color) followed by dim content. \r +
         * erase-line clears stale content from the previous paint
         * so a shorter line doesn't inherit trailing chars. */
        fputs("\r" ANSI_ERASE_LINE ANSI_DIM_CYAN, stdout);
        fputs(glyph, stdout);
        fputs(" " ANSI_RESET ANSI_DIM, stdout);
        fputs(s->tool_status_content ? s->tool_status_content : "", stdout);
        fputs(ANSI_RESET, stdout);
        fflush(stdout);
        return;
    }
    }
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
        if (s->mode != SPINNER_OFF) {
            /* Auto-transition SPINNER_INLINE_HEADER → SPINNER_LINE if
             * the inline glyph has been visible without a refresh for
             * too long. The user has lost the ability to tell apart
             * reasoning vs. a network stall (no label inline);
             * switching to the labeled line spinner restores that
             * signal. We do the transition under the mutex so an
             * in-flight spinner_hide observes either fully-inline or
             * fully-line state, never a torn snapshot.
             *
             * The transition writes \n directly to stdout, bypassing
             * disp's trail/held tracking. Callers that depend on disp
             * accounting use spinner_hide's return to detect the
             * post-transition "cursor at col 0 of empty row" state. */
            if (s->mode == SPINNER_INLINE_HEADER &&
                monotonic_ms() - s->inline_shown_at >= INLINE_TIMEOUT_MS) {
                if (s->inline_drawn) {
                    erase_inline_glyph(s->inline_pad);
                    s->inline_drawn = 0;
                }
                fputc('\n', stdout);
                s->mode = SPINNER_LINE;
                s->inline_pad = 0;
                /* Fall through to draw_frame_locked for the line draw. */
            }
            draw_frame_locked(s);

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

/* Set up state for the new mode and emit the first frame synchronously
 * so the spinner is on screen the moment this returns rather than
 * waiting for the thread to be scheduled. The thread will redraw at
 * its next tick (same glyph if within the 80ms frame window, no
 * visible change). */
static void show_locked(struct spinner *s, enum spinner_mode mode)
{
    enum spinner_mode prev = s->mode;
    s->mode = mode;
    if (mode == SPINNER_INLINE_HEADER)
        s->inline_shown_at = monotonic_ms();
    if (prev != mode) {
        s->inline_drawn = 0; /* fresh draw — no prior glyph to backspace over */
        draw_frame_locked(s);
    }
}

void spinner_show(struct spinner *s)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->mu);
    enum spinner_mode prev = s->mode;
    s->inline_pad = 0;
    show_locked(s, SPINNER_LINE);
    pthread_mutex_unlock(&s->mu);
    if (prev == SPINNER_OFF && s->started)
        pthread_cond_signal(&s->cv);
}

void spinner_show_inline_header(struct spinner *s)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->mu);
    enum spinner_mode prev = s->mode;
    s->inline_pad = 0;
    show_locked(s, SPINNER_INLINE_HEADER);
    pthread_mutex_unlock(&s->mu);
    if (prev == SPINNER_OFF && s->started)
        pthread_cond_signal(&s->cv);
}

void spinner_show_inline_text(struct spinner *s, int pad)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->mu);
    enum spinner_mode prev = s->mode;
    s->inline_pad = pad ? 1 : 0;
    show_locked(s, SPINNER_INLINE_TEXT);
    pthread_mutex_unlock(&s->mu);
    if (prev == SPINNER_OFF && s->started)
        pthread_cond_signal(&s->cv);
}

void spinner_show_tool_status(struct spinner *s, const char *content)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->mu);
    enum spinner_mode prev = s->mode;
    free(s->tool_status_content);
    s->tool_status_content = xstrdup(content ? content : "");
    s->inline_pad = 0;
    show_locked(s, SPINNER_TOOL_STATUS);
    pthread_mutex_unlock(&s->mu);
    if (prev == SPINNER_OFF && s->started)
        pthread_cond_signal(&s->cv);
}

void spinner_set_tool_status_content(struct spinner *s, const char *content)
{
    if (!s)
        return;
    const char *next = content ? content : "";
    pthread_mutex_lock(&s->mu);
    if (s->mode == SPINNER_TOOL_STATUS) {
        if (!s->tool_status_content || strcmp(s->tool_status_content, next) != 0) {
            free(s->tool_status_content);
            s->tool_status_content = xstrdup(next);
            /* No synchronous repaint here. The thread's next tick
             * (~80ms on TTY) picks up the new content; on non-TTY
             * the thread doesn't run and the update stays silent
             * until the next show / hide / mode change.
             *
             * This caps terminal I/O at the spinner's tick rate
             * regardless of how many post-cap content updates the
             * tool emits — without it, a 1000-line suppressed run
             * would emit 1000 \r-rewind paint sequences (visible to
             * humans as flicker, problematic for non-TTY captures
             * where every "elided" line lands in the byte stream
             * despite the footer saying it was suppressed). */
        }
    }
    pthread_mutex_unlock(&s->mu);
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
        if (s->mode == SPINNER_LINE)
            draw_frame_locked(s);
    }
    pthread_mutex_unlock(&s->mu);
}

static void erase_inline_locked(struct spinner *s)
{
    if (!s->inline_drawn)
        return;
    erase_inline_glyph(s->inline_pad);
    fflush(stdout);
    s->inline_drawn = 0;
}

int spinner_hide(struct spinner *s)
{
    if (!s)
        return 0;
    pthread_mutex_lock(&s->mu);
    int was_inline = 0;
    if (s->mode != SPINNER_OFF) {
        was_inline = is_inline_mode(s->mode);
        /* No-tty path: skip the actual erase (nothing drawn) but still
         * reset state and report the real last-requested mode so
         * callers' silent-cluster bookkeeping picks the right branch. */
        if (s->started || s->mode == SPINNER_TOOL_STATUS) {
            if (is_inline_mode(s->mode))
                erase_inline_locked(s);
            else
                erase_line_locked();
        }
        s->mode = SPINNER_OFF;
        s->inline_pad = 0;
        free(s->tool_status_content);
        s->tool_status_content = NULL;
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
        if (s->mode != SPINNER_OFF) {
            if (is_inline_mode(s->mode))
                erase_inline_locked(s);
            else
                erase_line_locked();
            s->mode = SPINNER_OFF;
        }
        s->stop = 1;
        pthread_mutex_unlock(&s->mu);
        pthread_cond_signal(&s->cv);
        pthread_join(s->thread, NULL);
    }
    pthread_mutex_destroy(&s->mu);
    pthread_cond_destroy(&s->cv);
    free(s->label);
    free(s->tool_status_content);
    free(s);
}
