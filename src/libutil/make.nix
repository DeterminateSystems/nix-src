# libnixutil, transcribed from meson.build and its subdirectories.
{ lib, nixMake }:

let
  inherit (nixMake.hostPlatform) isDarwin isLinux isx86_64;
in

nixMake.mkComponent {
  name = "determinate-nix-util";
  libName = "nixutil";

  root = ./.;

  # Every .cc file under the root is a compilation unit, except these.
  # Meson only builds linux/ on Linux.
  excludeSources = [
    "windows"
    "freebsd"
  ]
  ++ lib.optional (!isLinux) "linux"
  ++ lib.optional (!isDarwin) "darwin";

  includeDirs = [
    ""
    "include"
    "unix"
    "unix/include"
  ]
  ++ lib.optional isLinux "linux/include"
  ++ [ "widecharwidth" ];

  files =
    nixMake.commonSupportFiles
    # Interposes __cxa_throw; linked into libnixutil only, and only on
    # Linux (see nix-meson-build-support/common/cxa-throw).
    // lib.optionalAttrs isLinux {
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
    # The values Meson's checks find on each platform (on Darwin: macOS
    # has F_GETPATH, but not close_range, copy_file_range, pipe2 or
    # posix_fallocate).
    "util-config-private.hh" = {
      # package.nix only enables libcpuid on x86_64.
      HAVE_LIBCPUID = if isx86_64 then 1 else 0;
      HAVE_POSIX_FALLOCATE = if isLinux then 1 else 0;
      # The default of the Meson option `sh-program`.
      SH_PROGRAM = "/bin/sh";
    };
    "unix/util-unix-config-private.hh" = {
      # FreeBSD-only.
      HAVE_BINDAT = 0;
      HAVE_CONNECTAT = 0;
      # glibc >= 2.34 has it; macOS does not.
      HAVE_CLOSEFROM = if isLinux then 1 else 0;
      HAVE_CLOSE_RANGE = if isLinux then 1 else 0;
      HAVE_COPY_FILE_RANGE = if isLinux then 1 else 0;
      HAVE_DECL_AT_SYMLINK_NOFOLLOW = 1;
      HAVE_F_GETPATH = if isLinux then 0 else 1;
      HAVE_LUTIMES = 1;
      HAVE_PIPE2 = if isLinux then 1 else 0;
      HAVE_STRSIGNAL = 1;
      HAVE_SYSCONF = 1;
      HAVE_UTIMENSAT = 1;
    };
  };

  # bump-memory-resource.hh uses std::hardware_destructive_interference_size
  # without including <new>. libstdc++ provides it via <atomic>, and Meson's
  # precompiled header hides this on Darwin, but libc++ does not.
  unitCxxFlags = lib.optionalAttrs (!isLinux) {
    "bump-memory-resource.cc" = [
      "-include"
      "new"
    ];
  };

  extraCxxFlags = nixMake.weakVtablesFlags;

  linkFlags = nixMake.wrapAssertFailFlags;
}
