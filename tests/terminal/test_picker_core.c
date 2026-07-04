/* SPDX-License-Identifier: MIT */
#include <string.h>

#include "harness.h"
#include "terminal/picker_core.h"

/* picker_run itself needs a tty and can't be driven headless; picker_core
 * holds the pure filter predicate and selection/scroll navigation, which
 * are the testable pieces. The navigation tests build a picker_state over a
 * synthetic item list (filtered[] = identity) and drive the cursor
 * directly. */

static void test_empty_query_matches_all(void)
{
    EXPECT(picker_core_match("anything", ""));
    EXPECT(picker_core_match("anything", NULL));
    EXPECT(picker_core_match("", ""));
    /* A NULL label matches only the empty query. */
    EXPECT(picker_core_match(NULL, ""));
    EXPECT(!picker_core_match(NULL, "x"));
}

static void test_substring_case_insensitive(void)
{
    EXPECT(picker_core_match("anthropic/claude-opus-4", "opus"));
    EXPECT(picker_core_match("anthropic/claude-opus-4", "OPUS"));
    EXPECT(picker_core_match("anthropic/claude-opus-4", "Claude"));
    EXPECT(picker_core_match("anthropic/claude-opus-4", "4"));
    EXPECT(!picker_core_match("anthropic/claude-opus-4", "gpt"));
}

static void test_all_terms_must_match(void)
{
    /* Space-separated terms are AND-ed, order-independent. */
    EXPECT(picker_core_match("anthropic/claude-opus-4", "anthropic opus"));
    EXPECT(picker_core_match("anthropic/claude-opus-4", "opus anthropic"));
    EXPECT(!picker_core_match("anthropic/claude-opus-4", "anthropic gpt"));
}

static void test_whitespace_handling(void)
{
    /* Leading / trailing / repeated spaces yield no spurious empty terms. */
    EXPECT(picker_core_match("hello world", "  hello   world  "));
    EXPECT(picker_core_match("hello world", "   "));
    /* The query's spaces are term separators, not literal — so a single
     * token that isn't itself present fails even if its pieces appear. */
    EXPECT(!picker_core_match("hello world", "llowo"));
}

/* ---- navigation ---- */

struct nav_fixture {
    struct picker_item items[256];
    struct picker_opts opts;
    size_t filtered[256];
    struct picker_state s;
};

/* Build a state over `n` items, filtered 1:1, with the cursor at the top.
 * Callers set `sel` to start elsewhere. */
static void nav_init(struct nav_fixture *f, size_t n, int viewport)
{
    memset(f, 0, sizeof *f);
    for (size_t i = 0; i < n; i++) {
        f->items[i].label = "x";
        f->filtered[i] = i;
    }
    f->opts.items = f->items;
    f->opts.n = n;
    f->s.opts = &f->opts;
    f->s.filtered = f->filtered;
    f->s.n_filtered = n;
    f->s.viewport = viewport;
}

static void test_page_jumps_half_and_centers(void)
{
    struct nav_fixture f;
    nav_init(&f, 100, 20);

    picker_core_page_sel(&f.s, +1);
    EXPECT(f.s.sel == 10); /* half of a 20-row viewport */
    EXPECT(f.s.top == 0);  /* centered, clamped at the top */

    picker_core_page_sel(&f.s, +1);
    EXPECT(f.s.sel == 20);
    EXPECT(f.s.top == 10); /* sel centered: 20 - viewport/2 */
}

static void test_page_clamps_at_ends(void)
{
    struct nav_fixture f;
    nav_init(&f, 100, 20);
    f.s.sel = 95;

    picker_core_page_sel(&f.s, +1);
    EXPECT(f.s.sel == 99); /* clamps to the last row */
    EXPECT(f.s.top == 80); /* window bottom-anchored: 100 - 20 */

    picker_core_page_sel(&f.s, -1);
    EXPECT(f.s.sel == 89); /* 99 - viewport/2 */
    EXPECT(f.s.top == 79); /* re-centered: 89 - viewport/2 */
}

static void test_move_sel_steps_and_clamps(void)
{
    struct nav_fixture f;
    nav_init(&f, 3, 10);

    picker_core_move_sel(&f.s, -1);
    EXPECT(f.s.sel == 0); /* clamped at the top */
    picker_core_move_sel(&f.s, +1);
    EXPECT(f.s.sel == 1);
    picker_core_move_sel(&f.s, +1);
    picker_core_move_sel(&f.s, +1);
    EXPECT(f.s.sel == 2); /* clamped at the bottom */
}

/* Opening on a caller-provided initial row: the selection lands on the
 * item and the window centers around it instead of anchoring at the top. */
static void test_select_item_centers(void)
{
    struct nav_fixture f;
    nav_init(&f, 100, 20);

    picker_core_select_item(&f.s, 50);
    EXPECT(f.s.sel == 50);
    EXPECT(f.s.top == 40); /* centered: 50 - viewport/2 */

    /* An item index outside the filter leaves the selection untouched. */
    picker_core_select_item(&f.s, 500);
    EXPECT(f.s.sel == 50);
}

/* A query edit must reset the selection to the first match — clamping the
 * old offset into the shrunken list would make Enter grab an arbitrary
 * match (e.g. cursor opened deep on a "current" row, then a typed query
 * matching two items would land on the second). */
static void test_recompute_resets_selection(void)
{
    struct nav_fixture f;
    nav_init(&f, 8, 10);
    f.items[5].label = "openai";
    f.items[6].label = "openai-compatible";
    f.items[7].label = "openrouter";
    f.s.sel = 7; /* opened with the cursor on a deep row */

    buf_init(&f.s.query);
    buf_append_str(&f.s.query, "openai");
    picker_core_recompute(&f.s);
    EXPECT(f.s.n_filtered == 2);
    EXPECT(f.s.sel == 0); /* first match, not the clamped old offset */
    EXPECT(f.s.top == 0);
    buf_free(&f.s.query);
}

int main(void)
{
    test_empty_query_matches_all();
    test_substring_case_insensitive();
    test_all_terms_must_match();
    test_whitespace_handling();
    test_page_jumps_half_and_centers();
    test_page_clamps_at_ends();
    test_move_sel_steps_and_clamps();
    test_select_item_centers();
    test_recompute_resets_selection();
    T_REPORT();
}
