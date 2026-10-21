/* SPDX-License-Identifier: MIT */
#include "render/footer.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "buf.h"
#include "xalloc.h"
#include "render/disp.h"
#include "render/spinner.h"
#include "system/clock.h"
#include "terminal/ansi.h"
#include "terminal/live_region.h"
#include "terminal/sgr.h"
#include "terminal/width.h"

/* An indicator that would be replaced this quickly reads as flicker, not feedback. */
#define STATUS_SHOW_GRACE_MS 300

enum footer_mode {
    FOOTER_HIDDEN = 0,
    FOOTER_STATUS,
    FOOTER_ROWS,
};

struct footer {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    enum footer_mode mode;
    int stop_requested;

    struct spinner_label label;
    footer_label_override_fn label_override; /* NULL when unset */
    void *label_override_user;
    long timer_started_at_ms;
    int parked_rows; /* from where output ends to the footer */
    int parked_cols; /* the terminal width when the footer moved below the output */
    /* A deferred status show waiting out its grace period on the animation thread. */
    int status_show_pending;
    long status_show_at_ms;
    /* Prepared rows, newest last, painted above a blank row and the status row. */
    char *row_bytes[FOOTER_ROWS_MAX];
    int row_cells[FOOTER_ROWS_MAX];
    int row_count;
    struct live_region region;

    struct disp *disp; /* borrowed; NULL when unattached */
    struct disp_live_area live;
    /* Stepped aside for the attached output; changes meanwhile are painted on resume. */
    int suspended;
    int suspended_synced;
};

void footer_build_frame(struct buf *frame, struct live_region *region,
                        const struct footer_row *rows, int row_count, int terminal_cols)
{
    buf_append_str(frame, ANSI_SYNC_BEGIN);
    /* Rows paint from the default style, whatever the output above left active. */
    buf_append_str(frame, ANSI_RESET);
    live_region_rewind(region, frame, terminal_cols);
    /* Rows prepared before a narrowing would wrap, and wide characters make their wrapped height
     * unpredictable; clipped, every row paints on one physical row. The last column stays free, as
     * filling it can trigger deferred autowrap. */
    int max_cells = terminal_cols > 1 ? terminal_cols - 1 : 1;
    struct buf clipped;
    buf_init(&clipped);
    for (int row = 0; row < row_count; row++) {
        const char *bytes = rows[row].bytes;
        size_t len = strlen(bytes);
        int cells = rows[row].cells;
        if (cells > max_cells) {
            buf_reset(&clipped);
            cells = clip_styled_row(&clipped, bytes, len, max_cells, 1);
            bytes = clipped.data ? clipped.data : "";
            len = clipped.len;
        }
        live_region_add_row(region, frame, bytes, len, cells);
    }
    buf_free(&clipped);
    /* The cursor is hidden while the footer shows, so it rests at the row's start, where no
     * terminal's rewrap can carry it off the row. */
    live_region_end_frame(region, frame, row_count - 1, 0);
    buf_append_str(frame, ANSI_SYNC_END);
}

static void write_frame_locked(struct buf *frame)
{
    fwrite(frame->data ? frame->data : "", 1, frame->len, stdout);
    fflush(stdout);
}

/* Returns the row's cells. */
static int format_status_locked(const struct footer *footer, struct buf *out)
{
    long elapsed_ms =
        footer->timer_started_at_ms > 0 ? monotonic_ms() - footer->timer_started_at_ms : 0;
    const char *label =
        footer->label_override ? footer->label_override(footer->label_override_user) : NULL;
    /* Reserve the last terminal column because filling it can trigger deferred autowrap. */
    return spinner_format_status(out, spinner_glyph_now(), label ? label : footer->label.text,
                                 elapsed_ms, term_width() - 1);
}

int footer_rows_that_fit(int row_count, int terminal_rows)
{
    int fit = terminal_rows - 3;
    if (fit < 0)
        return 0;
    return row_count < fit ? row_count : fit;
}

/* The whole frame repaints each tick, as the status row's glyph and timer change with it. */
static void draw_frame_locked(struct footer *footer)
{
    if (footer->mode == FOOTER_HIDDEN)
        return;

    struct footer_row rows[FOOTER_ROWS_MAX + 2];
    int count = 0;
    int prepared = 0;
    if (footer->mode == FOOTER_ROWS)
        prepared = footer_rows_that_fit(footer->row_count, term_height());
    if (prepared > 0) {
        /* The newest rows are kept. */
        for (int row = footer->row_count - prepared; row < footer->row_count; row++)
            rows[count++] = (struct footer_row){footer->row_bytes[row], footer->row_cells[row]};
        rows[count++] = (struct footer_row){"", 0};
    }
    struct buf status;
    buf_init(&status);
    int status_cells = format_status_locked(footer, &status);
    rows[count++] = (struct footer_row){status.data ? status.data : "", status_cells};

    struct buf frame;
    buf_init(&frame);
    footer_build_frame(&frame, &footer->region, rows, count, term_width());
    write_frame_locked(&frame);
    buf_free(&frame);
    buf_free(&status);
}

int footer_build_return(struct buf *out, const struct disp *disp, int gap_rows, int parked_cols,
                        int terminal_cols)
{
    size_t col = disp_screen_column(disp, terminal_cols);
    /* A row that fits on one screen row at both widths keeps its place. Terminals rewrap a longer
     * one on resize in their own ways, some not at all, and no cursor movement restores a pending
     * wrap: only the blank rows below the output keep their place then. */
    size_t narrower = (size_t)(parked_cols < terminal_cols ? parked_cols : terminal_cols);
    int rewrapped = parked_cols != terminal_cols && disp->column >= narrower;
    int below = gap_rows > 0 && disp->column > 0 && (rewrapped || col == (size_t)terminal_cols);
    if (below) {
        gap_rows--;
        col = 0;
    }
    char move[32];
    if (gap_rows > 0) {
        snprintf(move, sizeof(move), ANSI_CSI "%dA", gap_rows);
        buf_append_str(out, move);
    }
    if (col > 0) {
        snprintf(move, sizeof(move), ANSI_CSI "%zuG", col + 1);
        buf_append_str(out, move);
    }
    return below;
}

static void erase_locked(struct footer *footer)
{
    struct buf erase;
    buf_init(&erase);
    live_region_erase(&footer->region, &erase, term_width());
    /* Worked out now: a resize since painting rewraps the output. Unattached, the footer was
     * painted where the cursor was, and the erase leaves it there. */
    if (footer->disp && footer_build_return(&erase, footer->disp, footer->parked_rows,
                                            footer->parked_cols, term_width()))
        disp_note_row_break(footer->disp);
    write_frame_locked(&erase);
    buf_free(&erase);
    /* Frames end in a reset, so output written next would lose the styling it had open. */
    if (footer->disp) {
        sgr_emit(&footer->disp->style, stdout);
        fflush(stdout);
    }
}

/* Prepared rows continue the output, starting on the row after it. */
static int rows_gap(const struct disp *disp)
{
    return disp->column > 0 ? 1 : 0;
}

/* Move from where output ends to the footer's first row. */
static void park_locked(struct footer *footer, int rows)
{
    footer->parked_rows = rows;
    footer->parked_cols = term_width();
    for (int i = 0; i < rows; i++)
        fputc('\n', stdout);
}

static void show_locked(struct footer *footer, enum footer_mode mode, int parked_rows)
{
    footer->status_show_pending = 0;
    spinner_label_settle(&footer->label, monotonic_ms());
    if (footer->suspended) {
        footer->mode = mode;
        return;
    }
    if (footer->mode == mode && footer->parked_rows == parked_rows)
        return;

    /* A transition erases one indicator before painting the next; synchronized output keeps the
     * change from rendering as a blank-and-repaint. */
    enum footer_mode previous_mode = footer->mode;
    int synced = previous_mode != FOOTER_HIDDEN;
    if (synced) {
        fputs(ANSI_SYNC_BEGIN, stdout);
        erase_locked(footer);
    }

    footer->mode = mode;
    park_locked(footer, parked_rows);
    draw_frame_locked(footer);
    if (synced) {
        fputs(ANSI_SYNC_END, stdout);
        fflush(stdout);
    }

    if (previous_mode == FOOTER_HIDDEN)
        pthread_cond_signal(&footer->wake);
}

/* A change made while output is stepping around the footer is only recorded; closing the batch
 * paints it, in the same synchronized frame as the output. */
static void finish_output(struct footer *footer)
{
    if (footer->disp)
        disp_flush(footer->disp);
}

void footer_show(struct footer *footer)
{
    if (!footer)
        return;

    pthread_mutex_lock(&footer->mutex);
    show_locked(footer, FOOTER_STATUS, footer->disp ? footer_status_gap(footer->disp) : 0);
    pthread_mutex_unlock(&footer->mutex);
    finish_output(footer);
}

void footer_show_deferred(struct footer *footer)
{
    if (!footer)
        return;

    pthread_mutex_lock(&footer->mutex);
    if (footer->mode == FOOTER_HIDDEN) {
        footer->status_show_pending = 1;
        footer->status_show_at_ms = monotonic_ms() + STATUS_SHOW_GRACE_MS;
        pthread_cond_signal(&footer->wake);
    }
    pthread_mutex_unlock(&footer->mutex);
}

static void free_rows_locked(struct footer *footer)
{
    for (int row = 0; row < footer->row_count; row++) {
        free(footer->row_bytes[row]);
        footer->row_bytes[row] = NULL;
    }
    footer->row_count = 0;
}

static int rows_equal_locked(const struct footer *footer, const struct footer_row *rows, int count)
{
    if (footer->row_count != count)
        return 0;
    for (int row = 0; row < count; row++) {
        if (footer->row_cells[row] != rows[row].cells ||
            strcmp(footer->row_bytes[row], rows[row].bytes ? rows[row].bytes : "") != 0)
            return 0;
    }
    return 1;
}

void footer_set_rows(struct footer *footer, const struct footer_row *rows, int count)
{
    if (!footer || count < 1)
        return;

    if (count > FOOTER_ROWS_MAX) {
        rows += count - FOOTER_ROWS_MAX;
        count = FOOTER_ROWS_MAX;
    }

    pthread_mutex_lock(&footer->mutex);
    if (!rows_equal_locked(footer, rows, count)) {
        free_rows_locked(footer);
        for (int row = 0; row < count; row++) {
            footer->row_bytes[row] = xstrdup(rows[row].bytes ? rows[row].bytes : "");
            footer->row_cells[row] = rows[row].cells;
        }
        footer->row_count = count;
    }
    show_locked(footer, FOOTER_ROWS, footer->disp ? rows_gap(footer->disp) : 0);
    pthread_mutex_unlock(&footer->mutex);
    finish_output(footer);
}

void footer_set_label(struct footer *footer, const char *key, const char *label)
{
    if (!footer)
        return;

    pthread_mutex_lock(&footer->mutex);
    if (spinner_label_set(&footer->label, key, label) && footer->mode != FOOTER_HIDDEN &&
        !footer->suspended)
        draw_frame_locked(footer);
    pthread_mutex_unlock(&footer->mutex);
}

void footer_request_label(struct footer *footer, const char *key, const char *label)
{
    if (!footer)
        return;

    pthread_mutex_lock(&footer->mutex);
    if (spinner_label_request(&footer->label, key, label, monotonic_ms()) &&
        footer->mode != FOOTER_HIDDEN && !footer->suspended)
        draw_frame_locked(footer);
    pthread_mutex_unlock(&footer->mutex);
}

void footer_set_label_override(struct footer *footer, footer_label_override_fn override, void *user)
{
    if (!footer)
        return;

    pthread_mutex_lock(&footer->mutex);
    footer->label_override = override;
    footer->label_override_user = user;
    pthread_mutex_unlock(&footer->mutex);
}

void footer_set_timer(struct footer *footer, long started_at_ms)
{
    if (!footer)
        return;

    pthread_mutex_lock(&footer->mutex);
    footer->timer_started_at_ms = started_at_ms > 0 ? started_at_ms : 0;
    pthread_mutex_unlock(&footer->mutex);
}

static void hide_locked(struct footer *footer)
{
    footer->status_show_pending = 0;
    if (footer->mode == FOOTER_HIDDEN)
        return;

    if (!footer->suspended)
        erase_locked(footer);
    footer->mode = FOOTER_HIDDEN;
    footer->parked_rows = 0;
    free_rows_locked(footer);
}

void footer_hide(struct footer *footer)
{
    if (!footer)
        return;

    pthread_mutex_lock(&footer->mutex);
    hide_locked(footer);
    pthread_mutex_unlock(&footer->mutex);
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

static void *footer_thread(void *arg)
{
    struct footer *footer = arg;

    /* Keep process signals on threads that own the foreground operation and terminal state. */
    sigset_t mask;
    sigfillset(&mask);
    pthread_sigmask(SIG_SETMASK, &mask, NULL);

    pthread_mutex_lock(&footer->mutex);
    while (!footer->stop_requested) {
        if (footer->suspended) {
            pthread_cond_wait(&footer->wake, &footer->mutex);
            continue;
        }
        if (footer->mode == FOOTER_HIDDEN) {
            if (!footer->status_show_pending) {
                pthread_cond_wait(&footer->wake, &footer->mutex);
                continue;
            }
            long wait_ms = footer->status_show_at_ms - monotonic_ms();
            if (wait_ms <= 0) {
                show_locked(footer, FOOTER_STATUS, 0);
                continue;
            }
            struct timespec show_deadline = deadline_after_ms(wait_ms);
            pthread_cond_timedwait(&footer->wake, &footer->mutex, &show_deadline);
            continue;
        }

        spinner_label_settle(&footer->label, monotonic_ms());
        draw_frame_locked(footer);
        struct timespec deadline = deadline_after_ms(SPINNER_FRAME_INTERVAL_MS);
        pthread_cond_timedwait(&footer->wake, &footer->mutex, &deadline);
    }
    pthread_mutex_unlock(&footer->mutex);
    return NULL;
}

int footer_status_gap(const struct disp *disp)
{
    size_t newlines = disp->column > 0 ? 0 : disp->committed_newlines;
    return newlines >= 2 ? 0 : 2 - (int)newlines;
}

static void suspend_for_output(void *user)
{
    struct footer *footer = user;
    pthread_mutex_lock(&footer->mutex);
    if (footer->suspended) {
        pthread_mutex_unlock(&footer->mutex);
        return;
    }
    footer->suspended = 1;
    if (footer->mode != FOOTER_HIDDEN) {
        /* The erase and the repaint after the output land in one synchronized frame. */
        fputs(ANSI_SYNC_BEGIN, stdout);
        footer->suspended_synced = 1;
        erase_locked(footer);
    }
    pthread_mutex_unlock(&footer->mutex);
}

static int resume_after_output(void *user)
{
    struct footer *footer = user;
    pthread_mutex_lock(&footer->mutex);
    /* The footer stays away while output stops inside a sequence its bytes would corrupt, or where
     * disp has no column it can safely return to. Inside a sequence even the synchronized update
     * stays open, until a later resume; terminals bound how long they hold one. */
    int cols = term_width();
    int mid_sequence = disp_mid_sequence(footer->disp);
    if (mid_sequence || (cols > 0 && disp_screen_column(footer->disp, cols) == (size_t)cols)) {
        if (footer->suspended_synced && !mid_sequence) {
            fputs(ANSI_SYNC_END, stdout);
            footer->suspended_synced = 0;
        }
        fflush(stdout);
        pthread_mutex_unlock(&footer->mutex);
        return 0;
    }
    footer->suspended = 0;
    int gap =
        footer->mode == FOOTER_ROWS ? rows_gap(footer->disp) : footer_status_gap(footer->disp);
    if (footer->mode != FOOTER_HIDDEN) {
        park_locked(footer, gap);
        draw_frame_locked(footer);
    }
    if (footer->suspended_synced) {
        fputs(ANSI_SYNC_END, stdout);
        footer->suspended_synced = 0;
    }
    fflush(stdout);
    pthread_cond_signal(&footer->wake);
    pthread_mutex_unlock(&footer->mutex);
    return 1;
}

void footer_attach(struct footer *footer, struct disp *disp)
{
    if (!footer)
        return;
    footer->disp = disp;
    footer->live = (struct disp_live_area){
        .suspend = suspend_for_output,
        .resume = resume_after_output,
        .user = footer,
    };
    disp->live = &footer->live;
}

/* Test seam: e2e scenarios shorten the label's settle time, which would otherwise dominate their
 * run time. Env-only on purpose — not a user tunable. */
static long label_settle_ms(void)
{
    const char *value = getenv("HAX_LABEL_SETTLE_MS");
    return value ? strtol(value, NULL, 10) : SPINNER_LABEL_SETTLE_MS;
}

struct footer *footer_new(const char *label)
{
    if (!isatty(fileno(stdout)))
        return NULL;

    struct footer *footer = xcalloc(1, sizeof(*footer));
    spinner_label_init(&footer->label, label);
    footer->label.settle_ms = label_settle_ms();
    pthread_mutex_init(&footer->mutex, NULL);
    pthread_cond_init(&footer->wake, NULL);

    if (pthread_create(&footer->thread, NULL, footer_thread, footer) != 0) {
        pthread_mutex_destroy(&footer->mutex);
        pthread_cond_destroy(&footer->wake);
        spinner_label_free(&footer->label);
        free(footer);
        return NULL;
    }
    return footer;
}

void footer_free(struct footer *footer)
{
    if (!footer)
        return;

    if (footer->disp) {
        footer->disp->live = NULL;
        footer->disp->live_suspended = 0;
    }
    pthread_mutex_lock(&footer->mutex);
    hide_locked(footer);
    /* Never leave the terminal in synchronized mode, even on an abandoned suspension. */
    if (footer->suspended_synced) {
        fputs(ANSI_SYNC_END, stdout);
        footer->suspended_synced = 0;
    }
    footer->stop_requested = 1;
    pthread_cond_signal(&footer->wake);
    pthread_mutex_unlock(&footer->mutex);

    pthread_join(footer->thread, NULL);
    pthread_mutex_destroy(&footer->mutex);
    pthread_cond_destroy(&footer->wake);
    spinner_label_free(&footer->label);
    live_region_free(&footer->region);
    free(footer);
}
