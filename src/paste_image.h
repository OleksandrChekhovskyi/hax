/* SPDX-License-Identifier: MIT */
#ifndef HAX_PASTE_IMAGE_H
#define HAX_PASTE_IMAGE_H

#include <stddef.h>

/*
 * Ctrl-V paste policy for the REPL prompt, registered with the editor
 * via input_set_paste_cb. A clipboard image is persisted to a tracked
 * temp file (system/tempfiles.h) and handed back as a
 * "[pasted image: <path>] " marker — the model fetches the pixels with
 * the `read` tool, which owns validation, size budgets, and
 * text-only-model fallbacks. With no image on offer, falls back to the
 * clipboard's text so the binding never feels dead.
 *
 * Returns a malloc'd string to insert at the cursor, or NULL to insert
 * nothing (empty clipboard, no helper available).
 */
char *paste_image_capture(void);

/* CRLF/CR -> LF and NUL-strip, in place; returns the new length.
 * Matches the editor's bracketed-paste normalization so hook-pasted
 * text and terminal-pasted text agree. Exposed for unit tests. */
size_t paste_image_normalize_text(char *s, size_t n);

/* When `text` consists entirely of file:// URIs (one per line — the
 * shape a file-manager "copy" or a drag-and-drop pastes), return a
 * malloc'd replacement with each URI percent-decoded to a plain path,
 * wrapped in a pasted-image marker when the file sniffs as an image.
 * Returns NULL when the text isn't a URI list. Exposed for unit tests. */
char *paste_image_uris_to_paths(const char *text);

#endif /* HAX_PASTE_IMAGE_H */
