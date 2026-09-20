# libnixmain, transcribed from src/libmain/meson.build.
{
  nixMake,
  nix-util,
  nix-store,
  nix-expr,
}:

nixMake.mkComponent {
  name = "nix-main";
  libName = "nixmain";

  deps = [
    nix-util
    nix-store
    # Only for the NIX_USE_BOEHMGC macro, as in the Meson build.
    nix-expr
  ];

  roots = [
    {
      root = ../../src/libmain;
      prefix = "";
    }
  ];

  # unix/stack.cc is a plain source file; there is no windows/ directory.
  includeDirs = [
    ""
    "include"
  ];

  files = nixMake.commonSupportFiles;

  configHeaders = {
    "main-config-private.hh" = {
      HAVE_PUBSETBUF = 1;
    };
  };

  linkFlags = [ "-Wl,--wrap=__assert_fail" ];
}
