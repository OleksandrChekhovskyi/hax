{
  description = "hax - a minimalist, terminal-native coding agent written in C";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-parts.url = "github:hercules-ci/flake-parts";
    git-hooks-nix = {
      url = "github:cachix/git-hooks.nix";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    inputs@{ flake-parts, ... }:
    flake-parts.lib.mkFlake { inherit inputs; } {
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "x86_64-darwin"
        "aarch64-darwin"
        "x86_64-freebsd"
      ];

      imports = [ inputs.git-hooks-nix.flakeModule ];

      flake.overlays.default = final: _prev: {
        hax = final.callPackage ./nix/hax.nix {
          src = ./.;
          inherit (final) coreutils tmux;
        };
      };

      perSystem =
        { config, pkgs, ... }:
        let
          hax = pkgs.callPackage ./nix/hax.nix {
            inherit (pkgs) tmux coreutils;
            src = ./.;
          };
        in
        {
          packages = {
            default = hax;
            inherit hax;
          };

          apps = {
            default = {
              type = "app";
              program = "${hax}/bin/hax";
            };
            hax = {
              type = "app";
              program = "${hax}/bin/hax";
            };
          };

          pre-commit.settings = {
            hooks = {
              nixfmt.enable = true;
              deadnix.enable = true;
              statix.enable = true;

              clang-format = {
                enable = true;
                name = "clang-format";
                package = pkgs.clang-tools;
                entry = "${pkgs.clang-tools}/bin/clang-format -i";
                files = "\\.(c|h)$";
              };

              shellcheck = {
                enable = true;
                args = [ "-x" ];
              };

              hax-lint-style = {
                enable = true;
                name = "hax lint_style.py";
                package = pkgs.python3;
                entry = "python3 scripts/lint_style.py";
                files = "\\.(c|h)$";
                pass_filenames = false;
              };

              # Full project lint (clang-tidy + clang-format check + lint_style)
              # is version-sensitive and heavy; available on demand only.
              hax-lint = {
                enable = true;
                name = "hax make lint";
                package = pkgs.gnumake;
                entry = "make lint";
                pass_filenames = false;
                stages = [ "manual" ];
                extraPackages = [
                  pkgs.meson
                  pkgs.ninja
                  pkgs.pkg-config
                  pkgs.python3
                  pkgs.clang-tools
                  pkgs.curl
                  pkgs.jansson
                ];
              };
            };
          };

          devShells.default = pkgs.mkShell {
            packages = [
              pkgs.gcc
              pkgs.meson
              pkgs.ninja
              pkgs.pkg-config
              pkgs.python3
              pkgs.fzf
              pkgs.tmux
              pkgs.clang-tools
              pkgs.gnumake
            ]
            ++ config.pre-commit.settings.enabledPackages;

            shellHook = ''
              ${config.pre-commit.installationScript}
            '';
          };

          formatter = pkgs.writeShellScriptBin "fmt" ''
            ${pkgs.lib.getExe config.pre-commit.settings.package} run --all-files --config ${config.pre-commit.settings.configFile}
          '';
        };
    };
}
