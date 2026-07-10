/* SPDX-License-Identifier: MIT */
#ifndef HAX_BUSY_H
#define HAX_BUSY_H

/*
 * Foreground busy window: one spinner plus an armed Esc watcher around a
 * blocking network fetch on the REPL foreground path (the /model catalog
 * fetch, /usage). Thread busy_tick into the fetch as its http_tick_cb so
 * Esc aborts the transfer; busy_end reports whether that happened, so the
 * caller can treat cancellation as "never mind" instead of a failure.
 * Both parts are no-ops on a non-TTY, so scripted runs are unaffected.
 */

struct busy;

/* Show a spinner with `label` and arm the Esc watcher. */
struct busy *busy_begin(const char *label);

/* http_tick_cb-shaped cancel hook for the blocking call. `user` unused. */
int busy_tick(void *user);

/* Hide the spinner, disarm the watcher (draining queued input so the Esc
 * doesn't leak into whatever reads stdin next), free `b`. Returns 1 when
 * Esc was pressed during the window, 0 otherwise — and on 1 has already
 * printed the dim "[interrupted]" marker line, so callers just abandon
 * the command without emitting anything else. */
int busy_end(struct busy *b);

#endif /* HAX_BUSY_H */
