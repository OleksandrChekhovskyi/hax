/* SPDX-License-Identifier: MIT */
#ifndef HAX_NOTIFY_H
#define HAX_NOTIFY_H

/* Emit a terminal "attention" notification on stdout: an OSC 9 desktop
 * notification on terminals that support it (iTerm2, Ghostty, WezTerm,
 * Warp), otherwise a BEL. Kitty uses OSC 99 with a richer protocol and
 * is intentionally left to the BEL path.
 *
 * Inside tmux we default to BEL because tmux silently drops DCS
 * passthrough when `allow-passthrough` is off (the default), and the
 * setting is not detectable in-band. BEL still triggers tmux's
 * activity flag and reaches the host terminal. Force HAX_NOTIFY=osc9
 * to wrap OSC 9 in a DCS passthrough envelope when passthrough is
 * known to be enabled.
 *
 * No-op when stdout isn't a TTY, when HAX_NOTIFY=0/off/false, or when
 * TERM=dumb. Set HAX_NOTIFY=bel or HAX_NOTIFY=osc9 to force a method. */
void notify_attention(void);

#endif /* HAX_NOTIFY_H */
