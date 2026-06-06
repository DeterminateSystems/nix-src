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

### `BloomFilter`

URL of a [Bloom filter](@docroot@/protocols/binary-cache-bloom-filter.md) that enumerates the store paths held by this cache.
Clients may use it to skip `.narinfo` requests for paths the filter rules out.

The value is either an absolute URL or a path relative to the cache root:

```
BloomFilter: /bloom-filter
BloomFilter: https://filters.example.com/cache-abc.bloom
```

If absent, the cache does not publish a Bloom filter and clients must not assume one is available at any default location.

## Example

```
StoreDir: /nix/store
WantMassQuery: 1
Priority: 30
BloomFilter: /bloom-filter
```

## Caching Behavior

Nix caches `nix-cache-info` in the [cache directory](@docroot@/command-ref/env-common.md#env-NIX_CACHE_HOME) with a 7-day TTL.

## See Also

- [HTTP Binary Cache Store](@docroot@/store/types/http-binary-cache-store.md)
- [Serving a Nix store via HTTP](@docroot@/package-management/binary-cache-substituter.md)
- [`substituters`](@docroot@/command-ref/conf-file.md#conf-substituters)
