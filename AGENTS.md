# AGENTS.md

Guidance for AI agents working in this repository. Keep this file high-level: commands,
architecture seams, and durable conventions belong here; module-level details belong in code or
headers.

## Build, test, lint

```sh
make                                      # build (quiet; sets up build/ on first run)
make tests                                # build + all tests (unit + e2e)
make lint                                 # clang-format + style script + clang-tidy
scripts/check.sh test <name>...           # build + selected tests (one or more names)
```

`Makefile` delegates to `scripts/check.sh`, which drops routine runner progress but relays
compiler and test diagnostics whether or not the phase succeeds, so a clean run is just a
compact confirmation — prefer these over raw meson invocations to keep output small. The
verbose equivalents (`meson compile -C build`, `meson test -C build --print-errorlogs`)
remain available for per-test timings or full build logs.

`make lint` is the single "is the code clean" gate: clang-format, the project style checks
in `scripts/lint_style.py`, and clang-tidy. Failures say what to fix; the conventions they
enforce are documented where they live (`.clang-format`, `.clang-tidy`, the script's
docstring). Run `clang-format -i` on any C source/header you touch before reporting done.

`BUILD_DIR` selects the build directory; these presets are set up on first use. Any other
name needs `meson setup <dir> <options>` first.

| `BUILD_DIR` | Meson options | For |
| --- | --- | --- |
| `build` (default) | `debugoptimized` from `meson.build` | everyday build, test, lint |
| `build-asan` | `-Db_sanitize=address,undefined` | memory errors, undefined behavior |
| `build-tsan` | `-Db_sanitize=thread` | data races |
| `build-release` | `--buildtype=release` | extra inlining warnings; run before a release |

```sh
BUILD_DIR=build-asan make tests
BUILD_DIR=build-tsan scripts/check.sh test <name>
BUILD_DIR=build-release make
```

Tests are plain C binaries using `tests/harness.h` (`EXPECT`, `EXPECT_STR_EQ`, `T_SKIP`,
`T_REPORT`). Create scratch directories with the harness's `t_tempdir()`, which removes them
at process exit; raw `mkdtemp` in tests fails `make lint`.
To add a test, append its source to `test_sources` in `tests/meson.build`, grouped to mirror
the production `sources` list. Test names are path-derived: `tools/test_read.c` becomes
`tools/read`, and `test_util.c` becomes `util`.

End-to-end scenarios follow the same conventions in Python: standalone scripts under
`tests/e2e/`, registered in `e2e_scenarios` in `tests/meson.build`. They run the built binary
hermetically against mock scripts from `scripts/mock/` via `tests/e2e/harness.py`; its
docstrings are the how-to.

Useful manual/debug knobs:

- `HAX_PROVIDER=mock` runs the scripted/mock provider. Pair with `HAX_MOCK_SCRIPT=path` or
  `scripts/stream_demo.py` for visual checks without a live LLM.
- `HAX_TRACE=path` logs HTTP/SSE traffic with auth redacted.
- `HAX_TRANSCRIPT=path` logs the model-facing transcript, including tools and results.

### Driving the interactive UI

The REPL prompt and the pickers need a real tty, so they can't be checked by piping stdin.
Use tmux rather than hand-rolled pty scripts — send keys, capture the pane, read the result:

```sh
tmux new-session -d -s haxtest -x 110 -y 32 'HAX_PROVIDER=mock ./build/hax'
tmux send-keys -t haxtest '/model' Enter   # keys; Enter/C-u/Escape as named keys
tmux capture-pane -t haxtest -p            # pane text, escapes already resolved
tmux kill-session -t haxtest
```

Scope cleanup to exactly what you started. The user may be working inside tmux, and this agent
may itself be running inside hax, so anything that matches by name takes their session down
along with the one under test: no `kill-server`, and no `pkill hax` / `killall hax` /
`pkill -f hax`. Kill the session you named, or the PID you captured at launch.

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

Layer boundaries and the rules that keep them:

- `src/agent_core.{c,h}` and `src/agent_loop.{c,h}` contain behavior shared by the interactive
  (`src/agent.c`) and one-shot (`src/oneshot.c`) frontends. Keep frontend-specific I/O and
  rendering out of the shared layers.
- `src/turn.{c,h}` is a pure state machine: consume `struct stream_event`, emit
  `struct item`. Keep I/O out of it.
- `src/provider.h` defines the flat conversation view (`struct context` / `struct item`) and
  the provider streaming interface. Provider adapters translate native APIs/SSE into
  `struct stream_event`; provider-specific JSON should not leak into the agent.
- `src/providers/registry.{c,h}` owns provider discovery: each compiled-in provider exports one
  `const struct provider_factory PROVIDER_<NAME>`, and `BUILTINS[]` order is autoselect priority.
- Protocol-compatible providers should reuse the shared family translation via presets
  (`src/providers/openai.c` or `src/providers/anthropic.c`) where possible. Purely static
  endpoints should be config-defined providers rather than new C shims.
- `src/providers/openai.c` serves both OpenAI request protocols, selected per preset by
  `enum openai_wire`: Chat Completions (`openai_{events,messages}.c`) and Responses
  (`responses_{events,messages}.c`, shared with `codex.c`). First-party OpenAI speaks Responses
  because recent reasoning models reject function tools with a reasoning effort on the older
  endpoint, and only Responses returns replayable encrypted reasoning.
- `src/tool.h` defines the tool seam. Each tool lives under `src/tools/`, exports exactly one
  `const struct tool`, and returns freshly allocated output from `run(args_json, ctx)`; tool
  errors are output the model can recover from, not NULL returns.
- `src/tools/task_registry.{c,h}` owns background tasks: bash commands detach into tasks at
  their timeout or on an explicit `background` request, `task_wait` is the sole model-facing
  control, and the agent loop injects finished-task notes as user items before each request.
  Both frontends call `task_registry_shutdown()` on exit; `no_tasks` disables the whole
  mechanism (bash reverts to kill-on-timeout).
- `src/transport/sse.{c,h}` and `src/transport/http.{c,h}` are the transport boundaries.
  Streaming code uses the shared tick callback for cancellation/idle handling.
- `src/config.{c,h}` is the configuration access layer. Declare user-facing tunables in the
  config registry and read them by canonical key; reserve direct `getenv` calls for process
  environment facts or deliberately env-only secrets.
- `src/model_meta.{c,h}` is the per-model metadata access layer (window, output cap,
  modalities, effort levels). Consumers ask it, never a tier directly; every provider
  `destroy()` must call `model_meta_release`.
- `src/catalog.{c,h}` is the models.dev tier underneath it; providers opt in by setting
  `provider->catalog_id`. Cost *estimation* lives in the agent layer (`agent_session_spend`),
  never in provider adapters.
- `src/terminal/ansi.h` centralizes ANSI escape sequences; do not inline raw escape literals.
  Colors go through the semantic roles in `src/terminal/theme.{c,h}` (presets resolved from the
  `theme` config key at startup); bold/dim/italic attributes stay direct `ANSI_*`.
- `src/render/disp.{c,h}` owns where display bytes go: every renderer writes through a
  `struct disp` and its sink, never `stdout` directly. Cursor-addressed output (markdown
  retro-wrap, tool-block overprints) is settled by `src/terminal/vt_resolve.{c,h}` before it
  can go anywhere but a terminal. `src/transcript.{c,h}` is what the model saw,
  `src/history.{c,h}` what the user saw — keep them distinct.

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
- Follow [`docs/code-style.md`](docs/code-style.md): prefer precise names, types, and structure over
  explanatory comments.
- Comments document only durable contracts, constraints, or non-obvious rationale. Do not narrate
  code, preserve change history or reviewer discussion, or inventory current implementation details.

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
