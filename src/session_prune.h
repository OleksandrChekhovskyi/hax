/* SPDX-License-Identifier: MIT */
#ifndef HAX_SESSION_PRUNE_H
#define HAX_SESSION_PRUNE_H

#include <time.h>

/* Cutoff derived from session_retention_days, or 0 when pruning is disabled. */
time_t session_retention_cutoff(void);

/* Delete canonical session files across every cwd directory whose last
 * append predates `cutoff`. `exclude_path`, when non-NULL, is never removed.
 * Active writers are skipped. Exposed as the deterministic sweep primitive;
 * normal startup uses the throttled background wrapper below. Returns 0 on a
 * complete best-effort sweep. */
int session_prune_before(time_t cutoff, const char *exclude_path);

/* Start a best-effort global prune when the configured daily sweep is due.
 * The work runs on a joinable background job; call session_prune_shutdown at
 * process teardown. Both entry points are idempotent. */
void session_prune_start(const char *exclude_path);
void session_prune_shutdown(void);

#endif /* HAX_SESSION_PRUNE_H */
