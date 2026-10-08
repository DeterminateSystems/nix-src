#include "nix/util/zstd-patch.hh"
#include "nix/util/compression.hh"

#include <gtest/gtest.h>

#include <random>

namespace nix {

static std::string randomString(size_t size, uint64_t seed)
{
    std::mt19937_64 rng(seed);
    std::string s(size, '\0');
    for (auto & c : s)
        c = (char) rng();
    return s;
}

static std::string applyPatch(std::string_view base, std::string_view patch)
{
    StringSink sink;
    applyZstdPatch(base, patch, sink);
    return std::move(sink.s);
}

TEST(zstdPatch, roundtrip)
{
    auto base = randomString(1 << 20, 1);
    auto target = base;
    /* Make some small changes. */
    target.replace(1000, 5, "hello");
    target.insert(500000, "inserted");
    target.erase(800000, 100);

    auto patch = makeZstdPatch(base, target, 19);
    ASSERT_EQ(applyPatch(base, patch), target);

    /* The base is random, so the patch is only small because it
       refers to the base. */
    ASSERT_LT(patch.size(), 1000);
}

TEST(zstdPatch, emptyBase)
{
    auto target = randomString(10000, 2);
    auto patch = makeZstdPatch("", target, 3);
    ASSERT_EQ(applyPatch("", patch), target);
}

TEST(zstdPatch, emptyTarget)
{
    auto base = randomString(10000, 3);
    auto patch = makeZstdPatch(base, "", 3);
    ASSERT_EQ(applyPatch(base, patch), "");
}

TEST(zstdPatch, wrongBase)
{
    auto base = randomString(100000, 4);
    auto target = base;
    target.replace(50000, 5, "hello");
    auto patch = makeZstdPatch(base, target, 19);

    /* Applying to the wrong base produces garbage (or an error), so
       callers must check the base's hash. */
    auto otherBase = randomString(100000, 5);
    try {
        ASSERT_NE(applyPatch(otherBase, patch), target);
    } catch (CompressionError &) {
    }
}

TEST(zstdPatch, truncated)
{
    auto base = randomString(100000, 6);
    auto target = randomString(100000, 7);
    auto patch = makeZstdPatch(base, target, 3);
    ASSERT_THROW(applyPatch(base, patch.substr(0, patch.size() / 2)), CompressionError);
}

TEST(zstdPatch, trailingGarbage)
{
    auto base = randomString(1000, 8);
    auto patch = makeZstdPatch(base, base, 3);
    ASSERT_THROW(applyPatch(base, patch + "garbage"), CompressionError);
}

} // namespace nix
