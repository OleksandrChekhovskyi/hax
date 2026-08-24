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
    'test_agent.c',
    'test_agent_core.c',
    'test_agent_dispatch.c',
    'test_agent_env.c',
    'test_agent_loop.c',
    'test_agent_tool.c',
    'test_agent_usage.c',
    'test_banner.c',
    'test_catalog.c',
    'test_catalog_fetch.c',
    'test_cli.c',
    'test_compact.c',
    'test_config.c',
    'test_cred_store.c',
    'test_effort.c',
    'test_file_mention.c',
    'test_hax_embed.c',
    'test_history.c',
    'test_model_meta.c',
    'test_model_sort.c',
    'test_harness.c',
    'test_oneshot.c',
    'test_paste_image.c',
    'test_provider.c',
    'test_select_config.c',
    'test_select_model.c',
    'test_session.c',
    'test_session_prune.c',
    'test_slash.c',
    'test_tool_schema.c',
    'test_trace.c',
    'test_transcript.c',
    'test_turn.c',
    'test_util.c',

    'providers/test_anthropic_body.c',
    'providers/test_anthropic_events.c',
    'providers/test_anthropic_models.c',
    'providers/test_chat_body.c',
    'providers/test_chat_events.c',
    'providers/test_codex.c',
    'providers/test_codex_auth.c',
    'providers/test_codex_login.c',
    'providers/test_codex_settings.c',
    'providers/test_config_provider.c',
    'providers/test_http_provider.c',
    'providers/test_llamacpp.c',
    'providers/test_mock.c',
    'providers/test_model_info.c',
    'providers/test_openai.c',
    'providers/test_opencode.c',
    'providers/test_registry.c',
    'providers/test_responses_body.c',
    'providers/test_responses_events.c',
    'providers/test_stream_retry.c',
    'providers/test_wire.c',

    'render/test_ctrl_strip.c',
    'render/test_diff_color.c',
    'render/test_disp.c',
    'render/test_markdown.c',
    'render/test_markdown_scan.c',
    'render/test_markdown_table.c',
    'render/test_markdown_wrap.c',
    'render/test_render_ctx.c',
    'render/test_spinner.c',
    'render/test_tool_render.c',

    'system/test_bg_job.c',
    'system/test_browser.c',
    'system/test_cancel.c',
    'system/test_fs.c',
    'system/test_git.c',
    'system/test_keepawake.c',
    'system/test_os.c',
    'system/test_path.c',
    'system/test_spawn.c',
    'system/test_tempfiles.c',

    'terminal/test_clipboard.c',
    'terminal/test_input.c',
    'terminal/test_input_core.c',
    'terminal/test_interrupt.c',
    'terminal/test_picker_core.c',
    'terminal/test_theme.c',
    'terminal/test_vt_resolve.c',
    'terminal/test_width.c',

    'text/test_base64.c',
    'text/test_diff.c',
    'text/test_sha256.c',
    'text/test_urlencode.c',
    'text/test_utf8.c',
    'text/test_utf8_sanitize.c',
    'text/test_width.c',

    'tools/test_bash.c',
    'tools/test_bash_cd_strip.c',
    'tools/test_bash_classify.c',
    'tools/test_edit.c',
    'tools/test_image_sniff.c',
    'tools/test_output_cap.c',
    'tools/test_path_preprocess.c',
    'tools/test_read.c',
    'tools/test_task.c',
    'tools/test_task_wait.c',
    'tools/test_write.c',

    'transport/test_api_error.c',
    'transport/test_ca.c',
    'transport/test_http.c',
    'transport/test_oauth.c',
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

# End-to-end scenarios: Python scripts under e2e/ that drive the built binary
# through the mock provider (see e2e/harness.py). Naming mirrors the C tests:
# e2e/test_oneshot_text.py registers as e2e/oneshot_text, so the name prefix
# selects the group (`meson test 'e2e/*'`).
e2e_scenarios = [
    'oneshot_text',
    'oneshot_tool',
]

python = find_program('python3')
foreach name : e2e_scenarios
    test('e2e' / name,
        python,
        args: [files('e2e/test_' + name + '.py')],
        env: {'HAX_BIN': hax_exe.full_path()},
        depends: hax_exe,
        timeout: 60,
    )
endforeach

# Runs against the meson-built cffi extension, which only -Dembed=true produces
# and only an interpreter with cffi and headers can build. When that interpreter
# was unavailable the extension is empty, so the test is not registered at all.
if get_option('embed') and have_binding
    binding_env = {'HAX_EXTENSION_DIR': meson.project_build_root() / 'bindings'}

    # Unlike the C tests and the e2e scenarios, this one loads a sanitized libhax into an
    # interpreter that is not itself instrumented, so the runtime arrives with the extension
    # instead of first in the library list. ASan aborts on that before the first test runs;
    # preloading the runtime puts it back in front. Leak detection stays off because CPython
    # holds interpreter state until exit by design, so LSan would report the interpreter
    # rather than hax; the C tests own leak coverage for the library.
    sanitizers = get_option('b_sanitize')
    if 'thread' in sanitizers or 'memory' in sanitizers
        # Neither runtime works by preloading: both need the main program compiled with them,
        # and this one is a stock interpreter. UBSan needs nothing, so it is not listed here.
        binding_env += {'HAX_SANITIZER_SKIP':
            'thread and memory sanitizers need an instrumented interpreter'}
    elif 'address' in sanitizers
        asan_runtime = ''
        foreach candidate : ['libasan.so',
                             'libclang_rt.asan-' + host_machine.cpu_family() + '.so',
                             'libclang_rt.asan_osx_dynamic.dylib']
            if asan_runtime == ''
                # A name the toolchain cannot place comes back unchanged, not as a path.
                probe = run_command(meson.get_compiler('c').cmd_array(),
                    '-print-file-name=' + candidate, check: false).stdout().strip()
                if probe != candidate and probe != '' and fs.exists(probe)
                    asan_runtime = probe
                endif
            endif
        endforeach
        if asan_runtime == ''
            # Aborting on library order tells the reader nothing; skip with the real reason.
            binding_env += {'HAX_SANITIZER_SKIP':
                'the AddressSanitizer runtime was not found, so it cannot be preloaded'}
        else
            preload = host_machine.system() == 'darwin' ? 'DYLD_INSERT_LIBRARIES' : 'LD_PRELOAD'
            binding_env += {preload: asan_runtime, 'LSAN_OPTIONS': 'detect_leaks=0'}
        endif
    endif

    test('bindings/python',
        py_installation,
        args: [files('bindings/test_python.py')],
        env: binding_env,
        depends: binding_extension,
        timeout: 120,
    )
endif
