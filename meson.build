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
    language: 'c',
)

libcurl = dependency('libcurl')
jansson = dependency('jansson')
threads = dependency('threads')

sources = files(
    'src/agent.c',
    'src/ctrl_strip.c',
    'src/diff.c',
    'src/env.c',
    'src/fs.c',
    'src/http.c',
    'src/input.c',
    'src/input_core.c',
    'src/interrupt.c',
    'src/main.c',
    'src/markdown.c',
    'src/spinner.c',
    'src/sse.c',
    'src/trace.c',
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
    include_directories: include_directories('src'),
    dependencies: [libcurl, jansson, threads],
    install: true,
)

subdir('tests')
