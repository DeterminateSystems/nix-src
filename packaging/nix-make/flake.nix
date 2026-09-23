# Proof of concept: building Nix with Nix as the C++ build system, with one
# derivation per compilation unit. The `#include` closure of each unit is
# computed at evaluation time by a scanner running under `builtins.wasm`.
{
  inputs.nix.url = "../..";
  inputs.nixpkgs.follows = "nix/nixpkgs";
  # The built-in flake schemas that ship with Nix.
  inputs.flake-schemas.url = "../../src/libcmd/builtin-flake-schemas";
  inputs.nix-wasm-module-make.url = "https://flakehub.com/f/DeterminateSystems/nix-wasm-module-make/0";

  outputs =
    {
      self,
      nix,
      nixpkgs,
      nix-wasm-module-make,
      flake-schemas,
    }:
    let
      system = "x86_64-linux";

      pkgs = import nixpkgs {
        inherit system;
        # Provides `nixDependencies2`, the same dependency overrides used by the Meson build.
        overlays = [ nix.overlays.internal ];
      };

      # Build all components with the configuration given by `cfg`, a module
      # setting the options declared in config.nix. The result is a package
      # scope (like `nixComponents` in packaging/components.nix), so it can be
      # extended with `overrideScope`.
      makeNixVariant =
        cfg:
        let
          config =
            (nixpkgs.lib.evalModules {
              modules = [
                ./config.nix
                cfg
              ];
            }).config;
        in
        pkgs.lib.makeScope pkgs.newScope (self: {
          nixMake = import ./lib.nix { inherit pkgs config nix-wasm-module-make; };

          nix-util = self.callPackage ../../src/libutil/make.nix { };
          nix-store = self.callPackage ../../src/libstore/make.nix { };
          nix-fetchers = self.callPackage ../../src/libfetchers/make.nix { };
          nix-expr = self.callPackage ../../src/libexpr/make.nix { };
          nix-flake = self.callPackage ../../src/libflake/make.nix { };
          nix-main = self.callPackage ../../src/libmain/make.nix { };
          nix-cmd = self.callPackage ../../src/libcmd/make.nix { };
          nix = self.callPackage ../../src/nix/make.nix { };

          test-runner = self.callPackage ../../tests/functional/test-runner.nix { };
          functional-tests = self.callPackage ../../tests/functional/make.nix { };
        });
    in
    rec {
      packages.${system} = {
        inherit (make.${system}.release) test-runner;
      };

      devShells.${system}.default = pkgs.mkShell {
        packages = [ make.${system}.release.test-runner ];
      };

      # The build variants, named after Meson's build types.
      make.${system} = {
        release = makeNixVariant {
          optimize = true;
          debug = false;
        };
        debugoptimized = makeNixVariant {
          optimize = true;
          debug = true;
        };
        debug = makeNixVariant {
          optimize = false;
          debug = true;
        };
        # Like `release`, but without the Boehm garbage collector.
        nogc = makeNixVariant {
          optimize = true;
          debug = false;
          boehmgc = false;
        };
        # Like `debug`, but built with clang, which compiles faster.
        debug-fast = makeNixVariant {
          optimize = false;
          debug = true;
          compiler = "clang";
        };
      };

      # Describe the `make` output to `nix flake show` and `nix flake check`.
      # Defining `schemas` replaces the built-in ones, so re-export the ones
      # for the other outputs of this flake.
      schemas = {
        inherit (flake-schemas.schemas)
          schemas
          packages
          devShells
          ;

        make = {
          version = 1;
          doc = ''
            The `make` output provides the Nix components (libraries and the
            `nix` executable) built with Nix as the build system, per system
            and build variant (`release`, `debugoptimized`, `debug`, `nogc`,
            `debug-fast`).
          '';
          roles.nix-build = { };
          appendSystem = true;
          defaultAttrPath = [
            "release"
            "nix"
          ];
          inventory =
            output:
            flake-schemas.lib.mkChildren (
              builtins.mapAttrs (system: variants: {
                forSystems = [ system ];
                children = builtins.mapAttrs (variant: components: {
                  forSystems = [ system ];
                  shortDescription = "The `${variant}` build variant";
                  # The components; a scope also has helper attributes.
                  children = builtins.mapAttrs (name: package: {
                    what = "package";
                    forSystems = [ system ];
                    derivationAttrPath = [ ];
                  }) (nixpkgs.lib.filterAttrs (_: nixpkgs.lib.isDerivation) components);
                }) variants;
              }) output
            );
        };
      };
    };
}
