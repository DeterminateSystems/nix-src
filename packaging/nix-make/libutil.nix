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
  ];

  # Every .cc file under the root is a compilation unit, except these.
  excludeSources = [
    "windows"
    "freebsd"
  ];

  includeDirs = [
    ""
    "include"
    "unix"
    "unix/include"
    "linux/include"
    "widecharwidth"
  ];

  files = nixMake.commonSupportFiles // {
    # Interposes __cxa_throw; linked into libnixutil only.
    "nix-meson-build-support/common/cxa-throw/interpose-cxa-throw.cc" =
      ../../nix-meson-build-support/common/cxa-throw/interpose-cxa-throw.cc;
    "nix-meson-build-support/common/cxa-throw/is-logic-error.hh" =
      ../../nix-meson-build-support/common/cxa-throw/is-logic-error.hh;

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
