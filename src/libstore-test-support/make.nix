# libnix-store-test-support, helpers for the unit tests of libnixstore and
# the components built on it, transcribed from meson.build.
{
  nixMake,
  nix-util,
  nix-util-test-support,
  nix-store,
  nix-store-c,
}:

nixMake.mkComponent {
  name = "determinate-nix-store-test-support";
  libName = "nix-store-test-support";

  deps = [
    nix-util
    nix-util-test-support
    nix-store
    nix-store-c
  ];

  root = ./.;

  includeDirs = [ "include" ];

  files = nixMake.commonSupportFiles;

  linkFlags = [ "-Wl,--wrap=__assert_fail" ];
}
