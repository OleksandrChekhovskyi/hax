/* SPDX-License-Identifier: MIT */
#ifndef HAX_TEXT_SHELL_QUOTE_H
#define HAX_TEXT_SHELL_QUOTE_H

/* Return a newly allocated shell-safe, single-quoted copy. NULL becomes empty. */
char *shell_single_quote(const char *str);

#endif /* HAX_TEXT_SHELL_QUOTE_H */
