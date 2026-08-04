# Debugging

## Wire trace

Set `HAX_TRACE` to capture HTTP requests, response statuses, and SSE events:

```sh
HAX_TRACE=/tmp/hax-trace.md hax
```

The trace is plain Markdown-like text and is truncated at startup. Authorization is redacted.
Entries include elapsed-time tags so pauses between streamed chunks are visible. `HAX_TRACE`
records HTTP transport traffic, including provider requests, metadata probes, catalog refreshes,
and account-usage queries. It is silent for a mock-only run because no network is used.

## Transcript log

Set `HAX_TRANSCRIPT` to mirror the same model-facing transcript available from Ctrl-T:

```sh
HAX_TRANSCRIPT=/tmp/hax-transcript.txt hax
```

The transcript includes the system prompt, advertised tools, user/assistant items, tool calls,
tool results, and reasoning items where present. The file is truncated at startup and on
`/new`, then appended as the conversation grows. It is useful when debugging prompt/context
behavior rather than raw HTTP.

## Ctrl-T transcript view

In the REPL, press Ctrl-T to open the current transcript in `$PAGER`. This is an in-memory view
and does not require `HAX_TRANSCRIPT`.

Ctrl-O is its user-facing sibling: the same conversation as it was displayed (Markdown, tool
previews, no system prompt or tool schemas). Reach for Ctrl-T when the question is what the
model received, and Ctrl-O when it is what happened.

## Mock provider

The mock provider exercises dispatch and rendering without an LLM:

```sh
HAX_PROVIDER=mock hax
HAX_PROVIDER=mock HAX_MOCK_SCRIPT=scripts/mock/demo.txt hax
```

Without a script, it parses the latest user message heuristically. For example, typing
``run `ls -la` `` can trigger a real `bash` tool call.

With `HAX_MOCK_SCRIPT`, each provider `stream()` call consumes one scripted turn. Blank lines
and lines whose first non-whitespace character is `#` are ignored. The directives are:

```text
text <message>
reasoning <message>
space
tool <name> <json>
delay <ms>
usage in=N out=M [cached=K] [cache_write=W] [cache_write_1h=H] [cost=D]
end-turn
```

`text` and `reasoning` decode `\\n`, `\\t`, and `\\\\`, then stream the result in small deltas;
`reasoning` is displayed only with `HAX_SHOW_REASONING=1`. `space` emits one single-space text
delta. `delay` sets the pacing for later text, reasoning, space, and tool emissions; zero restores
burst mode. `tool` takes a name and a single-line JSON object. `usage` sets the accounting on the
turn's final event, and `end-turn` completes the turn. A final turn may end at EOF.

`{{CWD}}` in text or tool arguments expands to the process working directory, allowing checked-in
fixtures to exercise path normalization without hard-coding a machine-specific path.

Mock-provider fixtures live under `scripts/mock/`:

| Fixture | Use |
| --- | --- |
| `demo.txt` | End-to-end rendering and multi-turn tool dispatch. |
| `layout.txt` | Header, gutter, and Markdown layout. |
| `diff.txt` | Path normalization and diff rendering. |
| `pause.txt` | Pauses between streamed events. |
| `tasks.txt` | Background-task lifecycle. |
| `theme.txt` | Semantic color roles. |

Mock runs leave nothing behind: no session file for `/resume` to list, and no prompts added
to Up/Ctrl-R recall, so driving the UI doesn't bury real conversations under fixtures. Pass
`HAX_NO_SESSION=0` when the thing under test *is* the session or recall machinery.

## Demo scripts

Useful executable helpers in `scripts/`:

| Script | Use |
| --- | --- |
| `stream_demo.py` | Streaming patterns through the bash tool. |
| `mock_openai_server.py` | Lightweight OpenAI-compatible test server. |

`stream_demo.py` modes include `short`, `long`, `slow`, `burst`, `ansi`, `binary`, `piped`,
and `python_buffer`.

## Vision fixtures

`scripts/vision_fixtures.py` writes small deterministic PNGs (default
`/tmp/hax-vision-fixtures`) and prints, for each, the prompt to use and the expected answer —
ask the model to `read` one and check that the answer matches. Each fixture isolates one
question only the pixels can answer: a solid color to name (baseline pipeline check), a
red/blue layout to locate, dots to count, block text to transcribe. `--edge` adds oversized
fixtures the read tool must *refuse* (per-side pixel cap and byte cap), for exercising the
downscale-hint error path. `docs/screenshot.png` doubles as a realistic text-heavy fixture.

Capability detection can be pinned with `HAX_IMAGE_INPUT=on|off` (default `auto`: live
llama.cpp `/props` or OpenRouter `/endpoints` probe, then models.dev catalog modalities).
Wrong colors or counts indicate a pipeline bug; extra hallucinated detail on the layout
fixture is model quality, not plumbing.

## Rendering and terminal knobs

- `HAX_MARKDOWN=0` disables Markdown rendering.
- `HAX_DISPLAY_WIDTH=<cols>` forces a stable render width, useful for fixtures.
- `HAX_SHOW_REASONING=1` displays reasoning deltas when a provider emits them.
- `HAX_NOTIFY=off` disables terminal/desktop completion notifications.

## Provider startup checks

If the REPL starts with no provider selected, use `/provider`; unavailable rows show a reason.
For one-shot `-p`, provider construction failures are fatal.

Common checks:

- Codex: `~/.codex/auth.json` must exist and contain `tokens.access_token` and
  `tokens.account_id`.
- OpenAI/OpenRouter/Anthropic: make sure the expected API key environment variable is visible
  to the `hax` process.
- `openai-compatible` and `anthropic-compatible`: set the corresponding base URL.
- llama.cpp/ollama: make sure the local server is reachable and the model is configured or
  discoverable.
