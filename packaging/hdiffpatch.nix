# HDiffPatch (https://github.com/sisong/HDiffPatch), used for
# experimental `nix nario export --diff-algorithm hdiffpatch`.
#
# Upstream doesn't install a library, so we install the static library
# that its Makefile builds, its headers, and a pkg-config file.
{
  lib,
  stdenv,
  fetchFromGitHub,
  zstd,
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

  buildInputs = [ zstd ];

  # Disable everything that requires source checkouts of other
  # projects next to the HDiffPatch source tree, and link against zstd
  # from Nixpkgs. Disable multi-threading, since Nix already computes
  # diffs in parallel.
  makeFlags = [
    "ZSTD=2"
    "LDEF=0"
    "ZLIB=0"
    "LZMA=0"
    "MD5=0"
    "XXH=0"
    "VCD=0"
    "BSD=0"
    "BZIP2=0"
    "DIR_DIFF=0"
    "MT=0"
  ];

  # The static library gets linked into a shared library. Disable
  # progress messages on stdout, which would end up in the output of
  # programs like `nix nario export`.
  env.NIX_CFLAGS_COMPILE = "-fPIC -D_IS_OUT_DIFF_INFO=0";

  enableParallelBuilding = true;

  # These must match the configuration selected by `makeFlags` and
  # `NIX_CFLAGS_COMPILE`, since they affect the headers.
  cflags = lib.concatStringsSep " " [
    "-D_IS_OUT_DIFF_INFO=0"
    "-D_IS_USED_MULTITHREAD=0"
    "-DZ7_ST"
    "-D_IS_NEED_BSDIFF=0"
    "-D_IS_NEED_VCDIFF=0"
    "-D_IS_NEED_DIR_DIFF_PATCH=0"
    "-D_IS_NEED_ALL_CompressPlugin=0"
    "-D_IS_NEED_DEFAULT_CompressPlugin=0"
    "-D_IS_NEED_ALL_ChecksumPlugin=0"
    "-D_IS_NEED_DEFAULT_ChecksumPlugin=0"
    "-D_CompressPlugin_zstd"
  ];

  installPhase = ''
    runHook preInstall

    install -D -m 644 libhdiffpatch.a $out/lib/libhdiffpatch.a
    install -D -m 755 hdiffz $out/bin/hdiffz
    install -D -m 755 hpatchz $out/bin/hpatchz

    # The headers refer to each other relative to the root of the
    # source tree, so keep that structure.
    find . -name '*.h' -exec install -D -m 644 {} $out/include/hdiffpatch/{} \;

    mkdir -p $out/lib/pkgconfig
    cat > $out/lib/pkgconfig/hdiffpatch.pc <<EOF
    prefix=$out
    libdir=$out/lib
    includedir=$out/include

    Name: hdiffpatch
    Description: Binary diff and patch library
    Version: ${finalAttrs.version}
    Requires.private: libzstd
    Cflags: -I$out/include $cflags
    Libs: -L$out/lib -lhdiffpatch
    EOF

    runHook postInstall
  '';

  meta = {
    description = "Binary diff and patch library for large files and directories";
    homepage = "https://github.com/sisong/HDiffPatch";
    license = lib.licenses.mit;
  };
})
