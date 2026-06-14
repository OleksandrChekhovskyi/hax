/* SPDX-License-Identifier: MIT */
#ifndef HAX_KEEPAWAKE_H
#define HAX_KEEPAWAKE_H

/*
 * Idle-sleep inhibitor: keeps the machine from going to sleep while a
 * user turn is in flight (model streaming + tool dispatch), so an
 * unattended long run isn't cut short by the idle/lid timer. Inhibits
 * idle *system* sleep only — the display is left free to blank.
 *
 * The assertion is held by a spawned helper for its lifetime:
 *   - macOS: `caffeinate -i -w <hax_pid>`
 *   - Linux: `systemd-inhibit --what=idle --mode=block -- sleep <forever>`
 * Both are tied to hax's lifetime — caffeinate via -w, systemd-inhibit
 * via PR_SET_PDEATHSIG — so an orphaned helper exits if hax dies
 * without running release (a SIGKILL, say). Everything else (helper
 * binary missing, an unsupported OS, the feature disabled by config)
 * is a silent no-op: sleep prevention is best-effort and never
 * surfaces an error to the user.
 *
 * acquire/release are idempotent — acquire is a no-op when a helper is
 * already running, release a no-op when none is — and acquire is gated
 * by the `keep_awake` config key (default on), so when the feature is
 * off it does nothing.
 */

/* Begin inhibiting idle sleep (spawns the helper if not already up). */
void keepawake_acquire(void);

/* Stop inhibiting idle sleep (terminates and reaps the helper). */
void keepawake_release(void);

#endif
