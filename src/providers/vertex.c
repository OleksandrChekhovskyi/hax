/* SPDX-License-Identifier: MIT */
#include "providers/vertex.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "diag.h"
#include "provider.h"
#include "xalloc.h"
#include "providers/vertex_auth.h"

/* The default region mirrors Claude Code's CLOUD_ML_REGION default; users override it per
 * deployment. Host and path both consume resolve_settings() so their fallbacks cannot diverge. */

static const char *env_nonempty(const char *name)
{
    const char *value = getenv(name);
    return value && *value ? value : NULL;
}

struct vertex_settings {
    const char *project;
    const char *location;
};

static const char *resolve_setting(const char *key, const char *primary_env,
                                   const char *alternate_env, const char *fallback)
{
    const char *value = config_str_nonempty(key);
    if (value)
        return value;
    value = env_nonempty(primary_env);
    if (value)
        return value;
    value = alternate_env ? env_nonempty(alternate_env) : NULL;
    return value ? value : fallback;
}

static struct vertex_settings resolve_settings(void)
{
    return (struct vertex_settings){
        .project = resolve_setting("providers.vertex.project", "GOOGLE_CLOUD_PROJECT",
                                   "ANTHROPIC_VERTEX_PROJECT_ID", NULL),
        .location = resolve_setting("providers.vertex.location", "GOOGLE_CLOUD_LOCATION",
                                    "CLOUD_ML_REGION", "us-east5"),
    };
}

static int project_valid(const char *project)
{
    size_t len = strlen(project);
    if (len == 0 || len > 128 || !isalnum((unsigned char)project[0]) ||
        !isalnum((unsigned char)project[len - 1]))
        return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)project[i];
        if (!islower(c) && !isdigit(c) && c != '-' && c != '.' && c != ':')
            return 0;
    }
    return 1;
}

static int location_valid(const char *location)
{
    size_t len = strlen(location);
    if (len == 0 || len > 63 || !isalnum((unsigned char)location[0]) ||
        !isalnum((unsigned char)location[len - 1]))
        return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)location[i];
        if (!islower(c) && !isdigit(c) && c != '-')
            return 0;
    }
    return 1;
}

static int validate_settings(const struct vertex_settings *settings)
{
    if (!settings->project) {
        hax_err("provider 'vertex': project is not set (configure providers.vertex.project / "
                "HAX_VERTEX_PROJECT, GOOGLE_CLOUD_PROJECT, or ANTHROPIC_VERTEX_PROJECT_ID)");
        return -1;
    }
    if (!project_valid(settings->project)) {
        hax_err("provider 'vertex': invalid project (use a Google Cloud project ID or number)");
        return -1;
    }
    if (!location_valid(settings->location)) {
        hax_err("provider 'vertex': invalid location (use lowercase letters, digits, and hyphens)");
        return -1;
    }
    return 0;
}

char *vertex_resolve_base_url(const struct provider_def *def)
{
    (void)def;
    struct vertex_settings settings = resolve_settings();
    if (validate_settings(&settings) != 0)
        return NULL;

    char *host;
    if (strcmp(settings.location, "global") == 0) {
        host = xstrdup("aiplatform.googleapis.com");
    } else if (strcmp(settings.location, "us") == 0 || strcmp(settings.location, "eu") == 0) {
        /* Multi-region endpoints are served from the region's replica host. */
        host = xasprintf("aiplatform.%s.rep.googleapis.com", settings.location);
    } else {
        host = xasprintf("%s-aiplatform.googleapis.com", settings.location);
    }
    char *url = xasprintf("https://%s", host);
    free(host);
    return url;
}

char *vertex_resolve_path(const struct provider_def *def)
{
    (void)def;
    struct vertex_settings settings = resolve_settings();
    if (validate_settings(&settings) != 0)
        return NULL;
    return xasprintf("/v1/projects/%s/locations/%s/publishers/anthropic/models/"
                     "{model}:streamRawPredict",
                     settings.project, settings.location);
}

void vertex_prepare_availability(const struct provider_def *def, struct provider_availability *out)
{
    (void)def;
    memset(out, 0, sizeof(*out));
    if (!resolve_settings().project) {
        out->available = 0;
        out->reason = xstrdup("project not set");
        return;
    }
    char *reason = NULL;
    if (vertex_auth_local_status(&reason)) {
        out->available = 1;
    } else {
        out->available = 0;
        out->reason = reason ? reason : xstrdup("no Google credentials");
    }
}