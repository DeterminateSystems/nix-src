# libnix-expr-test-support, helpers for the unit tests of libnixexpr and
# the components built on it, transcribed from meson.build.
{
  nixMake,
  nix-util,
  nix-util-test-support,
  nix-store,
  nix-store-test-support,
  nix-expr,
  nix-expr-c,
}:

nixMake.mkComponent {
  name = "determinate-nix-expr-test-support";
  libName = "nix-expr-test-support";

  deps = [
    nix-util
    nix-util-test-support
    nix-store
    nix-store-test-support
    nix-expr
    nix-expr-c
  ];

  root = ./.;

  includeDirs = [ "include" ];

  files = nixMake.commonSupportFiles;

  linkFlags = [ "-Wl,--wrap=__assert_fail" ];
}
