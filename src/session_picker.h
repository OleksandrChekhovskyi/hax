/* SPDX-License-Identifier: MIT */
#ifndef HAX_SESSION_PICKER_H
#define HAX_SESSION_PICKER_H

/*
 * Interactive "which past conversation?" chooser, shared by the
 * --resume CLI flag and the /resume slash command. Presents the sessions
 * recorded for `cwd` newest-first through the generic list picker (see
 * terminal/picker.h): the first prompt is the searchable row, the
 * relative time a dim detail column, so a long history is filtered by
 * typing rather than paged.
 *
 * Returns a malloc'd path to the chosen session file, or NULL when the
 * user cancels, there are no sessions, or stdin isn't a terminal (a
 * picker needs interactive input). `exclude_path`, when non-NULL, is
 * hidden from the list — used by /resume to drop the currently-live
 * session. Caller frees the returned path.
 *
 * `shown`, when non-NULL, is set to 1 iff an interactive picker was opened
 * for this call — an interactive terminal with at least one session — and 0
 * otherwise (non-tty, or nothing to resume, which prints its own note). It
 * exists for /resume's blank-line bookkeeping: it reports that the cursor
 * was left at the picker's start row, which holds whether the user
 * cancelled, selected, or picker_run() failed its raw-mode setup without
 * painting (all leave the cursor in that same spot). So a NULL return with
 * shown==1 just means "no session chosen", not "nothing was drawn".
 */
char *session_picker_run(const char *cwd, const char *exclude_path, int *shown);

#endif /* HAX_SESSION_PICKER_H */
