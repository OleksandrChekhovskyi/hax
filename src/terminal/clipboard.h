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

/* Budget for one whole paste operation. Reading runs while the editor
 * holds the terminal in raw mode, where Ctrl-C is just a queued byte —
 * stalled helpers (classic case: an X11 selection owner that never
 * answers) must time out rather than freeze the REPL. One absolute
 * deadline spans every helper the fallback chains try, so stalls can't
 * stack; healthy helpers answer in milliseconds, and the budget only
 * needs to cover the slowest legitimate run (osascript cold-starting
 * on a large screenshot). */
#define CLIPBOARD_PASTE_TIMEOUT_MS 5000

/*
 * Read an image from the user's clipboard: wl-paste (Wayland), xclip
 * (X11), then osascript (macOS) — each helper that isn't installed or
 * has no image on offer fails fast and falls through. Returns malloc'd
 * raw image bytes with *out_len set, or NULL when no helper produced
 * any. Bytes are unvalidated; callers sniff before trusting them.
 *
 * `deadline_ms` is an absolute monotonic_ms() instant; helper attempts
 * past it are skipped (callers typically pass
 * monotonic_ms() + CLIPBOARD_PASTE_TIMEOUT_MS, sharing one deadline
 * with the follow-up text read).
 *
 * No OSC 52 analog exists for reading, so inside an SSH session this
 * reads the remote host's clipboard — usually empty on a headless box,
 * which surfaces as NULL rather than an error.
 */
char *clipboard_paste_image(size_t *out_len, long deadline_ms);

/*
 * Text sibling of clipboard_paste_image: pbpaste / wl-paste / xclip /
 * xsel, under the same deadline contract. Returns malloc'd
 * NUL-terminated bytes (*out_len excludes the NUL) or NULL. Raw
 * clipboard content — callers normalize line endings / strip NULs as
 * their context needs.
 */
char *clipboard_paste_text(size_t *out_len, long deadline_ms);

#endif /* HAX_CLIPBOARD_H */
