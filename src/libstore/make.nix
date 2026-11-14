# libnixstore, transcribed from meson.build and its subdirectories.
{
  pkgs,
  nixMake,
  nix-util,
}:

let
  inherit (pkgs) lib;
  inherit (nixMake.hostPlatform) isLinux isDarwin;
  # Meson's checks find these on Linux but not on Darwin.
  onLinux = if isLinux then 1 else 0;
in

nixMake.mkComponent {
  name = "determinate-nix-store";
  libName = "nixstore";

  deps = [ nix-util ];

  root = ./.;

  # Meson builds linux/ on Linux and darwin/ on Darwin, and the chroot
  # builder only on Linux (and FreeBSD).
  excludeSources = [
    "windows"
    "freebsd"
  ]
  ++ lib.optional (!isDarwin) "darwin"
  ++ lib.optionals (!isLinux) [
    "linux"
    "unix/build/chroot-derivation-builder.cc"
  ]
  ++ lib.optional (!nixMake.config.awsAuth) "aws-creds.cc";

  includeDirs = [
    ""
    "include"
  ]
  ++ lib.optionals isLinux [
    "linux/build"
    "linux/include"
  ]
  ++ lib.optional isDarwin "darwin/build"
  ++ [
    "unix/build"
    "unix/include"
  ];

  files = nixMake.commonSupportFiles // {
    "schema.sql.gen.hh" = nixMake.mkStringHeader ./schema.sql;
    "ca-specific-schema.sql.gen.hh" = nixMake.mkStringHeader ./ca-specific-schema.sql;
  };

  configHeaders = {
    "include/nix/store/config.hh" = {
      NIX_LOCAL_SYSTEM = nixMake.hostPlatform.system;
      # macOS has <sys/xattr.h>, but not llistxattr.
      NIX_SUPPORT_ACL = onLinux;
      NIX_WITH_AWS_AUTH = nixMake.config.awsAuth;
    };
    "store-config-private.hh" = {
      # Hard links to symlinks fail on macOS.
      CAN_LINK_SYMLINK = onLinux;
      DETERMINATE_NIX_VERSION = lib.fileContents ../../.version-determinate;
      HAVE_EMBEDDED_SANDBOX_SHELL = 0;
      # Only checked for (and so only defined) on Linux.
      HAVE_LANDLOCK = if isLinux then 1 else null;
      HAVE_MOVE_MOUNT = onLinux;
      HAVE_OPEN_TREE = onLinux;
      HAVE_POSIX_FALLOCATE = onLinux;
      # package.nix only enables seccomp on Linux.
      HAVE_SECCOMP = onLinux;
      HAVE_STATVFS = 1;
      IS_STATIC = null;
      LSOF = "lsof";
      NIX_CONF_DIR = "/etc/nix";
      NIX_LOG_DIR = "/nix/var/log/nix";
      NIX_STATE_DIR = "/nix/var/nix";
      NIX_STORE_DIR = "/nix/store";
      NIX_USE_WASMTIME = nixMake.config.wasm;
      PACKAGE_VERSION = lib.fileContents ../../.version;
      # package.nix only has a sandbox shell on Linux.
      SANDBOX_SHELL = if isLinux then "${pkgs.busybox-sandbox-shell}/bin/busybox" else null;
    };
  };

  # The Darwin sandbox (`sandbox_init_with_parameters`), as in meson.build.
  extraLinkLibs = lib.optional isDarwin "-lsandbox";

  extraCxxFlags = nixMake.weakVtablesFlags;

  linkFlags = nixMake.wrapAssertFailFlags;
}
