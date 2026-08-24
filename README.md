<div align="center">

# hax

**A minimalist, terminal-native coding agent written in C.**

<img src="./docs/screenshot.png" width="720"
     alt="hax answering a question about its own source using a local llama.cpp model">

</div>

## Key features

- **Lightweight by design** — A single native C binary with a small dependency set.
  Starts instantly and uses only a few MB of memory, leaving more RAM for local models.
- **Local models are first-class** — Start `llama-server -m [model].gguf`, then
  `hax --provider llama.cpp`, and hax auto-discovers the model and runtime capabilities.
  No custom provider config block is needed for the default setup.
- **Respects your terminal** — Streaming Markdown and live tool output, reflowed for display
  in the terminal. Only redraws the current streaming line or the input area, native scrollback
  is preserved. Does not take over or mess with your terminal.
- **Inspectable** — See exactly what was sent to the model and what it replied in a usable
  transcript view (Ctrl+T). Optionally collect a detailed wire protocol trace.
- **Broad provider support** — OpenAI (+compatible), Anthropic (+compatible), Codex (via a ChatGPT
  subscription), OpenRouter, OpenCode Zen/Go, llama.cpp, and custom endpoints.
- **Well-behaved Unix tool** — XDG paths, clean stdout in `-p` one-shot mode with resume hints on
  stderr, plain-text config and session files, composition via subprocesses instead of plugins.

## Target audience

Developers who live in the terminal, run local models, audit what their tools do, package
software for distros, or run agents where resources are scarce. If you want MCP marketplaces, a
plugin runtime, IDE panels, or per-command permission prompts, other agents build exactly that —
hax deliberately doesn't, and [docs/philosophy.md](./docs/philosophy.md) explains each omission
and the pattern that covers the need.

If "fancy new AI tech in an old-school minimalist package" sounds like your vibe, you might
like this.

## Install

hax runs on Linux, macOS, FreeBSD, and OpenBSD; on Windows, use it under
[WSL](https://learn.microsoft.com/en-us/windows/wsl/). The BSDs build from source only.

With [Homebrew](https://brew.sh) (macOS or Linux):

```sh
brew install oleksandrchekhovskyi/hax/hax
```

On Arch Linux, hax is in the [AUR](https://aur.archlinux.org/packages/hax) as `hax`.

On any Linux distribution, download the prebuilt static binary for your architecture (x86_64 or
aarch64) from the [latest release](https://github.com/OleksandrChekhovskyi/hax/releases/latest),
then unpack the `hax` binary into any directory on your `PATH`.

### From source

Building from source gives a binary linked against your system's shared libraries instead of
a static one:

```sh
git clone https://github.com/OleksandrChekhovskyi/hax.git
cd hax
scripts/install_deps.sh   # Debian/Ubuntu, Fedora, Arch, openSUSE, Alpine, macOS, FreeBSD, OpenBSD
make                      # the binary is now at ./build/hax
make install              # optional; may prompt for sudo
```

`scripts/install_deps.sh` installs the build dependencies — a C compiler, `libcurl`,
`jansson`, `meson`, `ninja`, and `pkg-config` — plus `fzf`, which hax uses for `@file`
completion when available. On other platforms, install those packages by hand and run `make`.

For hacking on hax, `make symlink` links the freshly built binary into `~/.local/bin` so it
stays on `PATH` across rebuilds. `make lint` additionally needs `clang-format` and
`clang-tidy` (`scripts/install_deps.sh lint` installs them).

The examples below use `hax` as if it is on `PATH`; after a plain build, use `./build/hax`.

## Connect a provider

The easiest first run is interactive: start `hax`, then use `/provider` to see available providers
and choose a model. hax remembers interactive provider, model, and effort selections.

| Provider | Setup |
| --- | --- |
| `codex` | Run `/login` inside hax. |
| `openai` | Set `OPENAI_API_KEY`. |
| `anthropic` | Set `ANTHROPIC_API_KEY`. |
| `openrouter` | Set `OPENROUTER_API_KEY`. |
| `opencode-zen` / `opencode-go` | Set `OPENCODE_API_KEY`. |
| `llama.cpp` | Run `llama-server`. |
| `ollama` | Run `ollama serve`. |
| Compatible or custom endpoint | See [docs/providers.md](./docs/providers.md). |

## Quick start

Run hax from the project directory you want it to work in:

```sh
hax                         # interactive REPL
hax -p "list TODOs"         # run one prompt and print the final answer
printf "explain x" | hax -p # read the prompt from stdin
hax -c                      # continue the latest session for this directory
hax --resume                # pick a past session for this directory
hax --resume=ID -p "next"   # resume a specific session in one-shot mode
```

Run `hax --help` for CLI usage. In the REPL, type `/help` for slash commands and shortcuts.
See [docs/usage.md](./docs/usage.md) for more detailed usage documentation.

## Configuration

Configuration is optional. Interactive selections are remembered separately from your config file;
CLI flags apply only to the current run, and environment variables are useful for shells and
scripts. Resumed conversations restore their own provider, model, effort, and preset unless a
CLI selection flag overrides them.

See [docs/configuration.md](./docs/configuration.md) for the file format, resolution order, presets,
and the setting reference.

## More docs

- [docs/debugging.md](./docs/debugging.md) — trace/transcript logs, mock provider, and demo scripts.
- [docs/embedding.md](./docs/embedding.md) — hosting hax in another program, and the Python binding.
- [CONTRIBUTING.md](./CONTRIBUTING.md) — how to propose, prepare, and submit a change.

## License

MIT.
