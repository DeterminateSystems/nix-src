# Derivation metadata

> **Note**
>
> This feature is only available if the [`provenance` experimental feature](@docroot@/development/experimental-features.md#xp-feature-provenance) is enabled.

[`builtins.derivationWithMeta`](./builtins.md#builtins-derivationWithMeta) works like [`derivation`](./derivations.md) but with the important difference that it accepts an extra `__meta` attribute.
Nix stores the contents of `__meta` in the provenance of the derivation and of any outputs that Nix builds from it.
You can use it to record information such as a package's description, license, or maintainers alongside the store paths it produces, without changing things like the derivation's hash or the output store path.

## Example

```nix
builtins.derivationWithMeta {
  name = "hello";
  system = builtins.currentSystem;
  builder = "/bin/sh";
  args = [ "-c" "echo hello > $out" ];
  __meta = {
    description = "Prints a greeting";
    license = "MIT";
  };
}
```

When you've built this derivation, [`nix provenance show`](@docroot@/command-ref/new-cli/nix3-provenance-show.md) can display this metadata:

```console
$ nix provenance show /nix/store/9388r86smd1v928zp59rwrn43mm2z4id-hello
/nix/store/9388r86smd1v928zp59rwrn43mm2z4id-hello
← built from derivation /nix/store/7j0yix78mp3cz0yxmkaki79f92x03v5a-hello.drv (output out) on my-machine for x86_64-linux
← with derivation metadata
  {
    "description": "Prints a greeting",
    "license": "MIT"
  }
```

The metadata also appears under the `provenance` field in the output of [`nix path-info --json`](@docroot@/command-ref/new-cli/nix3-path-info.md).

## How `__meta` works

- `__meta` must be an attribute set.
  Nix converts it to JSON the same way that [`builtins.toJSON`](./builtins.md#builtins-toJSON) does.
- `__meta` isn't part of the derivation.
  Nix doesn't pass it to the builder, and it has no effect on the derivation's store path, so changing the metadata doesn't cause a rebuild.
- Two derivations that differ only in `__meta` are considered the same derivation.
  Nix keeps the metadata from whichever one it records first.
- Nix records metadata in an output's provenance only when it builds that output.
  It doesn't update the provenance of outputs that already exist, such as outputs that were built earlier or substituted from a binary cache.
- Nix evaluates `__meta` in full when it instantiates the derivation.
  Every value inside it must evaluate without errors, even ones nothing else uses.
- `__meta` can't contain [string context](./string-context.md).
  That rules out paths and strings that refer to store paths, such as `"${pkgs.hello}"`.

## Falling back to `derivation`

If the `provenance` feature is disabled, `builtins.derivationWithMeta` doesn't exist.
Passing `__meta` to plain `derivation` doesn't work either, because Nix tries to turn it into an environment variable for the builder.
To support both cases, check for the builtin and drop `__meta` when it's missing:

```nix
let
  mkDerivation =
    if builtins ? derivationWithMeta then
      builtins.derivationWithMeta
    else
      attrs: derivation (removeAttrs attrs [ "__meta" ]);
in
mkDerivation {
  # ...
  __meta = {
    description = "Prints a greeting";
  };
}
```
