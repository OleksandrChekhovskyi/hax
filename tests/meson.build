test_util = executable('test_util',
    sources: [
        'test_util.c',
        '../src/util.c',
    ],
)
test('util', test_util)

test_env = executable('test_env',
    sources: [
        'test_env.c',
        '../src/env.c',
        '../src/utf8_sanitize.c',
        '../src/util.c',
    ],
)
test('env', test_env)

test_read = executable('test_read',
    sources: [
        'tools/test_read.c',
        '../src/tools/read.c',
        '../src/utf8_sanitize.c',
        '../src/util.c',
    ],
    dependencies: [jansson],
)
test('tools/read', test_read)

test_bash = executable('test_bash',
    sources: [
        'tools/test_bash.c',
        '../src/tools/bash.c',
        '../src/cmd_classify.c',
        '../src/interrupt.c',
        '../src/utf8_sanitize.c',
        '../src/util.c',
    ],
    dependencies: [jansson, threads, libutil],
)
test('tools/bash', test_bash)

test_interrupt = executable('test_interrupt',
    sources: [
        'test_interrupt.c',
        '../src/interrupt.c',
        '../src/util.c',
    ],
    dependencies: [threads],
)
test('interrupt', test_interrupt)

test_write = executable('test_write',
    sources: [
        'tools/test_write.c',
        '../src/tools/write.c',
        '../src/fs.c',
        '../src/diff.c',
        '../src/utf8_sanitize.c',
        '../src/util.c',
    ],
    dependencies: [jansson],
)
test('tools/write', test_write)

test_edit = executable('test_edit',
    sources: [
        'tools/test_edit.c',
        '../src/tools/edit.c',
        '../src/fs.c',
        '../src/diff.c',
        '../src/utf8_sanitize.c',
        '../src/util.c',
    ],
    dependencies: [jansson],
)
test('tools/edit', test_edit)

test_sse = executable('test_sse',
    sources: [
        'test_sse.c',
        '../src/sse.c',
        '../src/util.c',
    ],
)
test('sse', test_sse)

test_turn = executable('test_turn',
    sources: [
        'test_turn.c',
        '../src/turn.c',
        '../src/util.c',
    ],
)
test('turn', test_turn)

test_codex_events = executable('test_codex_events',
    sources: [
        'providers/test_codex_events.c',
        '../src/providers/codex_events.c',
        '../src/util.c',
    ],
    dependencies: [jansson],
)
test('providers/codex_events', test_codex_events)

test_openai_events = executable('test_openai_events',
    sources: [
        'providers/test_openai_events.c',
        '../src/providers/openai_events.c',
        '../src/util.c',
    ],
    dependencies: [jansson],
)
test('providers/openai_events', test_openai_events)

test_diff = executable('test_diff',
    sources: [
        'test_diff.c',
        '../src/diff.c',
        '../src/utf8_sanitize.c',
        '../src/util.c',
    ],
)
test('diff', test_diff)

test_markdown = executable('test_markdown',
    sources: [
        'test_markdown.c',
        '../src/markdown.c',
        '../src/util.c',
    ],
)
test('markdown', test_markdown)

test_ctrl_strip = executable('test_ctrl_strip',
    sources: [
        'test_ctrl_strip.c',
        '../src/ctrl_strip.c',
        '../src/util.c',
    ],
)
test('ctrl_strip', test_ctrl_strip)

test_utf8_sanitize = executable('test_utf8_sanitize',
    sources: [
        'test_utf8_sanitize.c',
        '../src/utf8_sanitize.c',
        '../src/util.c',
    ],
)
test('utf8_sanitize', test_utf8_sanitize)

test_input_core = executable('test_input_core',
    sources: [
        'test_input_core.c',
        '../src/input_core.c',
        '../src/util.c',
    ],
)
test('input_core', test_input_core)

test_disp = executable('test_disp',
    sources: [
        'test_disp.c',
        '../src/disp.c',
        '../src/util.c',
    ],
)
test('disp', test_disp)

test_cmd_classify = executable('test_cmd_classify',
    sources: [
        'test_cmd_classify.c',
        '../src/cmd_classify.c',
        '../src/util.c',
    ],
)
test('cmd_classify', test_cmd_classify)

test_spawn = executable('test_spawn',
    sources: [
        'test_spawn.c',
        '../src/spawn.c',
    ],
)
test('spawn', test_spawn)

test_transcript = executable('test_transcript',
    sources: [
        'test_transcript.c',
        '../src/transcript.c',
        '../src/util.c',
    ],
    dependencies: [jansson],
)
test('transcript', test_transcript)

test_agent_core = executable('test_agent_core',
    sources: [
        'test_agent_core.c',
        '../src/agent_core.c',
        '../src/env.c',
        '../src/turn.c',
        '../src/utf8_sanitize.c',
        '../src/util.c',
    ],
)
test('agent_core', test_agent_core)

test_slash = executable('test_slash',
    sources: [
        'test_slash.c',
        '../src/slash.c',
        '../src/agent_core.c',
        '../src/env.c',
        '../src/turn.c',
        '../src/utf8_sanitize.c',
        '../src/util.c',
    ],
)
test('slash', test_slash)

test_tool_render = executable('test_tool_render',
    sources: [
        'test_tool_render.c',
        '../src/tool_render.c',
        '../src/disp.c',
        '../src/ctrl_strip.c',
        '../src/spinner.c',
        '../src/utf8_sanitize.c',
        '../src/util.c',
    ],
    dependencies: [threads],
)
test('tool_render', test_tool_render)
