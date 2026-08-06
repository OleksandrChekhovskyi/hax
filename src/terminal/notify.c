/* SPDX-License-Identifier: MIT */
#include "terminal/notify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "config.h"
#include "terminal/ansi.h"

enum notify_method {
    NOTIFY_METHOD_DISABLED,
    NOTIFY_METHOD_BEL,
    NOTIFY_METHOD_OSC9,
};

/* Kitty consumes unsupported OSC 9, including its BEL terminator, so detect support positively. */
static int terminal_supports_osc9(void)
{
    static const char *const TERM_PROGRAMS[] = {"iTerm.app", "ghostty", "WezTerm", "WarpTerminal"};
    const char *term_program = getenv("TERM_PROGRAM");
    if (term_program) {
        for (size_t i = 0; i < sizeof(TERM_PROGRAMS) / sizeof(TERM_PROGRAMS[0]); i++) {
            if (strcmp(term_program, TERM_PROGRAMS[i]) == 0)
                return 1;
        }
    }
    if (getenv("GHOSTTY_RESOURCES_DIR") || getenv("GHOSTTY_BIN_DIR"))
        return 1;
    if (getenv("WEZTERM_EXECUTABLE") || getenv("WEZTERM_PANE"))
        return 1;
    return 0;
}

static enum notify_method select_notify_method(void)
{
    if (!isatty(STDOUT_FILENO))
        return NOTIFY_METHOD_DISABLED;

    const char *configured_method = config_str("notify");
    if (configured_method && strcasecmp(configured_method, "off") == 0)
        return NOTIFY_METHOD_DISABLED;
    if (configured_method && strcasecmp(configured_method, "bel") == 0)
        return NOTIFY_METHOD_BEL;
    if (configured_method && strcasecmp(configured_method, "osc9") == 0)
        return NOTIFY_METHOD_OSC9;

    const char *terminal_type = getenv("TERM");
    if (terminal_type && strcmp(terminal_type, "dumb") == 0)
        return NOTIFY_METHOD_DISABLED;

    /* tmux may drop DCS passthrough and offers no in-band query for allow-passthrough. */
    if (getenv("TMUX"))
        return NOTIFY_METHOD_BEL;

    return terminal_supports_osc9() ? NOTIFY_METHOD_OSC9 : NOTIFY_METHOD_BEL;
}

static void emit_osc9_notification(void)
{
    int tmux_wrap = getenv("TMUX") != NULL;
    if (tmux_wrap)
        fputs(ANSI_TMUX_PASSTHROUGH_BEGIN, stdout);
    fputs(ANSI_ESC "]9;hax: ready" ANSI_BEL, stdout);
    if (tmux_wrap)
        fputs(ANSI_TMUX_PASSTHROUGH_END, stdout);
}

void notify_attention(void)
{
    enum notify_method method = select_notify_method();

    switch (method) {
    case NOTIFY_METHOD_DISABLED:
        return;
    case NOTIFY_METHOD_BEL:
        fputs(ANSI_BEL, stdout);
        break;
    case NOTIFY_METHOD_OSC9:
        emit_osc9_notification();
        break;
    }
    fflush(stdout);
}
