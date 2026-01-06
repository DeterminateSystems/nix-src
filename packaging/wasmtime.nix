{
  lib,
  stdenv,
  rust_1_89,
  fetchFromGitHub,
  buildPackages,
  cmake,
  installShellFiles,
  nix-update-script,
  enableShared ? !stdenv.hostPlatform.isStatic,
  enableStatic ? stdenv.hostPlatform.isStatic,
}:
rust_1_89.packages.stable.rustPlatform.buildRustPackage (finalAttrs: {
  pname = "wasmtime";
  version = "40.0.0";

  src = fetchFromGitHub {
    owner = "bytecodealliance";
    repo = "wasmtime";
    tag = "v${finalAttrs.version}";
    hash = "sha256-d5j+QSEWIVwVRMT/QGc6x3cVBTFZBpoxiBagmpLV1e8=";
    fetchSubmodules = true;
  };

  # Disable cargo-auditable until https://github.com/rust-secure-code/cargo-auditable/issues/124 is solved.
  auditable = false;

  cargoHash = "sha256-PIUJHkeGi8gao7n+SLzcxNYTl2KxKiwJZPW+sFYf0AY=";
  cargoBuildFlags = [
    "--package"
    "wasmtime-c-api"
    "--no-default-features"
    "--features cranelift,wasi,pooling-allocator,wat,demangle,gc-null"
  ];

  outputs = [
    "out"
    "lib"
  ];

  nativeBuildInputs = [
    cmake
    installShellFiles
  ];

  doCheck =
    with stdenv.buildPlatform;
    # SIMD tests are only executed on platforms that support all
    # required processor features (e.g. SSE3, SSSE3 and SSE4.1 on x86_64):
    # https://github.com/bytecodealliance/wasmtime/blob/v9.0.0/cranelift/codegen/src/isa/x64/mod.rs#L220
    (isx86_64 -> sse3Support && ssse3Support && sse4_1Support)
    &&
      # The dependency `wasi-preview1-component-adapter` fails to build because of:
      # error: linker `rust-lld` not found
      !isAarch64;

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

  passthru = {
    updateScript = nix-update-script {
      extraArgs = [
        "--version-regex"
        "^v(\\d+\\.\\d+\\.\\d+)$"
      ];
    };
  };

  meta = {
    description = "Standalone JIT-style runtime for WebAssembly, using Cranelift";
    homepage = "https://wasmtime.dev/";
    license = [
      lib.licenses.asl20
      lib.licenses.llvm-exception
    ];
    mainProgram = "wasmtime";
    maintainers = with lib.maintainers; [
      ereslibre
      nekowinston
    ];
    platforms = lib.platforms.unix;
    changelog = "https://github.com/bytecodealliance/wasmtime/blob/v${finalAttrs.version}/RELEASES.md";
  };
})
