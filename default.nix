{ pkgs ? import <nixpkgs> {} }:

pkgs.stdenv.mkDerivation {
  pname = "qemu-psp";
  version = "0.0.1";

  # gitTracked gets all committed files; unions in the meson subprojects
  # which are fetched via .wrap and not committed to git
  src = pkgs.lib.fileset.toSource {
    root = ./.;
    fileset = pkgs.lib.fileset.unions [
      (pkgs.lib.fileset.gitTracked ./.)
      ./subprojects/slirp
      ./subprojects/dtc
      ./subprojects/keycodemapdb
      ./subprojects/berkeley-softfloat-3
      ./subprojects/berkeley-testfloat-3
    ];
  };

  nativeBuildInputs = with pkgs; [
    meson
    ninja
    pkg-config
    (python3.withPackages (ps: [ ps.distlib ]))
    flex
    bison
    gnumake
  ];

  # --enable-debug uses -O0, which conflicts with NixOS's default _FORTIFY_SOURCE
  hardeningDisable = [ "fortify" ];

  buildInputs = with pkgs; [
    glib
    pixman
    zlib
    nettle
    gmp
  ];

  configurePhase = let
    rpath = pkgs.lib.makeLibraryPath [
      pkgs.glib
      pkgs.pixman
      pkgs.zlib
      pkgs.nettle
      pkgs.gmp
    ];
  in ''
    patchShebangs .
    ./configure \
      --target-list=arm-softmmu \
      --without-default-features \
      --enable-slirp \
      --enable-fdt=internal \
      --audio-drv-list= \
      --enable-debug \
      --enable-plugins \
      --enable-nettle \
      --prefix=$out \
      --extra-ldflags="-Wl,-rpath,${rpath}"
  '';

  buildPhase = ''
    ninja -C build
  '';

  installPhase = ''
    ninja -C build install
  '';
}
