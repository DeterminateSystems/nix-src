# Binary Cache Bloom Filter Format

A [binary cache](@docroot@/package-management/binary-cache-substituter.md) may publish a Bloom filter of all store paths it contains.
The filter's URL is announced through the [`BloomFilter`](@docroot@/protocols/nix-cache-info.md#bloomfilter) field of the cache's [`nix-cache-info`](@docroot@/protocols/nix-cache-info.md) file — either as an absolute URL or as a path relative to the cache root.
A cache that does not advertise the field does not provide a Bloom filter; clients must not probe for one at a default path.

A Bloom filter lets a client decide that a store path is **definitely not** in the cache without issuing a `.narinfo` request.
Membership tests are one-sided: a "not present" answer is authoritative, while a "possibly present" answer must still be confirmed by fetching the `.narinfo`.
False positives occur at a configurable rate; false negatives do not.

MIME type: `application/octet-stream`

## Format

The response is binary, little-endian, with a fixed 32-byte header followed by the raw bit array:

| Offset | Size       | Field     | Description                                              |
|-------:|-----------:|-----------|----------------------------------------------------------|
| 0      | 8          | `magic`   | ASCII bytes `NixBloom` (no terminating NUL).             |
| 8      | 8          | `version` | `uint64` format version. Currently `1`.                  |
| 16     | 8          | `k`       | `uint64` number of hash functions.                       |
| 24     | 8          | `m`       | `uint64` size of the bit array, in bits. Multiple of 8.  |
| 32     | `m / 8`    | `bits`    | The bit array. Bit at position `p` is `bits[p / 8] >> (p % 8)` masked with `1`. |

The total response size is `32 + m / 8` bytes.

## Membership test

A client tests whether a store path *might* be in the cache as follows:

1. Take the path's [hash part](@docroot@/protocols/store-path.md) — the first 32 [Nix32](@docroot@/protocols/nix32.md) characters of its base name.
2. Decode it into a 20-byte (160-bit) sequence using Nix32 decoding.
3. Read two 64-bit unsigned values from the decoded bytes, little-endian:
   - `h1` from bytes `0..8`
   - `h2` from bytes `8..16`
   (The trailing 4 bytes are unused.)
4. For each `i` in `0, 1, …, k − 1`, compute the bit position
   ```
   pos = ((h1 + i * h2) mod 2^64) mod m
   ```
   The intermediate addition and multiplication wrap modulo 2^64 (standard unsigned 64-bit overflow) before the modulo by `m`.
5. If every `bits[pos / 8] >> (pos % 8)` has its low bit set, the path is *possibly* present; otherwise it is *definitely not* present.

This is the standard Kirsch-Mitzenmacher double-hashing scheme.
Because a store path's hash part is already a cryptographic hash, no further hashing is required.

## Server-side construction

The server populates the filter by performing the same membership procedure for every valid store path and OR-ing in the resulting bits.

Parameters are chosen from the count `n` of valid paths and a target false-positive rate `p`:

```
m = ceil(-n * ln(p) / (ln 2)^2),  rounded up to a multiple of 8
k = max(1, round((m / n) * ln 2))
```

If `n` is zero, the server may emit a minimal filter (e.g., `m = 8`, `k = 1`, all bits zero), which correctly reports every query as "not present".

The choice of `p` is server-defined and not advertised separately: a client can infer the asymptotic FPR from `m` and the number of paths in the cache, but does not need to in order to use the filter.

## Caching

The Bloom filter changes whenever the cache's path set changes.
Clients should refetch periodically; an HTTP cache lifetime on the order of minutes-to-hours is typically appropriate.

## Example

A cache containing roughly 500 000 paths, with a 1% target false-positive rate, produces a filter with `k = 7` and `m ≈ 4.7 × 10^6` bits — roughly 590 KB on the wire including the header.

## See Also

- [Nix Cache Info Format](@docroot@/protocols/nix-cache-info.md)
- [Store Path Specification](@docroot@/protocols/store-path.md)
- [Nix32 Encoding](@docroot@/protocols/nix32.md)
- [HTTP Binary Cache Store](@docroot@/store/types/http-binary-cache-store.md)
