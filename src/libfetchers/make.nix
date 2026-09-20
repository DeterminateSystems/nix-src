# libnixfetchers, transcribed from meson.build.
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

  root = ./.;

  includeDirs = [
    ""
    "include"
  ];

  files = nixMake.commonSupportFiles // {
    "builtin-flake-registry.json.gen.hh" = nixMake.mkStringHeader ./builtin-flake-registry.json;
  };

  linkFlags = [ "-Wl,--wrap=__assert_fail" ];
}
