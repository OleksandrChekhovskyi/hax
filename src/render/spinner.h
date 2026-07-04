/* SPDX-License-Identifier: MIT */
#ifndef HAX_SPINNER_H
#define HAX_SPINNER_H

/* A single-line indicator animated by a background thread. All entry
 * points are idempotent and safe on non-TTY stdout (no thread, line
 * draws short-circuit; SPINNER_TOOL_STATUS still writes so test
 * capture sees the rendered row).
 *
 * One busy-indicator presentation: a dim "⠋ label" row of its own
 * (SPINNER_LINE), optionally *parked* below a still-open content line
 * via spinner_park. A parked show is a pure terminal excursion —
 * spinner_hide erases the row and returns the cursor exactly, so
 * caller-side layout bookkeeping is never disturbed. The only other
 * visual mode is SPINNER_TOOL_STATUS, the live tool-output preview
 * row (a content row with an animated gutter glyph, not a busy
 * indicator).
 *
 * Invariant: between a show and the matching hide, the caller must
 * not write content bytes to stdout — the spinner owns the cursor.
 * Label and timer updates are safe (they repaint the spinner's row).
 *
 * The animation frame index derives from CLOCK_MONOTONIC at draw
 * time, so it advances with wall time, not with repaint frequency. */

struct spinner;

enum spinner_mode {
    SPINNER_OFF = 0,     /* not visible */
    SPINNER_LINE,        /* "⠋ working..." labeled spinner row, either
                          * drawn in place (cursor at column 0 of a
                          * fresh row) or parked below a content line
                          * via spinner_park. */
    SPINNER_TOOL_STATUS, /* live tool-output status row: dim-cyan
                          * glyph + dim content, repainted at tick
                          * rate so animation keeps moving between
                          * content updates. */
};

struct spinner *spinner_new(const char *label);
void spinner_free(struct spinner *s);

/* SPINNER_LINE drawn in place: the cursor sits at column 0 of a fresh
 * row (the caller has already emitted its block separator) and the row
 * is painted right there. spinner_hide erases the row and leaves the
 * cursor at column 0 of it. */
void spinner_show(struct spinner *s);

/* SPINNER_LINE parked below the caller's current position, one blank
 * line of separation; spinner_hide returns the cursor exactly to
 * where the caller was. `col` is the cursor's current column: > 0 for
 * mid-line (an open coalescing tool header — restored via relative
 * cursor movement), 0 for column 0 of an empty row (the row itself is
 * the blank gap). The caller must guarantee its content line cannot
 * wrap (col strictly inside the terminal): the cursor-up restore
 * assumes single physical rows. */
void spinner_park(struct spinner *s, int col);

/* SPINNER_TOOL_STATUS — start showing a tool-output live status row:
 * `<dim-cyan glyph> <space> <dim content>`, repainted at tick rate.
 * `content` is copied. A parked line spinner is unwound first, so the
 * status row lands where the caller's next content row belongs. */
void spinner_show_tool_status(struct spinner *s, const char *content);

/* Update the SPINNER_TOOL_STATUS content. Deliberately does not paint
 * synchronously — the thread's next tick repaints, capping terminal
 * I/O at the tick rate however fast the tool streams. Idempotent. */
void spinner_set_tool_status_content(struct spinner *s, const char *content);

/* Hide the spinner (mode → SPINNER_OFF). Erases the spinner's row; a
 * parked show additionally returns the cursor to the caller's saved
 * position (see spinner_park), so the caller can resume writing as if
 * the spinner had never been shown. */
void spinner_hide(struct spinner *s);

/* Immediately swap the SPINNER_LINE label, discarding any pending
 * deferred request. Repaints in place when the line row is visible.
 * `key` is the same state identity spinner_request_label uses, so a
 * later request for the same state confirms in place instead of
 * re-settling. NULL/"" mean the "working" / "working..." defaults.
 * For ground-truth switches: render-state transitions, statuses that
 * must appear the moment they happen (retry countdown, compacting),
 * and ones that did their own settling (the table stall clock). */
void spinner_set_label(struct spinner *s, const char *key, const char *label);

/* Request a SPINNER_LINE label with settle-time hysteresis, keyed by
 * state. Invariant: the displayed label names a specific state only
 * while that state is re-affirmed or left unchallenged; it switches
 * after LABEL_SETTLE_MS of a stable successor, and demotes to plain
 * "working..." after that long of churn (contradicting requests where
 * no single challenger settles) — so transient states never flicker
 * in, and stale claims never overstay.
 *
 * `key` identifies the state independent of the display text:
 * re-requesting the same key keeps the settle clock running while
 * refreshing the text ("processing... 42%" settles once, then ticks
 * in place), and a request matching the *displayed* key updates
 * immediately and cancels any pending switch away. Silence arms
 * nothing — an unchallenged label (a long tool run with no events)
 * stays up. NULL/"" mean the "working" / "working..." defaults. */
void spinner_request_label(struct spinner *s, const char *key, const char *label);

/* Arm (start_ms > 0) or disarm (0) the elapsed-time counter on
 * SPINNER_LINE rows: once the wait passes TIMER_MIN_MS, frames draw
 * "⠋ 1m 08s · label". The agent arms it with the user turn's start
 * time so the counter matches the end-of-turn stats line. Tool-status
 * rows never draw it — they're owned by tool output. */
void spinner_set_timer(struct spinner *s, long start_ms);

/* Current spinner glyph derived from CLOCK_MONOTONIC, independent of
 * the spinner thread's animation state. Returns a const, NUL-
 * terminated UTF-8 string occupying one terminal cell. Useful when
 * something other than the spinner module needs to render a glyph
 * synced with the spinner's animation. */
const char *spinner_glyph_now(void);

#endif /* HAX_SPINNER_H */
