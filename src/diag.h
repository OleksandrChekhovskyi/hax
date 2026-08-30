/* SPDX-License-Identifier: MIT */
#ifndef HAX_DIAG_H
#define HAX_DIAG_H

/* Emit one `hax: <message>` line to stderr, without changing control flow. Callers supply no prefix
 * or newline. hax_err uses the error color and hax_warn the warning color on terminals. */
__attribute__((format(printf, 1, 2))) void hax_err(const char *format, ...);
__attribute__((format(printf, 1, 2))) void hax_warn(const char *format, ...);

/* Monotonic count of completed hax_err() and hax_warn() writes. */
unsigned long hax_diag_sequence(void);

#endif /* HAX_DIAG_H */
