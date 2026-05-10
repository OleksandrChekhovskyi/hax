/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent.h"
#include "agent_core.h"
#include "harness.h"
#include "slash.h"
#include "tool.h"
#include "util.h"

/* Stand-in tool symbols so agent_core.c (which references TOOLS[] in
 * agent_session_init) links. We never invoke .run; the tests touch
 * session items directly without going through any tool. */
static char *stub_run(const char *args, tool_emit_display_fn emit, void *user)
{
    (void)args;
    (void)emit;
    (void)user;
    return xstrdup("");
}
const struct tool TOOL_READ = {.def = {.name = "read"}, .run = stub_run};
const struct tool TOOL_BASH = {.def = {.name = "bash"}, .run = stub_run};
const struct tool TOOL_WRITE = {.def = {.name = "write"}, .run = stub_run};
const struct tool TOOL_EDIT = {.def = {.name = "edit"}, .run = stub_run};

/* agent.c isn't linked here (it pulls the entire REPL graph: input,
 * spinner, disp, ...). Stub the two symbols slash.c reaches into:
 *
 * - agent_print_banner: called by agent_new_conversation; tests don't
 *   assert on its output.
 * - agent_new_conversation: stand in for the real /new behavior. Only
 *   the session-reset half is observable in slash tests (transcript log
 *   stays NULL, banner is silent), so we replicate just that. */
void agent_print_banner(const struct provider *p, const struct agent_session *s)
{
    (void)p;
    (void)s;
}
void agent_new_conversation(struct agent_state *st)
{
    agent_session_reset(st->sess);
}

/* Redirect stdout to a temp file so we can inspect what slash_dispatch
 * printed. Returns the captured bytes (caller frees) and restores
 * stdout. */
static char *capture_stdout(void (*body)(void *), void *user)
{
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    EXPECT(saved >= 0);

    FILE *tmp = tmpfile();
    EXPECT(tmp != NULL);
    int tmpfd = fileno(tmp);
    EXPECT(dup2(tmpfd, STDOUT_FILENO) >= 0);

    body(user);

    fflush(stdout);
    EXPECT(dup2(saved, STDOUT_FILENO) >= 0);
    close(saved);

    /* Slurp tmp from offset 0. */
    EXPECT(fseek(tmp, 0, SEEK_END) == 0);
    long n = ftell(tmp);
    EXPECT(n >= 0);
    EXPECT(fseek(tmp, 0, SEEK_SET) == 0);
    char *buf = xmalloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, tmp);
    buf[got] = '\0';
    fclose(tmp);
    return buf;
}

/* ---------- dispatcher: not-a-command / unknown / bad usage ---------- */

struct dispatch_call {
    const char *line;
    struct slash_ctx *ctx;
    enum slash_result result;
};

static void do_dispatch(void *user)
{
    struct dispatch_call *c = user;
    c->result = slash_dispatch(c->line, c->ctx);
}

static void test_dispatch_not_a_command(void)
{
    /* Lines that don't start with '/' must return NOT_A_COMMAND and
     * print nothing — the agent loop relies on silent passthrough. */
    struct agent_state st = {0};
    struct slash_ctx ctx = {.state = &st};
    struct dispatch_call c = {.line = "hello world", .ctx = &ctx};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_NOT_A_COMMAND);
    EXPECT_STR_EQ(out, "");
    free(out);

    /* Empty string: also not a command. NULL would be a programming
     * bug at the call site (agent.c filters empty before dispatch),
     * but the dispatcher tolerates both. */
    c.line = "";
    out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_NOT_A_COMMAND);
    EXPECT_STR_EQ(out, "");
    free(out);
}

static void test_dispatch_unknown(void)
{
    /* A bareword token that doesn't match any registered command
     * still gets the red "unknown" error so typos are caught loudly,
     * not silently shipped to the model. */
    struct agent_state st = {0};
    struct slash_ctx ctx = {.state = &st};
    struct dispatch_call c = {.line = "/nonesuch", .ctx = &ctx};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_UNKNOWN);
    EXPECT(strstr(out, "/nonesuch") != NULL);
    EXPECT(strstr(out, "/help") != NULL);
    free(out);
}

static void test_dispatch_path_falls_through(void)
{
    /* Lines whose first token contains a '/' or '.' are filesystem
     * paths, not commands. They must return SLASH_NOT_A_COMMAND so
     * the agent forwards them to the model verbatim — losing a
     * prompt like "/tmp/repro.c crashes, inspect it" to an "unknown
     * command" error would be much worse than not catching a typo. */
    struct agent_state st = {0};
    struct slash_ctx ctx = {.state = &st};
    const char *paths[] = {
        "/tmp/repro.c crashes, inspect it",
        "/etc/passwd is owned by root",
        "/usr/local/bin/foo",
        "/help.txt is a file",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        struct dispatch_call c = {.line = paths[i], .ctx = &ctx};
        char *out = capture_stdout(do_dispatch, &c);
        EXPECT(c.result == SLASH_NOT_A_COMMAND);
        EXPECT_STR_EQ(out, "");
        free(out);
    }
}

static void test_dispatch_control_bytes_fall_through(void)
{
    /* A pasted control-byte payload like "/\x1b[2J" must NOT be
     * echoed back through the unknown-command diagnostic — the
     * terminal would interpret the embedded escape. Bareword check
     * makes this unreachable: any non-[a-zA-Z0-9_-] byte in the
     * first token routes the line to the model as plain text. */
    struct agent_state st = {0};
    struct slash_ctx ctx = {.state = &st};
    struct dispatch_call c = {.line = "/\x1b[2J", .ctx = &ctx};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_NOT_A_COMMAND);
    EXPECT_STR_EQ(out, "");
    free(out);
}

static void test_dispatch_bare_slash_falls_through(void)
{
    /* Bare "/" and whitespace-only "/   " are degenerate inputs:
     * empty token after the slash. Treating them as "unknown
     * command" would be inconsistent with the bareword rule
     * (which routes anything that isn't a clean command to the
     * model). Make them silent fall-throughs instead. */
    struct agent_state st = {0};
    struct slash_ctx ctx = {.state = &st};
    struct dispatch_call c = {.line = "/", .ctx = &ctx};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_NOT_A_COMMAND);
    EXPECT_STR_EQ(out, "");
    free(out);

    c.line = "/   ";
    out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_NOT_A_COMMAND);
    EXPECT_STR_EQ(out, "");
    free(out);
}

static void test_dispatch_bad_usage(void)
{
    /* /help takes no arguments — passing one must produce BAD_USAGE
     * with a diagnostic mentioning the command name, not silently
     * run the handler. */
    struct agent_state st = {0};
    struct slash_ctx ctx = {.state = &st};
    struct dispatch_call c = {.line = "/help foo", .ctx = &ctx};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_BAD_USAGE);
    EXPECT(strstr(out, "/help") != NULL);
    free(out);
}

static void test_dispatch_bad_usage_uses_alias_name(void)
{
    /* When BAD_USAGE fires on an alias invocation, the diagnostic
     * must echo what the user typed (`/clear`), not the canonical
     * name it resolves to (`/new`). Otherwise the message reads as
     * "I rejected /clear but I'm telling you about /new", which is
     * confusing. */
    struct agent_state st = {0};
    struct slash_ctx ctx = {.state = &st};
    struct dispatch_call c = {.line = "/clear now", .ctx = &ctx};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_BAD_USAGE);
    EXPECT(strstr(out, "/clear") != NULL);
    EXPECT(strstr(out, "/new") == NULL);
    free(out);
}

/* ---------- /help ---------- */

static void test_help_lists_commands_and_shortcuts(void)
{
    struct agent_state st = {0};
    struct slash_ctx ctx = {.state = &st};
    struct dispatch_call c = {.line = "/help", .ctx = &ctx};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_HANDLED);

    /* Both registered commands must appear, the alias must be
     * mentioned, and the Shortcuts section must be present with at
     * least one of the entries we list. Asserting on substrings, not
     * exact bytes, so future tweaks to formatting don't break the
     * test for cosmetic reasons. */
    EXPECT(strstr(out, "commands") != NULL);
    EXPECT(strstr(out, "/new") != NULL);
    EXPECT(strstr(out, "/clear") != NULL); /* alias */
    EXPECT(strstr(out, "/help") != NULL);
    EXPECT(strstr(out, "shortcuts") != NULL);
    EXPECT(strstr(out, "esc") != NULL);
    EXPECT(strstr(out, "ctrl-t") != NULL);
    free(out);
}

/* ---------- /new and its alias /clear ---------- */

static void seed_session(struct agent_session *s)
{
    /* Pretend a couple of turns happened. agent_session_reset must
     * clear all of them. */
    agent_session_add_user(s, "first prompt");
    items_append(&s->items, &s->n_items, &s->cap_items,
                 (struct item){.kind = ITEM_ASSISTANT_MESSAGE, .text = xstrdup("first reply")});
    agent_session_add_user(s, "second prompt");
}

static void test_new_clears_session(void)
{
    struct agent_session s = {0};
    seed_session(&s);
    EXPECT(s.n_items > 0);
    size_t cap_before = s.cap_items;

    struct provider p = {.name = "test", .default_model = NULL};
    struct agent_state st = {.sess = &s, .provider = &p};
    struct slash_ctx ctx = {.state = &st};
    struct dispatch_call c = {.line = "/new", .ctx = &ctx};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_HANDLED);
    free(out);

    EXPECT(s.n_items == 0);
    /* Capacity preserved — reset is for "fresh conversation", not
     * "free everything"; we shouldn't pay realloc overhead just to
     * grow the vector back to its prior size on the next turn. */
    EXPECT(s.cap_items == cap_before);

    agent_session_free(&s);
}

static void test_clear_alias_runs_new(void)
{
    /* /clear must reach the same handler as /new — same session-reset
     * effect, no separate code path. Verifies the alias plumbing in
     * find_command rather than the handler itself. */
    struct agent_session s = {0};
    seed_session(&s);
    EXPECT(s.n_items > 0);

    struct provider p = {.name = "test", .default_model = NULL};
    struct agent_state st = {.sess = &s, .provider = &p};
    struct slash_ctx ctx = {.state = &st};
    struct dispatch_call c = {.line = "/clear", .ctx = &ctx};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_HANDLED);
    free(out);

    EXPECT(s.n_items == 0);
    agent_session_free(&s);
}

static void test_new_rejects_extra_args(void)
{
    /* "/new now" must NOT clear — extra args fall through to BAD_USAGE
     * before the handler runs. Otherwise a typo could quietly nuke a
     * conversation. */
    struct agent_session s = {0};
    seed_session(&s);
    size_t n_before = s.n_items;

    struct provider p = {.name = "test", .default_model = NULL};
    struct agent_state st = {.sess = &s, .provider = &p};
    struct slash_ctx ctx = {.state = &st};
    struct dispatch_call c = {.line = "/new now", .ctx = &ctx};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_BAD_USAGE);
    free(out);

    EXPECT(s.n_items == n_before);
    agent_session_free(&s);
}

static void test_dispatch_trims_trailing_whitespace(void)
{
    /* "/help   " (no other args, just trailing whitespace) must be
     * accepted, not rejected as BAD_USAGE — readline edits and
     * accidental space-Enter shouldn't break a known command. */
    struct agent_state st = {0};
    struct slash_ctx ctx = {.state = &st};
    struct dispatch_call c = {.line = "/help   ", .ctx = &ctx};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_HANDLED);
    free(out);
}

int main(void)
{
    test_dispatch_not_a_command();
    test_dispatch_unknown();
    test_dispatch_path_falls_through();
    test_dispatch_control_bytes_fall_through();
    test_dispatch_bare_slash_falls_through();
    test_dispatch_bad_usage();
    test_dispatch_bad_usage_uses_alias_name();
    test_help_lists_commands_and_shortcuts();
    test_new_clears_session();
    test_clear_alias_runs_new();
    test_new_rejects_extra_args();
    test_dispatch_trims_trailing_whitespace();
    T_REPORT();
}
