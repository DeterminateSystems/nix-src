R""(

# Examples

* Show the provenance of a store path:

  ```console
  # nix provenance show /run/current-system
  /nix/store/k145bdxhdb89i4fkvgdisdz1yh2wiymm-nixos-system-machine-25.05.20251210.d2b1213
  ← copied from cache.flakehub.com
  ← built from derivation /nix/store/w3p3xkminq61hs00kihd34w1dglpj5s9-nixos-system-machine-25.05.20251210.d2b1213.drv (output out) on build-machine for x86_64-linux
  ← instantiated from flake output github:my-org/my-repo/6b03eb949597fe96d536e956a2c14da9901dbd21?dir=machine#nixosConfigurations.machine.config.system.build.toplevel
  ```

# Description

Show the provenance chain of one or more store paths. For each store path, this displays where it came from: what binary cache it was copied from, what flake it was built from, and so on.

The provenance chain shows the history of how the store path came to exist, including:

- **Copied**: The path was copied from another Nix store, typically a binary cache.
- **Built**: The path was built from a derivation.
- **Flake evaluation**: The derivation was instantiated during the evaluation of a flake output.
- **Fetched**: The path was obtained by fetching a source tree.
- **Meta**: Metadata associated with the derivation.

Note: if you want provenance in JSON format, use the `provenance` field returned by `nix path-info --json`.

)""
