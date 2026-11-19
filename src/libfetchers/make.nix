# libnixfetchers, transcribed from meson.build.
{
  nixMake,
  nix-util,
  nix-store,
}:

nixMake.mkComponent {
  name = "determinate-nix-fetchers";
  libName = "nixfetchers";

  deps = [
    nix-util
    nix-store
  ];

  root = ./.;

  includeDirs = [
    ""
    "include"
  ];

  files = nixMake.commonSupportFiles;

  linkFlags = nixMake.wrapAssertFailFlags;
}
