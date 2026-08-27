# Embedding hax

hax is a program first. Its normal extension point is the one-shot mode — `hax -p`, clean stdout,
resumable sessions — driven from a script, as [philosophy.md](./philosophy.md) describes. Reach for
the library only when you need something that composition from outside cannot express: tools
defined by the host that the model calls inside a turn, or per-event access to a running turn.

Everything else is better served by a subprocess.

## Building the library

```sh
meson setup build-embed -Dembed=true
meson compile -C build-embed
```

This adds `libhax` beside the `hax` binary. It is off by default because a single-binary install is
this project's normal shape, and because the extra link costs every build that does not want it.

## Lifecycle

`hax_init()` replaces the setup `main()` performs, and takes three ownership decisions from the
caller. A host process usually owns its locale, libcurl, and exit handling already, so it passes
zero for each:

```c
struct hax_embed_options options = {
    .own_locale = 0,       /* setenv() from a library races other threads */
    .own_curl_global = 0,  /* curl_global_init() must happen exactly once per process */
    .own_atexit = 0,       /* an atexit handler outliving its module runs unmapped code */
    .diag = my_diag_sink,  /* NULL keeps diagnostics on stderr */
};
if (hax_init(&options) != 0)
    return -1;
```

Destroy every provider before `hax_shutdown()`: providers join background work that global libcurl
teardown must outlive.

**One agent per process.** Configuration, provider selection, theme, and diagnostics are
process-wide state. `hax_init()` refuses a second call rather than letting two agents silently
share one configuration.

## Tool calls from the host

Advertising a tool and dispatching one are separate steps, and a host needs both. The model can
only call what it was told exists, so register the definition first:

```c
struct tool_param params[] = {
    {.name = "order_id", .type = "string", .description = "which order", .required = 1},
};
struct tool_def def = {.name = "lookup_order", .description = "Return the contents of an order.",
                       .params = params, .n_params = 1};
agent_session_add_tool(&session, &def);
```

The session deep-copies the def, so the caller keeps ownership of everything it passed. A name
already advertised is replaced rather than added twice, which is how a host redefines a built-in.
Raw mode advertises no tools and rejects the call.

`agent_loop_hooks.tool_call` then receives one model-issued call and returns the matching result:

```c
static struct item on_tool_call(const struct item *call, enum agent_loop_tool_action action,
                                int image_input, void *user)
{
    return agent_tool_result_make(call, "done", NULL);
}

struct agent_loop_hooks hooks = {.user = my_state, .tool_call = on_tool_call};
```

Return an owned result on every path, including failure. The loop pairs each call with a result,
and history that ends on an unpaired call is malformed for the next request. Leaving `tool_call`
NULL runs hax's own tools, and a hook that handles only the names it knows can fall back to them
with `agent_tool_call_run()`.

`action` says whether the call should run at all, and the hook owes the same answer the loop
writes for itself when there is no hook. Only `AGENT_LOOP_TOOL_RUN` dispatches:

| `action` | Result text | `origin` |
| --- | --- | --- |
| `AGENT_LOOP_TOOL_RUN` | whatever the tool returned | leave at `ITEM_ORIGIN_NONE` |
| `AGENT_LOOP_TOOL_SKIP` | `INTERRUPT_MARKER` | `ITEM_ORIGIN_SKIPPED` |
| `AGENT_LOOP_TOOL_REFUSE` | `REFUSED_RESULT` | `ITEM_ORIGIN_REFUSED` |

```c
if (action != AGENT_LOOP_TOOL_RUN) {
    int skip = action == AGENT_LOOP_TOOL_SKIP;
    struct item result =
        agent_tool_result_make(call, skip ? INTERRUPT_MARKER : REFUSED_RESULT, NULL);
    result.origin = skip ? ITEM_ORIGIN_SKIPPED : ITEM_ORIGIN_REFUSED;
    return result;
}
```

Both halves matter. The text is what the model reads, so an invented marker gives it a
vocabulary that appears nowhere else in the conversation. `origin` is what everything else
reads: `session` persists it, so a resumed conversation can still tell a call that never ran
from one that ran and returned that text, and `history` renders the two differently. The action,
not the presentation path, decides both.

## Cancellation

`system/cancel.h` owns the latched pause and abort flags, independent of how they were requested.
An embedder calls `cancel_request_abort()` from a signal handler or another thread; the terminal's
Esc watcher is just another producer. Do not call `interrupt_init()` from a library — it claims
SIGINT and the terminal.

## Struct layouts

A binding compiled against these headers agrees with them by construction. `hax_abi()` covers the
other case: a `libhax` swapped underneath an extension built earlier, where a changed struct size
would corrupt memory rather than fail. Compare it at load time and refuse to run on a mismatch, as
the Python binding does at import.

## Python

[bindings/python](../bindings/python) is a cffi binding over `libhax`, built in API mode. The
declarations in `hax_build.py` are compiled against the real headers, so a struct that gains a
field or a signature that changes fails the build rather than misreading memory at runtime.

cffi only emits the C; meson compiles and links it like any other target, so the binding needs no
second build system and no setuptools:

```sh
meson setup build-embed -Dembed=true && meson compile -C build-embed
```

The extension is skipped, with a message, when the chosen interpreter lacks cffi or development
headers. Meson prefers the project's `.venv` and falls back to `python3`.

```python
import hax

with hax.Agent(provider="anthropic") as agent:

    @agent.tool
    def lookup_order(order_id: str):
        """Return the contents of an order."""
        return database[order_id]

    print(agent.send("what is in order 4417?"))
```

`HAX_EXTENSION_DIR` selects the built extension; otherwise `build-embed/bindings` and
`build/bindings` beside the source tree are searched, so a plain `meson compile` is enough to make
`import hax` work.

[bindings/python/README.md](../bindings/python/README.md) is the binding's own documentation: the
API, when to prefer `hax -p` instead, and two runnable examples.
