/* SPDX-License-Identifier: MIT */
#include "compact_text.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "provider.h"
#include "xalloc.h"
#include "text/utf8.h"
#include "tools/output_cap.h"

/* How far a turn is shortened, each level removing more than the one before. */
enum turn_level {
    LEVEL_WHOLE,
    LEVEL_OUTPUTS_CLIPPED, /* tool outputs cut to whole lines of head and, mostly, tail */
    LEVEL_OUTPUTS_DROPPED, /* tool outputs dropped, call arguments clipped short */
    LEVEL_DIALOGUE,        /* user text and replies ending a user turn, clipped; tool use dropped */
    LEVEL_OMITTED,
    LEVEL_COUNT,
};

/* Bytes a clipped text keeps, `head` of them from its start and the rest from its end. */
struct clip {
    size_t keep;
    size_t head;
};

/* Tool output is clipped the way tool results are truncated, keeping mostly its end. Call
 * arguments keep just enough to show the target and intent: their payload is the tool's business,
 * and what came of it is in the assistant's reply. */
static const struct clip CLIP_OUTPUT = {2000, 2000 / OUTPUT_CAP_HEAD_DIVISOR};
static const struct clip CLIP_ARGUMENTS = {300, 150};
static const struct clip CLIP_TEXT = {1000, 500};

struct text_turn {
    const struct item *items;
    size_t n_items;
    /* Index into `items` of the latest typed prompt, which is never clipped, or SIZE_MAX when
     * another turn holds it. */
    size_t latest_prompt;
    int uses_tools;
    /* The turn before this one used tools too, so at LEVEL_DIALOGUE its marker covers this
     * turn's tool use. */
    int continues_tool_use;
    char *renders[LEVEL_COUNT]; /* owned; "" at LEVEL_OMITTED */
    size_t lengths[LEVEL_COUNT];
    enum turn_level level;
    enum turn_level max_level;
};

/* Where a head of at most `budget` bytes ends: after whole lines, or at a character boundary when
 * its first line is longer. */
static size_t head_end(const char *text, size_t budget)
{
    size_t end = whole_lines_head(text, budget, SIZE_MAX, NULL);
    return end ? end : utf8_char_start(text, budget);
}

/* Where a tail of at most `budget` of the `length` > `budget` bytes starts: at whole lines, or at a
 * character boundary when its last line is longer. */
static size_t tail_start(const char *text, size_t length, size_t budget)
{
    size_t window = length - budget;
    int mid_line = text[window - 1] != '\n';
    size_t start = whole_lines_tail(text + window, budget, mid_line, SIZE_MAX, NULL);
    return mid_line && start == 0 ? utf8_char_start(text, window) : window + start;
}

/* `clip` may be NULL to append `text` whole. */
static void append_clipped(struct buf *out, const char *text, const struct clip *clip)
{
    size_t length = strlen(text);
    if (!clip || length <= clip->keep) {
        buf_append(out, text, length);
        return;
    }
    size_t head = head_end(text, clip->head);
    size_t tail = tail_start(text, length, clip->keep - clip->head);
    char marker[64];
    snprintf(marker, sizeof(marker), "%s[... %zu bytes omitted ...]\n",
             head > 0 && text[head - 1] != '\n' ? "\n" : "", tail - head);
    buf_append(out, text, head);
    buf_append_str(out, marker);
    buf_append_str(out, text + tail);
}

static void append_block(struct buf *out, const char *label, const char *text,
                         const struct clip *clip)
{
    buf_append_str(out, label);
    buf_append_str(out, "\n");
    append_clipped(out, text, clip);
    buf_append_str(out, "\n\n");
}

static void append_tool_result(struct buf *out, const struct item *item, enum turn_level level)
{
    if (level >= LEVEL_OUTPUTS_DROPPED) {
        buf_append_str(out, "[tool result omitted]\n\n");
        return;
    }
    buf_append_str(out, "[tool result]\n");
    append_clipped(out, item->output ? item->output : "",
                   level == LEVEL_OUTPUTS_CLIPPED ? &CLIP_OUTPUT : NULL);
    for (size_t i = 0; i < item->n_images; i++) {
        char *placeholder = item_image_placeholder(&item->images[i]);
        buf_append_str(out, "\n");
        buf_append_str(out, placeholder);
        free(placeholder);
    }
    buf_append_str(out, "\n\n");
}

/* At LEVEL_DIALOGUE an assistant message stays only when it ends a user turn, in a turn that calls
 * no tools; the narration between tool calls goes with them. */
static void append_item(struct buf *out, const struct text_turn *turn, size_t index,
                        enum turn_level level)
{
    const struct item *item = &turn->items[index];
    switch (item->kind) {
    case ITEM_USER_MESSAGE:
        if (item->text && *item->text)
            append_block(out, "[user]", item->text,
                         level >= LEVEL_DIALOGUE && index != turn->latest_prompt ? &CLIP_TEXT
                                                                                 : NULL);
        break;
    case ITEM_ASSISTANT_MESSAGE:
        if (item->text && *item->text && (level < LEVEL_DIALOGUE || !turn->uses_tools))
            append_block(out, "[assistant]", item->text,
                         level >= LEVEL_DIALOGUE ? &CLIP_TEXT : NULL);
        break;
    case ITEM_TOOL_CALL:
        if (level < LEVEL_DIALOGUE) {
            char *label = xasprintf("[tool call: %s]", item->tool_name ? item->tool_name : "");
            append_block(out, label, item->tool_arguments_json ? item->tool_arguments_json : "",
                         level >= LEVEL_OUTPUTS_DROPPED ? &CLIP_ARGUMENTS : NULL);
            free(label);
        }
        break;
    case ITEM_TOOL_RESULT:
        if (level < LEVEL_DIALOGUE)
            append_tool_result(out, item, level);
        break;
    case ITEM_REASONING:
    case ITEM_TURN_BOUNDARY:
    case ITEM_TURN_USAGE:
        break;
    }
}

static int is_seed(const struct item *item)
{
    return item->kind == ITEM_USER_MESSAGE && item->origin == ITEM_ORIGIN_COMPACT_SEED;
}

static int uses_tools(const struct item *items, size_t n_items)
{
    for (size_t i = 0; i < n_items; i++)
        if (items[i].kind == ITEM_TOOL_CALL || items[i].kind == ITEM_TOOL_RESULT)
            return 1;
    return 0;
}

static char *render_turn(const struct text_turn *turn, enum turn_level level)
{
    struct buf out;
    buf_init(&out);
    if (level < LEVEL_OMITTED)
        for (size_t i = 0; i < turn->n_items; i++)
            if (!is_seed(&turn->items[i]))
                append_item(&out, turn, i, level);
    /* Without a marker, the summarizer could take work that was done as never attempted. */
    if (level == LEVEL_DIALOGUE && turn->uses_tools && (out.len > 0 || !turn->continues_tool_use))
        buf_append_str(&out, "[assistant's tool use omitted]\n\n");
    return out.data ? buf_steal(&out) : xstrdup("");
}

/* A marker can outgrow what it replaces, such as a short tool result, so a level that would
 * lengthen the turn keeps the one before: raising a level must never cost room. */
static void render_levels(struct text_turn *turn)
{
    for (int level = LEVEL_WHOLE; level < LEVEL_COUNT; level++) {
        turn->renders[level] = render_turn(turn, level);
        turn->lengths[level] = strlen(turn->renders[level]);
        if (level > LEVEL_WHOLE && turn->lengths[level] > turn->lengths[level - 1]) {
            free(turn->renders[level]);
            turn->renders[level] = xstrdup(turn->renders[level - 1]);
            turn->lengths[level] = turn->lengths[level - 1];
        }
    }
}

/* A protected turn goes through the levels like any other but stops at the last one that leaves
 * it content, so it is never left out. */
static void set_max_level(struct text_turn *turn, int is_protected)
{
    turn->max_level = is_protected ? LEVEL_WHOLE : LEVEL_OMITTED;
    for (int level = LEVEL_OUTPUTS_CLIPPED; is_protected && level < LEVEL_COUNT; level++)
        if (turn->lengths[level] > 0)
            turn->max_level = level;
}

static void free_turns(struct text_turn *turns, size_t n_turns)
{
    for (size_t i = 0; i < n_turns; i++)
        for (int level = 0; level < LEVEL_COUNT; level++)
            free(turns[i].renders[level]);
    free(turns);
}

static size_t latest_typed_prompt(const struct item *items, size_t n_items)
{
    for (size_t i = n_items; i-- > 0;)
        if (item_is_typed_prompt(&items[i]))
            return i;
    return SIZE_MAX;
}

/* Turns with content, oldest first. The newest turn and the latest typed prompt's turn are
 * protected: the prompt's turn may still lose all but the prompt. */
static struct text_turn *split_turns(const struct item *items, size_t n_items, size_t *n_turns)
{
    struct text_turn *turns = xmalloc((n_items + 1) * sizeof(*turns));
    size_t prompt = latest_typed_prompt(items, n_items);
    size_t count = 0;
    const struct item *previous = items;

    for (size_t start = 0; start < n_items;) {
        size_t end = start + 1;
        while (end < n_items && items[end].kind != ITEM_TURN_BOUNDARY)
            end++;
        struct text_turn *turn = &turns[count];
        memset(turn, 0, sizeof(*turn));
        turn->items = &items[start];
        turn->n_items = end - start;
        turn->latest_prompt = prompt >= start && prompt < end ? prompt - start : SIZE_MAX;
        turn->uses_tools = uses_tools(turn->items, turn->n_items);
        turn->continues_tool_use =
            start > 0 && uses_tools(previous, (size_t)(&items[start] - previous));
        render_levels(turn);
        if (turn->lengths[LEVEL_WHOLE] > 0)
            count++;
        else
            for (int level = 0; level < LEVEL_COUNT; level++)
                free(turn->renders[level]);
        previous = &items[start];
        start = end;
    }

    for (size_t i = 0; i < count; i++)
        set_max_level(&turns[i], turns[i].latest_prompt != SIZE_MAX || i == count - 1);
    *n_turns = count;
    return turns;
}

static void append_gap(struct buf *out, size_t turns)
{
    char marker[64];
    snprintf(marker, sizeof(marker), "[%zu turn%s omitted]\n\n", turns, turns == 1 ? "" : "s");
    buf_append_str(out, marker);
}

static void append_seed(struct buf *out, const struct item *items, size_t n_items)
{
    for (size_t i = 0; i < n_items; i++)
        if (is_seed(&items[i]))
            append_block(out, "[earlier summary]", items[i].text ? items[i].text : "", NULL);
}

/* Raise levels oldest turn first, every turn reaching a level before any reaches the next, until
 * `fixed` plus the turns fit `max_tokens`. Returns whether they do. */
static int shorten_to_fit(struct text_turn *turns, size_t n_turns, size_t fixed, long max_tokens)
{
    size_t total = fixed;
    for (size_t i = 0; i < n_turns; i++)
        total += turns[i].lengths[LEVEL_WHOLE];

    for (int level = LEVEL_OUTPUTS_CLIPPED; level < LEVEL_COUNT; level++) {
        for (size_t i = 0; i < n_turns && bytes_token_estimate(total) > max_tokens; i++) {
            struct text_turn *turn = &turns[i];
            if (level > (int)turn->max_level)
                continue;
            total = total - turn->lengths[turn->level] + turn->lengths[level];
            turn->level = level;
        }
    }
    return bytes_token_estimate(total) <= max_tokens;
}

/* Turns are left out oldest first, skipping only the two protected turns, so left-out turns form
 * at most two gaps: before the latest typed prompt's turn and after it. */
#define MAX_GAPS 2

int compact_text_fit(const struct item *items, size_t n_items, long max_tokens,
                     struct compact_text *out)
{
    memset(out, 0, sizeof(*out));
    size_t n_turns;
    struct text_turn *turns = split_turns(items, n_items, &n_turns);
    struct buf seed;
    buf_init(&seed);
    append_seed(&seed, items, n_items);

    size_t whole = seed.len;
    for (size_t i = 0; i < n_turns; i++)
        whole += turns[i].lengths[LEVEL_WHOLE];
    if (bytes_token_estimate(whole) > max_tokens) {
        /* Room for the gap markers at their longest, before knowing where they fall. */
        struct buf gaps;
        buf_init(&gaps);
        for (int i = 0; i < MAX_GAPS; i++)
            append_gap(&gaps, n_turns);
        size_t gap_room = gaps.len;
        buf_free(&gaps);
        if (!shorten_to_fit(turns, n_turns, seed.len + gap_room, max_tokens)) {
            buf_free(&seed);
            free_turns(turns, n_turns);
            return -1;
        }
    }

    struct buf text;
    buf_init(&text);
    if (seed.len)
        buf_append(&text, seed.data, seed.len);
    /* A gap counts the empty turns around left-out ones; empty turns alone are covered by the
     * tool-use marker before them. */
    size_t gap = 0;
    int gap_has_left_out = 0;
    for (size_t i = 0; i < n_turns; i++) {
        const char *render = turns[i].renders[turns[i].level];
        if (!*render) {
            gap++;
            gap_has_left_out |= turns[i].level == LEVEL_OMITTED;
            continue;
        }
        if (gap_has_left_out)
            append_gap(&text, gap);
        gap = 0;
        gap_has_left_out = 0;
        buf_append_str(&text, render);
    }
    buf_free(&seed);
    free_turns(turns, n_turns);

    out->text = text.data ? buf_steal(&text) : xstrdup("");
    out->tokens = text_token_estimate(out->text);
    return 0;
}

void compact_text_destroy(struct compact_text *text)
{
    free(text->text);
    memset(text, 0, sizeof(*text));
}
