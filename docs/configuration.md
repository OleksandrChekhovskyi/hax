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

`/config` lists every setting with its resolved value, source, and description. Bright rows
are editable as session-only overrides; dimmed rows report how to change them. Use
`/config <key> default` to clear an override. Provider, model, effort, and preset changes remain
under their dedicated commands.

API keys are shown only as `set` or `unset`. `/config` reflects hax's `HAX_*` settings, not
provider fallbacks (`OPENAI_API_KEY`, `ANTHROPIC_API_KEY`, `OPENROUTER_API_KEY`), so a
fallback-only key may appear unset even when authentication works.

The CLI selection flags and presets also land in the runtime-override tier, applied in this
order: a preset first, then explicit `--provider` / `--model` / `--effort` flags on top. So
for a single run the effective order is: flags → preset → environment → `state.json` →
`config.json`. The flags and startup presets (`--preset`, `HAX_PRESET`) persist nothing;
the interactive `/preset` persists the preset's *name* to `state.json` like the other
selectors (see [Presets](#presets)).

## Config file format

`config.json` is plain JSON: no comments or trailing commas. Dotted keys are written as nested
objects, though flat dotted keys are also accepted.

```json
{
  "provider": "openai-compatible",
  "provider_name": "vLLM",
  "model": "Qwen3-30B",
  "effort": "medium",
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

## Presets

A preset is a named, complete selection: which provider to talk to, and optionally which
model, reasoning effort, and system prompt. Define them under `presets.<name>` in
`config.json`:

```json
{
  "presets": {
    "review": {
      "description": "thorough code review on a strong model",
      "provider": "codex",
      "model": "gpt-5.6-sol",
      "effort": "high",
      "system_prompt": "You are a meticulous code reviewer. ..."
    },
    "scout": {
      "description": "fast, cheap exploration",
      "provider": "openrouter",
      "model": "qwen/qwen3-coder:free"
    }
  }
}
```

Semantics:

- `provider` is required; it anchors the selection.
- `model` and `effort` are optional. Omitted, the provider's own default applies
  (llama.cpp discovers the served model), shadowing any env/state/config value — the same
  rule a `/provider` switch uses, so a stale `HAX_MODEL` can't leak into a different backend.
  Explicit `--model`/`--effort` flags still win over the preset.
- `system_prompt` is optional. Omitted, normal resolution applies — your configured prompt,
  or the built-in one.
- Applying a preset writes the whole selection, so presets replace each other rather than
  compose: switching from a preset that set a system prompt to one that doesn't restores the
  regular prompt.
- `description` is reserved metadata, shown by the `/preset` picker and listed in the system
  prompt's subagents section so the model knows which preset fits which delegated task.
- A definition must be an object: nested under `presets` as above, or a single flat
  `"presets.<name>": {...}` key. Fully-flat leaf spellings (`"presets.<name>.provider": ...`)
  are not assembled into a preset — the same exception to the flat-key grammar that other
  structured blocks (like `catalog.models`) carry.
- Nothing else is presettable, deliberately: a preset must be fully honored whenever it is
  applied, including mid-session via `/preset`, and only these per-request keys qualify.
  Endpoint and credential settings are bound at provider construction — define a custom
  `providers.<name>` block and point the preset's `provider` at it. Project-context stripping
  and session recording are startup-bound — use the `--bare` / `--no-session` flags.
- Application is all-or-nothing: an invalid member is an error and nothing is applied.

Select a preset with `--preset <name>`, the `HAX_PRESET` env var (or a `preset` config key),
or `/preset` in the REPL. The active preset shows dim in the banner — `hax [review] › …` —
which matters because a preset may have swapped the system prompt; `/session` lists it too,
and Ctrl-T shows the exact prompt in effect.

`/preset` persists like the other selectors, but by *name* (a `preset` key in `state.json`,
machine-local like the rest): the next launch re-applies the then-current definition, so
editing the preset in `config.json` changes what you get. An explicit `/provider`, `/model`,
or `/effort` pick exits the preset — the committed selection replaces the whole stance,
clearing its name and system prompt. `--preset` and `HAX_PRESET` apply per-run and persist
nothing.

Explicit per-run input beats the persisted stance as a whole: when any selection flag or
selection env var (`HAX_PROVIDER`, `HAX_MODEL`, `HAX_EFFORT`, `HAX_SYSTEM_PROMPT`)
is set, a preset coming from `state.json` or a config-file `preset` default is skipped
entirely — presets apply whole or not at all, never blended with other input. A preset named
explicitly for the run (`--preset`, `HAX_PRESET`) still
applies, and still shadows selection env vars per the flags → preset → env order;
`HAX_PRESET=` (empty) disables any preset for the run. A persisted name whose definition has
since been renamed or deleted warns at startup and is skipped; a failing explicit `--preset`
is an error.

Earlier `/provider`/`/model`/`/effort` picks stay in `state.json` underneath a persisted
preset. They are fully shadowed while the preset is active (it writes the whole selection),
and come back into effect whenever the stance is skipped — a one-off `HAX_PRESET=` run, an
explicit selection env var, or a stale name — so disabling a preset falls back to your last
explicit selection, not to auto-selection.

## Setting reference

The list below mirrors `src/config.c`; each bullet starts with the canonical key and environment
variable. `/config` marks settings that can be changed mid-session.

### Selection and prompt

- `preset` / `HAX_PRESET` — preset to apply at startup, as if passed via `--preset`. An
  explicit empty value disables a config-file default.
- `provider` / `HAX_PROVIDER` — backend id. Built-ins are `codex`, `openai`,
  `openai-compatible`, `anthropic`, `anthropic-compatible`, `llama.cpp`, `ollama`,
  `openrouter`, and `mock`. When unset, startup auto-selects: the built-in default (`codex`)
  first, then the first available provider in priority order.
- `model` / `HAX_MODEL` — model id. Required when the provider has no default and cannot
  auto-fill it.
- `effort` / `HAX_EFFORT` — provider-specific reasoning effort. Empty
  string forces omission.
- `system_prompt` / `HAX_SYSTEM_PROMPT` — override the base system prompt. Empty string sends
  no system message.
- `provider_name` / `HAX_PROVIDER_NAME` — override the banner display name for compiled-in
  providers.
- `no_env` / `HAX_NO_ENV` — skip the Environment section.
- `no_agents_md` / `HAX_NO_AGENTS_MD` — skip AGENTS.md discovery.
- `no_skills` / `HAX_NO_SKILLS` — skip the skills listing.
- `no_subagents` / `HAX_NO_SUBAGENTS` — skip the subagents section (see
  [usage.md](./usage.md)).

### Display

- `markdown` / `HAX_MARKDOWN` — render Markdown on TTY output; piped output is raw. Default
  `1`.
- `show_reasoning` / `HAX_SHOW_REASONING` — show reasoning deltas live; changeable
  mid-session. Controls display of reasoning, never whether the model reasons.
- `sort_models` / `HAX_SORT_MODELS` — `on` forces the `/model` picker alphabetical, `off` keeps
  the server's catalog order, for every provider. Default `auto`: each provider picks its own
  default — alphabetical for `openai` and `openrouter`, catalog order elsewhere.
- `context_limit` / `HAX_CONTEXT_LIMIT` — manual context-window size for percentage display
  and auto-compaction.
- `display_width` / `HAX_DISPLAY_WIDTH` — force render width in columns.
- `notify` / `HAX_NOTIFY` — desktop notification style: `auto`, `bel`, `osc9`, or `off`. Default
  `auto` detects from the terminal.
- `theme` / `HAX_THEME` — color theme: `auto`, `dark`, `light`, `ansi`, or `off`. Default
  `auto`. `dark` and `light` are fixed 256-color palettes tuned for the respective
  background; `ansi` uses only the classic 16-color SGRs, so colors follow the terminal's
  own scheme (the right choice for carefully themed terminals); `off` disables colors while
  keeping bold/dim/italic. `auto` picks `off` when `NO_COLOR` is set (or the terminal is
  dumb), `ansi` without 256-color support, and otherwise `dark` — or `light` when a
  `COLORFGBG` environment variable reports a light background. Terminals rarely advertise
  their background, so on a light scheme you typically want to set `light` explicitly.

`HAX_CONTEXT_LIMIT` overrides provider auto-detection. Current auto-detection exists for
Codex, llama.cpp, and OpenRouter; `openai`, `anthropic`, and custom providers with a
catalog identity (`catalog_id`, defaulting to the provider's name — see
[providers.md](./providers.md)) fall back to the model catalog's per-model window (see
below). Other providers show absolute token counts unless a manual limit is set.

### Model catalog

hax uses a per-model metadata catalog — cost rates and window limits — to estimate spend for
providers that don't report per-response cost (the `~$` figure on the stats line) and to fill
in unknown context windows. Metadata resolves from two tiers: a `models` block in the config
file, then a cached snapshot of [models.dev](https://models.dev/api.json).

- `catalog.url` / `HAX_CATALOG_URL` — catalog endpoint, in the models.dev `api.json` shape.
  Empty disables fetching entirely. Default `https://models.dev/api.json`.
- `catalog.refresh` / `HAX_CATALOG_REFRESH` — re-fetch the cached snapshot when older than
  this; `0` disables fetching. Default `24h`.

The snapshot is cached at `$XDG_CACHE_HOME/hax/catalog.json` (`~/.cache/hax/catalog.json`) and
fetched in the background, at most once per run, only when a session actually streams on a
catalog-mapped provider. A stale cache keeps serving; fetch failures are silent — but when the
snapshot hasn't refreshed for over 30 days (endpoint unreachable, response rejected), hax
prints a one-line warning that estimates may be stale. With fetching disabled, an existing
cache file — or a hand-placed one — is still read, and no staleness warning is issued.

The `catalog.models` block declares or overrides per-model metadata, keyed by catalog
provider id then model id, with the models.dev field names (costs are USD per million
tokens):

```json
{
  "catalog": {
    "models": {
      "openai": {
        "gpt-5.3-codex": {
          "cost": {"input": 1.25, "output": 10, "cache_read": 0.125},
          "limit": {"context": 400000, "output": 128000}
        }
      }
    }
  }
}
```

Config fields win field-by-field; the cached catalog fills whatever the block leaves unset.
Estimates use reported token counts only and skip what they can't price, so treat the `~$`
figure as an approximation — cross-check against the provider's own accounting (`/usage`)
where it matters.

### Behavior

- `keep_awake` / `HAX_KEEP_AWAKE` — best-effort idle sleep inhibition while a turn is
  running. Default `1`.
- `compact.auto` / `HAX_COMPACT_AUTO` — auto-summarize history near the context window.
  Default `1`.
- `compact.threshold` / `HAX_COMPACT_THRESHOLD` — auto-compaction trigger percentage, `1`–`100`.
  Default `85`; an out-of-range value falls back to it.
- `max_turns` / `HAX_MAX_TURNS` — interactive only: pause the agent loop for confirmation
  after this many model round-trips within one user turn (Enter resumes). Unset or `0`
  means unlimited.

### Recording and observability

- `no_session` / `HAX_NO_SESSION` — disable session recording and resume.
- `transcript` / `HAX_TRANSCRIPT` — file path for a plain-text transcript mirror; empty disables.
- `trace` / `HAX_TRACE` — file path for HTTP/SSE trace output; empty disables.

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
  failures, up to `100`. Default `4`.
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
- `openai.send_cache_key` / `HAX_OPENAI_SEND_CACHE_KEY` — `auto`, `on`, or `off`: `on`/`off`
  force sending (or suppressing) a stable `prompt_cache_key`, while `auto` uses the provider
  default. Because `auto` is a real value, setting it at a higher tier (env, `/config`)
  shadows a lower-tier `on`/`off` and restores the provider default.
- `openai.request_cost` / `HAX_OPENAI_REQUEST_COST` — `auto`, `on`, or `off`: `on`/`off` force
  sending (or not) `usage: {include: true}` so the backend reports per-response cost (an
  OpenRouter extension); `auto` uses the provider default (on for `openrouter`, off elsewhere).

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
- `anthropic.cache` / `HAX_ANTHROPIC_CACHE` — `auto`, `on`, or `off`: `on`/`off` force sending
  (or suppressing) prompt cache-control breakpoints, while `auto` uses the provider default.
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
  Default `hax`; empty omits it.
- `openrouter.referer` / `HAX_OPENROUTER_REFERER` — `HTTP-Referer` header, OpenRouter's app
  identifier for attribution. Defaults to the hax project URL; empty disables attribution.
- `mock.script` / `HAX_MOCK_SCRIPT` — mock-provider script path.

## Custom provider blocks

Custom provider blocks live under `providers` in `config.json`; see
[providers.md](providers.md#custom-providers). Provider blocks intentionally do not read the
global OpenAI/Anthropic environment variables. Use `api_key_env` inside the block to name the
secret variable for that provider.
