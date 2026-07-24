/* SPDX-License-Identifier: MIT */
#ifndef HAX_PICKER_H
#define HAX_PICKER_H

#include <stddef.h>

/*
 * Generic interactive "choose one from a list" selector.
 *
 * A raw-mode, redraw-in-place list with live type-to-filter — the shape
 * you want whenever the REPL needs the user to pick something from a set
 * that may be small (providers, reasoning levels) or large (an
 * OpenRouter model catalog). Built on the same escape-sequence decoder
 * the line editor uses (input_core_decode_escape), so arrow keys and the
 * usual control bindings behave consistently.
 *
 * Layout is an optional bold title, a dim search field (a magnifier glyph
 * with a "type to search" placeholder, gaining a matched/total count once
 * you type), then the scrolling list. The terminal cursor is hidden for the
 * duration — the search field is a labeled box, not a prompt — so the
 * highlighted row is the only focus indicator.
 *
 * Keys:
 *   - Up / Down (or Ctrl-P / Ctrl-N) move the selection
 *   - Home / End jump to first / last match
 *   - printable bytes extend the filter; Backspace shortens it; Ctrl-U clears
 *   - Enter selects the highlighted row
 *   - Esc / Ctrl-C / Ctrl-G cancel
 *
 * The filter is a case-insensitive, space-separated AND of substrings
 * matched against each item's `label` (see picker_core_match). On exit the
 * picker erases its own painted area, leaving the cursor at column 0 of
 * the line it started on so the caller can print a confirmation (or
 * nothing) with no leftover rows.
 *
 * Like the line editor, the picker needs an interactive terminal: it
 * returns -1 immediately when stdin or stdout isn't a tty (so a piped
 * invocation doesn't dump a menu into a file and block on input).
 */

struct picker_item {
    const char *label;  /* primary text; what the filter matches against */
    const char *detail; /* optional dim text shown after the label; may be NULL */
    /* When set, the whole row is rendered dim and `detail` gains a dash
     * separator — an advisory "exists but probably won't work right now"
     * (e.g. an unconfigured provider, with `detail` carrying the reason).
     * Purely visual: the row still matches the filter and is selectable,
     * so the caller decides what accepting it means (typically: try
     * anyway and report the exact failure). */
    int dim;
    /* When set, the row is tagged "✓ current" in the ok color after the
     * label — persistent state, distinct from the (accent, moving)
     * selection highlight. Callers mark the already-active choice with this rather
     * than smuggling "current" through `detail`, where it would render
     * dim and read like a failure reason. */
    int current;
    /* Optional description, wrapped below the list when the row is selected.
     * A newline is a hard break, so a desc can put structured fields on one
     * line and prose on the next rather than running them together; the
     * footer's height is capped either way, and the overflow is elided. */
    const char *desc;
};

struct picker_opts {
    const char *title; /* bold header line; may be NULL */
    const struct picker_item *items;
    size_t n;
    const char *empty_note; /* dim note shown (and -1 returned) when n == 0; may be NULL */
    /* Item index the cursor starts on (0 = first row). Callers pass the
     * "current" row so Enter-without-navigation re-picks what's active. */
    size_t initial;
};

/* Run the picker. Returns the selected 0-based index into `items`, or -1
 * on cancel, an empty list, or a non-tty stdin/stdout. */
long picker_run(const struct picker_opts *opts);

#endif /* HAX_PICKER_H */
