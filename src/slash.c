/* SPDX-License-Identifier: MIT */
#include "slash.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent.h"
#include "render/render_ctx.h"
#include "select.h"
#include "session.h"
#include "session_picker.h"
#include "util.h"
#include "terminal/ansi.h"
#include "terminal/clipboard.h"
#include "terminal/ui.h"

/* Maximum number of aliases a single command can advertise. Three is
 * already plenty (e.g. /new + /clear + /reset is the absolute ceiling
 * I'd expect any single command to want); the array is NULL-terminated
 * so unused slots stay empty. */
#define SLASH_MAX_ALIASES 3

struct slash_cmd {
    const char *name;
    const char *aliases[SLASH_MAX_ALIASES + 1]; /* NULL-terminated */
    const char *summary;
    /* When set, the command accepts an optional trailing argument (passed
     * via slash_ctx.arg) instead of being rejected with "takes no
     * arguments." The argument is still optional — the command runs with
     * arg == NULL when none is given. */
    int takes_arg;
    /* When set, the handler drives the render pipeline (disp) itself and
     * leaves the trailing-newline state correct — e.g. /compact's notice,
     * /resume's replay. Raw-output handlers (the default) print straight to
     * stdout and bypass disp, so the dispatcher resets the trail after them
     * (see slash_dispatch) to model the fresh line their output ended on. */
    int drives_disp;
    void (*run)(struct slash_ctx *ctx);
};

struct shortcut_def {
    const char *key;
    const char *desc;
};

static void slash_run_new(struct slash_ctx *ctx);
static void slash_run_resume(struct slash_ctx *ctx);
static void slash_run_provider(struct slash_ctx *ctx);
static void slash_run_model(struct slash_ctx *ctx);
static void slash_run_effort(struct slash_ctx *ctx);
static void slash_run_compact(struct slash_ctx *ctx);
static void slash_run_copy(struct slash_ctx *ctx);
static void slash_run_usage(struct slash_ctx *ctx);
static void slash_run_help(struct slash_ctx *ctx);

/* Registry. Order is preserved in /help output, so list user-facing
 * commands before meta ones. */
static const struct slash_cmd COMMANDS[] = {
    {
        .name = "new",
        .aliases = {"clear", NULL},
        .summary = "start a fresh conversation",
        .run = slash_run_new,
    },
    {
        .name = "resume",
        .aliases = {NULL},
        .summary = "resume a past conversation",
        .drives_disp = 1,
        .run = slash_run_resume,
    },
    {
        .name = "provider",
        .aliases = {NULL},
        .summary = "switch provider, then model and effort",
        .drives_disp = 1,
        .run = slash_run_provider,
    },
    {
        .name = "model",
        .aliases = {NULL},
        .summary = "switch model, then effort",
        .drives_disp = 1,
        .run = slash_run_model,
    },
    {
        .name = "effort",
        .aliases = {NULL},
        .summary = "set reasoning effort",
        .drives_disp = 1,
        .run = slash_run_effort,
    },
    {
        .name = "compact",
        .aliases = {NULL},
        .summary = "summarize history to free up context (optional: focus instructions)",
        .takes_arg = 1,
        .drives_disp = 1,
        .run = slash_run_compact,
    },
    {
        .name = "copy",
        .aliases = {NULL},
        .summary = "copy last response to clipboard",
        .run = slash_run_copy,
    },
    {
        .name = "usage",
        .aliases = {NULL},
        .summary = "show provider usage info",
        .run = slash_run_usage,
    },
    {
        .name = "help",
        .aliases = {NULL},
        .summary = "show this help",
        .run = slash_run_help,
    },
};
#define N_COMMANDS (sizeof(COMMANDS) / sizeof(COMMANDS[0]))

/* hax-specific or non-obvious bindings only. The full readline-style
 * motion set (Ctrl-A/E/B/F/W/U/K/H, arrows, Home/End) is intentionally
 * omitted: users who know readline already know them, and listing
 * everything would push the more useful bindings off the screen. */
static const struct shortcut_def SHORTCUTS[] = {
    {"enter", "submit prompt"},
    {"shift-enter", "insert newline (terminal must be configured to send LF)"},
    {"esc", "interrupt model or running tool"},
    {"ctrl-c", "cancel current prompt line"},
    {"ctrl-d", "quit (on empty prompt)"},
    {"ctrl-g", "edit prompt in $EDITOR"},
    {"ctrl-t", "view transcript in $PAGER"},
    {"ctrl-l", "clear screen and redraw prompt"},
};
#define N_SHORTCUTS (sizeof(SHORTCUTS) / sizeof(SHORTCUTS[0]))

static const struct slash_cmd *find_command(const char *name)
{
    for (size_t i = 0; i < N_COMMANDS; i++) {
        if (strcmp(COMMANDS[i].name, name) == 0)
            return &COMMANDS[i];
        for (size_t j = 0; COMMANDS[i].aliases[j]; j++) {
            if (strcmp(COMMANDS[i].aliases[j], name) == 0)
                return &COMMANDS[i];
        }
    }
    return NULL;
}

enum slash_result slash_dispatch(const char *line, struct slash_ctx *ctx)
{
    if (!line || line[0] != '/')
        return SLASH_NOT_A_COMMAND;

    /* Parse the first whitespace-delimited token after '/'. Only treat
     * the line as a slash command when that token is a "bareword" —
     * letters, digits, '_' or '-'. Anything else (most importantly a
     * '/' or '.', as in "/tmp/repro.c crashes") falls through to the
     * model as plain text, so absolute paths in normal prompts aren't
     * swallowed by the dispatcher. The narrow case this can't disam-
     * biguate is a bareword path on its own ("/tmp" alone) — rare in
     * practice; rephrase as "the /tmp dir" or wrap in backticks. We
     * can revisit with a "//" escape if it ever becomes a real pain. */
    const char *p = line + 1;
    const char *start = p;
    int is_bareword = 1;
    while (*p && !isspace((unsigned char)*p)) {
        unsigned char c = (unsigned char)*p;
        if (!isalnum(c) && c != '_' && c != '-')
            is_bareword = 0;
        p++;
    }
    size_t name_len = (size_t)(p - start);

    if (name_len == 0 || !is_bareword)
        return SLASH_NOT_A_COMMAND;

    while (*p && isspace((unsigned char)*p))
        p++;
    int has_extra = (*p != '\0');

    /* From here the line is committed as a slash command — every outcome
     * (handler output, "unknown command", "no arguments", etc.) is slash
     * output and gets the standard leading blank-line gap that ordinary model
     * turns get. Emit it through disp (not a raw putchar) so the trail counter
     * stays truthful: handlers that drive the render pipeline (/compact's
     * agent_compact, /resume's replay) then see the real cursor position
     * instead of stacking a second separator. Doing it once here also means
     * handlers don't each have to remember, and /usage's spinner-prep gets its
     * prepped row for free.
     *
     * Raw-output handlers (the default — /help, /usage, ... and the error
     * paths below) print straight to stdout, bypassing disp, so afterwards the
     * trail still reflects this gap rather than their output. Reset it to 1
     * once they return (and on the error paths) to model the fresh line their
     * trailing newline left, so the pre-prompt separator still emits the
     * trailing blank. drives_disp handlers maintain disp themselves and are
     * left untouched. */
    struct disp *d = &ctx->state->r->disp;
    disp_block_separator(d);

    /* Stack copy so we can NUL-terminate without touching the caller's
     * buffer. 64 is generous for command names — the longest plausible
     * one is ~12 chars. */
    char name_buf[64];
    if (name_len >= sizeof(name_buf)) {
        ui_error("unknown command. type /help for the list.");
        d->trail = 1;
        return SLASH_UNKNOWN;
    }
    memcpy(name_buf, start, name_len);
    name_buf[name_len] = '\0';

    const struct slash_cmd *cmd = find_command(name_buf);
    if (!cmd) {
        ui_error("unknown command: /%s. type /help for the list.", name_buf);
        d->trail = 1;
        return SLASH_UNKNOWN;
    }
    if (has_extra && !cmd->takes_arg) {
        /* Echo the token the user actually typed, not the canonical
         * name — `/clear now` should report on `/clear`, not on the
         * `/new` it resolves to. */
        ui_error("/%s takes no arguments.", name_buf);
        d->trail = 1;
        return SLASH_BAD_USAGE;
    }

    /* For arg-taking commands, hand over the trailing text (NULL when none).
     * `p` already sits past the command token and its following whitespace;
     * the line is NUL-terminated so it needs no further trimming on the left,
     * and the input editor strips trailing whitespace before dispatch. */
    ctx->arg = (cmd->takes_arg && has_extra) ? p : NULL;
    cmd->run(ctx);
    if (!cmd->drives_disp)
        d->trail = 1;
    return SLASH_HANDLED;
}

/* ---------- /new ---------- */

static void slash_run_new(struct slash_ctx *ctx)
{
    agent_new_conversation(ctx->state);
}

/* ---------- /resume ---------- */

static void slash_run_resume(struct slash_ctx *ctx)
{
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) {
        ui_error("cannot determine working directory");
        return;
    }
    /* Hide the live session from the list — resuming the conversation
     * you're already in is a no-op. The picker prints its own "nothing
     * to resume" note and returns NULL when the list is empty or the
     * user cancels. */
    const char *current = session_log_path(ctx->state->slog);
    int shown = 0;
    char *path = session_picker_run(cwd, current, &shown);
    /* The dispatcher's leading-gap separator left disp at trail = 2. A shown
     * picker draws full-screen then erases itself back onto that same blank
     * line (its raw drawing bypasses disp but lands where it started), so
     * trail = 2 still matches the cursor — leave it. When no picker was shown
     * (non-tty, or nothing to resume), a raw note was printed instead, so model
     * its fresh-line end (trail = 1) for the pre-prompt separator. */
    if (!shown)
        ctx->state->r->disp.trail = 1;
    if (!path)
        return;
    agent_resume_session(ctx->state, path);
    free(path);
}

/* ---------- /provider, /model, /effort ---------- */

/* All three drive the picker + render pipeline themselves (drives_disp),
 * so the dispatcher leaves disp bookkeeping to the select flow. The chain
 * is provider → model → effort; /model and /effort enter partway in. */
static void slash_run_provider(struct slash_ctx *ctx)
{
    select_provider(ctx->state);
}

static void slash_run_model(struct slash_ctx *ctx)
{
    select_model(ctx->state);
}

static void slash_run_effort(struct slash_ctx *ctx)
{
    select_effort(ctx->state);
}

/* ---------- /compact ---------- */

static void slash_run_compact(struct slash_ctx *ctx)
{
    /* agent_compact drives the same render pipeline /resume does, so the
     * spinner + dim notice land below the echoed command like any other
     * slash output. ctx->arg carries optional focus instructions. */
    agent_compact(ctx->state, ctx->arg, 0);
}

/* ---------- /copy ---------- */

static void slash_run_copy(struct slash_ctx *ctx)
{
    /* Walk the items vector backwards for the most recent assistant
     * message with non-empty text. Tool calls, tool results, reasoning
     * items, and turn boundaries are skipped — they're not what the
     * user means by "the last response". The text field already holds
     * the model's raw Markdown, so no conversion is needed. */
    const struct item *msg = NULL;
    if (ctx->state && ctx->state->sess) {
        struct agent_session *s = ctx->state->sess;
        for (size_t i = s->n_items; i > 0; i--) {
            const struct item *it = &s->items[i - 1];
            if (it->kind == ITEM_ASSISTANT_MESSAGE && it->text && it->text[0]) {
                msg = it;
                break;
            }
        }
    }
    if (!msg) {
        ui_note("no assistant response to copy");
        return;
    }
    size_t len = strlen(msg->text);
    const char *err = NULL;
    if (clipboard_copy(msg->text, len, &err) == 0) {
        ui_note("copied %zu byte%s to clipboard", len, len == 1 ? "" : "s");
        return;
    }
    ui_error("clipboard copy failed: %s", err ? err : "unknown error");
}

/* ---------- /usage ---------- */

static void slash_run_usage(struct slash_ctx *ctx)
{
    /* Cast away const: provider methods (stream, query_usage, destroy)
     * all take a writable `struct provider *` since they may mutate
     * adapter state. agent_state holds a const pointer because most
     * commands only need read-only fields (->name, ->default_model);
     * this is the one place we hand the object to a method. */
    struct provider *p = (struct provider *)ctx->state->provider;
    if (!p) {
        ui_note("no provider selected — use /provider to choose one first");
        return;
    }
    if (!p->query_usage) {
        ui_note("/usage is not supported by the %s provider", p->name ? p->name : "?");
        return;
    }
    p->query_usage(p);
}

/* ---------- /help ---------- */

static void pad_spaces(int n)
{
    for (int i = 0; i < n; i++)
        fputc(' ', stdout);
}

/* Aliases get their own row rather than being squeezed inline next to
 * the canonical name — folding `/new (alias: /clear)` onto one line
 * pushes the description column far to the right and only gets worse
 * as commands accumulate. Each alias renders dim on both sides so it
 * reads as a sub-entry of the command above without breaking the
 * scannable single-column-of-names layout. */
static void print_cmd_row(const char *name, const char *summary, int dim, int gutter)
{
    fputs("  ", stdout);
    fputs(dim ? ANSI_DIM ANSI_CYAN : ANSI_CYAN, stdout);
    int w = 1 + (int)strlen(name);
    printf("/%s", name);
    fputs(ANSI_RESET, stdout);
    pad_spaces(gutter - w);
    if (dim)
        fputs(ANSI_DIM, stdout);
    fputs(summary, stdout);
    if (dim)
        fputs(ANSI_RESET, stdout);
    fputc('\n', stdout);
}

static void slash_run_help(struct slash_ctx *ctx)
{
    (void)ctx;

    /* Column-1 width: longest "/name" (canonical or alias) across the
     * commands section, longest key in the shortcuts section, take
     * the max, +2 for gutter. Computed dynamically so adding a
     * longer command or shortcut key in the future keeps the
     * columns aligned without retuning a magic number. */
    size_t col1 = 0;
    for (size_t i = 0; i < N_COMMANDS; i++) {
        size_t w = 1 + strlen(COMMANDS[i].name); /* "/name" */
        if (w > col1)
            col1 = w;
        for (size_t j = 0; COMMANDS[i].aliases[j]; j++) {
            size_t aw = 1 + strlen(COMMANDS[i].aliases[j]);
            if (aw > col1)
                col1 = aw;
        }
    }
    for (size_t i = 0; i < N_SHORTCUTS; i++) {
        size_t w = strlen(SHORTCUTS[i].key);
        if (w > col1)
            col1 = w;
    }
    int gutter = (int)col1 + 2;

    fputs(ANSI_BOLD "commands" ANSI_RESET "\n", stdout);
    for (size_t i = 0; i < N_COMMANDS; i++) {
        print_cmd_row(COMMANDS[i].name, COMMANDS[i].summary, 0, gutter);
        for (size_t j = 0; COMMANDS[i].aliases[j]; j++) {
            char *summary = xasprintf("alias for /%s", COMMANDS[i].name);
            print_cmd_row(COMMANDS[i].aliases[j], summary, 1, gutter);
            free(summary);
        }
    }

    fputc('\n', stdout);
    fputs(ANSI_BOLD "shortcuts" ANSI_RESET "\n", stdout);
    for (size_t i = 0; i < N_SHORTCUTS; i++) {
        fputs("  ", stdout);
        fputs(ANSI_CYAN, stdout);
        fputs(SHORTCUTS[i].key, stdout);
        fputs(ANSI_RESET, stdout);
        pad_spaces(gutter - (int)strlen(SHORTCUTS[i].key));
        fputs(SHORTCUTS[i].desc, stdout);
        fputc('\n', stdout);
    }
}
