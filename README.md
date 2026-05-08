# hax

A minimalist coding agent in C. Multi-provider from day one; ships with adapters for Codex /
ChatGPT and a family of presets over the OpenAI Chat Completions API (real OpenAI, generic
"OpenAI-compatible" endpoints, llama.cpp llama-server, OpenRouter).

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
./build/hax --raw -p "hello"  # bare model, no system prompt, no tools
./build/hax --help            # full usage
```

| Flag       | What it does                                                        |
|------------|---------------------------------------------------------------------|
| `-p`       | Non-interactive mode. Runs the prompt to completion (tools and all) |
|            | and prints the final assistant message to stdout.                   |
| `--raw`    | Send only the prompt — no system prompt, no env block, no AGENTS.md,|
|            | no skills, no tools. Useful as a barebones chat interface.          |
| `-h`       | Show usage.                                                         |

Pick a provider with `HAX_PROVIDER` (default `codex`). Supported values:

| `HAX_PROVIDER`      | Backend                                               |
|---------------------|-------------------------------------------------------|
| `codex` (default)   | Codex Responses via ChatGPT + context auto-detect     |
| `openai`            | OpenAI Chat Completions at api.openai.com             |
| `openai-compatible` | Any OpenAI-compatible endpoint (bring your own URL)   |
| `llama.cpp`         | Local `llama-server` with model + context auto-detect |
| `openrouter`        | OpenRouter with attribution + context auto-detect     |
| `mock`              | In-process scripted/heuristic stub for manual testing |

### Codex (ChatGPT subscription)

Reuses the OAuth token that the official `codex` CLI stores in `~/.codex/auth.json`. If the
token is expired, run `codex` once to refresh it, then re-run `hax`. At startup, hax also
probes ChatGPT's `/backend-api/codex/models` catalog in the background to auto-detect the
chosen model's context window for the per-turn `%`-of-context display; `HAX_CONTEXT_LIMIT`
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

`HAX_OPENAI_BASE_URL` is rejected here on purpose — for custom endpoints, use
`openai-compatible` (which keeps `OPENAI_API_KEY` and `prompt_cache_key` scoped to real
OpenAI so they don't leak to a third-party server).

### OpenAI-compatible (vLLM, Ollama, LM Studio, oMLX, custom proxies)

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
entirely. Auto-discovers the loaded model (`/v1/models`) and context window (`/props`), so
`HAX_MODEL` is filled for you when unset and the per-turn `%`-of-context display lights up
without manual configuration:

```sh
HAX_PROVIDER=llama.cpp HAX_LLAMACPP_PORT=9090 ./build/hax
```

If the server is unreachable and `HAX_MODEL` is unset, hax fails fast with the URL it tried
and the override knobs. If `HAX_MODEL` is set, the model probe is skipped entirely. The
`/props` probe runs in the background, so a slow or missing endpoint never delays the first
prompt — failure just leaves the context-percent display hidden. If llama-server is started
with `--api-key`, set `HAX_OPENAI_API_KEY` to the matching token — it's forwarded to the
discovery probes too, not just the streaming request.

### OpenRouter

Reads `HAX_OPENAI_API_KEY` (preferred) or `OPENROUTER_API_KEY`. Probes
`/api/v1/models/{model}/endpoints` in the background to auto-detect the chosen model's
context window (used for the per-turn `%`-of-context display). Always sends `X-Title: hax` for
attribution on OpenRouter's leaderboards (override with `HAX_OPENROUTER_TITLE`); add an
`HTTP-Referer` via `HAX_OPENROUTER_REFERER` if you want one.

```sh
HAX_PROVIDER=openrouter \
HAX_MODEL=anthropic/claude-sonnet-4.6 \
OPENROUTER_API_KEY=... \
./build/hax
```

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

Pair with `scripts/stream_demo.py` (`fake-ninja` / `fake-meson` / `fake-vitest`) for
realistic streaming patterns through the bash tool.

## Environment variables

### Provider & model

- `HAX_PROVIDER` — one of `codex` (default), `openai`, `openai-compatible`, `llama.cpp`,
  `openrouter`, `mock`
- `HAX_MODEL` — model id. With `codex`, defaults to `model` from `~/.codex/config.toml`,
  falling back to `gpt-5.3-codex`. With `llama.cpp`, auto-filled from `/v1/models` when
  unset. With `mock`, defaults to a placeholder so the provider works without
  configuration. With every other provider, required (hax exits with an error if it's unset)
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

### OpenAI-family auth and routing (openai, openai-compatible, llama.cpp, openrouter)

- `HAX_OPENAI_BASE_URL` — required for `openai-compatible`; overrides the default for
  `llama.cpp`; rejected by `openai` and `openrouter` (both locked to their real hosts so
  their default API-key fallbacks can't leak to an unrelated endpoint)
- `HAX_OPENAI_API_KEY` — preferred Bearer token across all OpenAI-family presets. Each
  preset also picks up its conventional global as a fallback: `OPENAI_API_KEY` for `openai`,
  `OPENROUTER_API_KEY` for `openrouter`. `openai-compatible` deliberately has no global
  fallback so a configured OpenAI key isn't forwarded to an unrelated endpoint
- `HAX_OPENAI_SEND_CACHE_KEY` — set to any non-empty value to send a stable per-session
  `prompt_cache_key`. On by default for `openai` and `openrouter` (both honor prefix
  caching); off by default for `openai-compatible` and `llama.cpp` because some local
  servers (notably vLLM) reject unknown JSON fields. This switch lets hosted compat
  backends like Together, Fireworks, or Groq opt in

### llama.cpp preset

- `HAX_LLAMACPP_PORT` — optional. Port for the local `llama-server` (defaults to `8080`).
  Used only when `HAX_OPENAI_BASE_URL` is unset; the URL becomes
  `http://127.0.0.1:<port>/v1`

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

### Display & observability

- `HAX_CONTEXT_LIMIT` — optional manual override for the model's context window, used to
  show a percentage on the per-turn usage line. Accepts a plain number or a `k`/`m` suffix
  (1024-base): `256k`, `128K`, `1m`, `262144`. Auto-detected for `codex` (from the catalog's
  `context_window`), `llama.cpp` (from `/props`), and `openrouter` (from the per-model
  `endpoints[].context_length`); auto-detection runs in the background at startup and lights up the
  display once the response lands. The env var, when set, wins over auto-detection. When
  unset and the auto-probe didn't fire or returned nothing, hax shows the absolute counts
  only
- `HAX_TRACE` — path to a Markdown file that will receive a pretty-printed dump of every
  HTTP request, response status, and SSE event (Authorization redacted). Plain text,
  truncated on startup so each run begins fresh; most readable when opened in an editor
  that renders Markdown
- `HAX_TRANSCRIPT` — path to a file that mirrors the Ctrl-T transcript view (system prompt,
  advertised tools, and every turn's items including tool calls + results). Plain text,
  truncated on startup and on `/new`, then appended to as the conversation grows. Useful
  for debugging higher-level behavior than `HAX_TRACE` shows, and works with
  `HAX_PROVIDER=mock` where there are no HTTP calls to trace

## License

MIT.
