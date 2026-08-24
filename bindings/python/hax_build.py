"""Declarations for the _hax_cffi extension, and the generator meson drives.

cffi API mode compiles these against the real hax headers, so the declarations below are checked
rather than trusted: a struct that gains a field, or a signature that changes, fails the build
instead of silently misreading memory at runtime. `...;` marks a struct as partial, letting the
declarations name only the fields the binding touches while cffi resolves the true layout.

This module only *emits* C. Meson compiles and links it, so the extension is built by the same
build system as the rest of hax and needs no setuptools:

    meson setup build-embed -Dembed=true && meson compile -C build-embed
"""

from __future__ import annotations

import sys

from cffi import FFI


ffibuilder = FFI()

ffibuilder.cdef(
    """
/* --- provider.h --- */
enum item_kind {
    ITEM_USER_MESSAGE, ITEM_ASSISTANT_MESSAGE, ITEM_TOOL_CALL, ITEM_TOOL_RESULT,
    ITEM_REASONING, ITEM_TURN_BOUNDARY, ITEM_TURN_USAGE
};

enum item_origin { ITEM_ORIGIN_NONE, ... };

struct item {
    enum item_kind kind;
    char *text;
    char *call_id;
    char *tool_name;
    char *tool_arguments_json;
    char *output;
    enum item_origin origin;
    ...;
};

void item_free(struct item *item);

struct provider;

/* --- tool.h --- */
struct tool_run_ctx {
    int image_input;
    ...;
};

/* --- agent_core.h --- */
struct hax_opts {
    int raw;
    const char *resume_path;
    int provider_autoselected;
};

struct agent_session {
    char *model;
    char *model_label;
    char *effort;
    char *system_prompt;
    struct item *items;
    size_t n_items;
    ...;
};

void agent_session_init(struct agent_session *session, struct provider *provider,
                        const struct hax_opts *opts);
void agent_session_free(struct agent_session *session);
void agent_session_add_user(struct agent_session *session, const char *text);

/* --- agent_tool.h --- */
struct agent_tool_call { ...; };

void agent_tool_call_init(struct agent_tool_call *tc, const struct item *call);
void agent_tool_call_destroy(struct agent_tool_call *tc);
char *agent_tool_call_run(const struct agent_tool_call *tc, struct tool_run_ctx *ctx);
struct item agent_tool_result_make(const struct item *call, const char *output,
                                   struct tool_run_ctx *ctx);

/* --- agent_loop.h --- */
enum agent_loop_tool_action { AGENT_LOOP_TOOL_RUN, AGENT_LOOP_TOOL_REFUSE, AGENT_LOOP_TOOL_SKIP };
enum agent_loop_signal { AGENT_LOOP_SIG_NONE, AGENT_LOOP_SIG_PAUSE, AGENT_LOOP_SIG_ABORT };
enum agent_loop_outcome {
    AGENT_LOOP_COMPLETE, AGENT_LOOP_PROVIDER_ERROR, AGENT_LOOP_INTERRUPTED,
    AGENT_LOOP_PAUSED, AGENT_LOOP_MAX_TURNS
};

struct agent_loop_hooks {
    void *user;
    int (*checkpoint)(void *user);
    struct item (*tool_call)(const struct item *call, enum agent_loop_tool_action action,
                             int image_input, void *user);
    ...;
};

struct agent_loop_result {
    enum agent_loop_outcome outcome;
    int turns;
    long last_context_tokens;
    char *error_message;
    ...;
};

struct agent_loop_params {
    struct agent_session *session;
    struct provider *provider;
    struct transcript_log *tlog;
    struct session_log *slog;
    int max_turns;
    int continued;
    struct agent_loop_hooks hooks;
};

void agent_loop_run(const struct agent_loop_params *params, struct agent_loop_result *result);
void agent_loop_result_destroy(struct agent_loop_result *result);

/* --- system/cancel.h --- */
void cancel_request_abort(void);
void cancel_clear_requests(void);

/* --- config.h --- */
void config_set_override(const char *key, const char *value);

/* --- util.h --- */
enum hax_diag_level { HAX_DIAG_ERR, HAX_DIAG_WARN };

/* --- hax_embed.h --- */
struct hax_embed_options {
    int own_locale;
    int own_curl_global;
    int own_atexit;
    void (*diag)(enum hax_diag_level level, const char *message, void *user);
    void *diag_user;
};

int hax_init(const struct hax_embed_options *options);
void hax_shutdown(void);
struct provider *hax_provider_new(const char *name);
void hax_provider_destroy(struct provider *provider);

struct hax_abi {
    unsigned version;
    size_t sizeof_item;
    size_t sizeof_agent_session;
    size_t sizeof_agent_loop_params;
    size_t sizeof_agent_loop_result;
    size_t sizeof_agent_loop_hooks;
};

const struct hax_abi *hax_abi(void);
#define HAX_ABI_VERSION ...

/* Trampolines: cffi emits real C functions that call into Python and handle the GIL. */
extern "Python" struct item hax_py_tool_call(const struct item *, enum agent_loop_tool_action,
                                             int, void *);
extern "Python" int hax_py_checkpoint(void *);
extern "Python" void hax_py_diag(enum hax_diag_level, const char *, void *);

/* free() for the strings hax hands back. */
void free(void *);
"""
)


def configure() -> None:
    """Set the module name and the C preamble. Include paths, libraries, and flags are meson's."""
    ffibuilder.set_source(
        "_hax_cffi",
        """
        #include "agent_core.h"
        #include "agent_loop.h"
        #include "agent_tool.h"
        #include "config.h"
        #include "hax_embed.h"
        #include "provider.h"
        #include "system/cancel.h"
        #include "tool.h"
        #include "util.h"
        #include <stdlib.h>
        """,
    )


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: hax_build.py <output.c>")
    configure()
    ffibuilder.emit_c_code(sys.argv[1])
