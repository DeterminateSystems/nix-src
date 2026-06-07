#include "nix/store/bloom-filter.hh"

#include <cmath>
#include <cstring>

namespace nix {

std::string buildBloomFilter(const StorePathSet & paths, double falsePositiveRate)
{
    size_t n = paths.size();

    uint64_t mBits;
    uint32_t k;
    if (n == 0) {
        mBits = 8;
        k = 1;
    } else {
        constexpr double ln2 = 0.6931471805599453;
        double mF = -double(n) * std::log(falsePositiveRate) / (ln2 * ln2);
        mBits = ((uint64_t(std::ceil(mF)) + 7) / 8) * 8;
        long kL = std::lround((double(mBits) / double(n)) * ln2);
        k = uint32_t(std::max<long>(1, kL));
    }

    const size_t headerLen = 8 + 4 + 4 + 8;
    std::string out(headerLen + mBits / 8, '\0');

    std::memcpy(out.data(), "NixBloom", 8);
    auto writeU32 = [&](size_t off, uint32_t v) {
        for (int i = 0; i < 4; ++i)
            out[off + i] = char((v >> (8 * i)) & 0xff);
    };
    auto writeU64 = [&](size_t off, uint64_t v) {
        for (int i = 0; i < 8; ++i)
            out[off + i] = char((v >> (8 * i)) & 0xff);
    };
    writeU32(8, 1);
    writeU32(12, k);
    writeU64(16, mBits);

    char * bits = out.data() + headerLen;

    for (auto & path : paths)
        forEachBloomBitPosition(path, k, mBits, [&](uint64_t pos) { bits[pos / 8] |= uint8_t(1) << (pos % 8); });

    return out;
}

} // namespace nix
