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
`T_REPORT`). To add one, append an `executable(...)` + `test(...)` pair to `tests/meson.build`
listing the test source plus the production `.c` files it links against.

## Architecture

hax is a single-binary REPL:
`input → build context → provider streams events → assemble turn → dispatch tools → loop`.
Everything funnels through small, stable interfaces in `src/provider.h` and `src/tool.h`.

**`struct provider` (provider.h)** is the multi-provider seam. Each adapter exposes
`stream(provider, context, model, cb, user)` that drives an SSE response and emits
`struct stream_event` (text deltas, tool-call start/delta/end, reasoning items, done with
usage, error). Adapters own their native SSE parsing and translate to the unified event
vocabulary; the agent never sees provider-specific JSON.

Adapters live in `src/providers/`. Where a provider has a non-trivial SSE-event-to-
`stream_event` translation, the pure translation logic is split into a sibling
`<provider>_events.{c,h}` so it can be unit-tested without HTTP.

**Provider registry (`struct provider_factory`)** — each adapter exports one
`const struct provider_factory PROVIDER_<NAME>` symbol pairing the `HAX_PROVIDER` env value
with its constructor. `src/main.c` collects them into `PROVIDERS[]`; the first entry is the
default when `HAX_PROVIDER` is unset, and the "unknown provider" error builds its supported
list from the array. Adding a new provider = drop a file under `src/providers/`, append the
`&PROVIDER_*` symbol to that array, and add the source to `meson.build`.

**Presets over the OpenAI Chat Completions translation** — `openai.c` owns the shared
message/tool/SSE translation and exposes
`openai_provider_new_preset(const struct openai_preset *)` plus a thin
`openai_provider_new()` shim for real OpenAI. The other shims —
`openai_compat.c` (bring-your-own URL), `llamacpp.c` (with `/v1/models` + `/props` probes),
and `openrouter.c` (with `/api/v1/models` context-length probe + attribution headers) —
each supply a `struct openai_preset` declaring defaults: display name, default base URL,
API-key env fallback, prompt_cache_key policy, extra request headers. New OpenAI-compatible
backends are typically a ~30-line preset file.

**`struct context` and `struct item` (provider.h)** are the flat conversation view: a sequence
of `USER_MESSAGE | ASSISTANT_MESSAGE | TOOL_CALL | TOOL_RESULT | REASONING`. `REASONING`
carries opaque provider-specific JSON (Codex's encrypted chain-of-thought) round-tripped
verbatim — non-Codex adapters ignore it.

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

**`src/sse.{c,h}`** is a small boundary-safe SSE parser used by adapters.
**`src/http.{c,h}`** wraps libcurl: `http_sse_post` for the streaming response path (with
configurable idle timeout and a polled cancel hook), and `http_get` for synchronous JSON
probes the preset shims use to auto-discover model/context limits at startup.

**`src/ansi.h`** centralizes ANSI escape sequences — never inline `\033[...m` literals; add a
constant there.

Other modules under `src/` (diff, markdown, trace, util, spinner, fs, …) are small, focused
helpers; their headers describe what they do.

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
**jansson** (JSON), **pthreads**. Line editing is in-tree (`src/input.c`), not a dependency.

Rule: every dependency must be in Debian main and either ship with macOS or be a single
`brew install`. Don't add a dependency without confirming that property; don't suggest GPL
libraries (would break MIT distribution).

Out of scope intentionally: ncurses (raw ANSI instead), TOML/YAML config (env vars only),
OpenSSL direct linking, glib, libxdiff.
