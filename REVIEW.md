# Project review: hax

Reviewed at `189816f` (Release v0.4.0), 2026-08-23.

## Scope and method

This is a whole-project review, not a diff review. What was actually executed:

- `make tests` — 103/103 pass, exit 0.

- `make lint` — clean (clang-format, `scripts/lint_style.py`, clang-tidy).

- `BUILD_DIR=build-asan make tests` — clean under AddressSanitizer + UBSan.

- `BUILD_DIR=build-tsan make tests` — clean under ThreadSanitizer.

- Static sweep of `src/` for the usual C hazards, signal-handler safety, allocator overflow behavior, credential file modes, and libcurl configuration.

- Cross-check of the 66-entry config registry in `src/config.c` against `docs/configuration.md` and `docs/providers.md`.

- Runtime spot check: binary size, resident set, startup latency, linked libraries.

Not attempted: live-provider integration, the interactive REPL under a terminal, cross-platform verification on macOS or the BSDs, or a line-by-line audit of all 47k lines. Claims below are scoped to what was checked; inferences are marked as such.

## Verdict

This is an unusually disciplined C codebase. The engineering hygiene is at the top of what one sees in a single-maintainer project: every seam is documented, every subsystem has a split between a testable pure core and a thin I/O shell, and the sanitizer and lint gates are green without qualification. The design has an explicit, written philosophy, and the code visibly obeys it rather than merely citing it.

The weaknesses are not defects in what exists — they are structural risks around what does not: a bus factor of one, no automated coverage of the interactive terminal path (the largest untested surface), no fuzzing of network-facing parsers, and a handful of modules that have outgrown their file. None of these are urgent. All of them get more expensive the longer the project runs.

## Metrics

| Measure | Value |
| --- | --- |
| Production C (`src/`) | 47,411 lines, 223 files |
| Tests (C + Python) | 43,941 lines, 107 files |
| Test-to-source ratio | ~0.93:1 |
| Unit/integration tests | 101 binaries, all passing |
| End-to-end scenarios | 2 |
| Direct dependencies | libcurl, jansson, pthreads, libm (optional) |
| Binary size | 2.5 MB (dynamic, debugoptimized) |
| RSS at `--version` | 9.7 MB |
| Startup | below timer resolution |
| Commits / authors | 330 / 1 |
| Development window | 2026-04-24 to 2026-08-22 |

Per-directory distribution: `src/` root 18.2k lines, `providers/` 8.8k, `terminal/` 6.0k, `tools/` 5.2k, `render/` 4.8k, `system/` 1.7k, `text/` 1.5k, `transport/` 1.3k.

## What is genuinely good

**The memory-safety posture is real, not claimed.** Zero occurrences of `strcpy`, `strcat`, `sprintf`, `strncpy`, `atoi`, `alloca`, or `system()` in production sources. `buf_grow` in `src/util.c:627` performs explicit `SIZE_MAX` overflow checks before doubling, and `buf_append` guards the length arithmetic separately. Allocation failure is fatal by policy (`xmalloc` and friends abort), which removes an entire class of untested error paths — the correct trade for a short-lived interactive process.

**The testable-core pattern is applied consistently and pays off.** `input_core.c` /`input.c`, `picker_core.c`/`picker.c`, and the provider `*_body.c`/`*_events.c` splits all separate pure state machines from terminal or HTTP attachment. The evidence that this is working: 1,214 lines of test for `input_core.c` alone, and the provider protocol adapters are tested against fixture JSON without a network. `AGENTS.md` states the rule ("do not require HTTP or a TTY to test parsing and state transitions") and the code actually follows it.

**Concurrency is deliberately small and bounded.** Only six files touch `pthread_*`. Background work goes through one cooperative primitive (`system/bg_job`) with an explicit contract: join before destroying anything the worker can reach. TSAN is green and runs in CI on every push.

**Signal handling is correct.** `restore_and_reraise_signal` in `src/terminal/interrupt.c:279` uses only async-signal-safe calls (`tcsetattr`, `write`, `signal`, `raise`), restores the terminal, then re-raises with the default disposition — the textbook shape. The fatal hook (`bash_shell_pgids_kill`) is likewise `kill()`-only. The comment at `bash_process.c:473` proves the fixed 128-slot pgid table cannot overflow by tying it to the declared `task.max_running` ceiling of 64, and that ceiling is genuinely enforced in the config registry (`src/config.c:149`). This is the rare case of a size constant whose justification is both written down and true.

**Credential and state file handling is careful.** Mode 0600 throughout, `mkstemp` plus `fchmod` (with a comment explaining why `fchmod` is needed under a restrictive umask), atomic rename, advisory cross-process locking in `cred_store.c`, and 0700 on session directories because project paths leak through directory names. The `cred_store_update` read-modify-write transaction API is a thoughtful touch that most projects would have gotten wrong.

**libcurl is configured conservatively.** `CURLOPT_FOLLOWLOCATION` is never set, so redirects are not followed and `Authorization` headers cannot leak cross-origin. TLS verification is left at libcurl's secure defaults; nothing disables `VERIFYPEER` or `VERIFYHOST`.

**Documentation is complete and load-bearing.** All 66 config keys are documented. `provider.h` and `tool.h` document ownership, tri-state conventions, and invariants at the field level. `docs/philosophy.md` is the strongest artifact in the repo: it argues each omission (no MCP, no hooks, no permission prompts, no custom slash commands) on the merits and names the pattern that covers the need. The "no permission prompts" section is intellectually honest in a way that is uncommon — it concedes that in-turn interception is a real gap rather than pretending a substitute exists.

**Security claims are correctly scoped.** `bash_classify.h` states plainly that its heuristic "selects only the display preview mode; it never changes execution or model-facing output." A 734-line command classifier is exactly the kind of thing that gets mislabeled as a safety boundary; this one is not.

**CI is thorough for a project this age.** Ten jobs: Ubuntu x86_64 and arm64, ASan, TSan, Debian stable, Arch, Alpine/musl, macOS, plus FreeBSD and OpenBSD in QEMU. Lint runs on three toolchains chosen to bracket the LLVM version range `.clang-tidy` promises to support, and the matrix comment explains that choice. The `--init` container option is there because task tests assert orphan reaping — that is someone who debugged a real CI failure and wrote down why.

**Commit messages explain intent.** `f3e1edb` ("Advertise a preset as a subagent persona only when it has a description") describes the observed model behavior that motivated the change. This is what makes a single-author history survivable by a second person.

## Findings

Ranked by expected cost of leaving them alone.

### 1. Bus factor of one

330 commits, one author, four months. Every architectural decision, every unwritten invariant, and all operational knowledge (release process, AUR and Homebrew publishing, the BSD CI quirks) sits with one person. The documentation quality materially reduces this risk — `AGENTS.md`, `docs/philosophy.md`, and the header comments are close to a working handover document — but they do not eliminate it.

This is not a code problem and there is no code fix. It is the dominant risk to the project's continuity and belongs at the top of any honest review.

### 2. No automated coverage of the interactive terminal path

`AGENTS.md` documents a tmux-driven manual procedure for exercising the REPL, which is a sound technique — but nothing in the test suite uses it. There are no pty tests: `openpty`, `forkpty`, and `posix_openpt` appear nowhere in `tests/`. The two end-to-end scenarios cover only `hax -p` one-shot mode, and total 21 lines.

The uncovered surface is large and is where users spend their time: `select.c` (1,504 lines), `terminal/picker.c` (723), the tty half of `terminal/input.c`, `session_picker.c`, `login.c`, `terminal/ui.c`, `render/progress.c`. The `_core` split means the *logic* in these areas is tested — what is not tested is that the assembled REPL paints, reads keys, and recovers terminal state correctly. Regressions here are exactly the kind users notice first and report worst.

Concrete fix: extend `tests/e2e/harness.py` with a pty-backed driver (`pty.openpty` plus `pexpect`-style expect/send, or shell out to tmux as the docs already describe) and add scenarios for at least: prompt echo and submit, Ctrl+C mid-turn with terminal restoration, `/model` picker navigation and cancel, and resume-hint empty-send. Five scenarios would cover most of the risk. Note the trade-off: pty tests are the flakiest category of test in existence, so they need generous timeouts and should probably run as a separate meson suite that CI can report on without gating.

### 3. No fuzzing of parsers that consume remote input

`grep -ri fuzz` over the repo returns nothing. The SSE parser (`transport/sse.c`), the streaming Markdown renderer (`render/markdown.c`, whose `step_inline` is a 262-line state machine), the UTF-8 sanitizer, `image_sniff.c` (parses image headers for dimensions), and every provider event adapter all consume bytes that originate off-machine. ASan and UBSan are already wired up, so the marginal cost of a libFuzzer or AFL++ target is low — the harness is already there, only the drivers and a seed corpus are missing.

Start with `sse.c` and `markdown.c`: both are stateful, both are byte-oriented, both run on every token of every response. Fixture-based tests confirm the happy paths; they do not explore the input space. This is the single highest-yield unexercised technique available to the project.

Caveat on severity: nothing found in this review suggests an actual bug in these parsers, and the model is on the trusting side of the trust boundary anyway (a hostile provider already has better options than a Markdown parser bug). The argument for fuzzing here is cheap regression insurance on hot, complex code, not an active vulnerability.

### 4. `select.c` has outgrown its file

1,504 lines spanning at least eight distinct concerns: provider autoselection and availability probing, model choice, effort choice, preset selection, preset saving, tint selection, config setting editing, and session selection restore. It is coherent in that everything is "a thing the user picks", but that is a weak cohesion criterion — the module's dependencies are the union of all eight features' dependencies.

The tests already know this: `test_select_model.c` and `test_select_config.c` are split along seams the source does not have. Splitting into `select_provider.c`, `select_model.c`, `select_preset.c`, and `select_config.c` would mirror the existing test structure and cost nothing architecturally.

Same observation, weaker, for `agent_run` in `src/agent.c:1186` at 311 lines. It is linear — resource acquisition, then the REPL loop, then teardown — and the ownership web (several resources borrowed by a single `state` struct) is genuinely intricate, so splitting it risks obscuring lifetimes more than the length obscures anything. Lower priority than `select.c`, and reasonable to leave alone.

### 5. No warning when an API key crosses plaintext HTTP

`base_url` for config-defined providers is accepted verbatim, and nothing in `src/` checks its scheme. A `providers.*.base_url` of `http://internal-gateway/v1` combined with an `api_key` sends that key in cleartext with no diagnostic. Plaintext is legitimate for the loopback defaults (`llamacpp.c:27`, `recipes.c:44` both use `http://127.0.0.1`), so a blanket HTTPS requirement would be wrong.

Fix: warn once at provider construction when the scheme is `http://`, the host is not loopback, and an API key is configured. One `hax_warn` call, no behavior change, consistent with the project's stated preference for restraint over enforcement.

### 6. libcurl protocol allowlist is not set

Neither `CURLOPT_PROTOCOLS_STR` nor its predecessor is set, so a `base_url` typo or a copy-pasted config could hand curl a `file://`, `scp://`, or `ldap://` URL — and libcurl on Debian links `librtmp`, `libssh2`, and `libldap`, so those handlers are present. Impact is low because the config is user-supplied and redirects are not followed, but the response body would land in a `HAX_TRACE` log. `curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https")` at both setopt sites in `transport/http.c` is a two-line change with no downside.

### 7. Element-array reallocation lacks the overflow guard `buf_grow` has

Roughly a dozen sites follow the pattern `xrealloc(p, capacity * sizeof(*p))` after doubling `capacity` — `turn.c:59`, `compact.c:212`, `session.c:1000`, `agent_core.c:75`, `config.c:475`, and others. Unlike `buf_grow`, none check that the multiplication cannot wrap.

This is unreachable in practice: reaching a capacity where `capacity * sizeof(struct item)` overflows `size_t` requires having already allocated more memory than the address space holds. It is listed only because `buf_grow` demonstrates the project's own standard, and the inconsistency is the kind of thing a future reader will trip over. A shared `grow_array(ptr, &cap, elemsize)` helper in `util.h` would settle it and shorten a dozen call sites.

### 8. `shell_pgids` is `volatile pid_t`, not `volatile sig_atomic_t`

`src/tools/bash_process.c:479`. The C standard permits a signal handler to read only objects of type `volatile sig_atomic_t`; `pid_t` happens to be `int` on every platform hax targets, so this is correct in practice and portable enough for the supported set. Worth a one-line comment acknowledging the deviation, or a change to `volatile sig_atomic_t` with a `pid_t` cast at the `kill()` call. Pedantic, listed for completeness.

### 9. `bash_classify.c` is expensive for what it decides

734 production lines plus 374 test lines — 1,108 lines total, about 2.3% of the codebase — to choose between three display preview modes. The implementation is careful and the tests are good, but measured against the project's own stated bar ("would it be used routinely rather than configured once and forgotten", "implemented directly, as a feature, not as a framework for features"), a per-command allowlist of 60-odd binaries with per-command operand-arity specs is the largest complexity-per-user-benefit ratio in the repo.

This is a judgment call, not a defect, and reasonable people will disagree — collapsed previews for `ls` and `grep` genuinely improve the transcript. The point is only that a minimalism-first project should periodically ask whether its own largest heuristic still clears its own bar. An alternative worth considering: classify on a much shorter list (the ten commands that actually dominate agent transcripts) and default everything else to the current behavior, at maybe a fifth of the code.

### 10. Version string falls back to a bare hash in a tagless clone

`meson.build` runs `git describe --tags --always --dirty=+`, and the `--always` flag means the command succeeds with a bare commit hash when no tags are present. `vcs_tag`'s `fallback` only applies when the command *fails*, so a clone made with `--no-tags` reports `hax 189816f` rather than `hax v0.4.0` — reproduced in this working copy. The comment block explains the reasoning for `--always` (pre-tag checkouts should build), so this is a considered trade rather than an oversight; the cost is that bug reports from such clones carry a hash that may not resolve to a release. If the release version matters more than the pre-tag case, dropping `--always` and letting the declared fallback handle it is the alternative.

### 11. Thin test coverage on a few small modules

`test_trace.c` contains a single test (`test_credential_headers_redacted`). That is the most important behavior of the trace subsystem, so the coverage is well-chosen, but trace file formatting, rotation, and truncation are unverified. Similarly, `openrouter.c`'s account-credits rendering (`print_key_usage`, `print_account_credits`) has no test, though the parsing half is thoroughly covered in `test_model_info.c`. Low priority; noted so the map is complete.

## An alternative framing

The review above evaluates hax as a piece of systems software, and by that standard it is excellent. A different framing is worth stating because it changes what the priorities should be.

hax is not primarily competing on code quality — it is competing on a thesis: that a coding agent does not need MCP, plugins, hooks, or a permission system, and that a 2.5 MB C binary with two dependencies is a better shape for this tool than the alternatives. Under that framing, the findings above are mostly not what determines whether the project succeeds. What determines it is whether the thesis survives contact with users: whether "run a script around `hax -p`" is actually an adequate substitute for hooks in daily use, whether five tools (read, edit, write, bash, task_wait) with no dedicated search or glob tool produces competitive agent behavior on real tasks, and whether the absence of in-turn interception becomes a wall.

Those are empirical questions this review cannot answer, and they are not visible in the source. The one thing the codebase does say about them is encouraging: the philosophy document names the gaps rather than hiding them, which is the posture that makes a thesis testable instead of merely asserted. If there is a recommendation here, it is that the project would benefit more from instrumented evidence about agent task success — the tool set in particular — than from any of the code findings above.

## Recommendations, prioritized

1. **Reduce the bus factor.** Nothing in the code; everything in process. The docs are already most of the way to a handover artifact.

2. **Add a pty-backed e2e layer** with roughly five REPL scenarios, as a separate meson suite. Largest coverage gap by a wide margin.

3. **Add fuzz targets for `sse.c` and `markdown.c`.** ASan/UBSan infrastructure already exists.

4. **Split `select.c`** along the seams the tests already use.

5. **Two-line hardening:** `CURLOPT_PROTOCOLS_STR` allowlist, plus a plaintext-HTTP-with-API-key warning.

6. **Introduce a checked `grow_array` helper** and convert the dozen doubling sites.

7. **Re-examine `bash_classify.c` against the project's own feature bar.**

## What this review does not cover

- No live provider was exercised; all provider verification was against fixtures and the mock.

- The interactive REPL was not run. Every claim about terminal behavior comes from reading the code and the tests, not from observing it.

- macOS and BSD behavior was not verified locally. CI covers them; this review did not.

- Performance was measured only at startup. No profiling of streaming, rendering, or large session load.

- Not all 47k lines were read. Depth was concentrated in the seams (`provider.h`, `tool.h`), the largest modules, and the areas where C projects typically fail: allocation, signals, concurrency, credentials, and network configuration.
