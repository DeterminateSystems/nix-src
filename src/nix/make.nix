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
  # and `scripts` symlinks are ignored by the scanner, so the files that
  # are `#embed`ded through them are listed in `files`.
  includeDirs = [ "" ];

  files = nixMake.commonSupportFiles // {
    "doc/manual/generate-manpage.nix" = ../../doc/manual/generate-manpage.nix;
    "doc/manual/generate-settings.nix" = ../../doc/manual/generate-settings.nix;
    "doc/manual/generate-store-info.nix" = ../../doc/manual/generate-store-info.nix;
    "doc/manual/utils.nix" = ../../doc/manual/utils.nix;
    "help-stores.md" = ../../doc/manual/source/store/types/index.md.in;
    "profiles.md.gen.hh" = nixMake.mkStringHeader ../../doc/manual/source/command-ref/files/profiles.md;
    "unpack-channel.nix.gen.hh" = nixMake.mkStringHeader ./nix-channel/unpack-channel.nix;
    "buildenv.nix.gen.hh" = nixMake.mkStringHeader ./nix-env/buildenv.nix;
  };

  configHeaders = {
    "cli-config-private.hh" = {
      NIX_CLI_VERSION = lib.fileContents ../../.version;
      # Fallbacks only: Nix normally locates itself via /proc/self/exe.
      NIX_BIN_DIR = "/nix/var/nix/profiles/default/bin";
      NIX_MAN_DIR = "/nix/var/nix/profiles/default/share/man";
      # The default of the Meson option `bash-program`.
      FALLBACK_BASH = "bash";
      HAVE_MIMALLOC = 1;
      HAVE_SENTRY = nixMake.config.sentry;
      HAVE_OTEL = nixMake.config.otel;
    }
    // lib.optionalAttrs nixMake.config.sentry {
      CRASHPAD_HANDLER_PATH = "${nixDeps.sentry-native}/bin/crashpad_handler";
    };
  };

  linkFlags = nixMake.wrapAssertFailFlags;

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

  # A Python snippet, with `out` bound.
  postInstall = ''
    os.makedirs(f"{out}/libexec/nix")
    os.symlink("../../bin/nix", f"{out}/libexec/nix/build-remote")
  '';
}
