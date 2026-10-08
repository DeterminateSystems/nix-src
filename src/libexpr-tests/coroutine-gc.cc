#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/eval-gc.hh"
#include "nix/util/serialise.hh"

namespace nix {

class CoroutineGCTest : public LibExprTest
{};

/* Regression test for a crash in the garbage collector: when the
   body of a `sinkToSource` coroutine streams its output through a
   `sourceToSink` coroutine (as `copyStorePath()` does with a
   decompression sink), the outer coroutine yields from the *inner*
   coroutine's stack. The GC hooks used to record such a yield as the
   saved stack pointer of the outer coroutine's own stack, so a
   collection would scan from the inner stack up to the base of the
   outer stack, i.e. across a guard page, and crash with SIGSEGV. */
TEST_F(CoroutineGCTest, nestedCoroutineYield)
{
#if NIX_USE_BOEHMGC
    std::string data;
    for (size_t i = 0; i < 1024 * 1024; ++i)
        data.push_back('a' + i % 26);

    auto source = sinkToSource([&](Sink & outerSink) {
        /* Like a decompression sink: the inner coroutine's body
           writes to the outer coroutine's sink, so the outer
           coroutine yields on the inner coroutine's stack. */
        auto inner = sourceToSink([&](Source & src) { src.drainInto(outerSink); });
        std::string_view rest = data;
        while (!rest.empty()) {
            auto chunk = rest.substr(0, 100000);
            (*inner)(chunk);
            rest.remove_prefix(chunk.size());
        }
        inner->finish();
    });

    std::string result;
    char buf[65536];
    while (true) {
        size_t n;
        try {
            n = source->read(buf, sizeof(buf));
        } catch (EndOfFile &) {
            break;
        }
        result.append(buf, n);
        /* The outer coroutine is now suspended from the inner
           coroutine's stack. Run a collection, which scans every
           suspended stack from its saved stack pointer to its base. */
        GC_gcollect();
    }

    ASSERT_EQ(result, data);
#else
    GTEST_SKIP() << "requires the Boehm GC";
#endif
}

} // namespace nix
