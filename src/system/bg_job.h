/* SPDX-License-Identifier: MIT */
#ifndef HAX_SYSTEM_BG_JOB_H
#define HAX_SYSTEM_BG_JOB_H

struct bg_job;

typedef void (*bg_job_fn)(struct bg_job *job, void *arg);

/* Start a joinable thread running fn(job, arg). fn must be non-NULL. Ownership and lifetime of arg
 * are callback-specific. The callback is not invoked if the thread cannot be created. Returns NULL
 * on thread creation failure. */
struct bg_job *bg_job_spawn(bg_job_fn fn, void *arg);

/* Request cooperative cancellation. A NULL job is allowed. */
void bg_job_cancel(struct bg_job *job);

/* Return whether cancellation was requested. A NULL job returns false. */
int bg_job_cancel_requested(const struct bg_job *job);

/* Adapt bg_job_cancel_requested to callback APIs that accept a void pointer. */
int bg_job_cancel_tick(void *job);

/* Wait up to `timeout_ms` for the worker to finish. Returns 1 when it finished (a NULL job counts)
 * and 0 on timeout. Does not reap the job; bg_job_join is still required. */
int bg_job_wait_ms(struct bg_job *job, long timeout_ms);

/* Wait for the worker, then free the job. A NULL job is allowed. Every successful spawn must be
 * joined exactly once, before any caller-owned state the worker may access is destroyed. */
void bg_job_join(struct bg_job *job);

#endif /* HAX_SYSTEM_BG_JOB_H */
