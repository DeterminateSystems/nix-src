# The `nix` executable, transcribed from meson.build.
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
  # The overridden dependencies used by the Meson build, layered over Nixpkgs.
  nixDeps = pkgs // pkgs.nixDependencies2;
in

nixMake.mkComponent {
  name = "determinate-nix";
  type = "executable";
  exeName = "nix";

  deps = [
    nix-util
    nix-store
    nix-fetchers
    nix-expr
    nix-flake
    nix-main
    nix-cmd
  ];

  root = ./.;

  # Every .cc file under the root (including the legacy commands in
  # subdirectories and unix/) is a compilation unit. The `doc`, `misc`
  # and `scripts` symlinks are ignored by the scanner.
  includeDirs = [ "" ];

  files = nixMake.commonSupportFiles // {
    "generate-manpage.nix.gen.hh" = nixMake.mkStringHeader ../../doc/manual/generate-manpage.nix;
    "generate-settings.nix.gen.hh" = nixMake.mkStringHeader ../../doc/manual/generate-settings.nix;
    "generate-store-info.nix.gen.hh" = nixMake.mkStringHeader ../../doc/manual/generate-store-info.nix;
    "utils.nix.gen.hh" = nixMake.mkStringHeader ../../doc/manual/utils.nix;
    "get-env.sh.gen.hh" = nixMake.mkStringHeader ./get-env.sh;
    "help-stores.md.gen.hh" = nixMake.mkStringHeader ../../doc/manual/source/store/types/index.md.in;
    "profiles.md.gen.hh" = nixMake.mkStringHeader ../../doc/manual/source/command-ref/files/profiles.md;
    "unpack-channel.nix.gen.hh" = nixMake.mkStringHeader ./nix-channel/unpack-channel.nix;
    "buildenv.nix.gen.hh" = nixMake.mkStringHeader ./nix-env/buildenv.nix;
    "baked-flake.nix.gen.hh" = nixMake.mkStringHeader ./baked-flake.nix;
  };

  configHeaders = {
    "cli-config-private.hh" = {
      NIX_CLI_VERSION = lib.fileContents ../../.version;
      # Fallbacks only: Nix normally locates itself via /proc/self/exe.
      NIX_BIN_DIR = "/nix/var/nix/profiles/default/bin";
      NIX_MAN_DIR = "/nix/var/nix/profiles/default/share/man";
      HAVE_MIMALLOC = 1;
      HAVE_SENTRY = nixMake.config.sentry;
      HAVE_OTEL = nixMake.config.otel;
    }
    // lib.optionalAttrs nixMake.config.sentry {
      CRASHPAD_HANDLER_PATH = "${nixDeps.sentry-native}/bin/crashpad_handler";
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

  # A nushell snippet.
  postInstall = ''
    mkdir $"($env.out)/libexec/nix"
    ln -s ../../bin/nix $"($env.out)/libexec/nix/build-remote"
  '';
}
