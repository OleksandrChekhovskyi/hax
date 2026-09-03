/* SPDX-License-Identifier: MIT */
#ifndef HAX_TEXT_PLACEHOLDER_H
#define HAX_TEXT_PLACEHOLDER_H

/* Brace placeholders in configuration strings: "{name}" stands for a value resolved later. */

/* True when text contains "{name}". */
int placeholder_present(const char *text, const char *name);

/* Owned copy of text with every "{name}" replaced by value. */
char *placeholder_expand(const char *text, const char *name, const char *value);

#endif /* HAX_TEXT_PLACEHOLDER_H */
