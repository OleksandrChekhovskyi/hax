# Changelog

Notable user-facing changes, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/). Each release's section becomes its GitHub release
notes (see [docs/releasing.md](docs/releasing.md)).

## [Unreleased]

### Added

- The llama.cpp provider understands llama-server's multi-model router mode. `/model` lists the
  server's whole catalog with load state, context, and image capability; a configured model is
  matched by id or alias; and hax never makes the router load a model you didn't select — picking
  one warms it up in the background.
- FreeBSD and OpenBSD are supported, and CI builds and tests them on every change alongside Linux
  and macOS. Both build from source, and `scripts/install_deps.sh` installs their dependencies.
- On Arch Linux, hax is available in the AUR as `hax`, refreshed automatically by each stable
  release.

### Fixed

- OpenAI reasoning summaries no longer render their step titles glued together
  ("...color string lengthInvestigating combining marks..."): the Responses providers now put
  each summary part on its own line. Reasoning blocks after the first in a response also wrap
  at the right column in the live view, and the history view (Ctrl-O) separates consecutive
  reasoning blocks the way the live view does instead of running them into one paragraph.
- With `show_reasoning` on, the history view (Ctrl-O) no longer loses the dim italic reasoning
  styling after the first line. `less` resets styling at every line, so the settled output now
  reopens carried-over styling on each line; bold or italic spans wrapped across lines are
  covered too.
- The transcript and history views (Ctrl-T, Ctrl-O) no longer show literal `ESC[1m` escapes or
  garbled non-ASCII, and the `$EDITOR` buffer and the `@file` picker keep non-ASCII intact. A plain
  `PAGER=less`, or a system with no locale configured, previously broke them. hax also warns at
  startup now when no UTF-8 locale is available at all.
- `hax --version` no longer reports the version of an unrelated repository. A release tarball ships
  no `.git`, so building one inside another checkout — an AUR packaging clone, say — stamped that
  repository's commit hash into the binary.

## [0.3.0] - 2026-08-12

### Added

- Installable via the `oleksandrchekhovskyi/hax` Homebrew tap. Each stable release points the
  formula at the published source tarball automatically.
- `make install` and `make symlink` complete the from-source flow. `scripts/install_deps.sh`
  now defaults to the full desktop set (build deps plus `fzf`), takes `ci` for the bare set, and
  supports Fedora/RHEL and openSUSE.
- `/session` shows the resolved context window before any request has reported usage
  (`? / 256k`), so the limit is visible up front.

### Changed

- The `task_kill` tool is merged into `task_wait`: a `kill` argument stops the background task
  and returns its final output in the same call — immediately, or after `timeout_seconds` to
  give the task a last window to finish on its own. Stopping a task and collecting its output
  no longer takes two model round trips.
- Compiled-in default model names are gone. Defaults now come only from live state (llama.cpp
  server discovery, or the model Codex mirrors from `~/.codex/config.toml`), so shipped
  binaries no longer park first-time users on a stale or expensive tier. Without a default,
  pick one with `/model` or `--model`.
- An unset `$VISUAL`/`$EDITOR` falls back to the first of `editor`/`nano`/`vim`/`vi` on
  `PATH`, and an unset `$PAGER` to `less -R` or `more`, instead of assuming `vi` and `less`.
  A configured value that does not resolve is reported as an error rather than failing at
  spawn.
- `--help` wraps at the terminal width the same way `/help` does, so it no longer overflows
  narrow terminals.

### Fixed

- `/new` and `/resume` now stop running background tasks and record each task's final state in
  the conversation being left, as quitting always did. Completed work no longer announces
  itself into a conversation that never started it, and task ids restart at `t1` with each
  fresh conversation.
- The parked "working..." spinner no longer blinks off and on around every silent tool call
  (reads, quiet bash). Erase, cluster text, and repark now land as one frame.

## [0.2.0] - 2026-08-08

### Added

- Releases now include fully static Linux binaries for x86_64 and aarch64 with a `SHA256SUMS`
  file. Each tarball contains the binary as `hax`, ready to extract into `PATH`; it runs on any
  distribution with no dependencies.
- The system TLS certificate store is located automatically (override with the standard
  `CURL_CA_BUNDLE`, `SSL_CERT_FILE`, or `SSL_CERT_DIR` environment variable), so HTTPS works
  even when the binary was built for a different distribution. Certificate errors on systems
  with no CA store now say how to fix them.

### Changed

- Unified diffs for write/edit results are computed by an in-tree diff implementation instead
  of shelling out to `diff`, so `diffutils` is no longer a runtime dependency and the write and
  edit tools work on minimal systems where `diff` is absent.

## [0.1.0] - 2026-08-07

Initial public release.
