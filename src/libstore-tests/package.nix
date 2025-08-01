{
  lib,
  buildPackages,
  stdenv,
  mkMesonExecutable,

  nix-store,
  nix-store-c,
  nix-store-test-support,
  sqlite,

  rapidcheck,
  gtest,
  runCommand,

  # Configuration Options

  version,
  filesetToSource,
}:

let
  inherit (lib) fileset;
in

mkMesonExecutable (finalAttrs: {
  pname = "nix-store-tests";
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

  # Hack for sake of the dev shell
  passthru.externalBuildInputs = [
    sqlite
    rapidcheck
    gtest
  ];

  buildInputs = finalAttrs.passthru.externalBuildInputs ++ [
    nix-store
    nix-store-c
    nix-store-test-support
  ];

  mesonFlags = [
  ];

  passthru = {
    tests = {
      run =
        let
          # Some data is shared with the functional tests: they create it,
          # we consume it.
          data = filesetToSource {
            root = ../..;
            fileset = lib.fileset.unions [
              ./data
              ../../tests/functional/derivation
            ];
          };
        in
        runCommand "${finalAttrs.pname}-run"
          {
            meta.broken = !stdenv.hostPlatform.emulatorAvailable buildPackages;
          }
          (''
            export _NIX_TEST_UNIT_DATA=${data + "/src/libstore-tests/data"}
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
