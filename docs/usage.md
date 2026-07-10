# Usage

## CLI modes

With no arguments, `hax` starts an interactive REPL.

```sh
hax                         # interactive
hax -p "fix the failing test" # one-shot mode
printf "explain x" | hax -p # one-shot prompt from stdin
```

Options:

| Option | Meaning |
| --- | --- |
| `-p`, `--print` | Run the prompt to completion and print the final assistant message to stdout. |
| `-c`, `--continue` | Resume the newest session recorded for the current directory. |
| `--resume` | Pick a past session for the current directory. |
| `--resume=ID` | Resume a specific session id or unique prefix. Also works with `-p`. |
| `--raw` | Send only the prompt: no system prompt, env block, AGENTS.md, skills, or tools. |
| `-h`, `--help` | Show CLI help. |

In `-p` mode, positional arguments are joined with spaces. If no positional arguments are
present and stdin is not a terminal, stdin becomes the prompt. A bare `-p` on a terminal is an
error.

`-p` prints a one-line `provider · model · effort` banner to stderr before the run starts, so
the backend that produced the answer is always visible; stdout carries only the answer.
Silence it with `2>/dev/null` if needed.

`--resume` without an id opens a picker, so `-p --resume` requires `--resume=ID` instead.

## Sessions

Every non-empty conversation is recorded as append-only JSONL under:

```text
${XDG_STATE_HOME:-$HOME/.local/state}/hax/sessions/<encoded-cwd>/
```

Sessions are keyed by working directory. `-c`, `--resume`, and `/resume` only list sessions
for the directory you are currently in.

Resuming appends to the same session file. `/new` starts a fresh session id. On exit, hax
prints a resume hint such as:

```text
resume with: hax --resume=<id>
```

In `-p` mode this hint goes to stderr so stdout stays suitable for piping. Set
`HAX_NO_SESSION=1` to disable session recording.

## REPL commands

Type `/help` in the REPL for the live command list and keyboard shortcuts.

| Command | Meaning |
| --- | --- |
| `/new` | Start a fresh conversation. |
| `/clear` | Alias for `/new`. |
| `/resume` | Pick and resume a past session for this directory. |
| `/provider` | Switch provider, then choose model and effort where applicable. |
| `/model` | Switch model for the current provider, then choose effort where applicable. |
| `/effort` | Set reasoning effort when the provider exposes effort levels. |
| `/compact [focus]` | Summarize history to free context; optional focus text guides the summary. |
| `/copy` | Copy the latest assistant text response to the clipboard. |
| `/session` | Show this session's info and local usage totals (tokens, time worked, spend). |
| `/usage` | Show provider account usage (subscription windows, key credits) when supported. |
| `/help` | Show commands and shortcuts. |

A line beginning with `/` is only treated as a command when the first token is a bare command
name. Paths like `/tmp/repro.c crashes` pass through to the model.

## Stats line

After each user turn the REPL prints a dim one-line summary:

```text
8.9k / 256k (3%) · 42s · $0.042
```

- The leading figure is context usage as the last response reported it, with the window size
  and percentage when the context limit is known (when it isn't, the bare count is labeled:
  `context 8.9k`).
- The duration is wall-clock time for that user turn, including tool runs.
- The dollar amount is the session's cumulative spend. When the provider reports per-response
  cost (currently OpenRouter), the figure is exact. Otherwise, for providers with a model
  catalog identity (`codex`, `openai`, `anthropic`, and custom providers — see `catalog_id`
  in [providers.md](./providers.md)),
  hax estimates it from the reported token counts and per-model rates fetched from
  [models.dev](https://models.dev), shown with a tilde: `~$0.042`. For subscription backends
  like Codex this is the API-equivalent cost — what the same tokens would have billed on the
  paid API — which makes cost/benefit comparisons across models possible. Local backends
  (llama.cpp, ollama) show no dollar figure. See the model catalog section in
  [configuration.md](./configuration.md) for tuning or disabling the catalog fetch.

Set `HAX_STATS_VERBOSE=1` for the fully labeled diagnostic form, adding `out` (tokens
generated this user turn) and `cached` (prefix-cache hits) — useful for diagnosing cache
behavior:

```text
context 8.9k / 256k (3%) · out 595 · cached 2.7k · worked 42s · spent $0.042
```

On narrow terminals the line wraps between fields rather than mid-number.

Once a user turn has been running for 30 seconds, the busy spinner shows the same elapsed
counter live (`⠋ 42s · working...`), so a long-running user turn's age is visible before the
stats line lands.

`/session` shows the cumulative counterpart: user turns, model requests, tool calls (with a
per-tool breakdown), time worked, current context usage, token totals, and spend for the
current sitting. The `tokens total` row sums across every request — each request resends the
full conversation, so total `in` grows faster than `context` — and the cache percentage is the
hit rate of summed cached tokens against summed input. Compaction requests (manual `/compact`
or automatic) count like any other request, in both the request count and the token/spend
totals. Totals reset on `/new` and are not carried across `--resume`. `/usage` is different —
it asks the provider what it knows about your account (Codex plan windows, OpenRouter key
credits).

In `-p` mode, an equivalent stats line is printed to stderr at the end of the run, above the
resume hint, whenever the backend reported usage.

## Keyboard shortcuts

The REPL supports readline-style editing. Hax-specific or notable bindings:

| Key | Meaning |
| --- | --- |
| Enter | Submit prompt. |
| Shift-Enter | Insert newline, if your terminal sends LF for Shift-Enter. |
| Esc | Interrupt the model or a running tool. |
| Ctrl-C | Cancel the current prompt line. |
| Ctrl-D | Quit on an empty prompt. |
| Ctrl-G | Edit the prompt in `$EDITOR`. |
| Ctrl-T | View the current transcript in `$PAGER`. |
| Ctrl-L | Clear screen and redraw the prompt. |

## Prompt context

Unless `--raw` is used, hax sends:

1. the built-in system prompt, or `HAX_SYSTEM_PROMPT` if set;
2. an `<env>` block with platform, cwd, shell, model, preferred commands, and related
   process facts;
3. discovered `AGENTS.md` files and skill descriptions; and
4. tool schemas for `read`, `bash`, `write`, and `edit`.

`HAX_SYSTEM_PROMPT=""` omits only the system message; tools still remain available. `--raw`
omits the system prompt, env block, AGENTS.md/skills, and tools.

AGENTS.md discovery loads the global file first:

```text
${XDG_CONFIG_HOME:-$HOME/.config}/hax/AGENTS.md
```

For projects inside a git worktree, hax then loads AGENTS.md files from the repo root down to
the current directory. Outside a git worktree, it only considers `./AGENTS.md`. Skills are
discovered from `./.agents/skills/<name>/SKILL.md` and
`${XDG_CONFIG_HOME:-$HOME/.config}/hax/skills/<name>/SKILL.md`; set `HAX_NO_AGENTS_MD=1` to
skip both AGENTS.md and skills.

## Compaction

Hax can summarize earlier history to free context:

- `/compact` runs manual compaction.
- `/compact focus text...` adds focus instructions for the summary.
- `HAX_COMPACT_AUTO` controls automatic compaction near the context limit; it is on by default.
- `HAX_COMPACT_THRESHOLD` sets the auto-compaction trigger percentage; default `85`.

Automatic compaction needs a known context window, either from `HAX_CONTEXT_LIMIT` or a
provider-specific auto-probe. Without a known window, manual `/compact` still works.
