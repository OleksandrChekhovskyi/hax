# The cffi extension for the Python binding. cffi emits the C; meson compiles
# and links it, so the whole build stays in one build system and needs no
# setuptools. Both steps want the interpreter that has cffi and development
# headers, which is normally the project's uv environment.
py_installation = import('python').find_installation(
    fs.exists(venv_python) ? venv_python : 'python3',
    required: false,
)

binding_extension = []
have_binding = false
python_headers = py_installation.found() and fs.exists(
    py_installation.get_path('include') / 'Python.h')
if python_headers
    have_cffi = run_command(py_installation, '-c', 'import cffi', check: false).returncode() == 0
    if not have_cffi
        message('cffi is not installed for @0@; skipping the Python binding'
            .format(py_installation.full_path()))
    else
        cffi_source = custom_target('_hax_cffi.c',
            input: 'python/hax_build.py',
            output: '_hax_cffi.c',
            command: [py_installation, '@INPUT@', '@OUTPUT@'],
        )
        have_binding = true
        binding_extension = py_installation.extension_module('_hax_cffi',
            cffi_source,
            link_with: hax_shared,
            dependencies: [libcurl.partial_dependency(compile_args: true, includes: true),
                           jansson.partial_dependency(compile_args: true, includes: true)],
            install: false,
        )
    endif
else
    message('no Python installation with development headers; skipping the Python binding')
endif
