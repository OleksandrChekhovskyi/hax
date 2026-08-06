/* SPDX-License-Identifier: MIT */
#ifndef HAX_TERMINAL_CLIPBOARD_H
#define HAX_TERMINAL_CLIPBOARD_H

#include <stddef.h>

/* Terminals may silently discard larger OSC 52 payloads. */
#define CLIPBOARD_OSC52_MAX_BYTES 100000

/* Copy `text` to the user's clipboard. Local native helpers are preferred; SSH sessions and the
 * local fallback use OSC 52, with tmux passthrough when needed. Return 0 on success or -1 on
 * failure. When `error` is non-NULL, failure sets it to a borrowed static message. */
int clipboard_copy(const char *text, size_t text_len, const char **error);

/* Build an OSC 52 sequence for `text`, optionally wrapped for tmux passthrough. Return an
 * allocated, NUL-terminated sequence, or NULL when `text_len` exceeds CLIPBOARD_OSC52_MAX_BYTES.
 * `out_len` may receive the sequence length excluding the terminator. */
char *clipboard_osc52_sequence(const char *text, size_t text_len, int tmux_wrap, size_t *out_len);

/* Clipboard reads run while the editor is in raw mode, so all fallback attempts share one bounded
 * operation deadline. */
#define CLIPBOARD_PASTE_TIMEOUT_MS 5000

/* Read unvalidated image bytes from the platform clipboard. Return an allocated buffer and set
 * `out_len`, or return NULL on failure or when no supported image is available. `deadline_ms` is an
 * absolute monotonic_ms() instant shared by every helper attempt. In SSH sessions this reads the
 * remote host's clipboard because OSC 52 has no read operation. */
char *clipboard_paste_image(size_t *out_len, long deadline_ms);

/* Read text from the platform clipboard under the same deadline contract. Return allocated,
 * NUL-terminated bytes and set `out_len` to their length excluding the terminator. Return NULL on
 * failure or for an empty clipboard. The bytes are otherwise unnormalized. */
char *clipboard_paste_text(size_t *out_len, long deadline_ms);

#endif /* HAX_TERMINAL_CLIPBOARD_H */
