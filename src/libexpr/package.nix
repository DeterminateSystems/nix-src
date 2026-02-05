{
  lib,
  stdenv,
  mkMesonLibrary,

  bison,
  flex,
  cmake, # for resolving toml11 dep

  nix-util,
  nix-store,
  nix-fetchers,
  boost,
  boehmgc,
  nlohmann_json,
  toml11,
  wasmtime,

  # Configuration Options

  version,

  # Whether to use garbage collection for the Nix language evaluator.
  #
  # If it is disabled, we just leak memory, but this is not as bad as it
  # sounds so long as evaluation just takes places within short-lived
  # processes. (When the process exits, the memory is reclaimed; it is
  # only leaked *within* the process.)
  #
  # Temporarily disabled on Windows because the `GC_throw_bad_alloc`
  # symbol is missing during linking.
  enableGC ? !stdenv.hostPlatform.isWindows,

  # Whether to use wasmtime for wasm integration in the Nix language evaluator
  #
  # Temporarily disabled when static linking due to Rust not compiling
  enableWasm ? !stdenv.hostPlatform.isStatic,
}:

let
  inherit (lib) fileset;
in

mkMesonLibrary (finalAttrs: {
  pname = "determinate-nix-expr";
  inherit version;

  workDir = ./.;
  fileset = fileset.unions [
    ../../nix-meson-build-support
    ./nix-meson-build-support
    ../../.version
    ./.version
    ./meson.build
    ./meson.options
    ./primops/meson.build
    ./include/nix/expr/meson.build
    (fileset.fileFilter (file: file.hasExt "cc") ./.)
    (fileset.fileFilter (file: file.hasExt "hh") ./.)
    ./lexer.l
    ./parser.y
    (fileset.difference (fileset.fileFilter (file: file.hasExt "nix") ./.) ./package.nix)
  ];

  nativeBuildInputs = [
    bison
    flex
    cmake
  ];

  buildInputs = [
    toml11
  ]
  ++ lib.optional enableWasm wasmtime;

  propagatedBuildInputs = [
    nix-util
    nix-store
    nix-fetchers
    boost
    nlohmann_json
  ]
  ++ lib.optional enableGC boehmgc;

  mesonFlags = [
    (lib.mesonEnable "gc" enableGC)
    (lib.mesonEnable "wasm" enableWasm)
  ];

  # Fixes a problem with the "nix-expr-libcxxStdenv-static" package output.
  # For some reason that is not clear, it is wanting to use libgcc_eh which is not available.
  # Force this to be built with compiler-rt over libgcc_eh works.
  # Issue: https://github.com/NixOS/nixpkgs/issues/177129
  NIX_CFLAGS_COMPILE = lib.optional (
    stdenv.cc.isClang
    && stdenv.hostPlatform.isStatic
    && stdenv.cc.libcxx != null
    && stdenv.cc.libcxx.isLLVM
  ) "-rtlib=compiler-rt";

  meta = {
    platforms = lib.platforms.unix ++ lib.platforms.windows;
  };

})
