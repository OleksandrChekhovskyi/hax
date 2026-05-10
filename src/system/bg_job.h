/* SPDX-License-Identifier: MIT */
#ifndef HAX_BG_JOB_H
#define HAX_BG_JOB_H

/* Per-job background thread with cooperative cancel and join.
 *
 * Each owner spawns a job for its own state, holds the handle, and is
 * responsible for joining (with a cancel signal first if needed) before
 * tearing down any memory the worker might still be touching. There is
 * no global registry: destroying one provider mid-run cleans up its own
 * jobs without affecting any other, which keeps the lifetime contract
 * local instead of "remember to call bg_job_join_all() at exit".
 *
 * Worker contract:
 *   - bg_job_fn receives its own job handle (so it can poll bg_job_cancelled())
 *     and the user-supplied `arg` pointer.
 *   - The worker is responsible for freeing `arg` before returning.
 *   - The worker must not touch state outside `arg` and atomic fields
 *     on objects whose lifetime is guaranteed to outlive the join.
 *
 * Lifecycle:
 *   bg_job_spawn  -> launches a joinable thread
 *   bg_job_cancel -> sets the per-job cancel flag (idempotent)
 *   bg_job_join   -> waits for the worker to return, then frees the handle
 *
 * Every successful bg_job_spawn must be paired with exactly one bg_job_join.
 * No "detach" mode -- that brings back the cross-cutting lifetime
 * problems each owner is trying to manage locally. */

struct bg_job;

typedef void (*bg_job_fn)(struct bg_job *job, void *arg);

/* Spawn a joinable thread running fn(job, arg). Returns NULL on
 * pthread_create failure (rare). The typical caller fallback is to
 * skip whatever optional work the job would have done. */
struct bg_job *bg_job_spawn(bg_job_fn fn, void *arg);

/* Set the job's atomic cancel flag. Workers observe via bg_job_cancelled().
 * Idempotent; safe to call before, during, or after the worker runs. */
void bg_job_cancel(struct bg_job *job);

/* Worker-side cancel poll. NULL job is treated as not-cancelled so
 * sentinel writes don't crash. */
int bg_job_cancelled(const struct bg_job *job);

/* http_tick_cb-shaped wrapper that workers pass straight to http_get /
 * http_sse_post: `http_get(..., bg_job_tick, job, &out)`. Returns non-zero
 * once `job` has been cancelled, aborting the in-flight transfer. */
int bg_job_tick(void *job);

/* Wait for the worker to finish, then free the handle. After this
 * returns, `job` is invalid. NULL is a no-op. */
void bg_job_join(struct bg_job *job);

#endif /* HAX_BG_JOB_H */
