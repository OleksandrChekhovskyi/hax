/* SPDX-License-Identifier: MIT */
#ifndef HAX_AGENT_ENV_H
#define HAX_AGENT_ENV_H

/* Build the prompt suffix (<env>, AGENTS.md, skills). Returns malloc'd text
 * or NULL when empty. Rebuilt at session start and after model/provider changes,
 * then reused for prompt-cache stability. AGENTS.md loads global first, then
 * project root → cwd (or cwd only without .git); project skills shadow global. */
char *agent_env_build_suffix(const char *model);

#endif /* HAX_AGENT_ENV_H */
