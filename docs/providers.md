# Providers

Start hax interactively and use `/provider` for the easiest setup: unavailable entries include a
reason, and choosing a provider continues into model and reasoning-effort pickers when supported.
Selections are remembered in `state.json`.

For one run, use CLI flags or environment variables:

```sh
hax --provider=openrouter --model=anthropic/claude-sonnet-5
HAX_PROVIDER=llama.cpp hax
```

CLI flags are preferable in scripts because they are explicit and override saved state. Keep API
keys in environment variables rather than command arguments or `config.json`.

## Choosing a provider

| Provider id | Best fit | Required setup |
| --- | --- | --- |
| `codex` | Existing ChatGPT/Codex subscription | Run `/login`. |
| `openai` | Direct OpenAI API | `OPENAI_API_KEY`; choose a model. |
| `anthropic` | Direct Anthropic API | `ANTHROPIC_API_KEY`; choose a model. |
| `openrouter` | Many vendors through one API | `OPENROUTER_API_KEY`; choose a model. |
| `opencode-zen` | Curated pay-as-you-go models | `OPENCODE_API_KEY`; choose a model. |
| `opencode-go` | OpenCode's model subscription | `OPENCODE_API_KEY`; choose a model. |
| `llama.cpp` | Local `llama-server` | Start the server; model is normally discovered. |
| `ollama` | Local Ollama models | Start `ollama serve`; choose a pulled model. |
| `openai-compatible` | OpenAI Chat Completions-compatible endpoint | Base URL; usually choose a model. |
| `anthropic-compatible` | Anthropic Messages-compatible proxy/server | Base URL; usually choose a model. |

When no provider is selected, hax picks the first available one: the hosted providers (Codex,
OpenAI, Anthropic, OpenRouter, OpenCode), then the local servers (llama.cpp, Ollama), then the
generic compatible endpoints and any user-defined providers. Auto-selection is convenient
interactively; pass a provider explicitly in automation so a newly available backend cannot change
a script's behavior.

If an explicitly selected provider cannot start, the REPL opens without one and directs you to
`/provider`; one-shot mode exits with an error. A one-shot banner on stderr identifies the provider,
model, effort, and whether selection was automatic.

## Codex

`codex` uses the ChatGPT Codex backend with a ChatGPT subscription login. Run `/login` and pick a
flow: **browser** opens `auth.openai.com` in this machine's browser and finishes through a
`localhost` redirect (some organizations only permit this flow), while **device code** shows a code
to approve in a browser on any device, so it also works over ssh. Either way hax stores the tokens
in `~/.local/state/hax/auth.json` and refreshes them automatically; the codex CLI is not needed.
`/logout` removes the login.

Alternatively, hax picks up credentials written by the official codex CLI:

```sh
codex                         # log in or refresh credentials
hax --provider=codex
```

CLI credentials are borrowed read-only: hax never modifies `~/.codex/auth.json` and never refreshes
its token, so when it expires, rerun `codex` — or `/login` once to switch to a hax-managed login,
which takes precedence whenever both exist.

hax reads `model` and `model_reasoning_effort` from `~/.codex/config.toml` as provider defaults. If
none is configured, choose a model with `/model` or pass `--model`.

`/usage` shows subscription windows reported by ChatGPT. Codex does not report monetary cost per
response, so any `~$` amount is an API-equivalent estimate from model metadata, not a charge against
the subscription.

Codex serves some models a smaller default context window than they support — the `/model` picker
shows the sanctioned ceiling as "272k context (up to 872k)". A
[`catalog.models`](./configuration.md#model-metadata) override raises the window for
one model; keying the block `codex` leaves the `openai` provider alone:

```json
{
  "catalog": {
    "models": {
      "codex": {
        "gpt-5.6-luna": {"limit": {"context": 872000}}
      }
    }
  }
}
```

Values above the ceiling are not clamped; the backend rejects requests larger than it actually serves.

## OpenAI

```sh
export OPENAI_API_KEY=...
hax --provider=openai
```

OpenAI has no fixed model default. Choose one with `/model`, set `model` in config, or pass
`--model`. hax uses `https://api.openai.com/v1` with the Responses API — the best fit for current
reasoning models and tool calls. Credentials come from `OPENAI_API_KEY`, and the endpoint is
pinned: no setting can redirect the key elsewhere or change the protocol. A `providers.openai`
config block accepts the same advanced fields as custom providers (minus the pinned `base_url`
and `api`), though they are rarely needed; an OpenAI-shaped endpoint elsewhere belongs in a
[custom provider](#custom-providers).

## Anthropic

```sh
export ANTHROPIC_API_KEY=...
hax --provider=anthropic
```

Choose a model with `/model`, config, or `--model`. hax uses `https://api.anthropic.com/v1` with
credentials from `ANTHROPIC_API_KEY`; the endpoint is pinned.

First-party Anthropic uses adaptive thinking, so `/effort` offers the effort levels exposed by hax.
Prompt caching is enabled with a 1h TTL, and the output-token limit follows model metadata when
available (falling back to 32000); a `providers.anthropic` config block can override advanced
fields such as `max_tokens` when an older model needs it. A different endpoint — a proxy, say —
belongs in a [custom provider](#custom-providers).

## OpenRouter

```sh
export OPENROUTER_API_KEY=...
hax --provider=openrouter --model=anthropic/claude-sonnet-5
```

OpenRouter has no fixed model default. `/model` lists its catalog, and `/effort` requests reasoning on
models that expose it. Credentials come from `OPENROUTER_API_KEY`.

OpenRouter reports per-response cost, which hax uses in turn stats and `/session`; `/usage` shows API
key spend and available credits. Model metadata also supplies context limits and image/tool
capabilities when available.

The [transcript](debugging.md#transcript-log) reports the upstream endpoint OpenRouter routed each
response to, which is how to confirm that `extra_body` routing preferences took effect.

By default hax sends app attribution (`HTTP-Referer`, `X-Title`, `X-OpenRouter-Categories`) and
the conversation id as `x-session-id`, which OpenRouter uses for sticky routing and to group the
conversation in its activity view. Override or remove any of them through `extra_headers`
([below](#request-passthrough)):

```json
{
  "providers": {
    "openrouter": {
      "extra_headers": { "X-Title": "my-tool", "HTTP-Referer": "" }
    }
  }
}
```

Before sending proprietary code, review the selected endpoint's retention/training policy and your
OpenRouter privacy settings. Free and paid models have separate training controls, and a free model
should not be assumed private. Enable the account-level training opt-out or zero-data-retention
routing when your work requires it.

## OpenCode Zen and Go

[OpenCode Zen](https://opencode.ai/docs/zen/) (pay-as-you-go) and
[OpenCode Go](https://opencode.ai/docs/go/) (subscription) share one key:

```sh
export OPENCODE_API_KEY=...
hax --provider=opencode-zen --model=kimi-k2.7-code
```

Use `/model` to choose a model; hax automatically uses the API required by each supported model.
Gemini entries are not supported because their API is not implemented. If a newly added model is not
yet described by the model catalog, see the `model_apis` override under
[Custom providers](#custom-providers).

On `opencode-go`, `/usage` shows the subscription's rolling, weekly, and monthly limits. Zen does
not expose usage through its API, so check the OpenCode dashboard instead.

Requests carry the conversation id as `x-opencode-session`, which the gateway requires for
routing and prompt caching, and `x-opencode-client: hax`. Both can be overridden in
`extra_headers` ([below](#request-passthrough)).

## llama.cpp

`llama.cpp` selects the convenience provider for a local `llama-server` at
`http://127.0.0.1:8080/v1`:

```sh
llama-server -m /path/to/model.gguf -c 32768
hax --provider=llama.cpp
```

Its config block is named `providers.llamacpp` because dots separate config path components.

Use `HAX_LLAMACPP_PORT=9090` for another local port, or `HAX_LLAMACPP_BASE_URL` for a complete URL.
If the server uses `--api-key`, set `HAX_LLAMACPP_API_KEY`.

Both a classic single-model server and router mode (`llama-server` started without a model) work.
hax adopts the model automatically when the server leaves no ambiguity — the single served model,
or a router's only running one — and otherwise starts without a model. `/model` then shows the
server's catalog with load state, context size, and image capability. Selecting an idle model starts
loading it in the background, which can take a while; hax never loads a model you did not select.
With `--no-models-autoload`, load models through llama.cpp's own tooling and pick a running one.

hax probes llama.cpp for context and image capability when possible. Start the server with a context
large enough for an agent session; the llama.cpp default is often too small once system instructions,
project context, tool results, and the desired output are combined.

## Ollama

Ollama is a shipped custom provider preconfigured for `http://127.0.0.1:11434/v1`:

```sh
ollama serve
hax --provider=ollama --model=qwen3:8b
```

Choose a pulled model explicitly. hax does not guess which model you intend from Ollama's list, and
one-shot mode requires a model.

Ollama's runtime context defaults can be small for coding-agent prompts. Set a larger
`OLLAMA_CONTEXT_LENGTH` before starting `ollama serve` (or raise `num_ctx` on the model), and set
`context_limit` to the same value if you want hax's percentage display. A too-small context commonly
appears as a response ending with `length`.

Override the endpoint in `config.json` — `port` for another local port, or a full `base_url`:

```json
{
  "providers": {
    "ollama": {
      "base_url": "http://127.0.0.1:11500/v1"
    }
  }
}
```

## Compatible built-ins

`openai-compatible` and `anthropic-compatible` are shipped providers for a generic endpoint you
name at run time. They are ordinary [custom providers](#custom-providers) — configured through
their own `providers.openai-compatible` / `providers.anthropic-compatible` blocks — whose keys
additionally bind environment variables, so a one-off endpoint needs no config file. The variables
affect only these two providers; the full key list is in
[configuration.md](./configuration.md#provider-settings).

### OpenAI-compatible

Use this for an endpoint implementing OpenAI Chat Completions:

```sh
HAX_PROVIDER=openai-compatible \
HAX_OPENAI_DISPLAY_NAME=vLLM \
HAX_OPENAI_BASE_URL=http://127.0.0.1:8000/v1 \
HAX_MODEL=Qwen3-30B \
hax
```

`HAX_OPENAI_BASE_URL` is required. If authentication is needed, use `HAX_OPENAI_API_KEY`; hax does
not fall back to `OPENAI_API_KEY` for compatible endpoints. The default request protocol is Chat
Completions; set `HAX_OPENAI_API=responses` for a Responses endpoint. Set
`HAX_OPENAI_REASONING_FORMAT=nested` only when the server expects `reasoning: {"effort": ...}`
instead of a flat `reasoning_effort` field.

### Anthropic-compatible

Use this for an endpoint implementing Anthropic Messages:

```sh
HAX_PROVIDER=anthropic-compatible \
HAX_ANTHROPIC_BASE_URL=http://127.0.0.1:18080/v1 \
HAX_MODEL=local-model \
hax
```

`HAX_ANTHROPIC_BASE_URL` is required. Use `HAX_ANTHROPIC_API_KEY` when authentication is needed; hax
does not fall back to `ANTHROPIC_API_KEY`. Compatible endpoints default to budget thinking and leave
explicit prompt-cache controls off for broader compatibility.

For a static endpoint you use regularly, prefer a named custom provider instead of repeatedly
exporting the generic base URL.

## Custom providers

Add any static OpenAI Chat Completions, OpenAI Responses, or Anthropic Messages endpoint under
`providers` in `config.json`:

```json
{
  "providers": {
    "groq": {
      "base_url": "https://api.groq.com/openai/v1",
      "api_key_env": "GROQ_API_KEY"
    },
    "company-proxy": {
      "display_name": "Company proxy",
      "api": "anthropic-messages",
      "base_url": "https://llm.example.com/v1",
      "api_key_env": "COMPANY_LLM_KEY",
      "catalog_id": "anthropic"
    }
  }
}
```

Common fields:

| Field | Purpose |
| --- | --- |
| `base_url` | Required endpoint root, unless shipped defaults supply one. |
| `display_name` | Human-readable banner name. |
| `api` | `openai-completions` (default), `openai-responses`, `anthropic-messages`, or `catalog`. |
| `model_apis` | Model-id globs mapped to `api` dialects; the first match sets that model's protocol. |
| `api_key_env` | Name of the environment variable holding the key; recommended. |
| `api_key` | Literal key, or `$VAR` to read an environment variable. |
| `sort_models` | Sort this provider's model picker newest-first (default); `off` keeps server order. |
| `catalog_id` | Provider id in models.dev for cost/context metadata; empty disables lookup. |
| `metadata_api` | `/models` dialect: `openai` (flat list) or `anthropic` (paginated); defaults to the request protocol's family. |
| `extra_body` | Raw JSON members merged into every request body ([below](#request-passthrough)). |
| `extra_headers` | HTTP headers sent on every request ([below](#request-passthrough)). |

A custom provider named after its models.dev id (for example `groq`) uses that identity by default.
Use `catalog_id` when a proxy name differs from the underlying provider. Do not map local models to a
hosted provider merely because names look similar: prices and context limits may differ.

`api: "catalog"` declares a mixed-protocol gateway the model catalog already describes: each model
routes by the catalog's per-model API — how the shipped OpenCode providers work — and models the
catalog leaves unmapped use Chat Completions. `model_apis` rules also switch a provider into this
mode and take precedence over catalog hints; either form makes every dialect's config fields apply,
each to the models speaking it.

`metadata_api` selects the `/models` shape and its auth scheme independently of the request
protocol, since a proxy or gateway can pair either metadata side with either wire — an
`anthropic-messages` endpoint behind an OpenAI-style `/v1/models`, say. It defaults to the family
of the `api` protocol, so most providers never set it.

For `openai-completions`, advanced fields are `reasoning_format`, `reasoning_roundtrip`,
`send_cache_key`, `request_cost`, `cache`, and `cache_ttl`; reasoning replay is automatic per
model, so `reasoning_roundtrip` is only for an endpoint the catalog describes wrongly.
`openai-responses` accepts `send_cache_key`; its reasoning format and encrypted round-trip are
fixed by the protocol.

Anthropic-style blocks accept `max_tokens`, `thinking_mode`, `thinking_budget`, `cache`, `cache_ttl`,
and `version`. Leave advanced fields unset unless the endpoint documents them. Selecting a provider
warns about block members hax does not recognize or that its `api` dialect does not use.

Every provider reads only its own block. The `HAX_OPENAI_*` and `HAX_ANTHROPIC_*` variables belong
to the shipped `openai-compatible` / `anthropic-compatible` blocks and do not bleed into others;
for a custom provider, only the variable named by `api_key_env` is read. Provider names cannot
contain `.`; a block named after a shipped provider configures that provider rather than
replacing it.

### Request passthrough

`extra_body` and `extra_headers` pass provider-specific request features through without dedicated
hax support. They work in every provider's block, compiled-in (`providers.openrouter`,
`providers.codex`) or custom, and are read from the config file only — no environment aliases, no
`/config` override.

`extra_body` is a JSON object merged into every request body after the fields hax builds: a member
overrides the built field of the same name, and an object member extends a built object instead of
replacing it. Values are sent verbatim with their JSON types, so write `false`, not `"false"`.
Members hax itself owns — the model and conversation, the system prompt, the tool list, the
streaming setup — are ignored with a warning naming the member.

`extra_headers` is an object of header names and non-empty string values added to every request to
the provider. A value of `$VAR` reads the environment variable `VAR`, keeping a credential out of
the config file, like an inline `api_key: "$VAR"`; `$$` escapes a literal leading `$`. A header
whose variable is unset is dropped with a warning. A header hax sends for the provider by default
can be overridden by name (case-insensitive) or removed with an empty string value.

A value may contain `{session_id}`, the conversation's stable id — the one `--resume` takes, so it
survives restarts, while `/new` starts a fresh one — for gateways that route or cache by session.

```json
{
  "providers": {
    "openrouter": {
      "extra_body": {
        "provider": { "order": ["baseten"], "allow_fallbacks": false }
      }
    },
    "gateway": {
      "base_url": "https://gateway.example.com/v1",
      "api_key_env": "GATEWAY_API_KEY",
      "extra_body": { "service_tier": "priority" },
      "extra_headers": {
        "x-gateway-project": "hax",
        "x-gateway-key": "$GATEWAY_EXTRA_KEY",
        "x-gateway-session": "{session_id}"
      }
    }
  }
}
```

Prefer a dedicated field when one exists — `api_key`/`api_key_env` for the credential, `version`
for `anthropic-version` — because hax cannot reason about a value injected behind its back: an
`extra_body` that changes between runs can also defeat prompt caching. `HAX_TRACE` redacts API
keys and `$VAR`-resolved values wherever they appear, but cannot recognize a credential written
literally into a header value — one more reason to prefer `$VAR`.

## Mock provider

`mock` is a development backend with no network or model. It is hidden from auto-selection and the
provider picker but can be selected explicitly:

```sh
HAX_PROVIDER=mock hax
HAX_PROVIDER=mock HAX_MOCK_SCRIPT=scripts/mock/demo.txt hax
```

See [Troubleshooting and development diagnostics](./debugging.md#mock-provider) for its script format.
