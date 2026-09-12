# Ask-user-question tool plan

Add an `ask_user_question` tool so the model can present structured questions — typed
options with a free-text fallback — instead of ending the turn with prose. Modeled on the
pi `ask-user-question` extension (~/code/pi-extensions), scoped to its local-TUI behavior.

## Scope

In scope (the local-TUI core of the pi extension):

- Tool `ask_user_question`: `questions[]` of `{prompt, options[], allowOther}`, with
  `label`/`value`/`description` per option and `id`/`label` per question.
- Single question: option list, navigate, select.
- Multiple questions: tab bar with answered marks, tab to review/revise answers, final
  Submit tab (Enter only when all answered).
- "Type something." free-text option per question (`allowOther`, default true).
- Esc cancels the pending question (result: `User cancelled the question`).
- Non-TTY: recoverable error result naming the constraint.

Out of scope (pi features that need infrastructure hax deliberately lacks):

- Remote answer relay (the `rc` extension's client/transport).
- Subagent question relay (needs RPC-mode children; hax subagents are one-shot
  `hax -p` processes).
- One pending ask at a time is automatic here: dispatch is sequential and the REPL has no
  re-entrancy, so no singleton machinery is needed.

## Tool contract

Name: `ask_user_question`.

Parameters:

```json
{
  "questions": [
    {
      "id": "q1",
      "label": "Scope",
      "prompt": "Which scope should the fix cover?",
      "options": [
        { "label": "This file only", "value": "file", "description": "..." },
        { "label": "Repo-wide", "value": "repo" }
      ],
      "allowOther": true
    }
  ]
}
```

Normalization (all defaults, mirroring pi): question `id`/`label` default to `Q1`, `Q2`,
...; option `value` defaults to `label`; `allowOther` defaults to `true`; `description`
optional. Cap at 5 questions and 9 options each — beyond that, validation error result.

Result to model (one line per question, pi format):

```
Q1: user selected: 1. This file only
Q2: user wrote: the free-text answer
```

- `user wrote:` for free-text answers; selected labels with their 1-based option index.
- If a selected option's `value` differs from its `label`, append ` (value: <value>)` — the
  model consumes this text and must see the value it asked for by key.
- Cancellation: `User cancelled the question`.

All of this is a recoverable outcome, not a failure: the model adapts (proceeds with a
default, asks differently). No `is_error`-style marker.

## Components

### 1. `src/terminal/question.h/.c` — the multi-question widget

Blocking, TTY-only, same contract as `picker_run` (terminal/picker.h:32): borrowed inputs,
owns raw mode, returns on answer or cancel.

```c
struct ask_question_option {
    const char *label;
    const char *value;      /* defaults to label when NULL/empty */
    const char *description;
};

struct ask_question {
    const char *id;
    const char *label;
    const char *prompt;
    const struct ask_question_option *options;
    size_t n_options;
    int allow_other;
};

struct ask_answer {
    const char *question_id;
    int index;        /* 0-based option index, -1 for free text */
    char *text;       /* option label or free text; owned */
    char *value;      /* option value; owned */
    int was_custom;
};

struct ask_result {
    int cancelled;
    struct ask_answer *answers;  /* owned, in question order, one per answered question */
    size_t n_answers;
};

int question_run(const struct ask_question *questions, size_t n_questions);
```

(`question_run` fills a caller-allocated `struct ask_result`, or returns one — the
implementer picks the ownership shape that fits `picker_run`'s precedent, and the plan
doesn't care which.)

Layout, per question tab:

```
 ─────────────────────────────────────────────
  Q2 of 3 · Scope
  Which scope should the fix cover?

   1. This file only
      only touch src/foo.c
   > 2. Repo-wide
   3. Type something.

  ↑↓ move  Tab next  Enter select  Esc cancel
 ─────────────────────────────────────────────
```

- Tab bar row (only when `n_questions > 1`), above the question: one cell per question —
  `1.` ... `N.` plus `Submit` — current cell highlighted; answered cells marked (e.g.
  `✓` or the selected label truncated into the cell). Mirrors pi's tab bar semantics:
  Tab/← moves forward, Shift+Tab/→ moves back, wraparound; the Submit tab is reachable
  at any time and shows a summary list of all current answers (free text marked
  `(wrote)`); Enter on the Submit tab submits only when every question has an answer,
  otherwise the hint row says which are missing.
- Selecting an option records the answer and advances to the next question tab (or
  Submit on the last one). Revisiting a tab with an answer pre-selects it; selecting a
  different option replaces the answer.
- Free-text option: Enter on `Type something.` switches to a single-line input mode in
  the option area (reuses the terminal input primitives; Esc returns to the list, Enter
  submits the typed text; empty input is rejected with a hint). The prompt and options
  stay visible above the input line.
- Esc at any point (option list, tab bar, free text) cancels: result `cancelled = 1`.
- Rendering: reuse `terminal/ansi.h` + `theme` roles (accent for the chrome bar and
  selection, dim for hints, muted for descriptions) the same way `picker.c` does; reuse
  its raw-mode enable/disable and its frame/repaint approach rather than inventing a new
  one. `terminal/width.h` for display-cell math; wrap long prompts/options with the
  existing wrapping helpers used by `render/` (e.g. `markdown_wrap`'s line-wrap or the
  picker's own clipping — whatever the picker already uses).
- Non-TTY or terminal setup failure: return the cancelled/error state so the tool layer
  can emit its recoverable result; never block.

### 2. `src/tools/ask.c` — the tool

- `const struct tool TOOL_ASK` (tool.h registration pattern, like TOOL_READ).
- `run()`:
  - Parse + normalize args with jansson; on malformed input return a recoverable error
    string naming the problem (bad types, missing `prompt`, empty `options`, over the
    caps). This path is TTY-independent and unit-tested.
  - `isatty` check on stdin/stdout (same convention as `picker_run`): when not a TTY,
    return the recoverable result:
    `This run's terminal is not interactive, so the question cannot be asked; answer
    differently or proceed with your best default.`
  - TTY path: `interrupt_disarm()` → `question_run(...)` → `interrupt_arm()` (the
    interrupt watcher thread owns stdin while armed; the picker owns it while running —
    same arm/disarm discipline the REPL uses around `agent_loop_run`, agent.c). Clear any
    interrupt requests the picker window swallowed, then re-arm, so the run's pause/abort
    semantics restart clean. Build the model-facing result text (format above).
- `display` hooks (agent_dispatch header integration):
  - `format_argument`: `"3 questions (Scope, Priority, Rollout)"` — labels comma-joined,
    truncated.
  - `preview_mode`: `TOOL_PREVIEW_COLLAPSED` is wrong (the call must show before a long
    interactive window); use `TOOL_PREVIEW_HEAD` with `header_rows = 1` — the default.
    Verify against how collapsed clusters treat unknown tool names (agent_dispatch.c
    `write_cluster_line` only coalesces `read`).
- `advertise`: always (no config gate). A config kill-switch is a candidate
  (`tools.ask = false`), but check whether any other tool has one first; if the pattern
  doesn't exist, skip it — don't invent the first kill-switch for this tool alone.

### 3. Wiring

- `src/tool.h`: `extern const struct tool TOOL_ASK;`
- `src/agent_core.c`: add `&TOOL_ASK` to `TOOLS[]`.
- System prompt, `DEFAULT_SYSTEM_PROMPT` (agent_core.c:25), extend the "Ask only when
  genuinely blocked" paragraph with one sentence: when asking, call `ask_user_question`
  with a short prompt and a few concrete options, best answer first — prose questions are
  the fallback when the tool is unavailable. Keep the paragraph's existing "one targeted
  question and a recommended default" guidance (the recommended default is option 1).
- `tests/meson.build`: `tools/ask.c` into the tools group; new `tests/test_ask.c`.
- `meson.build`: `src/terminal/question.c` + `src/tools/ask.c` in sources.

### 4. Tests (`tests/test_ask.c`)

TTY-independent surface only (picker itself is TTY-only, verified by hand with tmux per
AGENTS.md):

- `question_run` normalization helpers factored into testable pure functions if the
  implementer judges that worthwhile (id/label defaults, value fallback, caps) — prefer
  exposing the pure pieces over over-testing.
- `run()` with `HAX_...`/non-TTY: malformed args (missing prompt, empty options, >5
  questions, >9 options, non-array questions) each yield a distinct recoverable message;
  non-TTY yields the not-interactive result; a well-formed call with a forced non-TTY
  stdin (the harness sets this up; check how other tools fake isatty — if none do, drive
  `run()` via a test that redirects stdin to a file, matching `tests/` precedent).
- Result text formatting (the `user selected:` / `user wrote:` / `(value:)` assembly) as
  a pure helper, tested directly with hand-built answer structs.

## Verification (main agent, per AGENTS.md)

1. `make tests` clean; `make lint` clean.
2. `BUILD_DIR=build-asan make tests` clean (new code touches malloc-heavy territory).
3. tmux e2e with the mock provider: a scripted turn whose tool call is
   `ask_user_question` with 2 questions (one with `allowOther` true, one false), drive
   the picker by keys (select option, Tab to next, type free text, Esc on a second
   invocation → cancelled result), capture the pane, assert the model-facing result lines
   via `HAX_TRANSCRIPT`/session log.
4. Non-TTY: `./build/hax -p ... < /dev/null` path returns the recoverable error.
5. CHANGELOG entry under `[Unreleased]`.

## Not done (deliberate)

- No `notify` integration, no config key, no per-question "required" flag (pi has none).
- No persistence beyond the ordinary session log (the call + result are normal items).
- No i18n of the hint row; matches the rest of the UI.
