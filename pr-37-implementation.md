# PR 37 implementation notes

Each numbered section describes one independently committed change from `pr-37-review-plan.md`.
The first nine changes are implemented here; the remaining review items are still pending.

## 1. Redact Vertex authentication secrets from HTTP traces

### Problem

The OAuth refresh request sends the ADC client secret and refresh token in a form-encoded body.
`http_post` logs that body before sending the request and logs non-success response bodies before
returning. Vertex did not register these credentials with the shared trace redactor, so enabling
`HAX_TRACE` could write them to disk. Registering them after the request would be too late.

The standard `Authorization` header is already redacted by name, but that protection does not
cover an access token echoed in an error body. Tokens from every Vertex source therefore need
value-based registration as well.

### Changes

- Replace Vertex's private form encoder with the existing `url_encode` function. It preserves the
  request encoding while removing a duplicate implementation.
- Register both raw and percent-encoded spellings of the client secret and refresh token before
  calling `http_post`. Both request bodies and echoed OAuth errors use the existing shared
  redaction mechanism; no Vertex-specific logger is introduced.
- Register access tokens when acquired from explicit configuration, the Google environment
  fallback, native OAuth refresh, or gcloud. This also registers newly acquired tokens after
  renewal, without changing credential precedence or refresh behavior.
- Add `providers/vertex_auth` as a focused authentication test target, ready for the later auth
  module extraction. Its OAuth fixture uses loopback HTTP and its gcloud fixture is a local shell
  stub. HOME, the ADC path, the Cloud SDK directory, and the token endpoint are isolated; no real
  Google credentials or public token endpoints are used.
- Reuse `tests/loopback.h` and the PATH helpers from upstream master at
  `7d0acc170d2c922e3d8d2f61963ce6668bb0b343`. These fixtures were absent from this branch, so their
  contents were imported without merging, switching branches, or changing production interfaces.
- Add an Unreleased changelog entry describing the trace security fix.

The shared trace registry copies registered values, so temporary encoded strings can still be
freed immediately after constructing the request. Only diagnostic output is redacted; the HTTP
server continues to receive the original credentials.

### Regression coverage

The new test checks:

1. Configured and environment-sourced literal access tokens are absent from traced error bodies.
2. OAuth secrets containing `+`, `&`, and `%` reach the loopback server with the correct form
   encoding, but neither their raw nor encoded spelling appears in the trace.
3. A real loopback HTTP 400 response echoing both spellings is redacted.
4. OAuth-issued and fake-gcloud-issued access tokens are registered for error-body redaction.

Access-token echo checks call the shared trace response function directly after obtaining a token
through the auth source. They test registration, not the later provider-level 401/retry behavior.

### Validation

- Before the production fix, `providers/vertex_auth` failed with 15 redaction assertions.
- After the fix, `scripts/check.sh test providers/vertex_auth trace text/url harness` passed all
  four targets.
- `make lint` passed after formatting all touched C sources and headers and adding the direct
  `<stdatomic.h>` include identified by clang-tidy.
- `git diff --check` passed.
- `make tests` passed all 121 tests with external catalog refresh disabled in the parent test
  environment and HTTP(S)/ALL proxies pointed at a closed loopback port, bypassed only for
  localhost. Google token/credential environment values were cleared or pointed at a nonexistent
  test path. The proxy guard also covered the existing e2e harness, which strips `HAX_*` variables.
- Sanitizer setup was attempted for `providers/vertex_auth`, `trace`, `text/url`, and `harness`:

  ```sh
  BUILD_DIR=build-asan scripts/check.sh test providers/vertex_auth trace text/url harness
  BUILD_DIR=build-tsan scripts/check.sh test providers/vertex_auth trace text/url harness
  ```

  Neither build could link: this environment lacks `libasan.so.8.0.0`, `libubsan.so.1.0.0`, and
  `libtsan.so.2.0.0`. No sanitizer test result is claimed, and no check was suppressed.

No live Google Cloud validation was performed.

### Next change

Repair the existing Vertex request test's catalog lifetime and isolation: prevent unwanted catalog
fetches, join any catalog worker before teardown, and clean up the loopback server on construction
failure. This change deliberately does not alter catalog lifecycle, configuration resolution,
endpoint construction, metadata semantics, ADC support, cancellation, or retry policy.

## 2. Isolate Vertex request tests and close their resource lifetimes

### Problem

Streaming through the Vertex provider consults model metadata, which can start the process-wide
catalog refresh worker. The C request test neither isolated the catalog cache nor disabled that
refresh, and it never called `catalog_shutdown()`. Joining the HTTP fixture thread did not join
this separate worker, which is the thread leak reported by TSan in the review.

A local sentinel reproduced the unwanted side effect before this change: the existing C test
passed but made one catalog HTTP request. Its early return after failed provider construction also
left its server thread and listener alive. The Python smoke test likewise allowed catalog fetching
and only stopped its server after a successful subprocess launch and completion.

### Changes

- Give the C tests a private HOME, config directory, Cloud SDK directory, and catalog cache. Clear
  inherited Google project, location, token, ADC-path, and OAuth-endpoint settings. Keep loopback
  requests independent of the caller's proxy configuration.
- Disable catalog refresh with a run-tier override in C, so `config_load()` and inherited
  `HAX_CATALOG_REFRESH` values cannot re-enable it. The Python smoke test disables refresh in its
  isolated configuration; its harness already removes inherited `HAX_*` settings.
- Seed both request tests with a catalog snapshot whose model has an 8192-token output limit, and
  assert that the outgoing request uses that value. This verifies that the tests actually use the
  private catalog rather than a developer's cache or a live response.
- Initialize curl explicitly in the C test process. After the tests, call `catalog_shutdown()`
  before releasing configuration, cleaning up global curl state, or allowing temporary-directory
  cleanup at process exit. Catalog shutdown stays at the process boundary, not provider destruction.
- Replace both private C HTTP servers with `tests/loopback.h`. Bind the request listener first,
  construct the provider, and only then start serving. Construction and server-start failures
  reach cleanup. Successful requests join the server before inspecting its captured buffers.
- Use the shared fixture's complete-request reader for OAuth, and assert both refresh requests.
  Release its allocated reply even when server startup fails. Write the ADC fixture with the
  existing filesystem helper so a file-creation failure does not dereference a null `FILE *`.
- Clear inherited Google settings in the Python smoke test, retaining its explicit fake token.
  Manage its listener with a context manager and shut down and join the server thread in `finally`,
  including when launching hax fails or times out.

No production behavior changes in this piece, so no user-facing changelog entry is added. The
Python smoke test remains until its unique assertions are migrated during the planned test
consolidation; this change does not remove coverage or redesign the provider.

### Validation

- `scripts/check.sh test providers/vertex providers/vertex_auth e2e/vertex catalog catalog_fetch
  transport/oauth` passed all six targets.
- A manual isolation regression supplied an inherited stale catalog with conflicting limits,
  conflicting Google settings, an inherited token and ADC file, an aggressive catalog-refresh
  interval, and sentinel HTTP/proxy endpoints. Both C and Python Vertex tests passed, the sentinel
  observed **zero** requests, and the inherited catalog remained unchanged. Before the fix, the C
  test made one request to the same kind of catalog sentinel.
- Injected `FileNotFoundError` and `TimeoutExpired` into the Python test's subprocess launch. In
  both cases the server thread was joined and the listener was closed.
- `make lint` passed, including formatting and clang-tidy.
- The final `make tests` run passed all 121 tests with the same external-network guard described
  in change 1. An earlier full run failed the existing IPv6-conflict scenario at
  `tests/transport/test_oauth.c:436-437`, consistent with an ephemeral IPv4 port collision. The
  unchanged OAuth test passed the focused rerun and the subsequent full suite; no test was skipped
  and no unrelated production or test code was changed.
- Retried ASan/UBSan and TSan Meson setup. Both remain blocked by the missing runtime libraries
  listed in change 1. The TSan CI result is therefore still pending; the local checks do not claim
  a sanitizer pass.

No public Google endpoint or real credential was used for these checks.

### Next change

Change Vertex's thinking mode from `adaptive` to `prefer-adaptive`, with behavioral coverage for
both catalog-listed budget-only models and unlisted models. Configuration, endpoint resolution,
metadata API semantics, and auth hardening remain separate pending pieces.

## 3. Follow model metadata when selecting Vertex thinking mode

### Problem

Vertex's `adaptive` default pins thinking mode and bypasses the catalog. A Claude model whose
catalog entry declares only token-budget reasoning therefore receives an unsupported adaptive
request. A selected effort can also cause an inappropriate `output_config` to be sent.

### Changes

- Change the shipped Vertex definition to `thinking_mode = "prefer-adaptive"`. This is the only
  production-code change: the existing resolver already follows known model capabilities and
  falls back to adaptive thinking when capabilities are unknown.
- Add a behavioral regression in `tests/providers/test_http_provider.c`, constructing the actual
  shipped Vertex provider and capturing its outgoing request bodies with the shared loopback
  fixture. The test does not simply assert the definition's string value.
- Extend the private catalog fixture with a synthetic budget-only Vertex model and an 8192-token
  output limit. Check that it receives `thinking.type = "enabled"`, a budget of 8191 tokens, and
  neither adaptive thinking nor `output_config`, both without an effort and with `high` selected.
- Check that an unlisted model still receives adaptive thinking without choosing an effort first.
  This preserves the intended fallback and distinguishes `prefer-adaptive` from `auto`.
- Disable catalog refresh in the HTTP-provider test process, use run-tier overrides for the fake
  Vertex credentials and endpoint, bypass proxies for loopback, and shut down the catalog before
  global curl cleanup. The test restores its configuration overrides after destroying the provider.
- Add an Unreleased changelog entry describing the corrected default.

Explicit user thinking-mode overrides retain their existing precedence. No resolver, auth, endpoint,
or metadata-API behavior is otherwise changed.

### Validation

- With the original `adaptive` definition, the new regression failed seven assertions on the
  budget-only requests. The unlisted-model adaptive fallback already passed.
- After the one-line fix, all five focused targets passed:

  ```sh
  scripts/check.sh test providers/http_provider providers/vertex providers/anthropic_body \
      providers/registry e2e/vertex
  ```

- `make tests` passed all 121 tests with the external-network guard described in change 1.
- `make lint` and `git diff --check` passed; both touched C files were formatted.
- Retried ASan/UBSan and TSan setup. Both remain blocked by the missing sanitizer runtime
  libraries listed in change 1; no sanitizer pass is claimed.

All request checks used a synthetic catalog and loopback responses, not live Google Cloud.

### Next change

Implement explicit `metadata_api = "none"` semantics: disable default remote model listing and
probing while preserving deliberately installed catalog listing, and cover this in the generic
HTTP-provider tests.

## 4. Add an explicit disabled metadata API

### Problem

The metadata parser recognized only OpenAI and Anthropic dialects. Simply setting Vertex's
`metadata_api` to `none` would therefore fall back to Anthropic behavior and install a remote
model probe. Its previous `openai` declaration avoided the probe by describing an API the endpoint
does not serve, while relying on a custom catalog-listing hook to replace the default listing.

### Changes

- Recognize `none` in both provider definitions and configuration overrides. It installs no default
  remote model listing, per-model probe, or metadata-header dialect.
- Make `http_provider_metadata_headers()` return NULL for this mode without invoking auth hooks.
  Streaming headers continue to follow the request wire and are unaffected.
- Set Vertex's definition to `metadata_api = "none"` and remove the misleading OpenAI stand-in
  comment. Its explicit catalog-backed listing remains installed.
- Retain the existing rule for explicit listing/probe hooks: they apply when the selected metadata
  API matches the definition's own setting. Deliberately overriding a `none` definition with
  `openai` or `anthropic` installs that remote dialect's defaults instead of retaining a mismatched
  listing hook. Conversely, configuring `none` on a remote-metadata definition removes its
  listing/probe hooks; unrelated hooks, such as usage reporting, remain unchanged.
- Document the new value in the provider guide and local header contracts, and add an Unreleased
  changelog entry.

This setting controls provider model-metadata queries, not models.dev catalog refresh. Catalog
metadata remains available, and `catalog.refresh: 0` is still the way to disable its background
fetch. No new provider-specific boolean or metadata abstraction is introduced.

### Regression coverage

- Exercise definition-level and configured `none` across Chat Completions, Responses, and Messages.
  Check that construction emits no unknown-setting warning and that listing, probing, and metadata
  headers are absent. Trigger model-metadata refresh and verify that no connection reaches the
  bound loopback listener after provider teardown.
- Construct a generic catalog-only provider and actually list the private snapshot's model,
  including its output limit. Then override its metadata API to OpenAI and Anthropic and verify
  the corresponding listing, probe, and version-header behavior.
- Check that the real Vertex definition retains catalog listing and has no metadata probe or
  headers, while the existing streaming regression continues to send valid requests.
- Extend the registry hook test: `none` removes OpenRouter's remote listing and custom probe but
  leaves its usage hook installed. Existing dialect-override tests remain in place.

### Validation

- Before implementation, the new checks failed 22 assertions in `providers/http_provider` and
  two in `providers/registry`, including unwanted loopback probe connections.
- After implementation, these focused targets passed:

  ```sh
  scripts/check.sh test providers/http_provider providers/registry providers/vertex \
      providers/anthropic_models providers/openai_models
  ```

- `make tests` passed all 121 tests with the external-network guard described in change 1.
- `make lint` and `git diff --check` passed; all touched C sources and headers were formatted.
- ASan/UBSan and TSan setup were retried and still fail to link because the runtime libraries
  listed in change 1 are missing. No sanitizer pass is claimed.

No live Google Cloud endpoint or real Google credential was used for these checks.

### Next change

Keep the `vertex` provider ID, change its display name to `google vertex`, and shorten picker
availability reasons while retaining full setup advice in request diagnostics and documentation.

## 5. Clarify the Vertex name and picker availability reasons

### Problem

The display label `Vertex AI` did not identify Google consistently with the other provider names.
Unavailable picker rows also contained complete setup instructions, making them unnecessarily long.
For delegated ADC credentials, the picker reported availability even when gcloud was absent; the
first request then failed with only a generic command-failure message.

### Changes

- Change the default display name to lowercase `google vertex`. The stable provider ID remains
  `vertex`, so command-line selection, configuration keys, and stored identities do not change.
  Explicit `providers.vertex.display_name` overrides still work.
- Use `project not set`, `ADC unavailable`, and `gcloud not found` as the picker reasons. Keep
  availability checks silent and local: they inspect configuration, credential files, and executable
  presence without performing HTTP requests or launching gcloud.
- Use `ADC unavailable` rather than `ADC not found` because the current loader also reports failure
  for malformed files and incomplete user credentials. More precise missing/unreadable/malformed
  classification remains part of the planned auth hardening; this piece does not change ADC parsing.
- Require an executable gcloud only for credentials delegated to it. Native `authorized_user`
  refresh and literal access tokens remain usable without the CLI.
- Resolve gcloud with the shared `fs_which()` helper before a delegated request, run the resolved
  executable, and report installation/PATH advice plus the explicit-token alternative if it is
  absent. Picker discovery uses the same lookup rules. Existing missing-project and missing-ADC
  diagnostics retain their detailed setup instructions.
- Document the short reasons and their remedies, and state explicitly that this provider serves
  Claude on Google Vertex AI, not Gemini. A locally available credential source is not a guarantee
  of valid tokens or project permissions. Add an Unreleased changelog entry.

The later auth-module extraction will move credential status behind its planned small interface;
this change does not introduce that boundary early or duplicate the credential parser.

### Regression coverage

- Check the default display label, unchanged `vertex` ID, and custom display-name override.
- Exercise missing project, missing ADC, malformed JSON, and incomplete user credentials, asserting
  exact short reasons and no emitted diagnostics.
- Check that native user credentials and literal tokens remain available with no gcloud on PATH.
- Check delegated credentials with missing, non-executable, and executable fake gcloud files. A
  marker-writing stub proves that availability does not run it; a bound loopback token endpoint
  receives no connection during the checks.
- Verify that auth request failures retain login/token guidance for missing ADC and give explicit
  Google Cloud CLI installation/PATH guidance for missing gcloud. Existing fake-gcloud token and
  redaction tests continue to exercise successful delegated requests.

### Validation

- Before implementation, the updated checks failed 14 assertions across `providers/vertex`,
  `providers/vertex_auth`, and `providers/registry`.
- After implementation, all five focused targets passed:

  ```sh
  scripts/check.sh test providers/vertex providers/vertex_auth providers/registry \
      providers/http_provider e2e/vertex
  ```

- `make tests` passed all 121 tests with the external-network guard described in change 1.
- `make lint` and `git diff --check` passed; all touched C sources and headers were formatted.
- Retried ASan/UBSan and TSan setup. Both remain blocked by the missing sanitizer runtime
  libraries listed in change 1; no sanitizer pass is claimed.

No real Google credentials or public endpoints were used; gcloud was a local test stub.

### Next change

Reconcile explicit hax settings with Google's conventional environment variables: remove
`env_var_alt`, establish precedence, and resolve project/location consistently for the endpoint host
and request path.

## 6. Separate hax settings from Google environment fallbacks

### Problem

Google's conventional variables were registered directly as hax setting aliases, including a
one-off `env_var_alt` field added only for Vertex. That mixed external Google inputs into the
process-wide config registry and let the registry's location default mask provider-owned fallback
logic. Moving those reads into Vertex without changing path construction would instead resolve the
host correctly but leave `{project}` or `{location}` empty in the request path.

### Changes

- Remove `env_var_alt` and its second environment lookup from the generic config registry.
- Give the three explicit hax settings conventional hax-owned aliases:
  `HAX_VERTEX_PROJECT`, `HAX_VERTEX_LOCATION`, and secret `HAX_VERTEX_ACCESS_TOKEN`.
- Move Google's variables into Vertex resolution. Project and location now follow one rule:
  non-empty hax setting, primary Google variable, alternate variable, then provider default.
  Project has no default; location defaults to `us-east5`. Access tokens use the non-empty hax
  setting first and `GOOGLE_OAUTH_ACCESS_TOKEN` second.
- Remove the location default from the registry. It now reports no configured value when only an
  external Google variable or the provider default is active, instead of hiding those fallbacks.
- Resolve project and location through one shared Vertex settings function for endpoint host,
  request path, and picker availability.
- Add a narrow `resolve_path` definition hook. Vertex uses it to build the raw-predict path from
  the same project/location precedence as its host rather than asking generic HTTP-provider code
  to interpret Google environment variables. Other providers retain existing path-template
  behavior.
- Document the exact precedence, empty-value behavior, aliases, and `us-east5` default. Add an
  Unreleased changelog entry.

An explicit `providers.vertex.base_url` still overrides host selection only. Validation of required
path values under that override and generic placeholder hardening belong to the next endpoint piece.

### Regression coverage

- Check regional, global, `us`, and `eu` hosts together with their fully resolved request paths.
- Verify hax config values beat populated primary and alternate Google variables.
- Verify empty hax values fall through, primary variables beat alternates, empty primaries fall
  through to alternates, and location reaches `us-east5` only after every other source is absent.
- Verify `HAX_VERTEX_PROJECT` and `HAX_VERTEX_LOCATION` participate in normal config resolution and
  beat Google variables; empty aliases fall through.
- Check that Google's project/location variables no longer report themselves as config-registry
  sources, and that the registry itself has no location default.
- Check registry aliases and secrecy for all three settings, and that the shipped provider uses
  the narrow path hook.
- Verify configured and HAX access tokens beat `GOOGLE_OAUTH_ACCESS_TOKEN`, while an empty HAX
  token falls through to Google's value. Existing trace assertions cover every selected token.
- Keep the full streaming test as an integration check that project and location still appear in
  the raw-predict request path.

### Validation

- All six focused targets passed:

  ```sh
  scripts/check.sh test config providers/vertex providers/vertex_auth providers/http_provider \
      providers/registry e2e/vertex
  ```

- `make tests` passed all 121 tests with Google and hax Vertex variables cleared in the external
  network guard.
- `make lint` and `git diff --check` passed; every touched C source/header was formatted.
- ASan/UBSan and TSan setup were retried and remain blocked by the missing runtime libraries
  listed in change 1; no sanitizer pass is claimed.

No real Google credentials or public endpoints were used.

### Next change

Harden endpoint construction: replace ad-hoc placeholder expansion, resolve every model occurrence,
fail required unresolved placeholders, validate project/location under base URL overrides, avoid
host truncation, and remove duplicate construction diagnostics.

## 7. Harden endpoint placeholder expansion and Vertex validation

### Problem

HTTP-provider path templates had a second, ad-hoc replacement implementation. Missing values were
silently deleted, `{model}` replaced only its first occurrence, and port parsing was duplicated.
Vertex host formatting used a fixed 128-byte buffer, so long locations could be truncated into a
different hostname. A custom base URL bypassed project validation, and a failed base resolver was
followed by a generic `no base_url` diagnostic even though the hook had already explained the
failure.

### Changes

- Resolve named path-template values with the shared `placeholder_expand()` primitive. It replaces
  every occurrence, including repeated config values and repeated `{model}` placeholders.
- Preserve `{model}` until request time, but fail construction with one diagnostic when another
  placeholder has no non-empty value, when `{port}` cannot resolve, or when an opening brace is
  unterminated. Required values are no longer silently removed from the request path.
- Extract one typed, bounded port resolver and use it for both base URL and path templates. Existing
  malformed/out-of-range fallback behavior for shipped local providers is unchanged.
- Resolve path templates before provider discovery or auth-state construction, so endpoint failures
  do not acquire resources that then need partial-provider cleanup.
- Treat a NULL `resolve_path` result as a reported construction failure. The hook contract now
  states this explicitly.
- Validate Vertex project and location in Vertex code. Projects accept lowercase project IDs,
  numbers, and domain-scoped forms without URL separators; locations accept lowercase letters,
  digits, and hyphens and fit one DNS label. Both are validated when a custom base URL is present.
- Replace `dynamic_host[128]` with allocated formatting. Valid location text is either represented
  completely or rejected; it is never truncated into another endpoint.
- Respect the existing base-resolver diagnostic contract: generic construction adds `no base_url`
  only when no resolver hook exists. Vertex therefore emits exactly one actionable error.
- Document endpoint validation and add an Unreleased changelog entry.

Project/location fallback remains in Vertex's resolver, not generic template code. This piece does
not add URL encoding or automatic model-name conversion; model IDs retain their existing endpoint
spelling.

### Regression coverage

- Send a request through a generic path template containing a repeated config placeholder, two
  `{model}` occurrences, and `{port}`. Assert all occurrences and the def fallback port in the
  captured request target.
- Check that a missing named value, unresolved port, and unterminated placeholder each reject
  construction with exactly one diagnostic.
- Use a reporting base resolver that returns NULL and verify generic construction does not append a
  second diagnostic.
- Construct Vertex without a project both with and without an explicit base URL, asserting one
  diagnostic in each case.
- Reject project path injection, location path injection, and a location longer than a DNS label
  even under a base override.
- Resolve a valid 63-character location and compare the complete allocated regional URL.
- Retain the successful explicit-base streaming test, which checks the full project/location/model
  raw-predict target.

### Validation

- All five focused targets passed:

  ```sh
  scripts/check.sh test providers/http_provider providers/vertex providers/registry \
      text/placeholder e2e/vertex
  ```

- `make tests` passed all 121 tests under the external-network and credential guard.
- `make lint` and `git diff --check` passed; all touched C sources and headers were formatted.
- ASan/UBSan and TSan setup were retried and remain blocked by the missing runtime libraries listed
  in change 1; no sanitizer pass is claimed.

All endpoint requests used loopback fixtures; no Google endpoint or credential was used.

### Next change

Extract Google ADC credential loading, token renewal, auth operations, and credential status into
`vertex_auth.{c,h}` before hardening non-refresh preparation, cancellation, recovery, and ADC
support.

## 8. Extract Vertex authentication and harden refresh semantics

### Problem

Credential loading, renewal, and the auth hooks lived inside `vertex.c` next to endpoint
construction, so availability had to construct and inspect a private credential session. `tick`
was ignored during credential acquisition, so a cancelled caller could not stop an OAuth refresh
or a slow gcloud child. Preparation without refresh could still spawn network work after a reload,
could report success for an expired renewable token, and a rejected literal was always resent even
when it had not changed. The refresh exchange also lacked error classification, gcloud's alternate
Cloud SDK directory was ignored, and a non-object ADC root leaked.

### Changes

- Move credential loading, token renewal, the credential session, and all `http_auth_ops` into a
  new `vertex_auth.{c,h}` module. Endpoint construction and picker presentation stay in `vertex.c`.
  Register the source in `meson.build` and mirror the boundary in tests, which now include the auth
  header directly. `vertex.c` keeps the project/location validation and build hooks only.
- Expose `vertex_auth_local_status()` as the picker's credential check. It classifies an explicit
  token, a native `authorized_user` file, and delegated kinds, reports `ADC unavailable` or
  `gcloud not found`, and never performs network I/O or executes gcloud.
- Honor `CLOUDSDK_CONFIG` as the alternate Cloud SDK directory when
  `GOOGLE_APPLICATION_CREDENTIALS` is unset. Release a successfully parsed non-object ADC JSON root
  instead of leaking it.
- With refresh disallowed, preparation only reloads local credentials: it never performs HTTP or
  spawns gcloud, fails when no usable token exists, and fails for a token the early-refresh margin
  marks as already expiring instead of reporting success. A fresh token remains usable.
- Thread `tick` through the OAuth HTTP request and use a cancelled call's tick when dispatching to
  gcloud. Extend the shared process facility with `spawn_capture_stdout_checked()`, which polls an
  optional cancellation callback and kills and reaps the child on cancellation or failure, keeping
  the existing bounded timeout and output behavior.
- Refuse to resend an unchanged rejected literal token; permit exactly one retry when re-reading
  the literal produces a different token. A renewable source recovers with one forced renewal
  regardless of whether the new token string changed. Re-classify the credential source after each
  reload so a transition to a literal never runs the old exchange with missing ADC fields.
- Trim and reject whitespace-only gcloud output. Apply the early-refresh margin to user tokens as
  well: a token not outliving the margin is treated as already expiring.
- Classify refresh errors: preserve bounded OAuth details, distinguish server-reported
  `invalid_grant` (with re-authentication advice) from transport and other server failures, and
  never expose credential values in diagnostics.
- Narrow the documented ADC scope: metadata-server credentials (Compute Engine, Cloud Run, GKE)
  are an explicit non-goal of this change. The initial feature supports explicit tokens and file
  credentials only. Add an Unreleased changelog entry.

### Regression coverage

- Preparation with refresh disallowed and no token fails locally with a bound token endpoint
  receiving no connection; after one permitted refresh the same call succeeds without further
  network work.
- A 1-second user token becomes "already expiring" through the margin, so refresh-disabled
  preparation fails and no second connection occurs.
- Rejected literals: unchanged means no resend; an env-visible change allows one retry, then stops.
- Source transition: a gcloud-delegated source switches to a literal during recovery and adopts it.
- Whitespace-only gcloud output fails with a "no usable token" diagnostic.
- A `sleep 30` gcloud stub with an immediately-cancelling tick returns within 5 seconds, proving
  the child is killed rather than waited out.
- An OAuth endpoint replying `{"error":"invalid_grant"}` produces a message with the code and
  re-authentication advice.
- Credential-path precedence: `CLOUDSDK_CONFIG` supplies the ADC file when the application
  credential is unset; an explicitly configured file wins even when `CLOUDSDK_CONFIG` is valid.
- Existing trace-redaction, refresh, recovery, setup-diagnostics, availability, and streaming tests
  keep passing against the moved module.

### Validation

- All seven focused targets passed before and after the boundary move:

  ```sh
  scripts/check.sh test providers/vertex providers/vertex_auth providers/http_provider \
      providers/registry config text/placeholder e2e/vertex
  ```

- `make tests` passed all 121 tests, including lint (`make lint`) and `git diff --check`.
- ASan/UBSan and TSan setup were retried and remain blocked by the missing sanitizer runtime
  libraries listed in change 1; no sanitizer pass is claimed.

No real Google credentials, endpoints, or installed gcloud were used; gcloud was a local test stub.

### Next change

Metadata-server ADC credentials were deliberately deferred and the documentation narrowed
accordingly. The remaining auth follow-ups are: metadata support; distinguishing absent, unreadable,
and malformed credential files; and preserving the ADC file across refreshes. Then continue with
payload-error classification, test consolidation, documentation cleanup, and final validation.

## 9. Distinguish ADC credential faults

### Problem

`load_adc` collapsed every failure into a NULL root, so an absent file, an unreadable one, and a
file that is not valid JSON all produced the same "no Google ADC credentials" diagnostic. Users
could not tell whether to authenticate, fix permissions, or repair the file.

### Changes

- Return a load outcome from `load_adc`: absent (no file at the resolved path), unreadable (a file
  exists but could not be opened), or malformed (parses to something other than a JSON object).
  `ENOENT` separates absence from permission and I/O errors.
- Report each class distinctly on the request path. Absence keeps the login/token guidance; an
  unreadable file points at permissions on `GOOGLE_APPLICATION_CREDENTIALS`; malformed content
  points at re-running `gcloud auth application-default login`.
- Keep the picker's concise `ADC unavailable` reason unchanged; the added detail belongs to request
  diagnostics, not to a picker row.
- Document the distinction in the provider guide and add an Unreleased changelog entry.

Delegated credential kinds (service account and unrecognized shapes) remain the gcloud path, as
before. Metadata-server credentials stay a documented, deferred follow-up; this change does not
add fallbacks to unreadable or malformed configured files.

### Regression coverage

- A path with no file fails preparation with "no Google ADC credentials".
- A `chmod 000` file carrying valid user ADC fails with an "unreadable" diagnostic.
- A non-JSON file fails with a "not valid JSON" diagnostic.
- Existing missing-ADC, malformed, and unavailable picker assertions still pass against the moved
  module and the status interface.

### Validation

- The focused `providers/vertex_auth` and `providers/vertex` targets pass.
- `make tests` passed all 121 tests under the external-network and credential guard; `make lint`
  and `git diff --check` passed.
- ASan/UBSan and TSan setup remain blocked by the missing sanitizer runtime libraries listed in
  change 1; no sanitizer pass is claimed.

### Next change

Add metadata-server ADC support, which was deferred in change 8: a bounded, cancellable, proxy-
bypassing token request with `Metadata-Flavor: Google`, falling back only when no higher-priority
credential source exists. Then continue with payload-error classification and test consolidation.
