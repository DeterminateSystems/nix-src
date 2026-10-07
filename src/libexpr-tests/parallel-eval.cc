#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/parallel-eval.hh"

namespace nix {

class ParallelEvalTest : public LibExprTest
{
public:
    ParallelEvalTest()
        : LibExprTest(openStore("dummy://"), [](bool & readOnlyMode) {
            EvalSettings settings{readOnlyMode};
            settings.nixPath = {};
            settings.evalCores = 4;
            return settings;
        })
    {
    }
};

TEST_F(ParallelEvalTest, executorEnabled)
{
    ASSERT_TRUE(state.executor->enabled);
    ASSERT_EQ(state.executor->evalCores, 4u);
}

TEST_F(ParallelEvalTest, concatStringsSepThunks)
{
    auto v = eval("builtins.concatStringsSep \",\" (builtins.genList (i: builtins.toString (i + 1)) 32)");
    ASSERT_THAT(
        v, IsStringEq("1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32"));
}

TEST_F(ParallelEvalTest, concatStringsSepReportsFirstThrow)
{
    try {
        eval("builtins.concatStringsSep \"\" (builtins.genList (i: throw \"e${builtins.toString i}\") 16)");
        FAIL() << "expected ThrownError";
    } catch (const ThrownError & e) {
        ASSERT_THAT(e.what(), testing::HasSubstr("e0"));
    }
}

TEST_F(ParallelEvalTest, toStringNestedListThunks)
{
    auto v = eval("builtins.toString (builtins.genList (i: builtins.genList (j: i * 4 + j + 1) 4) 4)");
    ASSERT_THAT(v, IsStringEq("1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16"));
}

TEST_F(ParallelEvalTest, preForceListElementsDefaultForce)
{
    auto v = eval("builtins.genList (i: i + 1) 8", false);
    ASSERT_EQ(v.type(), nList);
    state.preForceListElements(v, noPos);
    for (auto elem : v.listView()) {
        state.forceValue(*elem, noPos);
        ASSERT_EQ(elem->type(), nInt);
    }
}

} // namespace nix
