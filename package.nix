# Meson-based build of hax. The Nix build has no git checkout, so the `vcs_tag`
# step in meson.build falls back to the version this derivation reads from there.
{
  lib,
  stdenv,
  meson,
  ninja,
  pkg-config,
  curl,
  jansson,
  bash,
  git,
  systems,
  doCheck ? false,
}:

let
  lines = lib.splitString "\n" (builtins.readFile ./meson.build);
  matches = map (builtins.match ".*version: '([^']+)'.*") lines;
  versionMatch = lib.findFirst (m: m != null) null matches;
  version =
    if versionMatch == null then
      throw "could not parse the version from meson.build"
    else
      builtins.head versionMatch;
in
stdenv.mkDerivation {
  pname = "hax";
  inherit version;

  src = ./.;

  nativeBuildInputs = [ meson ninja pkg-config ] ++ lib.optionals doCheck [ bash git ];
  buildInputs = [ curl jansson ];

  inherit doCheck;

  # The e2e scenarios need a tty and tmux, so the check phase runs only the C
  # unit test binaries meson produces in the build tree. Two tests are excluded
  # because they exercise things the Nix build sandbox cannot provide — both run
  # fine via `make tests` on a normal host:
  #   - test_system_browser invokes absolute /usr/bin/printf and /bin/mv;
  #   - test_tools_write's setuid case needs chmod(04755), which is refused on the
  #     sandbox filesystem.
  checkPhase = ''
    runHook preCheck
    failed=
    for t in $(find . -type f -executable -name 'test_*' | sort); do
      case "$t" in
        *test_system_browser) echo "skip $t (needs /usr/bin/printf, /bin/mv)"; continue ;;
        *test_tools_write) echo "skip $t (setuid chmod unsupported in sandbox)"; continue ;;
      esac
      echo "=== $t ==="
      "$t" || failed="$failed $t"
    done
    if [ -n "$failed" ]; then echo "FAILED:$failed"; exit 1; fi
    runHook postCheck
  '';

  meta = with lib; {
    description = "A minimalist, terminal-native coding agent written in C";
    homepage = "https://github.com/OleksandrChekhovskyi/hax";
    license = licenses.mit;
    mainProgram = "hax";
    platforms = systems;
  };
}
