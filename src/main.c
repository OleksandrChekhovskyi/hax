/* SPDX-License-Identifier: MIT */
#include <curl/curl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent.h"
#include "oneshot.h"
#include "providers/codex.h"
#include "providers/llamacpp.h"
#include "providers/openai.h"
#include "providers/openai_compat.h"
#include "providers/openrouter.h"
#include "util.h"

/* Hard cap on agentic round-trips in -p mode. Generous — a real task
 * rarely needs more than a couple of dozen — but bounded so a confused
 * model can't spin forever in a pipeline where Ctrl-C may not reach us
 * cleanly. No CLI knob for this yet; add one if a real use case shows up. */
#define ONESHOT_MAX_TURNS 100

/* Registry of available providers. First entry is the default when
 * HAX_PROVIDER is unset. Adding a new preset = drop a file under
 * src/providers/ and append its PROVIDER_* symbol here. */
static const struct provider_factory *const PROVIDERS[] = {
    &PROVIDER_CODEX,    &PROVIDER_OPENAI,     &PROVIDER_OPENAI_COMPAT,
    &PROVIDER_LLAMACPP, &PROVIDER_OPENROUTER,
};
#define N_PROVIDERS (sizeof(PROVIDERS) / sizeof(PROVIDERS[0]))

static struct provider *pick_provider(void)
{
    const char *which = getenv("HAX_PROVIDER");
    if (!which || !*which)
        return PROVIDERS[0]->new();

    for (size_t i = 0; i < N_PROVIDERS; i++) {
        if (strcmp(which, PROVIDERS[i]->name) == 0)
            return PROVIDERS[i]->new();
    }

    fprintf(stderr, "hax: unknown HAX_PROVIDER='%s' (supported:", which);
    for (size_t i = 0; i < N_PROVIDERS; i++)
        fprintf(stderr, " %s", PROVIDERS[i]->name);
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

int main(int argc, char **argv)
{
    /* Must run before anything that emits or decodes multibyte text —
     * libedit in particular crashes on non-UTF-8 LC_CTYPE if our prompt
     * carries any UTF-8 byte. Touches LC_CTYPE only; LC_NUMERIC etc.
     * stay at the C locale so printf output remains predictable. */
    locale_init_utf8();

    struct hax_opts opts = {0};
    int print_mode = 0;

    /* `-h` and `-p` keep their conventional short forms; --raw is
     * long-only since `-r` is already overloaded by other agents
     * (Claude/pi-mono use it for --resume) and we may want to mirror
     * that later without a collision. */
    enum {
        OPT_RAW = 0x100,
    };
    static const struct option long_opts[] = {
        {"help", no_argument, NULL, 'h'},
        {"print", no_argument, NULL, 'p'},
        {"raw", no_argument, NULL, OPT_RAW},
        {NULL, 0, NULL, 0},
    };

    int c;
    while ((c = getopt_long(argc, argv, "hp", long_opts, NULL)) != -1) {
        switch (c) {
        case 'h':
            fputs(HELP_TEXT, stdout);
            return 0;
        case 'p':
            print_mode = 1;
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

    int rc = 1;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        fprintf(stderr, "hax: curl_global_init failed\n");
        goto err_prompt;
    }

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
    return rc;
}
