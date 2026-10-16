# libnixfetchersc, the C API of libnixfetchers, transcribed from meson.build.
{
  nixMake,
  nix-util,
  nix-util-c,
  nix-store,
  nix-store-c,
  nix-fetchers,
}:

nixMake.mkComponent {
  name = "determinate-nix-fetchers-c";
  libName = "nixfetchersc";

  deps = [
    nix-util
    nix-util-c
    nix-store
    nix-store-c
    nix-fetchers
  ];

  root = ./.;

  includeDirs = [ "" ];
  # The headers live in the root, which the default would not export.
  publicIncludeDirs = [ "" ];

  files = nixMake.commonSupportFiles;

  linkFlags = [ "-Wl,--wrap=__assert_fail" ];
}
