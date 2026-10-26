# Nixpkgs overlay exposing hax as `hax`.
{ systems }:
final: _prev: {
  hax = final.callPackage ./package.nix { inherit systems; };
}
