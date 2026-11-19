# libnixflake, transcribed from meson.build.
{
  nixMake,
  nix-util,
  nix-store,
  nix-fetchers,
  nix-expr,
}:

nixMake.mkComponent {
  name = "determinate-nix-flake";
  libName = "nixflake";

  deps = [
    nix-util
    nix-store
    nix-fetchers
    nix-expr
  ];

  root = ./.;

  includeDirs = [
    ""
    "include"
  ];

  files = nixMake.commonSupportFiles;

  linkFlags = nixMake.wrapAssertFailFlags;
}
