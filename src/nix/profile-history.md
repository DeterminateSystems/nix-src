R""(

# Examples

* Show the changes between each version of your default profile:

  ```console
  # nix profile history
  Version 508 (2020-04-10):
    flake:nixpkgs#legacyPackages.x86_64-linux.awscli: 1.17.13 added

  Version 509 (2020-05-16) <- 508:
    flake:nixpkgs#legacyPackages.x86_64-linux.awscli: 1.17.13 -> 1.18.211
  ```

# Description

This command shows what packages were added, removed or upgraded
between subsequent versions of a profile. It only shows top-level
packages, not dependencies; for that, use [`nix profile
diff-closures`](./nix3-profile-diff-closures.md).

The addition of a package to a profile is denoted by the string
*version* `added`, whereas the removal is denoted by *version* ` removed`.

If a package was replaced by a different store path with the same
version (e.g. because it was rebuilt against a newer version of a
dependency), this is denoted by *version* `changed`.

With `--show-source`, the locked flake reference of each package
is shown as well. For a package that was upgraded or changed, the old
and new references are shown:

  ```console
  # nix profile history --show-source
  ...
  Version 510 (2020-05-20) <- 509:
    flake:nixpkgs#legacyPackages.x86_64-linux.awscli: 1.18.211 changed (github:NixOS/nixpkgs/6fd8c75 -> github:NixOS/nixpkgs/ffc5e35)
  ```

)""
