# The `nix` executable, transcribed from src/nix/meson.build.
{
  pkgs,
  nixMake,
  nix-util,
  nix-store,
  nix-fetchers,
  nix-expr,
  nix-flake,
  nix-main,
  nix-cmd,
}:

let
  inherit (pkgs) lib;
in

nixMake.mkComponent {
  name = "nix";
  type = "executable";

  deps = [
    nix-util
    nix-store
    nix-fetchers
    nix-expr
    nix-flake
    nix-main
    nix-cmd
  ];

  root = ../../src/nix;

  # Every .cc file under the root (including the legacy commands in
  # subdirectories and unix/) is a compilation unit. The `doc`, `misc`
  # and `scripts` symlinks are ignored by the scanner.
  includeDirs = [ "" ];

  files = nixMake.commonSupportFiles // {
    "generate-manpage.nix.gen.hh" = nixMake.mkStringHeader ../../doc/manual/generate-manpage.nix;
    "generate-settings.nix.gen.hh" = nixMake.mkStringHeader ../../doc/manual/generate-settings.nix;
    "generate-store-info.nix.gen.hh" = nixMake.mkStringHeader ../../doc/manual/generate-store-info.nix;
    "utils.nix.gen.hh" = nixMake.mkStringHeader ../../doc/manual/utils.nix;
    "get-env.sh.gen.hh" = nixMake.mkStringHeader ../../src/nix/get-env.sh;
    "help-stores.md.gen.hh" = nixMake.mkStringHeader ../../doc/manual/source/store/types/index.md.in;
    "profiles.md.gen.hh" = nixMake.mkStringHeader ../../doc/manual/source/command-ref/files/profiles.md;
    "unpack-channel.nix.gen.hh" = nixMake.mkStringHeader ../../src/nix/nix-channel/unpack-channel.nix;
    "buildenv.nix.gen.hh" = nixMake.mkStringHeader ../../src/nix/nix-env/buildenv.nix;
  };

  configHeaders = {
    "cli-config-private.hh" = {
      NIX_CLI_VERSION = lib.fileContents ../../.version;
      # Fallbacks only: Nix normally locates itself via /proc/self/exe.
      NIX_BIN_DIR = "/nix/var/nix/profiles/default/bin";
      NIX_MAN_DIR = "/nix/var/nix/profiles/default/share/man";
      HAVE_MIMALLOC = 1;
      # Crash reporting and tracing are disabled for now.
      HAVE_SENTRY = 0;
      HAVE_OTEL = 0;
    };
  };

  linkFlags = [ "-Wl,--wrap=__assert_fail" ];

  binSymlinks = [
    "nix-build"
    "nix-channel"
    "nix-collect-garbage"
    "nix-copy-closure"
    "nix-daemon"
    "nix-env"
    "nix-hash"
    "nix-instantiate"
    "nix-prefetch-url"
    "nix-shell"
    "nix-store"
  ];

  postInstall = ''
    mkdir -p $out/libexec/nix
    ln -s ../../bin/nix $out/libexec/nix/build-remote
  '';
}
