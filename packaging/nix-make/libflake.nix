# libnixflake, transcribed from src/libflake/meson.build.
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

  roots = [
    {
      root = ../../src/libflake;
      prefix = "";
    }
  ];

  includeDirs = [
    ""
    "include"
  ];

  files = nixMake.commonSupportFiles // {
    "call-flake.nix.gen.hh" = nixMake.mkStringHeader ../../src/libflake/call-flake.nix;
  };

  linkFlags = [ "-Wl,--wrap=__assert_fail" ];
}
