# libnixflakec, the C API of libnixflake, transcribed from meson.build.
{
  nixMake,
  nix-util,
  nix-util-c,
  nix-store,
  nix-store-c,
  nix-fetchers,
  nix-fetchers-c,
  nix-expr,
  nix-expr-c,
  nix-flake,
}:

nixMake.mkComponent {
  name = "determinate-nix-flake-c";
  libName = "nixflakec";

  deps = [
    nix-util
    nix-util-c
    nix-store
    nix-store-c
    nix-fetchers
    nix-fetchers-c
    nix-expr
    nix-expr-c
    nix-flake
  ];

  root = ./.;

  includeDirs = [ "" ];
  # The headers live in the root, which the default would not export.
  publicIncludeDirs = [ "" ];

  files = nixMake.commonSupportFiles;

  linkFlags = [ "-Wl,--wrap=__assert_fail" ];
}
