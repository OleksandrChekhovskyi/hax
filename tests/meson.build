test_util = executable('test_util',
    sources: [
        'test_util.c',
        '../src/text/utf8.c',
        '../src/util.c',
    ],
)
test('util', test_util)

test_retry = executable('test_retry',
    sources: [
        'transport/test_retry.c',
        '../src/transport/retry.c',
        '../src/text/utf8.c',
        '../src/util.c',
    ],
    dependencies: [jansson],
)
test('retry', test_retry)

test_api_error = executable('test_api_error',
    sources: [
        'transport/test_api_error.c',
        '../src/transport/api_error.c',
        '../src/transport/sse.c',
        '../src/text/utf8.c',
        '../src/util.c',
    ],
    dependencies: [jansson],
)
test('api_error', test_api_error)

test_agent_env = executable('test_agent_env',
    sources: [
        'test_agent_env.c',
        '../src/agent_env.c',
        '../src/system/path.c',
        '../src/text/utf8_sanitize.c',
        '../src/text/utf8.c',
        '../src/util.c',
    ],
)
test('agent_env', test_agent_env)

test_path = executable('test_path',
    sources: [
        'system/test_path.c',
        '../src/system/path.c',
        '../src/text/utf8.c',
        '../src/util.c',
    ],
)
test('path', test_path)

test_read = executable('test_read',
    sources: [
        'tools/test_read.c',
        '../src/tools/read.c',
        '../src/system/path.c',
        '../src/text/utf8_sanitize.c',
        '../src/text/utf8.c',
        '../src/util.c',
    ],
    dependencies: [jansson],
)
test('tools/read', test_read)

test_bash = executable('test_bash',
    sources: [
        'tools/test_bash.c',
        '../src/tools/bash.c',
        '../src/tools/bash_classify.c',
        '../src/terminal/interrupt.c',
        '../src/system/path.c',
        '../src/text/utf8_sanitize.c',
        '../src/text/utf8.c',
        '../src/util.c',
    ],
    dependencies: [jansson, threads],
)
test('tools/bash', test_bash)

test_interrupt = executable('test_interrupt',
    sources: [
        'terminal/test_interrupt.c',
        '../src/terminal/interrupt.c',
        '../src/text/utf8.c',
        '../src/util.c',
    ],
    dependencies: [threads],
)
test('interrupt', test_interrupt)

test_bg_job = executable('test_bg_job',
    sources: [
        'system/test_bg_job.c',
        '../src/system/bg_job.c',
        '../src/text/utf8.c',
        '../src/util.c',
    ],
    dependencies: [threads],
)
test('bg_job', test_bg_job)

test_write = executable('test_write',
    sources: [
        'tools/test_write.c',
        '../src/tools/write.c',
        '../src/system/fs.c',
        '../src/system/diff.c',
        '../src/system/path.c',
        '../src/text/utf8_sanitize.c',
        '../src/text/utf8.c',
        '../src/util.c',
    ],
    dependencies: [jansson],
)
test('tools/write', test_write)

test_edit = executable('test_edit',
    sources: [
        'tools/test_edit.c',
        '../src/tools/edit.c',
        '../src/system/fs.c',
        '../src/system/diff.c',
        '../src/system/path.c',
        '../src/text/utf8_sanitize.c',
        '../src/text/utf8.c',
        '../src/util.c',
    ],
    dependencies: [jansson],
)
test('tools/edit', test_edit)

test_sse = executable('test_sse',
    sources: [
        'transport/test_sse.c',
        '../src/transport/sse.c',
        '../src/text/utf8.c',
        '../src/util.c',
    ],
)
test('sse', test_sse)

test_turn = executable('test_turn',
    sources: [
        'test_turn.c',
        '../src/turn.c',
        '../src/text/utf8.c',
        '../src/util.c',
    ],
)
test('turn', test_turn)

test_codex_events = executable('test_codex_events',
    sources: [
        'providers/test_codex_events.c',
        '../src/providers/codex_events.c',
        '../src/text/utf8.c',
        '../src/util.c',
    ],
    dependencies: [jansson],
)
test('providers/codex_events', test_codex_events)

test_openai_events = executable('test_openai_events',
    sources: [
        'providers/test_openai_events.c',
        '../src/providers/openai_events.c',
        '../src/text/utf8.c',
        '../src/util.c',
    ],
    dependencies: [jansson],
)
test('providers/openai_events', test_openai_events)

test_diff = executable('test_diff',
    sources: [
        'system/test_diff.c',
        '../src/system/diff.c',
        '../src/system/path.c',
        '../src/text/utf8_sanitize.c',
        '../src/text/utf8.c',
        '../src/util.c',
    ],
)
test('diff', test_diff)

test_markdown = executable('test_markdown',
    sources: [
        'render/test_markdown.c',
        '../src/render/markdown.c',
        '../src/text/utf8.c',
        '../src/util.c',
    ],
)
test('markdown', test_markdown)

test_ctrl_strip = executable('test_ctrl_strip',
    sources: [
        'render/test_ctrl_strip.c',
        '../src/render/ctrl_strip.c',
        '../src/text/utf8.c',
        '../src/util.c',
    ],
)
test('ctrl_strip', test_ctrl_strip)

test_utf8_sanitize = executable('test_utf8_sanitize',
    sources: [
        'text/test_utf8_sanitize.c',
        '../src/text/utf8_sanitize.c',
        '../src/text/utf8.c',
        '../src/util.c',
    ],
)
test('utf8_sanitize', test_utf8_sanitize)

test_utf8 = executable('test_utf8',
    sources: [
        'text/test_utf8.c',
        '../src/text/utf8.c',
        '../src/util.c',
    ],
)
test('utf8', test_utf8)

test_input_core = executable('test_input_core',
    sources: [
        'terminal/test_input_core.c',
        '../src/terminal/input_core.c',
        '../src/text/utf8.c',
        '../src/util.c',
    ],
)
test('input_core', test_input_core)

test_disp = executable('test_disp',
    sources: [
        'render/test_disp.c',
        '../src/render/disp.c',
        '../src/text/utf8.c',
        '../src/util.c',
    ],
)
test('disp', test_disp)

test_bash_classify = executable('test_bash_classify',
    sources: [
        'tools/test_bash_classify.c',
        '../src/tools/bash_classify.c',
        '../src/text/utf8.c',
        '../src/util.c',
    ],
)
test('tools/bash_classify', test_bash_classify)

test_spawn = executable('test_spawn',
    sources: [
        'system/test_spawn.c',
        '../src/system/spawn.c',
    ],
)
test('spawn', test_spawn)

test_transcript = executable('test_transcript',
    sources: [
        'test_transcript.c',
        '../src/transcript.c',
        '../src/text/utf8.c',
        '../src/util.c',
    ],
    dependencies: [jansson],
)
test('transcript', test_transcript)

test_agent_core = executable('test_agent_core',
    sources: [
        'test_agent_core.c',
        '../src/agent_core.c',
        '../src/agent_env.c',
        '../src/system/path.c',
        '../src/turn.c',
        '../src/text/utf8_sanitize.c',
        '../src/text/utf8.c',
        '../src/util.c',
    ],
)
test('agent_core', test_agent_core)

test_slash = executable('test_slash',
    sources: [
        'test_slash.c',
        '../src/slash.c',
        '../src/agent_core.c',
        '../src/terminal/clipboard.c',
        '../src/agent_env.c',
        '../src/system/path.c',
        '../src/system/spawn.c',
        '../src/turn.c',
        '../src/text/utf8_sanitize.c',
        '../src/text/utf8.c',
        '../src/util.c',
    ],
)
test('slash', test_slash)

test_clipboard = executable('test_clipboard',
    sources: [
        'terminal/test_clipboard.c',
        '../src/terminal/clipboard.c',
        '../src/system/spawn.c',
        '../src/text/utf8.c',
        '../src/util.c',
    ],
)
test('clipboard', test_clipboard)

test_tool_render = executable('test_tool_render',
    sources: [
        'render/test_tool_render.c',
        '../src/render/tool_render.c',
        '../src/render/disp.c',
        '../src/render/ctrl_strip.c',
        '../src/render/spinner.c',
        '../src/text/utf8_sanitize.c',
        '../src/text/utf8.c',
        '../src/util.c',
    ],
    dependencies: [threads],
)
test('tool_render', test_tool_render)
