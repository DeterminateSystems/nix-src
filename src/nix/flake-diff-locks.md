R""(

# Examples

* Show how the locked inputs of two versions of a flake differ:

  ```console
  # nix flake diff-locks github:NixOS/nix/2.28.0 github:NixOS/nix/2.29.0
  • Updated input 'nixpkgs':
      'github:NixOS/nixpkgs/48d12d5' (2024-12-16)
    → 'github:NixOS/nixpkgs/adaa24f' (2025-05-13)
  ```

* Show what has changed in the lock file of the flake in the worktree relative to a previous Git revision:

  ```console
  # nix flake diff-locks '.?rev=26842787496f2293c676fb36db38dacfd63497e0'
  ```

  or relative to a Git ref:

  ```console
  # nix flake diff-locks '.?ref=HEAD'
  ```

# Description

This command shows the differences between the lock files of two flakes *old-flake* and *new-flake*: inputs that were added, removed or updated. *new-flake* defaults to the flake in the current directory.
The flakes do not need to use the same lock file format version; a change in the lock file version is also reported.

By default, only the locks contained in the two flakes' own lock files are compared.
With `--transitive`, inputs that have a lock file of their own are fetched in order to include their transitive locks in the comparison.

)""
