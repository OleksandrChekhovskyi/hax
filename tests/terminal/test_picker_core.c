/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "harness.h"
#include "xalloc.h"
#include "system/locale.h"
#include "terminal/picker.h"
#include "terminal/picker_core.h"

static void test_empty_query_matches_all(void)
{
    EXPECT(picker_core_match("anything", ""));
    EXPECT(picker_core_match("anything", NULL));
    EXPECT(picker_core_match("", ""));
    EXPECT(picker_core_match(NULL, ""));
    EXPECT(!picker_core_match(NULL, "x"));
}

static void test_substring_ignores_ascii_case(void)
{
    EXPECT(picker_core_match("anthropic/claude-opus-4", "opus"));
    EXPECT(picker_core_match("anthropic/claude-opus-4", "OPUS"));
    EXPECT(picker_core_match("anthropic/claude-opus-4", "Claude"));
    EXPECT(picker_core_match("anthropic/claude-opus-4", "4"));
    EXPECT(!picker_core_match("anthropic/claude-opus-4", "gpt"));
}

static void test_all_terms_must_match(void)
{
    EXPECT(picker_core_match("anthropic/claude-opus-4", "anthropic opus"));
    EXPECT(picker_core_match("anthropic/claude-opus-4", "opus anthropic"));
    EXPECT(!picker_core_match("anthropic/claude-opus-4", "anthropic gpt"));
}

static void test_query_space_handling(void)
{
    EXPECT(picker_core_match("hello world", "  hello   world  "));
    EXPECT(picker_core_match("hello world", "   "));
    EXPECT(!picker_core_match("hello world", "llowo"));
}

struct navigation_fixture {
    struct picker_item items[256];
    struct picker_opts options;
    size_t matches[256];
    struct picker_core core;
};

static void init_navigation_fixture(struct navigation_fixture *fixture, size_t item_count,
                                    int viewport_rows)
{
    memset(fixture, 0, sizeof *fixture);
    for (size_t i = 0; i < item_count; i++) {
        fixture->items[i].label = "x";
        fixture->matches[i] = i;
    }
    fixture->options.items = fixture->items;
    fixture->options.item_count = item_count;
    fixture->core.options = &fixture->options;
    fixture->core.matches = fixture->matches;
    fixture->core.match_count = item_count;
    fixture->core.viewport_rows = viewport_rows;
}

static void test_page_selection_jumps_half_and_centers(void)
{
    struct navigation_fixture fixture;
    init_navigation_fixture(&fixture, 100, 20);

    picker_core_page_selection(&fixture.core, PICKER_DIRECTION_NEXT);
    EXPECT(fixture.core.selection == 10);
    EXPECT(fixture.core.first_visible == 0);

    picker_core_page_selection(&fixture.core, PICKER_DIRECTION_NEXT);
    EXPECT(fixture.core.selection == 20);
    EXPECT(fixture.core.first_visible == 10);
}

static void test_page_selection_clamps_at_ends(void)
{
    struct navigation_fixture fixture;
    init_navigation_fixture(&fixture, 100, 20);
    fixture.core.selection = 95;

    picker_core_page_selection(&fixture.core, PICKER_DIRECTION_NEXT);
    EXPECT(fixture.core.selection == 99);
    EXPECT(fixture.core.first_visible == 80);

    picker_core_page_selection(&fixture.core, PICKER_DIRECTION_PREVIOUS);
    EXPECT(fixture.core.selection == 89);
    EXPECT(fixture.core.first_visible == 79);
}

static void test_move_selection_steps_and_clamps(void)
{
    struct navigation_fixture fixture;
    init_navigation_fixture(&fixture, 3, 10);

    picker_core_move_selection(&fixture.core, PICKER_DIRECTION_PREVIOUS);
    EXPECT(fixture.core.selection == 0);
    picker_core_move_selection(&fixture.core, PICKER_DIRECTION_NEXT);
    EXPECT(fixture.core.selection == 1);
    picker_core_move_selection(&fixture.core, PICKER_DIRECTION_NEXT);
    picker_core_move_selection(&fixture.core, PICKER_DIRECTION_NEXT);
    EXPECT(fixture.core.selection == 2);
}

static void test_zero_viewport_still_clamps_view(void)
{
    struct navigation_fixture fixture;
    init_navigation_fixture(&fixture, 3, 0);
    fixture.core.selection = 2;

    picker_core_clamp_view(&fixture.core);
    EXPECT(fixture.core.first_visible == 2);
}

static void test_select_item_centers(void)
{
    struct navigation_fixture fixture;
    init_navigation_fixture(&fixture, 100, 20);

    picker_core_select_item(&fixture.core, 50);
    EXPECT(fixture.core.selection == 50);
    EXPECT(fixture.core.first_visible == 40);

    picker_core_select_item(&fixture.core, 500);
    EXPECT(fixture.core.selection == 50);
}

static void test_update_matches_resets_selection(void)
{
    struct navigation_fixture fixture;
    init_navigation_fixture(&fixture, 8, 10);
    fixture.items[5].label = "openai";
    fixture.items[6].label = "openai-compatible";
    fixture.items[7].label = "openrouter";
    fixture.core.selection = 7;

    buf_init(&fixture.core.query);
    buf_append_str(&fixture.core.query, "openai");
    picker_core_update_matches(&fixture.core);
    EXPECT(fixture.core.match_count == 2);
    EXPECT(fixture.core.matches[0] == 5);
    EXPECT(fixture.core.matches[1] == 6);
    EXPECT(fixture.core.selection == 0);
    EXPECT(fixture.core.first_visible == 0);
    buf_free(&fixture.core.query);
}

static void test_sanitize_replaces_escape_sequences(void)
{
    const char *unsafe = "safe\x1b[2J\x1b[Hgone";
    struct buf output;
    buf_init(&output);

    picker_core_append_sanitized(&output, unsafe, strlen(unsafe));
    buf_append(&output, "", 1);
    EXPECT(strchr(output.data, 0x1b) == NULL);
    EXPECT_STR_EQ(output.data, "safe?[2J?[Hgone");
    buf_free(&output);
}

static void test_sanitize_replaces_controls_and_keeps_utf8(void)
{
    const char *controls = "a\rb\ac";
    struct buf output;
    buf_init(&output);

    picker_core_append_sanitized(&output, controls, strlen(controls));
    buf_append(&output, "", 1);
    EXPECT_STR_EQ(output.data, "a?b?c");
    buf_free(&output);

    if (!locale_have_utf8())
        return;

    buf_init(&output);
    picker_core_append_sanitized(&output, "c – ü", strlen("c – ü"));
    buf_append(&output, "", 1);
    EXPECT_STR_EQ(output.data, "c – ü");
    buf_free(&output);
}

static void test_sanitize_accepts_counted_text(void)
{
    struct buf output;
    buf_init(&output);

    picker_core_append_sanitized(&output, "abcdef", 3);
    buf_append(&output, "", 1);
    EXPECT_STR_EQ(output.data, "abc");
    buf_free(&output);
}

static void test_text_cells_accounts_for_line_break(void)
{
    EXPECT(picker_core_text_cells("abc") == 3);
    EXPECT(picker_core_text_cells("abc\nrest") == 4);
    EXPECT(picker_core_text_cells("abc\rrest") == 4);
}

static char *make_label(int cells)
{
    char *label = xmalloc((size_t)cells + 1);
    memset(label, 'x', (size_t)cells);
    label[cells] = '\0';
    return label;
}

static void test_label_cells_without_detail(void)
{
    struct picker_item item = {.label = "anything"};
    struct picker_item current = {.label = "anything", .current = 1};

    EXPECT(picker_core_label_cells(&item, 100) == 98);
    EXPECT(picker_core_label_cells(&current, 100) == 98 - PICKER_CURRENT_TAG_CELLS);
}

static void test_label_cells_yields_to_detail(void)
{
    char *label = make_label(95);
    struct picker_item item = {.label = label, .detail = "3m ago"};

    int cells = picker_core_label_cells(&item, 100);
    EXPECT(cells == 90);
    EXPECT(picker_core_text_cells(label) > cells);
    free(label);
}

static void test_label_cells_keeps_half_the_row(void)
{
    char *label = make_label(95);
    char *long_detail = make_label(90);
    struct picker_item item = {.label = label, .detail = long_detail};

    EXPECT(picker_core_label_cells(&item, 100) == 49);
    free(label);
    free(long_detail);
}

static void test_short_label_uses_natural_width(void)
{
    struct picker_item item = {.label = "short", .detail = "3m ago"};

    int cells = picker_core_label_cells(&item, 100);
    EXPECT(cells == 5);
    EXPECT(picker_core_text_cells("short") == cells);
}

static void test_dim_label_accounts_for_wider_separator(void)
{
    char *label = make_label(95);
    struct picker_item plain = {.label = label, .detail = "3m ago"};
    struct picker_item dimmed = {.label = label, .detail = "3m ago", .dim = 1};

    EXPECT(picker_core_label_cells(&dimmed, 100) == picker_core_label_cells(&plain, 100) - 1);
    free(label);
}

static void test_label_cells_narrow_terminal(void)
{
    struct picker_item item = {.label = "some label", .detail = "3m ago"};

    EXPECT(picker_core_label_cells(&item, 4) >= 1);
    EXPECT(picker_core_label_cells(&item, 1) >= 1);
    EXPECT(picker_core_label_cells(&item, 0) >= 1);
}

int main(void)
{
    locale_init_utf8();
    test_label_cells_without_detail();
    test_label_cells_yields_to_detail();
    test_label_cells_keeps_half_the_row();
    test_short_label_uses_natural_width();
    test_dim_label_accounts_for_wider_separator();
    test_label_cells_narrow_terminal();
    test_text_cells_accounts_for_line_break();
    test_sanitize_replaces_escape_sequences();
    test_sanitize_replaces_controls_and_keeps_utf8();
    test_sanitize_accepts_counted_text();
    test_empty_query_matches_all();
    test_substring_ignores_ascii_case();
    test_all_terms_must_match();
    test_query_space_handling();
    test_page_selection_jumps_half_and_centers();
    test_page_selection_clamps_at_ends();
    test_move_selection_steps_and_clamps();
    test_zero_viewport_still_clamps_view();
    test_select_item_centers();
    test_update_matches_resets_selection();
    T_REPORT();
}
