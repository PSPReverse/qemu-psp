{ pkgs ? import <nixpkgs> {} }:

let
  qemu-psp = import ./default.nix { inherit pkgs; };
in
pkgs.mkShell {
  inputsFrom = [ qemu-psp ];
  # --enable-debug uses -O0, which conflicts with NixOS default _FORTIFY_SOURCE
  hardeningDisable = [ "fortify" ];
}
