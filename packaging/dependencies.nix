# These overrides are applied to the dependencies of the Nix components.

{
  # Flake inputs; used for sources
  inputs,

  # The raw Nixpkgs, not affected by this scope
  pkgs,

  stdenv,
}:

let
  inherit (pkgs) lib;
in
scope: {
  inherit stdenv;

  mimalloc =
    if lib.versionAtLeast pkgs.mimalloc.version "3.3.2" then
      pkgs.mimalloc
    else
      pkgs.mimalloc.overrideAttrs rec {
        version = "3.3.2";
        src = pkgs.fetchFromGitHub {
          owner = "microsoft";
          repo = "mimalloc";
          tag = "v${version}";
          hash = "sha256-GZ37qQVDe9jgMb4Coe5oKvgaLTspZDlSkS5rdy1MfUU=";
        };
      };

  boehmgc =
    (pkgs.boehmgc.override {
      enableLargeConfig = true;
      inherit stdenv;
    }).overrideAttrs
      (attrs: {
        patches = (attrs.patches or [ ]) ++ [
          ./patches/boehmgc-batch-malloc-many.patch
          ./patches/boehmgc-gctest-tiny-freelists-heap-growth.patch
        ];

        env = (attrs.env or { }) // {
          # Increase the initial mark stack size to avoid stack
          # overflows, since these inhibit parallel marking (see
          # GC_mark_some()). To check whether the mark stack is too
          # small, run Nix with GC_PRINT_STATS=1 and look for messages
          # such as `Mark stack overflow`, `No room to copy back mark
          # stack`, and `Grew mark stack to ... frames`.
          NIX_CFLAGS_COMPILE = toString (
            [
              "-DINITIAL_MARK_STACK_SIZE=1048576"
              "-DGC_MANY_BLOCKS_DEFAULT=64"
              # Serve allocations up to 1520 bytes (95 granules) from
              # the per-thread freelists instead of taking the global
              # allocation lock. The default (25, i.e. <= 384 bytes) is
              # too small for parallel evaluation: e.g. a typical
              # derivation attrset (~46 attrs) is a 752-byte Bindings,
              # of which nixpkgs evaluation does hundreds of thousands,
              # all serialized on GC_allocate_ml.
              "-DGC_TINY_FREELISTS=96"
            ]
            # For some reason that is not clear, it is wanting to use libgcc_eh which is not available.
            # Force this to be built with compiler-rt & libunwind over libgcc_eh works.
            # Issue: https://github.com/NixOS/nixpkgs/issues/177129
            ++
              lib.optionals
                (
                  stdenv.cc.isClang
                  && stdenv.hostPlatform.isStatic
                  && stdenv.cc.libcxx != null
                  && stdenv.cc.libcxx.isLLVM
                )
                [
                  "-rtlib=compiler-rt"
                  "-unwindlib=libunwind"
                ]
          );
        };

        buildInputs =
          (attrs.buildInputs or [ ])
          ++ lib.optional (
            stdenv.cc.isClang
            && stdenv.hostPlatform.isStatic
            && stdenv.cc.libcxx != null
            && stdenv.cc.libcxx.isLLVM
          ) pkgs.llvmPackages.libunwind;
      });

  lowdown =
    if lib.versionAtLeast pkgs.lowdown.version "2.0.2" then
      pkgs.lowdown
    else
      pkgs.lowdown.overrideAttrs (prevAttrs: rec {
        version = "2.0.2";
        src = pkgs.fetchurl {
          url = "https://kristaps.bsd.lv/lowdown/snapshots/lowdown-${version}.tar.gz";
          hash = "sha512-cfzhuF4EnGmLJf5EGSIbWqJItY3npbRSALm+GarZ7SMU7Hr1xw0gtBFMpOdi5PBar4TgtvbnG4oRPh+COINGlA==";
        };
        nativeBuildInputs = prevAttrs.nativeBuildInputs ++ [ pkgs.buildPackages.bmake ];
        postInstall =
          lib.replaceStrings [ "lowdown.so.1" "lowdown.1.dylib" ] [ "lowdown.so.2" "lowdown.2.dylib" ]
            (prevAttrs.postInstall or "");
      });

  curl = pkgs.curl.override {
    http3Support = !pkgs.stdenv.hostPlatform.isWindows;
    # Make sure we enable all the dependencies for Content-Encoding/Transfer-Encoding decompression.
    zstdSupport = true;
    brotliSupport = true;
    zlibSupport = true;
    # libpsl uses a data file needed at runtime, not useful for nix.
    pslSupport = !stdenv.hostPlatform.isStatic;
    idnSupport = !stdenv.hostPlatform.isStatic;
  };

  libblake3 =
    (pkgs.libblake3.override {
      inherit stdenv;
      # Nixpkgs disables tbb on static
      useTBB =
        !(
          stdenv.hostPlatform.isWindows
          || stdenv.hostPlatform.isStatic
          # Some tbb tests fail with libc++.
          || (stdenv.cc.libcxx != null && stdenv.cc.libcxx.isLLVM)
        );
    })
    # For some reason that is not clear, it is wanting to use libgcc_eh which is not available.
    # Force this to be built with compiler-rt & libunwind over libgcc_eh works.
    # Issue: https://github.com/NixOS/nixpkgs/issues/177129
    .overrideAttrs
      (
        attrs:
        lib.optionalAttrs
          (
            stdenv.cc.isClang
            && stdenv.hostPlatform.isStatic
            && stdenv.cc.libcxx != null
            && stdenv.cc.libcxx.isLLVM
          )
          {
            NIX_CFLAGS_COMPILE = [
              "-rtlib=compiler-rt"
              "-unwindlib=libunwind"
            ];

            buildInputs = [
              pkgs.llvmPackages.libunwind
            ];
          }
      );

  sqlite =
    if !stdenv.hostPlatform.isWindows then
      pkgs.sqlite
    else
      pkgs.sqlite.overrideAttrs (prevAttrs: {
        nativeBuildInputs = lib.filter (x: !(x.pname == "tcl")) prevAttrs.nativeBuildInputs or [ ];
        configureFlags = (lib.filter (x: !(lib.hasPrefix "--with-tcl" x)) prevAttrs.configureFlags) ++ [
          "--disable-tcl"
        ];
      });

  libgit2 =
    (
      if lib.versionAtLeast pkgs.libgit2.version "1.9.4" then
        pkgs.libgit2
      else
        # Grab newer libgit2.
        pkgs.libgit2.overrideAttrs rec {
          version = "1.9.4";
          src = pkgs.fetchFromGitHub {
            owner = "libgit2";
            repo = "libgit2";
            tag = "v${version}";
            hash = "sha256-ZKUiz3pdFE2SKxh53X2oyr7hs32Njj5YVA0OXDXz7h0=";
          };
        }
    ).overrideAttrs
      (old: {
        separateDebugInfo = true;

        patches = old.patches or [ ] ++ [
          # Fix a use-after-free crash when `git_thread_create` fails during
          # pack building (e.g. with EAGAIN under thread pressure), leaving
          # orphaned delta-search worker threads running while the
          # packbuilder is freed.
          ./patches/libgit2-packbuilder-dont-fail-on-thread-create-error.patch
        ];
      });

  # TODO Hack until https://github.com/NixOS/nixpkgs/issues/45462 is fixed.
  boost =
    (pkgs.boost.override {
      extraB2Args = [
        "--with-container"
        "--with-context"
        "--with-coroutine"
        "--with-iostreams"
        "--with-url"
        "--with-thread"
      ];
      patches = [
        ./patches/0001-Fix-uncaught_exceptions-not-accounting-for-forced_un.patch
        ./patches/0002-Fix-cross-thread-corruption-of-the-exception-state-i.patch
      ];
      enableIcu = false;
      inherit stdenv;
    }).overrideAttrs
      (old: {
        # Need to remove `--with-*` to use `--with-libraries=...`
        buildPhase = lib.replaceStrings [ "--without-python" ] [ "" ] old.buildPhase;
        installPhase = lib.replaceStrings [ "--without-python" ] [ "" ] old.installPhase;
      });

  wasmtime = pkgs.callPackage ./wasmtime.nix { };

  sentry-native = (pkgs.callPackage ./sentry-native.nix { }).override {
    # Avoid having two curls in our closure.
    inherit (scope) curl;
  };

  libmicrohttpd = pkgs.libmicrohttpd.overrideDerivation (old: {
    # Don't pull in gnutls since it's pretty big and we don't need it.
    configureFlags = old.configureFlags or [ ] ++ [ "--without-gnutls" ];

    # Required for configuration detection for getsockname (for automatic port allocation for `nix serve`)
    __darwinAllowLocalNetworking = true;
  });
}
