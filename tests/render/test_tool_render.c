/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "harness.h"
#include "render/disp.h"
#include "render/tool_render.h"
#include "system/locale.h"
#include "terminal/ansi.h"

#define STRIP_FIRST      ANSI_DIM ANSI_CYAN "\xE2\x94\x8C " ANSI_RESET
#define STRIP_BODY       ANSI_DIM ANSI_CYAN "\xE2\x94\x82 " ANSI_RESET
#define STRIP_CLOSE      "\r" ANSI_DIM ANSI_CYAN "\xE2\x94\x94" ANSI_RESET
#define STRIP_CLOSE_SOLO "\r" ANSI_DIM ANSI_CYAN "\xE2\x80\xBA" ANSI_RESET

static char capture_buf[131072];

static void capture_init(void)
{
    locale_init_utf8();
    char path[64];
    snprintf(path, sizeof(path), "/tmp/haxrender.%d.out", (int)getpid());
    if (!freopen(path, "w+", stdout)) {
        perror("freopen");
        exit(1);
    }
    unlink(path);
}

static void capture_reset(void)
{
    fflush(stdout);
    if (ftruncate(fileno(stdout), 0) != 0) {
        perror("ftruncate");
        exit(1);
    }
    rewind(stdout);
}

static const char *capture_read(void)
{
    fflush(stdout);
    fseek(stdout, 0, SEEK_SET);
    size_t n = fread(capture_buf, 1, sizeof(capture_buf) - 1, stdout);
    capture_buf[n] = 0;
    return capture_buf;
}

static const char *render_one(enum tool_render_mode mode, const char *bytes, size_t n)
{
    capture_reset();
    struct disp d = {0};
    struct tool_render r;
    tool_render_init(&r, &d, NULL, mode);
    tool_render_feed(&r, bytes, n);
    tool_render_finalize(&r);
    tool_render_free(&r);
    return capture_read();
}

static void test_empty_emits_nothing(void)
{
    const char *out = render_one(TOOL_RENDER_HEAD, "", 0);
    EXPECT_STR_EQ(out, "");
}

static void test_only_blank_lines_no_preview(void)
{
    const char *in = "\n  \n\t\n";
    const char *out = render_one(TOOL_RENDER_HEAD, in, strlen(in));
    EXPECT_STR_EQ(out, "");
}

static void test_head_only_under_cap_shows_all_lines(void)
{
    const char *in = "hello\nworld\n";
    const char *out = render_one(TOOL_RENDER_HEAD, in, strlen(in));
    EXPECT(strstr(out, "hello") != NULL);
    EXPECT(strstr(out, "world") != NULL);
    EXPECT(strstr(out, STRIP_FIRST) != NULL);
    EXPECT(strstr(out, STRIP_CLOSE) != NULL);
    EXPECT(strstr(out, STRIP_BODY ANSI_DIM "world" ANSI_RESET STRIP_CLOSE) != NULL);
}

static void test_single_line_uses_solo_close(void)
{
    const char *in = "only\n";
    const char *out = render_one(TOOL_RENDER_HEAD, in, strlen(in));
    EXPECT(strstr(out, "only") != NULL);
    EXPECT(strstr(out, STRIP_FIRST ANSI_DIM "only" ANSI_RESET STRIP_CLOSE_SOLO) != NULL);
}

static void test_partial_trailing_line_committed(void)
{
    capture_reset();
    struct disp d = {0};
    struct tool_render r;
    tool_render_init(&r, &d, NULL, TOOL_RENDER_HEAD);
    tool_render_feed(&r, "no newline", 10);
    tool_render_finalize(&r);
    tool_render_free(&r);
    const char *out = capture_read();
    EXPECT(strstr(out, "no newline") != NULL);
    EXPECT(strstr(out, STRIP_FIRST ANSI_DIM "no newline" ANSI_RESET STRIP_CLOSE_SOLO) != NULL);
    EXPECT(d.pending_newlines == 1);
}

static void test_completed_line_held_silently_without_spinner(void)
{
    capture_reset();
    struct disp d = {0};
    struct tool_render r;
    tool_render_init(&r, &d, NULL, TOOL_RENDER_HEAD);
    tool_render_feed(&r, "live\n", 5);
    EXPECT_STR_EQ(capture_read(), "");
    EXPECT(d.pending_newlines == 0);
    tool_render_finalize(&r);
    EXPECT(strstr(capture_read(), "live") != NULL);
    tool_render_free(&r);
}

static void test_next_line_commits_prior_line(void)
{
    capture_reset();
    struct disp d = {0};
    struct tool_render r;
    tool_render_init(&r, &d, NULL, TOOL_RENDER_HEAD);
    tool_render_feed(&r, "one\n", 4);
    tool_render_feed(&r, "two\n", 4);
    /* The second line commits the first and is itself held until finalize. */
    const char *mid = capture_read();
    EXPECT(strstr(mid, STRIP_FIRST ANSI_DIM "one") != NULL);
    EXPECT(strstr(mid, "two") == NULL);
    tool_render_finalize(&r);
    EXPECT(strstr(capture_read(), "two") != NULL);
    tool_render_free(&r);
}

static void test_blank_lines_elided_between_content(void)
{
    const char *in = "a\n\n\nb\n";
    const char *out = render_one(TOOL_RENDER_HEAD, in, strlen(in));
    EXPECT(strstr(out, STRIP_FIRST ANSI_DIM "a") != NULL);
    EXPECT(strstr(out, STRIP_BODY ANSI_DIM "b" ANSI_RESET STRIP_CLOSE) != NULL);
}

static void test_whitespace_only_lines_elided(void)
{
    const char *in = "hello\n  \n\t\nworld\n";
    const char *out = render_one(TOOL_RENDER_HEAD, in, strlen(in));
    EXPECT(strstr(out, STRIP_FIRST ANSI_DIM "hello") != NULL);
    EXPECT(strstr(out, STRIP_BODY ANSI_DIM "world" ANSI_RESET STRIP_CLOSE) != NULL);
}

static void test_indented_content_preserved(void)
{
    const char *in = "    indented\n";
    const char *out = render_one(TOOL_RENDER_HEAD, in, strlen(in));
    EXPECT(strstr(out, STRIP_FIRST ANSI_DIM "    indented" ANSI_RESET STRIP_CLOSE_SOLO) != NULL);
}

static void test_tab_expanded_to_four_spaces(void)
{
    const char *in = "\t\thello\n";
    const char *out = render_one(TOOL_RENDER_HEAD, in, strlen(in));
    EXPECT(strchr(out, '\t') == NULL);
    EXPECT(strstr(out, "        hello") != NULL);
}

static void test_head_only_exceeds_cap_emits_footer(void)
{
    char in[8000];
    size_t n = 0;
    for (int i = 0; i < 200; i++)
        n += (size_t)snprintf(in + n, sizeof(in) - n, "line%03d\n", i);
    const char *out = render_one(TOOL_RENDER_HEAD, in, n);
    EXPECT(strstr(out, "line000") != NULL);
    EXPECT(strstr(out, "line001") != NULL);
    EXPECT(strstr(out, "more line") != NULL);
    EXPECT(strstr(out, STRIP_CLOSE) != NULL);
}

static void test_head_tail_under_cap_shows_all_lines(void)
{
    const char *in = "a\nb\n";
    const char *out = render_one(TOOL_RENDER_HEAD_TAIL, in, strlen(in));
    EXPECT(strstr(out, STRIP_FIRST ANSI_DIM "a") != NULL);
    EXPECT(strstr(out, STRIP_BODY ANSI_DIM "b" ANSI_RESET STRIP_CLOSE) != NULL);
}

static void test_head_tail_exceeds_cap_emits_marker_and_tail(void)
{
    char in[8000];
    size_t n = 0;
    for (int i = 0; i < 200; i++)
        n += (size_t)snprintf(in + n, sizeof(in) - n, "row%03d\n", i);
    const char *out = render_one(TOOL_RENDER_HEAD_TAIL, in, n);
    EXPECT(strstr(out, "row000") != NULL);
    EXPECT(strstr(out, "row199") != NULL);
    EXPECT(strstr(out, "more line") != NULL);
    EXPECT(strstr(out, " ...") != NULL);
    EXPECT(strstr(out, STRIP_CLOSE) != NULL);
}

static void test_head_tail_modest_overflow_replays_inline(void)
{
    const char *in = "a\nb\nc\nd\ne\n";
    const char *out = render_one(TOOL_RENDER_HEAD_TAIL, in, strlen(in));
    EXPECT(strstr(out, "a") != NULL);
    EXPECT(strstr(out, "b") != NULL);
    EXPECT(strstr(out, "c") != NULL);
    EXPECT(strstr(out, "d") != NULL);
    EXPECT(strstr(out, "e") != NULL);
    EXPECT(strstr(out, "more line") == NULL);
    EXPECT(strstr(out, STRIP_CLOSE) != NULL);
    EXPECT(strstr(out, STRIP_BODY ANSI_DIM "e" ANSI_RESET STRIP_CLOSE) != NULL);
}

static void test_long_line_truncated_with_ellipsis(void)
{
    char in[300];
    memset(in, 'A', 200);
    memcpy(in + 200, "TAIL_MARKER", 11);
    in[211] = '\n';
    const char *out = render_one(TOOL_RENDER_HEAD, in, 212);
    EXPECT(strstr(out, "...") != NULL);
    EXPECT(strstr(out, "TAIL_MARKER") == NULL);
    EXPECT(strstr(out, STRIP_CLOSE_SOLO) != NULL);
}

static void test_over_indented_content_renders_truncation_marker(void)
{
    char in[300];
    memset(in, ' ', 200);
    in[200] = 'x';
    in[201] = '\n';
    const char *out = render_one(TOOL_RENDER_HEAD, in, 202);
    EXPECT(strstr(out, "...") != NULL);
    EXPECT(strstr(out, STRIP_CLOSE_SOLO) != NULL);
}

static void test_long_unbroken_input_buffer_bounded(void)
{
    capture_reset();
    struct disp d = {0};
    struct tool_render r;
    tool_render_init(&r, &d, NULL, TOOL_RENDER_HEAD);
    char *buf = malloc(65536);
    memset(buf, 'X', 65536);
    tool_render_feed(&r, buf, 65536);
    EXPECT(r.line.len <= 4096);
    free(buf);
    tool_render_finalize(&r);
    EXPECT(strstr(capture_read(), "XXX") != NULL);
    tool_render_free(&r);
}

static void test_diff_colors_added_and_removed(void)
{
    const char *in = "--- a/x\n"
                     "+++ b/x\n"
                     "@@ -1,1 +1,1 @@\n"
                     "-old\n"
                     "+new\n";
    const char *out = render_one(TOOL_RENDER_DIFF, in, strlen(in));
    EXPECT(strstr(out, "--- a/x") == NULL);
    EXPECT(strstr(out, "+++ b/x") == NULL);
    EXPECT(strstr(out, "@@ -1,1 +1,1 @@") != NULL);
    EXPECT(strstr(out, "-old") != NULL);
    EXPECT(strstr(out, "+new") != NULL);
    EXPECT(strstr(out, ANSI_GREEN) != NULL);
    EXPECT(strstr(out, ANSI_RED) != NULL);
    EXPECT(strstr(out, ANSI_DIM) != NULL);
}

static void test_diff_context_lines_dimmed(void)
{
    const char *in = "@@ -1,3 +1,3 @@\n"
                     "-old\n"
                     " context\n"
                     "+new\n";
    const char *out = render_one(TOOL_RENDER_DIFF, in, strlen(in));
    EXPECT(strstr(out, ANSI_DIM " context" ANSI_RESET) != NULL);
    EXPECT(strstr(out, ANSI_RED "-old" ANSI_RESET) != NULL);
    EXPECT(strstr(out, ANSI_GREEN "+new" ANSI_RESET) != NULL);
}

static void test_diff_file_headers_elided(void)
{
    const char *in = "--- /abs/path\n"
                     "+++ /abs/path\n"
                     "@@ -1,1 +1,1 @@\n"
                     "-old\n"
                     "+new\n";
    const char *out = render_one(TOOL_RENDER_DIFF, in, strlen(in));
    EXPECT(strstr(out, "/abs/path") == NULL);
    EXPECT(strstr(out, "@@ -1,1 +1,1 @@") != NULL);
    EXPECT(strstr(out, "-old") != NULL);
    EXPECT(strstr(out, "+new") != NULL);
}

static void test_diff_tab_expanded_to_four_spaces(void)
{
    const char *in = "+\thello\n";
    const char *out = render_one(TOOL_RENDER_DIFF, in, strlen(in));
    EXPECT(strchr(out, '\t') == NULL);
    EXPECT(strstr(out, "+    hello") != NULL);
}

static void test_diff_flushes_partial_trailing_line(void)
{
    const char *in = "+added";
    const char *out = render_one(TOOL_RENDER_DIFF, in, strlen(in));
    EXPECT(strstr(out, ANSI_GREEN "+added" ANSI_RESET) != NULL);
}

static void test_diff_preserves_blank_lines(void)
{
    const char *in = "+a\n"
                     "  \n"
                     "+b\n";
    const char *out = render_one(TOOL_RENDER_DIFF, in, strlen(in));
    EXPECT(strstr(out, "+a") != NULL);
    EXPECT(strstr(out, "+b") != NULL);
    const char *p = strstr(out, STRIP_BODY);
    EXPECT(p != NULL);
    EXPECT(strstr(p + 1, STRIP_BODY) != NULL);
}

static void test_diff_dash_content_not_mistaken_for_header(void)
{
    const char *in = "--- a/x\n"
                     "+++ b/x\n"
                     "@@ -1,2 +1,2 @@\n"
                     "--- removed dashes\n"
                     "+++ added pluses\n";
    const char *out = render_one(TOOL_RENDER_DIFF, in, strlen(in));
    EXPECT(strstr(out, "--- a/x") == NULL);
    EXPECT(strstr(out, "+++ b/x") == NULL);
    EXPECT(strstr(out, ANSI_RED "--- removed dashes") != NULL);
    EXPECT(strstr(out, ANSI_GREEN "+++ added pluses") != NULL);
}

static void test_diff_inter_hunk_separator_stays_dim(void)
{
    const char *in = "--- a/x\n"
                     "+++ b/x\n"
                     "@@ -1 +1 @@\n"
                     "-old\n"
                     "+new\n"
                     "@@ -9 +9 @@\n"
                     "-foo\n"
                     "+bar\n";
    const char *out = render_one(TOOL_RENDER_DIFF, in, strlen(in));
    EXPECT(strstr(out, ANSI_DIM "@@ -9 +9 @@") != NULL);
}

static void test_ctrl_bytes_dropped_before_render(void)
{
    const char in[] = "ab\x07\x1b[31mc\x1b[mdef\n";
    const char *out = render_one(TOOL_RENDER_HEAD, in, sizeof(in) - 1);
    EXPECT(strstr(out, STRIP_FIRST ANSI_DIM "abcdef" ANSI_RESET STRIP_CLOSE_SOLO) != NULL);
}

static void test_emit_callback_sets_flag(void)
{
    capture_reset();
    struct disp d = {0};
    struct tool_render r;
    tool_render_init(&r, &d, NULL, TOOL_RENDER_HEAD);
    EXPECT(r.display_was_called == 0);
    tool_render_emit("x", 1, &r);
    EXPECT(r.display_was_called == 1);
    struct tool_render r2;
    tool_render_init(&r2, &d, NULL, TOOL_RENDER_HEAD);
    tool_render_emit("", 0, &r2);
    EXPECT(r2.display_was_called == 1);
    tool_render_finalize(&r);
    tool_render_free(&r);
    tool_render_finalize(&r2);
    tool_render_free(&r2);
}

static void test_begin_live_without_output_leaves_no_rows(void)
{
    capture_reset();
    struct disp d = {0};
    struct tool_render r;
    tool_render_init(&r, &d, NULL, TOOL_RENDER_HEAD_TAIL);
    tool_render_begin_live(&r);
    tool_render_finalize(&r);
    tool_render_free(&r);
    const char *out = capture_read();
    EXPECT(strstr(out, STRIP_FIRST) == NULL);
    EXPECT(strstr(out, STRIP_CLOSE) == NULL);
    EXPECT(strstr(out, STRIP_CLOSE_SOLO) == NULL);
    EXPECT(r.rows_emitted == 0);
}

static void test_begin_live_without_spinner_matches_plain_render(void)
{
    capture_reset();
    struct disp d = {0};
    struct tool_render r;
    tool_render_init(&r, &d, NULL, TOOL_RENDER_HEAD_TAIL);
    tool_render_begin_live(&r);
    tool_render_feed(&r, "first\nsecond\n", 13);
    tool_render_finalize(&r);
    tool_render_free(&r);
    const char *out = capture_read();
    /* Without a spinner there is no placeholder row; the block matches a plain render. */
    EXPECT(strstr(out, STRIP_FIRST ANSI_DIM "first") != NULL);
    EXPECT(strstr(out, STRIP_BODY ANSI_DIM "second" ANSI_RESET STRIP_CLOSE) != NULL);
    EXPECT(r.rows_emitted == 2);
}

static void test_finalize_is_idempotent(void)
{
    capture_reset();
    struct disp d = {0};
    struct tool_render r;
    tool_render_init(&r, &d, NULL, TOOL_RENDER_HEAD);
    tool_render_feed(&r, "x\n", 2);
    tool_render_finalize(&r);
    size_t after_first = strlen(capture_read());
    tool_render_finalize(&r);
    size_t after_second = strlen(capture_read());
    EXPECT(after_first == after_second);
    tool_render_free(&r);
}

static void test_head_tail_blank_only_suppression_no_phantom_marker(void)
{
    const char *in = "a\nb\nc\nd\n\n";
    const char *out = render_one(TOOL_RENDER_HEAD_TAIL, in, strlen(in));
    EXPECT(strstr(out, "a") != NULL);
    EXPECT(strstr(out, "d") != NULL);
    EXPECT(strstr(out, "more line") == NULL);
    EXPECT(strstr(out, "more byte") == NULL);
}

static void test_blank_lines_after_cap_dont_emit_phantom_footer(void)
{
    char in[256];
    size_t n = 0;
    for (int i = 0; i < 8; i++)
        n += (size_t)snprintf(in + n, sizeof(in) - n, "L%d\n", i);
    for (int i = 0; i < 5; i++)
        in[n++] = '\n';
    const char *out = render_one(TOOL_RENDER_HEAD, in, n);
    EXPECT(strstr(out, "L0") != NULL);
    EXPECT(strstr(out, "L7") != NULL);
    EXPECT(strstr(out, "more line") == NULL);
}

static void test_head_tail_renders_long_suppressed_line(void)
{
    char *in = malloc(8192);
    size_t n = 0;
    for (int i = 0; i < 4; i++)
        n += (size_t)snprintf(in + n, 8192 - n, "head%d\n", i);
    memcpy(in + n, "HEAD_MARKER", 11);
    n += 11;
    memset(in + n, 'A', 5000);
    n += 5000;
    in[n++] = '\n';
    const char *out = render_one(TOOL_RENDER_HEAD_TAIL, in, n);
    EXPECT(strstr(out, "HEAD_MARKER") != NULL);
    free(in);
}

static void test_head_tail_blanks_in_tail_dont_eat_visible_count(void)
{
    char *in = malloc(8192);
    size_t n = 0;
    for (int i = 0; i < 4; i++)
        n += (size_t)snprintf(in + n, 8192 - n, "head%d\n", i);
    for (int i = 0; i < 100; i++)
        n += (size_t)snprintf(in + n, 8192 - n, "mid%d\n", i);
    n += (size_t)snprintf(in + n, 8192 - n, "T_REPORT();\n}\n\n[output truncated]\n");
    const char *out = render_one(TOOL_RENDER_HEAD_TAIL, in, n);
    EXPECT(strstr(out, "mid99") != NULL);
    EXPECT(strstr(out, "T_REPORT();") != NULL);
    EXPECT(strstr(out, "}") != NULL);
    EXPECT(strstr(out, "[output truncated]") != NULL);
    free(in);
}

static void test_post_cap_lines_dont_flood_non_tty_output(void)
{
    char in[8000];
    size_t n = 0;
    for (int i = 0; i < 200; i++)
        n += (size_t)snprintf(in + n, sizeof(in) - n, "row%03d\n", i);
    const char *out = render_one(TOOL_RENDER_HEAD_TAIL, in, n);
    int late_visible = 0;
    char buf[16];
    for (int i = 5; i < 196; i++) {
        snprintf(buf, sizeof(buf), "row%03d", i);
        if (strstr(out, buf))
            late_visible++;
    }
    EXPECT(late_visible == 0);
    EXPECT(strstr(out, "row000") != NULL);
    EXPECT(strstr(out, "row199") != NULL);
}

static void test_head_tail_no_orphan_continuation_byte_in_replay(void)
{
    char *in = malloc(8192);
    size_t n = 0;
    for (int i = 0; i < 4; i++)
        n += (size_t)snprintf(in + n, 8192 - n, "head%d\n", i);
    in[n++] = 'a';
    for (int i = 0; i < 1000; i++) {
        in[n++] = '\xCC';
        in[n++] = '\x81';
    }
    in[n++] = '\n';
    const char *out = render_one(TOOL_RENDER_HEAD_TAIL, in, n);
    size_t out_len = strlen(out);
    for (size_t k = 0; k < out_len; k++) {
        unsigned char c = (unsigned char)out[k];
        if ((c & 0xC0) == 0x80) {
            EXPECT(k > 0 && (unsigned char)out[k - 1] >= 0x80);
        }
    }
    free(in);
}

static void test_head_only_partial_trailing_counted_as_one_line(void)
{
    char in[256];
    size_t n = 0;
    for (int i = 0; i < 8; i++)
        n += (size_t)snprintf(in + n, sizeof(in) - n, "L%d\n", i);
    memcpy(in + n, "TAIL", 4);
    n += 4;
    const char *out = render_one(TOOL_RENDER_HEAD, in, n);
    EXPECT(strstr(out, "L0") != NULL);
    EXPECT(strstr(out, "L7") != NULL);
    EXPECT(strstr(out, "1 more line") != NULL);
    EXPECT(strstr(out, "more byte") == NULL);
}

static void test_head_tail_substituted_tail_no_bogus_marker(void)
{
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
    const char *out = render_one(TOOL_RENDER_HEAD_TAIL, in, n);
    EXPECT(strstr(out, "more line") == NULL);
    EXPECT(strstr(out, "more byte") == NULL);
    EXPECT(strstr(out, "head0") != NULL);
    EXPECT(strchr(out, '?') != NULL);
    free(in);
}

static void test_head_tail_unsafe_codepoint_no_bogus_tail_row(void)
{
    const char *in = "head0\nhead1\nhead2\nhead3\n\xE2\x80\xAE\n";
    const char *out = render_one(TOOL_RENDER_HEAD_TAIL, in, strlen(in));
    EXPECT(strstr(out, "head0") != NULL);
    EXPECT(strstr(out, "head3") != NULL);
    EXPECT(strchr(out, '?') != NULL);
    EXPECT(strstr(out, STRIP_BODY ANSI_DIM "3" ANSI_RESET) == NULL);
}

static void test_unsafe_codepoint_substituted(void)
{
    const char *in = "ab\xE2\x80\xAE"
                     "cd\n";
    const char *out = render_one(TOOL_RENDER_HEAD, in, strlen(in));
    EXPECT(strstr(out, "\xE2\x80\xAE") == NULL);
    EXPECT(strstr(out, "ab") != NULL);
    EXPECT(strstr(out, "cd") != NULL);
    EXPECT(strchr(out, '?') != NULL);
}

int main(void)
{
    capture_init();
    test_empty_emits_nothing();
    test_only_blank_lines_no_preview();
    test_head_only_under_cap_shows_all_lines();
    test_single_line_uses_solo_close();
    test_partial_trailing_line_committed();
    test_completed_line_held_silently_without_spinner();
    test_next_line_commits_prior_line();
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
    test_diff_context_lines_dimmed();
    test_diff_file_headers_elided();
    test_diff_tab_expanded_to_four_spaces();
    test_diff_flushes_partial_trailing_line();
    test_diff_preserves_blank_lines();
    test_diff_dash_content_not_mistaken_for_header();
    test_diff_inter_hunk_separator_stays_dim();
    test_ctrl_bytes_dropped_before_render();
    test_emit_callback_sets_flag();
    test_begin_live_without_output_leaves_no_rows();
    test_begin_live_without_spinner_matches_plain_render();
    test_finalize_is_idempotent();
    test_blank_lines_after_cap_dont_emit_phantom_footer();
    test_head_tail_blank_only_suppression_no_phantom_marker();
    test_head_tail_renders_long_suppressed_line();
    test_head_only_partial_trailing_counted_as_one_line();
    test_head_tail_no_orphan_continuation_byte_in_replay();
    test_head_tail_blanks_in_tail_dont_eat_visible_count();
    test_post_cap_lines_dont_flood_non_tty_output();
    test_head_tail_substituted_tail_no_bogus_marker();
    test_head_tail_unsafe_codepoint_no_bogus_tail_row();
    test_unsafe_codepoint_substituted();
    T_REPORT();
}
