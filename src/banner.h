/* SPDX-License-Identifier: MIT */
#ifndef HAX_BANNER_H
#define HAX_BANNER_H

#include <stdio.h>

struct agent_session;
struct provider;

/* Gutter-prefixed chrome rows for the interactive frontend: the startup identity, its key
 * tips, and pager headers. Rows wrap at display_width() by breaking between segments, so
 * related tokens stay together; a segment wider than a whole row word-wraps. Output goes
 * straight to the stream, outside disp bookkeeping — callers writing into a disp-tracked
 * stream reconcile with disp_sync_external_line(). */

struct banner_writer {
    FILE *out;
    int columns;
    int col;
    int fresh; /* no segment on the current row yet */
    const char *style;
    const char *style_off;
};

/* Begin a banner block: emits the first row's gutter. */
void banner_open(struct banner_writer *w, FILE *out);

/* Append one segment wrapped in style_open/style_off, breaking the row rather than splitting
 * the segment. `separator` joins consecutive segments on one row and is dropped at a break. */
void banner_put(struct banner_writer *w, const char *separator, const char *style_open,
                const char *style_off, const char *text);

/* End the block: closes any open style and finishes the last row. */
void banner_close(struct banner_writer *w);

/* The identity rows: hax [preset] › provider · model · effort. When the identity overflows
 * its row, the break lands after the provider so model and effort stay together. */
void banner_identity(FILE *out, const struct provider *provider,
                     const struct agent_session *session);

/* Print the startup identity and key-tip rows to stdout; the caller supplies any leading gap. */
void banner_print(const struct provider *provider, const struct agent_session *session);

#endif /* HAX_BANNER_H */
