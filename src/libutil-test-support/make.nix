# libnix-util-test-support, helpers for the unit tests of libnixutil and
# the components built on it, transcribed from meson.build.
{
  nixMake,
  nix-util,
  nix-util-c,
}:

nixMake.mkComponent {
  name = "determinate-nix-util-test-support";
  libName = "nix-util-test-support";

  deps = [
    nix-util
    nix-util-c
  ];

  root = ./.;

  # Helpers for running the tests under Wine.
  excludeSources = [ "windows" ];

  includeDirs = [ "include" ];

  files = nixMake.commonSupportFiles;

  linkFlags = nixMake.wrapAssertFailFlags;
}
