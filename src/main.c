/* SPDX-License-Identifier: MIT */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>

#include "agent.h"
#include "config.h"
#include "oneshot.h"
#include "providers/registry.h"
#include "select.h"
#include "session.h"
#include "session_picker.h"
#include "terminal/interrupt.h"
#include "trace.h"
#include "transcript.h"
#include "util.h"

/* Cap agentic -p loops so a confused model can't spin forever in a pipeline. */
#define ONESHOT_MAX_TURNS 100

static struct provider *pick_provider(int print_mode)
{
    const char *which = config_str("provider");
    /* Explicit provider picks fail strictly; an implicit default may autoselect. */
    int chosen = which && *which;
    const struct provider_factory *f = chosen ? provider_find(which) : provider_default();
    if (!f) {
        /* Unknown explicit provider; show the supported set. */
        fprintf(stderr, "hax: unknown provider '%s' (supported: ", which);
        provider_list_names(stderr);
        fprintf(stderr, ")\n");
        return NULL;
    }

    /* Explicit choices and one-shot mode fail strictly; one-shot cannot prompt
     * or emit autoselect chatter onto piped stdout. */
    if (chosen || print_mode)
        return f->new(f->name);

    /* Interactive cold start: try the default, then probe alternatives, else
     * start provider-less so the banner can point at /provider. */
    return provider_autoselect();
}

static const char HELP_TEXT[] =
    "usage: hax [OPTIONS] [PROMPT...]\n"
    "\n"
    "A minimalist coding assistant in your terminal.\n"
    "\n"
    "With no arguments, runs an interactive REPL.\n"
    "\n"
    "Options:\n"
    "  -p, --print      Non-interactive mode. Runs the prompt to completion\n"
    "                   and prints the final assistant message to stdout.\n"
    "                   The prompt comes from PROMPT positional arguments\n"
    "                   (joined with spaces) when given, otherwise from\n"
    "                   stdin if stdin is not a terminal.\n"
    "  -c, --continue   Resume the most recent conversation in this\n"
    "                   directory.\n"
    "      --resume[=ID]\n"
    "                   Resume a past conversation in this directory. With\n"
    "                   no ID, pick one from an interactive list; with a\n"
    "                   session ID, resume it directly — the ID form also\n"
    "                   works with -p.\n"
    "      --raw        Send only the prompt text — no system prompt, no\n"
    "                   environment block, no AGENTS.md, no skills, and no\n"
    "                   tools. Useful as a barebones chat interface.\n"
    "  -h, --help       Show this help and exit.\n"
    "\n"
    "Configuration is via environment variables (HAX_PROVIDER, HAX_MODEL,\n"
    "and the rest) or ~/.config/hax/config.json; env wins. See README.md.\n";

/* Join positional prompt args with spaces. Returns malloc'd. */
static char *join_args(int n, char **argv)
{
    if (n <= 0)
        return xstrdup("");
    size_t total = 0;
    for (int i = 0; i < n; i++)
        total += strlen(argv[i]) + 1;
    char *out = xmalloc(total);
    char *q = out;
    for (int i = 0; i < n; i++) {
        if (i > 0)
            *q++ = ' ';
        size_t len = strlen(argv[i]);
        memcpy(q, argv[i], len);
        q += len;
    }
    *q = '\0';
    return out;
}

/* Read the piped/file stdin prompt for -p. Returns malloc'd. */
static char *read_all_stdin(void)
{
    struct buf b;
    buf_init(&b);
    char chunk[4096];
    for (;;) {
        size_t n = fread(chunk, 1, sizeof(chunk), stdin);
        if (n > 0)
            buf_append(&b, chunk, n);
        if (n < sizeof(chunk)) {
            if (ferror(stdin)) {
                buf_free(&b);
                return NULL;
            }
            break;
        }
    }
    /* buf_steal returns NULL when empty; callers expect a string. */
    if (!b.data)
        return xstrdup("");
    return buf_steal(&b);
}

/* Strip one trailing LF/CRLF from stdin prompts, but leave other whitespace:
 * spaces/tabs may be intentional (markdown breaks, indented code, exact text). */
static void strip_trailing_newline(char *s)
{
    size_t n = strlen(s);
    if (n > 0 && s[n - 1] == '\n')
        s[--n] = '\0';
    if (n > 0 && s[n - 1] == '\r')
        s[--n] = '\0';
}

/* Resolve --resume=ARG (exact id or unique prefix) within `cwd`'s sessions.
 * Returns a malloc'd path, or NULL on no/ambiguous match. */
static char *resolve_resume_arg(const char *cwd, const char *arg)
{
    struct session_entry *list;
    size_t n;
    session_list(cwd, &list, &n);

    char *match = NULL;
    /* Exact id first. */
    for (size_t i = 0; i < n; i++) {
        if (list[i].id && strcmp(list[i].id, arg) == 0) {
            match = xstrdup(list[i].path);
            break;
        }
    }
    /* Else a unique id prefix. */
    if (!match) {
        size_t hits = 0, len = strlen(arg);
        for (size_t i = 0; i < n; i++) {
            if (list[i].id && strncmp(list[i].id, arg, len) == 0) {
                hits++;
                free(match);
                match = xstrdup(list[i].path);
            }
        }
        if (hits != 1) {
            free(match);
            match = NULL;
        }
    }
    session_list_free(list, n);
    return match;
}

int main(int argc, char **argv)
{
    /* Establish UTF-8 LC_CTYPE before any multibyte prompt/UI; leave other
     * locale categories at C so numeric output stays predictable. */
    locale_init_utf8();

    struct hax_opts opts = {0};
    int print_mode = 0;
    int continue_mode = 0;
    int resume_mode = 0;
    const char *resume_arg = NULL;

    /* Initialized up front so error paths can share the goto unwind without
     * crossing declarations they later read. */
    int rc = 1;
    char *prompt = NULL;
    char *resume_path = NULL;

    /* --resume takes an optional ID: bare opens the picker, =ID resumes directly. */
    enum {
        OPT_RAW = 0x100,
        OPT_RESUME,
    };
    static const struct option long_opts[] = {
        {"help", no_argument, NULL, 'h'},     {"print", no_argument, NULL, 'p'},
        {"continue", no_argument, NULL, 'c'}, {"resume", optional_argument, NULL, OPT_RESUME},
        {"raw", no_argument, NULL, OPT_RAW},  {NULL, 0, NULL, 0},
    };

    int c;
    while ((c = getopt_long(argc, argv, "hpc", long_opts, NULL)) != -1) {
        switch (c) {
        case 'h':
            fputs(HELP_TEXT, stdout);
            return 0;
        case 'p':
            print_mode = 1;
            break;
        case 'c':
            continue_mode = 1;
            break;
        case OPT_RESUME:
            resume_mode = 1;
            resume_arg = optarg; /* NULL when given as bare --resume */
            break;
        case OPT_RAW:
            opts.raw = 1;
            break;
        case '?':
            /* getopt_long already printed the diagnostic. */
            fprintf(stderr, "Try 'hax --help' for usage.\n");
            return 1;
        default:
            return 1;
        }
    }

    if (continue_mode && resume_mode) {
        hax_err("use only one of --continue / --resume");
        goto err_prompt;
    }
    /* A bare --resume opens an interactive picker, which makes no sense in
     * -p mode (and would fight prompt-from-stdin). Require an explicit id
     * there; -c stays valid since it auto-selects the most recent. */
    if (print_mode && resume_mode && !resume_arg) {
        hax_err("-p with --resume requires a session id (e.g. --resume=ID)");
        goto err_prompt;
    }

    /* Reject positional args in interactive mode: they would otherwise
     * be silently ignored and the user would be left wondering why
     * `hax fix this bug` did nothing. */
    if (!print_mode && optind < argc) {
        hax_err("positional arguments require -p / --print\n"
                "Try 'hax --help' for usage.");
        goto err_prompt;
    }

    /* Validate the -p prompt before provider startup so prompt errors aren't
     * hidden behind probe/auth failures. */
    if (print_mode) {
        if (optind < argc) {
            prompt = join_args(argc - optind, argv + optind);
        } else if (!isatty(fileno(stdin))) {
            prompt = read_all_stdin();
            if (!prompt) {
                hax_err("failed to read stdin");
                goto err_prompt;
            }
            strip_trailing_newline(prompt);
        } else {
            hax_err("-p requires a prompt (positional args or piped stdin)");
            goto err_prompt;
        }
        if (!*prompt) {
            hax_err("-p prompt is empty");
            goto err_prompt;
        }
    }

    /* Resolve resume state before provider startup so bad ids or cancelled
     * pickers exit cleanly. */
    if (continue_mode || resume_mode) {
        char cwd[4096];
        if (!getcwd(cwd, sizeof(cwd))) {
            hax_err("getcwd failed");
            goto err_prompt;
        }
        if (continue_mode) {
            struct session_entry *list;
            size_t n;
            session_list(cwd, &list, &n);
            if (n > 0)
                resume_path = xstrdup(list[0].path); /* newest by mtime */
            session_list_free(list, n);
            if (!resume_path) {
                /* In -p, --continue without history would silently run the prompt
                 * against empty context; interactive mode can just start fresh. */
                if (print_mode) {
                    hax_err("no past conversation in this directory to continue");
                    goto err_prompt;
                }
                hax_warn("no past conversation in this directory; starting fresh");
            }
        } else if (resume_arg) {
            if (!*resume_arg) {
                /* Empty --resume= would prefix-match every session; refuse it. */
                hax_err("--resume= requires a session id");
                goto err_prompt;
            }
            resume_path = resolve_resume_arg(cwd, resume_arg);
            if (!resume_path) {
                if (strchr(resume_arg, '/'))
                    hax_err("--resume takes a session id, not a path "
                            "(sessions are per-directory)");
                else
                    hax_err("no session matching '%s'", resume_arg);
                goto err_prompt;
            }
        } else {
            /* The picker enters raw mode before agent_run() would install restore
             * handlers; install them here so signals/exit leave the shell cooked. */
            interrupt_init();
            resume_path = session_picker_run(cwd, NULL, NULL);
            if (!resume_path) {
                /* Cancelled, or nothing to resume / no TTY — not an error. */
                rc = 0;
                goto err_prompt;
            }
        }
    }
    opts.resume_path = resume_path;

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        hax_err("curl_global_init failed");
        goto err_prompt;
    }

    /* Load the user config first: everything below resolves settings
     * through config_str/etc. (env still wins over the file), including
     * the trace/transcript paths. */
    config_init();

    /* Truncate HAX_TRACE/HAX_TRANSCRIPT before provider startup so fast-fail
     * runs don't leave stale files. The transcript header is written later
     * when sys+tools are known. */
    trace_init();
    transcript_log_init();

    struct provider *p = pick_provider(print_mode);
    /* Provider construction failure is fatal only for -p; the REPL can start
     * provider-less and point the user at /provider. */
    if (!p && print_mode)
        goto err_curl;

    /* agent_run writes back the live provider after /provider swaps; one-shot doesn't. */
    rc = print_mode ? oneshot_run(p, prompt, &opts, ONESHOT_MAX_TURNS) : agent_run(&p, &opts);

    /* destroy() joins provider background work before curl_global_cleanup(). */
    if (p)
        p->destroy(p);
err_curl:
    curl_global_cleanup();
err_prompt:
    config_free();
    free(prompt);
    free(resume_path);
    return rc;
}
