#include "nix/util/hdiff-patch.hh"
#include "nix/util/compression.hh"

#include <gtest/gtest.h>

#include <random>

namespace nix {

static std::string randomHdiffString(size_t size, uint64_t seed)
{
    std::mt19937_64 rng(seed);
    std::string s(size, '\0');
    for (auto & c : s)
        c = (char) rng();
    return s;
}

static std::string applyHdiffPatchToString(std::string_view base, std::string_view patch)
{
    StringSink sink;
    applyHdiffPatch(base, patch, sink);
    return std::move(sink.s);
}

TEST(hdiffPatch, roundtrip)
{
    auto base = randomHdiffString(1 << 20, 1);
    auto target = base;
    /* Make some small changes. */
    target.replace(1000, 5, "hello");
    target.insert(500000, "inserted");
    target.erase(800000, 100);

    auto patch = makeHdiffPatch(base, target, 19);
    ASSERT_EQ(applyHdiffPatchToString(base, patch), target);

    /* The base is random, so the patch is only small because it
       refers to the base. */
    ASSERT_LT(patch.size(), 1000);
}

TEST(hdiffPatch, emptyBase)
{
    auto target = randomHdiffString(10000, 2);
    auto patch = makeHdiffPatch("", target, 3);
    ASSERT_EQ(applyHdiffPatchToString("", patch), target);
}

TEST(hdiffPatch, emptyTarget)
{
    auto base = randomHdiffString(10000, 3);
    auto patch = makeHdiffPatch(base, "", 3);
    ASSERT_EQ(applyHdiffPatchToString(base, patch), "");
}

TEST(hdiffPatch, wrongBase)
{
    auto base = randomHdiffString(100000, 4);
    auto target = base;
    target.replace(50000, 5, "hello");
    auto patch = makeHdiffPatch(base, target, 19);

    /* Applying to the wrong base produces garbage (or an error), so
       callers must check the base's hash. */
    auto otherBase = randomHdiffString(100000, 5);
    try {
        ASSERT_NE(applyHdiffPatchToString(otherBase, patch), target);
    } catch (CompressionError &) {
    }

    /* A base of the wrong size is rejected. */
    ASSERT_THROW(applyHdiffPatchToString(base.substr(1), patch), CompressionError);
}

TEST(hdiffPatch, truncated)
{
    auto base = randomHdiffString(100000, 6);
    auto target = randomHdiffString(100000, 7);
    auto patch = makeHdiffPatch(base, target, 3);
    ASSERT_THROW(applyHdiffPatchToString(base, patch.substr(0, patch.size() / 2)), CompressionError);
}

TEST(hdiffPatch, trailingGarbage)
{
    auto base = randomHdiffString(1000, 8);
    auto patch = makeHdiffPatch(base, base, 3);
    ASSERT_THROW(applyHdiffPatchToString(base, patch + "garbage"), CompressionError);
}

TEST(hdiffPatch, garbage)
{
    ASSERT_THROW(applyHdiffPatchToString("", "not a patch"), CompressionError);
}

} // namespace nix
