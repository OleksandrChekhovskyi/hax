{
  description = "hax — a minimalist, terminal-native coding agent";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { nixpkgs, ... }:
    let
      releasePlatforms = builtins.fromJSON (builtins.readFile ./release-platforms.json);
      systems = map (target: target.nix_system) releasePlatforms;
      perSystem = nixpkgs.lib.genAttrs systems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          package = pkgs.callPackage ./package.nix { inherit systems; };
          app = {
            type = "app";
            program = "${package}/bin/hax";
          };
        in {
          packages = {
            hax = package;
            default = package;
          };
          apps = {
            hax = app;
            default = app;
          };
          checks.hax = pkgs.callPackage ./package.nix { inherit systems; doCheck = true; };
        });
    in
    {
      packages = nixpkgs.lib.mapAttrs (_: outputs: outputs.packages) perSystem;
      apps = nixpkgs.lib.mapAttrs (_: outputs: outputs.apps) perSystem;
      checks = nixpkgs.lib.mapAttrs (_: outputs: outputs.checks) perSystem;
      overlays.default = import ./overlay.nix { inherit systems; };
    };
}
