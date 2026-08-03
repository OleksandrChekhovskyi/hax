/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_CODEX_SETTINGS_H
#define HAX_PROVIDERS_CODEX_SETTINGS_H

#include <stddef.h>

/* Read a string value assigned to `key` at the top level of a TOML document. Scanning stops at the
 * first table header, so a key of the same name nested under a table is never reported. Returns
 * NULL when the key is absent or its value is not a quoted string; the caller owns the result.
 *
 * This understands only the subset of TOML that Codex writes: one assignment per line, basic and
 * literal strings, and `#` comments. Multi-line strings and dotted keys are not supported. */
char *codex_toml_top_level_string(const char *contents, size_t contents_len, const char *key);

/* Read the model and reasoning effort defaults from ~/.codex/config.toml. Each output is set to a
 * value the caller owns, or NULL when the file, the key, or its value is missing or empty. */
void codex_load_settings(char **model, char **effort);

#endif /* HAX_PROVIDERS_CODEX_SETTINGS_H */
