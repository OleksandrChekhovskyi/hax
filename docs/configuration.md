# Configuration

Hax has one registry for user-facing settings. Each setting has a canonical config key and,
when applicable, an environment variable.

## Files and precedence

Paths use XDG defaults:

| Purpose | Path |
| --- | --- |
| Config file | `${XDG_CONFIG_HOME:-$HOME/.config}/hax/config.json` |
| Runtime selection state | `${XDG_STATE_HOME:-$HOME/.local/state}/hax/state.json` |
| Sessions | `${XDG_STATE_HOME:-$HOME/.local/state}/hax/sessions/` |

Resolution order:

```text
runtime override → environment → state.json → config.json → registry/provider default
```

`/provider`, `/model`, and `/effort` create runtime overrides for the current process and
persist them to `state.json`. On the next launch, an explicit environment variable still wins
over that state.

## Config file format

`config.json` is plain JSON: no comments or trailing commas. Dotted keys are written as nested
objects, though flat dotted keys are also accepted.

```json
{
  "provider": "openai-compatible",
  "provider_name": "vLLM",
  "model": "Qwen3-30B",
  "reasoning_effort": "medium",
  "openai": {
    "base_url": "http://127.0.0.1:8000/v1"
  }
}
```

Scalar values may be strings or JSON numbers/booleans; hax normalizes scalar leaves to
strings internally.

Boolean settings accept `1`/`true`/`yes`/`on` and `0`/`false`/`no`/`off`, case-insensitive.
Invalid typed values fall back to the setting default rather than silently changing behavior.

Duration settings accept bare seconds or a suffix: `ms`, `s`, `m`, `h`. Size settings accept a
plain number or `k`/`m` suffixes using 1024-base units.

## Common examples

OpenAI-compatible local server:

```json
{
  "provider": "openai-compatible",
  "model": "Qwen3-30B",
  "provider_name": "vLLM",
  "openai": { "base_url": "http://127.0.0.1:8000/v1" }
}
```

Ollama on a non-default port:

```json
{
  "provider": "ollama",
  "model": "qwen3:8b",
  "providers": {
    "ollama": { "base_url": "http://127.0.0.1:11500/v1" }
  }
}
```

Custom provider with a key in the environment:

```json
{
  "providers": {
    "groq": {
      "base_url": "https://api.groq.com/openai/v1",
      "api_key_env": "GROQ_API_KEY"
    }
  }
}
```

## Setting reference

The list below mirrors the setting registry in `src/config.c`. Each bullet starts with the
canonical key and its environment variable.

### Selection and prompt

- `provider` / `HAX_PROVIDER` — backend id. Built-ins are `codex`, `openai`,
  `openai-compatible`, `anthropic`, `anthropic-compatible`, `llama.cpp`, `ollama`,
  `openrouter`, and `mock`. When unset, startup auto-selects: the built-in default (`codex`)
  first, then the first available provider in priority order.
- `model` / `HAX_MODEL` — model id. Required when the provider has no default and cannot
  auto-fill it.
- `reasoning_effort` / `HAX_REASONING_EFFORT` — provider-specific reasoning effort. Empty
  string forces omission.
- `system_prompt` / `HAX_SYSTEM_PROMPT` — override the base system prompt. Empty string sends
  no system message.
- `provider_name` / `HAX_PROVIDER_NAME` — override the banner display name for compiled-in
  providers.
- `no_env` / `HAX_NO_ENV` — skip the `<env>` block.
- `no_agents_md` / `HAX_NO_AGENTS_MD` — skip AGENTS.md and skill discovery.

### Display

- `markdown` / `HAX_MARKDOWN` — render Markdown on TTY output; piped output is raw. Default
  `1`.
- `show_reasoning` / `HAX_SHOW_REASONING` — show reasoning deltas live. For OpenRouter, also
  requests reasoning output.
- `context_limit` / `HAX_CONTEXT_LIMIT` — manual context-window size for percentage display
  and auto-compaction.
- `display_width` / `HAX_DISPLAY_WIDTH` — force render width in columns.
- `notify` / `HAX_NOTIFY` — desktop notification style: `osc9`, `bel`, or falsy to disable.

`HAX_CONTEXT_LIMIT` overrides provider auto-detection. Current auto-detection exists for
Codex, llama.cpp, and OpenRouter. Other providers show absolute token counts unless a manual
limit is set.

### Behavior

- `keep_awake` / `HAX_KEEP_AWAKE` — best-effort idle sleep inhibition while a turn is
  running. Default `1`.
- `compact.auto` / `HAX_COMPACT_AUTO` — auto-summarize history near the context window.
  Default `1`.
- `compact.threshold` / `HAX_COMPACT_THRESHOLD` — auto-compaction trigger percentage. Default
  `85`.

### Recording and observability

- `no_session` / `HAX_NO_SESSION` — disable session recording and resume.
- `transcript` / `HAX_TRANSCRIPT` — file path for a plain-text transcript mirror.
- `trace` / `HAX_TRACE` — file path for HTTP/SSE trace output.

See [debugging.md](./debugging.md) for trace and transcript details.

### Tools

- `tool_output_cap` / `HAX_TOOL_OUTPUT_CAP` — max bytes of tool output sent back to the
  model. Default `50k`.
- `bash.timeout` / `HAX_BASH_TIMEOUT` — default bash-tool timeout; `0` disables. Default `2m`.
- `bash.timeout_max` / `HAX_BASH_TIMEOUT_MAX` — maximum timeout the model may request per
  bash call; `0` disables the cap. Default `30m`.
- `bash.timeout_grace` / `HAX_BASH_TIMEOUT_GRACE` — grace period between SIGTERM and SIGKILL;
  `0` skips the grace period. Default `2s`.
- `bash.shell` / `HAX_BASH_SHELL` — shell the bash tool execs, as a `$PATH` name or a path.
  Default: `bash` when available, otherwise `sh`.

The model can request `timeout_seconds` on a bash call, bounded by `bash.timeout_max`.

### HTTP transport

- `http.max_retries` / `HAX_HTTP_MAX_RETRIES` — additional retries for transient HTTP
  failures. Default `4`.
- `http.retry_base` / `HAX_HTTP_RETRY_BASE` — initial retry backoff; later retries double with
  jitter. Default `1s`.
- `http.idle_timeout` / `HAX_HTTP_IDLE_TIMEOUT` — streaming silence before giving up; `0`
  disables. Default `10m`.

### OpenAI-family providers

These settings apply to built-in OpenAI-family presets: `openai`, `openai-compatible`,
`llama.cpp`, and `openrouter`. They do not apply to config-defined custom providers such as
`ollama`, which read their own `providers.<name>` block.

- `openai.base_url` / `HAX_OPENAI_BASE_URL` — required for `openai-compatible`; overrides
  `llama.cpp`; ignored by `openai` and `openrouter`.
- `openai.api_key` / `HAX_OPENAI_API_KEY` — bearer token for OpenAI-family presets.
- `openai.reasoning_format` / `HAX_OPENAI_REASONING_FORMAT` — `flat` or `nested`; mainly for
  `openai-compatible`. Default `flat`.
- `openai.reasoning_roundtrip` / `HAX_REASONING_ROUNDTRIP` — replay reasoning text on later
  turns: `off`, `on`, or a field name. Default is provider-specific.
- `openai.send_cache_key` / `HAX_OPENAI_SEND_CACHE_KEY` — send a stable `prompt_cache_key`
  when enabled. Default is provider-specific.

API-key fallbacks:

- `openai`: `OPENAI_API_KEY` after `HAX_OPENAI_API_KEY`.
- `openrouter`: `OPENROUTER_API_KEY` after `HAX_OPENAI_API_KEY`.
- `openai-compatible`: no `OPENAI_API_KEY` fallback.
- `llama.cpp`: uses only `HAX_OPENAI_API_KEY` if its server requires auth.

### Anthropic-family providers

These settings apply to `anthropic` and `anthropic-compatible`. Config-defined Anthropic
providers use the same leaf names under `providers.<name>`.

- `anthropic.base_url` / `HAX_ANTHROPIC_BASE_URL` — required for `anthropic-compatible`;
  ignored by real `anthropic`.
- `anthropic.api_key` / `HAX_ANTHROPIC_API_KEY` — `x-api-key` token for Anthropic-family
  providers.
- `anthropic.max_tokens` / `HAX_ANTHROPIC_MAX_TOKENS` — max output tokens, including thinking
  and text. Default `32000`.
- `anthropic.thinking_mode` / `HAX_ANTHROPIC_THINKING_MODE` — `adaptive`, `budget`, or `off`.
  Default is provider-specific.
- `anthropic.thinking_budget` / `HAX_ANTHROPIC_THINKING_BUDGET` — budget-mode thinking tokens.
  Default is `max_tokens - 1`.
- `anthropic.cache` / `HAX_ANTHROPIC_CACHE` — send prompt cache-control breakpoints. Default
  is provider-specific.
- `anthropic.cache_ttl` / `HAX_ANTHROPIC_CACHE_TTL` — cache TTL. Default `1h`; accepted
  values are provider/API dependent (`5m` or `1h`).
- `anthropic.version` / `HAX_ANTHROPIC_VERSION` — `anthropic-version` request header. Default
  `2023-06-01`.

API-key fallbacks:

- `anthropic`: `ANTHROPIC_API_KEY` after `HAX_ANTHROPIC_API_KEY`.
- `anthropic-compatible`: no `ANTHROPIC_API_KEY` fallback.

### Per-provider settings

- `llamacpp.port` / `HAX_LLAMACPP_PORT` — port for local `llama-server` when
  `openai.base_url` is unset. Default `8080`.
- `openrouter.title` / `HAX_OPENROUTER_TITLE` — `X-Title` attribution header for OpenRouter.
  Default `hax`.
- `openrouter.referer` / `HAX_OPENROUTER_REFERER` — optional `HTTP-Referer` header for
  OpenRouter.
- `mock.script` / `HAX_MOCK_SCRIPT` — mock-provider script path.

## Custom provider blocks

Custom provider blocks live under `providers` in `config.json`; see
[providers.md](providers.md#custom-providers). Provider blocks intentionally do not read the
global OpenAI/Anthropic environment variables. Use `api_key_env` inside the block to name the
secret variable for that provider.
