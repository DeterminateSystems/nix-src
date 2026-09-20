# libnixfetchers, transcribed from src/libfetchers/meson.build.
{
  nixMake,
  nix-util,
  nix-store,
}:

nixMake.mkComponent {
  name = "nix-fetchers";
  libName = "nixfetchers";

  deps = [
    nix-util
    nix-store
  ];

  roots = [
    {
      root = ../../src/libfetchers;
      prefix = "";
    }
  ];

  includeDirs = [
    ""
    "include"
  ];

  files = nixMake.commonSupportFiles // {
    "builtin-flake-registry.json.gen.hh" =
      nixMake.mkStringHeader ../../src/libfetchers/builtin-flake-registry.json;
  };

  linkFlags = [ "-Wl,--wrap=__assert_fail" ];
}
