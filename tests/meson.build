tests_inc = include_directories('../src', '.')

test_util = executable('test_util',
    sources: [
        'test_util.c',
        '../src/util.c',
    ],
    include_directories: tests_inc,
)
test('util', test_util)

test_env = executable('test_env',
    sources: [
        'test_env.c',
        '../src/env.c',
        '../src/util.c',
    ],
    include_directories: tests_inc,
)
test('env', test_env)

test_read = executable('test_read',
    sources: [
        'tools/test_read.c',
        '../src/tools/read.c',
        '../src/util.c',
    ],
    include_directories: tests_inc,
    dependencies: [jansson],
)
test('tools/read', test_read)

test_bash = executable('test_bash',
    sources: [
        'tools/test_bash.c',
        '../src/tools/bash.c',
        '../src/interrupt.c',
        '../src/util.c',
    ],
    include_directories: tests_inc,
    dependencies: [jansson, threads],
)
test('tools/bash', test_bash)

test_interrupt = executable('test_interrupt',
    sources: [
        'test_interrupt.c',
        '../src/interrupt.c',
        '../src/util.c',
    ],
    include_directories: tests_inc,
    dependencies: [threads],
)
test('interrupt', test_interrupt)

test_write = executable('test_write',
    sources: [
        'tools/test_write.c',
        '../src/tools/write.c',
        '../src/fs.c',
        '../src/diff.c',
        '../src/util.c',
    ],
    include_directories: tests_inc,
    dependencies: [jansson],
)
test('tools/write', test_write)

test_edit = executable('test_edit',
    sources: [
        'tools/test_edit.c',
        '../src/tools/edit.c',
        '../src/fs.c',
        '../src/diff.c',
        '../src/util.c',
    ],
    include_directories: tests_inc,
    dependencies: [jansson],
)
test('tools/edit', test_edit)

test_sse = executable('test_sse',
    sources: [
        'test_sse.c',
        '../src/sse.c',
        '../src/util.c',
    ],
    include_directories: tests_inc,
)
test('sse', test_sse)

test_turn = executable('test_turn',
    sources: [
        'test_turn.c',
        '../src/turn.c',
        '../src/util.c',
    ],
    include_directories: tests_inc,
)
test('turn', test_turn)

test_codex_events = executable('test_codex_events',
    sources: [
        'providers/test_codex_events.c',
        '../src/providers/codex_events.c',
        '../src/util.c',
    ],
    include_directories: tests_inc,
    dependencies: [jansson],
)
test('providers/codex_events', test_codex_events)

test_openai_events = executable('test_openai_events',
    sources: [
        'providers/test_openai_events.c',
        '../src/providers/openai_events.c',
        '../src/util.c',
    ],
    include_directories: tests_inc,
    dependencies: [jansson],
)
test('providers/openai_events', test_openai_events)

test_diff = executable('test_diff',
    sources: [
        'test_diff.c',
        '../src/diff.c',
        '../src/util.c',
    ],
    include_directories: tests_inc,
)
test('diff', test_diff)

test_markdown = executable('test_markdown',
    sources: [
        'test_markdown.c',
        '../src/markdown.c',
        '../src/util.c',
    ],
    include_directories: tests_inc,
)
test('markdown', test_markdown)

test_ctrl_strip = executable('test_ctrl_strip',
    sources: [
        'test_ctrl_strip.c',
        '../src/ctrl_strip.c',
        '../src/util.c',
    ],
    include_directories: tests_inc,
)
test('ctrl_strip', test_ctrl_strip)

test_input_core = executable('test_input_core',
    sources: [
        'test_input_core.c',
        '../src/input_core.c',
        '../src/util.c',
    ],
    include_directories: tests_inc,
)
test('input_core', test_input_core)
