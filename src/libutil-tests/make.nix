# The unit tests of libnixutil, transcribed from meson.build and
# package.nix: the `nix-util-tests` executable, with the derivations that
# run it as `tests.run` and `tests.run-without-new-syscalls`.
{
  pkgs,
  nixMake,
  nix-util,
  nix-util-c,
  nix-util-test-support,
}:

let
  inherit (pkgs) lib;
  inherit (nixMake.hostPlatform) isLinux;

  tests = nixMake.mkComponent {
    name = "determinate-nix-util-tests";
    type = "executable";
    exeName = "nix-util-tests";

    deps = [
      nix-util
      nix-util-c
      nix-util-test-support
    ];

    root = ./.;

    # libnixutil only interposes __cxa_throw on Linux (see
    # src/libutil/make.nix).
    excludeSources = lib.optional (!isLinux) "cxa-throw.cc";

    includeDirs = [ "" ];

    files = nixMake.commonSupportFiles;

    configHeaders = {
      "util-tests-config.hh" = {
        PACKAGE_VERSION = lib.fileContents ../../.version;
        HAVE_CXA_THROW = if isLinux then 1 else 0;
      };
    };

    # The tests have no main() of their own; it comes from gtest_main,
    # which no #include refers to, so it is linked explicitly (after the
    # -lgtest from pkg-config, which also provides its -L).
    extraLinkLibs = [ "-lgtest_main" ];

    linkFlags = nixMake.wrapAssertFailFlags;
  };

  exe = "${tests}/bin/${tests.exeName}";

  env = {
    _NIX_TEST_UNIT_DATA = ./data;
  };
in

tests
// {
  tests = {
    run = nixMake.runTest {
      name = "${tests.name}-run";
      command = [ exe ];
      inherit env;
    };
  }
  # Run the same tests with newer syscalls disabled via seccomp, to
  # exercise fallback paths (iterative openat for openat2, /proc/self/fd
  # for fchmodat2). Linux only, as in Meson.
  // lib.optionalAttrs isLinux {
    run-without-new-syscalls = nixMake.runTest {
      name = "${tests.name}-run-without-new-syscalls";
      command = [
        "enosys"
        "--syscall"
        "openat2"
        "--syscall"
        "fchmodat2"
        "--"
        exe
      ];
      inherit env;
      nativeBuildInputs = [ pkgs.util-linux ];
    };
  };
}
