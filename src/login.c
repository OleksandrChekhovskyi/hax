/* SPDX-License-Identifier: MIT */
#include "login.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent.h"
#include "provider.h"
#include "xalloc.h"
#include "providers/codex.h"
#include "providers/codex_login.h"
#include "providers/registry.h"
#include "render/disp.h"
#include "render/render_ctx.h"
#include "terminal/picker.h"
#include "terminal/ui.h"

/* A provider account hax can log in to and out of itself. Flows print their own outcome; `status`
 * returns an owned picker description or NULL when there is nothing to describe. `adopt` hands
 * freshly stored credentials to a live provider of this identity. */
struct login_method {
    const char *provider_id;
    const char *detail; /* account and flow, e.g. "ChatGPT account (device login)" */
    int (*login)(void);
    int (*logout)(void);
    char *(*status)(void);
    int (*logged_in)(void);
    void (*adopt)(struct provider *provider);
};

static const struct login_method METHODS[] = {
    {
        .provider_id = "codex",
        .detail = "ChatGPT account (browser or device login)",
        .login = codex_login_run,
        .logout = codex_logout_run,
        .status = codex_login_status,
        .logged_in = codex_login_present,
        .adopt = codex_provider_reload_auth,
    },
};
#define N_METHODS (sizeof(METHODS) / sizeof(METHODS[0]))

static const struct login_method *find_method(const char *provider_id)
{
    const char *canonical = provider_canonical_id(provider_id);
    for (size_t i = 0; i < N_METHODS; i++) {
        if (strcmp(METHODS[i].provider_id, canonical) == 0)
            return &METHODS[i];
    }
    return NULL;
}

static const char *method_label(const struct login_method *method)
{
    const struct provider_def *def = provider_find(method->provider_id);
    return def ? provider_display_name(def) : method->provider_id;
}

/* Pick among `methods`. Returns NULL on cancellation or non-tty. */
static const struct login_method *
choose_method(const char *title, const struct login_method *const *methods, size_t method_count)
{
    struct picker_item *items = xcalloc(method_count, sizeof(*items));
    char **descriptions = xcalloc(method_count, sizeof(*descriptions));
    for (size_t i = 0; i < method_count; i++) {
        items[i].label = method_label(methods[i]);
        items[i].detail = methods[i]->detail;
        descriptions[i] = methods[i]->status();
        items[i].description = descriptions[i] ? descriptions[i] : "not logged in";
    }

    struct picker_opts options = {
        .title = title,
        .items = items,
        .item_count = method_count,
    };
    long selected_index = picker_run(&options);

    for (size_t i = 0; i < method_count; i++)
        free(descriptions[i]);
    free(descriptions);
    free(items);
    return selected_index >= 0 ? methods[selected_index] : NULL;
}

static void note_needs_argument(struct agent_state *state, const char *verb)
{
    if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO))
        return; /* the picker ran and was cancelled; nothing was printed */
    ui_note("/%s needs a provider name when not interactive", verb);
    disp_sync_external_line(&state->render->disp);
}

static const struct login_method *resolve_argument(struct agent_state *state, const char *argument)
{
    const struct login_method *method = find_method(argument);
    if (!method) {
        ui_error("no login flow for '%s' — hax manages logins for: codex", argument);
        disp_sync_external_line(&state->render->disp);
    }
    return method;
}

void login_command(struct agent_state *state, const char *argument)
{
    const struct login_method *method;
    if (argument && *argument) {
        method = resolve_argument(state, argument);
        if (!method)
            return;
    } else {
        const struct login_method *all[N_METHODS];
        for (size_t i = 0; i < N_METHODS; i++)
            all[i] = &METHODS[i];
        method = choose_method("log in to", all, N_METHODS);
        if (!method) {
            note_needs_argument(state, "login");
            return;
        }
    }

    int result = method->login();
    if (result == 0) {
        int live = state->provider &&
                   strcmp(provider_stable_id(state->provider), method->provider_id) == 0;
        if (live)
            method->adopt(state->provider);
        else
            ui_note("run /provider to switch to %s", method_label(method));
    }
    /* Cancellation inside the flow already printed its interruption marker. */
    disp_sync_external_line(&state->render->disp);
}

void logout_command(struct agent_state *state, const char *argument)
{
    const struct login_method *method = NULL;
    if (argument && *argument) {
        method = resolve_argument(state, argument);
        if (!method)
            return;
    } else {
        const struct login_method *logged_in[N_METHODS];
        size_t logged_in_count = 0;
        for (size_t i = 0; i < N_METHODS; i++)
            if (METHODS[i].logged_in())
                logged_in[logged_in_count++] = &METHODS[i];
        if (logged_in_count == 0) {
            ui_note("no hax-managed logins to remove");
            disp_sync_external_line(&state->render->disp);
            return;
        }
        /* Show the picker even for a single login so removal is confirmed, not immediate. */
        method = choose_method("log out of", logged_in, logged_in_count);
        if (!method) {
            note_needs_argument(state, "logout");
            return;
        }
    }

    int result = method->logout();
    if (result == 0)
        ui_note("no hax-managed login for %s", method_label(method));
    else if (result > 0)
        ui_note("logged out of %s", method_label(method));
    if (result > 0 && state->provider &&
        strcmp(provider_stable_id(state->provider), method->provider_id) == 0)
        method->adopt(state->provider);
    disp_sync_external_line(&state->render->disp);
}
