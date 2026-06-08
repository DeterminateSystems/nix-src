#pragma once
///@file

#include "nix/store/path.hh"
#include "nix/util/base-nix-32.hh"
#include "nix/util/util.hh"

#include <cassert>
#include <cstdint>
#include <string>

namespace nix {

/**
 * Build a bloom-filter blob (32-byte header + raw bit array, see
 * `doc/manual/source/protocols/binary-cache-bloom-filter.md`) from a
 * set of store paths.
 */
std::string buildBloomFilter(const StorePathSet & paths, double falsePositiveRate);

/**
 * Invoke `f(uint64_t pos)` for each of the `k` bit positions in an
 * `mBits`-sized Bloom filter that correspond to `path`.
 *
 * Kirsch-Mitzenmacher double hashing over the 160 bits of the path's
 * `hashPart`; intermediate arithmetic wraps modulo 2^64 before the
 * final modulo by `mBits`. See
 * `doc/manual/source/protocols/binary-cache-bloom-filter.md` for the
 * full specification.
 */
template<typename F>
void forEachBloomBitPosition(const StorePath & path, uint32_t k, uint64_t mBits, F && f)
{
    auto raw = BaseNix32::decode(std::string(path.hashPart()));
    assert(raw.size() == 20);
    auto * b = reinterpret_cast<unsigned char *>(raw.data());
    uint64_t h1 = readLittleEndian<uint64_t>(b);
    uint64_t h2 = readLittleEndian<uint64_t>(b + 8);
    for (uint32_t i = 0; i < k; ++i)
        f((h1 + uint64_t(i) * h2) % mBits);
}

} // namespace nix
