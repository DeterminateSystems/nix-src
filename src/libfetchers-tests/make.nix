# The unit tests of libnixfetchers, transcribed from meson.build and
# package.nix: the `nix-fetchers-tests` executable, with the derivation
# that runs it as `tests.run`.
{
  nixMake,
  nix-store-test-support,
  nix-fetchers,
  nix-fetchers-c,
}:

let
  tests = nixMake.mkComponent {
    name = "determinate-nix-fetchers-tests";
    type = "executable";
    exeName = "nix-fetchers-tests";

    deps = [
      nix-store-test-support
      nix-fetchers
      nix-fetchers-c
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
