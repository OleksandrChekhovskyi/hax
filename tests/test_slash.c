/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent.h"
#include "agent_core.h"
#include "agent_usage.h"
#include "harness.h"
#include "provider.h"
#include "slash.h"
#include "tool.h"
#include "xalloc.h"
#include "render/render_ctx.h"

/* Link-only tool stubs; slash tests never invoke them. */
static char *stub_run(const char *args, struct tool_run_ctx *ctx)
{
    (void)args;
    (void)ctx;
    return xstrdup("");
}
const struct tool TOOL_READ = {.def = {.name = "read"}, .run = stub_run};
const struct tool TOOL_BASH = {.def = {.name = "bash"}, .run = stub_run};
const struct tool TOOL_WRITE = {.def = {.name = "write"}, .run = stub_run};
const struct tool TOOL_EDIT = {.def = {.name = "edit"}, .run = stub_run};

/* Link-only agent stubs retain the session effects asserted below. */
void agent_new_conversation(struct agent_state *state)
{
    agent_session_reset(state->session);
}
double agent_session_spend(const struct session_stats *stats, int *estimated)
{
    return agent_spend_total(&stats->spend, estimated);
}

/* Scriptable picker state distinguishes cancellation from an unavailable picker. */
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
void agent_resume_session(struct agent_state *state, const char *path)
{
    (void)state;
    (void)path;
}
int agent_compact(struct agent_state *state, const char *instructions, int automatic)
{
    (void)state;
    (void)instructions;
    (void)automatic;
    return 0;
}

/* History stubs model no selectable user turns. */
size_t agent_user_turn_count(const struct agent_session *session)
{
    (void)session;
    return 0;
}
const char *agent_user_turn_text(const struct agent_session *session, size_t turn_index)
{
    (void)session;
    (void)turn_index;
    return NULL;
}
void agent_undo(struct agent_state *state, size_t turn_index)
{
    (void)state;
    (void)turn_index;
}
void agent_fork(struct agent_state *state, size_t turn_index)
{
    (void)state;
    (void)turn_index;
}
struct picker_opts;
long picker_run(const struct picker_opts *opts)
{
    (void)opts;
    return -1;
}

/* Selector stubs expose only routing state relevant to slash commands. */
void select_provider(struct agent_state *state)
{
    (void)state;
}
void select_model(struct agent_state *state)
{
    (void)state;
}
void select_effort(struct agent_state *state)
{
    (void)state;
}
static int stub_preset_rc = 0;
static const char *stub_preset_name = NULL;
static int stub_preset_announce = -1;
int select_preset(struct agent_state *state, const char *name, int announce)
{
    (void)state;
    stub_preset_name = name;
    stub_preset_announce = announce;
    return stub_preset_rc;
}
static const char *stub_preset_save_argument = NULL;
void select_preset_save(struct agent_state *state, const char *argument)
{
    (void)state;
    stub_preset_save_argument = argument;
}
void select_config(struct agent_state *state, const char *argument)
{
    (void)state;
    (void)argument;
}

/* Return owned captured stdout and restore the original descriptor. */
static char *capture_stdout(void (*body)(void *), void *user)
{
    fflush(stdout);
    int saved_fd = dup(STDOUT_FILENO);
    EXPECT(saved_fd >= 0);

    FILE *capture = tmpfile();
    EXPECT(capture != NULL);
    int capture_fd = fileno(capture);
    EXPECT(dup2(capture_fd, STDOUT_FILENO) >= 0);

    body(user);

    fflush(stdout);
    EXPECT(dup2(saved_fd, STDOUT_FILENO) >= 0);
    close(saved_fd);

    EXPECT(fseek(capture, 0, SEEK_END) == 0);
    long byte_count = ftell(capture);
    EXPECT(byte_count >= 0);
    EXPECT(fseek(capture, 0, SEEK_SET) == 0);
    char *output = xmalloc((size_t)byte_count + 1);
    size_t bytes_read = fread(output, 1, (size_t)byte_count, capture);
    output[bytes_read] = '\0';
    fclose(capture);
    return output;
}

/* ---------- dispatcher: not-a-command / unknown / bad usage ---------- */

struct dispatch_call {
    const char *line;
    struct agent_state *state;
    enum slash_result result;
};

static void do_dispatch(void *user)
{
    struct dispatch_call *c = user;
    c->result = slash_dispatch(c->line, c->state);
}

static void test_dispatch_not_a_command(void)
{
    struct agent_state state = {0};
    struct dispatch_call c = {.line = "hello world", .state = &state};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_NOT_A_COMMAND);
    EXPECT_STR_EQ(out, "");
    free(out);

    c.line = "";
    out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_NOT_A_COMMAND);
    EXPECT_STR_EQ(out, "");
    free(out);
}

static void test_dispatch_unknown(void)
{
    struct render_ctx r = {0};
    r.disp.committed_newlines = 1; /* models the cursor one line below the echoed command */
    struct agent_state state = {.render = &r};
    struct dispatch_call c = {.line = "/nonesuch", .state = &state};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_UNKNOWN);
    EXPECT(strstr(out, "/nonesuch") != NULL);
    EXPECT(strstr(out, "/help") != NULL);
    free(out);
}

static void test_dispatch_path_falls_through(void)
{
    /* Command parsing must not consume absolute paths intended for the model. */
    struct agent_state state = {0};
    const char *paths[] = {
        "/tmp/repro.c crashes, inspect it",
        "/etc/passwd is owned by root",
        "/usr/local/bin/foo",
        "/help.txt is a file",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        struct dispatch_call c = {.line = paths[i], .state = &state};
        char *out = capture_stdout(do_dispatch, &c);
        EXPECT(c.result == SLASH_NOT_A_COMMAND);
        EXPECT_STR_EQ(out, "");
        free(out);
    }
}

static void test_dispatch_control_bytes_fall_through(void)
{
    /* Echoing an invalid command token could execute its terminal control bytes. */
    struct agent_state state = {0};
    struct dispatch_call c = {.line = "/\x1b[2J", .state = &state};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_NOT_A_COMMAND);
    EXPECT_STR_EQ(out, "");
    free(out);
}

static void test_dispatch_bare_slash_falls_through(void)
{
    struct agent_state state = {0};
    struct dispatch_call c = {.line = "/", .state = &state};
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
    struct render_ctx r = {0};
    r.disp.committed_newlines = 1;
    struct agent_state state = {.render = &r};
    struct dispatch_call c = {.line = "/help foo", .state = &state};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_BAD_USAGE);
    EXPECT(strstr(out, "/help") != NULL);
    free(out);
}

/* ---------- /help ---------- */

static void test_help_lists_commands_and_shortcuts(void)
{
    struct render_ctx r = {0};
    r.disp.committed_newlines = 1;
    struct agent_state state = {.render = &r};
    struct dispatch_call c = {.line = "/help", .state = &state};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_HANDLED);

    EXPECT(strstr(out, "commands") != NULL);
    EXPECT(strstr(out, "/new") != NULL);
    EXPECT(strstr(out, "/clear") != NULL);
    EXPECT(strstr(out, "/help") != NULL);
    EXPECT(strstr(out, "shortcuts") != NULL);
    EXPECT(strstr(out, "esc") != NULL);
    EXPECT(strstr(out, "ctrl-t") != NULL);
    free(out);
}

/* /help and /session content is ASCII, so plain byte length measures row width. */
static char *strip_sgr(const char *s)
{
    char *out = xmalloc(strlen(s) + 1);
    size_t n = 0;
    while (*s) {
        if (*s == '\x1b' && s[1] == '[') {
            s += 2;
            while (*s && !(*s >= '@' && *s <= '~'))
                s++;
            if (*s)
                s++;
            continue;
        }
        out[n++] = *s++;
    }
    out[n] = '\0';
    return out;
}

static void expect_rows_fit(const char *out, size_t max_cells)
{
    const char *row = out;
    while (*row) {
        const char *end = strchr(row, '\n');
        size_t row_len = end ? (size_t)(end - row) : strlen(row);
        if (row_len > max_cells)
            FAIL("row exceeds %zu cells: %.*s", max_cells, (int)row_len, row);
        if (!end)
            break;
        row = end + 1;
    }
}

static void test_help_wraps_to_narrow_width(void)
{
    struct render_ctx r = {0};
    r.disp.committed_newlines = 1;
    struct agent_state state = {.render = &r};
    struct dispatch_call c = {.line = "/help", .state = &state};

    setenv("HAX_DISPLAY_WIDTH", "30", 1);
    char *raw = capture_stdout(do_dispatch, &c);
    unsetenv("HAX_DISPLAY_WIDTH");
    EXPECT(c.result == SLASH_HANDLED);

    char *out = strip_sgr(raw);
    free(raw);
    expect_rows_fit(out, 30);
    /* The longest summaries survive the stacked narrow layout intact. */
    EXPECT(strstr(out, "shift-enter") != NULL);
    EXPECT(strstr(out, "configured to send") != NULL);
    free(out);
}

/* ---------- /session ---------- */

static void test_session_prints_totals(void)
{
    struct render_ctx r = {0};
    r.disp.committed_newlines = 1;
    struct agent_state state = {.render = &r};
    state.stats.user_turns = 3;
    state.stats.requests = 7;
    state.stats.tool_calls = 6;
    state.stats.tools[0].name = "bash";
    state.stats.tools[0].count = 4;
    state.stats.tools[1].name = "read";
    state.stats.tools[1].count = 2;
    state.stats.worked_ms = 68000;
    state.stats.input_tokens = 5530;
    state.stats.output_tokens = 412;
    state.stats.cached_tokens = 2048;
    state.stats.cache_write_tokens = 1024;
    state.stats.uncached_input_tokens = 5530 - 2048 - 1024;
    /* Without a provider, only the reported total charge can be displayed. */
    struct stream_usage reported = {.input_tokens = 5530,
                                    .output_tokens = 412,
                                    .cached_tokens = 2048,
                                    .cache_write_tokens = 1024,
                                    .cache_write_1h_tokens = -1,
                                    .cost = 0.042};
    agent_spend_account(&state.stats.spend, &reported, NULL, NULL);
    state.stats.latest_context_tokens = 4000;
    struct dispatch_call c = {.line = "/session", .state = &state};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_HANDLED);
    EXPECT(strstr(out, "not recorded") != NULL);
    EXPECT(strstr(out, "user turns") != NULL);
    EXPECT(strstr(out, "requests") != NULL);
    EXPECT(strstr(out, "tool calls") != NULL);
    EXPECT(strstr(out, "6 · bash 4 · read 2") != NULL);
    EXPECT(strstr(out, "time worked") != NULL);
    EXPECT(strstr(out, "1m 08s") != NULL);
    EXPECT(strstr(out, "context") != NULL);
    EXPECT(strstr(out, "4k") != NULL);
    EXPECT(strstr(out, "tokens total") != NULL);
    EXPECT(strstr(out, "in 2.5k · cache 2k · write 1k · out 412") != NULL);
    EXPECT(strstr(out, "$0.042") != NULL);
    EXPECT(strstr(out, "~$") == NULL);
    free(out);
    agent_spend_free(&state.stats.spend);
}

static void test_session_hides_unreported_rows(void)
{
    struct render_ctx r = {0};
    r.disp.committed_newlines = 1;
    struct agent_state state = {.render = &r};
    struct dispatch_call c = {.line = "/session", .state = &state};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_HANDLED);
    /* Identity rows stay; zero-activity and unknown measurements are omitted. */
    EXPECT(strstr(out, "not recorded") != NULL);
    EXPECT(strstr(out, "provider") != NULL);
    EXPECT(strstr(out, "user turns") == NULL);
    EXPECT(strstr(out, "requests") == NULL);
    EXPECT(strstr(out, "time worked") == NULL);
    EXPECT(strstr(out, "tool calls") == NULL);
    EXPECT(strstr(out, "context") == NULL);
    EXPECT(strstr(out, "tokens") == NULL);
    EXPECT(strstr(out, "$") == NULL);
    free(out);
}

static void test_session_shows_window_before_first_request(void)
{
    struct render_ctx r = {0};
    r.disp.committed_newlines = 1;
    struct agent_state state = {.render = &r};
    struct dispatch_call c = {.line = "/session", .state = &state};

    setenv("HAX_CONTEXT_LIMIT", "262144", 1);
    char *out = capture_stdout(do_dispatch, &c);
    unsetenv("HAX_CONTEXT_LIMIT");
    EXPECT(c.result == SLASH_HANDLED);
    EXPECT(strstr(out, "context") != NULL);
    EXPECT(strstr(out, "? / 262k") != NULL);
    EXPECT(strstr(out, "%") == NULL);
    free(out);
}

static void test_session_marks_estimated_spend(void)
{
    struct render_ctx r = {0};
    r.disp.committed_newlines = 1;
    struct agent_state state = {.render = &r};
    struct stream_usage paid = {-1, -1, -1, -1, -1, 0.030};
    agent_spend_account(&state.stats.spend, &paid, NULL, NULL);
    struct stream_usage u = {.input_tokens = 1000,
                             .output_tokens = 50,
                             .cached_tokens = -1,
                             .cache_write_tokens = -1,
                             .cache_write_1h_tokens = -1,
                             .cost = -1};
    agent_spend_account(&state.stats.spend, &u, NULL, NULL);
    struct dispatch_call c = {.line = "/session", .state = &state};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_HANDLED);
    EXPECT(strstr(out, "~$0.030") != NULL);
    free(out);
    agent_spend_free(&state.stats.spend);
}

static void test_session_wraps_to_narrow_width(void)
{
    struct render_ctx r = {0};
    r.disp.committed_newlines = 1;
    struct agent_state state = {.render = &r};
    state.stats.user_turns = 3;
    state.stats.requests = 7;
    state.stats.input_tokens = 5530;
    state.stats.output_tokens = 412;
    state.stats.cached_tokens = 2048;
    state.stats.cache_write_tokens = 1024;
    state.stats.uncached_input_tokens = 5530 - 2048 - 1024;
    struct dispatch_call c = {.line = "/session", .state = &state};

    setenv("HAX_DISPLAY_WIDTH", "30", 1);
    char *raw = capture_stdout(do_dispatch, &c);
    unsetenv("HAX_DISPLAY_WIDTH");
    EXPECT(c.result == SLASH_HANDLED);

    char *out = strip_sgr(raw);
    free(raw);
    expect_rows_fit(out, 30);
    /* The token row wraps at segment spaces rather than truncating. */
    EXPECT(strstr(out, "tokens total") != NULL);
    EXPECT(strstr(out, "out 412") != NULL);
    free(out);
}

/* ---------- /new and its alias /clear ---------- */

static void seed_session(struct agent_session *session)
{
    agent_session_add_user(session, "first prompt");
    agent_session_append(
        session, (struct item){.kind = ITEM_ASSISTANT_MESSAGE, .text = xstrdup("first reply")});
    agent_session_add_user(session, "second prompt");
}

static void test_new_clears_session_without_switching_preset(void)
{
    struct agent_session s = {0};
    seed_session(&s);
    EXPECT(s.n_items > 0);
    stub_preset_name = NULL;
    struct render_ctx r = {0};
    r.disp.committed_newlines = 1;
    struct agent_state state = {.session = &s, .render = &r};
    struct dispatch_call c = {.line = "/new", .state = &state};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_HANDLED);
    free(out);

    EXPECT(stub_preset_name == NULL);
    EXPECT(s.n_items == 0);
    agent_session_free(&s);
}

static void test_clear_alias_runs_new(void)
{
    struct agent_session s = {0};
    seed_session(&s);
    EXPECT(s.n_items > 0);

    struct render_ctx r = {0};
    r.disp.committed_newlines = 1;
    struct agent_state state = {.session = &s, .render = &r};
    struct dispatch_call c = {.line = "/clear", .state = &state};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_HANDLED);
    free(out);

    EXPECT(s.n_items == 0);
    agent_session_free(&s);
}

static void test_new_with_preset_switches_then_clears(void)
{
    struct agent_session s = {0};
    seed_session(&s);
    EXPECT(s.n_items > 0);

    stub_preset_rc = 0;
    stub_preset_name = NULL;
    stub_preset_announce = -1;
    struct render_ctx r = {0};
    r.disp.committed_newlines = 1;
    struct agent_state state = {.session = &s, .render = &r};
    struct dispatch_call c = {.line = "/new work", .state = &state};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_HANDLED);
    free(out);

    EXPECT(stub_preset_name != NULL && strcmp(stub_preset_name, "work") == 0);
    EXPECT(stub_preset_announce == 0);
    EXPECT(s.n_items == 0);
    agent_session_free(&s);
}

static void test_new_keeps_conversation_when_preset_fails(void)
{
    struct agent_session s = {0};
    seed_session(&s);
    size_t n_before = s.n_items;

    stub_preset_rc = -1;
    struct render_ctx r = {0};
    r.disp.committed_newlines = 1;
    struct agent_state state = {.session = &s, .render = &r};
    struct dispatch_call c = {.line = "/new nwo", .state = &state};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_HANDLED);
    free(out);

    EXPECT(s.n_items == n_before);
    stub_preset_rc = 0;
    agent_session_free(&s);
}

static void test_clear_alias_takes_preset_too(void)
{
    struct agent_session s = {0};
    seed_session(&s);

    stub_preset_rc = 0;
    stub_preset_name = NULL;
    struct render_ctx r = {0};
    r.disp.committed_newlines = 1;
    struct agent_state state = {.session = &s, .render = &r};
    struct dispatch_call c = {.line = "/clear work", .state = &state};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_HANDLED);
    free(out);

    EXPECT(stub_preset_name != NULL && strcmp(stub_preset_name, "work") == 0);
    EXPECT(s.n_items == 0);
    agent_session_free(&s);
}

static void test_preset_save_routes_whole_argument(void)
{
    struct render_ctx r = {0};
    r.disp.committed_newlines = 1;
    struct agent_state state = {.render = &r};

    stub_preset_save_argument = NULL;
    stub_preset_name = NULL;
    struct dispatch_call c = {.line = "/preset-save scout rose", .state = &state};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_HANDLED);
    free(out);
    EXPECT(stub_preset_save_argument != NULL &&
           strcmp(stub_preset_save_argument, "scout rose") == 0);
    EXPECT(stub_preset_name == NULL);

    stub_preset_save_argument = "not overwritten";
    struct dispatch_call bare = {.line = "/preset-save", .state = &state};
    out = capture_stdout(do_dispatch, &bare);
    EXPECT(bare.result == SLASH_HANDLED);
    free(out);
    EXPECT(stub_preset_save_argument == NULL);
}

static void test_dispatch_trims_trailing_whitespace(void)
{
    struct render_ctx r = {0};
    r.disp.committed_newlines = 1;
    struct agent_state state = {.render = &r};
    struct dispatch_call c = {.line = "/help   ", .state = &state};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_HANDLED);
    free(out);
}

static void test_resume_cancelled_picker_keeps_newline_state(void)
{
    /* Cancellation erases an opened picker back to the separator row. */
    stub_picker_shown = 1;
    stub_picker_path = NULL;
    struct render_ctx r = {0};
    r.disp.committed_newlines = 1;
    struct agent_state state = {.render = &r};
    struct dispatch_call c = {.line = "/resume", .state = &state};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_HANDLED);
    EXPECT(r.disp.committed_newlines == 2);
    free(out);
}

static void test_resume_selected_session_keeps_newline_state(void)
{
    /* Selection also erases the picker before replay begins. */
    stub_picker_shown = 1;
    stub_picker_path = "/tmp/some-session.jsonl";
    struct render_ctx r = {0};
    r.disp.committed_newlines = 1;
    struct agent_state state = {.render = &r};
    struct dispatch_call c = {.line = "/resume", .state = &state};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_HANDLED);
    EXPECT(r.disp.committed_newlines == 2);
    free(out);
}

static void test_resume_no_picker_repairs_newline_state(void)
{
    /* Without a picker, its raw note leaves the cursor below the separator row. */
    stub_picker_shown = 0;
    stub_picker_path = NULL;
    struct render_ctx r = {0};
    r.disp.committed_newlines = 1;
    struct agent_state state = {.render = &r};
    struct dispatch_call c = {.line = "/resume", .state = &state};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_HANDLED);
    EXPECT(r.disp.committed_newlines == 1);
    free(out);
}

/* ---------- /undo and /fork routing ---------- */

static void test_undo_fork_empty_conversation(void)
{
    struct render_ctx r = {0};
    r.disp.committed_newlines = 1;
    struct agent_session s = {0};
    struct agent_state state = {.session = &s, .render = &r};

    struct dispatch_call cu = {.line = "/undo", .state = &state};
    char *out = capture_stdout(do_dispatch, &cu);
    EXPECT(cu.result == SLASH_HANDLED);
    EXPECT(strstr(out, "nothing to undo") != NULL);
    free(out);

    struct dispatch_call cf = {.line = "/fork", .state = &state};
    out = capture_stdout(do_dispatch, &cf);
    EXPECT(cf.result == SLASH_HANDLED);
    EXPECT(strstr(out, "nothing to fork") != NULL);
    free(out);
}

static void test_compaction_seed_history_rules(void)
{
    /* A synthetic compaction seed is history but not a selectable user turn. */
    struct agent_session s = {0};
    agent_session_append(&s, (struct item){.kind = ITEM_USER_MESSAGE,
                                           .text = xstrdup("seed"),
                                           .origin = ITEM_ORIGIN_COMPACT_SEED});
    struct render_ctx r = {0};
    r.disp.committed_newlines = 1;
    struct agent_state state = {.session = &s, .render = &r};

    struct dispatch_call cf = {.line = "/fork 0", .state = &state};
    char *out = capture_stdout(do_dispatch, &cf);
    EXPECT(cf.result == SLASH_HANDLED);
    EXPECT(strstr(out, "nothing to fork") == NULL);
    free(out);

    struct dispatch_call ct = {.line = "/fork 0\t", .state = &state};
    out = capture_stdout(do_dispatch, &ct);
    EXPECT(ct.result == SLASH_HANDLED);
    EXPECT(strstr(out, "takes a number") == NULL);
    free(out);

    struct dispatch_call cu = {.line = "/undo 1", .state = &state};
    out = capture_stdout(do_dispatch, &cu);
    EXPECT(cu.result == SLASH_HANDLED);
    EXPECT(strstr(out, "nothing to undo") != NULL);
    free(out);

    struct dispatch_call cp = {.line = "/fork", .state = &state};
    out = capture_stdout(do_dispatch, &cp);
    EXPECT(cp.result == SLASH_HANDLED);
    EXPECT(strstr(out, "nothing to fork") != NULL);
    free(out);

    agent_session_free(&s);
}

int main(void)
{
    /* Row-layout and row-presence assertions depend on these; the variables leak in from any
     * hax parent or user environment. */
    unsetenv("HAX_DISPLAY_WIDTH");
    unsetenv("HAX_CONTEXT_LIMIT");

    test_dispatch_not_a_command();
    test_dispatch_unknown();
    test_dispatch_path_falls_through();
    test_dispatch_control_bytes_fall_through();
    test_dispatch_bare_slash_falls_through();
    test_dispatch_bad_usage();
    test_help_lists_commands_and_shortcuts();
    test_help_wraps_to_narrow_width();
    test_session_prints_totals();
    test_session_hides_unreported_rows();
    test_session_shows_window_before_first_request();
    test_session_marks_estimated_spend();
    test_session_wraps_to_narrow_width();
    test_new_clears_session_without_switching_preset();
    test_clear_alias_runs_new();
    test_new_with_preset_switches_then_clears();
    test_new_keeps_conversation_when_preset_fails();
    test_clear_alias_takes_preset_too();
    test_preset_save_routes_whole_argument();
    test_dispatch_trims_trailing_whitespace();
    test_resume_cancelled_picker_keeps_newline_state();
    test_resume_selected_session_keeps_newline_state();
    test_resume_no_picker_repairs_newline_state();
    test_undo_fork_empty_conversation();
    test_compaction_seed_history_rules();
    T_REPORT();
}
