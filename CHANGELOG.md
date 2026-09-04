# Changelog

Notable user-facing changes, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/). Each release's section becomes its GitHub release
notes (see [docs/releasing.md](docs/releasing.md)).

## [Unreleased]

### Added

- Named custom terminal color and glyph themes in `config.json`, with inherited semantic color
  roles, terminal palette/truecolor values, attributes, backgrounds, and model-output tints.
  Separate glyph themes include prompt and message gutters, tool/picker marks, and spinner frames
  with UTF-8 and ASCII defaults.
- A resumed one-shot run no longer requires a prompt: `hax --resume=ID -p` (or `--json`)
  continues the conversation from where it stopped, including after an interrupt or pause.
- `hax --json` (implies `-p`) streams the run as JSON lines on stdout for orchestrators and
  scripts: the conversation records in the session-file schema as they are appended, closed by a
  `result` record with the outcome, final text, cost, and session id. Plain `-p` output is
  unchanged. The session-file format is now documented as a supported read surface. See
  [docs/sessions.md](docs/sessions.md).
- `/login` for the codex provider now offers a browser flow (authorization code with PKCE through
  a `localhost` redirect) alongside device login, for organizations that block the device flow.
  Device login remains the ssh-friendly path. See [docs/providers.md](docs/providers.md#codex).
- Provider blocks accept `metadata_api` to pick the `/models` dialect (`openai` or `anthropic`)
  independently of the request protocol, for proxies that pair one with the other. Mixed-protocol
  gateways now authenticate metadata requests correctly even when models are rerouted across
  protocol families. See [docs/providers.md](docs/providers.md#custom-providers).
- `sort_models` and `catalog_id` now work in every provider's config block, not only custom
  ones; `providers.openai.sort_models`, for example, keeps the picker in server order.
- `extra_headers` values accept `{session_id}`, the conversation's stable id, for gateways that
  route or cache by session. Headers hax sends by default can now be overridden by name or removed
  with an empty value. See [docs/providers.md](docs/providers.md#request-passthrough).
- OpenRouter requests carry the conversation id as `x-session-id`, OpenRouter's sticky-routing
  key, which also groups a conversation's generations in its activity view.

### Changed

- One-shot runs now stop cleanly on signals instead of dying mid-flight. SIGINT/SIGTERM
  interrupts like the REPL's double Esc — running tools are killed, completed work is saved,
  `--json` still closes with a `result` record, exit status 130 — and a second signal kills the
  process at once. SIGUSR1 pauses like a single Esc: work in flight finishes and the run stops
  at the next turn boundary. See [docs/usage.md](docs/usage.md#cli-modes).
- `max_turns` now also bounds one-shot runs, which previously stopped only at a built-in limit.
  The default is now spelled `auto` (interactive unlimited, one-shot 100); `0` still means the
  same.
- Token counts are decimal everywhere, unlike byte sizes, which keep 1024-base suffixes. The
  stats line, `/session`, and the `/model` picker agree ("200k" for a 200000-token window rather
  than "195k"), and `context_limit` and catalog `limit` fields written with suffixes now parse
  decimally: `"272k"` means 272000 tokens.
- `catalog.models` overrides now beat metadata the provider reports live, and blocks may be keyed
  by the runtime provider id, so codex models can be overridden separately from `openai`. This
  allows raising codex's served context window per model; the `/model` picker shows the sanctioned
  ceiling ("272k context (up to 872k)"). See [docs/providers.md](docs/providers.md#codex).
- Skill discovery now follows the cross-agent `.agents/skills` convention: project skills are
  collected from the current directory up to the repository root, as `AGENTS.md` already was, and
  `~/.agents/skills` is read alongside `~/.config/hax/skills`. Skills installed globally by other
  tools work in hax without being copied or symlinked, and running hax from a subdirectory no
  longer hides skills defined at the project root. The nearest match for a name wins. See
  [docs/usage.md](docs/usage.md#project-instructions-and-context).
- The first-party `openai`, `anthropic`, and `openrouter` providers pin their protocol along
  with their endpoint: `providers.<id>.api` now warns instead of switching the wire. Use
  `model_apis` for per-model protocols, or a custom provider.
- The `providers.openrouter.title` and `providers.openrouter.referer` settings and their
  `HAX_OPENROUTER_*` aliases are gone; override or remove the attribution headers through
  `providers.openrouter.extra_headers`. See [docs/providers.md](docs/providers.md#openrouter).

### Fixed

- OpenCode Zen and Go requests now send the `x-opencode-session` header the gateway requires;
  Requests without it may error starting 2026-09-06.
- The session key sent to providers for routing and prompt caching now follows the conversation
  rather than the process: a resumed conversation keeps its cache, and `/new` starts fresh.
- Interactively resuming an interrupted conversation (`--resume`, `-c`, `/resume`) now shows the
  resume hint and accepts the empty-Enter continue, which previously worked only within the
  interrupted process.
- OpenCode Go usage-window limits now surface immediately instead of triggering retries that cannot
  succeed before the window resets.
- Skill discovery now ignores descriptions from unterminated YAML frontmatter or unsupported block
  scalars instead of advertising incomplete metadata.
- Chat Completions streams now detect upstream provider failures signaled through the finish
  reason (including OpenRouter's `error` and `network_error` sentinels), retry them like other
  transient failures, and surface an error once retries are exhausted. Previously such streams
  rendered as empty successful responses.

## [0.4.0] - 2026-08-22

### Added

- OpenCode Zen and Go providers (`opencode-zen`, `opencode-go`): set `OPENCODE_API_KEY`, choose a
  model, and hax selects the API it needs. `/usage` shows OpenCode Go's subscription limits. See
  [docs/providers.md](docs/providers.md#opencode-zen-and-go).
- `/login` signs in to ChatGPT for the codex provider and keeps the token refreshed, so the codex
  CLI is no longer required. Existing codex CLI credentials remain a read-only fallback. See
  [docs/providers.md](docs/providers.md#codex).
- llama.cpp multi-model router support: `/model` shows the server catalog and load state, selecting
  an idle model warms it in the background, and hax never loads a model you did not select.
- Custom gateways can route different models through their required APIs. Provider blocks also
  accept `extra_body` and `extra_headers` for documented gateway features such as routing rules,
  service tiers, and additional credentials. See
  [docs/providers.md](docs/providers.md#custom-providers).
- The transcript records the provider, model, and reasoning effort used for each turn, plus a
  different model or OpenRouter endpoint reported by the response.
- FreeBSD and OpenBSD can now build from source, and Arch Linux users can install the `hax` AUR
  package. Stable releases update both the AUR package and Homebrew tap automatically.

### Changed

- **Breaking:** provider settings now belong to `providers.<id>` blocks and no longer leak between
  endpoints. Several keys and environment variables changed scope; users with advanced provider
  configuration should revisit [docs/providers.md](docs/providers.md) and
  [docs/configuration.md](docs/configuration.md#provider-settings). llama.cpp settings now live under
  the dot-free `providers.llamacpp` config block; the user-facing `llama.cpp` selection remains
  accepted. Auto-selection tries the built-in providers before compatible and custom providers.
- `/model` now uses a version-aware order by default: model families stay together and newer
  versions appear first. Set `sort_models` to `off` to preserve server order.
- Only presets with a `description` are offered to the model as subagent roles. Favorite-only
  presets remain user shortcuts instead of inviting unrequested delegation based on a name alone.
- `/provider` shows human-readable display names while keeping the selectable id visible. Unknown
  or inapplicable provider settings now warn instead of being silently ignored, and `/config` keeps
  provider settings out of its general picker.
- Reasoning effort now applies to Anthropic-compatible models on custom gateways unless an explicit
  `thinking_mode` overrides it.
- Config updates made by hax preserve JSON numbers and booleans instead of rewriting them as
  strings.

### Fixed

- Interrupted and failed responses no longer carry unfinished reasoning into the next request. An
  interrupt before any answer text or tool call leaves the conversation unchanged, while a stream
  that ends unexpectedly is retried automatically from a clean attempt.
- Reasoning now continues correctly across turns for affected Kimi, GLM, DeepSeek, and MiniMax
  models on OpenCode, and OpenAI, Gemini, Kimi, and MiniMax models on OpenRouter.
- Completed tool calls whose arguments arrive all at once, including Grok on OpenCode Go, no longer
  run with empty arguments.
- Markdown headings, multi-part reasoning summaries, wrapped styling, and consecutive reasoning
  blocks render consistently in both the live and history views.
- Transcript, history, editor, and file-picker views preserve non-ASCII text and no longer expose
  terminal escape sequences when used with a plain pager or without a configured locale.
- HTTP traces redact credentials found in request and error bodies, including values loaded from
  environment variables for provider headers.
- Keyless custom providers with a configured URL are selectable even when they do not expose a
  model-list endpoint.
- Custom prompt caching defaults to the same 1h TTL as built-in providers; invalid `cache_ttl`
  values now warn and fall back safely.
- Release-tarball builds no longer pick up the version of an unrelated enclosing Git repository.

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
