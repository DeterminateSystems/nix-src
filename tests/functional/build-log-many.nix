with import ./config.nix;

{ n }:

# `n` builds that print one line and exit right away.
builtins.genList (
  i:
  mkDerivation {
    name = "build-log-many-${toString i}";
    buildCommand = ''
      echo "output of build ${toString i}"
      echo > $out
    '';
  }
) n
