# hax

A minimalist coding agent in C. Multi-provider from day one, but v1 ships with a single
adapter that reuses your existing Codex / ChatGPT subscription.

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

`hax` reuses the OAuth token that the official `codex` CLI stores in
`~/.codex/auth.json`. If the token is expired, run `codex` once to refresh it,
then re-run `hax`.

```sh
./build/hax
```

Environment variables:

- `HAX_MODEL` — model id (default: `gpt-5.3-codex`)
- `HAX_SYSTEM_PROMPT` — override the built-in system prompt

## License

MIT.
