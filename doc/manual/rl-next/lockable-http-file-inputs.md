---
synopsis: "The Lockable HTTP Protocol now applies to file inputs"
---

The [Lockable HTTP Protocol](@docroot@/protocols/tarball-fetcher.md) only applied to tarball inputs.
Nix ignored the `Link: <...>; rel="immutable"` header for an input with `flake = false` and a URL without a tarball extension, because such an input is a file input.

Nix now applies the immutable URL to file inputs too:

```nix
inputs.determinate-pkg = {
  url = "https://install.determinate.systems/determinate-pkg/stable/Universal";
  flake = false;
};
```

The immutable URL must have the same input type as the input that Nix fetched.
For a file input whose immutable URL has a tarball extension, the server must put the prefix `file+` in front of the transport scheme.

Nix also compares the contents that it downloads against the `narHash` in the immutable URL, and gives an error if the two values are different.
Before, Nix ignored that value.
