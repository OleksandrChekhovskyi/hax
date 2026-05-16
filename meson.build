project('hax', 'c',
    version: '0.1.0',
    license: 'MIT',
    default_options: [
        'c_std=c11',
        'warning_level=3',
        'buildtype=debugoptimized',
    ],
)

add_project_arguments(
    '-D_POSIX_C_SOURCE=200809L',
    # _XOPEN_SOURCE=700 is SUSv4 (POSIX 2008 + XSI). Needed on glibc to
    # expose realpath() — _POSIX_C_SOURCE alone doesn't gate it through.
    '-D_XOPEN_SOURCE=700',
    '-D_DARWIN_C_SOURCE',
    # Project headers go through -iquote, not -I, so `#include <foo.h>`
    # always reaches a system header rather than shadowing it with one
    # of ours (e.g. `src/util.h` would collide with macOS's `<util.h>`
    # under -I). The `tests/` entry serves the same purpose for
    # `harness.h` and friends.
    '-iquote', meson.project_source_root() / 'src',
    '-iquote', meson.project_source_root() / 'tests',
    language: 'c',
)

libcurl = dependency('libcurl')
jansson = dependency('jansson')
threads = dependency('threads')

sources = files(
    'src/agent.c',
    'src/agent_core.c',
    'src/transport/api_error.c',
    'src/system/bg_job.c',
    'src/terminal/clipboard.c',
    'src/tools/bash_cd_strip.c',
    'src/tools/bash_classify.c',
    'src/render/ctrl_strip.c',
    'src/system/diff.c',
    'src/render/disp.c',
    'src/agent_env.c',
    'src/system/fs.c',
    'src/transport/http.c',
    'src/terminal/input.c',
    'src/terminal/input_core.c',
    'src/terminal/interrupt.c',
    'src/main.c',
    'src/render/markdown.c',
    'src/terminal/notify.c',
    'src/oneshot.c',
    'src/system/path.c',
    'src/render/progress.c',
    'src/transport/retry.c',
    'src/slash.c',
    'src/system/spawn.c',
    'src/render/spinner.c',
    'src/transport/sse.c',
    'src/render/tool_render.c',
    'src/trace.c',
    'src/transcript.c',
    'src/turn.c',
    'src/text/utf8.c',
    'src/text/utf8_sanitize.c',
    'src/util.c',
    'src/providers/codex.c',
    'src/providers/codex_events.c',
    'src/providers/llamacpp.c',
    'src/providers/mock.c',
    'src/providers/ollama.c',
    'src/providers/openai.c',
    'src/providers/openai_compat.c',
    'src/providers/openai_events.c',
    'src/providers/openrouter.c',
    'src/providers/probe.c',
    'src/providers/registry.c',
    'src/tools/bash.c',
    'src/tools/edit.c',
    'src/tools/read.c',
    'src/tools/write.c',
)

executable('hax',
    sources: sources,
    dependencies: [libcurl, jansson, threads],
    install: true,
)

subdir('tests')
