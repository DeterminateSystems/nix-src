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

  configHeaders = {
    "fetchers-config-private.hh" = {
      # The defaults of the Meson options `git-program` and `hg-program`.
      GIT_PROGRAM = "git";
      HG_PROGRAM = "hg";
    };
  };

  linkFlags = nixMake.wrapAssertFailFlags;
}
