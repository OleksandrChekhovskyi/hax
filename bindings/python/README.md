# hax for Python

A cffi binding over `libhax`: hax's agent loop — provider round-trips, tool dispatch, compaction,
abort repair, session history — driven from Python, with tools you define in Python that the model
calls inside the turn.

## When to use this, and when not to

hax is a program first. Its normal extension point is `hax -p`: clean stdout, resumable sessions,
driven from a script. That covers most automation, needs no build step, and survives every
refactor. Prefer it.

Reach for this binding when you need something a subprocess cannot express:

- **Tools backed by host state** — a live database connection, an authenticated client, an
  in-memory cache. The model calls a Python function, not a shell command.
- **Policy on every tool call** — validate, rewrite, or refuse a call before it runs, in code
  rather than in a prompt.
- **Per-event access to a running turn** — observing the loop as it goes, not parsing output after.

If none of those apply, use the binary.

## Requirements

- `libhax`, built with `-Dembed=true`.
- A Python interpreter with development headers and `cffi`. The project's `uv` environment
  provides both; meson prefers it and falls back to `python3`.

## Build

```sh
meson setup build-embed -Dembed=true
meson compile -C build-embed
```

Meson generates and compiles the extension along with everything else — there is no separate
Python build step and no setuptools. If the chosen interpreter lacks headers or `cffi`, meson
prints a message and skips the binding; the rest of the build is unaffected.

Set `HAX_EXTENSION_DIR` to select a built extension. Without it, `build-embed/bindings` and
`build/bindings` beside the source tree are searched, so a plain `meson compile` is enough.

## Quick start

```python
import hax

with hax.Agent(provider="anthropic", model="claude-sonnet-5") as agent:

    @agent.tool
    def lookup_order(order_id):
        """Return the contents of an order."""
        return database[order_id]

    print(agent.send("what is in order 4417?"))
```

Provider setup is hax's own: `ANTHROPIC_API_KEY`, `OPENAI_API_KEY`, a `~/.config/hax/config.json`
block, and so on. See [docs/providers.md](../../docs/providers.md). Anything the binary can talk
to, the binding can.

## Examples

**`example.py`** — the smallest complete turn with one host tool, against the mock provider.

**`example_database.py`** — a read-only SQL agent against a real provider. Its tools close over a
live SQLite connection, and the read-only policy is enforced in Python, where the connection is,
rather than hoped for in the prompt. Asking "which customer spent the most, and on what?" makes
the model inspect the schema, write a join, and answer from the rows it retrieved.

```sh
export ANTHROPIC_API_KEY=...
uv run python bindings/python/example_database.py --provider anthropic \
    --model claude-sonnet-5 "which customer spent the most, and on what?"
```

Both accept `--provider mock` with `HAX_MOCK_SCRIPT` to replay a fixture instead of calling a
model, which is how the test suite exercises them.

## API

### `Agent(provider=None, model=None, *, system_prompt=None, max_turns=100, record_session=False)`

One conversation against one provider. `provider` and `model` fall back to hax's configuration
when omitted. `record_session=True` writes a resumable session log, off by default so an embedded
agent leaves nothing behind unless asked.

Use it as a context manager, or call `close()`. Both release the provider and hax's process-wide
state.

### `@agent.tool`

Registers a function as a tool. The name and signature come from the function; the docstring
becomes its description. A Python tool **shadows** a built-in of the same name, and any name the
host does not claim still runs hax's own tool — so `read`, `edit`, `write`, `bash`, and
`task_wait` remain available alongside yours.

Arguments arrive parsed from the model's JSON, by keyword. A `dict` or `list` return value is
sent back as JSON so the model can parse it; anything else is stringified.

A model that gets the call wrong — arguments that are not JSON, a JSON array where an object is
required, an argument name your function does not take — gets a tool error describing the
mistake and can correct it on the next turn. Your function is never called with arguments it
cannot accept, so a `TypeError` out of `send()` is always from your own body.

### `agent.send(prompt) -> str`

Runs one user turn to completion — every provider round-trip and tool call until the model stops
calling tools — and returns the final assistant text. Blocking, and interruptible with `cancel()`
from another thread.

### `agent.cancel()`

Asks a running `send()` to stop, which raises `HaxCancelled` in the thread that called it. Safe
from any thread: the GIL is released for the duration of the loop, and hax's cancel flags are
process-wide and atomic. A stopped turn leaves repaired, fully paired history, so the same Agent
continues normally on the next `send()`. Cancelling with no turn running is latched and consumed
by the next one.

### `agent.compact() -> bool`

Summarizes the conversation in place and reports whether a summary was appended. The loop calls
this on its own once the context crosses hax's `compact.threshold`, so most callers never need
it; it is here for a host that wants to compact at a moment of its own choosing.

Compaction appends a summary seed rather than deleting anything, so `items` still shows every
turn — what shrinks is the window the model sees. `compactions` counts how many have happened.

### `agent.items -> list[dict]`

The conversation as plain dicts with `kind`, `origin`, `text`, `call_id`, `tool_name`,
`arguments`, and `output`. `kind` is one of `user`, `assistant`, `tool_call`, `tool_result`,
`reasoning`, `boundary`, `usage`. Readable after `close()`.

`origin` is empty for an ordinary item and names hax's own provenance otherwise: `skipped` and
`refused` for a call that never ran, `interrupted` for a response cut short, `compact_seed` for a
summary, `continuation`, `summarized`, `task_note`. It is what separates a tool result hax wrote
from one your tool returned, which no amount of reading `output` can tell you — a tool may
legitimately return the same text.

### `agent.diagnostics -> list[str]`

Every hax diagnostic since construction. hax normally writes these to stderr; the binding captures
them instead and attaches the most recent one to errors it raises.

## Errors

`HaxError` is the base. `HaxProviderError` covers a failed or rejected provider stream, and
`HaxCancelled` a turn stopped by `cancel()`.

An exception raised inside your tool propagates out of `send()` unchanged, with its original
traceback. It cannot unwind through the C loop, so the binding stashes it, asks the loop to stop,
and hands hax a well-formed error result first — history stays consistent, and the turn ends at a
clean seam.

## Limits

- **One agent per process.** hax keeps configuration, provider selection, and diagnostics in
  process-wide state. Constructing a second `Agent` while one is live raises rather than letting
  two silently share one configuration.
- **No streaming API yet.** `send()` blocks until the turn completes, though `cancel()` can stop
  it. The underlying loop does expose a per-event hook; a callback API over it would be a small
  addition, a generator API a larger one.
- **The GIL is released** around the loop and reacquired for each callback, so other Python
  threads run during a provider round-trip. That is what makes `cancel()` from another thread
  work rather than deadlock.

## Layout

```
hax_build.py   cffi declarations; emits the glue C that meson compiles
hax/           the Agent API
example.py     minimal example
example_database.py
```

`hax_build.py`'s declarations are hand-written but **checked**: cffi compiles them against the real
hax headers, so a struct that gains a field or a signature that changes fails `meson compile`
rather than misreading memory at runtime. After changing a header the binding declares, rebuild.

## Tests

`tests/bindings/test_python.py`, registered with meson when the binding is built:

```sh
meson test -C build-embed bindings/python
```

See [docs/embedding.md](../../docs/embedding.md) for the C side — lifecycle, hooks, cancellation.
