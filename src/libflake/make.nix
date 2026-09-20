# libnixflake, transcribed from meson.build.
{
  nixMake,
  nix-util,
  nix-store,
  nix-fetchers,
  nix-expr,
}:

nixMake.mkComponent {
  name = "nix-flake";
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

  files = nixMake.commonSupportFiles // {
    "call-flake.nix.gen.hh" = nixMake.mkStringHeader ./call-flake.nix;
  };

  linkFlags = [ "-Wl,--wrap=__assert_fail" ];
}
