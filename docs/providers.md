# Providers

Select a provider with `HAX_PROVIDER`, the `provider` key in config, or `/provider` in the
REPL. Runtime selections are saved to `state.json` so they stick across runs without editing
`config.json`.

## Selection and auto-selection

Built-in providers, in auto-selection and picker order:

1. `codex`
2. `openai-compatible`
3. `anthropic-compatible`
4. `llama.cpp`
5. `openai`
6. `anthropic`
7. `openrouter`

Config-defined providers, including the built-in `ollama` recipe, are listed after those.
`mock` is intentionally hidden from auto-selection and `/provider`, but still works with
`HAX_PROVIDER=mock`.

If a provider is explicitly configured and cannot start, the interactive REPL opens without a
provider and points you at `/provider`; in `-p` mode the failure is fatal because there is no
picker. Cold start with no configured provider tries available providers in the order above,
in both modes. `-p` always prints a one-line `provider · model · effort` banner to stderr —
marked `(auto-selected)` when the provider was inferred rather than explicitly configured —
so the backend that answered is visible even in pipelines.

`/provider` shows unavailable providers dimmed with a reason. After picking a provider it also
walks through model and reasoning-effort selection when those are available.

## Model and effort selection

`HAX_MODEL` or the `model` config key sets the model. Providers with no default model require
one in `-p` mode; interactively, `/model` can pick one if the provider exposes a model list.

`HAX_REASONING_EFFORT` or `/effort` sets a provider-specific effort value. The picker only
offers effort levels when the current provider exposes a ladder. If the value is not accepted
by the current provider's ladder, hax falls back to that provider's default.

## Codex

`HAX_PROVIDER=codex` uses the ChatGPT/Codex backend and reuses the OAuth token stored by the
official `codex` CLI in `~/.codex/auth.json`.

```sh
HAX_PROVIDER=codex hax
```

If startup says the Codex CLI is not logged in, run the official `codex` CLI once to refresh
its auth file. Hax reads `model` and `model_reasoning_effort` from `~/.codex/config.toml` as
provider defaults. If no model is configured there or in hax config, the current built-in
fallback is `gpt-5.3-codex`.

Codex supports `/usage`. It also probes ChatGPT's model catalog in the background to discover
the selected model's context window for the percentage display; `HAX_CONTEXT_LIMIT` overrides
that.

## OpenAI

`HAX_PROVIDER=openai` uses `https://api.openai.com/v1` and ignores `HAX_OPENAI_BASE_URL`.
This keeps an OpenAI API key from being sent to a third-party endpoint by accident.

```sh
HAX_PROVIDER=openai HAX_MODEL=<model> OPENAI_API_KEY=... hax
```

API-key lookup order:

1. `HAX_OPENAI_API_KEY`
2. `OPENAI_API_KEY`

OpenAI has no built-in model default in hax, so set `HAX_MODEL` or choose one with `/model`.
Prompt-cache keys are sent by default; disable with `HAX_OPENAI_SEND_CACHE_KEY=0`.

## OpenAI-compatible

`HAX_PROVIDER=openai-compatible` targets any endpoint with an OpenAI Chat Completions-style
`/v1/chat/completions` API. `HAX_OPENAI_BASE_URL` is required.

```sh
HAX_PROVIDER=openai-compatible \
HAX_PROVIDER_NAME=vLLM \
HAX_OPENAI_BASE_URL=http://127.0.0.1:8000/v1 \
HAX_MODEL=Qwen3-30B \
hax
```

This preset does not fall back to `OPENAI_API_KEY`; use `HAX_OPENAI_API_KEY` if the endpoint
needs a bearer token. Prompt-cache keys are off by default. `HAX_OPENAI_REASONING_FORMAT`
selects how effort is encoded: `flat` for `reasoning_effort` or `nested` for
`reasoning: {"effort": ...}`.

## Anthropic

`HAX_PROVIDER=anthropic` uses `https://api.anthropic.com/v1` and ignores
`HAX_ANTHROPIC_BASE_URL`.

```sh
HAX_PROVIDER=anthropic ANTHROPIC_API_KEY=... hax
```

API-key lookup order:

1. `HAX_ANTHROPIC_API_KEY`
2. `ANTHROPIC_API_KEY`

The real Anthropic preset has a built-in default model, but setting `HAX_MODEL` is still
recommended if you want a specific model. Its default thinking mode is `adaptive`, so
`/effort` offers `low`, `medium`, `high`, `xhigh`, and `max`.
Prompt cache-control breakpoints are sent by default; disable with `HAX_ANTHROPIC_CACHE=0`.

Useful knobs:

- `HAX_ANTHROPIC_MAX_TOKENS` — max output tokens, including thinking and text. Default
  `32000`.
- `HAX_ANTHROPIC_THINKING_MODE` — `adaptive`, `budget`, or `off`.
- `HAX_ANTHROPIC_THINKING_BUDGET` — budget-mode thinking tokens.
- `HAX_ANTHROPIC_CACHE_TTL` — cache TTL, `5m` or `1h`; default `1h`.
- `HAX_ANTHROPIC_VERSION` — `anthropic-version` header; default `2023-06-01`.

## Anthropic-compatible

`HAX_PROVIDER=anthropic-compatible` targets an Anthropic Messages-style `/v1/messages`
endpoint. `HAX_ANTHROPIC_BASE_URL` is required.

```sh
HAX_PROVIDER=anthropic-compatible \
HAX_ANTHROPIC_BASE_URL=http://127.0.0.1:18080/v1 \
HAX_MODEL=local-model \
hax
```

This preset does not fall back to `ANTHROPIC_API_KEY`; use `HAX_ANTHROPIC_API_KEY` if the
endpoint needs an `x-api-key` token. It defaults to budget thinking, tolerates empty thinking
signatures used by some compat servers, and leaves cache-control off by default. Opt into
cache-control with `HAX_ANTHROPIC_CACHE=1`.

## llama.cpp

`HAX_PROVIDER=llama.cpp` is a convenience preset for local `llama-server`. It defaults to
`http://127.0.0.1:8080/v1`.

```sh
HAX_PROVIDER=llama.cpp hax
HAX_PROVIDER=llama.cpp HAX_LLAMACPP_PORT=9090 hax
```

`HAX_OPENAI_BASE_URL` overrides the URL entirely. If `llama-server` was started with
`--api-key`, set `HAX_OPENAI_API_KEY`; the key is used for discovery probes as well as chat
requests.

At startup hax reconciles the configured model against `/v1/models`. If the configured model
is served, it keeps it; otherwise it adopts the first served model. If the server is
unreachable and no model is configured, startup fails with the URL it tried. If `HAX_MODEL` is
set, hax trusts it and lets the first request surface any connection error.

The context-window percentage is probed from `/props` in the background. If `/props` is slow
or unavailable, only absolute token counts are shown. For long prompts, start llama-server
with a large enough `-c` / `--ctx-size`.

## ollama

`ollama` is a shipped config-provider recipe using ollama's OpenAI-compatible endpoint. It is
selected like any other provider:

```sh
HAX_PROVIDER=ollama HAX_MODEL=qwen3:8b hax
```

Default URL: `http://127.0.0.1:11434/v1`. Override it in `config.json`:

```json
{ "providers": { "ollama": { "base_url": "http://127.0.0.1:11500/v1" } } }
```

A model is required in one-shot mode. Set `HAX_MODEL` or `model` in config, or choose one
with `/model` interactively. Hax does not guess from `ollama list` because the OpenAI
endpoint's model catalog is a pulled-model list, not necessarily the model you intend to run.

Hax does not auto-detect ollama's runtime context window. Set `HAX_CONTEXT_LIMIT` to your
`OLLAMA_CONTEXT_LENGTH` if you want the percentage display. Ollama's default context is often
small for an agent prompt; if responses truncate with `length`, restart `ollama serve` with a
larger `OLLAMA_CONTEXT_LENGTH` or raise `num_ctx` on the model.

## OpenRouter

`HAX_PROVIDER=openrouter` uses `https://openrouter.ai/api/v1` and ignores
`HAX_OPENAI_BASE_URL`.

```sh
HAX_PROVIDER=openrouter \
HAX_MODEL=anthropic/claude-sonnet-4.6 \
OPENROUTER_API_KEY=... \
hax
```

API-key lookup order:

1. `HAX_OPENAI_API_KEY`
2. `OPENROUTER_API_KEY`

OpenRouter has no built-in model default in hax. For app attribution on OpenRouter's
dashboards, hax sends `HTTP-Referer` (the app identifier, defaulting to the hax project URL),
`X-Title: hax`, and `X-OpenRouter-Categories: cli-agent` by default. Override the referer and
title with `HAX_OPENROUTER_REFERER` and `HAX_OPENROUTER_TITLE`; set the referer to an empty
string to disable attribution entirely.

For the context percentage, hax probes
`/api/v1/models/<model>/endpoints` and uses the largest returned `context_length`.
`HAX_SHOW_REASONING=1` both requests OpenRouter reasoning output and shows reasoning deltas.

OpenRouter reports per-response cost, which feeds the spend figure on the stats line and in
`/session`. OpenRouter also supports `/usage`, showing the API key's total spend, the account's
remaining prepaid credits, and — when the key carries a per-key spending cap — the cap and its
remaining allowance.

## Custom providers

Any static OpenAI Chat Completions or Anthropic Messages endpoint can be added under the
`providers` object in `~/.config/hax/config.json`:

```json
{
  "providers": {
    "groq": {
      "base_url": "https://api.groq.com/openai/v1",
      "api_key_env": "GROQ_API_KEY"
    },
    "my-anthropic-proxy": {
      "api": "anthropic-messages",
      "base_url": "https://example.test/v1",
      "api_key_env": "MY_PROXY_KEY",
      "display_name": "Anthropic proxy"
    }
  }
}
```

Recognized common keys:

- `base_url` — required unless supplied by a built-in recipe such as `ollama`.
- `display_name` — banner label.
- `api` — `openai-completions` (default) or `anthropic-messages`.
- `api_key` — literal token. Prefer `api_key_env` for real secrets.
- `api_key_env` — environment variable holding the token.
- `sort_models` — sort the `/model` picker alphabetically instead of the server's catalog
  order. Default off.

OpenAI-style custom providers also recognize `reasoning_format` (`flat` or `nested`) and
`send_cache_key`. Anthropic-style custom providers recognize the Anthropic settings from
[configuration.md](./configuration.md): `max_tokens`, `thinking_mode`, `thinking_budget`,
`cache`, `cache_ttl`, and `version`.

Custom providers read only their own `providers.<name>` block. Global `HAX_OPENAI_*` or
`HAX_ANTHROPIC_*` variables do not bleed into them, except for the explicit environment
variable named by `api_key_env`.

Provider names cannot contain `.` because dots separate config-key path components. A custom
provider with the same name as a built-in is ignored in favor of the built-in.

## Mock

`HAX_PROVIDER=mock` is an in-process provider for local testing. It needs no network, API key,
or model.

```sh
HAX_PROVIDER=mock hax
HAX_PROVIDER=mock HAX_MOCK_SCRIPT=scripts/mock_demo.txt hax
```

With `HAX_MOCK_SCRIPT`, the mock provider plays a small line-based script. Without a script,
it uses simple heuristics: for example, a backtick-quoted command can trigger the real `bash`
tool so rendering and dispatch paths can be exercised without an LLM.
