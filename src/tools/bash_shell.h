/* SPDX-License-Identifier: MIT */
#ifndef HAX_BASH_SHELL_H
#define HAX_BASH_SHELL_H

/* Resolve bash.shell → PATH bash → /bin/bash → /bin/sh; return a malloc'd path. */
char *bash_resolve_shell(void);

#endif /* HAX_BASH_SHELL_H */
