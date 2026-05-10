/* SPDX-License-Identifier: MIT */
#ifndef HAX_MARKDOWN_H
#define HAX_MARKDOWN_H

#include <stddef.h>

/* Pragmatic Markdown renderer for streaming text — NOT a CommonMark
 * implementation. Supports `**bold**`, `*italic*`/`_italic_`, `` `code` ``,
 * line-start headers (#, ##, ###), code fences (``` with language label),
 * and `* ` list markers (preserved literally so they don't trigger italic).
 * Bold and inline-code are tracked as independent attributes, so `**`code`**`
 * (bold around inline code) renders correctly. Emphasis markers require a
 * non-alphanumeric byte on the left to open, so identifiers like
 * `compile_commands.json` and arithmetic like `5*3*7` stay literal.
 *
 * Output uses real italic for emphasis, cyan for inline code, dim for code
 * fences (with the language label on its own line in italic), and bold for
 * headings. Inline-code and code-fence spans are verbatim — no nested marker
 * processing inside them.
 *
 * Code-fence opener and closer follow CommonMark's count rule: a 4-backtick
 * fence isn't closed by a 3-backtick line, and a closer cannot have an info
 * string. Note that this means a model emitting a Markdown demo with a
 * 3-backtick outer fence containing a bare 3-backtick line will close at
 * that bare line — to nest, the source needs more outer backticks.
 *
 * The renderer is stateful across feed calls — it buffers a small tail of
 * ambiguous trailing bytes (a single `*` that may become `**`, etc.) so a
 * marker split across deltas resolves correctly on the next feed. Style
 * state spans deltas within a turn; call md_reset between turns. */

/* Emit callback. is_raw is 1 for ANSI escape bytes (zero-width, must not
 * affect any held-newline state in the consumer) and 0 for content bytes
 * that are visible on the terminal. The split lets a downstream like the
 * disp_* helpers route raw bytes around their trail/held bookkeeping so a
 * style transition after a buffered \n doesn't reset the trail counter. */
typedef void (*md_emit_fn)(const char *bytes, size_t n, int is_raw, void *user);

struct md_renderer;

/* wrap_width: cells per row before the renderer inserts a soft break at
 * the last word boundary. 0 disables wrapping (content passes through
 * verbatim, exactly as the model emitted it — used by tests for byte-
 * exact comparison and as a safety hatch). Code fences and headings are
 * always passed through verbatim regardless of wrap_width. The renderer
 * also collapses single \n inside paragraphs to a space (CommonMark soft
 * break) so a model emitting narrow lines is rewrapped to the available
 * width; \n followed by a block marker (list bullet, heading, fence
 * opener, blank line) stays a hard break. */
struct md_renderer *md_new(md_emit_fn emit, void *user, int wrap_width);
void md_feed(struct md_renderer *m, const char *s, size_t n);
void md_flush(struct md_renderer *m); /* commit pending tail at end of turn */
/* Re-sample wrap_width here; lets the agent pass display_width() between
 * turns so a SIGWINCH-style resize is picked up without a callback. */
void md_reset(struct md_renderer *m, int wrap_width);
void md_free(struct md_renderer *m);

#endif /* HAX_MARKDOWN_H */
