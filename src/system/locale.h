/* SPDX-License-Identifier: MIT */
#ifndef HAX_SYSTEM_LOCALE_H
#define HAX_SYSTEM_LOCALE_H

/* Set LC_CTYPE, and only LC_CTYPE, to a UTF-8 locale: LC_NUMERIC under the user's locale can put a
 * decimal comma in serialized JSON. Publishes the choice to the environment unless a non-UTF-8
 * LC_ALL is pinned there. Call before other initialization, and before any thread reads the
 * environment. */
void locale_init_utf8(void);
/* Return whether this process can decode and measure multibyte text. */
int locale_have_utf8(void);
/* Return the LC_CTYPE a child must be given to read what hax renders, or NULL when the environment
 * already supplies one — the usual case — or when no UTF-8 locale exists to give. Only a pinned
 * LC_ALL, which outranks the published LC_CTYPE, makes this necessary. Valid until the next
 * setlocale(). */
const char *locale_child_ctype_override(void);

#endif /* HAX_SYSTEM_LOCALE_H */
