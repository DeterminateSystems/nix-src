{
  lib,
  stdenv,
  mkMesonLibrary,

  aws-c-common,
  aws-crt-cpp,
  boost,
  brotli,
  cmake, # for resolving aws-crt-cpp dep
  curl,
  libarchive,
  libblake3,
  libcpuid,
  libsodium,
  nlohmann_json,
  openssl,
  zstd,

  # Configuration Options

  version,

  withAWS ?
    # Default is this way because there have been issues building this dependency
    (lib.meta.availableOn stdenv.hostPlatform aws-c-common) && !stdenv.hostPlatform.isStatic,
}:

let
  inherit (lib) fileset;
in

mkMesonLibrary (finalAttrs: {
  pname = "determinate-nix-util";
  inherit version;

  workDir = ./.;
  fileset = fileset.unions [
    ../../nix-meson-build-support
    ./nix-meson-build-support
    ../../.version
    ./.version
    ../../.version-determinate
    ./widecharwidth
    ./meson.build
    ./meson.options
    ./include/nix/util/meson.build
    ./linux/meson.build
    ./linux/include/nix/util/meson.build
    ./freebsd/meson.build
    ./freebsd/include/nix/util/meson.build
    ./unix/meson.build
    ./unix/include/nix/util/meson.build
    ./windows/meson.build
    ./windows/include/nix/util/meson.build
    (fileset.fileFilter (file: file.hasExt "cc") ./.)
    (fileset.fileFilter (file: file.hasExt "hh") ./.)
  ];

  nativeBuildInputs = lib.optional withAWS cmake;

  buildInputs = [
    brotli
    curl
    libblake3
    libsodium
    openssl
    zstd
  ]
  ++ lib.optional stdenv.hostPlatform.isx86_64 libcpuid
  ++ lib.optional withAWS aws-crt-cpp;

  propagatedBuildInputs = [
    boost
    libarchive
    nlohmann_json
  ];

  mesonFlags = [
    (lib.mesonEnable "cpuid" stdenv.hostPlatform.isx86_64)
    (lib.mesonEnable "s3-aws-auth" withAWS)
  ];

  meta = {
    platforms = lib.platforms.unix ++ lib.platforms.windows;
  };

})
