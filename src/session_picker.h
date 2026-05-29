/* SPDX-License-Identifier: MIT */
#ifndef HAX_SESSION_PICKER_H
#define HAX_SESSION_PICKER_H

/*
 * Interactive "which past conversation?" chooser, shared by the
 * --resume CLI flag and the /resume slash command. Lists the sessions
 * recorded for `cwd` newest-first (index, relative time, message count,
 * first prompt) and reads a numbered selection from stdin.
 *
 * Returns a malloc'd path to the chosen session file, or NULL when the
 * user cancels, there are no sessions, or stdin isn't a terminal (a
 * picker needs interactive input). `exclude_path`, when non-NULL, is
 * hidden from the list — used by /resume to drop the currently-live
 * session. Caller frees the returned path.
 */
char *session_picker_run(const char *cwd, const char *exclude_path);

#endif /* HAX_SESSION_PICKER_H */
