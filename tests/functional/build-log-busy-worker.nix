with import ./config.nix;

{ n, sizes }:

let
  dep = mkDerivation {
    name = "build-log-busy-worker-dep";
    buildCommand = "echo > $out";
  };

  # The pre-build hook blocks the worker when it starts this build,
  # which is when `dep` has been built.
  slow = mkDerivation {
    name = "build-log-busy-worker-slow";
    inherit dep;
    buildCommand = "echo > $out";
  };
in

[ slow ]

# Builds that print one line and exit while the worker is blocked.
++ builtins.genList (
  i:
  mkDerivation {
    name = "build-log-busy-worker-${toString i}";
    buildCommand = ''
      sleep 1
      echo "output of build ${toString i}"
      echo > $out
    '';
  }
) n

# Builds that print about as much as fits in a pipe on macOS and exit
# while the worker is blocked.
++ map (
  size:
  mkDerivation {
    name = "build-log-busy-worker-big-${toString size}";
    buildCommand = ''
      sleep 1
      head -c ${toString size} /dev/zero | tr '\0' x
      echo
      echo "end of output"
      echo > $out
    '';
  }
) sizes
