/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "render/markdown_table.h"
#include "terminal/ansi.h"

#define DIM  ANSI_DIM
#define OFF  ANSI_BOLD_OFF
#define BLD  ANSI_BOLD
#define BUL  DIM "\xe2\x80\xa2 " OFF
#define HL   "\xe2\x94\x80"
#define VB   "\xe2\x94\x82"
#define CR   "\xe2\x94\xbc"
#define TSEP " " DIM VB OFF " "

struct fixture {
    struct md_table table;
    struct md_table_context ctx;
    struct buf text;
    struct buf raw;
    struct buf wire;
    int inline_bytewise;
    int commits;
    int row_resets;
};

static void append_output(struct fixture *f, const char *bytes, size_t n, int is_raw)
{
    buf_append(is_raw ? &f->raw : &f->text, bytes, n);
    buf_append(&f->wire, bytes, n);
}

static void emit_direct(const char *bytes, size_t n, int is_raw, void *user)
{
    append_output(user, bytes, n, is_raw);
}

static void emit_text(void *user, const char *s, size_t n)
{
    append_output(user, s, n, 0);
}

static void emit_raw(void *user, const char *s, size_t n)
{
    struct fixture *f = user;
    if (f->ctx.styled)
        append_output(f, s, n, 1);
}

static void replay_raw(void *user, const char *s, size_t n)
{
    emit_raw(user, s, n);
}

static void open_bold(void *user)
{
    emit_raw(user, BLD, strlen(BLD));
}

static void close_bold(void *user)
{
    emit_raw(user, OFF, strlen(OFF));
}

/* Inline parsing stays covered through md_renderer; this fixture supplies plain rendered cells.
 * Bytewise mode exercises width accounting across callback boundaries. */
static void render_inline(void *user, const char *s, size_t n, int bold_base, md_table_emit_fn emit,
                          void *emit_user)
{
    struct fixture *f = user;
    (void)bold_base;
    if (!f->inline_bytewise) {
        emit(s, n, 0, emit_user);
        return;
    }
    for (size_t i = 0; i < n; i++)
        emit(s + i, 1, 0, emit_user);
}

static void commit_pending(void *user)
{
    struct fixture *f = user;
    f->commits++;
}

static void row_reset(void *user)
{
    struct fixture *f = user;
    f->row_resets++;
}

static void setup(struct fixture *f, int wrap_width)
{
    memset(f, 0, sizeof(*f));
    md_table_reset(&f->table);
    f->ctx = (struct md_table_context){
        .user = f,
        .emit_direct = emit_direct,
        .emit_text = emit_text,
        .emit_raw = emit_raw,
        .replay_raw = replay_raw,
        .open_bold = open_bold,
        .close_bold = close_bold,
        .render_inline = render_inline,
        .commit_pending = commit_pending,
        .row_reset = row_reset,
        .styled = 1,
        .wrap_width = wrap_width,
    };
}

static void reset(struct fixture *f, int wrap_width)
{
    md_table_reset(&f->table);
    buf_reset(&f->text);
    buf_reset(&f->raw);
    buf_reset(&f->wire);
    f->inline_bytewise = 0;
    f->commits = 0;
    f->row_resets = 0;
    f->ctx.wrap_width = wrap_width;
}

static void teardown(struct fixture *f)
{
    md_table_free(&f->table);
    buf_free(&f->text);
    buf_free(&f->raw);
    buf_free(&f->wire);
}

static const char *buf_string(const struct buf *b)
{
    return b->data ? b->data : "";
}

static void render_table(struct fixture *f, const char *input)
{
    struct buf work;
    buf_init(&work);
    buf_append(&work, input, strlen(input));

    size_t offset = 0;
    enum md_table_result result = md_table_try_start(&f->table, &work, &offset);
    if (result == MD_TABLE_DEFER) {
        md_table_finish(&f->table, &f->ctx, &work);
        buf_free(&work);
        return;
    }
    EXPECT(result == MD_TABLE_ADVANCED);

    while (offset < work.len) {
        result = md_table_step(&f->table, &f->ctx, &work, &offset);
        if (result == MD_TABLE_ADVANCED)
            continue;
        if (result == MD_TABLE_DEFER) {
            struct buf tail;
            buf_init(&tail);
            buf_append(&tail, work.data + offset, work.len - offset);
            md_table_finish(&f->table, &f->ctx, &tail);
            buf_free(&tail);
        }
        break;
    }

    if (md_table_is_collecting(&f->table)) {
        struct buf tail;
        buf_init(&tail);
        md_table_finish(&f->table, &f->ctx, &tail);
        buf_free(&tail);
    }
    buf_free(&work);
}

static void test_collection_results(void)
{
    struct fixture f;
    setup(&f, 40);

    /* Deferred and terminating lines stay unconsumed so the parser can retry or handle them. */
    const char *head = "| A |\n|---|\n";
    struct buf work;
    buf_init(&work);
    buf_append(&work, head, strlen(head));
    buf_append(&work, "| x |", 5);

    size_t offset = 0;
    EXPECT(md_table_try_start(&f.table, &work, &offset) == MD_TABLE_ADVANCED);
    EXPECT(offset == strlen(head));
    EXPECT(md_table_is_collecting(&f.table));
    EXPECT(f.wire.len == 0);

    size_t before = offset;
    EXPECT(md_table_step(&f.table, &f.ctx, &work, &offset) == MD_TABLE_DEFER);
    EXPECT(offset == before);

    buf_append(&work, "\n", 1);
    EXPECT(md_table_step(&f.table, &f.ctx, &work, &offset) == MD_TABLE_ADVANCED);
    EXPECT(offset == work.len);

    buf_append(&work, "after\n", 6);
    before = offset;
    EXPECT(md_table_step(&f.table, &f.ctx, &work, &offset) == MD_TABLE_PASS);
    EXPECT(offset == before);
    EXPECT(!md_table_is_collecting(&f.table));
    EXPECT(f.commits == 1);
    EXPECT(f.row_resets == 1);
    EXPECT_STR_EQ(buf_string(&f.wire), BLD "A" OFF "\n" DIM HL OFF "\n"
                                           "x\n");

    buf_free(&work);
    teardown(&f);
}

static void test_aligned_layout_and_callback_kinds(void)
{
    struct fixture f;
    setup(&f, 40);

    /* Grid layout bypasses wrapping but must preserve raw and text callback classification. */
    render_table(&f, "| Name | Age |\n|---|---|\n| Bob | 30 |\n| Alice | 7 |");
    EXPECT_STR_EQ(buf_string(&f.wire), BLD "Name" OFF " " TSEP BLD "Age" OFF
                                           "\n" DIM HL HL HL HL HL HL CR HL HL HL HL OFF "\n"
                                           "Bob  " TSEP "30\n"
                                           "Alice" TSEP "7\n");
    const char *raw = BLD OFF DIM OFF BLD OFF DIM OFF DIM OFF DIM OFF;
    EXPECT_MEM_EQ(f.raw.data, f.raw.len, raw, strlen(raw));
    EXPECT(strstr(buf_string(&f.text), "Alice") != NULL);

    teardown(&f);
}

static void test_unstyled_layout_suppresses_sgr(void)
{
    struct fixture f;
    setup(&f, 40);
    f.ctx.styled = 0;

    /* Disabling styles suppresses SGR without removing the Unicode grid structure. */
    render_table(&f, "| A | B |\n|---|---|\n| 1 | 2 |");
    EXPECT_STR_EQ(buf_string(&f.raw), "");
    EXPECT(strchr(buf_string(&f.wire), '\x1b') == NULL);
    EXPECT(strstr(buf_string(&f.wire), "A " VB " B") != NULL);

    teardown(&f);
}

static void test_column_alignment(void)
{
    struct fixture f;
    setup(&f, 40);

    /* Delimiter colons control padding independently for each column. */
    render_table(&f, "| L | R | C |\n|:--|--:|:-:|\n| aaaa | bbbb | cccc |\n| x | y | z |");
    EXPECT_STR_EQ(buf_string(&f.wire),
                  BLD "L" OFF "   " TSEP "   " BLD "R" OFF TSEP " " BLD "C" OFF
                      "\n" DIM HL HL HL HL HL CR HL HL HL HL HL HL CR HL HL HL HL HL OFF "\n"
                      "aaaa" TSEP "bbbb" TSEP "cccc\n"
                      "x   " TSEP "   y" TSEP " z\n");

    teardown(&f);
}

static void test_reflow_layout(void)
{
    struct fixture f;
    setup(&f, 20);

    /* A grid wider than the wrap budget becomes readable labeled records instead of clipping. */
    render_table(&f, "| Component | Role | Owner |\n|---|---|---|\n"
                     "| parser | reads tokens | ann |\n| writer | emits bytes | bob |");
    EXPECT_STR_EQ(buf_string(&f.wire),
                  BUL BLD "Component" OFF ": parser\n"
                          "  " BLD "Role" OFF ": reads tokens\n"
                          "  " BLD "Owner" OFF ": ann\n" BUL BLD "Component" OFF ": writer\n"
                          "  " BLD "Role" OFF ": emits bytes\n"
                          "  " BLD "Owner" OFF ": bob\n");

    teardown(&f);
}

static void test_invalid_delimiters_pass(void)
{
    /* False-positive headers must remain untouched for ordinary Markdown parsing. */
    static const char *cases[] = {
        "| a | b |\nplain prose\n",
        "| A | B |\n|---|---|---|\n",
        "| A | B |\n|:-:-|---|\n",
    };
    struct fixture f;
    setup(&f, 40);

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct buf work;
        buf_init(&work);
        buf_append(&work, cases[i], strlen(cases[i]));
        size_t offset = 0;
        EXPECT(md_table_try_start(&f.table, &work, &offset) == MD_TABLE_PASS);
        EXPECT(offset == 0);
        EXPECT(!md_table_is_collecting(&f.table));
        buf_free(&work);
        reset(&f, 40);
    }

    teardown(&f);
}

static void test_eof_final_row_and_header_only(void)
{
    struct fixture f;
    setup(&f, 40);

    /* EOF completes a final row, while a header-only table must not vanish through reflow. */
    render_table(&f, "| A | B |\n|---|---|\n| 1 | 2 |");
    EXPECT_STR_EQ(buf_string(&f.wire), BLD "A" OFF TSEP BLD "B" OFF "\n" DIM HL HL CR HL HL OFF "\n"
                                           "1" TSEP "2\n");

    reset(&f, 5);
    render_table(&f, "| AB | CD |\n|---|---|");
    EXPECT_STR_EQ(buf_string(&f.wire),
                  BLD "AB" OFF TSEP BLD "CD" OFF "\n" DIM HL HL HL CR HL HL HL OFF "\n");

    teardown(&f);
}

static void test_split_utf8_cell_width(void)
{
    struct fixture f;
    setup(&f, 40);
    f.inline_bytewise = 1;

    /* Cell width must be measured across inline callback boundaries, including split UTF-8. */
    render_table(&f, "| X | Y |\n|---|---|\n| \xc3\xa9\xc3\xa9 | z |");
    EXPECT_STR_EQ(buf_string(&f.wire),
                  BLD "X" OFF " " TSEP BLD "Y" OFF "\n" DIM HL HL HL CR HL HL OFF "\n"
                      "\xc3\xa9\xc3\xa9" TSEP "z\n");

    teardown(&f);
}

static void test_literal_pipe_falls_back_losslessly(void)
{
    struct fixture f;
    setup(&f, 40);

    /* Unsupported literal pipes must trigger verbatim fallback rather than drop extra cells. */
    const char *code = "| Cmd | Meaning |\n|---|---|\n| `ls | wc` | count lines |\n";
    render_table(&f, code);
    EXPECT_STR_EQ(buf_string(&f.wire), code);
    EXPECT_STR_EQ(buf_string(&f.raw), "");

    reset(&f, 40);
    const char *escaped = "| Expr | Meaning |\n|---|---|\n| a \\| b | union |\n";
    render_table(&f, escaped);
    EXPECT_STR_EQ(buf_string(&f.wire), escaped);

    teardown(&f);
}

static void test_row_span_overflow_falls_back_losslessly(void)
{
    struct fixture f;
    setup(&f, 40);

    /* Exceeding fixed layout metadata must preserve every row through verbatim fallback. */
    size_t cap = 64 * 1024;
    char *input = xmalloc(cap);
    int p = snprintf(input, cap, "| H |\n|---|\n");
    for (int i = 0; i < 2100 && (size_t)p < cap - 32; i++)
        p += snprintf(input + p, cap - (size_t)p, "| r%d |\n", i);
    render_table(&f, input);
    EXPECT(strstr(buf_string(&f.wire), "r0") != NULL);
    EXPECT(strstr(buf_string(&f.wire), "r2099") != NULL);
    EXPECT_STR_EQ(buf_string(&f.raw), "");

    free(input);
    teardown(&f);
}

static void test_oversized_partial_bails(void)
{
    struct fixture f;
    setup(&f, 40);

    /* A newline-less row cannot grow collection past the cap and must survive verbatim. */
    struct buf work;
    buf_init(&work);
    const char *head = "| H |\n|---|\n";
    buf_append(&work, head, strlen(head));
    size_t offset = 0;
    EXPECT(md_table_try_start(&f.table, &work, &offset) == MD_TABLE_ADVANCED);

    size_t len = 70000;
    char *row = xmalloc(len);
    row[0] = '|';
    row[1] = ' ';
    memset(row + 2, 'a', len - 4);
    row[len - 2] = ' ';
    row[len - 1] = '|';
    EXPECT(md_table_bail_partial(&f.table, &f.ctx, row, len));
    EXPECT(!md_table_is_collecting(&f.table));
    EXPECT(strncmp(buf_string(&f.wire), head, strlen(head)) == 0);
    EXPECT(strstr(buf_string(&f.wire), "aaaaaaaaaaaaaaaaaaaa") != NULL);

    free(row);
    buf_free(&work);
    teardown(&f);
}

static void test_oversized_complete_row_is_not_collected(void)
{
    struct fixture f;
    setup(&f, 40);

    /* An oversized complete row finalizes the safe prefix and remains for parser passthrough. */
    size_t big = 70000;
    char *input = xmalloc(big + 64);
    int p = snprintf(input, big + 64, "| H |\n|---|\n| ");
    size_t body_offset = (size_t)p - 2;
    memset(input + p, 'a', big);
    p += (int)big;
    p += snprintf(input + p, 64, " |\n");

    struct buf work;
    buf_init(&work);
    buf_append(&work, input, (size_t)p);
    size_t offset = 0;
    EXPECT(md_table_try_start(&f.table, &work, &offset) == MD_TABLE_ADVANCED);
    EXPECT(offset == body_offset);
    EXPECT(md_table_step(&f.table, &f.ctx, &work, &offset) == MD_TABLE_PASS);
    EXPECT(offset == body_offset);
    EXPECT(!md_table_is_collecting(&f.table));
    EXPECT(strncmp(buf_string(&f.wire), BLD, strlen(BLD)) == 0);

    buf_free(&work);
    free(input);
    teardown(&f);
}

static void test_oversized_header_passes(void)
{
    struct fixture f;
    setup(&f, 40);

    /* Rejecting an oversized header before collection bounds buffering and layout work. */
    size_t big = 70000;
    char *input = xmalloc(big + 64);
    int p = snprintf(input, big + 64, "| ");
    memset(input + p, 'H', big);
    p += (int)big;
    p += snprintf(input + p, 64, " |\n|---|\n");
    struct buf work;
    buf_init(&work);
    buf_append(&work, input, (size_t)p);
    size_t offset = 0;
    EXPECT(md_table_try_start(&f.table, &work, &offset) == MD_TABLE_PASS);
    EXPECT(offset == 0);
    EXPECT(!md_table_is_collecting(&f.table));

    buf_free(&work);
    free(input);
    teardown(&f);
}

static void test_eof_row_over_cap_stays_in_tail(void)
{
    struct fixture f;
    setup(&f, 40);

    /* EOF must not absorb a final row that would push the collected table past its cap. */
    size_t row_len = 60000;
    char *row = xmalloc(row_len + 8);
    int p = snprintf(row, row_len + 8, "| ");
    memset(row + p, 'a', row_len);
    p += (int)row_len;
    p += snprintf(row + p, 8, " |\n");

    struct buf work;
    buf_init(&work);
    const char *head = "| H |\n|---|\n";
    buf_append(&work, head, strlen(head));
    buf_append(&work, row, (size_t)p);
    size_t offset = 0;
    EXPECT(md_table_try_start(&f.table, &work, &offset) == MD_TABLE_ADVANCED);
    EXPECT(md_table_step(&f.table, &f.ctx, &work, &offset) == MD_TABLE_ADVANCED);

    struct buf tail;
    buf_init(&tail);
    buf_append(&tail, "| ", 2);
    for (int i = 0; i < 10000; i++)
        buf_append(&tail, "b", 1);
    buf_append(&tail, " |", 2);
    size_t tail_len = tail.len;
    md_table_finish(&f.table, &f.ctx, &tail);
    EXPECT(tail.len == tail_len);
    EXPECT(!memcmp(tail.data, "| bbbbbbbbbb", 12));
    EXPECT(!md_table_is_collecting(&f.table));

    buf_free(&tail);
    buf_free(&work);
    free(row);
    teardown(&f);
}

int main(void)
{
    locale_init_utf8();

    test_collection_results();
    test_aligned_layout_and_callback_kinds();
    test_unstyled_layout_suppresses_sgr();
    test_column_alignment();
    test_reflow_layout();
    test_invalid_delimiters_pass();
    test_eof_final_row_and_header_only();
    test_split_utf8_cell_width();
    test_literal_pipe_falls_back_losslessly();
    test_row_span_overflow_falls_back_losslessly();
    test_oversized_partial_bails();
    test_oversized_complete_row_is_not_collected();
    test_oversized_header_passes();
    test_eof_row_over_cap_stays_in_tail();

    T_REPORT();
}
