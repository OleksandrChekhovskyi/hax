/* SPDX-License-Identifier: MIT */
#ifndef HAX_AGENT_ENV_H
#define HAX_AGENT_ENV_H

/* Build the enabled model-facing context sections:
 *
 * - background-task and subagent operating guidance;
 * - working directory, home, OS, shell, model, repository, and command-line tool facts;
 * - global and project AGENTS.md instructions;
 * - discovered skill names, descriptions, and reusable SKILL.md paths.
 *
 * `model` is borrowed and labels the Environment section when non-empty. Returns an allocated
 * prompt suffix, or NULL when every section is empty or disabled. The snapshot intentionally omits
 * wall-clock data so the caller can reuse it across turns for prompt-cache stability.
 *
 * Global AGENTS.md precedes project files from repository root to cwd. Nearer project skills shadow
 * global skills. Without a Git root, project discovery is limited to cwd. */
char *agent_env_build_suffix(const char *model);

#endif /* HAX_AGENT_ENV_H */
