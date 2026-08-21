# Changelog

Notable user-facing changes, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/). Each release's section becomes its GitHub release
notes (see [docs/releasing.md](docs/releasing.md)).

## [Unreleased]

### Added

- OpenCode Zen and Go providers (`opencode-zen`, `opencode-go`): set `OPENCODE_API_KEY` and pick
  a model — hax speaks each model's API automatically. On `opencode-go`, `/usage` shows the
  subscription's rate-limit windows. See
  [docs/providers.md](docs/providers.md#opencode-zen-and-go).
- Custom providers can serve several protocols behind one URL: `api: "catalog"` routes each model
  by the model catalog, and `model_apis` maps model-id globs to APIs explicitly. See
  [docs/providers.md](docs/providers.md#custom-providers).
- `/login` signs in to ChatGPT for the codex provider, so the codex CLI is no longer needed:
  approve a code on the printed `auth.openai.com` page and hax keeps the token refreshed from
  then on. `/logout` removes the login. Credentials from the codex CLI keep working (read-only)
  when no hax-managed login exists. See [docs/providers.md](docs/providers.md#codex).
- The transcript names what served each turn below its usage line: provider, model and reasoning
  effort as they were for that turn, the model the response reported when it differs from the
  request, and OpenRouter's upstream endpoint.
- `HAX_TRACE` redacts credentials inside request and error bodies, not just headers.
- Every provider block accepts `extra_body` and `extra_headers`: raw JSON members merged into
  each request body (OpenRouter routing preferences, service tiers, sampling knobs) and extra
  HTTP headers on every request (gateway credentials, attribution). A `$VAR` header value or
  inline `api_key` reads the environment variable instead, and `HAX_TRACE` redacts such values
  wherever they appear. See [docs/providers.md](docs/providers.md#request-passthrough).
- The llama.cpp provider understands llama-server's multi-model router mode. `/model` lists the
  server's whole catalog with load state, context, and image capability; a configured model is
  matched by id or alias; and hax never makes the router load a model you didn't select — picking
  one warms it up in the background.
- FreeBSD and OpenBSD are supported, and CI builds and tests them on every change alongside Linux
  and macOS. Both build from source, and `scripts/install_deps.sh` installs their dependencies.
- On Arch Linux, hax is available in the AUR as `hax`, refreshed automatically by each stable
  release.

### Changed

- The `/model` picker sorts every provider's list by default with a version-aware order: model
  families group alphabetically, newer versions come first (`gpt-5.6` before `gpt-5`, whether
  versions are dotted or dashed), and a base model precedes its dated snapshots and named
  variants. Previously only some providers sorted, and purely alphabetically. `sort_models off`
  (global or per provider) restores server order.
- **Breaking:** every provider now reads settings only from its own `providers.<id>` config block,
  so a key or quirk configured for one endpoint can no longer leak into another. Per-provider
  config keys and some environment variables moved or changed scope in the process; if you
  configure providers beyond their API-key variables, revisit your setup against
  [docs/providers.md](docs/providers.md) and
  [docs/configuration.md](docs/configuration.md#provider-settings). Auto-selection now also tries
  the generic -compatible providers after every compiled-in one.
- The `/provider` picker shows display names — `llama.cpp`, a configured `display_name` — and
  notes the selectable provider id below the list when it differs from the highlighted label.
- `/config` no longer lists `providers.*` keys: providers are configured through `/provider`,
  environment variables, and `config.json`. A specific key can still be queried by name.
- A `providers.<name>` block member that hax does not recognize, or that the provider does not
  use — including Chat Completions-only fields on a Responses endpoint, and typos in a
  compiled-in provider's block — now warns at construction instead of being silently ignored.
- Selecting a reasoning effort now takes effect on Anthropic-protocol models of custom endpoints
  and gateways, by switching the request to adaptive thinking; previously it was silently
  ignored there. An explicit `thinking_mode` setting still wins.
- Keyless config-defined providers count a configured `base_url` as available instead of probing
  `/models`, which a generic endpoint may not serve; the ollama recipe still probes its local
  server.
- hax-written `config.json` updates (`/config`, preset save) no longer rewrite numbers and
  booleans elsewhere in the file as strings.

### Fixed

- A response stream that dies mid-generation is retried automatically instead of failing the
  turn with `[provider error — enter to retry]`; anything already rendered is closed with a dim
  `[unexpected end]` marker and the retry re-streams from scratch.
- Retrying after a provider error no longer feeds the failed turn's truncated reasoning back to
  the model, which could derail it into garbage output after repeated failures.
- Models that need to see their own earlier reasoning (Kimi K3, GLM, DeepSeek and MiniMax on the
  OpenCode providers) no longer stop reasoning after the first turn of a conversation.
- Summarized and encrypted reasoning from OpenAI, Gemini, Kimi and MiniMax models on OpenRouter
  survives across turns instead of being dropped after the turn that produced it.
- Tool calls from backends that deliver the arguments only with the completed call instead of
  streaming them (Grok via OpenCode Go) no longer run with empty arguments and derail the
  conversation.
- Custom providers that enable prompt caching now default the cache-breakpoint TTL to 1h like the
  built-in providers, instead of falling back to the API's 5m. A `cache_ttl` value other than
  `5m`/`1h` now warns and uses the default instead of silently behaving as 5m.
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
