# Changelog

Notable user-facing changes, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/). Each release's section becomes its GitHub release
notes (see [docs/releasing.md](docs/releasing.md)).

## [Unreleased]

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
