# Configuration

Configuration is optional: the interactive pickers work without a file and remember provider/model
choices. Add `config.json` for stable defaults, custom providers, presets, or behavior changes.

## Files and resolution

hax follows the XDG Base Directory specification. The table shows the paths used when the
corresponding `XDG_CONFIG_HOME`, `XDG_STATE_HOME`, or `XDG_CACHE_HOME` variable is unset.

| Purpose | Default path |
| --- | --- |
| User configuration | `~/.config/hax/config.json` |
| Remembered interactive selections | `~/.local/state/hax/state.json` |
| Sessions and prompt history | `~/.local/state/hax/` |
| Model metadata cache | `~/.cache/hax/catalog.json` |

`config.json` expresses durable, user-owned intent. It is suitable for a dotfiles repository or a
symlink from one, provided secrets remain in environment variables. `state.json` is machine-local
convenience state written by hax when you use the interactive selectors; do not edit or version it.
Keeping them separate lets interactive `/provider` and `/model` choices persist without rewriting
your portable configuration.

For an ordinary setting, the first defined value wins:

```text
current-process override → resumed conversation → environment → state.json → config.json → default
```

The resumed-conversation tier applies to provider, model, effort, and preset. This ensures `hax -c`
continues on the backend that previously handled the session even when your current default changed.
Explicit CLI selection flags still override it.

Sources behave as follows:

- `--provider`, `--model`, `--effort`, and `--preset` apply only to this process and do not persist.
  A preset is applied first, then explicit selection flags are layered over it.
- `/provider`, `/model`, `/effort`, and `/preset` affect the current process and save the selection in
  `state.json` for later runs.
- `HAX_*` environment variables are useful for per-shell and automated runs. They override saved
  interactive choices.
- `config.json` should hold durable defaults, provider definitions, and presets. Do not edit
  `state.json` by hand.

A preset is a complete stance rather than a bag of partial defaults. Explicit provider/model/effort
input exits or suppresses a lower-priority preset instead of silently mixing with it. Select a preset
explicitly when that is what you want.

In the REPL, `/config` shows each registered setting's resolved value, source, and description. It
also identifies settings that can be changed for the running process:

```text
/config                         # pick a setting
/config theme light             # set a process override
/config compact.threshold 75
/config theme default           # clear the process override
```

Provider, model, effort, and preset use their dedicated commands. API keys are displayed only as set
or unset. `/config` tracks only hax's own settings, so a first-party credential variable such as
`OPENAI_API_KEY` authenticates without appearing there. Provider blocks (`providers.<id>.*`) are not
listed either: select a provider with `/provider`, and define its settings through environment
variables or `config.json`. A specific registered key can still be queried by name, for example
`/config providers.openai-compatible.base_url`.

## Config file format

`config.json` is plain JSON without comments or trailing commas. Dotted keys are normally represented
as nested objects:

```json
{
  "provider": "openrouter",
  "model": "anthropic/claude-sonnet-5",
  "theme": "dark",
  "compact": {
    "threshold": 80
  }
}
```

Flat dotted keys such as `"compact.threshold": 80` are also accepted for scalar settings. Keep
structured values such as `providers`, `presets`, and `catalog.models` nested for clarity.

Scalar strings, numbers, and booleans are accepted. Booleans recognize `1/0`, `true/false`,
`yes/no`, and `on/off`, case-insensitively. Durations accept seconds or `ms`, `s`, `m`, and `h`
suffixes. Byte sizes accept plain bytes or `k` and `m` suffixes using 1024-base units; token
counts (`context_limit`, catalog `limit` fields) use decimal `k` and `m`, so `"272k"` means
272000 tokens. Invalid typed values use the setting's default; `/config` rejects invalid runtime
values with an error.

Keep secrets in environment variables where possible:

```sh
export OPENAI_API_KEY=...
export ANTHROPIC_API_KEY=...
export OPENROUTER_API_KEY=...
```

For a custom provider, use `api_key_env` to name its credential variable rather than putting the
credential itself in JSON.

## Common configurations

### Local OpenAI-compatible server

```json
{
  "provider": "openai-compatible",
  "model": "Qwen3-30B",
  "providers": {
    "openai-compatible": {
      "display_name": "vLLM",
      "base_url": "http://127.0.0.1:8000/v1"
    }
  }
}
```

### Ollama on a different port

```json
{
  "provider": "ollama",
  "model": "qwen3:8b",
  "providers": {
    "ollama": {
      "base_url": "http://127.0.0.1:11500/v1"
    }
  }
}
```

### Light terminal theme with restrained notifications

```json
{
  "theme": "light",
  "notify": "off",
  "display_width": 100
}
```

## Presets

A preset is first of all a name for a favorite provider/model/effort selection — useful when you
regularly switch between a daily model, a cheaper alternative, and a stronger model for difficult
work. A preset can also become a role by adding a description, system-prompt instructions, and a
visual tint. Only presets with a description are advertised to the model for subagent delegation;
a favorite that is just a name stays yours alone.

The easiest way to save a favorite is to select it with `/provider`, `/model`, and `/effort`, then run
`/preset-save <name>`. Edit `config.json` when you want to add role-specific fields:

```json
{
  "presets": {
    "daily": {
      "description": "favorite everyday model",
      "provider": "openrouter",
      "model": "deepseek/deepseek-v4-flash-0731",
      "tint": "teal"
    },
    "review": {
      "description": "thorough code review",
      "provider": "codex",
      "model": "gpt-5.6-sol",
      "effort": "high",
      "system_prompt_append": "@prompts/review.md",
      "tint": "rose"
    }
  }
}
```

Model availability, pricing, and data policies change; treat these IDs as examples and prefer choices
shown by your `/model` picker. Apply a preset with `--preset daily`, `HAX_PRESET=review`,
`/preset review`, or `/new review`. `/preset` offers a picker when no name is given. The active preset
appears in the banner and `/session`.

Preset fields:

| Field | Meaning |
| --- | --- |
| `provider` | Required provider id. |
| `model` | Optional model id; omitted means the provider's own default/discovery. |
| `effort` | Optional provider-specific reasoning effort. |
| `system_prompt` | Replace the built-in base prompt. Prefer appending unless replacement is required. |
| `system_prompt_append` | Add role-specific instructions after the base prompt. |
| `tint` | `teal`, `violet`, `rose`, or `sage`. |
| `description` | Human-readable purpose. Required for the preset to be offered to the model as a delegation target. |

`system_prompt` and `system_prompt_append` accept `@path`; relative paths resolve from the hax config
directory, while absolute paths and `@~/...` work directly. Prefer `system_prompt_append` so updates
to hax's built-in safety and workflow guidance are retained.

A preset replaces the previous preset as a whole. Explicitly choosing a provider, model, or effort
leaves the active preset. Endpoint and credential settings are not preset fields; define those on a
custom provider and point the preset to it.

`/preset-save <name> [tint]` writes the current selection to `config.json` as a preset and activates
it. The command preserves other loaded keys but normalizes JSON formatting. Because it rewrites
the configuration loaded at startup, do not edit the file concurrently in another window. Existing
preset names require confirmation before replacement; their description is retained.

## Setting reference

The tables below cover persistent and environment configuration. `/config` is the best source for
the current binary's resolved values and whether a setting is runtime-tunable. `—` means unset or
provider-dependent.

### Selection and context

| Config key | Environment | Default | Purpose |
| --- | --- | --- | --- |
| `preset` | `HAX_PRESET` | — | Startup preset; an empty environment value disables a config default. |
| `provider` | `HAX_PROVIDER` | auto | Provider id. |
| `model` | `HAX_MODEL` | — | Provider-specific model id. |
| `effort` | `HAX_EFFORT` | — | Provider-specific reasoning effort; empty omits it. |
| `system_prompt` | `HAX_SYSTEM_PROMPT` | built in | Replace the base prompt; `@path` reads a file; `(none)` removes the entire system message. |
| `system_prompt_append` | `HAX_SYSTEM_PROMPT_APPEND` | — | Append text or an `@path` file to the base prompt. |
| `no_env` | `HAX_NO_ENV` | off | Omit the Environment section. |
| `no_agents_md` | `HAX_NO_AGENTS_MD` | off | Omit discovered `AGENTS.md` files. |
| `no_skills` | `HAX_NO_SKILLS` | off | Omit skill descriptions. |
| `no_subagents` | `HAX_NO_SUBAGENTS` | off | Omit delegation guidance. |
| `no_tasks` | `HAX_NO_TASKS` | off | Disable managed background tasks. |

`(none)` for `system_prompt` is stronger than an empty string: it sends no system message at all.
`--raw` is usually clearer when the goal is a prompt-only chat with no tools or context.

### Display and behavior

| Config key | Environment | Default | Purpose |
| --- | --- | --- | --- |
| `markdown` | `HAX_MARKDOWN` | on | Render Markdown on terminal output; piped output remains raw. |
| `show_reasoning` | `HAX_SHOW_REASONING` | off | Display reasoning emitted by the provider; does not enable reasoning. |
| `sort_models` | `HAX_SORT_MODELS` | `auto` | Sort model picker newest-first (`on`), keep server order (`off`), or use provider default (sorted unless the provider opts out). |
| `context_limit` | `HAX_CONTEXT_LIMIT` | auto | Override the model context-window size used for display and compaction. |
| `display_width` | `HAX_DISPLAY_WIDTH` | `auto` | `auto`, `terminal`, or an exact width of at least 20 columns. |
| `notify` | `HAX_NOTIFY` | `auto` | Completion notification: `auto`, `bel`, `osc9`, or `off`. |
| `theme` | `HAX_THEME` | `auto` | `auto`, `dark`, `light`, `ansi`, or `off`. |
| `tint` | `HAX_TINT` | `teal` | Model-output tint: `teal`, `violet`, `rose`, or `sage`. |
| `keep_awake` | `HAX_KEEP_AWAKE` | on | Best-effort idle-sleep inhibition while a turn runs. |
| `compact.auto` | `HAX_COMPACT_AUTO` | on | Automatically summarize history near the context limit. |
| `compact.threshold` | `HAX_COMPACT_THRESHOLD` | `85` | Context percentage that triggers automatic compaction. |
| `max_turns` | `HAX_MAX_TURNS` | `auto` | Model round-trips per user turn: interactive pauses, one-shot aborts. `auto`: unlimited interactively, 100 in one-shot. |

`theme=auto` respects `NO_COLOR`, terminal color support, and `COLORFGBG` when available. Terminals
rarely report a light background reliably, so set `light` explicitly if auto detection is wrong.
`theme=ansi` uses the terminal's own 16-color palette; identity tints apply only to dark/light themes.

### Recording

| Config key | Environment | Default | Purpose |
| --- | --- | --- | --- |
| `no_session` | `HAX_NO_SESSION` | `auto` | Skip new sessions and prompt-history writes; auto skips the mock provider. |
| `session_retention_days` | `HAX_SESSION_RETENTION_DAYS` | `30` | Remove inactive sessions after N days; `0` keeps them. |
| `transcript` | `HAX_TRANSCRIPT` | — | Mirror the Ctrl-T transcript to a file. |
| `trace` | `HAX_TRACE` | — | Write an HTTP/SSE diagnostic trace. |

### Model metadata

| Config key | Environment | Default | Purpose |
| --- | --- | --- | --- |
| `catalog.url` | `HAX_CATALOG_URL` | models.dev | Metadata catalog URL; empty disables fetching. |
| `catalog.refresh` | `HAX_CATALOG_REFRESH` | `24h` | Refresh age; `0` disables fetching. |

hax uses model metadata for context limits, image capability, and estimated spend. It caches
[models.dev](https://models.dev/api.json) data and refreshes it in the background only when needed.
A stale cache remains usable. Set `catalog.url` empty or `catalog.refresh` to `0` to prevent network
refreshes; an existing cache may still be read.

Per-model overrides can be placed under `catalog.models`, with costs in USD per million tokens:

```json
{
  "catalog": {
    "models": {
      "openai": {
        "example-model": {
          "cost": {"input": 1.25, "output": 10, "cache_read": 0.125},
          "limit": {"context": 400000, "output": 128000}
        }
      }
    }
  }
}
```

Blocks are keyed by the runtime provider id (the `providers.<id>` name) or by the models.dev
provider key when the two differ; the runtime-id block wins field by field. Configured fields
override both the cached snapshot and metadata the provider reports live — see
[Codex](./providers.md#codex) for a worked context-window override. Model API overrides for mixed
gateways are covered under [Custom providers](./providers.md#custom-providers). A `~` on displayed
spend means the result is estimated from token counts and this metadata; check provider billing
for authoritative costs.

### Tools and transport

| Config key | Environment | Default | Purpose |
| --- | --- | --- | --- |
| `image_input` | `HAX_IMAGE_INPUT` | `auto` | Auto-detect image support, or force it `on`/`off`. |
| `tool_output_cap` | `HAX_TOOL_OUTPUT_CAP` | `50k` | Maximum tool-output bytes retained for the model. |
| `bash.timeout` | `HAX_BASH_TIMEOUT` | `2m` | Default command timeout before detaching; `0` disables. |
| `bash.timeout_max` | `HAX_BASH_TIMEOUT_MAX` | `30m` | Maximum timeout a model may request; `0` removes the cap. |
| `bash.timeout_grace` | `HAX_BASH_TIMEOUT_GRACE` | `2s` | SIGTERM-to-SIGKILL grace period. |
| `bash.background_yield` | `HAX_BASH_BACKGROUND_YIELD` | `5s` | Initial output window before explicit backgrounding detaches. |
| `bash.shell` | `HAX_BASH_SHELL` | bash/sh | Shell executable used by the bash tool. |
| `task.wait_timeout` | `HAX_TASK_WAIT_TIMEOUT` | `10m` | Default wait for a background task. |
| `task.max_running` | `HAX_TASK_MAX_RUNNING` | `32` | Maximum concurrent background tasks, up to 64. |
| `http.max_retries` | `HAX_HTTP_MAX_RETRIES` | `4` | Additional retries for transient HTTP failures. |
| `http.retry_base` | `HAX_HTTP_RETRY_BASE` | `1s` | Initial retry backoff. |
| `http.idle_timeout` | `HAX_HTTP_IDLE_TIMEOUT` | `10m` | Streaming silence before failure; `0` disables. |

When `no_tasks` is on, reaching `bash.timeout` kills the command instead of detaching it. Standard
`CURL_CA_BUNDLE`, `SSL_CERT_FILE`, and `SSL_CERT_DIR` variables can override TLS certificate lookup.

### Provider settings

Every provider reads settings only from its own `providers.<id>` block; nothing bleeds between
providers. For the first-party providers the endpoint, protocol, and credential variable
(`OPENAI_API_KEY`, `ANTHROPIC_API_KEY`, `OPENROUTER_API_KEY`) are pinned — `base_url` and `api`
in their blocks warn and are ignored, so no setting can redirect a first-party key or change what
it speaks. Their other advanced fields (the same ones
[custom providers](./providers.md#custom-providers) accept) are honored but rarely needed; a
different endpoint is a custom provider, not a tweak. Codex is pinned the same way, and its
credentials come from the ChatGPT login ([`/login`](./providers.md#codex)) rather than a key, so
`api_key` and `api_key_env` in its block warn and are ignored.

The shipped `openai-compatible` and `anthropic-compatible` providers are configured the same way —
through their own `providers.<name>` blocks — and additionally bind environment variables to those
keys so a one-off endpoint needs no config file.

Keys in the `providers.openai-compatible` block:

| Config key | Environment | Default | Purpose |
| --- | --- | --- | --- |
| `base_url` | `HAX_OPENAI_BASE_URL` | — | Endpoint; required. |
| `api_key` | `HAX_OPENAI_API_KEY` | — | Bearer token. Does not inherit `OPENAI_API_KEY`. |
| `display_name` | `HAX_OPENAI_DISPLAY_NAME` | — | Banner and picker name. |
| `api` | `HAX_OPENAI_API` | `chat` | `chat` (Chat Completions) or `responses`. |
| `reasoning_format` | `HAX_OPENAI_REASONING_FORMAT` | `flat` | Effort request shape: `flat` or `nested`. |
| `reasoning_roundtrip` | `HAX_REASONING_ROUNDTRIP` | `auto` | Replay reasoning text: `off`, `on`, or a field name. |
| `send_cache_key` | `HAX_OPENAI_SEND_CACHE_KEY` | `auto` | Send a stable prompt-cache key. |
| `request_cost` | `HAX_OPENAI_REQUEST_COST` | `auto` | Request provider-specific per-response cost data. |
| `cache` | `HAX_OPENAI_CACHE` | `auto` | Send explicit prompt-cache breakpoints. |
| `cache_ttl` | `HAX_OPENAI_CACHE_TTL` | `1h` | Cache TTL: `5m` or `1h`. |

Leave cache and cost controls on `auto` unless the endpoint documents support — the correct
behavior varies by provider and model.

Keys in the `providers.anthropic-compatible` block:

| Config key | Environment | Default | Purpose |
| --- | --- | --- | --- |
| `base_url` | `HAX_ANTHROPIC_BASE_URL` | — | Endpoint; required. |
| `api_key` | `HAX_ANTHROPIC_API_KEY` | — | `x-api-key` token. Does not inherit `ANTHROPIC_API_KEY`. |
| `display_name` | `HAX_ANTHROPIC_DISPLAY_NAME` | — | Banner and picker name. |
| `max_tokens` | `HAX_ANTHROPIC_MAX_TOKENS` | model cap | Maximum output including thinking; clamped to known model limits. |
| `thinking_mode` | `HAX_ANTHROPIC_THINKING_MODE` | `budget` | `adaptive`, `budget`, or `off`. When unset, selecting an effort switches to adaptive. |
| `thinking_budget` | `HAX_ANTHROPIC_THINKING_BUDGET` | max minus 1 | Budget-mode thinking tokens. |
| `cache` | `HAX_ANTHROPIC_CACHE` | `auto` | Send prompt-cache breakpoints. |
| `cache_ttl` | `HAX_ANTHROPIC_CACHE_TTL` | `1h` | Cache TTL: `5m` or `1h`. |
| `version` | `HAX_ANTHROPIC_VERSION` | `2023-06-01` | API version header. |

When model metadata has no output limit, `max_tokens` falls back internally to 32000.

### Provider-specific and development settings

| Config key | Environment | Default | Purpose |
| --- | --- | --- | --- |
| `providers.llamacpp.base_url` | `HAX_LLAMACPP_BASE_URL` | — | Full llama-server URL; overrides the port. |
| `providers.llamacpp.api_key` | `HAX_LLAMACPP_API_KEY` | — | Bearer token when llama-server uses `--api-key`. |
| `providers.llamacpp.port` | `HAX_LLAMACPP_PORT` | `8080` | llama-server port when no base URL is set. |
| `providers.openrouter.title` | `HAX_OPENROUTER_TITLE` | `hax` | OpenRouter attribution title; empty disables. |
| `providers.openrouter.referer` | `HAX_OPENROUTER_REFERER` | `https://usehax.dev` | OpenRouter attribution URL; empty disables. |
| `providers.mock.script` | `HAX_MOCK_SCRIPT` | — | Mock-provider script path. |

Custom provider blocks are documented in [providers.md](./providers.md#custom-providers).
