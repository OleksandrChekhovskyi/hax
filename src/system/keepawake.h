/* SPDX-License-Identifier: MIT */
#ifndef HAX_KEEPAWAKE_H
#define HAX_KEEPAWAKE_H

/* Best-effort idle system sleep inhibition. Display blanking is unaffected. Unsupported platforms,
 * disabled configuration, missing platform support, and runtime failures are silent no-ops. Acquire
 * and release are idempotent; acquire is gated by the `keep_awake` config key. */
void keepawake_acquire(void);
void keepawake_release(void);

#endif
