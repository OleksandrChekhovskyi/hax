project('hax', 'c',
    version: '0.2.0',
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

# include_type system turns the dependencies' -I flags into -isystem so
# clang-tidy does not report findings inside third-party headers (Homebrew
# installs them outside the default system include path).
libcurl = dependency('libcurl', include_type: 'system')
jansson = dependency('jansson', include_type: 'system')
threads = dependency('threads')

# The version baked into the binary. `--always` keeps pre-tag checkouts
# building (bare commit hash); the fallback covers builds without git, such as
# dist tarballs, where it must match the tag scheme's leading "v".
version_h = vcs_tag(
    command: ['git', 'describe', '--tags', '--always', '--dirty=+'],
    input: 'src/version.h.in',
    output: 'version.h',
    fallback: 'v' + meson.project_version(),
)

# Production sources, grouped by subdirectory and sorted within each group.
# `main.c` is deliberately excluded: it lives only in the `hax` executable so
# that test binaries (which bring their own `main`) can link the same objects
# via `hax_lib` without colliding. Keep these lists sorted — additions should
# slot into the right group alphabetically.
sources = files(
    'src/agent.c',
    'src/agent_core.c',
    'src/agent_dispatch.c',
    'src/agent_env.c',
    'src/agent_loop.c',
    'src/agent_tool.c',
    'src/agent_usage.c',
    'src/banner.c',
    'src/busy.c',
    'src/catalog.c',
    'src/cli.c',
    'src/compact.c',
    'src/config.c',
    'src/effort.c',
    'src/file_mention.c',
    'src/history.c',
    'src/model_meta.c',
    'src/oneshot.c',
    'src/paste_image.c',
    'src/provider.c',
    'src/select.c',
    'src/session.c',
    'src/session_picker.c',
    'src/session_prune.c',
    'src/slash.c',
    'src/tool_schema.c',
    'src/trace.c',
    'src/transcript.c',
    'src/turn.c',
    'src/util.c',

    'src/providers/anthropic.c',
    'src/providers/anthropic_compat.c',
    'src/providers/anthropic_events.c',
    'src/providers/anthropic_messages.c',
    'src/providers/codex.c',
    'src/providers/codex_auth.c',
    'src/providers/codex_settings.c',
    'src/providers/config_provider.c',
    'src/providers/llamacpp.c',
    'src/providers/mock.c',
    'src/providers/openai.c',
    'src/providers/openai_compat.c',
    'src/providers/openai_events.c',
    'src/providers/openai_messages.c',
    'src/providers/openrouter.c',
    'src/providers/registry.c',
    'src/providers/responses_events.c',
    'src/providers/responses_messages.c',

    'src/render/ctrl_strip.c',
    'src/render/diff_color.c',
    'src/render/disp.c',
    'src/render/markdown.c',
    'src/render/markdown_scan.c',
    'src/render/markdown_table.c',
    'src/render/markdown_wrap.c',
    'src/render/progress.c',
    'src/render/render_ctx.c',
    'src/render/spinner.c',
    'src/render/tool_render.c',

    'src/system/bg_job.c',
    'src/system/fs.c',
    'src/system/git.c',
    'src/system/keepawake.c',
    'src/system/os.c',
    'src/system/path.c',
    'src/system/spawn.c',
    'src/system/tempfiles.c',

    'src/terminal/clipboard.c',
    'src/terminal/input.c',
    'src/terminal/input_core.c',
    'src/terminal/interrupt.c',
    'src/terminal/notify.c',
    'src/terminal/picker.c',
    'src/terminal/picker_core.c',
    'src/terminal/theme.c',
    'src/terminal/ui.c',
    'src/terminal/vt_resolve.c',
    'src/terminal/width.c',

    'src/text/base64.c',
    'src/text/diff.c',
    'src/text/utf8.c',
    'src/text/utf8_sanitize.c',
    'src/text/width.c',

    'src/tools/bash.c',
    'src/tools/bash_cd_strip.c',
    'src/tools/bash_classify.c',
    'src/tools/bash_env.c',
    'src/tools/bash_output.c',
    'src/tools/bash_process.c',
    'src/tools/bash_shell.c',
    'src/tools/edit.c',
    'src/tools/image_sniff.c',
    'src/tools/output_cap.c',
    'src/tools/path_preprocess.c',
    'src/tools/read.c',
    'src/tools/task_kill.c',
    'src/tools/task_registry.c',
    'src/tools/task_wait.c',
    'src/tools/write.c',

    'src/transport/api_error.c',
    'src/transport/ca.c',
    'src/transport/http.c',
    'src/transport/retry.c',
    'src/transport/sse.c',
)

# One archive shared by the executable and every test. Static-archive linking
# is lazy: each consumer pulls only the objects it transitively references, so
# tests get exactly the closure they need without hand-listing sources. The
# target is named distinctly from the `hax` executable so `meson compile -C
# build hax` stays unambiguous.
hax_lib = static_library('hax_lib',
    sources,
    version_h,
    dependencies: [libcurl, jansson, threads],
)

# Wrap the library as a dependency so consumers inherit both the link against
# `hax_lib` and the include dirs / compile flags of libcurl / jansson / threads
# — their own translation units (`main.c`, each test) include <jansson.h> etc.
# and need those paths at compile time, which `link_with` alone wouldn't carry.
# Only the compile-side of each dependency is propagated here via
# partial_dependency(): the link args already ride along through `link_with`
# (static libraries propagate their link deps), so re-adding them would make
# every link emit `ld: warning: ignoring duplicate libraries`.
hax_dep = declare_dependency(
    link_with: hax_lib,
    dependencies: [
        libcurl.partial_dependency(compile_args: true, includes: true),
        jansson.partial_dependency(compile_args: true, includes: true),
        threads.partial_dependency(compile_args: true, includes: true),
    ],
)

hax_exe = executable('hax',
    'src/main.c',
    dependencies: [hax_dep],
    install: true,
)

subdir('tests')
