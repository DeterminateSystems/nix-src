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
  };
}
