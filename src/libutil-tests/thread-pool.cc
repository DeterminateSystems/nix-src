#include "nix/util/thread-pool.hh"

#include <gtest/gtest.h>

#include <random>

namespace nix {

static size_t unitSize(const size_t &)
{
    return 1;
}

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
        unitSize,
        10,
        8);

    ASSERT_EQ(consumed.size(), n);
}

TEST(processOrdered, respectsBufferLimit)
{
    const size_t maxBuffered = 5, maxThreads = 4;

    std::atomic<size_t> buffered = 0, maxSeen = 0;
    size_t count = 0;

    processOrdered<size_t>(
        100,
        [&](size_t i) {
            /* Make the first item slow, so that other items pile up
               behind it. */
            if (i == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            auto cur = ++buffered;
            size_t prev = maxSeen;
            while (cur > prev && !maxSeen.compare_exchange_weak(prev, cur))
                ;
            return i;
        },
        [&](size_t i, size_t &&) {
            --buffered;
            count++;
        },
        unitSize,
        maxBuffered,
        maxThreads);

    ASSERT_EQ(count, 100);
    /* The limit can be exceeded by the items that were already
       started when it was reached. */
    ASSERT_LE(maxSeen, maxBuffered + 2 * maxThreads);
}

TEST(processOrdered, slowItemDoesNotBlockOthers)
{
    const size_t n = 100;

    std::atomic<size_t> produced = 0;

    processOrdered<size_t>(
        n,
        [&](size_t i) {
            if (i == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            produced++;
            return i;
        },
        [&](size_t i, size_t &&) {
            /* Since results have size 0, all other items should have
               been produced while the first one was in progress. */
            if (i == 0)
                ASSERT_EQ(produced, n);
        },
        [](const size_t &) -> size_t { return 0; },
        0,
        4);
}

TEST(processOrdered, empty)
{
    processOrdered<int>(
        0,
        [](size_t) -> int { throw Error("should not be called"); },
        [](size_t, int &&) { FAIL(); },
        [](const int &) -> size_t { return 0; },
        10);
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
            [](size_t, size_t &&) {},
            unitSize,
            10),
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
            },
            unitSize,
            10),
        Error);
}

} // namespace nix
