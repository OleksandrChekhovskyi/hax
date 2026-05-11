/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "harness.h"
#include "util.h"
#include "render/disp.h"
#include "render/spinner.h"
#include "render/tool_render.h"
#include "terminal/ansi.h"

/* The streaming pipeline now repaints the bottom-of-block status row
 * via \r + erase-line every time a new non-blank line lands, so the
 * captured stdout includes intermediate paints + spinner glyphs.
 * Tests focus on the *final* visible state — the last commit / footer
 * / elision marker / tail replay + close-glyph overprint — by looking
 * for marker substrings that only appear once the renderer has settled
 * on its final output.
 *
 * Out of scope for these unit tests: the spinner thread's tick-rate
 * repaints. stdout is freopen'd to a regular file so isatty()→0 and
 * spinner_new doesn't spawn its thread; only synchronous draws (from
 * spinner_show_tool_status / set_tool_status_content) appear in the
 * captured bytes. Anything about animation timing or main-thread vs
 * spinner-thread interleaving needs a tty + manual visual check. */

#define STRIP_FIRST      ANSI_DIM_CYAN "\xE2\x94\x8C " ANSI_RESET     /* ┌  */
#define STRIP_BODY       ANSI_DIM_CYAN "\xE2\x94\x82 " ANSI_RESET     /* │  */
#define STRIP_CLOSE      "\r" ANSI_DIM_CYAN "\xE2\x94\x94" ANSI_RESET /* └ */
#define STRIP_CLOSE_SOLO "\r" ANSI_DIM_CYAN "\xE2\x80\xBA" ANSI_RESET /* › */

/* All Braille spinner frames in src/render/spinner.c share the U+28xx prefix
 * "\xE2\xA0\x..", so this two-byte needle proves a status_paint
 * happened without pinning a specific frame (frame index is derived
 * from monotonic time at paint moment). */
#define SPINNER_BRAILLE_PREFIX "\xE2\xA0"

static char captured[131072];

static void cap_init(void)
{
    /* mbrtowc() in tool_render's codepoint walk needs a UTF-8 LC_CTYPE
     * to decode multi-byte input as a single codepoint; otherwise
     * each byte gets substituted individually. Use the same fallback
     * chain production does (env locale → C.UTF-8 → en_US.UTF-8) so
     * tests work on minimal containers without en_US.UTF-8. */
    locale_init_utf8();
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

static const char *render_one(enum render_mode mode, const char *bytes, size_t n)
{
    cap_reset();
    struct disp d = {0};
    struct spinner *sp = spinner_new(NULL);
    struct tool_render r;
    tool_render_init(&r, &d, sp, mode);
    tool_render_feed(&r, bytes, n);
    tool_render_finalize(&r);
    tool_render_free(&r);
    spinner_free(sp);
    return cap_read();
}

/* ---------- empty / blank inputs ---------- */

static void test_empty_emits_nothing(void)
{
    const char *out = render_one(R_HEAD_ONLY, "", 0);
    EXPECT_STR_EQ(out, "");
}

static void test_only_blank_lines_no_preview(void)
{
    /* Pure-blank input (incl. ws-only) never sets started — finalize
     * emits nothing, no strip, no close glyph. */
    const char *in = "\n  \n\t\n";
    const char *out = render_one(R_HEAD_ONLY, in, strlen(in));
    EXPECT_STR_EQ(out, "");
}

/* ---------- under-cap rendering ---------- */

static void test_head_only_under_cap_shows_all_lines(void)
{
    /* Two non-blank lines under the cap render as two committed rows
     * (┌ + │) with their content; close glyph overprints with └. */
    const char *in = "hello\nworld\n";
    const char *out = render_one(R_HEAD_ONLY, in, strlen(in));
    EXPECT(strstr(out, "hello") != NULL);
    EXPECT(strstr(out, "world") != NULL);
    EXPECT(strstr(out, STRIP_FIRST) != NULL);
    EXPECT(strstr(out, STRIP_CLOSE) != NULL);
    /* The final commit lays down "│ world" + held \n right before
     * the close-glyph overprint; that contiguous pattern only exists
     * once finalize ran cleanly. */
    EXPECT(strstr(out, STRIP_BODY ANSI_DIM "world" ANSI_RESET STRIP_CLOSE) != NULL);
}

static void test_single_line_uses_solo_close(void)
{
    /* One visible row → close glyph promotes "┌" to "›" via overprint. */
    const char *in = "only\n";
    const char *out = render_one(R_HEAD_ONLY, in, strlen(in));
    EXPECT(strstr(out, "only") != NULL);
    EXPECT(strstr(out, STRIP_FIRST ANSI_DIM "only" ANSI_RESET STRIP_CLOSE_SOLO) != NULL);
}

static void test_partial_trailing_line_committed(void)
{
    /* Output ends mid-line — finalize processes the buffered partial
     * line, status_commits it as the final row, and close-glyphs solo. */
    cap_reset();
    struct disp d = {0};
    struct spinner *sp = spinner_new(NULL);
    struct tool_render r;
    tool_render_init(&r, &d, sp, R_HEAD_ONLY);
    tool_render_feed(&r, "no newline", 10);
    tool_render_finalize(&r);
    tool_render_free(&r);
    spinner_free(sp);
    const char *out = cap_read();
    EXPECT(strstr(out, "no newline") != NULL);
    EXPECT(strstr(out, STRIP_FIRST ANSI_DIM "no newline" ANSI_RESET STRIP_CLOSE_SOLO) != NULL);
    /* status_commit's held \n stays held — caller's block separator
     * collapses or commits it later. */
    EXPECT(d.held == 1);
}

/* ---------- status row / spinner glyph ---------- */

static void test_status_paints_with_spinner_glyph(void)
{
    /* During streaming (before finalize), the status row carries a
     * Braille spinner glyph as its gutter prefix. The paint happens
     * on every non-blank \n. */
    cap_reset();
    struct disp d = {0};
    struct spinner *sp = spinner_new(NULL);
    struct tool_render r;
    tool_render_init(&r, &d, sp, R_HEAD_ONLY);
    tool_render_feed(&r, "live\n", 5);
    /* Captured bytes so far must include a Braille spinner cell + the
     * line content. */
    const char *mid = cap_read();
    EXPECT(strstr(mid, SPINNER_BRAILLE_PREFIX) != NULL);
    EXPECT(strstr(mid, "live") != NULL);
    /* Status hasn't been committed to a permanent row yet — no \n
     * after "live" has been flushed (the held \n is buffered in disp). */
    EXPECT(d.held == 0);
    tool_render_finalize(&r);
    tool_render_free(&r);
    spinner_free(sp);
}

static void test_streaming_repaints_then_commits_prior(void)
{
    /* Two consecutive non-blank lines: the first is painted as status,
     * then committed when the second arrives. After streaming both
     * lines: status shows "two", and the captured bytes contain at
     * least one ┌-strip (the commit of "one"). */
    cap_reset();
    struct disp d = {0};
    struct spinner *sp = spinner_new(NULL);
    struct tool_render r;
    tool_render_init(&r, &d, sp, R_HEAD_ONLY);
    tool_render_feed(&r, "one\n", 4);
    tool_render_feed(&r, "two\n", 4);
    const char *mid = cap_read();
    EXPECT(strstr(mid, "one") != NULL);
    EXPECT(strstr(mid, "two") != NULL);
    /* Commit of "one" means STRIP_FIRST + DIM + "one" appears in the
     * stream. */
    EXPECT(strstr(mid, STRIP_FIRST ANSI_DIM "one") != NULL);
    tool_render_finalize(&r);
    tool_render_free(&r);
    spinner_free(sp);
}

/* ---------- empty / whitespace-only line elision ---------- */

static void test_blank_lines_elided_between_content(void)
{
    /* Empty rows between content disappear from the preview entirely;
     * the visible output is "a" then "b" with one strip each. */
    const char *in = "a\n\n\nb\n";
    const char *out = render_one(R_HEAD_ONLY, in, strlen(in));
    EXPECT(strstr(out, STRIP_FIRST ANSI_DIM "a") != NULL);
    EXPECT(strstr(out, STRIP_BODY ANSI_DIM "b" ANSI_RESET STRIP_CLOSE) != NULL);
}

static void test_whitespace_only_lines_elided(void)
{
    /* Same elision policy applies to spaces / tabs. */
    const char *in = "hello\n  \n\t\nworld\n";
    const char *out = render_one(R_HEAD_ONLY, in, strlen(in));
    EXPECT(strstr(out, STRIP_FIRST ANSI_DIM "hello") != NULL);
    EXPECT(strstr(out, STRIP_BODY ANSI_DIM "world" ANSI_RESET STRIP_CLOSE) != NULL);
}

static void test_indented_content_preserved(void)
{
    /* Leading whitespace before non-ws content is signal — preserved
     * in the committed row's content. */
    const char *in = "    indented\n";
    const char *out = render_one(R_HEAD_ONLY, in, strlen(in));
    EXPECT(strstr(out, STRIP_FIRST ANSI_DIM "    indented" ANSI_RESET STRIP_CLOSE_SOLO) != NULL);
}

static void test_tab_expanded_to_four_spaces(void)
{
    /* Raw \t reaching the terminal expands to the next column-multiple-
     * of-8 tab stop, but wcwidth('\t') == -1 made cell math count it as
     * one cell — the mismatch wrapped content out of the gutter. Tabs
     * are substituted with 4 spaces before any cell math runs; assert
     * no raw tab survives and the expanded indent is visible. */
    const char *in = "\t\thello\n";
    const char *out = render_one(R_HEAD_ONLY, in, strlen(in));
    EXPECT(strchr(out, '\t') == NULL);
    EXPECT(strstr(out, "        hello") != NULL);
}

/* ---------- head-cap suppression ---------- */

static void test_head_only_exceeds_cap_emits_footer(void)
{
    /* Far over both 8-line and 3000-byte caps. */
    char in[8000];
    size_t n = 0;
    for (int i = 0; i < 200; i++)
        n += (size_t)snprintf(in + n, sizeof(in) - n, "line%03d\n", i);

    const char *out = render_one(R_HEAD_ONLY, in, n);

    /* Early lines made it through. */
    EXPECT(strstr(out, "line000") != NULL);
    EXPECT(strstr(out, "line001") != NULL);
    /* Footer reports the suppressed count (lines only — bytes are
     * intentionally not in the marker text since the preview shrinks
     * inputs through line elision, row truncation, and dangerous-
     * codepoint substitution, so the byte number would be misleading). */
    EXPECT(strstr(out, "more line") != NULL);
    /* Multi-row block → close glyph overprints with └. */
    EXPECT(strstr(out, STRIP_CLOSE) != NULL);
}

static void test_head_tail_under_cap_shows_all_lines(void)
{
    const char *in = "a\nb\n";
    const char *out = render_one(R_HEAD_TAIL, in, strlen(in));
    EXPECT(strstr(out, STRIP_FIRST ANSI_DIM "a") != NULL);
    EXPECT(strstr(out, STRIP_BODY ANSI_DIM "b" ANSI_RESET STRIP_CLOSE) != NULL);
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

    EXPECT(strstr(out, "row000") != NULL); /* head kept */
    EXPECT(strstr(out, "row199") != NULL); /* tail kept */
    EXPECT(strstr(out, "more line") != NULL);
    EXPECT(strstr(out, " ...") != NULL); /* elision marker has trailing " ..." */
    EXPECT(strstr(out, STRIP_CLOSE) != NULL);
}

static void test_head_tail_modest_overflow_replays_inline(void)
{
    /* 5 short lines with HT head cap = 4 → 1 line gets suppressed but
     * fits in the tail (≤ DISP_HT_TAIL_LINES = 4). Finalize takes the
     * tail-fits-inline branch: drop the status, replay tail rows in
     * place. The result should show all 5 lines without an elision
     * marker. */
    const char *in = "a\nb\nc\nd\ne\n";
    const char *out = render_one(R_HEAD_TAIL, in, strlen(in));
    EXPECT(strstr(out, "a") != NULL);
    EXPECT(strstr(out, "b") != NULL);
    EXPECT(strstr(out, "c") != NULL);
    EXPECT(strstr(out, "d") != NULL);
    EXPECT(strstr(out, "e") != NULL);
    /* No elision marker — all content visible. */
    EXPECT(strstr(out, "more line") == NULL);
    EXPECT(strstr(out, STRIP_CLOSE) != NULL);
    /* Final committed row should be "e" + close glyph overprint. */
    EXPECT(strstr(out, STRIP_BODY ANSI_DIM "e" ANSI_RESET STRIP_CLOSE) != NULL);
}

/* ---------- per-line truncation ---------- */

static void test_long_line_truncated_with_ellipsis(void)
{
    /* Single line that overflows display_width(). The renderer
     * truncates with "..." and the post-truncation bytes are dropped. */
    char in[300];
    memset(in, 'A', 200);
    memcpy(in + 200, "TAIL_MARKER", 11);
    in[211] = '\n';
    const char *out = render_one(R_HEAD_ONLY, in, 212);
    EXPECT(strstr(out, "...") != NULL);
    EXPECT(strstr(out, "TAIL_MARKER") == NULL);
    EXPECT(strstr(out, STRIP_CLOSE_SOLO) != NULL);
}

static void test_over_indented_content_renders_truncation_marker(void)
{
    /* Leading whitespace longer than the row's safe budget combined
     * with non-ws content: status_content gets the truncated form
     * (truncate_for_display caps at content_budget()), so the row is
     * still visible — empty rows would have been elided. */
    char in[300];
    memset(in, ' ', 200);
    in[200] = 'x';
    in[201] = '\n';
    const char *out = render_one(R_HEAD_ONLY, in, 202);
    EXPECT(strstr(out, "...") != NULL);
    EXPECT(strstr(out, STRIP_CLOSE_SOLO) != NULL);
}

static void test_long_unbroken_input_buffer_bounded(void)
{
    /* A flood of bytes without any \n must not grow the line buffer
     * unbounded. The buffer caps at LINE_BUF_CAP (4096); the
     * line_total_bytes counter still tracks the true byte count for
     * accurate suppression accounting. */
    cap_reset();
    struct disp d = {0};
    struct spinner *sp = spinner_new(NULL);
    struct tool_render r;
    tool_render_init(&r, &d, sp, R_HEAD_ONLY);
    char *buf = malloc(65536);
    memset(buf, 'X', 65536);
    tool_render_feed(&r, buf, 65536);
    /* Line buffer is bounded at 4096 even though we fed 65536 bytes. */
    EXPECT(r.line.len <= 4096);
    /* True byte count tracked separately. */
    EXPECT(r.line_total_bytes == 65536);
    free(buf);
    tool_render_finalize(&r);
    tool_render_free(&r);
    spinner_free(sp);
}

/* ---------- diff coloring (unchanged path) ---------- */

static void test_diff_colors_added_and_removed(void)
{
    const char *in = "--- a/x\n"
                     "+++ b/x\n"
                     "@@ -1,1 +1,1 @@\n"
                     "-old\n"
                     "+new\n";
    const char *out = render_one(R_DIFF, in, strlen(in));

    EXPECT(strstr(out, "--- a/x") != NULL);
    EXPECT(strstr(out, "+++ b/x") != NULL);
    EXPECT(strstr(out, "@@ -1,1 +1,1 @@") != NULL);
    EXPECT(strstr(out, "-old") != NULL);
    EXPECT(strstr(out, "+new") != NULL);
    EXPECT(strstr(out, ANSI_GREEN) != NULL);
    EXPECT(strstr(out, ANSI_RED) != NULL);
    EXPECT(strstr(out, ANSI_DIM) != NULL);
}

static void test_diff_tab_expanded_to_four_spaces(void)
{
    /* Same substitution rule applies to the diff path — a model
     * emitting tab-indented source in a unified diff must not blow
     * past content_budget. */
    const char *in = "+\thello\n";
    const char *out = render_one(R_DIFF, in, strlen(in));
    EXPECT(strchr(out, '\t') == NULL);
    EXPECT(strstr(out, "+    hello") != NULL);
}

static void test_diff_flushes_partial_trailing_line(void)
{
    /* No trailing newline — finalize must flush the buffered line. */
    const char *in = "+added";
    const char *out = render_one(R_DIFF, in, strlen(in));
    EXPECT(strstr(out, ANSI_GREEN "+added" ANSI_RESET) != NULL);
}

static void test_diff_preserves_blank_lines(void)
{
    /* Diff context can include blank or whitespace-only lines (a hunk
     * where surrounding code is blank). R_DIFF emits every line as
     * its own row, including empty ones — distinct from the live
     * preview's elision policy. */
    const char *in = "+a\n"
                     "  \n"
                     "+b\n";
    const char *out = render_one(R_DIFF, in, strlen(in));
    EXPECT(strstr(out, "+a") != NULL);
    EXPECT(strstr(out, "+b") != NULL);
    /* Three rows means three strips: ┌ + 2× │. */
    const char *p = strstr(out, STRIP_BODY);
    EXPECT(p != NULL);
    EXPECT(strstr(p + 1, STRIP_BODY) != NULL);
}

/* ---------- ctrl-byte stripping (live path) ---------- */

static void test_ctrl_bytes_dropped_before_render(void)
{
    /* Embedded ANSI sequence and bell byte filtered before status
     * paint / commit. Single visible row → solo close glyph. */
    const char in[] = "ab\x07\x1b[31mc\x1b[mdef\n";
    const char *out = render_one(R_HEAD_ONLY, in, sizeof(in) - 1);
    EXPECT(strstr(out, STRIP_FIRST ANSI_DIM "abcdef" ANSI_RESET STRIP_CLOSE_SOLO) != NULL);
}

/* ---------- emit callback wiring ---------- */

static void test_emit_callback_sets_flag(void)
{
    cap_reset();
    struct disp d = {0};
    struct spinner *sp = spinner_new(NULL);
    struct tool_render r;
    tool_render_init(&r, &d, sp, R_HEAD_ONLY);
    EXPECT(r.emit_called == 0);
    tool_render_emit("x", 1, &r);
    EXPECT(r.emit_called == 1);
    /* Even an empty feed should mark called. */
    struct tool_render r2;
    tool_render_init(&r2, &d, sp, R_HEAD_ONLY);
    tool_render_emit("", 0, &r2);
    EXPECT(r2.emit_called == 1);
    tool_render_finalize(&r);
    tool_render_free(&r);
    tool_render_finalize(&r2);
    tool_render_free(&r2);
    spinner_free(sp);
}

static void test_finalize_is_idempotent(void)
{
    /* Calling tool_render_finalize twice must not re-emit the close
     * glyph — the second call has to short-circuit at the !started
     * check. Otherwise an inadvertent double-finalize would overprint
     * whatever's now under the cursor with another └/›. */
    cap_reset();
    struct disp d = {0};
    struct spinner *sp = spinner_new(NULL);
    struct tool_render r;
    tool_render_init(&r, &d, sp, R_HEAD_ONLY);
    tool_render_feed(&r, "x\n", 2);
    tool_render_finalize(&r);
    size_t after_first = strlen(cap_read());
    tool_render_finalize(&r);
    size_t after_second = strlen(cap_read());
    EXPECT(after_first == after_second);
    tool_render_free(&r);
    spinner_free(sp);
}

/* ---------- cap accounting / dangerous-codepoint substitution ---------- */

static void test_head_tail_blank_only_suppression_no_phantom_marker(void)
{
    /* HEAD_TAIL with input that exactly fills the head cap and has
     * a single trailing blank line: the blank transitions head_full
     * via commit-on-blank but contributes zero renderable content.
     * No "(N more lines)" marker should appear — there's nothing
     * meaningful to report. */
    const char *in = "a\nb\nc\nd\n\n";
    const char *out = render_one(R_HEAD_TAIL, in, strlen(in));
    /* All 4 head lines visible. */
    EXPECT(strstr(out, "a") != NULL);
    EXPECT(strstr(out, "d") != NULL);
    /* No marker, no phantom "(0 more bytes)" or "(1 more line, 0 …)"
     * wording. */
    EXPECT(strstr(out, "more line") == NULL);
    EXPECT(strstr(out, "more byte") == NULL);
}

static void test_blank_lines_after_cap_dont_emit_phantom_footer(void)
{
    /* HEAD_ONLY cap is 8 visible lines. Feed exactly 8 non-blank
     * lines followed by 5 blanks. The blanks transition head_full
     * (so subsequent non-blank lines would land in suppression) but
     * do NOT contribute to the footer's line count, since blanks are
     * silently elided everywhere — claiming "5 more lines elided" for
     * content the user wasn't going to see anyway is misleading. */
    char in[256];
    size_t n = 0;
    for (int i = 0; i < 8; i++)
        n += (size_t)snprintf(in + n, sizeof(in) - n, "L%d\n", i);
    /* 5 blank lines past the cap. */
    for (int i = 0; i < 5; i++)
        in[n++] = '\n';

    const char *out = render_one(R_HEAD_ONLY, in, n);

    /* All 8 lines visible. */
    EXPECT(strstr(out, "L0") != NULL);
    EXPECT(strstr(out, "L7") != NULL);
    /* No footer: blank lines are silently elided everywhere — head,
     * tail, suppression counters — so a run of trailing blanks past
     * the cap doesn't fire a "(N more lines)" marker. The input still
     * needs to TRANSITION head_full (so a non-blank line arriving
     * after the blanks would correctly land in suppression), but the
     * marker text only mentions renderable content. */
    EXPECT(strstr(out, "more line") == NULL);
}

static void test_head_tail_renders_long_suppressed_line(void)
{
    /* HEAD_TAIL must render a suppressed line that's longer than
     * LINE_BUF_CAP. The line buffer caps at 4096 bytes, but
     * tail_push_byte feeds the ring inline (uncapped), so a 5KB line
     * still surfaces in the visible tail. Tail rows use the same
     * right-truncation as head rows and live streaming, so we verify
     * the line START reaches the rendered row by placing a marker
     * there. */
    char *in = malloc(8192);
    size_t n = 0;
    /* 4 head lines fill the cap. */
    for (int i = 0; i < 4; i++)
        n += (size_t)snprintf(in + n, 8192 - n, "head%d\n", i);
    /* One enormous suppressed line: HEAD_MARKER + 5000 'A's + \n. */
    memcpy(in + n, "HEAD_MARKER", 11);
    n += 11;
    memset(in + n, 'A', 5000);
    n += 5000;
    in[n++] = '\n';
    const char *out = render_one(R_HEAD_TAIL, in, n);
    /* Marker at the line start survives row truncation (which keeps
     * the head and drops the tail). */
    EXPECT(strstr(out, "HEAD_MARKER") != NULL);
    free(in);
}

static void test_head_tail_blanks_in_tail_dont_eat_visible_count(void)
{
    /* When the suppressed input ends with blank lines mixed into
     * the trailing region (as `git show` does between the commit
     * body and a tool-appended footer like "[output truncated]"),
     * the back-walk must reach further back so DISP_HT_TAIL_LINES
     * non-blank lines remain visible. Counting raw \n boundaries
     * would let the blank consume one of the four slots, leaving
     * only 3 rendered tail rows. */
    char *in = malloc(8192);
    size_t n = 0;
    /* 4 head lines fill the head cap (HEAD_TAIL = 4). */
    for (int i = 0; i < 4; i++)
        n += (size_t)snprintf(in + n, 8192 - n, "head%d\n", i);
    /* Many suppressed middle lines. */
    for (int i = 0; i < 100; i++)
        n += (size_t)snprintf(in + n, 8192 - n, "mid%d\n", i);
    /* Tail region: T_REPORT();\n}\n\n[output truncated]\n
     * (a blank line between "}" and "[output truncated]"). */
    n += (size_t)snprintf(in + n, 8192 - n, "T_REPORT();\n}\n\n[output truncated]\n");
    const char *out = render_one(R_HEAD_TAIL, in, n);
    /* Without the back-walk-for-non-blank fix, the blank between
     * "}" and "[output truncated]" consumes a tail slot, so the
     * row immediately preceding "T_REPORT();" (e.g., "mid99") is
     * missing from the rendered output. With the fix, all 4 of
     * mid99 / T_REPORT() / } / [output truncated] are visible. */
    EXPECT(strstr(out, "mid99") != NULL);
    EXPECT(strstr(out, "T_REPORT();") != NULL);
    EXPECT(strstr(out, "}") != NULL);
    EXPECT(strstr(out, "[output truncated]") != NULL);
    free(in);
}

static void test_post_cap_lines_dont_flood_non_tty_output(void)
{
    /* Post-cap content updates must NOT trigger immediate paints —
     * non-TTY captures (CI logs, redirected output, tests) would
     * otherwise see every "elided" line land in the byte stream
     * despite the footer/marker saying it was suppressed. The
     * spinner thread handles live repaints on TTY at its tick rate
     * (~12 fps); non-TTY has no thread, so post-cap updates stay
     * silent. We verify the byte stream by counting how many lines
     * past the cap actually appear: only the cap-tripper's show
     * paint should leak (one paint, not 200). */
    char in[8000];
    size_t n = 0;
    /* HEAD_TAIL cap=4. Use 200 distinctly-numbered lines so we can
     * count which late lines appear. */
    for (int i = 0; i < 200; i++)
        n += (size_t)snprintf(in + n, sizeof(in) - n, "row%03d\n", i);
    const char *out = render_one(R_HEAD_TAIL, in, n);

    /* row000..row003 are committed head rows (visible). row004 is
     * the cap-tripper, painted via spinner_show_tool_status (one
     * paint visible). row005..row195 should NOT appear in mid output
     * (their content updates went silent post-cap). row196..row199
     * appear via finalize tail-row replay (kept tail). */
    int late_visible = 0;
    char buf[16];
    for (int i = 5; i < 196; i++) {
        snprintf(buf, sizeof(buf), "row%03d", i);
        if (strstr(out, buf))
            late_visible++;
    }
    /* The cap-tripper (row004) is allowed; middle suppressed lines
     * are not. Without the silent-set-content fix, all 191 of these
     * land in the captured stream as repaint sequences. */
    EXPECT(late_visible == 0);
    /* Sanity: head + tail rows still visible. */
    EXPECT(strstr(out, "row000") != NULL);
    EXPECT(strstr(out, "row199") != NULL);
}

static void test_head_tail_no_orphan_continuation_byte_in_replay(void)
{
    /* A long suppressed line of multi-byte codepoints (combining
     * acute U+0301 = 0xCC 0x81) wraps the byte-based tail ring at a
     * codepoint boundary — the slice starts in the middle of a
     * codepoint, with a bare 0x81 continuation byte at the front.
     * Without trimming, that orphan byte reaches the terminal and
     * renders as garbage. After the fix, every 0x81 in the captured
     * output is preceded by its 0xCC lead byte. */
    char *in = malloc(8192);
    size_t n = 0;
    for (int i = 0; i < 4; i++)
        n += (size_t)snprintf(in + n, 8192 - n, "head%d\n", i);
    in[n++] = 'a';
    /* 1000 × U+0301 = 2000 bytes, well past the 1500-byte ring. */
    for (int i = 0; i < 1000; i++) {
        in[n++] = '\xCC';
        in[n++] = '\x81';
    }
    in[n++] = '\n';
    const char *out = render_one(R_HEAD_TAIL, in, n);
    /* Walk the captured bytes — every continuation byte (0x80–0xBF)
     * must have a UTF-8 lead byte (0xC0–0xFF) immediately before it.
     * Specifically for U+0301 the lead is 0xCC. */
    size_t out_len = strlen(out);
    for (size_t k = 0; k < out_len; k++) {
        unsigned char c = (unsigned char)out[k];
        if ((c & 0xC0) == 0x80) {
            /* Must be preceded by a lead byte (0xC0+) or a previous
             * continuation that itself follows a lead. The simple
             * check: prior byte must NOT be ASCII (< 0x80). */
            EXPECT(k > 0 && (unsigned char)out[k - 1] >= 0x80);
        }
    }
    free(in);
}

static void test_head_only_partial_trailing_counted_as_one_line(void)
{
    /* 8 non-blank lines fill the HEAD_ONLY cap, then "TAIL" arrives
     * with no \n. The unterminated remainder counts as one suppressed
     * non-blank line in the footer marker. */
    char in[256];
    size_t n = 0;
    for (int i = 0; i < 8; i++)
        n += (size_t)snprintf(in + n, sizeof(in) - n, "L%d\n", i);
    memcpy(in + n, "TAIL", 4);
    n += 4;
    const char *out = render_one(R_HEAD_ONLY, in, n);
    /* Head visible. */
    EXPECT(strstr(out, "L0") != NULL);
    EXPECT(strstr(out, "L7") != NULL);
    /* Footer reports exactly one elided line (the unterminated
     * "TAIL" remainder). The marker is line-only — no byte clause —
     * so the user-facing wording is "1 more line". */
    EXPECT(strstr(out, "1 more line") != NULL);
    EXPECT(strstr(out, "more byte") == NULL);
}

static void test_head_tail_substituted_tail_no_bogus_marker(void)
{
    /* HEAD_TAIL with a single suppressed line of many dangerous
     * codepoints: each U+202E shrinks 3 bytes → 1 byte ("?") in the
     * ring. 600 of them: suppressed_bytes = 1801 (over the 1500-byte
     * cap), suppressed_display_bytes = 601 (well under). The display-
     * unit tail view fits without elision, so no "(0 more lines,
     * 0 more bytes)" bogus marker should be emitted. */
    char *in = malloc(8192);
    size_t n = 0;
    for (int i = 0; i < 4; i++)
        n += (size_t)snprintf(in + n, 8192 - n, "head%d\n", i);
    for (int i = 0; i < 600; i++) {
        in[n++] = '\xE2';
        in[n++] = '\x80';
        in[n++] = '\xAE';
    }
    in[n++] = '\n';
    const char *out = render_one(R_HEAD_TAIL, in, n);
    /* No elision marker — the display tail fits. */
    EXPECT(strstr(out, "more line") == NULL);
    EXPECT(strstr(out, "more byte") == NULL);
    /* Head and substituted tail row both rendered. */
    EXPECT(strstr(out, "head0") != NULL);
    EXPECT(strchr(out, '?') != NULL);
    free(in);
}

static void test_head_tail_dangerous_codepoint_no_bogus_tail_row(void)
{
    /* HEAD_TAIL with a single dangerous-codepoint suppressed line:
     * the ring stores the substituted display bytes (1 byte "?" per
     * substitution), but suppressed_bytes counts the original input
     * bytes (3 for U+202E). Without a separate display-byte counter
     * the ring slice would over-extend into preceding head bytes
     * and replay them as bogus tail rows. */
    const char *in = "head0\nhead1\nhead2\nhead3\n\xE2\x80\xAE\n";
    const char *out = render_one(R_HEAD_TAIL, in, strlen(in));
    /* All 4 head lines visible. */
    EXPECT(strstr(out, "head0") != NULL);
    EXPECT(strstr(out, "head3") != NULL);
    /* The substituted tail row "?" is visible (or "???" under C
     * locale per-byte substitution). */
    EXPECT(strchr(out, '?') != NULL);
    /* The trailing 3 of "head3" must NOT appear as its own tail row.
     * Specifically, the bytes "│ 3" (body strip + content "3") would
     * indicate the bug — the suppression slice grabbed bytes from
     * the prior head line. */
    EXPECT(strstr(out, STRIP_BODY ANSI_DIM "3" ANSI_RESET) == NULL);
}

static void test_dangerous_codepoint_substituted(void)
{
    /* U+202E "RIGHT-TO-LEFT OVERRIDE" is a Trojan-Source-style bidi
     * codepoint utf8_codepoint_cells flags as dangerous. It must be
     * substituted with "?" before reaching the terminal so it can't
     * reorder/hide the surrounding preview text. The exact form of
     * the substitution depends on the runtime locale: under UTF-8
     * the 3-byte sequence collapses to one "?", under a C locale
     * mbrtowc decodes byte-by-byte and each byte becomes "?". The
     * security property — original bytes don't survive — holds in
     * both cases. */
    const char *in = "ab\xE2\x80\xAE"
                     "cd\n";
    const char *out = render_one(R_HEAD_ONLY, in, strlen(in));
    /* Original dangerous byte sequence must not survive. */
    EXPECT(strstr(out, "\xE2\x80\xAE") == NULL);
    /* Some substitution happened (one or three "?" depending on
     * locale) and the surrounding ASCII passed through unchanged. */
    EXPECT(strstr(out, "ab") != NULL);
    EXPECT(strstr(out, "cd") != NULL);
    EXPECT(strchr(out, '?') != NULL);
}

/* ---------- spinner clock derivation ---------- */

static void test_spinner_glyph_now_returns_braille_frame(void)
{
    /* spinner_glyph_now() returns one of 10 Braille frames; all share
     * the U+28xx prefix (\xE2\xA0..). */
    extern const char *spinner_glyph_now(void);
    const char *g = spinner_glyph_now();
    EXPECT(g != NULL);
    EXPECT(g[0] == (char)0xE2);
    EXPECT(g[1] == (char)0xA0);
    EXPECT(g[3] == 0); /* 3-byte UTF-8 codepoint, NUL-terminated */
}

int main(void)
{
    cap_init();

    test_empty_emits_nothing();
    test_only_blank_lines_no_preview();
    test_head_only_under_cap_shows_all_lines();
    test_single_line_uses_solo_close();
    test_partial_trailing_line_committed();
    test_status_paints_with_spinner_glyph();
    test_streaming_repaints_then_commits_prior();
    test_blank_lines_elided_between_content();
    test_whitespace_only_lines_elided();
    test_indented_content_preserved();
    test_tab_expanded_to_four_spaces();
    test_head_only_exceeds_cap_emits_footer();
    test_head_tail_under_cap_shows_all_lines();
    test_head_tail_exceeds_cap_emits_marker_and_tail();
    test_head_tail_modest_overflow_replays_inline();
    test_long_line_truncated_with_ellipsis();
    test_over_indented_content_renders_truncation_marker();
    test_long_unbroken_input_buffer_bounded();
    test_diff_colors_added_and_removed();
    test_diff_tab_expanded_to_four_spaces();
    test_diff_flushes_partial_trailing_line();
    test_diff_preserves_blank_lines();
    test_ctrl_bytes_dropped_before_render();
    test_emit_callback_sets_flag();
    test_finalize_is_idempotent();
    test_blank_lines_after_cap_dont_emit_phantom_footer();
    test_head_tail_blank_only_suppression_no_phantom_marker();
    test_head_tail_renders_long_suppressed_line();
    test_head_only_partial_trailing_counted_as_one_line();
    test_head_tail_no_orphan_continuation_byte_in_replay();
    test_head_tail_blanks_in_tail_dont_eat_visible_count();
    test_post_cap_lines_dont_flood_non_tty_output();
    test_head_tail_substituted_tail_no_bogus_marker();
    test_head_tail_dangerous_codepoint_no_bogus_tail_row();
    test_dangerous_codepoint_substituted();
    test_spinner_glyph_now_returns_braille_frame();

    T_REPORT();
}
