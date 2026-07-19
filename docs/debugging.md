# Debugging

## Wire trace

Set `HAX_TRACE` to capture HTTP requests, response statuses, and SSE events:

```sh
HAX_TRACE=/tmp/hax-trace.md hax
```

The trace is plain Markdown-like text and is truncated at startup. Authorization is redacted.
Entries include elapsed-time tags so pauses between streamed chunks are visible. `HAX_TRACE`
only records provider traffic; it is silent for the mock provider because no network is used.

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

## Mock provider

The mock provider exercises dispatch and rendering without an LLM:

```sh
HAX_PROVIDER=mock hax
HAX_PROVIDER=mock HAX_MOCK_SCRIPT=scripts/mock_demo.txt hax
```

Without a script, it parses the latest user message heuristically. For example, typing
``run `ls -la` `` can trigger a real `bash` tool call.

With `HAX_MOCK_SCRIPT`, each provider `stream()` call consumes one scripted turn. The script
DSL supports:

```text
text <message>
reasoning <message>
space
tool <name> <json>
delay <ms>
usage in=N out=M [cached=K] [cache_write=W] [cost=D]
end-turn
```

`reasoning` emits a chain-of-thought delta (rendered only with `HAX_SHOW_REASONING=1`), and
`space` joins consecutive `text` directives with a single-space delta. Blank lines and `#`
comments are ignored. The full DSL reference lives at the top of `src/providers/mock.c`.

See `scripts/mock_demo.txt` for examples covering tool previews and multi-turn continuation.

## Demo scripts

Useful scripts in `scripts/`:

| Script | Use |
| --- | --- |
| `mock_demo.txt` | Scripted mock-provider demo. |
| `mock_layout.txt` | Layout fixtures: header reflow, gutter rendering, markdown wrap. |
| `stream_demo.py` | Streaming patterns through the bash tool. |
| `mock_openai_server.py` | Lightweight OpenAI-compatible test server. |
| `diff_demo.txt` | Manual diff/rendering fixture. |
| `pause_demo.txt` | Streaming-pause behavior fixture (no spinner over live text). |
| `theme_demo.txt` | One-screen tour of the semantic color roles; run under each `HAX_THEME`. |

`stream_demo.py` modes include `short`, `long`, `slow`, `burst`, `ansi`, `binary`, `piped`,
and `python_buffer`.

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
