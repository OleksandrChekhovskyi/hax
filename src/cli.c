/* SPDX-License-Identifier: MIT */
#include "cli.h"

/* getopt_long and struct option live in glibc's bits/getopt_ext.h, which the
 * include cleaner is told to ignore, so it cannot see this header being used. */
#include <getopt.h> // IWYU pragma: keep
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "buf.h"
#include "diag.h"
#include "session.h"
#include "session_picker.h"
#include "util.h"
#include "version.h"
#include "terminal/ansi.h"
#include "terminal/interrupt.h"
#include "terminal/theme.h"
#include "terminal/ui.h"
#include "terminal/width.h"
#include "text/width.h"
#include "tools/bash_env.h"

static const struct help_option {
    const char *flags;
    const char *description;
} HELP_OPTIONS[] = {
    {"-p, --print",
     "Non-interactive mode. Runs the prompt to completion and prints the final assistant "
     "message to stdout. The prompt comes from PROMPT positional arguments (joined with "
     "spaces) when given, otherwise from stdin if stdin is not a terminal."},
    {"-c, --continue", "Resume the most recent conversation in this directory."},
    {"--resume[=ID]",
     "Resume a past conversation in this directory. With no ID, pick one from an interactive "
     "list; with a session ID, resume it directly — the ID form also works with -p. Resuming "
     "restores the provider, model, effort, and preset the conversation was last using; the "
     "selection flags override them."},
    {"--no-session",
     "Don't record this conversation: nothing to resume afterwards, and the prompts you type "
     "aren't added to Ctrl-R recall. Earlier sessions and prompts stay readable."},
    {"--raw",
     "Send only the prompt text — no system prompt, no Environment section, no AGENTS.md, no "
     "skills, and no tools. Useful as a barebones chat interface."},
    {"--bare",
     "Run without project and delegation context — no AGENTS.md, skills, or subagents "
     "section. The Environment section, tools, and base system prompt remain (unlike --raw)."},
    {"--provider=NAME", "Select the backend for this run."},
    {"--model=ID", "Select the model for this run."},
    {"--effort=LEVEL", "Select the reasoning effort for this run."},
    {"--preset=NAME",
     "Apply the named preset — a presets.NAME selection from the config file. Explicit "
     "selection flags win over the preset's values."},
    {"-h, --help", "Show this help and exit."},
    {"-v, --version", "Show version and exit."},
};

void cli_print_help(void)
{
    int output_is_tty = isatty(fileno(stdout));
    const char *chrome = output_is_tty ? theme_open(THEME_CHROME) : "";
    const char *bold = output_is_tty ? ANSI_BOLD : "";
    const char *reset = output_is_tty ? ANSI_RESET : "";
    int columns = display_width();

    const char *tagline = "a minimalist coding assistant in your terminal";
    int name_cells = (int)strlen("hax ") + (int)strlen(HAX_VERSION);
    if (name_cells + 3 + (int)display_cells(tagline) <= columns) {
        printf("%shax %s%s — %s\n", bold, HAX_VERSION, reset, tagline);
    } else {
        printf("%shax %s%s\n", bold, HAX_VERSION, reset);
        ui_wrapped_rows(tagline, 0, columns, "");
    }
    fputc('\n', stdout);

    printf("%susage:%s ", bold, reset);
    ui_wrapped_rows("hax [OPTIONS] [PROMPT...]", (int)strlen("usage: "), columns, "");
    fputc('\n', stdout);

    ui_wrapped_rows("With no arguments, runs an interactive REPL.", 0, columns, "");
    fputc('\n', stdout);
    printf("%soptions%s\n", bold, reset);

    size_t flag_width = 0;
    for (size_t i = 0; i < sizeof(HELP_OPTIONS) / sizeof(*HELP_OPTIONS); i++) {
        size_t width = strlen(HELP_OPTIONS[i].flags);
        if (width > flag_width)
            flag_width = width;
    }
    for (size_t i = 0; i < sizeof(HELP_OPTIONS) / sizeof(*HELP_OPTIONS); i++)
        ui_label_row(HELP_OPTIONS[i].flags, chrome, HELP_OPTIONS[i].description, "",
                     (int)flag_width + 4, columns);

    fputc('\n', stdout);
    ui_wrapped_rows("The selection flags (--provider, --model, --effort, --preset) apply to "
                    "this run only and take priority over every other source. Configuration "
                    "and precedence are covered in README.md.",
                    0, columns, "");
}

static const char *empty_selection_flag(const struct cli_selection *selection)
{
    if (selection->provider && !*selection->provider)
        return "--provider=";
    if (selection->model && !*selection->model)
        return "--model=";
    if (selection->effort && !*selection->effort)
        return "--effort=";
    if (selection->preset && !*selection->preset)
        return "--preset=";
    return NULL;
}

enum cli_parse_result cli_parse(int argc, char **argv, struct cli_options *options)
{
    enum {
        OPT_RAW = 0x100,
        OPT_RESUME,
        OPT_PROVIDER,
        OPT_MODEL,
        OPT_EFFORT,
        OPT_PRESET,
        OPT_BARE,
        OPT_NO_SESSION,
    };
    static const struct option long_options[] = {
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'v'},
        {"print", no_argument, NULL, 'p'},
        {"continue", no_argument, NULL, 'c'},
        {"resume", optional_argument, NULL, OPT_RESUME},
        {"no-session", no_argument, NULL, OPT_NO_SESSION},
        {"raw", no_argument, NULL, OPT_RAW},
        {"bare", no_argument, NULL, OPT_BARE},
        {"provider", required_argument, NULL, OPT_PROVIDER},
        {"model", required_argument, NULL, OPT_MODEL},
        {"effort", required_argument, NULL, OPT_EFFORT},
        {"preset", required_argument, NULL, OPT_PRESET},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    optind = 1;

    int saw_continue = 0;
    int saw_resume = 0;
    int option;
    while ((option = getopt_long(argc, argv, "hpcv", long_options, NULL)) != -1) {
        switch (option) {
        case 'h':
            cli_print_help();
            return CLI_PARSE_EXIT;
        case 'v':
            printf("hax %s\n", HAX_VERSION);
            return CLI_PARSE_EXIT;
        case 'p':
            options->one_shot = 1;
            break;
        case 'c':
            saw_continue = 1;
            options->resume_mode = CLI_RESUME_LATEST;
            break;
        case OPT_RESUME:
            saw_resume = 1;
            options->resume_mode = CLI_RESUME_SELECT;
            options->resume_id = optarg;
            break;
        case OPT_RAW:
            options->agent_options.raw = 1;
            break;
        case OPT_PROVIDER:
            options->selection.provider = optarg;
            break;
        case OPT_MODEL:
            options->selection.model = optarg;
            break;
        case OPT_EFFORT:
            options->selection.effort = optarg;
            break;
        case OPT_PRESET:
            options->selection.preset = optarg;
            break;
        case OPT_BARE:
            options->bare = 1;
            break;
        case OPT_NO_SESSION:
            options->no_session = 1;
            break;
        case '?':
            fprintf(stderr, "Try 'hax --help' for usage.\n");
            return CLI_PARSE_ERROR;
        default:
            return CLI_PARSE_ERROR;
        }
    }

    if (saw_continue && saw_resume) {
        hax_err("use only one of --continue / --resume");
        return CLI_PARSE_ERROR;
    }

    const char *empty_flag = empty_selection_flag(&options->selection);
    if (empty_flag) {
        hax_err("%s requires a value", empty_flag);
        return CLI_PARSE_ERROR;
    }
    if (options->resume_mode == CLI_RESUME_SELECT && options->resume_id && !*options->resume_id) {
        hax_err("--resume= requires a session id");
        return CLI_PARSE_ERROR;
    }
    if (options->one_shot && options->resume_mode == CLI_RESUME_SELECT && !options->resume_id) {
        hax_err("-p with --resume requires a session id (e.g. --resume=ID)");
        return CLI_PARSE_ERROR;
    }
    if (!options->one_shot && optind < argc) {
        hax_err("positional arguments require -p / --print\n"
                "Try 'hax --help' for usage.");
        return CLI_PARSE_ERROR;
    }

    options->first_prompt_arg = optind;
    return CLI_PARSE_OK;
}

static char *join_arguments(int count, char **arguments)
{
    if (count == 0)
        return xstrdup("");

    size_t output_size = 0;
    for (int i = 0; i < count; i++)
        output_size += strlen(arguments[i]) + 1;

    char *output = xmalloc(output_size);
    char *next = output;
    for (int i = 0; i < count; i++) {
        if (i > 0)
            *next++ = ' ';
        size_t length = strlen(arguments[i]);
        memcpy(next, arguments[i], length);
        next += length;
    }
    *next = '\0';
    return output;
}

static char *read_stream(FILE *input)
{
    struct buf buffer;
    buf_init(&buffer);

    char chunk[4096];
    for (;;) {
        size_t bytes_read = fread(chunk, 1, sizeof(chunk), input);
        if (bytes_read > 0)
            buf_append(&buffer, chunk, bytes_read);
        if (bytes_read == sizeof(chunk))
            continue;
        if (ferror(input)) {
            buf_free(&buffer);
            return NULL;
        }
        break;
    }

    return buf_steal(&buffer);
}

static void strip_final_newline(char *text)
{
    size_t length = strlen(text);
    if (length > 0 && text[length - 1] == '\n')
        text[--length] = '\0';
    if (length > 0 && text[length - 1] == '\r')
        text[length - 1] = '\0';
}

int cli_read_prompt(const struct cli_options *options, int argc, char **argv, FILE *input,
                    int input_is_tty, char **prompt)
{
    *prompt = NULL;
    if (!options->one_shot)
        return 0;

    if (options->first_prompt_arg < argc) {
        *prompt =
            join_arguments(argc - options->first_prompt_arg, argv + options->first_prompt_arg);
    } else if (!input_is_tty) {
        *prompt = read_stream(input);
        if (!*prompt) {
            hax_err("failed to read stdin");
            return -1;
        }
        strip_final_newline(*prompt);
    } else {
        hax_err("-p requires a prompt (positional args or piped stdin)");
        return -1;
    }

    if (!**prompt) {
        hax_err("-p prompt is empty");
        free(*prompt);
        *prompt = NULL;
        return -1;
    }
    return 0;
}

static char *resolve_session_id(const char *cwd, const char *id)
{
    struct session_entry *sessions;
    size_t session_count;
    session_list(cwd, &sessions, &session_count);

    char *match = NULL;
    for (size_t i = 0; i < session_count; i++) {
        if (sessions[i].id && strcmp(sessions[i].id, id) == 0) {
            match = xstrdup(sessions[i].path);
            break;
        }
    }

    if (!match) {
        size_t match_count = 0;
        size_t id_length = strlen(id);
        for (size_t i = 0; i < session_count; i++) {
            if (sessions[i].id && strncmp(sessions[i].id, id, id_length) == 0) {
                match_count++;
                free(match);
                match = xstrdup(sessions[i].path);
            }
        }
        if (match_count != 1) {
            free(match);
            match = NULL;
        }
    }

    session_list_free(sessions, session_count);
    return match;
}

enum cli_session_result cli_resolve_session(const struct cli_options *options, char **path)
{
    *path = NULL;
    if (options->resume_mode == CLI_RESUME_NONE)
        return CLI_SESSION_READY;

    char *cwd = getcwd(NULL, 0);
    if (!cwd) {
        hax_err("getcwd failed");
        return CLI_SESSION_ERROR;
    }

    if (options->resume_mode == CLI_RESUME_LATEST) {
        struct session_entry *sessions;
        size_t session_count;
        session_list(cwd, &sessions, &session_count);
        if (session_count > 0)
            *path = xstrdup(sessions[0].path);
        session_list_free(sessions, session_count);

        if (!*path && options->one_shot) {
            hax_err("no past conversation in this directory to continue");
            free(cwd);
            return CLI_SESSION_ERROR;
        }
        if (!*path)
            hax_warn("no past conversation in this directory; starting fresh");
    } else if (options->resume_id) {
        *path = resolve_session_id(cwd, options->resume_id);
        if (!*path) {
            if (strchr(options->resume_id, '/'))
                hax_err("--resume takes a session id, not a path (sessions are per-directory)");
            else
                hax_err("no session matching '%s'", options->resume_id);
            free(cwd);
            return CLI_SESSION_ERROR;
        }
    } else {
        /* The picker enters raw mode before the REPL installs its terminal restore handlers. */
        interrupt_init();
        *path = session_picker_run(cwd, NULL, NULL);
        if (!*path) {
            free(cwd);
            return CLI_SESSION_EXIT;
        }
    }

    free(cwd);
    if (*path)
        (void)session_touch(*path);
    return CLI_SESSION_READY;
}

int cli_check_subagent_depth(void)
{
    /* Depth is process ancestry supplied by the parent, not user configuration. */
    const char *value = getenv("HAX_SUBAGENT_DEPTH");
    if (!value || !*value)
        return 0;

    int depth;
    if (!parse_int(value, &depth) || depth < 0)
        depth = HAX_SUBAGENT_MAX_DEPTH;
    if (depth < HAX_SUBAGENT_MAX_DEPTH)
        return 0;

    hax_err("subagent depth limit (%d) reached — run the task directly instead of spawning "
            "another hax",
            HAX_SUBAGENT_MAX_DEPTH);
    return -1;
}
