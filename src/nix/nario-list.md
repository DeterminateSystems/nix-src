R""(

# Examples

* List the contents of a nario file:

  ```console
  # nix nario list < dump.nario
  /nix/store/4y1jj6cwvslmfh1bzkhbvhx77az6yf00-xgcc-14.2.1.20250322-libgcc: 201856 bytes
  /nix/store/d8hnbm5hvbg2vza50garppb63y724i94-libunistring-1.3: 2070240 bytes
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

)""
