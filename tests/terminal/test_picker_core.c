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

/* Build a state over `n` all-enabled items, filtered 1:1, with the cursor
 * at the top. Callers flip individual items' `disabled` and set `sel`. */
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

/* The regression the snap fix targets: a PageUp landing inside a disabled
 * run must snap UP to the next enabled row, not bounce forward to where it
 * started. Enabled rows at 0..50 and 101; 51..100 disabled. */
static void test_page_up_across_disabled_block(void)
{
    struct nav_fixture f;
    nav_init(&f, 102, 20);
    for (size_t i = 51; i <= 100; i++)
        f.items[i].disabled = 1;
    f.s.sel = 101;

    picker_core_page_sel(&f.s, -1); /* target 91 (disabled) -> snap up to 50 */
    EXPECT(f.s.sel == 50);

    picker_core_page_sel(&f.s, -1); /* keeps moving, never stuck */
    EXPECT(f.s.sel == 40);
}

/* Symmetric case: a PageDown into a disabled run snaps DOWN. */
static void test_page_down_across_disabled_block(void)
{
    struct nav_fixture f;
    nav_init(&f, 102, 20);
    for (size_t i = 51; i <= 100; i++)
        f.items[i].disabled = 1;
    f.s.sel = 50;

    picker_core_page_sel(&f.s, +1); /* target 60 (disabled) -> snap down to 101 */
    EXPECT(f.s.sel == 101);
}

static void test_move_sel_skips_disabled(void)
{
    struct nav_fixture f;
    nav_init(&f, 5, 10);
    f.items[1].disabled = 1;
    f.items[2].disabled = 1;

    picker_core_move_sel(&f.s, +1);
    EXPECT(f.s.sel == 3); /* steps past 1 and 2 */
    picker_core_move_sel(&f.s, -1);
    EXPECT(f.s.sel == 0);
}

int main(void)
{
    test_empty_query_matches_all();
    test_substring_case_insensitive();
    test_all_terms_must_match();
    test_whitespace_handling();
    test_page_jumps_half_and_centers();
    test_page_clamps_at_ends();
    test_page_up_across_disabled_block();
    test_page_down_across_disabled_block();
    test_move_sel_skips_disabled();
    T_REPORT();
}
