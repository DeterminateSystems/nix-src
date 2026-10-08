#include "nix/util/thread-pool.hh"

#include <gtest/gtest.h>

#include <random>

namespace nix {

TEST(processOrdered, consumesInOrder)
{
    const size_t n = 200;

    std::vector<size_t> consumed;

    processOrdered<size_t>(
        n,
        [&](size_t i) {
            /* Make items finish out of order. */
            thread_local std::mt19937 rng(std::hash<std::thread::id>{}(std::this_thread::get_id()));
            std::this_thread::sleep_for(std::chrono::microseconds(rng() % 1000));
            return i * i;
        },
        [&](size_t i, size_t && res) {
            ASSERT_EQ(i, consumed.size());
            ASSERT_EQ(res, i * i);
            consumed.push_back(i);
        },
        0,
        8);

    ASSERT_EQ(consumed.size(), n);
}

TEST(processOrdered, respectsWindow)
{
    const size_t window = 5;

    std::atomic<size_t> outstanding = 0, maxOutstanding = 0;
    size_t count = 0;

    processOrdered<size_t>(
        100,
        [&](size_t i) {
            auto cur = ++outstanding;
            size_t prev = maxOutstanding;
            while (cur > prev && !maxOutstanding.compare_exchange_weak(prev, cur))
                ;
            /* Make the first item slow, so that other items pile up
               behind it. */
            if (i == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return i;
        },
        [&](size_t i, size_t &&) {
            --outstanding;
            count++;
        },
        window,
        8);

    ASSERT_EQ(count, 100);
    ASSERT_LE(maxOutstanding, window);
}

TEST(processOrdered, empty)
{
    processOrdered<int>(0, [](size_t) -> int { throw Error("should not be called"); }, [](size_t, int &&) { FAIL(); });
}

TEST(processOrdered, produceThrows)
{
    ASSERT_THROW(
        processOrdered<size_t>(
            100,
            [](size_t i) {
                if (i == 42)
                    throw Error("boom");
                return i;
            },
            [](size_t, size_t &&) {}),
        Error);
}

TEST(processOrdered, consumeThrows)
{
    ASSERT_THROW(
        processOrdered<size_t>(
            100,
            [](size_t i) { return i; },
            [](size_t i, size_t &&) {
                if (i == 42)
                    throw Error("boom");
            }),
        Error);
}

} // namespace nix
