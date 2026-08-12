{
  description = "hax — a minimalist, terminal-native coding agent written in C.";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        inherit system;
        pkgs = nixpkgs.legacyPackages.${system};
      });
    in {
      packages = forAllSystems ({ pkgs, ... }: {
        default = pkgs.stdenv.mkDerivation {
          pname = "hax";
          version = "0.3.0";
          src = self;
          nativeBuildInputs = with pkgs; [ meson ninja pkg-config ];
          buildInputs = with pkgs; [ curl jansson ];
        };
      });

      devShells = forAllSystems ({ pkgs, ... }: {
        default = pkgs.mkShell {
          # Mirrors scripts/install_deps.sh: build/test deps, the optional
          # runtime tool (fzf, for @file completion), and the lint toolchain
          # (clang-format, clang-tidy via clang-tools).
          packages = with pkgs; [
            # build
            meson
            ninja
            pkg-config
            curl
            curl.dev
            jansson
            # tests (e2e is Python; interactive UI checks use tmux)
            python3
            tmux
            # optional runtime tool
            fzf
            # lint — clang-tools ships nixpkgs-wrapped clang-tidy/clang-format
            # that can find system headers, but omits the run-clang-tidy
            # Python driver that scripts/check.sh calls. Pull that one script
            # from clang-unwrapped; it shells out to the clang-tidy on PATH,
            # which resolves to the wrapped copy from clang-tools. Pinned to
            # LLVM 19 (~Debian stable) — the project's .clang-tidy targets
            # that range, and newer LLVMs surface unrelated new-check noise.
            llvmPackages_19.clang-tools
            (writeShellScriptBin "run-clang-tidy"
              ''exec ${llvmPackages_19.clang-unwrapped}/bin/run-clang-tidy "$@"'')
          ];
        };
      });

      formatter = forAllSystems ({ pkgs, ... }: pkgs.nixfmt);
    };
}
