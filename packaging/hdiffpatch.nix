# HDiffPatch (https://github.com/sisong/HDiffPatch), used for
# experimental `nix nario export --diff-algorithm hdiffpatch`.
{
  lib,
  stdenv,
  fetchFromGitHub,
  zstd,
  zlib,
}:

stdenv.mkDerivation (finalAttrs: {
  pname = "hdiffpatch";
  version = "5.1.3";

  src = fetchFromGitHub {
    owner = "sisong";
    repo = "HDiffPatch";
    tag = "v${finalAttrs.version}";
    hash = "sha256-8bDefNdKO9bLJzENY1OEap7tTSzI7C/oUECgXBJSiEE=";
  };

  buildInputs = [
    zstd
    zlib
  ];

  # Disable everything that requires source checkouts of other
  # projects next to the HDiffPatch source tree, and link against zstd
  # and zlib from Nixpkgs.
  makeFlags = [
    "ZSTD=2"
    "LDEF=0"
    "ZLIB=2"
    "LZMA=0"
    "MD5=0"
    "XXH=0"
    "VCD=0"
    "BSD=0"
    "BZIP2=0"
    "DIR_DIFF=0"
  ];

  enableParallelBuilding = true;

  installPhase = ''
    runHook preInstall
    install -D -m 755 hdiffz $out/bin/hdiffz
    install -D -m 755 hpatchz $out/bin/hpatchz
    runHook postInstall
  '';

  meta = {
    description = "Binary diff and patch tools for large files and directories";
    homepage = "https://github.com/sisong/HDiffPatch";
    license = lib.licenses.mit;
    mainProgram = "hdiffz";
  };
})
