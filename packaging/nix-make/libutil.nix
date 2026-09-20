# libnixutil, transcribed from src/libutil/meson.build and its subdirectories.
{ pkgs, nixMake }:

let
  # The overridden dependencies used by the Meson build, layered over Nixpkgs.
  deps = pkgs // pkgs.nixDependencies2;
in

nixMake.mkComponent {
  name = "nix-util";
  libName = "nixutil";

  roots = [
    {
      root = ../../src/libutil;
      prefix = "";
    }
    {
      root = ../../nix-meson-build-support;
      prefix = "nix-meson-build-support";
    }
  ];

  includeDirs = [
    ""
    "include"
    "unix"
    "unix/include"
    "linux/include"
    "widecharwidth"
  ];

  files = {
    "include/nix/util/config.hh" = nixMake.mkConfigHeader "config.hh" {
      NIX_UBSAN_ENABLED = 0;
      NIX_ASAN_ENABLED = 0;
    };
    "util-config-private.hh" = nixMake.mkConfigHeader "util-config-private.hh" {
      HAVE_LIBCPUID = 1;
      HAVE_POSIX_FALLOCATE = 1;
    };
    "unix/util-unix-config-private.hh" = nixMake.mkConfigHeader "util-unix-config-private.hh" {
      HAVE_CLOSE_RANGE = 1;
      HAVE_COPY_FILE_RANGE = 1;
      HAVE_DECL_AT_SYMLINK_NOFOLLOW = 1;
      HAVE_F_GETPATH = 0;
      HAVE_LUTIMES = 1;
      HAVE_PIPE2 = 1;
      HAVE_STRSIGNAL = 1;
      HAVE_SYSCONF = 1;
      HAVE_UTIMENSAT = 1;
    };
  };

  sources = [
    "archive.cc"
    "args.cc"
    "base-n.cc"
    "base-nix-32.cc"
    "bump-memory-resource.cc"
    "caching-source-accessor.cc"
    "canon-path.cc"
    "checked-arithmetic.cc"
    "compression-algo.cc"
    "compression-settings.cc"
    "compression.cc"
    "compute-levels.cc"
    "config-global.cc"
    "configuration.cc"
    "current-process.cc"
    "english.cc"
    "environment-variables.cc"
    "error.cc"
    "executable-path.cc"
    "exit.cc"
    "experimental-features.cc"
    "file-content-address.cc"
    "file-descriptor.cc"
    "file-system.cc"
    "forwarding-source-accessor.cc"
    "fs-sink.cc"
    "git.cc"
    "hash.cc"
    "hilite.cc"
    "json-utils.cc"
    "logging.cc"
    "memory-source-accessor.cc"
    "memory-source-accessor/json.cc"
    "mounted-source-accessor.cc"
    "nar-accessor.cc"
    "nar-cache.cc"
    "nar-listing.cc"
    "pos-table.cc"
    "position.cc"
    "posix-source-accessor.cc"
    "processes.cc"
    "provenance.cc"
    "serialise.cc"
    "signature/local-keys.cc"
    "signature/signer.cc"
    "source-accessor.cc"
    "source-path.cc"
    "strings.cc"
    "suggestions.cc"
    "table.cc"
    "tarfile.cc"
    "tee-logger.cc"
    "terminal.cc"
    "thread-pool.cc"
    "union-source-accessor.cc"
    "unix-domain-socket.cc"
    "url.cc"
    "users.cc"
    "util.cc"
    "xml-writer.cc"

    # linux/meson.build
    "linux/cgroup.cc"
    "linux/linux-namespaces.cc"

    # unix/meson.build
    "unix/current-process.cc"
    "unix/environment-variables.cc"
    "unix/file-descriptor.cc"
    "unix/file-path.cc"
    "unix/file-system-at.cc"
    "unix/file-system.cc"
    "unix/muxable-pipe.cc"
    "unix/processes.cc"
    "unix/signals.cc"
    "unix/users.cc"
    "unix/xdg-dirs.cc"

    # nix-meson-build-support/common
    "nix-meson-build-support/common/assert-fail/wrap-assert-fail.cc"
    "nix-meson-build-support/common/cxa-throw/interpose-cxa-throw.cc"
  ];

  externalDeps = [
    {
      prefix = "boost/";
      pkg = deps.boost;
    }
    {
      prefix = "nlohmann/";
      pkg = deps.nlohmann_json;
      pkgconfig = [ "nlohmann_json" ];
    }
    {
      prefix = "archive";
      pkg = deps.libarchive;
      pkgconfig = [ "libarchive" ];
    }
    {
      prefix = "openssl/";
      pkg = deps.openssl;
      pkgconfig = [ "libcrypto" ];
    }
    {
      prefix = "sodium.h";
      pkg = deps.libsodium;
      pkgconfig = [ "libsodium" ];
    }
    {
      prefix = "blake3.h";
      pkg = deps.libblake3;
      pkgconfig = [ "libblake3" ];
    }
    {
      prefix = "brotli/";
      pkg = deps.brotli;
      pkgconfig = [
        "libbrotlicommon"
        "libbrotlidec"
        "libbrotlienc"
      ];
    }
    {
      prefix = "zstd.h";
      pkg = deps.zstd;
      pkgconfig = [ "libzstd" ];
    }
    {
      prefix = "libcpuid/";
      pkg = deps.libcpuid;
      pkgconfig = [ "libcpuid" ];
    }
  ];

  linkFlags = [ "-Wl,--wrap=__assert_fail" ];

  linkPkgConfig = [
    "libarchive"
    "libblake3"
    "libcrypto"
    "libsodium"
    "libbrotlicommon"
    "libbrotlidec"
    "libbrotlienc"
    "libzstd"
    "libcpuid"
  ];

  linkLibs = [
    "-lboost_context"
    "-lboost_coroutine"
    "-lboost_iostreams"
    "-lboost_url"
    "-pthread"
    "-ldl"
  ];

  linkDeps = with deps; [
    boost
    libarchive
    libblake3
    openssl
    libsodium
    brotli
    zstd
    libcpuid
  ];
}
