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

### `GetNarInfosV1`

The path (relative to the cache URL) of an endpoint for fetching the
metadata of multiple store paths in a single request. If this field is
present, a client may send a `POST` request to this path whose body is
a list of store path hash parts (one per line). The server responds
with one JSON object per line (newline-delimited JSON), each being the
[version 2 JSON representation](@docroot@/protocols/json/store-object-info.md)
of a store path's metadata, extended with a `"path"` field holding the
store path it describes. This lets a client fetch the metadata for many
paths in one request instead of one request per path. A path whose
object is absent from the response is not available in the cache.

Each object may also contain a `"partialClosure"` field: an array of
store paths that are (some of) the path's indirect references. This is
a hint that lets a client start fetching the metadata of an entire
closure without waiting for the intervening objects; it need not be
complete and is not covered by the signature.

If the field is absent, the client falls back to fetching each
`.narinfo` individually.

## Other fields

Any field not listed above is stored verbatim in the client's
[on-disk cache](#caching-behavior) of `nix-cache-info` but is otherwise
ignored. This keeps the format forward-compatible: a newer server can
advertise a capability that an older client persists, and a later
client version can start using it without the server's metadata having
to be re-fetched.

## Example

```text
StoreDir: /nix/store
WantMassQuery: 1
Priority: 30
GetNarInfosV1: /get-narinfos-v1
```

## Caching Behavior

Nix caches `nix-cache-info` in the [cache directory](@docroot@/command-ref/env-common.md#env-NIX_CACHE_HOME) with a 7-day TTL.

## See Also

- [HTTP Binary Cache Store](@docroot@/store/types/http-binary-cache-store.md)
- [Serving a Nix store via HTTP](@docroot@/package-management/binary-cache-substituter.md)
- [`substituters`](@docroot@/command-ref/conf-file.md#conf-substituters)
