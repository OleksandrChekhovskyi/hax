/* SPDX-License-Identifier: MIT */
#ifndef HAX_PATH_PREPROCESS_H
#define HAX_PATH_PREPROCESS_H

/* preprocess_args hook (see struct tool) shared by the file tools
 * (read / edit / write). When the `path` argument expands to an absolute
 * path strictly under the process cwd, rewrite it to the cwd-relative
 * form and return the re-serialized args JSON (caller frees). Returns
 * NULL when there's nothing to rewrite — a relative path, a path outside
 * cwd, or malformed args — leaving the agent to use the model's original.
 *
 * The motivating case mirrors bash_cd_strip: some models (notably the
 * Qwen family) emit a redundant absolute path on every file call. Since
 * the agent never chdirs mid-session, opening the relative form against
 * cwd hits the same file, so the rewrite is a filesystem no-op that just
 * trims the displayed header and diff labels. Like cd-strip, this only
 * shadows the local invocation — the model's original args stay in
 * conversation history. */
char *tool_normalize_path_args(const char *args_json);

#endif /* HAX_PATH_PREPROCESS_H */
