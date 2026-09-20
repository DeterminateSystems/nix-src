# Proof of concept: building Nix with Nix as the C++ build system, with one
# derivation per compilation unit. The `#include` closure of each unit is
# computed at evaluation time by a scanner running under `builtins.wasm`.
{
  inputs.nix.url = "../..";
  inputs.nixpkgs.follows = "nix/nixpkgs";
  # The built-in flake schemas that ship with Nix.
  inputs.flake-schemas.url = "../../src/libcmd/builtin-flake-schemas";

  outputs =
    {
      self,
      nix,
      nixpkgs,
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
      # setting the options declared in config.nix.
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

          nixMake = import ./lib.nix { inherit pkgs config; };
        in
        rec {
          nix-util = import ../../src/libutil/make.nix { inherit nixMake; };
          nix-store = import ../../src/libstore/make.nix {
            inherit pkgs nixMake;
            inherit nix-util;
          };
          nix-fetchers = import ../../src/libfetchers/make.nix {
            inherit nixMake;
            inherit nix-util nix-store;
          };
          nix-expr = import ../../src/libexpr/make.nix {
            inherit pkgs nixMake;
            inherit nix-util nix-store nix-fetchers;
          };
          nix-flake = import ../../src/libflake/make.nix {
            inherit nixMake;
            inherit
              nix-util
              nix-store
              nix-fetchers
              nix-expr
              ;
          };
          nix-main = import ../../src/libmain/make.nix {
            inherit nixMake;
            inherit nix-util nix-store nix-expr;
          };
          nix-cmd = import ../../src/libcmd/make.nix {
            inherit nixMake;
            inherit
              nix-util
              nix-store
              nix-fetchers
              nix-expr
              nix-flake
              nix-main
              ;
          };
          nix = import ../../src/nix/make.nix {
            inherit pkgs nixMake;
            inherit
              nix-util
              nix-store
              nix-fetchers
              nix-expr
              nix-flake
              nix-main
              nix-cmd
              ;
          };
        };
    in
    rec {
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
      };

      packages.${system} = make.${system}.release // {
        default = make.${system}.release.nix;
      };

      # Describe the `make` output to `nix flake show` and `nix flake check`.
      # Defining `schemas` replaces the built-in ones, so re-export the ones
      # for the other outputs of this flake.
      schemas = {
        inherit (flake-schemas.schemas) packages schemas;

        make = {
          version = 1;
          doc = ''
            The `make` output provides the Nix components (libraries and the
            `nix` executable) built with Nix as the build system, per system
            and build variant (`release`, `debugoptimized`, `debug`, `nogc`).
          '';
          roles.nix-build = { };
          appendSystem = true;
          defaultAttrPath = [ "release" "nix" ];
          inventory =
            output:
            flake-schemas.lib.mkChildren (
              builtins.mapAttrs (system: variants: {
                forSystems = [ system ];
                children = builtins.mapAttrs (variant: components: {
                  forSystems = [ system ];
                  shortDescription = "The `${variant}` build variant";
                  children = builtins.mapAttrs (name: package: {
                    what = "package";
                    forSystems = [ system ];
                    derivationAttrPath = [ ];
                  }) components;
                }) variants;
              }) output
            );
        };
      };
    };
}
