/* SPDX-License-Identifier: MIT */
#ifndef HAX_SPINNER_H
#define HAX_SPINNER_H

/* A single-line indicator animated by a background thread. All entry
 * points are idempotent and safe to call when stdout is not a TTY (in
 * which case the thread isn't created and ticking draws short-circuit;
 * synchronous content updates for SPINNER_TOOL_STATUS still produce
 * output so non-tty test capture sees the rendered row).
 *
 * The spinner can be in one of several visual modes — `enum
 * spinner_mode` is the single source of truth. The frame index is
 * derived from CLOCK_MONOTONIC at draw time, so multiple repaints
 * within the same animation frame show the same glyph and the
 * animation advances with wall time rather than with how often we
 * happen to redraw. */

struct spinner;

enum spinner_mode {
    SPINNER_OFF = 0,       /* not visible */
    SPINNER_LINE,          /* "⠋ working..." labeled spinner on its own row */
    SPINNER_INLINE_HEADER, /* glyph after a tool dispatch header (e.g.
                            * "[read] foo.c "); auto-transitions to
                            * SPINNER_LINE after INLINE_TIMEOUT_MS so a
                            * stalled tool surfaces the label. */
    SPINNER_INLINE_TEXT,   /* glyph after the model's last bytes during
                            * stream idle; never auto-transitions
                            * (injecting a \n would break paragraph
                            * layout the model is composing). */
    SPINNER_TOOL_STATUS,   /* live tool-output status row: dim-cyan
                            * glyph + dim content, repainted at tick
                            * rate so animation keeps moving between
                            * content updates. */
};

struct spinner *spinner_new(const char *label);
void spinner_free(struct spinner *s);

/* SPINNER_LINE — full labeled row. */
void spinner_show(struct spinner *s);

/* SPINNER_INLINE_HEADER — caller has just written a tool-dispatch
 * header (e.g. "[read] foo.c ") and wants the glyph appended at the
 * cursor. After INLINE_TIMEOUT_MS without further activity the thread
 * auto-transitions to SPINNER_LINE: the inline glyph is erased in
 * place, \n is emitted (directly to stdout, bypassing disp), and the
 * labeled spinner draws on the row below. spinner_hide returns 0 in
 * that case so the caller knows the cursor is at column 0 of a fresh
 * row.
 *
 * Invariant: between show and hide, the caller must NOT write to
 * stdout. The auto-transition can fire at any time and races with
 * concurrent caller writes — interleaved bytes would land mid-
 * transition. Use SPINNER_INLINE_TEXT (sticky) if the caller may need
 * to keep emitting around the spinner. */
void spinner_show_inline_header(struct spinner *s);

/* SPINNER_INLINE_TEXT — model stream went quiet mid-paragraph; glyph
 * sits at the cursor (end of model text). Never auto-transitions
 * because injecting a \n would break the paragraph layout the model
 * is composing. The sticky bit clears on hide so a subsequent
 * spinner_show_inline_header gets the default behavior.
 *
 * `pad`: when 1, draw a leading space cell before the glyph so it
 * doesn't appear glued to a non-whitespace character; both cells are
 * erased on hide and the cursor returns to the original position.
 * Pass 0 when the cursor is already at column 0 or right after a
 * space. */
void spinner_show_inline_text(struct spinner *s, int pad);

/* SPINNER_TOOL_STATUS — start showing a tool-output live status row:
 * `<dim-cyan glyph> <space> <dim content>` on a single row, repainted
 * at the spinner's tick rate so the glyph animates with wall time.
 * `content` is copied; safe to free after the call. Subsequent
 * content updates go through spinner_set_tool_status_content. */
void spinner_show_tool_status(struct spinner *s, const char *content);

/* Update the content shown in SPINNER_TOOL_STATUS mode. Stores the
 * new content under the mutex; the spinner thread's next tick
 * (~80ms on TTY) repaints with the new value. Does NOT paint
 * synchronously — for high-frequency content updates (post-cap
 * tool output, line-buffered streams) this caps terminal I/O at the
 * tick rate and keeps non-TTY captures from filling with repaint
 * sequences for content that the preview footer says was elided.
 * Idempotent — passing the current content is a no-op. */
void spinner_set_tool_status_content(struct spinner *s, const char *content);

/* Hide the spinner (mode → SPINNER_OFF). Returns 1 if the cursor was
 * left at the end of caller-written text — i.e., the last show was
 * an inline mode (HEADER without auto-transition, or TEXT). Returns 0
 * if the cursor is at column 0 of an erased row (LINE / TOOL_STATUS,
 * SPINNER_INLINE_HEADER after auto-transition, or any mode when the
 * spinner is disabled but the last show was non-inline). */
int spinner_hide(struct spinner *s);

/* Swap the SPINNER_LINE label. If the spinner is in line mode and
 * visible, the label is repainted on the same row; otherwise the
 * change just takes effect on the next show. NULL/"" reverts to the
 * default ("working..."). */
void spinner_set_label(struct spinner *s, const char *label);

/* Current spinner glyph derived from CLOCK_MONOTONIC, independent of
 * the spinner thread's animation state. Returns a const, NUL-
 * terminated UTF-8 string occupying one terminal cell. Useful when
 * something other than the spinner module needs to render a glyph
 * synced with the spinner's animation. */
const char *spinner_glyph_now(void);

#endif /* HAX_SPINNER_H */
