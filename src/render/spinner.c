/* SPDX-License-Identifier: MIT */
#include "render/spinner.h"

#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "xalloc.h"
#include "system/clock.h"
#include "terminal/ansi.h"
#include "text/fmt.h"
#include "text/width.h"

#define TIMER_MIN_MS 30000

#define DEFAULT_LABEL     "working..."
#define DEFAULT_LABEL_KEY "working"

static const char *const SPINNER_FRAMES[] = {
    "\xE2\xA0\x8B", "\xE2\xA0\x99", "\xE2\xA0\xB9", "\xE2\xA0\xB8", "\xE2\xA0\xBC",
    "\xE2\xA0\xB4", "\xE2\xA0\xA6", "\xE2\xA0\xA7", "\xE2\xA0\x87", "\xE2\xA0\x8F",
};
#define SPINNER_FRAME_COUNT (sizeof(SPINNER_FRAMES) / sizeof(SPINNER_FRAMES[0]))

const char *spinner_glyph_now(void)
{
    long now_ms = monotonic_ms();
    if (now_ms < 0)
        now_ms = 0;
    size_t frame = (size_t)(now_ms / SPINNER_FRAME_INTERVAL_MS) % SPINNER_FRAME_COUNT;
    return SPINNER_FRAMES[frame];
}

int spinner_format_status(struct buf *out, const char *glyph, const char *label, long elapsed_ms,
                          int max_cells)
{
    int label_budget = max_cells - 2; /* glyph and separating space */
    if (label_budget < 0)
        label_budget = 0;
    int cells = 2;

    buf_append_str(out, ANSI_DIM);
    buf_append_str(out, glyph);
    buf_append_str(out, " ");

    if (elapsed_ms >= TIMER_MIN_MS) {
        char duration[32];
        format_duration_steady(duration, sizeof(duration), elapsed_ms);
        int prefix_cells = (int)strlen(duration) + 3; /* spaces and middle dot */
        if (prefix_cells + (int)display_cells(label) <= label_budget) {
            buf_append_str(out, duration);
            buf_append_str(out, " \xC2\xB7 ");
            label_budget -= prefix_cells;
            cells += prefix_cells;
        }
    }

    char *fitted = truncate_for_display(label, (size_t)label_budget);
    buf_append_str(out, fitted);
    cells += (int)display_cells(fitted);
    free(fitted);
    buf_append_str(out, ANSI_RESET);
    return cells;
}

void spinner_label_init(struct spinner_label *label, const char *text)
{
    memset(label, 0, sizeof(*label));
    label->text = xstrdup(text && *text ? text : DEFAULT_LABEL);
    label->key = xstrdup(DEFAULT_LABEL_KEY);
    label->settle_ms = SPINNER_LABEL_SETTLE_MS;
}

static void clear_pending(struct spinner_label *label)
{
    free(label->pending_text);
    free(label->pending_key);
    label->pending_text = NULL;
    label->pending_key = NULL;
}

void spinner_label_free(struct spinner_label *label)
{
    clear_pending(label);
    free(label->text);
    free(label->key);
    label->text = NULL;
    label->key = NULL;
}

static int is_default_key(const char *key)
{
    return strcmp(key, DEFAULT_LABEL_KEY) == 0;
}

static int display(struct spinner_label *label, const char *key, const char *text)
{
    if (strcmp(label->key, key) != 0) {
        free(label->key);
        label->key = xstrdup(key);
    }
    if (strcmp(label->text, text) == 0)
        return 0;

    free(label->text);
    label->text = xstrdup(text);
    return 1;
}

int spinner_label_set(struct spinner_label *label, const char *key, const char *text)
{
    const char *set_key = key && *key ? key : DEFAULT_LABEL_KEY;
    clear_pending(label);
    label->held = !is_default_key(set_key);
    return display(label, set_key, text && *text ? text : DEFAULT_LABEL);
}

int spinner_label_request(struct spinner_label *label, const char *key, const char *text,
                          long now_ms)
{
    if (label->held)
        return 0;

    const char *requested_key = key && *key ? key : DEFAULT_LABEL_KEY;
    const char *requested_text = text && *text ? text : DEFAULT_LABEL;

    if (strcmp(label->key, requested_key) == 0) {
        clear_pending(label);
        return display(label, requested_key, requested_text);
    }

    int changed = display(label, DEFAULT_LABEL_KEY, DEFAULT_LABEL);
    if (is_default_key(requested_key)) {
        clear_pending(label);
    } else if (label->pending_key && strcmp(label->pending_key, requested_key) == 0) {
        if (strcmp(label->pending_text, requested_text) != 0) {
            free(label->pending_text);
            label->pending_text = xstrdup(requested_text);
        }
    } else {
        clear_pending(label);
        label->pending_text = xstrdup(requested_text);
        label->pending_key = xstrdup(requested_key);
        label->pending_since_ms = now_ms;
    }
    return changed;
}

void spinner_label_settle(struct spinner_label *label, long now_ms)
{
    if (!label->pending_key || now_ms - label->pending_since_ms < label->settle_ms)
        return;

    display(label, label->pending_key, label->pending_text);
    clear_pending(label);
}
