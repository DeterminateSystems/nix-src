#include "nix/util/thread-pool.hh"

#include <gtest/gtest.h>

#include <future>

namespace nix {

TEST(ThreadPool, executesWorkBeforeProcess)
{
    ThreadPool pool(2);

    std::promise<void> promise;
    auto future = promise.get_future();

    /* A single work item must not wait for the caller to call
       process(). */
    pool.enqueue([&]() { promise.set_value(); });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(60)), std::future_status::ready);

    pool.process();
}

} // namespace nix
