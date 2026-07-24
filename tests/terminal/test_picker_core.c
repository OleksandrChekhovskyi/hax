/* SPDX-License-Identifier: MIT */
#include <string.h>

#include "harness.h"
#include "util.h"
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

/* Row text is not the picker's own: a model description can be written by a
 * third party and relayed through a provider catalog. The frame emitter
 * passes escape sequences through at zero width so the picker's own colors
 * survive, so anything reaching it must already be stripped of them. */
static void test_sanitize_strips_escapes(void)
{
    struct buf b;
    buf_init(&b);
    /* A CSI sequence that would otherwise clear the screen and reposition
     * the cursor: every byte of it must land as a visible placeholder. */
    const char *evil = "safe\x1b[2J\x1b[Hgone";
    picker_core_append_sanitized(&b, evil, strlen(evil));
    buf_append(&b, "", 1);
    EXPECT(strchr(b.data, 0x1b) == NULL);
    EXPECT_STR_EQ(b.data, "safe?[2J?[Hgone");
    buf_free(&b);
}

static void test_sanitize_strips_c0_and_keeps_utf8(void)
{
    struct buf b;
    buf_init(&b);
    /* Bare CR and BEL are display hazards. Control filtering is locale
     * independent; the multi-byte passthrough below is not, since cell
     * measurement goes through wcwidth. */
    const char *s = "a\rb\ac";
    picker_core_append_sanitized(&b, s, strlen(s));
    buf_append(&b, "", 1);
    EXPECT_STR_EQ(b.data, "a?b?c");
    buf_free(&b);

    if (!locale_have_utf8())
        return; /* no UTF-8 LC_CTYPE here — the ASCII checks above still ran */
    buf_init(&b);
    /* Multi-byte text must survive intact rather than being mangled into
     * one placeholder per byte. */
    picker_core_append_sanitized(&b, "c – ü", strlen("c – ü"));
    buf_append(&b, "", 1);
    EXPECT_STR_EQ(b.data, "c – ü");
    buf_free(&b);
}

static void test_sanitize_counted_not_terminated(void)
{
    /* Takes a length, not a NUL-terminator — the footer hands it one
     * wrapped slice of a longer description at a time. */
    struct buf b;
    buf_init(&b);
    picker_core_append_sanitized(&b, "abcdef", 3);
    buf_append(&b, "", 1);
    EXPECT_STR_EQ(b.data, "abc");
    buf_free(&b);
}

/* Build a label of exactly `cells` ASCII cells. */
static char *label_of(int cells)
{
    char *s = xmalloc((size_t)cells + 1);
    memset(s, 'x', (size_t)cells);
    s[cells] = '\0';
    return s;
}

static void test_label_cells_without_detail(void)
{
    /* Nothing competing: the label gets the row minus the marker column. */
    struct picker_item it = {.label = "anything"};
    EXPECT(picker_core_label_cells(&it, 100) == 98);
    /* The "✓ current" tag reserves its own space. */
    struct picker_item cur = {.label = "anything", .current = 1};
    EXPECT(picker_core_label_cells(&cur, 100) == 98 - (int)strlen("  * current"));
}

static void test_label_cells_yields_to_detail(void)
{
    /* A detail claims the room the label doesn't need, so the label's
     * allowance drops below the row width. This is the case the footer
     * reservation used to miss: every /resume row carries a relative-time
     * detail, so a label can be clipped well before it reaches 98 cells. */
    char *lbl = label_of(95);
    struct picker_item it = {.label = lbl, .detail = "3m ago"};
    int cells = picker_core_label_cells(&it, 100);
    EXPECT(cells == 90); /* 98 - 2 separator - 6 detail */
    /* ... and that 95-cell label really is clipped by it. */
    EXPECT(picker_core_clip_width(lbl) > cells);
    free(lbl);
}

static void test_label_cells_keeps_half_the_row(void)
{
    /* A detail long enough to crowd the label out is capped at half the
     * row — the label never shrinks past that. */
    char *lbl = label_of(95);
    char *huge = label_of(90);
    struct picker_item it = {.label = lbl, .detail = huge};
    EXPECT(picker_core_label_cells(&it, 100) == 49); /* 98 / 2 */
    free(lbl);
    free(huge);
}

static void test_label_cells_short_label_keeps_only_what_it_uses(void)
{
    /* A label shorter than its allowance hands the slack to the detail,
     * and reports its natural width — so it is never treated as clipped. */
    struct picker_item it = {.label = "short", .detail = "3m ago"};
    int cells = picker_core_label_cells(&it, 100);
    EXPECT(cells == 5);
    EXPECT(picker_core_clip_width("short") == cells); /* fits exactly */
}

static void test_label_cells_dim_row_wider_separator(void)
{
    /* A dim row separates with " - " rather than "  ", one cell more. */
    char *lbl = label_of(95);
    struct picker_item plain = {.label = lbl, .detail = "3m ago"};
    struct picker_item dim = {.label = lbl, .detail = "3m ago", .dim = 1};
    EXPECT(picker_core_label_cells(&dim, 100) == picker_core_label_cells(&plain, 100) - 1);
    free(lbl);
}

static void test_label_cells_narrow_terminal(void)
{
    /* Degenerate widths still yield a usable budget rather than 0 or less. */
    struct picker_item it = {.label = "some label", .detail = "3m ago"};
    EXPECT(picker_core_label_cells(&it, 4) >= 1);
    EXPECT(picker_core_label_cells(&it, 1) >= 1);
    EXPECT(picker_core_label_cells(&it, 0) >= 1);
}

int main(void)
{
    locale_init_utf8();
    test_label_cells_without_detail();
    test_label_cells_yields_to_detail();
    test_label_cells_keeps_half_the_row();
    test_label_cells_short_label_keeps_only_what_it_uses();
    test_label_cells_dim_row_wider_separator();
    test_label_cells_narrow_terminal();
    test_sanitize_strips_escapes();
    test_sanitize_strips_c0_and_keeps_utf8();
    test_sanitize_counted_not_terminated();
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
