# libnixutilc, the C API of libnixutil, transcribed from meson.build.
{
  pkgs,
  nixMake,
  nix-util,
}:

let
  inherit (pkgs) lib;
in

nixMake.mkComponent {
  name = "determinate-nix-util-c";
  libName = "nixutilc";

  deps = [ nix-util ];

  root = ./.;

  includeDirs = [ "" ];
  # The headers live in the root, which the default would not export.
  publicIncludeDirs = [ "" ];

  files = nixMake.commonSupportFiles;

  configHeaders = {
    "nix_api_util_config.h" = {
      PACKAGE_VERSION = lib.fileContents ../../.version;
    };
  };

  extraCxxFlags = nixMake.weakVtablesFlags;

  linkFlags = [ "-Wl,--wrap=__assert_fail" ];
}
