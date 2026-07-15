/* SPDX-License-Identifier: MIT */
#ifndef HAX_AGENT_ENV_H
#define HAX_AGENT_ENV_H

/* Build a malloc'd system-prompt suffix containing the enabled Subagents,
 * Environment, Project Context, and skills sections; return NULL if empty.
 * `model` labels the Environment section when non-empty.
 *
 * The suffix is reused across turns for prompt-cache stability and omits
 * wall-clock data. AGENTS.md loads global context first, then project root →
 * cwd; without a Git root, project context is limited to cwd. Project skills
 * shadow global skills with the same name. */
char *agent_env_build_suffix(const char *model);

#endif /* HAX_AGENT_ENV_H */
