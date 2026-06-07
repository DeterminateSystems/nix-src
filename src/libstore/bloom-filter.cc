#include "nix/store/bloom-filter.hh"
#include "nix/util/serialise.hh"

#include <cmath>

namespace nix {

std::string buildBloomFilter(const StorePathSet & paths, double falsePositiveRate)
{
    size_t n = paths.size();

    uint64_t mBits = 8;
    uint32_t k = 1;
    if (n) {
        constexpr double ln2 = 0.6931471805599453;
        double mF = -double(n) * std::log(falsePositiveRate) / (ln2 * ln2);
        mBits = ((uint64_t(std::ceil(mF)) + 7) / 8) * 8;
        long kL = std::lround((double(mBits) / double(n)) * ln2);
        k = uint32_t(std::max<long>(1, kL));
    }

    constexpr size_t headerLen = 8 + 8 + 8 + 8;
    StringSink sink(headerLen + mBits / 8);

    using namespace std::string_view_literals;
    sink("NixBloom"sv);
    sink << 1;    // version
    sink << k;
    sink << mBits;
    assert(sink.s.size() == headerLen);

    sink.s.resize(headerLen + mBits / 8);
    char * bits = sink.s.data() + headerLen;
    for (auto & path : paths)
        forEachBloomBitPosition(path, k, mBits, [&](uint64_t pos) { bits[pos / 8] |= uint8_t(1) << (pos % 8); });

    return std::move(sink.s);
}

} // namespace nix
