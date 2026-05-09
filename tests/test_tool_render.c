/* SPDX-License-Identifier: MIT */
#include "tool_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ansi.h"
#include "disp.h"
#include "harness.h"

/* Same stdout-capture pattern as test_disp.c — see comments there. */

/* Gutter strip byte sequences — defined inline so tests can spell out
 * the exact expected output. Must match the helpers in disp.c. */
#define STRIP_FIRST      ANSI_DIM_CYAN "\xE2\x94\x8C " ANSI_RESET     /* ┌  */
#define STRIP_BODY       ANSI_DIM_CYAN "\xE2\x94\x82 " ANSI_RESET     /* │  */
#define STRIP_CLOSE      "\r" ANSI_DIM_CYAN "\xE2\x94\x94" ANSI_RESET /* └ */
#define STRIP_CLOSE_SOLO "\r" ANSI_DIM_CYAN "\xE2\x80\xBA" ANSI_RESET /* › */

static char captured[65536];

static void cap_init(void)
{
    char path[64];
    snprintf(path, sizeof(path), "/tmp/haxrender.%d.out", (int)getpid());
    if (!freopen(path, "w+", stdout)) {
        perror("freopen");
        exit(1);
    }
    unlink(path);
}

static void cap_reset(void)
{
    fflush(stdout);
    if (ftruncate(fileno(stdout), 0) != 0) {
        perror("ftruncate");
        exit(1);
    }
    rewind(stdout);
}

static const char *cap_read(void)
{
    fflush(stdout);
    fseek(stdout, 0, SEEK_SET);
    size_t n = fread(captured, 1, sizeof(captured) - 1, stdout);
    captured[n] = 0;
    return captured;
}

/* Run one feed+finalize cycle on a fresh renderer with a fresh disp.
 * Returns the captured stdout bytes (valid until the next cap_read). */
static const char *render_one(enum render_mode mode, const char *bytes, size_t n)
{
    cap_reset();
    struct disp d = {0};
    struct tool_render r;
    tool_render_init(&r, &d, NULL, mode);
    tool_render_feed(&r, bytes, n);
    tool_render_finalize(&r);
    tool_render_free(&r);
    return cap_read();
}

/* ---------- empty input ---------- */

static void test_empty_emits_nothing(void)
{
    const char *out = render_one(R_HEAD_ONLY, "", 0);
    /* No started byte → no dim wrapper, no footer, no newline. */
    EXPECT_STR_EQ(out, "");
}

/* ---------- under-cap output renders inline with dim wrapper ---------- */

static void test_head_only_under_cap(void)
{
    const char *in = "hello\nworld\n";
    const char *out = render_one(R_HEAD_ONLY, in, strlen(in));
    /* Each row gets a gutter strip; the trailing \n between rows
     * commits before the second strip's disp_write flushes held NLs.
     * The final row's \n stays held (finalize doesn't flush it), then
     * the close-glyph overprint targets the last row's body strip. */
    EXPECT_STR_EQ(out, STRIP_FIRST ANSI_DIM "hello\n" STRIP_BODY ANSI_DIM
                                            "world" ANSI_RESET STRIP_CLOSE);
}

static void test_head_only_no_trailing_nl_queues_separator(void)
{
    /* When output ends mid-line under the cap, finalize closes the row
     * (queueing a \n in disp) and overprints with the solo close glyph. */
    cap_reset();
    struct disp d = {0};
    struct tool_render r;
    tool_render_init(&r, &d, NULL, R_HEAD_ONLY);
    tool_render_feed(&r, "no newline", 10);
    tool_render_finalize(&r);
    tool_render_free(&r);
    EXPECT_STR_EQ(cap_read(), STRIP_FIRST ANSI_DIM "no newline" ANSI_RESET STRIP_CLOSE_SOLO);
    EXPECT(d.held == 1);
}

/* ---------- over-cap output gets footer ---------- */

static void test_head_only_exceeds_cap_emits_footer(void)
{
    /* Far over both 8-line and 3000-byte caps. Robust to small tweaks
     * in the constants — we don't pin the exact line count. */
    char in[8000];
    size_t n = 0;
    for (int i = 0; i < 200; i++)
        n += (size_t)snprintf(in + n, sizeof(in) - n, "line%03d\n", i);

    const char *out = render_one(R_HEAD_ONLY, in, n);

    /* Early lines made it through. */
    EXPECT(strstr(out, "line000") != NULL);
    EXPECT(strstr(out, "line001") != NULL);
    /* Late lines were suppressed. */
    EXPECT(strstr(out, "line199") == NULL);
    /* Footer reports the suppressed count. */
    EXPECT(strstr(out, "more line") != NULL);
    EXPECT(strstr(out, "more byte") != NULL);
    /* Multi-row block → close glyph overprints with └. */
    EXPECT(strstr(out, STRIP_CLOSE) != NULL);
}

/* ---------- head-tail elision ---------- */

static void test_head_tail_under_cap(void)
{
    const char *in = "a\nb\n";
    const char *out = render_one(R_HEAD_TAIL, in, strlen(in));
    /* Same shape as head-only when output fits — strips plus close. */
    EXPECT_STR_EQ(out, STRIP_FIRST ANSI_DIM "a\n" STRIP_BODY ANSI_DIM "b" ANSI_RESET STRIP_CLOSE);
}

static void test_head_tail_exceeds_cap_emits_marker_and_tail(void)
{
    /* Way over the head cap and over the tail line cap — guarantees
     * an elision marker plus the kept tail. */
    char in[8000];
    size_t n = 0;
    for (int i = 0; i < 200; i++)
        n += (size_t)snprintf(in + n, sizeof(in) - n, "row%03d\n", i);

    const char *out = render_one(R_HEAD_TAIL, in, n);

    EXPECT(strstr(out, "row000") != NULL);
    EXPECT(strstr(out, "row199") != NULL); /* tail kept */
    EXPECT(strstr(out, "more line") != NULL);
    EXPECT(strstr(out, " ...") != NULL); /* elision marker */
    /* Multi-row block → close glyph overprints with └. */
    EXPECT(strstr(out, STRIP_CLOSE) != NULL);
}

/* ---------- diff coloring ---------- */

static void test_diff_colors_added_and_removed(void)
{
    const char *in = "--- a/x\n"
                     "+++ b/x\n"
                     "@@ -1,1 +1,1 @@\n"
                     "-old\n"
                     "+new\n";
    const char *out = render_one(R_DIFF, in, strlen(in));

    /* All five line contents survive the renderer. */
    EXPECT(strstr(out, "--- a/x") != NULL);
    EXPECT(strstr(out, "+++ b/x") != NULL);
    EXPECT(strstr(out, "@@ -1,1 +1,1 @@") != NULL);
    EXPECT(strstr(out, "-old") != NULL);
    EXPECT(strstr(out, "+new") != NULL);
    /* Colors are applied somewhere in the stream — green for added, red
     * for removed, dim for headers. (We don't pin exact placement
     * because disp_raw lets escapes float ahead of buffered newlines —
     * see disp.h — and that's zero-width visually but breaks contiguous
     * substring matches.) */
    EXPECT(strstr(out, ANSI_GREEN) != NULL);
    EXPECT(strstr(out, ANSI_RED) != NULL);
    EXPECT(strstr(out, ANSI_DIM) != NULL);
}

static void test_diff_flushes_partial_trailing_line(void)
{
    /* No trailing newline — finalize must flush the buffered line. */
    const char *in = "+added";
    const char *out = render_one(R_DIFF, in, strlen(in));
    EXPECT(strstr(out, ANSI_GREEN "+added" ANSI_RESET) != NULL);
}

/* ---------- ctrl-byte stripping ---------- */

static void test_ctrl_bytes_dropped_before_render(void)
{
    /* Embedded ANSI sequence and bell byte should be filtered before
     * the strip + content lands. Single row, so close uses the solo
     * chevron. */
    const char in[] = "ab\x07\x1b[31mc\x1b[mdef\n";
    const char *out = render_one(R_HEAD_ONLY, in, sizeof(in) - 1);
    EXPECT_STR_EQ(out, STRIP_FIRST ANSI_DIM "abcdef" ANSI_RESET STRIP_CLOSE_SOLO);
}

/* ---------- exact-at-cap output ---------- */

static void test_head_only_exact_line_cap(void)
{
    /* Output is exactly DISP_HEAD_ONLY_LINES (8) lines, each terminated
     * by \n, and stops there. The line cap fires on the last \n; with
     * no further bytes to suppress, finalize must still close the
     * block on the cap row's strip — not on a blank row past it. */
    char in[256];
    size_t n = 0;
    for (int i = 0; i < 8; i++)
        n += (size_t)snprintf(in + n, sizeof(in) - n, "L%d\n", i);
    const char *out = render_one(R_HEAD_ONLY, in, n);

    /* All 8 lines made it through. */
    EXPECT(strstr(out, "L0\n") != NULL);
    EXPECT(strstr(out, "L7") != NULL);
    /* No "(N more)" footer — no bytes were suppressed. */
    EXPECT(strstr(out, "more line") == NULL);
    /* The crucial assertion: the last line's content is immediately
     * followed by ANSI_RESET + STRIP_CLOSE, with NO intervening \n.
     * If close_head_block had flushed the held \n eagerly (the bug),
     * the cap row's \n would be committed to the byte stream before
     * the close glyph, and "L7" would be followed by "\n" first. */
    EXPECT(strstr(out, "L7" ANSI_RESET STRIP_CLOSE) != NULL);
}

/* ---------- per-line truncation ---------- */

static void test_long_line_truncated_with_ellipsis(void)
{
    /* Build a single line that overflows display_width()'s 100-cell
     * cap (98 cells of content after subtracting the 2-cell strip).
     * The renderer should emit "..." mid-row and suppress the rest
     * until \n. We don't assert exact byte length — just that "..."
     * appears in the output and the post-truncation content does not. */
    char in[300];
    memset(in, 'A', 200);
    memcpy(in + 200, "TAIL_MARKER", 11);
    in[211] = '\n';
    const char *out = render_one(R_HEAD_ONLY, in, 212);
    EXPECT(strstr(out, "...") != NULL);
    /* The byte run after the ellipsis should be elided from the
     * preview row. */
    EXPECT(strstr(out, "TAIL_MARKER") == NULL);
    /* Single visible row → solo close glyph. */
    EXPECT(strstr(out, "\xE2\x80\xBA") != NULL); /* › */
}

static void test_long_line_after_short_line_uses_body_strip(void)
{
    /* Short line + long line: rows_emitted increments past the first
     * row, so the long line gets a body strip "│" rather than the
     * top-corner "┌". Per-line truncation still fires on the long row. */
    char in[300];
    memcpy(in, "short\n", 6);
    memset(in + 6, 'B', 200);
    in[206] = '\n';
    const char *out = render_one(R_HEAD_ONLY, in, 207);
    EXPECT(strstr(out, "\xE2\x94\x8C") != NULL); /* ┌ on first row */
    EXPECT(strstr(out, "\xE2\x94\x82") != NULL); /* │ on second row */
    EXPECT(strstr(out, "...") != NULL);          /* truncation marker */
    /* Multi-row block → close glyph overprints with └. */
    EXPECT(strstr(out, STRIP_CLOSE) != NULL);
}

/* ---------- over-indented content ---------- */

static void test_over_indented_content_renders_truncation_marker(void)
{
    /* Leading whitespace longer than the row's safe budget combined
     * with non-ws content used to drop both the ws and the content,
     * making the row vanish from the preview entirely. The renderer
     * must keep the row visible by emitting strip + bounded ws +
     * "..." marker. */
    char in[300];
    memset(in, ' ', 200);
    in[200] = 'x';
    in[201] = '\n';
    const char *out = render_one(R_HEAD_ONLY, in, 202);
    /* Truncation marker present → row was rendered, not elided. */
    EXPECT(strstr(out, "...") != NULL);
    /* Single visible row → solo close glyph. */
    EXPECT(strstr(out, STRIP_CLOSE_SOLO) != NULL);
}

static void test_whitespace_flood_buffer_bounded(void)
{
    /* A flood of leading whitespace without a newline must not grow
     * the deferred-ws buffer unbounded. Bound is the row's "safe"
     * budget (content_budget() - 3); on any reasonable terminal
     * width that's well under 1KB, regardless of input size. */
    cap_reset();
    struct disp d = {0};
    struct tool_render r;
    tool_render_init(&r, &d, NULL, R_HEAD_ONLY);
    char *buf = malloc(65536);
    memset(buf, ' ', 65536);
    tool_render_feed(&r, buf, 65536);
    EXPECT(r.row_ws.len < 1024);
    free(buf);
    tool_render_finalize(&r);
    tool_render_free(&r);
}

/* ---------- empty-line skipping ---------- */

static void test_empty_lines_skipped_between_content(void)
{
    /* Empty rows between content should disappear from the preview;
     * the visible output is "a" and "b" with one strip each, no blank
     * row in between. The model still gets the raw bytes — that's a
     * separate path, not exercised here. */
    const char *in = "a\n\n\nb\n";
    const char *out = render_one(R_HEAD_ONLY, in, strlen(in));
    EXPECT_STR_EQ(out, STRIP_FIRST ANSI_DIM "a\n" STRIP_BODY ANSI_DIM "b" ANSI_RESET STRIP_CLOSE);
}

static void test_whitespace_only_lines_skipped(void)
{
    /* Rows that contain only spaces or tabs must be elided too —
     * common in diff context, git log indented bodies, etc. The
     * surrounding visible content renders as if the ws-only rows
     * weren't there. */
    const char *in = "hello\n  \n\t\nworld\n";
    const char *out = render_one(R_HEAD_ONLY, in, strlen(in));
    EXPECT_STR_EQ(out, STRIP_FIRST ANSI_DIM "hello\n" STRIP_BODY ANSI_DIM
                                            "world" ANSI_RESET STRIP_CLOSE);
}

static void test_indented_content_preserved(void)
{
    /* Leading whitespace before non-ws content is signal — flush the
     * deferred ws on first non-ws codepoint so indentation survives. */
    const char *in = "    indented\n";
    const char *out = render_one(R_HEAD_ONLY, in, strlen(in));
    EXPECT_STR_EQ(out, STRIP_FIRST ANSI_DIM "    indented" ANSI_RESET STRIP_CLOSE_SOLO);
}

static void test_only_empty_lines_no_preview(void)
{
    /* Pure-blank input produces no preview block at all — started
     * stays 0, so finalize emits nothing (no dim wrapper, no close
     * glyph). */
    const char *in = "\n  \n\t\n";
    const char *out = render_one(R_HEAD_ONLY, in, strlen(in));
    EXPECT_STR_EQ(out, "");
}

static void test_head_tail_blank_only_tail_keeps_close_glyph_aligned(void)
{
    /* "a\nb\nc\nd\n\n" in R_HEAD_TAIL: 4 visible lines hit the head
     * cap on a \n boundary, then a single blank \n is suppressed.
     * close_head_block runs (cap landed on a newline) and a later
     * suppress_byte flushes the cap row's held \n via
     * head_close_emit_pending, putting the cursor on a new blank row
     * past d. With ws-only rows now elided in tail replay,
     * emit_tail_bytes would emit nothing — leaving no strip on that
     * row for the close-glyph overprint to land on. The placeholder
     * strip in render_finalize_capped restores the invariant. */
    const char *in = "a\nb\nc\nd\n\n";
    const char *out = render_one(R_HEAD_TAIL, in, strlen(in));
    /* All 4 visible lines made it through the head. */
    EXPECT(strstr(out, "a") != NULL);
    EXPECT(strstr(out, "d") != NULL);
    /* No "(N more lines)" footer — tail fits inline. */
    EXPECT(strstr(out, "more line") == NULL);
    /* The close glyph overprints a strip — specifically, the
     * placeholder strip emitted by render_finalize_capped. The byte
     * sequence STRIP_BODY + ANSI_DIM (row_strip_open's dim re-open)
     * + ANSI_RESET (finalize's dim close) + STRIP_CLOSE only appears
     * when the placeholder was emitted; without it the close glyph
     * would be preceded by ANSI_DIM ANSI_RESET (no STRIP_BODY),
     * landing on bare terrain. */
    EXPECT(strstr(out, STRIP_BODY ANSI_DIM ANSI_RESET STRIP_CLOSE) != NULL);
}

static void test_diff_preserves_empty_lines(void)
{
    /* Diff context can legitimately include blank or whitespace-only
     * lines (a hunk where the surrounding code is blank). R_DIFF
     * routes through emit_diff_line, which always emits a row, so
     * the ws-skip logic in row_emit_codepoint doesn't apply here. */
    const char *in = "+a\n"
                     "  \n"
                     "+b\n";
    const char *out = render_one(R_DIFF, in, strlen(in));
    /* Expect three strips: ┌ on the first row, │ on the two body rows.
     * STRIP_BODY appearing at least twice proves the middle ws-only
     * row was emitted. */
    EXPECT(strstr(out, "+a") != NULL);
    EXPECT(strstr(out, "+b") != NULL);
    const char *p = strstr(out, STRIP_BODY);
    EXPECT(p != NULL);
    EXPECT(strstr(p + 1, STRIP_BODY) != NULL);
}

/* ---------- emit callback wiring ---------- */

static void test_emit_callback_sets_flag(void)
{
    cap_reset();
    struct disp d = {0};
    struct tool_render r;
    tool_render_init(&r, &d, NULL, R_HEAD_ONLY);
    EXPECT(r.emit_called == 0);
    tool_render_emit("x", 1, &r);
    EXPECT(r.emit_called == 1);
    /* Even an empty feed should mark called. */
    struct tool_render r2;
    tool_render_init(&r2, &d, NULL, R_HEAD_ONLY);
    tool_render_emit("", 0, &r2);
    EXPECT(r2.emit_called == 1);
    tool_render_finalize(&r);
    tool_render_free(&r);
    tool_render_finalize(&r2);
    tool_render_free(&r2);
}

int main(void)
{
    cap_init();

    test_empty_emits_nothing();
    test_head_only_under_cap();
    test_head_only_no_trailing_nl_queues_separator();
    test_head_only_exceeds_cap_emits_footer();
    test_head_tail_under_cap();
    test_head_tail_exceeds_cap_emits_marker_and_tail();
    test_diff_colors_added_and_removed();
    test_diff_flushes_partial_trailing_line();
    test_ctrl_bytes_dropped_before_render();
    test_head_only_exact_line_cap();
    test_long_line_truncated_with_ellipsis();
    test_long_line_after_short_line_uses_body_strip();
    test_over_indented_content_renders_truncation_marker();
    test_whitespace_flood_buffer_bounded();
    test_empty_lines_skipped_between_content();
    test_whitespace_only_lines_skipped();
    test_indented_content_preserved();
    test_only_empty_lines_no_preview();
    test_head_tail_blank_only_tail_keeps_close_glyph_aligned();
    test_diff_preserves_empty_lines();
    test_emit_callback_sets_flag();

    T_REPORT();
}
