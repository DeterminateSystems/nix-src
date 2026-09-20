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

      nixMake = import ./lib.nix { inherit pkgs; };

      components = rec {
        nix-util = import ./libutil.nix { inherit nixMake; };
        nix-store = import ./libstore.nix {
          inherit pkgs nixMake;
          inherit nix-util;
        };
        nix-fetchers = import ./libfetchers.nix {
          inherit nixMake;
          inherit nix-util nix-store;
        };
        nix-expr = import ./libexpr.nix {
          inherit pkgs nixMake;
          inherit nix-util nix-store nix-fetchers;
        };
        nix-flake = import ./libflake.nix {
          inherit nixMake;
          inherit
            nix-util
            nix-store
            nix-fetchers
            nix-expr
            ;
        };
        nix-main = import ./libmain.nix {
          inherit nixMake;
          inherit nix-util nix-store nix-expr;
        };
        nix-cmd = import ./libcmd.nix {
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
        nix = import ./nix.nix {
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
    {
      packages.${system} = components // {
        default = components.nix;
      };

      # The raw scanner output for each component, for debugging.
      lib.${system}.scan = pkgs.lib.mapAttrs (_: c: c.units) components;
    };
}
