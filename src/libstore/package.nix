{
  lib,
  stdenv,
  mkMesonLibrary,

  unixtools,
  apple-sdk,
  freebsd,

  nix-util,
  boost,
  curl,
  aws-c-common,
  aws-crt-cpp,
  libseccomp,
  nlohmann_json,
  sqlite,
  cmake, # for resolving aws-crt-cpp dep
  wasmtime,

  busybox-sandbox-shell ? null,
  pkgsStatic,

  # Configuration Options

  version,

  embeddedSandboxShell ? stdenv.hostPlatform.isStatic && !stdenv.hostPlatform.isDarwin,

  withSandboxShell ? stdenv.hostPlatform.isLinux || stdenv.hostPlatform.isFreeBSD,
  sandboxShell ?
    if stdenv.hostPlatform.isLinux then
      "${busybox-sandbox-shell}/bin/busybox"
    else if stdenv.hostPlatform.isFreeBSD then
      "${pkgsStatic.bash}/bin/bash"
    else
      null,

  withAWS ?
    # Default is this way because there have been issues building this dependency
    (lib.meta.availableOn stdenv.hostPlatform aws-c-common) && !stdenv.hostPlatform.isStatic,

  enableWasm ? !stdenv.hostPlatform.isStatic,
}:

let
  inherit (lib) fileset;
in

mkMesonLibrary (finalAttrs: {
  pname = "determinate-nix-store";
  inherit version;

  workDir = ./.;
  fileset = fileset.unions [
    ../../nix-meson-build-support
    ./nix-meson-build-support
    # FIXME: get rid of these symlinks.
    ../../.version
    ./.version
    ../../.version-determinate
    ./meson.build
    ./meson.options
    ./include/nix/store/meson.build
    ./linux/meson.build
    ./linux/include/nix/store/meson.build
    ./unix/meson.build
    ./unix/include/nix/store/meson.build
    ./windows/meson.build
    (fileset.fileFilter (file: file.hasExt "cc") ./.)
    (fileset.fileFilter (file: file.hasExt "hh") ./.)
    (fileset.fileFilter (file: file.hasExt "sb") ./.)
    (fileset.fileFilter (file: file.hasExt "md") ./.)
    (fileset.fileFilter (file: file.hasExt "sql") ./.)
  ];

  nativeBuildInputs =
    lib.optional withAWS cmake ++ lib.optional embeddedSandboxShell unixtools.hexdump;

  buildInputs = [
    boost
    curl
    sqlite
  ]
  ++ lib.optional stdenv.hostPlatform.isLinux libseccomp
  ++ lib.optional stdenv.hostPlatform.isFreeBSD freebsd.libjail
  ++ lib.optional withAWS aws-crt-cpp
  ++ lib.optional enableWasm wasmtime;

  propagatedBuildInputs = [
    nix-util
    nlohmann_json
  ];

  mesonFlags = [
    (lib.mesonEnable "seccomp-sandboxing" stdenv.hostPlatform.isLinux)
    (lib.mesonBool "embedded-sandbox-shell" embeddedSandboxShell)
    (lib.mesonEnable "s3-aws-auth" withAWS)
    (lib.mesonEnable "wasm" enableWasm)
  ]
  ++ lib.optionals withSandboxShell [
    (lib.mesonOption "sandbox-shell" sandboxShell)
  ];

  meta = {
    platforms = lib.platforms.unix ++ lib.platforms.windows;
  };

})
