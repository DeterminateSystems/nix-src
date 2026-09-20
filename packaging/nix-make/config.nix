# The build configuration options, as a NixOS-style module. A build
# variant is defined by evaluating this module together with a module
# setting the options, see `makeNixVariant` in flake.nix.
{ lib, ... }:

{
  options = {
    optimize = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Whether to compile with optimization.";
    };

    debug = lib.mkOption {
      type = lib.types.bool;
      default = false;
      description = "Whether to include debug information.";
    };
  };
}
