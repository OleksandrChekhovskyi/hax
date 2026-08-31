/* SPDX-License-Identifier: MIT */
#include "terminal/input_core.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "xalloc.h"
#include "terminal/input.h"
#include "text/utf8.h"

/* ---------------- public API: alloc / free ---------------- */

struct input *input_new(void)
{
    struct input *in = xcalloc(1, sizeof(*in));
    in->buf = xmalloc(64);
    in->cap = 64;
    in->buf[0] = '\0';
    return in;
}

void input_free(struct input *in)
{
    if (!in)
        return;
    free(in->buf);
    free(in->draft);
    for (size_t i = 0; i < in->hist_n; i++)
        free(in->hist[i]);
    free(in->hist);
    free(in->persist_path);
    free(in->preseed);
    free(in);
}

int input_core_prompt_width(const char *prompt)
{
    int width = 0;
    size_t len = strlen(prompt);

    for (size_t offset = 0; offset < len;) {
        unsigned char byte = (unsigned char)prompt[offset];
        if (byte == '\n')
            break;
        if (byte == 0x1b && offset + 1 < len && prompt[offset + 1] == '[') {
            offset += 2;
            while (offset < len && (unsigned char)prompt[offset] < 0x40)
                offset++;
            if (offset < len)
                offset++;
            continue;
        }

        size_t consumed;
        int glyph_width = utf8_codepoint_cells(prompt, len, offset, &consumed);
        if (glyph_width < 0)
            glyph_width = 1;
        width += glyph_width;
        offset += consumed ? consumed : 1;
    }
    return width;
}

/* ---------------- buffer ops ---------------- */

static void ensure_buffer_capacity(struct input *in, size_t required)
{
    if (required <= in->cap)
        return;

    size_t capacity = in->cap ? in->cap : 64;
    while (capacity < required)
        capacity *= 2;
    in->buf = xrealloc(in->buf, capacity);
    in->cap = capacity;
}

void input_core_set_buffer(struct input *in, const char *text)
{
    size_t len = text ? strlen(text) : 0;

    ensure_buffer_capacity(in, len + 1);
    if (len > 0)
        memcpy(in->buf, text, len);
    in->buf[len] = '\0';
    in->len = len;
    in->cursor = len;
}

void input_core_insert(struct input *in, const char *bytes, size_t len)
{
    if (len == 0)
        return;
    ensure_buffer_capacity(in, in->len + len + 1);
    memmove(in->buf + in->cursor + len, in->buf + in->cursor, in->len - in->cursor);
    memcpy(in->buf + in->cursor, bytes, len);
    in->len += len;
    in->cursor += len;
    in->buf[in->len] = '\0';
}

void input_core_commit_paste(struct input *in, const char *body, size_t len)
{
    if (in->paste_filter) {
        char *replacement = in->paste_filter(body, in->paste_filter_user);
        if (replacement) {
            input_core_insert(in, replacement, strlen(replacement));
            free(replacement);
            return;
        }
    }
    input_core_insert(in, body, len);
}

void input_core_replace_span(struct input *in, size_t start, size_t end, const char *text)
{
    if (start > end || end > in->len)
        return;
    size_t replacement_len = text ? strlen(text) : 0;
    size_t tail_len = in->len - end;
    ensure_buffer_capacity(in, in->len - (end - start) + replacement_len + 1);
    memmove(in->buf + start + replacement_len, in->buf + end, tail_len);
    if (replacement_len > 0)
        memcpy(in->buf + start, text, replacement_len);
    in->len = start + replacement_len + tail_len;
    in->cursor = start + replacement_len;
    in->buf[in->len] = '\0';
}

static void buf_erase(struct input *in, size_t pos, size_t n)
{
    if (pos >= in->len || n == 0)
        return;
    if (pos + n > in->len)
        n = in->len - pos;
    memmove(in->buf + pos, in->buf + pos + n, in->len - pos - n);
    in->len -= n;
    in->buf[in->len] = '\0';
    if (in->cursor > pos + n)
        in->cursor -= n;
    else if (in->cursor > pos)
        in->cursor = pos;
}

/* ---------------- motions / edits ---------------- */

size_t input_core_line_start(const struct input *in)
{
    size_t i = in->cursor;
    while (i > 0 && in->buf[i - 1] != '\n')
        i--;
    return i;
}

size_t input_core_line_end(const struct input *in)
{
    size_t i = in->cursor;
    while (i < in->len && in->buf[i] != '\n')
        i++;
    return i;
}

void input_core_move_left(struct input *in)
{
    if (in->cursor > 0)
        in->cursor = utf8_prev(in->buf, in->cursor);
}

void input_core_move_right(struct input *in)
{
    if (in->cursor < in->len)
        in->cursor = utf8_next(in->buf, in->len, in->cursor);
}

void input_core_delete_back(struct input *in)
{
    if (in->cursor == 0)
        return;
    size_t prev = utf8_prev(in->buf, in->cursor);
    buf_erase(in, prev, in->cursor - prev);
}

void input_core_delete_fwd(struct input *in)
{
    if (in->cursor >= in->len)
        return;
    size_t next = utf8_next(in->buf, in->len, in->cursor);
    buf_erase(in, in->cursor, next - in->cursor);
}

void input_core_kill_to_eol(struct input *in)
{
    size_t e = input_core_line_end(in);
    if (e == in->cursor && e < in->len && in->buf[e] == '\n')
        e++; /* on empty line, eat the newline so Ctrl-K joins lines */
    buf_erase(in, in->cursor, e - in->cursor);
}

void input_core_kill_to_bol(struct input *in)
{
    size_t b = input_core_line_start(in);
    buf_erase(in, b, in->cursor - b);
}

/* Ctrl-W uses whitespace boundaries; Meta word operations use readline's alphanumeric
 * boundaries. Bytes >= 0x80 count as word bytes so neither scan splits a UTF-8 sequence. */
static size_t scan_ws_left(const char *buf, size_t i)
{
    while (i > 0 && isspace((unsigned char)buf[i - 1]))
        i--;
    while (i > 0 && !isspace((unsigned char)buf[i - 1]))
        i--;
    return i;
}

static int is_word_byte(unsigned char c)
{
    return c >= 0x80 || isalnum(c);
}

static size_t scan_alnum_left(const char *buf, size_t i)
{
    while (i > 0 && !is_word_byte((unsigned char)buf[i - 1]))
        i--;
    while (i > 0 && is_word_byte((unsigned char)buf[i - 1]))
        i--;
    return i;
}

static size_t scan_alnum_right(const char *buf, size_t len, size_t i)
{
    while (i < len && !is_word_byte((unsigned char)buf[i]))
        i++;
    while (i < len && is_word_byte((unsigned char)buf[i]))
        i++;
    return i;
}

void input_core_move_word_left(struct input *in)
{
    in->cursor = scan_alnum_left(in->buf, in->cursor);
}

void input_core_move_word_right(struct input *in)
{
    in->cursor = scan_alnum_right(in->buf, in->len, in->cursor);
}

void input_core_kill_word_back(struct input *in)
{
    size_t i = scan_ws_left(in->buf, in->cursor);
    buf_erase(in, i, in->cursor - i);
}

void input_core_kill_word_back_alnum(struct input *in)
{
    size_t i = scan_alnum_left(in->buf, in->cursor);
    buf_erase(in, i, in->cursor - i);
}

void input_core_kill_word_fwd(struct input *in)
{
    size_t e = scan_alnum_right(in->buf, in->len, in->cursor);
    buf_erase(in, in->cursor, e - in->cursor);
}

/* ---------------- history ---------------- */

int input_core_history_add(struct input *in, const char *line)
{
    if (!line || !*line)
        return 0;
    if (in->hist_n > 0 && strcmp(in->hist[in->hist_n - 1], line) == 0)
        return 0;
    /* Remove back-to-front so each deletion preserves unvisited indices. */
    for (size_t i = in->hist_n; i > 0; i--) {
        if (strcmp(in->hist[i - 1], line) == 0) {
            free(in->hist[i - 1]);
            memmove(&in->hist[i - 1], &in->hist[i], (in->hist_n - i) * sizeof(char *));
            in->hist_n--;
        }
    }
    if (in->hist_n + 1 > in->hist_cap) {
        in->hist_cap = in->hist_cap ? in->hist_cap * 2 : 16;
        in->hist = xrealloc(in->hist, in->hist_cap * sizeof(char *));
    }
    in->hist[in->hist_n++] = xstrdup(line);
    if (in->hist_n > INPUT_CORE_HISTORY_MAX) {
        free(in->hist[0]);
        memmove(&in->hist[0], &in->hist[1], (in->hist_n - 1) * sizeof(char *));
        in->hist_n--;
    }
    return 1;
}

long input_core_history_search(const struct input *in, const char *query, long start, int dir)
{
    if (!query || !*query || in->hist_n == 0 || (dir != 1 && dir != -1))
        return -1;
    for (long i = start; i >= 0 && (size_t)i < in->hist_n; i += dir) {
        if (strstr(in->hist[i], query))
            return i;
    }
    return -1;
}

/* ---------------- history persistence (encode/decode) ---------------- */

char *input_core_history_encode(const char *entry)
{
    if (!entry)
        return xstrdup("");

    size_t len = strlen(entry);
    char *encoded = xmalloc(len * 2 + 1);
    size_t encoded_len = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char byte = (unsigned char)entry[i];
        if (byte == '\\') {
            encoded[encoded_len++] = '\\';
            encoded[encoded_len++] = '\\';
        } else if (byte == '\n') {
            encoded[encoded_len++] = '\\';
            encoded[encoded_len++] = 'n';
        } else {
            encoded[encoded_len++] = (char)byte;
        }
    }
    encoded[encoded_len] = '\0';
    return encoded;
}

char *input_core_history_decode(const char *encoded, size_t len)
{
    char *entry = xmalloc(len + 1);
    size_t entry_len = 0;

    for (size_t i = 0; i < len; i++) {
        if (encoded[i] == '\\' && i + 1 < len) {
            char escaped = encoded[i + 1];
            if (escaped == '\\') {
                entry[entry_len++] = '\\';
                i++;
                continue;
            }
            if (escaped == 'n') {
                entry[entry_len++] = '\n';
                i++;
                continue;
            }
        }
        entry[entry_len++] = encoded[i];
    }
    entry[entry_len] = '\0';
    return entry;
}

/* hist_pos == hist_n denotes the live draft. Entering history snapshots that draft;
 * returning past the newest entry restores and releases it. */
static void hist_save_draft(struct input *in)
{
    free(in->draft);
    in->draft = xstrdup(in->buf);
}

static void hist_load(struct input *in, size_t pos)
{
    if (pos < in->hist_n) {
        input_core_set_buffer(in, in->hist[pos]);
        in->hist_pos = pos;
    } else {
        input_core_set_buffer(in, in->draft ? in->draft : "");
        free(in->draft);
        in->draft = NULL;
        in->hist_pos = in->hist_n;
    }
}

void input_core_history_prev(struct input *in)
{
    if (in->hist_n == 0)
        return;
    if (in->hist_pos == in->hist_n)
        hist_save_draft(in);
    if (in->hist_pos > 0)
        hist_load(in, in->hist_pos - 1);
}

void input_core_history_next(struct input *in)
{
    if (in->hist_pos >= in->hist_n)
        return;
    hist_load(in, in->hist_pos + 1);
}

/* ---------------- render walker ---------------- */

struct render_glyph {
    const char *bytes;
    size_t len;
    int width;
    size_t offset;
    size_t consumed;
    int is_space;
};

static const char SUBSTITUTE_BYTES[1] = {'?'};
static const char TAB_SPACES[INPUT_CORE_TAB_WIDTH] = {' ', ' ', ' ', ' '};

static int decode_glyph(const char *buf, size_t len, size_t offset, struct render_glyph *glyph)
{
    if (offset >= len)
        return 0;

    unsigned char byte = (unsigned char)buf[offset];
    glyph->offset = offset;
    if (byte == '\t') {
        glyph->bytes = TAB_SPACES;
        glyph->len = INPUT_CORE_TAB_WIDTH;
        glyph->width = INPUT_CORE_TAB_WIDTH;
        glyph->consumed = 1;
        glyph->is_space = 0;
        return 1;
    }
    if (byte < 0x20 || byte == 0x7f) {
        glyph->bytes = SUBSTITUTE_BYTES;
        glyph->len = 1;
        glyph->width = 1;
        glyph->consumed = 1;
        glyph->is_space = 0;
        return 1;
    }

    size_t consumed;
    int width = utf8_codepoint_cells(buf, len, offset, &consumed);
    if (width < 0) {
        glyph->bytes = SUBSTITUTE_BYTES;
        glyph->len = 1;
        glyph->width = 1;
        glyph->consumed = consumed ? consumed : 1;
        glyph->is_space = 0;
        return 1;
    }
    glyph->bytes = buf + offset;
    glyph->len = consumed;
    glyph->width = width;
    glyph->consumed = consumed ? consumed : 1;
    glyph->is_space = consumed == 1 && byte == ' ';
    return 1;
}

/* Buffer the current word so overflow can replay it on the next row; overlong words
 * degrade to character wrapping. Resolve the cursor only at a glyph's final position after any
 * replay. Keep post-emit columns below `columns` to avoid terminals' delayed-wrap state in the
 * last cell, where subsequent cursor positioning becomes ambiguous. */
#define PENDING_GLYPHS_MAX 256

struct render_state {
    int continuation_column;
    int columns;
    size_t cursor;
    input_render_cb emit;
    void *user;

    int row;
    int col;

    int cursor_row;
    int cursor_col;
    int cursor_resolved;

    struct render_glyph pending[PENDING_GLYPHS_MAX];
    size_t pending_len;
    int pending_width;
    int pending_after_space;
};

static int cursor_in_range(size_t cursor, size_t start, size_t consumed)
{
    return cursor >= start && cursor < start + consumed;
}

static void emit_glyph(struct render_state *state, const struct render_glyph *glyph)
{
    if (!state->cursor_resolved && cursor_in_range(state->cursor, glyph->offset, glyph->consumed)) {
        state->cursor_row = state->row;
        state->cursor_col = state->col;
        state->cursor_resolved = 1;
    }
    if (state->emit) {
        struct input_render_event event = {
            .kind = INPUT_RENDER_GLYPH,
            .bytes = glyph->bytes,
            .n = glyph->len,
            .width = glyph->width,
            .row = state->row,
            .col = state->col,
        };
        state->emit(&event, state->user);
    }
    state->col += glyph->width;
}

static void emit_row_break(struct render_state *state)
{
    state->row++;
    state->col = state->continuation_column;
    if (state->emit) {
        struct input_render_event event = {
            .kind = INPUT_RENDER_ROW_BREAK,
            .bytes = NULL,
            .n = 0,
            .width = 0,
            .row = state->row,
            .col = state->col,
        };
        state->emit(&event, state->user);
    }
}

static void flush_pending(struct render_state *state)
{
    for (size_t i = 0; i < state->pending_len; i++)
        emit_glyph(state, &state->pending[i]);
    state->pending_len = 0;
    state->pending_width = 0;
    state->pending_after_space = 0;
}

/* Clear the replay flag so later overflow in the same overlong token character-wraps. */
static void replay_pending_on_new_row(struct render_state *state)
{
    emit_row_break(state);
    for (size_t i = 0; i < state->pending_len; i++)
        emit_glyph(state, &state->pending[i]);
    state->pending_len = 0;
    state->pending_width = 0;
    state->pending_after_space = 0;
}

static void resolve_cursor(struct render_state *state)
{
    if (state->cursor_resolved)
        return;

    state->cursor_row = state->row;
    state->cursor_col = state->col;
    state->cursor_resolved = 1;
}

void input_core_render(const char *buf, size_t len, size_t cursor, int prompt_width,
                       int continuation_column, int columns, input_render_cb emit, void *user,
                       struct input_layout *out)
{
    struct render_state state = {
        .continuation_column = continuation_column,
        .columns = columns,
        .cursor = cursor,
        .emit = emit,
        .user = user,
        .row = 0,
        .col = prompt_width,
    };

    size_t offset = 0;
    while (offset < len) {
        unsigned char byte = (unsigned char)buf[offset];
        if (byte == '\n') {
            flush_pending(&state);
            /* A cursor on '\n' remains at the end of the current row. */
            if (!state.cursor_resolved && state.cursor == offset) {
                state.cursor_row = state.row;
                state.cursor_col = state.col;
                state.cursor_resolved = 1;
            }
            emit_row_break(&state);
            offset++;
            continue;
        }

        struct render_glyph glyph;
        if (!decode_glyph(buf, len, offset, &glyph))
            break;

        /* Flush a full pending buffer before a combining mark so it remains attached to
         * its host glyph and cursor resolution uses the post-flush position. */
        if (glyph.width == 0) {
            if (state.pending_len < PENDING_GLYPHS_MAX) {
                state.pending[state.pending_len++] = glyph;
            } else {
                flush_pending(&state);
                emit_glyph(&state, &glyph);
            }
            offset += glyph.consumed;
            continue;
        }

        if (glyph.is_space) {
            /* Drop an overflowing boundary space rather than leading the next row with it. */
            int space_overflows = (state.columns > 0 &&
                                   state.col + state.pending_width + glyph.width >= state.columns);
            flush_pending(&state);
            if (space_overflows && state.col > state.continuation_column) {
                if (!state.cursor_resolved &&
                    cursor_in_range(state.cursor, glyph.offset, glyph.consumed)) {
                    state.cursor_row = state.row;
                    state.cursor_col = state.col;
                    state.cursor_resolved = 1;
                }
                emit_row_break(&state);
            } else {
                emit_glyph(&state, &glyph);
                state.pending_after_space = 1;
            }
            offset += glyph.consumed;
            continue;
        }

        int prospective_column = state.col + state.pending_width + glyph.width;
        if (state.columns > 0 && prospective_column >= state.columns) {
            if (state.pending_len > 0 && state.pending_after_space &&
                state.col > state.continuation_column) {
                /* Never replay onto another empty row. */
                replay_pending_on_new_row(&state);
            } else {
                flush_pending(&state);
                if (state.col > state.continuation_column)
                    emit_row_break(&state);
            }
        }

        if (state.pending_len < PENDING_GLYPHS_MAX) {
            state.pending[state.pending_len++] = glyph;
            state.pending_width += glyph.width;
        } else {
            /* The fixed pending buffer bounds memory for unbroken tokens. */
            flush_pending(&state);
            if (state.columns > 0 && state.col + glyph.width >= state.columns &&
                state.col > state.continuation_column)
                emit_row_break(&state);
            emit_glyph(&state, &glyph);
        }
        offset += glyph.consumed;
    }

    if (state.pending_len > 0 && state.columns > 0 &&
        state.col + state.pending_width >= state.columns && state.pending_after_space &&
        state.col > state.continuation_column) {
        replay_pending_on_new_row(&state);
    } else {
        flush_pending(&state);
    }

    resolve_cursor(&state);

    if (out) {
        out->cursor_row = state.cursor_row;
        out->cursor_col = state.cursor_col;
        out->end_row = state.row;
        out->end_col = state.col;
        out->total_rows = state.row + 1;
    }
}

void input_core_compute_layout(const char *buf, size_t len, size_t cursor, int prompt_width,
                               int columns, struct input_layout *out)
{
    input_core_render(buf, len, cursor, prompt_width, prompt_width, columns, NULL, NULL, out);
}

struct render_window {
    int first_row;
    int last_row;
    input_render_cb emit;
    void *user;
};

static void emit_window_event(const struct input_render_event *event, void *user)
{
    struct render_window *window = user;

    if (event->row < window->first_row || event->row > window->last_row)
        return;
    window->emit(event, window->user);
}

void input_core_render_window(const char *buf, size_t len, size_t cursor, int prompt_width,
                              int continuation_column, int columns, int first_row, int last_row,
                              input_render_cb emit, void *user, struct input_layout *out)
{
    struct render_window window = {
        .first_row = first_row,
        .last_row = last_row,
        .emit = emit,
        .user = user,
    };
    input_core_render(buf, len, cursor, prompt_width, continuation_column, columns,
                      emit ? emit_window_event : NULL, emit ? &window : NULL, out);
}

int input_core_window_top(int prev_top, int cursor_row, int total_rows, int rows)
{
    if (rows <= 0)
        return 0;
    int max_top = total_rows - rows;
    if (max_top < 0)
        max_top = 0;
    int top = prev_top;
    if (top < 0)
        top = 0;
    if (top > max_top)
        top = max_top;
    if (cursor_row < top)
        top = cursor_row;
    else if (cursor_row > top + rows - 1)
        top = cursor_row - (rows - 1);
    return top;
}

/* ---------------- escape-sequence decoder ---------------- */

/* Capture through the final byte and return -1 on EOF, overflow, or the read cap. Continue
 * after overflow so the unconsumed tail cannot leak into text input. rxvt uses '$' as a final for
 * modified tilde keys even though ECMA-48 classifies it as an intermediate byte. */
static int read_csi_seq(input_byte_reader read, void *user, char *seq, int cap)
{
    const int MAX_READS = 64;
    int n = 0;
    int overflowed = 0;
    for (int reads = 0; reads < MAX_READS; reads++) {
        int b = read(user);
        if (b < 0)
            return -1;
        if (n + 1 < cap)
            seq[n++] = (char)b;
        else
            overflowed = 1;
        if ((b >= 0x40 && b <= 0x7E) || b == '$') {
            if (overflowed)
                return -1;
            seq[n] = '\0';
            return n;
        }
    }
    return -1;
}

/* Parse xterm's "1;<modifier><final>" form, where the modifier is one plus a
 * Shift/Alt/Ctrl/Meta bitmask. Ignore trailing parameters such as kitty's key-event type and
 * return the unmodified value for malformed or short forms. */
static int parse_xterm_mod(const char *seq, int n)
{
    if (n < 4 || seq[0] != '1' || seq[1] != ';')
        return 1;
    int v = 0;
    for (int i = 2; i < n - 1; i++) {
        if (seq[i] == ';')
            break;
        if (seq[i] < '0' || seq[i] > '9')
            return 1;
        v = v * 10 + (seq[i] - '0');
        if (v > 99)
            return 1;
    }
    return v ? v : 1;
}

/* Shift alone remains character motion because the editor has no selection state. */
static int xterm_mod_implies_word(int mod)
{
    if (mod < 1)
        return 0;
    return ((mod - 1) & 0xE) != 0;
}

static enum input_action arrow_to_action(char final, int word)
{
    switch (final) {
    case 'A':
        return INPUT_ACTION_HISTORY_PREV;
    case 'B':
        return INPUT_ACTION_HISTORY_NEXT;
    case 'C':
        return word ? INPUT_ACTION_MOVE_WORD_RIGHT : INPUT_ACTION_MOVE_RIGHT;
    case 'D':
        return word ? INPUT_ACTION_MOVE_WORD_LEFT : INPUT_ACTION_MOVE_LEFT;
    case 'H':
        return INPUT_ACTION_LINE_START;
    case 'F':
        return INPUT_ACTION_LINE_END;
    }
    return INPUT_ACTION_NONE;
}

enum input_action input_core_decode_escape(input_byte_reader read, void *user)
{
    int b = read(user);
    if (b < 0)
        return INPUT_ACTION_NONE; /* bare ESC */

    /* iTerm2's Esc+ mode prefixes Option cursor keys with another ESC. Bound stripping
     * prevents a stream of ESC bytes from monopolizing the decoder. */
    int meta = 0;
    for (int strip = 0; b == 0x1b && strip < 4; strip++) {
        b = read(user);
        if (b < 0)
            return INPUT_ACTION_NONE;
        meta = 1;
    }
    if (b == 0x1b)
        return INPUT_ACTION_NONE;

    if (b == '[') {
        char seq[32];
        int n = read_csi_seq(read, user, seq, sizeof(seq));
        if (n < 0)
            return INPUT_ACTION_NONE;

        if (n == 1) {
            /* rxvt lowercase CSI arrows denote Shift, which remains character motion. */
            char final = seq[0];
            if (final >= 'a' && final <= 'd')
                final -= 'a' - 'A';
            return arrow_to_action(final, meta);
        }
        if (strcmp(seq, "200~") == 0)
            return INPUT_ACTION_PASTE_BEGIN;

        /* xterm and rxvt vary the final byte for modified tilde keys, but modifiers do not
         * change these actions. Require a one-digit key code so function keys cannot alias
         * Home, End, or Delete. */
        char final = seq[n - 1];
        if ((final == '~' || final == '^' || final == '$' || final == '@') &&
            (n == 2 || (n >= 4 && seq[1] == ';'))) {
            switch (seq[0]) {
            case '1':
            case '7':
                return INPUT_ACTION_LINE_START;
            case '4':
            case '8':
                return INPUT_ACTION_LINE_END;
            case '3':
                return INPUT_ACTION_DELETE_FWD;
            case '5':
                return INPUT_ACTION_PAGE_UP;
            case '6':
                return INPUT_ACTION_PAGE_DOWN;
            }
        }
        /* Alt, Ctrl, and Meta select word motion; Home and End ignore modifiers. */
        if (n >= 4 && seq[0] == '1' && seq[1] == ';') {
            int mod = parse_xterm_mod(seq, n);
            return arrow_to_action(seq[n - 1], xterm_mod_implies_word(mod) || meta);
        }
        return INPUT_ACTION_NONE;
    }

    if (b == 'O') {
        /* SS3 supports plain, xterm-modified, and rxvt lowercase cursor keys. Reading
         * through the final byte also drains unrecognized SS3 function keys. */
        char seq[32];
        int n = read_csi_seq(read, user, seq, sizeof(seq));
        if (n < 0)
            return INPUT_ACTION_NONE;
        char final = seq[n - 1];
        int word;
        if (final >= 'a' && final <= 'd') {
            final -= 'a' - 'A';
            word = 1;
        } else {
            word = xterm_mod_implies_word(parse_xterm_mod(seq, n));
        }
        return arrow_to_action(final, word || meta);
    }

    /* Alt+Enter is the newline fallback for terminals without distinct Shift+Enter. */
    if (b == '\r' || b == '\n')
        return INPUT_ACTION_INSERT_NEWLINE;

    /* macOS Terminal emits these Meta bindings when Option is configured as Meta. */
    switch (b) {
    case 'b':
    case 'B':
        return INPUT_ACTION_MOVE_WORD_LEFT;
    case 'f':
    case 'F':
        return INPUT_ACTION_MOVE_WORD_RIGHT;
    case 'd':
    case 'D':
        return INPUT_ACTION_KILL_WORD_FWD;
    case 0x7f: /* Alt+Backspace (most terminals) */
    case 0x08: /* Alt+Backspace (terminals that map Backspace to ^H) */
        return INPUT_ACTION_KILL_WORD_BACK_ALNUM;
    }
    return INPUT_ACTION_NONE;
}
