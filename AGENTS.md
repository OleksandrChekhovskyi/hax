# AGENTS.md

This file provides guidance to AI agents when working with code in this repository.

## Build, test, lint

```sh
meson setup build              # once
meson compile -C build         # build
meson test -C build --print-errorlogs   # run all tests
meson test -C build <name>     # single test (names are registered in tests/meson.build)
```

`Makefile` is a thin nvim `:make` wrapper around the same commands (`make`, `make tests`,
`make lint`, `make clean`).

`make lint` runs `clang-format --dry-run --Werror` over `src/` and `tests/`. Run
`clang-format -i` over any C file you touch before reporting work complete; the style is
enforced exactly as `.clang-format` describes (LLVM base, Linux braces, **4-space indent,
spaces not tabs**, 100-col limit).

ASan build for sharper error messages:
`meson setup build-asan -Db_sanitize=address,undefined && meson compile -C build-asan`.

Tests are plain C binaries using the macros in `tests/harness.h` (`EXPECT`, `EXPECT_STR_EQ`,
`T_REPORT`). All production code (everything but `src/main.c`) compiles into a single
`hax_lib` archive that both the `hax` executable and the tests link against; lazy
static-archive linking pulls in only the objects each test transitively references. To add a
test, append its source path to the `test_sources` list in `tests/meson.build` (grouped by
subdirectory, mirroring the `sources` list) — no need to list production sources or
dependencies (they propagate through `hax_dep`). The registered test name is derived from the
path: `test_` prefix and `.c` suffix stripped, subdirectory kept (`tools/test_read.c` →
`tools/read`, `test_util.c` → `util`). That name is what `meson test -C build <name>` matches.

For manual / visual checks of the dispatch + rendering pipeline without an LLM round-trip,
`HAX_PROVIDER=mock` activates a scripted provider (`src/providers/mock.c`) that emits canned
text and tool calls. With `HAX_MOCK_SCRIPT=path` it replays a small line-based DSL one turn
per `stream()` call (see `scripts/mock_demo.txt` for an example); without a script it parses
the latest user message heuristically (a backtick-quoted argument becomes a bash or read
tool call). Pair it with `scripts/stream_demo.py` to drive the bash tool through realistic
streaming patterns (`short`, `long`, `slow`, `burst`, `ansi`, `binary`, `piped`,
`python_buffer`) at adjustable cadence via `--delay`.

Two debug logs are useful when something goes wrong; both truncate on startup (and
`HAX_TRANSCRIPT` also truncates on `/new`):

- `HAX_TRACE=path` — wire-level Markdown dump of every HTTP request/response and SSE event
  (Authorization redacted). Plain text, no ANSI. Use when you suspect a provider-
  translation bug.
- `HAX_TRANSCRIPT=path` — append-only mirror of the Ctrl-T transcript view: system prompt,
  advertised tools, and every turn's items including tool calls + results. Plain text, no
  ANSI — `cat`/`grep`/diff work directly. Use when you need a less-verbose, model-
  perspective view of the conversation, or when debugging the mock provider (no HTTP, so
  `HAX_TRACE` is empty there).

## Architecture

hax is a single-binary REPL:
`input → build context → provider streams events → assemble turn → dispatch tools → loop`.
Everything funnels through small, stable interfaces in `src/provider.h` and `src/tool.h`.

**Terminology:** a bare **turn** is ONE model round-trip — a single `stream()` call
producing one assistant response (text and/or a batch of tool calls). A **user turn** is the
larger unit: one user prompt plus every turn it spawns (call tools → run them → stream again
→ …) until a turn comes back with no tool calls — so a user turn contains one or more turns.
The bare-turn sense follows the Anthropic / OpenAI agent SDKs (`maxTurns` / `max_turns`).
"turn" is overloaded in the wider field — alone it often means a whole user→assistant
exchange — so hax keeps that broader meaning behind the qualified **"user turn"** and never
bare "turn." `ITEM_TURN_BOUNDARY` marks the seam between consecutive turns; the per-user-turn
usage summary aggregates across them (see `src/turn.{c,h}`).

**`struct provider` (provider.h)** is the multi-provider seam. Each adapter exposes
`stream(provider, context, model, cb, user, tick, tick_user)` that drives an SSE response and
emits `struct stream_event` (text deltas, tool-call start/delta/end, reasoning items, done
with usage, error). Adapters own their native SSE parsing and translate to the unified event
vocabulary; the agent never sees provider-specific JSON. The `tick` slot threads through to
`http_sse_post` so the agent's idle/cancel bookkeeping rides the same callback. The shared `context_limit` atomic is
an optional, late-fill slot for provider-owned context-window probes; the agent reads it when
rendering the per-user-turn `%`-of-context display, after honoring `HAX_CONTEXT_LIMIT`.

Adapters live in `src/providers/`. Where a provider has a non-trivial SSE-event-to-
`stream_event` translation, the pure translation logic is split into a sibling
`<provider>_events.{c,h}` so it can be unit-tested without HTTP.

**Provider registry (`struct provider_factory`)** — each adapter exports one
`const struct provider_factory PROVIDER_<NAME>` symbol pairing the `HAX_PROVIDER` env value
with its constructor. The factory's `new(name)` / `available(name, reason)` hooks take the
factory's own name, so one generic constructor can serve many config-defined providers (see
below); the compiled-in factories serve one provider each and ignore the argument.
`src/providers/registry.{c,h}` collects the compiled-in factories into `BUILTINS[]` (in
autoselect-priority order — `provider_all()[0]` is the default for the unset-`HAX_PROVIDER`
case) and exposes `provider_find(name)` / `provider_all(n)` / `provider_list_names(out)` /
`provider_default()`. The registry lives next to the adapters, not in `provider.h`, so the
seam header stays a pure interface and callers (the `/provider` picker in `select.c`) can
`#include "providers/registry.h"` without dragging in the closed set. Adding a compiled-in
provider = drop a file under `src/providers/`, add its source to the `src/providers/` group
of `sources` in `meson.build` (kept sorted), then add the `extern PROVIDER_*` declaration to
`registry.h` and insert the symbol into `BUILTINS[]` in `registry.c` at the right priority
position.

**Presets over the OpenAI Chat Completions translation** — `openai.c` owns the shared
message/tool/SSE translation and exposes
`openai_provider_new_preset(const struct openai_preset *)` plus a thin
`openai_provider_new()` shim for real OpenAI. The other shims —
`openai_compat.c` (bring-your-own URL), `llamacpp.c` (synchronous `/v1/models` model probe +
background `/props` context probe), and `openrouter.c` (background
`/api/v1/models/{model}/endpoints` context probe + attribution headers) — each supply a
`struct openai_preset` declaring defaults: display name, default base URL, API-key env
fallback, prompt_cache_key policy, extra request headers. New OpenAI-compatible backends with
real auth/probe logic are typically a ~30-line preset file.

**Config-defined providers (`providers/config_provider.{c,h}`)** — backends with no custom
code are *data*, not a preset file: a named `providers.<name>` block in `config.json` (the
file/runtime-override lane — no per-provider env binding; the env/ad-hoc lane stays the
global `openai-compatible` preset reading `openai.*`), optionally seeded by a built-in
*recipe* in the `RECIPES[]` table (ollama ships as one). A recipe is the default field set
for a well-known endpoint; a config block overlays it key-by-key. The `api` field picks the
dialect (today only `openai-completions`, which builds via `openai_provider_new_preset` with
`openai_preset.config_prefix = "providers.<name>"` so the named provider reads its *own*
subtree — base_url / api_key / send_cache_key / reasoning_roundtrip — and a stray
`HAX_OPENAI_*` can't bleed in). The API key is the one value taken from the environment, via
a recipe- or config-declared `api_key_env`. `config_providers()` builds a heap factory per
recipe/config name (deduped); `registry.c` merges them below `BUILTINS[]` (a built-in name
wins) so they appear in `provider_find` / `provider_all` / `/provider` with no code change.
Ship a recipe only for an endpoint fully describable by static metadata (no auth/transport
code) — model discovery is live via `/v1/models`, so there is no catalog to maintain.

**`struct context` and `struct item` (provider.h)** are the flat conversation view: a sequence
of `USER_MESSAGE | ASSISTANT_MESSAGE | TOOL_CALL | TOOL_RESULT | REASONING`. `REASONING` carries
either Codex's opaque encrypted chain-of-thought (`reasoning_json`, round-tripped verbatim) or
plain CoT text (`reasoning_text`) captured from an openai-family `reasoning_content` stream and
replayed as `reasoning_content` when the provider opts in (see
`openai_preset.roundtrip_reasoning_field`).

**`src/turn.{c,h}`** is a pure state machine that assembles one streamed response into
`struct item`s. Driven by `turn_on_event`, drained by `turn_take_items`. No I/O — keeps
display logic decoupled.

**`src/agent.c`** is the REPL. It owns the conversation history vector, the tool table, and
the display layer (`struct disp`) that buffers trailing newlines so block separators between
user text / model text / tool calls always render as exactly one blank line.

**`struct tool` (tool.h)** — each tool lives in its own translation unit under `src/tools/`
and exports exactly one `const struct tool` symbol that the agent registers. `run(args_json)`
returns a freshly-allocated string (never NULL — error messages are tool output the model can
recover from). Set `output_is_diff = 1` for tools whose successful output is a unified diff
(rendered colored, uncapped); failure messages from the same tool are auto-detected by the
missing `--- ` prefix and fall through to the standard dim preview.

**`src/transport/sse.{c,h}`** is a small boundary-safe SSE parser used by adapters.
**`src/transport/http.{c,h}`** wraps libcurl: `http_sse_post` for the streaming response path
(with configurable idle timeout and a polled tick callback), and `http_get` for bounded JSON
GETs used by startup probes and Codex usage queries. The tick is `int (*)(void *user)` —
called from libcurl's progress hook (~1Hz, fires even when the server is silent) and on every
received chunk; non-zero return aborts the transfer. The agent uses it both to honor Esc
cancellation and to detect "model went quiet mid-text" idle and surface a spinner. All
libcurl handles set `CURLOPT_NOSIGNAL` so foreground streams and background probes can run
concurrently.

**`src/system/bg_job.{c,h}`** is the tiny background job primitive for provider-owned async
work: spawn/cancel/join plus `bg_job_tick(void *job)` — an `http_tick_cb`-shaped wrapper
that workers pass straight to `http_get` / `http_sse_post` with their job pointer.
`src/providers/probe.{c,h}` builds on it for context-window probes; each provider that
spawns a probe owns the handle and joins it in `destroy()` before freeing the target state.

**`src/terminal/ansi.h`** centralizes ANSI escape sequences — never inline `\033[...m`
literals; add a constant there.

Other modules under `src/` (`config`, `system/diff`, `render/markdown`, `trace`, `util`,
`render/spinner`, `system/fs`, …) are small, focused helpers; their headers describe what
they do. `config.{c,h}` holds the process-wide user config
(`~/.config/hax/config.json`, loaded once at startup). Every tunable has a canonical key
and an env-var binding declared once in a registry inside `config.c`; callers read by
canonical key via `config_str` / `config_int` / `config_bool` / `config_size` /
`config_duration_ms` (never `getenv`), so a setting can't be accidentally env-only and the
parse grammar lives in one place. Resolution is `runtime override → env → file → registry
default`: `config_set_override` is the session-scoped runtime tier (what `/model` etc. will
write), `config_persist` writes the file tier ("remember this"), and env still wins over the
file so the quick `HAX_FOO=bar hax …` invocation is unchanged. The file uses the canonical
keys as nested objects (`{"openai": {"base_url": …}}`; flat-dotted also accepted). This is
the seam runtime model/provider/preset selection builds on.

## Code style and conventions

- Linux-kernel style, portabilized: 4-space indent (see `.clang-format`), snake_case, `{` on
  new line for function definitions and same line for control flow, no `typedef`'d structs
  (always `struct foo`).
- C11, `-D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -D_DARWIN_C_SOURCE`, warning_level=3.
- Plain `malloc`/`calloc`/`free` (helpers `xmalloc`/`xstrdup`/`xasprintf` in `src/util.h`
  abort on OOM). No arena.
- Multi-resource cleanup: kernel-style goto-unwind labels in reverse-acquisition order
  (`goto err_free_buf:` / `goto err_close_fd:`).
- `json_decref` jansson roots at end of every turn and at every early-exit.
  `curl_easy_cleanup` on libcurl handles. `input_readline()` returns malloc'd memory — caller frees.
- Skip kernel idioms not portable to userspace: no `likely()`/`unlikely()`, `BUG_ON`,
  `ERR_PTR`, `kmalloc`. Use `<stdint.h>` types (`uint32_t`), plain negative-int returns +
  `errno`.
- Every source file starts with `/* SPDX-License-Identifier: MIT */`.
- Markdown (this file, `README.md`, etc.) is hard-wrapped at ~100 columns, same as code.

## Dependencies

Current pinned set is in `meson.build` — at time of writing: **libcurl** (HTTPS+SSE),
**jansson** (JSON), **pthreads**. Line editing is in-tree (`src/terminal/input.c`), not a
dependency.

Rule: every dependency must be in Debian main and either ship with macOS or be a single
`brew install`. Don't add a dependency without confirming that property; don't suggest GPL
libraries (would break MIT distribution).

Out of scope intentionally: ncurses (raw ANSI instead), TOML/YAML config (configuration is
env vars plus an optional JSON file parsed with the already-present jansson — see
`src/config.{c,h}` — so no new config-parser dependency), OpenSSL direct linking, glib,
libxdiff.
