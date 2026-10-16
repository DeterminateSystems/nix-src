R""(

# Examples

* Bake the `patchelf` flake into the directory `./patchelf-baked`:

  ```console
  # nix flake bake github:NixOS/patchelf --dest-dir ./patchelf-baked
  ```

  The baked flake can be used like any other flake:

  ```console
  # nix flake show path:./patchelf-baked
  # nix build path:./patchelf-baked#patchelf
  ```

* Bake the outputs for all systems, not just the current one:

  ```console
  # nix flake bake github:NixOS/patchelf --dest-dir ./patchelf-baked --all-systems
  ```

# Description

This command evaluates the outputs of the flake specified by flake reference *flake-url* and writes a new, *baked* flake to the directory *path* (specified by `--dest-dir`). The baked flake provides the same outputs as the original flake, but they are pre-evaluated: every derivation is replaced by a [baked derivation](@docroot@/language/builtins.md#builtins-bakedDerivation), that is, a derivation with the builder `builtin:substitute` that is never built but whose outputs are obtained by substitution from a binary cache. As a result, evaluating a baked flake is much faster than evaluating the original, since it does (almost) no evaluation.

The baked flake has no inputs, so it can be used without access to the inputs of the original flake. It provides its own [flake schemas](@docroot@/protocols/flake-schemas.md) (in the `schemas` output) that reproduce the structure of the original flake's outputs, so `nix flake show` and `nix flake check` work on it without needing the original flake's schemas.

Derivation attributes in the baked flake have the same `name`, `system`, `outPath`, `outputName`, `outputs` and `meta.mainProgram` as in the original flake, so commands such as `nix build` and `nix run` behave the same. Their `drvPath` is different, however: it refers to the baked derivation rather than the original one. The outputs of a baked flake can also be used as inputs of other derivations, e.g. by another flake that has the baked flake as an input.

Building the outputs of a baked flake requires that their store paths can be [substituted](@docroot@/command-ref/conf-file.md#conf-substituters) (or are already present in the Nix store). If a store path cannot be substituted, the build fails; Nix will not fall back to building the original derivation.

By default, only the outputs for the current system are baked. Use `--all-systems` to bake the outputs for all systems.

# Limitations

* Only outputs that are recognised by the [flake schemas](@docroot@/protocols/flake-schemas.md) are baked. Non-derivation outputs, such as `overlays`, `nixosModules` and `templates`, are omitted from the baked flake.

* Derivations whose output paths are not known at evaluation time cannot be baked. This includes [content-addressed derivations](@docroot@/store/derivation/outputs/content-address.md) and impure derivations. `nix flake bake` prints a warning for each such derivation and omits it from the baked flake.

* Outputs whose derivation is nested inside the output attribute (such as `nixosConfigurations.<name>.config.system.build.toplevel`) are baked at that nested attribute. Other attributes of the output are not included.

This command requires the [`baked-derivations`](@docroot@/development/experimental-features.md#xp-feature-baked-derivations) experimental feature, both in the Nix CLI and in the Nix daemon.

)""
