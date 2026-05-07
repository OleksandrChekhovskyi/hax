/* SPDX-License-Identifier: MIT */
#ifndef HAX_ENV_H
#define HAX_ENV_H

/* Build the trailing portion of the system prompt: an <env> metadata block
 * followed by any discovered AGENTS.md files. The caller is expected to
 * append this to the base system prompt (role/instructions first, env and
 * project context after — order matches Claude Code, opencode, pi-mono,
 * codex). Returns a freshly-allocated string the caller must free, or
 * NULL if nothing would be emitted — i.e. when the env block is disabled
 * via HAX_NO_ENV AND either AGENTS.md is disabled via HAX_NO_AGENTS_MD or
 * no AGENTS.md files and no SKILL.md files were found.
 *
 * Built once at session start and reused across turns so the assembled
 * system prompt stays byte-identical and cache-friendly — every field
 * (cwd, os, shell, model, is_git_repo, preferred_commands) reflects
 * process startup state and is fixed for the session's lifetime. The
 * model has tools if it really needs the wall clock, so we deliberately
 * don't emit a `date:` field that would drift across midnight and
 * invalidate the cache.
 *
 * `model` is interpolated into the env block; pass NULL or "" to omit it.
 *
 * AGENTS.md discovery walks cwd → filesystem root looking for a `.git`
 * marker. If found, every AGENTS.md from the project root down to cwd
 * (inclusive) is emitted, farthest-first so the closest file takes
 * precedence. If no `.git` is found anywhere, only the cwd-level AGENTS.md
 * (if any) is considered — we don't pull in unrelated files from /home or
 * the filesystem root. A global file at
 * `${XDG_CONFIG_HOME:-$HOME/.config}/hax/AGENTS.md` is loaded first
 * (lowest priority).
 *
 * Skills are discovered alongside AGENTS.md (gated by the same
 * HAX_NO_AGENTS_MD knob). Each `.agents/skills/<name>/SKILL.md` (cwd) and
 * `${XDG_CONFIG_HOME:-$HOME/.config}/hax/skills/<name>/SKILL.md` (global)
 * is enumerated; YAML frontmatter `description:` is parsed (if present)
 * and emitted as a single sorted markdown list. Project entries shadow
 * global entries with the same name. */
char *env_build_suffix(const char *model);

#endif /* HAX_ENV_H */
