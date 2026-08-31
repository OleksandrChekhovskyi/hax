/* SPDX-License-Identifier: MIT */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent_core.h"
#include "agent_dispatch.h"
#include "harness.h"
#include "history.h"
#include "provider.h"
#include "tool.h"
#include "xalloc.h"
#include "render/disp.h"
#include "render/render_ctx.h"
#include "system/locale.h"
#include "terminal/vt_resolve.h"

/* Render through the paged-history sink and resolver. Markdown stays disabled so assertions do
 * not depend on wrapping. Caller frees. */
static char *render(enum history_detail detail, const struct item *items, size_t n, int reasoning)
{
    char *raw = NULL;
    size_t raw_len = 0;
    FILE *mem = open_memstream(&raw, &raw_len);
    if (!mem) {
        perror("open_memstream");
        exit(1);
    }
    struct render_ctx render = {.disp = {.sink = mem, .committed_newlines = 2},
                                .show_reasoning = reasoning};
    history_render(&render, detail, items, n, 0);
    render_set_mode(&render, RENDER_IDLE);
    disp_commit_newlines(&render.disp);
    fclose(mem);

    char *out = NULL;
    size_t out_len = 0;
    FILE *settled = open_memstream(&out, &out_len);
    if (!settled) {
        perror("open_memstream");
        exit(1);
    }
    vt_resolve(raw, raw_len, settled);
    fclose(settled);
    free(raw);
    return out;
}

/* Drop SGR runs from settled rows. Needed wherever an assertion spans text
 * the renderer styled in pieces — a coalesced read line closes and reopens
 * dim around each appended name, so "a.h, b.h" is not a literal substring of
 * the output, and a check for "\n\n[read]" would never match (the tag is
 * preceded by its style escapes) whether or not the blank line is there.
 * Caller frees. */
static char *strip_sgr(const char *s)
{
    char *out = xmalloc(strlen(s) + 1);
    size_t w = 0;
    for (size_t i = 0; s[i];) {
        if (s[i] == 0x1b && s[i + 1] == '[') {
            size_t j = i + 2;
            while (s[j] && s[j] != 'm')
                j++;
            i = s[j] ? j + 1 : j;
            continue;
        }
        out[w++] = s[i++];
    }
    out[w] = '\0';
    return out;
}

/* One user prompt, one verbose bash call, its result, one answer — the
 * shape almost every turn has. The command is deliberately not one
 * bash_classify calls exploration, or the call would render collapsed (which
 * test_full_keeps_collapsed_calls_quiet covers separately). Strings are
 * literals: history_render only reads, and nothing here goes through
 * item_free. */
static struct item *sample_turn(size_t *n)
{
    static struct item items[4];
    memset(items, 0, sizeof(items));
    items[0].kind = ITEM_USER_MESSAGE;
    items[0].text = (char *)"count the sources";
    items[1].kind = ITEM_TOOL_CALL;
    items[1].call_id = (char *)"c1";
    items[1].tool_name = (char *)"bash";
    items[1].tool_arguments_json = (char *)"{\"command\":\"printf 'the count'\"}";
    items[2].kind = ITEM_TOOL_RESULT;
    items[2].call_id = (char *)"c1";
    items[2].output = (char *)"COUNT_OUTPUT_164\n";
    items[3].kind = ITEM_ASSISTANT_MESSAGE;
    items[3].text = (char *)"164 lines.";
    *n = 4;
    return items;
}

/* Brief is the inline resume replay: the call collapses to one line and
 * the output is left out entirely — the budget there is one screen. */
static void test_brief_omits_tool_output(void)
{
    size_t n;
    struct item *items = sample_turn(&n);
    char *out = render(HISTORY_BRIEF, items, n, 0);
    EXPECT(strstr(out, "count the sources") != NULL);
    EXPECT(strstr(out, "[bash]") != NULL);
    EXPECT(strstr(out, "the count") != NULL);
    EXPECT(strstr(out, "164 lines.") != NULL);
    EXPECT(strstr(out, "COUNT_OUTPUT_164") == NULL); /* the result body stays out */
    free(out);
}

/* Full is the paged view: the same call keeps its header and gets its
 * output back, rebuilt from the stored result. */
static void test_full_shows_tool_output(void)
{
    size_t n;
    struct item *items = sample_turn(&n);
    char *out = render(HISTORY_FULL, items, n, 0);
    EXPECT(strstr(out, "[bash]") != NULL);
    EXPECT(strstr(out, "the count") != NULL);
    /* The body sits inside a closed block: a single-row block ends with
     * the solo chevron, and the cursor ops that painted it (\r overprint,
     * erase-line) are resolved rather than leaked into the output. */
    const char *chevron = strstr(out, "\xE2\x80\xBA");
    EXPECT(chevron != NULL && strstr(chevron, "COUNT_OUTPUT_164") != NULL);
    EXPECT(strstr(out, "\r") == NULL);
    EXPECT(strstr(out, "\x1b[K") == NULL);
    free(out);
}

/* A model-only tail (a background launch note, an image-budget note) was
 * never displayed live, so the replay must not resurface it. */
static void test_hidden_tail_stays_hidden(void)
{
    size_t n;
    struct item *items = sample_turn(&n);
    items[2].output = (char *)"COUNT_OUTPUT_164\n"
                              "\n[finished during launch; no task created]";
    items[2].output_hidden_tail = strlen("\n[finished during launch; no task created]");
    char *out = render(HISTORY_FULL, items, n, 0);
    EXPECT(strstr(out, "COUNT_OUTPUT_164") != NULL);
    EXPECT(strstr(out, "finished during launch") == NULL);
    free(out);
}

/* A collapsed call showed no output live, so it must not gain any here —
 * `read` is collapsed preview, and its result can be a whole file. */
static void test_full_keeps_collapsed_calls_quiet(void)
{
    struct item items[2] = {0};
    items[0].kind = ITEM_TOOL_CALL;
    items[0].call_id = (char *)"c1";
    items[0].tool_name = (char *)"read";
    items[0].tool_arguments_json = (char *)"{\"path\":\"/etc/hostname\"}";
    items[1].kind = ITEM_TOOL_RESULT;
    items[1].call_id = (char *)"c1";
    items[1].output = (char *)"     1\tsecret-file-contents\n";

    char *out = render(HISTORY_FULL, items, 2, 0);
    EXPECT(strstr(out, "[read]") != NULL);
    /* Abbreviated to the basename, as the live breadcrumb is — the full path
     * belongs to the verbose header, which a collapsed call never gets. */
    EXPECT(strstr(out, "hostname") != NULL);
    EXPECT(strstr(out, "/etc/hostname") == NULL);
    EXPECT(strstr(out, "secret-file-contents") == NULL);
    free(out);
}

static void test_collapsed_cluster_spans_turns_tightly(void)
{
    struct item items[5] = {0};
    items[0].kind = ITEM_TOOL_CALL;
    items[0].call_id = (char *)"a";
    items[0].tool_name = (char *)"bash";
    items[0].tool_arguments_json = (char *)"{\"command\":\"ls src\"}";
    items[1].kind = ITEM_TURN_BOUNDARY;
    items[2].kind = ITEM_TOOL_CALL;
    items[2].call_id = (char *)"b";
    items[2].tool_name = (char *)"read";
    items[2].tool_arguments_json = (char *)"{\"path\":\"/tmp/aaa.h\"}";
    items[3].kind = ITEM_TURN_BOUNDARY;
    items[4].kind = ITEM_TOOL_CALL;
    items[4].call_id = (char *)"c";
    items[4].tool_name = (char *)"read";
    items[4].tool_arguments_json = (char *)"{\"path\":\"/tmp/bbb.h\",\"offset\":10,\"limit\":20}";

    char *out = render(HISTORY_FULL, items, 5, 0);
    char *plain = strip_sgr(out);
    EXPECT(strstr(plain, "/tmp/aaa.h") == NULL); /* basenames, not paths */
    /* Two reads share one line, and a requested range keeps its suffix. */
    EXPECT(strstr(plain, "aaa.h, bbb.h:10-29") != NULL);
    EXPECT(strstr(plain, "\n\n[read]") == NULL); /* nothing splits the cluster */
    free(plain);
    free(out);
}

/* An empty send after an interrupted turn appends CONTINUE_MARKER as a user
 * item so the model isn't handed a stop and a go at once. Nothing was typed
 * and the screen showed no prompt row, so replaying it as one invents a
 * message the user never sent. */
static void test_continue_marker_is_not_echoed(void)
{
    struct item items[5] = {0};
    items[0].kind = ITEM_USER_MESSAGE;
    items[0].text = (char *)"do the work";
    items[1].kind = ITEM_ASSISTANT_MESSAGE;
    items[1].text = (char *)INTERRUPT_MARKER;
    items[2].kind = ITEM_TURN_BOUNDARY;
    items[3].kind = ITEM_USER_MESSAGE;
    items[3].text = (char *)CONTINUE_MARKER;
    items[3].origin = ITEM_ORIGIN_CONTINUATION;
    items[4].kind = ITEM_ASSISTANT_MESSAGE;
    items[4].text = (char *)"picking it up";

    char *out = render(HISTORY_FULL, items, 5, 0);
    EXPECT(strstr(out, "do the work") != NULL);
    EXPECT(strstr(out, "picking it up") != NULL);
    EXPECT(strstr(out, CONTINUE_MARKER) == NULL);
    /* The interrupt marker it followed is still the user's cue. */
    EXPECT(strstr(out, INTERRUPT_MARKER) != NULL);
    free(out);

    /* The same text typed by hand is a message like any other: provenance is
     * the flag, not the bytes, so it gets its prompt row back. */
    items[3].origin = ITEM_ORIGIN_NONE;
    char *typed = render(HISTORY_FULL, items, 5, 0);
    EXPECT(strstr(typed, CONTINUE_MARKER) != NULL);
    free(typed);
}

static void test_undrawn_reasoning_still_breaks_text(void)
{
    struct item items[3] = {0};
    items[0].kind = ITEM_ASSISTANT_MESSAGE;
    items[0].text = (char *)"BEFORE_END";
    items[1].kind = ITEM_REASONING;
    items[1].reasoning_text = (char *)"thinking out loud";
    items[2].kind = ITEM_ASSISTANT_MESSAGE;
    items[2].text = (char *)"AFTER_START";

    char *hidden = render(HISTORY_FULL, items, 3, 0);
    EXPECT(strstr(hidden, "thinking out loud") == NULL);
    EXPECT(strstr(hidden, "BEFORE_ENDAFTER_START") == NULL); /* two answers, not one */
    EXPECT(strstr(hidden, "BEFORE_END") != NULL && strstr(hidden, "AFTER_START") != NULL);
    free(hidden);

    /* Opaque reasoning (Codex sends reasoning_json only) breaks the same way
     * with reasoning shown — there is no text to render either way. */
    items[1].reasoning_text = NULL;
    char *opaque = render(HISTORY_FULL, items, 3, 1);
    EXPECT(strstr(opaque, "BEFORE_ENDAFTER_START") == NULL);
    free(opaque);
}

/* The live view renders each reasoning item as its own block; replay must
 * not run them together into one paragraph. */
static void test_consecutive_reasoning_items_are_separate_blocks(void)
{
    struct item items[2] = {0};
    items[0].kind = ITEM_REASONING;
    items[0].reasoning_text = (char *)"FIRST_THOUGHT";
    items[1].kind = ITEM_REASONING;
    items[1].reasoning_text = (char *)"SECOND_THOUGHT";

    char *out = render(HISTORY_FULL, items, 2, 1);
    char *plain = strip_sgr(out);
    EXPECT(strstr(plain, "FIRST_THOUGHTSECOND_THOUGHT") == NULL);
    EXPECT(strstr(plain, "FIRST_THOUGHT\n\nSECOND_THOUGHT") != NULL);
    free(plain);
    free(out);
}

/* Results are paired by call_id, not by adjacency: a parallel batch is
 * stored as (C1, C2, R1, R2) and each call must still get its own body. */
static void test_batched_results_pair_by_id(void)
{
    struct item items[4] = {0};
    items[0].kind = ITEM_TOOL_CALL;
    items[0].call_id = (char *)"a";
    items[0].tool_name = (char *)"bash";
    items[0].tool_arguments_json = (char *)"{\"command\":\"first --run\"}";
    items[1].kind = ITEM_TOOL_CALL;
    items[1].call_id = (char *)"b";
    items[1].tool_name = (char *)"bash";
    items[1].tool_arguments_json = (char *)"{\"command\":\"second --run\"}";
    items[2].kind = ITEM_TOOL_RESULT;
    items[2].call_id = (char *)"a";
    items[2].output = (char *)"OUT_FIRST\n";
    items[3].kind = ITEM_TOOL_RESULT;
    items[3].call_id = (char *)"b";
    items[3].output = (char *)"OUT_SECOND\n";

    char *out = render(HISTORY_FULL, items, 4, 0);
    const char *first = strstr(out, "first --run");
    const char *out_first = strstr(out, "OUT_FIRST");
    const char *second = strstr(out, "second --run");
    const char *out_second = strstr(out, "OUT_SECOND");
    EXPECT(first && out_first && second && out_second);
    /* Each body sits under its own call, in order. */
    EXPECT(first < out_first && out_first < second && second < out_second);
    free(out);
}

/* Brief collapses every call onto a quiet line, including the tools that
 * render verbosely live — so the line still has to name what was touched.
 * `read` abbreviates to a basename (it reads as a list of files); everything
 * else keeps its configured display argument, which for edit/write is the path. */
static void test_brief_names_verbose_tool_args(void)
{
    struct item items[3] = {0};
    items[0].kind = ITEM_TOOL_CALL;
    items[0].call_id = (char *)"e";
    items[0].tool_name = (char *)"edit";
    items[0].tool_arguments_json = (char *)"{\"path\":\"/tmp/deep/edited.c\"}";
    items[1].kind = ITEM_TOOL_CALL;
    items[1].call_id = (char *)"w";
    items[1].tool_name = (char *)"write";
    items[1].tool_arguments_json = (char *)"{\"path\":\"/tmp/deep/written.c\",\"content\":\"x\"}";

    char *out = render(HISTORY_BRIEF, items, 2, 0);
    EXPECT(strstr(out, "[edit]") != NULL);
    EXPECT(strstr(out, "/tmp/deep/edited.c") != NULL);
    EXPECT(strstr(out, "[write]") != NULL);
    EXPECT(strstr(out, "/tmp/deep/written.c") != NULL);
    free(out);
}

/* A nameless call is a malformed provider response — replay it under a "?"
 * tag rather than crashing on the lookup, in either detail level (brief goes
 * through the quiet line, full through the verbose header). */
static void test_nameless_call_renders_in_both_modes(void)
{
    struct item items[2] = {0};
    items[0].kind = ITEM_TOOL_CALL;
    items[0].call_id = (char *)"n";
    items[0].tool_arguments_json = (char *)"{\"whatever\":\"NAMELESS_ARG\"}";
    items[1].kind = ITEM_TOOL_RESULT;
    items[1].call_id = (char *)"n";
    items[1].output = (char *)"NAMELESS_BODY\n";

    char *full = render(HISTORY_FULL, items, 2, 0);
    EXPECT(strstr(full, "[?]") != NULL);
    EXPECT(strstr(full, "NAMELESS_ARG") != NULL); /* generic JSON arg fallback */
    EXPECT(strstr(full, "NAMELESS_BODY") != NULL);
    free(full);

    char *brief = render(HISTORY_BRIEF, items, 2, 0);
    EXPECT(strstr(brief, "[?]") != NULL);
    free(brief);
}

/* Dispatch displays the preprocessed args, not the model's emission that
 * history stores: bash drops a redundant `cd <cwd> &&` prefix, so replaying
 * the original puts a prefix on the line that the screen never showed. The
 * call stays quiet either way — bash_classify counts `cd` as a neutral
 * prefix — which the hidden output asserts. */
static void test_replays_preprocessed_args(void)
{
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        T_SKIP("getcwd failed");
        return;
    }
    char *args = xasprintf("{\"command\":\"cd %s && ls src\"}", cwd);
    struct item items[2] = {0};
    items[0].kind = ITEM_TOOL_CALL;
    items[0].call_id = (char *)"b";
    items[0].tool_name = (char *)"bash";
    items[0].tool_arguments_json = args;
    items[1].kind = ITEM_TOOL_RESULT;
    items[1].call_id = (char *)"b";
    items[1].output = (char *)"LISTING_BODY\n";

    char *out = render(HISTORY_FULL, items, 2, 0);
    char *plain = strip_sgr(out);
    EXPECT(strstr(plain, "cd ") == NULL);           /* the stripped prefix stays gone */
    EXPECT(strstr(plain, "[bash] ls src") != NULL); /* quiet, as it was live */
    EXPECT(strstr(plain, "LISTING_BODY") == NULL);  /* exploration output stays hidden */
    free(plain);
    free(out);
    free(args);
}

/* Esc partway through a batch skips the pending calls, and live each one
 * renders a verbose header plus "[interrupted]" (dispatch_tool_skipped) —
 * quiet tools included. Collapsed to a breadcrumb the call would read as
 * having run and returned nothing, so the outcome has to survive replay. */
static void test_skipped_call_replays_its_outcome(void)
{
    struct item items[2] = {0};
    items[0].kind = ITEM_TOOL_CALL;
    items[0].call_id = (char *)"s";
    items[0].tool_name = (char *)"read";
    items[0].tool_arguments_json = (char *)"{\"path\":\"/etc/hostname\"}";
    items[1].kind = ITEM_TOOL_RESULT;
    items[1].call_id = (char *)"s";
    items[1].output = (char *)INTERRUPT_MARKER;
    items[1].origin = ITEM_ORIGIN_SKIPPED;

    for (int brief = 0; brief < 2; brief++) {
        char *out = render(brief ? HISTORY_BRIEF : HISTORY_FULL, items, 2, 0);
        EXPECT(strstr(out, "[read]") != NULL);
        EXPECT(strstr(out, INTERRUPT_MARKER) != NULL);
        /* The verbose header the skipped block used, not the quiet basename. */
        EXPECT(strstr(out, "/etc/hostname") != NULL);
        free(out);
    }
}

/* The outcome comes off the item, not the output text — a tool that ran and
 * happened to print those same bytes (`printf '[interrupted]'`, or a command
 * echoing the refusal message) is a call that ran, and replaying it as
 * skipped would have the view inventing an outcome. Here that means the quiet
 * breadcrumb a collapsed `read` earns, with its result still unshown. */
static void test_ran_call_printing_a_marker_is_not_undispatched(void)
{
    struct item items[2] = {0};
    items[0].kind = ITEM_TOOL_CALL;
    items[0].call_id = (char *)"r";
    items[0].tool_name = (char *)"read";
    items[0].tool_arguments_json = (char *)"{\"path\":\"/etc/hostname\"}";
    items[1].kind = ITEM_TOOL_RESULT;
    items[1].call_id = (char *)"r";
    items[1].output = (char *)INTERRUPT_MARKER; /* origin stays ITEM_ORIGIN_NONE */

    char *out = render(HISTORY_FULL, items, 2, 0);
    EXPECT(strstr(out, "[read]") != NULL);
    EXPECT(strstr(out, "hostname") != NULL);
    EXPECT(strstr(out, "/etc/hostname") == NULL); /* quiet line, not a header */
    EXPECT(strstr(out, INTERRUPT_MARKER) == NULL);
    free(out);

    /* Same for the refusal text out of a bash command that really ran. */
    struct item ran[2] = {0};
    ran[0].kind = ITEM_TOOL_CALL;
    ran[0].call_id = (char *)"b";
    ran[0].tool_name = (char *)"bash";
    ran[0].tool_arguments_json = (char *)"{\"command\":\"printf '%s' \\\"$msg\\\"\"}";
    ran[1].kind = ITEM_TOOL_RESULT;
    ran[1].call_id = (char *)"b";
    ran[1].output = (char *)REFUSED_RESULT;

    char *echoed = render(HISTORY_FULL, ran, 2, 0);
    EXPECT(strstr(echoed, REFUSED_MARKER) == NULL); /* not a refusal */
    EXPECT(strstr(echoed, REFUSED_RESULT) != NULL); /* just output */
    free(echoed);
}

/* Same for a --raw refusal: the user saw the reason, the model got an error it
 * could act on, and replay owes the user's version. */
static void test_refused_call_replays_the_refusal(void)
{
    struct item items[2] = {0};
    items[0].kind = ITEM_TOOL_CALL;
    items[0].call_id = (char *)"x";
    items[0].tool_name = (char *)"read";
    items[0].tool_arguments_json = (char *)"{\"path\":\"/etc/hostname\"}";
    items[1].kind = ITEM_TOOL_RESULT;
    items[1].call_id = (char *)"x";
    items[1].output = (char *)REFUSED_RESULT;
    items[1].origin = ITEM_ORIGIN_REFUSED;

    char *out = render(HISTORY_FULL, items, 2, 0);
    EXPECT(strstr(out, REFUSED_MARKER) != NULL);
    EXPECT(strstr(out, REFUSED_RESULT) == NULL); /* the model-facing text stays off screen */
    free(out);
}

/* An Esc that lands after the tools ran appends a synthetic assistant marker
 * *behind* the results (agent_session_mark_interrupt). It was never streamed,
 * so hoisting it into the streamed phase would put "[interrupted]" above the
 * block it interrupted. */
static void test_post_dispatch_interrupt_marker_stays_below_tools(void)
{
    struct item items[3] = {0};
    items[0].kind = ITEM_TOOL_CALL;
    items[0].call_id = (char *)"m";
    items[0].tool_name = (char *)"bash";
    items[0].tool_arguments_json = (char *)"{\"command\":\"mutate --now\"}";
    items[1].kind = ITEM_TOOL_RESULT;
    items[1].call_id = (char *)"m";
    items[1].output = (char *)"MUTATE_BODY\n";
    items[2].kind = ITEM_ASSISTANT_MESSAGE;
    items[2].text = (char *)"[interrupted]";

    char *out = render(HISTORY_FULL, items, 3, 0);
    const char *body = strstr(out, "MUTATE_BODY");
    const char *marker = strstr(out, "[interrupted]");
    EXPECT(body != NULL && marker != NULL);
    EXPECT(body < marker);
    free(out);
}

/* A call_id only has to be unique within its response, and local
 * OpenAI-compatible backends reuse `call_0` every turn. Pairing must stay
 * inside the turn, or an orphan adopts the next turn's result and its body
 * shows up twice. */
static void test_repeated_call_ids_do_not_pair_across_turns(void)
{
    struct item items[4] = {0};
    items[0].kind = ITEM_TOOL_CALL;
    items[0].call_id = (char *)"call_0";
    items[0].tool_name = (char *)"bash";
    items[0].tool_arguments_json = (char *)"{\"command\":\"orphaned --call\"}";
    items[1].kind = ITEM_TURN_BOUNDARY;
    items[2].kind = ITEM_TOOL_CALL;
    items[2].call_id = (char *)"call_0";
    items[2].tool_name = (char *)"bash";
    items[2].tool_arguments_json = (char *)"{\"command\":\"later --call\"}";
    items[3].kind = ITEM_TOOL_RESULT;
    items[3].call_id = (char *)"call_0";
    items[3].output = (char *)"LATER_BODY\n";

    char *out = render(HISTORY_FULL, items, 4, 0);
    const char *first = strstr(out, "orphaned --call");
    const char *second = strstr(out, "later --call");
    const char *body = strstr(out, "LATER_BODY");
    EXPECT(first != NULL && second != NULL && body != NULL);
    /* The body sits under the call that produced it, and only there. */
    EXPECT(first < second && second < body);
    EXPECT(strstr(body + 1, "LATER_BODY") == NULL);
    free(out);
}

/* An orphan call (batch cut short by an interrupt) has no result to pair
 * with and must still render its header rather than crash. */
static void test_orphan_call_renders_header(void)
{
    struct item items[1] = {0};
    items[0].kind = ITEM_TOOL_CALL;
    items[0].call_id = (char *)"gone";
    items[0].tool_name = (char *)"bash";
    items[0].tool_arguments_json = (char *)"{\"command\":\"orphaned --call\"}";

    char *out = render(HISTORY_FULL, items, 1, 0);
    EXPECT(strstr(out, "orphaned --call") != NULL);
    free(out);
}

/* A compaction seed is synthetic: mark the boundary instead of echoing the
 * summary as something the user typed. */
static void test_compaction_seed_renders_as_marker(void)
{
    struct item items[1] = {0};
    items[0].kind = ITEM_USER_MESSAGE;
    items[0].text = (char *)"SUMMARY OF EARLIER WORK";
    items[0].origin = ITEM_ORIGIN_COMPACT_SEED;

    char *out = render(HISTORY_FULL, items, 1, 0);
    EXPECT(strstr(out, "conversation compacted") != NULL);
    EXPECT(strstr(out, "SUMMARY OF EARLIER WORK") == NULL);
    free(out);
}

/* A session resumed on a compaction seed must not print its first real prompt
 * onto the marker's row. */
static void test_marker_separates_from_next_block(void)
{
    struct item items[2] = {0};
    items[0].kind = ITEM_USER_MESSAGE;
    items[0].text = (char *)"SUMMARY OF EARLIER WORK";
    items[0].origin = ITEM_ORIGIN_COMPACT_SEED;
    items[1].kind = ITEM_USER_MESSAGE;
    items[1].text = (char *)"FIRST_REAL_PROMPT";

    char *out = render(HISTORY_FULL, items, 2, 0);
    char *plain = strip_sgr(out);
    EXPECT(strstr(plain, "conversation compacted ──\n\n") != NULL);
    EXPECT(strstr(plain, "compacted ──\xE2\x96\x8C") == NULL);
    free(plain);
    free(out);

    items[0].kind = ITEM_ASSISTANT_MESSAGE;
    items[0].text = (char *)"[interrupted]";
    items[0].origin = ITEM_ORIGIN_INTERRUPTED;

    char *after_interrupt = render(HISTORY_FULL, items, 2, 0);
    char *iplain = strip_sgr(after_interrupt);
    EXPECT(strstr(iplain, "[interrupted]\n\n") != NULL);
    free(iplain);
    free(after_interrupt);
}

/* Reasoning follows the live setting, so turning it on shows reasoning for
 * turns that were displayed without it. */
static void test_reasoning_follows_setting(void)
{
    struct item items[1] = {0};
    items[0].kind = ITEM_REASONING;
    items[0].reasoning_text = (char *)"thinking out loud";

    char *off = render(HISTORY_FULL, items, 1, 0);
    EXPECT(strstr(off, "thinking out loud") == NULL);
    free(off);

    char *on = render(HISTORY_FULL, items, 1, 1);
    EXPECT(strstr(on, "thinking out loud") != NULL);
    free(on);
}

/* The stored "\n[interrupted]" tail renders as the dim out-of-band marker
 * with the partial response above it, not as answer text. */
static void test_interrupt_marker_split_out(void)
{
    struct item items[1] = {0};
    items[0].kind = ITEM_ASSISTANT_MESSAGE;
    items[0].text = (char *)"partial answer\n[interrupted]";
    items[0].origin = ITEM_ORIGIN_INTERRUPTED;

    char *out = render(HISTORY_FULL, items, 1, 0);
    const char *partial = strstr(out, "partial answer");
    const char *marker = strstr(out, "[interrupted]");
    EXPECT(partial && marker && partial < marker);
    /* Dim out-of-band block, not part of the answer's own paragraph. */
    char *plain = strip_sgr(out);
    EXPECT(strstr(plain, "partial answer\n\n[interrupted]") != NULL);
    free(plain);
    free(out);

    /* An answer the model really ended on that line is the model's words: no
     * stamp, no split, no dim notice — the marker is provenance, not a
     * spelling. */
    items[0].origin = ITEM_ORIGIN_NONE;
    char *genuine = render(HISTORY_FULL, items, 1, 0);
    char *gplain = strip_sgr(genuine);
    EXPECT(strstr(gplain, "partial answer\n[interrupted]") != NULL);
    EXPECT(strstr(gplain, "partial answer\n\n[interrupted]") == NULL);
    free(gplain);
    free(genuine);
}

/* A write/edit whose stored result is a diff renders through the diff
 * path; an empty one is the no-op marker, as it was live. */
static void test_diff_result_and_no_op_marker(void)
{
    struct item items[2] = {0};
    items[0].kind = ITEM_TOOL_CALL;
    items[0].call_id = (char *)"w";
    items[0].tool_name = (char *)"write";
    items[0].tool_arguments_json = (char *)"{\"path\":\"/tmp/x\",\"content\":\"b\\n\"}";
    items[1].kind = ITEM_TOOL_RESULT;
    items[1].call_id = (char *)"w";
    items[1].output = (char *)"--- /tmp/x\n+++ /tmp/x\n@@ -1 +1 @@\n-a\n+b\n";

    char *out = render(HISTORY_FULL, items, 2, 0);
    EXPECT(strstr(out, "@@ -1 +1 @@") != NULL);
    EXPECT(strstr(out, "+b") != NULL);
    free(out);

    items[1].output = (char *)"";
    char *noop = render(HISTORY_FULL, items, 2, 0);
    EXPECT(strstr(noop, "(no changes)") != NULL);
    free(noop);
}

/* Render a slice, the way the inline replay does: it anchors on the last
 * user message rather than the start of the conversation. */
static char *render_from(enum history_detail detail, const struct item *items, size_t n,
                         size_t start)
{
    char *raw = NULL;
    size_t raw_len = 0;
    FILE *mem = open_memstream(&raw, &raw_len);
    if (!mem) {
        perror("open_memstream");
        exit(1);
    }
    struct render_ctx render = {.disp = {.sink = mem, .committed_newlines = 2}};
    history_render(&render, detail, items, n, start);
    render_set_mode(&render, RENDER_IDLE);
    disp_commit_newlines(&render.disp);
    fclose(mem);

    char *out = NULL;
    size_t out_len = 0;
    FILE *settled = open_memstream(&out, &out_len);
    if (!settled) {
        perror("open_memstream");
        exit(1);
    }
    vt_resolve(raw, raw_len, settled);
    fclose(settled);
    free(raw);
    return out;
}

static void test_start_index_skips_earlier_items(void)
{
    struct item items[3] = {0};
    items[0].kind = ITEM_USER_MESSAGE;
    items[0].text = (char *)"OLD_PROMPT";
    items[1].kind = ITEM_USER_MESSAGE;
    items[1].text = (char *)"NEW_PROMPT";
    items[2].kind = ITEM_ASSISTANT_MESSAGE;
    items[2].text = (char *)"NEW_ANSWER";

    char *out = render_from(HISTORY_BRIEF, items, 3, 1);
    EXPECT(strstr(out, "OLD_PROMPT") == NULL);
    EXPECT(strstr(out, "NEW_PROMPT") != NULL);
    EXPECT(strstr(out, "NEW_ANSWER") != NULL);
    free(out);
}

/* Nothing to render is not a special case for the caller to guard: an empty
 * vector and an out-of-range start both produce no rows and no crash. */
static void test_empty_and_out_of_range_slices(void)
{
    struct item items[1] = {0};
    items[0].kind = ITEM_ASSISTANT_MESSAGE;
    items[0].text = (char *)"only item";

    char *empty = render_from(HISTORY_FULL, items, 0, 0);
    EXPECT_STR_EQ(empty, "");
    free(empty);

    char *past_end = render_from(HISTORY_FULL, items, 1, 5);
    EXPECT_STR_EQ(past_end, "");
    free(past_end);
}

/* Turn plumbing is deliberately not part of this view — boundaries and
 * usage footers belong to the transcript and /session. Pinned so a later
 * change doesn't start leaking per-turn accounting into the conversation. */
static void test_turn_plumbing_not_rendered(void)
{
    struct turn_usage usage = {0};
    usage.usage.output_tokens = 1234;
    struct item items[3] = {0};
    items[0].kind = ITEM_TURN_BOUNDARY;
    items[1].kind = ITEM_TURN_USAGE;
    items[1].usage = &usage;
    items[2].kind = ITEM_ASSISTANT_MESSAGE;
    items[2].text = (char *)"just the answer";

    char *out = render_from(HISTORY_FULL, items, 3, 0);
    EXPECT(strstr(out, "just the answer") != NULL);
    EXPECT(strstr(out, "1234") == NULL);
    free(out);
}

/* A tool the registry doesn't know still renders its header and body — the
 * preview just falls back to the head-only mode with no diff coloring. */
static void test_unknown_tool_renders_generically(void)
{
    struct item items[2] = {0};
    items[0].kind = ITEM_TOOL_CALL;
    items[0].call_id = (char *)"u";
    items[0].tool_name = (char *)"no_such_tool";
    items[0].tool_arguments_json = (char *)"{\"whatever\":\"ARG_VALUE\"}";
    items[1].kind = ITEM_TOOL_RESULT;
    items[1].call_id = (char *)"u";
    items[1].output = (char *)"UNKNOWN_TOOL_BODY\n";

    char *out = render_from(HISTORY_FULL, items, 2, 0);
    EXPECT(strstr(out, "no_such_tool") != NULL);
    EXPECT(strstr(out, "ARG_VALUE") != NULL); /* generic JSON arg fallback */
    EXPECT(strstr(out, "UNKNOWN_TOOL_BODY") != NULL);
    free(out);
}

/* Consecutive same-kind items concatenate into one block, as they do live
 * when a provider splits an answer across items. */
static void test_consecutive_assistant_items_join(void)
{
    struct item items[2] = {0};
    items[0].kind = ITEM_ASSISTANT_MESSAGE;
    items[0].text = (char *)"first half ";
    items[1].kind = ITEM_ASSISTANT_MESSAGE;
    items[1].text = (char *)"second half";

    char *out = render_from(HISTORY_FULL, items, 2, 0);
    EXPECT_STR_EQ(out, "first half second half\n");
    free(out);
}

/* A new-file `write` streams the content for display and returns only a
 * "created ..." summary (tools/write.c), stamping ITEM_ORIGIN_SUMMARIZED to
 * say so. Replaying the result alone would put the summary where the content
 * was; the content is still in the call arguments, so the view rebuilds the
 * preview the user actually saw. */
static struct item *new_file_write(const char *content_json, size_t *n)
{
    static struct item items[2];
    memset(items, 0, sizeof(items));
    items[0].kind = ITEM_TOOL_CALL;
    items[0].call_id = (char *)"w";
    items[0].tool_name = (char *)"write";
    items[0].tool_arguments_json = (char *)content_json;
    items[1].kind = ITEM_TOOL_RESULT;
    items[1].call_id = (char *)"w";
    items[1].output = (char *)"created /tmp/x (2 lines, 18 bytes)";
    items[1].origin = ITEM_ORIGIN_SUMMARIZED;
    *n = 2;
    return items;
}

static void test_new_file_write_replays_content(void)
{
    size_t n;
    struct item *items =
        new_file_write("{\"path\":\"/tmp/x\",\"content\":\"WROTE_ONE\\nWROTE_TWO\\n\"}", &n);
    char *out = render(HISTORY_FULL, items, n, 0);
    EXPECT(strstr(out, "WROTE_ONE") != NULL);
    EXPECT(strstr(out, "WROTE_TWO") != NULL);
    /* The summary was model-facing only; the body stands in for it. */
    EXPECT(strstr(out, "created /tmp/x") == NULL);
    free(out);
}

/* Content that renders no rows (blank, whitespace, control-only) would leave
 * a bare header, so the summary comes back as the block body — the same
 * fallback dispatch applies live, on the same actual row count. */
static void test_new_file_write_falls_back_to_summary(void)
{
    size_t n;
    struct item *items = new_file_write("{\"path\":\"/tmp/x\",\"content\":\"   \\n\"}", &n);
    char *blank = render(HISTORY_FULL, items, n, 0);
    EXPECT(strstr(blank, "created /tmp/x") != NULL);
    free(blank);

    items = new_file_write("{\"path\":\"/tmp/x\",\"content\":\"\"}", &n);
    char *empty = render(HISTORY_FULL, items, n, 0);
    EXPECT(strstr(empty, "created /tmp/x") != NULL);
    free(empty);
}

/* A failed write returns an error message, which is exactly what the user saw,
 * and stamps no provenance — nothing was streamed. Replaying the content there
 * would show the file the model asked for as though the write had succeeded. */
static void test_failed_write_shows_error_not_content(void)
{
    struct item items[2] = {0};
    items[0].kind = ITEM_TOOL_CALL;
    items[0].call_id = (char *)"w";
    items[0].tool_name = (char *)"write";
    items[0].tool_arguments_json = (char *)"{\"path\":\"/tmp\",\"content\":\"WROTE_ONE\\n\"}";
    items[1].kind = ITEM_TOOL_RESULT;
    items[1].call_id = (char *)"w";
    items[1].output = (char *)"/tmp exists but is not a regular file";

    char *out = render(HISTORY_FULL, items, 2, 0);
    EXPECT(strstr(out, "exists but is not a regular file") != NULL);
    EXPECT(strstr(out, "WROTE_ONE") == NULL);
    free(out);

    /* Same for an argument-validation failure, which never reaches the file
     * system at all. */
    items[1].output = (char *)"invalid arguments: unexpected token";
    char *bad_args = render(HISTORY_FULL, items, 2, 0);
    EXPECT(strstr(bad_args, "invalid arguments") != NULL);
    EXPECT(strstr(bad_args, "WROTE_ONE") == NULL);
    free(bad_args);

    /* And when the failure's own text opens the way a success summary does:
     * fs_write_with_diff prefixes the path, so a write to a blocked path named
     * "created blocked" reports "created blocked exists but is not a regular
     * file". Provenance, not the prefix, is what says the write happened. */
    items[0].tool_arguments_json =
        (char *)"{\"path\":\"created blocked\",\"content\":\"WROTE_ONE\\n\"}";
    items[1].output = (char *)"created blocked exists but is not a regular file";
    char *lookalike = render(HISTORY_FULL, items, 2, 0);
    EXPECT(strstr(lookalike, "exists but is not a regular file") != NULL);
    EXPECT(strstr(lookalike, "WROTE_ONE") == NULL);
    free(lookalike);
}

/* An overwrite returns the diff and must keep replaying it — the streamed
 * argument is only consulted for the created-summary shape, or a rewrite
 * would show the new content in place of the change. */
static void test_overwrite_still_replays_diff(void)
{
    struct item items[2] = {0};
    items[0].kind = ITEM_TOOL_CALL;
    items[0].call_id = (char *)"w";
    items[0].tool_name = (char *)"write";
    items[0].tool_arguments_json = (char *)"{\"path\":\"/tmp/x\",\"content\":\"WROTE_NEW\\n\"}";
    items[1].kind = ITEM_TOOL_RESULT;
    items[1].call_id = (char *)"w";
    items[1].output = (char *)"--- /tmp/x\n+++ /tmp/x\n@@ -1 +1 @@\n-WAS_OLD\n+WROTE_NEW\n";

    char *out = render(HISTORY_FULL, items, 2, 0);
    EXPECT(strstr(out, "@@ -1 +1 @@") != NULL);
    EXPECT(strstr(out, "-WAS_OLD") != NULL);
    free(out);
}

/* Tool calls are dispatched after the stream completes, so text a turn
 * emitted *after* a call still appeared above that call's block live. Stored
 * items keep stream order, so replaying them positionally would hoist the
 * block above text that preceded it on screen. */
static void test_text_after_tool_call_renders_above_it(void)
{
    struct item items[4] = {0};
    items[0].kind = ITEM_ASSISTANT_MESSAGE;
    items[0].text = (char *)"BEFORE_CALL";
    items[1].kind = ITEM_TOOL_CALL;
    items[1].call_id = (char *)"c";
    items[1].tool_name = (char *)"bash";
    items[1].tool_arguments_json = (char *)"{\"command\":\"printf x --run\"}";
    items[2].kind = ITEM_ASSISTANT_MESSAGE;
    items[2].text = (char *)"AFTER_CALL";
    items[3].kind = ITEM_TOOL_RESULT;
    items[3].call_id = (char *)"c";
    items[3].output = (char *)"TOOL_BODY\n";

    char *out = render(HISTORY_FULL, items, 4, 0);
    const char *before = strstr(out, "BEFORE_CALL");
    const char *after = strstr(out, "AFTER_CALL");
    const char *tool = strstr(out, "printf x --run");
    EXPECT(before && after && tool);
    /* Both texts above the call, in stream order. */
    EXPECT(before < after && after < tool);
    /* And a tool call between them was a block break live, so they must not
     * be glued into one paragraph. */
    EXPECT(strstr(out, "BEFORE_CALLAFTER_CALL") == NULL);
    free(out);
}

/* Each turn is its own stream, so a turn ending in text and the next opening
 * with text stay separate paragraphs — merging them would form a sentence
 * neither turn produced. */
static void test_turns_do_not_merge_text(void)
{
    struct item items[3] = {0};
    items[0].kind = ITEM_ASSISTANT_MESSAGE;
    items[0].text = (char *)"end of turn one.";
    items[1].kind = ITEM_TURN_BOUNDARY;
    items[2].kind = ITEM_ASSISTANT_MESSAGE;
    items[2].text = (char *)"Start of turn two.";

    char *out = render(HISTORY_FULL, items, 3, 0);
    EXPECT(strstr(out, "end of turn one.Start") == NULL);
    EXPECT(strstr(out, "end of turn one.") != NULL);
    EXPECT(strstr(out, "Start of turn two.") != NULL);
    free(out);
}

/* ---------- collapsed-preview selection ---------- */

static struct item collapsed_probe(const char *tool_name, const char *args_json)
{
    struct item call = {0};
    call.kind = ITEM_TOOL_CALL;
    call.tool_name = (char *)tool_name;
    call.tool_arguments_json = (char *)args_json;
    return call;
}

static void test_collapsed_by_static_mode(void)
{
    struct item call = collapsed_probe("read", "{\"path\":\"/etc/hostname\"}");
    EXPECT(tool_call_preview_mode(&call) == TOOL_PREVIEW_COLLAPSED);
}

static void test_collapsed_by_selector(void)
{
    struct item explore = collapsed_probe("bash", "{\"command\":\"ls src\"}");
    EXPECT(tool_call_preview_mode(&explore) == TOOL_PREVIEW_COLLAPSED);
    struct item mutate = collapsed_probe("bash", "{\"command\":\"rm -rf build\"}");
    EXPECT(tool_call_preview_mode(&mutate) == TOOL_PREVIEW_HEAD_TAIL);
}

static void test_verbose_tools_are_not_collapsed(void)
{
    struct item write_call = collapsed_probe("write", "{\"path\":\"/tmp/x\",\"content\":\"y\"}");
    EXPECT(tool_call_preview_mode(&write_call) == TOOL_PREVIEW_HEAD);
    struct item edit_call = collapsed_probe("edit", "{\"path\":\"/tmp/x\"}");
    EXPECT(tool_call_preview_mode(&edit_call) == TOOL_PREVIEW_HEAD);
}

static void test_unknown_and_nameless_calls(void)
{
    struct item unknown = collapsed_probe("no_such_tool", "{}");
    EXPECT(tool_call_preview_mode(&unknown) == TOOL_PREVIEW_HEAD);
    struct item nameless = collapsed_probe(NULL, NULL);
    EXPECT(tool_call_preview_mode(&nameless) == TOOL_PREVIEW_HEAD);
    struct item argless = collapsed_probe("bash", NULL);
    EXPECT(tool_call_preview_mode(&argless) == TOOL_PREVIEW_HEAD_TAIL);
}

int main(void)
{
    locale_init_utf8();
    test_brief_omits_tool_output();
    test_full_shows_tool_output();
    test_hidden_tail_stays_hidden();
    test_full_keeps_collapsed_calls_quiet();
    test_collapsed_cluster_spans_turns_tightly();
    test_brief_names_verbose_tool_args();
    test_nameless_call_renders_in_both_modes();
    test_replays_preprocessed_args();
    test_skipped_call_replays_its_outcome();
    test_ran_call_printing_a_marker_is_not_undispatched();
    test_refused_call_replays_the_refusal();
    test_post_dispatch_interrupt_marker_stays_below_tools();
    test_continue_marker_is_not_echoed();
    test_undrawn_reasoning_still_breaks_text();
    test_consecutive_reasoning_items_are_separate_blocks();
    test_batched_results_pair_by_id();
    test_repeated_call_ids_do_not_pair_across_turns();
    test_orphan_call_renders_header();
    test_compaction_seed_renders_as_marker();
    test_marker_separates_from_next_block();
    test_reasoning_follows_setting();
    test_interrupt_marker_split_out();
    test_diff_result_and_no_op_marker();
    test_start_index_skips_earlier_items();
    test_empty_and_out_of_range_slices();
    test_turn_plumbing_not_rendered();
    test_unknown_tool_renders_generically();
    test_consecutive_assistant_items_join();
    test_new_file_write_replays_content();
    test_new_file_write_falls_back_to_summary();
    test_failed_write_shows_error_not_content();
    test_overwrite_still_replays_diff();
    test_text_after_tool_call_renders_above_it();
    test_turns_do_not_merge_text();
    test_collapsed_by_static_mode();
    test_collapsed_by_selector();
    test_verbose_tools_are_not_collapsed();
    test_unknown_and_nameless_calls();
    T_REPORT();
}
