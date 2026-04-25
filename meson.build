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
    '-D_DARWIN_C_SOURCE',
    language: 'c',
)

libcurl = dependency('libcurl')
jansson = dependency('jansson')
libedit = dependency('libedit')
threads = dependency('threads')

sources = files(
    'src/agent.c',
    'src/http.c',
    'src/main.c',
    'src/markdown.c',
    'src/spinner.c',
    'src/sse.c',
    'src/trace.c',
    'src/turn.c',
    'src/util.c',
    'src/providers/codex.c',
    'src/providers/codex_events.c',
    'src/providers/openai.c',
    'src/providers/openai_events.c',
    'src/tools/bash.c',
    'src/tools/read.c',
)

executable('hax',
    sources: sources,
    include_directories: include_directories('src'),
    dependencies: [libcurl, jansson, libedit, threads],
    install: true,
)

subdir('tests')
