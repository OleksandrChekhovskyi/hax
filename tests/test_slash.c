/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent.h"
#include "agent_core.h"
#include "harness.h"
#include "provider.h"
#include "slash.h"
#include "tool.h"
#include "xalloc.h"
#include "render/render_ctx.h"
#include "terminal/input_core.h"

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
    EXPECT(strstr(out, "start a fresh conversation [preset]") != NULL);
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

/* One user turn: prompt, reply, and a footer whose costs are given directly, as a provider
 * that reports charges would leave them. */
static void add_priced_user_turn(struct agent_session *session, const char *provider,
                                 const char *model, long input, long output, long cached,
                                 long cache_write, double cost, int estimated)
{
    agent_session_add_user(session, "prompt");
    agent_session_append(session,
                         (struct item){.kind = ITEM_ASSISTANT_MESSAGE, .text = xstrdup("reply")});
    struct turn_usage *usage = xcalloc(1, sizeof(*usage));
    usage->usage =
        (struct stream_usage){input, output, cached, cache_write, -1, estimated ? -1 : cost};
    usage->elapsed_ms = 1000;
    usage->uncached_input_tokens =
        input - (cached > 0 ? cached : 0) - (cache_write > 0 ? cache_write : 0);
    usage->cost_input = -1;
    usage->cost_cache_read = -1;
    usage->cost_cache_write = -1;
    usage->cost_output = -1;
    usage->cost_total = cost;
    usage->cost_estimated = estimated;
    agent_session_append(session, (struct item){.kind = ITEM_TURN_USAGE,
                                                .usage = usage,
                                                .provider = xstrdup(provider),
                                                .model = xstrdup(model)});
}

static void add_tool_call(struct agent_session *session, const char *tool_name)
{
    agent_session_append(session, (struct item){.kind = ITEM_TOOL_CALL,
                                                .call_id = xstrdup("c"),
                                                .tool_name = xstrdup(tool_name),
                                                .tool_arguments_json = xstrdup("{}")});
}

static void test_session_prints_totals(void)
{
    struct render_ctx r = {0};
    r.disp.committed_newlines = 1;
    struct agent_session s = {0};
    add_priced_user_turn(&s, "prov", "m", 2000, 200, 1024, 512, 0.02, 0);
    add_tool_call(&s, "bash");
    add_tool_call(&s, "bash");
    add_tool_call(&s, "read");
    add_priced_user_turn(&s, "prov", "m", 3530, 212, 1024, 512, 0.022, 0);
    agent_session_add_worked(&s, 68000);
    struct agent_state state = {.session = &s, .render = &r};
    struct dispatch_call c = {.line = "/session", .state = &state};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_HANDLED);
    EXPECT(strstr(out, "not recorded") != NULL);
    EXPECT(strstr(out, "user turns") != NULL);
    EXPECT(strstr(out, "requests") != NULL);
    EXPECT(strstr(out, "tool calls") != NULL);
    EXPECT(strstr(out, "3 · bash 2 · read 1") != NULL);
    EXPECT(strstr(out, "time worked") != NULL);
    EXPECT(strstr(out, "1m 08s") != NULL);
    EXPECT(strstr(out, "context") != NULL);
    EXPECT(strstr(out, "3.7k") != NULL);
    EXPECT(strstr(out, "tokens") != NULL);
    EXPECT(strstr(out, "in 2.5k · cache 2k · write 1k · out 412") != NULL);
    EXPECT(strstr(out, "$0.042") != NULL);
    EXPECT(strstr(out, "~$") == NULL);
    EXPECT(strstr(out, "undone") == NULL);
    free(out);
    agent_session_free(&s);
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
    struct agent_session s = {0};
    add_priced_user_turn(&s, "prov", "m", 1000, 50, -1, -1, 0.010, 0);
    add_priced_user_turn(&s, "prov", "m", 1000, 50, -1, -1, 0.020, 1);
    struct agent_state state = {.session = &s, .render = &r};
    struct dispatch_call c = {.line = "/session", .state = &state};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_HANDLED);
    EXPECT(strstr(out, "~$0.030") != NULL);
    free(out);
    agent_session_free(&s);
}

/* A model switch mid-conversation gets one token row per model, and undone user turns stay in
 * the totals, flagged on the count the screen no longer shows. */
static void test_session_splits_models_and_counts_undone(void)
{
    struct render_ctx r = {0};
    r.disp.committed_newlines = 1;
    struct agent_session s = {0};
    add_priced_user_turn(&s, "prov", "small", 1000, 100, -1, -1, 0.01, 0);
    add_priced_user_turn(&s, "prov", "large", 2000, 200, -1, -1, 0.10, 0);
    add_priced_user_turn(&s, "prov", "large", 3000, 300, -1, -1, 0.20, 0);
    agent_session_retire(&s, items_user_turn_cut(s.items, s.n_items, 2));
    struct agent_state state = {.session = &s, .render = &r};
    struct dispatch_call c = {.line = "/session", .state = &state};
    char *out = capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_HANDLED);
    EXPECT(strstr(out, "prov · small") != NULL);
    EXPECT(strstr(out, "$0.010 · in 1k · out 100") != NULL);
    EXPECT(strstr(out, "prov · large") != NULL);
    EXPECT(strstr(out, "$0.300 · in 5k · out 500") != NULL);
    EXPECT(strstr(out, "2 · 1 undone") != NULL);
    EXPECT(strstr(out, "$0.31") != NULL);
    free(out);
    agent_session_free(&s);
}

static void test_session_wraps_to_narrow_width(void)
{
    struct render_ctx r = {0};
    r.disp.committed_newlines = 1;
    struct agent_session s = {0};
    add_priced_user_turn(&s, "prov", "m", 5530, 412, 2048, 1024, -1, 1);
    struct agent_state state = {.session = &s, .render = &r};
    struct dispatch_call c = {.line = "/session", .state = &state};

    setenv("HAX_DISPLAY_WIDTH", "30", 1);
    char *raw = capture_stdout(do_dispatch, &c);
    unsetenv("HAX_DISPLAY_WIDTH");
    EXPECT(c.result == SLASH_HANDLED);

    char *out = strip_sgr(raw);
    free(raw);
    expect_rows_fit(out, 30);
    /* The token row wraps at segment spaces rather than truncating. */
    EXPECT(strstr(out, "tokens") != NULL);
    EXPECT(strstr(out, "out 412") != NULL);
    free(out);
    agent_session_free(&s);
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

/* ---------- name completion and prompt hints ---------- */

static void expect_completion(const char *prefix, const char *expected)
{
    char *completion = slash_complete_name(prefix);

    if (!expected)
        EXPECT(completion == NULL);
    else if (!completion)
        FAIL("no completion for '%s', expected '%s'", prefix, expected);
    else
        EXPECT_STR_EQ(completion, expected);
    free(completion);
}

static void test_complete_name_like_a_shell(void)
{
    expect_completion("mo", "model ");
    expect_completion("help", "help ");
    expect_completion("cle", "clear ");
    expect_completion("pre", "preset");
    expect_completion("preset", NULL);
    expect_completion("preset-", "preset-save ");
    expect_completion("c", NULL);
    expect_completion("", NULL);
    expect_completion("zzz", NULL);
}

static void expect_candidates(const char *prefix, const char *expected)
{
    char *candidates = slash_name_candidates(prefix);

    if (!expected)
        EXPECT(candidates == NULL);
    else if (!candidates)
        FAIL("no candidates for '%s', expected '%s'", prefix, expected);
    else
        EXPECT_STR_EQ(candidates, expected);
    free(candidates);
}

static void test_name_candidates_list_ambiguous_prefixes(void)
{
    expect_candidates("c", "/clear /config /compact /copy");
    expect_candidates("preset", "/preset /preset-save");
    expect_candidates("mo", NULL);
    expect_candidates("zzz", NULL);

    char *all = slash_name_candidates("");
    EXPECT(all && strncmp(all, "/new /clear /resume ", 20) == 0);
    free(all);

    char *listing = slash_completer.candidates("pre", slash_completer.user);
    EXPECT(listing != NULL);
    if (listing)
        EXPECT_STR_EQ(listing, "  /preset /preset-save");
    free(listing);

    char *bare = slash_completer.candidates("", slash_completer.user);
    EXPECT(bare != NULL);
    if (bare)
        EXPECT_STR_EQ(bare, "  see /help");
    free(bare);
}

static int match_name(const char *buffer, size_t len, size_t cursor, size_t *start, size_t *end)
{
    return slash_completer.match(buffer, len, cursor, start, end, slash_completer.user);
}

static void test_completer_matches_name_at_cursor(void)
{
    size_t start = 999;
    size_t end = 999;

    EXPECT(match_name("/mo", 3, 3, &start, &end) == 1);
    EXPECT(start == 1);
    EXPECT(end == 3);

    EXPECT(match_name("/", 1, 1, &start, &end) == 1);
    EXPECT(start == 1);
    EXPECT(end == 1);

    EXPECT(match_name("/mo x", 5, 3, &start, &end) == 1);
    EXPECT(end == 3);

    EXPECT(match_name("/mo x", 5, 5, &start, &end) == 0);
    EXPECT(match_name("/mo", 3, 2, &start, &end) == 0);
    EXPECT(match_name("/home/x", 7, 7, &start, &end) == 0);
    EXPECT(match_name("hello", 5, 5, &start, &end) == 0);
    EXPECT(match_name("@foo", 4, 4, &start, &end) == 0);
    EXPECT(match_name("", 0, 0, &start, &end) == 0);
}

static void expect_hint(const char *line, const char *expected)
{
    char *hint = slash_hint(line);

    if (!expected)
        EXPECT(hint == NULL);
    else if (!hint)
        FAIL("no hint for '%s', expected '%s'", line, expected);
    else
        EXPECT_STR_EQ(hint, expected);
    free(hint);
}

static void test_hint_shows_argument_placeholder(void)
{
    expect_hint("/new", " [preset]");
    expect_hint("/new ", "[preset]");
    expect_hint("/new   ", "[preset]");
    expect_hint("/preset", " [name]");
    expect_hint("/clear", " [preset]");
}

static void test_hint_stays_quiet_otherwise(void)
{
    expect_hint("/mo", NULL);
    expect_hint("/pre", NULL);
    expect_hint("/", NULL);
    expect_hint("/zzz", NULL);
    expect_hint("/zzz x", NULL);
    expect_hint("/model", NULL);
    expect_hint("/model ", NULL);
    expect_hint("/model foo", NULL);
    expect_hint("/new foo", NULL);
}

static void test_hint_ignores_non_commands(void)
{
    expect_hint("", NULL);
    expect_hint("hello", NULL);
    expect_hint("/home/x", NULL);
    expect_hint("/mo\n", NULL);
    expect_hint("/new\nfoo", NULL);
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
    test_session_splits_models_and_counts_undone();
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
    test_complete_name_like_a_shell();
    test_name_candidates_list_ambiguous_prefixes();
    test_completer_matches_name_at_cursor();
    test_hint_shows_argument_placeholder();
    test_hint_stays_quiet_otherwise();
    test_hint_ignores_non_commands();
    T_REPORT();
}
