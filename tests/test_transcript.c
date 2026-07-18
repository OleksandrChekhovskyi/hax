/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "provider.h"
#include "transcript.h"
#include "terminal/ansi.h"

/* Render into a memstream so the test can inspect the bytes without
 * touching the filesystem or stdout. */
static char *render_to_string(const char *sys, const struct item *items, size_t n)
{
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f) {
        perror("open_memstream");
        exit(1);
    }
    transcript_render(f, sys, NULL, 0, items, n);
    fclose(f);
    return buf;
}

static char *render_with_tools(const struct tool_def *tools, size_t n_tools)
{
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f) {
        perror("open_memstream");
        exit(1);
    }
    transcript_render(f, NULL, tools, n_tools, NULL, 0);
    fclose(f);
    return buf;
}

static int contains(const char *hay, const char *needle)
{
    return strstr(hay, needle) != NULL;
}

static int count_occurrences(const char *hay, const char *needle)
{
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += strlen(needle))
        n++;
    return n;
}

static void test_banner_box(void)
{
    char *out = render_to_string(NULL, NULL, 0);
    EXPECT(contains(out, "TRANSCRIPT"));
    /* Three-line box: top, sides, bottom. */
    EXPECT(contains(out, "┏"));
    EXPECT(contains(out, "┃"));
    EXPECT(contains(out, "┗"));
    free(out);
}

static void test_turn_rules_count_boundary_markers(void)
{
    /* Mimics what agent.c writes into items[]: a TURN_BOUNDARY just
     * before the user message (so the user's input lives under its
     * own turn header), and another before each follow-up model
     * request after tool dispatch. */
    struct item items[] = {
        {.kind = ITEM_TURN_BOUNDARY},
        {.kind = ITEM_USER_MESSAGE, .text = (char *)"do the thing"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = (char *)"reading first"},
        {.kind = ITEM_TOOL_CALL, .call_id = (char *)"c1", .tool_name = (char *)"read"},
        {.kind = ITEM_TOOL_RESULT, .call_id = (char *)"c1", .output = (char *)"file contents"},
        {.kind = ITEM_TURN_BOUNDARY},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = (char *)"all done"},
    };
    char *out = render_to_string(NULL, items, sizeof(items) / sizeof(items[0]));
    EXPECT(contains(out, "# turn 1 "));
    EXPECT(contains(out, "# turn 2 "));
    EXPECT(!contains(out, "# turn 3 "));
    const char *t1 = strstr(out, "# turn 1");
    const char *t2 = strstr(out, "# turn 2");
    const char *user = strstr(out, "do the thing");
    const char *first_a = strstr(out, "reading first");
    const char *result = strstr(out, "file contents");
    const char *final_a = strstr(out, "all done");
    /* Turn 1 wraps the user message and the first assistant reply. */
    EXPECT(t1 && user && first_a && t1 < user && user < first_a);
    /* Turn 2 sits between the tool result and the follow-up reply. */
    EXPECT(result && t2 && final_a && result < t2 && t2 < final_a);
    free(out);
}

static void test_parallel_calls_render_paired_with_results(void)
{
    /* Agent's history serializes a parallel batch as
     * (CALL_a, CALL_b, RESULT_a, RESULT_b). The transcript should
     * pair each call with its matching result inline, so the visual
     * order is CALL_a, RESULT_a, CALL_b, RESULT_b. */
    struct item items[] = {
        {.kind = ITEM_TOOL_CALL,
         .call_id = (char *)"c_alpha",
         .tool_name = (char *)"read",
         .tool_arguments_json = (char *)"{\"path\":\"alpha.c\"}"},
        {.kind = ITEM_TOOL_CALL,
         .call_id = (char *)"c_beta",
         .tool_name = (char *)"read",
         .tool_arguments_json = (char *)"{\"path\":\"beta.c\"}"},
        {.kind = ITEM_TOOL_RESULT, .call_id = (char *)"c_alpha", .output = (char *)"ALPHA_BODY"},
        {.kind = ITEM_TOOL_RESULT, .call_id = (char *)"c_beta", .output = (char *)"BETA_BODY"},
    };
    char *out = render_to_string(NULL, items, 4);
    const char *p_alpha_path = strstr(out, "alpha.c");
    const char *p_alpha_body = strstr(out, "ALPHA_BODY");
    const char *p_beta_path = strstr(out, "beta.c");
    const char *p_beta_body = strstr(out, "BETA_BODY");
    EXPECT(p_alpha_path && p_alpha_body && p_beta_path && p_beta_body);
    /* Each call's body must follow its own header, and the alpha
     * pair must precede the beta pair. */
    EXPECT(p_alpha_path < p_alpha_body);
    EXPECT(p_alpha_body < p_beta_path);
    EXPECT(p_beta_path < p_beta_body);
    /* Each result is rendered exactly once. */
    EXPECT(strstr(p_alpha_body + 1, "ALPHA_BODY") == NULL);
    EXPECT(strstr(p_beta_body + 1, "BETA_BODY") == NULL);
    free(out);
}

static void test_no_boundary_no_turn_rule(void)
{
    /* The renderer is a faithful pass over items[] — without an
     * explicit TURN_BOUNDARY there is no turn rule, even when a user
     * message is present. */
    struct item items[] = {{.kind = ITEM_USER_MESSAGE, .text = (char *)"hello"}};
    char *out = render_to_string(NULL, items, 1);
    EXPECT(!contains(out, "turn"));
    EXPECT(contains(out, "── user ──"));
    free(out);
}

static void test_system_prompt(void)
{
    char *out = render_to_string("you are hax", NULL, 0);
    EXPECT(contains(out, "system prompt"));
    EXPECT(contains(out, "you are hax"));
    free(out);
}

static void test_user_message_has_section_rule(void)
{
    struct item items[] = {{.kind = ITEM_USER_MESSAGE, .text = (char *)"hello"}};
    char *out = render_to_string(NULL, items, 1);
    /* The `── user ──` rule plays the same role here that the
     * `── assistant ──` / `── tool result ──` rules play elsewhere
     * in the transcript — a single anchor per turn instead of a
     * per-line strip that would visually fall apart when the pager
     * soft-wraps long lines. */
    EXPECT(contains(out, "── user ──"));
    EXPECT(contains(out, ANSI_BRIGHT_MAGENTA));
    EXPECT(contains(out, "hello"));
    /* The legacy per-line `▌ ` strip has been retired in favor of
     * the section rule. */
    EXPECT(!contains(out, "▌ "));
    free(out);
}

static void test_user_compact_seed_label(void)
{
    /* A compaction seed keeps the user body verbatim and untruncated (it
     * IS what went on the wire) but renders under its own rule with a dim
     * body: the accent stays exclusive to human-typed input. */
    struct item items[] = {
        {.kind = ITEM_USER_MESSAGE, .text = (char *)"line one\nline two", .compact_seed = 1}};
    char *out = render_to_string(NULL, items, 1);
    EXPECT(contains(out, "── compaction seed ──"));
    EXPECT(!contains(out, "── user ──"));
    EXPECT(!contains(out, ANSI_BRIGHT_MAGENTA));
    /* Dim, re-applied per line so it survives the pager. */
    EXPECT(contains(out, ANSI_DIM "line one" ANSI_RESET "\n" ANSI_DIM "line two" ANSI_RESET));
    free(out);
}

static void test_user_multiline_raw(void)
{
    /* The transcript renders user text raw — embedded newlines pass
     * through verbatim, no per-line prefix. */
    struct item items[] = {{.kind = ITEM_USER_MESSAGE, .text = (char *)"one\ntwo"}};
    char *out = render_to_string(NULL, items, 1);
    EXPECT(contains(out, "one" ANSI_FG_DEFAULT "\n" ANSI_BRIGHT_MAGENTA "two"));
    EXPECT(count_occurrences(out, ANSI_BRIGHT_MAGENTA) == 2);
    EXPECT(!contains(out, "▌ "));
    free(out);
}

static void test_assistant_message(void)
{
    struct item items[] = {{.kind = ITEM_ASSISTANT_MESSAGE, .text = (char *)"sure thing"}};
    char *out = render_to_string(NULL, items, 1);
    EXPECT(contains(out, "assistant"));
    EXPECT(contains(out, "sure thing"));
    free(out);
}

static void test_tool_call_pretty_prints_args(void)
{
    struct item items[] = {{
        .kind = ITEM_TOOL_CALL,
        .call_id = (char *)"call_42",
        .tool_name = (char *)"read",
        .tool_arguments_json = (char *)"{\"path\":\"foo.c\"}",
    }};
    char *out = render_to_string(NULL, items, 1);
    EXPECT(contains(out, "[read]"));
    /* call_id is intentionally not rendered — pairing is inline, so
     * the id would be noise. Available via HAX_TRACE if needed. */
    EXPECT(!contains(out, "call_42"));
    /* JSON_INDENT(2) inserts a newline + two-space indent before the key. */
    EXPECT(contains(out, "\n  \"path\": \"foo.c\""));
    free(out);
}

static void test_tool_call_invalid_json_dumps_verbatim(void)
{
    struct item items[] = {{
        .kind = ITEM_TOOL_CALL,
        .tool_name = (char *)"bash",
        .tool_arguments_json = (char *)"{not json",
    }};
    char *out = render_to_string(NULL, items, 1);
    EXPECT(contains(out, "[bash]"));
    EXPECT(contains(out, "{not json"));
    free(out);
}

static void test_tool_result_unshortened(void)
{
    /* Build a 4 KiB body to confirm nothing truncates it. */
    size_t n = 4096;
    char *body = malloc(n + 1);
    memset(body, 'x', n);
    body[n] = '\0';
    struct item items[] = {{.kind = ITEM_TOOL_RESULT, .output = body}};
    char *out = render_to_string(NULL, items, 1);
    EXPECT(contains(out, "tool result"));
    /* Count the x's that landed in the output — should match the body. */
    size_t x_count = 0;
    for (const char *p = out; *p; p++)
        if (*p == 'x')
            x_count++;
    EXPECT(x_count == n);
    free(out);
    free(body);
}

static void test_read_result_dims_line_number_prefix(void)
{
    /* The gutter read.c fabricates (spaces, digits, arrow) is dimmed;
     * the file content after the arrow stays on the default foreground
     * and byte-exact. */
    struct item items[] = {
        {.kind = ITEM_TOOL_CALL, .call_id = (char *)"c1", .tool_name = (char *)"read"},
        {.kind = ITEM_TOOL_RESULT,
         .call_id = (char *)"c1",
         .output = (char *)"     1→foo\n     2→bar\n"},
    };
    char *out = render_to_string(NULL, items, 2);
    EXPECT(contains(out, ANSI_DIM "     1→" ANSI_RESET "foo"));
    EXPECT(contains(out, ANSI_DIM "     2→" ANSI_RESET "bar"));
    free(out);
}

static void test_read_result_non_prefixed_lines_stay_plain(void)
{
    /* Error messages and truncation markers carry no line-number
     * prefix — they must pass through unstyled, per line. */
    struct item items[] = {
        {.kind = ITEM_TOOL_CALL, .call_id = (char *)"c1", .tool_name = (char *)"read"},
        {.kind = ITEM_TOOL_RESULT,
         .call_id = (char *)"c1",
         .output = (char *)"     1→foo\n\n[truncated at 500 lines; file has more — pass "
                           "offset/limit to read more]"},
    };
    char *out = render_to_string(NULL, items, 2);
    EXPECT(contains(out, ANSI_DIM "     1→" ANSI_RESET "foo"));
    EXPECT(contains(out, "\n[truncated at 500 lines"));
    EXPECT(!contains(out, ANSI_DIM "[truncated"));
    free(out);
}

static void test_edit_diff_result_colored(void)
{
    struct item items[] = {
        {.kind = ITEM_TOOL_CALL, .call_id = (char *)"c1", .tool_name = (char *)"edit"},
        {.kind = ITEM_TOOL_RESULT,
         .call_id = (char *)"c1",
         .output = (char *)"--- a/f.c\n+++ b/f.c\n@@ -1,2 +1,2 @@\n keep\n-old\n+new\n"},
    };
    char *out = render_to_string(NULL, items, 2);
    /* Default (ansi) preset: THEME_REMOVE = red, THEME_ADD = green. */
    EXPECT(contains(out, ANSI_RED "-old" ANSI_RESET));
    EXPECT(contains(out, ANSI_GREEN "+new" ANSI_RESET));
    /* Metadata lines are dimmed — and unlike the live preview, the
     * file headers are kept, not elided. */
    EXPECT(contains(out, ANSI_DIM "--- a/f.c" ANSI_RESET));
    EXPECT(contains(out, ANSI_DIM "+++ b/f.c" ANSI_RESET));
    EXPECT(contains(out, ANSI_DIM "@@ -1,2 +1,2 @@" ANSI_RESET));
    /* Context is real file content: default foreground, no styling —
     * the transcript's baseline for tool output (only the live
     * preview dims it, to match its all-dim preview rows). */
    EXPECT(contains(out, ANSI_RESET "\n keep\n"));
    EXPECT(!contains(out, ANSI_DIM " keep"));
    free(out);
}

static void test_write_created_confirmation_stays_plain(void)
{
    /* A new-file write returns "created ..." — not a diff, so no diff
     * coloring may fire despite the tool being diff-capable. */
    struct item items[] = {
        {.kind = ITEM_TOOL_CALL, .call_id = (char *)"c1", .tool_name = (char *)"write"},
        {.kind = ITEM_TOOL_RESULT,
         .call_id = (char *)"c1",
         .output = (char *)"created /tmp/x.c (3 lines, 42 bytes)"},
    };
    char *out = render_to_string(NULL, items, 2);
    EXPECT(contains(out, "created /tmp/x.c"));
    EXPECT(!contains(out, ANSI_GREEN));
    EXPECT(!contains(out, ANSI_RED));
    free(out);
}

static void test_diff_lookalike_from_other_tool_stays_plain(void)
{
    /* Only edit/write results are diff-colored — bash output that
     * happens to be a unified diff renders verbatim. */
    struct item items[] = {
        {.kind = ITEM_TOOL_CALL, .call_id = (char *)"c1", .tool_name = (char *)"bash"},
        {.kind = ITEM_TOOL_RESULT,
         .call_id = (char *)"c1",
         .output = (char *)"--- a/f.c\n+++ b/f.c\n@@ -1 +1 @@\n-old\n+new\n"},
    };
    char *out = render_to_string(NULL, items, 2);
    EXPECT(contains(out, "-old\n+new"));
    EXPECT(!contains(out, ANSI_GREEN));
    EXPECT(!contains(out, ANSI_RED));
    free(out);
}

static void test_orphan_read_result_stays_plain(void)
{
    /* An orphan result has no paired call, hence no tool name — the
     * styled body renderers must not fire. */
    struct item items[] = {
        {.kind = ITEM_TOOL_RESULT, .call_id = (char *)"c9", .output = (char *)"     1→foo"}};
    char *out = render_to_string(NULL, items, 1);
    EXPECT(contains(out, "     1→foo"));
    EXPECT(!contains(out, ANSI_DIM "     1→"));
    free(out);
}

static void test_plain_mode_file_tool_results_have_no_escapes(void)
{
    /* The HAX_TRANSCRIPT log path (color=0) must stay pure plain text
     * — styling is strictly a color-mode addition. */
    struct item items[] = {
        {.kind = ITEM_TOOL_CALL, .call_id = (char *)"c1", .tool_name = (char *)"read"},
        {.kind = ITEM_TOOL_RESULT, .call_id = (char *)"c1", .output = (char *)"     1→foo\n"},
        {.kind = ITEM_TOOL_CALL, .call_id = (char *)"c2", .tool_name = (char *)"edit"},
        {.kind = ITEM_TOOL_RESULT,
         .call_id = (char *)"c2",
         .output = (char *)"--- a/f.c\n+++ b/f.c\n@@ -1 +1 @@\n-old\n+new\n"},
    };
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f) {
        perror("open_memstream");
        exit(1);
    }
    int turn_no = 0;
    transcript_render_items(f, 0, items, 4, 0, &turn_no);
    fclose(f);
    EXPECT(contains(buf, "     1→foo"));
    EXPECT(contains(buf, "-old\n+new"));
    EXPECT(!contains(buf, "\x1b["));
    free(buf);
}

static void test_tools_section_renders_each_tool(void)
{
    struct tool_def tools[] = {
        {.name = "read",
         .description = "Read a file from disk.",
         .parameters_schema_json = "{\"type\":\"object\","
                                   "\"properties\":{\"path\":{\"type\":\"string\"}},"
                                   "\"required\":[\"path\"]}"},
        {.name = "bash",
         .description = "Run a shell command.",
         .parameters_schema_json = "{\"type\":\"object\","
                                   "\"properties\":{\"command\":{\"type\":\"string\"}}}"},
    };
    char *out = render_with_tools(tools, 2);
    EXPECT(contains(out, "── tools ──"));
    EXPECT(contains(out, "[read]"));
    EXPECT(contains(out, "Read a file from disk."));
    EXPECT(contains(out, "[bash]"));
    EXPECT(contains(out, "Run a shell command."));
    /* Schema is pretty-printed: JSON_INDENT(2) inserts newline + spaces. */
    EXPECT(contains(out, "\n  \"type\": \"object\""));
    EXPECT(contains(out, "\"path\""));
    EXPECT(contains(out, "\"command\""));
    /* read precedes bash in the output. */
    const char *pr = strstr(out, "[read]");
    const char *pb = strstr(out, "[bash]");
    EXPECT(pr && pb && pr < pb);
    free(out);
}

static void test_tools_section_omitted_when_empty(void)
{
    /* --raw mode advertises no tools — the section should disappear
     * entirely, not appear with an empty body. */
    char *out = render_with_tools(NULL, 0);
    EXPECT(!contains(out, "── tools ──"));
    free(out);
}

static void test_turn_usage_footer(void)
{
    /* Estimated request: "~" total, non-overlapping token categories
     * ("in" is the uncached remainder) each with its component cost. */
    struct turn_usage est = {
        .usage = {.input_tokens = 3072,
                  .output_tokens = 512,
                  .cached_tokens = 1024,
                  .cache_write_tokens = -1,
                  .cache_write_1h_tokens = -1,
                  .cost = -1},
        .elapsed_ms = 42000,
        .cost_in = 0.025,
        .cost_cache_read = 0.048,
        .cost_cache_write = -1,
        .cost_out = 0.084,
        .cost_total = 0.157,
        .cost_estimated = 1,
    };
    struct item items[] = {
        {.kind = ITEM_TURN_BOUNDARY},
        {.kind = ITEM_USER_MESSAGE, .text = (char *)"question"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = (char *)"answer"},
        {.kind = ITEM_TURN_USAGE, .usage = &est},
    };
    char *out = render_to_string(NULL, items, sizeof(items) / sizeof(items[0]));
    EXPECT(contains(out, "42s · ~$0.157 · in 2.0k $0.025 · cache 1.0k $0.048 · out 512 $0.084"));
    /* The footer trails the response it accounts. */
    const char *ans = strstr(out, "answer");
    const char *foot = strstr(out, "42s ·");
    EXPECT(ans && foot && ans < foot);
    free(out);

    /* Reported request: exact total, bare token counts — a reported
     * charge can't be decomposed, so no per-category dollars appear. */
    struct turn_usage exact = {
        .usage = {.input_tokens = 1000,
                  .output_tokens = 50,
                  .cached_tokens = -1,
                  .cache_write_tokens = -1,
                  .cache_write_1h_tokens = -1,
                  .cost = 0.0012},
        .elapsed_ms = 3000,
        .cost_in = -1,
        .cost_cache_read = -1,
        .cost_cache_write = -1,
        .cost_out = -1,
        .cost_total = 0.0012,
        .cost_estimated = 0,
    };
    items[3].usage = &exact;
    out = render_to_string(NULL, items, sizeof(items) / sizeof(items[0]));
    EXPECT(contains(out, "3s · $0.0012 · in 1000 · out 50"));
    EXPECT(!contains(out, "~$"));
    free(out);
}

static void test_reasoning_shows_id(void)
{
    struct item items[] = {{
        .kind = ITEM_REASONING,
        .reasoning_json = (char *)"{\"id\":\"rs_abc\",\"encrypted_content\":\"xxxx\"}",
    }};
    char *out = render_to_string(NULL, items, 1);
    EXPECT(contains(out, "[reasoning]"));
    EXPECT(contains(out, "rs_abc"));
    /* Encrypted blob should NOT be dumped verbatim. */
    EXPECT(!contains(out, "xxxx"));
    free(out);
}

int main(void)
{
    test_banner_box();
    test_turn_rules_count_boundary_markers();
    test_parallel_calls_render_paired_with_results();
    test_no_boundary_no_turn_rule();
    test_system_prompt();
    test_user_message_has_section_rule();
    test_user_compact_seed_label();
    test_user_multiline_raw();
    test_assistant_message();
    test_tool_call_pretty_prints_args();
    test_tool_call_invalid_json_dumps_verbatim();
    test_tool_result_unshortened();
    test_read_result_dims_line_number_prefix();
    test_read_result_non_prefixed_lines_stay_plain();
    test_edit_diff_result_colored();
    test_write_created_confirmation_stays_plain();
    test_diff_lookalike_from_other_tool_stays_plain();
    test_orphan_read_result_stays_plain();
    test_plain_mode_file_tool_results_have_no_escapes();
    test_tools_section_renders_each_tool();
    test_tools_section_omitted_when_empty();
    test_reasoning_shows_id();
    test_turn_usage_footer();
    T_REPORT();
}
