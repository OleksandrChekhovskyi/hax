/* SPDX-License-Identifier: MIT */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>

#include "agent.h"
#include "catalog.h"
#include "config.h"
#include "oneshot.h"
#include "providers/registry.h"
#include "select.h"
#include "session.h"
#include "session_picker.h"
#include "terminal/ansi.h"
#include "terminal/interrupt.h"
#include "tools/bash_export.h"
#include "trace.h"
#include "transcript.h"
#include "util.h"

/* Hard cap on agentic round-trips in -p mode. Generous — a real task
 * rarely needs more than a couple of dozen — but bounded so a confused
 * model can't spin forever in a pipeline where Ctrl-C may not reach us
 * cleanly. No CLI knob for this yet; add one if a real use case shows up. */
#define ONESHOT_MAX_TURNS 100

/* The subagent depth cap lives in tools/bash_export.h (the guard here and
 * the stamp in the bash tool must agree). Deliberately a plain getenv, not
 * a config key: it's a process fact set by the parent, not a user
 * tunable. */

static struct provider *pick_provider(int print_mode, int *provider_autoselected)
{
    const char *which = config_str("provider");
    /* "Explicit" = the user named a provider somewhere (HAX_PROVIDER, the
     * config file's "provider" key, or a prior /provider pick in state.json). */
    if (which && *which) {
        const struct provider_factory *f = provider_find(which);
        if (!f) {
            /* Unknown name — a typo in HAX_PROVIDER / the config / a stale
             * state.json pick. Name the value, not the source, with the
             * supported set as the help. */
            fprintf(stderr, "hax: unknown provider '%s' (supported: ", which);
            provider_list_names(stderr);
            fprintf(stderr, ")\n");
            return NULL;
        }
        /* Strict construction: the user asked for this backend, so "codex
         * not logged in" / "server down" is the answer they need. Fatal in
         * one-shot mode; interactively the REPL opens provider-less. */
        return f->new(f->name);
    }

    /* Cold start with nothing configured: one path picks something in both
     * modes. The auto-selector tries the built-in default first (cheap to
     * check, so the common logged-in start stays instant and silent), and on
     * its failure probes the rest and starts on the first available one. */
    struct provider *p = provider_autoselect();
    if (p) {
        /* Inferred, not configured — the one-shot banner marks it, default
         * included: nothing pins the pick, so it can change with the
         * environment (interactively /provider shows the why instead). */
        *provider_autoselected = 1;
    } else if (print_mode) {
        /* The availability probes are silent, so without this a one-shot
         * run would exit with no diagnostic at all. Interactively the REPL
         * opens provider-less with the banner pointing at /provider. */
        hax_err("no provider available (set HAX_PROVIDER or configure one)");
    }
    return p;
}

/* Flag column + description lines per option; descriptions embed '\n' for
 * continuation lines, indented by the printer to the shared column. Kept as
 * data so the TTY and piped renderings come from one source. */
static const struct help_opt {
    const char *flags;
    const char *desc;
} HELP_OPTS[] = {
    {"-p, --print", "Non-interactive mode. Runs the prompt to completion and prints the final\n"
                    "assistant message to stdout. The prompt comes from PROMPT positional\n"
                    "arguments (joined with spaces) when given, otherwise from stdin if stdin\n"
                    "is not a terminal."},
    {"-c, --continue", "Resume the most recent conversation in this directory."},
    {"--resume[=ID]", "Resume a past conversation in this directory. With no ID, pick one from\n"
                      "an interactive list; with a session ID, resume it directly — the ID form\n"
                      "also works with -p."},
    {"--no-session", "Don't record this conversation (nothing to resume)."},
    {"--raw", "Send only the prompt text — no system prompt, no environment block,\n"
              "no AGENTS.md, no skills, and no tools. Useful as a barebones chat\n"
              "interface."},
    {"--bare", "Run without the environment-derived context — env block, AGENTS.md,\n"
               "skills, subagents section. Tools and the base system prompt remain\n"
               "(unlike --raw)."},
    {"--provider=NAME", "Select the backend for this run."},
    {"--model=ID", "Select the model for this run."},
    {"--effort=LEVEL", "Select the reasoning effort for this run."},
    {"--preset=NAME", "Apply the named preset — a presets.NAME selection from the config\n"
                      "file. Explicit selection flags win over the preset's values."},
    {"-h, --help", "Show this help and exit."},
};

static void print_help(void)
{
    /* Cyan flags + bold headers on a terminal, matching the REPL's /help;
     * piped output (hax -h | less, docs generation) stays plain. */
    int tty = isatty(fileno(stdout));
    const char *cyan = tty ? ANSI_CYAN : "";
    const char *bold = tty ? ANSI_BOLD : "";
    const char *reset = tty ? ANSI_RESET : "";

    printf("%susage:%s hax [OPTIONS] [PROMPT...]\n\n", bold, reset);
    printf("A minimalist coding assistant in your terminal.\n\n"
           "With no arguments, runs an interactive REPL.\n\n");
    printf("%soptions%s\n", bold, reset);

    size_t col = 0;
    for (size_t i = 0; i < sizeof(HELP_OPTS) / sizeof(*HELP_OPTS); i++) {
        size_t w = strlen(HELP_OPTS[i].flags);
        if (w > col)
            col = w;
    }
    for (size_t i = 0; i < sizeof(HELP_OPTS) / sizeof(*HELP_OPTS); i++) {
        printf("  %s%s%s%*s", cyan, HELP_OPTS[i].flags, reset,
               (int)(col - strlen(HELP_OPTS[i].flags) + 2), "");
        for (const char *p = HELP_OPTS[i].desc; *p;) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            if (p != HELP_OPTS[i].desc)
                printf("%*s", (int)(col + 4), "");
            printf("%.*s\n", (int)len, p);
            p += len + (nl != NULL);
        }
    }

    printf("\nThe selection flags (--provider, --model, --effort, --preset) apply to this run\n"
           "only and take priority over every other source. Persistent configuration is via\n"
           "environment variables (HAX_PROVIDER, HAX_MODEL, and the rest), saved runtime picks,\n"
           "then ~/.config/hax/config.json — in that order. See README.md.\n");
}

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
     * the line editor and renderers size glyphs with wcwidth(), which
     * mis-measures under a non-UTF-8 LC_CTYPE and breaks cursor
     * positioning. Touches LC_CTYPE only; LC_NUMERIC etc. stay at the C
     * locale so printf output remains predictable. */
    locale_init_utf8();

    struct hax_opts opts = {0};
    int print_mode = 0;
    int continue_mode = 0;
    int resume_mode = 0;
    const char *resume_arg = NULL;

    /* Declared up front and initialized to safe values so every error
     * path can funnel through the err_prompt/err_curl unwind below with a
     * plain `goto` — no per-site free() boilerplate, and no goto crossing
     * an initializer it would later read. */
    int rc = 1;
    char *prompt = NULL;
    char *resume_path = NULL;

    /* Selection overrides gathered during parsing and applied after
     * config_init() below (the preset needs the config file loaded).
     * Values are borrowed argv pointers — valid for the whole run. */
    const char *opt_provider = NULL;
    const char *opt_model = NULL;
    const char *opt_effort = NULL;
    const char *opt_preset = NULL;
    int opt_no_session = 0;
    int opt_bare = 0;

    /* `-h`, `-p`, `-c` keep conventional short forms; everything else is
     * long-only (`-r` is avoided to leave room for a future short
     * alias without collision). --resume takes an optional ID, so
     * `--resume` alone opens the picker and `--resume=ID` is direct. */
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
    static const struct option long_opts[] = {
        {"help", no_argument, NULL, 'h'},
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

    int c;
    while ((c = getopt_long(argc, argv, "hpc", long_opts, NULL)) != -1) {
        switch (c) {
        case 'h':
            print_help();
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
        case OPT_PROVIDER:
            opt_provider = optarg;
            break;
        case OPT_MODEL:
            opt_model = optarg;
            break;
        case OPT_EFFORT:
            opt_effort = optarg;
            break;
        case OPT_PRESET:
            opt_preset = optarg;
            break;
        case OPT_BARE:
            opt_bare = 1;
            break;
        case OPT_NO_SESSION:
            opt_no_session = 1;
            break;
        case '?':
            /* getopt_long already printed the diagnostic. */
            fprintf(stderr, "Try 'hax --help' for usage.\n");
            return 1;
        default:
            return 1;
        }
    }

    /* Refuse to nest past the subagent depth cap. After parsing so --help
     * still works anywhere, before any real work so a runaway chain dies
     * fast with a diagnostic the spawning model can read and act on.
     * Checked parse: this is the recursion backstop, so a malformed,
     * negative, or overflowing value reads as at-cap (refuse) rather than
     * resetting the chain — atoi would wrap or yield 0. */
    const char *depth_env = getenv("HAX_SUBAGENT_DEPTH");
    if (depth_env && *depth_env) {
        int depth = 0;
        if (!parse_int(depth_env, &depth) || depth < 0)
            depth = HAX_SUBAGENT_MAX_DEPTH;
        if (depth >= HAX_SUBAGENT_MAX_DEPTH) {
            hax_err("subagent depth limit (%d) reached — run the task directly instead of "
                    "spawning another hax",
                    HAX_SUBAGENT_MAX_DEPTH);
            return 1;
        }
    }

    /* Empty selection-flag values (e.g. an unset shell var in
     * --provider="$P") — reject like `--resume=`: the flags exist to name
     * an explicit value, and falling through would silently do something
     * else entirely (--provider= auto-selects another backend, --model=
     * takes the provider default, --effort= disables effort, --preset=
     * leaves a lower-tier stance visible-but-unapplied). The empty-means-
     * disable spelling belongs to the env vars, where it's documented. */
    const char *empty_flag = NULL;
    if (opt_provider && !*opt_provider)
        empty_flag = "--provider=";
    else if (opt_model && !*opt_model)
        empty_flag = "--model=";
    else if (opt_effort && !*opt_effort)
        empty_flag = "--effort=";
    else if (opt_preset && !*opt_preset)
        empty_flag = "--preset=";
    if (empty_flag) {
        hax_err("%s requires a value", empty_flag);
        goto err_prompt;
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

    /* Acquire and validate the -p prompt before initializing curl or
     * constructing a provider — the latter can be expensive (llama.cpp
     * probes the local server, codex reads OAuth state) and a typo'd
     * `hax -p` with no piped input shouldn't surface as a probe or auth
     * error before the real "needs a prompt" diagnostic. */
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

    /* Resolve --continue / --resume to a session file before any provider
     * work: --continue picks the newest session in this cwd, --resume[=ID]
     * either matches an id/path or opens the interactive picker. Done here
     * so a cancelled picker or a bad id exits cleanly without constructing
     * a provider. */
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
                /* Nothing to continue. In -p that's an error: the prompt
                 * would otherwise run against empty history with only an
                 * easy-to-miss stderr note, contradicting the explicit
                 * request. Interactively, just start a fresh REPL. */
                if (print_mode) {
                    hax_err("no past conversation in this directory to continue");
                    goto err_prompt;
                }
                hax_warn("no past conversation in this directory; starting fresh");
            }
        } else if (resume_arg) {
            if (!*resume_arg) {
                /* `--resume=` (empty, e.g. an unset shell var) would prefix-
                 * match every session — refuse it rather than resume an
                 * arbitrary one. (Bare `--resume` with no '=' is the picker.) */
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
            /* The bare-`--resume` picker switches the terminal to raw mode
             * here, during option resolution — before agent_run() installs
             * the terminal-restore atexit/signal handlers. Install them now
             * so a SIGTERM/SIGHUP/SIGQUIT (or normal exit) while the picker
             * is up still restores cooked mode and re-shows the cursor,
             * rather than leaving the parent shell raw. Idempotent and
             * TTY-gated: agent_run()'s later call is then a no-op. */
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

    /* Apply the preset (if any), then the explicit selection flags on top.
     * Both land in the session-override tier, so the order alone gives the
     * precedence: flag > preset > env > saved state > config file. The
     * preset resolves through config too (--preset flag, else HAX_PRESET /
     * a "preset" key), read verbatim so an explicit empty disables a
     * configured default. Applied before trace_init so a preset can carry
     * trace/transcript paths like any other setting. */
    /* A preset named explicitly for this run (--preset, HAX_PRESET) always
     * applies — that's the documented flag > preset > env order. A preset
     * arriving by resolution instead (a /preset stance persisted in
     * state.json, or a config-file default) yields to ANY explicit per-run
     * selection: its values would land in the override tier and beat the
     * env vars, silently breaking the "a one-off HAX_FOO=bar hax always
     * wins" promise for persisted state. Explicit input suppresses the
     * whole stance, not per-key — presets apply whole or not at all — and
     * does so silently, like every other case of explicit input shadowing
     * persisted state; the banner shows what actually runs. */
    int explicit_sel = opt_provider || opt_model || opt_effort || getenv("HAX_PROVIDER") ||
                       getenv("HAX_MODEL") || getenv("HAX_EFFORT") || getenv("HAX_SYSTEM_PROMPT");

    const char *preset = opt_preset ? opt_preset : getenv("HAX_PRESET");
    int preset_explicit = preset != NULL;
    if (!preset) {
        preset = config_str("preset"); /* state.json stance / config default */
        if (preset && *preset && explicit_sel) {
            config_set_override("preset", ""); /* keep the banner stance-free */
            preset = NULL;
        }
    }
    if (preset && *preset) {
        char *err = NULL;
        if (config_preset_apply(preset, &err) != 0) {
            /* An explicitly named preset (--preset or HAX_PRESET) is a hard
             * error — a subagent or scripted invocation must not silently
             * run on the wrong setup. A name that came from resolution
             * instead (a /preset persisted to state.json before its
             * definition was renamed, a stale config default) must not
             * brick every launch: warn and continue without it, shadowing
             * the name for this run so the banner doesn't claim a stance
             * that isn't applied. */
            if (preset_explicit) {
                hax_err("%s", err ? err : "preset failed to apply");
                free(err);
                goto err_curl;
            }
            hax_warn("%s", err ? err : "preset failed to apply");
            free(err);
            config_set_override("preset", "");
        }
    }
    if (opt_provider)
        config_set_override("provider", opt_provider);
    if (opt_model)
        config_set_override("model", opt_model);
    if (opt_effort)
        config_set_override("effort", opt_effort);
    /* --bare = every environment-derived context section stripped in one
     * flag — the common shape for scripted / subagent scout runs, and one
     * place to grow when new context sections appear. Deliberately a flag,
     * not a preset: none of these keys are presettable (they're
     * startup-latched), and the definition can widen without touching the
     * preset contract. Recording is a separate axis (--no-session), NOT
     * bundled: a bare run stays resumable — the recovery path for a
     * subagent killed by a tool timeout — unless disposability is asked
     * for explicitly. */
    if (opt_bare) {
        config_set_override("no_env", "1");
        config_set_override("no_agents_md", "1");
        config_set_override("no_skills", "1");
        config_set_override("no_subagents", "1");
    }
    if (opt_no_session)
        config_set_override("no_session", "1");

    /* Truncate HAX_TRACE and HAX_TRANSCRIPT here, before any provider
     * startup or session init. Without this, a fast-fail run (bad
     * config, missing HAX_MODEL, no OAuth) would exit with stale files
     * still on disk despite the documented truncate-on-startup
     * behavior. trace_init forces the lazy fopen in trace.c; the
     * transcript file is just truncated — its header gets written
     * later when sys+tools are known. */
    trace_init();
    transcript_log_init();

    struct provider *p = pick_provider(print_mode, &opts.provider_autoselected);
    /* No usable provider (explicit one can't construct, or nothing available
     * to auto-select) is fatal only in one-shot mode, which can't prompt for
     * an alternative. Interactively we start the REPL with no provider:
     * pick_provider already printed why, the banner points at /provider, and
     * agent_run streams nothing until a working one is chosen. */
    if (!p && print_mode)
        goto err_curl;

    /* agent_run may swap the provider at runtime (/provider), updating `p`
     * to whatever is live at exit (possibly from NULL to a real one); the
     * one-shot path never does. Either way `p` below is the provider to tear
     * down — NULL if the user exited without ever choosing one. */
    rc = print_mode ? oneshot_run(p, prompt, &opts, ONESHOT_MAX_TURNS) : agent_run(&p, &opts);

    /* Each provider's destroy() is responsible for joining whatever
     * background work it spawned (probes, prefetches, ...) before
     * releasing the state those workers may still be writing to.
     * curl_global_cleanup() runs after destroy(), so any libcurl
     * handles in flight have necessarily been wound down by then. */
    if (p)
        p->destroy(p);
err_curl:
    /* Like provider destroy: join the catalog's background fetch (it may
     * hold a libcurl handle) before curl_global_cleanup below. */
    catalog_shutdown();
    curl_global_cleanup();
err_prompt:
    config_free();
    free(prompt);
    free(resume_path);
    return rc;
}
