/* SPDX-License-Identifier: MIT */
#ifndef HAX_RENDER_TOOL_RENDER_H
#define HAX_RENDER_TOOL_RENDER_H

#include <stddef.h>

#include "buf.h"
#include "render/ctrl_strip.h"
#include "text/utf8_sanitize.h"

struct disp;
struct spinner;

#define TOOL_RENDER_GUTTER_COLS 2

/* Preview policy for sanitized tool output. */
enum tool_render_mode {
    TOOL_RENDER_DIFF,      /* uncapped, line-colored unified diff */
    TOOL_RENDER_HEAD,      /* capped head followed by an elision marker */
    TOOL_RENDER_HEAD_TAIL, /* capped head and tail with middle elision */
};

/* Streaming renderer for user-visible tool output. Feed accepts arbitrary chunk boundaries and
 * removes terminal controls, malformed UTF-8, and unsafe format codepoints. Head previews omit
 * blank lines; diff previews preserve them. The spinner is NULL on non-TTY stdout, which selects
 * plain non-live rendering: completed rows only, no repaints. */
struct tool_render {
    struct disp *disp;
    struct spinner *spinner;
    enum tool_render_mode mode;

    struct ctrl_strip strip;
    struct utf8_sanitizer sanitizer;
    int display_was_called;

    struct buf line;
    size_t line_tail_bytes;
    int line_has_non_whitespace;

    struct buf status_line;
    int status_visible;
    /* The live row is the empty placeholder, not committed content. */
    int status_placeholder;
    int block_open;

    int rows_emitted;
    int head_lines_emitted;
    size_t head_bytes_emitted;
    int head_complete;

    char *tail;
    size_t tail_write_pos;
    int tail_full;
    int suppressed_lines;
    size_t suppressed_tail_bytes;

    struct buf diff_line;
    int diff_hunk_started;
};

/* disp and spinner are borrowed and must outlive the renderer; spinner may be NULL. */
void tool_render_init(struct tool_render *render, struct disp *disp, struct spinner *spinner,
                      enum tool_render_mode mode);
/* Release internal storage without finalizing pending output. */
void tool_render_free(struct tool_render *render);

/* Change policy before feeding any bytes. */
void tool_render_set_mode(struct tool_render *render, enum tool_render_mode mode);

/* Show an empty live preview row — an animated gutter awaiting content — so a running tool is
 * visible before its first output line, which replaces the row in place. A block that never
 * produces output erases the row again. No-op without a spinner. */
void tool_render_begin_live(struct tool_render *render);

void tool_render_feed(struct tool_render *render, const char *bytes, size_t len);

/* Flush buffered input and close the preview. Repeated calls have no effect. */
void tool_render_finalize(struct tool_render *render);

/* Write the gutter prefix for standalone one-line tool output. */
void tool_render_write_marker_gutter(struct disp *disp);

/* Tool display callback; data must point to an initialized struct tool_render. */
void tool_render_emit(const char *bytes, size_t len, void *data);

#endif /* HAX_RENDER_TOOL_RENDER_H */
