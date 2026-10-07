# libnixstorec, the C API of libnixstore, transcribed from meson.build.
{
  nixMake,
  nix-util,
  nix-util-c,
  nix-store,
}:

nixMake.mkComponent {
  name = "determinate-nix-store-c";
  libName = "nixstorec";

  deps = [
    nix-util
    nix-util-c
    nix-store
  ];

  root = ./.;

  includeDirs = [ "" ];
  # The headers live in the root, which the default would not export.
  publicIncludeDirs = [ "" ];

  files = nixMake.commonSupportFiles;

  extraCxxFlags = nixMake.weakVtablesFlags;

  linkFlags = [ "-Wl,--wrap=__assert_fail" ];
}
