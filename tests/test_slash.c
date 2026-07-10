/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent.h"
#include "agent_core.h"
#include "harness.h"
#include "render/render_ctx.h"
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
 * spinner, disp, ...). Stub the symbols slash.c reaches into:
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
/* agent_session_spend folds a catalog estimate for the open pricing
 * segment into the spend figure; these tests run with no provider and no
 * catalog, so mirror just the no-catalog arithmetic (reported cost +
 * settled estimate; approximate when any inexact component exists). */
double agent_session_spend(const struct session_stats *t, const struct provider *p,
                           const char *model, int *approx)
{
    (void)p;
    (void)model;
    if (approx)
        *approx = t->est_cost > 0 || spend_has_tokens(&t->spend) || t->est_dropped;
    return t->spend.reported + t->est_cost;
}

/* /resume reaches into session.c / session_picker.c / agent.c too. None
 * are linked here (and pulling session.h would drag in jansson), so
 * forward-declare the opaque handle and stub the three symbols slash.c
 * references. The picker stub is scriptable via the two globals below so
 * the resume tests can exercise the cancelled-vs-nothing-shown paths. */
struct session_log;
const char *session_log_path(const struct session_log *log)
{
    (void)log;
    return NULL;
}
/* /session prints the resume id; NULL = "not recorded", which is what a
 * stubbed-out log should read as. */
const char *session_log_resume_hint(const struct session_log *log)
{
    (void)log;
    return NULL;
}
/* Scripts the session_picker_run stub: whether an interactive picker was
 * shown, and the path it "selects" (NULL = cancel or nothing to resume). */
static int stub_picker_shown = 0;
static const char *stub_picker_path = NULL;
char *session_picker_run(const char *cwd, const char *exclude_path, int *shown)
{
    (void)cwd;
    (void)exclude_path;
    if (shown)
        *shown = stub_picker_shown;
    return stub_picker_path ? xstrdup(stub_picker_path) : NULL;
}
void agent_resume_session(struct agent_state *st, const char *path)
{
    (void)st;
    (void)path;
}
int agent_compact(struct agent_state *st, const char *instructions, int is_auto)
{
    (void)st;
    (void)instructions;
    (void)is_auto;
    return 0;
}

/* /provider, /model, /effort dispatch into select.c, which pulls in the
 * whole provider + picker + agent graph. The slash tests only assert on
 * dispatch routing, so stub the three entry points the registry calls. */
void select_provider(struct agent_state *st)
{
    (void)st;
}
void select_model(struct agent_state *st)
{
    (void)st;
}
void select_effort(struct agent_state *st)
{
    (void)st;
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
    struct render_ctx r = {0};
    r.disp.trail = 1; /* models the cursor one line below the echoed command */
    struct agent_state st = {.r = &r};
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
    struct render_ctx r = {0};
    r.disp.trail = 1;
    struct agent_state st = {.r = &r};
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
    struct render_ctx r = {0};
    r.disp.trail = 1;
    struct agent_state st = {.r = &r};
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
    struct render_ctx r = {0};
    r.disp.trail = 1;
    struct agent_state st = {.r = &r};
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

/* ---------- /session ---------- */

static void test_session_prints_totals(void)
{
    struct render_ctx r = {0};
    r.disp.trail = 1;
    struct agent_state st = {.r = &r};
    st.stats.turns = 3;
    st.stats.requests = 7;
    st.stats.tool_calls = 6;
    st.stats.tools[0].name = "bash";
    st.stats.tools[0].count = 4;
    st.stats.tools[1].name = "read";
    st.stats.tools[1].count = 2;
    st.stats.worked_ms = 68000;   /* 1m 08s */
    st.stats.input_tokens = 5530; /* 5.4k */
    st.stats.output_tokens = 412;
    st.stats.cached_tokens = 2048; /* 2.0k; 2048*100/5530 = 37% hit rate */
    st.stats.spend.reported = 0.042;
    st.stats.last_ctx = 4000; /* 3.9k; no provider ⇒ no limit/percent */
    struct slash_ctx ctx = {.state = &st};
    struct dispatch_call c = {.line = "/session", .ctx = &ctx};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_HANDLED);
    /* Stubbed session_log_resume_hint returns NULL ⇒ "not recorded". */
    EXPECT(strstr(out, "not recorded") != NULL);
    EXPECT(strstr(out, "user turns") != NULL);
    EXPECT(strstr(out, "requests") != NULL);
    EXPECT(strstr(out, "tool calls") != NULL);
    EXPECT(strstr(out, "6 · bash 4 · read 2") != NULL);
    EXPECT(strstr(out, "time worked") != NULL);
    EXPECT(strstr(out, "1m 08s") != NULL);
    EXPECT(strstr(out, "context") != NULL);
    EXPECT(strstr(out, "3.9k") != NULL);
    EXPECT(strstr(out, "tokens total") != NULL);
    EXPECT(strstr(out, "in 5.4k") != NULL);
    EXPECT(strstr(out, "out 412") != NULL);
    EXPECT(strstr(out, "cached 2.0k (37%)") != NULL);
    EXPECT(strstr(out, "$0.042") != NULL);
    free(out);
}

static void test_session_hides_unreported_rows(void)
{
    /* Zero totals (a backend that reports no usage, no provider-reported
     * cost): the tokens and spend rows are dropped rather than shown as
     * zeros; turns and time worked always render. */
    struct render_ctx r = {0};
    r.disp.trail = 1;
    struct agent_state st = {.r = &r};
    struct slash_ctx ctx = {.state = &st};
    struct dispatch_call c = {.line = "/session", .ctx = &ctx};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_HANDLED);
    EXPECT(strstr(out, "user turns") != NULL);
    EXPECT(strstr(out, "requests") != NULL);
    EXPECT(strstr(out, "time worked") != NULL);
    EXPECT(strstr(out, "tool calls") == NULL);
    EXPECT(strstr(out, "tokens") == NULL);
    EXPECT(strstr(out, "$") == NULL);
    free(out);
}

static void test_session_marks_estimated_spend(void)
{
    /* When an estimated component contributes, the spend row carries the
     * "~" approximation marker (the stubbed agent_session_spend mirrors
     * the real reported+estimate arithmetic). */
    struct render_ctx r = {0};
    r.disp.trail = 1;
    struct agent_state st = {.r = &r};
    st.stats.spend.reported = 0.030;
    st.stats.est_cost = 0.012;
    struct slash_ctx ctx = {.state = &st};
    struct dispatch_call c = {.line = "/session", .ctx = &ctx};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_HANDLED);
    EXPECT(strstr(out, "~$0.042") != NULL);
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
    struct render_ctx r = {0};
    r.disp.trail = 1;
    struct agent_state st = {.sess = &s, .provider = &p, .r = &r};
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
    struct render_ctx r = {0};
    r.disp.trail = 1;
    struct agent_state st = {.sess = &s, .provider = &p, .r = &r};
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
    struct render_ctx r = {0};
    r.disp.trail = 1;
    struct agent_state st = {.sess = &s, .provider = &p, .r = &r};
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
    struct render_ctx r = {0};
    r.disp.trail = 1;
    struct agent_state st = {.r = &r};
    struct slash_ctx ctx = {.state = &st};
    struct dispatch_call c = {.line = "/help   ", .ctx = &ctx};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_HANDLED);
    free(out);
}

static void test_resume_cancelled_picker_keeps_trail(void)
{
    /* A shown picker that the user cancels erases itself back onto the
     * dispatcher's leading-gap line, so trail = 2 still matches the cursor —
     * slash_run_resume must leave it so the pre-prompt separator adds no
     * second blank line. */
    stub_picker_shown = 1;
    stub_picker_path = NULL;
    struct render_ctx r = {0};
    r.disp.trail = 1;
    struct agent_state st = {.r = &r};
    struct slash_ctx ctx = {.state = &st};
    struct dispatch_call c = {.line = "/resume", .ctx = &ctx};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_HANDLED);
    EXPECT(r.disp.trail == 2);
    free(out);
}

static void test_resume_selected_session_keeps_trail(void)
{
    /* Selecting a session is the same gap situation — the picker was shown and
     * erased back, so trail = 2 — and agent_resume_session's replay sees that
     * and doesn't stack a second blank line above the resumed view.
     * (agent_resume_session is stubbed, so this asserts the trail, not the
     * replay itself.) */
    stub_picker_shown = 1;
    stub_picker_path = "/tmp/some-session.jsonl";
    struct render_ctx r = {0};
    r.disp.trail = 1;
    struct agent_state st = {.r = &r};
    struct slash_ctx ctx = {.state = &st};
    struct dispatch_call c = {.line = "/resume", .ctx = &ctx};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_HANDLED);
    EXPECT(r.disp.trail == 2);
    free(out);
}

static void test_resume_no_picker_repairs_trail(void)
{
    /* No interactive picker (non-tty, or nothing to resume): session_picker_run
     * prints a raw note straight to stdout and returns with shown = 0, so the
     * cursor is one newline past the note, not at the gap line. slash_run_resume
     * must repair trail = 1 so the pre-prompt separator still emits the blank;
     * leaving the dispatcher's trail = 2 would drop it. */
    stub_picker_shown = 0;
    stub_picker_path = NULL;
    struct render_ctx r = {0};
    r.disp.trail = 1;
    struct agent_state st = {.r = &r};
    struct slash_ctx ctx = {.state = &st};
    struct dispatch_call c = {.line = "/resume", .ctx = &ctx};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_HANDLED);
    EXPECT(r.disp.trail == 1);
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
    test_session_prints_totals();
    test_session_hides_unreported_rows();
    test_session_marks_estimated_spend();
    test_new_clears_session();
    test_clear_alias_runs_new();
    test_new_rejects_extra_args();
    test_dispatch_trims_trailing_whitespace();
    test_resume_cancelled_picker_keeps_trail();
    test_resume_selected_session_keeps_trail();
    test_resume_no_picker_repairs_trail();
    T_REPORT();
}
