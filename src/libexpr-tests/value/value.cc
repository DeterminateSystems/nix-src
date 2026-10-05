#include "nix/expr/value.hh"
#include "nix/expr/nixexpr.hh"
#include "nix/expr/static-string-data.hh"

#include "nix/store/tests/libstore.hh"
#include <gtest/gtest.h>

namespace nix {

class ValueTest : public LibStoreTest
{};

TEST_F(ValueTest, unsetValue)
{
    Value unsetValue;
    ASSERT_EQ(false, unsetValue.isValid());
}

TEST_F(ValueTest, vInt)
{
    Value vInt;
    vInt.mkInt(42);
    ASSERT_EQ(true, vInt.isValid());
}

TEST_F(ValueTest, staticString)
{
    Value vStr1;
    Value vStr2;
    vStr1.mkStringNoCopy("foo"_sds);
    vStr2.mkStringNoCopy("foo"_sds);

    auto & sd1 = vStr1.string_data();
    auto & sd2 = vStr2.string_data();

    // The strings should be the same
    ASSERT_EQ(sd1.view(), sd2.view());

    // The strings should also be backed by the same (static) allocation
    ASSERT_EQ(&sd1, &sd2);
}

/* Finishing a value that holds a thunk indicates that two writers own
   the same `Value`; it must abort with a diagnostic identifying the
   thunk. */
TEST_F(ValueTest, finishThunkPanics)
{
    ExprInt expr{42};
    Value v;
    v.mkThunk(nullptr, &expr);
    EXPECT_DEATH(v.mkInt(1), "finished value written into a Value that holds a thunk.*thunk of expression");
}

} // namespace nix
