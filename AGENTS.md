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

The stable extension seams are `src/provider.h` and `src/tool.h`.

Terminology:

- A **turn** is one provider `stream()` round-trip producing one assistant response and optional
  tool calls.
- A **user turn** is one user prompt plus every spawned turn until the model stops requesting
  tools.
- `ITEM_TURN_BOUNDARY` separates consecutive turns inside one user turn.

Core boundaries:

- The canonical conversation state is the flat, provider-independent `struct item` log owned by
  `struct agent_session`. Compaction appends a summary seed without deleting prior history; build
  model-visible windows with `agent_session_context()` rather than slicing the raw log.
- Provider adapters own native API protocols and wire JSON; shared transport owns HTTP/SSE
  mechanics. Adapters serialize `struct context` and emit provider-independent
  `struct stream_event`; agent behavior must not depend on native response shapes.
- `src/turn.{c,h}` is a pure state machine from borrowed stream events to owned conversation items.
  Keep I/O and presentation out of it.
- Behavior shared by the interactive and one-shot frontends belongs below `src/agent.c` and
  `src/oneshot.c`, primarily in `agent_core` and `agent_loop`. Frontends supply presentation and
  cancellation through hooks rather than duplicating the continuation loop.
- Declare user-facing settings in the config registry and consume them by canonical key. Direct
  environment reads are for startup/bootstrap decisions, conventional process environment, or
  deliberately environment-only secrets.
- Process-wide config and live provider state are foreground-thread state. Resolve config and
  prepare owned worker inputs before spawning background work. Use `system/bg_job` for ordinary
  cooperative jobs, and join every worker before destroying state it may access or tearing down
  global libcurl state.
- `model_meta` is the resolved view for live provider/model capability decisions; `catalog` is its
  lower-level metadata and pricing source. Cost estimation belongs in `agent_usage`, not provider
  adapters.
- `transcript` renders the model-facing conversation, `history` reconstructs the user-facing
  display, and `session` is structured resumable persistence. Do not substitute one representation
  for another.
- Interactive conversation rendering flows through `render_ctx` and `disp`; live indicators are
  explicit direct-terminal owners. Use `terminal/ansi.h` for fixed controls and semantic `theme`
  roles for colors. Settle cursor-addressed output with `vt_resolve` before writing it to a pager,
  file, or other non-terminal sink.

Extension workflows:

- Reuse the OpenAI or Anthropic protocol families through presets when wire-compatible. Prefer a
  config-defined provider for a static endpoint variant rather than a new C shim.
- A compiled-in provider needs its source in `meson.build`, a factory declaration in
  `providers/registry.h`, and a `BUILTINS[]` entry at the intended autoselect priority.
- A compiled-in tool needs its source in `meson.build`, an exported `const struct tool` declaration
  in `tool.h`, and an entry in `agent_core.c`'s `TOOLS[]`.
- Keep protocol translation and terminal-independent state machines pure and separately testable;
  do not require HTTP or a TTY to test parsing and state transitions.

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
- Avoid non-portable kernel idioms: no `likely()`/`unlikely()`, `BUG_ON`, `ERR_PTR`, or
  `kmalloc`. Use `<stdint.h>` types and plain negative-int returns plus `errno`.
- Markdown is hard-wrapped around 100 columns, same as code.
- Before changing code in any language, read [`docs/code-style.md`](docs/code-style.md) and apply
  its general naming, structure, and comment principles using that language's conventions.

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

Dependencies are declared in `meson.build`. Keep the footprint small; before adding one, read
[`docs/philosophy.md`](docs/philosophy.md#small-dependency-footprint). Every new dependency must be
in Debian main and either ship with macOS or be available via a single `brew install`. Do not add
GPL libraries.
