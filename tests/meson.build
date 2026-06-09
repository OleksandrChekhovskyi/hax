# Each entry is just the test's source path; everything else is derived. The
# registered test name is the path with the `test_` prefix and `.c` suffix
# stripped, keeping the subdirectory (so `tools/test_read.c` -> `tools/read`,
# `transport/test_sse.c` -> `transport/sse`, top-level `test_util.c` -> `util`).
# That makes this list a direct mirror of the `sources` list in the top-level
# meson.build — keep it grouped by subdirectory and sorted within each group.
#
# Every test is a single .c file linked against `hax_dep`: lazy static-archive
# linking pulls in only the production objects it transitively references, and
# the dependency carries the libcurl/jansson/threads include dirs and link
# flags — so a test needs nothing here but its source path.
test_sources = [
    'test_agent_core.c',
    'test_agent_dispatch.c',
    'test_agent_env.c',
    'test_config.c',
    'test_session.c',
    'test_slash.c',
    'test_transcript.c',
    'test_turn.c',
    'test_util.c',

    'providers/test_codex_events.c',
    'providers/test_openai_events.c',
    'providers/test_openai_messages.c',

    'render/test_ctrl_strip.c',
    'render/test_disp.c',
    'render/test_markdown.c',
    'render/test_tool_render.c',

    'system/test_bg_job.c',
    'system/test_diff.c',
    'system/test_path.c',
    'system/test_spawn.c',

    'terminal/test_clipboard.c',
    'terminal/test_input_core.c',
    'terminal/test_interrupt.c',
    'terminal/test_picker.c',

    'text/test_utf8.c',
    'text/test_utf8_sanitize.c',

    'tools/test_bash.c',
    'tools/test_bash_cd_strip.c',
    'tools/test_bash_classify.c',
    'tools/test_edit.c',
    'tools/test_read.c',
    'tools/test_write.c',

    'transport/test_api_error.c',
    'transport/test_retry.c',
    'transport/test_sse.c',
]

fs = import('fs')
foreach src : test_sources
    stem = fs.stem(src).substring(5)  # drop the leading 'test_'
    parent = fs.parent(src)
    name = parent == '.' ? stem : parent / stem
    exe = executable('test_' + name.underscorify(),
        src,
        dependencies: [hax_dep],
    )
    test(name, exe)
endforeach
