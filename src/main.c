/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>

#include "agent.h"
#include "catalog.h"
#include "cli.h"
#include "config.h"
#include "oneshot.h"
#include "provider.h"
#include "select.h"
#include "session.h"
#include "session_prune.h"
#include "trace.h"
#include "transcript.h"
#include "util.h"
#include "providers/registry.h"
#include "terminal/theme.h"
#include "transport/ca.h"

/* Bounds unattended agent loops when an interrupt cannot reliably reach a pipeline. */
#define ONESHOT_MAX_TURNS 100

static struct provider *select_initial_provider(int one_shot, int *autoselected)
{
    const char *name = config_str("provider");
    if (name && *name) {
        int restored = strcmp(config_source("provider"), "conversation") == 0;
        const struct provider_factory *factory = provider_find(name);
        struct provider *provider = NULL;

        if (!factory) {
            fprintf(stderr, "hax: unknown provider '%s' (supported: ", name);
            provider_list_names(stderr);
            fprintf(stderr, ")\n");
        } else {
            provider = factory->new(factory->id);
        }
        if (!provider && restored)
            hax_warn("'%s' is what this session was using — pass --provider/--model to "
                     "continue it elsewhere",
                     name);
        return provider;
    }

    struct provider *provider = provider_autoselect();
    if (provider)
        *autoselected = 1;
    else if (one_shot)
        hax_err("no provider available (set HAX_PROVIDER or configure one)");
    return provider;
}

static int restore_resumed_selection(const char *path, int one_shot, int restore_preset)
{
    struct session_meta metadata;
    if (session_read_meta(path, &metadata) != 0) {
        session_meta_free(&metadata);
        return 0;
    }

    char *error = NULL;
    int result = 0;
    if (config_restore_selection(CONFIG_TIER_CONVERSATION, metadata.provider, metadata.model,
                                 metadata.effort, restore_preset ? metadata.preset : NULL,
                                 &error) != 0) {
        const char *message = error ? error : "preset failed to apply";
        if (one_shot) {
            hax_err("%s (recorded by this session; pass --preset or --model to run it differently)",
                    message);
            result = -1;
        } else {
            hax_warn("%s — resuming without it", message);
        }
    }

    free(error);
    session_meta_free(&metadata);
    return result;
}

static int run_replaces_resumed_preset(const struct cli_options *options)
{
    const struct cli_selection *selection = &options->selection;
    const char *environment_preset = getenv("HAX_PRESET");

    /* Bash exports an empty HAX_PRESET to prevent inherited stances; it names no replacement. */
    return selection->provider || selection->model || selection->effort || selection->preset ||
           (environment_preset && *environment_preset);
}

static int has_per_setting_selection(const struct cli_options *options)
{
    const struct cli_selection *selection = &options->selection;

    return selection->provider || selection->model || selection->effort || getenv("HAX_PROVIDER") ||
           getenv("HAX_MODEL") || getenv("HAX_EFFORT") || getenv("HAX_SYSTEM_PROMPT");
}

static int apply_run_selection(const struct cli_options *options)
{
    const struct cli_selection *selection = &options->selection;
    const char *preset = selection->preset ? selection->preset : getenv("HAX_PRESET");
    int preset_named_for_run = preset != NULL;

    if (!preset && strcmp(config_source("preset"), "conversation") != 0) {
        preset = config_str("preset");
        if (preset && *preset && has_per_setting_selection(options)) {
            /* Presets are atomic, so any per-setting run selection suppresses persisted stances. */
            config_set_override("preset", "");
            preset = NULL;
        }
    }

    if (preset && *preset) {
        char *error = NULL;
        if (config_preset_apply(preset, CONFIG_TIER_RUN, &error) != 0) {
            const char *message = error ? error : "preset failed to apply";
            if (preset_named_for_run) {
                hax_err("%s", message);
                free(error);
                return -1;
            }
            hax_warn("%s — starting without it", message);
            free(error);
            config_set_override("preset", "");
        }
    }

    if (selection->provider)
        config_set_override("provider", selection->provider);
    if (selection->model)
        config_set_override("model", selection->model);
    if (selection->effort)
        config_set_override("effort", selection->effort);
    if (options->bare) {
        config_set_override("no_agents_md", "1");
        config_set_override("no_skills", "1");
        config_set_override("no_subagents", "1");
    }
    if (options->no_session)
        config_set_override("no_session", "1");

    return 0;
}

static void initialize_config(void)
{
    locale_init_utf8();

    /* Config-load diagnostics can honor terminal color, but not a theme in an unreadable file. */
    theme_set("auto");
    config_init();
    theme_init();
}

static void initialize_run_services(const char *resume_path)
{
    trace_init();
    transcript_log_init();
    session_prune_start(resume_path);
}

int main(int argc, char **argv)
{
    initialize_config();

    int result = 1;
    char *prompt = NULL;
    char *resume_path = NULL;
    struct provider *provider = NULL;
    struct cli_options options;

    enum cli_parse_result parse_result = cli_parse(argc, argv, &options);
    if (parse_result == CLI_PARSE_EXIT) {
        result = 0;
        goto cleanup_config;
    }
    if (parse_result == CLI_PARSE_ERROR || cli_check_subagent_depth() != 0)
        goto cleanup_config;
    if (cli_read_prompt(&options, argc, argv, stdin, isatty(fileno(stdin)), &prompt) != 0)
        goto cleanup_config;

    enum cli_session_result session_result = cli_resolve_session(&options, &resume_path);
    if (session_result == CLI_SESSION_EXIT) {
        result = 0;
        goto cleanup_config;
    }
    if (session_result == CLI_SESSION_ERROR)
        goto cleanup_config;

    options.agent_options.resume_path = resume_path;
    int restore_preset = !run_replaces_resumed_preset(&options);
    if (resume_path &&
        restore_resumed_selection(resume_path, options.one_shot, restore_preset) != 0)
        goto cleanup_config;

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        hax_err("curl_global_init failed");
        goto cleanup_config;
    }
    ca_init();

    if (apply_run_selection(&options) != 0)
        goto cleanup_curl;

    /* A restored or newly applied preset may change the theme. */
    theme_init();
    initialize_run_services(resume_path);

    unsigned long diagnostics_before_provider = hax_diag_sequence();
    provider =
        select_initial_provider(options.one_shot, &options.agent_options.provider_autoselected);
    if (options.one_shot && provider && hax_diag_sequence() != diagnostics_before_provider) {
        fputc('\n', stderr);
        fflush(stderr);
    }
    if (!provider && options.one_shot)
        goto cleanup_provider;

    result = options.one_shot
                 ? oneshot_run(provider, prompt, &options.agent_options, ONESHOT_MAX_TURNS)
                 : agent_run(&provider, &options.agent_options);

cleanup_provider:
    /* Providers must join background work before global libcurl teardown. */
    if (provider)
        provider->destroy(provider);
cleanup_curl:
    session_prune_shutdown();
    catalog_shutdown();
    curl_global_cleanup();
cleanup_config:
    config_free();
    free(prompt);
    free(resume_path);
    return result;
}
