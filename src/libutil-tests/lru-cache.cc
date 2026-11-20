#include "nix/util/lru-cache.hh"
#include <gtest/gtest.h>

namespace nix {

/* ----------------------------------------------------------------------------
 * size
 * --------------------------------------------------------------------------*/

TEST(LRUCache, sizeOfEmptyCacheIsZero)
{
    LRUCache<std::string, std::string> c(10);
    ASSERT_EQ(c.size(), 0u);
}

TEST(LRUCache, sizeOfSingleElementCacheIsOne)
{
    LRUCache<std::string, std::string> c(10);
    c.upsert("foo", "bar");
    ASSERT_EQ(c.size(), 1u);
}

/* ----------------------------------------------------------------------------
 * upsert / get
 * --------------------------------------------------------------------------*/

TEST(LRUCache, getFromEmptyCache)
{
    LRUCache<std::string, std::string> c(10);
    auto val = c.get("x");
    ASSERT_EQ(val.has_value(), false);
}

TEST(LRUCache, getExistingValue)
{
    LRUCache<std::string, std::string> c(10);
    c.upsert("foo", "bar");
    auto val = c.get("foo");
    ASSERT_EQ(val, "bar");
}

TEST(LRUCache, getNonExistingValueFromNonEmptyCache)
{
    LRUCache<std::string, std::string> c(10);
    c.upsert("foo", "bar");
    auto val = c.get("another");
    ASSERT_EQ(val.has_value(), false);
}

TEST(LRUCache, upsertOnZeroCapacityCache)
{
    LRUCache<std::string, std::string> c(0);
    c.upsert("foo", "bar");
    auto val = c.get("foo");
    ASSERT_EQ(val.has_value(), false);
}

TEST(LRUCache, updateExistingValue)
{
    LRUCache<std::string, std::string> c(1);
    c.upsert("foo", "bar");

    auto val = c.get("foo");
    ASSERT_EQ(val.value_or("error"), "bar");
    ASSERT_EQ(c.size(), 1u);

    c.upsert("foo", "changed");
    val = c.get("foo");
    ASSERT_EQ(val.value_or("error"), "changed");
    ASSERT_EQ(c.size(), 1u);
}

TEST(LRUCache, overwriteOldestWhenCapacityIsReached)
{
    LRUCache<std::string, std::string> c(3);
    c.upsert("one", "eins");
    c.upsert("two", "zwei");
    c.upsert("three", "drei");

    ASSERT_EQ(c.size(), 3u);
    ASSERT_EQ(c.get("one").value_or("error"), "eins");

    // exceed capacity
    c.upsert("another", "whatever");

    ASSERT_EQ(c.size(), 3u);
    // Retrieving "one" makes it the most recent element thus
    // two will be the oldest one and thus replaced.
    ASSERT_EQ(c.get("two").has_value(), false);
    ASSERT_EQ(c.get("another").value(), "whatever");
}

TEST(LRUCache, updateMakesMostRecentAndEvictionsRepeat)
{
    LRUCache<std::string, std::string> c(2);
    c.upsert("one", "eins");
    c.upsert("two", "zwei");

    // Updating "one" makes "two" the oldest.
    c.upsert("one", "uno");
    c.upsert("three", "drei");
    ASSERT_EQ(c.size(), 2u);
    ASSERT_FALSE(c.get("two").has_value());
    ASSERT_EQ(c.get("one").value(), "uno");
    ASSERT_EQ(c.get("three").value(), "drei");

    // "one" is now the oldest.
    c.upsert("four", "vier");
    c.upsert("five", "fünf");
    ASSERT_EQ(c.size(), 2u);
    ASSERT_FALSE(c.get("one").has_value());
    ASSERT_FALSE(c.get("three").has_value());
    ASSERT_EQ(c.get("four").value(), "vier");
    ASSERT_EQ(c.get("five").value(), "fünf");

    ASSERT_TRUE(c.erase("four"));
    c.upsert("six", "sechs");
    ASSERT_EQ(c.size(), 2u);
    ASSERT_EQ(c.get("five").value(), "fünf");
    ASSERT_EQ(c.get("six").value(), "sechs");
}

/* ----------------------------------------------------------------------------
 * clear
 * --------------------------------------------------------------------------*/

TEST(LRUCache, clearEmptyCache)
{
    LRUCache<std::string, std::string> c(10);
    c.clear();
    ASSERT_EQ(c.size(), 0u);
}

TEST(LRUCache, clearNonEmptyCache)
{
    LRUCache<std::string, std::string> c(10);
    c.upsert("one", "eins");
    c.upsert("two", "zwei");
    c.upsert("three", "drei");
    ASSERT_EQ(c.size(), 3u);
    c.clear();
    ASSERT_EQ(c.size(), 0u);
}

/* ----------------------------------------------------------------------------
 * erase
 * --------------------------------------------------------------------------*/

TEST(LRUCache, eraseFromEmptyCache)
{
    LRUCache<std::string, std::string> c(10);
    ASSERT_EQ(c.erase("foo"), false);
    ASSERT_EQ(c.size(), 0u);
}

TEST(LRUCache, eraseMissingFromNonEmptyCache)
{
    LRUCache<std::string, std::string> c(10);
    c.upsert("one", "eins");
    ASSERT_EQ(c.erase("foo"), false);
    ASSERT_EQ(c.size(), 1u);
    ASSERT_EQ(c.get("one").value_or("error"), "eins");
}

TEST(LRUCache, eraseFromNonEmptyCache)
{
    LRUCache<std::string, std::string> c(10);
    c.upsert("one", "eins");
    ASSERT_EQ(c.erase("one"), true);
    ASSERT_EQ(c.size(), 0u);
    ASSERT_EQ(c.get("one").value_or("empty"), "empty");
}
} // namespace nix
