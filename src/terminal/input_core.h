/* SPDX-License-Identifier: MIT */
#ifndef HAX_INPUT_CORE_H
#define HAX_INPUT_CORE_H

#include <stddef.h>
#include <termios.h>

/* Shared editor state and terminal-independent operations. */

/* `match` performs no I/O and returns the replacement span [start, end).
 * `pick` runs in cooked mode with the edit area erased and owns the terminal until it returns.
 * It returns a malloc'd replacement, or NULL to leave the buffer unchanged. */
struct input_modal_completer {
    int (*match)(const char *buf, size_t len, size_t cursor, size_t *start, size_t *end,
                 void *user);
    char *(*pick)(const char *token, void *user);
    void *user;
};

/* Slots for application-bound modal control keys (input_bind_modal_key).
 * Small on purpose: these are top-level views, not a general keymap. */
#define INPUT_MODAL_KEYS_MAX 4

struct input {
    /* current edit buffer (NUL-terminated, may contain '\n') */
    char *buf;
    size_t len, cap;
    size_t cursor; /* byte offset into buf */

    /* history (oldest first); hist_pos == hist_n means "current draft" */
    char **hist;
    size_t hist_n, hist_cap;
    size_t hist_pos;
    char *draft; /* saved buffer at first Up; restored on Down past end */

    /* The newest entry came from a session-only add; a persistent re-add of the
     * same line must still reach the file. */
    int hist_newest_unpersisted;

    /* Screen-relative state for repainting the visible edit area. */
    int painted_cursor_row;
    int painted_rows;
    int previous_paint_clipped;
    int window_top;
    int top_indicator_width;
    int hint_painted; /* the last paint drew the exit hint */

    int continuation_at_column_zero;
    const char *prompt; /* borrowed for the duration of input_readline */
    int display_columns;
    int terminal_rows; /* 0 disables viewport clipping */

    /* Terminal state */
    struct termios saved_termios;
    int raw_active;

    char *persist_path; /* owned; NULL disables persistence */

    /* Built-in editing keys take precedence; fn == NULL marks an unused slot. */
    struct input_modal_key {
        unsigned char key;
        void (*fn)(void *user);
        void *user;
    } modal_keys[INPUT_MODAL_KEYS_MAX];

    /* Borrowed; NULL makes Tab insert a literal tab. */
    const struct input_modal_completer *completer;

    /* Returns malloc'd insertion text. Empty bracketed pastes also invoke this hook. */
    char *(*paste_hook)(void *user);
    void *paste_hook_user;

    /* Returns a malloc'd replacement, or NULL to preserve the paste body. */
    char *(*paste_filter)(const char *text, void *user);
    void *paste_filter_user;

    /* Enter on an empty buffer submits when set. */
    int empty_submit;

    /* Ctrl-C on an empty buffer arms this; a consecutive Ctrl-C quits and
     * any other key disarms. */
    int exit_armed;

    char *preseed; /* owned; consumed by the next input_readline */
};

/* Result of input_core_compute_layout. All fields are 0-indexed offsets
 * within the edit area (row 0 = the prompt row, col 0 = the leftmost
 * column). `total_rows` is end_row + 1. */
struct input_layout {
    int cursor_row, cursor_col;
    int end_row, end_col;
    int total_rows;
};

/* ---- buffer ---- */
void input_core_set_buffer(struct input *in, const char *text);
void input_core_insert(struct input *in, const char *bytes, size_t len);

/* `body` must be NUL-terminated with len == strlen(body). Insert and free a non-NULL
 * filter result; insert `body` verbatim when the filter returns NULL. */
void input_core_commit_paste(struct input *in, const char *body, size_t len);

/* Replace buf[start..end) with `text` (NULL = delete the span), leaving
 * the cursor right after the inserted text. No-op when the span is out
 * of range. */
void input_core_replace_span(struct input *in, size_t start, size_t end, const char *text);

/* ---- motions / edits (operate on the buffer at in->cursor) ---- */
size_t input_core_line_start(const struct input *in);
size_t input_core_line_end(const struct input *in);
void input_core_move_left(struct input *in);
void input_core_move_right(struct input *in);
void input_core_move_word_left(struct input *in);
void input_core_move_word_right(struct input *in);
void input_core_delete_back(struct input *in);
void input_core_delete_fwd(struct input *in);
void input_core_kill_to_eol(struct input *in);
void input_core_kill_to_bol(struct input *in);
void input_core_kill_word_back(struct input *in);
void input_core_kill_word_back_alnum(struct input *in);
void input_core_kill_word_fwd(struct input *in);

/* ---- history ---- */
void input_core_history_prev(struct input *in);
void input_core_history_next(struct input *in);

/* Return the first case-sensitive substring match from `start`, stepping by `dir` (-1
 * older, +1 newer). Return -1 for no match, invalid direction, empty query, or invalid start. */
long input_core_history_search(const struct input *in, const char *query, long start, int dir);

/* Add a non-empty entry after removing prior exact matches. Return 1 when history changes;
 * return 0 for invalid input or a repeat of the newest entry. */
int input_core_history_add(struct input *in, const char *line);

/* Encode backslash as "\\" and LF as "\n"; decode reverses both and preserves unknown
 * escapes. Both return malloc'd strings. */
char *input_core_history_encode(const char *entry);
char *input_core_history_decode(const char *encoded, size_t len);

/* Older entries are evicted past this cap. */
#define INPUT_CORE_HISTORY_MAX 1000

/* ---- layout / utf-8 ---- */

/* Display columns of `prompt` up to its first '\n' or end, treating CSI
 * sequences (\x1b[...<final>) as zero columns. */
int input_core_prompt_width(const char *prompt);

/* Compute cursor and end positions using the renderer's word wrapping, tab expansion, and
 * unsafe-byte substitution. Continuation rows start at `prompt_width`. */
void input_core_compute_layout(const char *buf, size_t len, size_t cursor, int prompt_width,
                               int columns, struct input_layout *out);

/* Emit glyph and row-break events in visual order while computing layout. Word wrapping
 * prefers the last ASCII space and falls back to a mid-token break. `prompt_width` positions the
 * first row, `continuation_column` positions later rows, and `columns == 0` disables wrapping. */
enum input_render_kind {
    INPUT_RENDER_GLYPH,
    INPUT_RENDER_ROW_BREAK,
};

struct input_render_event {
    enum input_render_kind kind;
    const char *bytes; /* GLYPH: bytes to write (may point at a static substitute) */
    size_t n;          /* GLYPH: byte length */
    int width;         /* GLYPH: cell width */
    int row;           /* destination row (0 = first row) */
    int col;           /* GLYPH: column of this glyph; ROW_BREAK: indent of new row */
};

typedef void (*input_render_cb)(const struct input_render_event *ev, void *user);

void input_core_render(const char *buf, size_t len, size_t cursor, int prompt_width,
                       int continuation_column, int columns, input_render_cb emit, void *user,
                       struct input_layout *out);

/* Forward events for rows [first_row, last_row], including the break into first_row.
 * Layout output still describes the full buffer. */
void input_core_render_window(const char *buf, size_t len, size_t cursor, int prompt_width,
                              int continuation_column, int columns, int first_row, int last_row,
                              input_render_cb emit, void *user, struct input_layout *out);

/* Keep `prev_top` while the cursor is visible; otherwise move the window minimally and
 * clamp it to the content. */
int input_core_window_top(int prev_top, int cursor_row, int total_rows, int rows);

/* Tabs use a fixed width rather than terminal tab stops. */
#define INPUT_CORE_TAB_WIDTH 4

/* ---- terminal-independent escape-sequence decoder ---- */

enum input_action {
    INPUT_ACTION_NONE = 0, /* unknown / abandoned (timeout, overflow) */
    INPUT_ACTION_MOVE_LEFT,
    INPUT_ACTION_MOVE_RIGHT,
    INPUT_ACTION_MOVE_WORD_LEFT,
    INPUT_ACTION_MOVE_WORD_RIGHT,
    INPUT_ACTION_LINE_START,
    INPUT_ACTION_LINE_END,
    INPUT_ACTION_DELETE_FWD,
    INPUT_ACTION_HISTORY_PREV,
    INPUT_ACTION_HISTORY_NEXT,
    INPUT_ACTION_PAGE_UP,
    INPUT_ACTION_PAGE_DOWN,
    INPUT_ACTION_KILL_WORD_FWD,
    INPUT_ACTION_KILL_WORD_BACK_ALNUM,
    INPUT_ACTION_INSERT_NEWLINE,
    INPUT_ACTION_PASTE_BEGIN, /* body follows in the byte stream */
};

/* Return 0..255, or -1 when no byte is available. */
typedef int (*input_byte_reader)(void *user);

/* Decode bytes following an already-consumed ESC. Reads are bounded; unknown, partial, or
 * abandoned sequences return INPUT_ACTION_NONE. */
enum input_action input_core_decode_escape(input_byte_reader read, void *user);

#endif /* HAX_INPUT_CORE_H */
