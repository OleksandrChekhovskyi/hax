/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent.h"
#include "agent_core.h"
#include "config.h"
#include "harness.h"
#include "output.h"
#include "provider.h"
#include "slash.h"
#include "tool.h"
#include "xalloc.h"
#include "render/render_ctx.h"
#include "terminal/input_core.h"
#include "text/completion.h"

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

/* Stubs copy borrowed arguments, which the dispatcher frees once the handler returns. */
static void record_argument(char **slot, const char *value)
{
    free(*slot);
    *slot = value ? xstrdup(value) : NULL;
}

/* Selector stubs expose only routing state relevant to slash commands. */
static char *stub_selector_argument = NULL;
void select_provider(struct agent_state *state, const char *provider)
{
    (void)state;
    record_argument(&stub_selector_argument, provider);
}
void select_model(struct agent_state *state, const char *model)
{
    (void)state;
    record_argument(&stub_selector_argument, model);
}
void select_effort(struct agent_state *state, const char *level)
{
    (void)state;
    record_argument(&stub_selector_argument, level);
}
/* Choice stubs show which state completion hands the selectors. */
static struct agent_state *stub_choices_state = NULL;
void select_provider_choices(struct completion *choices)
{
    completion_add(choices, "mock");
    completion_add(choices, "openai");
}
void select_model_choices(struct agent_state *state, struct completion *choices)
{
    (void)state;
    completion_add(choices, "anthropic/claude-sonnet-4");
    completion_add(choices, "openai/gpt-5");
    completion_add(choices, "openai/gpt-5-mini");
    choices->separator = '/';
}
void select_effort_choices(struct agent_state *state, struct completion *choices)
{
    stub_choices_state = state;
    completion_add(choices, "low");
    completion_add(choices, "high");
    completion_add(choices, "default");
}
static int stub_preset_rc = 0;
static char *stub_preset_name = NULL;
static int stub_preset_announce = -1;
int select_preset(struct agent_state *state, const char *name, int announce)
{
    (void)state;
    record_argument(&stub_preset_name, name);
    stub_preset_announce = announce;
    return stub_preset_rc;
}
static char *stub_preset_save_argument = NULL;
void select_preset_save(struct agent_state *state, const char *argument)
{
    (void)state;
    record_argument(&stub_preset_save_argument, argument);
}
void select_config(struct agent_state *state, const char *argument)
{
    (void)state;
    (void)argument;
}
void select_config_key_choices(struct completion *choices)
{
    completion_add(choices, "bash.timeout");
    completion_add(choices, "bash.timeout_max");
    completion_add(choices, "bash.shell");
    completion_add(choices, "theme");
    choices->separator = '.';
}
static char *stub_value_key = NULL;
void select_config_value_choices(const char *key, struct completion *choices)
{
    record_argument(&stub_value_key, key);
    completion_add(choices, "dark");
    completion_add(choices, "default");
}
void select_tint_choices(struct completion *choices)
{
    completion_add(choices, "teal");
    completion_add(choices, "violet");
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
    char *out = t_capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_NOT_A_COMMAND);
    EXPECT_STR_EQ(out, "");
    free(out);

    c.line = "";
    out = t_capture_stdout(do_dispatch, &c);
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
    char *out = t_capture_stdout(do_dispatch, &c);
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
        char *out = t_capture_stdout(do_dispatch, &c);
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
    char *out = t_capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_NOT_A_COMMAND);
    EXPECT_STR_EQ(out, "");
    free(out);
}

static void test_dispatch_bare_slash_falls_through(void)
{
    struct agent_state state = {0};
    struct dispatch_call c = {.line = "/", .state = &state};
    char *out = t_capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_NOT_A_COMMAND);
    EXPECT_STR_EQ(out, "");
    free(out);

    c.line = "/   ";
    out = t_capture_stdout(do_dispatch, &c);
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
    char *out = t_capture_stdout(do_dispatch, &c);
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
    char *out = t_capture_stdout(do_dispatch, &c);
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

static void test_help_wraps_to_narrow_width(void)
{
    struct render_ctx r = {0};
    r.disp.committed_newlines = 1;
    struct agent_state state = {.render = &r};
    struct dispatch_call c = {.line = "/help", .state = &state};

    setenv("HAX_DISPLAY_WIDTH", "30", 1);
    char *raw = t_capture_stdout(do_dispatch, &c);
    unsetenv("HAX_DISPLAY_WIDTH");
    EXPECT(c.result == SLASH_HANDLED);

    char *out = t_strip_sgr(raw);
    free(raw);
    t_expect_rows_fit(out, 30);
    /* The longest summaries survive the stacked narrow layout intact. */
    EXPECT(strstr(out, "shift-enter") != NULL);
    EXPECT(strstr(out, "configured to send") != NULL);
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
    record_argument(&stub_preset_name, NULL);
    struct render_ctx r = {0};
    r.disp.committed_newlines = 1;
    struct agent_state state = {.session = &s, .render = &r};
    struct dispatch_call c = {.line = "/new", .state = &state};
    char *out = t_capture_stdout(do_dispatch, &c);
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
    char *out = t_capture_stdout(do_dispatch, &c);
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
    record_argument(&stub_preset_name, NULL);
    stub_preset_announce = -1;
    struct render_ctx r = {0};
    r.disp.committed_newlines = 1;
    struct agent_state state = {.session = &s, .render = &r};
    struct dispatch_call c = {.line = "/new work", .state = &state};
    char *out = t_capture_stdout(do_dispatch, &c);
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
    char *out = t_capture_stdout(do_dispatch, &c);
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
    record_argument(&stub_preset_name, NULL);
    struct render_ctx r = {0};
    r.disp.committed_newlines = 1;
    struct agent_state state = {.session = &s, .render = &r};
    struct dispatch_call c = {.line = "/clear work", .state = &state};
    char *out = t_capture_stdout(do_dispatch, &c);
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

    record_argument(&stub_preset_save_argument, NULL);
    record_argument(&stub_preset_name, NULL);
    struct dispatch_call c = {.line = "/preset-save scout rose", .state = &state};
    char *out = t_capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_HANDLED);
    free(out);
    EXPECT(stub_preset_save_argument != NULL &&
           strcmp(stub_preset_save_argument, "scout rose") == 0);
    EXPECT(stub_preset_name == NULL);

    record_argument(&stub_preset_save_argument, "not overwritten");
    struct dispatch_call bare = {.line = "/preset-save", .state = &state};
    out = t_capture_stdout(do_dispatch, &bare);
    EXPECT(bare.result == SLASH_HANDLED);
    free(out);
    EXPECT(stub_preset_save_argument == NULL);
}

static void test_selectors_receive_arguments(void)
{
    struct render_ctx r = {0};
    r.disp.committed_newlines = 1;
    struct agent_state state = {.render = &r};
    const char *const lines[][2] = {
        {"/provider openrouter", "openrouter"},
        {"/model vendor/model-1", "vendor/model-1"},
        {"/effort high", "high"},
        {"/effort", NULL},
    };

    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        record_argument(&stub_selector_argument, "not called");
        struct dispatch_call c = {.line = lines[i][0], .state = &state};
        char *out = t_capture_stdout(do_dispatch, &c);
        free(out);
        EXPECT(c.result == SLASH_HANDLED);
        if (!lines[i][1])
            EXPECT(stub_selector_argument == NULL);
        else
            EXPECT(stub_selector_argument && strcmp(stub_selector_argument, lines[i][1]) == 0);
    }
}

static void test_dispatch_trims_trailing_whitespace(void)
{
    struct render_ctx r = {0};
    r.disp.committed_newlines = 1;
    struct agent_state state = {.render = &r};
    struct dispatch_call c = {.line = "/help   ", .state = &state};
    char *out = t_capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_HANDLED);
    free(out);

    /* Completion leaves a space after the argument it fills in. */
    record_argument(&stub_preset_name, NULL);
    struct dispatch_call preset = {.line = "/preset  focus \t", .state = &state};
    out = t_capture_stdout(do_dispatch, &preset);
    EXPECT(preset.result == SLASH_HANDLED);
    free(out);
    EXPECT(stub_preset_name != NULL && strcmp(stub_preset_name, "focus") == 0);
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
    char *out = t_capture_stdout(do_dispatch, &c);
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
    char *out = t_capture_stdout(do_dispatch, &c);
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
    char *out = t_capture_stdout(do_dispatch, &c);
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
    char *out = t_capture_stdout(do_dispatch, &cu);
    EXPECT(cu.result == SLASH_HANDLED);
    EXPECT(strstr(out, "nothing to undo") != NULL);
    free(out);

    struct dispatch_call cf = {.line = "/fork", .state = &state};
    out = t_capture_stdout(do_dispatch, &cf);
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
    char *out = t_capture_stdout(do_dispatch, &cf);
    EXPECT(cf.result == SLASH_HANDLED);
    EXPECT(strstr(out, "nothing to fork") == NULL);
    free(out);

    struct dispatch_call ct = {.line = "/fork 0\t", .state = &state};
    out = t_capture_stdout(do_dispatch, &ct);
    EXPECT(ct.result == SLASH_HANDLED);
    EXPECT(strstr(out, "takes a number") == NULL);
    free(out);

    struct dispatch_call cu = {.line = "/undo 1", .state = &state};
    out = t_capture_stdout(do_dispatch, &cu);
    EXPECT(cu.result == SLASH_HANDLED);
    EXPECT(strstr(out, "nothing to undo") != NULL);
    free(out);

    struct dispatch_call cp = {.line = "/fork", .state = &state};
    out = t_capture_stdout(do_dispatch, &cp);
    EXPECT(cp.result == SLASH_HANDLED);
    EXPECT(strstr(out, "nothing to fork") != NULL);
    free(out);

    agent_session_free(&s);
}

/* ---------- /session and /tasks routing ---------- */

/* Their modules test the output; this pins the table entries that reach them. */
static void test_status_commands_reach_their_modules(void)
{
    struct render_ctx r = {0};
    r.disp.committed_newlines = 1;
    struct agent_state state = {.render = &r};

    struct dispatch_call c = {.line = "/session", .state = &state};
    char *out = t_capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_HANDLED);
    EXPECT(strstr(out, "provider") != NULL);
    free(out);

    c = (struct dispatch_call){.line = "/tasks", .state = &state};
    out = t_capture_stdout(do_dispatch, &c);
    EXPECT(c.result == SLASH_HANDLED);
    EXPECT(strstr(out, "no background tasks") != NULL);
    free(out);
}

/* ---------- completion and prompt hints ---------- */

static struct agent_state completion_state;
static struct input_completer slash_completer;

static void expect_completion(const char *text, const char *expected)
{
    char *completion = slash_completer.complete(text, slash_completer.user);

    if (!expected)
        EXPECT(completion == NULL);
    else if (!completion)
        FAIL("no completion for '%s', expected '%s'", text, expected);
    else
        EXPECT_STR_EQ(completion, expected);
    free(completion);
}

static void expect_candidates(const char *text, const char *expected)
{
    char *candidates = slash_completer.candidates(text, slash_completer.user);

    if (!expected)
        EXPECT(candidates == NULL);
    else if (!candidates)
        FAIL("no candidates for '%s', expected '%s'", text, expected);
    else
        EXPECT_STR_EQ(candidates, expected);
    free(candidates);
}

static void test_complete_names_and_aliases(void)
{
    expect_completion("mo", "model ");
    expect_completion("cle", "clear ");
    expect_completion("pre", "preset");
    expect_completion("zzz", NULL);
}

static void test_name_candidates_list_ambiguous_prefixes(void)
{
    expect_candidates("c", "  /clear /config /compact /copy");
    expect_candidates("mo", NULL);
    expect_candidates("", "  see /help");
}

static void test_complete_preset_arguments(void)
{
    EXPECT(config_load("{\"presets\": {\"review\": {\"provider\": \"mock\"},"
                       "\"fast\": {\"provider\": \"mock\"},"
                       "\"focus\": {\"provider\": \"mock\"},"
                       "\"code review\": {\"provider\": \"mock\"},"
                       "\"code write\": {\"provider\": \"mock\"}}}") == 0);

    expect_completion("preset r", "preset review ");
    expect_completion("preset  re", "preset  review ");
    expect_completion("new re", "new review ");
    expect_completion("clear re", "clear review ");
    expect_candidates("preset ", "  fast focus review");

    expect_completion("preset c", NULL);
    expect_completion("preset review r", NULL);
    expect_completion("compact r", NULL);
    expect_completion("zzz r", NULL);

    EXPECT(config_load(NULL) == 0);
    expect_completion("preset ", NULL);
    expect_candidates("preset ", NULL);
}

static void test_complete_selection_arguments(void)
{
    expect_completion("provider o", "provider openai ");
    expect_candidates("provider ", "  mock openai");
    expect_completion("effort h", "effort high ");
    EXPECT(stub_choices_state == &completion_state);
    expect_candidates("effort ", "  low high default");
    expect_completion("effort high h", NULL);
    expect_completion("model op", "model openai/");
    expect_completion("model openai/", "model openai/gpt-5");
    expect_completion("model gpt", NULL);
    expect_candidates("model ", "  anthropic/ openai/");
    expect_candidates("model openai/gpt", "  gpt-5 gpt-5-mini");
}

static void test_complete_config_arguments(void)
{
    expect_completion("config ba", "config bash.");
    expect_candidates("config ", "  bash. theme");
    expect_candidates("config bash.", "  timeout timeout_max shell");
    expect_completion("config theme da", "config theme dark ");
    EXPECT_STR_EQ(stub_value_key, "theme");
    expect_candidates("config theme ", "  dark default");
    expect_completion("config theme dark da", NULL);
}

static void test_complete_preset_save_arguments(void)
{
    EXPECT(config_load("{\"presets\": {\"review\": {\"provider\": \"mock\"}}}") == 0);

    expect_completion("preset-save r", "preset-save review ");
    expect_completion("preset-save review t", "preset-save review teal ");
    expect_candidates("preset-save fresh ", "  teal violet");
    expect_completion("preset-save review teal t", NULL);

    EXPECT(config_load(NULL) == 0);
}

static void test_complete_task_arguments(void)
{
    expect_completion("tasks k", "tasks kill ");
    expect_completion("tasks kill a", "tasks kill all ");
}

static void test_complete_login_arguments(void)
{
    setenv("XDG_STATE_HOME", t_tempdir(), 1);
    expect_completion("login c", "login codex ");
    expect_completion("login codex c", NULL);
    /* Nothing is logged in to log out of. */
    expect_completion("logout c", NULL);
}

static int match_word(const char *buffer, size_t cursor, size_t *start, size_t *end)
{
    return slash_completer.match(buffer, strlen(buffer), cursor, start, end, slash_completer.user);
}

static void test_completer_matches_word_at_cursor(void)
{
    size_t start = 999;
    size_t end = 999;

    EXPECT(match_word("/mo", 3, &start, &end) == 1);
    EXPECT(start == 1);
    EXPECT(end == 3);

    EXPECT(match_word("/", 1, &start, &end) == 1);
    EXPECT(start == 1);
    EXPECT(end == 1);

    EXPECT(match_word("/mo x", 3, &start, &end) == 1);
    EXPECT(end == 3);

    /* The span runs from the name through the argument word ending at the cursor. */
    EXPECT(match_word("/preset x", 9, &start, &end) == 1);
    EXPECT(start == 1);
    EXPECT(end == 9);
    EXPECT(match_word("/preset ", 8, &start, &end) == 1);
    EXPECT(end == 8);

    EXPECT(match_word("/preset x", 8, &start, &end) == 0);
    EXPECT(match_word("/compact @src", 13, &start, &end) == 0);
    EXPECT(match_word("/zzz x", 6, &start, &end) == 0);
    EXPECT(match_word("/mo", 2, &start, &end) == 0);
    EXPECT(match_word("/new\nfoo", 8, &start, &end) == 0);
    EXPECT(match_word("/home/x", 7, &start, &end) == 0);
    EXPECT(match_word("hello", 5, &start, &end) == 0);
    EXPECT(match_word("@foo", 4, &start, &end) == 0);
    EXPECT(match_word("", 0, &start, &end) == 0);
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
    expect_hint("/effort", " [level]");
}

static void test_hint_shows_later_argument_placeholder(void)
{
    expect_hint("/preset-save review ", "[tint]");
    expect_hint("/preset-save review", NULL);
    expect_hint("/preset-save review teal ", NULL);
    expect_hint("/config theme ", "[value]");
    expect_hint("/config  theme  ", "[value]");
    expect_hint("/config catalog.url ", NULL);
    expect_hint("/config zzz ", NULL);
    expect_hint("/tasks kill ", "<id>... | all");
    expect_hint("/tasks kill t1 ", NULL);
}

static void test_hint_stays_quiet_otherwise(void)
{
    expect_hint("/mo", NULL);
    expect_hint("/pre", NULL);
    expect_hint("/", NULL);
    expect_hint("/zzz", NULL);
    expect_hint("/zzz x", NULL);
    expect_hint("/copy", NULL);
    expect_hint("/copy ", NULL);
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
    /* Row-layout and /tasks assertions depend on these; the variables leak in from any hax parent
     * or user environment. */
    unsetenv("HAX_DISPLAY_WIDTH");
    unsetenv("HAX_NO_TASKS");
    slash_completer_init(&slash_completer, &completion_state);

    test_dispatch_not_a_command();
    test_dispatch_unknown();
    test_dispatch_path_falls_through();
    test_dispatch_control_bytes_fall_through();
    test_dispatch_bare_slash_falls_through();
    test_dispatch_bad_usage();
    test_help_lists_commands_and_shortcuts();
    test_help_wraps_to_narrow_width();
    test_new_clears_session_without_switching_preset();
    test_clear_alias_runs_new();
    test_new_with_preset_switches_then_clears();
    test_new_keeps_conversation_when_preset_fails();
    test_clear_alias_takes_preset_too();
    test_preset_save_routes_whole_argument();
    test_selectors_receive_arguments();
    test_dispatch_trims_trailing_whitespace();
    test_resume_cancelled_picker_keeps_newline_state();
    test_resume_selected_session_keeps_newline_state();
    test_resume_no_picker_repairs_newline_state();
    test_undo_fork_empty_conversation();
    test_compaction_seed_history_rules();
    test_status_commands_reach_their_modules();
    test_complete_names_and_aliases();
    test_name_candidates_list_ambiguous_prefixes();
    test_complete_preset_arguments();
    test_complete_selection_arguments();
    test_complete_config_arguments();
    test_complete_preset_save_arguments();
    test_complete_task_arguments();
    test_complete_login_arguments();
    test_completer_matches_word_at_cursor();
    test_hint_shows_argument_placeholder();
    test_hint_shows_later_argument_placeholder();
    test_hint_stays_quiet_otherwise();
    test_hint_ignores_non_commands();
    T_REPORT();
}
