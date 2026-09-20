# Proof of concept: building Nix with Nix as the C++ build system, with one
# derivation per compilation unit. The `#include` closure of each unit is
# computed at evaluation time by a scanner running under `builtins.wasm`.
{
  inputs.nix.url = "../..";
  inputs.nixpkgs.follows = "nix/nixpkgs";

  outputs =
    {
      self,
      nix,
      nixpkgs,
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
      make.release = makeNixVariant {
        optimize = true;
        debug = false;
      };
      make.debugoptimized = makeNixVariant {
        optimize = true;
        debug = true;
      };
      make.debug = makeNixVariant {
        optimize = false;
        debug = true;
      };

      packages.${system} = make.release // {
        default = make.release.nix;
      };
    };
}
