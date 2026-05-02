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
    /* Trailing \n is held, not committed by finalize when held>0 — so
     * dim opens, content "hello\nworld" lands, dim closes. */
    EXPECT_STR_EQ(out, ANSI_DIM "hello\nworld" ANSI_RESET);
}

static void test_head_only_no_trailing_nl_queues_separator(void)
{
    /* When output ends mid-line under the cap, finalize queues a \n in
     * disp so the next block starts cleanly — the byte is held, not
     * committed, so block_separator can still collapse it. */
    cap_reset();
    struct disp d = {0};
    struct tool_render r;
    tool_render_init(&r, &d, NULL, R_HEAD_ONLY);
    tool_render_feed(&r, "no newline", 10);
    tool_render_finalize(&r);
    tool_render_free(&r);
    EXPECT_STR_EQ(cap_read(), ANSI_DIM "no newline" ANSI_RESET);
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
}

/* ---------- head-tail elision ---------- */

static void test_head_tail_under_cap(void)
{
    const char *in = "a\nb\n";
    const char *out = render_one(R_HEAD_TAIL, in, strlen(in));
    /* Same as head-only when output fits — content + dim wrapper. */
    EXPECT_STR_EQ(out, ANSI_DIM "a\nb" ANSI_RESET);
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
     * the dim wrapper is applied. */
    const char in[] = "ab\x07\x1b[31mc\x1b[mdef\n";
    const char *out = render_one(R_HEAD_ONLY, in, sizeof(in) - 1);
    EXPECT_STR_EQ(out, ANSI_DIM "abcdef" ANSI_RESET);
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
    test_emit_callback_sets_flag();

    T_REPORT();
}
