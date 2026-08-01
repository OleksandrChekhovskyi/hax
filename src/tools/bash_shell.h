/* SPDX-License-Identifier: MIT */
#ifndef HAX_TOOLS_BASH_SHELL_H
#define HAX_TOOLS_BASH_SHELL_H

/* Resolve bash.shell → PATH bash → /bin/bash → /bin/sh; return a malloc'd path. */
char *bash_resolve_shell(void);

#endif /* HAX_TOOLS_BASH_SHELL_H */
