# The build configuration options, as a NixOS-style module. A build
# variant is defined by evaluating this module together with a module
# setting the options, see `makeNixVariant` in flake.nix.
{ lib, ... }:

{
  options = {
    compiler = lib.mkOption {
      type = lib.types.enum [
        "gcc"
        "clang"
      ];
      default = "gcc";
      description = "The C++ compiler to build Nix with. Dependencies are used as built by Nixpkgs either way.";
    };

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

    sentry = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Whether to enable Sentry crash reporting in the `nix` executable.";
    };

    otel = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Whether to enable distributed tracing via OpenTelemetry in the `nix` executable.";
    };

    awsAuth = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Whether to support AWS authentication for S3 binary caches.";
    };

    boehmgc = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Whether to use the Boehm garbage collector in the Nix language evaluator.";
    };

    wasm = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Whether to support WebAssembly: `builtins.wasm` in the evaluator and building WASI derivations.";
    };
  };
}
