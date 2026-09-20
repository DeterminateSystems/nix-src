# Generic machinery for building C++ components with one derivation per
# compilation unit.
{ pkgs }:

let
  inherit (pkgs) lib;

  getDeps = builtins.wasm {
    path = ./scanner.wasm;
    function = "getDeps";
  };

  externalDeps = import ./deps.nix { inherit pkgs; };

  # `builtins.parallel xs x` starts evaluating the values `xs` on other
  # threads and returns `x`. It requires the `parallel-eval` experimental
  # feature (and `eval-cores`); without it, evaluation is sequential.
  parallel = builtins.parallel or (xs: x: x);

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

  commonLinkLibs = [
    "-pthread"
    "-ldl"
    "-lrt"
  ];

  # Write a file to the store. `builtins.toFile` cannot reference
  # derivations, so fall back to `writeText` for content with context;
  # such a file is then built when the scanner reads it at eval time.
  writeFile =
    name: text:
    if builtins.hasContext text then pkgs.writeText name text else builtins.toFile name text;

  # Generate a header with `#define`s, like Meson's `configure_file()`.
  # Integers and booleans become bare defines, strings become quoted
  # defines, and `null` becomes an `#undef`.
  mkConfigHeader =
    name: attrs:
    writeFile name (
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

  # Wrap a file in a C++ raw string literal, like the `gen_header`
  # generator in nix-meson-build-support/generate-header.
  mkStringHeader =
    path:
    builtins.toFile "${baseNameOf path}.gen.hh" "R\"__NIX_STR(\n${builtins.readFile path})__NIX_STR\"\n";

  # Map external (angle-bracket) includes to the entries of `externalDeps`,
  # by prefix match, deduplicated. Unmatched includes (libc, libstdc++) are
  # ignored.
  matchExternalDeps =
    externalIncludes:
    let
      matches = lib.filter (d: d != null) (
        map (inc: lib.findFirst (d: lib.hasPrefix d.prefix inc) null externalDeps) externalIncludes
      );
    in
    lib.attrValues (lib.listToAttrs (map (d: lib.nameValuePair d.prefix d) matches));

  depPackages = deps: lib.concatMap (d: lib.toList d.pkg) deps;

  compileUnit =
    component: unit:
    let
      deps = matchExternalDeps unit.externalIncludes;
    in
    pkgs.runCommandCC "${baseNameOf unit.path}.o"
      {
        __structuredAttrs = true;
        inherit (unit) src includes;
        srcPath = unit.path;
        includeDirs = component.includeDirs ++ component.depIncludeDirs;
        cxxFlags = commonCxxFlags ++ component.extraCxxFlags;
        pkgConfigDeps = lib.concatMap (d: d.pkgconfig or [ ]) deps;
        buildInputs = depPackages deps;
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
    let
      # Link against every external dependency used by any unit.
      deps = matchExternalDeps (lib.unique (lib.concatMap (u: u.externalIncludes) component.units));
    in
    pkgs.runCommandCC component.name
      {
        __structuredAttrs = true;
        # Instantiate the objects and the dependencies in parallel.
        objects = parallel (map (d: d.drvPath) component.allDeps ++ map (o: o.drvPath) objects) objects;
        inherit (component) libName linkFlags;
        linkPkgConfig = lib.concatMap (d: d.pkgconfig or [ ]) deps;
        linkLibs =
          map (d: "-l${d.libName}") component.allDeps
          ++ lib.concatMap (d: d.libs or [ ]) deps
          ++ component.extraLinkLibs
          ++ commonLinkLibs;
        buildInputs = component.allDeps ++ depPackages deps;
        nativeBuildInputs = [ pkgs.pkg-config ];
        passthru = {
          inherit objects;
          inherit (component) units;
          inherit component;
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
    - `deps`: other components this one depends on. Their public headers
      are made available under `<name>/` and their libraries are linked.
      Dependencies are transitive.
    - `roots`: list of `{ root; prefix; }` directories scanned for sources and headers.
    - `includeDirs`: include search path, relative to the root namespace.
    - `publicIncludeDirs`: the subset of `includeDirs` exported to
      dependent components. Defaults to all but the root.
    - `sourceExtensions`: suffixes identifying compilation units.
    - `sources`: explicit list of compilation units, as paths in the root
      namespace. By default, every file in the roots or `files` with a
      source extension is a compilation unit.
    - `excludeSources`: paths (files, or directories with everything below
      them) to leave out of the compilation units.
    - `files`: extra files (e.g. generated headers) by path in the root namespace.
    - `configHeaders`: generated config headers, as an attribute set from
      path in the root namespace to the `#define`s (see `mkConfigHeader`).
      Their macros are also used to evaluate preprocessor conditionals.
    - `defines`, `undefines`: further macros known to be defined (with
      value) or undefined when evaluating preprocessor conditionals, on
      top of the platform defaults and the config headers.
    - `extraCxxFlags`, `linkFlags`, `extraLinkLibs`: what they say.

    External dependencies (compile flags and libraries) are derived from
    the `#include`s of the units, via `deps.nix`.
  */
  mkComponent =
    {
      name,
      libName,
      deps ? [ ],
      roots,
      includeDirs,
      publicIncludeDirs ? lib.filter (d: d != "") includeDirs,
      sourceExtensions ? [
        ".cc"
        ".cpp"
        ".cxx"
        ".c"
      ],
      sources ? null,
      excludeSources ? [ ],
      files ? { },
      configHeaders ? { },
      defines ? { },
      undefines ? [ ],
      extraCxxFlags ? [ ],
      linkFlags ? [ ],
      extraLinkLibs ? [ ],
    }@args:
    let
      # The transitive closure of the dependencies, direct ones first.
      allDeps = lib.unique (deps ++ lib.concatMap (d: d.component.allDeps) deps);

      # Dependencies contribute their roots, public include directories and
      # extra files under `<name>/`, but no compilation units.
      prefixed = d: p: if p == "" then d.component.name else "${d.component.name}/${p}";
      depRoots = lib.concatMap (
        d:
        map (r: {
          inherit (r) root;
          prefix = prefixed d r.prefix;
        }) d.component.roots
      ) allDeps;
      depIncludeDirs = lib.concatMap (d: map (prefixed d) d.component.publicIncludeDirs) allDeps;
      depFiles = lib.foldl' (
        acc: d: acc // lib.mapAttrs' (k: v: lib.nameValuePair (prefixed d k) v) d.component.files
      ) { } allDeps;
      depExcludes = map (d: d.component.name) allDeps;

      # Generated config headers, whose macros are also known to the scanner.
      configFiles = lib.mapAttrs (path: attrs: mkConfigHeader (baseNameOf path) attrs) configHeaders;
      configDefines = lib.foldl' (acc: attrs: acc // attrs) { } (lib.attrValues configHeaders);
      allFiles = files // configFiles;

      # Macros for evaluating conditionals: platform defaults, then the
      # dependencies' config macros, then our own. `null` means undefined.
      allDefines =
        lib.foldl' (acc: d: acc // d.component.allDefines) platformDefines allDeps
        // configDefines
        // defines;
      knownDefines = lib.mapAttrs (
        _: v: if builtins.isBool v then (if v then "1" else "0") else toString v
      ) (lib.filterAttrs (_: v: v != null) allDefines);
      knownUndefines = lib.unique (
        platformUndefines ++ undefines ++ lib.attrNames (lib.filterAttrs (_: v: v == null) allDefines)
      );

      # Scan this component in parallel with its dependencies.
      units = parallel (map (d: d.units) deps) (
        getDeps (
          {
            inherit builtins;
            inherit sourceExtensions;
            roots = roots ++ depRoots;
            includeDirs = includeDirs ++ depIncludeDirs;
            excludeSources = excludeSources ++ depExcludes;
            # Work around a crash in `builtins.wasm` (Nix <= 3.22.5) when
            # copying "layered" attribute sets (the result of `//`) into Wasm:
            # `mapAttrs` produces a fresh, non-layered attribute set.
            files = lib.mapAttrs (_: v: v) (allFiles // depFiles);
            defines = lib.mapAttrs (_: v: v) knownDefines;
            undefines = knownUndefines;
          }
          // lib.optionalAttrs (sources != null) { inherit sources; }
        )
      );

      component = args // {
        files = allFiles;
        inherit
          units
          allDeps
          allDefines
          depIncludeDirs
          publicIncludeDirs
          extraCxxFlags
          linkFlags
          extraLinkLibs
          ;
      };
    in
    linkSharedLibrary component (map (compileUnit component) units);

  # Macros the scanner can rely on when evaluating preprocessor conditionals
  # (Linux, x86_64, GCC). Only what is certain: an unknown macro keeps both
  # branches of a conditional, but a wrong one would drop includes.
  platformDefines = {
    __linux__ = 1;
    __gnu_linux__ = 1;
    __unix__ = 1;
    __unix = 1;
    __x86_64__ = 1;
    __LP64__ = 1;
  };
  platformUndefines = [
    "_WIN32"
    "_WIN64"
    "__CYGWIN__"
    "__MINGW32__"
    "__APPLE__"
    "__MACH__"
    "__FreeBSD__"
    "__NetBSD__"
    "__OpenBSD__"
    "__DragonFly__"
    "__sun"
    "__clang__"
  ];

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
    mkStringHeader
    mkComponent
    commonSupportFiles
    ;
}
