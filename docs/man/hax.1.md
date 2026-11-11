# hax(1)

## Synopsis

**hax** [*preset*] [*option*...]\
**hax** [*preset*] **-p** [*option*...] [*prompt*...]

## Description

**hax** is a coding agent for the terminal. It sends your prompt to a language model along with
context about your environment and project: the working directory, platform and shell, useful
command-line programs, `AGENTS.md` instructions, and available skills. It then runs the tools the
model calls — reading, editing, and writing files and executing shell commands — and feeds each
result back, until the model replies without calling another tool.

<!-- include ../usage.md -->

## Environment

Every setting has a `HAX_*` variable, listed in [hax-config(5)](../configuration.md); those
override saved choices and `config.json`. Variables hax reads beyond its own settings:

| Variable | Purpose |
| --- | --- |
| `OPENAI_API_KEY`, `ANTHROPIC_API_KEY`, `OPENROUTER_API_KEY`, `OPENCODE_API_KEY`, `DEEPSEEK_API_KEY`, `MINIMAX_API_KEY` | Credentials for the hosted providers; see [hax-providers(7)](../providers.md). |
| `VISUAL`, `EDITOR` | Editor for Ctrl-G, in that order of preference. |
| `PAGER` | Pager for Ctrl-O and Ctrl-T; `less -R` when unset. |
| `NO_COLOR` | Disables color under the default `theme=auto`. |
| `XDG_CONFIG_HOME`, `XDG_STATE_HOME`, `XDG_CACHE_HOME` | Base directories for the files below. |
| `CURL_CA_BUNDLE`, `SSL_CERT_FILE`, `SSL_CERT_DIR` | TLS certificate lookup. |

## Files

Paths are shown for unset `XDG_*` variables.

| Path | Purpose |
| --- | --- |
| `~/.config/hax/config.json` | User configuration; see [hax-config(5)](../configuration.md). |
| `~/.config/hax/AGENTS.md` | Instructions for every project, read before the project's own. |
| `~/.config/hax/skills/` | Skills offered only to hax. |
| `~/.local/state/hax/state.json` | Provider, model, and effort remembered from the pickers. |
| `~/.local/state/hax/auth.json` | Logins made with `/login`. |
| `~/.local/state/hax/sessions/` | Sessions and prompt history, one directory per working directory; see [hax-sessions(5)](../sessions.md). |
| `~/.cache/hax/catalog.json` | Cached model metadata: context limits, capabilities, prices. |

## Exit status

| Status | Meaning |
| --- | --- |
| 0 | The REPL exited normally, or a one-shot run completed. |
| 1 | An error: invalid usage, a provider failure, or a one-shot run that paused or hit `max_turns`. |
| 130 | A one-shot run was interrupted by SIGINT or SIGTERM. |

## See also

[hax-config(5)](../configuration.md), [hax-sessions(5)](../sessions.md),
[hax-providers(7)](../providers.md)

<https://github.com/OleksandrChekhovskyi/hax>
