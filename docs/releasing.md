# Releasing

Releases are tag-driven: pushing a `v*` tag publishes a GitHub release with the source tarball,
fully static Linux binaries for x86_64 and aarch64 (`scripts/build_static.sh`, each tarball
containing the binary under its canonical name `hax`), and a `SHA256SUMS` file, with the
matching `CHANGELOG.md` section as notes (`.github/workflows/release.yml`). Release binaries
build from a plain source snapshot and stamp exactly the declared project version, which the
workflow checks against the tag; dev builds stamp `git describe` — tag + distance + commit,
with a `+` suffix marking a dirty tree.

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
tag matches the meson version, builds and tests the static binaries and the dist tarball,
publishes the release, and finally points the downstream packages at the published source
tarball. Prerelease tags (`-rc.N`) are published as GitHub prereleases and skip both package
bumps, so `releases/latest` and the packages keep tracking the newest stable release.

## Downstream packages

hax is packaged in the Homebrew tap
([homebrew-hax](https://github.com/OleksandrChekhovskyi/homebrew-hax)) and in the
[AUR](https://aur.archlinux.org/packages/hax). The workflow bumps each through a script that
can also be run by hand:

| Package | Script | Secret |
| --- | --- | --- |
| Homebrew tap | `scripts/maint/bump_homebrew.py` | `HOMEBREW_TAP_TOKEN` |
| AUR | `scripts/maint/bump_aur.py` | `AUR_SSH_KEY` |

`HOMEBREW_TAP_TOKEN` is a PAT with push access to the tap; the default workflow token only
reaches this repository. The workflow hands it to the script as `GH_TOKEN`, which is the name a
hand-run needs — `GH_TOKEN=$(gh auth token)` will do when that login can push to the tap.
`AUR_SSH_KEY` is an SSH private key whose public half sits on the AUR account, in a field that
takes one key per line. AUR keys are account-scoped — there are no per-package deploy keys — so
that key can write every package the account maintains.

## When a run fails

Up to the publish step, a failed run leaves nothing external behind — for a transient failure
(a flaked test), just re-run the job from the Actions UI. Only when the fix needs new commits
must the tag be deleted and re-created on the fixed commit. If the publish step itself dies
mid-upload, delete the half-made GitHub release before re-running. If only a package bump
fails, the release itself is complete and self-contained and that package merely lags one
version: re-run the job, or run its script by hand. Re-runs are safe — the scripts refuse to
move a package backwards, and report a green no-op when it is already current.
