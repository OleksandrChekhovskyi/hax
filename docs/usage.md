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
| `--no-session` | Don't record this conversation: nothing to resume, and the prompts you type aren't added to Ctrl-R recall (earlier ones still recall). |
| `--raw` | Send only the prompt: no system prompt, Environment section, AGENTS.md, skills, or tools. Still recorded — a raw chat can be continued with `-c`. |
| `--bare` | Drop project and delegation context (AGENTS.md, skills, and the subagents section); the Environment section, tools, and base prompt remain, unlike `--raw`. |
| `--provider=NAME` | Select the backend for this run. Beats env vars, saved picks, and config. |
| `--model=ID` | Select the model for this run. Same precedence as `--provider`. |
| `--effort=LEVEL` | Select reasoning effort for this run. Same precedence as `--provider`. |
| `--preset=NAME` | Apply the `presets.NAME` selection from config (see [configuration.md](./configuration.md)). Explicit flags above still win. |
| `-h`, `--help` | Show CLI help. |
| `-v`, `--version` | Show version and exit. |

In `-p` mode, positional arguments are joined with spaces. If no positional arguments are
present and stdin is not a terminal, stdin becomes the prompt. A bare `-p` on a terminal is an
error.

`-p` prints a one-line `provider · model [· effort] [· session <id>]` banner to stderr before
the run starts, so the backend that produced the answer is always visible; stdout carries only
the answer. Silence it with `2>/dev/null` if needed. When recording is enabled, the session id
appears up front (not only in the exit hint) so a run that is killed mid-flight — say, by a
caller's timeout — can still be picked up with `--resume=<id>`.

`--resume` without an id opens a picker, so `-p --resume` requires `--resume=ID` instead.

## Sessions

Every non-empty conversation is recorded as append-only JSONL under:

```text
${XDG_STATE_HOME:-$HOME/.local/state}/hax/sessions/<encoded-cwd>/
```

Sessions are keyed by working directory. `-c`, `--resume`, and `/resume` only list sessions
for the directory you are currently in. Rows are labeled with the opening prompt; the selected
row also shows what the session started as — the banner's `[preset] provider · model · effort`,
then the git branch and HEAD commit subject at the time — followed by the rest of a prompt too
long for one row. Sessions inactive for 30 days are pruned automatically; selecting one to resume
refreshes its activity time. Configure `session_retention_days` or set it to `0` to keep sessions
indefinitely.

Resuming appends to the same session file. `/new` starts a fresh session id. On exit, hax
prints a resume hint such as:

```text
resume with: hax --resume=<id>
```

In `-p` mode this hint goes to stderr so stdout stays suitable for piping. Pass
`--no-session` to keep a conversation off disk: there is nothing to resume afterwards, and
the prompts you type aren't added to Ctrl-R recall. Both stores stay readable — resuming an
older session and recalling an earlier prompt work as usual; this run just doesn't add to
either.

### What resuming restores

Resuming restores the provider, model, effort, and preset the conversation was last using —
a mid-session `/model` or `/preset` switch included — so it continues where it left off
instead of on whatever this run happens to be set to. Environment variables and saved picks
don't redirect it; the selection flags do:

```sh
hax --resume=<id> --model=<other>       # swaps the model
hax --resume=<id> --provider=<other>    # the recorded model goes with its provider
hax --resume=<id> --preset=<other>      # swaps the whole preset
```

A run redirected that way is itself recorded, so the next resume picks up from there rather
than snapping back. Naming a provider, model, or effort leaves the conversation's preset
behind — a preset is a whole selection, so it can't be half-kept, the same way an explicit
`/model` exits a preset mid-session. Use `--preset` to move between presets.

The `-p` banner marks a restored selection `(resumed)`, and `/resume` reports the switch in
the REPL. If the recorded provider can't be used (logged out, server down, a provider that
no longer exists) or its preset has since been deleted, hax says so rather than quietly
answering from something else: `-p` exits with the reason, and the REPL opens the
conversation with the problem called out so you can choose where to continue it.

## REPL commands

Type `/help` in the REPL for the live command list and keyboard shortcuts.

| Command | Meaning |
| --- | --- |
| `/new [preset]` | Start a fresh conversation, optionally switching to a config-defined preset in the same step (the `/new` + `/preset` pair as one command, with one banner). A preset that doesn't apply leaves the conversation untouched. |
| `/clear` | Alias for `/new`. |
| `/resume` | Pick and resume a past session for this directory. |
| `/undo [n]` | Revert the conversation to before an earlier prompt: without an argument, pick one from a list; `n` counts turns back from the end (`/undo 1` drops the most recent). Destructive and not undoable — history and the session file are truncated in place, with no redo; the dropped prompt is left in editor recall (Up-arrow) to re-edit. |
| `/fork [n]` | Branch a new session before an earlier prompt, leaving the original whole and resumable. Without an argument, pick one from a list; `n` counts turns back from the end, and `/fork 0` clones the whole conversation at the current tip. |
| `/provider` | Switch provider, then choose model and effort where applicable. |
| `/model` | Switch model for the current provider, then choose effort where applicable. |
| `/effort` | Set reasoning effort when the provider exposes effort levels. |
| `/preset [name]` | Switch to a config-defined preset (shown as `[name]` in the banner, in the preset's `tint`); without a name, pick from a list. Persists by name; an explicit `/provider`, `/model`, or `/effort` pick exits it. |
| `/preset-save <name> [tint]` | Save what this session is running — provider, model, effort — as `presets.<name>` in `config.json`, then switch into it. Without a tint argument, a picker offers one (`none` keeps your own `tint`); an existing name asks before it's replaced. |
| `/config [key [value]]` | Inspect settings or change a runtime-tunable setting for this session. See [configuration.md](./configuration.md). |
| `/compact [focus]` | Summarize history to free context; optional focus text guides the summary. |
| `/copy` | Copy the latest assistant text response to the clipboard. |
| `/tasks [kill <id>... \| kill all]` | List background tasks (see below); `kill` stops the named ones (or all of them) — the model still learns their final state with its next prompt. |
| `/session` | Show this session's info and local usage totals (tokens, time worked, spend). |
| `/usage` | Show provider account usage (subscription windows, key credits) when supported. |
| `/help` | Show commands and shortcuts. |

A line beginning with `/` is only treated as a command when the first token is a bare command
name. Paths like `/tmp/repro.c crashes` pass through to the model.

## Stats line

After each user turn the REPL prints a dim one-line summary:

```text
42s · 8.9k / 256k (3%) · $0.042
```

- The duration is wall-clock time for that user turn, including tool runs.
- The next figure is context usage as the last response reported it, with the window size and
  percentage when the context limit is known (when it isn't, the bare count is labeled:
  `context 8.9k`).
- The dollar amount is the session's cumulative spend. When the provider reports per-response
  cost (currently OpenRouter), the figure is exact. Otherwise, for providers with a model
  catalog identity (`codex`, `openai`, `anthropic`, and custom providers — see `catalog_id`
  in [providers.md](./providers.md)),
  hax estimates it from the reported token counts and per-model rates — whatever the backend
  itself quotes for the model, falling back to [models.dev](https://models.dev) — shown with
  a tilde: `~$0.042`. A backend's own rates are preferred because they are what it will
  actually charge, a router's margin included. For subscription backends
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
42s · $0.19 · in 20.3k ~$0.025 · cache 160k ~$0.048 · write 8.2k ~$0.031 · out 2.1k ~$0.084
```

`in` is the uncached input remainder, `cache`/`write` are prefix-cache reads and writes.
On most backends the three sum to what the request sent, since a cached or written token is
one the model didn't have to process fresh. Not everywhere: where a cache write is billed as
a surcharge on top of input rather than in place of it (Google's), the same tokens are both
read and written and the counts deliberately overlap — each row is a charge, not a slice.
The component costs always carry a `~`:
no backend reports a decomposed charge, so the split is computed from rates even when the
total beside it is exact. The total carries a `~` only when it too is an estimate. That
asymmetry is the point — the total says what the turn cost, the categories say where it
went, and on a long agentic session the answer is usually the cache line. Estimates are
tier-aware: models with long-context pricing (e.g. different rates above 200k input tokens)
bill each request at the tier its own input size selects.

Once a user turn has been running for 30 seconds, the busy spinner shows the same elapsed
counter live (`⠋ 42s · working...`), so a long-running user turn's age is visible before the
stats line lands.

`/session` shows the cumulative counterpart: user turns, model requests, tool calls (with a
per-tool breakdown), time worked, current context usage, token totals, and spend for the
current sitting. The `tokens total` row sums across every request in the same categories the
transcript footers use — `in` (uncached input), `cache`/`write` (prefix-cache reads and
writes), `out` — each with its summed estimated cost where rates resolve, so the
session's overall cost breakdown reads at a glance (a large `cache` count next to a small
`in` is the prefix cache working). Each request resends the full conversation, so summed
input grows faster than `context`. As in the transcript footers, the category costs are
marked `~` even when the `spend` row is an exact provider-reported charge. Compaction
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
| Up / Down | Recall the previous / next prompt. Kept across runs, unless `--no-session`. |
| Ctrl-R | Search earlier prompts (incremental reverse search). |
| Esc | Pause after the current step (soft interrupt): in-flight work finishes, then the prompt returns. |
| Esc Esc | Interrupt the model or a running tool immediately. |
| Ctrl-C | Clear the prompt line (recallable with Up); twice on an empty prompt to quit. |
| Ctrl-D | Quit on an empty prompt. |
| Ctrl-L | Clear screen and redraw the prompt. |
| Ctrl-G | Edit the prompt in `$EDITOR`. |
| Ctrl-O | View the conversation so far in `$PAGER`, rendered as it was on screen. |
| Ctrl-T | View the model-facing transcript in `$PAGER`. |
| Ctrl-V | Paste an image from the clipboard (falls back to pasting clipboard text). |
| Tab | On an `@`-prefixed word: pick a project file to mention. Elsewhere: insert a tab. |

Typing `@src` and pressing Tab opens [`fzf`](https://github.com/junegunn/fzf) over the
project's files (tracked + untracked-but-not-ignored in a git repo; pruned `find`
elsewhere), with the token seeding the filter. `@../…`, `@~/…`, and absolute paths start
the walk at that directory and keep the typed prefix on the selection; in-tree paths like
`@src/foo` stay on the project list. The pick replaces the `@…` token; cancel leaves it
alone. Inserted as plain text — the model reads the file with its `read` tool as usual.
fzf on `$PATH` is required: without it Tab prints a short notice, and `/help` dims the row.

Ctrl-V pastes an image from the clipboard (on macOS plain Cmd+V works too): the image lands
in a temp file and a `[pasted image: …]` marker is inserted into the prompt — the model reads
it like any other image path. With no image on the clipboard, Ctrl-V pastes its text instead,
and copied files (file manager copy, drag-and-drop) paste as plain paths. On Linux this needs
`wl-paste` (Wayland) or `xclip` (X11); macOS works out of the box. The temp files are cleaned
up on `/new` and exit.

## Reviewing the conversation

Two paged views, deliberately different:

- **Ctrl-O — the conversation.** What was on screen: your prompts with their accent stripe,
  the model's answers with Markdown rendering and wrapping, tool calls with their output
  previews. Opens with a two-row banner: the provider, model, effort and preset the
  conversation is on — the *current* selection, since a mid-conversation `/model` or
  `/preset` switch is a display hint and never enters history — then the view's own label and
  prompt count, in the slot the REPL's key tips occupy. Reasoning appears when
  `show_reasoning` is on, so enabling it and pressing Ctrl-O shows the reasoning for turns
  that were displayed without it. Scroll and search it in the pager without touching your
  terminal scrollback.
- **Ctrl-T — the transcript.** What the *model* sees: the system prompt, every advertised
  tool schema, every tool argument and result verbatim and uncapped, plus per-request stats
  footers. After a compaction it starts at the summary, because that is where the model's
  context now begins. The debugging view (see [debugging.md](./debugging.md)).

Both use `$PAGER`, defaulting to `less -R` so colors survive.

Tool output in the Ctrl-O view is rebuilt from what the conversation stored, so for `bash` the
preview reflects the output the model received — already capped, with `bash`'s own
`[output truncated: …]` marker inside it. Expect elision counts to differ from the ones shown
live: those counted the raw stream. A `write` that created a file replays its content from the
call arguments, since that is what was on screen and the result recorded only a one-line
summary. Exploration calls (`read`, a `bash` command classified as exploration) stay
one-liners, as they were live.

After `/resume`, `/undo`, or `/fork`, the last turn is replayed inline above the prompt with
tool calls collapsed — one turn, deliberately, since the replay lands below everything
already on screen. The dim rule counts the messages it isn't showing and points at Ctrl-O
for the rest.

## Pausing, steering, and resuming

While the model is working, the first Esc requests a *soft* pause: nothing in flight is
cancelled — the streaming response finishes and any tool calls it made run to completion — and
the loop then stops at the next clean turn boundary and returns to the normal prompt. The one
exception is a request that has produced no output yet (the model is still processing the
prompt): there is nothing a pause could lose, so it is stopped right away and simply re-sent on
resume — without this, an Esc pressed during that silence would ride through a whole extra
turn, tools included, before pausing. A dim hint above the prompt
(`[paused — enter to continue]`) explains the state:

- **Enter on an empty prompt** resumes the turn exactly where it stopped, adding nothing to
  the conversation.
- **Typing a message** steers: it lands as an ordinary user message at the boundary — after
  the completed tool results, before the next model request — so it is written against the
  state you can see, never against work that happened after you hit send.
- **Slash commands work normally** at the paused prompt: `/model` to switch models
  mid-task, `/compact`, `/session`, and the rest. The hint reappears with the prompt until
  the turn is resumed or the state is cleared by a history-rewriting command (`/new`,
  `/undo`, `/fork`, `/resume`, a manual `/compact`).

A second Esc escalates to a hard interrupt — the stream is aborted, a running tool is killed,
and the partial turn is repaired with `[interrupted]` markers so history stays well-formed.
Hard-interrupted and provider-errored turns are resumable the same way: Enter continues
(recorded as a terse `[continue]` user message when the transcript ends in `[interrupted]`
markers, as a silent retry after a clean pre-stream failure), while a typed message simply
takes the turn in a new direction instead. `max_turns` (see
[configuration.md](./configuration.md)) stops the loop at the same kind of boundary every N
round-trips, turning the pause into a periodic check-in.

## Prompt context

Unless `--raw` is used, hax sends:

1. the built-in system prompt (or your `system_prompt` replacement), plus any
   `system_prompt_append` text;
2. an Environment section with working/home directories, operating system, command shell,
   model, Git root, and command preferences;
3. discovered `AGENTS.md` files and skill descriptions; and
4. tool schemas for `read`, `edit`, `write`, and `bash`.

Replacing or emptying the system prompt affects only item 1; the other sections still follow
(see the `no_*` settings in [configuration.md](./configuration.md)). `HAX_SYSTEM_PROMPT="(none)"`
omits the whole system message while tools remain available. `--raw` omits the system prompt,
Environment section, AGENTS.md/skills, and tools.

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

## Background tasks

A bash command that outlives its timeout is not killed: it detaches into a background task and
keeps running, with its output accumulating in a log file. The model can also detach a command
up front (`background: true`, optionally with a short task name), wait on one task at a time —
streaming its output live, exactly like a foreground command — and stop tasks it no longer
needs. A backgrounded command that finishes within its brief initial-output window returns
synchronously instead, reporting that no task was created. When a task finishes while the model
is busy elsewhere, a one-line note is delivered with the model's next request, so nothing needs
polling. A task tracks the shell itself, so processes that detach from it (a trailing `&`) are
killed when the shell exits rather than left running untracked.

Tasks belong to the conversation that started them: `/tasks` lists and stops them, and leaving
that conversation — `/new`, `/resume`, or quitting — stops the running ones and records each
task's final state in the session file being left behind. Dying by a signal kills the running
tasks too, but records nothing. In `-p` mode any task nobody waited on is killed once the final
answer is printed.

`no_tasks` / `HAX_NO_TASKS=1` disables the whole mechanism, restoring kill-at-timeout bash.
See [configuration.md](./configuration.md) for the related settings.

## Subagents

The system prompt includes a short section telling the model it can delegate a self-contained
task to a fresh hax instance — `hax -p "<task>"` through its own bash tool — and to do so only
when the user asks for it. There is no dedicated subagent machinery: a subagent is just hax
run by hax, so everything above about `-p`, sessions, and resume applies to it.

- The child inherits the parent's exact provider, model, and effort: the bash tool exports the
  parent run's effective selection as `HAX_PROVIDER` / `HAX_MODEL` / `HAX_EFFORT`
  for its children, so even unpersisted picks (an auto-selected provider, a mid-session
  `/model`) carry over.
- A different role or backend per subagent is one flag away: `--preset review`, or explicit
  `--provider` / `--model` / `--effort`; `--bare` makes a cheap scout without project context.
  Only `--preset` (with the defined presets and their descriptions) is advertised to the model —
  preset values are user-vetted, whereas the explicit flags would have it guess identifiers
  it can't enumerate. To make the model use a specific setup, name the flags in AGENTS.md or
  a skill.
- Subagents run as background tasks (`background: true`), so several can explore in parallel
  while the parent waits on whichever result it needs next.
- The child's session id is printed to stderr at startup, so a subagent that dies with its
  task (a kill, a hax exit) still leaves a resumable session; the parent can continue it with
  `hax --resume=<id> -p "..."` instead of redoing the work. That follow-up runs on the earlier
  child's own provider, model, and preset rather than the inherited ones — resuming beats
  inheritance, as everywhere else. `--no-session` opts a throwaway query out of this.
- Nesting is capped: children run with `HAX_SUBAGENT_DEPTH` incremented, and hax refuses to
  start at depth 3 — a backstop against runaway recursive spawning.

## Compaction

Hax can summarize earlier history to free context:

- `/compact` runs manual compaction.
- `/compact focus text...` adds focus instructions for the summary.
- `HAX_COMPACT_AUTO` controls automatic compaction near the context limit; it is on by default.
- `HAX_COMPACT_THRESHOLD` sets the auto-compaction trigger percentage; default `85`.

Automatic compaction needs a known context window, from `HAX_CONTEXT_LIMIT`, a
provider-specific auto-probe, or the model catalog. Without a known window, manual `/compact`
still works.
