# Releasing

Releases are tag-driven: pushing a `v*` tag runs `.github/workflows/release.yml`, which publishes
a GitHub release with the source tarball, prebuilt Linux and macOS binaries, and a `SHA256SUMS`
file, using the matching `CHANGELOG.md` section as notes.

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

The tag push runs the full CI matrix and, in parallel, the release workflow: it fails early if
the tag does not match the `meson.build` version, builds and tests every artifact, publishes the
release, and finally bumps the downstream repositories below. Prerelease tags (`-rc.N`) are
published as GitHub prereleases and skip those bumps, so `releases/latest` and everything
downstream keep tracking the newest stable release.

The prebuilt binaries bundle libcurl (Linux) and jansson (macOS) at versions pinned, with their
checksums, in `scripts/build_linux.sh` and `scripts/build_macos.sh`; the remaining Linux
libraries come from Alpine's current packages. Before a release, bump a pin whose upstream has
shipped security fixes since.

## Downstream repositories

hax is packaged in the Homebrew tap
([homebrew-hax](https://github.com/OleksandrChekhovskyi/homebrew-hax)) and in the
[AUR](https://aur.archlinux.org/packages/hax), and the
[usehax.dev](https://github.com/OleksandrChekhovskyi/usehax.dev) website publishes its man pages.
The workflow bumps each through a script that can also be run by hand:

| Target | Script | Secret |
| --- | --- | --- |
| Homebrew tap | `scripts/maint/bump_homebrew.py` | `HOMEBREW_TAP_TOKEN` |
| AUR | `scripts/maint/bump_aur.py` | `AUR_SSH_KEY` |
| Website manual | `scripts/maint/bump_site.py` | `SITE_TOKEN` |

`HOMEBREW_TAP_TOKEN` and `SITE_TOKEN` are PATs with push access to their repositories; the
default workflow token only reaches this one. The workflow hands each to its script as
`GH_TOKEN`, which is the name a hand-run needs — `GH_TOKEN=$(gh auth token)` will do when that
login can push to the target.

`AUR_SSH_KEY` is an SSH private key whose public half sits on the AUR account. AUR keys are
account-scoped — there are no per-package deploy keys — so that key can write every package the
account maintains.

## When a run fails

Up to the publish step, a failed run leaves nothing external behind — for a transient failure
(a flaked test), just re-run the job from the Actions UI. Only when the fix needs new commits
must the tag be deleted and re-created on the fixed commit. If the publish step itself dies
mid-upload, delete the half-made GitHub release before re-running. If only a downstream bump
fails, the release itself is complete and self-contained and that target merely lags one
version: re-run the job, or run its script by hand. Re-runs are safe — the scripts refuse to
move a target backwards, and report a green no-op when it is already current.
