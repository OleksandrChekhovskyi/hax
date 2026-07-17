/* SPDX-License-Identifier: MIT */
#include "render/spinner.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "util.h"
#include "terminal/ansi.h"
#include "terminal/theme.h"

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

/* The armed elapsed-time prefix stays hidden until the user turn is
 * this old — its job is "this is taking long", so it should not
 * decorate routine turns. Once shown it stays: elapsed only grows. */
#define TIMER_MIN_MS 30000

/* How long a requested label state must stay stable before the
 * displayed label switches to it — long enough that transient states
 * never surface, short enough that a sustained one gets named while
 * the user is still watching the wait. */
#define LABEL_SETTLE_MS 2000

struct spinner {
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    char *label;         /* displayed label text */
    char *label_key;     /* stability key of the displayed label */
    char *pending_label; /* requested label awaiting settle; NULL = none */
    char *pending_key;
    long pending_since; /* monotonic_ms the pending key was first requested */
    /* monotonic_ms of the first request contradicting the displayed
     * key since it was last confirmed; 0 = unchallenged. Only requests
     * arm it — silence (a long tool run with no events) means the
     * state hasn't changed, so the label stays. */
    long conflict_since;
    long timer_base;           /* monotonic_ms user-turn start; 0 = no elapsed counter */
    enum spinner_mode mode;    /* SPINNER_OFF when not visible */
    int park_rows;             /* rows the parked excursion moved down; 0 = drawn in place */
    int park_col;              /* origin column to restore; <= 0 restores column 0 */
    char *tool_status_content; /* SPINNER_TOOL_STATUS: row content */
    int stop;                  /* thread should exit */
    int started;               /* thread was successfully created */
    int enabled;               /* stdout is a tty */
};

const char *spinner_glyph_now(void)
{
    long ms = monotonic_ms();
    if (ms < 0)
        ms = 0;
    size_t frame = (size_t)(ms / FRAME_INTERVAL_MS) % N_FRAMES;
    return FRAMES[frame];
}

/* Draw a frame for the current mode. The !started gate keeps
 * SPINNER_LINE off non-tty stdout; SPINNER_TOOL_STATUS writes
 * regardless — tool_render delegates its row painting here and
 * non-tty capture needs the bytes. */
static void draw_frame_locked(struct spinner *s)
{
    const char *glyph = spinner_glyph_now();
    switch (s->mode) {
    case SPINNER_OFF:
        return;
    case SPINNER_LINE: {
        if (!s->started)
            return;
        /* Draw before erasing the row tail so sync-less terminals don't
         * show a blank row between frames. The elapsed counter leads
         * the label so its column survives label swaps. Everything is
         * clamped to one physical row (last column reserved against
         * deferred autowrap): a wrapped row would break both the
         * \r-repaint and a parked hide's fixed cursor-up distance.
         * Labels are ASCII, so bytes == cells. */
        int budget = term_width() - 1 - 2; /* minus glyph + space */
        fputs("\r" ANSI_DIM, stdout);
        fputs(glyph, stdout);
        fputc(' ', stdout);
        if (s->timer_base > 0) {
            long elapsed = monotonic_ms() - s->timer_base;
            if (elapsed >= TIMER_MIN_MS) {
                char t[32];
                format_duration(t, sizeof(t), elapsed);
                if ((int)(strlen(t) + 3 + strlen(s->label)) <= budget) {
                    fputs(t, stdout);
                    fputs(" \xC2\xB7 ", stdout);
                    budget -= (int)strlen(t) + 3;
                }
            }
        }
        size_t lab = strlen(s->label);
        if (budget < 0)
            budget = 0;
        if (lab > (size_t)budget)
            lab = (size_t)budget;
        fwrite(s->label, 1, lab, stdout);
        fputs(ANSI_RESET ANSI_ERASE_LINE, stdout);
        fflush(stdout);
        return;
    }
    case SPINNER_TOOL_STATUS: {
        /* Two-tone gutter row: quiet-chrome glyph (matches the tool
         * block's box-drawing strip style) followed by dim content.
         * Erase after drawing so shorter updates don't leave trailing
         * chars without blanking the row first. */
        fputs("\r", stdout);
        fputs(theme_open(THEME_CHROME_DIM), stdout);
        fputs(glyph, stdout);
        fputs(" " ANSI_RESET ANSI_DIM, stdout);
        fputs(s->tool_status_content ? s->tool_status_content : "", stdout);
        fputs(ANSI_RESET ANSI_ERASE_LINE, stdout);
        fflush(stdout);
        return;
    }
    }
}

static void drop_pending_locked(struct spinner *s)
{
    free(s->pending_label);
    free(s->pending_key);
    s->pending_label = NULL;
    s->pending_key = NULL;
}

/* Two-sided settling, called from the animation thread and at show
 * time. Promote: a request that stayed stable for LABEL_SETTLE_MS
 * becomes the displayed label. Demote: a specific label contradicted
 * for that long with no challenger settling falls back to neutral —
 * churn must not leave a stale claim up indefinitely. Promote is
 * checked first so a single stable challenger switches cleanly
 * instead of detouring through "working...". */
static void settle_label_locked(struct spinner *s)
{
    long now = monotonic_ms();
    if (s->pending_key && now - s->pending_since >= LABEL_SETTLE_MS) {
        free(s->label);
        free(s->label_key);
        s->label = s->pending_label;
        s->label_key = s->pending_key;
        s->pending_label = NULL;
        s->pending_key = NULL;
        s->conflict_since = 0;
        return;
    }
    if (s->conflict_since && now - s->conflict_since >= LABEL_SETTLE_MS &&
        strcmp(s->label_key, "working") != 0) {
        free(s->label);
        free(s->label_key);
        s->label = xstrdup("working...");
        s->label_key = xstrdup("working");
        s->conflict_since = 0;
        /* A pending neutral request is satisfied by the demotion. */
        if (s->pending_key && strcmp(s->pending_key, "working") == 0)
            drop_pending_locked(s);
    }
}

/* Erase the spinner's row and, for a parked show, walk the cursor back
 * to the caller's saved position. Leaves mode/park state untouched —
 * callers reset those. The started/TOOL_STATUS gate mirrors the draw
 * side: line rows only ever hit a TTY, tool-status rows also appear in
 * non-tty capture and must be erased symmetrically there. */
static void erase_locked(struct spinner *s)
{
    if (!s->started && s->mode != SPINNER_TOOL_STATUS)
        return;
    fputs("\r" ANSI_ERASE_LINE, stdout);
    if (s->park_rows > 0) {
        fprintf(stdout, "\x1b[%dA", s->park_rows);
        if (s->park_col > 0)
            fprintf(stdout, "\x1b[%dG", s->park_col + 1);
    }
    fflush(stdout);
}

/* Enter `mode`, emitting the first frame synchronously so the spinner
 * is up the moment this returns. Any previous visible mode is unwound
 * first (erase + park restore) so the new row lands at the caller's
 * content position, not wherever the old excursion left the cursor. */
static void show_locked(struct spinner *s, enum spinner_mode mode, int park_rows, int park_col)
{
    settle_label_locked(s);
    if (s->mode == mode && s->park_rows == park_rows && s->park_col == park_col)
        return; /* already up in the right place; keep animating */
    if (s->mode != SPINNER_OFF)
        erase_locked(s);
    s->mode = mode;
    s->park_rows = park_rows;
    s->park_col = park_col;
    if (park_rows > 0 && s->started) {
        for (int i = 0; i < park_rows; i++)
            fputc('\n', stdout);
    }
    draw_frame_locked(s);
}

/* Wake the thread after a show so the animation loop starts ticking;
 * only needed on the OFF → visible edge (otherwise it's already in
 * the timed-wait loop). */
static void wake_thread_after_show(struct spinner *s, enum spinner_mode prev)
{
    if (prev == SPINNER_OFF && s->started)
        pthread_cond_signal(&s->cv);
}

void spinner_show(struct spinner *s)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->mu);
    enum spinner_mode prev = s->mode;
    show_locked(s, SPINNER_LINE, 0, 0);
    pthread_mutex_unlock(&s->mu);
    wake_thread_after_show(s, prev);
}

void spinner_park(struct spinner *s, int col)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->mu);
    enum spinner_mode prev = s->mode;
    /* col == 0 means the origin row is itself the blank gap; a
     * mid-line park needs an extra row so one blank line separates
     * the content line from the spinner. */
    int rows = col > 0 ? 2 : 1;
    show_locked(s, SPINNER_LINE, rows, col > 0 ? col : 0);
    pthread_mutex_unlock(&s->mu);
    wake_thread_after_show(s, prev);
}

void spinner_show_tool_status(struct spinner *s, const char *content)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->mu);
    enum spinner_mode prev = s->mode;
    free(s->tool_status_content);
    s->tool_status_content = xstrdup(content ? content : "");
    show_locked(s, SPINNER_TOOL_STATUS, 0, 0);
    pthread_mutex_unlock(&s->mu);
    wake_thread_after_show(s, prev);
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
            /* No synchronous repaint — the thread's next tick picks
             * the content up. Caps terminal I/O at the tick rate no
             * matter how fast the tool streams, and keeps non-TTY
             * captures (no thread there) from filling with repaints
             * of content the footer says was elided. */
        }
    }
    pthread_mutex_unlock(&s->mu);
}

void spinner_set_label(struct spinner *s, const char *key, const char *label)
{
    if (!s)
        return;
    const char *k = key && *key ? key : "working";
    const char *next = label && *label ? label : "working...";
    pthread_mutex_lock(&s->mu);
    /* Immediate set is authoritative "now" state: no stale deferred
     * request may promote over it, and it starts unchallenged. */
    drop_pending_locked(s);
    s->conflict_since = 0;
    if (strcmp(s->label_key, k) != 0) {
        free(s->label_key);
        s->label_key = xstrdup(k);
    }
    if (strcmp(s->label, next) != 0) {
        free(s->label);
        s->label = xstrdup(next);
        if (s->mode == SPINNER_LINE)
            draw_frame_locked(s);
    }
    pthread_mutex_unlock(&s->mu);
}

void spinner_request_label(struct spinner *s, const char *key, const char *label)
{
    if (!s)
        return;
    const char *k = key && *key ? key : "working";
    const char *next = label && *label ? label : "working...";
    pthread_mutex_lock(&s->mu);
    if (s->label_key && strcmp(s->label_key, k) == 0) {
        /* Confirmation: refresh the text in place; a pending switch
         * away was a momentary excursion — cancel it. */
        drop_pending_locked(s);
        s->conflict_since = 0;
        if (strcmp(s->label, next) != 0) {
            free(s->label);
            s->label = xstrdup(next);
            if (s->mode == SPINNER_LINE)
                draw_frame_locked(s);
        }
    } else if (s->pending_key && strcmp(s->pending_key, k) == 0) {
        /* Same candidate — clock keeps running, text stays fresh. */
        if (strcmp(s->pending_label, next) != 0) {
            free(s->pending_label);
            s->pending_label = xstrdup(next);
        }
    } else {
        drop_pending_locked(s);
        s->pending_label = xstrdup(next);
        s->pending_key = xstrdup(k);
        s->pending_since = monotonic_ms();
        /* The demotion clock arms on the first contradiction and
         * deliberately survives churn onto further keys — that churn
         * is exactly what demotion exists for. */
        if (!s->conflict_since)
            s->conflict_since = s->pending_since;
    }
    pthread_mutex_unlock(&s->mu);
}

void spinner_set_timer(struct spinner *s, long start_ms)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->mu);
    /* No repaint needed: the prefix only appears on the thread-driven
     * SPINNER_LINE row; the next tick picks the base up. */
    s->timer_base = start_ms > 0 ? start_ms : 0;
    pthread_mutex_unlock(&s->mu);
}

static void hide_locked(struct spinner *s)
{
    if (s->mode == SPINNER_OFF)
        return;
    erase_locked(s);
    s->mode = SPINNER_OFF;
    s->park_rows = 0;
    s->park_col = 0;
    free(s->tool_status_content);
    s->tool_status_content = NULL;
}

void spinner_hide(struct spinner *s)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->mu);
    hide_locked(s);
    pthread_mutex_unlock(&s->mu);
}

static void *spinner_thread(void *arg)
{
    struct spinner *s = arg;

    /* Block all signals so SIGINT/SIGPIPE/SIGWINCH are delivered to the
     * main thread, which owns the line editor, libcurl, and stdout's
     * line state. */
    sigset_t mask;
    sigfillset(&mask);
    pthread_sigmask(SIG_SETMASK, &mask, NULL);

    pthread_mutex_lock(&s->mu);
    while (!s->stop) {
        if (s->mode != SPINNER_OFF) {
            settle_label_locked(s);
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
    s->label_key = xstrdup("working");
    s->enabled = isatty(fileno(stdout));
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv, NULL);

    if (s->enabled) {
        if (pthread_create(&s->thread, NULL, spinner_thread, s) == 0)
            s->started = 1;
    }
    return s;
}

void spinner_free(struct spinner *s)
{
    if (!s)
        return;
    if (s->started) {
        pthread_mutex_lock(&s->mu);
        hide_locked(s);
        s->stop = 1;
        pthread_mutex_unlock(&s->mu);
        pthread_cond_signal(&s->cv);
        pthread_join(s->thread, NULL);
    }
    pthread_mutex_destroy(&s->mu);
    pthread_cond_destroy(&s->cv);
    free(s->label);
    free(s->label_key);
    free(s->pending_label);
    free(s->pending_key);
    free(s->tool_status_content);
    free(s);
}
