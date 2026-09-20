# libnixcmd, transcribed from src/libcmd/meson.build.
{
  nixMake,
  nix-util,
  nix-store,
  nix-fetchers,
  nix-expr,
  nix-flake,
  nix-main,
}:

nixMake.mkComponent {
  name = "nix-cmd";
  libName = "nixcmd";

  deps = [
    nix-util
    nix-store
    nix-fetchers
    nix-expr
    nix-flake
    nix-main
  ];

  root = ../../src/libcmd;

  # unix/unix-socket-server.cc is a plain source file; there is no windows/ directory.
  includeDirs = [
    ""
    "include"
  ];

  files = nixMake.commonSupportFiles // {
    "call-flake-schemas.nix.gen.hh" = nixMake.mkStringHeader ../../src/libcmd/call-flake-schemas.nix;
    "builtin-flake-schemas.nix.gen.hh" =
      nixMake.mkStringHeader ../../src/libcmd/builtin-flake-schemas.nix;
  };

  configHeaders = {
    "cmd-config-private.hh" = {
      HAVE_LOWDOWN = 1;
      HAVE_LOWDOWN_1_4 = 1;
      HAVE_LOWDOWN_3 = 1;
      # Use editline rather than readline.
      USE_READLINE = 0;
    };
  };

  linkFlags = [ "-Wl,--wrap=__assert_fail" ];
}
