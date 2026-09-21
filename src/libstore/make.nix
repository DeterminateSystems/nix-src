# libnixstore, transcribed from meson.build and its subdirectories.
{
  pkgs,
  nixMake,
  nix-util,
}:

let
  inherit (pkgs) lib;
in

nixMake.mkComponent {
  name = "determinate-nix-store";
  libName = "nixstore";

  deps = [ nix-util ];

  root = ./.;

  excludeSources = [
    "windows"
    "freebsd"
    "darwin"
  ]
  ++ lib.optional (!nixMake.config.awsAuth) "aws-creds.cc";

  includeDirs = [
    ""
    "include"
    "linux/build"
    "linux/include"
    "unix/build"
    "unix/include"
  ];

  files = nixMake.commonSupportFiles // {
    "schema.sql.gen.hh" = nixMake.mkStringHeader ./schema.sql;
    "ca-specific-schema.sql.gen.hh" = nixMake.mkStringHeader ./ca-specific-schema.sql;
  };

  configHeaders = {
    "include/nix/store/config.hh" = {
      NIX_LOCAL_SYSTEM = "x86_64-linux";
      NIX_SUPPORT_ACL = 1;
      NIX_WITH_AWS_AUTH = nixMake.config.awsAuth;
    };
    "store-config-private.hh" = {
      CAN_LINK_SYMLINK = 1;
      DETERMINATE_NIX_VERSION = lib.fileContents ../../.version-determinate;
      HAVE_EMBEDDED_SANDBOX_SHELL = 0;
      HAVE_LANDLOCK = 1;
      HAVE_MOVE_MOUNT = 1;
      HAVE_OPEN_TREE = 1;
      HAVE_POSIX_FALLOCATE = 1;
      HAVE_SECCOMP = 1;
      HAVE_STATVFS = 1;
      IS_STATIC = null;
      LSOF = "lsof";
      NIX_CONF_DIR = "/etc/nix";
      NIX_LOG_DIR = "/nix/var/log/nix";
      NIX_STATE_DIR = "/nix/var/nix";
      NIX_STORE_DIR = "/nix/store";
      NIX_USE_WASMTIME = nixMake.config.wasm;
      PACKAGE_VERSION = lib.fileContents ../../.version;
      SANDBOX_SHELL = "${pkgs.busybox-sandbox-shell}/bin/busybox";
    };
  };

  linkFlags = [ "-Wl,--wrap=__assert_fail" ];
}
