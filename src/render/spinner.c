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

#define FRAME_INTERVAL_MS 80
#define LABEL_SETTLE_MS   2000
#define TIMER_MIN_MS      30000

#define DEFAULT_LABEL     "working..."
#define DEFAULT_LABEL_KEY "working"

static const char *const SPINNER_FRAMES[] = {
    "\xE2\xA0\x8B", "\xE2\xA0\x99", "\xE2\xA0\xB9", "\xE2\xA0\xB8", "\xE2\xA0\xBC",
    "\xE2\xA0\xB4", "\xE2\xA0\xA6", "\xE2\xA0\xA7", "\xE2\xA0\x87", "\xE2\xA0\x8F",
};
#define SPINNER_FRAME_COUNT (sizeof(SPINNER_FRAMES) / sizeof(SPINNER_FRAMES[0]))

enum spinner_mode {
    SPINNER_HIDDEN = 0,
    SPINNER_LABEL,
    SPINNER_TOOL_STATUS,
};

struct spinner {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    enum spinner_mode mode;
    int stop_requested;
    int animation_thread_started;

    char *displayed_label;
    char *displayed_key;
    char *pending_label;
    char *pending_key;
    long pending_since_ms;
    /* The first request contradicting the displayed key. Silence does not contradict a label. */
    long contradicted_since_ms;

    long timer_started_at_ms;
    int parked_rows;
    int origin_col;
    char *tool_status_content;
};

const char *spinner_glyph_now(void)
{
    long now_ms = monotonic_ms();
    if (now_ms < 0)
        now_ms = 0;
    size_t frame = (size_t)(now_ms / FRAME_INTERVAL_MS) % SPINNER_FRAME_COUNT;
    return SPINNER_FRAMES[frame];
}

/* Erasing the old row tail after repaint avoids a blank frame without synchronized output. */
static void finish_row_repaint(void)
{
    fputs(ANSI_RESET ANSI_ERASE_LINE, stdout);
    fflush(stdout);
}

static void draw_label_row_locked(struct spinner *spinner, const char *glyph)
{
    if (!spinner->animation_thread_started)
        return;

    /* Reserve the last terminal column because filling it can trigger deferred autowrap. */
    int label_budget = term_width() - 1 - 2; /* glyph and separating space */
    if (label_budget < 0)
        label_budget = 0;

    fputs("\r" ANSI_DIM, stdout);
    fputs(glyph, stdout);
    fputc(' ', stdout);

    if (spinner->timer_started_at_ms > 0) {
        long elapsed_ms = monotonic_ms() - spinner->timer_started_at_ms;
        if (elapsed_ms >= TIMER_MIN_MS) {
            char duration[32];
            format_duration_steady(duration, sizeof(duration), elapsed_ms);
            size_t prefix_cells = strlen(duration) + 3; /* spaces and middle dot */
            if (prefix_cells + display_cells(spinner->displayed_label) <= (size_t)label_budget) {
                fputs(duration, stdout);
                fputs(" \xC2\xB7 ", stdout);
                label_budget -= (int)prefix_cells;
            }
        }
    }

    char *label = truncate_for_display(spinner->displayed_label, (size_t)label_budget);
    fputs(label, stdout);
    free(label);
    finish_row_repaint();
}

static void draw_tool_status_row_locked(const struct spinner *spinner, const char *glyph)
{
    fputs("\r", stdout);
    fputs(theme_open(THEME_CHROME_DIM), stdout);
    fputs(glyph, stdout);
    fputs(" " ANSI_RESET ANSI_DIM, stdout);
    fputs(spinner->tool_status_content ? spinner->tool_status_content : "", stdout);
    finish_row_repaint();
}

static void draw_frame_locked(struct spinner *spinner)
{
    const char *glyph = spinner_glyph_now();

    switch (spinner->mode) {
    case SPINNER_HIDDEN:
        break;
    case SPINNER_LABEL:
        draw_label_row_locked(spinner, glyph);
        break;
    case SPINNER_TOOL_STATUS:
        draw_tool_status_row_locked(spinner, glyph);
        break;
    }
}

static void clear_pending_label_locked(struct spinner *spinner)
{
    free(spinner->pending_label);
    free(spinner->pending_key);
    spinner->pending_label = NULL;
    spinner->pending_key = NULL;
}

static int set_displayed_label_locked(struct spinner *spinner, const char *key, const char *label)
{
    if (strcmp(spinner->displayed_key, key) != 0) {
        free(spinner->displayed_key);
        spinner->displayed_key = xstrdup(key);
    }
    if (strcmp(spinner->displayed_label, label) == 0)
        return 0;

    free(spinner->displayed_label);
    spinner->displayed_label = xstrdup(label);
    return 1;
}

/* A stable candidate replaces the displayed state. Sustained churn instead clears a stale claim. */
static void settle_label_locked(struct spinner *spinner)
{
    long now_ms = monotonic_ms();
    if (spinner->pending_key && now_ms - spinner->pending_since_ms >= LABEL_SETTLE_MS) {
        free(spinner->displayed_label);
        free(spinner->displayed_key);
        spinner->displayed_label = spinner->pending_label;
        spinner->displayed_key = spinner->pending_key;
        spinner->pending_label = NULL;
        spinner->pending_key = NULL;
        spinner->contradicted_since_ms = 0;
        return;
    }

    if (!spinner->contradicted_since_ms ||
        now_ms - spinner->contradicted_since_ms < LABEL_SETTLE_MS ||
        strcmp(spinner->displayed_key, DEFAULT_LABEL_KEY) == 0)
        return;

    set_displayed_label_locked(spinner, DEFAULT_LABEL_KEY, DEFAULT_LABEL);
    spinner->contradicted_since_ms = 0;
    if (spinner->pending_key && strcmp(spinner->pending_key, DEFAULT_LABEL_KEY) == 0)
        clear_pending_label_locked(spinner);
}

static void erase_locked(const struct spinner *spinner)
{
    if (!spinner->animation_thread_started && spinner->mode != SPINNER_TOOL_STATUS)
        return;

    fputs("\r" ANSI_ERASE_LINE, stdout);
    if (spinner->parked_rows > 0) {
        fprintf(stdout, "\x1b[%dA", spinner->parked_rows);
        if (spinner->origin_col > 0)
            fprintf(stdout, "\x1b[%dG", spinner->origin_col + 1);
    }
    fflush(stdout);
}

static void show_locked(struct spinner *spinner, enum spinner_mode mode, int parked_rows,
                        int origin_col)
{
    settle_label_locked(spinner);
    if (spinner->mode == mode && spinner->parked_rows == parked_rows &&
        spinner->origin_col == origin_col)
        return;

    enum spinner_mode previous_mode = spinner->mode;
    if (previous_mode != SPINNER_HIDDEN)
        erase_locked(spinner);

    spinner->mode = mode;
    spinner->parked_rows = parked_rows;
    spinner->origin_col = origin_col;
    if (parked_rows > 0 && spinner->animation_thread_started) {
        for (int i = 0; i < parked_rows; i++)
            fputc('\n', stdout);
    }
    draw_frame_locked(spinner);

    if (previous_mode == SPINNER_HIDDEN && spinner->animation_thread_started)
        pthread_cond_signal(&spinner->wake);
}

void spinner_show(struct spinner *spinner)
{
    if (!spinner)
        return;

    pthread_mutex_lock(&spinner->mutex);
    show_locked(spinner, SPINNER_LABEL, 0, 0);
    pthread_mutex_unlock(&spinner->mutex);
}

void spinner_park(struct spinner *spinner, int cursor_col)
{
    if (!spinner)
        return;

    pthread_mutex_lock(&spinner->mutex);
    /* At column zero the current empty row is the gap; an open line needs another row. */
    int parked_rows = cursor_col > 0 ? 2 : 1;
    show_locked(spinner, SPINNER_LABEL, parked_rows, cursor_col > 0 ? cursor_col : 0);
    pthread_mutex_unlock(&spinner->mutex);
}

void spinner_show_tool_status(struct spinner *spinner, const char *content)
{
    if (!spinner)
        return;

    pthread_mutex_lock(&spinner->mutex);
    free(spinner->tool_status_content);
    spinner->tool_status_content = xstrdup(content ? content : "");
    show_locked(spinner, SPINNER_TOOL_STATUS, 0, 0);
    pthread_mutex_unlock(&spinner->mutex);
}

void spinner_set_tool_status_content(struct spinner *spinner, const char *content)
{
    if (!spinner)
        return;

    const char *new_content = content ? content : "";
    pthread_mutex_lock(&spinner->mutex);
    if (spinner->mode == SPINNER_TOOL_STATUS &&
        (!spinner->tool_status_content || strcmp(spinner->tool_status_content, new_content) != 0)) {
        free(spinner->tool_status_content);
        spinner->tool_status_content = xstrdup(new_content);
    }
    pthread_mutex_unlock(&spinner->mutex);
}

void spinner_set_label(struct spinner *spinner, const char *key, const char *label)
{
    if (!spinner)
        return;

    const char *new_key = key && *key ? key : DEFAULT_LABEL_KEY;
    const char *new_label = label && *label ? label : DEFAULT_LABEL;
    pthread_mutex_lock(&spinner->mutex);
    clear_pending_label_locked(spinner);
    spinner->contradicted_since_ms = 0;
    int label_changed = set_displayed_label_locked(spinner, new_key, new_label);
    if (label_changed && spinner->mode == SPINNER_LABEL)
        draw_frame_locked(spinner);
    pthread_mutex_unlock(&spinner->mutex);
}

void spinner_request_label(struct spinner *spinner, const char *key, const char *label)
{
    if (!spinner)
        return;

    const char *requested_key = key && *key ? key : DEFAULT_LABEL_KEY;
    const char *requested_label = label && *label ? label : DEFAULT_LABEL;
    pthread_mutex_lock(&spinner->mutex);

    if (strcmp(spinner->displayed_key, requested_key) == 0) {
        clear_pending_label_locked(spinner);
        spinner->contradicted_since_ms = 0;
        int label_changed = set_displayed_label_locked(spinner, requested_key, requested_label);
        if (label_changed && spinner->mode == SPINNER_LABEL)
            draw_frame_locked(spinner);
    } else if (spinner->pending_key && strcmp(spinner->pending_key, requested_key) == 0) {
        if (strcmp(spinner->pending_label, requested_label) != 0) {
            free(spinner->pending_label);
            spinner->pending_label = xstrdup(requested_label);
        }
    } else {
        clear_pending_label_locked(spinner);
        spinner->pending_label = xstrdup(requested_label);
        spinner->pending_key = xstrdup(requested_key);
        spinner->pending_since_ms = monotonic_ms();
        /* Keep the first contradiction time across changing candidates so churn can demote. */
        if (!spinner->contradicted_since_ms)
            spinner->contradicted_since_ms = spinner->pending_since_ms;
    }

    pthread_mutex_unlock(&spinner->mutex);
}

void spinner_set_timer(struct spinner *spinner, long started_at_ms)
{
    if (!spinner)
        return;

    pthread_mutex_lock(&spinner->mutex);
    spinner->timer_started_at_ms = started_at_ms > 0 ? started_at_ms : 0;
    pthread_mutex_unlock(&spinner->mutex);
}

static void hide_locked(struct spinner *spinner)
{
    if (spinner->mode == SPINNER_HIDDEN)
        return;

    erase_locked(spinner);
    spinner->mode = SPINNER_HIDDEN;
    spinner->parked_rows = 0;
    spinner->origin_col = 0;
    free(spinner->tool_status_content);
    spinner->tool_status_content = NULL;
}

void spinner_hide(struct spinner *spinner)
{
    if (!spinner)
        return;

    pthread_mutex_lock(&spinner->mutex);
    hide_locked(spinner);
    pthread_mutex_unlock(&spinner->mutex);
}

/* pthread_cond_timedwait portably uses CLOCK_REALTIME; macOS cannot select CLOCK_MONOTONIC. */
static struct timespec next_frame_deadline(void)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += FRAME_INTERVAL_MS * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += deadline.tv_nsec / 1000000000L;
        deadline.tv_nsec %= 1000000000L;
    }
    return deadline;
}

static void *spinner_thread(void *arg)
{
    struct spinner *spinner = arg;

    /* Keep process signals on threads that own the foreground operation and terminal state. */
    sigset_t mask;
    sigfillset(&mask);
    pthread_sigmask(SIG_SETMASK, &mask, NULL);

    pthread_mutex_lock(&spinner->mutex);
    while (!spinner->stop_requested) {
        if (spinner->mode == SPINNER_HIDDEN) {
            pthread_cond_wait(&spinner->wake, &spinner->mutex);
            continue;
        }

        settle_label_locked(spinner);
        draw_frame_locked(spinner);
        struct timespec deadline = next_frame_deadline();
        pthread_cond_timedwait(&spinner->wake, &spinner->mutex, &deadline);
    }
    pthread_mutex_unlock(&spinner->mutex);
    return NULL;
}

struct spinner *spinner_new(const char *label)
{
    struct spinner *spinner = xcalloc(1, sizeof(*spinner));
    spinner->displayed_label = xstrdup(label && *label ? label : DEFAULT_LABEL);
    spinner->displayed_key = xstrdup(DEFAULT_LABEL_KEY);
    pthread_mutex_init(&spinner->mutex, NULL);
    pthread_cond_init(&spinner->wake, NULL);

    if (isatty(fileno(stdout)) &&
        pthread_create(&spinner->thread, NULL, spinner_thread, spinner) == 0)
        spinner->animation_thread_started = 1;
    return spinner;
}

void spinner_free(struct spinner *spinner)
{
    if (!spinner)
        return;

    pthread_mutex_lock(&spinner->mutex);
    hide_locked(spinner);
    spinner->stop_requested = 1;
    if (spinner->animation_thread_started)
        pthread_cond_signal(&spinner->wake);
    pthread_mutex_unlock(&spinner->mutex);

    if (spinner->animation_thread_started)
        pthread_join(spinner->thread, NULL);
    pthread_mutex_destroy(&spinner->mutex);
    pthread_cond_destroy(&spinner->wake);
    free(spinner->displayed_label);
    free(spinner->displayed_key);
    free(spinner->pending_label);
    free(spinner->pending_key);
    free(spinner->tool_status_content);
    free(spinner);
}
