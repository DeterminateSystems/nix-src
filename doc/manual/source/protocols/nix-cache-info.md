# Nix Cache Info Format

The `nix-cache-info` file is a metadata file at the root of a [binary cache](@docroot@/package-management/binary-cache-substituter.md) (e.g., `https://cache.example.com/nix-cache-info`).

MIME type: `text/x-nix-cache-info`

## Format

Line-based key-value format:

```
Key: value
```

Leading and trailing whitespace is trimmed from values.
Lines without a colon are ignored.
Unknown keys are silently ignored.

## Fields

### `StoreDir`

The Nix store directory path that this cache was built for (e.g., `/nix/store`).

If present, Nix verifies that this matches the client's store directory:

```
error: binary cache 'https://example.com' is for Nix stores with prefix '/nix/store', not '/home/user/nix/store'
```

### `WantMassQuery`

`1` or `0`. Sets the default for [`want-mass-query`](@docroot@/store/types/http-binary-cache-store.md#store-http-binary-cache-store-want-mass-query).

### `Priority`

Integer. Sets the default for [`priority`](@docroot@/store/types/http-binary-cache-store.md#store-http-binary-cache-store-priority).

### `Features`

A space-separated list of optional protocol features that the cache
server supports. Clients ignore any feature names they don't
recognise, and assume no features if the field is absent. This allows
a client to use server capabilities beyond the basic binary cache
protocol only when they're available.

Currently defined features:

- `get-narinfos-v1`: the server accepts a `POST` request to
  `get-narinfos-v1` whose body is a list of store path hash parts (one
  per line) and responds with the concatenation of the corresponding
  `.narinfo` files, separated by empty lines. This lets a client fetch
  the `.narinfo` files for many paths in a single request instead of
  one request per path. A path whose `.narinfo` is absent from the
  response is not available in the cache.

## Example

```
StoreDir: /nix/store
WantMassQuery: 1
Priority: 30
Features: get-narinfos-v1
```

## Caching Behavior

Nix caches `nix-cache-info` in the [cache directory](@docroot@/command-ref/env-common.md#env-NIX_CACHE_HOME) with a 7-day TTL.

## See Also

- [HTTP Binary Cache Store](@docroot@/store/types/http-binary-cache-store.md)
- [Serving a Nix store via HTTP](@docroot@/package-management/binary-cache-substituter.md)
- [`substituters`](@docroot@/command-ref/conf-file.md#conf-substituters)
