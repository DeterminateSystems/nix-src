# Run the functional tests against the nix-make-built `nix`, using
# the test runner (no Meson). The output holds the test logs.
{
  lib,
  stdenv,
  test-runner,
  nix,

  # The test scripts to run, relative to this directory. (Not named `tests`,
  # since `callPackage` would then pass nixpkgs' `tests` attribute.)
  testScripts ? [ "simple.sh" ],
}:

derivation {
  name = "${nix.name}-functional-tests";
  system = stdenv.hostPlatform.system;
  builder = stdenv.shell;
  args = [
    "-e"
    (builtins.toFile "builder.sh" ''
      if [ -e "$NIX_ATTRS_SH_FILE" ]; then . "$NIX_ATTRS_SH_FILE"; fi
      source $stdenv/setup >/dev/null
      run-tests \
        --nix-bin-dir "$nix/bin" \
        --source-dir "$src/tests/functional" \
        --build-dir "$TMPDIR/build" \
        "''${testScripts[@]}"
      mkdir -p "$out"
      cp -r "$TMPDIR/build/logs" "$out/"
    '')
  ];
  __structuredAttrs = true;
  inherit stdenv nix testScripts;
  # Rooted at the repository so that the `.version` symlink resolves.
  src = lib.fileset.toSource {
    root = ../..;
    fileset = lib.fileset.unions [
      ./.
      ../../.version
    ];
  };
  nativeBuildInputs = [ test-runner ];
  # Required for HTTP `nix serve`-based tests in binary-cache.sh
  __darwinAllowLocalNetworking = true;
}
