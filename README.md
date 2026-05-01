# hax

A minimalist coding agent in C. Multi-provider from day one; currently ships
with adapters for Codex / ChatGPT and any OpenAI-compatible Chat Completions
endpoint.

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

Pick a provider with `HAX_PROVIDER` (default `codex`).

### Codex (ChatGPT subscription)

Reuses the OAuth token that the official `codex` CLI stores in
`~/.codex/auth.json`. If the token is expired, run `codex` once to refresh it,
then re-run `hax`.

```sh
./build/hax
```

### OpenAI-compatible (real OpenAI, local servers, proxies)

Works with anything that speaks `/v1/chat/completions` — the OpenAI API itself,
or local backends like oMLX, vLLM, llama.cpp, Ollama, etc.

Real OpenAI (base URL and key default to `https://api.openai.com/v1` and
`$OPENAI_API_KEY`):

```sh
HAX_PROVIDER=openai HAX_MODEL=gpt-5.4 ./build/hax
```

Local server:

```sh
HAX_PROVIDER=openai \
HAX_PROVIDER_NAME=oMLX \
HAX_OPENAI_BASE_URL=http://127.0.0.1:8000/v1 \
HAX_OPENAI_API_KEY=... \
HAX_MODEL=Qwen3.6-35B-A3B-8bit \
./build/hax
```

## Environment variables

### Provider & model

- `HAX_PROVIDER` — `codex` (default) or `openai`
- `HAX_MODEL` — model id. With `codex`, defaults to `model` from `~/.codex/config.toml`,
  falling back to `gpt-5.3-codex`; required when using `openai` (hax exits with an error if it's
  unset)
- `HAX_SYSTEM_PROMPT` — override the built-in system prompt. Set to an empty string to send no
  system message at all (some OpenAI-compatible chat templates reject system messages)
- `HAX_REASONING_EFFORT` — optional. Passed verbatim to the provider: `reasoning.effort` for the
  Codex Responses API, `reasoning_effort` for Chat Completions. Typical values are `minimal`,
  `low`, `medium`, `high`, `xhigh` — but hax doesn't validate, so anything the model accepts
  works. With `codex`, defaults to `model_reasoning_effort` from `~/.codex/config.toml`; otherwise
  the field is omitted and the server picks its own default. Set it to an empty string to force
  omission

### OpenAI provider

- `HAX_OPENAI_BASE_URL` — optional for `openai`; defaults to `https://api.openai.com/v1`; set to
  point at a local or proxy endpoint
- `HAX_OPENAI_API_KEY` — optional for `openai`; sent as `Authorization: Bearer`. Falls back to
  `OPENAI_API_KEY` only when the resolved base URL targets real OpenAI (default or explicit
  `https://api.openai.com/...`), so a globally configured OpenAI key is never forwarded to a
  custom endpoint. May be omitted for local servers that don't require auth
- `HAX_PROVIDER_NAME` — optional display name for the `openai` provider
- `HAX_OPENAI_SEND_CACHE_KEY` — set to any non-empty value to send a stable per-session
  `prompt_cache_key` even when `HAX_OPENAI_BASE_URL` is custom. Useful for hosted
  OpenAI-compatible providers (Together, Fireworks, Groq, OpenRouter, etc.) whose prefix caching
  benefits from an affinity hint. Off by default for non-OpenAI URLs because some local servers
  (notably vLLM) reject unknown JSON fields. Always sent to real `api.openai.com`

### Runtime limits

- `HAX_HTTP_IDLE_TIMEOUT` — optional. App-layer silence on a streaming response before libcurl
  gives up with `Timeout was reached`. Accepts plain seconds or a `ms`/`s`/`m`/`h` suffix (e.g.
  `10m`, `2h`). Default `600`; set to `0` to disable. Bump or disable when running against a
  local server (e.g. `llama-server`) whose prompt evaluation can take longer than the default
  and that doesn't send SSE heartbeats
- `HAX_BASH_TIMEOUT` — optional. How long the `bash` tool will wait for a command to complete
  before sending SIGTERM to the whole process group and reporting `[timed out after Ns]` to the
  model. Accepts plain seconds or a `ms`/`s`/`m`/`h` suffix. Default `120` (2 min); set to `0`
  to disable. The model can also pass `timeout_seconds` per call to override this for slow
  commands (test suites, builds)
- `HAX_BASH_TIMEOUT_MAX` — optional. Hard ceiling on the per-call `timeout_seconds` override
  the model can request. Same suffix syntax as above. Default `1800` (30 min); set to `0` to
  disable the ceiling. Without a cap, a confused model can request "1 day" and the safety net
  erodes
- `HAX_BASH_TIMEOUT_GRACE` — optional. Window after the SIGTERM during which the command can
  wind down cleanly (flush output, drop locks, remove temp files) before SIGKILL escalates.
  Same suffix syntax as above. Default `2` (seconds); set to `0` to skip the grace window and
  SIGKILL immediately

### Display & observability

- `HAX_CONTEXT_LIMIT` — optional. Model's context window, used to show a percentage on the
  per-turn usage line. Accepts a plain number or a `k`/`m` suffix (1024-base): `256k`, `128K`,
  `1m`, `262144`. There's no reliable way to auto-detect this across local OpenAI-compatible
  servers, so it's opt-in. When unset, hax shows the absolute counts only
- `HAX_TRACE` — path to a Markdown file that will receive a pretty-printed dump of every HTTP
  request, response status, and SSE event (Authorization redacted). Opened in append mode;
  `tail -f` works, but the file is most readable when opened in an editor that renders Markdown

## License

MIT.
