# Generic machinery for building C++ components with one derivation per
# compilation unit.
{ pkgs }:

let
  inherit (pkgs) lib;

  getDeps = builtins.wasm {
    path = ./scanner.wasm;
    function = "getDeps";
  };

  # Non-optimized for now, for faster iteration.
  optimizationFlags = [ "-O0" ];

  # From nix-meson-build-support/common/meson.build (GCC-supported subset).
  warningFlags = [
    "-Wall"
    "-Wdeprecated-copy"
    "-Werror=suggest-override"
    "-Werror=switch"
    "-Werror=switch-enum"
    "-Werror=undef"
    "-Werror=unused-result"
    "-Werror=sign-compare"
    "-Werror=return-type"
    "-Werror=non-virtual-dtor"
    "-Wignored-qualifiers"
    "-Wimplicit-fallthrough"
    "-Wno-deprecated-declarations"
    "-Wno-interference-size"
    "-Wno-subobject-linkage"
  ];

  commonCxxFlags = [
    "-std=c++23"
    "-fPIC"
    "-pthread"
    "-D_FILE_OFFSET_BITS=64"
    "-D_GLIBCXX_USE_TBB_PAR_BACKEND=0"
    "-fno-semantic-interposition"
  ]
  ++ optimizationFlags
  ++ warningFlags;

  # Generate a header with `#define`s, like Meson's `configure_file()`.
  # Integers and booleans become bare defines, strings become quoted
  # defines, and `null` becomes an `#undef`.
  mkConfigHeader =
    name: attrs:
    builtins.toFile name (
      "#pragma once\n"
      + lib.concatStrings (
        lib.mapAttrsToList (
          k: v:
          if v == null then
            "#undef ${k}\n"
          else if builtins.isBool v then
            "#define ${k} ${if v then "1" else "0"}\n"
          else if builtins.isInt v then
            "#define ${k} ${toString v}\n"
          else
            "#define ${k} ${builtins.toJSON v}\n"
        ) attrs
      )
    );

  # Map the external (angle-bracket) includes of a unit to the entries of
  # the component's `externalDeps` table, by prefix match. Unmatched
  # includes (libc, libstdc++) are ignored.
  matchExternalDeps =
    externalDeps: externalIncludes:
    let
      matches = lib.filter (d: d != null) (
        map (inc: lib.findFirst (d: lib.hasPrefix d.prefix inc) null externalDeps) externalIncludes
      );
    in
    # Deduplicate by prefix.
    lib.attrValues (lib.listToAttrs (map (d: lib.nameValuePair d.prefix d) matches));

  compileUnit =
    component: unit:
    let
      deps = matchExternalDeps component.externalDeps unit.externalIncludes;
    in
    pkgs.runCommandCC "${baseNameOf unit.path}.o"
      {
        __structuredAttrs = true;
        inherit (unit) src includes;
        srcPath = unit.path;
        inherit (component) includeDirs;
        cxxFlags = commonCxxFlags ++ component.extraCxxFlags;
        pkgConfigDeps = lib.concatMap (d: d.pkgconfig or [ ]) deps;
        buildInputs = map (d: d.pkg) deps;
        nativeBuildInputs = [ pkgs.pkg-config ];
        # `_FORTIFY_SOURCE` warns on every unit when not optimizing.
        hardeningDisable = [ "fortify" ];
      }
      ''
        mkdir tree
        cd tree

        # Recreate the source layout: each header and the source file is a
        # separate store path.
        for name in "''${!includes[@]}"; do
          mkdir -p "$(dirname "$name")"
          ln -s "''${includes[$name]}" "$name"
        done
        mkdir -p "$(dirname "$srcPath")"
        ln -s "$src" "$srcPath"

        flags=()
        for dir in "''${includeDirs[@]}"; do
          flags+=("-I''${dir:-.}")
        done
        if [[ ''${#pkgConfigDeps[@]} -gt 0 ]]; then
          flags+=($(pkg-config --cflags "''${pkgConfigDeps[@]}"))
        fi

        $CXX "''${cxxFlags[@]}" "''${flags[@]}" -c "$srcPath" -o "$out"
      '';

  linkSharedLibrary =
    component: objects:
    pkgs.runCommandCC component.name
      {
        __structuredAttrs = true;
        inherit objects;
        inherit (component)
          libName
          linkFlags
          linkLibs
          linkPkgConfig
          ;
        buildInputs = component.linkDeps;
        nativeBuildInputs = [ pkgs.pkg-config ];
        passthru = {
          inherit objects;
          inherit (component) units;
        };
      }
      ''
        mkdir -p $out/lib

        libs=()
        if [[ ''${#linkPkgConfig[@]} -gt 0 ]]; then
          libs+=($(pkg-config --libs "''${linkPkgConfig[@]}"))
        fi

        $CXX -shared -fPIC \
          -Wl,-soname,lib$libName.so \
          -Wl,--as-needed -Wl,--no-undefined \
          "''${linkFlags[@]}" \
          -o $out/lib/lib$libName.so \
          "''${objects[@]}" \
          "''${libs[@]}" \
          "''${linkLibs[@]}"
      '';

  /**
    Build a shared library component.

    - `name`: derivation name (e.g. `nix-util`).
    - `libName`: library name without `lib` prefix (e.g. `nixutil`).
    - `roots`: list of `{ root; prefix; }` directories scanned for sources and headers.
    - `includeDirs`: include search path, relative to the root namespace.
    - `sourceExtensions`: suffixes identifying compilation units.
    - `sources`: explicit list of compilation units, as paths in the root
      namespace. By default, every file in the roots or `files` with a
      source extension is a compilation unit.
    - `excludeSources`: paths (files, or directories with everything below
      them) to leave out of the compilation units.
    - `files`: extra files (e.g. generated headers) by path in the root namespace.
    - `externalDeps`: list of `{ prefix; pkg; pkgconfig ? []; }` mapping
      external `#include`s to packages.
    - `extraCxxFlags`, `linkFlags`, `linkLibs`, `linkPkgConfig`, `linkDeps`: what they say.
  */
  mkComponent =
    {
      name,
      libName,
      roots,
      includeDirs,
      sourceExtensions ? [
        ".cc"
        ".cpp"
        ".cxx"
        ".c"
      ],
      sources ? null,
      excludeSources ? [ ],
      files ? { },
      externalDeps ? [ ],
      extraCxxFlags ? [ ],
      linkFlags ? [ ],
      linkLibs ? [ ],
      linkPkgConfig ? [ ],
      linkDeps ? [ ],
    }@args:
    let
      units = getDeps (
        {
          inherit builtins;
          inherit
            roots
            includeDirs
            sourceExtensions
            excludeSources
            ;
          # Work around a crash in `builtins.wasm` (Nix <= 3.22.5) when
          # copying "layered" attribute sets (the result of `//`) into Wasm:
          # `mapAttrs` produces a fresh, non-layered attribute set.
          files = lib.mapAttrs (_: v: v) files;
        }
        // lib.optionalAttrs (sources != null) { inherit sources; }
      );
      component = args // {
        inherit
          units
          externalDeps
          extraCxxFlags
          linkFlags
          linkLibs
          linkPkgConfig
          linkDeps
          ;
      };
    in
    linkSharedLibrary component (map (compileUnit component) units);

  # Sources from nix-meson-build-support that Meson links into every component.
  commonSupportFiles = {
    "nix-meson-build-support/common/assert-fail/wrap-assert-fail.cc" =
      ../../nix-meson-build-support/common/assert-fail/wrap-assert-fail.cc;
  };

in
{
  inherit
    getDeps
    mkConfigHeader
    mkComponent
    commonSupportFiles
    ;
}
