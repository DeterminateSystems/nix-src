# libnixutil, transcribed from meson.build and its subdirectories.
{ nixMake }:

nixMake.mkComponent {
  name = "determinate-nix-util";
  libName = "nixutil";

  root = ./.;

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

  };

  configHeaders = {
    "include/nix/util/config.hh" = {
      NIX_UBSAN_ENABLED = 0;
      NIX_ASAN_ENABLED = 0;
    };
    "util-config-private.hh" = {
      HAVE_LIBCPUID = 1;
      HAVE_POSIX_FALLOCATE = 1;
    };
    "unix/util-unix-config-private.hh" = {
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

  extraCxxFlags = nixMake.weakVtablesFlags;

  linkFlags = [ "-Wl,--wrap=__assert_fail" ];
}
