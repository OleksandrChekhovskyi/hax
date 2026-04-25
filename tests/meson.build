tests_inc = include_directories('../src', '.')

test_util = executable('test_util',
    sources: [
        'test_util.c',
        '../src/util.c',
    ],
    include_directories: tests_inc,
)
test('util', test_util)

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
        '../src/util.c',
    ],
    include_directories: tests_inc,
    dependencies: [jansson],
)
test('tools/bash', test_bash)

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

test_markdown = executable('test_markdown',
    sources: [
        'test_markdown.c',
        '../src/markdown.c',
        '../src/util.c',
    ],
    include_directories: tests_inc,
)
test('markdown', test_markdown)
