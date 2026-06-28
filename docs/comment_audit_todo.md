# Comment audit todo

Audit comments for accuracy and concision after the AGENTS.md guidance update. Work one chunk at a
time so reviews stay focused.

For each chunk:

- Delete comments that restate nearby code.
- Shorten comments that are accurate but wordy.
- Keep or clarify comments that explain non-obvious behavior, protocol quirks, terminal edge
  cases, lifetime/ownership, or security/safety constraints.
- Update stale comments instead of preserving wording.
- Run `clang-format -i` on touched C sources/headers, then the relevant Meson tests.

## Chunks

- [ ] Agent REPL and turn orchestration
  - Source:
    - `src/agent.c`
    - `src/agent.h`
    - `src/agent_core.c`
    - `src/agent_core.h`
    - `src/agent_dispatch.c`
    - `src/agent_dispatch.h`
    - `src/agent_env.c`
    - `src/agent_env.h`
    - `src/main.c`
    - `src/oneshot.c`
    - `src/oneshot.h`
  - Tests:
    - `tests/test_agent_core.c`
    - `tests/test_agent_dispatch.c`
    - `tests/test_agent_env.c`

- [ ] Conversation model and compaction
  - Source:
    - `src/provider.h`
    - `src/turn.c`
    - `src/turn.h`
    - `src/compact.c`
    - `src/compact.h`
    - `src/tool.h`
  - Tests:
    - `tests/test_compact.c`
    - `tests/test_turn.c`

- [ ] Configuration and provider selection
  - Source:
    - `src/config.c`
    - `src/config.h`
    - `src/select.c`
    - `src/select.h`
    - `src/providers/registry.c`
    - `src/providers/registry.h`
    - `src/providers/probe.c`
    - `src/providers/probe.h`
    - `src/providers/config_provider.c`
    - `src/providers/config_provider.h`
  - Tests:
    - `tests/test_config.c`
    - `tests/providers/test_config_provider.c`
    - `tests/providers/test_registry.c`

- [ ] OpenAI-compatible providers
  - Source:
    - `src/providers/openai.c`
    - `src/providers/openai.h`
    - `src/providers/openai_compat.c`
    - `src/providers/openai_compat.h`
    - `src/providers/openai_events.c`
    - `src/providers/openai_events.h`
    - `src/providers/openrouter.c`
    - `src/providers/openrouter.h`
    - `src/providers/llamacpp.c`
    - `src/providers/llamacpp.h`
  - Tests:
    - `tests/providers/test_openai_events.c`
    - `tests/providers/test_openai_messages.c`

- [ ] Anthropic-compatible providers
  - Source:
    - `src/providers/anthropic.c`
    - `src/providers/anthropic.h`
    - `src/providers/anthropic_compat.c`
    - `src/providers/anthropic_compat.h`
    - `src/providers/anthropic_events.c`
    - `src/providers/anthropic_events.h`
  - Tests:
    - `tests/providers/test_anthropic_events.c`
    - `tests/providers/test_anthropic_messages.c`

- [ ] Codex and mock providers
  - Source:
    - `src/providers/codex.c`
    - `src/providers/codex.h`
    - `src/providers/codex_events.c`
    - `src/providers/codex_events.h`
    - `src/providers/mock.c`
    - `src/providers/mock.h`
  - Tests:
    - `tests/providers/test_codex_events.c`

- [ ] Transport and API errors
  - Source:
    - `src/transport/api_error.c`
    - `src/transport/api_error.h`
    - `src/transport/http.c`
    - `src/transport/http.h`
    - `src/transport/retry.c`
    - `src/transport/retry.h`
    - `src/transport/sse.c`
    - `src/transport/sse.h`
  - Tests:
    - `tests/transport/test_api_error.c`
    - `tests/transport/test_retry.c`
    - `tests/transport/test_sse.c`

- [ ] Rendering core and markdown
  - Source:
    - `src/render/ctrl_strip.c`
    - `src/render/ctrl_strip.h`
    - `src/render/disp.c`
    - `src/render/disp.h`
    - `src/render/markdown.c`
    - `src/render/markdown.h`
    - `src/render/render_ctx.c`
    - `src/render/render_ctx.h`
  - Tests:
    - `tests/render/test_ctrl_strip.c`
    - `tests/render/test_disp.c`
    - `tests/render/test_markdown.c`

- [ ] Rendering progress, spinner, and tool output
  - Source:
    - `src/render/progress.c`
    - `src/render/progress.h`
    - `src/render/spinner.c`
    - `src/render/spinner.h`
    - `src/render/tool_render.c`
    - `src/render/tool_render.h`
  - Tests:
    - `tests/render/test_tool_render.c`

- [ ] Terminal input and UI
  - Source:
    - `src/terminal/ansi.h`
    - `src/terminal/input.c`
    - `src/terminal/input.h`
    - `src/terminal/input_core.c`
    - `src/terminal/input_core.h`
    - `src/terminal/ui.c`
    - `src/terminal/ui.h`
  - Tests:
    - `tests/terminal/test_input_core.c`

- [ ] Terminal integration helpers
  - Source:
    - `src/terminal/clipboard.c`
    - `src/terminal/clipboard.h`
    - `src/terminal/interrupt.c`
    - `src/terminal/interrupt.h`
    - `src/terminal/notify.c`
    - `src/terminal/notify.h`
    - `src/terminal/picker.c`
    - `src/terminal/picker.h`
  - Tests:
    - `tests/terminal/test_clipboard.c`
    - `tests/terminal/test_interrupt.c`
    - `tests/terminal/test_picker.c`

- [ ] Sessions and slash commands
  - Source:
    - `src/session.c`
    - `src/session.h`
    - `src/session_picker.c`
    - `src/session_picker.h`
    - `src/slash.c`
    - `src/slash.h`
  - Tests:
    - `tests/test_session.c`
    - `tests/test_slash.c`

- [ ] System utilities
  - Source:
    - `src/system/bg_job.c`
    - `src/system/bg_job.h`
    - `src/system/diff.c`
    - `src/system/diff.h`
    - `src/system/fs.c`
    - `src/system/fs.h`
    - `src/system/keepawake.c`
    - `src/system/keepawake.h`
    - `src/system/path.c`
    - `src/system/path.h`
    - `src/system/spawn.c`
    - `src/system/spawn.h`
  - Tests:
    - `tests/system/test_bg_job.c`
    - `tests/system/test_diff.c`
    - `tests/system/test_keepawake.c`
    - `tests/system/test_path.c`
    - `tests/system/test_spawn.c`

- [ ] Built-in tools
  - Source:
    - `src/tools/bash.c`
    - `src/tools/bash_cd_strip.c`
    - `src/tools/bash_cd_strip.h`
    - `src/tools/bash_classify.c`
    - `src/tools/bash_classify.h`
    - `src/tools/edit.c`
    - `src/tools/path_preprocess.c`
    - `src/tools/path_preprocess.h`
    - `src/tools/read.c`
    - `src/tools/write.c`
  - Tests:
    - `tests/tools/test_bash.c`
    - `tests/tools/test_bash_cd_strip.c`
    - `tests/tools/test_bash_classify.c`
    - `tests/tools/test_edit.c`
    - `tests/tools/test_read.c`
    - `tests/tools/test_write.c`

- [ ] Text, tracing, transcript, and shared utilities
  - Source:
    - `src/text/utf8.c`
    - `src/text/utf8.h`
    - `src/text/utf8_sanitize.c`
    - `src/text/utf8_sanitize.h`
    - `src/trace.c`
    - `src/trace.h`
    - `src/transcript.c`
    - `src/transcript.h`
    - `src/util.c`
    - `src/util.h`
  - Tests:
    - `tests/harness.h`
    - `tests/text/test_utf8.c`
    - `tests/text/test_utf8_sanitize.c`
    - `tests/test_trace.c`
    - `tests/test_transcript.c`
    - `tests/test_util.c`
