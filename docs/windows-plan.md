# Native Windows support plan

Status: design approved in principle; implementation has not started.

## Objective

Produce a native x64 Windows build of hax that:

- uses the MSVC ABI and does not link to Cygwin, MSYS2, or a pthread compatibility runtime;
- runs interactively in Windows Terminal and Git Bash/mintty;
- uses Git for Windows' Bash as the model-facing command shell;
- preserves hax's Unix-like paths and shell contract instead of exposing Windows details to the
  model throughout the codebase;
- builds its pinned libcurl and Jansson sources as part of the Meson build, without vcpkg;
- retains the existing Linux, macOS, FreeBSD, and OpenBSD behavior and build paths.

Git for Windows itself contains an MSYS2 runtime. That runtime belongs to the separately installed
`bash.exe` process and its children; it is not loaded into `hax.exe`. The native executable must be
verifiably free of `msys-2.0.dll`, `cygwin1.dll`, and pthread DLL dependencies.

## Initial support boundary

The first supported target is:

- x86-64 Windows 10 or newer;
- Clang targeting `x86_64-pc-windows-msvc`, or MSVC from Visual Studio 2022;
- Windows Terminal, a ConPTY-backed Git Bash/mintty session, or mintty's legacy named-pipe path;
- Git for Windows installed for command execution;
- UTF-8 text internally and at the terminal boundary.

The design should not prevent arm64 later, but arm64 is not an initial release gate. Interactive
startup hard-fails when the terminal cannot provide VT input/output, either through console modes or
a recognized mintty MSYS pty. Supporting a legacy non-VT Windows console is explicitly out of
scope.

Running hax from PowerShell must not require Git Bash to be the parent shell. Git Bash is a required
command backend discovered and launched by native hax. Running hax from Git Bash must also work,
including when mintty is using a named pipe rather than pcon/ConPTY.

## Architectural rule

Minimize changes to the existing POSIX implementation. Prefer three techniques, in order:

1. compile-time aliases for exact CRT equivalents such as `_stricmp` and `_setmode`;
2. small Windows compatibility implementations for the subset hax actually uses;
3. separately selected Windows source files only where process, terminal, or filesystem semantics
   genuinely differ.

Do not introduce a broad cross-platform framework or rewrite working Unix modules merely to make
the interfaces theoretically platform-neutral. Provider, conversation, rendering, and tool
state-machine code should remain unchanged wherever possible. Win32 handles and UTF-16 stay inside
the small number of Windows implementation files that need them.

The Windows boundary converts UTF-8 to UTF-16 immediately before Win32 APIs and converts results
back immediately. The rest of hax continues to use UTF-8 and Unix-style paths.

## Dependency strategy

### Meson WrapDB, not vcpkg

Prefer Meson subprojects over vcpkg or a separately installed SDK. WrapDB currently supplies
`curl` and `jansson` wraps with Meson build definitions, so Windows can build both static libraries
with the same compiler invocation and CRT settings as hax. CMake is not needed for these
dependencies.

Pin reviewed WrapDB revisions in `subprojects/*.wrap`. Let Meson download the upstream source and
WrapDB patch archives on first setup, verifying the hashes recorded by each wrap. Meson's package
cache can reuse those downloads locally, but the archives do not need to be committed. Do not
maintain local curl/Jansson source lists or generated configuration headers.

The Windows build therefore requires only:

- a C compiler and Windows SDK;
- Meson and Ninja;
- Git for Windows at runtime and while running the Bash-related tests.

Unix builds continue using system `libcurl`, `jansson`, platform threads, and optional `libm`.
Windows forces the static wrap fallbacks. A developer may still point Meson at compatible native
static packages for experimentation, but that is not the documented or CI build path.

Dependency license texts must be included in binary release artifacts. CI must exercise a clean
setup that downloads and hash-verifies every pinned wrap rather than relying on a developer cache.

Both dependencies must use exactly the same MSVC runtime selection as hax. This is load-bearing
because Jansson allocates strings returned to hax and curl exposes allocation/free pairs across the
API boundary. Mixed `/MT`, `/MD`, debug, and release CRTs do not reliably fail at link time; they
can instead produce warnings or cross-heap runtime faults.

### Windows libcurl configuration

The Windows build needs HTTP, HTTPS, proxies, the easy API, URL API, transfer progress, and date
parsing. It does not need curl's command-line executable, non-HTTP protocols, or HTTP/2/3.

Build static libcurl with:

- Schannel as the only TLS backend and Windows native CA trust;
- HTTP/1.1 as the only HTTP protocol version;
- the curl executable, tests, examples, manuals, and documentation disabled;
- OpenSSL and all other alternate TLS backends disabled;
- HTTP/2 and HTTP/3 backends disabled explicitly;
- LDAP, SSH, RTSP, FTP, mail, and other unused protocols disabled;
- Brotli, zstd, libpsl, libidn2, and external zlib disabled initially.

The exact Meson option names belong to the pinned WrapDB revisions and must be recorded in the
build. The build must fail if Schannel is silently disabled, if an unexpected third-party library
is selected, or if curl becomes shared.

HTTPS tests must succeed without a CA bundle or certificate-related environment variable.

### Jansson configuration

Build the WrapDB Jansson subproject as a static library with tests, examples, and documentation
disabled. Preserve its UTF-8 validation and allocator behavior. Its static-library compile
definition and CRT selection must propagate through the Meson dependency object.

## Unix-style path façade

### Canonical internal form

Use Git Bash-style paths as hax's canonical internal and model-facing form on Windows:

```text
C:\Users\alice\src\app  -> /c/Users/alice/src/app
D:\models                -> /d/models
\\server\share\repo      -> //server/share/repo
```

This keeps existing assumptions useful:

- absolute paths begin with `/`;
- `/` is the only internal separator;
- `~`, relative paths, and shell quoting retain their existing meaning;
- commands shown to the model can be pasted into Git Bash;
- MSYS argument conversion translates `/c/...` for native child programs.

At process startup, normalize command-line paths, the current directory, environment-derived paths,
and Win32 API results into this form. At filesystem and native process boundaries, translate drive
and UNC forms back to UTF-16 Windows paths.

Do not scatter drive-letter tests throughout callers. `system/path` owns conversion and root
semantics.

### Home and application data

Preserve hax's Unix layout consistently regardless of the parent shell:

- use non-empty `HOME` when supplied;
- otherwise derive home from `%USERPROFILE%` and expose it internally as `/c/Users/...`;
- continue honoring `XDG_CONFIG_HOME`, `XDG_STATE_HOME`, and `XDG_CACHE_HOME`;
- retain `~/.config/hax`, `~/.local/state/hax`, and `~/.cache/hax` fallbacks.

Using `%APPDATA%` only when launched from PowerShell would create two independent hax installations
for the same user, so the initial port deliberately keeps the existing XDG-compatible layout.

Environment paths may arrive as either `C:\...` or `/c/...`; normalize both.

### Git Bash virtual paths

Drive paths and UNC paths are first-class. Git Bash-only virtual roots such as `/usr`, `/mingw64`,
and custom MSYS mounts cannot always be translated without consulting the MSYS runtime.

The initial file tools guarantee project paths, drive paths, UNC paths, and home-relative paths. If
a model passes a shell-private absolute path to a native file tool, return a clear error rather than
adding a `cygpath` subprocess fallback to ordinary path handling.

### Executable lookup

There are two lookup domains:

1. Native programs launched directly use `SearchPathW`, Windows `PATH`, and `PATHEXT`.
2. Commands intended for the Bash tool are resolved by Git Bash and its transformed POSIX `PATH`.

Do not use the current colon-only `fs_which()` implementation on a native Windows environment.
Environment discovery shown to the model should describe tools available to Git Bash, because that
is where model-generated commands run.

## Terminal support

### Terminal classification

Do not use `_isatty()` as the Windows terminal test. Classify each standard handle as one of:

```c
enum terminal_stream_kind {
    TERMINAL_STREAM_CONSOLE,
    TERMINAL_STREAM_MSYS_PTY,
    TERMINAL_STREAM_REDIRECTED,
};
```

Classification order:

1. `GetConsoleMode()` succeeds: a VT-capable console or ConPTY handle, subject to setup succeeding.
2. The handle is a pipe and its `FileNameInfo` contains a recognized MSYS pty name: mintty's legacy
   named-pipe path.
3. Everything else is redirected input/output.

Query pipe names with `GetFileInformationByHandleEx(..., FileNameInfo, ...)`, normalize case, and
recognize the stable `msys-...-pty...` portion without depending on a full version-specific name.
Cygwin terminals are not an initial target.

A failed `GetConsoleMode()` means only that console mode calls are unavailable. It must not disable
ANSI styling or interactivity for a recognized mintty pty. Conversely, interactive startup on an
unrecognized pipe hard-fails instead of accumulating terminal heuristics.

### Output setup

For a console/ConPTY output handle:

- save the original console mode;
- require `ENABLE_VIRTUAL_TERMINAL_PROCESSING` and `ENABLE_PROCESSED_OUTPUT` to succeed;
- save the original output code page and select `CP_UTF8`;
- restore both later.

If VT mode cannot be enabled, interactive startup exits with one clear unsupported-terminal error.
There is no legacy Console rendering backend.

Do not enable `DISABLE_NEWLINE_AUTO_RETURN` by default. hax preserves scrollback and emits ordinary
line-oriented output; the flag is useful for full-screen TUIs but can damage normal newline and
last-column behavior here.

For a recognized MSYS pty pipe, do not call console mode APIs. Continue emitting VT and OSC
sequences because mintty parses them at the other end.

Set stdout and stderr to binary mode so the CRT cannot inject CRLF into cursor-addressed output.
Use unbuffered output for terminal streams, or preserve the existing explicit flush points. A pipe
that represents mintty must not acquire ordinary fully buffered redirected-output behavior.

For truly redirected output, retain clean one-shot behavior and the existing style policy.

### What startup setup does and does not solve

VT output setup is mostly a one-time operation: after successful classification, the existing ANSI
renderer can continue writing the same bytes. It does not require a Windows rendering rewrite.

Input still has three small lifecycle requirements after startup:

- switch between raw and cooked modes when hax, an editor, or a picker owns input;
- wait on a console handle or MSYS pipe without POSIX `poll()`;
- restore modes on exit and refresh cached size at safe input boundaries.

The legacy mintty pipe also needs an occasional VT size query because no console buffer exists.
These operations belong in one Windows terminal backend; the editor and escape parsers remain
shared.

### Input setup

For a console/ConPTY input handle:

- save the original mode and input code page;
- select `CP_UTF8`;
- enable `ENABLE_VIRTUAL_TERMINAL_INPUT`;
- enable `ENABLE_EXTENDED_FLAGS` and clear `ENABLE_QUICK_EDIT_MODE` while hax owns input;
- switch line input, echo, and processed-input flags according to whether the editor or interrupt
  watcher currently owns the terminal.

The existing byte-oriented escape parser remains the source of truth for arrows, bracketed paste,
and editing keys. The backend supplies UTF-8 and VT bytes rather than `INPUT_RECORD` structures.

For a recognized MSYS pty pipe, read bytes directly. Mintty supplies escape sequences without
console mode changes. Waiting must use Win32 handle/event operations, not CRT `poll()`.

Input ownership remains explicit: the editor, picker, terminal query parser, and interrupt watcher
must never read concurrently. A small pushback queue preserves bytes that were read while waiting
for a terminal response but belong to the user.

### Terminal sizing

For console and pcon-backed handles, use `GetConsoleScreenBufferInfo()` and prefer the visible
window dimensions over the backing buffer dimensions.

For a legacy mintty named pipe:

1. use a recently cached size when available;
2. while the main input layer exclusively owns stdin, issue `CSI 18 t` and parse the
   `CSI 8 ; rows ; cols t` response with a short deadline;
3. retain unrelated input bytes in the pushback queue;
4. fall back to configured `display_width`, then the existing width default.

Do not use the `ESC[999;999H` cursor-position trick unless `CSI 18 t` proves unusable in supported
mintty versions; moving the live cursor to discover size is visibly destructive and complicates
scrollback preservation.

Do not issue terminal queries from rendering or worker threads. Named-pipe dimensions may be
refreshed at prompt boundaries and other points where the input layer already owns stdin.

### Mode restoration and abnormal exit

Console modes and code pages belong to the shared console, not just hax. Save and restore them:

- during normal mode transitions;
- in an `atexit` handler;
- from a `SetConsoleCtrlHandler` handler for Ctrl-C, close, logoff, and shutdown events where the
  operating system permits cleanup;
- before launching an editor, pager, picker, or other interactive child.

Cleanup paths must be idempotent. The control handler performs only bounded Win32 operations and
signals normal code to finish when time permits. Process Job Objects provide the independent
backstop for child cleanup.

### Required terminal test matrix

Exercise every interactive feature in:

1. Windows Terminal with PowerShell or `cmd.exe`;
2. Git Bash/mintty with pcon enabled;
3. Git Bash/mintty with `MSYS=disable_pcon`;
4. redirected stdin/stdout in one-shot mode.

Checks include Unicode, styling, prompt editing, resize/reflow, bracketed paste, Ctrl-C, Esc/Esc,
pager/editor transitions, clipboard paste, and restoration after interruption.

## Git Bash discovery

Resolve the command shell once during startup, before background threads begin:

1. if `bash.shell` / `HAX_BASH_SHELL` is set, require that exact executable;
2. otherwise split native `PATH` on `;` and find the first regular `git.exe`, `git.cmd`, `git.bat`,
   or `git` entry;
3. starting at that entry's parent, walk ancestors and test `usr/bin/bash.exe`, then
   `bin/bash.exe` beneath each ancestor;
4. require the first match to pass a short `bash -c` identity/path-behavior check.

This deliberately mirrors Git for Windows' layout without registry probes, fixed installation
paths, WSL-launcher checks, or independent Bash lookup. `git-bash.exe` is not a candidate because it
opens a terminal window; hax needs the real console shell under `usr/bin` or `bin`.

Cache the normalized Git and Bash paths. If Git, Bash, or validation is missing, print one
actionable error and exit. Git Bash is part of the Windows runtime contract, not an optional
capability.

## Process and task backend

### Portable process object

Replace public `pid_t`, POSIX wait status, signal, and raw pipe ownership with an opaque process
object. The Windows implementation owns:

- process and primary-thread handles;
- a Job Object;
- input/output pipe handles;
- cached exit state and normalized termination reason;
- shell path and command metadata needed by background tasks.

Callers consume normalized results such as an exit code, interrupted state, timeout state, or
forced-termination state. POSIX code translates its wait status into the same representation.

### Creation and containment

Launch Git Bash with `CreateProcessW` using:

- an explicitly quoted argv command line;
- a native UTF-16 working directory;
- inherited standard handles limited by an explicit handle list;
- merged stdout/stderr for Bash tool calls;
- `CREATE_SUSPENDED` so containment exists before user code runs;
- a Job Object configured with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`.

Assign the suspended process to the job before resuming it. This closes the race in which Bash
could create an uncontained descendant. Avoid inheritable-handle leaks into unrelated children.

The Git Bash command contract remains `bash -c`, matching Unix. Do not add `-l` or source user
profiles implicitly; doing so changes command reproducibility and startup side effects.

### Output and waiting

Windows anonymous pipe readiness cannot be treated like POSIX `poll()`. Use either overlapped named
pipes or a dedicated reader thread plus events. One process abstraction must serve:

- bounded direct-command capture;
- synchronous streaming Bash calls;
- background task draining;
- pager input;
- fzf output;
- editor waiting.

Preserve the current output caps, binary detection, spool-file behavior, and live display hooks.
Never wait for a child while unread output can fill its pipe.

### Cancellation and tree termination

Windows has no reliable general SIGTERM equivalent. Keep the initial backend deterministic:
`TerminateJobObject()` is the single cancellation operation for timeouts, Esc/Esc, task stops, and
shutdown. Do not add console-event or `taskkill` fallbacks until a concrete use case justifies their
extra process-state branches.

The resulting model-facing status describes interruption or forced termination without pretending
a Unix signal occurred. Existing Unix grace and signal wording remains unchanged on Unix.

The first process prototype must prove that Git Bash commands and their native/MSYS descendants do
not survive timeout, Esc/Esc, background-task shutdown, or abrupt hax termination. Do this before
porting the full interactive UI.

## Threads and synchronization

Minimize churn by implementing only the pthread subset hax already uses on Windows: thread
create/join, mutexes, condition variables, static mutex initialization, timed waits, signal, and
broadcast. A small internal compatibility header can map those operations to Win32 threads, SRW
locks, condition variables, and events while leaving the six current pthread-using modules largely
unchanged. Windows-only signal-mask calls move into the terminal/process backend rather than being
faked.

PThreads4W is an acceptable fallback if the internal subset proves more complex than expected, but
it is not the default: vendoring another general-purpose threading package creates more update and
license work than the small API surface justifies. If selected, it must be statically linked and
must not add a runtime DLL.

Timed waits and spurious-wakeup behavior need focused tests. Existing Unix code continues to use
native pthreads unchanged.

## Filesystem and persistence

Implement native UTF-16 filesystem operations for:

- recursive directory creation;
- regular-file inspection;
- symlink/reparse-point handling;
- atomic file replacement;
- exclusive temporary-file creation;
- file and directory flushing where Windows provides an equivalent;
- directory enumeration;
- session pruning;
- credential and session locks with `LockFileEx`;
- executable lookup.

Use `ReplaceFileW` for an existing destination and `MoveFileExW` with appropriate flags for
creation/fallback. Preserve same-directory staging so replacement remains atomic. Explicitly test
behavior when antivirus software or another process temporarily holds a file.

Windows ACLs replace POSIX mode bits; do not invent mode emulation. Credential files should inherit
the user's private profile-directory ACL, and temporary files should be created with non-inheritable
handles in a private per-process directory.

Session pruning must retain its current race-resistant intent. Windows file identity and sharing
modes replace inode comparisons and `openat`/`O_NOFOLLOW` checks.

## Other Windows integrations

Implement narrow native backends for:

- clipboard text and image access through Win32 clipboard APIs;
- keep-awake through `SetThreadExecutionState` or a power request;
- OS description through Windows version APIs;
- console notifications through the existing BEL/OSC path where supported;
- locale initialization without relying on `nl_langinfo()`;
- command-line parsing without a new getopt dependency;
- case-insensitive string helpers and the small model-pattern matcher currently using `fnmatch()`.

Retain Schannel's native certificate store. The Unix CA bundle probing code must not run on a
Schannel build unless an explicit standard curl environment override requires it.

## Source and build organization

Preserve current filenames and Unix implementations unless most of a file is inherently
platform-specific. Small exact differences may use short `_WIN32` branches. Select a companion
Windows source only for substantial implementations such as process creation, Bash process
control, terminal modes/waits, and advanced filesystem operations.

A likely minimal addition set is:

```text
src/system/win32.c               UTF-8 conversion, paths, errors, handle helpers
src/system/thread_win32.h        internal pthread subset over Win32 primitives
src/system/spawn_win32.c         CreateProcess, capture, pipes, and Job Objects
src/tools/bash_process_win32.c   streaming Bash process ownership
src/terminal/windows.c           terminal classification, modes, waits, and size
```

Meson selects these instead of the files whose implementation is wholly POSIX. Pure parsers and
state machines remain shared. Do not split every existing module into `_posix` and `_win32` pairs as
an architectural exercise.

## Implementation sequence

### Phase 1: dependency and compile skeleton

- add pinned curl and Jansson WrapDB files;
- force their static Meson subprojects on Windows with Schannel and HTTP/1.1 only;
- add Windows source selection and system libraries;
- establish UTF-8/UTF-16 and basic error helpers;
- compile a native `hax.exe` far enough to enumerate remaining unsupported modules;
- verify its import table has no MSYS/Cygwin/pthread runtime.

Exit gate: a clean Windows setup downloads the pinned wraps and links a minimal native executable
with static Jansson and Schannel curl; curl reports Schannel and no unexpected dependency backend.

### Phase 2: Unix path façade and basic filesystem

- normalize argv, environment, cwd, home, drive, and UNC paths;
- port file reading, writing, directory creation, temporary files, and config lookup;
- add a portable command-line parser and string compatibility helpers;
- run pure unit tests and one-shot raw/mock flows without invoking a shell command.

Exit gate: `hax.exe --version`, `--help`, raw one-shot mode, config loading, sessions, and native
read/edit/write work in a Unicode path.

### Phase 3: Git Bash process prototype

- implement discovery and validation;
- implement CreateProcess, pipes, normalized exit status, and Job Objects;
- run ordinary, Unicode-path, timeout, descendant, and forced-shutdown commands;
- prove no descendant survives containment cleanup in all supported launch terminals.

Exit gate: synchronous Bash calls and process-tree termination pass focused tests in PowerShell and
Git Bash.

### Phase 4: full tools and background tasks

- port streaming display, output spooling, background task adoption, waits, and shutdown;
- port direct git capture, fzf pipes, editor, and pager execution;
- keep Git Bash discovery as one mandatory startup check rather than per-feature branches.

Exit gate: Bash and task suites pass with equivalent output/timeout semantics, adjusted only where
Windows has no signal identity.

### Phase 5: terminal backends

- add console/ConPTY mode management;
- add MSYS pty pipe recognition;
- port input waiting and interruption;
- add terminal size query/caching for the legacy mintty pipe path;
- add mode restoration and binary/unbuffered stream setup.

Exit gate: the complete interactive test matrix works and every exit path restores its parent
terminal.

### Phase 6: native integrations and hardening

- clipboard, keep-awake, OS description, and Schannel-specific CA tests;
- race, handle-leak, cancellation, and Unicode audits;
- package a standalone `hax.exe` plus notices;
- document installation and Git for Windows detection;
- add Windows CI and release artifacts.

Exit gate: automated Windows tests, manual terminal scenarios, Unix tests, release build, and lint
all pass.

## Verification

### Automated gates

Windows CI must run:

```text
meson setup build-win
meson compile -C build-win
meson test -C build-win --print-errorlogs
```

It must additionally:

- configure from an empty Meson package cache and verify wrap hashes;
- inspect `hax.exe` imports for forbidden runtimes;
- verify libcurl reports Schannel;
- exercise HTTPS against a controlled test endpoint/certificate setup;
- run Python one-shot end-to-end scenarios;
- test Unicode paths, spaces, drive roots, and UNC fixtures where CI permits them;
- prove timeout and shutdown kill a nested Bash descendant tree;
- verify missing or invalid Git Bash produces one startup error and a nonzero exit.

Existing Unix CI remains mandatory. Windows abstractions must not weaken POSIX process cleanup,
credential locking, or terminal behavior.

### Manual gates

Run the interactive checklist in Windows Terminal, mintty pcon, and mintty with
`MSYS=disable_pcon`. Capture:

- detected terminal kind and dimensions in a debug-only diagnostic;
- prompt and streamed Markdown rendering;
- Unicode and emoji alignment;
- editing keys and bracketed paste;
- Ctrl-C, Esc, Esc/Esc, close-window, and abnormal tool termination;
- pager/editor/fzf return to the original prompt;
- foreground and background descendant cleanup;
- terminal mode restoration after every case.

## Early proof points and stop conditions

Resolve these before broad mechanical porting:

1. The pinned WrapDB builds must propagate static curl/Jansson targets, one CRT selection, and
   Windows system link requirements correctly.
2. A Job Object must contain Git Bash's process behavior under both pcon and legacy mintty paths.
3. The MSYS named-pipe heuristic must distinguish a terminal from ordinary redirected pipes.
4. `CSI 18 t` must provide reliable sizing on the supported legacy mintty path without losing user
   input.
5. Console control handling and byte input must preserve current Ctrl-C and Esc semantics.

If any proof point has no defensible implementation, stop that phase with the failing prototype,
observed handles/process tree, attempted approaches, and the smallest decision needed next. Do not
hide a failed containment or terminal assumption behind reduced tests.

## Completion criteria

Native Windows support is complete when:

- a clean machine with the documented compiler, Meson, Ninja, Git for Windows, and network access
  can download the pinned wraps and build the repository;
- the resulting `hax.exe` has no Cygwin, MSYS2, pthread, curl, or Jansson DLL dependency;
- HTTPS uses Schannel and Windows trust without OpenSSL or a bundled CA file;
- model-facing paths and commands remain consistently Unix-like;
- PowerShell, Windows Terminal, mintty pcon, and mintty legacy-pipe scenarios behave transparently;
- missing or invalid Git Bash fails startup once with a clear diagnostic and nonzero status;
- process descendants, terminal modes, handles, credentials, sessions, and temporary files retain
  their cleanup and safety guarantees;
- all Windows and existing Unix verification gates pass.
