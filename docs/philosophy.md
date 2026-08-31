# Philosophy

hax is a minimalist coding agent: one native C binary, a small dependency set, a few megabytes
of memory. This document records the general approach and then walks through specific decisions
that illustrate it — mostly features other agents ship that hax deliberately does not. The goal
is that "why doesn't hax have X?" usually has a written answer, along with the pattern that
covers the need.

## General approach

**Build genuinely useful things directly into the binary; skip questionable features even when
they are conventional.** A feature earns its place by being broadly useful in day-to-day work,
not by appearing in other agents' feature lists.

**Extensibility comes from substrates hax already has: config, markdown files, and
subprocesses.** There is no embedded scripting runtime, no plugin ABI, no extension marketplace.
What you can already shape without touching C:

- Providers and endpoints as config blocks — see [providers.md](./providers.md).
- Presets: named bundles of provider/model/effort/system-prompt; presets with descriptions can
  also serve as subagent roles — see [configuration.md](./configuration.md).
- System prompt base and append, inline or from a file (`@path`).
- Project instructions via the `AGENTS.md` hierarchy, and skills via `SKILL.md` directories —
  see [usage.md](./usage.md).

**hax composes as a Unix tool.** The one-shot mode (`hax -p`, clean stdout, resumable sessions)
is the extension point. Orchestration that would be a hook or plugin elsewhere is a script that
drives hax from the outside, where it can be versioned, inspected, and tested like any other
program. A driver that needs to observe a run — turns, tool calls, cost — gets `hax --json`, a
JSONL stream of the same records the session file holds ([sessions.md](./sessions.md)). This
covers observing and sequencing runs, not interception inside a turn — that gap is a deliberate
omission, discussed below.

**Prefer telling the model over building machinery around it.** Models follow project
instructions well. A convention documented in `AGENTS.md` or a skill usually replaces a feature.

## Decisions

### No MCP

A capability the model needs is a CLI tool with a README. The model already knows how to run
programs, read their help text, and recover from their errors; a skill (`SKILL.md`) tells it
when to reach for one. This keeps capabilities usable outside hax too — by you, by scripts, by
other agents — instead of locking them behind an agent-specific protocol, another long-running
process, and another config surface.

### No hooks or plugins

Event hooks elsewhere serve a handful of needs, each of which hax answers more directly:

- Notifications when the interactive REPL needs attention: built in (`notify`).
- Observability and debugging: JSONL sessions record the conversation history — `hax --json`
  streams the same records live for a driving process — `HAX_TRANSCRIPT` shows the model-facing
  context, and `HAX_TRACE` captures redacted HTTP/SSE diagnostics.
- Auto-formatting after edits: rewriting files under the model desynchronizes it from the tree
  it thinks it just edited. Tell the model to run the formatter (an `AGENTS.md` line); the
  change stays part of its own work.
- Quality gates and orchestration: drive hax from outside instead of injecting control flow
  inside a turn:

  ```sh
  hax -p "fix the failing tests"
  until make tests; do
      hax -c -p "tests are still failing, keep fixing"
  done
  ```

What outside scripts cannot express is in-turn interception: vetoing or rewriting a tool call,
or injecting behavior between a tool result and the next model request. hax omits that
deliberately rather than claiming a substitute — an in-process event system would add a
protocol, a config schema, and failure modes to the core loop, and its main use elsewhere is
per-command approval, covered next.

### No permission prompts

hax deliberately has no per-command approval gate. Such a gate can catch accidents, but it
earns its keep poorly: it interrupts every step of routine work and trains reflexive approval,
and because it runs inside the process it guards, it is not a security boundary — the same
shell, filesystem, and credentials remain one confused command away. Isolation against a
mistaken or compromised process comes from the operating system: run hax in a container or VM
when working on untrusted input or when the blast radius matters.

What hax provides instead is control and restraint, not enforcement: the default system prompt
forbids destructive git operations and tells the model to ask before irreversible actions, Esc
pauses a running turn for steering, and `max_turns` bounds unattended runs.

### No custom slash commands

Reusable prompts are files. Keep them in your repo (or anywhere) and `@`-mention one at the
prompt — the fzf picker makes them discoverable, and the model reads the file it is pointed at.
That is an instruction to the model rather than the deterministic text expansion a command
system performs, but the extra read is cheap and the outcome is the same in practice. Personal
repetition is already covered by prompt history (Ctrl+R, persisted across sessions), and
project conventions belong in `AGENTS.md`, where they apply without being invoked.

### Small dependency footprint

hax links only what it genuinely needs: libcurl for HTTPS, jansson for JSON, platform threads.
A dependency is a permanent tax on every build, port, and audit, so the default answer to "just
add a library" is no. One format serves both config and the wire because a JSON parser is
linked anyway — TOML or YAML would mean a second parser for a marginal gain in comfort.
Terminal handling and unified-diff generation are in-tree rather than ncurses or a diff library.
Where a separate program already does the job well, hax runs it instead of linking it — `fzf` for
the `@` file picker, and `$EDITOR` and `$PAGER` where they fit — the same out-of-process
composition the rest of the design leans on. What does get linked must be in Debian main and
either ship with macOS or be one `brew install` away, and must not be GPL.

## The bar for new features

When something seems missing, the questions are, in order: can an `AGENTS.md` line or a skill
cover it? Can a script around `hax -p` cover it? Would it be used routinely rather than
configured once and forgotten? Only then does it belong in the binary — implemented directly,
as a feature, not as a framework for features.
