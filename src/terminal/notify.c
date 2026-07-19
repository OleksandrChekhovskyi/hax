/* SPDX-License-Identifier: MIT */
#include "terminal/notify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp — POSIX puts it here, not in <string.h> */
#include <unistd.h>

#include "config.h"

/* Method selected for the current process — resolved once on first call,
 * since the answer is a function of the environment and the env doesn't
 * change underneath us. */
enum method {
    M_UNRESOLVED = 0,
    M_DISABLED,
    M_BEL,
    M_OSC9,
};

/* Identify the host terminal from the usual env breadcrumbs. We only
 * need to distinguish "supports OSC 9" from "doesn't" — anything we
 * can't positively identify falls back to BEL, which every terminal
 * handles. Kitty is deliberately excluded: it implements desktop
 * notifications via OSC 99 with a different payload format and
 * silently swallows OSC 9 (consuming our trailing BEL with it), so
 * letting it fall through to the BEL emitter is the right behavior. */
static int terminal_supports_osc9(void)
{
    const char *term_program = getenv("TERM_PROGRAM");
    if (term_program) {
        if (strcmp(term_program, "iTerm.app") == 0 || strcmp(term_program, "ghostty") == 0 ||
            strcmp(term_program, "WezTerm") == 0 || strcmp(term_program, "WarpTerminal") == 0)
            return 1;
    }
    if (getenv("GHOSTTY_RESOURCES_DIR") || getenv("GHOSTTY_BIN_DIR"))
        return 1;
    if (getenv("WEZTERM_EXECUTABLE") || getenv("WEZTERM_PANE"))
        return 1;
    return 0;
}

static enum method resolve_method(void)
{
    /* Hard gate: writing escape sequences to a pipe pollutes the
     * captured output and never reaches a human. */
    if (!isatty(fileno(stdout)))
        return M_DISABLED;

    /* Explicit methods override detection; "auto", unset, and unknown values
     * use the heuristics below. The dumb-terminal opt-out is auto-only. */
    const char *cfg = config_str("notify");
    if (cfg && strcasecmp(cfg, "off") == 0)
        return M_DISABLED;
    if (cfg && strcasecmp(cfg, "bel") == 0)
        return M_BEL;
    if (cfg && strcasecmp(cfg, "osc9") == 0)
        return M_OSC9;

    const char *term = getenv("TERM");
    if (term && strcmp(term, "dumb") == 0)
        return M_DISABLED;

    /* Inside tmux, OSC 9 is unreliable: tmux silently drops the DCS
     * passthrough envelope when `allow-passthrough` is off (the
     * default since 3.3), so a wrapped OSC 9 produces no output at
     * all — and there's no in-band query to detect the setting.
     * BEL behaves well in either case: tmux marks the pane with its
     * own activity flag and passes the byte through to the host
     * terminal, which can ring/flash/notify per its own settings.
     * Force `HAX_NOTIFY=osc9` to override when passthrough is known
     * to be enabled. */
    if (getenv("TMUX"))
        return M_BEL;

    return terminal_supports_osc9() ? M_OSC9 : M_BEL;
}

/* Inside tmux the OSC must be smuggled through a DCS passthrough so
 * the outer terminal receives it. The payload is a fixed string from
 * a string literal — no untrusted bytes — so no escaping is required;
 * the doubled ESC and `\x1b\\` terminator are the constant DCS frame
 * around the OSC. */
static void emit_osc9(void)
{
    const char *m = "hax: ready";
    if (getenv("TMUX"))
        printf("\x1bPtmux;\x1b\x1b]9;%s\x07\x1b\\", m);
    else
        printf("\x1b]9;%s\x07", m);
}

void notify_attention(void)
{
    /* Resolve per call so runtime changes apply immediately. */
    enum method m = resolve_method();

    switch (m) {
    case M_DISABLED:
    case M_UNRESOLVED:
        return;
    case M_BEL:
        putchar('\x07');
        break;
    case M_OSC9:
        emit_osc9();
        break;
    }
    fflush(stdout);
}
