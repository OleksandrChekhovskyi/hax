# Releasing

Releases are tag-driven: pushing a `v*` tag publishes a GitHub release with the source tarball
and the matching `CHANGELOG.md` section as notes (`.github/workflows/release.yml`). The version
baked into the binary comes from `git describe`, so tagged builds report the tag, dev builds
report tag + distance + commit, and a `+` suffix marks a dirty tree.

Versions follow [Semantic Versioning](https://semver.org/): `vMAJOR.MINOR.PATCH`, with
`-rc.N` pre-release suffixes when a release candidate is warranted.

To cut a release:

1. Retitle the `## [Unreleased]` section in `CHANGELOG.md` to `## [X.Y.Z] - YYYY-MM-DD` and
   set `version:` in `meson.build` to `X.Y.Z`. Start a fresh `## [Unreleased]` section on top.
2. Commit (e.g. "Release v0.1.0") and push; wait for CI to go green.
3. Tag and push the tag:

   ```sh
   git tag -a vX.Y.Z -m "hax vX.Y.Z"
   git push origin vX.Y.Z
   ```

The tag push runs the full CI matrix and, in parallel, the release workflow: it verifies the
tag matches the meson version, builds and tests the dist tarball, and publishes the release.
Publishing is the workflow's last step, so a failed run leaves nothing external behind — for a
transient failure (a flaked test), just re-run the job from the Actions UI. Only when the fix
needs new commits must the tag be deleted and re-created on the fixed commit. If the publish
step itself dies mid-upload, delete the half-made GitHub release before re-running.
