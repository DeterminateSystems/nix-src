# libnixexprc, the C API of libnixexpr, transcribed from meson.build.
{
  nixMake,
  nix-util,
  nix-util-c,
  nix-store,
  nix-store-c,
  nix-expr,
}:

nixMake.mkComponent {
  name = "determinate-nix-expr-c";
  libName = "nixexprc";

  deps = [
    nix-util
    nix-util-c
    nix-store
    nix-store-c
    nix-expr
  ];

  root = ./.;

  includeDirs = [ "" ];
  # The headers live in the root, which the default would not export.
  publicIncludeDirs = [ "" ];

  files = nixMake.commonSupportFiles;

  extraCxxFlags = nixMake.weakVtablesFlags;

  linkFlags = [ "-Wl,--wrap=__assert_fail" ];
}
