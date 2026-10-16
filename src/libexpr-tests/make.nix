# The unit tests of libnixexpr, transcribed from meson.build and
# package.nix: the `nix-expr-tests` executable, with the derivation that
# runs it as `tests.run`.
{
  pkgs,
  nixMake,
  nix-expr,
  nix-expr-c,
  nix-expr-test-support,
}:

let
  inherit (pkgs) lib;

  tests = nixMake.mkComponent {
    name = "determinate-nix-expr-tests";
    type = "executable";
    exeName = "nix-expr-tests";

    deps = [
      nix-expr
      nix-expr-c
      nix-expr-test-support
    ];

    root = ./.;

    # The benchmarks (Meson option `benchmarks`, off by default).
    excludeSources = [
      "bench-main.cc"
      "dynamic-attrs-bench.cc"
      "get-drvs-bench.cc"
      "regex-cache-bench.cc"
    ];

    includeDirs = [ "" ];

    files = nixMake.commonSupportFiles;

    configHeaders = {
      "expr-tests-config.hh" = {
        PACKAGE_VERSION = lib.fileContents ../../.version;
      };
    };

    linkFlags = [ "-Wl,--wrap=__assert_fail" ];
  };
in

tests
// {
  tests.run = nixMake.runTest {
    name = "${tests.name}-run";
    command = [ "${tests}/bin/${tests.exeName}" ];
    env = {
      _NIX_TEST_UNIT_DATA = ./data;
      NIX_STORE = "";
    };
  };
}
