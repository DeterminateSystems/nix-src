# The unit tests of libnixstore, transcribed from meson.build and
# package.nix: the `nix-store-tests` executable, with the derivation that
# runs it as `tests.run`.
{
  pkgs,
  nixMake,
  nix-store,
  nix-store-c,
  nix-store-test-support,
}:

let
  inherit (pkgs) lib;

  tests = nixMake.mkComponent {
    name = "determinate-nix-store-tests";
    type = "executable";
    exeName = "nix-store-tests";

    deps = [
      nix-store
      nix-store-c
      nix-store-test-support
    ];

    root = ./.;

    # The benchmarks (Meson option `benchmarks`, off by default).
    excludeSources = [
      "bench-main.cc"
      "derivation-parser-bench.cc"
      "ref-scan-bench.cc"
      "register-valid-paths-bench.cc"
    ];

    includeDirs = [ "" ];

    files = nixMake.commonSupportFiles;

    configHeaders = {
      "store-tests-config.hh" = {
        PACKAGE_VERSION = lib.fileContents ../../.version;
        NIX_STORE_DIR = "/nix/store";
      };
    };

    linkFlags = [ "-Wl,--wrap=__assert_fail" ];
  };

  # Some data is shared with the functional tests: they create it, we
  # consume it. Rooted at the repository so that both are present.
  data = lib.fileset.toSource {
    root = ../..;
    fileset = lib.fileset.unions [
      ./data
      ../../tests/functional/derivation
    ];
  };
in

tests
// {
  tests.run = nixMake.runTest {
    name = "${tests.name}-run";
    command = [ "${tests}/bin/${tests.exeName}" ];
    env = {
      _NIX_TEST_UNIT_DATA = "${data}/src/libstore-tests/data";
      NIX_REMOTE = "$HOME/store";
    };
    # The HTTPS binary cache tests generate certificates with `openssl`.
    nativeBuildInputs = [ pkgs.openssl ];
  };
}
