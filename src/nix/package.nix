{
  stdenv,
  lib,
  mkMesonExecutable,
  llvmPackages,

  nix-store,
  nix-expr,
  nix-main,
  nix-cmd,
  sentry-native,

  libmicrohttpd,

  nix-expr-c,
  nix-fetchers-c,
  nix-flake-c,
  nix-main-c,
  nix-store-c,
  nix-util-c,

  mimalloc,

  # Configuration Options

  version,

  # Whether to link against mimalloc for malloc override.
  # Significantly improves evaluation performance on allocation-heavy
  # workloads (~10-15% on large evaluations).
  # mimalloc is disabled on FreeBSD due to a crash in nixpkgs 25.11.
  # Once the nixpkgs flake is updated, mimalloc can be enabled again.
  withMimalloc ? !stdenv.hostPlatform.isWindows && !stdenv.hostPlatform.isFreeBSD,

  # Whether to embed the public C API into the `nix` executable so plugins can
  # resolve those symbols without linking Nix libraries directly.
  withPluginCApi ? !stdenv.hostPlatform.isWindows && !stdenv.hostPlatform.isStatic,
}:

let
  inherit (lib) fileset;
  enableSentry = !stdenv.hostPlatform.isStatic;
in

mkMesonExecutable (finalAttrs: {
  pname = "determinate-nix";
  inherit version;

  workDir = ./.;
  fileset = fileset.unions (
    [
      ../../nix-meson-build-support
      ./nix-meson-build-support
      ../../.version
      ./.version
      ./meson.build
      ./meson.options

      # Symbolic links to other dirs
      ## exes
      ./doc
      ## dirs
      ./scripts
      ../../scripts
      ./misc
      ../../misc

      # Doc nix files for --help
      ../../doc/manual/generate-manpage.nix
      ../../doc/manual/utils.nix
      ../../doc/manual/generate-settings.nix
      ../../doc/manual/generate-store-info.nix

      # Other files to be included as string literals
      ./nix-channel/unpack-channel.nix
      ./nix-env/buildenv.nix
      ./get-env.sh
      ./help-stores.md
      ../../doc/manual/source/store/types/index.md.in
      ./profiles.md
      ../../doc/manual/source/command-ref/files/profiles.md

      # Files
    ]
    ++ [
      (fileset.fileFilter (file: file.hasExt "cc") ./.)
      (fileset.fileFilter (file: file.hasExt "hh") ./.)
      (fileset.fileFilter (file: file.hasExt "md") ./.)
    ]
  );

  buildInputs = [
    nix-store
    nix-expr
    nix-main
    nix-cmd
    libmicrohttpd
  ]
  ++ lib.optionals withPluginCApi [
    nix-expr-c
    nix-fetchers-c
    nix-flake-c
    nix-main-c
    nix-store-c
    nix-util-c
  ]
  ++ lib.optional withMimalloc mimalloc
  ++ lib.optional (
    stdenv.cc.isClang
    && stdenv.hostPlatform.isStatic
    && stdenv.cc.libcxx != null
    && stdenv.cc.libcxx.isLLVM
  ) llvmPackages.libunwind
  ++ lib.optional enableSentry sentry-native;

  mesonFlags = [
    (lib.mesonEnable "mimalloc" withMimalloc)
    (lib.mesonBool "plugin-c-api" withPluginCApi)
    (lib.mesonEnable "sentry" enableSentry)
  ]
  ++ lib.optional enableSentry (
    lib.mesonOption "crashpad-handler" "${sentry-native}/bin/crashpad_handler"
  );

  postInstall = lib.optionalString stdenv.hostPlatform.isStatic ''
    mkdir -p $out/nix-support
    echo "file binary-dist $out/bin/nix" >> $out/nix-support/hydra-build-products
  '';

  # Fixes a problem with the "nix-cli-libcxxStdenv-static" package output.
  # For some reason that is not clear, it is wanting to use libgcc_eh which is not available.
  # Force this to be built with compiler-rt & libunwind over libgcc_eh works.
  # Issue: https://github.com/NixOS/nixpkgs/issues/177129
  NIX_CFLAGS_COMPILE = lib.optionalString (
    stdenv.cc.isClang
    && stdenv.hostPlatform.isStatic
    && stdenv.cc.libcxx != null
    && stdenv.cc.libcxx.isLLVM
  ) "-rtlib=compiler-rt -unwindlib=libunwind";

  passthru = {
    exportsPluginCApi = withPluginCApi;
  };

  meta = {
    mainProgram = "nix";
    platforms = lib.platforms.unix ++ lib.platforms.windows;
  };

})
