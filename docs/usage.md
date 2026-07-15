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
| `--no-session` | Don't record this conversation; there will be nothing to resume. |
| `--raw` | Send only the prompt: no system prompt, Environment section, AGENTS.md, skills, or tools. Still recorded — a raw chat can be continued with `-c`. |
| `--bare` | Drop project and delegation context (AGENTS.md, skills, and the subagents section); the Environment section, tools, and base prompt remain, unlike `--raw`. |
| `--provider=NAME` | Select the backend for this run. Beats env vars, saved picks, and config. |
| `--model=ID` | Select the model for this run. Same precedence as `--provider`. |
| `--effort=LEVEL` | Select reasoning effort for this run. Same precedence as `--provider`. |
| `--preset=NAME` | Apply the `presets.NAME` selection from config (see [configuration.md](./configuration.md)). Explicit flags above still win. |
| `-h`, `--help` | Show CLI help. |

In `-p` mode, positional arguments are joined with spaces. If no positional arguments are
present and stdin is not a terminal, stdin becomes the prompt. A bare `-p` on a terminal is an
error.

`-p` prints a one-line `provider · model · effort · session <id>` banner to stderr before the
run starts, so the backend that produced the answer is always visible; stdout carries only the
answer. Silence it with `2>/dev/null` if needed. The session id appears up front (not only in
the exit hint) so a run that is killed mid-flight — say, by a caller's timeout — can still be
picked up with `--resume=<id>`.

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
| `/undo [n]` | Revert the conversation to before an earlier prompt: without an argument, pick one from a list; `n` counts turns back from the end (`/undo 1` drops the most recent). Destructive and not undoable — history and the session file are truncated in place, with no redo; the dropped prompt is left in editor recall (Up-arrow) to re-edit. |
| `/fork [n]` | Branch a new session before an earlier prompt, leaving the original whole and resumable. Without an argument, pick one from a list; `n` counts turns back from the end, and `/fork 0` clones the whole conversation at the current tip. |
| `/provider` | Switch provider, then choose model and effort where applicable. |
| `/model` | Switch model for the current provider, then choose effort where applicable. |
| `/effort` | Set reasoning effort when the provider exposes effort levels. |
| `/preset [name]` | Switch to a config-defined preset (shown dim in the banner); without a name, pick from a list. Persists by name; an explicit `/provider`, `/model`, or `/effort` pick exits it. |
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

On narrow terminals the line wraps between fields rather than mid-number.

The per-request detail lives in the transcript (Ctrl-T, and the `HAX_TRANSCRIPT` mirror):
each model round-trip ends with a dim stats footer — time, the request's cost, then the
token categories with their estimated component costs — useful for seeing where a session's
spend actually goes (context replay vs. cache reads vs. output) and for diagnosing cache
behavior:

```text
42s · ~$0.19 · in 20.3k $0.025 · cache 160k $0.048 · write 8.2k $0.031 · out 2.1k $0.084
```

`in` is the uncached input remainder, `cache`/`write` are prefix-cache reads and writes, so
the counts sum to what the request actually sent. The `~` on the total marks a catalog
estimate and governs the whole line; when the provider reports an exact charge (OpenRouter),
the total is exact and the categories show bare token counts — a reported charge can't be
decomposed. Estimates are tier-aware: models with long-context pricing (e.g. different rates
above 200k input tokens) bill each request at the tier its own input size selects.

Once a user turn has been running for 30 seconds, the busy spinner shows the same elapsed
counter live (`⠋ 42s · working...`), so a long-running user turn's age is visible before the
stats line lands.

`/session` shows the cumulative counterpart: user turns, model requests, tool calls (with a
per-tool breakdown), time worked, current context usage, token totals, and spend for the
current sitting. The `tokens total` row sums across every request in the same categories the
transcript footers use — `in` (uncached input), `cache`/`write` (prefix-cache reads and
writes), `out` — each with its summed estimated cost where catalog rates resolve, so the
session's overall cost breakdown reads at a glance (a large `cache` count next to a small
`in` is the prefix cache working). Each request resends the full conversation, so summed
input grows faster than `context`. Provider-reported charges can't be decomposed, so their
categories show bare counts and the exact total stays on the `spend` row. Compaction
requests (manual `/compact` or automatic) count like any other request, in
both the request count and the token/spend totals. Totals reset on `/new` and are not carried
across `--resume`. `/usage` is different — it asks the provider what it knows about your
account (Codex plan windows, OpenRouter key credits).

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
| Ctrl-L | Clear screen and redraw the prompt. |
| Ctrl-G | Edit the prompt in `$EDITOR`. |
| Ctrl-T | View the current transcript in `$PAGER`. |
| Tab | On an `@`-prefixed word: pick a project file to mention. Elsewhere: insert a tab. |

Typing `@src` and pressing Tab opens [`fzf`](https://github.com/junegunn/fzf) over the
project's files (tracked and untracked-but-not-ignored inside a git repository; a pruned
`find` elsewhere), with the token pre-seeding the fuzzy filter. The selected path replaces the
`@src` token; cancelling leaves the prompt untouched. The path is inserted as plain text — the
model reads the file with its `read` tool as usual. fzf on `$PATH` is required: without it,
Tab on an `@` word prints a short notice instead, and `/help` shows the binding dimmed.

## Prompt context

Unless `--raw` is used, hax sends:

1. the built-in system prompt, or `HAX_SYSTEM_PROMPT` if set;
2. an Environment section with working/home directories, operating system, command shell,
   model, Git root, and command preferences;
3. discovered `AGENTS.md` files and skill descriptions; and
4. tool schemas for `read`, `bash`, `write`, and `edit`.

`HAX_SYSTEM_PROMPT=""` omits only the system message; tools still remain available. `--raw`
omits the system prompt, Environment section, AGENTS.md/skills, and tools.

AGENTS.md discovery loads the global file first:

```text
${XDG_CONFIG_HOME:-$HOME/.config}/hax/AGENTS.md
```

For projects inside a git worktree, hax then loads AGENTS.md files from the repo root down to
the current directory. Outside a git worktree, it only considers `./AGENTS.md`. Skills are
discovered from `./.agents/skills/<name>/SKILL.md` and
`${XDG_CONFIG_HOME:-$HOME/.config}/hax/skills/<name>/SKILL.md`. Each context section has its
own opt-out: `HAX_NO_ENV=1` skips Environment, `HAX_NO_AGENTS_MD=1` skips AGENTS.md,
`HAX_NO_SKILLS=1` skips the skills listing, and `HAX_NO_SUBAGENTS=1` skips the subagents
section described below. `--bare` sets the latter three while retaining Environment.

## Subagents

The system prompt includes a short section telling the model it can delegate a self-contained
task to a fresh hax instance — `hax -p "<task>"` through its own bash tool — and to do so only
when the user asks for it. There is no dedicated subagent machinery: a subagent is just hax
run by hax, so everything above about `-p`, sessions, and resume applies to it.

- The child inherits the parent's exact provider, model, and effort: the bash tool exports the
  parent session's effective selection as `HAX_PROVIDER` / `HAX_MODEL` / `HAX_EFFORT`
  for its children, so even session-only picks (an auto-selected provider, a mid-session
  `/model`) carry over.
- A different role or backend per subagent is one flag away: `--preset review`, or explicit
  `--provider` / `--model` / `--effort`; `--bare` makes a cheap scout without project context.
  Only `--preset` (with the defined presets and their descriptions) is advertised to the model —
  preset values are user-vetted, whereas the explicit flags would have it guess identifiers
  it can't enumerate. To make the model use a specific setup, name the flags in AGENTS.md or
  a skill.
- The child's session id is printed to stderr at startup, so a subagent that outlives the bash
  tool's timeout still leaves a resumable session; the parent can continue it with
  `hax --resume=<id> -p "..."` instead of redoing the work. Resume restores the conversation,
  not the selection (hax-wide behavior — `/resume` works the same), so a follow-up to a
  preset-backed child must repeat the original `--preset`/flags; the prompt section says so.
  `--no-session` opts a throwaway query out of this.
- Nesting is capped: children run with `HAX_SUBAGENT_DEPTH` incremented, and hax refuses to
  start at depth 3 — a backstop against runaway recursive spawning.

## Compaction

Hax can summarize earlier history to free context:

- `/compact` runs manual compaction.
- `/compact focus text...` adds focus instructions for the summary.
- `HAX_COMPACT_AUTO` controls automatic compaction near the context limit; it is on by default.
- `HAX_COMPACT_THRESHOLD` sets the auto-compaction trigger percentage; default `85`.

Automatic compaction needs a known context window, either from `HAX_CONTEXT_LIMIT` or a
provider-specific auto-probe. Without a known window, manual `/compact` still works.
