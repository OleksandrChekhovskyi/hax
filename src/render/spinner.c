/* SPDX-License-Identifier: MIT */
#include "render/spinner.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "buf.h"
#include "util.h"
#include "terminal/ansi.h"
#include "terminal/theme.h"
#include "terminal/width.h"
#include "text/width.h"

#define FRAME_INTERVAL_MS 80
#define LABEL_SETTLE_MS   2000
#define TIMER_MIN_MS      30000
/* An indicator that would be replaced this quickly reads as flicker, not feedback. */
#define LABEL_SHOW_GRACE_MS 300

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
    /* A swap bracket opened by spinner_swap_begin() and not yet closed. */
    int swap_open;
    /* A deferred label show waiting out its grace period on the animation thread. */
    int label_show_pending;
    long label_show_at_ms;
    int pending_parked_rows;
    int pending_origin_col;
    /* Prepared tool-status rows, newest last; the animated glyph overprints the last row. */
    char *tool_view_bytes[SPINNER_TOOL_VIEW_ROWS_MAX];
    int tool_view_cells[SPINNER_TOOL_VIEW_ROWS_MAX];
    int tool_view_count;
    int tool_view_dirty;
    struct spinner_tool_frame painted_frame;
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

/* The animated gutter overprints the row's first cell, so it must follow the row's erase. */
static void append_glyph_overprint(struct buf *out, const char *glyph)
{
    buf_append_str(out, "\r");
    buf_append_str(out, theme_open(THEME_CHROME_DIM));
    buf_append_str(out, glyph);
    buf_append_str(out, ANSI_RESET);
}

void spinner_build_tool_frame(struct buf *frame, const struct spinner_row *rows, int row_count,
                              const char *glyph, int terminal_cols,
                              const struct spinner_tool_frame *previous,
                              struct spinner_tool_frame *painted)
{
    buf_append_str(frame, ANSI_SYNC_BEGIN);
    if (previous) {
        /* The previous glyph overprint left the cursor on the last logical row's first physical
         * row, so only preceding rows contribute to the climb even after a resize rewrap. */
        int climb =
            reflow_physical_rows(previous->row_widths, previous->row_count - 1, terminal_cols);
        if (climb > 0) {
            char cursor_up[16];
            snprintf(cursor_up, sizeof(cursor_up), ANSI_CSI "%dA", climb);
            buf_append_str(frame, cursor_up);
        }
    }
    buf_append_str(frame, "\r");

    painted->row_count = 0;
    for (int row = 0; row < row_count; row++) {
        int last = row == row_count - 1;
        buf_append_str(frame, rows[row].bytes);
        if (painted->row_count < SPINNER_TOOL_VIEW_ROWS_MAX)
            painted->row_widths[painted->row_count++] = rows[row].cells;
        buf_append_str(frame, last ? ANSI_ERASE_BELOW : ANSI_ERASE_LINE "\r\n");
    }
    append_glyph_overprint(frame, glyph);
    buf_append_str(frame, ANSI_SYNC_END);
}

/* The cursor idles at the end of the glyph row between frames. */
static void draw_glyph_only_locked(const char *glyph)
{
    fputs("\r", stdout);
    fputs(theme_open(THEME_CHROME_DIM), stdout);
    fputs(glyph, stdout);
    fputs(ANSI_RESET, stdout);
    fflush(stdout);
}

static void draw_tool_view_locked(struct spinner *spinner, const char *glyph)
{
    if (!spinner->tool_view_dirty && spinner->painted_frame.row_count > 0) {
        draw_glyph_only_locked(glyph);
        return;
    }

    struct spinner_row rows[SPINNER_TOOL_VIEW_ROWS_MAX];
    for (int row = 0; row < spinner->tool_view_count; row++) {
        rows[row].bytes = spinner->tool_view_bytes[row];
        rows[row].cells = spinner->tool_view_cells[row];
    }

    struct buf frame;
    buf_init(&frame);
    struct spinner_tool_frame painted;
    spinner_build_tool_frame(&frame, rows, spinner->tool_view_count, glyph, term_width(),
                             spinner->painted_frame.row_count > 0 ? &spinner->painted_frame : NULL,
                             &painted);
    fwrite(frame.data ? frame.data : "", 1, frame.len, stdout);
    fflush(stdout);
    spinner->painted_frame = painted;
    spinner->tool_view_dirty = 0;
    buf_free(&frame);
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
        draw_tool_view_locked(spinner, glyph);
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

static void erase_locked(struct spinner *spinner)
{
    if (spinner->painted_frame.row_count > 0) {
        /* Recompute under the current width so a resize between paint and erase still clears
         * every row. The cursor rests on the last logical row's first physical row, so only
         * preceding rows contribute to the climb. */
        int terminal_cols = term_width();
        int climb = reflow_physical_rows(spinner->painted_frame.row_widths,
                                         spinner->painted_frame.row_count - 1, terminal_cols);
        int last_row_wraps =
            spinner->painted_frame.row_widths[spinner->painted_frame.row_count - 1] > terminal_cols;
        fputs("\r", stdout);
        if (climb > 0)
            fprintf(stdout, "\x1b[%dA", climb);
        /* Erase-below only for multi-row extents: its screen-wide damage makes multiplexers
         * repaint far more than the region. */
        if (climb > 0 || last_row_wraps)
            fputs(ANSI_ERASE_BELOW, stdout);
        else
            fputs(ANSI_ERASE_LINE, stdout);
        spinner->painted_frame.row_count = 0;
        fflush(stdout);
        return;
    }

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
    spinner->label_show_pending = 0;
    settle_label_locked(spinner);
    if (spinner->mode == mode && spinner->parked_rows == parked_rows &&
        spinner->origin_col == origin_col)
        return;

    /* A transition erases one indicator before painting the next; synchronized output keeps the
     * swap from rendering as a blank-and-repaint. */
    enum spinner_mode previous_mode = spinner->mode;
    int synced = previous_mode != SPINNER_HIDDEN;
    if (synced) {
        fputs(ANSI_SYNC_BEGIN, stdout);
        erase_locked(spinner);
    }

    spinner->mode = mode;
    spinner->parked_rows = parked_rows;
    spinner->origin_col = origin_col;
    for (int i = 0; i < parked_rows; i++)
        fputc('\n', stdout);
    draw_frame_locked(spinner);
    if (synced) {
        fputs(ANSI_SYNC_END, stdout);
        fflush(stdout);
    }

    if (previous_mode == SPINNER_HIDDEN)
        pthread_cond_signal(&spinner->wake);
}

/* A first appearance from hidden waits out a grace period on the animation thread, so a label
 * whose wait ends quickly never blinks. Transitions from a visible mode stay immediate, and so
 * does a show inside an open swap bracket: there the erase and this repaint land in one
 * synchronized frame, while deferring would end the bracket with the indicator erased. */
static void request_label_show_locked(struct spinner *spinner, int parked_rows, int origin_col)
{
    if (spinner->mode != SPINNER_HIDDEN || spinner->swap_open) {
        show_locked(spinner, SPINNER_LABEL, parked_rows, origin_col);
        return;
    }
    spinner->label_show_pending = 1;
    spinner->label_show_at_ms = monotonic_ms() + LABEL_SHOW_GRACE_MS;
    spinner->pending_parked_rows = parked_rows;
    spinner->pending_origin_col = origin_col;
    pthread_cond_signal(&spinner->wake);
}

void spinner_show(struct spinner *spinner)
{
    if (!spinner)
        return;

    pthread_mutex_lock(&spinner->mutex);
    request_label_show_locked(spinner, 0, 0);
    pthread_mutex_unlock(&spinner->mutex);
}

void spinner_park(struct spinner *spinner, int cursor_col)
{
    if (!spinner)
        return;

    pthread_mutex_lock(&spinner->mutex);
    /* At column zero the current empty row is the gap; an open line needs another row. */
    int parked_rows = cursor_col > 0 ? 2 : 1;
    request_label_show_locked(spinner, parked_rows, cursor_col > 0 ? cursor_col : 0);
    pthread_mutex_unlock(&spinner->mutex);
}

static void free_tool_view_locked(struct spinner *spinner)
{
    for (int row = 0; row < spinner->tool_view_count; row++) {
        free(spinner->tool_view_bytes[row]);
        spinner->tool_view_bytes[row] = NULL;
    }
    spinner->tool_view_count = 0;
}

static int tool_view_equals_locked(const struct spinner *spinner, const struct spinner_row *rows,
                                   int count)
{
    if (spinner->tool_view_count != count)
        return 0;
    for (int row = 0; row < count; row++) {
        if (spinner->tool_view_cells[row] != rows[row].cells ||
            strcmp(spinner->tool_view_bytes[row], rows[row].bytes ? rows[row].bytes : "") != 0)
            return 0;
    }
    return 1;
}

void spinner_set_tool_status_view(struct spinner *spinner, const struct spinner_row *rows,
                                  int count)
{
    if (!spinner || count < 1)
        return;

    if (count > SPINNER_TOOL_VIEW_ROWS_MAX) {
        rows += count - SPINNER_TOOL_VIEW_ROWS_MAX;
        count = SPINNER_TOOL_VIEW_ROWS_MAX;
    }

    pthread_mutex_lock(&spinner->mutex);
    if (!tool_view_equals_locked(spinner, rows, count)) {
        free_tool_view_locked(spinner);
        for (int row = 0; row < count; row++) {
            spinner->tool_view_bytes[row] = xstrdup(rows[row].bytes ? rows[row].bytes : "");
            spinner->tool_view_cells[row] = rows[row].cells;
        }
        spinner->tool_view_count = count;
        spinner->tool_view_dirty = 1;
    }
    show_locked(spinner, SPINNER_TOOL_STATUS, 0, 0);
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
    spinner->label_show_pending = 0;
    if (spinner->mode == SPINNER_HIDDEN)
        return;

    erase_locked(spinner);
    spinner->mode = SPINNER_HIDDEN;
    spinner->parked_rows = 0;
    spinner->origin_col = 0;
    free_tool_view_locked(spinner);
}

void spinner_hide(struct spinner *spinner)
{
    if (!spinner)
        return;

    pthread_mutex_lock(&spinner->mutex);
    hide_locked(spinner);
    pthread_mutex_unlock(&spinner->mutex);
}

void spinner_swap_begin(struct spinner *spinner)
{
    if (!spinner)
        return;

    /* The bracket must open under the lock: a frame drawn between opening and hiding would end
     * it before the caller's replacement writes. */
    pthread_mutex_lock(&spinner->mutex);
    if (spinner->mode != SPINNER_HIDDEN) {
        fputs(ANSI_SYNC_BEGIN, stdout);
        spinner->swap_open = 1;
    }
    hide_locked(spinner);
    pthread_mutex_unlock(&spinner->mutex);
}

void spinner_swap_end(struct spinner *spinner)
{
    if (!spinner)
        return;

    pthread_mutex_lock(&spinner->mutex);
    if (spinner->swap_open) {
        fputs(ANSI_SYNC_END, stdout);
        fflush(stdout);
        spinner->swap_open = 0;
    }
    pthread_mutex_unlock(&spinner->mutex);
}

/* pthread_cond_timedwait portably uses CLOCK_REALTIME; macOS cannot select CLOCK_MONOTONIC. */
static struct timespec deadline_after_ms(long wait_ms)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += wait_ms / 1000;
    deadline.tv_nsec += (wait_ms % 1000) * 1000000L;
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
            if (!spinner->label_show_pending) {
                pthread_cond_wait(&spinner->wake, &spinner->mutex);
                continue;
            }
            long wait_ms = spinner->label_show_at_ms - monotonic_ms();
            if (wait_ms <= 0) {
                show_locked(spinner, SPINNER_LABEL, spinner->pending_parked_rows,
                            spinner->pending_origin_col);
                continue;
            }
            struct timespec show_deadline = deadline_after_ms(wait_ms);
            pthread_cond_timedwait(&spinner->wake, &spinner->mutex, &show_deadline);
            continue;
        }

        settle_label_locked(spinner);
        draw_frame_locked(spinner);
        struct timespec deadline = deadline_after_ms(FRAME_INTERVAL_MS);
        pthread_cond_timedwait(&spinner->wake, &spinner->mutex, &deadline);
    }
    pthread_mutex_unlock(&spinner->mutex);
    return NULL;
}

struct spinner *spinner_new(const char *label)
{
    if (!isatty(fileno(stdout)))
        return NULL;

    struct spinner *spinner = xcalloc(1, sizeof(*spinner));
    spinner->displayed_label = xstrdup(label && *label ? label : DEFAULT_LABEL);
    spinner->displayed_key = xstrdup(DEFAULT_LABEL_KEY);
    pthread_mutex_init(&spinner->mutex, NULL);
    pthread_cond_init(&spinner->wake, NULL);

    if (pthread_create(&spinner->thread, NULL, spinner_thread, spinner) != 0) {
        pthread_mutex_destroy(&spinner->mutex);
        pthread_cond_destroy(&spinner->wake);
        free(spinner->displayed_label);
        free(spinner->displayed_key);
        free(spinner);
        return NULL;
    }
    return spinner;
}

void spinner_free(struct spinner *spinner)
{
    if (!spinner)
        return;

    pthread_mutex_lock(&spinner->mutex);
    hide_locked(spinner);
    /* Never leave the terminal in synchronized mode, even on an abandoned swap. */
    if (spinner->swap_open) {
        fputs(ANSI_SYNC_END, stdout);
        spinner->swap_open = 0;
    }
    spinner->stop_requested = 1;
    pthread_cond_signal(&spinner->wake);
    pthread_mutex_unlock(&spinner->mutex);

    pthread_join(spinner->thread, NULL);
    pthread_mutex_destroy(&spinner->mutex);
    pthread_cond_destroy(&spinner->wake);
    free(spinner->displayed_label);
    free(spinner->displayed_key);
    free(spinner->pending_label);
    free(spinner->pending_key);
    free(spinner);
}
