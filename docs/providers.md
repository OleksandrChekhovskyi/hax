# Providers

Start hax interactively and use `/provider` for the easiest setup: unavailable entries include a
reason, and choosing a provider continues into model and reasoning-effort pickers when supported.
Selections are remembered in `state.json`. Unless a section below says otherwise, a provider has no
default model: pick one with `/model`, `--model`, or `model` in config.

For one run, use CLI flags or environment variables:

```sh
hax --provider=openrouter --model=anthropic/claude-sonnet-5
HAX_PROVIDER=llama.cpp hax
```

CLI flags are preferable in scripts because they are explicit and override saved state. Keep API
keys in environment variables rather than command arguments or `config.json`.

## Choosing a provider

| Provider | Description |
| --- | --- |
| `codex` | Your ChatGPT subscription, through the Codex backend. Run `/login`. |
| `openai` | The OpenAI API. Set `OPENAI_API_KEY`. |
| `anthropic` | The Anthropic API. Set `ANTHROPIC_API_KEY`. |
| `openrouter` | Models from many vendors through one API. Set `OPENROUTER_API_KEY`. |
| `opencode-zen` | OpenCode's curated pay-as-you-go models. Set `OPENCODE_API_KEY`. |
| `opencode-go` | OpenCode's model subscription. Set `OPENCODE_API_KEY`. |
| `deepseek` | The DeepSeek API. Set `DEEPSEEK_API_KEY`. |
| `llama.cpp` | A local llama.cpp server. Start `llama-server`. |
| `ollama` | A local Ollama server. Start `ollama serve` and choose a model. |

When no provider is selected, hax picks the first available one in table order, then a
[one-off endpoint](#one-off-endpoints) whose base URL is set, then any custom providers.
Auto-selection is convenient interactively; pass a provider explicitly in automation so a newly
available backend cannot change a script's behavior.

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
shows the sanctioned ceiling as "272k context (up to 872k)". A `catalog.models` override (see
Model metadata in [configuration.md](./configuration.md#model-metadata)) raises the window for one
model; keying the block `codex` leaves the `openai` provider alone:

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

For an OpenAI-shaped endpoint elsewhere, use a [custom provider](#custom-providers).

## Anthropic

```sh
export ANTHROPIC_API_KEY=...
hax --provider=anthropic
```

Prompts are cached with a 1h TTL. If an older model rejects the default output limit, set
`providers.anthropic.max_tokens`. For a proxy or another endpoint, use a
[custom provider](#custom-providers).

## OpenRouter

```sh
export OPENROUTER_API_KEY=...
hax --provider=openrouter --model=anthropic/claude-sonnet-5
```

Turn stats and `/session` show the cost OpenRouter reports; `/usage` shows API key spend and
available credits.

The transcript (Ctrl-T, or the `HAX_TRANSCRIPT` file) reports the upstream endpoint OpenRouter
routed each response to, which is how to confirm that `extra_body` routing preferences took effect.

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

Gemini models are not supported. If a newly added model fails because the model catalog does not
describe it yet, see the `model_apis` override under [Custom providers](#custom-providers).

On `opencode-go`, `/usage` shows the subscription's rolling, weekly, and monthly limits. Zen does
not expose usage through its API, so check the OpenCode dashboard instead.

Requests carry the conversation id as `x-opencode-session`, which the gateway requires for
routing and prompt caching, and `x-opencode-client: hax`. Both can be overridden in
`extra_headers` ([below](#request-passthrough)).

## DeepSeek

```sh
export DEEPSEEK_API_KEY=...
hax --provider=deepseek --model=deepseek-flash
```

`/usage` shows the account balance. Cost estimates use off-peak prices; weekday peak hours
(01:00–04:00 and 06:00–10:00 UTC) cost double.

DeepSeek may train on what you send, code included, and stores it in China. Review its
[privacy policy](https://cdn.deepseek.com/policies/en-US/deepseek-privacy-policy.html) and training
opt-out before sending proprietary code.

## llama.cpp

`llama.cpp` connects to a local `llama-server`, by default at `http://127.0.0.1:9931/v1`:

```sh
llama-server -m /path/to/model.gguf
hax --provider=llama.cpp
```

Its config block is named `providers.llamacpp` because dots separate config path components.

Use `HAX_LLAMACPP_PORT` for another local port (`8080` for llama.cpp releases before v0.7.0), or
`HAX_LLAMACPP_BASE_URL` for a complete URL. If the server uses `--api-key`, set
`HAX_LLAMACPP_API_KEY`.

Both a classic single-model server and router mode (`llama-server` started without a model) work.
hax adopts the model automatically when the server leaves no ambiguity — the single served model,
or a router's only running one — and otherwise starts without a model. `/model` then shows the
server's catalog with load state, context size, and image capability. Selecting an idle model starts
loading it in the background, which can take a while; hax never loads a model you did not select.
With `--no-models-autoload`, load models through llama.cpp's own tooling and pick a running one.

hax reads the context size the server is running with, so no `context_limit` is needed.
`llama-server` can choose a small one, for example to fit device memory; if it is too small for an
agent session, restart the server with a larger `-c` / `--ctx-size`.

## Ollama

`ollama` connects to a local Ollama server, by default at `http://127.0.0.1:11434/v1`:

```sh
ollama serve
hax --provider=ollama --model=qwen3:8b
```

Choose a pulled model explicitly. hax does not guess which model you intend from Ollama's list, and
one-shot mode requires a model.

Ollama's default context depends on GPU memory and can be too small for an agent session on smaller
machines. Set a larger `OLLAMA_CONTEXT_LENGTH` before starting `ollama serve` (or raise `num_ctx` on
the model), and set `context_limit` to the same value so hax compacts before the context fills.

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

## Custom providers

Add any static OpenAI Chat Completions, OpenAI Responses, or Anthropic Messages endpoint under
`providers` in `config.json`:

```json
{
  "providers": {
    "groq": {
      "base_url": "https://api.groq.com/openai/v1",
      "api_key_env": "GROQ_API_KEY",
      "catalog_id": "groq"
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
| `catalog_id` | Provider id in models.dev for cost/context metadata; unset means none. |
| `metadata_api` | `/models` dialect: `openai` (flat list) or `anthropic` (paginated); defaults to the request protocol's family. |
| `extra_body` | Raw JSON members merged into every request body ([below](#request-passthrough)). |
| `extra_headers` | HTTP headers sent on every request ([below](#request-passthrough)). |
| `overflow_phrases` | Phrases in this provider's error messages that mean the conversation exceeds the model's context window, for servers whose wording hax does not already recognize. |

A custom provider has no catalog identity until `catalog_id` names one, so a block for a local
server or a private proxy never contacts models.dev. Set it to the models.dev provider id (for
example `groq`) to get pricing and context metadata for a hosted provider, or to the underlying
provider's id for a proxy in front of one. Do not map local models to a hosted provider merely
because names look similar: prices and context limits may differ.

`api: "catalog"` is for a gateway that serves models over different protocols, like the shipped
OpenCode providers: each model uses the protocol the catalog lists for it. `model_apis` rules route
the same way and take precedence over the catalog. Models neither covers use the provider's `api`,
or Chat Completions under `catalog`. Each protocol's advanced fields apply to the models using it.

`metadata_api` is for an endpoint whose model listing does not match its request protocol, such as
an `anthropic-messages` endpoint with an OpenAI-style `/v1/models`; it also selects how the listing
authenticates. Most providers never need it.

Advanced fields depend on the protocol:

| `api` | Advanced fields |
| --- | --- |
| `openai-completions` | `reasoning_format`, `reasoning_roundtrip`, `reasoning_required`, `send_cache_key`, `request_cost`, `cache`, `cache_ttl` |
| `openai-responses` | `send_cache_key` |
| `anthropic-messages` | `max_tokens`, `thinking_mode`, `thinking_budget`, `cache`, `cache_ttl`, `version` |

Their values and defaults are listed under Provider settings in
[configuration.md](./configuration.md#provider-settings). Leave them unset unless the endpoint
documents a need; selecting a provider warns about block members hax does not recognize or that its
`api` does not use.

Reasoning replay needs no setup: hax returns a model's reasoning in the field the server streamed it
in. Set `reasoning_roundtrip` to a field name only for a server that reads reasoning from a
different field, or to `off` for one that rejects it. Turn `reasoning_required` on for a server
that rejects tool calls without their reasoning, such as a proxy to DeepSeek without a
`catalog_id`; it takes effect once the field is known from `catalog_id` or `reasoning_roundtrip`.

Every provider reads only its own block; for a custom provider, the only environment variable read
is the one named by `api_key_env`. Provider names cannot contain `.`; a block named after a shipped
provider configures that provider rather than replacing it.

### One-off endpoints

For a quick test or a script, `openai-compatible` and `anthropic-compatible` reach an endpoint named
entirely at run time, with no config file. Each takes the same fields as a custom provider, and its
common fields also have environment variables, listed under Provider settings in
[configuration.md](./configuration.md#provider-settings). `/provider` offers them only once their
base URL is set.

```sh
HAX_PROVIDER=openai-compatible \
HAX_OPENAI_DISPLAY_NAME=vLLM \
HAX_OPENAI_BASE_URL=http://127.0.0.1:8000/v1 \
HAX_MODEL=Qwen3-30B \
hax
```

`openai-compatible` requires `HAX_OPENAI_BASE_URL` and sends Chat Completions requests; set
`HAX_OPENAI_API=responses` for a Responses endpoint. Set `HAX_OPENAI_REASONING_FORMAT=nested` only
when the server expects `reasoning: {"effort": ...}` instead of a flat `reasoning_effort` field.

`anthropic-compatible` requires `HAX_ANTHROPIC_BASE_URL` and sends Messages requests. It gets the
first-party thinking and prompt-cache defaults, except that a model the catalog does not list falls
back to budget thinking. Override `thinking_mode` or `cache` when an endpoint needs it.

For authentication, set `HAX_OPENAI_API_KEY` or `HAX_ANTHROPIC_API_KEY`; hax does not fall back to
`OPENAI_API_KEY` or `ANTHROPIC_API_KEY`. The `HAX_OPENAI_*` and `HAX_ANTHROPIC_*` variables
configure only these two providers.

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
        "provider": {
          "order": ["baseten"],
          "allow_fallbacks": false
        }
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

`mock` is a development backend with no network or model; without a script it echoes the prompt
back. It is hidden from auto-selection and the provider picker but can be selected explicitly,
which is handy for trying the interface or the `--json` stream without credentials:

```sh
HAX_PROVIDER=mock hax
```

Mock runs record no session unless `HAX_NO_SESSION=0` is set. `HAX_MOCK_SCRIPT` names a script of
canned turns to replay instead; the format is described in `docs/debugging.md` in the hax source
tree.
