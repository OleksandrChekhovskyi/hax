# hax

A minimalist coding agent in C. Multi-provider from day one; currently ships
with adapters for Codex / ChatGPT and any OpenAI-compatible Chat Completions
endpoint.

## Build

Requires `libcurl`, `jansson`, `libedit`, and `meson`.

```sh
# Linux (Debian/Ubuntu)
sudo apt install libcurl4-openssl-dev libjansson-dev libedit-dev meson ninja-build pkg-config

# macOS (Homebrew)
brew install jansson meson ninja pkg-config
# libcurl and libedit ship with macOS

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

- `HAX_PROVIDER` — `codex` (default) or `openai`
- `HAX_MODEL` — model id. Defaults to `gpt-5.3-codex` when using `codex`;
  required when using `openai` (hax exits with an error if it's unset)
- `HAX_SYSTEM_PROMPT` — override the built-in system prompt. Set to an empty
  string to send no system message at all (some OpenAI-compatible chat
  templates reject system messages)
- `HAX_OPENAI_BASE_URL` — optional for `openai`; defaults to
  `https://api.openai.com/v1`; set to point at a local or proxy endpoint
- `HAX_OPENAI_API_KEY` — optional for `openai`; sent as `Authorization: Bearer`.
  Falls back to `OPENAI_API_KEY` only when the resolved base URL targets real
  OpenAI (default or explicit `https://api.openai.com/...`), so a globally
  configured OpenAI key is never forwarded to a custom endpoint. May be
  omitted for local servers that don't require auth
- `HAX_PROVIDER_NAME` — optional display name for the `openai` provider
- `HAX_OPENAI_SEND_CACHE_KEY` — set to any non-empty value to send a stable
  per-session `prompt_cache_key` even when `HAX_OPENAI_BASE_URL` is custom.
  Useful for hosted OpenAI-compatible providers (Together, Fireworks, Groq,
  OpenRouter, etc.) whose prefix caching benefits from an affinity hint. Off
  by default for non-OpenAI URLs because some local servers (notably vLLM)
  reject unknown JSON fields. Always sent to real `api.openai.com`
- `HAX_TRACE` — path to a Markdown file that will receive a pretty-printed
  dump of every HTTP request, response status, and SSE event (Authorization
  redacted). Opened in append mode; `tail -f` works, but the file is most
  readable when opened in an editor that renders Markdown

## License

MIT.
