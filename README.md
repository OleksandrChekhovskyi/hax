# hax

A minimalist coding agent in C. Multi-provider from day one; ships with adapters for Codex /
ChatGPT and a family of presets over the OpenAI Chat Completions API (real OpenAI, generic
"OpenAI-compatible" endpoints, llama.cpp llama-server, ollama, OpenRouter).

## Build

Requires `libcurl`, `jansson`, and `meson`.

```sh
# Linux (Debian/Ubuntu)
sudo apt install libcurl4-openssl-dev libjansson-dev meson ninja-build pkg-config

# macOS (Homebrew)
brew install jansson meson ninja pkg-config
# libcurl ships with macOS

meson setup build
meson compile -C build
```

## Run

With no arguments, hax launches an interactive REPL.

```sh
./build/hax                   # interactive
./build/hax -p "list TODOs"   # non-interactive: run, print final answer, exit
echo "explain x" | ./build/hax -p   # prompt from stdin
./build/hax -c                # resume the most recent conversation here
./build/hax --resume          # pick a past conversation from a list
./build/hax --resume=ID -p "and now?"   # resume a specific session, non-interactively
./build/hax --raw -p "hello"  # bare model, no system prompt, no tools
./build/hax --help            # full usage
```

| Flag         | What it does                                                      |
|--------------|-------------------------------------------------------------------|
| `-p`         | Non-interactive mode. Runs the prompt to completion (tools and    |
|              | all) and prints the final assistant message to stdout.            |
| `-c`         | Resume the most recent conversation in the current directory.     |
| `--resume`   | Resume a past conversation in this directory. Bare opens an       |
| `[=ID]`      | interactive picker; `--resume=ID` (a session id) resumes directly |
|              | — the ID form also works with `-p`. `/resume` does the same in    |
|              | the REPL.                                                         |
| `--raw`      | Send only the prompt — no system prompt, no env block, no         |
|              | AGENTS.md, no skills, no tools. A barebones chat interface.       |
| `-h`         | Show usage.                                                       |

Every conversation (interactive or `-p`) is recorded to an append-only JSONL
file under `$XDG_STATE_HOME/hax/sessions/<encoded-cwd>/`, keyed by the working
directory, so `-c` / `--resume` only ever offer conversations from the project
you're in. Resuming (including `/resume`) appends to the chosen session's own
file; only `/new` starts a fresh one. On exit hax prints the session id (on
stderr in `-p` mode, so piped stdout stays clean) so you can `--resume=<id>` it
later. Set `HAX_NO_SESSION=1` to disable recording.

Pick a provider with `HAX_PROVIDER` (default `codex`). Supported values:

| `HAX_PROVIDER`      | Backend                                               |
|---------------------|-------------------------------------------------------|
| `codex` (default)   | Codex Responses via ChatGPT + context auto-detect     |
| `openai`            | OpenAI Chat Completions at api.openai.com             |
| `openai-compatible` | Any OpenAI-compatible endpoint (bring your own URL)   |
| `llama.cpp`         | Local `llama-server` with model + context auto-detect |
| `ollama`            | Local `ollama` with context auto-detect               |
| `openrouter`        | OpenRouter with attribution + context auto-detect     |
| `mock`              | In-process scripted/heuristic stub for manual testing |

### Codex (ChatGPT subscription)

Reuses the OAuth token that the official `codex` CLI stores in `~/.codex/auth.json`. If the
token is expired, run `codex` once to refresh it, then re-run `hax`. At startup, hax also
probes ChatGPT's `/backend-api/codex/models` catalog in the background to auto-detect the
chosen model's context window for the per-user-turn `%`-of-context display; `HAX_CONTEXT_LIMIT`
still wins when set.

```sh
./build/hax
```

### OpenAI

Locked to `https://api.openai.com/v1`. Reads `HAX_OPENAI_API_KEY` (preferred) or
`OPENAI_API_KEY`.

```sh
HAX_PROVIDER=openai HAX_MODEL=gpt-5.5 ./build/hax
```

`HAX_OPENAI_BASE_URL` is ignored here — the endpoint is fixed, so a base URL left set for
another backend can't redirect it (or block selecting `openai` in `/provider`). For a custom
endpoint, use `openai-compatible`, which keeps `OPENAI_API_KEY` and `prompt_cache_key` scoped
to real OpenAI so they don't leak to a third-party server.

### OpenAI-compatible (vLLM, LM Studio, oMLX, custom proxies)

Generic preset for any backend that speaks `/v1/chat/completions`. `HAX_OPENAI_BASE_URL` is
required; no implicit `OPENAI_API_KEY` fallback.

```sh
HAX_PROVIDER=openai-compatible \
HAX_PROVIDER_NAME=oMLX \
HAX_OPENAI_BASE_URL=http://127.0.0.1:8000/v1 \
HAX_MODEL=Qwen3.6-35B-A3B-8bit \
./build/hax
```

### llama.cpp

Convenience preset for a local `llama-server`. Defaults to `http://127.0.0.1:8080/v1`; set
`HAX_LLAMACPP_PORT` for a different port, or `HAX_OPENAI_BASE_URL` to override the URL
entirely. Auto-discovers the served model (`/v1/models`) and context window (`/props`), so the
model is filled for you and the per-user-turn `%`-of-context display lights up without manual
configuration:

```sh
HAX_PROVIDER=llama.cpp HAX_LLAMACPP_PORT=9090 ./build/hax
```

The served model is treated as live server state, not a stored preference: at startup hax
reconciles against `/v1/models`, keeping a configured/remembered model only if the server is
actually serving it and otherwise adopting what it serves. This keeps the banner accurate
across server restarts with a different model, and works whether `llama-server` runs in
single-model mode or its experimental router mode (launched with no model, serving several on
demand). When only one model is served, `/model` skips the picker and just uses it; in router
mode it lists the several to choose from.

If the server is unreachable and no model is configured, hax fails fast with the URL it tried
and the override knobs; an explicitly set `HAX_MODEL` is trusted in that case so the real
connection error surfaces at the first prompt instead. The `/props` probe runs in the
background, so a slow or missing endpoint never delays the first prompt — failure just leaves
the context-percent display hidden. If llama-server is started with `--api-key`, set
`HAX_OPENAI_API_KEY` to the matching token — it's forwarded to the discovery probes too, not
just the streaming request.

### ollama

Built-in config for a local `ollama` daemon — the shipped example of a
[custom provider](#custom-providers). Defaults to `http://127.0.0.1:11434/v1`; for a
different port or a remote host, set its `base_url` in `config.json` (see below). `HAX_MODEL`
is required — ollama's chat endpoint rejects requests without one, and no signal it exposes
reliably picks the right model (`/api/ps` decays after `OLLAMA_KEEP_ALIVE`, `/v1/models` is
just the pulled-model catalog), so hax asks once rather than guess.

```sh
HAX_PROVIDER=ollama HAX_MODEL=qwen3:8b ./build/hax
```

For a non-default port, add to `config.json`:

```json
{ "providers": { "ollama": { "base_url": "http://127.0.0.1:11500/v1" } } }
```

Run `ollama list` to see what's installed locally. Unlike llama.cpp, the `%`-of-context
display is **not** auto-detected for ollama: the only context value it exposes before a model
is loaded (`/api/show`) is the model's *training* maximum, not the runtime window — that's
`OLLAMA_CONTEXT_LENGTH` (default 4096), readable only from `/api/ps` once the first request
has loaded the model. Probing the training maximum would badly over-state headroom and mask
truncation, so hax doesn't; set `HAX_CONTEXT_LIMIT` to your `OLLAMA_CONTEXT_LENGTH` if you
want the display.

That default of **4096** is small — hax's system prompt plus tool schemas nearly fill it on
their own, so against a default daemon the first reply often truncates to `length` after a
word or two. ollama's OpenAI endpoint ignores a per-request `num_ctx`, so the only fix is a
larger server-side context: restart `ollama serve` with `OLLAMA_CONTEXT_LENGTH=16384` (or
raise `num_ctx` on the model). hax says as much in the error when it hits this.

### OpenRouter

Reads `HAX_OPENAI_API_KEY` (preferred) or `OPENROUTER_API_KEY`. Probes
`/api/v1/models/{model}/endpoints` in the background to auto-detect the chosen model's
context window (used for the per-user-turn `%`-of-context display). Always sends `X-Title: hax` for
attribution on OpenRouter's leaderboards (override with `HAX_OPENROUTER_TITLE`); add an
`HTTP-Referer` via `HAX_OPENROUTER_REFERER` if you want one.

```sh
HAX_PROVIDER=openrouter \
HAX_MODEL=anthropic/claude-sonnet-4.6 \
OPENROUTER_API_KEY=... \
./build/hax
```

### Custom providers

Any backend that speaks the OpenAI Chat Completions API can be added as a named provider in
`config.json`, then selected with `/provider`, `HAX_PROVIDER=<name>`, or a stored default —
no rebuild. Each entry is self-contained: it reads only its own block, never the global
`HAX_OPENAI_*` variables, so a key or URL left in your environment can't bleed into it.

```json
{
  "providers": {
    "groq": { "base_url": "https://api.groq.com/openai/v1", "api_key_env": "GROQ_API_KEY" },
    "my-vllm": {
      "base_url": "http://gpu-box:8000/v1",
      "display_name": "vLLM (qwen)",
      "reasoning_format": "nested"
    }
  }
}
```

Recognized keys: `base_url` (required); `display_name`; `api` (the wire dialect — defaults to
`openai-completions`, the only one today); `api_key` (a literal token) or `api_key_env` (the
name of the env var holding it — the API key is the one value read from the environment,
since a secret belongs there rather than in a committed file); `reasoning_format`
(`flat`/`nested`); and `send_cache_key`. Models are discovered live from `/v1/models`, so
there's no model list to maintain — `/model` lists whatever the endpoint serves. The shipped
`ollama` provider is one of these with built-in defaults; override any field by adding a
`providers.ollama` block.

A provider name doubles as its config-key path, so it can't contain a `.` (the key separator)
— use `-`. A name that collides with a built-in (`openai`, `llama.cpp`, …) is ignored in
favor of the built-in.

### Mock (manual testing without an LLM)

In-process stub that emits canned events instead of calling out to a real backend. Useful
for visually exercising the dispatch and rendering pipelines, smoke-testing changes, or
producing deterministic repros for bug reports. No network, no API key, no `HAX_MODEL`
required.

Two modes:

- **Scripted** — `HAX_MOCK_SCRIPT=path/to/script.txt` plays one turn of a small line-based
  DSL per `stream()` call. Directives: `text <message>`, `tool <name> <json>`,
  `delay <ms>`, `usage in=N out=M [cached=K]`, `end-turn`. See `scripts/mock_demo.txt` for
  a worked example covering bash / read / write previews and a multi-message continuation.

- **Interactive** — no script, parses the latest user message heuristically: a backtick-
  quoted argument becomes a `bash` (or `read`, when prefixed with the verb) tool call,
  anything else is echoed back. Designed so `HAX_PROVIDER=mock hax` is useful out of the
  box — type ``run `ls -la` `` and watch a real bash dispatch render.

```sh
# scripted
HAX_PROVIDER=mock HAX_MOCK_SCRIPT=scripts/mock_demo.txt ./build/hax

# interactive
HAX_PROVIDER=mock ./build/hax
```

Pair with `scripts/stream_demo.py` (`short`, `long`, `slow`, `burst`, `ansi`, `binary`,
`piped`, `python_buffer`) for realistic streaming patterns through the bash tool.

## Configuration

Every setting can be supplied two ways: an `HAX_*` environment variable, or
`~/.config/hax/config.json` (the same directory as `AGENTS.md`). Runtime picks made from the
REPL (`/provider`, `/model`, `/effort` — see below) persist to a third, machine-local file,
`~/.local/state/hax/state.json`, so the committable `config.json` can live in a dotfiles repo
while your last-used selection stays separate. Precedence is **environment > selection state
(`state.json`) > config file > built-in default**, so a quick `HAX_MODEL=… hax …` still wins
over both, and everything but the env var is optional — with no files, hax behaves exactly as
it always has.

Each `HAX_FOO_BAR` variable maps to a canonical config key, lowercased and grouped by
provider/area: `HAX_MODEL` → `model`, `HAX_OPENAI_BASE_URL` → `openai.base_url`,
`HAX_LLAMACPP_PORT` → `llamacpp.port`, `HAX_BASH_TIMEOUT` → `bash.timeout`. In the file those
are nested objects (a flat `"openai.base_url"` key is accepted too), and scalar values may be
written bare or quoted (`5000` ≡ `"5000"`). On/off settings accept `1`/`true`/`yes`/`on` as
truthy and `0`/`false`/`no`/`off` as falsy (case-insensitive); anything else is invalid and
reads as the setting's default — a typo never silently flips a switch:

```json
{
  "provider": "openai-compatible",
  "provider_name": "vLLM",
  "model": "Qwen3-30B",
  "reasoning_effort": "medium",
  "openai": { "base_url": "http://127.0.0.1:8000/v1" }
}
```

Provider, model, and reasoning effort can also be changed mid-session from the REPL, without
restarting: `/provider` lists every selectable backend (unconfigured or unreachable ones
shown dimmed, with the reason) and, once you pick one, walks you through `/model` then
`/effort`; `/model` and `/effort` enter that chain partway. Models are auto-discovered from
the provider where it exposes a catalog, and the effort levels are the ones that provider
actually accepts. Each pick takes effect on the next turn and is written to `state.json`, so
it sticks across runs (while an explicit `HAX_*` env var still wins for a one-off
invocation). Picking a model or effort also pins the current provider, so a setup built on an
auto-selected provider (see below) sticks as a whole rather than reverting next launch.

Because of this, a provider that can't start no longer aborts the interactive REPL. If you
explicitly chose it (codex not logged in, a missing API key, a local server that's down), the
REPL opens on a `no provider — use /provider` banner with the reason above it. If nothing is
configured, hax instead auto-selects the first available backend on startup, in priority order
— the compiled-in factories first: the built-in default (codex), then a configured generic
`openai-compatible` endpoint, then the local `llama.cpp` server, then the cloud-key backends
(`openai`, `openrouter`); then any config-defined providers (including the `ollama` recipe),
which are appended after the built-ins — and only falls back to the `no provider` banner when
none is available (the startup banner shows which was picked). That
ordering is also why a configured `HAX_OPENAI_BASE_URL` auto-starts `openai-compatible` rather
than `llama.cpp`/`ollama`, which honor the same override but rank lower. The one-shot `-p` path
still fails fast on a provider that can't start, since it can't prompt for an alternative.

The variables below are the full list of settings; each maps to a config key by the rule
above.

### Provider & model

- `HAX_PROVIDER` — one of `codex` (default), `openai`, `openai-compatible`, `llama.cpp`,
  `ollama`, `openrouter`, `mock`
- `HAX_MODEL` — model id. With `codex`, defaults to `model` from `~/.codex/config.toml`,
  falling back to `gpt-5.3-codex`. With `llama.cpp`, auto-filled from `/v1/models` when
  unset. With `mock`, defaults to a placeholder so the provider works without
  configuration. With every other provider (including `ollama`), required (hax exits with
  an error if it's unset)
- `HAX_MOCK_SCRIPT` — path to a mock-script file (only honored when `HAX_PROVIDER=mock`).
  Without it, the mock provider falls back to interactive heuristic responses
- `HAX_PROVIDER_NAME` — optional display name; useful with `openai-compatible` to label the
  banner ("oMLX", "vLLM", …)
- `HAX_SYSTEM_PROMPT` — override the built-in system prompt. Set to an empty string to send
  no system message at all (some OpenAI-compatible chat templates reject system messages)
- `HAX_REASONING_EFFORT` — optional. Passed verbatim to the provider: `reasoning.effort` for
  the Codex Responses API, `reasoning_effort` for Chat Completions. Typical values are
  `minimal`, `low`, `medium`, `high`, `xhigh` — but hax doesn't validate, so anything the
  model accepts works. With `codex`, defaults to `model_reasoning_effort` from
  `~/.codex/config.toml`; otherwise the field is omitted and the server picks its own
  default. Set it to an empty string to force omission
- `HAX_NO_ENV` — set truthy to skip the `<env>` block (platform, cwd, available tools,
  preferred commands) normally appended to the system prompt
- `HAX_NO_AGENTS_MD` — set truthy to skip injecting AGENTS.md project instructions
  (global, project, and parent-directory files) into the system prompt

### OpenAI-family auth and routing (openai, openai-compatible, llama.cpp, openrouter)

These globals configure the compiled-in OpenAI-family presets. They do **not** apply to
custom providers (including `ollama`), which read only their own `config.json` block — see
[Custom providers](#custom-providers).

- `HAX_OPENAI_BASE_URL` — required for `openai-compatible`; overrides the default for
  `llama.cpp`; ignored by `openai` and `openrouter` (both fixed to their real hosts so their
  default API-key fallbacks can't leak to an unrelated endpoint, and a value left set for
  another backend can't redirect or disable them)
- `HAX_OPENAI_API_KEY` — preferred Bearer token across all OpenAI-family presets. Each
  preset also picks up its conventional global as a fallback: `OPENAI_API_KEY` for `openai`,
  `OPENROUTER_API_KEY` for `openrouter`. `openai-compatible` deliberately has no global
  fallback so a configured OpenAI key isn't forwarded to an unrelated endpoint
- `HAX_OPENAI_SEND_CACHE_KEY` — set truthy to send a stable per-session `prompt_cache_key`,
  falsy (`0`, `false`, `no`, `off`) to suppress it. On by default for `openai` and
  `openrouter` (both honor prefix caching); off by default for `openai-compatible` and
  `llama.cpp` because some local servers (notably vLLM) reject unknown JSON fields. The
  switch lets hosted compat backends like Together, Fireworks, or Groq opt in (or the hosted
  presets opt out)
- `HAX_OPENAI_REASONING_FORMAT` — only honored by `openai-compatible`: how reasoning effort
  is encoded in the request, `flat` (`reasoning_effort`, the default) or `nested`
  (`reasoning: {"effort": ...}`, used by some routers)
- `HAX_REASONING_ROUNDTRIP` — replay plain reasoning text back to the model on later turns:
  `off`/`0` disables, `on`/`1` sends it as `reasoning_content`, any other value names the
  field to use. Defaults per preset (off for most; on where the backend expects it)

### llama.cpp preset

- `HAX_LLAMACPP_PORT` — optional. Port for the local `llama-server` (defaults to `8080`).
  Used only when `HAX_OPENAI_BASE_URL` is unset; the URL becomes
  `http://127.0.0.1:<port>/v1`

### ollama

A custom provider with built-in defaults, configured in `config.json` rather than by env
vars — there is no `HAX_OLLAMA_*`. Defaults to `http://127.0.0.1:11434/v1`; set a
`providers.ollama.base_url` entry for a different port or host. See
[Custom providers](#custom-providers).

### OpenRouter preset

- `OPENROUTER_API_KEY` — fallback API key when `HAX_OPENAI_API_KEY` is unset
- `HAX_OPENROUTER_TITLE` — `X-Title` header value (defaults to `hax`); shown on OpenRouter's
  attribution dashboards
- `HAX_OPENROUTER_REFERER` — optional `HTTP-Referer` header; omitted when unset

### Runtime limits

- `HAX_HTTP_IDLE_TIMEOUT` — optional. App-layer silence on a streaming response before
  libcurl gives up with `Timeout was reached`. Accepts plain seconds or a `ms`/`s`/`m`/`h`
  suffix (e.g. `10m`, `2h`). Default `600`; set to `0` to disable. Bump or disable when
  running against a local server (e.g. `llama-server`) whose prompt evaluation can take
  longer than the default and that doesn't send SSE heartbeats
- `HAX_HTTP_MAX_RETRIES` — optional. Additional retries after a transient HTTP failure
  (timeouts, 429s, 5xx). Default `4`; set to `0` to fail on the first error
- `HAX_HTTP_RETRY_BASE` — optional. Base delay before the first retry; doubles per attempt
  with jitter, capped at 30s. Same suffix syntax as above. Default `1s`
- `HAX_TOOL_OUTPUT_CAP` — optional. Max bytes of a tool's output sent back to the model;
  longer output is truncated with a hint pointing at the full capture on disk. Accepts a
  `k`/`m` suffix (1024-base). Default `50k`
- `HAX_BASH_TIMEOUT` — optional. How long the `bash` tool will wait for a command to complete
  before sending SIGTERM to the whole process group and reporting `[timed out after Ns]` to
  the model. Accepts plain seconds or a `ms`/`s`/`m`/`h` suffix. Default `120` (2 min); set
  to `0` to disable. The model can also pass `timeout_seconds` per call to override this
  for slow commands (test suites, builds)
- `HAX_BASH_TIMEOUT_MAX` — optional. Hard ceiling on the per-call `timeout_seconds` override
  the model can request. Same suffix syntax as above. Default `1800` (30 min); set to `0` to
  disable the ceiling. Without a cap, a confused model can request "1 day" and the safety
  net erodes
- `HAX_BASH_TIMEOUT_GRACE` — optional. Window after the SIGTERM during which the command can
  wind down cleanly (flush output, drop locks, remove temp files) before SIGKILL escalates.
  Same suffix syntax as above. Default `2` (seconds); set to `0` to skip the grace window
  and SIGKILL immediately
- `HAX_KEEP_AWAKE` — optional. Keep the machine from going to sleep while a turn is running,
  so a long unattended run isn't cut short by the idle timer. Inhibits idle *system* sleep
  only — the display is still free to blank. On by default; set falsy (`0`, `false`, `no`,
  `off`) to disable. Best-effort: a no-op where the platform helper is missing (`caffeinate`
  on macOS, `systemd-inhibit` on Linux) or on other platforms

### Display & observability

- `HAX_MARKDOWN` — set falsy (`0`, `false`, `no`, `off`) to disable Markdown rendering and
  print the model's text raw. Default on when stdout is a TTY (and always off when it isn't)
- `HAX_SHOW_REASONING` — set truthy to print reasoning/CoT deltas live as they stream
  (default off). With `openrouter` it also opts the request into returning reasoning
- `HAX_DISPLAY_WIDTH` — optional. Force the content width in columns instead of the detected
  terminal width; mainly for tests and fixtures that need a stable layout
- `HAX_NOTIFY` — optional. Desktop-notification method for "model is done" while the
  terminal is unfocused: `osc9`, `bel`, or a falsy value to disable. Auto-detected by
  default (OSC 9 on terminals known to support it, BEL inside tmux and elsewhere)
- `HAX_CONTEXT_LIMIT` — optional manual override for the model's context window, used to
  show a percentage on the per-user-turn usage line. Accepts a plain number or a `k`/`m` suffix
  (1024-base): `256k`, `128K`, `1m`, `262144`. Auto-detected for `codex` (from the catalog's
  `context_window`), `llama.cpp` (from `/props`), `ollama` (from `/api/show`'s
  `model_info["<arch>.context_length"]`), and `openrouter` (from the per-model
  `endpoints[].context_length`); auto-detection runs in the background at startup and lights up the
  display once the response lands. The env var, when set, wins over auto-detection. When
  unset and the auto-probe didn't fire or returned nothing, hax shows the absolute counts
  only
- `HAX_TRACE` — path to a Markdown file that will receive a pretty-printed dump of every
  HTTP request, response status, and SSE event (Authorization redacted). Each entry's
  header carries a `t+S.MMMs dt+S.MMMs` tag (wall-clock since the first trace entry, plus
  delta from the prior one) so pauses between SSE chunks are visible without inferring
  from per-provider `created` fields. Plain text, truncated on startup so each run begins
  fresh; most readable when opened in an editor that renders Markdown
- `HAX_TRANSCRIPT` — path to a file that mirrors the Ctrl-T transcript view (system prompt,
  advertised tools, and every turn's items including tool calls + results). Plain text,
  truncated on startup and on `/new`, then appended to as the conversation grows. Useful
  for debugging higher-level behavior than `HAX_TRACE` shows, and works with
  `HAX_PROVIDER=mock` where there are no HTTP calls to trace
- `HAX_NO_SESSION` — set truthy to disable session recording, so `-c` / `--resume` /
  `/resume` have nothing to persist or restore. Unset by default: every conversation is
  written to `$XDG_STATE_HOME/hax/sessions/`

## License

MIT.
