/* SPDX-License-Identifier: MIT */
#ifndef HAX_INPUT_CORE_H
#define HAX_INPUT_CORE_H

#include <stddef.h>
#include <termios.h>

/*
 * Pure (no-IO) line-editor state and operations, exposed for testing.
 * `input.c` owns the tty/IO layer and drives these ops in response to
 * decoded keypresses; tests can drive them directly without a tty.
 *
 * `struct input` is the shared state between the two layers — its tty
 * fields (saved_termios, raw_active) are touched only by input.c and
 * stay zero / unused in headless test contexts.
 */

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

    /* paint state — offsets within the edit area painted on screen */
    int last_cursor_row;
    int last_rows;

    /* per-call render cache */
    const char *prompt;
    int term_cols;

    /* tty (input.c only) */
    struct termios saved_termios;
    int raw_active;

    /* on-disk history persistence (input.c only). NULL = disabled. Set
     * by input_history_open; once set, input_history_add additionally
     * appends each accepted entry to this file. */
    char *persist_path;

    /* Ctrl-T hook (input.c only). When set, Ctrl-T at the prompt drops
     * out of raw mode, calls `transcript_cb(transcript_user)`, and
     * repaints. The callback is expected to take over stdout for the
     * duration (typically: popen a pager and pipe rendered content).
     * NULL = Ctrl-T is a no-op. */
    void (*transcript_cb)(void *user);
    void *transcript_user;
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
void input_core_buf_set(struct input *in, const char *s);
void input_core_buf_insert(struct input *in, const char *bytes, size_t n);

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

/* In-memory history append with erasedups semantics: any prior exact
 * match is removed first, so a recalled entry bumps to the top instead
 * of duplicating. No-op for NULL/empty input or for an exact repeat of
 * the current most-recent entry (the erasedups would be a self-cancel
 * anyway, and skipping spares an on-disk record).
 *
 * Returns 1 if the entry actually changed history, 0 if it was skipped.
 * The IO-layer wrapper in input.c (`input_history_add`) uses the return
 * value to decide whether to append to the on-disk file. */
int input_core_history_add(struct input *in, const char *line);

/* Pure encode/decode for the on-disk one-line-per-record format. Encode
 * maps literal backslash -> "\\" and literal LF -> "\n"; decode reverses
 * those, leaving unknown escapes verbatim for forward compatibility.
 * Both return malloc'd strings the caller frees. */
char *input_core_history_encode(const char *s);
char *input_core_history_decode(const char *s, size_t n);

/* In-memory history cap. Older entries are evicted past this. The IO
 * layer in input.c uses it as the basis for its on-disk bloat threshold,
 * so it lives in the header rather than as duplicated constants. */
#define INPUT_CORE_HISTORY_MAX 1000

/* ---- layout / utf-8 ---- */

/* Display columns of `s` up to its first '\n' or end, treating CSI
 * sequences (\x1b[...<final>) as zero columns. Used to compute the
 * prompt's painted width. */
int input_core_prompt_width(const char *s);

/* Walk the buffer once, computing where the cursor lands on screen and
 * where the painted content ends. Mirrors the wrap behavior of
 * input.c's emit step: explicit '\n' resets to column `prompt_w` (so
 * continuation lines align with the first line's content); terminal
 * soft-wrap from auto-wrap lands at column 0. */
void input_core_compute_layout(const char *buf, size_t len, size_t cursor, int prompt_w, int cols,
                               struct input_layout *out);

/* Number of bytes in the UTF-8 sequence starting at leading byte `c`.
 * Returns 1 for a malformed leader. */
int input_core_utf8_seq_len(unsigned char c);

/* Display columns of the codepoint at buf[i..]. Writes the codepoint's
 * UTF-8 byte length to *consumed (always >= 1 when i < len). Returns:
 *   < 0 — non-printable: C0/C1 controls, DEL, malformed UTF-8, embedded
 *         NUL. Caller should substitute (a single safe glyph) rather
 *         than emit the raw bytes — even UTF-8-encoded C1 controls can
 *         be interpreted by some terminals.
 *   == 0 — combining mark or zero-width joiner; rides on prior glyph.
 *   > 0 — printable codepoint occupying that many columns.
 * Tab and newline are NOT special-cased — callers handle them. */
int input_core_codepoint_width(const char *buf, size_t len, size_t i, size_t *consumed);

/* Spaces per tab. Layout and rendering both expand a tab to exactly
 * this many columns regardless of the current column — soft-tab style,
 * so each tab in the buffer advances the cursor by a consistent amount
 * (true tab-stop snapping makes the first tab after a prompt feel
 * short). We own both ends so the value is a free parameter. */
#define INPUT_CORE_TAB_WIDTH 4

/* ---- escape-sequence decoder ----
 *
 * The decoder is the pure (no-IO) heart of the line editor's terminal
 * input handling: it recognizes the various CSI / SS3 / rxvt / iTerm2
 * encodings emitted by real terminals and returns an action enum the
 * IO layer maps onto buffer mutations. Splitting it out from input.c
 * means we can exercise every encoding from unit tests by feeding a
 * byte array, without spinning up a pty. */

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
    INPUT_ACTION_KILL_WORD_FWD,
    INPUT_ACTION_KILL_WORD_BACK_ALNUM,
    INPUT_ACTION_INSERT_NEWLINE,
    INPUT_ACTION_PASTE_BEGIN, /* "ESC [ 2 0 0 ~" — caller reads body */
};

/* Byte-source callback: returns 0..255 on success or -1 on EOF /
 * timeout / cancel. The decoder calls this once per byte it needs;
 * tests pass a byte-array reader, the real path passes a poll-backed
 * adapter. */
typedef int (*input_byte_reader)(void *user);

/* Decode the bytes following an ESC byte (the leading ESC has already
 * been consumed by the caller). Reads as many bytes as the encoding
 * requires via `read`, internally bounded against runaway streams
 * (read cap, fixed seq buffer, ESC-strip cap). Returns
 * INPUT_ACTION_NONE for unknown payloads, partial sequences, or
 * abandoned reads. */
enum input_action input_core_decode_escape(input_byte_reader read, void *user);

#endif /* HAX_INPUT_CORE_H */
