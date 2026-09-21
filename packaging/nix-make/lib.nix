# Generic machinery for building C++ components with one derivation per
# compilation unit.
{ pkgs, config }:

let
  inherit (pkgs) lib;

  getDeps = builtins.wasm {
    path = ./scanner.wasm;
    function = "getDeps";
  };

  externalDeps = import ./deps.nix { inherit pkgs; };

  # The version, as in the package.nix files.
  defaultVersion = lib.fileContents ../../.version-determinate;

  # The stdenv providing the compiler. `clangStdenv` uses libstdc++, so the
  # dependencies built by Nixpkgs with GCC can be used as is.
  stdenv = if config.compiler == "clang" then pkgs.clangStdenv else pkgs.stdenv;

  # `builtins.parallel xs x` starts evaluating the values `xs` on other
  # threads and returns `x`. It requires the `parallel-eval` experimental
  # feature (and `eval-cores`); without it, evaluation is sequential.
  parallel = builtins.parallel or (xs: x: x);

  # Optimization and debug flags, following Meson's build types: `release`
  # is `-O3`, `debugoptimized` is `-O2 -g`, and `debug` is `-O0 -g`.
  optimizationFlags =
    if config.optimize then [ (if config.debug then "-O2" else "-O3") ] else [ "-O0" ];
  debugFlags = lib.optional config.debug "-g";

  # From nix-meson-build-support/common/meson.build, where Meson keeps the
  # ones the compiler supports; the last few are compiler-specific.
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
  ]
  ++ (
    if config.compiler == "clang" then
      [ "-Werror=c99-designator" ]
    else
      [
        "-Wno-interference-size"
        "-Wno-subobject-linkage"
      ]
  );

  # Meson enables this for libutil, libstore and libexpr only: all vtables
  # must have a "key" function so they are emitted as strong symbols, which
  # matters for dynamic linking on Darwin. Clang only.
  weakVtablesFlags = lib.optional (config.compiler == "clang") "-Werror=weak-vtables";

  commonCxxFlags = [
    "-std=c++23"
    "-fPIC"
    "-pthread"
    "-D_FILE_OFFSET_BITS=64"
    "-D_GLIBCXX_USE_TBB_PAR_BACKEND=0"
    "-fno-semantic-interposition"
  ]
  ++ optimizationFlags
  ++ debugFlags
  ++ warningFlags;

  commonLinkLibs = [
    "-pthread"
    "-ldl"
    "-lrt"
  ];

  # A stand-in for a configuration header (like Meson's `configure_file()`
  # output). It is empty: the macros are passed to the compiler as `-D`
  # flags instead, and only to the units that use them, so that changing a
  # macro does not rebuild every unit that includes the header.
  mkStubHeader =
    name:
    builtins.toFile name "#pragma once\n// The macros of this configuration header are passed to the compiler as -D flags.\n";

  # Wrap a file in a C++ raw string literal, like the `gen_header`
  # generator in nix-meson-build-support/generate-header.
  mkStringHeader =
    path:
    builtins.toFile "${baseNameOf path}.gen.hh" "R\"__NIX_STR(\n${builtins.readFile path})__NIX_STR\"\n";

  # The `dev` outputs, as `stdenv.mkDerivation` would pick for `buildInputs`.
  depPackages = deps: map lib.getDev (lib.concatMap (d: lib.toList d.pkg) deps);

  # A minimal derivation running a nushell build script, which (unlike
  # bash) can read the typed JSON of the structured attributes
  # (`$NIX_ATTRS_JSON_FILE`). A bash prelude sources the stdenv setup
  # script first, which provides the C++ compiler and handles
  # `buildInputs` etc. This avoids the evaluation cost of
  # `stdenv.mkDerivation`, which is significant for hundreds of
  # derivations, and unlike `runCommand` it does not set
  # `preferLocalBuild`, so units can be built remotely.
  mkNuDerivation =
    attrs: nuScript:
    derivation (
      {
        system = stdenv.hostPlatform.system;
        builder = stdenv.shell;
        args = [
          "-e"
          (builtins.toFile "builder.sh" ''
            # With structured attrs, attributes are not in the environment.
            if [ -e "$NIX_ATTRS_SH_FILE" ]; then . "$NIX_ATTRS_SH_FILE"; fi
            source $stdenv/setup
            exec nu --no-config-file ${nuScript}
          '')
        ];
        inherit stdenv;
        __structuredAttrs = true;
      }
      // attrs
      // {
        nativeBuildInputs = attrs.nativeBuildInputs or [ ] ++ [ pkgs.nushell ];
      }
    );

  # The stdenv's default hardening flags, except that `_FORTIFY_SOURCE`
  # warns on every unit when not optimizing, so it is disabled then (like
  # `hardeningDisable = [ "fortify" ]` in `stdenv.mkDerivation`).
  hardeningFlags = lib.concatStringsSep " " (
    lib.subtractLists (lib.optionals (!config.optimize) [
      "fortify"
      "fortify3"
    ]) stdenv.cc.defaultHardeningFlags
  );

  # Deduplicate strings in O(n log n) rather than `lib.unique`'s O(n^2).
  uniqueStrings = xs: lib.attrNames (lib.listToAttrs (map (x: lib.nameValuePair x null) xs));

  # Deduplicate a list by a key, keeping the first occurrence and the order.
  # Note: `lib.unique` on derivations would compare them by output path,
  # which instantiates them and all their inputs.
  uniqueBy =
    key: xs:
    map (x: x.value) (
      builtins.genericClosure {
        startSet = map (x: {
          key = key x;
          value = x;
        }) xs;
        operator = _: [ ];
      }
    );

  compileUnit =
    component: unit:
    let
      deps = component.externalDepsFor unit.externalIncludes;
    in
    mkNuDerivation {
      name = "${baseNameOf unit.path}.o";
      # The config macros this unit is sensitive to (see `configHeaders`),
      # as reported by the scanner; `null` means undefined. The builder
      # turns them into -D/-U flags.
      defines = unit.usedDefines;
      # Generated files come from the output of their generator derivation.
      src = if unit.src == null then component.allGenerated.${unit.path}.path else unit.src;
      includes =
        unit.includes
        // lib.listToAttrs (
          map (g: lib.nameValuePair g component.allGenerated.${g}.path) unit.generatedIncludes
        );
      srcPath = unit.path;
      includeDirs = component.includeDirs ++ component.depIncludeDirs;
      cxxFlags =
        commonCxxFlags
        ++ component.extraCxxFlags
        ++ (component.unitCxxFlags.${unit.path} or [ ])
        ++ lib.concatMap (d: d.cflags or [ ]) deps;
      pkgConfigDeps = lib.concatMap (d: d.pkgconfig or [ ]) deps;
      buildInputs = depPackages deps;
      nativeBuildInputs = [ pkgs.pkg-config ];
      NIX_HARDENING_ENABLE = hardeningFlags;
    } ./compile.nu;

  # Link the objects into a shared library or an executable.
  link =
    component: objects:
    let
      # Link against every external dependency used by any unit.
      deps = component.externalDepsFor component.allExternalIncludes;
    in
    mkNuDerivation {
      name = "${component.name}-${component.version}";
      # Instantiate the objects and the dependencies in parallel.
      objects = parallel (map (d: d.drvPath) component.allDeps ++ map (o: o.drvPath) objects) objects;
      inherit (component)
        type
        libName
        exeName
        binSymlinks
        linkFlags
        postInstall
        ;
      linkPkgConfig = lib.concatMap (d: d.pkgconfig or [ ]) deps;
      linkLibs =
        map (d: "-l${d.libName}") component.allDeps
        ++ lib.concatMap (d: d.libs or [ ]) deps
        ++ component.extraLinkLibs
        ++ commonLinkLibs;
      buildInputs = component.allDeps ++ depPackages deps;
      nativeBuildInputs = [ pkgs.pkg-config ];
    } ./link.nu
    // {
      inherit objects;
      inherit (component) units;
      inherit component;
    };

  /**
    Build a component: a shared library (the default) or an executable.

    - `name`: component name (e.g. `determinate-nix-util`); the derivation is
      named `<name>-<version>`.
    - `version`: defaults to the contents of `.version-determinate`.
    - `type`: `"library"` or `"executable"`.
    - `libName`: library name without `lib` prefix (e.g. `nixutil`).
    - `exeName`: executable name; defaults to `name`.
    - `binSymlinks`: names of symlinks to the executable to create in `bin/`.
    - `postInstall`: nushell snippet run after linking, with `$env.out` set.
    - `deps`: other components this one depends on. Their public headers
      are made available under `_deps/<name>/` and their libraries are linked.
      Dependencies are transitive.
    - `root`: the directory scanned for sources and headers; shorthand for
      a `roots` entry with an empty prefix.
    - `roots`: further `{ root; prefix; }` directories scanned for sources
      and headers, known by `prefix/<relative path>`.
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
      path in the root namespace to the `#define`s. The header itself is an
      empty stub; each unit gets the macros it uses, and the builder
      (compile.nu) turns them into `-D` flags. Their macros are also used
      to evaluate preprocessor conditionals.
    - `defines`, `undefines`: further macros known to be defined (with
      value) or undefined when evaluating preprocessor conditionals, on
      top of the platform defaults and the config headers.
    - `generated`: files generated at build time (e.g. by bison), as an
      attribute set from path in the root namespace to `{ from; path; }`:
      `from` is the real file whose `#include`s it inherits (so that it
      can be scanned without building it), and `path` is the generated
      file in the output of a derivation.
    - `extraCxxFlags`, `linkFlags`, `extraLinkLibs`: what they say.
    - `unitCxxFlags`: extra compiler flags for specific units, as an
      attribute set from path in the root namespace to a list of flags.

    External dependencies (compile flags and libraries) are derived from
    the `#include`s of the units, via `deps.nix`.
  */
  mkComponent =
    {
      name,
      version ? defaultVersion,
      type ? "library",
      libName ? null,
      exeName ? name,
      binSymlinks ? [ ],
      postInstall ? "",
      deps ? [ ],
      root ? null,
      roots ? [ ],
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
      generated ? { },
      defines ? { },
      undefines ? [ ],
      extraCxxFlags ? [ ],
      unitCxxFlags ? { },
      linkFlags ? [ ],
      extraLinkLibs ? [ ],
    }@args:
    let
      allRoots =
        lib.optional (root != null) {
          inherit root;
          prefix = "";
        }
        ++ roots;

      # The transitive closure of the dependencies, direct ones first.
      allDeps =
        let
          item = d: {
            key = d.component.name;
            value = d;
          };
        in
        map (x: x.value) (
          builtins.genericClosure {
            startSet = map item deps;
            operator = x: map item x.value.component.deps;
          }
        );

      # Dependencies contribute their roots, public include directories and
      # extra files under `_deps/<name>/` (a prefix that cannot collide with
      # the component's own directories), but no compilation units.
      depPrefix = d: "_deps/${d.component.name}";
      prefixed = d: p: if p == "" then depPrefix d else "${depPrefix d}/${p}";
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
      depExcludes = map depPrefix allDeps;
      depGenerated = lib.foldl' (
        acc: d:
        acc
        // lib.mapAttrs' (
          k: g: lib.nameValuePair (prefixed d k) (g // { from = prefixed d g.from; })
        ) d.component.generated
      ) { } allDeps;
      allGenerated = generated // depGenerated;

      # Generated config headers, whose macros are also known to the scanner.
      configFiles = lib.mapAttrs (path: _: mkStubHeader (baseNameOf path)) configHeaders;
      configDefines = lib.foldl' (acc: attrs: acc // attrs) { } (lib.attrValues configHeaders);
      allFiles = files // configFiles;

      # The macros a unit may be sensitive to: the dependencies' config
      # macros and our own. Each unit is passed only the ones it uses.
      sensitiveDefines =
        lib.foldl' (acc: d: acc // d.component.sensitiveDefines) { } allDeps // configDefines;

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
            roots = allRoots ++ depRoots;
            includeDirs = includeDirs ++ depIncludeDirs;
            excludeSources = excludeSources ++ depExcludes;
            # Work around a crash in `builtins.wasm` (Nix <= 3.22.5) when
            # copying "layered" attribute sets (the result of `//`) into Wasm:
            # `mapAttrs` produces a fresh, non-layered attribute set.
            files = lib.mapAttrs (_: v: v) (allFiles // depFiles);
            defines = lib.mapAttrs (_: v: v) knownDefines;
            undefines = knownUndefines;
            # The scanner reports, per unit, which of these occur in its
            # include closure, with their values.
            trackedDefines = lib.mapAttrs (_: v: v) sensitiveDefines;
            generated = lib.mapAttrs (_: g: g.from) allGenerated;
          }
          // lib.optionalAttrs (sources != null) { inherit sources; }
        )
      );

      # Every external include of any unit, and a map from external include to
      # its `externalDeps` entry, computed once per component. `externalDepsFor`
      # then maps a unit's external includes to its dependencies.
      allExternalIncludes = uniqueStrings (lib.concatMap (u: u.externalIncludes) units);
      externalDepByInclude = lib.listToAttrs (
        map (
          inc: lib.nameValuePair inc (lib.findFirst (d: lib.hasPrefix d.prefix inc) null externalDeps)
        ) allExternalIncludes
      );
      externalDepsFor =
        includes:
        uniqueBy (d: d.prefix) (
          lib.filter (d: d != null) (map (inc: externalDepByInclude.${inc}) includes)
        );

      component = args // {
        inherit allExternalIncludes externalDepsFor;
        inherit
          version
          type
          libName
          exeName
          binSymlinks
          postInstall
          ;
        roots = allRoots;
        # Only our own files and generated files are exported to dependents;
        # their own dependencies are resolved transitively.
        files = allFiles;
        inherit generated allGenerated;
        inherit
          deps
          units
          allDeps
          allDefines
          sensitiveDefines
          depIncludeDirs
          publicIncludeDirs
          extraCxxFlags
          unitCxxFlags
          linkFlags
          extraLinkLibs
          ;
      };
    in
    link component (map (compileUnit component) units);

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
  # The build configuration (see config.nix), for component files that
  # need to vary with it.
  inherit config;

  inherit
    getDeps
    weakVtablesFlags
    mkStringHeader
    mkComponent
    commonSupportFiles
    ;
}
