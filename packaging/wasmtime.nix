# Stripped-down version of https://github.com/NixOS/nixpkgs/blob/master/pkgs/by-name/wa/wasmtime/package.nix,
# license: https://github.com/NixOS/nixpkgs/blob/master/COPYING
{
  lib,
  stdenv,
  rust,
  fetchFromGitHub,
  cmake,
  enableShared ? !stdenv.hostPlatform.isStatic,
  enableStatic ? stdenv.hostPlatform.isStatic,
}:
rust.packages.stable.rustPlatform.buildRustPackage (finalAttrs: {
  pname = "wasmtime";
  version = "48.0.7";

  src = fetchFromGitHub {
    owner = "bytecodealliance";
    repo = "wasmtime";
    tag = "v${finalAttrs.version}";
    hash = "sha256-LBosQpECAGZLQPc7o/vsmmHUJQHs+SgsVM0N32rDlBI=";
    fetchSubmodules = true;
  };

  # Disable cargo-auditable until https://github.com/rust-secure-code/cargo-auditable/issues/124 is solved.
  auditable = false;

  cargoHash = "sha256-oYGT8mGpCL9bss3CFhZXEe+I17aASXg4OxzBKl53QJM=";
  cargoBuildFlags = [
    "--package"
    "wasmtime-c-api"
    "--no-default-features"
    "--features cache,cranelift,wasi,pooling-allocator,wat,demangle,gc,gc-null"
  ];

  outputs = [
    "out"
    "lib"
  ];

  nativeBuildInputs = [
    cmake
  ];

  doCheck = false;

  postInstall =
    let
      inherit (stdenv.hostPlatform.rust) cargoShortTarget;
    in
    ''
      moveToOutput lib $lib
      ${lib.optionalString (!enableShared) "rm -f $lib/lib/*.so{,.*}"}
      ${lib.optionalString (!enableStatic) "rm -f $lib/lib/*.a"}

      # copy the build.rs generated c-api headers
      # https://github.com/rust-lang/cargo/issues/9661
      mkdir -p $out
      cp -r target/${cargoShortTarget}/release/build/wasmtime-c-api-impl-*/out/include $out/include
    ''
    + lib.optionalString stdenv.hostPlatform.isDarwin ''
      install_name_tool -id \
        $lib/lib/libwasmtime.dylib \
        $lib/lib/libwasmtime.dylib
    '';
})
