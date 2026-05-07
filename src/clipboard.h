/* SPDX-License-Identifier: MIT */
#ifndef HAX_CLIPBOARD_H
#define HAX_CLIPBOARD_H

#include <stddef.h>

/* OSC 52 payload cap. Some terminals silently drop oversized OSC
 * sequences, so we reject early rather than let the user think a
 * paste-empty result was a successful copy. */
#define CLIPBOARD_OSC52_MAX_BYTES 100000

/*
 * Copy a chunk of text to the user's clipboard.
 *
 * Strategy:
 *  - Inside an SSH session (SSH_TTY/SSH_CONNECTION set) the host's
 *    native clipboard tools would write to the *remote* machine, which
 *    the user can't paste from. Use OSC 52 (DCS-wrapped when in tmux)
 *    so the bytes reach the local terminal emulator instead.
 *  - Locally, try the platform's native clipboard helper (pbcopy on
 *    macOS; wl-copy / xclip / xsel on Linux), then fall back to OSC 52
 *    if none are on PATH.
 *
 * Returns 0 on success. On failure, returns -1 and writes a short
 * human-readable reason to *err (statically allocated; do not free).
 * `err` may be NULL if the caller doesn't care.
 */
int clipboard_copy(const char *text, size_t len, const char **err);

/*
 * Build the OSC 52 escape sequence for `text`. When `tmux_wrap` is
 * non-zero, wrap the inner sequence in a DCS passthrough envelope so
 * tmux forwards it to the outer terminal.
 *
 * Returns NULL when len exceeds CLIPBOARD_OSC52_MAX_BYTES. On success,
 * returns a malloc'd NUL-terminated string; *out_len (when non-NULL)
 * gets the byte length excluding the NUL. Caller frees.
 *
 * Exposed so unit tests can verify the framing without driving real
 * clipboard I/O; `clipboard_copy` calls it internally.
 */
char *clipboard_osc52_sequence(const char *text, size_t len, int tmux_wrap, size_t *out_len);

#endif /* HAX_CLIPBOARD_H */
