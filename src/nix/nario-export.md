R""(

# Examples

* Export the closure of the build of `nixpkgs#hello`:

  ```console
  # nix nario export --format 2 -r nixpkgs#hello > dump.nario
  ```

  It can be imported into another store:

  ```console
  # nix nario import --no-check-sigs < dump.nario
  ```

* Export the closure of a new version of `hello`, assuming that the destination already has the closure of an old version:

  ```console
  # nix nario export --format 2 -r /nix/store/6i6xl6bmcpxqd51m8nlva40d5c1bhndx-hello-2.12.3 \
      --base /nix/store/nm7p8wxflggcwxfzayhysq4z6a1wg373-hello-2.12.3 > update.nario
  ```

# Description

This command prints to standard output a serialization of the specified store paths in `nario` format. This serialization can be imported into another store using `nix nario import`.

References of a path are not exported by default; use `-r` to export a complete closure.
Paths are exported in topologically sorted order (i.e. if path `X` refers to `Y`, then `Y` appears before `X`).
You must specify the desired `nario` version. Currently the following versions are supported:

* `1`: This version is compatible with the legacy `nix-store --export` and `nix-store --import` commands. It should be avoided because it is not memory-efficient on import. It does not support signatures, so you have to use `--no-check-sigs` on import.

* `2`: The latest version. Recommended.

# Binary diffs

The `--base` flag specifies one or more *installables* whose closure (the *base closure*) is assumed to be present in the store into which the nario will be imported. This can greatly reduce the size of the nario when deploying an update to a closure:

* Paths that are in the base closure are not included in the nario. Instead, the nario records that they're expected to be present, and `nix nario import` fails if they're missing or have a different NAR hash.

* For other paths, `nix nario export` looks for a suitable path in the base closure. How this path is selected is determined by the flag `--base-selection-method`. Currently, the only method is `by-name` (the default), which selects a path with the same name (ignoring the version), e.g. `hello-2.12.2` for `hello-2.12.3`. If a suitable path exists, the path is stored as a binary diff (computed using zstd) against the NAR of that base path, provided that the diff is sufficiently small. `nix nario import` reconstructs the path by applying the diff to the base path, after verifying that the base path has the expected NAR hash.

Binary diffs require nario format 2. Narios that contain binary diffs cannot be imported by versions of Nix that don't support them.

# Compression

By default, the NARs in a nario are not compressed. The `--compression` flag specifies a compression method (such as `zstd` or `xz`) for NARs that are not exported as binary diffs. (Binary diffs are always compressed.) This is preferable to compressing the nario as a whole, since compressing binary diffs a second time is a waste of time. For example:

```console
# nix nario export --format 2 --compression zstd -r /nix/store/6i6xl6bmcpxqd51m8nlva40d5c1bhndx-hello-2.12.3 > hello.nario
```

The `--compression-level` flag specifies the compression level. For `zstd`, the default is 9, which is still fast but compresses significantly better than zstd's own default of 3.

Compression requires nario format 2. Narios that contain compressed NARs cannot be imported by versions of Nix that don't support them.

)""
