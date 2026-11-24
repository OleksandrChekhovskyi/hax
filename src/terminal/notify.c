/* SPDX-License-Identifier: MIT */
#include "terminal/notify.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "config.h"
#include "terminal/ansi.h"
#include "terminal/focus.h"
#include "text/display_safe.h"
#include "text/utf8.h"

/* Under Ghostty's 255-byte notification body with the "hax: " prefix. */
#define MESSAGE_MAX_BYTES 200
#define ELLIPSIS          "\xe2\x80\xa6"

enum attention_signal {
    ATTENTION_NONE,
    ATTENTION_BELL_WHEN_AWAY,
    ATTENTION_BELL,
    ATTENTION_OSC9_WHEN_AWAY,
};

struct status_report {
    const char *bytes;
    size_t len;
};

/* Program Status Protocol: https://www.superlogical.com/rex/docs/build/program-status. Each report
 * replaces the terminal's whole record, so every one repeats `app`. ST rather than BEL terminates
 * it so that a terminal that fails to parse it does not ring a bell. Precomputed so that signal
 * handlers can write them. */
#define PROGRAM_STATUS(pairs) ANSI_ESC "]7501;" pairs ANSI_STRING_TERMINATOR
#define REPORT(pairs)         {PROGRAM_STATUS(pairs), sizeof(PROGRAM_STATUS(pairs)) - 1}
static const struct status_report REPORTS[] = {
    [NOTIFY_STATE_WORKING] = REPORT("state=working:app=hax"),
    [NOTIFY_STATE_BLOCKED] = REPORT("state=blocked:app=hax"),
    [NOTIFY_STATE_DONE] = REPORT("state=done:app=hax"),
    [NOTIFY_STATE_ERROR] = REPORT("state=error:app=hax"),
    [NOTIFY_STATE_IDLE] = REPORT("state=idle:app=hax"),
    [NOTIFY_STATE_CLEAR] = REPORT("state=clear"),
};

/* What hax is doing, whether or not reports are on; main-thread state. */
static enum notify_state current_state = NOTIFY_STATE_CLEAR;

/* The terminal's record, as last written by the main thread; a fatal-signal handler reads it. */
static volatile sig_atomic_t reported_state = NOTIFY_STATE_CLEAR;

/* VTE_VERSION 5400 (VTE 0.54) brought the parser that ignores unknown OSC sequences. Inside tmux
 * the sequence reaches tmux instead, which ignores or handles it. */
static int terminal_shows_unknown_osc(void)
{
    const char *vte_version = getenv("VTE_VERSION");
    if (!vte_version || getenv("TMUX"))
        return 0;
    char *end;
    long version = strtol(vte_version, &end, 10);
    return end != vte_version && version < 5400;
}

static int dumb_terminal(void)
{
    const char *terminal_type = getenv("TERM");
    return terminal_type && strcmp(terminal_type, "dumb") == 0;
}

static int reports_status(void)
{
    int configured = config_tristate("program_status");
    if (configured >= 0)
        return configured;
    return !dumb_terminal() && !terminal_shows_unknown_osc();
}

static enum attention_signal selected_attention(void)
{
    const char *configured = config_str("notify");
    if (configured && strcasecmp(configured, "off") == 0)
        return ATTENTION_NONE;
    if (configured && strcasecmp(configured, "bel") == 0)
        return ATTENTION_BELL;
    if (configured && strcasecmp(configured, "osc9") == 0)
        return ATTENTION_OSC9_WHEN_AWAY;
    return dumb_terminal() ? ATTENTION_NONE : ATTENTION_BELL_WHEN_AWAY;
}

/* OSC 9 rejects control characters, and a notification shows one line. Return NULL when no text
 * remains. */
static char *message_line(const char *message)
{
    if (!message)
        return NULL;
    char *line = flatten_for_display(message);
    size_t len = strlen(line);
    if (len > MESSAGE_MAX_BYTES) {
        len = utf8_char_start(line, MESSAGE_MAX_BYTES - strlen(ELLIPSIS));
        while (len > 0 && line[len - 1] == ' ')
            len--;
        memcpy(line + len, ELLIPSIS, sizeof(ELLIPSIS));
    }
    if (!line[0]) {
        free(line);
        return NULL;
    }
    return line;
}

static void write_report(FILE *out, enum notify_state state)
{
    fwrite(REPORTS[state].bytes, 1, REPORTS[state].len, out);
    reported_state = state;
}

void notify_status_write(FILE *out, enum notify_state state)
{
    current_state = state;
    if (reports_status())
        write_report(out, state);
    else if (reported_state != NOTIFY_STATE_CLEAR)
        write_report(out, NOTIFY_STATE_CLEAR);
}

void notify_attention_write(FILE *out, const char *message)
{
    int away = focus_current() == FOCUS_OUT;
    switch (selected_attention()) {
    case ATTENTION_NONE:
        break;
    case ATTENTION_BELL_WHEN_AWAY:
        if (away)
            fputs(ANSI_BEL, out);
        break;
    case ATTENTION_BELL:
        fputs(ANSI_BEL, out);
        break;
    case ATTENTION_OSC9_WHEN_AWAY: {
        if (!away)
            break;
        char *line = message_line(message);
        /* OSC 9 is only ever chosen explicitly, which implies tmux allows passthrough. The line
         * holds no ESC, so the passthrough needs no further quoting. */
        int tmux_wrap = getenv("TMUX") != NULL;
        if (tmux_wrap)
            fputs(ANSI_TMUX_PASSTHROUGH_BEGIN, out);
        fprintf(out, ANSI_ESC "]9;hax: %s" ANSI_BEL, line ? line : "ready");
        if (tmux_wrap)
            fputs(ANSI_TMUX_PASSTHROUGH_END, out);
        free(line);
        break;
    }
    }
}

/* The condition under which interrupt_init() installs the cleanup that withdraws a record. */
static int interactive_terminal(void)
{
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
}

void notify_status(enum notify_state state)
{
    if (!interactive_terminal())
        return;
    notify_status_write(stdout, state);
    fflush(stdout);
}

void notify_status_refresh_write(FILE *out)
{
    notify_status_write(out, current_state);
}

void notify_status_refresh(void)
{
    if (!interactive_terminal())
        return;
    notify_status_refresh_write(stdout);
    fflush(stdout);
}

void notify_status_withdraw(void)
{
    const struct status_report *clear = &REPORTS[NOTIFY_STATE_CLEAR];
    if (reported_state != NOTIFY_STATE_CLEAR)
        (void)!write(STDOUT_FILENO, clear->bytes, clear->len);
}

void notify_status_reassert(void)
{
    const struct status_report *report = &REPORTS[reported_state];
    if (reported_state != NOTIFY_STATE_CLEAR)
        (void)!write(STDOUT_FILENO, report->bytes, report->len);
}

void notify_acknowledge(void)
{
    if (current_state == NOTIFY_STATE_BLOCKED || current_state == NOTIFY_STATE_DONE ||
        current_state == NOTIFY_STATE_ERROR)
        notify_status(NOTIFY_STATE_IDLE);
}

void notify_attention(const char *message)
{
    if (!isatty(STDOUT_FILENO))
        return;
    notify_attention_write(stdout, message);
    fflush(stdout);
}
