# Changelog

Notable user-facing changes, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/). Each release's section becomes its GitHub release
notes (see [docs/releasing.md](docs/releasing.md)).

## [Unreleased]

### Added

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
