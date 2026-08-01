/* SPDX-License-Identifier: MIT */
#ifndef HAX_TOOLS_BASH_ENV_H
#define HAX_TOOLS_BASH_ENV_H

/* The child environment stamp and startup guard must use the same recursion cap. */
#define HAX_SUBAGENT_MAX_DEPTH 3

/* Publish the effective provider selection for child processes. A NULL or empty provider clears
 * the export; NULL model and effort values are exported as empty strings. Called only on the
 * dispatch thread. */
void bash_env_set_selection(const char *provider, const char *model, const char *effort);

/* Return a malloc'd environment vector whose entries remain borrowed. */
char **bash_build_child_env(void);

#endif /* HAX_TOOLS_BASH_ENV_H */
