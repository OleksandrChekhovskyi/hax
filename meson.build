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
    # of ours. (We carry a `src/util.h` whose name collides with macOS's
    # `<util.h>`; without -iquote, an `#include <util.h>` to pick up
    # forkpty(3) resolves to the project header instead.) The `tests/`
    # entry serves the same purpose for `harness.h` and friends.
    '-iquote', meson.project_source_root() / 'src',
    '-iquote', meson.project_source_root() / 'tests',
    language: 'c',
)

cc = meson.get_compiler('c')

libcurl = dependency('libcurl')
jansson = dependency('jansson')
threads = dependency('threads')
# forkpty(3) lives in libutil on glibc/musl Linux. macOS ships a
# libutil stub in libSystem, so find_library succeeds there too and
# the resulting -lutil is harmless. required: false keeps the build
# tolerant of platforms where the library isn't found at all.
libutil = cc.find_library('util', required: false)

sources = files(
    'src/agent.c',
    'src/agent_core.c',
    'src/cmd_classify.c',
    'src/ctrl_strip.c',
    'src/diff.c',
    'src/disp.c',
    'src/env.c',
    'src/fs.c',
    'src/http.c',
    'src/input.c',
    'src/input_core.c',
    'src/interrupt.c',
    'src/main.c',
    'src/markdown.c',
    'src/oneshot.c',
    'src/progress.c',
    'src/slash.c',
    'src/spawn.c',
    'src/spinner.c',
    'src/sse.c',
    'src/tool_render.c',
    'src/trace.c',
    'src/transcript.c',
    'src/turn.c',
    'src/utf8_sanitize.c',
    'src/util.c',
    'src/providers/codex.c',
    'src/providers/codex_events.c',
    'src/providers/llamacpp.c',
    'src/providers/openai.c',
    'src/providers/openai_compat.c',
    'src/providers/openai_events.c',
    'src/providers/openrouter.c',
    'src/tools/bash.c',
    'src/tools/edit.c',
    'src/tools/read.c',
    'src/tools/write.c',
)

executable('hax',
    sources: sources,
    dependencies: [libcurl, jansson, threads, libutil],
    install: true,
)

subdir('tests')
