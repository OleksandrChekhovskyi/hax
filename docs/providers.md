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
| `codex` | Existing ChatGPT/Codex subscription | Log in with the official `codex` CLI. |
| `openai` | Direct OpenAI API | `OPENAI_API_KEY`; choose a model. |
| `anthropic` | Direct Anthropic API | `ANTHROPIC_API_KEY`; choose a model. |
| `openrouter` | Many vendors through one API | `OPENROUTER_API_KEY`; choose a model. |
| `llama.cpp` | Local `llama-server` | Start the server; model is normally discovered. |
| `ollama` | Local Ollama models | Start `ollama serve`; choose a pulled model. |
| `openai-compatible` | OpenAI Chat Completions-compatible endpoint | Base URL; usually choose a model. |
| `anthropic-compatible` | Anthropic Messages-compatible proxy/server | Base URL; usually choose a model. |

With no configured provider, hax tries available providers in this order: Codex,
OpenAI-compatible, Anthropic-compatible, llama.cpp, OpenAI, Anthropic, OpenRouter, then
config-defined providers such as Ollama. Auto-selection is convenient interactively; configure or
pass the provider in automation so a newly available backend cannot change a script's behavior.

If an explicitly selected provider cannot start, the REPL opens without one and directs you to
`/provider`; one-shot mode exits with an error. A one-shot banner on stderr identifies the provider,
model, effort, and whether selection was automatic.

## Codex

`codex` uses the ChatGPT Codex backend and the OAuth credentials written by the official Codex CLI:

```sh
codex                         # log in or refresh credentials
hax --provider=codex
```

hax reads `model` and `model_reasoning_effort` from `~/.codex/config.toml` as provider defaults. If
none is configured, choose a model with `/model` or pass `--model`.

`/usage` shows subscription windows reported by ChatGPT. Codex does not report monetary cost per
response, so any `~$` amount is an API-equivalent estimate from model metadata, not a charge against
the subscription.

If authentication expires, run the official `codex` CLI again; hax cannot refresh that token itself.

## OpenAI

```sh
export OPENAI_API_KEY=...
hax --provider=openai
```

OpenAI has no fixed model default. Choose one with `/model`, set `model` in config, or pass
`--model`. hax uses `https://api.openai.com/v1` and deliberately ignores `openai.base_url`, preventing
a first-party key from being sent to a third-party endpoint by accident.

Credential order is `HAX_OPENAI_API_KEY`, then `OPENAI_API_KEY`. Requests use the Responses API by
default, which is the best fit for current reasoning models and tool calls. Set `openai.api` to
`chat` only for a specific compatibility need.

## Anthropic

```sh
export ANTHROPIC_API_KEY=...
hax --provider=anthropic
```

Choose a model with `/model`, config, or `--model`. hax uses `https://api.anthropic.com/v1` and
ignores `anthropic.base_url` for the first-party provider. Credential order is
`HAX_ANTHROPIC_API_KEY`, then `ANTHROPIC_API_KEY`.

First-party Anthropic defaults to adaptive thinking, so `/effort` offers the effort levels exposed by
hax. Prompt caching is enabled by default. The output-token limit follows model metadata when
available and otherwise falls back to 32000; override it with `anthropic.max_tokens` if a proxy or
older model needs a smaller value.

## OpenRouter

```sh
export OPENROUTER_API_KEY=...
hax --provider=openrouter --model=anthropic/claude-sonnet-5
```

OpenRouter has no fixed model default. `/model` lists its catalog, and `/effort` requests reasoning on
models that expose it. Credential order is `HAX_OPENAI_API_KEY`, then `OPENROUTER_API_KEY`.

OpenRouter reports per-response cost, which hax uses in turn stats and `/session`; `/usage` shows API
key spend and available credits. Model metadata also supplies context limits and image/tool
capabilities when available.

hax sends its project URL and title for OpenRouter app attribution by default. Set
`openrouter.referer` or `openrouter.title` to an empty string to omit those headers.

Before sending proprietary code, review the selected endpoint's retention/training policy and your
OpenRouter privacy settings. Free and paid models have separate training controls, and a free model
should not be assumed private. Enable the account-level training opt-out or zero-data-retention
routing when your work requires it.

## llama.cpp

`llama.cpp` is a convenience configuration for a local `llama-server` at
`http://127.0.0.1:8080/v1`:

```sh
llama-server -m /path/to/model.gguf -c 32768
hax --provider=llama.cpp
```

Use `HAX_LLAMACPP_PORT=9090` for another local port, or `HAX_OPENAI_BASE_URL` for a complete URL. If
the server uses `--api-key`, set `HAX_OPENAI_API_KEY`.

Both a classic single-model server and router mode (`llama-server` started without a model) work
as expected: hax adopts the model automatically when the server leaves no ambiguity — the single
served model, or a router's only running one — and otherwise starts without a model so `/model`
picks from the server's catalog, which shows each model's load state. hax never makes the router
load a model you didn't select; the first use of an idle model loads it, which can take a while.
With `--no-models-autoload`, hax does not override the server: load models through llama.cpp's own
tooling and pick a running one.

hax probes llama.cpp for context and image capability when possible. Start the server with a context
large enough for an agent session; the llama.cpp default is often too small once system instructions,
project context, tool results, and the desired output are combined.

## Ollama

Ollama is a shipped custom-provider recipe for `http://127.0.0.1:11434/v1`:

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

Override the endpoint in `config.json`:

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

### OpenAI-compatible

Use this for an endpoint implementing OpenAI Chat Completions:

```sh
HAX_PROVIDER=openai-compatible \
HAX_PROVIDER_NAME=vLLM \
HAX_OPENAI_BASE_URL=http://127.0.0.1:8000/v1 \
HAX_MODEL=Qwen3-30B \
hax
```

`HAX_OPENAI_BASE_URL` is required. If authentication is needed, use `HAX_OPENAI_API_KEY`; hax does
not fall back to `OPENAI_API_KEY` for compatible endpoints. The default request protocol is Chat
Completions. Set `openai.reasoning_format` to `nested` only when the server expects
`reasoning: {"effort": ...}` instead of a flat `reasoning_effort` field.

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
| `base_url` | Required endpoint root, unless a shipped recipe supplies one. |
| `display_name` | Human-readable banner name. |
| `api` | `openai-completions` (default), `openai-responses`, or `anthropic-messages`. |
| `api_key_env` | Name of the environment variable holding the key; recommended. |
| `api_key` | Literal key; avoid for real secrets. |
| `sort_models` | Alphabetize this provider's model picker. |
| `catalog_id` | Provider id in models.dev for cost/context metadata; empty disables lookup. |

A custom provider named after its models.dev id (for example `groq`) uses that identity by default.
Use `catalog_id` when a proxy name differs from the underlying provider. Do not map local models to a
hosted provider merely because names look similar: prices and context limits may differ.

For `openai-completions`, advanced fields are `reasoning_format`, `reasoning_roundtrip`,
`send_cache_key`, `request_cost`, `cache`, and `cache_ttl`. `openai-responses` accepts
`send_cache_key`; its reasoning format and encrypted round-trip are fixed by the protocol.
Anthropic-style blocks accept `max_tokens`, `thinking_mode`, `thinking_budget`, `cache`, `cache_ttl`,
and `version`. Leave advanced fields unset unless the endpoint documents them.

Custom providers read only their own block. Global `HAX_OPENAI_*` and `HAX_ANTHROPIC_*` settings do
not bleed into them; only the variable named by `api_key_env` is read. Provider names cannot contain
`.` and cannot override a compiled-in provider.

## Mock provider

`mock` is a development backend with no network or model. It is hidden from auto-selection and the
provider picker but can be selected explicitly:

```sh
HAX_PROVIDER=mock hax
HAX_PROVIDER=mock HAX_MOCK_SCRIPT=scripts/mock/demo.txt hax
```

See [Troubleshooting and development diagnostics](./debugging.md#mock-provider) for its script format.
