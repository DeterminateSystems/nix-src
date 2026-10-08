#include "nix/util/spilling-sink.hh"

#include <gtest/gtest.h>

namespace nix {

TEST(SpillingStringSink, belowLimit)
{
    SpillingStringSink sink(16);
    sink("hello");
    sink(" world");
    ASSERT_FALSE(sink.isSpilled());
    ASSERT_EQ(sink.size(), 11);
    ASSERT_EQ(sink.getSource()->drain(), "hello world");
}

TEST(SpillingStringSink, atLimit)
{
    SpillingStringSink sink(8);
    sink("0123");
    sink("4567");
    ASSERT_FALSE(sink.isSpilled());
    ASSERT_EQ(sink.getSource()->drain(), "01234567");
}

TEST(SpillingStringSink, empty)
{
    SpillingStringSink sink(8);
    ASSERT_FALSE(sink.isSpilled());
    ASSERT_EQ(sink.size(), 0);
    ASSERT_EQ(sink.getSource()->drain(), "");
}

TEST(SpillingStringSink, spills)
{
    SpillingStringSink sink(1000);
    std::string expected;
    for (int i = 0; i < 10000; ++i) {
        auto s = fmt("line %d\n", i);
        sink(s);
        expected += s;
    }
    ASSERT_TRUE(sink.isSpilled());
    ASSERT_EQ(sink.size(), expected.size());
    ASSERT_EQ(sink.getSource()->drain(), expected);
}

TEST(SpillingStringSink, spillsOnFirstWrite)
{
    SpillingStringSink sink(4);
    sink("0123456789");
    ASSERT_TRUE(sink.isSpilled());
    ASSERT_EQ(sink.getSource()->drain(), "0123456789");
}

} // namespace nix
