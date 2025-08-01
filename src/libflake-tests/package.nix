{
  lib,
  buildPackages,
  stdenv,
  mkMesonExecutable,

  nix-flake,
  nix-flake-c,
  nix-expr-test-support,

  rapidcheck,
  gtest,
  runCommand,

  # Configuration Options

  version,
  resolvePath,
}:

let
  inherit (lib) fileset;
in

mkMesonExecutable (finalAttrs: {
  pname = "nix-flake-tests";
  inherit version;

  workDir = ./.;
  fileset = fileset.unions [
    ../../nix-meson-build-support
    ./nix-meson-build-support
    ../../.version
    ./.version
    ./meson.build
    # ./meson.options
    (fileset.fileFilter (file: file.hasExt "cc") ./.)
    (fileset.fileFilter (file: file.hasExt "hh") ./.)
  ];

  buildInputs = [
    nix-flake
    nix-flake-c
    nix-expr-test-support
    rapidcheck
    gtest
  ];

  mesonFlags = [
  ];

  passthru = {
    tests = {
      run =
        runCommand "${finalAttrs.pname}-run"
          {
            meta.broken = !stdenv.hostPlatform.emulatorAvailable buildPackages;
          }
          (''
            export _NIX_TEST_UNIT_DATA=${resolvePath ./data}
            export HOME="$TMPDIR/home"
            mkdir -p "$HOME"
            ${stdenv.hostPlatform.emulator buildPackages} ${lib.getExe finalAttrs.finalPackage}
            touch $out
          '');
    };
  };

  meta = {
    platforms = lib.platforms.unix ++ lib.platforms.windows;
    mainProgram = finalAttrs.pname + stdenv.hostPlatform.extensions.executable;
  };

})
