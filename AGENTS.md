# AGENTS.md

Guidance for AI agents working in this repository. Keep this file high-level: commands,
architecture seams, and durable conventions belong here; module-level details belong in code or
headers.

## Build, test, lint

```sh
make                                      # build (quiet; sets up build/ on first run)
make tests                                # build + all tests
make lint                                 # clang-format check
scripts/check.sh test <name>              # build + one test
```

`Makefile` delegates to `scripts/check.sh`, which keeps successful output to a compact
confirmation and shows full compiler/test output only on failure — prefer these over raw
meson invocations to keep output small. The verbose equivalents (`meson compile -C build`,
`meson test -C build --print-errorlogs`) remain available for per-test timings or full
build logs.

`make lint` runs `clang-format --dry-run --Werror` over C sources/headers in `src/` and
`tests/`. Run `clang-format -i` on any C source/header you touch before reporting done.
Style is enforced by `.clang-format`: LLVM base, Linux braces, 4-space indent, spaces not
tabs, 100-column limit.

Sanitizer builds for sharper failures: set `BUILD_DIR=build-asan` (ASan/UBSan,
`-Db_sanitize=address,undefined`) or `BUILD_DIR=build-tsan` (ThreadSanitizer,
`-Db_sanitize=thread`); both are set up automatically when missing:

```sh
BUILD_DIR=build-asan make tests
BUILD_DIR=build-tsan scripts/check.sh test <name>
```

Tests are plain C binaries using `tests/harness.h` (`EXPECT`, `EXPECT_STR_EQ`, `T_SKIP`,
`T_REPORT`).
To add a test, append its source to `test_sources` in `tests/meson.build`, grouped to mirror
the production `sources` list. Test names are path-derived: `tools/test_read.c` becomes
`tools/read`, and `test_util.c` becomes `util`.

Useful manual/debug knobs:

- `HAX_PROVIDER=mock` runs the scripted/mock provider. Pair with `HAX_MOCK_SCRIPT=path` or
  `scripts/stream_demo.py` for visual checks without a live LLM.
- `HAX_TRACE=path` logs HTTP/SSE traffic with auth redacted.
- `HAX_TRANSCRIPT=path` logs the model-facing transcript, including tools and results.

## Architecture

hax is a single-binary REPL:

`input → build context → provider streams events → assemble turn → dispatch tools → loop`

The stable seams are `src/provider.h` and `src/tool.h`.

Terminology:

- A **turn** is one provider `stream()` round-trip producing one assistant response and
  optional tool calls.
- A **user turn** is one user prompt plus every spawned turn until the model stops requesting
  tools.
- `ITEM_TURN_BOUNDARY` separates consecutive turns inside one user turn.

Core modules and responsibilities:

- `src/agent.c` owns the REPL, conversation history, registered tools, dispatch loop, and
  display buffering.
- `src/turn.{c,h}` is a pure state machine: consume `struct stream_event`, emit
  `struct item`. Keep I/O out of it.
- `src/provider.h` defines the flat conversation view (`struct context` / `struct item`) and
  the provider streaming interface. Provider adapters translate native APIs/SSE into
  `struct stream_event`; provider-specific JSON should not leak into the agent.
- `src/providers/registry.{c,h}` owns provider discovery. Compiled-in providers export one
  `const struct provider_factory PROVIDER_<NAME>` and are listed in `BUILTINS[]` in
  autoselect priority order.
- Protocol-compatible providers should reuse the shared family translation via presets
  (`src/providers/openai.c` or `src/providers/anthropic.c`) where possible. Purely static
  endpoints should be config-defined providers rather than new C shims.
- `src/tool.h` defines the tool seam. Each tool lives under `src/tools/`, exports exactly one
  `const struct tool`, and returns freshly allocated output from `run(args_json)`; tool errors
  are output the model can recover from, not NULL returns.
- `src/transport/sse.{c,h}` and `src/transport/http.{c,h}` are the transport boundaries.
  Streaming code uses the shared tick callback for cancellation/idle handling.
- `src/config.{c,h}` is the configuration access layer. Declare user-facing tunables in the
  config registry and read them by canonical key; reserve direct `getenv` calls for process
  environment facts or deliberately env-only secrets.
- `src/catalog.{c,h}` is the model-metadata access layer (per-model cost rates, window
  limits): a config `catalog.models` tier over a background-cached models.dev snapshot.
  Providers opt in by setting `provider->catalog_id`; cost *estimation* lives in the agent
  layer (`agent_session_spend`), never in provider adapters.
- `src/terminal/ansi.h` centralizes ANSI escape sequences; do not inline raw escape literals.
  Colors go through the semantic roles in `src/terminal/theme.{c,h}` (presets resolved from the
  `theme` config key at startup); bold/dim/italic attributes stay direct `ANSI_*`.

When adding a compiled-in provider: add the source under `src/providers/`, list it in
`meson.build`, declare its factory in `registry.h`, and insert it into `BUILTINS[]` in
`registry.c` at the right priority.

When adapter event translation is non-trivial, split pure translation into
`<provider>_events.{c,h}` so it can be unit-tested without HTTP.

## Code style and conventions

- C11, warning level 3, with the project feature defines from `meson.build`.
- Linux-kernel-inspired userspace style: snake_case, no typedef'd structs, function braces on
  their own line, control-flow braces on the same line.
- Every source file starts with `/* SPDX-License-Identifier: MIT */`.
- Use plain `malloc`/`calloc`/`free`; `xmalloc`/`xstrdup`/`xasprintf` in `src/util.h` abort on
  OOM. No arenas.
- Use kernel-style goto cleanup for multi-resource functions, with labels in reverse
  acquisition order.
- Always release owned resources on success and all early exits: `json_decref` jansson roots,
  `curl_easy_cleanup` handles, `free` buffers, etc.
- `input_readline()` returns malloc'd memory; the caller owns it.
- Avoid non-portable kernel idioms: no `likely()`/`unlikely()`, `BUG_ON`, `ERR_PTR`, or
  `kmalloc`. Use `<stdint.h>` types and plain negative-int returns plus `errno`.
- Markdown is hard-wrapped around 100 columns, same as code.
- Only add comments that add significant value by explaining things not obvious from the code
  directly. Keep comments concise, avoid "walls of text".

## Git conventions

Do not create commits or perform any other git history manipulation unless the user explicitly
prompts for it. This includes commands such as `git commit`, `git commit --amend`, `git rebase`,
`git reset`, `git cherry-pick`, and `git merge`.

Do not switch or create branches and do not push anything to remote, unless explicitly prompted.

Commit messages follow these patterns:
- For the subject line, use sentence case with a present-tense verb (e.g., "Add", "Fix").
  Subject line does not end with a period.
- Prefer adding a brief explanatory body after the subject line. Describe why the change was made
  and summarize what changed, while keeping it concise. Explanatory body is free-form.
- For non-trivial commits, write the commit message to a temporary file first to check formatting
  before committing. Aim for approximately 80-90 columns in commit message prose.

## Dependencies

Current dependencies are pinned in `meson.build`: libcurl (HTTPS/SSE), jansson (JSON), and
Meson's platform threads dependency. Line editing is in-tree (`src/terminal/input.c`).

Every dependency must be in Debian main and either ship with macOS or be available via a
single `brew install`. Do not add GPL libraries.

Intentionally out of scope: ncurses, TOML/YAML config, direct OpenSSL linking, glib, libxdiff.
