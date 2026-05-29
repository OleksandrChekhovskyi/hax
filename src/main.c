/* SPDX-License-Identifier: MIT */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>

#include "agent.h"
#include "oneshot.h"
#include "providers/registry.h"
#include "session.h"
#include "session_picker.h"
#include "trace.h"
#include "transcript.h"
#include "util.h"

/* Hard cap on agentic round-trips in -p mode. Generous — a real task
 * rarely needs more than a couple of dozen — but bounded so a confused
 * model can't spin forever in a pipeline where Ctrl-C may not reach us
 * cleanly. No CLI knob for this yet; add one if a real use case shows up. */
#define ONESHOT_MAX_TURNS 100

static struct provider *pick_provider(void)
{
    const char *which = getenv("HAX_PROVIDER");
    if (!which || !*which)
        which = PROVIDER_DEFAULT_NAME;

    const struct provider_factory *f = provider_find(which);
    if (f)
        return f->new();

    fprintf(stderr, "hax: unknown HAX_PROVIDER='%s' (supported: ", which);
    provider_list_names(stderr);
    fprintf(stderr, ")\n");
    return NULL;
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
    "Configuration is via environment variables; see README.md for\n"
    "HAX_PROVIDER, HAX_MODEL, and the rest.\n";

/* Concatenate `argv[0..n-1]` with single spaces between elements. Returns
 * malloc'd. Used to assemble a positional-args prompt: `hax -p hello world`
 * → "hello world". */
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

/* Slurp every byte from stdin into a malloc'd buffer. Used when -p is
 * given without positional args and stdin is a pipe/file. */
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
    /* buf_steal returns NULL when nothing was appended; guarantee a
     * non-NULL string (possibly empty) so the caller can use it
     * uniformly. */
    if (!b.data)
        return xstrdup("");
    return buf_steal(&b);
}

/* Strip exactly one trailing newline (LF or CRLF) in place. Stdin from
 * `echo` / heredocs / editors arrives with a trailing newline that the
 * user almost certainly didn't mean as part of the prompt; a `printf`
 * pipe without a newline keeps every byte intact. Spaces and tabs are
 * left alone — they may carry intent (markdown hard break, indented
 * code, exact-match-this-string prompts). */
static void strip_trailing_newline(char *s)
{
    size_t n = strlen(s);
    if (n > 0 && s[n - 1] == '\n')
        s[--n] = '\0';
    if (n > 0 && s[n - 1] == '\r')
        s[--n] = '\0';
}

/* Resolve a --resume=ARG to a session file path. ARG is a session id —
 * exact match, or a unique prefix — among the sessions recorded for `cwd`.
 * Sessions are per-directory by design; there's no path/cross-directory
 * form (cd to the project to resume its work). Returns a malloc'd path, or
 * NULL when nothing matches or the prefix is ambiguous. Caller frees. */
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
    /* Must run before anything that emits or decodes multibyte text —
     * libedit in particular crashes on non-UTF-8 LC_CTYPE if our prompt
     * carries any UTF-8 byte. Touches LC_CTYPE only; LC_NUMERIC etc.
     * stay at the C locale so printf output remains predictable. */
    locale_init_utf8();

    struct hax_opts opts = {0};
    int print_mode = 0;
    int continue_mode = 0;
    int resume_mode = 0;
    const char *resume_arg = NULL;

    /* `-h`, `-p`, `-c` keep conventional short forms; --raw and --resume
     * are long-only (`-r` is avoided to leave room for a future short
     * alias without collision). --resume takes an optional ID, so
     * `--resume` alone opens the picker and `--resume=ID` is direct. */
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
        fprintf(stderr, "hax: use only one of --continue / --resume\n");
        return 1;
    }
    /* A bare --resume opens an interactive picker, which makes no sense in
     * -p mode (and would fight prompt-from-stdin). Require an explicit id
     * there; -c stays valid since it auto-selects the most recent. */
    if (print_mode && resume_mode && !resume_arg) {
        fprintf(stderr, "hax: -p with --resume requires a session id (e.g. --resume=ID)\n");
        return 1;
    }

    /* Reject positional args in interactive mode: they would otherwise
     * be silently ignored and the user would be left wondering why
     * `hax fix this bug` did nothing. */
    if (!print_mode && optind < argc) {
        fprintf(stderr, "hax: positional arguments require -p / --print\n"
                        "Try 'hax --help' for usage.\n");
        return 1;
    }

    /* Acquire and validate the -p prompt before initializing curl or
     * constructing a provider — the latter can be expensive (llama.cpp
     * probes the local server, codex reads OAuth state) and a typo'd
     * `hax -p` with no piped input shouldn't surface as a probe or auth
     * error before the real "needs a prompt" diagnostic. */
    char *prompt = NULL;
    if (print_mode) {
        if (optind < argc) {
            prompt = join_args(argc - optind, argv + optind);
        } else if (!isatty(fileno(stdin))) {
            prompt = read_all_stdin();
            if (!prompt) {
                fprintf(stderr, "hax: failed to read stdin\n");
                return 1;
            }
            strip_trailing_newline(prompt);
        } else {
            fprintf(stderr, "hax: -p requires a prompt (positional args or piped stdin)\n");
            return 1;
        }
        if (!*prompt) {
            fprintf(stderr, "hax: -p prompt is empty\n");
            free(prompt);
            return 1;
        }
    }

    /* Resolve --continue / --resume to a session file before any provider
     * work: --continue picks the newest session in this cwd, --resume[=ID]
     * either matches an id/path or opens the interactive picker. Done here
     * so a cancelled picker or a bad id exits cleanly without constructing
     * a provider. */
    char *resume_path = NULL;
    if (continue_mode || resume_mode) {
        char cwd[4096];
        if (!getcwd(cwd, sizeof(cwd))) {
            fprintf(stderr, "hax: getcwd failed\n");
            free(prompt);
            return 1;
        }
        if (continue_mode) {
            struct session_entry *list;
            size_t n;
            session_list(cwd, &list, &n);
            if (n > 0)
                resume_path = xstrdup(list[0].path); /* newest by mtime */
            session_list_free(list, n);
            if (!resume_path) {
                /* Nothing to continue. In -p that's an error: the prompt
                 * would otherwise run against empty history with only an
                 * easy-to-miss stderr note, contradicting the explicit
                 * request. Interactively, just start a fresh REPL. */
                if (print_mode) {
                    fprintf(stderr, "hax: no past conversation in this directory to continue\n");
                    free(prompt);
                    return 1;
                }
                fprintf(stderr, "hax: no past conversation in this directory; starting fresh\n");
            }
        } else if (resume_arg) {
            if (!*resume_arg) {
                /* `--resume=` (empty, e.g. an unset shell var) would prefix-
                 * match every session — refuse it rather than resume an
                 * arbitrary one. (Bare `--resume` with no '=' is the picker.) */
                fprintf(stderr, "hax: --resume= requires a session id\n");
                free(prompt);
                return 1;
            }
            resume_path = resolve_resume_arg(cwd, resume_arg);
            if (!resume_path) {
                if (strchr(resume_arg, '/'))
                    fprintf(stderr, "hax: --resume takes a session id, not a path "
                                    "(sessions are per-directory)\n");
                else
                    fprintf(stderr, "hax: no session matching '%s'\n", resume_arg);
                free(prompt);
                return 1;
            }
        } else {
            resume_path = session_picker_run(cwd, NULL);
            if (!resume_path) {
                /* Cancelled, or nothing to resume / no TTY. */
                free(prompt);
                return 0;
            }
        }
    }
    opts.resume_path = resume_path;

    int rc = 1;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        fprintf(stderr, "hax: curl_global_init failed\n");
        goto err_prompt;
    }

    /* Truncate HAX_TRACE and HAX_TRANSCRIPT here, before any provider
     * startup or session init. Without this, a fast-fail run (bad
     * config, missing HAX_MODEL, no OAuth) would exit with stale files
     * still on disk despite the documented truncate-on-startup
     * behavior. trace_init forces the lazy fopen in trace.c; the
     * transcript file is just truncated — its header gets written
     * later when sys+tools are known. */
    trace_init();
    transcript_log_init();

    struct provider *p = pick_provider();
    if (!p)
        goto err_curl;

    rc = print_mode ? oneshot_run(p, prompt, &opts, ONESHOT_MAX_TURNS) : agent_run(p, &opts);

    /* Each provider's destroy() is responsible for joining whatever
     * background work it spawned (probes, prefetches, ...) before
     * releasing the state those workers may still be writing to.
     * curl_global_cleanup() runs after destroy(), so any libcurl
     * handles in flight have necessarily been wound down by then. */
    p->destroy(p);
err_curl:
    curl_global_cleanup();
err_prompt:
    free(prompt);
    free(resume_path);
    return rc;
}
