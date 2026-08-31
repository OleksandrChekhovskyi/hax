/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "provider.h"
#include "transcript.h"
#include "system/locale.h"
#include "terminal/ansi.h"

static char *render_to_string(const char *system_prompt, const struct item *items, size_t n_items)
{
    char *output = NULL;
    size_t output_length = 0;
    FILE *stream = open_memstream(&output, &output_length);
    if (!stream) {
        perror("open_memstream");
        exit(1);
    }
    transcript_render(stream, system_prompt, NULL, 0, items, n_items);
    fclose(stream);
    return output;
}

static char *render_with_tools(const struct tool_def *tools, size_t n_tools)
{
    char *output = NULL;
    size_t output_length = 0;
    FILE *stream = open_memstream(&output, &output_length);
    if (!stream) {
        perror("open_memstream");
        exit(1);
    }
    transcript_render(stream, NULL, tools, n_tools, NULL, 0);
    fclose(stream);
    return output;
}

static char *render_item_range(enum transcript_render_mode mode, const struct item *items,
                               size_t n_items, size_t first_item, int *turn_number)
{
    char *output = NULL;
    size_t output_length = 0;
    FILE *stream = open_memstream(&output, &output_length);
    if (!stream) {
        perror("open_memstream");
        exit(1);
    }
    transcript_render_items(stream, mode, items, n_items, first_item, turn_number);
    fclose(stream);
    return output;
}

static int contains(const char *text, const char *substring)
{
    return strstr(text, substring) != NULL;
}

static size_t count_occurrences(const char *text, const char *substring)
{
    size_t count = 0;
    for (const char *match = text; (match = strstr(match, substring)); match += strlen(substring))
        count++;
    return count;
}

/* Must run before main() initializes the locale, because that is the only state deciding the
 * spelling and every platform the tests run on offers a UTF-8 locale to switch to. */
static void test_banner_degrades_to_ascii_without_utf8(void)
{
    char *out = render_to_string(NULL, NULL, 0);
    EXPECT(contains(out, "TRANSCRIPT"));

    EXPECT(contains(out, "+--"));
    EXPECT(contains(out, "|"));
    EXPECT(!contains(out, "┏"));
    EXPECT(!contains(out, "━"));
    free(out);
}

static void test_banner_box(void)
{
    char *out = render_to_string(NULL, NULL, 0);
    EXPECT(contains(out, "TRANSCRIPT"));

    EXPECT(contains(out, "┏"));
    EXPECT(contains(out, "┃"));
    EXPECT(contains(out, "┗"));
    free(out);
}

static void test_turn_rules_count_boundary_markers(void)
{
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
    const char *turn_1 = strstr(out, "# turn 1");
    const char *turn_2 = strstr(out, "# turn 2");
    const char *user = strstr(out, "do the thing");
    const char *first_answer = strstr(out, "reading first");
    const char *result = strstr(out, "file contents");
    const char *final_answer = strstr(out, "all done");

    EXPECT(turn_1 && user && first_answer && turn_1 < user && user < first_answer);

    EXPECT(result && turn_2 && final_answer && result < turn_2 && turn_2 < final_answer);
    free(out);
}

static void test_parallel_calls_render_paired_with_results(void)
{
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
    const char *alpha_path = strstr(out, "alpha.c");
    const char *alpha_body = strstr(out, "ALPHA_BODY");
    const char *beta_path = strstr(out, "beta.c");
    const char *beta_body = strstr(out, "BETA_BODY");
    EXPECT(alpha_path && alpha_body && beta_path && beta_body);

    EXPECT(alpha_path < alpha_body);
    EXPECT(alpha_body < beta_path);
    EXPECT(beta_path < beta_body);

    EXPECT(strstr(alpha_body + 1, "ALPHA_BODY") == NULL);
    EXPECT(strstr(beta_body + 1, "BETA_BODY") == NULL);
    free(out);
}

static void test_item_range_pairs_results_with_nonzero_offset(void)
{
    struct item items[] = {
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = (char *)"already rendered"},
        {.kind = ITEM_TOOL_CALL,
         .call_id = (char *)"c1",
         .tool_name = (char *)"read",
         .tool_arguments_json = (char *)"{\"path\":\"new.c\"}"},
        {.kind = ITEM_TOOL_RESULT, .call_id = (char *)"c1", .output = (char *)"NEW_BODY"},
    };
    int turn_number = 0;
    char *out = render_item_range(TRANSCRIPT_RENDER_ANSI, items, 3, 1, &turn_number);
    EXPECT(!contains(out, "already rendered"));
    const char *path = strstr(out, "new.c");
    const char *body = strstr(out, "NEW_BODY");
    EXPECT(path && body && path < body);
    EXPECT(strstr(body + 1, "NEW_BODY") == NULL);
    free(out);
}

static void test_no_boundary_no_turn_rule(void)
{
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

static void test_user_message_uses_section_rule_without_line_prefix(void)
{
    struct item items[] = {{.kind = ITEM_USER_MESSAGE, .text = (char *)"hello"}};
    char *out = render_to_string(NULL, items, 1);

    EXPECT(contains(out, "── user ──"));
    EXPECT(contains(out, ANSI_BRIGHT_MAGENTA));
    EXPECT(contains(out, "hello"));

    EXPECT(!contains(out, "▌ "));
    free(out);
}

static void test_synthetic_user_messages_have_distinct_labels(void)
{
    struct item items[] = {{.kind = ITEM_USER_MESSAGE,
                            .text = (char *)"line one\nline two",
                            .origin = ITEM_ORIGIN_COMPACT_SEED}};
    char *out = render_to_string(NULL, items, 1);
    EXPECT(contains(out, "── compaction seed ──"));
    EXPECT(!contains(out, "── user ──"));
    EXPECT(!contains(out, ANSI_BRIGHT_MAGENTA));

    EXPECT(contains(out, ANSI_DIM "line one" ANSI_RESET "\n" ANSI_DIM "line two" ANSI_RESET));
    free(out);

    struct item continuation_items[] = {{.kind = ITEM_USER_MESSAGE,
                                         .text = (char *)"[continue]",
                                         .origin = ITEM_ORIGIN_CONTINUATION}};
    char *continuation_output = render_to_string(NULL, continuation_items, 1);
    EXPECT(contains(continuation_output, "── continuation ──"));
    EXPECT(!contains(continuation_output, "── user ──"));
    EXPECT(!contains(continuation_output, ANSI_BRIGHT_MAGENTA));
    free(continuation_output);
}

static void test_user_multiline_raw(void)
{
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

static void test_tool_call_pretty_prints_args_without_id(void)
{
    struct item items[] = {{
        .kind = ITEM_TOOL_CALL,
        .call_id = (char *)"call_42",
        .tool_name = (char *)"read",
        .tool_arguments_json = (char *)"{\"path\":\"foo.c\"}",
    }};
    char *out = render_to_string(NULL, items, 1);
    EXPECT(contains(out, "[read]"));

    EXPECT(!contains(out, "call_42"));

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
    size_t body_length = 4096;
    char *body = malloc(body_length + 1);
    memset(body, 'x', body_length);
    body[body_length] = '\0';
    struct item items[] = {{.kind = ITEM_TOOL_RESULT, .output = body}};
    char *out = render_to_string(NULL, items, 1);
    EXPECT(contains(out, "tool result"));

    size_t x_count = 0;
    for (const char *cursor = out; *cursor; cursor++)
        if (*cursor == 'x')
            x_count++;
    EXPECT(x_count == body_length);
    free(out);
    free(body);
}

static void test_read_result_dims_line_number_prefix(void)
{
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

    EXPECT(contains(out, ANSI_DIM "--- a/f.c" ANSI_RESET));
    EXPECT(contains(out, ANSI_DIM "+++ b/f.c" ANSI_RESET));
    EXPECT(contains(out, ANSI_DIM "@@ -1,2 +1,2 @@" ANSI_RESET));

    EXPECT(contains(out, ANSI_RESET "\n keep\n"));
    EXPECT(!contains(out, ANSI_DIM " keep"));
    free(out);
}

static void test_write_created_confirmation_stays_plain(void)
{
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
    struct item items[] = {
        {.kind = ITEM_TOOL_RESULT, .call_id = (char *)"c9", .output = (char *)"     1→foo"}};
    char *out = render_to_string(NULL, items, 1);
    EXPECT(contains(out, "     1→foo"));
    EXPECT(!contains(out, ANSI_DIM "     1→"));
    free(out);
}

static void test_plain_mode_file_tool_results_have_no_escapes(void)
{
    struct item items[] = {
        {.kind = ITEM_TOOL_CALL, .call_id = (char *)"c1", .tool_name = (char *)"read"},
        {.kind = ITEM_TOOL_RESULT, .call_id = (char *)"c1", .output = (char *)"     1→foo\n"},
        {.kind = ITEM_TOOL_CALL, .call_id = (char *)"c2", .tool_name = (char *)"edit"},
        {.kind = ITEM_TOOL_RESULT,
         .call_id = (char *)"c2",
         .output = (char *)"--- a/f.c\n+++ b/f.c\n@@ -1 +1 @@\n-old\n+new\n"},
    };
    int turn_number = 0;
    char *output = render_item_range(TRANSCRIPT_RENDER_PLAIN, items, 4, 0, &turn_number);
    EXPECT(contains(output, "     1→foo"));
    EXPECT(contains(output, "-old\n+new"));
    EXPECT(!contains(output, "\x1b["));
    free(output);
}

static void test_tools_section_renders_each_tool(void)
{
    static const struct tool_param read_params[] = {
        {.name = "path", .type = "string", .required = 1},
    };
    static const struct tool_param bash_params[] = {
        {.name = "command", .type = "string"},
    };
    struct tool_def tools[] = {
        {.name = "read",
         .description = "Read a file from disk.",
         .params = read_params,
         .n_params = 1},
        {.name = "bash",
         .description = "Run a shell command.",
         .params = bash_params,
         .n_params = 1},
    };
    char *out = render_with_tools(tools, 2);
    EXPECT(contains(out, "── tools ──"));
    EXPECT(contains(out, "[read]"));
    EXPECT(contains(out, "Read a file from disk."));
    EXPECT(contains(out, "[bash]"));
    EXPECT(contains(out, "Run a shell command."));

    EXPECT(contains(out, "\n  \"type\": \"object\""));
    EXPECT(contains(out, "\"path\""));
    EXPECT(contains(out, "\"command\""));

    const char *read = strstr(out, "[read]");
    const char *bash = strstr(out, "[bash]");
    EXPECT(read && bash && read < bash);
    free(out);
}

static void test_tools_section_omitted_when_empty(void)
{
    char *out = render_with_tools(NULL, 0);
    EXPECT(!contains(out, "── tools ──"));
    free(out);
}

static char *render_usage(struct turn_usage *usage)
{
    struct item items[] = {
        {.kind = ITEM_TURN_BOUNDARY},
        {.kind = ITEM_USER_MESSAGE, .text = (char *)"question"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = (char *)"answer"},
        {.kind = ITEM_TURN_USAGE, .usage = usage},
    };
    return render_to_string(NULL, items, sizeof(items) / sizeof(items[0]));
}

static struct turn_usage exact_usage(void)
{
    return (struct turn_usage){
        .usage = {.input_tokens = 1000,
                  .output_tokens = 50,
                  .cached_tokens = -1,
                  .cache_write_tokens = 400,
                  .cache_write_1h_tokens = -1,
                  .cost = 0.0012},
        .elapsed_ms = 3000,
        .uncached_input_tokens = 600,
        .cost_input = 0.0002,
        .cost_cache_read = -1,
        .cost_cache_write = 0.0008,
        .cost_output = 0.0002,
        .cost_total = 0.0012,
        .cost_estimated = 0,
    };
}

static void test_estimated_turn_usage_footer(void)
{
    struct turn_usage usage = {
        .usage = {.input_tokens = 3072,
                  .output_tokens = 512,
                  .cached_tokens = 1024,
                  .cache_write_tokens = -1,
                  .cache_write_1h_tokens = -1,
                  .cost = -1},
        .elapsed_ms = 42000,
        .uncached_input_tokens = 2048,
        .cost_input = 0.025,
        .cost_cache_read = 0.048,
        .cost_cache_write = -1,
        .cost_output = 0.084,
        .cost_total = 0.157,
        .cost_estimated = 1,
    };
    char *out = render_usage(&usage);
    EXPECT(contains(out, "42s · ~$0.157 · in 2k ~$0.025 · cache 1k ~$0.048 · out 512 ~$0.084"));

    const char *answer = strstr(out, "answer");
    const char *footer = strstr(out, "42s ·");
    EXPECT(answer && footer && answer < footer);
    free(out);
}

static void test_exact_turn_usage_footer(void)
{
    struct turn_usage usage = exact_usage();
    char *out = render_usage(&usage);
    EXPECT(contains(out, "3s · $0.0012 · in 600 ~$0.0002 · write 400 ~$0.0008 · out 50 ~$0.0002"));
    EXPECT(!contains(out, "~$0.0012"));
    free(out);
}

static void test_overlapping_cache_usage_uses_precomputed_uncached_count(void)
{
    struct turn_usage usage = {
        .usage = {.input_tokens = 7047,
                  .output_tokens = 12,
                  .cached_tokens = 3524,
                  .cache_write_tokens = 3524,
                  .cache_write_1h_tokens = -1,
                  .cost = 0.0092163},
        .elapsed_ms = 2000,
        .uncached_input_tokens = 3523,
        .cost_input = 0.007046,
        .cost_cache_read = 0.0007048,
        .cost_cache_write = 0.0013215,
        .cost_output = 0.000144,
        .cost_total = 0.0092163,
        .cost_estimated = 0,
    };
    char *out = render_usage(&usage);
    EXPECT(contains(out, "in 3.5k ~$0.0070"));
    EXPECT(!contains(out, "in 0"));
    free(out);
}

static void test_tiny_usage_cost_is_omitted(void)
{
    struct turn_usage usage = exact_usage();
    usage.cost_input = 0.00003;
    char *out = render_usage(&usage);
    EXPECT(contains(out, "in 600 · write 400 ~$0.0008"));
    EXPECT(!contains(out, "$0.0000"));
    free(out);
}

static void test_usage_without_rates_has_bare_counts(void)
{
    struct turn_usage usage = exact_usage();
    usage.cost_input = usage.cost_cache_write = usage.cost_output = -1;
    char *out = render_usage(&usage);
    EXPECT(contains(out, "3s · $0.0012 · in 600 · write 400 · out 50"));
    EXPECT(!contains(out, "~$"));
    free(out);
}

static char *render_provenance(struct turn_usage *usage, const char *provider, const char *model)
{
    struct item items[] = {
        {.kind = ITEM_TURN_BOUNDARY},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = (char *)"answer"},
        {.kind = ITEM_TURN_USAGE,
         .usage = usage,
         .provider = (char *)provider,
         .model = (char *)model},
    };
    return render_to_string(NULL, items, sizeof(items) / sizeof(items[0]));
}

static void test_provenance_falls_back_to_wire_identity(void)
{
    struct turn_usage usage = exact_usage();
    char *out = render_provenance(&usage, "openrouter", "anthropic/claude-sonnet-5");
    EXPECT(contains(out, ANSI_DIM "openrouter · anthropic/claude-sonnet-5" ANSI_RESET));
    EXPECT(!contains(out, "→"));
    EXPECT(!contains(out, " via "));
    free(out);
}

static void test_provenance_prefers_display_labels(void)
{
    struct turn_usage usage = exact_usage();
    usage.provenance.provider_label = (char *)"llama.cpp";
    usage.provenance.model_label = (char *)"qwen3-30b-a3b";
    char *out = render_provenance(&usage, "llamacpp", "/models/qwen3-30b-a3b.gguf");
    EXPECT(contains(out, "llama.cpp · qwen3-30b-a3b"));
    EXPECT(!contains(out, ".gguf"));
    free(out);
}

static void test_provenance_reports_effort_with_the_model(void)
{
    struct turn_usage usage = exact_usage();
    usage.provenance.effort = (char *)"high";
    char *out = render_provenance(&usage, "anthropic", "claude-sonnet-5");
    EXPECT(contains(out, "anthropic · claude-sonnet-5 · high"));
    free(out);
}

/* Without the arrow the route would trail the effort level and read as part of it. */
static void test_provenance_arrows_route_after_effort(void)
{
    struct turn_usage usage = exact_usage();
    usage.provenance.effort = (char *)"low";
    usage.provenance.route = (char *)"OpenAI";
    char *out = render_provenance(&usage, "openrouter", "openai/gpt-5-mini");
    EXPECT(contains(out, "openrouter · openai/gpt-5-mini · low → OpenAI"));
    free(out);
}

static void test_provenance_omits_effort_when_unset(void)
{
    struct turn_usage usage = exact_usage();
    char *out = render_provenance(&usage, "anthropic", "claude-sonnet-5");
    EXPECT(contains(out, ANSI_DIM "anthropic · claude-sonnet-5" ANSI_RESET));
    free(out);
}

static void test_provenance_reports_route_and_served_model(void)
{
    struct turn_usage usage = exact_usage();
    usage.provenance.served_model = (char *)"deepseek/deepseek-v4";
    usage.provenance.route = (char *)"Wafer";
    char *out = render_provenance(&usage, "openrouter", "openrouter/auto");
    EXPECT(contains(out, "openrouter · openrouter/auto → deepseek/deepseek-v4 via Wafer"));
    free(out);
}

/* The response id is recorded for post-hoc correlation, not shown alongside the conversation. */
static void test_provenance_omits_response_id(void)
{
    struct turn_usage usage = exact_usage();
    usage.provenance.response_id = (char *)"gen-1787062607-yRVup";
    char *out = render_provenance(&usage, "openrouter", "openai/gpt-4o-mini");
    EXPECT(!contains(out, "gen-1787062607"));
    free(out);
}

static void test_provenance_omitted_without_identity(void)
{
    struct turn_usage usage = exact_usage();
    char *out = render_provenance(&usage, NULL, NULL);
    /* Nothing follows the accounting line: it closes the turn directly. */
    EXPECT(contains(out, "out 50 ~$0.0002" ANSI_RESET "\n\n"));
    free(out);
}

static void test_reasoning_text_has_section_and_dimmed_lines(void)
{
    struct item items[] = {{
        .kind = ITEM_REASONING,
        .reasoning_text = (char *)"first\nsecond",
    }};
    char *out = render_to_string(NULL, items, 1);
    EXPECT(contains(out, "── reasoning ──"));
    EXPECT(contains(out, ANSI_DIM "first" ANSI_RESET "\n" ANSI_DIM "second" ANSI_RESET));
    free(out);
}

static void test_opaque_reasoning_shows_id_without_payload(void)
{
    struct item items[] = {{
        .kind = ITEM_REASONING,
        .reasoning_json = (char *)"{\"id\":\"rs_abc\",\"encrypted_content\":\"xxxx\"}",
    }};
    char *out = render_to_string(NULL, items, 1);
    EXPECT(contains(out, "[reasoning]"));
    EXPECT(contains(out, "rs_abc"));
    EXPECT(!contains(out, "xxxx"));
    free(out);
}

int main(void)
{
    test_banner_degrades_to_ascii_without_utf8();

    /* The rules and box drawing asserted below are the UTF-8 spelling, which the renderer emits
     * only when an LC_CTYPE to decode them is available. */
    locale_init_utf8();

    test_banner_box();
    test_turn_rules_count_boundary_markers();
    test_parallel_calls_render_paired_with_results();
    test_item_range_pairs_results_with_nonzero_offset();
    test_no_boundary_no_turn_rule();
    test_system_prompt();
    test_user_message_uses_section_rule_without_line_prefix();
    test_synthetic_user_messages_have_distinct_labels();
    test_user_multiline_raw();
    test_assistant_message();
    test_tool_call_pretty_prints_args_without_id();
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
    test_reasoning_text_has_section_and_dimmed_lines();
    test_opaque_reasoning_shows_id_without_payload();
    test_estimated_turn_usage_footer();
    test_exact_turn_usage_footer();
    test_overlapping_cache_usage_uses_precomputed_uncached_count();
    test_tiny_usage_cost_is_omitted();
    test_usage_without_rates_has_bare_counts();
    test_provenance_falls_back_to_wire_identity();
    test_provenance_prefers_display_labels();
    test_provenance_reports_effort_with_the_model();
    test_provenance_arrows_route_after_effort();
    test_provenance_omits_effort_when_unset();
    test_provenance_reports_route_and_served_model();
    test_provenance_omits_response_id();
    test_provenance_omitted_without_identity();
    T_REPORT();
}
