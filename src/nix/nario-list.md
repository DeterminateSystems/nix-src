R""(

# Examples

* List the contents of a nario file:

  ```console
  # nix nario list < dump.nario
  /nix/store/f671jqvjcz37fsprzqn5jjsmyjj69p9b-xgcc-14.2.1.20250322-libgcc: 201856 bytes
  /nix/store/n7iwblclbrz20xinvy4cxrvippdhvqll-libunistring-1.3: 2070240 bytes
  …
  ```

* Use `--json` to get detailed information in JSON format:

  ```console
  # nix nario list --json < dump.nario
  {
    "paths": {
      "/nix/store/m1r53pnn…-hello-2.12.1": {
        "ca": null,
        "deriver": "/nix/store/qa8is0vm…-hello-2.12.1.drv",
        "narHash": "sha256-KSCYs4J7tFa+oX7W5M4D7ZYNvrWtdcWTdTL5fQk+za8=",
        "narSize": 234672,
        "references": [
          "/nix/store/g8zyryr9…-glibc-2.40-66",
          "/nix/store/m1r53pnn…-hello-2.12.1"
        ],
        "registrationTime": 1756900709,
        "signatures": [ "cache.nixos.org-1:QbG7A…" ],
        "ultimate": false
      },
      …
    },
    "version": 1
  }
  ```

# Description

This command lists the contents of a nario file read from standard input.

For narios created with `nix nario export --base`, paths that are stored as a binary diff are shown with the diff algorithm (currently always `zstd`), the base path of the diff and the size of the diff. Their file contents are not shown by `--recursive`, since that would require the base path. Paths that are expected to already be present at the destination are shown as such. In the JSON output, these have a `diff` or `present` attribute, respectively.

For narios created with `nix nario export --compression`, the compression method and compressed size of each NAR are shown. In the JSON output, these are in the `compression` attribute.

)""
