# The unit tests of libnixflake, transcribed from meson.build and
# package.nix: the `nix-flake-tests` executable, with the derivation that
# runs it as `tests.run`.
{
  nixMake,
  nix-expr-test-support,
  nix-flake,
  nix-flake-c,
}:

let
  tests = nixMake.mkComponent {
    name = "determinate-nix-flake-tests";
    type = "executable";
    exeName = "nix-flake-tests";

    deps = [
      nix-expr-test-support
      nix-flake
      nix-flake-c
    ];

    root = ./.;

    includeDirs = [ "" ];

    files = nixMake.commonSupportFiles;

    # The tests have no main() of their own; it comes from gtest_main
    # (see src/libutil-tests/make.nix).
    extraLinkLibs = [ "-lgtest_main" ];

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
