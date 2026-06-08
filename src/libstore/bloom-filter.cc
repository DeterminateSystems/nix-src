#include "nix/store/bloom-filter.hh"
#include "nix/util/serialise.hh"

#include <cmath>

namespace nix {

std::optional<BloomFilterParams> parseBloomFilterHeader(std::string_view header)
{
    using namespace std::string_view_literals;
    if (header.size() < bloomFilterHeaderLen || header.substr(0, 8) != "NixBloom"sv)
        return std::nullopt;

    StringSource source(header.substr(8));
    uint64_t version;
    uint32_t k;
    uint64_t mBits;
    try {
        source >> version >> k >> mBits;
    } catch (SerialisationError &) {
        return std::nullopt;
    }

    if (version != 1 || mBits == 0 || mBits % 8 != 0)
        return std::nullopt;

    return BloomFilterParams{.k = k, .mBits = mBits};
}

std::string buildBloomFilter(const StorePathSet & paths, double falsePositiveRate)
{
    /* Rejects NaN as well, because all comparisons with NaN are false. */
    if (!(falsePositiveRate > 0 && falsePositiveRate < 1))
        throw Error("Bloom filter false positive rate must be between 0 and 1, got %f", falsePositiveRate);

    size_t n = paths.size();

    uint64_t mBits = 8;
    uint32_t k = 1;
    if (n) {
        constexpr double ln2 = 0.6931471805599453;
        double mF = -double(n) * std::log(falsePositiveRate) / (ln2 * ln2);
        /* `falsePositiveRate` very close to 1 makes `mF` round down to zero;
           keep the floor of 8 bits so we never modulo by zero later. */
        mBits = std::max<uint64_t>(8, ((uint64_t(std::ceil(mF)) + 7) / 8) * 8);
        long kL = std::lround((double(mBits) / double(n)) * ln2);
        k = uint32_t(std::max<long>(1, kL));
    }

    StringSink sink(bloomFilterHeaderLen + mBits / 8);

    using namespace std::string_view_literals;
    sink("NixBloom"sv);
    sink << 1; // version
    sink << k;
    sink << mBits;
    assert(sink.s.size() == bloomFilterHeaderLen);

    sink.s.resize(bloomFilterHeaderLen + mBits / 8);
    char * bits = sink.s.data() + bloomFilterHeaderLen;
    for (auto & path : paths)
        forEachBloomBitPosition(path, k, mBits, [&](uint64_t pos) { bits[pos / 8] |= uint8_t(1) << (pos % 8); });

    return std::move(sink.s);
}

} // namespace nix
