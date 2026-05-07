/* SPDX-License-Identifier: MIT */
#ifndef HAX_SPINNER_H
#define HAX_SPINNER_H

/* A single-line "Working..." indicator animated by a background thread. All
 * entry points are idempotent and safe to call when stdout is not a TTY (in
 * which case the spinner becomes a no-op and no thread is created). */

struct spinner;

struct spinner *spinner_new(const char *label);
void spinner_show(struct spinner *s);
/* Hide the spinner. Returns 1 if the cursor is currently at the end of
 * caller-written text (inline mode at hide time, or spinner disabled
 * because stdout isn't a TTY — in either case nothing was drawn past
 * what the caller emitted). Returns 0 if the cursor is at column 0 of
 * an erased row (line mode, or post auto-transition from inline to
 * line). The read + hide happens under the spinner's mutex so the
 * answer correctly reflects the state even when the thread auto-
 * transitions between caller decisions. */
int spinner_hide(struct spinner *s);
/* Swap the label shown beside the glyph. If the spinner is currently
 * visible the new label is repainted on the same line; otherwise the
 * change just takes effect on the next show. Idempotent — passing the
 * current label is a no-op. NULL/"" reverts to the default ("working..."). */
void spinner_set_label(struct spinner *s, const char *label);
/* Inline mode: glyph only (no label, no leading line-erase). The
 * caller has already written some text on the current line and wants
 * the spinner appended at the cursor; on hide the glyph is erased
 * with backspace+space+backspace and the cursor returns to where the
 * caller left it. Use only when the line is short enough not to wrap
 * (caller is responsible for keeping it within the terminal width).
 *
 * After a few seconds with no further spinner_show_inline call, the
 * thread auto-transitions to line mode: glyph erased in place, \n
 * emitted, full line spinner ("⠋ working..." / "⠋ thinking...") drawn
 * on the row below. Subsequent spinner_hide returns 0 in that case so
 * the caller knows cursor is at column 0 of a fresh row, not at end
 * of the prior text. */
void spinner_show_inline(struct spinner *s);

/* Toggle the line-mode glyph between plain dim (default) and dim cyan.
 * Cyan mode is used by tool_render's streaming-row spinner so its glyph
 * matches the cyan gutter strip running down the side of the dim block
 * — visually they read as one continuous element. The default plain dim
 * is what the agent's "working..." / "thinking..." / "running..."
 * statuses use; those have no gutter strip and shouldn't pick up the
 * tint. The flag affects only line mode; inline mode (silent-tool
 * cluster) is always plain dim. Idempotent; safe to call when the
 * spinner is hidden — the new style applies on the next draw. */
void spinner_set_cyan(struct spinner *s, int cyan);

void spinner_free(struct spinner *s);

#endif /* HAX_SPINNER_H */
